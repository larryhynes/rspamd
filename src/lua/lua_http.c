/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "lua_common.h"
#include "buffer.h"
#include "dns.h"
#include "http.h"
#include "http_private.h"
#include "utlist.h"
#include "unix-std.h"

/***
 * @module rspamd_http
 * Rspamd HTTP module represents HTTP asynchronous client available from LUA code.
 * This module hides all complexity: DNS resolving, sessions management, zero-copy
 * text transfers and so on under the hood.
 * @example
local rspamd_http = require "rspamd_http"

local function symbol_callback(task)
	local function http_callback(err_message, code, body, headers)
		task:insert_result('SYMBOL', 1) -- task is available via closure
	end

 	rspamd_http.request({
 		task=task,
 		url='http://example.com/data',
 		body=task:get_content(),
 		callback=http_callback,
 		headers={Header='Value', OtherHeader='Value'},
 		mime_type='text/plain',
 		})
 end
 */

#define MAX_HEADERS_SIZE 8192

LUA_FUNCTION_DEF (http, request);

static const struct luaL_reg httplib_m[] = {
	LUA_INTERFACE_DEF (http, request),
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

struct lua_http_cbdata {
	lua_State *L;
	struct rspamd_http_connection *conn;
	struct rspamd_async_session *session;
	struct rspamd_async_watcher *w;
	struct rspamd_http_message *msg;
	struct event_base *ev_base;
	struct timeval tv;
	rspamd_inet_addr_t *addr;
	gchar *mime_type;
	gchar *host;
	gint fd;
	gint cbref;
};

static const int default_http_timeout = 5000;

static struct rspamd_dns_resolver *
lua_http_global_resolver (struct event_base *ev_base)
{
	static struct rspamd_dns_resolver *global_resolver;

	if (global_resolver == NULL) {
		global_resolver = dns_resolver_init (NULL, ev_base, NULL);
	}

	return global_resolver;
}

static void
lua_http_fin (gpointer arg)
{
	struct lua_http_cbdata *cbd = (struct lua_http_cbdata *)arg;

	luaL_unref (cbd->L, LUA_REGISTRYINDEX, cbd->cbref);
	if (cbd->conn) {
		/* Here we already have a connection, so we need to unref it */
		rspamd_http_connection_unref (cbd->conn);
	}
	else if (cbd->msg != NULL) {
		/* We need to free message */
		rspamd_http_message_unref (cbd->msg);
	}

	if (cbd->fd != -1) {
		close (cbd->fd);
	}

	if (cbd->addr) {
		rspamd_inet_address_destroy (cbd->addr);
	}

	if (cbd->mime_type) {
		g_free (cbd->mime_type);
	}

	if (cbd->host) {
		g_free (cbd->host);
	}

	g_slice_free1 (sizeof (struct lua_http_cbdata), cbd);
}

static void
lua_http_maybe_free (struct lua_http_cbdata *cbd)
{
	if (cbd->session) {
		rspamd_session_watcher_pop (cbd->session, cbd->w);
		rspamd_session_remove_event (cbd->session, lua_http_fin, cbd);
	}
	else {
		lua_http_fin (cbd);
	}
}

static void
lua_http_push_error (struct lua_http_cbdata *cbd, const char *err)
{
	lua_rawgeti (cbd->L, LUA_REGISTRYINDEX, cbd->cbref);
	lua_pushstring (cbd->L, err);

	if (lua_pcall (cbd->L, 1, 0, 0) != 0) {
		msg_info ("callback call failed: %s", lua_tostring (cbd->L, -1));
		lua_pop (cbd->L, 1);
	}
}

static void
lua_http_error_handler (struct rspamd_http_connection *conn, GError *err)
{
	struct lua_http_cbdata *cbd = (struct lua_http_cbdata *)conn->ud;

	lua_http_push_error (cbd, err->message);
	lua_http_maybe_free (cbd);
}

static int
lua_http_finish_handler (struct rspamd_http_connection *conn,
		struct rspamd_http_message *msg)
{
	struct lua_http_cbdata *cbd = (struct lua_http_cbdata *)conn->ud;
	struct rspamd_http_header *h, *htmp;
	const gchar *body;
	gsize body_len;

	lua_rawgeti (cbd->L, LUA_REGISTRYINDEX, cbd->cbref);
	/* Error */
	lua_pushnil (cbd->L);
	/* Reply code */
	lua_pushinteger (cbd->L, msg->code);
	/* Body */
	body = rspamd_http_message_get_body (msg, &body_len);

	if (body_len > 0) {
		lua_pushlstring (cbd->L, body, body_len);
	}
	else {
		lua_pushnil (cbd->L);
	}
	/* Headers */
	lua_newtable (cbd->L);

	HASH_ITER (hh, msg->headers, h, htmp) {
		lua_pushlstring (cbd->L, h->name->begin, h->name->len);
		lua_pushlstring (cbd->L, h->value->begin, h->value->len);
		lua_settable (cbd->L, -3);
	}

	if (lua_pcall (cbd->L, 4, 0, 0) != 0) {
		msg_info ("callback call failed: %s", lua_tostring (cbd->L, -1));
		lua_pop (cbd->L, 1);
	}

	lua_http_maybe_free (cbd);

	return 0;
}

static gboolean
lua_http_make_connection (struct lua_http_cbdata *cbd)
{
	int fd;

	rspamd_inet_address_set_port (cbd->addr, cbd->msg->port);
	fd = rspamd_inet_address_connect (cbd->addr, SOCK_STREAM, TRUE);

	if (fd == -1) {
		msg_info ("cannot connect to %V", cbd->msg->host);
		return FALSE;
	}
	cbd->fd = fd;
	cbd->conn = rspamd_http_connection_new (NULL,
			lua_http_error_handler,
			lua_http_finish_handler,
			RSPAMD_HTTP_CLIENT_SIMPLE,
			RSPAMD_HTTP_CLIENT,
			NULL,
			NULL);

	rspamd_http_connection_write_message (cbd->conn, cbd->msg,
			cbd->host, cbd->mime_type, cbd, fd,
			&cbd->tv, cbd->ev_base);
	/* Message is now owned by a connection object */
	cbd->msg = NULL;

	return TRUE;
}

static void
lua_http_dns_handler (struct rdns_reply *reply, gpointer ud)
{
	struct lua_http_cbdata *cbd = (struct lua_http_cbdata *)ud;

	if (reply->code != RDNS_RC_NOERROR) {
		lua_http_push_error (cbd, "unable to resolve host");
		lua_http_maybe_free (cbd);
	}
	else {
		if (reply->entries->type == RDNS_REQUEST_A) {
			cbd->addr = rspamd_inet_address_new (AF_INET,
					&reply->entries->content.a.addr);
		}
		else if (reply->entries->type == RDNS_REQUEST_AAAA) {
			cbd->addr = rspamd_inet_address_new (AF_INET6,
					&reply->entries->content.aaa.addr);
		}

		if (!lua_http_make_connection (cbd)) {
			lua_http_push_error (cbd, "unable to make connection to the host");
			lua_http_maybe_free (cbd);
		}
	}
}

static void
lua_http_push_headers (lua_State *L, struct rspamd_http_message *msg)
{
	const char *name, *value;

	lua_pushnil (L);
	while (lua_next (L, -2) != 0) {

		lua_pushvalue (L, -2);
		name = lua_tostring (L, -1);
		value = lua_tostring (L, -2);

		if (name != NULL && value != NULL) {
			rspamd_http_message_add_header (msg, name, value);
		}
		lua_pop (L, 2);
	}
}

/***
 * @function rspamd_http.request({params...})
 * This function creates HTTP request and accepts several parameters as a table using key=value syntax.
 * Required params are:
 *
 * - `url`
 * - `callback`
 * - `task`
 * @param {string} url specifies URL for a request in the standard URI form (e.g. 'http://example.com/path')
 * @param {function} callback specifies callback function in format  `function (err_message, code, body, headers)` that is called on HTTP request completion
 * @param {task} task if called from symbol handler it is generally a good idea to use the common task objects: event base, DNS resolver and events session
 * @param {table} headers optional headers in form `[name='value', name='value']`
 * @param {string} mime_type MIME type of the HTTP content (for example, `text/html`)
 * @param {string/text} body full body content, can be opaque `rspamd{text}` to avoid data copying
 * @param {number} timeout floating point request timeout value in seconds (default is 5.0 seconds)
 * @return {boolean} `true` if a request has been successfuly scheduled. If this value is `false` then some error occurred, the callback thus will not be called
 */
static gint
lua_http_request (lua_State *L)
{
	const gchar *url, *lua_body;
	gchar *to_resolve;
	gint cbref;
	gsize bodylen;
	struct event_base *ev_base;
	struct rspamd_http_message *msg;
	struct lua_http_cbdata *cbd;
	struct rspamd_dns_resolver *resolver;
	struct rspamd_async_session *session;
	struct rspamd_lua_text *t;
	struct rspamd_task *task = NULL;
	gdouble timeout = default_http_timeout;
	gchar *mime_type = NULL;

	if (lua_gettop (L) >= 2) {
		/* url, callback and event_base format */
		url = luaL_checkstring (L, 1);
		if (url == NULL || lua_type (L, 2) != LUA_TFUNCTION) {
			msg_err ("http request has bad params");
			lua_pushboolean (L, FALSE);
			return 1;
		}
		lua_pushvalue (L, 2);
		cbref = luaL_ref (L, LUA_REGISTRYINDEX);
		if (lua_gettop (L) >= 3 && rspamd_lua_check_udata (L, 3, "rspamd{ev_base}")) {
			ev_base = *(struct event_base **)lua_touserdata (L, 3);
		}
		else {
			ev_base = NULL;
		}

		if (lua_gettop (L) >= 4 && rspamd_lua_check_udata (L, 4, "rspamd{resolver}")) {
			resolver = *(struct rspamd_dns_resolver **)lua_touserdata (L, 4);
		}
		else {
			resolver = lua_http_global_resolver (ev_base);
		}

		if (lua_gettop (L) >= 5 && rspamd_lua_check_udata (L, 5, "rspamd{session}")) {
			session = *(struct rspamd_async_session **)lua_touserdata (L, 5);
		}
		else {
			session = NULL;
		}

		msg = rspamd_http_message_from_url (url);

		if (msg == NULL) {
			lua_pushboolean (L, FALSE);
			return 1;
		}
	}
	else if (lua_type (L, 1) == LUA_TTABLE) {
		lua_pushstring (L, "url");
		lua_gettable (L, -2);
		url = luaL_checkstring (L, -1);
		lua_pop (L, 1);

		lua_pushstring (L, "callback");
		lua_gettable (L, -2);
		if (url == NULL || lua_type (L, -1) != LUA_TFUNCTION) {
			lua_pop (L, 1);
			msg_err ("http request has bad params");
			lua_pushboolean (L, FALSE);
			return 1;
		}
		cbref = luaL_ref (L, LUA_REGISTRYINDEX);

		lua_pushstring (L, "task");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TUSERDATA) {
			task = lua_check_task (L, -1);
			ev_base = task->ev_base;
			resolver = task->resolver;
			session = task->s;
		}
		lua_pop (L, 1);

		if (task == NULL) {
			lua_pushstring (L, "ev_base");
			lua_gettable (L, -2);
			if (rspamd_lua_check_udata (L, -1, "rspamd{ev_base}")) {
				ev_base = *(struct event_base **)lua_touserdata (L, -1);
			}
			else {
				ev_base = NULL;
			}
			lua_pop (L, 1);

			lua_pushstring (L, "resolver");
			lua_gettable (L, -2);
			if (rspamd_lua_check_udata (L, -1, "rspamd{resolver}")) {
				resolver = *(struct rspamd_dns_resolver **)lua_touserdata (L, -1);
			}
			else {
				resolver = lua_http_global_resolver (ev_base);
			}
			lua_pop (L, 1);

			lua_pushstring (L, "session");
			lua_gettable (L, -2);
			if (rspamd_lua_check_udata (L, -1, "rspamd{session}")) {
				session = *(struct rspamd_async_session **)lua_touserdata (L, -1);
			}
			else {
				session = NULL;
			}
			lua_pop (L, 1);
		}

		msg = rspamd_http_message_from_url (url);
		if (msg == NULL) {
			msg_err ("cannot create HTTP message from url %s", url);
			lua_pushboolean (L, FALSE);
			return 1;
		}

		lua_pushstring (L, "headers");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TTABLE) {
			lua_http_push_headers (L, msg);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "timeout");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1) * 1000.;
		}
		lua_pop (L, 1);

		lua_pushstring (L, "mime_type");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TSTRING) {
			mime_type = g_strdup (lua_tostring (L, -1));
		}
		lua_pop (L, 1);

		lua_pushstring (L, "body");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TSTRING) {
			lua_body = lua_tolstring (L, -1, &bodylen);
			rspamd_http_message_set_body (msg, lua_body, bodylen);
		}
		else if (lua_type (L, -1) == LUA_TUSERDATA) {
			t = lua_check_text (L, -1);
			/* TODO: think about zero-copy possibilities */
			if (t) {
				rspamd_http_message_set_body (msg, t->start, t->len);
			}
		}

		lua_pop (L, 1);
	}
	else {
		msg_err ("http request has bad params");
		lua_pushboolean (L, FALSE);

		return 1;
	}

	cbd = g_slice_alloc0 (sizeof (*cbd));
	cbd->L = L;
	cbd->cbref = cbref;
	cbd->msg = msg;
	cbd->ev_base = ev_base;
	cbd->mime_type = mime_type;
	msec_to_tv (timeout, &cbd->tv);
	cbd->fd = -1;

	if (msg->host) {
		cbd->host = rspamd_fstring_cstr (msg->host);
	}

	if (session) {
		cbd->session = session;
		rspamd_session_add_event (session,
				(event_finalizer_t)lua_http_fin,
				cbd,
				g_quark_from_static_string ("lua http"));
		cbd->w = rspamd_session_get_watcher (session);
		rspamd_session_watcher_push (session);
	}

	if (rspamd_parse_inet_address (&cbd->addr, msg->host->str, msg->host->len)) {
		/* Host is numeric IP, no need to resolve */
		if (!lua_http_make_connection (cbd)) {
			lua_http_maybe_free (cbd);
			lua_pushboolean (L, FALSE);

			return 1;
		}
	}
	else {
		if (task == NULL) {
			to_resolve = g_malloc (msg->host->len + 1);
			rspamd_strlcpy (to_resolve, msg->host->str, msg->host->len + 1);

			if (!make_dns_request (resolver, session, NULL, lua_http_dns_handler, cbd,
					RDNS_REQUEST_A,
					to_resolve)) {
				lua_http_maybe_free (cbd);
				lua_pushboolean (L, FALSE);
				g_free (to_resolve);

				return 1;
			}

			g_free (to_resolve);
		}
		else {
			to_resolve = rspamd_mempool_fstrdup (task->task_pool, msg->host);

			if (!make_dns_request_task_forced (task, lua_http_dns_handler, cbd,
					RDNS_REQUEST_A, to_resolve)) {
				lua_http_maybe_free (cbd);
				lua_pushboolean (L, FALSE);

				return 1;
			}
		}
	}

	lua_pushboolean (L, TRUE);
	return 1;
}

static gint
lua_load_http (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, httplib_m);

	return 1;
}

void
luaopen_http (lua_State * L)
{
	rspamd_lua_add_preload (L, "rspamd_http", lua_load_http);
}
