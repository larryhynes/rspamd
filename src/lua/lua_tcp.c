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
#include "utlist.h"
#include "unix-std.h"

static void lua_tcp_handler (int fd, short what, gpointer ud);
/***
 * @module rspamd_tcp
 * Rspamd TCP module represents generic TCP asynchronous client available from LUA code.
 * This module hides all complexity: DNS resolving, sessions management, zero-copy
 * text transfers and so on under the hood. It can work in partial or complete modes:
 *
 * - partial mode is used when you need to call a continuation routine each time data is available for read
 * - complete mode calls for continuation merely when all data is read from socket (e.g. when a server sends reply and closes a connection)
 * @example
local logger = require "rspamd_logger"
local tcp = require "rspamd_tcp"

rspamd_config.SYM = function(task)

    local function cb(err, data)
        logger.infox('err: %1, data: %2', err, tostring(data))
    end

    tcp.request({
    	task = task,
    	host = "google.com",
    	port = 80,
    	data = {"GET / HTTP/1.0\r\n", "Host: google.com\r\n", "\r\n"},
    	callback = cb})
end
 */

LUA_FUNCTION_DEF (tcp, request);

static const struct luaL_reg tcp_libf[] = {
	LUA_INTERFACE_DEF (tcp, request),
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

struct lua_tcp_cbdata {
	lua_State *L;
	struct rspamd_async_session *session;
	struct event_base *ev_base;
	struct timeval tv;
	rspamd_inet_addr_t *addr;
	rspamd_mempool_t *pool;
	struct iovec *iov;
	GString *in;
	gchar *stop_pattern;
	struct rspamd_async_watcher *w;
	struct event ev;
	gint fd;
	gint cbref;
	guint iovlen;
	guint pos;
	guint total;
	gboolean partial;
	gboolean do_shutdown;
	guint16 port;
};

static const int default_tcp_timeout = 5000;

static struct rspamd_dns_resolver *
lua_tcp_global_resolver (struct event_base *ev_base)
{
	static struct rspamd_dns_resolver *global_resolver;

	if (global_resolver == NULL) {
		global_resolver = dns_resolver_init (NULL, ev_base, NULL);
	}

	return global_resolver;
}

static void
lua_tcp_fin (gpointer arg)
{
	struct lua_tcp_cbdata *cbd = (struct lua_tcp_cbdata *)arg;

	luaL_unref (cbd->L, LUA_REGISTRYINDEX, cbd->cbref);

	if (cbd->fd != -1) {
		event_del (&cbd->ev);
		close (cbd->fd);
	}

	if (cbd->addr) {
		rspamd_inet_address_destroy (cbd->addr);
	}

	g_slice_free1 (sizeof (struct lua_tcp_cbdata), cbd);
}

static void
lua_tcp_maybe_free (struct lua_tcp_cbdata *cbd)
{
	if (cbd->session) {
		rspamd_session_watcher_pop (cbd->session, cbd->w);
		rspamd_session_remove_event (cbd->session, lua_tcp_fin, cbd);
	}
	else {
		lua_tcp_fin (cbd);
	}
}

static void
lua_tcp_push_error (struct lua_tcp_cbdata *cbd, const char *err, ...)
{
	va_list ap;

	va_start (ap, err);
	lua_rawgeti (cbd->L, LUA_REGISTRYINDEX, cbd->cbref);
	lua_pushvfstring (cbd->L, err, ap);
	va_end (ap);

	if (lua_pcall (cbd->L, 1, 0, 0) != 0) {
		msg_info ("callback call failed: %s", lua_tostring (cbd->L, -1));
		lua_pop (cbd->L, 1);
	}
}

static void
lua_tcp_push_data (struct lua_tcp_cbdata *cbd, const gchar *str, gsize len)
{
	struct rspamd_lua_text *t;

	lua_rawgeti (cbd->L, LUA_REGISTRYINDEX, cbd->cbref);
	/* Error */
	lua_pushnil (cbd->L);
	/* Body */
	t = lua_newuserdata (cbd->L, sizeof (*t));
	rspamd_lua_setclass (cbd->L, "rspamd{text}", -1);
	t->start = str;
	t->len = len;
	t->own = FALSE;

	if (lua_pcall (cbd->L, 2, 0, 0) != 0) {
		msg_info ("callback call failed: %s", lua_tostring (cbd->L, -1));
		lua_pop (cbd->L, 1);
	}
}

static void
lua_tcp_write_helper (struct lua_tcp_cbdata *cbd)
{
	struct iovec *start;
	guint niov, i;
	gint flags = 0;
	gsize remain;
	gssize r;
	struct iovec *cur_iov;
	struct msghdr msg;

	if (cbd->pos == cbd->total) {
		goto call_finish_handler;
	}

	start = &cbd->iov[0];
	niov = cbd->iovlen;
	remain = cbd->pos;
	/* We know that niov is small enough for that */
	cur_iov = alloca (niov * sizeof (struct iovec));
	memcpy (cur_iov, cbd->iov, niov * sizeof (struct iovec));

	for (i = 0; i < cbd->iovlen && remain > 0; i++) {
		/* Find out the first iov required */
		start = &cur_iov[i];
		if (start->iov_len <= remain) {
			remain -= start->iov_len;
			start = &cur_iov[i + 1];
			niov--;
		}
		else {
			start->iov_base = (void *)((char *)start->iov_base + remain);
			start->iov_len -= remain;
			remain = 0;
		}
	}

	memset (&msg, 0, sizeof (msg));
	msg.msg_iov = start;
	msg.msg_iovlen = MIN (IOV_MAX, niov);
	g_assert (niov > 0);
#ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif
	r = sendmsg (cbd->fd, &msg, flags);

	if (r == -1) {
		lua_tcp_push_error (cbd, "IO write error while trying to write %d "
				"bytes: %s", (gint)remain, strerror (errno));
		lua_tcp_maybe_free (cbd);
		return;
	}
	else {
		cbd->pos += r;
	}

	if (cbd->pos >= cbd->total) {
		goto call_finish_handler;
	}
	else {
		/* Want to write more */
		event_add (&cbd->ev, &cbd->tv);
	}

	return;

call_finish_handler:

	if (!cbd->partial) {
		cbd->in = g_string_sized_new (BUFSIZ);
		rspamd_mempool_add_destructor (cbd->pool, rspamd_gstring_free_hard,
				cbd->in);
	}

	if (cbd->do_shutdown) {
		/* Half close the connection */
		shutdown (cbd->fd, SHUT_WR);
	}

	event_del (&cbd->ev);
#ifdef EV_CLOSED
	event_set (&cbd->ev, cbd->fd, EV_READ|EV_PERSIST|EV_CLOSED,
				lua_tcp_handler, cbd);
#else
	event_set (&cbd->ev, cbd->fd, EV_READ|EV_PERSIST, lua_tcp_handler, cbd);
#endif
	event_base_set (cbd->ev_base, &cbd->ev);
	event_add (&cbd->ev, &cbd->tv);
}

static void
lua_tcp_handler (int fd, short what, gpointer ud)
{
	struct lua_tcp_cbdata *cbd = ud;
	gchar inbuf[BUFSIZ];
	gssize r;
	guint slen;

	if (what == EV_READ) {
		g_assert (cbd->partial || cbd->in != NULL);

		r = read (cbd->fd, inbuf, sizeof (inbuf));

		if (r <= 0) {
			/*
			 * We actually can have connection reset here, so we just check if
			 * the cumulative buffer is not empty
			 */
			if (cbd->partial) {
				if (r < 0) {
					lua_tcp_push_error (cbd, "IO read error while trying to read %d "
									"bytes: %s", (gint)sizeof (inbuf),
									strerror (errno));
				}
			}
			else {
				if (cbd->in->len > 0) {
					lua_tcp_push_data (cbd, cbd->in->str, cbd->in->len);
				}
				else {
					lua_tcp_push_error (cbd, "IO read error while trying to write %d "
							"bytes: %s", (gint)sizeof (inbuf),
							strerror (errno));
				}
			}

			lua_tcp_maybe_free (cbd);
		}
		else {
			if (cbd->partial) {
				lua_tcp_push_data (cbd, inbuf, r);
			}
			else {
				g_string_append_len (cbd->in, inbuf, r);

				if (cbd->stop_pattern) {
					slen = strlen (cbd->stop_pattern);

					if (cbd->in->len >= slen) {
						if (memcmp (cbd->stop_pattern, cbd->in->str +
								(cbd->in->len - slen), slen) == 0) {
							lua_tcp_push_data (cbd, cbd->in->str, cbd->in->len);
							lua_tcp_maybe_free (cbd);
						}
					}
				}
			}
		}
	}
	else if (what == EV_WRITE) {
		lua_tcp_write_helper (cbd);
	}
#ifdef EV_CLOSED
	else if (what == EV_CLOSED) {
		lua_tcp_push_error (cbd, "Remote peer has closed the connection");
		lua_tcp_maybe_free (cbd);
	}
#endif
	else {
		lua_tcp_push_error (cbd, "IO timeout");
		lua_tcp_maybe_free (cbd);
	}
}

static gboolean
lua_tcp_make_connection (struct lua_tcp_cbdata *cbd)
{
	int fd;

	rspamd_inet_address_set_port (cbd->addr, cbd->port);
	fd = rspamd_inet_address_connect (cbd->addr, SOCK_STREAM, TRUE);

	if (fd == -1) {
		msg_info ("cannot connect to %s", rspamd_inet_address_to_string (cbd->addr));
		return FALSE;
	}
	cbd->fd = fd;

	event_set (&cbd->ev, cbd->fd, EV_WRITE, lua_tcp_handler, cbd);
	event_base_set (cbd->ev_base, &cbd->ev);
	event_add (&cbd->ev, &cbd->tv);

	return TRUE;
}

static void
lua_tcp_dns_handler (struct rdns_reply *reply, gpointer ud)
{
	struct lua_tcp_cbdata *cbd = (struct lua_tcp_cbdata *)ud;
	const struct rdns_request_name *rn;

	if (reply->code != RDNS_RC_NOERROR) {
		rn = rdns_request_get_name (reply->request, NULL);
		lua_tcp_push_error (cbd, "unable to resolve host: %s",
				rn->name);
		lua_tcp_maybe_free (cbd);
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

		rspamd_inet_address_set_port (cbd->addr, cbd->port);

		if (!lua_tcp_make_connection (cbd)) {
			lua_tcp_push_error (cbd, "unable to make connection to the host %s",
					rspamd_inet_address_to_string (cbd->addr));
			lua_tcp_maybe_free (cbd);
		}
	}
}

static gboolean
lua_tcp_arg_toiovec (lua_State *L, gint pos, rspamd_mempool_t *pool,
		struct iovec *vec)
{
	struct rspamd_lua_text *t;
	gsize len;
	const gchar *str;

	if (lua_type (L, pos) == LUA_TUSERDATA) {
		t = lua_check_text (L, pos);

		if (t) {
			vec->iov_base = (void *)t->start;
			vec->iov_len = t->len;

			if (t->own) {
				/* Steal ownership */
				t->own = FALSE;
				rspamd_mempool_add_destructor (pool, g_free, (void *)t->start);
			}
		}
		else {
			msg_err ("bad userdata argument at position %d", pos);
			return FALSE;
		}
	}
	else if (lua_type (L, pos) == LUA_TSTRING) {
		str = luaL_checklstring (L, pos, &len);
		vec->iov_base = rspamd_mempool_alloc (pool, len);
		memcpy (vec->iov_base, str, len);
		vec->iov_len = len;
	}
	else {
		msg_err ("bad argument at position %d", pos);
		return FALSE;
	}

	return TRUE;
}

/***
 * @function rspamd_tcp.request({params})
 * This function creates and sends TCP request to the specified host and port,
 * resolves hostname (if needed) and invokes continuation callback upon data received
 * from the remote peer. This function accepts table of arguments with the following
 * attributes
 *
 * - `task`: rspamd task objects (implies `pool`, `session`, `ev_base` and `resolver` arguments)
 * - `ev_base`: event base (if no task specified)
 * - `resolver`: DNS resolver (no task)
 * - `session`: events session (no task)
 * - `pool`: memory pool (no task)
 * - `host`: IP or name of the peer (required)
 * - `port`: remote port to use (required)
 * - `data`: a table of strings or `rspamd_text` objects that contains data pieces
 * - `callback`: continuation function (required)
 * - `timeout`: floating point value that specifies timeout for IO operations in seconds
 * - `partial`: boolean flag that specifies that callback should be called on any data portion received
 * - `stop_pattern`: stop reading on finding a certain pattern (e.g. \r\n.\r\n for smtp)
 * @return {boolean} true if request has been sent
 */
static gint
lua_tcp_request (lua_State *L)
{
	const gchar *host;
	gchar *stop_pattern = NULL;
	guint port;
	gint cbref, tp;
	struct event_base *ev_base;
	struct lua_tcp_cbdata *cbd;
	struct rspamd_dns_resolver *resolver;
	struct rspamd_async_session *session;
	struct rspamd_task *task = NULL;
	rspamd_mempool_t *pool;
	struct iovec *iov = NULL;
	guint niov = 0, total_out;
	gdouble timeout = default_tcp_timeout;
	gboolean partial = FALSE, do_shutdown = FALSE;

	if (lua_type (L, 1) == LUA_TTABLE) {
		lua_pushstring (L, "host");
		lua_gettable (L, -2);
		host = luaL_checkstring (L, -1);
		lua_pop (L, 1);

		lua_pushstring (L, "port");
		lua_gettable (L, -2);
		port = luaL_checknumber (L, -1);
		lua_pop (L, 1);

		lua_pushstring (L, "callback");
		lua_gettable (L, -2);
		if (host == NULL || lua_type (L, -1) != LUA_TFUNCTION) {
			lua_pop (L, 1);
			msg_err ("tcp request has bad params");
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
			pool = task->task_pool;
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

			lua_pushstring (L, "pool");
			lua_gettable (L, -2);
			if (rspamd_lua_check_udata (L, -1, "rspamd{mempool}")) {
				pool = *(rspamd_mempool_t **)lua_touserdata (L, -1);
			}
			else {
				pool = NULL;
			}
			lua_pop (L, 1);

			lua_pushstring (L, "resolver");
			lua_gettable (L, -2);
			if (rspamd_lua_check_udata (L, -1, "rspamd{resolver}")) {
				resolver = *(struct rspamd_dns_resolver **)lua_touserdata (L, -1);
			}
			else {
				resolver = lua_tcp_global_resolver (ev_base);
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

		lua_pushstring (L, "timeout");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1) * 1000.;
		}
		lua_pop (L, 1);

		lua_pushstring (L, "stop_pattern");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TSTRING) {
			stop_pattern = rspamd_mempool_strdup (pool, lua_tostring (L, -1));
		}
		lua_pop (L, 1);

		lua_pushstring (L, "partial");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TBOOLEAN) {
			partial = lua_toboolean (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "shutdown");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TBOOLEAN) {
			do_shutdown = lua_toboolean (L, -1);
		}
		lua_pop (L, 1);

		if (pool == NULL) {
			lua_pop (L, 1);
			msg_err ("tcp request has no memory pool associated");
			lua_pushboolean (L, FALSE);
			return 1;
		}

		lua_pushstring (L, "data");
		lua_gettable (L, -2);
		total_out = 0;

		tp = lua_type (L, -1);
		if (tp == LUA_TSTRING || tp == LUA_TUSERDATA) {
			iov = rspamd_mempool_alloc (pool, sizeof (*iov));
			niov = 1;

			if (!lua_tcp_arg_toiovec (L, -1, pool, iov)) {
				lua_pop (L, 1);
				msg_err ("tcp request has bad data argument");
				lua_pushboolean (L, FALSE);
				return 1;
			}

			total_out = iov[0].iov_len;
		}
		else if (tp == LUA_TTABLE) {
			/* Count parts */
			lua_pushnil (L);
			while (lua_next (L, -2) != 0) {
				niov ++;
				lua_pop (L, 1);
			}

			iov = rspamd_mempool_alloc (pool, sizeof (*iov) * niov);
			lua_pushnil (L);
			niov = 0;

			while (lua_next (L, -2) != 0) {
				if (!lua_tcp_arg_toiovec (L, -1, pool, &iov[niov])) {
					lua_pop (L, 2);
					msg_err ("tcp request has bad data argument at pos %d", niov);
					lua_pushboolean (L, FALSE);
					return 1;
				}

				total_out += iov[niov].iov_len;
				niov ++;

				lua_pop (L, 1);
			}
		}

		lua_pop (L, 1);
	}
	else {
		msg_err ("tcp request has bad params");
		lua_pushboolean (L, FALSE);

		return 1;
	}

	cbd = g_slice_alloc0 (sizeof (*cbd));
	cbd->L = L;
	cbd->cbref = cbref;
	cbd->ev_base = ev_base;
	msec_to_tv (timeout, &cbd->tv);
	cbd->fd = -1;
	cbd->pool = pool;
	cbd->partial = partial;
	cbd->do_shutdown = do_shutdown;
	cbd->iov = iov;
	cbd->iovlen = niov;
	cbd->total = total_out;
	cbd->pos = 0;
	cbd->port = port;
	cbd->stop_pattern = stop_pattern;

	if (session) {
		cbd->session = session;
		rspamd_session_add_event (session,
				(event_finalizer_t)lua_tcp_fin,
				cbd,
				g_quark_from_static_string ("lua tcp"));
		cbd->w = rspamd_session_get_watcher (session);
		rspamd_session_watcher_push (session);
	}

	if (rspamd_parse_inet_address (&cbd->addr, host, 0)) {
		rspamd_inet_address_set_port (cbd->addr, port);
		/* Host is numeric IP, no need to resolve */
		if (!lua_tcp_make_connection (cbd)) {
			lua_tcp_maybe_free (cbd);
			lua_pushboolean (L, FALSE);

			return 1;
		}
	}
	else {
		if (task == NULL) {
			if (!make_dns_request (resolver, session, NULL, lua_tcp_dns_handler, cbd,
					RDNS_REQUEST_A, host)) {
				lua_tcp_push_error (cbd, "cannot resolve host: %s", host);
				lua_tcp_maybe_free (cbd);
			}
		}
		else {
			if (!make_dns_request_task (task, lua_tcp_dns_handler, cbd,
					RDNS_REQUEST_A, host)) {
				lua_tcp_push_error (cbd, "cannot resolve host: %s", host);
				lua_tcp_maybe_free (cbd);
			}
		}
	}

	lua_pushboolean (L, TRUE);
	return 1;
}

static gint
lua_load_tcp (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, tcp_libf);

	return 1;
}

void
luaopen_tcp (lua_State * L)
{
	rspamd_lua_add_preload (L, "rspamd_tcp", lua_load_tcp);
}
