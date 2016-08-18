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
#include "dns.h"
#include "utlist.h"


/***
 * @module rspamd_resolver
 * This module allows to resolve DNS names from LUA code. All resolving is executed
 * asynchronously. Here is an example of name resolution:
 * @example
local function symbol_callback(task)
	local host = 'example.com'

	local function dns_cb(resolver, to_resolve, results, err, _, authenticated)
		if not results then
			rspamd_logger.infox('DNS resolving of %1 failed: %2', host, err)
			return
		end
		for _,r in ipairs(results) do
			-- r is of type rspamd{ip} here, but it can be converted to string
			rspamd_logger.infox('Resolved %1 to %2', host, tostring(r))
		end
	end

	task:get_resolver():resolve_a(task:get_session(), task:get_mempool(),
		host, dns_cb)
end
 */
struct rspamd_dns_resolver * lua_check_dns_resolver (lua_State * L);
void luaopen_dns_resolver (lua_State * L);

/* Lua bindings */
LUA_FUNCTION_DEF (dns_resolver, init);
LUA_FUNCTION_DEF (dns_resolver, resolve_a);
LUA_FUNCTION_DEF (dns_resolver, resolve_ptr);
LUA_FUNCTION_DEF (dns_resolver, resolve_txt);
LUA_FUNCTION_DEF (dns_resolver, resolve_mx);
LUA_FUNCTION_DEF (dns_resolver, resolve);

static const struct luaL_reg dns_resolverlib_f[] = {
	LUA_INTERFACE_DEF (dns_resolver, init),
	{NULL, NULL}
};

static const struct luaL_reg dns_resolverlib_m[] = {
	LUA_INTERFACE_DEF (dns_resolver, resolve_a),
	LUA_INTERFACE_DEF (dns_resolver, resolve_ptr),
	LUA_INTERFACE_DEF (dns_resolver, resolve_txt),
	LUA_INTERFACE_DEF (dns_resolver, resolve_mx),
	LUA_INTERFACE_DEF (dns_resolver, resolve),
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

struct rspamd_dns_resolver *
lua_check_dns_resolver (lua_State * L)
{
	void *ud = rspamd_lua_check_udata (L, 1, "rspamd{resolver}");
	luaL_argcheck (L, ud != NULL, 1, "'resolver' expected");
	return ud ? *((struct rspamd_dns_resolver **)ud) : NULL;
}

struct lua_dns_cbdata {
	lua_State *L;
	struct rspamd_dns_resolver *resolver;
	gint cbref;
	const gchar *to_resolve;
	const gchar *user_str;
	struct rspamd_async_watcher *w;
	struct rspamd_async_session *s;
};

static int
lua_dns_get_type (lua_State *L, int argno)
{
	int type = RDNS_REQUEST_A;
	const gchar *strtype;

	if (lua_type (L, argno) != LUA_TSTRING) {
		lua_pushvalue (L, argno);
		lua_gettable (L, lua_upvalueindex (1));

		type = lua_tonumber (L, -1);
		lua_pop (L, 1);
		if (type == 0) {
			rspamd_lua_typerror (L, argno, "dns_request_type");
		}
	}
	else {
		strtype = lua_tostring (L, argno);

		if (g_ascii_strcasecmp (strtype, "a") == 0) {
			type = RDNS_REQUEST_A;
		}
		else if (g_ascii_strcasecmp (strtype, "aaaa") == 0) {
			type = RDNS_REQUEST_AAAA;
		}
		else if (g_ascii_strcasecmp (strtype, "mx") == 0) {
			type = RDNS_REQUEST_MX;
		}
		else if (g_ascii_strcasecmp (strtype, "txt") == 0) {
			type = RDNS_REQUEST_TXT;
		}
		else if (g_ascii_strcasecmp (strtype, "ptr") == 0) {
			type = RDNS_REQUEST_PTR;
		}
		else {
			msg_err ("bad DNS type: %s", strtype);
		}
	}

	return type;
}

static void
lua_dns_callback (struct rdns_reply *reply, gpointer arg)
{
	struct lua_dns_cbdata *cd = arg;
	gint i = 0;
	struct rspamd_dns_resolver **presolver;
	struct rdns_reply_entry *elt;
	rspamd_inet_addr_t *addr;

	lua_rawgeti (cd->L, LUA_REGISTRYINDEX, cd->cbref);
	presolver = lua_newuserdata (cd->L, sizeof (gpointer));
	rspamd_lua_setclass (cd->L, "rspamd{resolver}", -1);

	*presolver = cd->resolver;
	lua_pushstring (cd->L, cd->to_resolve);

	/*
	 * XXX: rework to handle different request types
	 */
	if (reply->code == RDNS_RC_NOERROR) {
		lua_newtable (cd->L);
		LL_FOREACH (reply->entries, elt)
		{
			switch (elt->type) {
			case RDNS_REQUEST_A:
				addr = rspamd_inet_address_new (AF_INET, &elt->content.a.addr);
				rspamd_lua_ip_push (cd->L, addr);
				rspamd_inet_address_destroy (addr);
				lua_rawseti (cd->L, -2, ++i);
				break;
			case RDNS_REQUEST_AAAA:
				addr = rspamd_inet_address_new (AF_INET6, &elt->content.aaa.addr);
				rspamd_lua_ip_push (cd->L, addr);
				rspamd_inet_address_destroy (addr);
				lua_rawseti (cd->L, -2, ++i);
				break;
			case RDNS_REQUEST_PTR:
				lua_pushstring (cd->L, elt->content.ptr.name);
				lua_rawseti (cd->L, -2, ++i);
				break;
			case RDNS_REQUEST_TXT:
			case RDNS_REQUEST_SPF:
				lua_pushstring (cd->L, elt->content.txt.data);
				lua_rawseti (cd->L, -2, ++i);
				break;
			case RDNS_REQUEST_MX:
				/* mx['name'], mx['priority'] */
				lua_newtable (cd->L);
				rspamd_lua_table_set (cd->L, "name", elt->content.mx.name);
				lua_pushstring (cd->L, "priority");
				lua_pushnumber (cd->L, elt->content.mx.priority);
				lua_settable (cd->L, -3);

				lua_rawseti (cd->L, -2, ++i);
				break;
			}
		}
		lua_pushnil (cd->L);
	}
	else {
		lua_pushnil (cd->L);
		lua_pushstring (cd->L, rdns_strerror (reply->code));
	}

	if (cd->user_str != NULL) {
		lua_pushstring (cd->L, cd->user_str);
	}
	else {
		lua_pushnil (cd->L);
	}

	lua_pushboolean (cd->L, reply->authenticated);

	if (lua_pcall (cd->L, 6, 0, 0) != 0) {
		msg_info ("call to dns callback failed: %s", lua_tostring (cd->L, -1));
		lua_pop (cd->L, 1);
	}

	/* Unref function */
	luaL_unref (cd->L, LUA_REGISTRYINDEX, cd->cbref);

	if (cd->s) {
		rspamd_session_watcher_pop (cd->s, cd->w);
	}
}

/***
 * @function rspamd_resolver.init(ev_base, config)
 * @param {event_base} ev_base event base used for asynchronous events
 * @param {rspamd_config} config rspamd configuration parameters
 * @return {rspamd_resolver} new resolver object associated with the specified base
 */
static int
lua_dns_resolver_init (lua_State *L)
{
	struct rspamd_dns_resolver *resolver, **presolver;
	struct rspamd_config *cfg, **pcfg;
	struct event_base *base, **pbase;

	/* Check args */
	pbase = rspamd_lua_check_udata (L, 1, "rspamd{ev_base}");
	luaL_argcheck (L, pbase != NULL, 1, "'ev_base' expected");
	base = pbase ? *(pbase) : NULL;
	pcfg = rspamd_lua_check_udata (L, 2, "rspamd{config}");
	luaL_argcheck (L, pcfg != NULL,	 2, "'config' expected");
	cfg = pcfg ? *(pcfg) : NULL;

	if (base != NULL && cfg != NULL) {
		resolver = dns_resolver_init (NULL, base, cfg);
		if (resolver) {
			presolver = lua_newuserdata (L, sizeof (gpointer));
			rspamd_lua_setclass (L, "rspamd{resolver}", -1);
			*presolver = resolver;
		}
		else {
			lua_pushnil (L);
		}
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

static int
lua_dns_resolver_resolve_common (lua_State *L,
	struct rspamd_dns_resolver *resolver,
	enum rdns_request_type type,
	int first)
{
	struct rspamd_async_session *session = NULL;
	rspamd_mempool_t *pool = NULL;
	const gchar *to_resolve = NULL, *user_str = NULL;
	struct lua_dns_cbdata *cbdata;
	gint cbref = -1, ret;
	struct rspamd_task *task = NULL;
	GError *err = NULL;
	gboolean forced = FALSE;

	/* Check arguments */
	if (!rspamd_lua_parse_table_arguments (L, first, &err,
			"session=U{session};mempool=U{mempool};*name=S;*callback=F;"
			"option=S;task=U{task};forced=B",
			&session, &pool, &to_resolve, &cbref, &user_str, &task, &forced)) {

		if (err) {
			ret = luaL_error (L, "invalid arguments: %s", err->message);
			g_error_free (err);

			return ret;
		}

		return luaL_error (L, "invalid arguments");
	}

	if (task) {
		pool = task->task_pool;
		session = task->s;
	}

	if (pool != NULL && session != NULL && to_resolve != NULL && cbref != -1) {
		cbdata = rspamd_mempool_alloc0 (pool, sizeof (struct lua_dns_cbdata));
		cbdata->L = L;
		cbdata->resolver = resolver;
		cbdata->cbref = cbref;
		cbdata->user_str = rspamd_mempool_strdup (pool, user_str);

		if (type != RDNS_REQUEST_PTR) {
			cbdata->to_resolve = rspamd_mempool_strdup (pool, to_resolve);
		}
		else {
			char *ptr_str;

			ptr_str = rdns_generate_ptr_from_str (to_resolve);

			if (ptr_str == NULL) {
				msg_err_task_check ("wrong resolve string to PTR request: %s",
						to_resolve);
				lua_pushnil (L);

				return 1;
			}

			cbdata->to_resolve = rspamd_mempool_strdup (pool, ptr_str);
			to_resolve = cbdata->to_resolve;
			free (ptr_str);
		}

		if (task == NULL) {
			if (make_dns_request (resolver,
					session,
					pool,
					lua_dns_callback,
					cbdata,
					type,
					to_resolve)) {

				lua_pushboolean (L, TRUE);

				if (session) {
					cbdata->s = session;
					cbdata->w = rspamd_session_get_watcher (session);
					rspamd_session_watcher_push (session);
				}
			}
			else {
				lua_pushnil (L);
			}
		}
		else {
			if (forced) {
				ret = make_dns_request_task_forced (task,
						lua_dns_callback,
						cbdata,
						type,
						to_resolve);
			}
			else {
				ret = make_dns_request_task (task,
						lua_dns_callback,
						cbdata,
						type,
						to_resolve);
			}

			if (ret) {
				lua_pushboolean (L, TRUE);
				cbdata->s = session;
				cbdata->w = rspamd_session_get_watcher (session);
				rspamd_session_watcher_push (session);
			}
			else {
				lua_pushnil (L);
			}
		}
	}
	else {
		return luaL_error (L, "invalid arguments to lua_resolve");
	}

	return 1;

}

/***
 * @method resolver:resolve_a(session, pool, host, callback)
 * Resolve A record for a specified host.
 * @param {async_session} session asynchronous session normally associated with rspamd task (`task:get_session()`)
 * @param {mempool} pool memory pool for storing intermediate data
 * @param {string} host name to resolve
 * @param {function} callback callback function to be called upon name resolution is finished; must be of type `function (resolver, to_resolve, results, err)`
 * @return {boolean} `true` if DNS request has been scheduled
 */
static int
lua_dns_resolver_resolve_a (lua_State *L)
{
	struct rspamd_dns_resolver *dns_resolver = lua_check_dns_resolver (L);

	if (dns_resolver) {
		return lua_dns_resolver_resolve_common (L,
				   dns_resolver,
				   RDNS_REQUEST_A,
				   2);
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

/***
 * @method resolver:resolve_ptr(session, pool, ip, callback)
 * Resolve PTR record for a specified host.
 * @param {async_session} session asynchronous session normally associated with rspamd task (`task:get_session()`)
 * @param {mempool} pool memory pool for storing intermediate data
 * @param {string} ip name to resolve in string form (e.g. '8.8.8.8' or '2001:dead::')
 * @param {function} callback callback function to be called upon name resolution is finished; must be of type `function (resolver, to_resolve, results, err)`
 * @return {boolean} `true` if DNS request has been scheduled
 */
static int
lua_dns_resolver_resolve_ptr (lua_State *L)
{
	struct rspamd_dns_resolver *dns_resolver = lua_check_dns_resolver (L);

	if (dns_resolver) {
		return lua_dns_resolver_resolve_common (L,
				   dns_resolver,
				   RDNS_REQUEST_PTR,
				   2);
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

/***
 * @method resolver:resolve_txt(session, pool, host, callback)
 * Resolve TXT record for a specified host.
 * @param {async_session} session asynchronous session normally associated with rspamd task (`task:get_session()`)
 * @param {mempool} pool memory pool for storing intermediate data
 * @param {string} host name to get TXT record for
 * @param {function} callback callback function to be called upon name resolution is finished; must be of type `function (resolver, to_resolve, results, err)`
 * @return {boolean} `true` if DNS request has been scheduled
 */
static int
lua_dns_resolver_resolve_txt (lua_State *L)
{
	struct rspamd_dns_resolver *dns_resolver = lua_check_dns_resolver (L);

	if (dns_resolver) {
		return lua_dns_resolver_resolve_common (L,
				   dns_resolver,
				   RDNS_REQUEST_TXT,
				   2);
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

/***
 * @method resolver:resolve_mx(session, pool, host, callback)
 * Resolve MX record for a specified host.
 * @param {async_session} session asynchronous session normally associated with rspamd task (`task:get_session()`)
 * @param {mempool} pool memory pool for storing intermediate data
 * @param {string} host name to get MX record for
 * @param {function} callback callback function to be called upon name resolution is finished; must be of type `function (resolver, to_resolve, results, err)`
 * @return {boolean} `true` if DNS request has been scheduled
 */
static int
lua_dns_resolver_resolve_mx (lua_State *L)
{
	struct rspamd_dns_resolver *dns_resolver = lua_check_dns_resolver (L);

	if (dns_resolver) {
		return lua_dns_resolver_resolve_common (L,
				   dns_resolver,
				   RDNS_REQUEST_MX,
				   2);
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

/* XXX: broken currently */
static int
lua_dns_resolver_resolve (lua_State *L)
{
	struct rspamd_dns_resolver *dns_resolver = lua_check_dns_resolver (L);
	int type;

	type = lua_dns_get_type (L, 2);

	if (dns_resolver && type != 0) {
		return lua_dns_resolver_resolve_common (L, dns_resolver, type, 3);
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

static gint
lua_load_dns (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, dns_resolverlib_f);

	return 1;
}

void
luaopen_dns_resolver (lua_State * L)
{

	luaL_newmetatable (L, "rspamd{resolver}");
	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);
	lua_settable (L, -3);

	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{resolver}");
	lua_rawset (L, -3);

	{
		LUA_ENUM (L, RDNS_REQUEST_A,	 RDNS_REQUEST_A);
		LUA_ENUM (L, RDNS_REQUEST_PTR, RDNS_REQUEST_PTR);
		LUA_ENUM (L, RDNS_REQUEST_MX,	RDNS_REQUEST_MX);
		LUA_ENUM (L, RDNS_REQUEST_TXT, RDNS_REQUEST_TXT);
		LUA_ENUM (L, RDNS_REQUEST_SRV, RDNS_REQUEST_SRV);
		LUA_ENUM (L, RDNS_REQUEST_SPF, RDNS_REQUEST_SRV);
		LUA_ENUM (L, RDNS_REQUEST_AAA, RDNS_REQUEST_SRV);
	}

	luaL_register (L, NULL, dns_resolverlib_m);
	rspamd_lua_add_preload (L, "rspamd_resolver", lua_load_dns);

	lua_pop (L, 1);                      /* remove metatable from stack */
}
