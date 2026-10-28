/*
 * pg_walex - PostgreSQL WAL parsing and recovery tool
 * Copyright (C) 2026 songshaohua <developerhonor@petalmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * pg_walex - catalog reconstruction from WAL heap records
 */
#include "pwe_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define PWE_CAT_PAGE_SIZE 8192

static uint64
relkey(uint32 db, uint32 oid)
{
	return ((uint64) db << 32) | oid;
}

static void
free_rel(PweRel *rel)
{
	int			i;

	if (!rel)
		return;
	if (rel->attrs)
	{
		for (i = 0; i <= rel->natts; i++)
			free(rel->attrs[i]);
		free(rel->attrs);
	}
	pwe_rel_clear_rows(rel);
	if (rel->rows)
	{
		pwe_hash_free(rel->rows, NULL);
		free(rel->rows);
	}
	free(rel->pk);
	free(rel);
}

static void
free_type(PweType *t)
{
	free(t);
}

static void
free_index(PweIndex *i)
{
	free(i);
}

static void
free_con(PweCon *c)
{
	free(c->keys);
	free(c);
}

static void
free_enum(PweEnumVal *e)
{
	free(e);
}

static void free_toast_entry(PweToastEntry *e);

static void
free_seq(PweSeq *s)
{
	free(s);
}

static void
free_str(void *p)
{
	free(p);
}

static void
payload_free(PweCatPayload *p)
{
	if (!p)
		return;
	if (p->saved_rel)
		free_rel((PweRel *) p->saved_rel);
	free(p->raw);
	free(p);
}

PweCatalog *
pwe_catalog_create(int version, uint64 row_limit, uint64 toast_limit)
{
	PweCatalog *cat = pwe_calloc(1, sizeof(PweCatalog));

	cat->version = version;
	cat->row_limit = row_limit ? row_limit : 200000;
	cat->toast_limit = toast_limit ? toast_limit : (64ull * 1024 * 1024);
	pwe_hash_init(&cat->rels_by_oid);
	pwe_hash_init(&cat->rels_by_file);
	pwe_hash_init(&cat->types);
	pwe_hash_init(&cat->namespaces);
	pwe_hash_init(&cat->dbs);
	pwe_hash_init(&cat->roles);
	pwe_hash_init(&cat->indexes);
	pwe_hash_init(&cat->constraints);
	pwe_hash_init(&cat->enums);
	pwe_hash_init(&cat->enums_by_type);
	pwe_hash_init(&cat->sequences);
	pwe_hash_init(&cat->pending_attrs);
	pwe_hash_init(&cat->toast_chunks);
	pwe_hash_init(&cat->cat_rows);
	return cat;
}

void
pwe_catalog_destroy(PweCatalog *cat)
{
	if (!cat)
		return;
	pwe_hash_free(&cat->rels_by_oid, (void (*)(void *)) free_rel);
	pwe_hash_free(&cat->rels_by_file, NULL);
	pwe_hash_free(&cat->types, (void (*)(void *)) free_type);
	pwe_hash_free(&cat->namespaces, free_str);
	pwe_hash_free(&cat->dbs, free_str);
	pwe_hash_free(&cat->roles, free_str);
	pwe_hash_free(&cat->indexes, (void (*)(void *)) free_index);
	pwe_hash_free(&cat->constraints, (void (*)(void *)) free_con);
	pwe_hash_free(&cat->enums, (void (*)(void *)) free_enum);
	pwe_hash_free(&cat->enums_by_type, NULL);
	pwe_hash_free(&cat->sequences, (void (*)(void *)) free_seq);
	pwe_hash_free(&cat->pending_attrs, NULL);
	pwe_hash_free(&cat->toast_chunks, (void (*)(void *)) free_toast_entry);
	pwe_hash_free(&cat->cat_rows, NULL);
	free(cat->pgdata);
	free(cat);
}

/* ------------------------------------------------------------------ */
/* lookups                                                            */
/* ------------------------------------------------------------------ */
PweRel *
pwe_catalog_rel_by_oid(PweCatalog *cat, uint32 db, uint32 oid)
{
	return pwe_hash_get(&cat->rels_by_oid, relkey(db, oid));
}

PweRel *
pwe_catalog_rel_by_file(PweCatalog *cat, uint32 db, uint32 filenode)
{
	PweRel	   *rel = pwe_hash_get(&cat->rels_by_file, relkey(db, filenode));

	if (rel)
		return rel;
	return pwe_catalog_rel_by_oid(cat, db, filenode);
}

const char *
pwe_catalog_ns_name(PweCatalog *cat, uint32 db, uint32 oid)
{
	char	   *name = pwe_hash_get(&cat->namespaces, relkey(db, oid));

	if (name)
		return name;
	switch (oid)
	{
		case 11:
			return "pg_catalog";
		case 99:
			return "pg_toast";
		case 2200:
			return "public";
		default:
			return NULL;
	}
}

const char *
pwe_catalog_db_name(PweCatalog *cat, uint32 oid)
{
	char	   *name = pwe_hash_get(&cat->dbs, oid);

	if (name)
		return name;
	switch (oid)
	{
		case 1:
			return "template1";
		case 4:
			return "template0";
		case 5:
			return "postgres";
		default:
			return NULL;
	}
}

const char *
pwe_catalog_role_name(PweCatalog *cat, uint32 oid)
{
	char	   *name = pwe_hash_get(&cat->roles, oid);

	if (name)
		return name;
	if (oid == 10)
		return "postgres";
	return NULL;
}

PweIndex *
pwe_catalog_index(PweCatalog *cat, uint32 db, uint32 oid)
{
	return pwe_hash_get(&cat->indexes, relkey(db, oid));
}

PweCon *
pwe_catalog_constraint(PweCatalog *cat, uint32 db, uint32 oid)
{
	return pwe_hash_get(&cat->constraints, relkey(db, oid));
}

PweSeq *
pwe_catalog_sequence(PweCatalog *cat, uint32 db, uint32 relid)
{
	return pwe_hash_get(&cat->sequences, relkey(db, relid));
}

const char *
pwe_catalog_enum_label(PweCatalog *cat, uint32 db, uint32 valueoid)
{
	PweEnumVal *e = pwe_hash_get(&cat->enums, relkey(db, valueoid));

	return e ? e->label : NULL;
}

PweTypeInfo
pwe_catalog_type_info(PweCatalog *cat, uint32 db, uint32 typid)
{
	PweTypeInfo info;
	PweType    *t;

	memset(&info, 0, sizeof(info));
	info.oid = typid;
	info.typlen = -1;
	info.align = 'i';
	info.category = 'U';
	info.typtype = 'b';
	t = pwe_hash_get(&cat->types, relkey(db, typid));
	if (t)
	{
		memcpy(info.name, t->name, sizeof(info.name) - 1);
		info.name[sizeof(info.name) - 1] = '\0';
		info.nsp = t->nsp;
		info.typlen = t->typlen;
		info.byval = t->byval;
		info.align = t->align;
		info.typtype = t->typtype;
		info.category = t->category;
		info.relid = t->relid;
		info.elem = t->elem;
		info.array = t->array;
		info.basetype = t->basetype;
		info.typtypmod = t->typtypmod;
		info.notnull = t->notnull;
		info.found = true;
		if (strncmp(info.name, "_", 1) == 0 && info.elem != 0)
			info.category = 'A';
		return info;
	}
	{
		const char *name = NULL;
		int16		len;
		bool		byval;
		char		align;
		char		typtype;
		uint32		elem;
		uint32		array;

		if (pwe_builtin_type_lookup(typid, &name, &len, &byval, &align,
									&typtype, &elem, &array))
		{
			strncpy(info.name, name, PWE_NAMEDATALEN - 1);
			info.typlen = len;
			info.byval = byval;
			info.align = align;
			info.typtype = typtype;
			info.elem = elem;
			info.array = array;
			info.found = true;
			if (elem != 0)
				info.category = 'A';
			else if (typid == 25 || typid == 1042 || typid == 1043 ||
					 typid == 19 || typid == 114 || typid == 142)
				info.category = 'S';
			else
				info.category = 'N';
		}
		else
		{
			uint32		el = pwe_builtin_array_element(typid);

			if (el)
			{
				const char *ename = NULL;

				if (pwe_builtin_type_lookup(el, &ename, NULL, NULL, NULL, NULL,
											NULL, NULL) && ename)
				{
					snprintf(info.name, sizeof(info.name), "_%s", ename);
					info.typlen = -1;
					info.align = 'i';
					info.elem = el;
					info.category = 'A';
					info.typtype = 'b';
					info.found = true;
				}
			}
		}
	}
	return info;
}

/* ------------------------------------------------------------------ */
/* row cache                                                          */
/* ------------------------------------------------------------------ */
PweRowVals *
pwe_rowvals_create(int natts)
{
	PweRowVals *r = pwe_calloc(1, sizeof(PweRowVals));

	r->natts = natts;
	r->vals = pwe_calloc((size_t) natts + 1, sizeof(char *));
	return r;
}

void
pwe_rowvals_free(PweRowVals *r)
{
	int			i;

	if (!r)
		return;
	for (i = 0; i <= r->natts; i++)
		free(r->vals[i]);
	free(r->vals);
	if (r->copyvals)
	{
		for (i = 0; i <= r->natts; i++)
			free(r->copyvals[i]);
		free(r->copyvals);
	}
	free(r->raw);
	free(r);
}

PweRowVals *
pwe_row_get(PweRel *rel, uint32 blkno, uint16 offnum)
{
	if (!rel || !rel->rows)
		return NULL;
	return pwe_hash_get(rel->rows, ((uint64) blkno << 16) | offnum);
}

void
pwe_row_set(PweRel *rel, uint32 blkno, uint16 offnum, PweRowVals *row)
{
	uint64		key = ((uint64) blkno << 16) | offnum;
	PweRowVals *old;

	if (!rel)
	{
		pwe_rowvals_free(row);
		return;
	}
	if (!rel->rows)
	{
		rel->rows = pwe_malloc(sizeof(PweHash));
		pwe_hash_init(rel->rows);
	}
	if (rel->rows->count >= 200000 && pwe_hash_get(rel->rows, key) == NULL)
	{
		/* safety valve: stop caching new rows for this relation */
		rel->rows_disabled = true;
		pwe_rowvals_free(row);
		return;
	}
	old = pwe_hash_put(rel->rows, key, row);
	if (old)
		pwe_rowvals_free(old);
}

PweRowVals *
pwe_row_remove(PweRel *rel, uint32 blkno, uint16 offnum)
{
	if (!rel || !rel->rows)
		return NULL;
	return pwe_hash_remove(rel->rows, ((uint64) blkno << 16) | offnum);
}

void
pwe_rel_clear_rows(PweRel *rel)
{
	size_t		i;

	if (!rel || !rel->rows)
		return;
	for (i = 0; i < rel->rows->nbuckets; i++)
	{
		PweHashEntry *e = rel->rows->buckets[i];

		while (e)
		{
			PweHashEntry *next = e->next;

			pwe_rowvals_free(e->val);
			free(e);
			e = next;
		}
	}
	rel->rows->count = 0;
	memset(rel->rows->buckets, 0, sizeof(PweHashEntry *) * rel->rows->nbuckets);
}

/* ------------------------------------------------------------------ */
/* toast chunks                                                       */
/* ------------------------------------------------------------------ */
static uint64
toast_key(uint32 db, uint32 relid, uint32 valueid)
{
	uint64		k = ((uint64) db << 32) | relid;

	k ^= (uint64) valueid * 0x9E3779B97F4A7C15ULL;
	return k;
}

void
pwe_catalog_add_toast_chunk(PweCatalog *cat, uint32 db, uint32 relid,
							uint32 valueid, int32 seq,
							const uint8 *data, uint32 len)
{
	uint64		key = toast_key(db, relid, valueid);
	PweToastEntry *e = pwe_hash_get(&cat->toast_chunks, key);
	PweToastChunk *c;

	if (!e)
	{
		e = pwe_calloc(1, sizeof(PweToastEntry));
		e->db = db;
		e->relid = relid;
		e->valueid = valueid;
		pwe_hash_put(&cat->toast_chunks, key, e);
	}
	if (cat->toast_bytes + len > cat->toast_limit)
		return;
	c = pwe_calloc(1, sizeof(PweToastChunk));
	c->seq = seq;
	c->len = len;
	c->data = pwe_malloc(len ? len : 1);
	if (len)
		memcpy(c->data, data, len);
	c->next = e->chunks;
	e->chunks = c;
	cat->toast_bytes += len;
}

static void
free_toast_entry(PweToastEntry *e)
{
	PweToastChunk *c = e->chunks;

	while (c)
	{
		PweToastChunk *n = c->next;

		free(c->data);
		free(c);
		c = n;
	}
	free(e);
}

static void load_toast_from_disk(PweCatalog *cat, PweRel *rel, uint32 db,
								 uint32 filter_valueid);

bool
pwe_catalog_detoast(PweCatalog *cat, uint32 db, PweValue *v)
{
	uint64		key;
	PweToastEntry *e;
	PweToastChunk *c;
	int			nchunks = 0;
	int			i;
	uint32		total = 0;
	uint8	   *assembled;
	int32		expected;
	uint32		extsize;
	uint8		method;

	if (!v->toast.valid || v->data != NULL)
		return true;
	key = toast_key(db, v->toast.toastrelid, v->toast.valueid);
	e = pwe_hash_get(&cat->toast_chunks, key);
	if (!e || e->db != db || e->relid != v->toast.toastrelid ||
		e->valueid != v->toast.valueid)
	{
		/* fall back to the offline TOAST relation file */
		if (cat->pgdata)
		{
			PweRel	   *trel = pwe_catalog_rel_by_oid(cat, db, v->toast.toastrelid);

			if (!trel)
				trel = pwe_catalog_rel_by_file(cat, db, v->toast.toastrelid);
			if (trel)
			{
				load_toast_from_disk(cat, trel, db, v->toast.valueid);
				e = pwe_hash_get(&cat->toast_chunks, key);
			}
		}
		if (!e || e->db != db || e->relid != v->toast.toastrelid ||
			e->valueid != v->toast.valueid)
		{
			return false;
		}
	}
	for (c = e->chunks; c; c = c->next)
	{
		nchunks++;
		total += c->len;
	}
	if (nchunks == 0)
		return false;
	assembled = pwe_malloc(total ? total : 1);
	{
		uint8	  **parts = pwe_calloc((size_t) nchunks, sizeof(uint8 *));
		int		   *seqs = pwe_calloc((size_t) nchunks, sizeof(int));
		uint32	   *lens = pwe_calloc((size_t) nchunks, sizeof(uint32));

		i = 0;
		for (c = e->chunks; c; c = c->next)
		{
			parts[i] = c->data;
			seqs[i] = c->seq;
			lens[i] = c->len;
			i++;
		}
		for (i = 0; i < nchunks; i++)
		{
			int			j;
			int			best = i;

			for (j = i + 1; j < nchunks; j++)
				if (seqs[j] < seqs[best])
					best = j;
			if (best != i)
			{
				uint8	   *tp = parts[i];
				int			ts = seqs[i];
				uint32		tl = lens[i];

				parts[i] = parts[best];
				seqs[i] = seqs[best];
				lens[i] = lens[best];
				parts[best] = tp;
				seqs[best] = ts;
				lens[best] = tl;
			}
		}
		{
			uint32		off = 0;

			for (i = 0; i < nchunks; i++)
			{
				memcpy(assembled + off, parts[i], lens[i]);
				off += lens[i];
			}
		}
		free(parts);
		free(seqs);
		free(lens);
	}
	expected = v->toast.rawsize - 4;
	extsize = v->toast.extinfo & 0x3FFFFFFF;
	method = cat->version >= 14 ? (uint8) (v->toast.extinfo >> 30) : 0;
	if (expected < 0)
		expected = (int32) total;

	/*
	 * TOAST chunks contain VARDATA() of the compressed varlena, which in
	 * PostgreSQL is the 4-byte va_tcinfo followed by the compressed bytes.
	 * Detect and skip that prefix.
	 */
	{
		const uint8 *cdata = assembled;
		uint32		clen = total;
		bool		prefixed = false;

		if (total >= 8)
		{
			uint32		tcinfo = pwe_read_u32(assembled);

			if ((int32) (tcinfo & 0x3FFFFFFF) == expected)
			{
				uint8		pm = (uint8) (tcinfo >> 30);

				if (pm <= 2)
				{
					method = pm;
					cdata = assembled + 4;
					clen = total - 4;
					prefixed = true;
				}
			}
		}
		if (!prefixed && (uint32) expected == total &&
			(cat->version < 14 || extsize == (uint32) expected))
		{
			free(v->owned);
			v->owned = assembled;
			v->data = assembled;
			v->len = (int) total;
		}
		else
		{
			uint8	   *out = pwe_malloc((size_t) expected);
			bool		ok = false;

			if (method == 0)
				ok = pwe_pglz_decompress(cdata, clen, out, (size_t) expected);
			else if (method == 1)
				ok = pwe_lz4_decompress(cdata, clen, out, (size_t) expected);
			else if (method == 2)
				ok = pwe_zstd_decompress(cdata, clen, out, (size_t) expected);
			if (!ok)
			{
				free(out);
				if (!prefixed)
					free(assembled);
				return false;
			}
			free(assembled);
			free(v->owned);
			v->owned = out;
			v->data = out;
			v->len = expected;
		}
	}
	/*
	 * Keep the chunks cached: a later row (e.g. an UPDATE that does not
	 * re-toast an unchanged value) may reference the same valueid again.
	 * Memory is bounded by the add-time toast_limit instead.
	 */
	return true;
}

/* ------------------------------------------------------------------ */
/* value helpers                                                      */
/* ------------------------------------------------------------------ */
static bool val_null(PweValue *vals, int idx) { return !vals || idx <= 0 || vals[idx - 1].isnull; }
static uint32 val_u32(PweValue *vals, int idx) { return val_null(vals, idx) ? 0 : (uint32) (vals[idx - 1].u & 0xffffffff); }
static int32 val_i32(PweValue *vals, int idx) { return val_null(vals, idx) ? 0 : (int32) vals[idx - 1].u; }
static int16 val_i16(PweValue *vals, int idx) { return val_null(vals, idx) ? 0 : (int16) vals[idx - 1].u; }
static bool val_bool(PweValue *vals, int idx) { return val_null(vals, idx) ? false : vals[idx - 1].u != 0; }
static char val_char(PweValue *vals, int idx) { return val_null(vals, idx) ? '\0' : (char) (vals[idx - 1].u & 0xff); }

static void
val_name(PweValue *vals, int idx, char *out, size_t outlen)
{
	PweValue   *v;

	out[0] = '\0';
	if (val_null(vals, idx))
		return;
	v = &vals[idx - 1];
	if (v->data)
	{
		size_t		n = (size_t) v->len;

		if (n >= PWE_NAMEDATALEN)
			n = PWE_NAMEDATALEN - 1;
		if (n >= outlen)
			n = outlen - 1;
		memcpy(out, v->data, n);
		out[n] = '\0';
	}
}

static bool
decode_int2vector(PweValue *v, int16 *out, int max, int *n)
{
	const uint8 *p;
	int32		ndim;
	int			i;

	*n = 0;
	if (!v || v->isnull || !v->data || v->len < 8)
		return false;
	p = v->data;
	ndim = pwe_read_i32(p);
	if (ndim < 0 || ndim > max)
		return false;
	if (20 + ndim * 2 > v->len)
		return false;
	for (i = 0; i < ndim; i++)
		out[i] = pwe_read_i16(p + 20 + i * 2);
	*n = ndim;
	return true;
}

static bool
decode_int2array(PweValue *v, int16 *out, int max, int *n)
{
	const uint8 *p;
	int32		ndim;
	int32		dataoffset;
	int32		dims;
	int			i;
	uint32		off;
	uint32		nitems;

	*n = 0;
	if (!v || v->isnull || !v->data || v->len < 12)
	{
		return false;
	}
	p = v->data;
	ndim = pwe_read_i32(p);
	dataoffset = pwe_read_i32(p + 4);
	if (ndim != 1)
		return false;
	dims = pwe_read_i32(p + 12);
	if (dims < 0 || dims > max)
		return false;
	nitems = (uint32) dims;
	if (dataoffset != 0)
	{
		uint32		bitmaplen = (nitems + 7) / 8;
		const uint8 *bits;

		if (20 + bitmaplen > (uint32) v->len)
			return false;
		bits = p + 20;
		off = (uint32) dataoffset;
		for (i = 0; i < (int) nitems; i++)
		{
			if (!(bits[i / 8] & (1 << (i % 8))))
			{
				out[i] = 0;
				continue;
			}
			off = (off + 1u) & ~1u;
			if (off + 2 > (uint32) v->len)
				return false;
			out[i] = pwe_read_i16(p + off);
			off += 2;
		}
		*n = (int) nitems;
		return true;
	}
	off = 20;					/* MAXALIGN(16 + 8) - VARHDRSZ */
	for (i = 0; i < (int) nitems; i++)
	{
		off = (off + 1u) & ~1u;
		if (off + 2 > (uint32) v->len)
			return false;
		out[i] = pwe_read_i16(p + off);
		off += 2;
	}
	*n = (int) nitems;
	return true;
}

/* ------------------------------------------------------------------ */
/* snapshot application                                               */
/* ------------------------------------------------------------------ */
static void
ensure_rel_capacity(PweRel *rel, int natts)
{
	int			old;

	if (natts <= rel->natts)
		return;
	old = rel->natts;
	rel->natts = natts + 8;
	rel->attrs = pwe_realloc(rel->attrs, sizeof(PweAttr *) * (size_t) (rel->natts + 1));
	memset(rel->attrs + old + 1, 0, sizeof(PweAttr *) * (size_t) (rel->natts - old));
	if (old == 0)
		rel->attrs[0] = NULL;
}

static void
drain_pending_attrs(PweCatalog *cat, PweRel *rel)
{
	PweHash    *pending = pwe_hash_get(&cat->pending_attrs, relkey(rel->db, rel->oid));
	size_t		i;

	if (!pending)
		return;
	for (i = 0; i < pending->nbuckets; i++)
	{
		PweHashEntry *e = pending->buckets[i];

		while (e)
		{
			PweAttr    *a = e->val;

			if (a->num > 0)
			{
				ensure_rel_capacity(rel, a->num);
				free(rel->attrs[a->num]);
				rel->attrs[a->num] = a;
			}
			else
				free(a);
			e = e->next;
		}
	}
	free(pending->buckets);
	free(pending);
	pwe_hash_remove(&cat->pending_attrs, relkey(rel->db, rel->oid));
}

static void
rel_put_maps(PweCatalog *cat, PweRel *rel)
{
	pwe_hash_put(&cat->rels_by_oid, relkey(rel->db, rel->oid), rel);
	if (rel->filenode != 0)
		pwe_hash_put(&cat->rels_by_file, relkey(rel->db, rel->filenode), rel);
	/*
	 * Keep an OID-based fallback for lookups that only know a relation OID
	 * (e.g. pg_class.reltoastrelid), but never let it steal a live
	 * file-number mapping owned by another relation: after a heap rewrite the
	 * rewritten table's new relfilenode equals the transient pg_temp_*
	 * relation's OID.
	 */
	if (rel->filenode != rel->oid &&
		pwe_hash_get(&cat->rels_by_file, relkey(rel->db, rel->oid)) == NULL)
		pwe_hash_put(&cat->rels_by_file, relkey(rel->db, rel->oid), rel);
}

/*
 * Drop a file-number mapping only while it still belongs to "rel".
 *
 * A heap rewrite reuses the new heap's relfilenode as the final relfilenode of
 * the rewritten table (ALTER TABLE ... ALTER COLUMN TYPE).  The transient
 * pg_temp_* relation is removed from pg_class after the swap, so blindly
 * deleting the key here would clobber the rewritten table's fresh mapping and
 * every later change record for that relation would look unknown.
 */
static void
rel_unmap_file(PweCatalog *cat, uint32 db, uint32 filenode, PweRel *rel)
{
	if (filenode == 0)
		return;
	if (pwe_hash_get(&cat->rels_by_file, relkey(db, filenode)) != rel)
		return;
	pwe_hash_remove(&cat->rels_by_file, relkey(db, filenode));
}

static PweRel *
rel_from_snap(PweCatalog *cat, const PweRelSnap *s)
{
	PweRel	   *rel = pwe_catalog_rel_by_oid(cat, s->db, s->oid);
	uint32		oldfile;

	if (!rel)
	{
		rel = pwe_calloc(1, sizeof(PweRel));
		rel->oid = s->oid;
		rel->db = s->db;
		cat->relations_known++;
		pwe_hash_put(&cat->rels_by_oid, relkey(s->db, s->oid), rel);
	}
	oldfile = rel->filenode;
	memcpy(rel->name, s->name, sizeof(rel->name));
	rel->nsp = s->nsp;
	rel->relkind = s->relkind;
	rel->persistence = s->persistence;
	rel->filenode = s->filenode;
	rel->toastrelid = s->toastrelid;
	rel->reltype = s->typid;
	rel->am = s->am;
	{
		const char *ns = pwe_catalog_ns_name(cat, s->db, s->nsp);

		if (ns)
			snprintf(rel->nspname, sizeof(rel->nspname), "%s", ns);
		else
			rel->nspname[0] = '\0';
	}
	if (s->natts > 0)
		ensure_rel_capacity(rel, s->natts);
	if (oldfile != 0 && oldfile != s->filenode)
		rel_unmap_file(cat, s->db, oldfile, rel);
	rel_put_maps(cat, rel);
	drain_pending_attrs(cat, rel);
	return rel;
}

static void
rel_to_snap(PweRel *rel, PweRelSnap *s)
{
	memset(s, 0, sizeof(*s));
	s->oid = rel->oid;
	s->db = rel->db;
	s->nsp = rel->nsp;
	s->typid = rel->reltype;
	s->filenode = rel->filenode;
	s->toastrelid = rel->toastrelid;
	s->am = rel->am;
	s->natts = (int16) rel->natts;
	s->relkind = rel->relkind;
	s->persistence = rel->persistence;
	memcpy(s->name, rel->name, sizeof(s->name));
}

static PweRel *
detach_rel(PweCatalog *cat, uint32 db, uint32 oid)
{
	PweRel	   *rel = pwe_catalog_rel_by_oid(cat, db, oid);

	if (!rel)
		return NULL;
	rel_unmap_file(cat, db, rel->filenode, rel);
	rel_unmap_file(cat, db, oid, rel);
	pwe_hash_remove(&cat->rels_by_oid, relkey(db, oid));
	{
		PweHash    *pending = pwe_hash_remove(&cat->pending_attrs, relkey(db, oid));

		if (pending)
		{
			pwe_hash_free(pending, (void (*)(void *)) free);
			free(pending);
		}
	}
	return rel;
}

static void
attr_apply_snap(PweCatalog *cat, const PweAttrSnap *s)
{
	PweRel	   *rel = pwe_catalog_rel_by_oid(cat, s->db, s->relid);
	PweAttr    *a;

	if (!rel || s->num <= 0)
	{
		PweHash    *pending;
		uint64		key = relkey(s->db, s->relid);

		pending = pwe_hash_get(&cat->pending_attrs, key);
		if (!pending)
		{
			pending = pwe_malloc(sizeof(PweHash));
			pwe_hash_init(pending);
			pwe_hash_put(&cat->pending_attrs, key, pending);
		}
		a = pwe_hash_get(pending, (uint64) (uint16) s->num);
		if (!a)
		{
			a = pwe_calloc(1, sizeof(PweAttr));
			pwe_hash_put(pending, (uint64) (uint16) s->num, a);
		}
	}
	else
	{
		ensure_rel_capacity(rel, s->num);
		a = rel->attrs[s->num];
		if (!a)
		{
			a = pwe_calloc(1, sizeof(PweAttr));
			rel->attrs[s->num] = a;
		}
	}
	a->num = s->num;
	memcpy(a->name, s->name, sizeof(a->name));
	a->typid = s->typid;
	a->len = s->len;
	a->typmod = s->typmod;
	a->ndims = 0;
	a->align = s->align;
	a->byval = s->byval;
	a->notnull = s->notnull;
	a->dropped = s->dropped;
	a->hasdef = s->hasdef;
	a->identity = s->identity;
	a->generated = s->generated;
	a->collation = s->collation;
}

static void
attr_remove(PweCatalog *cat, uint32 db, uint32 relid, int16 num)
{
	PweRel	   *rel = pwe_catalog_rel_by_oid(cat, db, relid);

	if (rel && num > 0 && num <= rel->natts && rel->attrs)
	{
		free(rel->attrs[num]);
		rel->attrs[num] = NULL;
	}
	else
	{
		PweHash    *pending = pwe_hash_get(&cat->pending_attrs, relkey(db, relid));

		if (pending)
		{
			PweAttr    *a = pwe_hash_remove(pending, (uint64) (uint16) num);

			free(a);
		}
	}
}

static void
type_apply_snap(PweCatalog *cat, const PweTypeSnap *s)
{
	PweType    *t = pwe_hash_get(&cat->types, relkey(s->db, s->oid));

	if (!t)
	{
		t = pwe_calloc(1, sizeof(PweType));
		t->oid = s->oid;
		t->db = s->db;
		pwe_hash_put(&cat->types, relkey(s->db, s->oid), t);
	}
	memcpy(t->name, s->name, sizeof(t->name));
	t->nsp = s->nsp;
	t->typlen = s->typlen;
	t->byval = s->byval;
	t->align = s->align;
	t->typtype = s->typtype;
	t->category = 'U';
	t->relid = s->relid;
	t->elem = s->elem;
	t->array = s->array;
	t->basetype = s->basetype;
	t->typtypmod = s->typtypmod;
	t->notnull = s->notnull;
	t->defined = s->defined;
	if (s->name[0] == '_' && s->elem != 0)
		t->category = 'A';
	if (s->elem != 0)
	{
		PweType    *elem = pwe_hash_get(&cat->types, relkey(s->db, s->elem));

		if (elem)
			elem->array = s->oid;
	}
}

static void
ns_apply_snap(PweCatalog *cat, const PweNsSnap *s)
{
	char	   *old = pwe_hash_get(&cat->namespaces, relkey(s->db, s->oid));
	size_t		i;

	if (old)
		free(old);
	pwe_hash_put(&cat->namespaces, relkey(s->db, s->oid), pwe_strdup(s->name));
	for (i = 0; i < cat->rels_by_oid.nbuckets; i++)
	{
		PweHashEntry *e;

		for (e = cat->rels_by_oid.buckets[i]; e; e = e->next)
		{
			PweRel	   *rel = e->val;

			if (rel->db == s->db && rel->nsp == s->oid)
				snprintf(rel->nspname, sizeof(rel->nspname), "%s", s->name);
		}
	}
}

static void
rebuild_pk(PweCatalog *cat, PweRel *rel)
{
	size_t		i;

	free(rel->pk);
	rel->pk = NULL;
	rel->npk = 0;
	for (i = 0; i < cat->constraints.nbuckets; i++)
	{
		PweHashEntry *e;

		for (e = cat->constraints.buckets[i]; e; e = e->next)
		{
			PweCon	   *c = e->val;

			if (c->db == rel->db && c->relid == rel->oid && c->contype == 'p' &&
				c->nkeys > 0)
			{
				rel->pk = pwe_malloc(sizeof(int16) * c->nkeys);
				memcpy(rel->pk, c->keys, sizeof(int16) * c->nkeys);
				rel->npk = c->nkeys;
				return;
			}
		}
	}
	for (i = 0; i < cat->indexes.nbuckets; i++)
	{
		PweHashEntry *e;

		for (e = cat->indexes.buckets[i]; e; e = e->next)
		{
			PweIndex   *idx = e->val;

			if (idx->db == rel->db && idx->relid == rel->oid && idx->primary &&
				idx->nkeys > 0)
			{
				rel->pk = pwe_malloc(sizeof(int16) * idx->nkeys);
				memcpy(rel->pk, idx->keys, sizeof(int16) * idx->nkeys);
				rel->npk = idx->nkeys;
				return;
			}
		}
	}
}

static void
index_apply_snap(PweCatalog *cat, const PweIndexSnap *s)
{
	PweIndex   *idx = pwe_hash_get(&cat->indexes, relkey(s->db, s->oid));

	if (!idx)
	{
		idx = pwe_calloc(1, sizeof(PweIndex));
		idx->oid = s->oid;
		idx->db = s->db;
		pwe_hash_put(&cat->indexes, relkey(s->db, s->oid), idx);
	}
	idx->relid = s->relid;
	idx->natts = s->natts;
	idx->nkeyatts = s->nkeyatts;
	idx->unique = s->unique;
	idx->primary = s->primary;
	idx->valid = s->valid;
	idx->partial = s->partial;
	idx->am = s->am;
	memcpy(idx->name, s->name, sizeof(idx->name));
	memcpy(idx->keys, s->keys, sizeof(idx->keys));
	idx->nkeys = s->nkeys;
	{
		PweRel	   *rel = pwe_catalog_rel_by_oid(cat, s->db, s->relid);

		if (rel)
			rebuild_pk(cat, rel);
	}
}

static void
con_apply_snap(PweCatalog *cat, const PweConSnap *s)
{
	PweCon	   *c = pwe_hash_get(&cat->constraints, relkey(s->db, s->oid));

	if (!c)
	{
		c = pwe_calloc(1, sizeof(PweCon));
		c->oid = s->oid;
		c->db = s->db;
		pwe_hash_put(&cat->constraints, relkey(s->db, s->oid), c);
	}
	c->relid = s->relid;
	c->typid = s->typid;
	c->indid = s->indid;
	c->contype = s->contype;
	memcpy(c->name, s->name, sizeof(c->name));
	free(c->keys);
	c->keys = s->nkeys ? pwe_malloc(sizeof(int16) * s->nkeys) : NULL;
	if (s->nkeys)
		memcpy(c->keys, s->keys, sizeof(int16) * s->nkeys);
	c->nkeys = s->nkeys;
	c->deferrable = s->deferrable;
	c->deferred = s->deferred;
	c->validated = s->validated;
	{
		PweRel	   *rel = pwe_catalog_rel_by_oid(cat, s->db, s->relid);

		if (rel)
			rebuild_pk(cat, rel);
	}
}

static void
enum_apply_snap(PweCatalog *cat, const PweEnumSnap *s)
{
	PweEnumVal *e = pwe_hash_get(&cat->enums, relkey(s->db, s->oid));

	if (!e)
	{
		e = pwe_calloc(1, sizeof(PweEnumVal));
		e->oid = s->oid;
		e->db = s->db;
		pwe_hash_put(&cat->enums, relkey(s->db, s->oid), e);
	}
	e->typid = s->typid;
	e->sortorder = s->sortorder;
	memcpy(e->label, s->label, sizeof(e->label));
}

static void
seq_apply_snap(PweCatalog *cat, const PweSeqSnap *s)
{
	PweSeq	   *q = pwe_hash_get(&cat->sequences, relkey(s->db, s->relid));

	if (!q)
	{
		q = pwe_calloc(1, sizeof(PweSeq));
		q->relid = s->relid;
		q->db = s->db;
		pwe_hash_put(&cat->sequences, relkey(s->db, s->relid), q);
	}
	q->typid = s->typid;
	q->start = s->start;
	q->inc = s->inc;
	q->maxv = s->maxv;
	q->minv = s->minv;
	q->cache = s->cache;
	q->cycle = s->cycle;
}

/* ------------------------------------------------------------------ */
/* catalog row handlers                                               */
/* ------------------------------------------------------------------ */
static uint32
row_oid(PweCatalog *cat, int catid, const PweTuple *t, PweValue *vals)
{
	int			oc = pwe_cat_oid_col(catid, cat->version);

	if (oc > 0)
		return val_u32(vals, oc);
	if (t->has_oid)
		return t->oid;
	return 0;
}

static void
event_add(PweTxn *txn, uint64 lsn, PweCatPayload *payload)
{
	PweEvent   *ev;

	if (!txn)
	{
		payload_free(payload);
		return;
	}
	ev = pwe_calloc(1, sizeof(PweEvent));
	ev->lsn = lsn;
	ev->xid = txn->xid;
	ev->kind = PWE_EV_CAT;
	ev->cat = payload;
	if (txn->tail)
		txn->tail->next = ev;
	else
		txn->events = ev;
	txn->tail = ev;
	txn->bytes += sizeof(PweEvent) + sizeof(PweCatPayload);
}

static PweCatPayload *
payload_new(int cat, int op)
{
	PweCatPayload *p = pwe_calloc(1, sizeof(PweCatPayload));

	p->cat = cat;
	p->op = op;
	return p;
}

static void
apply_pg_class(PweCatalog *cat, uint32 db, PweTuple *t, PweValue *vals,
			   PweTxn *txn, uint64 lsn, bool is_delete)
{
	uint32		oid = row_oid(cat, PWE_CAT_PG_CLASS, t, vals);
	PweRel	   *rel;
	PweRelSnap	nsnap;
	PweRelSnap	osnap;
	bool		exists;
	PweCatPayload *ev = NULL;

	if (oid == 0)
		return;
	rel = pwe_catalog_rel_by_oid(cat, db, oid);
	exists = rel != NULL;
	memset(&osnap, 0, sizeof(osnap));
	if (exists)
		rel_to_snap(rel, &osnap);
	memset(&nsnap, 0, sizeof(nsnap));
	nsnap.oid = oid;
	nsnap.db = db;
	nsnap.nsp = val_u32(vals, pwe_cat_col(PWE_CAT_PG_CLASS, cat->version, REL_NSP));
	nsnap.typid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_CLASS, cat->version, REL_TYPE));
	nsnap.filenode = val_u32(vals, pwe_cat_col(PWE_CAT_PG_CLASS, cat->version, REL_FILENODE));
	nsnap.toastrelid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_CLASS, cat->version, REL_TOAST));
	nsnap.am = val_u32(vals, pwe_cat_col(PWE_CAT_PG_CLASS, cat->version, REL_AM));
	nsnap.natts = val_i16(vals, pwe_cat_col(PWE_CAT_PG_CLASS, cat->version, REL_NATTS));
	nsnap.relkind = val_char(vals, pwe_cat_col(PWE_CAT_PG_CLASS, cat->version, REL_KIND));
	nsnap.persistence = val_char(vals, pwe_cat_col(PWE_CAT_PG_CLASS, cat->version, REL_PERSIST));
	val_name(vals, pwe_cat_col(PWE_CAT_PG_CLASS, cat->version, REL_NAME),
			 nsnap.name, sizeof(nsnap.name));
	if (oid >= PWE_FIRST_NORMAL_OID && txn)
	{
		ev = payload_new(PWE_CAT_PG_CLASS,
						 is_delete ? PWE_OP_DELETE : (exists ? PWE_OP_UPDATE : PWE_OP_INSERT));
		ev->n.rel = nsnap;
		if (exists)
			ev->o.rel = osnap;
	}
	if (is_delete)
	{
		PweRel	   *detached = detach_rel(cat, db, oid);

		if (ev)
			ev->saved_rel = detached;
		else
			free_rel(detached);
	}
	else
	{
		rel = rel_from_snap(cat, &nsnap);
		if (nsnap.natts > rel->natts)
			ensure_rel_capacity(rel, nsnap.natts);
	}
	if (ev)
		event_add(txn, lsn, ev);
}

static void
attr_snap_from_attr(PweAttr *a, uint32 db, uint32 relid, PweAttrSnap *s)
{
	memset(s, 0, sizeof(*s));
	s->db = db;
	s->relid = relid;
	if (!a)
		return;
	s->num = a->num;
	memcpy(s->name, a->name, sizeof(s->name));
	s->typid = a->typid;
	s->len = a->len;
	s->typmod = a->typmod;
	s->align = a->align;
	s->byval = a->byval;
	s->notnull = a->notnull;
	s->dropped = a->dropped;
	s->hasdef = a->hasdef;
	s->identity = a->identity;
	s->generated = a->generated;
	s->collation = a->collation;
}

static void
apply_pg_attribute(PweCatalog *cat, uint32 db, PweTuple *t, PweValue *vals,
				   PweTxn *txn, uint64 lsn, bool is_delete)
{
	PweAttrSnap snap;
	PweAttrSnap osnap;
	PweRel	   *rel;
	PweCatPayload *ev = NULL;
	int16		num;

	memset(&snap, 0, sizeof(snap));
	snap.db = db;
	snap.relid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_RELID));
	snap.num = val_i16(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_NUM));
	num = snap.num;
	snap.typid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_TYPID));
	snap.len = val_i16(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_LEN));
	snap.typmod = val_i32(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_TYPMOD));
	snap.align = val_char(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_ALIGN));
	snap.byval = val_bool(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_BYVAL));
	snap.notnull = val_bool(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_NOTNULL));
	snap.dropped = val_bool(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_DROPPED));
	snap.hasdef = val_bool(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_HASDEF));
	snap.identity = val_char(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_IDENTITY));
	snap.generated = val_char(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_GENERATED));
	snap.collation = val_u32(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_COLLATION));
	val_name(vals, pwe_cat_col(PWE_CAT_PG_ATTRIBUTE, cat->version, ATT_NAME),
			 snap.name, sizeof(snap.name));
	if (snap.relid == 0 || num == 0)
		return;
	if (num < 0)
		return;					/* system attributes are not needed */
	rel = pwe_catalog_rel_by_oid(cat, db, snap.relid);
	memset(&osnap, 0, sizeof(osnap));
	{
		PweAttr    *existing = NULL;

		if (rel && num > 0 && num <= rel->natts && rel->attrs)
			existing = rel->attrs[num];
		if (existing)
			attr_snap_from_attr(existing, db, snap.relid, &osnap);
	}
	if (snap.relid >= PWE_FIRST_NORMAL_OID && txn)
	{
		ev = payload_new(PWE_CAT_PG_ATTRIBUTE,
						 is_delete ? PWE_OP_DELETE :
						 (osnap.num != 0 ? PWE_OP_UPDATE : PWE_OP_INSERT));
		ev->n.attr = snap;
		if (osnap.num != 0)
			ev->o.attr = osnap;
	}
	if (is_delete)
		attr_remove(cat, db, snap.relid, num);
	else
		attr_apply_snap(cat, &snap);
	if (ev)
		event_add(txn, lsn, ev);
}

static void
apply_pg_namespace(PweCatalog *cat, uint32 db, PweTuple *t, PweValue *vals,
				   PweTxn *txn, uint64 lsn, bool is_delete)
{
	PweNsSnap	snap;
	PweCatPayload *ev = NULL;

	memset(&snap, 0, sizeof(snap));
	snap.oid = row_oid(cat, PWE_CAT_PG_NAMESPACE, t, vals);
	snap.db = db;
	val_name(vals, pwe_cat_col(PWE_CAT_PG_NAMESPACE, cat->version, NS_NAME),
			 snap.name, sizeof(snap.name));
	if (snap.oid == 0)
		return;
	if (snap.oid >= PWE_FIRST_NORMAL_OID && txn)
	{
		ev = payload_new(PWE_CAT_PG_NAMESPACE, is_delete ? PWE_OP_DELETE : PWE_OP_INSERT);
		ev->n.ns = snap;
	}
	if (is_delete)
	{
		char	   *old = pwe_hash_remove(&cat->namespaces, relkey(db, snap.oid));

		free(old);
	}
	else
		ns_apply_snap(cat, &snap);
	if (ev)
		event_add(txn, lsn, ev);
}

static void
apply_pg_type(PweCatalog *cat, uint32 db, PweTuple *t, PweValue *vals,
			  PweTxn *txn, uint64 lsn, bool is_delete)
{
	PweTypeSnap snap;
	PweCatPayload *ev = NULL;
	PweType    *existing;

	memset(&snap, 0, sizeof(snap));
	snap.oid = row_oid(cat, PWE_CAT_PG_TYPE, t, vals);
	snap.db = db;
	snap.nsp = val_u32(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_NSP));
	snap.typlen = val_i16(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_LEN));
	snap.byval = val_bool(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_BYVAL));
	snap.typtype = val_char(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_TYPE));
	snap.align = val_char(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_ALIGN));
	snap.relid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_RELID));
	snap.elem = val_u32(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_ELEM));
	snap.array = val_u32(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_ARRAY));
	snap.basetype = val_u32(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_BASETYPE));
	snap.typtypmod = val_i32(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_TYPMOD));
	snap.notnull = val_bool(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_NOTNULL));
	snap.defined = val_bool(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_DEFINED));
	val_name(vals, pwe_cat_col(PWE_CAT_PG_TYPE, cat->version, TY_NAME),
			 snap.name, sizeof(snap.name));
	if (snap.oid == 0)
		return;
	existing = pwe_hash_get(&cat->types, relkey(db, snap.oid));
	if (snap.oid >= PWE_FIRST_NORMAL_OID && txn)
	{
		ev = payload_new(PWE_CAT_PG_TYPE,
						 is_delete ? PWE_OP_DELETE : (existing ? PWE_OP_UPDATE : PWE_OP_INSERT));
		ev->n.typ = snap;
	}
	if (is_delete)
	{
		PweType    *old = pwe_hash_remove(&cat->types, relkey(db, snap.oid));

		if (old)
			free(old);
	}
	else
		type_apply_snap(cat, &snap);
	if (ev)
		event_add(txn, lsn, ev);
}

static void
apply_pg_index(PweCatalog *cat, uint32 db, PweTuple *t, PweValue *vals,
			   PweTxn *txn, uint64 lsn, bool is_delete)
{
	PweIndexSnap snap;
	PweCatPayload *ev = NULL;
	int			keycol = pwe_cat_col(PWE_CAT_PG_INDEX, cat->version, IDX_KEY);

	memset(&snap, 0, sizeof(snap));
	snap.db = db;
	snap.relid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_INDEX, cat->version, IDX_INDRELID));
	snap.oid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_INDEX, cat->version, IDX_INDEXRELID));
	if (snap.oid == 0)
		snap.oid = row_oid(cat, PWE_CAT_PG_INDEX, t, vals);
	snap.natts = val_i16(vals, pwe_cat_col(PWE_CAT_PG_INDEX, cat->version, IDX_NATTS));
	snap.nkeyatts = val_i16(vals, pwe_cat_col(PWE_CAT_PG_INDEX, cat->version, IDX_NKEYATTS));
	snap.unique = val_bool(vals, pwe_cat_col(PWE_CAT_PG_INDEX, cat->version, IDX_UNIQUE));
	snap.primary = val_bool(vals, pwe_cat_col(PWE_CAT_PG_INDEX, cat->version, IDX_PRIMARY));
	snap.valid = true;
	snap.am = 0;
	snap.partial = !val_null(vals, pwe_cat_col(PWE_CAT_PG_INDEX, cat->version, IDX_PRED));
	if (snap.oid == 0)
		return;
	if (keycol > 0)
		decode_int2vector(&vals[keycol - 1], snap.keys, 32, &snap.nkeys);
	{
		PweRel	   *irel = pwe_catalog_rel_by_oid(cat, db, snap.oid);

		if (irel)
		{
			memcpy(snap.name, irel->name, sizeof(snap.name));
			snap.am = irel->am;
		}
	}
	if (snap.oid >= PWE_FIRST_NORMAL_OID && txn)
	{
		ev = payload_new(PWE_CAT_PG_INDEX, is_delete ? PWE_OP_DELETE : PWE_OP_INSERT);
		ev->n.idx = snap;
	}
	if (is_delete)
	{
		PweIndex   *old = pwe_hash_remove(&cat->indexes, relkey(db, snap.oid));

		if (old)
			free(old);
	}
	else
		index_apply_snap(cat, &snap);
	if (ev)
		event_add(txn, lsn, ev);
}

static void
apply_pg_constraint(PweCatalog *cat, uint32 db, PweTuple *t, PweValue *vals,
					PweTxn *txn, uint64 lsn, bool is_delete)
{
	PweConSnap	snap;
	PweCatPayload *ev = NULL;
	int			keycol = pwe_cat_col(PWE_CAT_PG_CONSTRAINT, cat->version, CON_KEY);

	memset(&snap, 0, sizeof(snap));
	snap.oid = row_oid(cat, PWE_CAT_PG_CONSTRAINT, t, vals);
	snap.db = db;
	snap.relid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_CONSTRAINT, cat->version, CON_RELID));
	snap.typid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_CONSTRAINT, cat->version, CON_TYPID));
	snap.indid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_CONSTRAINT, cat->version, CON_INDID));
	snap.contype = val_char(vals, pwe_cat_col(PWE_CAT_PG_CONSTRAINT, cat->version, CON_TYPE));
	snap.deferrable = val_bool(vals, pwe_cat_col(PWE_CAT_PG_CONSTRAINT, cat->version, CON_DEFERRABLE));
	snap.deferred = val_bool(vals, pwe_cat_col(PWE_CAT_PG_CONSTRAINT, cat->version, CON_DEFERRED));
	snap.validated = val_bool(vals, pwe_cat_col(PWE_CAT_PG_CONSTRAINT, cat->version, CON_VALIDATED));
	val_name(vals, pwe_cat_col(PWE_CAT_PG_CONSTRAINT, cat->version, CON_NAME),
			 snap.name, sizeof(snap.name));
	if (keycol > 0)
		decode_int2array(&vals[keycol - 1], snap.keys, 32, &snap.nkeys);
	if (snap.oid == 0)
		return;
	if ((snap.relid >= PWE_FIRST_NORMAL_OID || snap.typid >= PWE_FIRST_NORMAL_OID ||
		 snap.oid >= PWE_FIRST_NORMAL_OID) && txn)
	{
		ev = payload_new(PWE_CAT_PG_CONSTRAINT, is_delete ? PWE_OP_DELETE : PWE_OP_INSERT);
		ev->n.con = snap;
	}
	if (is_delete)
	{
		PweCon	   *old = pwe_hash_remove(&cat->constraints, relkey(db, snap.oid));

		if (old)
			free(old);
	}
	else
		con_apply_snap(cat, &snap);
	if (ev)
		event_add(txn, lsn, ev);
}

static void
apply_pg_enum(PweCatalog *cat, uint32 db, PweTuple *t, PweValue *vals,
			  PweTxn *txn, uint64 lsn, bool is_delete)
{
	PweEnumSnap snap;
	PweCatPayload *ev = NULL;

	memset(&snap, 0, sizeof(snap));
	snap.oid = row_oid(cat, PWE_CAT_PG_ENUM, t, vals);
	snap.db = db;
	snap.typid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_ENUM, cat->version, ENUM_TYPID));
	{
		PweValue   *v = &vals[pwe_cat_col(PWE_CAT_PG_ENUM, cat->version, ENUM_SORT) - 1];
		uint32		bits = 0;

		if (!v->isnull)
		{
			bits = (uint32) (v->u & 0xffffffff);
			memcpy(&snap.sortorder, &bits, 4);
		}
	}
	val_name(vals, pwe_cat_col(PWE_CAT_PG_ENUM, cat->version, ENUM_LABEL),
			 snap.label, sizeof(snap.label));
	if (snap.oid == 0)
		return;
	if (snap.oid >= PWE_FIRST_NORMAL_OID && txn)
	{
		ev = payload_new(PWE_CAT_PG_ENUM, is_delete ? PWE_OP_DELETE : PWE_OP_INSERT);
		ev->n.en = snap;
	}
	if (is_delete)
	{
		PweEnumVal *old = pwe_hash_remove(&cat->enums, relkey(db, snap.oid));

		if (old)
			free(old);
	}
	else
		enum_apply_snap(cat, &snap);
	if (ev)
		event_add(txn, lsn, ev);
}

static void
apply_pg_sequence(PweCatalog *cat, uint32 db, PweTuple *t, PweValue *vals,
				  PweTxn *txn, uint64 lsn, bool is_delete)
{
	PweSeqSnap	snap;
	PweCatPayload *ev = NULL;

	(void) t;
	memset(&snap, 0, sizeof(snap));
	snap.relid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_SEQUENCE, cat->version, SEQ_RELID));
	snap.db = db;
	snap.typid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_SEQUENCE, cat->version, SEQ_TYPID));
	{
		PweValue   *v;

		v = &vals[pwe_cat_col(PWE_CAT_PG_SEQUENCE, cat->version, SEQ_START) - 1];
		snap.start = v->isnull ? 0 : (int64) v->u;
		v = &vals[pwe_cat_col(PWE_CAT_PG_SEQUENCE, cat->version, SEQ_INC) - 1];
		snap.inc = v->isnull ? 0 : (int64) v->u;
		v = &vals[pwe_cat_col(PWE_CAT_PG_SEQUENCE, cat->version, SEQ_MAX) - 1];
		snap.maxv = v->isnull ? 0 : (int64) v->u;
		v = &vals[pwe_cat_col(PWE_CAT_PG_SEQUENCE, cat->version, SEQ_MIN) - 1];
		snap.minv = v->isnull ? 0 : (int64) v->u;
		v = &vals[pwe_cat_col(PWE_CAT_PG_SEQUENCE, cat->version, SEQ_CACHE) - 1];
		snap.cache = v->isnull ? 0 : (int64) v->u;
		v = &vals[pwe_cat_col(PWE_CAT_PG_SEQUENCE, cat->version, SEQ_CYCLE) - 1];
		snap.cycle = !v->isnull && v->u != 0;
	}
	if (snap.relid == 0)
		return;
	if (snap.relid >= PWE_FIRST_NORMAL_OID && txn)
	{
		ev = payload_new(PWE_CAT_PG_SEQUENCE, is_delete ? PWE_OP_DELETE : PWE_OP_INSERT);
		ev->n.seq = snap;
	}
	if (is_delete)
	{
		PweSeq	   *old = pwe_hash_remove(&cat->sequences, relkey(db, snap.relid));

		if (old)
			free(old);
	}
	else
		seq_apply_snap(cat, &snap);
	if (ev)
		event_add(txn, lsn, ev);
}

static void
apply_pg_database(PweCatalog *cat, PweTuple *t, PweValue *vals, PweTxn *txn,
				  uint64 lsn, bool is_delete)
{
	uint32		oid = row_oid(cat, PWE_CAT_PG_DATABASE, t, vals);
	char		name[PWE_NAMEDATALEN];

	val_name(vals, pwe_cat_col(PWE_CAT_PG_DATABASE, cat->version, DB_NAME),
			 name, sizeof(name));
	if (oid == 0)
		return;
	if (is_delete)
	{
		char	   *old = pwe_hash_remove(&cat->dbs, oid);

		free(old);
	}
	else
	{
		char	   *old = pwe_hash_get(&cat->dbs, oid);

		if (old)
			free(old);
		pwe_hash_put(&cat->dbs, oid, pwe_strdup(name));
	}
	if (txn && oid >= PWE_FIRST_NORMAL_OID)
	{
		PweCatPayload *ev = payload_new(PWE_CAT_PG_DATABASE,
										is_delete ? PWE_OP_DELETE : PWE_OP_INSERT);
		PweNsSnap	ns;

		memset(&ns, 0, sizeof(ns));
		ns.oid = oid;
		snprintf(ns.name, sizeof(ns.name), "%s", name);
		ev->n.ns = ns;
		event_add(txn, lsn, ev);
	}
}

static void
apply_pg_authid(PweCatalog *cat, PweTuple *t, PweValue *vals)
{
	uint32		oid = row_oid(cat, PWE_CAT_PG_AUTHID, t, vals);
	char		name[PWE_NAMEDATALEN];
	char	   *old;

	val_name(vals, pwe_cat_col(PWE_CAT_PG_AUTHID, cat->version, ROLE_NAME),
			 name, sizeof(name));
	if (oid == 0)
		return;
	old = pwe_hash_get(&cat->roles, oid);
	if (old)
		free(old);
	pwe_hash_put(&cat->roles, oid, pwe_strdup(name));
}

static void
apply_pg_attrdef(PweCatalog *cat, uint32 db, PweTuple *t, PweValue *vals)
{
	uint32		relid = val_u32(vals, pwe_cat_col(PWE_CAT_PG_ATTRDEF, cat->version, DEF_RELID));
	int16		num = val_i16(vals, pwe_cat_col(PWE_CAT_PG_ATTRDEF, cat->version, DEF_NUM));
	PweRel	   *rel = pwe_catalog_rel_by_oid(cat, db, relid);

	(void) t;
	if (rel && num > 0 && num <= rel->natts && rel->attrs && rel->attrs[num])
		rel->attrs[num]->hasdef = true;
}

/* ------------------------------------------------------------------ */
/* toast tuples                                                       */
/* ------------------------------------------------------------------ */
static void
store_toast_tuple(PweCatalog *cat, uint32 db, uint32 reloid, PweTuple *tup,
				  uint32 filter_valueid)
{
	uint32		chunk_id = 0;
	int32		chunk_seq = 0;
	const uint8 *data = NULL;
	int			datalen = 0;
	uint32		off = 0;
	int			i;

	for (i = 0; i < 3; i++)
	{
		bool		nullbit = false;

		if (i < 2)
		{
			off = (off + 3u) & ~3u;
			if (tup->has_null && tup->nullbits)
				nullbit = (tup->nullbits[i / 8] & (1 << (i % 8))) == 0;
			if (nullbit)
			{
				off += 4;
				continue;
			}
			if (off + 4 > tup->datalen)
				return;
			if (i == 0)
				chunk_id = pwe_read_u32(tup->data + off);
			else
				chunk_seq = pwe_read_i32(tup->data + off);
			off += 4;
		}
		else
		{
			uint32		consumed;
			const uint8 *p;

			off = (off + 3u) & ~3u;
			if (off + 1 > tup->datalen)
				return;
			if (tup->has_null && tup->nullbits &&
				!(tup->nullbits[2 / 8] & (1 << (2 % 8))))
				return;
			/* bit set means the attribute is NOT null */
			p = tup->data + off;
			if ((p[0] & 0x01) == 0)
			{
				uint32		len;

				if (off + 4 > tup->datalen)
					return;
				len = (pwe_read_u32(p) >> 2) & 0x3FFFFFFF;
				if (len < 4 || off + len > tup->datalen)
					return;
				data = p + 4;
				datalen = (int) (len - 4);
				consumed = len;
			}
			else if (p[0] == 0x01)
				return;
			else
			{
				uint32		len = (p[0] >> 1) & 0x7F;

				if (len < 1 || off + len > tup->datalen)
					return;
				data = p + 1;
				datalen = (int) (len - 1);
				consumed = len;
			}
			off += consumed;
		}
	}
	if (data && chunk_id != 0 &&
		(filter_valueid == 0 || chunk_id == filter_valueid))
		pwe_catalog_add_toast_chunk(cat, db, reloid, chunk_id, chunk_seq,
									data, (uint32) datalen);
}


/* ------------------------------------------------------------------ */
/* catalog row tracking (ctid -> row snapshot)                        */
/* ------------------------------------------------------------------ */
static uint64
ctid_key(uint32 blkno, uint16 offnum)
{
	return ((uint64) blkno << 16) | offnum;
}

static PweHash *
cat_row_map(PweCatalog *cat, uint32 db, uint32 reloid, bool create)
{
	uint64		key = relkey(db, reloid);
	PweHash    *map = pwe_hash_get(&cat->cat_rows, key);

	if (!map && create)
	{
		map = pwe_malloc(sizeof(PweHash));
		pwe_hash_init(map);
		pwe_hash_put(&cat->cat_rows, key, map);
	}
	return map;
}

static void
track_row(PweCatalog *cat, uint32 db, uint32 reloid, uint32 blkno, uint16 offnum,
		  PweCatPayload *p, const uint8 *raw, uint32 rawlen)
{
	PweHash    *map = cat_row_map(cat, db, reloid, true);
	PweCatPayload *old;

	if (raw && rawlen)
	{
		p->raw = pwe_malloc(rawlen);
		memcpy(p->raw, raw, rawlen);
		p->rawlen = rawlen;
	}
	old = pwe_hash_put(map, ctid_key(blkno, offnum), p);
	if (old)
		payload_free(old);
}

static PweCatPayload *
lookup_row(PweCatalog *cat, uint32 db, uint32 reloid, uint32 blkno, uint16 offnum)
{
	PweHash    *map = cat_row_map(cat, db, reloid, false);

	if (!map)
		return NULL;
	return pwe_hash_get(map, ctid_key(blkno, offnum));
}

bool
pwe_reconstruct_update(const uint8 *old_raw, uint32 old_rawlen,
					   const uint8 *data, uint32 datalen, uint8 flags,
					   uint8 **out, uint32 *outlen)
{
	const uint8 *p = data;
	uint32		rem = datalen;
	uint16		prefix = 0;
	uint16		suffix = 0;
	const uint8 *frag;
	uint32		fragen;
	uint8		hoff;
	uint32		headlen;
	uint8		old_hoff;
	uint32		old_data_off;
	uint32		old_data_len;
	uint32		total;
	uint8	   *buf;

	if (!old_raw || old_rawlen < 5)
		return false;
	if (flags & 0x20)
	{
		if (rem < 2)
			return false;
		prefix = pwe_read_u16(p);
		p += 2;
		rem -= 2;
	}
	if (flags & 0x40)
	{
		if (rem < 2)
			return false;
		suffix = pwe_read_u16(p);
		p += 2;
		rem -= 2;
	}
	if (rem < 5)
		return false;
	frag = p + 5;
	fragen = rem - 5;
	hoff = p[4];
	if (hoff < 23)
		return false;
	headlen = (uint32) hoff - 23;
	if (headlen > fragen)
		return false;
	old_hoff = old_raw[4];
	if (old_hoff < 23)
		return false;
	old_data_off = 5 + ((uint32) old_hoff - 23);
	old_data_len = old_rawlen > old_data_off ? old_rawlen - old_data_off : 0;
	if (prefix > old_data_len || suffix > old_rawlen)
		return false;
	total = 5 + headlen + prefix + (fragen - headlen) + suffix;
	buf = pwe_malloc(total);
	memcpy(buf, p, 5);
	memcpy(buf + 5, frag, headlen);
	memcpy(buf + 5 + headlen, old_raw + old_data_off, prefix);
	memcpy(buf + 5 + headlen + prefix, frag + headlen, fragen - headlen);
	memcpy(buf + 5 + headlen + prefix + (fragen - headlen),
		   old_raw + old_rawlen - suffix, suffix);
	*out = buf;
	*outlen = total;
	return true;
}

bool
pwe_catalog_reconstruct_update(PweCatalog *cat, uint32 db, uint32 relfilenode,
							   uint32 old_blkno, uint16 old_offnum,
							   const uint8 *data, uint32 datalen, uint8 flags,
							   uint8 **out, uint32 *outlen)
{
	PweCatPayload *old = lookup_row(cat, db, relfilenode, old_blkno, old_offnum);

	if (!old)
		return false;
	return pwe_reconstruct_update(old->raw, old->rawlen, data, datalen, flags,
								  out, outlen);
}

static PweCatPayload *
untrack_row(PweCatalog *cat, uint32 db, uint32 reloid, uint32 blkno, uint16 offnum)
{
	PweHash    *map = cat_row_map(cat, db, reloid, false);

	if (!map)
		return NULL;
	return pwe_hash_remove(map, ctid_key(blkno, offnum));
}

static void
snapshot_from_row(PweCatalog *cat, int catid, uint32 db, PweTuple *t,
				  PweValue *vals, PweCatPayload *p)
{
	memset(p, 0, sizeof(*p));
	p->cat = catid;
	p->op = PWE_OP_INSERT;
	switch (catid)
	{
		case PWE_CAT_PG_CLASS:
			p->n.rel.oid = row_oid(cat, catid, t, vals);
			p->n.rel.db = db;
			p->n.rel.nsp = val_u32(vals, pwe_cat_col(catid, cat->version, REL_NSP));
			p->n.rel.typid = val_u32(vals, pwe_cat_col(catid, cat->version, REL_TYPE));
			p->n.rel.filenode = val_u32(vals, pwe_cat_col(catid, cat->version, REL_FILENODE));
			p->n.rel.toastrelid = val_u32(vals, pwe_cat_col(catid, cat->version, REL_TOAST));
			p->n.rel.am = val_u32(vals, pwe_cat_col(catid, cat->version, REL_AM));
			p->n.rel.natts = val_i16(vals, pwe_cat_col(catid, cat->version, REL_NATTS));
			p->n.rel.relkind = val_char(vals, pwe_cat_col(catid, cat->version, REL_KIND));
			p->n.rel.persistence = val_char(vals, pwe_cat_col(catid, cat->version, REL_PERSIST));
			val_name(vals, pwe_cat_col(catid, cat->version, REL_NAME),
					 p->n.rel.name, sizeof(p->n.rel.name));
			break;
		case PWE_CAT_PG_ATTRIBUTE:
			p->n.attr.db = db;
			p->n.attr.relid = val_u32(vals, pwe_cat_col(catid, cat->version, ATT_RELID));
			p->n.attr.num = val_i16(vals, pwe_cat_col(catid, cat->version, ATT_NUM));
			p->n.attr.typid = val_u32(vals, pwe_cat_col(catid, cat->version, ATT_TYPID));
			p->n.attr.len = val_i16(vals, pwe_cat_col(catid, cat->version, ATT_LEN));
			p->n.attr.typmod = val_i32(vals, pwe_cat_col(catid, cat->version, ATT_TYPMOD));
			p->n.attr.align = val_char(vals, pwe_cat_col(catid, cat->version, ATT_ALIGN));
			p->n.attr.byval = val_bool(vals, pwe_cat_col(catid, cat->version, ATT_BYVAL));
			p->n.attr.notnull = val_bool(vals, pwe_cat_col(catid, cat->version, ATT_NOTNULL));
			p->n.attr.dropped = val_bool(vals, pwe_cat_col(catid, cat->version, ATT_DROPPED));
			p->n.attr.hasdef = val_bool(vals, pwe_cat_col(catid, cat->version, ATT_HASDEF));
			p->n.attr.identity = val_char(vals, pwe_cat_col(catid, cat->version, ATT_IDENTITY));
			p->n.attr.generated = val_char(vals, pwe_cat_col(catid, cat->version, ATT_GENERATED));
			p->n.attr.collation = val_u32(vals, pwe_cat_col(catid, cat->version, ATT_COLLATION));
			val_name(vals, pwe_cat_col(catid, cat->version, ATT_NAME),
					 p->n.attr.name, sizeof(p->n.attr.name));
			break;
		case PWE_CAT_PG_NAMESPACE:
			p->n.ns.oid = row_oid(cat, catid, t, vals);
			p->n.ns.db = db;
			val_name(vals, pwe_cat_col(catid, cat->version, NS_NAME),
					 p->n.ns.name, sizeof(p->n.ns.name));
			break;
		case PWE_CAT_PG_TYPE:
			p->n.typ.oid = row_oid(cat, catid, t, vals);
			p->n.typ.db = db;
			p->n.typ.nsp = val_u32(vals, pwe_cat_col(catid, cat->version, TY_NSP));
			p->n.typ.typlen = val_i16(vals, pwe_cat_col(catid, cat->version, TY_LEN));
			p->n.typ.byval = val_bool(vals, pwe_cat_col(catid, cat->version, TY_BYVAL));
			p->n.typ.typtype = val_char(vals, pwe_cat_col(catid, cat->version, TY_TYPE));
			p->n.typ.align = val_char(vals, pwe_cat_col(catid, cat->version, TY_ALIGN));
			p->n.typ.relid = val_u32(vals, pwe_cat_col(catid, cat->version, TY_RELID));
			p->n.typ.elem = val_u32(vals, pwe_cat_col(catid, cat->version, TY_ELEM));
			p->n.typ.array = val_u32(vals, pwe_cat_col(catid, cat->version, TY_ARRAY));
			p->n.typ.basetype = val_u32(vals, pwe_cat_col(catid, cat->version, TY_BASETYPE));
			p->n.typ.typtypmod = val_i32(vals, pwe_cat_col(catid, cat->version, TY_TYPMOD));
			p->n.typ.notnull = val_bool(vals, pwe_cat_col(catid, cat->version, TY_NOTNULL));
			p->n.typ.defined = val_bool(vals, pwe_cat_col(catid, cat->version, TY_DEFINED));
			val_name(vals, pwe_cat_col(catid, cat->version, TY_NAME),
					 p->n.typ.name, sizeof(p->n.typ.name));
			break;
		case PWE_CAT_PG_INDEX:
			p->n.idx.db = db;
			p->n.idx.oid = val_u32(vals, pwe_cat_col(catid, cat->version, IDX_INDEXRELID));
			if (p->n.idx.oid == 0)
				p->n.idx.oid = row_oid(cat, catid, t, vals);
			p->n.idx.relid = val_u32(vals, pwe_cat_col(catid, cat->version, IDX_INDRELID));
			p->n.idx.natts = val_i16(vals, pwe_cat_col(catid, cat->version, IDX_NATTS));
			p->n.idx.nkeyatts = val_i16(vals, pwe_cat_col(catid, cat->version, IDX_NKEYATTS));
			p->n.idx.unique = val_bool(vals, pwe_cat_col(catid, cat->version, IDX_UNIQUE));
			p->n.idx.primary = val_bool(vals, pwe_cat_col(catid, cat->version, IDX_PRIMARY));
			p->n.idx.valid = true;
			{
				int			keycol = pwe_cat_col(catid, cat->version, IDX_KEY);

				if (keycol > 0)
					decode_int2vector(&vals[keycol - 1], p->n.idx.keys, 32,
									  &p->n.idx.nkeys);
			}
			{
				PweRel	   *irel = pwe_catalog_rel_by_oid(cat, db, p->n.idx.oid);

				if (irel)
				{
					memcpy(p->n.idx.name, irel->name, sizeof(p->n.idx.name));
					p->n.idx.am = irel->am;
				}
			}
			break;
		case PWE_CAT_PG_CONSTRAINT:
			p->n.con.oid = row_oid(cat, catid, t, vals);
			p->n.con.db = db;
			p->n.con.relid = val_u32(vals, pwe_cat_col(catid, cat->version, CON_RELID));
			p->n.con.typid = val_u32(vals, pwe_cat_col(catid, cat->version, CON_TYPID));
			p->n.con.indid = val_u32(vals, pwe_cat_col(catid, cat->version, CON_INDID));
			p->n.con.contype = val_char(vals, pwe_cat_col(catid, cat->version, CON_TYPE));
			p->n.con.deferrable = val_bool(vals, pwe_cat_col(catid, cat->version, CON_DEFERRABLE));
			p->n.con.deferred = val_bool(vals, pwe_cat_col(catid, cat->version, CON_DEFERRED));
			p->n.con.validated = val_bool(vals, pwe_cat_col(catid, cat->version, CON_VALIDATED));
			val_name(vals, pwe_cat_col(catid, cat->version, CON_NAME),
					 p->n.con.name, sizeof(p->n.con.name));
			{
				int			keycol = pwe_cat_col(catid, cat->version, CON_KEY);

				if (keycol > 0)
					decode_int2array(&vals[keycol - 1], p->n.con.keys, 32,
									 &p->n.con.nkeys);
			}
			break;
		case PWE_CAT_PG_ENUM:
			p->n.en.oid = row_oid(cat, catid, t, vals);
			p->n.en.db = db;
			p->n.en.typid = val_u32(vals, pwe_cat_col(catid, cat->version, ENUM_TYPID));
			{
				PweValue   *v = &vals[pwe_cat_col(catid, cat->version, ENUM_SORT) - 1];
				uint32		bits = 0;

				if (!v->isnull)
				{
					bits = (uint32) (v->u & 0xffffffff);
					memcpy(&p->n.en.sortorder, &bits, 4);
				}
			}
			val_name(vals, pwe_cat_col(catid, cat->version, ENUM_LABEL),
					 p->n.en.label, sizeof(p->n.en.label));
			break;
		case PWE_CAT_PG_SEQUENCE:
			p->n.seq.relid = val_u32(vals, pwe_cat_col(catid, cat->version, SEQ_RELID));
			p->n.seq.db = db;
			p->n.seq.typid = val_u32(vals, pwe_cat_col(catid, cat->version, SEQ_TYPID));
			{
				PweValue   *v;

				v = &vals[pwe_cat_col(catid, cat->version, SEQ_START) - 1];
				p->n.seq.start = v->isnull ? 0 : (int64) v->u;
				v = &vals[pwe_cat_col(catid, cat->version, SEQ_INC) - 1];
				p->n.seq.inc = v->isnull ? 0 : (int64) v->u;
				v = &vals[pwe_cat_col(catid, cat->version, SEQ_MAX) - 1];
				p->n.seq.maxv = v->isnull ? 0 : (int64) v->u;
				v = &vals[pwe_cat_col(catid, cat->version, SEQ_MIN) - 1];
				p->n.seq.minv = v->isnull ? 0 : (int64) v->u;
				v = &vals[pwe_cat_col(catid, cat->version, SEQ_CACHE) - 1];
				p->n.seq.cache = v->isnull ? 0 : (int64) v->u;
				v = &vals[pwe_cat_col(catid, cat->version, SEQ_CYCLE) - 1];
				p->n.seq.cycle = !v->isnull && v->u != 0;
			}
			break;
		case PWE_CAT_PG_DATABASE:
			p->n.ns.oid = row_oid(cat, catid, t, vals);
			p->n.ns.db = 0;
			val_name(vals, pwe_cat_col(catid, cat->version, DB_NAME),
					 p->n.ns.name, sizeof(p->n.ns.name));
			break;
		default:
			break;
	}
}

static void
remove_by_snapshot(PweCatalog *cat, PweCatPayload *p, PweRel **saved)
{
	switch (p->cat)
	{
		case PWE_CAT_PG_CLASS:
			{
				PweRel	   *rel = detach_rel(cat, p->n.rel.db, p->n.rel.oid);

				if (saved)
					*saved = rel;
				else
					free_rel(rel);
			}
			break;
		case PWE_CAT_PG_ATTRIBUTE:
			attr_remove(cat, p->n.attr.db, p->n.attr.relid, p->n.attr.num);
			break;
		case PWE_CAT_PG_NAMESPACE:
			{
				char	   *old = pwe_hash_remove(&cat->namespaces,
												 relkey(p->n.ns.db, p->n.ns.oid));

				free(old);
			}
			break;
		case PWE_CAT_PG_TYPE:
			{
				PweType    *old = pwe_hash_remove(&cat->types,
												  relkey(p->n.typ.db, p->n.typ.oid));

				free(old);
			}
			break;
		case PWE_CAT_PG_INDEX:
			{
				PweIndex   *old = pwe_hash_remove(&cat->indexes,
												  relkey(p->n.idx.db, p->n.idx.oid));

				free(old);
			}
			break;
		case PWE_CAT_PG_CONSTRAINT:
			{
				PweCon	   *old = pwe_hash_remove(&cat->constraints,
												 relkey(p->n.con.db, p->n.con.oid));

				free(old);
			}
			break;
		case PWE_CAT_PG_ENUM:
			{
				PweEnumVal *old = pwe_hash_remove(&cat->enums,
												  relkey(p->n.en.db, p->n.en.oid));

				free(old);
			}
			break;
		case PWE_CAT_PG_SEQUENCE:
			{
				PweSeq	   *old = pwe_hash_remove(&cat->sequences,
												 relkey(p->n.seq.db, p->n.seq.relid));

				free(old);
			}
			break;
		case PWE_CAT_PG_DATABASE:
			{
				char	   *old = pwe_hash_remove(&cat->dbs, p->n.ns.oid);

				free(old);
			}
			break;
		default:
			break;
	}
}

static bool
snapshot_is_user(PweCatalog *cat, PweCatPayload *p)
{
	(void) cat;
	switch (p->cat)
	{
		case PWE_CAT_PG_CLASS:
			return p->n.rel.oid >= PWE_FIRST_NORMAL_OID;
		case PWE_CAT_PG_ATTRIBUTE:
			return p->n.attr.relid >= PWE_FIRST_NORMAL_OID;
		case PWE_CAT_PG_NAMESPACE:
			return p->n.ns.oid >= PWE_FIRST_NORMAL_OID;
		case PWE_CAT_PG_TYPE:
			return p->n.typ.oid >= PWE_FIRST_NORMAL_OID;
		case PWE_CAT_PG_INDEX:
			return p->n.idx.oid >= PWE_FIRST_NORMAL_OID;
		case PWE_CAT_PG_CONSTRAINT:
			return p->n.con.oid >= PWE_FIRST_NORMAL_OID ||
				p->n.con.relid >= PWE_FIRST_NORMAL_OID ||
				p->n.con.typid >= PWE_FIRST_NORMAL_OID;
		case PWE_CAT_PG_ENUM:
			return p->n.en.oid >= PWE_FIRST_NORMAL_OID;
		case PWE_CAT_PG_SEQUENCE:
			return p->n.seq.relid >= PWE_FIRST_NORMAL_OID;
		case PWE_CAT_PG_DATABASE:
			return p->n.ns.oid >= PWE_FIRST_NORMAL_OID;
		default:
			return false;
	}
}


static void
load_toast_from_disk(PweCatalog *cat, PweRel *rel, uint32 db,
					 uint32 filter_valueid)
{
	char		path[4096];
	int			fd;
	uint8	   *page;
	ssize_t		n;

	if (!cat->pgdata || !rel)
		return;
	snprintf(path, sizeof(path), "%s/base/%u/%u", cat->pgdata, db,
			 rel->filenode ? rel->filenode : rel->oid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return;
	page = pwe_malloc(PWE_CAT_PAGE_SIZE);
	while ((n = read(fd, page, PWE_CAT_PAGE_SIZE)) == PWE_CAT_PAGE_SIZE)
	{
		uint32		lower = pwe_read_u16(page + 12);
		uint32		nitems;
		uint32		i;

		if (lower < 24 || lower > PWE_CAT_PAGE_SIZE)
			continue;
		nitems = (lower - 24) / 4;
		for (i = 0; i < nitems; i++)
		{
			uint32		v = pwe_read_u32(page + 24 + i * 4);
			uint32		off = v & 0x7FFF;
			uint32		flags = (v >> 15) & 3;
			uint32		len = v >> 17;
			const uint8 *tup;
			uint16		infomask;
			uint8	   *payload;
			uint32		plen;
			PweTuple	dt;
			char		err[128];

			if (flags != 1 || off < 24 || len < 23 ||
				off + len > PWE_CAT_PAGE_SIZE)
				continue;
			tup = page + off;
			infomask = pwe_read_u16(tup + 20);
			if (!pwe_catalog_tuple_live(cat, tup, infomask))
				continue;
			plen = 5 + (len - 23);
			payload = pwe_malloc(plen);
			pwe_write_u16(payload, pwe_read_u16(tup + 18));
			pwe_write_u16(payload + 2, infomask);
			payload[4] = tup[22];
			memcpy(payload + 5, tup + 23, len - 23);
			if (pwe_tuple_parse(payload, plen, &dt, err, sizeof(err)))
				store_toast_tuple(cat, db, rel->oid, &dt, filter_valueid);
			free(payload);
		}
	}
	free(page);
	close(fd);
}

/* ------------------------------------------------------------------ */
/* offline catalog seeding (reads the data directory, no server)      */
/* ------------------------------------------------------------------ */
static int
xact_status(PweCatalog *cat, uint32 xid)
{
	uint32		seg;
	uint32		idx;
	uint32		shift;

	if (xid < 3)
		return 1;				/* Invalid/Frozen/Bootstrap: treat as committed */
	if (!cat->pgdata)
		return 1;
	seg = xid >> 15;			/* 32768 xids per 8K pg_xact page */
	idx = (xid & 32767) >> 2;
	shift = (xid & 3) * 2;
	if (seg != cat->xact_seg)
	{
		char		path[4096];
		int			fd;

		cat->xact_seg = seg;
		cat->xact_ok = false;
		snprintf(path, sizeof(path), "%s/pg_xact/%04X", cat->pgdata, seg);
		fd = open(path, O_RDONLY);
		if (fd >= 0)
		{
			ssize_t		n = pread(fd, cat->xact_buf, sizeof(cat->xact_buf), 0);

			close(fd);
			if (n >= (ssize_t) (idx + 1))
				cat->xact_ok = true;
		}
	}
	if (!cat->xact_ok)
		return 1;				/* unknown: keep the tuple */
	return (cat->xact_buf[idx] >> shift) & 3;
}

bool
pwe_catalog_tuple_live(PweCatalog *cat, const uint8 *tuple, uint16 infomask)
{
	uint32		xmin = pwe_read_u32(tuple);
	uint32		xmax = pwe_read_u32(tuple + 4);
	int			st;

	if ((infomask & 0x0300) == 0x0300 || xmin == 2 ||
		(infomask & 0x0100) != 0)	/* frozen / committed hint */
		st = 1;
	else
		st = xact_status(cat, xmin);
	if (st != 1 && st != 3)
		return false;			/* aborted or in-progress inserter */

	if (infomask & 0x0800)		/* HEAP_XMAX_INVALID */
		return true;
	if (infomask & 0x0080)		/* HEAP_XMAX_LOCK_ONLY */
		return true;
	if (infomask & 0x1000)		/* Multixact: keep (conservative) */
		return true;
	st = xact_status(cat, xmax);
	return !(st == 1 || st == 3);
}

static void
seed_catalog_page(PweCatalog *cat, uint32 db, uint32 catoid, const uint8 *page,
				  uint32 blkno)
{
	uint32		lower;
	uint32		nitems;
	uint32		i;

	lower = pwe_read_u16(page + 12);
	if (lower < 24 || lower > PWE_CAT_PAGE_SIZE)
		return;
	nitems = (lower - 24) / 4;
	for (i = 0; i < nitems; i++)
	{
		uint32		v = pwe_read_u32(page + 24 + i * 4);
		uint32		off = v & 0x7FFF;
		uint32		flags = (v >> 15) & 3;
		uint32		len = v >> 17;
		const uint8 *tup;
		uint16		infomask;
		uint8		hoff;
		uint8	   *payload;
		uint32		plen;
		PweRecord	fake;

		if (flags != 1)			/* LP_NORMAL only */
			continue;
		if (off < 24 || len < 23 || off + len > PWE_CAT_PAGE_SIZE)
			continue;
		tup = page + off;
		infomask = pwe_read_u16(tup + 20);
		if (!pwe_catalog_tuple_live(cat, tup, infomask))
			continue;
		hoff = tup[22];
		if (hoff < 23 || hoff > len)
			continue;
		plen = 5 + (len - 23);
		payload = pwe_malloc(plen);
		pwe_write_u16(payload, pwe_read_u16(tup + 18));
		pwe_write_u16(payload + 2, infomask);
		payload[4] = hoff;
		memcpy(payload + 5, tup + 23, len - 23);
		memset(&fake, 0, sizeof(fake));
		pwe_catalog_process_tuple(cat, &fake, NULL, db, catoid, blkno,
								  (uint16) (i + 1), UINT32_MAX, 0,
								  payload, plen, false, false);
		free(payload);
	}
}

static void
seed_catalog_file(PweCatalog *cat, uint32 db, uint32 catoid)
{
	char		path[4096];
	int			fd;
	uint8	   *page;
	uint32		blkno = 0;
	ssize_t		n;

	if (db == 0)
		snprintf(path, sizeof(path), "%s/global/%u", cat->pgdata, catoid);
	else
		snprintf(path, sizeof(path), "%s/base/%u/%u", cat->pgdata, db, catoid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return;
	page = pwe_malloc(PWE_CAT_PAGE_SIZE);
	while ((n = read(fd, page, PWE_CAT_PAGE_SIZE)) == PWE_CAT_PAGE_SIZE)
	{
		seed_catalog_page(cat, db, catoid, page, blkno);
		blkno++;
	}
	free(page);
	close(fd);
}

void
pwe_catalog_seed_from_pgdata(PweCatalog *cat, const char *pgdata, uint32 only_db)
{
	static const uint32 db_catalogs[] = {
		2615, 1247, 1259, 1249, 2610, 2606, 3501, 2224, 2604
	};
	DIR		   *dir;
	struct dirent *de;
	size_t		i;

	if (!cat || !pgdata || !pgdata[0])
		return;
	cat->pgdata = pwe_strdup(pgdata);
	cat->xact_fd = -1;
	cat->xact_seg = UINT32_MAX;
	cat->xact_ok = false;

	/* shared catalogs */
	seed_catalog_file(cat, 0, 1262);	/* pg_database */
	seed_catalog_file(cat, 0, 1260);	/* pg_authid */
	seed_catalog_file(cat, 0, 1213);	/* pg_tablespace */

	dir = opendir(pgdata);
	if (!dir)
		return;
	closedir(dir);

	{
		char		base[4096];

		snprintf(base, sizeof(base), "%s/base", pgdata);
		dir = opendir(base);
		if (!dir)
			return;
		while ((de = readdir(dir)) != NULL)
		{
			uint32		db;
			char	   *end;
			unsigned long v;

			if (de->d_name[0] == '.')
				continue;
			v = strtoul(de->d_name, &end, 10);
			if (*end != '\0' || v == 0 || v > 0xFFFFFFFFUL)
				continue;
			db = (uint32) v;
			if (only_db && db != only_db)
				continue;
			for (i = 0; i < sizeof(db_catalogs) / sizeof(db_catalogs[0]); i++)
				seed_catalog_file(cat, db, db_catalogs[i]);
		}
		closedir(dir);
	}
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */
static bool
catalog_object_exists(PweCatalog *cat, int catid, uint32 db, PweCatPayload *p)
{
	switch (catid)
	{
		case PWE_CAT_PG_CLASS:
			return pwe_catalog_rel_by_oid(cat, db, p->n.rel.oid) != NULL;
		case PWE_CAT_PG_ATTRIBUTE:
			{
				PweRel	   *rel = pwe_catalog_rel_by_oid(cat, db, p->n.attr.relid);

				return rel && p->n.attr.num > 0 && p->n.attr.num <= rel->natts &&
					rel->attrs && rel->attrs[p->n.attr.num] != NULL;
			}
		case PWE_CAT_PG_NAMESPACE:
			return pwe_hash_get(&cat->namespaces, relkey(db, p->n.ns.oid)) != NULL;
		case PWE_CAT_PG_TYPE:
			return pwe_hash_get(&cat->types, relkey(db, p->n.typ.oid)) != NULL;
		case PWE_CAT_PG_INDEX:
			return pwe_hash_get(&cat->indexes, relkey(db, p->n.idx.oid)) != NULL;
		case PWE_CAT_PG_CONSTRAINT:
			return pwe_hash_get(&cat->constraints, relkey(db, p->n.con.oid)) != NULL;
		case PWE_CAT_PG_ENUM:
			return pwe_hash_get(&cat->enums, relkey(db, p->n.en.oid)) != NULL;
		case PWE_CAT_PG_SEQUENCE:
			return pwe_hash_get(&cat->sequences, relkey(db, p->n.seq.relid)) != NULL;
		case PWE_CAT_PG_DATABASE:
			return pwe_hash_get(&cat->dbs, p->n.ns.oid) != NULL;
		default:
			return false;
	}
}

void
pwe_catalog_process_tuple(PweCatalog *cat, PweRecord *rec, PweTxn *txn,
						  uint32 db, uint32 relfilenode,
						  uint32 blkno, uint16 offnum,
						  uint32 old_blkno, uint16 old_offnum,
						  const uint8 *payload, uint32 len, bool is_delete,
						  bool fill_missing)
{
	PweTuple	tup;
	char		err[256];
	int			catid;
	PweRel	   *rel;
	int			natts;
	PweValue   *vals = NULL;

	if (txn && txn->db == 0 && db != 0)
		txn->db = db;
	catid = pwe_cat_lookup_oid(relfilenode);
	if (catid < 0)
	{
		rel = pwe_catalog_rel_by_file(cat, db, relfilenode);
		if (rel && !is_delete &&
			(rel->relkind == 't' || strncmp(rel->name, "pg_toast_", 9) == 0) &&
			pwe_tuple_parse(payload, len, &tup, err, sizeof(err)))
			store_toast_tuple(cat, db, rel->oid, &tup, 0);
		return;
	}
	natts = pwe_cat_natts(catid, cat->version);
	if (!pwe_cat_typecodes(catid, cat->version))
		return;

	if (is_delete)
	{
		PweCatPayload *snap = lookup_row(cat, db, relfilenode, blkno, offnum);
		PweCatPayload *ev = NULL;
		PweRel	   *saved = NULL;

		if (!snap && payload && len > 0 &&
			pwe_tuple_parse(payload, len, &tup, err, sizeof(err)))
		{
			PweValue   *dv = NULL;

			if (pwe_tuple_decode_catalog(&tup, pwe_cat_typecodes(catid, cat->version),
										 natts, &dv, err, sizeof(err)))
			{
				snap = pwe_calloc(1, sizeof(PweCatPayload));
				snapshot_from_row(cat, catid, db, &tup, dv, snap);
				pwe_values_free(dv, natts);
			}
		}
		if (!snap)
			return;
		if (snapshot_is_user(cat, snap))
		{
			ev = pwe_calloc(1, sizeof(PweCatPayload));
			*ev = *snap;
			ev->raw = NULL;
			ev->rawlen = 0;
			ev->op = PWE_OP_DELETE;
			memcpy(&ev->o, &snap->n, sizeof(ev->o));
			/* keep deleted relation object for abort undo */
			remove_by_snapshot(cat, snap, catid == PWE_CAT_PG_CLASS ? &saved : NULL);
			if (saved)
				ev->saved_rel = saved;
			if (txn)
				event_add(txn, rec->lsn, ev);
			else
				pwe_catalog_payload_cleanup(cat, ev);
		}
		else
			remove_by_snapshot(cat, snap, NULL);
		{
			PweCatPayload *tracked = untrack_row(cat, db, relfilenode, blkno, offnum);

			if (tracked)
				payload_free(tracked);
			else
				payload_free(snap);
		}
		return;
	}

	if (!pwe_tuple_parse(payload, len, &tup, err, sizeof(err)))
		return;
	if (!pwe_tuple_decode_catalog(&tup, pwe_cat_typecodes(catid, cat->version),
								  natts, &vals, err, sizeof(err)))
		return;

	if (fill_missing)
	{
		PweCatPayload probe;

		snapshot_from_row(cat, catid, db, &tup, vals, &probe);
		if (catalog_object_exists(cat, catid, db, &probe))
		{
			pwe_values_free(vals, natts);
			return;
		}
		if (catid == PWE_CAT_PG_ENUM)
		{
			size_t		bi;

			for (bi = 0; bi < cat->enums.nbuckets; bi++)
			{
				PweHashEntry *e;

				for (e = cat->enums.buckets[bi]; e; e = e->next)
				{
					PweEnumVal *ev = e->val;

					if (ev->db == db && ev->typid == probe.n.en.typid &&
						strcmp(ev->label, probe.n.en.label) == 0)
					{
						pwe_values_free(vals, natts);
						return;
					}
				}
			}
		}
	}

	/* apply the row */
	switch (catid)
	{
		case PWE_CAT_PG_CLASS:
			apply_pg_class(cat, db, &tup, vals, txn, rec->lsn, false);
			break;
		case PWE_CAT_PG_ATTRIBUTE:
			apply_pg_attribute(cat, db, &tup, vals, txn, rec->lsn, false);
			break;
		case PWE_CAT_PG_NAMESPACE:
			apply_pg_namespace(cat, db, &tup, vals, txn, rec->lsn, false);
			break;
		case PWE_CAT_PG_TYPE:
			apply_pg_type(cat, db, &tup, vals, txn, rec->lsn, false);
			break;
		case PWE_CAT_PG_INDEX:
			apply_pg_index(cat, db, &tup, vals, txn, rec->lsn, false);
			break;
		case PWE_CAT_PG_CONSTRAINT:
			apply_pg_constraint(cat, db, &tup, vals, txn, rec->lsn, false);
			break;
		case PWE_CAT_PG_ENUM:
			apply_pg_enum(cat, db, &tup, vals, txn, rec->lsn, false);
			break;
		case PWE_CAT_PG_SEQUENCE:
			apply_pg_sequence(cat, db, &tup, vals, txn, rec->lsn, false);
			break;
		case PWE_CAT_PG_DATABASE:
			apply_pg_database(cat, &tup, vals, txn, rec->lsn, false);
			break;
		case PWE_CAT_PG_AUTHID:
			apply_pg_authid(cat, &tup, vals);
			break;
		case PWE_CAT_PG_ATTRDEF:
			apply_pg_attrdef(cat, db, &tup, vals);
			break;
		default:
			break;
	}

	/* track ctid -> snapshot for later deletes */
	{
		PweCatPayload *snap = pwe_calloc(1, sizeof(PweCatPayload));

		snapshot_from_row(cat, catid, db, &tup, vals, snap);
		if (old_blkno != UINT32_MAX)
		{
			PweCatPayload *oldp = untrack_row(cat, db, relfilenode,
											  old_blkno, old_offnum);

			if (oldp)
				payload_free(oldp);
		}
		track_row(cat, db, relfilenode, blkno, offnum, snap, payload, len);
	}
	pwe_values_free(vals, natts);
}

/* ------------------------------------------------------------------ */
/* abort handling                                                     */
/* ------------------------------------------------------------------ */
void
pwe_catalog_undo(PweCatalog *cat, PweCatPayload *p)
{
	if (!p)
		return;
	switch (p->cat)
	{
		case PWE_CAT_PG_CLASS:
			if (p->op == PWE_OP_INSERT)
			{
				PweRel	   *rel = detach_rel(cat, p->n.rel.db, p->n.rel.oid);

				free_rel(rel);
			}
			else if (p->op == PWE_OP_UPDATE)
				rel_from_snap(cat, &p->o.rel);
			else if (p->saved_rel)
			{
				PweRel	   *rel = p->saved_rel;

				rel_put_maps(cat, rel);
				p->saved_rel = NULL;
			}
			break;
		case PWE_CAT_PG_ATTRIBUTE:
			if (p->op == PWE_OP_INSERT)
				attr_remove(cat, p->n.attr.db, p->n.attr.relid, p->n.attr.num);
			else
				attr_apply_snap(cat, &p->o.attr);
			break;
		case PWE_CAT_PG_NAMESPACE:
			if (p->op == PWE_OP_INSERT)
			{
				char	   *old = pwe_hash_remove(&cat->namespaces,
												 relkey(p->n.ns.db, p->n.ns.oid));

				free(old);
			}
			else
				ns_apply_snap(cat, &p->n.ns);
			break;
		case PWE_CAT_PG_TYPE:
			if (p->op == PWE_OP_INSERT)
			{
				PweType    *old = pwe_hash_remove(&cat->types,
												  relkey(p->n.typ.db, p->n.typ.oid));

				if (old)
					free(old);
			}
			else
				type_apply_snap(cat, &p->n.typ);
			break;
		case PWE_CAT_PG_INDEX:
			if (p->op == PWE_OP_INSERT)
			{
				PweIndex   *old = pwe_hash_remove(&cat->indexes,
												  relkey(p->n.idx.db, p->n.idx.oid));

				if (old)
					free(old);
			}
			else
				index_apply_snap(cat, &p->n.idx);
			break;
		case PWE_CAT_PG_CONSTRAINT:
			if (p->op == PWE_OP_INSERT)
			{
				PweCon	   *old = pwe_hash_remove(&cat->constraints,
												 relkey(p->n.con.db, p->n.con.oid));

				if (old)
					free(old);
			}
			else
				con_apply_snap(cat, &p->n.con);
			break;
		case PWE_CAT_PG_ENUM:
			if (p->op == PWE_OP_INSERT)
			{
				PweEnumVal *old = pwe_hash_remove(&cat->enums,
												  relkey(p->n.en.db, p->n.en.oid));

				if (old)
					free(old);
			}
			else
				enum_apply_snap(cat, &p->n.en);
			break;
		case PWE_CAT_PG_SEQUENCE:
			if (p->op == PWE_OP_INSERT)
			{
				PweSeq	   *old = pwe_hash_remove(&cat->sequences,
												 relkey(p->n.seq.db, p->n.seq.relid));

				if (old)
					free(old);
			}
			else
				seq_apply_snap(cat, &p->n.seq);
			break;
		default:
			break;
	}
}

void
pwe_catalog_payload_cleanup(PweCatalog *cat, PweCatPayload *p)
{
	(void) cat;
	payload_free(p);
}
