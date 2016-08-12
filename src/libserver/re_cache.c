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
#include "libmime/message.h"
#include "re_cache.h"
#include "cryptobox.h"
#include "ref.h"
#include "libserver/url.h"
#include "libserver/task.h"
#include "libserver/cfg_file.h"
#include "libutil/util.h"
#include "libutil/regexp.h"
#ifdef WITH_HYPERSCAN
#include "hs.h"
#include "unix-std.h"
#include <signal.h>

#ifndef WITH_PCRE2
#include <pcre.h>
#else
#include <pcre2.h>
#endif

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#endif

#define msg_err_re_cache(...) rspamd_default_log_function (G_LOG_LEVEL_CRITICAL, \
        "re_cache", cache->hash, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn_re_cache(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING, \
        "re_cache", cache->hash, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info_re_cache(...)   rspamd_default_log_function (G_LOG_LEVEL_INFO, \
        "re_cache", cache->hash, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_debug_re_cache(...)  rspamd_default_log_function (G_LOG_LEVEL_DEBUG, \
        "re_cache", cache->hash, \
        G_STRFUNC, \
        __VA_ARGS__)

#ifdef WITH_HYPERSCAN
#define RSPAMD_HS_MAGIC_LEN (sizeof (rspamd_hs_magic))
static const guchar rspamd_hs_magic[] = {'r', 's', 'h', 's', 'r', 'e', '1', '1'},
		rspamd_hs_magic_vector[] = {'r', 's', 'h', 's', 'r', 'v', '1', '1'};
#endif

struct rspamd_re_class {
	guint64 id;
	enum rspamd_re_type type;
	gpointer type_data;
	gsize type_len;
	GHashTable *re;
	gchar hash[rspamd_cryptobox_HASHBYTES + 1];
	rspamd_cryptobox_hash_state_t *st;
#ifdef WITH_HYPERSCAN
	hs_database_t *hs_db;
	hs_scratch_t *hs_scratch;
	gint *hs_ids;
	guint nhs;
#endif
};

enum rspamd_re_cache_elt_match_type {
	RSPAMD_RE_CACHE_PCRE = 0,
	RSPAMD_RE_CACHE_HYPERSCAN,
	RSPAMD_RE_CACHE_HYPERSCAN_PRE
};

struct rspamd_re_cache_elt {
	rspamd_regexp_t *re;
	enum rspamd_re_cache_elt_match_type match_type;
};

struct rspamd_re_cache {
	GHashTable *re_classes;
	GPtrArray *re;
	ref_entry_t ref;
	guint nre;
	guint max_re_data;
	gchar hash[rspamd_cryptobox_HASHBYTES + 1];
#ifdef WITH_HYPERSCAN
	gboolean hyperscan_loaded;
	gboolean disable_hyperscan;
	gboolean vectorized_hyperscan;
	hs_platform_info_t plt;
#endif
};

struct rspamd_re_runtime {
	guchar *checked;
	guchar *results;
	struct rspamd_re_cache *cache;
	struct rspamd_re_cache_stat stat;
	gboolean has_hs;
};

static GQuark
rspamd_re_cache_quark (void)
{
	return g_quark_from_static_string ("re_cache");
}

static guint64
rspamd_re_cache_class_id (enum rspamd_re_type type,
		gpointer type_data,
		gsize datalen)
{
	rspamd_cryptobox_fast_hash_state_t st;

	rspamd_cryptobox_fast_hash_init (&st, 0xdeadbabe);
	rspamd_cryptobox_fast_hash_update (&st, &type, sizeof (type));

	if (datalen > 0) {
		rspamd_cryptobox_fast_hash_update (&st, type_data, datalen);
	}

	return rspamd_cryptobox_fast_hash_final (&st);
}

static void
rspamd_re_cache_destroy (struct rspamd_re_cache *cache)
{
	GHashTableIter it;
	gpointer k, v;
	struct rspamd_re_class *re_class;

	g_assert (cache != NULL);
	g_hash_table_iter_init (&it, cache->re_classes);

	while (g_hash_table_iter_next (&it, &k, &v)) {
		re_class = v;
		g_hash_table_iter_steal (&it);
		g_hash_table_unref (re_class->re);
#ifdef WITH_HYPERSCAN
		if (re_class->hs_db) {
			hs_free_database (re_class->hs_db);
		}
		if (re_class->hs_scratch) {
			hs_free_scratch (re_class->hs_scratch);
		}
		if (re_class->hs_ids) {
			g_free (re_class->hs_ids);
		}
#endif
		g_slice_free1 (sizeof (*re_class), re_class);
	}

	g_hash_table_unref (cache->re_classes);
	g_ptr_array_free (cache->re, TRUE);
	g_slice_free1 (sizeof (*cache), cache);
}

static void
rspamd_re_cache_elt_dtor (gpointer e)
{
	struct rspamd_re_cache_elt *elt = e;

	rspamd_regexp_unref (elt->re);
	g_slice_free1 (sizeof (*elt), elt);
}

struct rspamd_re_cache *
rspamd_re_cache_new (void)
{
	struct rspamd_re_cache *cache;

	cache = g_slice_alloc (sizeof (*cache));
	cache->re_classes = g_hash_table_new (g_int64_hash, g_int64_equal);
	cache->nre = 0;
	cache->re = g_ptr_array_new_full (256, rspamd_re_cache_elt_dtor);
#ifdef WITH_HYPERSCAN
	cache->hyperscan_loaded = FALSE;
#endif
	REF_INIT_RETAIN (cache, rspamd_re_cache_destroy);

	return cache;
}

gboolean
rspamd_re_cache_is_hs_loaded (struct rspamd_re_cache *cache)
{
	g_assert (cache != NULL);

#ifdef WITH_HYPERSCAN
	return cache->hyperscan_loaded;
#else
	return FALSE;
#endif
}

rspamd_regexp_t *
rspamd_re_cache_add (struct rspamd_re_cache *cache, rspamd_regexp_t *re,
		enum rspamd_re_type type, gpointer type_data, gsize datalen)
{
	guint64 class_id;
	struct rspamd_re_class *re_class;
	rspamd_regexp_t *nre;
	struct rspamd_re_cache_elt *elt;

	g_assert (cache != NULL);
	g_assert (re != NULL);

	class_id = rspamd_re_cache_class_id (type, type_data, datalen);
	re_class = g_hash_table_lookup (cache->re_classes, &class_id);

	if (re_class == NULL) {
		re_class = g_slice_alloc0 (sizeof (*re_class));
		re_class->id = class_id;
		re_class->type_len = datalen;
		re_class->type = type;
		re_class->re = g_hash_table_new_full (rspamd_regexp_hash,
				rspamd_regexp_equal, NULL, (GDestroyNotify)rspamd_regexp_unref);

		if (datalen > 0) {
			re_class->type_data = g_slice_alloc (datalen);
			memcpy (re_class->type_data, type_data, datalen);
		}

		g_hash_table_insert (cache->re_classes, &re_class->id, re_class);
	}

	if ((nre = g_hash_table_lookup (re_class->re, rspamd_regexp_get_id (re)))
			== NULL) {
		/*
		 * We set re id based on the global position in the cache
		 */
		elt = g_slice_alloc0 (sizeof (*elt));
		/* One ref for re_class */
		nre = rspamd_regexp_ref (re);
		rspamd_regexp_set_cache_id (re, cache->nre++);
		/* One ref for cache */
		elt->re = rspamd_regexp_ref (re);
		g_ptr_array_add (cache->re, elt);
		rspamd_regexp_set_class (re, re_class);
		g_hash_table_insert (re_class->re, rspamd_regexp_get_id (nre), nre);
	}

	return nre;
}

void
rspamd_re_cache_replace (struct rspamd_re_cache *cache,
		rspamd_regexp_t *what,
		rspamd_regexp_t *with)
{
	guint64 re_id;
	struct rspamd_re_class *re_class;
	rspamd_regexp_t *src;
	struct rspamd_re_cache_elt *elt;

	g_assert (cache != NULL);
	g_assert (what != NULL);
	g_assert (with != NULL);

	re_class = rspamd_regexp_get_class (what);

	if (re_class != NULL) {
		re_id = rspamd_regexp_get_cache_id (what);

		g_assert (re_id != RSPAMD_INVALID_ID);
		src = g_hash_table_lookup (re_class->re, rspamd_regexp_get_id (what));
		elt = g_ptr_array_index (cache->re, re_id);
		g_assert (elt != NULL);
		g_assert (src != NULL);

		rspamd_regexp_set_cache_id (what, RSPAMD_INVALID_ID);
		rspamd_regexp_set_class (what, NULL);
		rspamd_regexp_set_cache_id (with, re_id);
		rspamd_regexp_set_class (with, re_class);
		/*
		 * On calling of this function, we actually unref old re (what)
		 */
		g_hash_table_insert (re_class->re,
				rspamd_regexp_get_id (what),
				rspamd_regexp_ref (with));

		rspamd_regexp_unref (elt->re);
		elt->re = rspamd_regexp_ref (with);
		/* XXX: do not touch match type here */
	}
}

static gint
rspamd_re_cache_sort_func (gconstpointer a, gconstpointer b)
{
	struct rspamd_re_cache_elt * const *re1 = a, * const *re2 = b;

	return rspamd_regexp_cmp (rspamd_regexp_get_id ((*re1)->re),
			rspamd_regexp_get_id ((*re2)->re));
}

void
rspamd_re_cache_init (struct rspamd_re_cache *cache, struct rspamd_config *cfg)
{
	guint i, fl;
	GHashTableIter it;
	gpointer k, v;
	struct rspamd_re_class *re_class;
	rspamd_cryptobox_hash_state_t st_global;
	rspamd_regexp_t *re;
	struct rspamd_re_cache_elt *elt;
	guchar hash_out[rspamd_cryptobox_HASHBYTES];

	g_assert (cache != NULL);

	rspamd_cryptobox_hash_init (&st_global, NULL, 0);
	/* Resort all regexps */
	g_ptr_array_sort (cache->re, rspamd_re_cache_sort_func);

	for (i = 0; i < cache->re->len; i ++) {
		elt = g_ptr_array_index (cache->re, i);
		re = elt->re;
		re_class = rspamd_regexp_get_class (re);
		g_assert (re_class != NULL);
		rspamd_regexp_set_cache_id (re, i);

		if (re_class->st == NULL) {
			re_class->st = g_slice_alloc (sizeof (*re_class->st));
			rspamd_cryptobox_hash_init (re_class->st, NULL, 0);
		}

		/* Update hashes */
		rspamd_cryptobox_hash_update (re_class->st, (gpointer) &re_class->id,
				sizeof (re_class->id));
		rspamd_cryptobox_hash_update (&st_global, (gpointer) &re_class->id,
				sizeof (re_class->id));
		rspamd_cryptobox_hash_update (re_class->st, rspamd_regexp_get_id (re),
				rspamd_cryptobox_HASHBYTES);
		rspamd_cryptobox_hash_update (&st_global, rspamd_regexp_get_id (re),
				rspamd_cryptobox_HASHBYTES);
		fl = rspamd_regexp_get_pcre_flags (re);
		rspamd_cryptobox_hash_update (re_class->st, (const guchar *)&fl,
				sizeof (fl));
		rspamd_cryptobox_hash_update (&st_global, (const guchar *) &fl,
				sizeof (fl));
		fl = rspamd_regexp_get_flags (re);
		rspamd_cryptobox_hash_update (re_class->st, (const guchar *) &fl,
				sizeof (fl));
		rspamd_cryptobox_hash_update (&st_global, (const guchar *) &fl,
				sizeof (fl));
		fl = rspamd_regexp_get_maxhits (re);
		rspamd_cryptobox_hash_update (re_class->st, (const guchar *) &fl,
				sizeof (fl));
		rspamd_cryptobox_hash_update (&st_global, (const guchar *) &fl,
				sizeof (fl));
	}

	rspamd_cryptobox_hash_final (&st_global, hash_out);
	rspamd_snprintf (cache->hash, sizeof (cache->hash), "%*xs",
			(gint) rspamd_cryptobox_HASHBYTES, hash_out);

	/* Now finalize all classes */
	g_hash_table_iter_init (&it, cache->re_classes);

	while (g_hash_table_iter_next (&it, &k, &v)) {
		re_class = v;

		if (re_class->st) {
			/*
			 * We finally update all classes with the number of expressions
			 * in the cache to ensure that if even a single re has been changed
			 * we won't be broken due to id mismatch
			 */
			rspamd_cryptobox_hash_update (re_class->st,
					(gpointer)&cache->re->len,
					sizeof (cache->re->len));
			rspamd_cryptobox_hash_final (re_class->st, hash_out);
			rspamd_snprintf (re_class->hash, sizeof (re_class->hash), "%*xs",
					(gint) rspamd_cryptobox_HASHBYTES, hash_out);
			g_slice_free1 (sizeof (*re_class->st), re_class->st);
			re_class->st = NULL;
		}
	}

#ifdef WITH_HYPERSCAN
	const gchar *platform = "generic";
	rspamd_fstring_t *features = rspamd_fstring_new ();

	cache->disable_hyperscan = cfg->disable_hyperscan;
	cache->vectorized_hyperscan = cfg->vectorized_hyperscan;

	g_assert (hs_populate_platform (&cache->plt) == HS_SUCCESS);

	/* Now decode what we do have */
	switch (cache->plt.tune) {
	case HS_TUNE_FAMILY_HSW:
		platform = "haswell";
		break;
	case HS_TUNE_FAMILY_SNB:
		platform = "sandy";
		break;
	case HS_TUNE_FAMILY_BDW:
		platform = "broadwell";
		break;
	case HS_TUNE_FAMILY_IVB:
		platform = "ivy";
		break;
	default:
		break;
	}

	if (cache->plt.cpu_features & HS_CPU_FEATURES_AVX2) {
		features = rspamd_fstring_append (features, "AVX2", 4);
	}

	hs_set_allocator (g_malloc, g_free);

	msg_info_re_cache ("loaded hyperscan engine witch cpu tune '%s' and features '%V'",
			platform, features);

	rspamd_fstring_free (features);
#endif
}

struct rspamd_re_runtime *
rspamd_re_cache_runtime_new (struct rspamd_re_cache *cache)
{
	struct rspamd_re_runtime *rt;
	g_assert (cache != NULL);

	rt = g_slice_alloc0 (sizeof (*rt));
	rt->cache = cache;
	REF_RETAIN (cache);
	rt->checked = g_slice_alloc0 (NBYTES (cache->nre));
	rt->results = g_slice_alloc0 (cache->nre);
	rt->stat.regexp_total = cache->nre;
#ifdef WITH_HYPERSCAN
	rt->has_hs = cache->hyperscan_loaded;
#endif

	return rt;
}

const struct rspamd_re_cache_stat *
rspamd_re_cache_get_stat (struct rspamd_re_runtime *rt)
{
	g_assert (rt != NULL);

	return &rt->stat;
}

static guint
rspamd_re_cache_process_pcre (struct rspamd_re_runtime *rt,
		rspamd_regexp_t *re, rspamd_mempool_t *pool,
		const guchar *in, gsize len,
		gboolean is_raw)
{
	guint r = 0;
	const gchar *start = NULL, *end = NULL;
	guint max_hits = rspamd_regexp_get_maxhits (re);
	guint64 id = rspamd_regexp_get_cache_id (re);
	gdouble t1, t2, pr;
	const gdouble slow_time = 0.1;

	if (in == NULL) {
		return rt->results[id];
	}

	if (len == 0) {
		len = strlen (in);
	}

	if (rt->cache->max_re_data > 0 && len > rt->cache->max_re_data) {
		len = rt->cache->max_re_data;
	}

	r = rt->results[id];

	if (max_hits == 0 || r < max_hits) {
		pr = rspamd_random_double_fast ();

		if (pr > 0.9) {
			t1 = rspamd_get_ticks ();
		}

		while (rspamd_regexp_search (re,
				in,
				len,
				&start,
				&end,
				is_raw,
				NULL)) {
			r++;

			if (max_hits > 0 && r >= max_hits) {
				break;
			}
		}

		rt->stat.regexp_checked++;
		rt->stat.bytes_scanned_pcre += len;
		rt->stat.bytes_scanned += len;

		if (r > 0) {
			rt->stat.regexp_matched += r;
		}

		if (pr > 0.9) {
			t2 = rspamd_get_ticks ();

			if (t2 - t1 > slow_time) {
				msg_info_pool ("regexp '%16s' took %.2f seconds to execute",
						rspamd_regexp_get_pattern (re), t2 - t1);
			}
		}
	}

	return r;
}

#ifdef WITH_HYPERSCAN
struct rspamd_re_hyperscan_cbdata {
	struct rspamd_re_runtime *rt;
	const guchar **ins;
	const guint *lens;
	guint count;
	rspamd_regexp_t *re;
	rspamd_mempool_t *pool;
};

static gint
rspamd_re_cache_hyperscan_cb (unsigned int id,
		unsigned long long from,
		unsigned long long to,
		unsigned int flags,
		void *ud)
{
	struct rspamd_re_hyperscan_cbdata *cbdata = ud;
	struct rspamd_re_runtime *rt;
	struct rspamd_re_cache_elt *pcre_elt;
	guint ret, maxhits, i, processed;

	rt = cbdata->rt;
	pcre_elt = g_ptr_array_index (rt->cache->re, id);
	maxhits = rspamd_regexp_get_maxhits (pcre_elt->re);

	if (pcre_elt->match_type == RSPAMD_RE_CACHE_HYPERSCAN) {
		ret = 1;
		setbit (rt->checked, id);

		if (maxhits == 0 || rt->results[id] < maxhits) {
			rt->results[id] += ret;
			rt->stat.regexp_matched++;
		}
	}
	else {
		if (!isset (rt->checked, id)) {

			processed = 0;

			for (i = 0; i < cbdata->count; i ++) {
				ret = rspamd_re_cache_process_pcre (rt,
						pcre_elt->re,
						cbdata->pool,
						cbdata->ins[i],
						cbdata->lens[i],
						FALSE);
				rt->results[id] = ret;
				setbit (rt->checked, id);

				processed += cbdata->lens[i];

				if (processed >= to) {
					break;
				}
			}
		}
	}

	return 0;
}
#endif

static guint
rspamd_re_cache_process_regexp_data (struct rspamd_re_runtime *rt,
		rspamd_regexp_t *re, rspamd_mempool_t *pool,
		const guchar **in, guint *lens,
		guint count,
		gboolean is_raw)
{

	guint64 re_id;
	guint ret = 0;
	guint i;

	re_id = rspamd_regexp_get_cache_id (re);

	if (count == 0 || in == NULL) {
		/* We assume this as absence of the specified data */
		setbit (rt->checked, re_id);
		rt->results[re_id] = ret;
		return ret;
	}

#ifndef WITH_HYPERSCAN
	for (i = 0; i < count; i++) {
		ret = rspamd_re_cache_process_pcre (rt,
				re,
				pool,
				in[i],
				lens[i],
				is_raw);
		rt->results[re_id] = ret;
	}

	setbit (rt->checked, re_id);
#else
	struct rspamd_re_cache_elt *elt;
	struct rspamd_re_class *re_class;
	struct rspamd_re_hyperscan_cbdata cbdata;

	elt = g_ptr_array_index (rt->cache->re, re_id);
	re_class = rspamd_regexp_get_class (re);

	if (rt->cache->disable_hyperscan || elt->match_type == RSPAMD_RE_CACHE_PCRE ||
			!rt->has_hs) {
		for (i = 0; i < count; i++) {
			ret = rspamd_re_cache_process_pcre (rt,
					re,
					pool,
					in[i],
					lens[i],
					is_raw);
			rt->results[re_id] = ret;
		}

		setbit (rt->checked, re_id);
	}
	else {
		for (i = 0; i < count; i ++) {
			if (rt->cache->max_re_data > 0 && lens[i] > rt->cache->max_re_data) {
				lens[i] = rt->cache->max_re_data;
			}
			rt->stat.bytes_scanned += lens[i];
		}

		g_assert (re_class->hs_scratch != NULL);
		g_assert (re_class->hs_db != NULL);

		/* Go through hyperscan API */
		if (!rt->cache->vectorized_hyperscan) {
			for (i = 0; i < count; i++) {
				cbdata.ins = &in[i];
				cbdata.re = re;
				cbdata.rt = rt;
				cbdata.lens = &lens[i];
				cbdata.count = 1;
				cbdata.pool = pool;

				if ((hs_scan (re_class->hs_db, in[i], lens[i], 0,
						re_class->hs_scratch,
						rspamd_re_cache_hyperscan_cb, &cbdata)) != HS_SUCCESS) {
					ret = 0;
				}
				else {
					ret = rt->results[re_id];
				}
			}
		}
		else {
			cbdata.ins = in;
			cbdata.re = re;
			cbdata.rt = rt;
			cbdata.lens = lens;
			cbdata.count = 1;
			cbdata.pool = pool;

			if ((hs_scan_vector (re_class->hs_db, (const char **)in, lens, count, 0,
					re_class->hs_scratch,
					rspamd_re_cache_hyperscan_cb, &cbdata)) != HS_SUCCESS) {
				ret = 0;
			}
			else {
				ret = rt->results[re_id];
			}
		}
	}
#endif

	return ret;
}

static void
rspamd_re_cache_finish_class (struct rspamd_re_runtime *rt,
		struct rspamd_re_class *re_class)
{
#ifdef WITH_HYPERSCAN
	guint i;
	guint64 re_id;

	/* Set all bits unchecked */
	for (i = 0; i < re_class->nhs; i++) {
		re_id = re_class->hs_ids[i];

		if (!isset (rt->checked, re_id)) {
			g_assert (rt->results[re_id] == 0);
			rt->results[re_id] = 0;
			setbit (rt->checked, re_id);
		}
	}
#endif
}

/*
 * Calculates the specified regexp for the specified class if it's not calculated
 */
static guint
rspamd_re_cache_exec_re (struct rspamd_task *task,
		struct rspamd_re_runtime *rt,
		rspamd_regexp_t *re,
		struct rspamd_re_class *re_class,
		gboolean is_strong)
{
	guint ret = 0, i, re_id;
	GPtrArray *headerlist;
	GList *slist;
	GHashTableIter it;
	struct raw_header *rh;
	const gchar *in, *end;
	const guchar **scvec;
	guint *lenvec;
	gboolean raw = FALSE;
	struct rspamd_mime_text_part *part;
	struct rspamd_url *url;
	struct rspamd_re_cache *cache = rt->cache;
	gpointer k, v;
	guint len, cnt;

	msg_debug_re_cache ("get to the slow path for re type: %s: %s",
			rspamd_re_cache_type_to_string (re_class->type),
			rspamd_regexp_get_pattern (re));
	re_id = rspamd_regexp_get_cache_id (re);

	switch (re_class->type) {
	case RSPAMD_RE_HEADER:
	case RSPAMD_RE_RAWHEADER:
		/* Get list of specified headers */
		headerlist = rspamd_message_get_header_array (task,
				re_class->type_data,
				is_strong);

		if (headerlist && headerlist->len > 0) {
			scvec = g_malloc (sizeof (*scvec) * headerlist->len);
			lenvec = g_malloc (sizeof (*lenvec) * headerlist->len);

			for (i = 0; i < headerlist->len; i ++) {
				rh = g_ptr_array_index (headerlist, i);

				if (re_class->type == RSPAMD_RE_RAWHEADER) {
					in = rh->value;
					raw = TRUE;
					lenvec[i] = strlen (rh->value);
				}
				else {
					in = rh->decoded;
					/* Validate input */
					if (!in || !g_utf8_validate (in, -1, &end)) {
						lenvec[i] = 0;
						scvec[i] = (guchar *)"";
						continue;
					}
					lenvec[i] = end - in;
				}

				scvec[i] = (guchar *)in;
			}

			ret = rspamd_re_cache_process_regexp_data (rt, re,
					task->task_pool, scvec, lenvec, headerlist->len, raw);
			debug_task ("checking header %s regexp: %s -> %d",
					re_class->type_data,
					rspamd_regexp_get_pattern (re), ret);
			g_free (scvec);
			g_free (lenvec);
		}
		break;
	case RSPAMD_RE_ALLHEADER:
		raw = TRUE;
		in = task->raw_headers_content.begin;
		len = task->raw_headers_content.len;
		ret = rspamd_re_cache_process_regexp_data (rt, re,
				task->task_pool, (const guchar **)&in, &len, 1, raw);
		debug_task ("checking allheader regexp: %s -> %d",
				rspamd_regexp_get_pattern (re), ret);
		break;
	case RSPAMD_RE_MIMEHEADER:
		headerlist = rspamd_message_get_mime_header_array (task,
				re_class->type_data,
				is_strong);

		if (headerlist && headerlist->len > 0) {
			scvec = g_malloc (sizeof (*scvec) * headerlist->len);
			lenvec = g_malloc (sizeof (*lenvec) * headerlist->len);

			for (i = 0; i < headerlist->len; i ++) {
				rh = g_ptr_array_index (headerlist, i);

				if (re_class->type == RSPAMD_RE_RAWHEADER) {
					in = rh->value;
					raw = TRUE;
					lenvec[i] = strlen (rh->value);
				}
				else {
					in = rh->decoded;
					/* Validate input */
					if (!in || !g_utf8_validate (in, -1, &end)) {
						lenvec[i] = 0;
						scvec[i] = (guchar *)"";
						continue;
					}
					lenvec[i] = end - in;
				}

				scvec[i] = (guchar *)in;
			}

			ret = rspamd_re_cache_process_regexp_data (rt, re,
					task->task_pool, scvec, lenvec, headerlist->len, raw);
			debug_task ("checking mime header %s regexp: %s -> %d",
					re_class->type_data,
					rspamd_regexp_get_pattern (re), ret);
			g_free (scvec);
			g_free (lenvec);
		}
		break;
	case RSPAMD_RE_MIME:
	case RSPAMD_RE_RAWMIME:
		/* Iterate through text parts */
		if (task->text_parts->len > 0) {
			cnt = task->text_parts->len;
			scvec = g_malloc (sizeof (*scvec) * cnt);
			lenvec = g_malloc (sizeof (*lenvec) * cnt);

			for (i = 0; i < task->text_parts->len; i++) {
				part = g_ptr_array_index (task->text_parts, i);

				/* Skip empty parts */
				if (IS_PART_EMPTY (part)) {
					lenvec[i] = 0;
					scvec[i] = (guchar *) "";
					continue;
				}

				/* Check raw flags */
				if (!IS_PART_UTF (part)) {
					raw = TRUE;
				}
				/* Select data for regexp */
				if (re_class->type == RSPAMD_RE_RAWMIME) {
					in = part->orig->data;
					len = part->orig->len;
					raw = TRUE;
				}
				else {
					in = part->content->data;
					len = part->content->len;
				}

				scvec[i] = (guchar *) in;
				lenvec[i] = len;
			}

			ret = rspamd_re_cache_process_regexp_data (rt, re,
					task->task_pool, scvec, lenvec, cnt, raw);
			debug_task ("checking mime regexp: %s -> %d",
					rspamd_regexp_get_pattern (re), ret);
			g_free (scvec);
			g_free (lenvec);
		}
		break;
	case RSPAMD_RE_URL:
		cnt = g_hash_table_size (task->urls) + g_hash_table_size (task->emails);

		if (cnt > 0) {
			scvec = g_malloc (sizeof (*scvec) * cnt);
			lenvec = g_malloc (sizeof (*lenvec) * cnt);
			g_hash_table_iter_init (&it, task->urls);
			i = 0;

			while (g_hash_table_iter_next (&it, &k, &v)) {
				url = v;
				in = url->string;
				len = url->urllen;
				raw = FALSE;

				scvec[i] = (guchar *)in;
				lenvec[i++] = len;
			}

			g_hash_table_iter_init (&it, task->emails);

			while (g_hash_table_iter_next (&it, &k, &v)) {
				url = v;
				in = url->string;
				len = url->urllen;
				raw = FALSE;

				scvec[i] = (guchar *) in;
				lenvec[i++] = len;
			}

			g_assert (i == cnt);

			ret = rspamd_re_cache_process_regexp_data (rt, re,
					task->task_pool, scvec, lenvec, i, raw);
			debug_task ("checking url regexp: %s -> %d",
					rspamd_regexp_get_pattern (re), ret);
			g_free (scvec);
			g_free (lenvec);
		}
		break;
	case RSPAMD_RE_BODY:
		raw = TRUE;
		in = task->msg.begin;
		len = task->msg.len;

		ret = rspamd_re_cache_process_regexp_data (rt, re, task->task_pool,
				(const guchar **)&in, &len, 1, raw);
		debug_task ("checking rawbody regexp: %s -> %d",
				rspamd_regexp_get_pattern (re), ret);
		break;
	case RSPAMD_RE_SABODY:
		/* According to SA docs:
		 * The 'body' in this case is the textual parts of the message body;
		 * any non-text MIME parts are stripped, and the message decoded from
		 * Quoted-Printable or Base-64-encoded format if necessary. The message
		 * Subject header is considered part of the body and becomes the first
		 * paragraph when running the rules. All HTML tags and line breaks will
		 * be removed before matching.
		 */
		cnt = task->text_parts->len + 1;
		scvec = g_malloc (sizeof (*scvec) * cnt);
		lenvec = g_malloc (sizeof (*lenvec) * cnt);

		/*
		 * Body rules also include the Subject as the first line
		 * of the body content.
		 */

		slist = rspamd_message_get_header (task, "Subject", FALSE);

		if (slist) {
			rh = slist->data;

			scvec[0] = (guchar *)rh->decoded;
			lenvec[0] = strlen (rh->decoded);
		}
		else {
			scvec[0] = (guchar *)"";
			lenvec[0] = 0;
		}
		for (i = 0; i < task->text_parts->len; i++) {
			part = g_ptr_array_index (task->text_parts, i);

			if (part->stripped_content) {
				scvec[i + 1] = (guchar *)part->stripped_content->data;
				lenvec[i + 1] = part->stripped_content->len;
			}
			else {
				scvec[i + 1] = (guchar *)"";
				lenvec[i + 1] = 0;
			}
		}

		ret = rspamd_re_cache_process_regexp_data (rt, re,
				task->task_pool, scvec, lenvec, cnt, TRUE);
		debug_task ("checking sa body regexp: %s -> %d",
				rspamd_regexp_get_pattern (re), ret);
		g_free (scvec);
		g_free (lenvec);
		break;
	case RSPAMD_RE_SARAWBODY:
		/* According to SA docs:
		 * The 'raw body' of a message is the raw data inside all textual
		 * parts. The text will be decoded from base64 or quoted-printable
		 * encoding, but HTML tags and line breaks will still be present.
		 * Multiline expressions will need to be used to match strings that are
		 * broken by line breaks.
		 */
		if (task->text_parts->len > 0) {
			cnt = task->text_parts->len;
			scvec = g_malloc (sizeof (*scvec) * cnt);
			lenvec = g_malloc (sizeof (*lenvec) * cnt);

			for (i = 0; i < task->text_parts->len; i++) {
				part = g_ptr_array_index (task->text_parts, i);

				if (part->orig) {
					scvec[i] = (guchar *)part->orig->data;
					lenvec[i] = part->orig->len;
				}
				else {
					scvec[i] = (guchar *)"";
					lenvec[i] = 0;
				}
			}

			ret = rspamd_re_cache_process_regexp_data (rt, re,
					task->task_pool, scvec, lenvec, cnt, TRUE);
			debug_task ("checking sa rawbody regexp: %s -> %d",
					rspamd_regexp_get_pattern (re), ret);
			g_free (scvec);
			g_free (lenvec);
		}
		break;
	case RSPAMD_RE_MAX:
		msg_err_task ("regexp of class invalid has been called: %s",
				rspamd_regexp_get_pattern (re));
		break;
	}

#if WITH_HYPERSCAN
	if (!rt->cache->disable_hyperscan && rt->has_hs) {
		rspamd_re_cache_finish_class (rt, re_class);
	}
#endif

	setbit (rt->checked, re_id);

	return rt->results[re_id];
}

gint
rspamd_re_cache_process (struct rspamd_task *task,
		struct rspamd_re_runtime *rt,
		rspamd_regexp_t *re,
		enum rspamd_re_type type,
		gpointer type_data,
		gsize datalen,
		gboolean is_strong)
{
	guint64 re_id;
	struct rspamd_re_class *re_class;
	struct rspamd_re_cache *cache;

	g_assert (rt != NULL);
	g_assert (task != NULL);
	g_assert (re != NULL);

	cache = rt->cache;
	re_id = rspamd_regexp_get_cache_id (re);

	if (re_id == RSPAMD_INVALID_ID || re_id > cache->nre) {
		msg_err_task ("re '%s' has no valid id for the cache",
				rspamd_regexp_get_pattern (re));
		return 0;
	}

	if (isset (rt->checked, re_id)) {
		/* Fast path */
		rt->stat.regexp_fast_cached ++;
		return rt->results[re_id];
	}
	else {
		/* Slow path */
		re_class = rspamd_regexp_get_class (re);

		if (re_class == NULL) {
			msg_err_task ("cannot find re class for regexp '%s'",
					rspamd_regexp_get_pattern (re));
			return 0;
		}

		return rspamd_re_cache_exec_re (task, rt, re, re_class,
				is_strong);
	}

	return 0;
}

void
rspamd_re_cache_runtime_destroy (struct rspamd_re_runtime *rt)
{
	g_assert (rt != NULL);

	g_slice_free1 (NBYTES (rt->cache->nre), rt->checked);
	g_slice_free1 (rt->cache->nre, rt->results);
	REF_RELEASE (rt->cache);
	g_slice_free1 (sizeof (*rt), rt);
}

void
rspamd_re_cache_unref (struct rspamd_re_cache *cache)
{
	if (cache) {
		REF_RELEASE (cache);
	}
}

struct rspamd_re_cache *
rspamd_re_cache_ref (struct rspamd_re_cache *cache)
{
	if (cache) {
		REF_RETAIN (cache);
	}

	return cache;
}

guint
rspamd_re_cache_set_limit (struct rspamd_re_cache *cache, guint limit)
{
	guint old;

	g_assert (cache != NULL);

	old = cache->max_re_data;
	cache->max_re_data = limit;

	return old;
}

const gchar *
rspamd_re_cache_type_to_string (enum rspamd_re_type type)
{
	const gchar *ret = "unknown";

	switch (type) {
	case RSPAMD_RE_HEADER:
		ret = "header";
		break;
	case RSPAMD_RE_RAWHEADER:
		ret = "raw header";
		break;
	case RSPAMD_RE_MIMEHEADER:
		ret = "mime header";
		break;
	case RSPAMD_RE_ALLHEADER:
		ret = "all headers";
		break;
	case RSPAMD_RE_MIME:
		ret = "part";
		break;
	case RSPAMD_RE_RAWMIME:
		ret = "raw part";
		break;
	case RSPAMD_RE_BODY:
		ret = "rawbody";
		break;
	case RSPAMD_RE_URL:
		ret = "url";
		break;
	case RSPAMD_RE_SABODY:
		ret = "sa body";
		break;
	case RSPAMD_RE_SARAWBODY:
		ret = "sa body";
		break;
	case RSPAMD_RE_MAX:
		ret = "invalid class";
		break;
	}

	return ret;
}

enum rspamd_re_type
rspamd_re_cache_type_from_string (const char *str)
{
	enum rspamd_re_type ret;
	guint64 h;

	/*
	 * To optimize this function, we apply hash to input string and
	 * pre-select it from the values
	 */

	if (str != NULL) {
		h = rspamd_cryptobox_fast_hash_specific (RSPAMD_CRYPTOBOX_XXHASH64,
				str, strlen (str), 0xdeadbabe);

		switch (h) {
		case G_GUINT64_CONSTANT(0x298b9c8a58887d44): /* header */
			ret = RSPAMD_RE_HEADER;
			break;
		case G_GUINT64_CONSTANT(0x467bfb5cd7ddf890): /* rawheader */
			ret = RSPAMD_RE_RAWHEADER;
			break;
		case G_GUINT64_CONSTANT(0xda081341fb600389): /* mime */
			ret = RSPAMD_RE_MIME;
			break;
		case G_GUINT64_CONSTANT(0xc35831e067a8221d): /* rawmime */
			ret = RSPAMD_RE_RAWMIME;
			break;
		case G_GUINT64_CONSTANT(0xc625e13dbe636de2): /* body */
		case G_GUINT64_CONSTANT(0xCCDEBA43518F721C): /* message */
			ret = RSPAMD_RE_BODY;
			break;
		case G_GUINT64_CONSTANT(0x286edbe164c791d2): /* url */
		case G_GUINT64_CONSTANT(0x7D9ACDF6685661A1): /* uri */
			ret = RSPAMD_RE_URL;
			break;
		case G_GUINT64_CONSTANT(0x796d62205a8778c7): /* allheader */
			ret = RSPAMD_RE_ALLHEADER;
			break;
		case G_GUINT64_CONSTANT(0xa3c6c153b3b00a5e): /* mimeheader */
			ret = RSPAMD_RE_MIMEHEADER;
			break;
		case G_GUINT64_CONSTANT(0x7794501506e604e9): /* sabody */
			ret = RSPAMD_RE_SABODY;
			break;
		case G_GUINT64_CONSTANT(0x28828962E7D2A05F): /* sarawbody */
			ret = RSPAMD_RE_SARAWBODY;
			break;
		default:
			ret = RSPAMD_RE_MAX;
			break;
		}
	}
	else {
		ret = RSPAMD_RE_MAX;
	}

	return ret;
}

#ifdef WITH_HYPERSCAN
static gboolean
rspamd_re_cache_is_finite (struct rspamd_re_cache *cache,
		rspamd_regexp_t *re, gint flags, gdouble max_time)
{
	pid_t cld;
	gint status;
	struct timespec ts;
	hs_compile_error_t *hs_errors;
	hs_database_t *test_db;
	gdouble wait_time;
	const gint max_tries = 10;
	gint tries = 0, rc;

	wait_time = max_time / max_tries;
	/* We need to restore SIGCHLD processing */
	signal (SIGCHLD, SIG_DFL);
	cld = fork ();
	g_assert (cld != -1);

	if (cld == 0) {
		/* Try to compile pattern */
		if (hs_compile (rspamd_regexp_get_pattern (re),
				flags | HS_FLAG_PREFILTER,
				cache->vectorized_hyperscan ? HS_MODE_VECTORED : HS_MODE_BLOCK,
				&cache->plt,
				&test_db,
				&hs_errors) != HS_SUCCESS) {
			exit (EXIT_FAILURE);
		}

		exit (EXIT_SUCCESS);
	}
	else {
		double_to_ts (wait_time, &ts);

		while ((rc = waitpid (cld, &status, WNOHANG)) == 0 && tries ++ < max_tries) {
			(void)nanosleep (&ts, NULL);
		}

		/* Child has been terminated */
		if (rc > 0) {
			/* Forget about SIGCHLD after this point */
			signal (SIGCHLD, SIG_IGN);

			if (WIFEXITED (status) && WEXITSTATUS (status) == EXIT_SUCCESS) {
				return TRUE;
			}
			else {
				msg_err_re_cache (
						"cannot approximate %s to hyperscan",
						rspamd_regexp_get_pattern (re));

				return FALSE;
			}
		}
		else {
			/* We consider that as timeout */
			kill (cld, SIGKILL);
			g_assert (waitpid (cld, &status, 0) != -1);
			msg_err_re_cache (
					"cannot approximate %s to hyperscan: timeout waiting",
					rspamd_regexp_get_pattern (re));
			signal (SIGCHLD, SIG_IGN);
		}
	}

	return FALSE;
}
#endif

gint
rspamd_re_cache_compile_hyperscan (struct rspamd_re_cache *cache,
		const char *cache_dir, gdouble max_time, gboolean silent,
		GError **err)
{
	g_assert (cache != NULL);
	g_assert (cache_dir != NULL);

#ifndef WITH_HYPERSCAN
	g_set_error (err, rspamd_re_cache_quark (), EINVAL, "hyperscan is disabled");
	return -1;
#else
	GHashTableIter it, cit;
	gpointer k, v;
	struct rspamd_re_class *re_class;
	gchar path[PATH_MAX], npath[PATH_MAX];
	hs_database_t *test_db;
	gint fd, i, n, *hs_ids = NULL, pcre_flags, re_flags;
	rspamd_cryptobox_fast_hash_state_t crc_st;
	guint64 crc;
	rspamd_regexp_t *re;
	hs_compile_error_t *hs_errors;
	guint *hs_flags = NULL;
	const gchar **hs_pats = NULL;
	gchar *hs_serialized;
	gsize serialized_len, total = 0;
	struct iovec iov[7];

	g_hash_table_iter_init (&it, cache->re_classes);

	while (g_hash_table_iter_next (&it, &k, &v)) {
		re_class = v;
		rspamd_snprintf (path, sizeof (path), "%s%c%s.hs", cache_dir,
				G_DIR_SEPARATOR, re_class->hash);

		if (rspamd_re_cache_is_valid_hyperscan_file (cache, path, TRUE, TRUE)) {

			fd = open (path, O_RDONLY, 00600);

			/* Read number of regexps */
			g_assert (fd != -1);
			lseek (fd, RSPAMD_HS_MAGIC_LEN + sizeof (cache->plt), SEEK_SET);
			read (fd, &n, sizeof (n));
			close (fd);

			if (re_class->type_len > 0) {
				if (!silent) {
					msg_info_re_cache (
							"skip already valid class %s(%*s) to cache %6s, %d regexps",
							rspamd_re_cache_type_to_string (re_class->type),
							(gint) re_class->type_len - 1,
							re_class->type_data,
							re_class->hash,
							n);
				}
			}
			else {
				if (!silent) {
					msg_info_re_cache (
							"skip already valid class %s to cache %6s, %d regexps",
							rspamd_re_cache_type_to_string (re_class->type),
							re_class->hash,
							n);
				}
			}

			continue;
		}

		rspamd_snprintf (path, sizeof (path), "%s%c%s.hs.new", cache_dir,
						G_DIR_SEPARATOR, re_class->hash);
		fd = open (path, O_CREAT|O_TRUNC|O_EXCL|O_WRONLY, 00600);

		if (fd == -1) {
			g_set_error (err, rspamd_re_cache_quark (), errno, "cannot open file "
					"%s: %s", path, strerror (errno));
			return -1;
		}

		g_hash_table_iter_init (&cit, re_class->re);
		n = g_hash_table_size (re_class->re);
		hs_flags = g_malloc0 (sizeof (*hs_flags) * n);
		hs_ids = g_malloc (sizeof (*hs_ids) * n);
		hs_pats = g_malloc (sizeof (*hs_pats) * n);
		i = 0;

		while (g_hash_table_iter_next (&cit, &k, &v)) {
			re = v;

			pcre_flags = rspamd_regexp_get_pcre_flags (re);
			re_flags = rspamd_regexp_get_flags (re);

			if (re_flags & RSPAMD_REGEXP_FLAG_PCRE_ONLY) {
				/* Do not try to compile bad regexp */
				msg_info_re_cache (
						"do not try compile %s to hyperscan as it is PCRE only",
						rspamd_regexp_get_pattern (re));
				continue;
			}

			hs_flags[i] = 0;
#ifndef WITH_PCRE2
			if (pcre_flags & PCRE_FLAG(UTF8)) {
				hs_flags[i] |= HS_FLAG_UTF8;
			}
#else
			if (pcre_flags & PCRE_FLAG(UTF)) {
				hs_flags[i] |= HS_FLAG_UTF8;
			}
#endif
			if (pcre_flags & PCRE_FLAG(CASELESS)) {
				hs_flags[i] |= HS_FLAG_CASELESS;
			}
			if (pcre_flags & PCRE_FLAG(MULTILINE)) {
				hs_flags[i] |= HS_FLAG_MULTILINE;
			}
			if (pcre_flags & PCRE_FLAG(DOTALL)) {
				hs_flags[i] |= HS_FLAG_DOTALL;
			}
			if (rspamd_regexp_get_maxhits (re) == 1) {
				hs_flags[i] |= HS_FLAG_SINGLEMATCH;
			}

			if (hs_compile (rspamd_regexp_get_pattern (re),
					hs_flags[i],
					cache->vectorized_hyperscan ? HS_MODE_VECTORED : HS_MODE_BLOCK,
					&cache->plt,
					&test_db,
					&hs_errors) != HS_SUCCESS) {
				msg_info_re_cache ("cannot compile %s to hyperscan, try prefilter match",
						rspamd_regexp_get_pattern (re));
				hs_free_compile_error (hs_errors);

				/* The approximation operation might take a significant
				 * amount of time, so we need to check if it's finite
				 */
				if (rspamd_re_cache_is_finite (cache, re, hs_flags[i], max_time)) {
					hs_flags[i] |= HS_FLAG_PREFILTER;
					hs_ids[i] = rspamd_regexp_get_cache_id (re);
					hs_pats[i] = rspamd_regexp_get_pattern (re);
					i++;
				}
			}
			else {
				hs_ids[i] = rspamd_regexp_get_cache_id (re);
				hs_pats[i] = rspamd_regexp_get_pattern (re);
				i ++;
				hs_free_database (test_db);
			}
		}
		/* Adjust real re number */
		n = i;

		if (n > 0) {
			/* Create the hs tree */
			if (hs_compile_multi (hs_pats,
					hs_flags,
					hs_ids,
					n,
					cache->vectorized_hyperscan ? HS_MODE_VECTORED : HS_MODE_BLOCK,
					&cache->plt,
					&test_db,
					&hs_errors) != HS_SUCCESS) {

				g_set_error (err, rspamd_re_cache_quark (), EINVAL,
						"cannot create tree of regexp when processing '%s': %s",
						hs_pats[hs_errors->expression], hs_errors->message);
				g_free (hs_flags);
				g_free (hs_ids);
				g_free (hs_pats);
				close (fd);
				unlink (path);
				hs_free_compile_error (hs_errors);

				return -1;
			}

			g_free (hs_pats);

			if (hs_serialize_database (test_db, &hs_serialized,
					&serialized_len) != HS_SUCCESS) {
				g_set_error (err,
						rspamd_re_cache_quark (),
						errno,
						"cannot serialize tree of regexp for %s",
						re_class->hash);

				close (fd);
				unlink (path);
				g_free (hs_ids);
				g_free (hs_flags);
				hs_free_database (test_db);

				return -1;
			}

			hs_free_database (test_db);

			/*
			 * Magic - 8 bytes
			 * Platform - sizeof (platform)
			 * n - number of regexps
			 * n * <regexp ids>
			 * n * <regexp flags>
			 * crc - 8 bytes checksum
			 * <hyperscan blob>
			 */
			rspamd_cryptobox_fast_hash_init (&crc_st, 0xdeadbabe);
			/* IDs -> Flags -> Hs blob */
			rspamd_cryptobox_fast_hash_update (&crc_st,
					hs_ids, sizeof (*hs_ids) * n);
			rspamd_cryptobox_fast_hash_update (&crc_st,
					hs_flags, sizeof (*hs_flags) * n);
			rspamd_cryptobox_fast_hash_update (&crc_st,
					hs_serialized, serialized_len);
			crc = rspamd_cryptobox_fast_hash_final (&crc_st);

			if (cache->vectorized_hyperscan) {
				iov[0].iov_base = (void *) rspamd_hs_magic_vector;
			}
			else {
				iov[0].iov_base = (void *) rspamd_hs_magic;
			}

			iov[0].iov_len = RSPAMD_HS_MAGIC_LEN;
			iov[1].iov_base = &cache->plt;
			iov[1].iov_len = sizeof (cache->plt);
			iov[2].iov_base = &n;
			iov[2].iov_len = sizeof (n);
			iov[3].iov_base = hs_ids;
			iov[3].iov_len = sizeof (*hs_ids) * n;
			iov[4].iov_base = hs_flags;
			iov[4].iov_len = sizeof (*hs_flags) * n;
			iov[5].iov_base = &crc;
			iov[5].iov_len = sizeof (crc);
			iov[6].iov_base = hs_serialized;
			iov[6].iov_len = serialized_len;

			if (writev (fd, iov, G_N_ELEMENTS (iov)) == -1) {
				g_set_error (err,
						rspamd_re_cache_quark (),
						errno,
						"cannot serialize tree of regexp to %s: %s",
						path, strerror (errno));
				close (fd);
				unlink (path);
				g_free (hs_ids);
				g_free (hs_flags);
				g_free (hs_serialized);

				return -1;
			}

			if (re_class->type_len > 0) {
				msg_info_re_cache (
						"compiled class %s(%*s) to cache %6s, %d regexps",
						rspamd_re_cache_type_to_string (re_class->type),
						(gint) re_class->type_len - 1,
						re_class->type_data,
						re_class->hash,
						n);
			}
			else {
				msg_info_re_cache (
						"compiled class %s to cache %6s, %d regexps",
						rspamd_re_cache_type_to_string (re_class->type),
						re_class->hash,
						n);
			}

			total += n;

			g_free (hs_serialized);
			g_free (hs_ids);
			g_free (hs_flags);
		}

		fsync (fd);

		/* Now rename temporary file to the new .hs file */
		rspamd_snprintf (npath, sizeof (path), "%s%c%s.hs", cache_dir,
				G_DIR_SEPARATOR, re_class->hash);

		if (rename (path, npath) == -1) {
			g_set_error (err,
					rspamd_re_cache_quark (),
					errno,
					"cannot rename %s to %s: %s",
					path, npath, strerror (errno));
			unlink (path);
			close (fd);

			return -1;
		}

		close (fd);
	}

	return total;
#endif
}

gboolean
rspamd_re_cache_is_valid_hyperscan_file (struct rspamd_re_cache *cache,
		const char *path, gboolean silent, gboolean try_load)
{
	g_assert (cache != NULL);
	g_assert (path != NULL);

#ifndef WITH_HYPERSCAN
	return FALSE;
#else
	gint fd, n, ret;
	guchar magicbuf[RSPAMD_HS_MAGIC_LEN];
	const guchar *mb;
	GHashTableIter it;
	gpointer k, v;
	struct rspamd_re_class *re_class;
	gsize len;
	const gchar *hash_pos;
	hs_platform_info_t test_plt;
	hs_database_t *test_db = NULL;
	guchar *map, *p, *end;
	rspamd_cryptobox_fast_hash_state_t crc_st;
	guint64 crc, valid_crc;

	len = strlen (path);

	if (len < sizeof (rspamd_cryptobox_HASHBYTES + 3)) {
		return FALSE;
	}

	if (memcmp (path + len - 3, ".hs", 3) != 0) {
		return FALSE;
	}

	hash_pos = path + len - 3 - (sizeof (re_class->hash) - 1);
	g_hash_table_iter_init (&it, cache->re_classes);

	while (g_hash_table_iter_next (&it, &k, &v)) {
		re_class = v;

		if (memcmp (hash_pos, re_class->hash, sizeof (re_class->hash) - 1) == 0) {
			/* Open file and check magic */
			fd = open (path, O_RDONLY);

			if (fd == -1) {
				if (!silent) {
					msg_err_re_cache ("cannot open hyperscan cache file %s: %s",
							path, strerror (errno));
				}
				return FALSE;
			}

			if (read (fd, magicbuf, sizeof (magicbuf)) != sizeof (magicbuf)) {
				msg_err_re_cache ("cannot read hyperscan cache file %s: %s",
						path, strerror (errno));
				close (fd);
				return FALSE;
			}

			if (cache->vectorized_hyperscan) {
				mb = rspamd_hs_magic_vector;
			}
			else {
				mb = rspamd_hs_magic;
			}

			if (memcmp (magicbuf, mb, sizeof (magicbuf)) != 0) {
				msg_err_re_cache ("cannot open hyperscan cache file %s: "
						"bad magic ('%*xs', '%*xs' expected)",
						path, (int) RSPAMD_HS_MAGIC_LEN, magicbuf,
						(int) RSPAMD_HS_MAGIC_LEN, mb);

				close (fd);
				return FALSE;
			}

			if (read (fd, &test_plt, sizeof (test_plt)) != sizeof (test_plt)) {
				msg_err_re_cache ("cannot read hyperscan cache file %s: %s",
						path, strerror (errno));
				close (fd);
				return FALSE;
			}

			if (memcmp (&test_plt, &cache->plt, sizeof (test_plt)) != 0) {
				msg_err_re_cache ("cannot open hyperscan cache file %s: "
						"compiled for a different platform",
						path);

				close (fd);
				return FALSE;
			}

			close (fd);

			if (try_load) {
				map = rspamd_file_xmap (path, PROT_READ, &len);

				if (map == NULL) {
					msg_err_re_cache ("cannot mmap hyperscan cache file %s: "
							"%s",
							path, strerror (errno));
					return FALSE;
				}

				p = map + RSPAMD_HS_MAGIC_LEN + sizeof (test_plt);
				end = map + len;
				n = *(gint *)p;
				p += sizeof (gint);

				if (n <= 0 || 2 * n * sizeof (gint) + /* IDs + flags */
						sizeof (guint64) + /* crc */
						RSPAMD_HS_MAGIC_LEN + /* header */
						sizeof (cache->plt) > len) {
					/* Some wrong amount of regexps */
					msg_err_re_cache ("bad number of expressions in %s: %d",
							path, n);
					munmap (map, len);
					return FALSE;
				}

				/*
				 * Magic - 8 bytes
				 * Platform - sizeof (platform)
				 * n - number of regexps
				 * n * <regexp ids>
				 * n * <regexp flags>
				 * crc - 8 bytes checksum
				 * <hyperscan blob>
				 */

				memcpy (&crc, p + n * 2 * sizeof (gint), sizeof (crc));
				rspamd_cryptobox_fast_hash_init (&crc_st, 0xdeadbabe);
				/* IDs */
				rspamd_cryptobox_fast_hash_update (&crc_st, p, n * sizeof (gint));
				/* Flags */
				rspamd_cryptobox_fast_hash_update (&crc_st, p + n * sizeof (gint),
						n * sizeof (gint));
				/* HS database */
				p += n * sizeof (gint) * 2 + sizeof (guint64);
				rspamd_cryptobox_fast_hash_update (&crc_st, p, end - p);
				valid_crc = rspamd_cryptobox_fast_hash_final (&crc_st);

				if (crc != valid_crc) {
					msg_warn_re_cache ("outdated or invalid hs database in %s: "
							"crc read %xL, crc expected %xL", path, crc, valid_crc);
					munmap (map, len);

					return FALSE;
				}

				if ((ret = hs_deserialize_database (p, end - p, &test_db))
						!= HS_SUCCESS) {
					msg_err_re_cache ("bad hs database in %s: %d", path, ret);
					munmap (map, len);

					return FALSE;
				}

				hs_free_database (test_db);
				munmap (map, len);
			}
			/* XXX: add crc check */

			return TRUE;
		}
	}

	if (!silent) {
		msg_warn_re_cache ("unknown hyperscan cache file %s", path);
	}

	return FALSE;
#endif
}


gboolean
rspamd_re_cache_load_hyperscan (struct rspamd_re_cache *cache,
		const char *cache_dir)
{
	g_assert (cache != NULL);
	g_assert (cache_dir != NULL);

#ifndef WITH_HYPERSCAN
	return FALSE;
#else
	gchar path[PATH_MAX];
	gint fd, i, n, *hs_ids = NULL, *hs_flags = NULL, total = 0, ret;
	GHashTableIter it;
	gpointer k, v;
	guint8 *map, *p, *end;
	struct rspamd_re_class *re_class;
	struct rspamd_re_cache_elt *elt;
	struct stat st;

	g_hash_table_iter_init (&it, cache->re_classes);

	while (g_hash_table_iter_next (&it, &k, &v)) {
		re_class = v;
		rspamd_snprintf (path, sizeof (path), "%s%c%s.hs", cache_dir,
				G_DIR_SEPARATOR, re_class->hash);

		if (rspamd_re_cache_is_valid_hyperscan_file (cache, path, FALSE, FALSE)) {
			msg_debug_re_cache ("load hyperscan database from '%s'",
					re_class->hash);

			fd = open (path, O_RDONLY);

			/* Read number of regexps */
			g_assert (fd != -1);
			fstat (fd, &st);

			map = mmap (NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);

			if (map == MAP_FAILED) {
				msg_err_re_cache ("cannot mmap %s: %s", path, strerror (errno));
				close (fd);
				return FALSE;
			}

			close (fd);
			end = map + st.st_size;
			p = map + RSPAMD_HS_MAGIC_LEN + sizeof (cache->plt);
			n = *(gint *)p;

			if (n <= 0 || 2 * n * sizeof (gint) + /* IDs + flags */
							sizeof (guint64) + /* crc */
							RSPAMD_HS_MAGIC_LEN + /* header */
							sizeof (cache->plt) > (gsize)st.st_size) {
				/* Some wrong amount of regexps */
				msg_err_re_cache ("bad number of expressions in %s: %d",
						path, n);
				munmap (map, st.st_size);
				return FALSE;
			}

			total += n;
			p += sizeof (n);
			hs_ids = g_malloc (n * sizeof (*hs_ids));
			memcpy (hs_ids, p, n * sizeof (*hs_ids));
			p += n * sizeof (*hs_ids);
			hs_flags = g_malloc (n * sizeof (*hs_flags));
			memcpy (hs_flags, p, n * sizeof (*hs_flags));

			/* Skip crc */
			p += n * sizeof (*hs_ids) + sizeof (guint64);

			/* Cleanup */
			if (re_class->hs_scratch != NULL) {
				hs_free_scratch (re_class->hs_scratch);
			}

			if (re_class->hs_db != NULL) {
				hs_free_database (re_class->hs_db);
			}

			if (re_class->hs_ids) {
				g_free (re_class->hs_ids);
			}

			re_class->hs_ids = NULL;
			re_class->hs_scratch = NULL;
			re_class->hs_db = NULL;

			if ((ret = hs_deserialize_database (p, end - p, &re_class->hs_db))
					!= HS_SUCCESS) {
				msg_err_re_cache ("bad hs database in %s: %d", path, ret);
				munmap (map, st.st_size);
				g_free (hs_ids);
				g_free (hs_flags);

				return FALSE;
			}

			munmap (map, st.st_size);

			g_assert (hs_alloc_scratch (re_class->hs_db,
					&re_class->hs_scratch) == HS_SUCCESS);

			/*
			 * Now find hyperscan elts that are successfully compiled and
			 * specify that they should be matched using hyperscan
			 */
			for (i = 0; i < n; i ++) {
				g_assert ((gint)cache->re->len > hs_ids[i] && hs_ids[i] >= 0);
				elt = g_ptr_array_index (cache->re, hs_ids[i]);

				if (hs_flags[i] & HS_FLAG_PREFILTER) {
					elt->match_type = RSPAMD_RE_CACHE_HYPERSCAN_PRE;
				}
				else {
					elt->match_type = RSPAMD_RE_CACHE_HYPERSCAN;
				}
			}

			re_class->hs_ids = hs_ids;
			g_free (hs_flags);
			re_class->nhs = n;
		}
		else {
			msg_err_re_cache ("invalid hyperscan hash file '%s'",
					path);
			return FALSE;
		}
	}

	msg_info_re_cache ("hyperscan database of %d regexps has been loaded", total);
	cache->hyperscan_loaded = TRUE;

	return TRUE;
#endif
}
