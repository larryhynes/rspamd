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
/***MODULE:dkim
 * rspamd module that checks dkim records of incoming email
 *
 * Allowed options:
 * - symbol_allow (string): symbol to insert in case of allow (default: 'R_DKIM_ALLOW')
 * - symbol_reject (string): symbol to insert (default: 'R_DKIM_REJECT')
 * - symbol_tempfail (string): symbol to insert in case of temporary fail (default: 'R_DKIM_TEMPFAIL')
 * - whitelist (map): map of whitelisted networks
 * - domains (map): map of domains to check
 * - strict_multiplier (number): multiplier for strict domains
 * - time_jitter (number): jitter in seconds to allow time diff while checking
 * - trusted_only (flag): check signatures only for domains in 'domains' map
 * - skip_mutli (flag): skip messages with multiply dkim signatures
 */

#include "config.h"
#include "libmime/message.h"
#include "libserver/dkim.h"
#include "libutil/hash.h"
#include "libutil/map.h"
#include "rspamd.h"
#include "utlist.h"
#include "lua/lua_common.h"

#define DEFAULT_SYMBOL_REJECT "R_DKIM_REJECT"
#define DEFAULT_SYMBOL_TEMPFAIL "R_DKIM_TEMPFAIL"
#define DEFAULT_SYMBOL_ALLOW "R_DKIM_ALLOW"
#define DEFAULT_CACHE_SIZE 2048
#define DEFAULT_CACHE_MAXAGE 86400
#define DEFAULT_TIME_JITTER 60
#define DEFAULT_MAX_SIGS 5

struct dkim_ctx {
	struct module_ctx ctx;
	const gchar *symbol_reject;
	const gchar *symbol_tempfail;
	const gchar *symbol_allow;

	rspamd_mempool_t *dkim_pool;
	radix_compressed_t *whitelist_ip;
	GHashTable *dkim_domains;
	guint strict_multiplier;
	guint time_jitter;
	rspamd_lru_hash_t *dkim_hash;
	rspamd_lru_hash_t *dkim_sign_hash;
	const gchar *sign_headers;
	gint sign_condition_ref;
	guint max_sigs;
	gboolean trusted_only;
	gboolean skip_multi;
};

struct dkim_check_result {
	rspamd_dkim_context_t *ctx;
	rspamd_dkim_key_t *key;
	struct rspamd_task *task;
	gint res;
	gint mult_allow, mult_deny;
	struct rspamd_async_watcher *w;
	struct dkim_check_result *next, *prev, *first;
};

static struct dkim_ctx *dkim_module_ctx = NULL;

static void dkim_symbol_callback (struct rspamd_task *task, void *unused);
static void dkim_sign_callback (struct rspamd_task *task, void *unused);

/* Initialization */
gint dkim_module_init (struct rspamd_config *cfg, struct module_ctx **ctx);
gint dkim_module_config (struct rspamd_config *cfg);
gint dkim_module_reconfig (struct rspamd_config *cfg);

module_t dkim_module = {
	"dkim",
	dkim_module_init,
	dkim_module_config,
	dkim_module_reconfig,
	NULL,
	RSPAMD_MODULE_VER
};

static void
dkim_module_key_dtor (gpointer k)
{
	rspamd_dkim_key_t *key = k;

	rspamd_dkim_key_unref (key);
}

gint
dkim_module_init (struct rspamd_config *cfg, struct module_ctx **ctx)
{
	dkim_module_ctx = g_malloc0 (sizeof (struct dkim_ctx));

	dkim_module_ctx->dkim_pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), "dkim");
	dkim_module_ctx->sign_headers = "from:sender:reply-to:subject:date:message-id:"
			"to:cc:mime-version:content-type:content-transfer-encoding:"
			"resent-to:resent-cc:resent-from:resent-sender:resent-message-id:"
			"in-reply-to:references:list-id:list-owner:list-unsubscribe:"
			"list-subscribe:list-post";
	dkim_module_ctx->sign_condition_ref = -1;
	dkim_module_ctx->max_sigs = DEFAULT_MAX_SIGS;

	*ctx = (struct module_ctx *)dkim_module_ctx;

	rspamd_rcl_add_doc_by_path (cfg,
			NULL,
			"DKIM check plugin",
			"dkim",
			UCL_OBJECT,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Map of IP addresses that should be excluded from DKIM checks",
			"whitelist",
			UCL_STRING,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Symbol that is added if DKIM check is successful",
			"symbol_allow",
			UCL_STRING,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Symbol that is added if DKIM check is unsuccessful",
			"symbol_reject",
			UCL_STRING,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Symbol that is added if DKIM check can't be completed (e.g. DNS failure)",
			"symbol_tempfail",
			UCL_STRING,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Size of DKIM keys cache",
			"dkim_cache_size",
			UCL_INT,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Allow this time difference when checking DKIM signature time validity",
			"time_jitter",
			UCL_TIME,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Domains to check DKIM for (check all domains if this option is empty)",
			"domains",
			UCL_STRING,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Map of domains that are treated as 'trusted' meaning that DKIM policy failure has more significant score",
			"trusted_domains",
			UCL_STRING,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Multiply dkim score by this factor for trusted domains",
			"strict_multiplier",
			UCL_FLOAT,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Check DKIM policies merely for `trusted_domains`",
			"trusted_only",
			UCL_BOOLEAN,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Do not check messages with multiple DKIM signatures",
			"skip_multi",
			UCL_BOOLEAN,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Lua script that tells if a message should be signed and with what params",
			"sign_condition",
			UCL_STRING,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Maximum number of DKIM signatures to check",
			"max_sigs",
			UCL_INT,
			NULL,
			0,
			NULL,
			0);

	return 0;
}

gint
dkim_module_config (struct rspamd_config *cfg)
{
	const ucl_object_t *value;
	gint res = TRUE, cb_id = -1, check_id = -1;
	guint cache_size;
	gboolean got_trusted = FALSE;

	if (!rspamd_config_is_module_enabled (cfg, "dkim")) {
		return TRUE;
	}

	dkim_module_ctx->whitelist_ip = radix_create_compressed ();

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "symbol_reject")) != NULL) {
		dkim_module_ctx->symbol_reject = ucl_obj_tostring (value);
	}
	else {
		dkim_module_ctx->symbol_reject = DEFAULT_SYMBOL_REJECT;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim",
		"symbol_tempfail")) != NULL) {
		dkim_module_ctx->symbol_tempfail = ucl_obj_tostring (value);
	}
	else {
		dkim_module_ctx->symbol_tempfail = DEFAULT_SYMBOL_TEMPFAIL;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "symbol_allow")) != NULL) {
		dkim_module_ctx->symbol_allow = ucl_obj_tostring (value);
	}
	else {
		dkim_module_ctx->symbol_allow = DEFAULT_SYMBOL_ALLOW;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim",
		"dkim_cache_size")) != NULL) {
		cache_size = ucl_obj_toint (value);
	}
	else {
		cache_size = DEFAULT_CACHE_SIZE;
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "time_jitter")) != NULL) {
		dkim_module_ctx->time_jitter = ucl_obj_todouble (value);
	}
	else {
		dkim_module_ctx->time_jitter = DEFAULT_TIME_JITTER;
	}

	if ((value =
			rspamd_config_get_module_opt (cfg, "dkim", "max_sigs")) != NULL) {
		dkim_module_ctx->max_sigs = ucl_object_toint (value);
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "whitelist")) != NULL) {

		rspamd_config_radix_from_ucl (cfg, value, "DKIM whitelist",
				&dkim_module_ctx->whitelist_ip, NULL);
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "domains")) != NULL) {
		if (!rspamd_map_add_from_ucl (cfg, value,
			"DKIM domains", rspamd_kv_list_read, rspamd_kv_list_fin,
			(void **)&dkim_module_ctx->dkim_domains)) {
			msg_warn_config ("cannot load dkim domains list from %s",
				ucl_obj_tostring (value));
		}
		else {
			got_trusted = TRUE;
		}
	}

	if (!got_trusted && (value =
			rspamd_config_get_module_opt (cfg, "dkim", "trusted_domains")) != NULL) {
		if (!rspamd_map_add_from_ucl (cfg, value,
				"DKIM domains", rspamd_kv_list_read, rspamd_kv_list_fin,
				(void **)&dkim_module_ctx->dkim_domains)) {
			msg_warn_config ("cannot load dkim domains list from %s",
					ucl_obj_tostring (value));
		}
		else {
			got_trusted = TRUE;
		}
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim",
		"strict_multiplier")) != NULL) {
		dkim_module_ctx->strict_multiplier = ucl_obj_toint (value);
	}
	else {
		dkim_module_ctx->strict_multiplier = 1;
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "trusted_only")) != NULL) {
		dkim_module_ctx->trusted_only = ucl_obj_toboolean (value);
	}
	else {
		dkim_module_ctx->trusted_only = FALSE;
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "skip_multi")) != NULL) {
		dkim_module_ctx->skip_multi = ucl_obj_toboolean (value);
	}
	else {
		dkim_module_ctx->skip_multi = FALSE;
	}

	if (dkim_module_ctx->trusted_only && !got_trusted) {
		msg_err_config (
			"trusted_only option is set and no trusted domains are defined; disabling dkim module completely as it is useless in this case");
	}
	else {
		cb_id = rspamd_symbols_cache_add_symbol (cfg->cache,
			dkim_module_ctx->symbol_reject,
			0,
			dkim_symbol_callback,
			NULL,
			SYMBOL_TYPE_NORMAL|SYMBOL_TYPE_FINE,
			-1);
		rspamd_symbols_cache_add_symbol (cfg->cache,
			dkim_module_ctx->symbol_tempfail,
			0,
			NULL, NULL,
			SYMBOL_TYPE_VIRTUAL|SYMBOL_TYPE_FINE,
			cb_id);
		rspamd_symbols_cache_add_symbol (cfg->cache,
			dkim_module_ctx->symbol_allow,
			0,
			NULL, NULL,
			SYMBOL_TYPE_VIRTUAL|SYMBOL_TYPE_FINE,
			cb_id);

		dkim_module_ctx->dkim_hash = rspamd_lru_hash_new (
				cache_size,
				g_free, /* Keys are just C-strings */
				dkim_module_key_dtor);

		msg_info_config ("init internal dkim module");
#ifndef HAVE_OPENSSL
		msg_warn_config (
			"openssl is not found so dkim rsa check is disabled, only check body hash, it is NOT safe to trust these results");
#endif
	}

	if ((value = rspamd_config_get_module_opt (cfg, "dkim", "sign_condition"))
			!= NULL) {
		const gchar *lua_script;

		lua_script = ucl_object_tostring (value);

		if (lua_script) {
			if (luaL_dostring (cfg->lua_state, lua_script) != 0) {
				msg_err_config ("cannot execute lua script for fuzzy "
						"learn condition: %s", lua_tostring (cfg->lua_state, -1));
			}
			else {
				if (lua_type (cfg->lua_state, -1) == LUA_TFUNCTION) {
					dkim_module_ctx->sign_condition_ref = luaL_ref (cfg->lua_state,
							LUA_REGISTRYINDEX);

					dkim_module_ctx->dkim_sign_hash = rspamd_lru_hash_new (
									128,
									g_free, /* Keys are just C-strings */
									(GDestroyNotify)rspamd_dkim_sign_key_unref);
					check_id = rspamd_symbols_cache_add_symbol (cfg->cache,
							"DKIM_SIGN",
							0,
							dkim_sign_callback,
							NULL,
							SYMBOL_TYPE_CALLBACK|SYMBOL_TYPE_FINE,
							-1);
					msg_info_config ("init condition script for DKIM signing");

					/*
					 * Allow dkim signing to be executed only after dkim check
					 */
					if (cb_id > 0) {
						rspamd_symbols_cache_add_delayed_dependency (cfg->cache,
								"DKIM_SIGN", dkim_module_ctx->symbol_reject);
					}

					rspamd_config_add_metric_symbol (cfg, DEFAULT_METRIC,
							"DKIM_SIGN", 0.0, "DKIM signature fake symbol",
							"dkim", RSPAMD_SYMBOL_FLAG_IGNORE, 1);

				}
				else {
					msg_err_config ("lua script must return "
							"function(task) and not %s",
							lua_typename (cfg->lua_state,
									lua_type (cfg->lua_state, -1)));
				}
			}
		}
	}

	return res;
}

gint
dkim_module_reconfig (struct rspamd_config *cfg)
{
	struct module_ctx saved_ctx;

	saved_ctx = dkim_module_ctx->ctx;
	rspamd_mempool_delete (dkim_module_ctx->dkim_pool);
	radix_destroy_compressed (dkim_module_ctx->whitelist_ip);
	if (dkim_module_ctx->dkim_domains) {
		g_hash_table_destroy (dkim_module_ctx->dkim_domains);
	}

	memset (dkim_module_ctx, 0, sizeof (*dkim_module_ctx));
	dkim_module_ctx->ctx = saved_ctx;
	dkim_module_ctx->dkim_pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), "dkim");
	dkim_module_ctx->sign_headers = "from:sender:reply-to:subject:date:message-id:"
			"to:cc:mime-version:content-type:content-transfer-encoding:"
			"resent-to:resent-cc:resent-from:resent-sender:resent-message-id:"
			"in-reply-to:references:list-id:list-owner:list-unsubscribe:"
			"list-subscribe:list-post";
	dkim_module_ctx->sign_condition_ref = -1;
	dkim_module_ctx->max_sigs = DEFAULT_MAX_SIGS;

	return dkim_module_config (cfg);
}

/*
 * Parse strict value for domain in format: 'reject_multiplier:deny_multiplier'
 */
static gboolean
dkim_module_parse_strict (const gchar *value, gint *allow, gint *deny)
{
	const gchar *colon;
	gulong val;

	colon = strchr (value, ':');
	if (colon) {
		if (rspamd_strtoul (value, colon - value, &val)) {
			*deny = val;
			colon++;
			if (rspamd_strtoul (colon, strlen (colon), &val)) {
				*allow = val;
				return TRUE;
			}
		}
	}
	return FALSE;
}

static void
dkim_module_check (struct dkim_check_result *res)
{
	gboolean all_done = TRUE, got_allow = FALSE;
	const gchar *strict_value;
	struct dkim_check_result *first, *cur, *sel = NULL;

	first = res->first;

	DL_FOREACH (first, cur) {
		if (cur->ctx == NULL) {
			continue;
		}

		if (cur->key != NULL && cur->res == -1) {
			cur->res = rspamd_dkim_check (cur->ctx, cur->key, cur->task);

			if (dkim_module_ctx->dkim_domains != NULL) {
				/* Perform strict check */
				if ((strict_value =
						g_hash_table_lookup (dkim_module_ctx->dkim_domains,
								rspamd_dkim_get_domain (cur->ctx))) != NULL) {
					if (!dkim_module_parse_strict (strict_value, &cur->mult_allow,
							&cur->mult_deny)) {
						cur->mult_allow = dkim_module_ctx->strict_multiplier;
						cur->mult_deny = dkim_module_ctx->strict_multiplier;
					}
				}
			}
		}

		if (cur->res == -1 || cur->key == NULL) {
			/* Still need a key */
			all_done = FALSE;
		}
	}

	if (all_done) {
		DL_FOREACH (first, cur) {
			if (cur->ctx == NULL) {
				continue;
			}

			if (cur->res == DKIM_CONTINUE) {
				rspamd_task_insert_result (cur->task,
						dkim_module_ctx->symbol_allow,
						cur->mult_allow * 1.0,
						g_list_prepend (NULL,
								rspamd_mempool_strdup (cur->task->task_pool,
										rspamd_dkim_get_domain (cur->ctx))));
				got_allow = TRUE;
				sel = NULL;
			}
			else if (!got_allow) {
				if (sel == NULL) {
					sel = cur;
				}
				else if (sel->res == DKIM_TRYAGAIN && cur->res != DKIM_TRYAGAIN) {
					sel = cur;
				}
			}
		}
	}

	if (sel != NULL) {
		if (sel->res == DKIM_REJECT) {
			rspamd_task_insert_result (sel->task,
					dkim_module_ctx->symbol_reject,
					sel->mult_deny * 1.0,
					g_list_prepend (NULL,
							rspamd_mempool_strdup (sel->task->task_pool,
									rspamd_dkim_get_domain (sel->ctx))));
		}
		else {
			rspamd_task_insert_result (sel->task,
					dkim_module_ctx->symbol_tempfail,
					1.0,
					g_list_prepend (NULL,
							rspamd_mempool_strdup (sel->task->task_pool,
									rspamd_dkim_get_domain (sel->ctx))));
		}
	}

	if (all_done) {
		rspamd_session_watcher_pop (res->task->s, res->w);
	}
}

static void
dkim_module_key_handler (rspamd_dkim_key_t *key,
	gsize keylen,
	rspamd_dkim_context_t *ctx,
	gpointer ud,
	GError *err)
{
	struct dkim_check_result *res = ud;
	struct rspamd_task *task;

	task = res->task;

	if (key != NULL) {
		/*
		 * We actually receive key with refcount = 1, so we just assume that
		 * lru hash owns this object now
		 */
		rspamd_lru_hash_insert (dkim_module_ctx->dkim_hash,
			g_strdup (rspamd_dkim_get_dns_key (ctx)),
			key, res->task->tv.tv_sec, rspamd_dkim_key_get_ttl (key));
		/* Another ref belongs to the check context */
		 res->key = rspamd_dkim_key_ref (key);
		/* Release key when task is processed */
		rspamd_mempool_add_destructor (res->task->task_pool,
				dkim_module_key_dtor, res->key);
	}
	else {
		/* Insert tempfail symbol */
		msg_info_task ("cannot get key for domain %s: %e",
				rspamd_dkim_get_dns_key (ctx), err);

		if (err != NULL) {
			res->res = DKIM_TRYAGAIN;
		}
	}

	if (err) {
		g_error_free (err);
	}

	dkim_module_check (res);
}

static void
dkim_symbol_callback (struct rspamd_task *task, void *unused)
{
	GList *hlist;
	rspamd_dkim_context_t *ctx;
	rspamd_dkim_key_t *key;
	GError *err = NULL;
	struct raw_header *rh;
	struct dkim_check_result *res = NULL, *cur;
	guint checked = 0;
	/* First check if a message has its signature */

	hlist = rspamd_message_get_header (task,
			DKIM_SIGNHEADER,
			FALSE);
	if (hlist != NULL) {
		if (task->user != NULL || rspamd_inet_address_is_local (task->from_addr)) {
			msg_info_task ("skip DKIM checks for local networks and authorized users");
			return;
		}
		/* Check whitelist */
		msg_debug_task ("dkim signature found");
		if (radix_find_compressed_addr (dkim_module_ctx->whitelist_ip,
				task->from_addr) == RADIX_NO_VALUE) {
			/* Parse signature */
			msg_debug_task ("create dkim signature");

			while (hlist != NULL) {
				rh = (struct raw_header *)hlist->data;

				if (rh->decoded == NULL || rh->decoded[0] == '\0') {
					msg_info_task ("<%s> cannot load empty DKIM context",
								task->message_id);
					hlist = g_list_next (hlist);
					continue;
				}

				if (res == NULL) {
					res = rspamd_mempool_alloc0 (task->task_pool, sizeof (*res));
					res->prev = res;
					res->w = rspamd_session_get_watcher (task->s);
					cur = res;
				}
				else {
					cur = rspamd_mempool_alloc0 (task->task_pool, sizeof (*res));
				}

				cur->first = res;
				cur->res = -1;
				cur->task = task;
				cur->mult_allow = 1.0;
				cur->mult_deny = 1.0;

				ctx = rspamd_create_dkim_context (rh->decoded,
						task->task_pool,
						dkim_module_ctx->time_jitter,
						&err);
				if (ctx == NULL) {
					if (err != NULL) {
						msg_info_task ("<%s> cannot parse DKIM context: %e",
								task->message_id, err);
						g_error_free (err);
						err = NULL;
					}
					else {
						msg_info_task ("<%s> cannot parse DKIM context: "
								"unknown error",
								task->message_id);
					}

					hlist = g_list_next (hlist);
					continue;
				}
				else {
					/* Get key */

					cur->ctx = ctx;

					if (dkim_module_ctx->trusted_only &&
							(dkim_module_ctx->dkim_domains == NULL ||
									g_hash_table_lookup (dkim_module_ctx->dkim_domains,
											rspamd_dkim_get_domain (ctx)) == NULL)) {
						msg_debug_task ("skip dkim check for %s domain",
								rspamd_dkim_get_domain (ctx));
						hlist = g_list_next (hlist);

						continue;
					}

					key = rspamd_lru_hash_lookup (dkim_module_ctx->dkim_hash,
							rspamd_dkim_get_dns_key (ctx),
							task->tv.tv_sec);

					if (key != NULL) {
						cur->key = rspamd_dkim_key_ref (key);
						/* Release key when task is processed */
						rspamd_mempool_add_destructor (task->task_pool,
								dkim_module_key_dtor, cur->key);
					}
					else {
						rspamd_get_dkim_key (ctx,
								task,
								dkim_module_key_handler,
								cur);
					}
				}

				if (res != cur) {
					DL_APPEND (res, cur);
				}

				if (dkim_module_ctx->skip_multi) {
					if (hlist->next) {
						msg_info_task ("message has multiple signatures but we"
								" check only one as 'skip_multi' is set");
					}

					break;
				}

				checked ++;

				if (checked > dkim_module_ctx->max_sigs) {
					msg_info_task ("message has multiple signatures but we"
							" stopped after %d checked signatures as limit"
							" is reached", checked);
					break;
				}

				hlist = g_list_next (hlist);
			}
		}
	}

	if (res != NULL) {
		rspamd_session_watcher_push (task->s);
		dkim_module_check (res);
	}
}

static void
dkim_sign_callback (struct rspamd_task *task, void *unused)
{
	lua_State *L;
	struct rspamd_task **ptask;
	gboolean sign = FALSE;
	gint err_idx;
	GString *tb, *hdr;
	GError *err = NULL;
	const gchar *selector = NULL, *domain = NULL, *key = NULL;
	rspamd_dkim_sign_context_t *ctx;
	rspamd_dkim_sign_key_t *dkim_key;

	if (dkim_module_ctx->sign_condition_ref != -1) {
		sign = FALSE;
		L = task->cfg->lua_state;
		lua_pushcfunction (L, &rspamd_lua_traceback);
		err_idx = lua_gettop (L);

		lua_rawgeti (L, LUA_REGISTRYINDEX, dkim_module_ctx->sign_condition_ref);
		ptask = lua_newuserdata (L, sizeof (struct rspamd_task *));
		*ptask = task;
		rspamd_lua_setclass (L, "rspamd{task}", -1);

		if (lua_pcall (L, 1, 1, err_idx) != 0) {
			tb = lua_touserdata (L, -1);
			msg_err_task ("call to user extraction script failed: %v", tb);
			g_string_free (tb, TRUE);
		}
		else {
			if (lua_istable (L, -1)) {
				/*
				 * Get the following elements:
				 * - selector
				 * - domain
				 * - key
				 */
				if (!rspamd_lua_parse_table_arguments (L, -1, &err,
						"*key=S;*domain=S;*selector=S",
						&key, &domain, &selector)) {
					msg_err_task ("invalid return value from sign condition: %e",
							err);
					g_error_free (err);

					return;
				}

				dkim_key = rspamd_lru_hash_lookup (dkim_module_ctx->dkim_sign_hash,
						key, time (NULL));

				if (dkim_key == NULL) {
					dkim_key = rspamd_dkim_sign_key_load (key, &err);

					if (dkim_key == NULL) {
						msg_err_task ("cannot load dkim key %s: %e",
								key, err);
						g_error_free (err);

						return;
					}

					rspamd_lru_hash_insert (dkim_module_ctx->dkim_sign_hash,
							g_strdup (key), dkim_key,
							time (NULL), 0);
				}

				ctx = rspamd_create_dkim_sign_context (task, dkim_key,
						DKIM_CANON_RELAXED, DKIM_CANON_RELAXED,
						dkim_module_ctx->sign_headers, &err);

				if (ctx == NULL) {
					msg_err_task ("cannot create sign context: %e",
							key, err);
					g_error_free (err);

					return;
				}

				hdr = rspamd_dkim_sign (task, selector, domain, 0, 0, ctx);

				if (hdr) {
					rspamd_mempool_set_variable (task->task_pool, "dkim-signature",
							hdr, rspamd_gstring_free_hard);
				}

				sign = TRUE;
			}
			else {
				sign = FALSE;
			}
		}

		/* Result + error function */
		lua_settop (L, 0);

		if (!sign) {
			msg_debug_task ("skip signing as dkim condition callback returned"
					" false");
			return;
		}
	}
}
