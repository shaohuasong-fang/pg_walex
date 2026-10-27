/*
 * pg_walex - WAL record -> SQL reconstruction engine
 */
#include "pwe_internal.h"

#include <inttypes.h>
#include <time.h>

#define PWE_HEAP_OPMASK 0x70
#define PWE_XLOG_HEAP_INIT_PAGE 0x80

static bool copy_is_rewrite_heap(const char *name);
static bool index_dropped_with_parent(PweTxn *txn, PweRelSnap *snap);

/* per-table COPY output state (--tocopy) */
typedef struct PweCopyTable
{
	uint64		key;			/* (db<<32)|reloid */
	uint32		db;
	uint32		oid;
	char		base[160];		/* sanitized "schema.table" for file names */
	char	   *qname;			/* SQL-qualified name for \copy */
	FILE	   *f;				/* open data segment file */
	int			segno;			/* current segment (1 => .data.sql) */
	bool		seg_open;
	int			nfields;		/* field count of the open segment */
	char	   *collist;		/* "(c1, c2)" of the open segment */
	bool		meta_written;	/* <base>.meta.sql written */
} PweCopyTable;

typedef struct PweRelInit
{
	PweRel		rel;			/* scalar fields; attrs point at the copies */
	PweAttr   **attrs;
	int16	   *pk;
} PweRelInit;

struct PweReconstructor
{
	int			version;
	PweSqlOptions opts;
	FILE	   *out;
	PweStats   *stats;
	PweCatalog *cat;
	PweHash		txns;			/* xid -> PweTxn* */
	uint32		cur_db;
	PweHash		enum_created;	/* label+type already created via CREATE TYPE */
	PweHash		type_created;	/* type already emitted as CREATE TYPE */
	PweHash		schema_created;	/* schema already emitted/known */
	PweHash		unknown_rels;
	PweStr		tmp;
	PweStr		sql;
	bool		warned_no_prefix;
	PweHash		copy_tables;	/* relkey -> PweCopyTable* (--tocopy) */
	PweHash		seed_rels;		/* tables whose metadata was synthesized */
	PweHash		pre_rels;		/* relkey -> PweRelInit* from --pgdata */
	PweHash		pre_indexes;	/* indexes already present in --pgdata */
	PweHash		pre_cons;		/* constraints already present in --pgdata */
	PweHash		created_objs;	/* relations already emitted a CREATE for */
	PweHash		dropped_objs;	/* relations already emitted a DROP for */
	PweHash		obj_names;		/* qualified name hash -> owning OID */
	PweHash		file_bases;		/* output file base -> db/oid owner */
	PweHash		meta_attrs;		/* relkey -> PweRelInit used for meta */
	bool		baseline;		/* --pgdata supplied the window-end catalog */
};

static void copy_emit_insert(PweReconstructor *rc, PweTxn *txn, PweRel *rel,
							 PweRowVals *row, uint64 lsn);
static void copy_flush_segments(PweReconstructor *rc);
static bool copy_is_rewrite_heap(const char *name);
static void copy_ensure_connect(PweReconstructor *rc, uint32 db);
static void seed_schema_now(PweReconstructor *rc, uint32 db, uint32 nspoid);

/* ------------------------------------------------------------------ */
/* transaction helpers                                                */
/* ------------------------------------------------------------------ */
static void
free_event(PweReconstructor *rc, PweEvent *ev)
{
	if (!ev)
		return;
	free(ev->sql);
	free(ev->collist);
	if (ev->cat)
		pwe_catalog_payload_cleanup(rc->cat, ev->cat);
	free(ev);
}

static void
free_txn(PweReconstructor *rc, PweTxn *txn)
{
	PweEvent   *ev = txn->events;

	while (ev)
	{
		PweEvent   *n = ev->next;

		free_event(rc, ev);
		ev = n;
	}
	{
		PweUndo    *u = txn->undo;

		while (u)
		{
			PweUndo    *n = u->next;

			pwe_rowvals_free(u->oldrow);
			free(u);
			u = n;
		}
	}
	free(txn);
}

static PweTxn *
get_txn(PweReconstructor *rc, uint32 xid, uint64 lsn, bool create)
{
	PweTxn	   *txn;

	if (xid == 0)
		return NULL;
	txn = pwe_hash_get(&rc->txns, xid);
	if (!txn && create)
	{
		txn = pwe_calloc(1, sizeof(PweTxn));
		txn->xid = xid;
		txn->first_lsn = lsn;
		pwe_hash_put(&rc->txns, xid, txn);
	}
	return txn;
}

static void
txn_add_sql(PweReconstructor *rc, PweTxn *txn, uint64 lsn, PweStr *sql,
			PweRel *rel)
{
	PweEvent   *ev;

	if (!txn || sql->len == 0)
		return;
	ev = pwe_calloc(1, sizeof(PweEvent));
	ev->lsn = lsn;
	ev->xid = txn->xid;
	ev->kind = PWE_EV_SQL;
	ev->sql = pwe_strdup(pwe_str_cstr(sql));
	if (rel)
		ev->relkey = ((uint64) rel->db << 32) | rel->oid;
	if (txn->tail)
		txn->tail->next = ev;
	else
		txn->events = ev;
	txn->tail = ev;
	txn->bytes += sql->len + sizeof(PweEvent);
}

static void
txn_push_undo(PweReconstructor *rc, PweTxn *txn, PweRel *rel, uint32 blkno,
			  uint16 offnum, PweRowVals *oldrow)
{
	PweUndo    *u;

	if (!txn)
	{
		pwe_rowvals_free(oldrow);
		return;
	}
	u = pwe_calloc(1, sizeof(PweUndo));
	u->relkey = ((uint64) rel->db << 32) | rel->oid;
	u->ctid = ((uint64) blkno << 16) | offnum;
	u->oldrow = oldrow;
	u->next = txn->undo;
	txn->undo = u;
}

static void
txn_rollback_rows(PweReconstructor *rc, PweTxn *txn)
{
	PweUndo    *u = txn->undo;

	while (u)
	{
		PweUndo    *next = u->next;
		uint32		db = (uint32) (u->relkey >> 32);
		uint32		oid = (uint32) u->relkey;
		PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, db, oid);
		uint32		blkno = (uint32) (u->ctid >> 16);
		uint16		offnum = (uint16) (u->ctid & 0xFFFF);

		if (rel)
		{
			PweRowVals *cur = pwe_row_remove(rel, blkno, offnum);

			pwe_rowvals_free(cur);
			if (u->oldrow)
				pwe_row_set(rel, blkno, offnum, u->oldrow);
			u->oldrow = NULL;
		}
		pwe_rowvals_free(u->oldrow);
		free(u);
		u = next;
	}
	txn->undo = NULL;
}

/* ------------------------------------------------------------------ */
/* SQL helpers                                                        */
/* ------------------------------------------------------------------ */
static bool
rel_is_system_object(PweRel *rel)
{
	if (!rel)
		return true;
	if (rel->relkind == 't')
		return true;
	if (rel->nspname[0] &&
		(strcmp(rel->nspname, "pg_toast") == 0 ||
		 strcmp(rel->nspname, "pg_catalog") == 0 ||
		 strcmp(rel->nspname, "information_schema") == 0))
		return true;
	return false;
}

static bool
rel_is_user(PweRel *rel)
{
	return rel && rel->oid >= PWE_FIRST_NORMAL_OID &&
		(rel->relkind == 'r' || rel->relkind == 'p' || rel->relkind == 'm' ||
		 rel->relkind == 'f');
}

static void
qualify_rel(PweRel *rel, PweStr *out)
{
	if (rel->nspname[0])
	{
		pwe_str_add_ident(out, rel->nspname);
		pwe_str_addc(out, '.');
	}
	pwe_str_add_ident(out, rel->name);
}

static void
qualify_snap(PweReconstructor *rc, PweRelSnap *s, PweStr *out)
{
	const char *ns = pwe_catalog_ns_name(rc->cat, s->db, s->nsp);

	if (ns && ns[0])
	{
		pwe_str_add_ident(out, ns);
		pwe_str_addc(out, '.');
	}
	pwe_str_add_ident(out, s->name);
}

static void
add_col_name(PweRel *rel, int16 attnum, PweStr *out)
{
	if (attnum > 0 && attnum <= rel->natts && rel->attrs && rel->attrs[attnum])
		pwe_str_add_ident(out, rel->attrs[attnum]->name);
	else
		pwe_str_addf(out, "att%d", attnum);
}

static void
warn_unknown_relation(PweReconstructor *rc, PweRecord *rec, PweBlock *blk)
{
	uint64		key = ((uint64) blk->db << 32) | blk->rel;

	if (!rc->opts.include_comments)
		return;
	if (rc->opts.only_database && blk->db != rc->opts.only_database)
		return;
	if (blk->rel < PWE_FIRST_NORMAL_OID)
		return;
	if (pwe_hash_get(&rc->unknown_rels, key))
		return;
	pwe_hash_put(&rc->unknown_rels, key, (void *) 1);
	fprintf(rc->out,
			"-- pg_walex: change on unknown relation (spc=%u db=%u rel=%u) at %X/%X; "
			"catalog state is incomplete (WAL window does not cover the catalog)\n",
			blk->spc, blk->db, blk->rel,
			(uint32) (rec->lsn >> 32), (uint32) rec->lsn);
}

static bool
table_filter_match(PweReconstructor *rc, PweRel *rel)
{
	const char *filter = rc->opts.only_table;
	char		qualified[256];

	if (!filter || !filter[0])
		return true;
	if (rel->nspname[0])
		snprintf(qualified, sizeof(qualified), "%s.%s", rel->nspname, rel->name);
	else
		snprintf(qualified, sizeof(qualified), "%s", rel->name);
	if (strcmp(filter, qualified) == 0)
		return true;
	if (strcmp(filter, rel->name) == 0)
		return true;
	return false;
}

/* decode one user tuple into literal values indexed by attnum */
static PweRowVals *
decode_user_row(PweReconstructor *rc, PweRel *rel, uint32 db, PweTuple *tup)
{
	PweValue   *vals = NULL;
	PweRowVals *row;
	char		err[256];
	int			n;

	if (!rel->attrs || rel->natts <= 0)
		return NULL;
	n = tup->natts;
	if (n > rel->natts)
		n = rel->natts;
	if (n <= 0)
		return NULL;
	if (!pwe_tuple_decode_row(tup, rel->attrs, n, &vals, err, sizeof(err)))
		return NULL;
	row = pwe_rowvals_create(n);
	if (rc->opts.tocopy)
		row->copyvals = pwe_calloc((size_t) n + 1, sizeof(char *));
	{
		int			i;

		for (i = 1; i <= n; i++)
		{
			PweAttr    *a = rel->attrs[i];
			PweTypeInfo ti;
			PweStr		lit;

			if (!a || a->dropped)
				continue;
			pwe_str_init(&lit);
			if (!vals[i].isnull && vals[i].toast.valid && vals[i].data == NULL)
				pwe_catalog_detoast(rc->cat, db, &vals[i]);
			ti = pwe_catalog_type_info(rc->cat, db, a->typid);
			pwe_value_literal(rc->cat, db, &vals[i], &ti, &lit, rc->version);
			row->vals[i] = pwe_strdup(pwe_str_cstr(&lit));
			pwe_str_free(&lit);
			if (row->copyvals)
			{
				PweStr		raw;

				pwe_str_init(&raw);
				if (!vals[i].isnull &&
					pwe_value_copy(rc->cat, db, &vals[i], &ti, &raw))
					row->copyvals[i] = pwe_strdup(pwe_str_cstr(&raw));
				pwe_str_free(&raw);
			}
		}
	}
	pwe_values_free(vals, n);
	return row;
}

static int
add_insert_columns(PweReconstructor *rc, PweRel *rel, int natts, PweStr *out,
				   bool *has_always_identity)
{
	int			i;
	int			count = 0;
	bool		first = true;

	(void) rc;
	pwe_str_addc(out, '(');
	for (i = 1; i <= natts; i++)
	{
		PweAttr    *a;

		if (i > rel->natts || !rel->attrs)
			break;
		a = rel->attrs[i];
		if (!a || a->dropped || a->generated == 's')
			continue;
		if (!first)
			pwe_str_adds(out, ", ");
		pwe_str_add_ident(out, a->name);
		first = false;
		count++;
		if (a->identity == 'a')
			*has_always_identity = true;
	}
	pwe_str_addc(out, ')');
	return count;
}

static void
add_insert_values(PweRel *rel, PweRowVals *row, PweStr *out)
{
	int			i;
	bool		first = true;

	pwe_str_addc(out, '(');
	for (i = 1; i <= row->natts; i++)
	{
		PweAttr    *a;

		if (i > rel->natts || !rel->attrs)
			break;
		a = rel->attrs[i];
		if (!a || a->dropped || a->generated == 's')
			continue;
		if (!first)
			pwe_str_adds(out, ", ");
		pwe_str_adds(out, row->vals[i] ? row->vals[i] : "NULL");
		first = false;
	}
	pwe_str_addc(out, ')');
}

static void
add_predicate(PweReconstructor *rc, PweRel *rel, PweRowVals *row,
			  uint32 blkno, uint16 offnum, PweStr *out, bool *used_ctid)
{
	int			i;
	bool		first = true;
	bool		have_pk = rel->npk > 0;

	*used_ctid = false;
	pwe_str_adds(out, " WHERE ");
	if (row)
	{
		if (have_pk)
		{
			for (i = 0; i < rel->npk; i++)
			{
				int16		attnum = rel->pk[i];

				if (attnum <= 0 || attnum > row->natts)
				{
					have_pk = false;
					break;
				}
			}
		}
		if (have_pk)
		{
			for (i = 0; i < rel->npk; i++)
			{
				int16		attnum = rel->pk[i];

				if (!first)
					pwe_str_adds(out, " AND ");
				add_col_name(rel, attnum, out);
				pwe_str_adds(out, " IS NOT DISTINCT FROM ");
				pwe_str_adds(out, row->vals[attnum] ? row->vals[attnum] : "NULL");
				first = false;
			}
			return;
		}
		/* no primary key: compare all available columns */
		for (i = 1; i <= row->natts && i <= rel->natts; i++)
		{
			PweAttr    *a;

			if (!rel->attrs)
				break;
			a = rel->attrs[i];
			if (!a || a->dropped)
				continue;
			if (!row->vals[i])
				continue;
			if (!first)
				pwe_str_adds(out, " AND ");
			pwe_str_add_ident(out, a->name);
			pwe_str_adds(out, " IS NOT DISTINCT FROM ");
			pwe_str_adds(out, row->vals[i]);
			first = false;
		}
		if (!first)
			return;
	}
	pwe_str_addf(out, "ctid = '(%u,%u)'::tid", blkno, offnum);
	*used_ctid = true;
	(void) rc;
}

/* ------------------------------------------------------------------ */
/* DML generation                                                     */
/* ------------------------------------------------------------------ */
static void
row_attach_raw(PweRowVals *row, const uint8 *payload, uint32 len)
{
	if (!row || !payload || len == 0 || len > 16384)
		return;
	free(row->raw);
	row->raw = pwe_malloc(len);
	memcpy(row->raw, payload, len);
	row->rawlen = len;
}

static void
emit_insert(PweReconstructor *rc, PweTxn *txn, PweRel *rel, PweRowVals *row,
			uint64 lsn)
{
	PweStr	   *s = &rc->sql;
	bool		always_id = false;
	int			natts = row->natts;

	if (rc->opts.only_database && rel->db != rc->opts.only_database)
		return;
	if (rc->opts.only_database && rel->db != rc->opts.only_database)
		return;
	if (rc->opts.only_database && rel->db != rc->opts.only_database)
		return;
	if (!table_filter_match(rc, rel))
		return;
	/*
	 * Write-away INSERTs into an ALTER TABLE rewrite heap (pg_temp_NNN) can
	 * never be replayed: that relation is a transient internal heap that is
	 * dropped once the rewrite completes, and no CREATE TABLE is emitted.
	 */
	if (copy_is_rewrite_heap(rel->name))
		return;
	if (rc->opts.tocopy)
	{
		copy_emit_insert(rc, txn, rel, row, lsn);
		return;
	}
	pwe_str_reset(s);
	pwe_str_adds(s, "INSERT INTO ");
	qualify_rel(rel, s);
	pwe_str_addc(s, ' ');
	add_insert_columns(rc, rel, natts, s, &always_id);
	if (always_id)
		pwe_str_adds(s, " OVERRIDING SYSTEM VALUE");
	pwe_str_adds(s, " VALUES ");
	add_insert_values(rel, row, s);
	pwe_str_adds(s, ";\n");
	txn_add_sql(rc, txn, lsn, s, rel);
	if (rc->stats)
		rc->stats->ninsert++;
}

static void
emit_update(PweReconstructor *rc, PweTxn *txn, PweRel *rel, PweRowVals *newrow,
			PweRowVals *oldrow, uint32 blkno, uint16 offnum,
			uint32 new_blkno, uint16 new_offnum, uint64 lsn)
{
	PweStr	   *s = &rc->sql;
	int			i;
	bool		first = true;
	bool		used_ctid = false;

	if (!table_filter_match(rc, rel))
		return;
	pwe_str_reset(s);
	pwe_str_adds(s, "UPDATE ");
	qualify_rel(rel, s);
	pwe_str_adds(s, " SET ");
	for (i = 1; i <= newrow->natts && i <= rel->natts; i++)
	{
		PweAttr    *a;

		if (!rel->attrs)
			break;
		a = rel->attrs[i];
		if (!a || a->dropped || a->generated == 's')
			continue;
		if (oldrow && i <= oldrow->natts)
		{
			const char *nv = newrow->vals[i] ? newrow->vals[i] : "NULL";
			const char *ov = oldrow->vals[i] ? oldrow->vals[i] : "NULL";

			if (strcmp(nv, ov) == 0)
				continue;
		}
		if (!first)
			pwe_str_adds(s, ", ");
		pwe_str_add_ident(s, a->name);
		pwe_str_adds(s, " = ");
		pwe_str_adds(s, newrow->vals[i] ? newrow->vals[i] : "NULL");
		first = false;
	}
	if (first)
		return;					/* nothing changed */
	/*
	 * Prefer the old row for the WHERE clause.  When the old row is not in
	 * the cache (WAL starts mid-life), fall back to the primary key values of
	 * the new row rather than a physical ctid.
	 */
	if (!oldrow && rel->npk > 0)
		oldrow = newrow;
	add_predicate(rc, rel, oldrow, blkno, offnum, s, &used_ctid);
	pwe_str_adds(s, ";\n");
	(void) used_ctid;
	txn_add_sql(rc, txn, lsn, s, rel);
	if (rc->stats)
		rc->stats->nupdate++;
	/*
	 * --rollback: invert the UPDATE by writing the previous value of every
	 * changed column back.  The row is matched on its post-update values
	 * (new tuple's ctid), because the old tuple no longer exists.
	 */
	if (rc->opts.rollback && oldrow)
	{
		bool		rfirst = true;
		bool		rctid = false;

		pwe_str_reset(s);
		pwe_str_adds(s, "UPDATE ");
		qualify_rel(rel, s);
		pwe_str_adds(s, " SET ");
		for (i = 1; i <= newrow->natts && i <= rel->natts; i++)
		{
			PweAttr    *a;

			if (!rel->attrs)
				break;
			a = rel->attrs[i];
			if (!a || a->dropped || a->generated == 's')
				continue;
			if (i > oldrow->natts)
				continue;
			{
				const char *nv = newrow->vals[i] ? newrow->vals[i] : "NULL";
				const char *ov = oldrow->vals[i] ? oldrow->vals[i] : "NULL";

				if (strcmp(nv, ov) == 0)
					continue;
			}
			if (!rfirst)
				pwe_str_adds(s, ", ");
			pwe_str_add_ident(s, a->name);
			pwe_str_adds(s, " = ");
			pwe_str_adds(s, oldrow->vals[i] ? oldrow->vals[i] : "NULL");
			rfirst = false;
		}
		if (!rfirst)
		{
			add_predicate(rc, rel, newrow, new_blkno, new_offnum, s, &rctid);
			pwe_str_adds(s, ";\n");
			txn_add_sql(rc, txn, lsn, s, NULL);
			if (txn->tail)
				txn->tail->stmt = PWE_STMT_ROLLBACK;
		}
		(void) rctid;
	}
}

static void
emit_delete(PweReconstructor *rc, PweTxn *txn, PweRel *rel, PweRowVals *oldrow,
			uint32 blkno, uint16 offnum, uint64 lsn)
{
	PweStr	   *s = &rc->sql;
	bool		used_ctid = false;

	if (!table_filter_match(rc, rel))
		return;
	pwe_str_reset(s);
	pwe_str_adds(s, "DELETE FROM ");
	qualify_rel(rel, s);
	add_predicate(rc, rel, oldrow, blkno, offnum, s, &used_ctid);
	pwe_str_adds(s, ";\n");
	txn_add_sql(rc, txn, lsn, s, rel);
	if (rc->stats)
		rc->stats->ndelete++;
	/*
	 * --rollback: invert the DELETE by re-inserting the removed row with all
	 * of its values (only possible when the old tuple was still cached).
	 */
	if (rc->opts.rollback && oldrow)
	{
		bool		always_id = false;

		pwe_str_reset(s);
		pwe_str_adds(s, "INSERT INTO ");
		qualify_rel(rel, s);
		pwe_str_addc(s, ' ');
		add_insert_columns(rc, rel, oldrow->natts, s, &always_id);
		if (always_id)
			pwe_str_adds(s, " OVERRIDING SYSTEM VALUE");
		pwe_str_adds(s, " VALUES ");
		add_insert_values(rel, oldrow, s);
		pwe_str_adds(s, ";\n");
		txn_add_sql(rc, txn, lsn, s, NULL);
		if (txn->tail)
			txn->tail->stmt = PWE_STMT_ROLLBACK;
	}
	(void) used_ctid;
}

static void
emit_truncate(PweReconstructor *rc, PweTxn *txn, PweRecord *rec)
{
	const uint8 *p = rec->main_data;
	uint32		dbid;
	uint32		nrelids;
	uint8		flags;
	uint32		i;
	PweStr	   *s = &rc->sql;

	if (!p || rec->main_len < 12)
		return;
	dbid = pwe_read_u32(p);
	nrelids = pwe_read_u32(p + 4);
	flags = p[8];
	/* relids[] is 4-byte aligned, so the fixed part is 12 bytes */
	if (nrelids > 100000 || rec->main_len < 12 + nrelids * 4)
		return;
	if (rc->opts.only_database && dbid != rc->opts.only_database)
		return;
	pwe_str_reset(s);
	pwe_str_adds(s, "TRUNCATE TABLE ");
	for (i = 0; i < nrelids; i++)
	{
		uint32		relid = pwe_read_u32(p + 12 + i * 4);
		PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, dbid, relid);

		if (i > 0)
			pwe_str_adds(s, ", ");
		if (rel)
			qualify_rel(rel, s);
		else
			pwe_str_addf(s, "\"rel_%u\"", relid);
	}
	if (flags & 0x01)
		pwe_str_adds(s, " CASCADE");
	if (flags & 0x02)
		pwe_str_adds(s, " RESTART IDENTITY");
	pwe_str_adds(s, ";\n");
	if (!rc->opts.start_lsn || rec->lsn >= rc->opts.start_lsn)
	{
		txn_add_sql(rc, txn, rec->lsn, s, NULL);
		if (rc->stats)
			rc->stats->ntruncate++;

		/*
		 * --rollback: TRUNCATE does not write the removed rows to WAL, so the
		 * only source of the old data is the row cache.  Re-insert every row
		 * we saw for the truncated relations before it is dropped below.
		 */
		if (rc->opts.rollback)
		{
			PweStr		undo;
			bool		any = false;

			pwe_str_init(&undo);
			for (i = 0; i < nrelids; i++)
			{
				uint32		relid = pwe_read_u32(p + 12 + i * 4);
				PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, dbid, relid);
				size_t		b;

				if (!rel || !rel->rows || rel->rows->count == 0)
					continue;
				if (rel->persistence == 't' || copy_is_rewrite_heap(rel->name))
					continue;
				for (b = 0; b < rel->rows->nbuckets; b++)
				{
					PweHashEntry *e;

					for (e = rel->rows->buckets[b]; e; e = e->next)
					{
						PweRowVals *row = e->val;
						bool		always_id = false;

						pwe_str_adds(&undo, "INSERT INTO ");
						qualify_rel(rel, &undo);
						pwe_str_addc(&undo, ' ');
						add_insert_columns(rc, rel, row->natts, &undo,
										   &always_id);
						if (always_id)
							pwe_str_adds(&undo, " OVERRIDING SYSTEM VALUE");
						pwe_str_adds(&undo, " VALUES ");
						add_insert_values(rel, row, &undo);
						pwe_str_adds(&undo, ";\n");
						any = true;
					}
				}
			}
			if (any)
			{
				txn_add_sql(rc, txn, rec->lsn, &undo, NULL);
				if (txn->tail)
					txn->tail->stmt = PWE_STMT_ROLLBACK;
			}
			pwe_str_free(&undo);
		}

		/* the truncated relations have no rows left in the cache */
		for (i = 0; i < nrelids; i++)
		{
			uint32		relid = pwe_read_u32(p + 12 + i * 4);
			PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, dbid, relid);

			if (rel)
				pwe_rel_clear_rows(rel);
		}
	}
}

/* ------------------------------------------------------------------ */
/* heap record processing                                             */
/* ------------------------------------------------------------------ */
static bool
tuple_payload_ok(PweBlock *blk)
{
	return blk->in_use && blk->has_data && blk->data && blk->data_len >= 5;
}

static void
process_insert(PweReconstructor *rc, PweRecord *rec, PweTxn *txn)
{
	PweBlock   *blk = &rec->blocks[0];
	uint16		offnum;
	uint32		blkno;
	PweTuple	tup;
	char		err[256];
	PweRel	   *rel;

	if (!blk->in_use || rec->main_len < 2)
		return;
	offnum = pwe_read_u16(rec->main_data);
	blkno = blk->blkno;
	if (txn && txn->db == 0 && blk->db != 0)
		txn->db = blk->db;
	pwe_catalog_process_tuple(rc->cat, rec, txn, blk->db, blk->rel,
							  blkno, offnum, UINT32_MAX, 0,
							  blk->data, blk->data_len, false, false);
	rel = pwe_catalog_rel_by_file(rc->cat, blk->db, blk->rel);
	if (!rel)
	{
		if (pwe_cat_lookup_oid(blk->rel) < 0)
			warn_unknown_relation(rc, rec, blk);
		return;
	}
	if (!rel_is_user(rel))
		return;
	if (!tuple_payload_ok(blk) ||
		!pwe_tuple_parse(blk->data, blk->data_len, &tup, err, sizeof(err)))
		return;
	{
		PweRowVals *row = decode_user_row(rc, rel, blk->db, &tup);

		if (!row)
			return;
		row_attach_raw(row, blk->data, blk->data_len);
		txn_push_undo(rc, txn, rel, blkno, offnum, pwe_row_remove(rel, blkno, offnum));
		pwe_row_set(rel, blkno, offnum, row);
		if (!rc->opts.start_lsn || rec->lsn >= rc->opts.start_lsn)
			emit_insert(rc, txn, rel, row, rec->lsn);
	}
}

static void
process_multi_insert(PweReconstructor *rc, PweRecord *rec, PweTxn *txn)
{
	PweBlock   *blk = &rec->blocks[0];
	const uint8 *mdata = rec->main_data;
	uint16		ntuples;
	bool		isinit = (rec->info & PWE_XLOG_HEAP_INIT_PAGE) != 0;
	const uint8 *ptr;
	const uint8 *end;
	int			i;
	PweRel	   *rel;

	if (!blk->in_use || rec->main_len < 4 || !blk->data)
		return;
	/* struct xl_heap_multi_insert: uint8 flags; uint16 ntuples (2-byte
	 * aligned, one padding byte); OffsetNumber offsets[]; */
	ntuples = pwe_read_u16(mdata + 2);
	if (!isinit && rec->main_len < 4 + (uint32) ntuples * 2)
		return;
	if (txn && txn->db == 0 && blk->db != 0)
		txn->db = blk->db;
	rel = pwe_catalog_rel_by_file(rc->cat, blk->db, blk->rel);
	if (!rel && pwe_cat_lookup_oid(blk->rel) < 0)
		warn_unknown_relation(rc, rec, blk);
	ptr = blk->data;
	end = blk->data + blk->data_len;
	for (i = 0; i < ntuples; i++)
	{
		uint16		datalen;
		uint16		infomask2;
		uint16		infomask;
		uint8		hoff;
		uint16		offnum;
		uint8	   *payload;
		PweTuple	tup;
		char		err[256];

		ptr = (const uint8 *) (((uintptr_t) ptr + 1) & ~(uintptr_t) 1);
		if (ptr + 7 > end)
			return;
		datalen = pwe_read_u16(ptr);
		infomask2 = pwe_read_u16(ptr + 2);
		infomask = pwe_read_u16(ptr + 4);
		hoff = ptr[6];
		ptr += 7;
		if (ptr + datalen > end)
			return;
		offnum = isinit ? (uint16) (1 + i) : pwe_read_u16(mdata + 4 + i * 2);
		payload = pwe_malloc(5 + datalen);
		pwe_write_u16(payload, infomask2);
		pwe_write_u16(payload + 2, infomask);
		payload[4] = hoff;
		memcpy(payload + 5, ptr, datalen);
		pwe_catalog_process_tuple(rc->cat, rec, txn, blk->db, blk->rel,
								  blk->blkno, offnum, UINT32_MAX, 0,
								  payload, 5 + datalen, false, false);
		if (rel_is_user(rel) && pwe_tuple_parse(payload, 5 + datalen, &tup,
												err, sizeof(err)))
		{
			PweRowVals *row = decode_user_row(rc, rel, blk->db, &tup);

			if (row)
			{
				row_attach_raw(row, payload, 5 + datalen);
				txn_push_undo(rc, txn, rel, blk->blkno, offnum,
							  pwe_row_remove(rel, blk->blkno, offnum));
				pwe_row_set(rel, blk->blkno, offnum, row);
				if (!rc->opts.start_lsn || rec->lsn >= rc->opts.start_lsn)
					emit_insert(rc, txn, rel, row, rec->lsn);
			}
		}
		free(payload);
		ptr += datalen;
	}
}

static void
process_delete(PweReconstructor *rc, PweRecord *rec, PweTxn *txn)
{
	PweBlock   *blk = &rec->blocks[0];
	uint16		offnum;
	uint8		flags;
	PweRel	   *rel;

	if (!blk->in_use || rec->main_len < 8)
		return;
	offnum = pwe_read_u16(rec->main_data + 4);
	flags = rec->main_data[7];
	if (txn && txn->db == 0 && blk->db != 0)
		txn->db = blk->db;
	pwe_catalog_process_tuple(rc->cat, rec, txn, blk->db, blk->rel,
							  blk->blkno, offnum, UINT32_MAX, 0,
							  (flags & 0x06) ? blk->data : NULL,
							  (flags & 0x06) ? blk->data_len : 0, true, false);
	rel = pwe_catalog_rel_by_file(rc->cat, blk->db, blk->rel);
	if (!rel)
	{
		if (pwe_cat_lookup_oid(blk->rel) < 0)
			warn_unknown_relation(rc, rec, blk);
		return;
	}
	if (!rel_is_user(rel))
		return;
	{
		PweRowVals *old = pwe_row_remove(rel, blk->blkno, offnum);

		if (!rc->opts.start_lsn || rec->lsn >= rc->opts.start_lsn)
			emit_delete(rc, txn, rel, old, blk->blkno, offnum, rec->lsn);
		txn_push_undo(rc, txn, rel, blk->blkno, offnum, old);
	}
}

static void
process_update(PweReconstructor *rc, PweRecord *rec, PweTxn *txn, bool hot)
{
	PweBlock   *blk = &rec->blocks[0];
	const uint8 *mdata = rec->main_data;
	uint16		old_offnum;
	uint16		new_offnum;
	uint8		flags;
	uint32		old_blkno;
	const uint8 *data;
	uint32		datalen;
	uint32		skip = 0;
	PweRel	   *rel;
	PweTuple	newtup;
	char		err[256];
	PweRowVals *oldrow = NULL;

	(void) hot;
	if (!blk->in_use || rec->main_len < 14)
		return;
	old_offnum = pwe_read_u16(mdata + 4);
	flags = mdata[7];
	new_offnum = pwe_read_u16(mdata + 12);
	old_blkno = rec->blocks[1].in_use ? rec->blocks[1].blkno : blk->blkno;
	if (txn && txn->db == 0 && blk->db != 0)
		txn->db = blk->db;
	if (!blk->data)
		return;
	data = blk->data;
	datalen = blk->data_len;
	if (flags & 0x20)
		skip += 2;
	if (flags & 0x40)
		skip += 2;
	if (datalen < skip + 5)
		return;
	/*
	 * prefix/suffix compressed new tuples: reconstruct them from the old
	 * catalog tuple when we have one (catalogs are never logically decoded,
	 * so their updates may be compressed even with wal_level=logical).
	 */
	if (skip > 0)
	{
		uint8	   *rebuilt = NULL;
		uint32		rlen = 0;

		if (pwe_cat_lookup_oid(blk->rel) >= 0)
		{
			if (pwe_catalog_reconstruct_update(rc->cat, blk->db, blk->rel,
											   old_blkno, old_offnum,
											   data, datalen, flags,
											   &rebuilt, &rlen))
			{
				pwe_catalog_process_tuple(rc->cat, rec, txn, blk->db, blk->rel,
										  blk->blkno, new_offnum,
										  old_blkno, old_offnum,
										  rebuilt, rlen, false, false);
				free(rebuilt);
			}
			return;
		}

		rel = pwe_catalog_rel_by_file(rc->cat, blk->db, blk->rel);
		if (rel_is_user(rel))
		{
			PweRowVals *old = pwe_row_get(rel, old_blkno, old_offnum);

			if (old && old->raw &&
				pwe_reconstruct_update(old->raw, old->rawlen, data, datalen,
									   flags, &rebuilt, &rlen))
			{
				PweTuple	rt;
				PweRowVals *nrow;

				if (pwe_tuple_parse(rebuilt, rlen, &rt, err, sizeof(err)) &&
					(nrow = decode_user_row(rc, rel, blk->db, &rt)) != NULL)
				{
					PweRowVals *removed;

					row_attach_raw(nrow, rebuilt, rlen);
					emit_update(rc, txn, rel, nrow, old, old_blkno, old_offnum,
								blk->blkno, new_offnum, rec->lsn);
					removed = pwe_row_remove(rel, old_blkno, old_offnum);
					if (removed && removed != old)
						pwe_rowvals_free(removed);
					pwe_row_set(rel, blk->blkno, new_offnum, nrow);
					txn_push_undo(rc, txn, rel, blk->blkno, new_offnum, NULL);
					txn_push_undo(rc, txn, rel, old_blkno, old_offnum,
								  removed ? removed : old);
					free(rebuilt);
					return;
				}
				free(rebuilt);
			}
			if (rc->opts.include_comments)
				fprintf(rc->out,
						"-- pg_walex: cannot reconstruct prefix-compressed UPDATE at %X/%X (old tuple unavailable)\n",
						(uint32) (rec->lsn >> 32), (uint32) rec->lsn);
		}
		return;
	}
	pwe_catalog_process_tuple(rc->cat, rec, txn, blk->db, blk->rel,
							  blk->blkno, new_offnum, old_blkno, old_offnum,
							  data + skip, datalen - skip, false, false);
	rel = pwe_catalog_rel_by_file(rc->cat, blk->db, blk->rel);
	if (!rel)
	{
		if (pwe_cat_lookup_oid(blk->rel) < 0)
			warn_unknown_relation(rc, rec, blk);
		return;
	}
	if (!rel_is_user(rel))
		return;
	if (!pwe_tuple_parse(data + skip, datalen - skip, &newtup, err, sizeof(err)))
		return;
	oldrow = pwe_row_remove(rel, old_blkno, old_offnum);
	{
		PweRowVals *newrow = decode_user_row(rc, rel, blk->db, &newtup);

		if (!newrow)
		{
			/* restore old row */
			if (oldrow)
				pwe_row_set(rel, old_blkno, old_offnum, oldrow);
			return;
		}
		if (!rc->opts.start_lsn || rec->lsn >= rc->opts.start_lsn)
			emit_update(rc, txn, rel, newrow, oldrow, old_blkno, old_offnum,
						blk->blkno, new_offnum, rec->lsn);
		if (!(flags & 0x0C))
			row_attach_raw(newrow, data + skip, datalen - skip);
		pwe_row_set(rel, blk->blkno, new_offnum, newrow);
		txn_push_undo(rc, txn, rel, blk->blkno, new_offnum, NULL);
		txn_push_undo(rc, txn, rel, old_blkno, old_offnum, oldrow);
	}
}


/* ------------------------------------------------------------------ */
/* full page image harvesting                                         */
/* ------------------------------------------------------------------ */
static bool
fpi_decompress(PweReconstructor *rc, PweBlock *b, uint8 *page)
{
	uint32		hole_len = 0;
	uint32		img_len = b->image_len;
	const uint8 *img = b->image;
	bool		compressed = false;
	uint8		method = 0;

	if (!img || img_len == 0)
		return false;
	if (rc->version < 15)
		compressed = (b->bimg_info & 0x02) != 0;
	else
	{
		compressed = (b->bimg_info & (0x04 | 0x08 | 0x10)) != 0;
		if (b->bimg_info & 0x04)
			method = 0;
		else if (b->bimg_info & 0x08)
			method = 1;
		else if (b->bimg_info & 0x10)
			method = 2;
	}
	if (compressed)
		hole_len = b->hole_length;
	else
		hole_len = b->hole_offset ? (uint32) (rc->opts.max_row_cache ? 0 : 0) : 0;

	if (!compressed)
	{
		uint32		first = b->hole_offset ? b->hole_offset : img_len;
		uint32		second = 0;

		if (b->hole_length > 0 && b->hole_offset > 0)
			second = img_len - b->hole_offset;
		if (b->hole_length > 0 && b->hole_offset > 0)
		{
			if (b->hole_offset + b->hole_length + second > PWE_BLCKSZ_DEFAULT)
				return false;
			memcpy(page, img, b->hole_offset);
			memset(page + b->hole_offset, 0, b->hole_length);
			memcpy(page + b->hole_offset + b->hole_length,
				   img + b->hole_offset, second);
		}
		else
		{
			if (first > PWE_BLCKSZ_DEFAULT)
				return false;
			memcpy(page, img, first);
		}
		return true;
	}

	{
		uint32		outlen = PWE_BLCKSZ_DEFAULT - hole_len;
		uint8	   *tmp = pwe_malloc(outlen ? outlen : 1);
		bool		ok = false;

		if (method == 0)
			ok = pwe_pglz_decompress(img, img_len, tmp, outlen);
		else if (method == 1)
			ok = pwe_lz4_decompress(img, img_len, tmp, outlen);
		else if (method == 2)
			ok = pwe_zstd_decompress(img, img_len, tmp, outlen);
		if (!ok)
		{
			free(tmp);
			return false;
		}
		if (hole_len > 0)
		{
			if (b->hole_offset + hole_len > PWE_BLCKSZ_DEFAULT)
			{
				free(tmp);
				return false;
			}
			memcpy(page, tmp, b->hole_offset);
			memset(page + b->hole_offset, 0, hole_len);
			memcpy(page + b->hole_offset + hole_len,
				   tmp + b->hole_offset, outlen - b->hole_offset);
		}
		else
			memcpy(page, tmp, PWE_BLCKSZ_DEFAULT);
		free(tmp);
	}
	return true;
}

static void
harvest_fpi(PweReconstructor *rc, PweRecord *rec, PweTxn *txn, PweBlock *b)
{
	PweRel	   *rel;
	int			catid;
	uint8		page[PWE_BLCKSZ_DEFAULT];
	uint32		lower;
	uint32		upper;
	uint32		nitems;
	uint32		i;

	if (!b->has_image || !b->image || b->image_len == 0)
		return;
	catid = pwe_cat_lookup_oid(b->rel);
	rel = pwe_catalog_rel_by_file(rc->cat, b->db, b->rel);
	if (catid < 0 && !(rel && rel->relkind == 't'))
		return;
	if (!fpi_decompress(rc, b, page))
		return;
	if (pwe_read_u16(page) != 0)	/* sanity: page header magic/lsn */
	{
		/* page lsn is 8 bytes; the first two bytes are not a magic; just
		 * validate pd_lower/pd_upper below */
	}
	lower = pwe_read_u16(page + 12);
	upper = pwe_read_u16(page + 14);
	if (lower < 24 || lower > PWE_BLCKSZ_DEFAULT || upper > PWE_BLCKSZ_DEFAULT ||
		upper < lower)
		return;
	nitems = (lower - 24) / 4;
	for (i = 1; i <= nitems; i++)
	{
		uint32		v = pwe_read_u32(page + 24 + (i - 1) * 4);
		uint32		off = v & 0x7FFF;
		uint32		flags = (v >> 15) & 3;
		uint32		tlen = v >> 17;
		uint8	   *payload;
		uint16		infomask2;
		uint16		infomask;

		if (flags != 1)
			continue;
		if (tlen < 23 || off + tlen > PWE_BLCKSZ_DEFAULT)
			continue;
		infomask2 = pwe_read_u16(page + off + 18);
		infomask = pwe_read_u16(page + off + 20);
		if (((infomask2 & 0x07FF) == 0) || (page[off + 22] < 23))
			continue;
		payload = pwe_malloc(5 + tlen - 23);
		pwe_write_u16(payload, infomask2);
		pwe_write_u16(payload + 2, infomask);
		payload[4] = page[off + 22];
		memcpy(payload + 5, page + off + 23, tlen - 23);
		pwe_catalog_process_tuple(rc->cat, rec, txn, b->db, b->rel,
								  b->blkno, (uint16) i, UINT32_MAX, 0,
								  payload, 5 + tlen - 23, false, true);
		free(payload);
	}
}

static void
process_heap_record(PweReconstructor *rc, PweRecord *rec, PweTxn *txn)
{
	uint8		op = rec->info & PWE_HEAP_OPMASK;
	int			bi;

	for (bi = 0; bi < rec->nblocks; bi++)
	{
		PweBlock   *b = &rec->blocks[bi];

		if (b->in_use && b->has_image)
			harvest_fpi(rc, rec, txn, b);
	}

	if (rec->rmid == 10)		/* RM_HEAP_ID */
	{
		switch (op)
		{
			case 0x00:
				process_insert(rc, rec, txn);
				break;
			case 0x10:
				process_delete(rc, rec, txn);
				break;
			case 0x20:
			case 0x40:
				process_update(rc, rec, txn, op == 0x40);
				break;
			case 0x30:
				emit_truncate(rc, txn, rec);
				break;
			default:
				break;
		}
	}
	else						/* RM_HEAP2_ID */
	{
		if (op == 0x50)
			process_multi_insert(rc, rec, txn);
	}
}

/* ------------------------------------------------------------------ */
/* logical messages                                                   */
/* ------------------------------------------------------------------ */
static void
process_logical_message(PweReconstructor *rc, PweRecord *rec)
{
	const uint8 *p = rec->main_data;
	uint32		dbid;
	bool		transactional;
	uint32		prefix_size;
	uint32		message_size;

	if (!p || rec->main_len < 24)
		return;
	dbid = pwe_read_u32(p);
	transactional = p[4] != 0;
	prefix_size = (uint32) pwe_read_u64(p + 8);
	message_size = (uint32) pwe_read_u64(p + 16);
	if (24 + prefix_size + message_size > rec->main_len)
		return;
	if (!rc->opts.logical_messages && !rc->opts.include_non_sql)
		return;
	{
		const char *prefix = (const char *) p + 24;
		const char *msg = prefix + prefix_size;
		uint32		i;

		fprintf(rc->out, "-- pg_walex logical message (db=%u, transactional=%s, prefix=\"%.*s\"): ",
				dbid, transactional ? "true" : "false",
				(int) prefix_size, prefix);
		for (i = 0; i < message_size; i++)
		{
			unsigned char c = (unsigned char) msg[i];

			if (c == '\n')
				fputs("\n--   ", rc->out);
			else if (c >= 0x20 || c == '\t')
				fputc(c, rc->out);
			else
				fprintf(rc->out, "\\x%02x", c);
		}
		fputc('\n', rc->out);
	}
}

/* ------------------------------------------------------------------ */
/* DDL emission                                                       */
/* ------------------------------------------------------------------ */
typedef struct PweDdlCtx
{
	PweReconstructor *rc;
	PweTxn	   *txn;
	FILE	   *out;
	bool	   *begun;
	const char *hdr_type;		/* type= of the transaction comment */
	uint64		hdr_lsn;		/* lsn= of the transaction comment */
	uint64		cur_lsn;		/* LSN of the catalog event being emitted */
	PweHash		new_rels;		/* relkey -> 1 */
	PweHash		dropped_rels;
	PweHash		new_types;
} PweDdlCtx;

/* classify a reconstructed statement for the `type=` comment field */
static const char *
sql_type_name(const char *sql)
{
	if (!sql)
		return "sql";
	if (strncmp(sql, "INSERT INTO ", 12) == 0)
		return "insert";
	if (strncmp(sql, "UPDATE ", 7) == 0)
		return "update";
	if (strncmp(sql, "DELETE FROM ", 12) == 0)
		return "delete";
	if (strncmp(sql, "TRUNCATE", 8) == 0)
		return "truncate";
	if (strncmp(sql, "CREATE ", 7) == 0)
		return "create";
	if (strncmp(sql, "ALTER ", 6) == 0)
		return "alter";
	if (strncmp(sql, "DROP ", 5) == 0)
		return "drop";
	return "sql";
}

static const char *
event_type_name(PweEvent *ev)
{
	if (ev->stmt == PWE_STMT_ROLLBACK)
		return "rollback";
	if (ev->kind == PWE_EV_SQL && ev->sql)
		return sql_type_name(ev->sql);
	if (ev->kind == PWE_EV_COPYDATA)
		return "insert";
	return NULL;
}

/* commit timestamp (microseconds since 2000-01-01) -> "YYYY-MM-DD hh:mm:ss.us+00" */
static void
format_commit_ts(char *buf, size_t buflen, int64 us)
{
	int64		unix_us = us + INT64_C(946684800000000);
	int64		secs = unix_us / INT64_C(1000000);
	int64		frac = unix_us % INT64_C(1000000);
	time_t		t = (time_t) secs;
	struct tm	tm;

	if (frac < 0)
	{
		frac += INT64_C(1000000);
		t -= 1;
	}
	if (!gmtime_r(&t, &tm))
	{
		snprintf(buf, buflen, "unknown");
		return;
	}
	snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d.%06d+00",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			 tm.tm_hour, tm.tm_min, tm.tm_sec, (int) frac);
}

static void
ensure_begin(PweReconstructor *rc, PweTxn *txn, FILE *out, bool *begun,
			 const char *type, uint64 lsn)
{
	if (*begun)
		return;
	if (rc->opts.emit_connect && !rc->opts.only_database &&
		txn->db && txn->db != rc->cur_db)
	{
		const char *dbname = pwe_catalog_db_name(rc->cat, txn->db);

		if (dbname)
			fprintf(out, "\\connect %s\n", dbname);
		else
			fprintf(out, "-- database oid %u\n", txn->db);
		rc->cur_db = txn->db;
	}
	if (rc->opts.include_comments)
	{
		char		lsnbuf[64];
		char		tsbuf[64];

		pwe_format_lsn(lsnbuf, sizeof(lsnbuf), lsn ? lsn : txn->commit_lsn);
		if (txn->has_commit_time)
			format_commit_ts(tsbuf, sizeof(tsbuf), txn->commit_time);
		else
			snprintf(tsbuf, sizeof(tsbuf), "unknown");
		fprintf(out, "-- pg_walex: type=%s,xid=%u,lsn=%s,timestamp=%s\n",
				type ? type : "sql", txn->xid, lsnbuf, tsbuf);
	}
	fprintf(out, "BEGIN;\n");
	*begun = true;
}

static void
emit_ddl_stmt(PweDdlCtx *ctx, PweStr *sql, const char *kind)
{
	if (sql->len == 0)
		return;
	if (!ctx->hdr_type)
		ctx->hdr_type = sql_type_name(pwe_str_cstr(sql));
	ensure_begin(ctx->rc, ctx->txn, ctx->out, ctx->begun,
				 ctx->hdr_type, ctx->hdr_lsn);
	fwrite(sql->data, 1, sql->len, ctx->out);
	if (sql->data[sql->len - 1] != '\n')
		fputc('\n', ctx->out);
	if (ctx->rc->stats)
	{
		ctx->rc->stats->nddl++;
		if (kind)
			ctx->rc->stats->nsql++;
	}
}

static const char *
am_name(uint32 am)
{
	switch (am)
	{
		case 2:
			return "heap";
		case 403:
			return "btree";
		case 405:
			return "hash";
		case 783:
			return "gist";
		case 2742:
			return "gin";
		case 3580:
			return "brin";
		case 4000:
			return "spgist";
		default:
			return "btree";
	}
}

static void
add_type_with_mod(PweReconstructor *rc, uint32 db, uint32 typid, int32 typmod,
				  PweStr *out)
{
	PweTypeInfo ti = pwe_catalog_type_info(rc->cat, db, typid);
	const char *ns = pwe_catalog_ns_name(rc->cat, db, ti.nsp);

	if (ti.typtype == 'b' || ti.oid < PWE_FIRST_NORMAL_OID)
		ns = "pg_catalog";
	pwe_type_display_name(&ti, ns, out, ns && strcmp(ns, "pg_catalog") != 0);
	pwe_format_typmod(&ti, typmod, out);
}

static bool
find_pk(PweCatalog *cat, PweRel *rel, int16 *keys, int *nkeys)
{
	size_t		i;

	for (i = 0; i < cat->constraints.nbuckets; i++)
	{
		PweHashEntry *e;

		for (e = cat->constraints.buckets[i]; e; e = e->next)
		{
			PweCon	   *c = e->val;

			if (c->db == rel->db && c->relid == rel->oid && c->contype == 'p' &&
				c->nkeys > 0)
			{
				int			j;

				for (j = 0; j < c->nkeys && j < 32; j++)
					keys[j] = c->keys[j];
				*nkeys = c->nkeys;
				return true;
			}
		}
	}
	return false;
}

static PweCon *
find_constraint_by_index(PweCatalog *cat, uint32 db, uint32 indexoid)
{
	size_t		i;

	for (i = 0; i < cat->constraints.nbuckets; i++)
	{
		PweHashEntry *e;

		for (e = cat->constraints.buckets[i]; e; e = e->next)
		{
			PweCon	   *c = e->val;

			if (c->db == db && c->indid == indexoid)
				return c;
		}
	}
	return NULL;
}

/* session-temporary schemas can never (and need never) be re-created */
static bool
schema_is_temp(const char *name)
{
	return name && (strncmp(name, "pg_temp_", 8) == 0 ||
					strncmp(name, "pg_toast_temp_", 14) == 0);
}

/*
 * Bulk replay modes (--tocopy / --batch-inserts) need extra hardening: the
 * plain SQL output must stay identical to the pre-feature behaviour.
 */
static bool
replay_mode(PweReconstructor *rc)
{
	return rc->opts.tocopy || rc->opts.batch_rows > 0;
}

/* pg_attribute rows of system columns are reported when a table is dropped */
static bool
syscol_name(const char *name)
{
	static const char *const names[] = {
		"ctid", "xmin", "cmin", "xmax", "cmax", "tableoid", "oid"
	};
	size_t		i;

	if (!name)
		return false;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (strcmp(name, names[i]) == 0)
			return true;
	return false;
}

/*
 * Replay name bookkeeping.
 *
 * With --pgdata the catalog is the end-of-window snapshot, so pg_class deletes
 * for objects that are created again inside the window can show up in a later
 * transaction.  A naive replay then sees "relation already exists" at the new
 * CREATE and later drops the freshly created object.  Track which OID owns
 * each qualified name: a colliding CREATE first drops the stale object, and a
 * DROP that no longer owns the name is stale and skipped.
 */
static uint64
obj_name_key(PweReconstructor *rc, uint32 db, uint32 nsp, const char *name)
{
	const char *ns = pwe_catalog_ns_name(rc->cat, db, nsp);
	char		buf[PWE_NAMEDATALEN * 2 + 32];
	uint64		h = 1469598103934665603ULL;
	size_t		i;
	int			n;

	n = snprintf(buf, sizeof(buf), "%u/%s/%s", db, ns ? ns : "",
				 name ? name : "");
	if (n <= 0)
		return h;
	for (i = 0; i < (size_t) n && i < sizeof(buf); i++)
	{
		h ^= (uint64) (unsigned char) buf[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static uint64
str_hash(const char *s)
{
	uint64		h = 1469598103934665603ULL;

	if (!s)
		return h;
	while (*s)
	{
		h ^= (uint64) (unsigned char) *s++;
		h *= 1099511628211ULL;
	}
	return h;
}

static bool
obj_name_taken(PweReconstructor *rc, uint32 db, uint32 nsp, const char *name,
			   uint32 oid)
{
	void	   *v = pwe_hash_get(&rc->obj_names,
								 obj_name_key(rc, db, nsp, name));

	return v != NULL && (uint32) ((uintptr_t) v - 1) != oid;
}

static bool
obj_name_owned(PweReconstructor *rc, uint32 db, uint32 nsp, const char *name,
			   uint32 oid)
{
	void	   *v = pwe_hash_get(&rc->obj_names,
								 obj_name_key(rc, db, nsp, name));

	return v != NULL && (uint32) ((uintptr_t) v - 1) == oid;
}

static void
obj_name_claim(PweReconstructor *rc, uint32 db, uint32 nsp, const char *name,
			   uint32 oid)
{
	pwe_hash_put(&rc->obj_names, obj_name_key(rc, db, nsp, name),
				 (void *) (uintptr_t) (oid + 1));
}

static void
ensure_schema(PweDdlCtx *ctx, uint32 db, uint32 nspoid)
{
	const char *name;

	if (nspoid == 0 || nspoid == 11 || nspoid == 99 ||
		pwe_hash_get(&ctx->rc->schema_created, ((uint64) db << 32) | nspoid))
		return;
	name = pwe_catalog_ns_name(ctx->rc->cat, db, nspoid);
	pwe_hash_put(&ctx->rc->schema_created, ((uint64) db << 32) | nspoid, (void *) 1);
	if (!name || !name[0] || (replay_mode(ctx->rc) && schema_is_temp(name)))
		return;
	{
		PweStr		sql;

		pwe_str_init(&sql);
		pwe_str_adds(&sql, "CREATE SCHEMA IF NOT EXISTS ");
		pwe_str_add_ident(&sql, name);
		pwe_str_adds(&sql, ";\n");
		emit_ddl_stmt(ctx, &sql, "CREATE SCHEMA");
		pwe_str_free(&sql);
	}
}

/* ------------------------------------------------------------------ */
/* COPY output mode (--tocopy)                                        */
/*                                                                    */
/* INSERTs are accumulated in per-table data segment files holding    */
/* PostgreSQL COPY text format.  CREATE TABLE goes to                 */
/* <schema>.<table>.meta.sql; every other statement (DDL, UPDATE,     */
/* DELETE, TRUNCATE) is written to manifest.txt in commit order.      */
/* Whenever a non-INSERT statement is emitted, every open data        */
/* segment is closed first and its \copy line is appended to the      */
/* manifest, so the replay order stays faithful to the WAL.  A        */
/* schema change therefore starts a new data segment                  */
/* (.data2.sql, ...).                                                 */
/* ------------------------------------------------------------------ */

/* COPY text-format escaping: backslash, tab, newline, carriage return */
static void
copy_escape(PweStr *out, const char *s, size_t len)
{
	size_t		i;

	for (i = 0; i < len; i++)
	{
		char		c = s[i];

		switch (c)
		{
			case '\\':
				pwe_str_adds(out, "\\\\");
				break;
			case '\t':
				pwe_str_adds(out, "\\t");
				break;
			case '\n':
				pwe_str_adds(out, "\\n");
				break;
			case '\r':
				pwe_str_adds(out, "\\r");
				break;
			default:
				pwe_str_addc(out, c);
				break;
		}
	}
}

static void
copy_sanitize_name(const char *nsp, const char *name, char *out, size_t outlen)
{
	size_t		o = 0;
	const char *parts[2] = {nsp, name};
	int			p;

	for (p = 0; p < 2; p++)
	{
		const char *s = parts[p];

		if (!s || !s[0])
			continue;
		if (o > 0 && o + 1 < outlen)
			out[o++] = '.';
		for (; *s && o + 1 < outlen; s++)
		{
			char		c = *s;

			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '_' || c == '-')
				out[o++] = c;
			else
				out[o++] = '_';
		}
	}
	out[o] = '\0';
}

static void
copy_data_filename(PweCopyTable *ct, int segno, char *out, size_t outlen)
{
	if (segno <= 1)
		snprintf(out, outlen, "%s.data.sql", ct->base);
	else
		snprintf(out, outlen, "%s.data%d.sql", ct->base, segno);
}

static PweCopyTable *
copy_table_get(PweReconstructor *rc, PweRel *rel, bool create)
{
	uint64		key = ((uint64) rel->db << 32) | rel->oid;
	PweCopyTable *ct;

	ct = pwe_hash_get(&rc->copy_tables, key);
	if (!ct && create)
	{
		char		base[140];		/* NAMEDATALEN*2 + dot fits */

		ct = pwe_calloc(1, sizeof(PweCopyTable));
		ct->key = key;
		ct->db = rel->db;
		ct->oid = rel->oid;
		copy_sanitize_name(rel->nspname, rel->name, base, sizeof(base));
		if (!base[0])
			snprintf(base, sizeof(base), "rel_%u", rel->oid);
		/*
		 * A relation re-created under the same name inside the window - or
		 * a same-named relation of another database - must not share
		 * data/meta files: the files are written once and would otherwise
		 * overwrite each other.
		 */
		{
			uint64		self = ((uint64) rel->db << 32) | rel->oid;
			void	   *prev = pwe_hash_get(&rc->file_bases, str_hash(base));

			if (prev && (uint64) (uintptr_t) prev != self)
				snprintf(ct->base, sizeof(ct->base), "%s_%u", base,
						 rel->oid);
			else
				snprintf(ct->base, sizeof(ct->base), "%s", base);
			pwe_hash_put(&rc->file_bases, str_hash(base),
						 (void *) (uintptr_t) self);
		}
		pwe_hash_put(&rc->copy_tables, key, ct);
	}
	return ct;
}

/* switch the replay connection to db when the manifest crosses databases */
static void
copy_ensure_connect(PweReconstructor *rc, uint32 db)
{
	const char *dbname;

	if (!rc->opts.emit_connect || rc->opts.only_database || db == 0 ||
		db == rc->cur_db)
		return;
	dbname = pwe_catalog_db_name(rc->cat, db);
	if (!dbname)
		return;
	fprintf(rc->out, "\\connect %s\n", dbname);
	rc->cur_db = db;
}

/*
 * Emit CREATE SCHEMA IF NOT EXISTS for a schema that the catalog knows from
 * --pgdata but whose creation is not part of the WAL window.  Marked in
 * rc->schema_created so ensure_schema() never repeats it.
 */
static void
seed_schema_now(PweReconstructor *rc, uint32 db, uint32 nspoid)
{
	const char *name;
	uint64		key;
	PweStr		sql;

	if (nspoid == 0 || nspoid == 11 || nspoid == 99)
		return;
	key = ((uint64) db << 32) | nspoid;
	if (pwe_hash_get(&rc->schema_created, key))
		return;
	pwe_hash_put(&rc->schema_created, key, (void *) 1);
	name = pwe_catalog_ns_name(rc->cat, db, nspoid);
	if (!name || !name[0] || (replay_mode(rc) && schema_is_temp(name)))
		return;
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "CREATE SCHEMA IF NOT EXISTS ");
	pwe_str_add_ident(&sql, name);
	pwe_str_adds(&sql, ";\n");
	fputs(pwe_str_cstr(&sql), rc->out);
	pwe_str_free(&sql);
	if (rc->stats)
		rc->stats->nddl++;
}

/* close the open segment, append its \copy line to the manifest */
static void
copy_segment_close(PweReconstructor *rc, PweCopyTable *ct)
{
	char		fname[192];

	if (!ct->seg_open)
		return;
	fputs("\\.\n", ct->f);
	fclose(ct->f);
	ct->f = NULL;
	ct->seg_open = false;
	copy_data_filename(ct, ct->segno, fname, sizeof(fname));
	if (!ct->meta_written)
	{
		/* without a CREATE TABLE the \copy cannot run: keep the data
		 * file but leave a comment instead of a failing statement */
		fprintf(rc->out,
				"-- pg_walex: skipped COPY %s FROM '%s' (column metadata not found; data kept in the file)\n",
				ct->qname ? ct->qname : ct->base, fname);
		return;
	}
	if (rc->opts.emit_connect && !rc->opts.only_database &&
		ct->db && ct->db != rc->cur_db)
		copy_ensure_connect(rc, ct->db);
	fprintf(rc->out, "\\copy %s %s FROM '%s';\n",
			ct->qname ? ct->qname : ct->base,
			ct->collist ? ct->collist : "", fname);
}

/* open the next data segment for the table */
static bool
copy_segment_open(PweReconstructor *rc, PweCopyTable *ct, PweRel *rel,
				  const char *collist, int nfields)
{
	char		path[4096];
	char		fname[192];
	PweStr		qn;

	ct->segno++;
	copy_data_filename(ct, ct->segno, fname, sizeof(fname));
	snprintf(path, sizeof(path), "%s/%s", rc->opts.output_dir, fname);
	ct->f = fopen(path, "w");
	if (!ct->f)
	{
		fprintf(stderr, "pg_walex: cannot create %s\n", path);
		return false;
	}
	free(ct->collist);
	ct->collist = pwe_strdup(collist);
	ct->nfields = nfields;
	pwe_str_init(&qn);
	qualify_rel(rel, &qn);
	free(ct->qname);
	ct->qname = pwe_strdup(pwe_str_cstr(&qn));
	pwe_str_free(&qn);
	ct->seg_open = true;
	return true;
}

static void
copy_flush_cb(uint64 key, void *val, void *arg)
{
	(void) key;
	copy_segment_close((PweReconstructor *) arg, (PweCopyTable *) val);
}

/* close every open segment, appending \copy lines to the manifest */
static void
copy_flush_segments(PweReconstructor *rc)
{
	if (!rc->opts.tocopy)
		return;
	pwe_hash_iterate(&rc->copy_tables, copy_flush_cb, rc);
}

/* render CREATE TABLE; returns false when no column metadata is known yet */
static bool
copy_render_meta(PweReconstructor *rc, PweRel *rel, PweStr *sql)
{
	int			i;
	int16		keys[32];
	int			nkeys = 0;
	bool		first = true;
	int			ncols = 0;

	pwe_str_adds(sql, "CREATE ");
	if (rel->persistence == 'u')
		pwe_str_adds(sql, "UNLOGGED ");
	pwe_str_adds(sql, "TABLE IF NOT EXISTS ");
	qualify_rel(rel, sql);
	pwe_str_adds(sql, " (\n");
	for (i = 1; i <= rel->natts; i++)
	{
		PweAttr    *a = rel->attrs ? rel->attrs[i] : NULL;

		if (!a || a->dropped)
			continue;
		if (!first)
			pwe_str_adds(sql, ",\n");
		pwe_str_adds(sql, "  ");
		pwe_str_add_ident(sql, a->name);
		pwe_str_addc(sql, ' ');
		add_type_with_mod(rc, rel->db, a->typid, a->typmod, sql);
		if (a->notnull)
			pwe_str_adds(sql, " NOT NULL");
		first = false;
		ncols++;
	}
	if (ncols == 0)
	{
		pwe_str_reset(sql);
		return false;
	}
	if (find_pk(rc->cat, rel, keys, &nkeys))
	{
		int			j;

		if (!first)
			pwe_str_adds(sql, ",\n");
		pwe_str_adds(sql, "  PRIMARY KEY (");
		for (j = 0; j < nkeys; j++)
		{
			if (j > 0)
				pwe_str_adds(sql, ", ");
			add_col_name(rel, keys[j], sql);
		}
		pwe_str_addc(sql, ')');
	}
	pwe_str_adds(sql, "\n);\n");
	return true;
}

/*
 * Metadata source for a synthesized meta file.  Relations inherited from
 * --pgdata are rendered from their snapshot at the start of the WAL window;
 * their live catalog entry may already include ALTERs logged later that the
 * replay will apply again.
 */
static PweRel *
meta_source_rel(PweReconstructor *rc, PweRel *rel)
{
	PweRelInit *ri;

	if (!rel)
		return NULL;
	ri = pwe_hash_get(&rc->pre_rels, ((uint64) rel->db << 32) | rel->oid);
	return ri ? &ri->rel : rel;
}

/* deep copy of the column metadata of a relation */
static PweRelInit *
rel_snapshot(PweRel *rel)
{
	PweRelInit *ri = pwe_calloc(1, sizeof(PweRelInit));
	int			j;

	ri->rel = *rel;
	ri->rel.rows = NULL;
	ri->rel.natts = rel->natts;
	ri->rel.npk = rel->npk;
	ri->attrs = pwe_calloc((size_t) rel->natts + 1, sizeof(PweAttr *));
	for (j = 0; j <= rel->natts; j++)
	{
		if (rel->attrs && rel->attrs[j])
		{
			ri->attrs[j] = pwe_malloc(sizeof(PweAttr));
			*ri->attrs[j] = *rel->attrs[j];
		}
	}
	ri->rel.attrs = ri->attrs;
	if (rel->npk > 0 && rel->pk)
	{
		ri->pk = pwe_malloc(sizeof(int16) * (size_t) rel->npk);
		memcpy(ri->pk, rel->pk, sizeof(int16) * (size_t) rel->npk);
		ri->rel.pk = ri->pk;
	}
	else
		ri->rel.pk = NULL;
	return ri;
}

/* does the relation already carry this column? */
static bool
rel_has_attr(PweRel *rel, const char *name)
{
	int			i;

	if (!rel || !name)
		return false;
	for (i = 0; i < rel->natts; i++)
		if (rel->attrs[i] && strcmp(rel->attrs[i]->name, name) == 0)
			return true;
	return false;
}

/*
 * Does the metadata that was used to CREATE the table already contain the
 * column?  Seeded relations use the --pgdata snapshot; relations created
 * inside the window use the catalog state captured when their metadata was
 * written (the live catalog at ALTER time already includes the added column).
 */
static bool
meta_has_attr(PweReconstructor *rc, PweRel *rel, const char *name)
{
	uint64		key;
	PweRelInit *ri;
	int			i;

	if (!rel || !name)
		return false;
	key = ((uint64) rel->db << 32) | rel->oid;
	ri = pwe_hash_get(&rc->pre_rels, key);
	if (!ri)
		ri = pwe_hash_get(&rc->meta_attrs, key);
	if (!ri)
		return rel_has_attr(rel, name);
	for (i = 1; i <= ri->rel.natts; i++)
	{
		if (ri->attrs[i] && !ri->attrs[i]->dropped &&
			strcmp(ri->attrs[i]->name, name) == 0)
			return true;
	}
	return false;
}

/* write <base>.meta.sql and reference it from the manifest */
static bool
copy_write_meta(PweReconstructor *rc, PweCopyTable *ct, PweRel *rel)
{
	char		path[4096];
	char		fname[192];
	FILE	   *mf;
	PweStr		sql;

	if (ct->meta_written)
		return true;
	pwe_str_init(&sql);
	if (!copy_render_meta(rc, meta_source_rel(rc, rel), &sql))
	{
		/* column metadata is not known yet: retry when data arrives */
		pwe_str_free(&sql);
		return false;
	}
	if (!pwe_hash_get(&rc->pre_rels, ((uint64) rel->db << 32) | rel->oid) &&
		!pwe_hash_get(&rc->meta_attrs, ((uint64) rel->db << 32) | rel->oid))
		pwe_hash_put(&rc->meta_attrs, ((uint64) rel->db << 32) | rel->oid,
					 rel_snapshot(meta_source_rel(rc, rel)));
	snprintf(fname, sizeof(fname), "%s.meta.sql", ct->base);
	snprintf(path, sizeof(path), "%s/%s", rc->opts.output_dir, fname);
	mf = fopen(path, "w");
	if (!mf)
	{
		fprintf(stderr, "pg_walex: cannot create %s\n", path);
		pwe_str_free(&sql);
		return false;
	}
	fwrite(sql.data, 1, sql.len, mf);
	fclose(mf);
	pwe_str_free(&sql);
	/*
	 * The manifest must reach the table's database and schema before the
	 * meta file is sourced: for tables whose CREATE TABLE is not part of the
	 * WAL window this is the first statement naming them.
	 */
	copy_ensure_connect(rc, ct->db);
	seed_schema_now(rc, rel->db, rel->nsp);
	/* the same name is still owned by an older object of this replay */
	if (obj_name_taken(rc, rel->db, rel->nsp, rel->name, rel->oid))
	{
		PweStr		drop;

		pwe_str_init(&drop);
		pwe_str_adds(&drop, "DROP TABLE IF EXISTS ");
		qualify_rel(rel, &drop);
		pwe_str_adds(&drop, " CASCADE;\n");
		fputs(pwe_str_cstr(&drop), rc->out);
		pwe_str_free(&drop);
	}
	fprintf(rc->out, "\\i '%s'\n", fname);
	obj_name_claim(rc, rel->db, rel->nsp, rel->name, rel->oid);
	ct->meta_written = true;
	/* this meta file was synthesized from the catalog / pgdata snapshot */
	if (pwe_hash_get(&rc->pre_rels, ((uint64) rel->db << 32) | rel->oid))
		pwe_hash_put(&rc->seed_rels, ((uint64) rel->db << 32) | rel->oid,
					 (void *) 1);
	else
		/* the table's CREATE TABLE is now accounted for */
		pwe_hash_put(&rc->created_objs, ((uint64) rel->db << 32) | rel->oid,
					 (void *) 1);
	return true;
}

/*
 * Make sure the replay target has a CREATE TABLE for "rel" before a
 * statement references it.  Tables created inside the WAL window are emitted
 * from their pg_class events; tables seeded from --pgdata (or created before
 * the WAL window) have no CREATE event, so their metadata is synthesized
 * here on first use.  Used by --tocopy and --batch-inserts only.
 */
static void
seed_table_meta(PweDdlCtx *ctx, PweRel *rel)
{
	PweReconstructor *rc = ctx->rc;
	uint64		key;
	PweStr		sql;

	if (!rel || rel_is_system_object(rel))
		return;
	if (rel->persistence == 't')
		return;
	if (rel->relkind != 'r' && rel->relkind != 'p')
		return;
	if (copy_is_rewrite_heap(rel->name))
		return;
	key = ((uint64) rel->db << 32) | rel->oid;
	/*
	 * Only relations inherited from --pgdata need synthesized metadata; a
	 * relation created inside the WAL window already has its CREATE TABLE in
	 * the output (from its pg_class event), and seeding it again would render
	 * later ALTER TABLE statements ineffective (column already exists).
	 */
	if (!pwe_hash_get(&rc->pre_rels, key))
		return;
	if (rc->opts.tocopy)
	{
		PweCopyTable *ct = copy_table_get(rc, rel, true);

		if (ct->meta_written)
			return;
		if (!ctx->hdr_type)
			ctx->hdr_type = "create";
		ensure_begin(rc, ctx->txn, ctx->out, ctx->begun,
					 ctx->hdr_type, ctx->hdr_lsn);
		copy_write_meta(rc, ct, rel);
		return;
	}
	if (pwe_hash_get(&rc->seed_rels, key))
		return;
	pwe_str_init(&sql);
	if (!copy_render_meta(rc, meta_source_rel(rc, rel), &sql))
	{
		/* column metadata is not known yet */
		pwe_str_free(&sql);
		return;
	}
	if (!ctx->hdr_type)
		ctx->hdr_type = "create";
	ensure_begin(rc, ctx->txn, ctx->out, ctx->begun,
				 ctx->hdr_type, ctx->hdr_lsn);
	seed_schema_now(rc, rel->db, rel->nsp);
	obj_name_claim(rc, rel->db, rel->nsp, rel->name, rel->oid);
	pwe_hash_put(&rc->seed_rels, key, (void *) 1);
	if (rc->opts.include_comments)
		fprintf(rc->out, "-- pg_walex: table metadata synthesized from catalog\n");
	fputs(pwe_str_cstr(&sql), rc->out);
	pwe_str_free(&sql);
	if (rc->stats)
		rc->stats->nddl++;
}

/* queue one INSERT as an escaped COPY data line (emitted at commit) */
static void
copy_emit_insert(PweReconstructor *rc, PweTxn *txn, PweRel *rel,
				 PweRowVals *row, uint64 lsn)
{
	PweStr		line;
	PweStr		cols;
	PweEvent   *ev;
	int			i;
	int			nf = 0;
	bool		first = true;

	if (!row->copyvals)
		return;
	pwe_str_init(&line);
	pwe_str_init(&cols);
	pwe_str_addc(&cols, '(');
	for (i = 1; i <= row->natts; i++)
	{
		PweAttr    *a;

		if (i > rel->natts || !rel->attrs)
			break;
		a = rel->attrs[i];
		if (!a || a->dropped || a->generated == 's')
			continue;
		if (!first)
		{
			pwe_str_addc(&line, '\t');
			pwe_str_adds(&cols, ", ");
		}
		pwe_str_add_ident(&cols, a->name);
		if (row->copyvals[i])
			copy_escape(&line, row->copyvals[i], strlen(row->copyvals[i]));
		else
			pwe_str_adds(&line, "\\N");
		first = false;
		nf++;
	}
	pwe_str_addc(&cols, ')');
	if (nf == 0)
	{
		pwe_str_free(&line);
		pwe_str_free(&cols);
		return;
	}
	pwe_str_addc(&line, '\n');
	ev = pwe_calloc(1, sizeof(PweEvent));
	ev->lsn = lsn;
	ev->xid = txn ? txn->xid : 0;
	ev->kind = PWE_EV_COPYDATA;
	ev->relkey = ((uint64) rel->db << 32) | rel->oid;
	ev->nfields = (uint16) nf;
	ev->collist = pwe_strdup(pwe_str_cstr(&cols));
	ev->sql = pwe_strdup(pwe_str_cstr(&line));
	pwe_str_free(&line);
	pwe_str_free(&cols);
	if (txn)
	{
		if (txn->tail)
			txn->tail->next = ev;
		else
			txn->events = ev;
		txn->tail = ev;
		txn->bytes += strlen(ev->sql) + strlen(ev->collist) + sizeof(PweEvent);
	}
	else
		free_event(rc, ev);		/* no transaction context: drop */
}

/* internal heap created by ALTER TABLE rewrites (make_new_heap) */
static bool
copy_is_rewrite_heap(const char *name)
{
	const char *p;

	if (strncmp(name, "pg_temp_", 8) != 0)
		return false;
	for (p = name + 8; *p; p++)
		if (*p < '0' || *p > '9')
			return false;
	return p > name + 8;
}

/* write one committed COPY data line to the table's segment file */
static void
copy_write_row(PweReconstructor *rc, PweEvent *ev)
{
	uint32		db = (uint32) (ev->relkey >> 32);
	uint32		oid = (uint32) ev->relkey;
	PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, db, oid);
	PweCopyTable *ct;

	if (!rel)
		return;
	if (rel->persistence == 't')
		return;					/* temporary tables are not replayed */
	ct = copy_table_get(rc, rel, true);
	if (!ct->meta_written && !copy_is_rewrite_heap(rel->name))
		copy_write_meta(rc, ct, rel);
	if (ct->seg_open && strcmp(ct->collist, ev->collist) != 0)
		copy_segment_close(rc, ct);
	if (!ct->seg_open &&
		!copy_segment_open(rc, ct, rel, ev->collist, (int) ev->nfields))
		return;
	fputs(ev->sql, ct->f);
	if (rc->stats)
		rc->stats->ninsert++;
}

/* CREATE TABLE in COPY mode: statement goes to the meta file */
static void
copy_handle_create_table(PweDdlCtx *ctx, PweRelSnap *snap)
{
	PweReconstructor *rc = ctx->rc;
	PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, snap->db, snap->oid);
	PweCopyTable *ct;
	PweStr		line;

	if (!rel || rel_is_system_object(rel))
		return;
	if (rel->persistence == 't')
		return;
	/* rewrite heaps are never replayed; their rows are kept as data only */
	if (copy_is_rewrite_heap(rel->name))
		return;
	ensure_schema(ctx, snap->db, rel->nsp);
	ct = copy_table_get(rc, rel, true);
	if (!copy_write_meta(rc, ct, rel))
	{
		/* no column metadata yet: defer, the CREATE TABLE is emitted
		 * with the first data row once the catalog knows the columns */
		return;
	}
	pwe_str_init(&line);
	pwe_str_addf(&line, "-- pg_walex: CREATE TABLE %s (see %s.meta.sql)\n",
				 ct->base, ct->base);
	if (!ctx->hdr_type)
		ctx->hdr_type = "create";
	emit_ddl_stmt(ctx, &line, "CREATE TABLE");
	pwe_str_free(&line);
}

static void
emit_create_table(PweDdlCtx *ctx, PweRelSnap *snap)
{
	PweReconstructor *rc = ctx->rc;
	PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, snap->db, snap->oid);
	PweStr		sql;
	int			i;
	int16		keys[32];
	int			nkeys = 0;

	if (rc->opts.tocopy)
	{
		copy_handle_create_table(ctx, snap);
		return;
	}
	if (!rel || rel_is_system_object(rel))
		return;
	if (rel->persistence == 't')
		return;					/* temporary tables are skipped */
	/* transient heap of ALTER TABLE ... TYPE; never replayed */
	if (copy_is_rewrite_heap(snap->name))
		return;
	if (replay_mode(rc) &&
		pwe_hash_get(&rc->created_objs, ((uint64) snap->db << 32) | snap->oid))
		return;
	ensure_schema(ctx, snap->db, rel->nsp);
	if (replay_mode(rc) &&
		obj_name_taken(rc, snap->db, rel->nsp, rel->name, snap->oid))
	{
		PweStr		drop;

		pwe_str_init(&drop);
		pwe_str_adds(&drop, "DROP TABLE IF EXISTS ");
		qualify_rel(rel, &drop);
		pwe_str_adds(&drop, " CASCADE;\n");
		emit_ddl_stmt(ctx, &drop, "DROP TABLE");
		pwe_str_free(&drop);
	}
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "CREATE ");
	if (rel->persistence == 'u')
		pwe_str_adds(&sql, "UNLOGGED ");
	pwe_str_adds(&sql, "TABLE ");
	qualify_rel(rel, &sql);
	{
		int			live = 0;

		for (i = 1; i <= rel->natts; i++)
		{
			PweAttr    *a = rel->attrs ? rel->attrs[i] : NULL;

			if (a && !a->dropped)
				live++;
		}
		if (live == 0)
		{
			if (rc->opts.include_comments)
				fprintf(rc->out,
						"-- pg_walex: skipped CREATE TABLE %s (column metadata not found in WAL; use --pgdata to seed the catalog)\n",
						rel->name);
			pwe_str_free(&sql);
			return;
		}
	}
	pwe_str_adds(&sql, " (\n");
	{
		bool		first = true;

		for (i = 1; i <= rel->natts; i++)
		{
			PweAttr    *a = rel->attrs ? rel->attrs[i] : NULL;

			if (!a || a->dropped)
				continue;
			if (!first)
				pwe_str_adds(&sql, ",\n");
			pwe_str_adds(&sql, "  ");
			pwe_str_add_ident(&sql, a->name);
			pwe_str_addc(&sql, ' ');
			add_type_with_mod(rc, snap->db, a->typid, a->typmod, &sql);
			if (a->notnull)
				pwe_str_adds(&sql, " NOT NULL");
			first = false;
		}
		if (find_pk(rc->cat, rel, keys, &nkeys))
		{
			int			j;

			if (!first)
				pwe_str_adds(&sql, ",\n");
			pwe_str_adds(&sql, "  PRIMARY KEY (");
			for (j = 0; j < nkeys; j++)
			{
				if (j > 0)
					pwe_str_adds(&sql, ", ");
				add_col_name(rel, keys[j], &sql);
			}
			pwe_str_addc(&sql, ')');
			first = false;
		}
	}
	pwe_str_adds(&sql, "\n);\n");
	if (replay_mode(rc))
	{
		pwe_hash_put(&rc->created_objs, ((uint64) snap->db << 32) | snap->oid,
					 (void *) 1);
		if (!pwe_hash_get(&rc->pre_rels, ((uint64) snap->db << 32) | snap->oid) &&
			!pwe_hash_get(&rc->meta_attrs, ((uint64) snap->db << 32) | snap->oid))
			pwe_hash_put(&rc->meta_attrs, ((uint64) snap->db << 32) | snap->oid,
						 rel_snapshot(rel));
		obj_name_claim(rc, snap->db, rel->nsp, rel->name, snap->oid);
	}
	emit_ddl_stmt(ctx, &sql, "CREATE TABLE");
	pwe_str_free(&sql);
}

static void
emit_create_sequence(PweDdlCtx *ctx, PweRelSnap *snap)
{
	PweReconstructor *rc = ctx->rc;
	PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, snap->db, snap->oid);
	PweSeq	   *seq = pwe_catalog_sequence(rc->cat, snap->db, snap->oid);
	PweStr		sql;
	PweTypeInfo ti;

	if (!rel || rel_is_system_object(rel) || rel->persistence == 't')
		return;
	if (replay_mode(rc) &&
		pwe_hash_get(&rc->created_objs, ((uint64) snap->db << 32) | snap->oid))
		return;
	ensure_schema(ctx, snap->db, rel->nsp);
	if ((rc->opts.tocopy || rc->opts.batch_rows > 0) &&
		obj_name_taken(rc, snap->db, rel->nsp, rel->name, snap->oid))
	{
		PweStr		drop;

		pwe_str_init(&drop);
		pwe_str_adds(&drop, "DROP SEQUENCE IF EXISTS ");
		qualify_rel(rel, &drop);
		pwe_str_adds(&drop, ";\n");
		emit_ddl_stmt(ctx, &drop, "DROP SEQUENCE");
		pwe_str_free(&drop);
	}
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "CREATE SEQUENCE ");
	qualify_rel(rel, &sql);
	if (seq)
	{
		ti = pwe_catalog_type_info(rc->cat, snap->db, seq->typid);
		if (ti.found)
		{
			pwe_str_adds(&sql, " AS ");
			pwe_type_display_name(&ti, NULL, &sql, false);
		}
		if (seq->inc)
			pwe_str_addf(&sql, " INCREMENT BY %lld", (long long) seq->inc);
		if (seq->minv)
			pwe_str_addf(&sql, " MINVALUE %lld", (long long) seq->minv);
		if (seq->maxv)
			pwe_str_addf(&sql, " MAXVALUE %lld", (long long) seq->maxv);
		pwe_str_addf(&sql, " START WITH %lld", (long long) seq->start);
		if (seq->cache)
			pwe_str_addf(&sql, " CACHE %lld", (long long) seq->cache);
		if (seq->cycle)
			pwe_str_adds(&sql, " CYCLE");
	}
	pwe_str_adds(&sql, ";\n");
	if (replay_mode(rc))
	{
		pwe_hash_put(&rc->created_objs, ((uint64) snap->db << 32) | snap->oid,
					 (void *) 1);
		obj_name_claim(rc, snap->db, rel->nsp, rel->name, snap->oid);
	}
	emit_ddl_stmt(ctx, &sql, "CREATE SEQUENCE");
	pwe_str_free(&sql);
}

static void
emit_create_index(PweDdlCtx *ctx, PweIndexSnap *isnap)
{
	PweReconstructor *rc = ctx->rc;
	PweRel	   *irel = pwe_catalog_rel_by_oid(rc->cat, isnap->db, isnap->oid);
	PweIndex   *idx = pwe_catalog_index(rc->cat, isnap->db, isnap->oid);
	PweCon	   *con;
	PweRel	   *trel;
	PweStr		sql;
	int			i;

	if (!irel || rel_is_system_object(irel) || irel->persistence == 't')
		return;
	if (replay_mode(rc) &&
		pwe_hash_get(&rc->created_objs, ((uint64) isnap->db << 32) | isnap->oid))
		return;
	ensure_schema(ctx, isnap->db, irel->nsp);
	con = find_constraint_by_index(rc->cat, isnap->db, isnap->oid);
	if (con && (con->contype == 'p' || con->contype == 'u'))
		return;					/* emitted as a table constraint */
	if (!idx || idx->nkeys == 0)
		return;
	for (i = 0; i < idx->nkeys; i++)
		if (idx->keys[i] == 0)
			return;				/* expression index: not reconstructible */
	trel = pwe_catalog_rel_by_oid(rc->cat, isnap->db, idx->relid);
	if (!trel)
		return;
	if (rc->opts.tocopy || rc->opts.batch_rows > 0)
	{
		/*
		 * A partial index predicate is a pg_node_tree; without decoding it
		 * the index would be created over all rows, which can even break the
		 * load (e.g. UNIQUE over a boolean flag).  Skip it in replay modes.
		 */
		if (idx->partial)
		{
			if (rc->opts.include_comments)
				fprintf(rc->out,
						"-- pg_walex: skipped partial index %s (predicate not decoded)\n",
						irel->name);
			return;
		}
		if (obj_name_taken(rc, isnap->db, irel->nsp, irel->name, isnap->oid))
		{
			PweStr		drop;

			pwe_str_init(&drop);
			pwe_str_adds(&drop, "DROP INDEX IF EXISTS ");
			pwe_str_add_ident(&drop, irel->name);
			pwe_str_adds(&drop, ";\n");
			emit_ddl_stmt(ctx, &drop, "DROP INDEX");
			pwe_str_free(&drop);
		}
		seed_table_meta(ctx, trel);
	}
	/* a table that already received its CREATE with an inline primary key */
	if (idx->primary &&
		(pwe_hash_get(&rc->seed_rels, ((uint64) isnap->db << 32) | idx->relid) ||
		 pwe_hash_get(&rc->created_objs, ((uint64) isnap->db << 32) | idx->relid)))
		return;
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "CREATE ");
	if (idx->unique)
		pwe_str_adds(&sql, "UNIQUE ");
	pwe_str_adds(&sql, "INDEX ");
	pwe_str_add_ident(&sql, irel->name);
	pwe_str_adds(&sql, " ON ");
	qualify_rel(trel, &sql);
	pwe_str_adds(&sql, " USING ");
	pwe_str_adds(&sql, am_name(idx->am));
	pwe_str_adds(&sql, " (");
	for (i = 0; i < idx->nkeys; i++)
	{
		if (i > 0)
			pwe_str_adds(&sql, ", ");
		add_col_name(trel, idx->keys[i], &sql);
	}
	pwe_str_adds(&sql, ");\n");
	if (replay_mode(rc))
	{
		pwe_hash_put(&rc->created_objs, ((uint64) isnap->db << 32) | isnap->oid,
					 (void *) 1);
		obj_name_claim(rc, isnap->db, irel->nsp, irel->name, isnap->oid);
	}
	emit_ddl_stmt(ctx, &sql, "CREATE INDEX");
	pwe_str_free(&sql);
}

static void
emit_constraint(PweDdlCtx *ctx, PweConSnap *cs)
{
	PweReconstructor *rc = ctx->rc;
	PweCon	   *con = pwe_catalog_constraint(rc->cat, cs->db, cs->oid);
	PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, cs->db, cs->relid);
	PweStr		sql;
	int			i;

	if (!con || !rel || rel_is_system_object(rel) || rel->persistence == 't')
		return;
	if (con->contype != 'p' && con->contype != 'u')
		return;
	if (replay_mode(rc) &&
		pwe_hash_get(&rc->created_objs, ((uint64) cs->db << 32) | cs->oid))
		return;					/* reported twice by the catalog stream */
	ensure_schema(ctx, cs->db, rel->nsp);
	if (rc->opts.tocopy || rc->opts.batch_rows > 0)
		seed_table_meta(ctx, rel);
	/*
	 * A synthesized meta file already carries the PRIMARY KEY clause; adding
	 * it again as a constraint would create a duplicate index with the same
	 * name (the constraint's index name equals <table>_pkey).
	 */
	if (con->contype == 'p' &&
		(pwe_hash_get(&rc->seed_rels, ((uint64) cs->db << 32) | cs->relid) ||
		 pwe_hash_get(&rc->created_objs, ((uint64) cs->db << 32) | cs->relid)))
		return;
	if (con->contype == 'p' &&
		pwe_hash_get(&ctx->new_rels, ((uint64) cs->db << 32) | cs->relid))
		return;
	if (con->nkeys == 0)
		return;
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "ALTER TABLE ");
	qualify_rel(rel, &sql);
	pwe_str_adds(&sql, " ADD CONSTRAINT ");
	pwe_str_add_ident(&sql, con->name[0] ? con->name : "constraint");
	pwe_str_adds(&sql, con->contype == 'p' ? " PRIMARY KEY (" : " UNIQUE (");
	for (i = 0; i < con->nkeys; i++)
	{
		if (i > 0)
			pwe_str_adds(&sql, ", ");
		add_col_name(rel, con->keys[i], &sql);
	}
	pwe_str_adds(&sql, ");\n");
	if (replay_mode(rc))
		pwe_hash_put(&rc->created_objs, ((uint64) cs->db << 32) | cs->oid,
					 (void *) 1);
	emit_ddl_stmt(ctx, &sql, "ADD CONSTRAINT");
	pwe_str_free(&sql);
}

static void
emit_add_column(PweDdlCtx *ctx, PweAttrSnap *as)
{
	PweReconstructor *rc = ctx->rc;
	PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, as->db, as->relid);
	PweStr		sql;
	uint64		key = ((uint64) as->db << 32) | as->relid;

	if (!rel || rel_is_system_object(rel) || rel->persistence == 't' ||
		pwe_hash_get(&ctx->new_rels, key))
		return;
	if (rel->relkind != 'r' && rel->relkind != 'p' && rel->relkind != 'f')
		return;
	if (as->num <= 0 || pwe_hash_get(&ctx->dropped_rels, key))
		return;
	/* the column is already part of the metadata used to create the table */
	if ((rc->opts.tocopy || rc->opts.batch_rows > 0) &&
		meta_has_attr(rc, rel, as->name))
		return;
	ensure_schema(ctx, as->db, rel->nsp);
	if (rc->opts.tocopy || rc->opts.batch_rows > 0)
		seed_table_meta(ctx, rel);
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "ALTER TABLE ");
	qualify_rel(rel, &sql);
	pwe_str_adds(&sql, " ADD COLUMN ");
	pwe_str_add_ident(&sql, as->name);
	pwe_str_addc(&sql, ' ');
	add_type_with_mod(rc, as->db, as->typid, as->typmod, &sql);
	/*
	 * pg_attrdef is not decoded, so ADD COLUMN ... NOT NULL without its
	 * DEFAULT would fail on a table that already holds rows.  Replay the
	 * column nullable instead of aborting the whole load.
	 */
	if (as->notnull && !(rc->opts.tocopy || rc->opts.batch_rows > 0))
		pwe_str_adds(&sql, " NOT NULL");
	pwe_str_adds(&sql, ";\n");
	emit_ddl_stmt(ctx, &sql, "ADD COLUMN");
	pwe_str_free(&sql);
}

static void
emit_drop_column(PweDdlCtx *ctx, PweAttrSnap *as)
{
	PweReconstructor *rc = ctx->rc;
	PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, as->db, as->relid);
	PweStr		sql;
	uint64		key = ((uint64) as->db << 32) | as->relid;

	if (!rel || rel_is_system_object(rel) || rel->persistence == 't' ||
		pwe_hash_get(&ctx->dropped_rels, key))
		return;
	if (rc->opts.tocopy || rc->opts.batch_rows > 0)
	{
		/*
		 * With --pgdata the table is re-created from the end-of-window
		 * catalog, so a column that was dropped inside the window is already
		 * absent from the metadata.  Cascades of a table that disappears
		 * later need no per-column drops either; the DROP TABLE removes
		 * everything.  IF EXISTS keeps the remaining case idempotent.
		 */
		if (syscol_name(as->name))
			return;
		if (rc->baseline && !pwe_hash_get(&rc->pre_rels, key) &&
			!pwe_hash_get(&rc->created_objs, key))
			return;
		/* the column is part of the metadata that created the table */
		if (meta_has_attr(rc, rel, as->name))
			return;
		seed_table_meta(ctx, rel);
	}
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "ALTER TABLE ");
	qualify_rel(rel, &sql);
	pwe_str_adds(&sql, " DROP COLUMN ");
	if (rc->opts.tocopy || rc->opts.batch_rows > 0)
		pwe_str_adds(&sql, "IF EXISTS ");
	pwe_str_add_ident(&sql, as->name);
	pwe_str_adds(&sql, ";\n");
	emit_ddl_stmt(ctx, &sql, "DROP COLUMN");
	pwe_str_free(&sql);
}

static bool
emit_drop_relation(PweDdlCtx *ctx, PweRelSnap *snap)
{
	PweReconstructor *rc = ctx->rc;
	PweStr		sql;
	const char *kind = NULL;

	if (snap->persistence == 't')
		return false;
	/*
	 * Rewrite heaps (pg_temp_NNN) created by ALTER TABLE ... TYPE are transient
	 * internal relations: no CREATE is emitted for them, so dropping them would
	 * only cause a replay error.
	 */
	if (copy_is_rewrite_heap(snap->name))
		return false;
	/*
	 * Toast indexes are internal objects that disappear together with their
	 * table; they are never created explicitly, so dropping them by name would
	 * only produce a replay error.
	 */
	if (snap->relkind == 'i')
	{
		const char *ns = pwe_catalog_ns_name(rc->cat, snap->db, snap->nsp);

		if (ns && strcmp(ns, "pg_toast") == 0)
			return false;
	}
	/*
	 * An index that goes away with its table or its constraint is not dropped
	 * by name: "DROP INDEX" would fail on a constraint index and the index no
	 * longer exists after "DROP TABLE".
	 */
	if (snap->relkind == 'i' && index_dropped_with_parent(ctx->txn, snap))
		return false;
	/*
	 * In replay-oriented modes only objects created by this replay can (and
	 * need to) be dropped.  Everything else is a cascade pg_class row of an
	 * object that is absent from the end-of-window catalog - e.g. the index
	 * and sequence rows of a table dropped later - and replaying it would
	 * only produce "does not exist" or "constraint requires it" errors.
	 */
	if (rc->opts.tocopy || rc->opts.batch_rows > 0)
	{
		uint64		key = ((uint64) snap->db << 32) | snap->oid;

		if (pwe_hash_get(&rc->dropped_objs, key))
			return false;
		/*
		 * Only the object that currently owns the name can be dropped; a
		 * stale pg_class delete (the old incarnation was already replaced by
		 * a colliding CREATE) must not remove its successor.
		 */
		if (!obj_name_owned(rc, snap->db, snap->nsp, snap->name, snap->oid))
			return false;
		pwe_hash_put(&rc->dropped_objs, key, (void *) 1);
	}
	switch (snap->relkind)
	{
		case 'r':
		case 'p':
			kind = "DROP TABLE ";
			break;
		case 'i':
			kind = "DROP INDEX ";
			break;
		case 'S':
			kind = "DROP SEQUENCE ";
			break;
		case 'v':
			/*
			 * View definitions are not part of WAL.  In replay-oriented
			 * modes the view cannot have been created, so drop it softly.
			 */
			kind = (rc->opts.tocopy || rc->opts.batch_rows > 0) ?
				"DROP VIEW IF EXISTS " : "DROP VIEW ";
			break;
		case 'm':
			kind = (rc->opts.tocopy || rc->opts.batch_rows > 0) ?
				"DROP MATERIALIZED VIEW IF EXISTS " : "DROP MATERIALIZED VIEW ";
			break;
		case 'f':
			kind = "DROP FOREIGN TABLE ";
			break;
		default:
			return false;
	}
	pwe_str_init(&sql);
	pwe_str_adds(&sql, kind);
	qualify_snap(rc, snap, &sql);
	pwe_str_adds(&sql, ";\n");
	emit_ddl_stmt(ctx, &sql, kind);
	pwe_str_free(&sql);
	return true;
}

/*
 * An index that disappears together with its table or its constraint must not
 * be dropped by name: PostgreSQL rejects "DROP INDEX" for a constraint index,
 * and after "DROP TABLE" the index no longer exists.  Both cases show up as a
 * pg_index / pg_constraint delete in the same transaction.
 */
static bool
index_dropped_with_parent(PweTxn *txn, PweRelSnap *snap)
{
	PweEvent   *ev;
	uint32		parent = 0;

	if (!txn)
		return false;
	for (ev = txn->events; ev; ev = ev->next)
	{
		PweCatPayload *p = ev->cat;

		if (!p || p->op != PWE_OP_DELETE)
			continue;
		if (p->cat == PWE_CAT_PG_CONSTRAINT && p->o.con.db == snap->db &&
			p->o.con.indid == snap->oid)
			return true;
		if (p->cat == PWE_CAT_PG_INDEX && p->o.idx.db == snap->db &&
			p->o.idx.oid == snap->oid)
			parent = p->o.idx.relid;
	}
	if (parent)
	{
		for (ev = txn->events; ev; ev = ev->next)
		{
			PweCatPayload *p = ev->cat;

			if (p && p->cat == PWE_CAT_PG_CLASS && p->op == PWE_OP_DELETE &&
				p->o.rel.db == snap->db && p->o.rel.oid == parent)
				return true;
		}
	}
	return false;
}

/*
 * Column definitions of a table that is being dropped.  pg_attribute rows are
 * deleted before the pg_class row, so by the time the table is detached its
 * attribute array is already empty; the delete events themselves still carry
 * the old attribute snapshots.
 */
typedef struct PweDropAttrs
{
	PweAttrSnap *attrs[1600];
	int			n;
	PweConSnap *pk;
} PweDropAttrs;

static int
drop_attr_cmp(const void *a, const void *b)
{
	const PweAttrSnap *x = *(const PweAttrSnap *const *) a;
	const PweAttrSnap *y = *(const PweAttrSnap *const *) b;

	return (int) x->num - (int) y->num;
}

static void
collect_drop_attrs(PweTxn *txn, uint32 db, uint32 relid, PweDropAttrs *out)
{
	PweEvent   *ev;

	memset(out, 0, sizeof(*out));
	if (!txn)
		return;
	for (ev = txn->events; ev; ev = ev->next)
	{
		PweCatPayload *p = ev->cat;

		if (!p || p->op != PWE_OP_DELETE)
			continue;
		if (p->cat == PWE_CAT_PG_ATTRIBUTE && p->o.attr.db == db &&
			p->o.attr.relid == relid && p->o.attr.num > 0 &&
			!p->o.attr.dropped)
		{
			if (out->n < (int) (sizeof(out->attrs) / sizeof(out->attrs[0])))
				out->attrs[out->n++] = &p->o.attr;
		}
		else if (p->cat == PWE_CAT_PG_CONSTRAINT && p->o.con.db == db &&
				 p->o.con.relid == relid && p->o.con.contype == 'p' &&
				 p->o.con.nkeys > 0)
			out->pk = &p->o.con;
	}
	if (out->n > 1)
		qsort(out->attrs, (size_t) out->n, sizeof(out->attrs[0]), drop_attr_cmp);
}

static PweAttrSnap *
find_drop_attr(PweDropAttrs *attrs, int16 num)
{
	int			i;

	for (i = 0; i < attrs->n; i++)
		if (attrs->attrs[i]->num == num)
			return attrs->attrs[i];
	return NULL;
}

/* INSERT column list built from the collected attribute snapshots */
static void
drop_insert_columns(PweDropAttrs *attrs, PweStr *out, bool *always_id)
{
	int			i;
	bool		first = true;

	pwe_str_addc(out, '(');
	for (i = 0; i < attrs->n; i++)
	{
		PweAttrSnap *a = attrs->attrs[i];

		if (a->generated == 's')
			continue;
		if (!first)
			pwe_str_adds(out, ", ");
		pwe_str_add_ident(out, a->name);
		first = false;
		if (a->identity == 'a')
			*always_id = true;
	}
	pwe_str_addc(out, ')');
}

static void
drop_insert_values(PweDropAttrs *attrs, PweRowVals *row, PweStr *out)
{
	int			i;
	bool		first = true;

	pwe_str_addc(out, '(');
	for (i = 0; i < attrs->n; i++)
	{
		PweAttrSnap *a = attrs->attrs[i];

		if (a->generated == 's')
			continue;
		if (!first)
			pwe_str_adds(out, ", ");
		if (a->num > 0 && a->num <= row->natts && row->vals[a->num])
			pwe_str_adds(out, row->vals[a->num]);
		else
			pwe_str_adds(out, "NULL");
		first = false;
	}
	pwe_str_addc(out, ')');
}

/*
 * Render "CREATE TABLE" for a table that is about to be dropped.  Column
 * definitions come from the pg_attribute delete events, falling back to the
 * detached relation when those are not in the read range.
 */
static bool
render_create_table_from_drop(PweReconstructor *rc, PweRel *rel,
							  PweDropAttrs *attrs, PweStr *sql)
{
	int			i;

	if (attrs->n == 0)
		return false;
	pwe_str_adds(sql, "CREATE ");
	if (rel->persistence == 'u')
		pwe_str_adds(sql, "UNLOGGED ");
	pwe_str_adds(sql, "TABLE ");
	qualify_rel(rel, sql);
	pwe_str_adds(sql, " (\n");
	for (i = 0; i < attrs->n; i++)
	{
		PweAttrSnap *a = attrs->attrs[i];

		if (i > 0)
			pwe_str_adds(sql, ",\n");
		pwe_str_adds(sql, "  ");
		pwe_str_add_ident(sql, a->name);
		pwe_str_addc(sql, ' ');
		add_type_with_mod(rc, rel->db, a->typid, a->typmod, sql);
		if (a->notnull)
			pwe_str_adds(sql, " NOT NULL");
	}
	if (attrs->pk)
	{
		pwe_str_adds(sql, ",\n  PRIMARY KEY (");
		for (i = 0; i < attrs->pk->nkeys && i < 32; i++)
		{
			PweAttrSnap *a = find_drop_attr(attrs, attrs->pk->keys[i]);

			if (i > 0)
				pwe_str_adds(sql, ", ");
			if (a)
				pwe_str_add_ident(sql, a->name);
			else
				pwe_str_addf(sql, "att%d", attrs->pk->keys[i]);
		}
		pwe_str_addc(sql, ')');
	}
	pwe_str_adds(sql, "\n);\n");
	return true;
}

/*
 * --rollback for DROP TABLE: recreate the table from the catalog snapshots
 * carried by the delete events and re-insert every row still held in the row
 * cache.  Only what is available is restored (definition + rows seen in the
 * WAL window); secondary indexes are not recreated.
 */
static void
emit_drop_rollback(PweDdlCtx *ctx, PweCatPayload *p)
{
	PweReconstructor *rc = ctx->rc;
	PweRel	   *rel = (PweRel *) p->saved_rel;
	PweDropAttrs attrs;
	PweStr		sql;
	size_t		b;

	if (!rc->opts.rollback || !rel)
		return;
	if (!(rel->relkind == 'r' || rel->relkind == 'p' || rel->relkind == 'f'))
		return;
	if (rel->persistence == 't' || copy_is_rewrite_heap(rel->name))
		return;
	collect_drop_attrs(ctx->txn, p->o.rel.db, p->o.rel.oid, &attrs);
	pwe_str_init(&sql);
	if (!render_create_table_from_drop(rc, rel, &attrs, &sql))
	{
		pwe_str_free(&sql);
		return;
	}
	if (rel->rows)
	{
		for (b = 0; b < rel->rows->nbuckets; b++)
		{
			PweHashEntry *e;

			for (e = rel->rows->buckets[b]; e; e = e->next)
			{
				PweRowVals *row = e->val;
				bool		always_id = false;

				pwe_str_adds(&sql, "INSERT INTO ");
				qualify_rel(rel, &sql);
				pwe_str_addc(&sql, ' ');
				drop_insert_columns(&attrs, &sql, &always_id);
				if (always_id)
					pwe_str_adds(&sql, " OVERRIDING SYSTEM VALUE");
				pwe_str_adds(&sql, " VALUES ");
				drop_insert_values(&attrs, row, &sql);
				pwe_str_adds(&sql, ";\n");
			}
		}
	}
	txn_add_sql(rc, ctx->txn, ctx->cur_lsn, &sql, NULL);
	if (ctx->txn->tail)
		ctx->txn->tail->stmt = PWE_STMT_ROLLBACK;
	pwe_str_free(&sql);
}

static void
emit_create_schema(PweDdlCtx *ctx, PweNsSnap *ns)
{
	PweStr		sql;

	pwe_hash_put(&ctx->rc->schema_created, ((uint64) ns->db << 32) | ns->oid, (void *) 1);
	if (replay_mode(ctx->rc) && schema_is_temp(ns->name))
		return;
	pwe_str_init(&sql);
	if (ctx->rc->opts.tocopy || ctx->rc->opts.batch_rows > 0)
		pwe_str_adds(&sql, "CREATE SCHEMA IF NOT EXISTS ");
	else
		pwe_str_adds(&sql, "CREATE SCHEMA ");
	pwe_str_add_ident(&sql, ns->name);
	pwe_str_adds(&sql, ";\n");
	emit_ddl_stmt(ctx, &sql, "CREATE SCHEMA");
	pwe_str_free(&sql);
}

static uint64
enum_label_key(uint32 db, uint32 typid, const char *label)
{
	uint64		h = ((uint64) db << 32) | typid;

	h ^= pwe_crc32c(0, label, strlen(label));
	return h;
}

static void
emit_create_type(PweDdlCtx *ctx, PweTypeSnap *ts)
{
	PweReconstructor *rc = ctx->rc;
	PweTypeInfo ti = pwe_catalog_type_info(rc->cat, ts->db, ts->oid);
	PweStr		sql;
	PweEnumVal *list[256];
	int			n = 0;
	size_t		i;

	if (ti.typtype != 'e' && ti.typtype != 'd')
		return;
	for (i = 0; i < rc->cat->enums.nbuckets && n < 256; i++)
	{
		PweHashEntry *e;

		for (e = rc->cat->enums.buckets[i]; e && n < 256; e = e->next)
		{
			PweEnumVal *ev = e->val;

			if (ev->db == ts->db && ev->typid == ts->oid)
				list[n++] = ev;
		}
	}
	if (ti.typtype == 'd')
		return;					/* domain definitions are not reconstructed */
	if (n == 0)
		return;
	/* sort by sortorder */
	for (i = 0; i < (size_t) n; i++)
	{
		int			j;
		int			best = (int) i;

		for (j = best + 1; j < n; j++)
			if (list[j]->sortorder < list[best]->sortorder)
				best = j;
		if (best != (int) i)
		{
			PweEnumVal *t = list[i];

			list[i] = list[best];
			list[best] = t;
		}
	}
	ensure_schema(ctx, ts->db, ts->nsp);
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "CREATE TYPE ");
	{
		const char *ns = pwe_catalog_ns_name(rc->cat, ts->db, ts->nsp);

		if (ns && strcmp(ns, "pg_catalog") != 0)
		{
			pwe_str_add_ident(&sql, ns);
			pwe_str_addc(&sql, '.');
		}
	}
	pwe_str_add_ident(&sql, ts->name);
	pwe_str_adds(&sql, " AS ENUM (");
	for (i = 0; i < (size_t) n; i++)
	{
		if (i > 0)
			pwe_str_adds(&sql, ", ");
		pwe_str_add_quoted(&sql, list[i]->label, strlen(list[i]->label));
	}
	pwe_str_adds(&sql, ");\n");
	pwe_hash_put(&ctx->rc->type_created,
				 ((uint64) ts->db << 32) | ts->oid, (void *) 1);
	for (i = 0; i < (size_t) n; i++)
		pwe_hash_put(&ctx->rc->enum_created,
					 enum_label_key(ts->db, ts->oid, list[i]->label), (void *) 1);
	emit_ddl_stmt(ctx, &sql, "CREATE TYPE");
	pwe_str_free(&sql);
}

static void
emit_create_type_from_catalog(PweDdlCtx *ctx, uint32 db, uint32 typid)
{
	PweReconstructor *rc = ctx->rc;
	PweTypeInfo ti = pwe_catalog_type_info(rc->cat, db, typid);
	PweEnumVal *list[256];
	int			n = 0;
	size_t		i;
	PweStr		sql;
	const char *ns;

	for (i = 0; i < rc->cat->enums.nbuckets && n < 256; i++)
	{
		PweHashEntry *e;

		for (e = rc->cat->enums.buckets[i]; e && n < 256; e = e->next)
		{
			PweEnumVal *ev = e->val;

			if (ev->db == db && ev->typid == typid)
				list[n++] = ev;
		}
	}
	if (n == 0)
		return;
	for (i = 0; i < (size_t) n; i++)
	{
		int			j;
		int			best = (int) i;

		for (j = best + 1; j < n; j++)
			if (list[j]->sortorder < list[best]->sortorder)
				best = j;
		if (best != (int) i)
		{
			PweEnumVal *t = list[i];

			list[i] = list[best];
			list[best] = t;
		}
	}
	ensure_schema(ctx, db, ti.nsp);
	ns = pwe_catalog_ns_name(rc->cat, db, ti.nsp);
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "CREATE TYPE ");
	if (ns && strcmp(ns, "pg_catalog") != 0)
	{
		pwe_str_add_ident(&sql, ns);
		pwe_str_addc(&sql, '.');
	}
	pwe_str_add_ident(&sql, ti.name);
	pwe_str_adds(&sql, " AS ENUM (");
	for (i = 0; i < (size_t) n; i++)
	{
		if (i > 0)
			pwe_str_adds(&sql, ", ");
		pwe_str_add_quoted(&sql, list[i]->label, strlen(list[i]->label));
	}
	pwe_str_adds(&sql, ");\n");
	pwe_hash_put(&rc->type_created, ((uint64) db << 32) | typid, (void *) 1);
	for (i = 0; i < (size_t) n; i++)
		pwe_hash_put(&rc->enum_created,
					 enum_label_key(db, typid, list[i]->label), (void *) 1);
	emit_ddl_stmt(ctx, &sql, "CREATE TYPE");
	pwe_str_free(&sql);
}

static void
emit_enum_add(PweDdlCtx *ctx, PweEnumSnap *es)
{
	PweReconstructor *rc = ctx->rc;
	PweTypeInfo ti = pwe_catalog_type_info(rc->cat, es->db, es->typid);
	PweStr		sql;
	const char *ns;

	if (!ti.found || ti.typtype != 'e')
		return;
	if (!pwe_hash_get(&rc->type_created, ((uint64) es->db << 32) | es->typid))
	{
		emit_create_type_from_catalog(ctx, es->db, es->typid);
		if (!pwe_hash_get(&rc->type_created, ((uint64) es->db << 32) | es->typid))
			return;
		return;
	}
	if (pwe_hash_get(&rc->enum_created, enum_label_key(es->db, es->typid, es->label)))
		return;
	ns = pwe_catalog_ns_name(rc->cat, es->db, ti.nsp);
	pwe_str_init(&sql);
	pwe_str_adds(&sql, "ALTER TYPE ");
	if (ns && strcmp(ns, "pg_catalog") != 0)
	{
		pwe_str_add_ident(&sql, ns);
		pwe_str_addc(&sql, '.');
	}
	pwe_str_add_ident(&sql, ti.name);
	pwe_str_adds(&sql, " ADD VALUE IF NOT EXISTS ");
	pwe_str_add_quoted(&sql, es->label, strlen(es->label));
	pwe_str_adds(&sql, ";\n");
	pwe_hash_put(&rc->enum_created,
				 enum_label_key(es->db, es->typid, es->label), (void *) 1);
	emit_ddl_stmt(ctx, &sql, "ALTER TYPE");
	pwe_str_free(&sql);
}

static void
emit_catalog_event(PweDdlCtx *ctx, PweCatPayload *p)
{
	PweReconstructor *rc = ctx->rc;

	switch (p->cat)
	{
		case PWE_CAT_PG_CLASS:
			if (p->op == PWE_OP_INSERT || p->op == PWE_OP_UPDATE)
			{
				if (p->op == PWE_OP_INSERT)
				{
					switch (p->n.rel.relkind)
					{
						case 'r':
						case 'p':
						case 'f':
							emit_create_table(ctx, &p->n.rel);
							break;
						case 'S':
							emit_create_sequence(ctx, &p->n.rel);
							break;
						case 'v':
							if (rc->opts.include_comments)
								fprintf(ctx->out,
										"-- pg_walex: CREATE VIEW (definition not present in WAL)\n");
							break;
						case 'm':
							if (rc->opts.include_comments)
								fprintf(ctx->out,
										"-- pg_walex: CREATE MATERIALIZED VIEW (definition not present in WAL)\n");
							break;
						default:
							break;
					}
				}
			}
			else if (emit_drop_relation(ctx, &p->o.rel))
				emit_drop_rollback(ctx, p);
			break;
		case PWE_CAT_PG_INDEX:
			if (p->op == PWE_OP_INSERT)
				emit_create_index(ctx, &p->n.idx);
			break;
		case PWE_CAT_PG_CONSTRAINT:
			if (p->op == PWE_OP_INSERT)
				emit_constraint(ctx, &p->n.con);
			break;
		case PWE_CAT_PG_ATTRIBUTE:
			if (p->op == PWE_OP_INSERT)
				emit_add_column(ctx, &p->n.attr);
			else if (p->op == PWE_OP_DELETE)
			{
				uint64		key = ((uint64) p->n.attr.db << 32) | p->n.attr.relid;

				if (!pwe_hash_get(&ctx->new_rels, key))
					emit_drop_column(ctx, &p->n.attr);
			}
			else					/* update: dropped / notnull / type change */
			{
				PweRel	   *rel = pwe_catalog_rel_by_oid(rc->cat, p->n.attr.db,
														  p->n.attr.relid);

				if (!p->o.attr.dropped && p->n.attr.dropped)
					emit_drop_column(ctx, &p->o.attr);
				else if (p->o.attr.notnull != p->n.attr.notnull &&
						 !(rc->baseline &&
						   (rc->opts.tocopy || rc->opts.batch_rows > 0)))
				{
					PweStr		sql;

					if (rel && rel->persistence != 't')
					{
						if (rc->opts.tocopy || rc->opts.batch_rows > 0)
							seed_table_meta(ctx, rel);
						pwe_str_init(&sql);
						pwe_str_adds(&sql, "ALTER TABLE ");
						qualify_rel(rel, &sql);
						pwe_str_adds(&sql, p->n.attr.notnull ?
									 " ALTER COLUMN " : " ALTER COLUMN ");
						pwe_str_add_ident(&sql, p->n.attr.name);
						pwe_str_adds(&sql, p->n.attr.notnull ?
									 " SET NOT NULL;\n" : " DROP NOT NULL;\n");
						emit_ddl_stmt(ctx, &sql, "ALTER COLUMN");
						pwe_str_free(&sql);
					}
				}
				else if ((rc->opts.tocopy || rc->opts.batch_rows > 0) &&
						 rel && rel->persistence != 't' &&
						 !rc->baseline &&
						 (p->o.attr.typid != p->n.attr.typid ||
						  p->o.attr.typmod != p->n.attr.typmod))
				{
					/*
					 * ALTER COLUMN TYPE (e.g. int4 -> int8) rewrites the
					 * table; rows logged afterwards already carry the new
					 * type, so the replay target must be altered to match.
					 * Emitted for --tocopy / --batch-inserts only, so the
					 * plain SQL output is unchanged.
					 */
					bool		ok = true;

					seed_table_meta(ctx, rel);
					if (rc->opts.tocopy)
					{
						PweCopyTable *ct = pwe_hash_get(&rc->copy_tables,
														((uint64) p->n.attr.db << 32) |
														p->n.attr.relid);

						ok = (ct && ct->meta_written);
					}
					if (ok)
					{
						PweStr		sql;
						PweStr		t;

						pwe_str_init(&sql);
						pwe_str_init(&t);
						pwe_str_adds(&sql, "ALTER TABLE ");
						qualify_rel(rel, &sql);
						pwe_str_adds(&sql, " ALTER COLUMN ");
						pwe_str_add_ident(&sql, p->n.attr.name);
						pwe_str_adds(&sql, " TYPE ");
						add_type_with_mod(rc, rel->db, p->n.attr.typid,
										  p->n.attr.typmod, &t);
						pwe_str_add(&sql, pwe_str_cstr(&t), t.len);
						pwe_str_adds(&sql, " USING ");
						pwe_str_add_ident(&sql, p->n.attr.name);
						pwe_str_adds(&sql, "::");
						pwe_str_add(&sql, pwe_str_cstr(&t), t.len);
						pwe_str_adds(&sql, ";\n");
						emit_ddl_stmt(ctx, &sql, "ALTER COLUMN TYPE");
						pwe_str_free(&sql);
						pwe_str_free(&t);
					}
				}
			}
			break;
		case PWE_CAT_PG_NAMESPACE:
			if (p->op == PWE_OP_INSERT)
				emit_create_schema(ctx, &p->n.ns);
			break;
		case PWE_CAT_PG_TYPE:
			if (p->op == PWE_OP_INSERT)
				emit_create_type(ctx, &p->n.typ);
			break;
		case PWE_CAT_PG_ENUM:
			if (p->op == PWE_OP_INSERT)
				emit_enum_add(ctx, &p->n.en);
			break;
		case PWE_CAT_PG_DATABASE:
			if (rc->opts.include_comments)
				fprintf(ctx->out, "-- pg_walex: %s DATABASE %s\n",
						p->op == PWE_OP_INSERT ? "CREATE" : "DROP", p->n.ns.name);
			break;
		default:
			break;
	}
}

/* --batch-inserts: flush the pending multi-VALUES INSERT buffer */
static void
batch_flush(PweReconstructor *rc, PweStr *pend, bool *pend_open)
{
	if (!*pend_open)
		return;
	pwe_str_adds(pend, ";\n");
	fputs(pwe_str_cstr(pend), rc->out);
	pwe_str_reset(pend);
	*pend_open = false;
}

/*
 * --batch-inserts: feed one SQL statement into the batch buffer.
 * Returns true when the statement was absorbed (an INSERT whose
 * "INSERT INTO .. VALUES " prefix matches the pending one is merged
 * into a single multi-row INSERT).
 */
static bool
batch_feed(PweReconstructor *rc, PweStr *pend, bool *pend_open,
		   size_t *pend_prefix, uint32 *pend_rows, const char *sql)
{
	const char *v;
	size_t		len,
				prefix_len,
				tlen;

	if (!rc->opts.batch_rows)
		return false;
	if (strncmp(sql, "INSERT INTO ", 12) != 0)
		return false;
	v = strstr(sql, " VALUES ");
	len = strlen(sql);
	if (!v || len < 2 || sql[len - 1] != '\n' || sql[len - 2] != ';')
		return false;
	prefix_len = (size_t) (v - sql) + 8;	/* includes " VALUES " */
	tlen = len - prefix_len - 2;	/* tuple list without trailing ";\n" */
	if (*pend_open && *pend_prefix == prefix_len &&
		memcmp(pend->data, sql, prefix_len) == 0)
	{
		pwe_str_adds(pend, ", ");
		pwe_str_add(pend, sql + prefix_len, tlen);
		(*pend_rows)++;
		if (*pend_rows >= rc->opts.batch_rows)
			batch_flush(rc, pend, pend_open);
		return true;
	}
	batch_flush(rc, pend, pend_open);
	pwe_str_add(pend, sql, len - 2);	/* statement without ";\n" */
	*pend_prefix = prefix_len;
	*pend_rows = 1;
	*pend_open = true;
	return true;
}

static void
emit_transaction(PweReconstructor *rc, PweTxn *txn)
{
	PweDdlCtx	ctx;

	if (rc->opts.only_xid && txn->xid != rc->opts.only_xid)
		return;
	if (rc->opts.only_database &&
		(txn->db == 0 || txn->db != rc->opts.only_database))
		return;
	PweEvent   *ev;
	bool		begun = false;
	uint64		total_sql = 0;
	PweStr		pend;
	bool		pend_open = false;
	size_t		pend_prefix = 0;
	uint32		pend_rows = 0;
	uint64		pend_rel = 0;	/* relation of the buffered INSERTs */

	pwe_str_init(&pend);
	memset(&ctx, 0, sizeof(ctx));
	ctx.rc = rc;
	ctx.txn = txn;
	ctx.out = rc->out;
	ctx.begun = &begun;
	ctx.hdr_type = NULL;			/* resolved from the first emitted statement */
	ctx.hdr_lsn = 0;
	pwe_hash_init(&ctx.new_rels);
	pwe_hash_init(&ctx.dropped_rels);
	pwe_hash_init(&ctx.new_types);
	/*
	 * The transaction comment describes the first emitted statement: its
	 * type (derived from the generated SQL) and the LSN of the WAL record
	 * that produced it.
	 */
	for (ev = txn->events; ev; ev = ev->next)
	{
		if (rc->opts.start_lsn && ev->lsn < rc->opts.start_lsn)
			continue;
		if (ev->kind == PWE_EV_SQL || ev->kind == PWE_EV_COPYDATA || ev->cat)
		{
			ctx.hdr_lsn = ev->lsn;
			ctx.hdr_type = event_type_name(ev);
			break;
		}
	}
	for (ev = txn->events; ev; ev = ev->next)
	{
		if (rc->opts.start_lsn && ev->lsn < rc->opts.start_lsn)
			continue;
		if (ev->cat)
		{
			if (ev->cat->cat == PWE_CAT_PG_CLASS && ev->cat->op == PWE_OP_INSERT)
				pwe_hash_put(&ctx.new_rels,
							 ((uint64) ev->cat->n.rel.db << 32) | ev->cat->n.rel.oid,
							 (void *) 1);
			if (ev->cat->cat == PWE_CAT_PG_TYPE && ev->cat->op == PWE_OP_INSERT)
				pwe_hash_put(&ctx.new_types,
							 ((uint64) ev->cat->n.typ.db << 32) | ev->cat->n.typ.oid,
							 (void *) 1);
			if (ev->cat->cat == PWE_CAT_PG_CLASS && ev->cat->op == PWE_OP_DELETE)
				pwe_hash_put(&ctx.dropped_rels,
							 ((uint64) ev->cat->o.rel.db << 32) | ev->cat->o.rel.oid,
							 (void *) 1);
		}
	}
	for (ev = txn->events; ev; ev = ev->next)
	{
		if (rc->opts.start_lsn && ev->lsn < rc->opts.start_lsn)
			continue;
		if (ev->kind == PWE_EV_COPYDATA)
		{
			/* --tocopy: append the row to the table's data segment */
			copy_write_row(rc, ev);
			if (rc->stats)
				rc->stats->nsql++;
			continue;
		}
		if (rc->opts.tocopy)
			copy_flush_segments(rc);	/* data precedes this statement */
		if (ev->kind == PWE_EV_SQL && ev->stmt == PWE_STMT_ROLLBACK)
		{
			char		lsnbuf[64];
			char		tsbuf[64];

			/* inverse statement: its own comment, never batched/copied */
			batch_flush(rc, &pend, &pend_open);
			pend_rel = 0;
			ensure_begin(rc, txn, rc->out, &begun, ctx.hdr_type, ctx.hdr_lsn);
			if (rc->opts.include_comments)
			{
				pwe_format_lsn(lsnbuf, sizeof(lsnbuf), ev->lsn);
				if (txn->has_commit_time)
					format_commit_ts(tsbuf, sizeof(tsbuf), txn->commit_time);
				else
					snprintf(tsbuf, sizeof(tsbuf), "unknown");
				fprintf(rc->out, "-- pg_walex: rollback,xid=%u,lsn=%s,timestamp=%s\n",
						txn->xid, lsnbuf, tsbuf);
			}
			if (ev->sql)
				fputs(ev->sql, rc->out);
			if (rc->stats)
				rc->stats->nsql++;
			continue;
		}
		if (ev->kind == PWE_EV_SQL)
		{
			if (!ctx.hdr_type)
				ctx.hdr_type = sql_type_name(ev->sql);
			ensure_begin(rc, txn, rc->out, &begun, ctx.hdr_type, ctx.hdr_lsn);
			if (ev->relkey && (rc->opts.tocopy || rc->opts.batch_rows > 0))
			{
				PweRel	   *srel = pwe_catalog_rel_by_oid(rc->cat,
														   (uint32) (ev->relkey >> 32),
														   (uint32) ev->relkey);
				/*
				 * Only a statement that cannot join the pending multi-VALUES
				 * buffer (another relation, or not an INSERT) has to flush it
				 * first; flushing before every INSERT would defeat batching.
				 */
				if (rc->opts.tocopy || !ev->sql ||
					strncmp(ev->sql, "INSERT INTO ", 12) != 0 ||
					ev->relkey != pend_rel)
				{
					batch_flush(rc, &pend, &pend_open);
					pend_rel = 0;
				}
				seed_table_meta(&ctx, srel);
			}
			else if (rc->opts.batch_rows > 0)
				batch_flush(rc, &pend, &pend_open);
			if (!batch_feed(rc, &pend, &pend_open, &pend_prefix,
							&pend_rows, ev->sql))
			{
				if (pend_open)
				{
					batch_flush(rc, &pend, &pend_open);
					pend_rel = 0;
				}
				fputs(ev->sql, rc->out);
			}
			else if (pend_open)
				pend_rel = ev->relkey;
			if (rc->stats)
				rc->stats->nsql++;
			total_sql++;
		}
		else if (ev->cat)
		{
			batch_flush(rc, &pend, &pend_open);
			pend_rel = 0;
			ctx.cur_lsn = ev->lsn;
			emit_catalog_event(&ctx, ev->cat);
		}
	}
	batch_flush(rc, &pend, &pend_open);
	if (begun)
	{
		fprintf(rc->out, "COMMIT;\n\n");
		if (rc->stats)
			rc->stats->ntxns++;
	}
	pwe_hash_free(&ctx.new_rels, NULL);
	pwe_hash_free(&ctx.dropped_rels, NULL);
	pwe_hash_free(&ctx.new_types, NULL);
	pwe_str_free(&pend);
	(void) total_sql;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */
PweReconstructor *
pwe_recon_create(int version, const PweSqlOptions *opts, FILE *out, PweStats *stats)
{
	PweReconstructor *rc = pwe_calloc(1, sizeof(PweReconstructor));

	rc->version = version;
	rc->opts = *opts;
	rc->out = out ? out : stdout;
	rc->stats = stats;
	rc->cat = pwe_catalog_create(version, opts->max_row_cache, 0);
	rc->baseline = (opts->pgdata && opts->pgdata[0]) ? true : false;
	pwe_value_set_legacy_arrays(!(opts->tocopy || opts->batch_rows > 0));
	if (opts->pgdata && opts->pgdata[0])
		pwe_catalog_seed_from_pgdata(rc->cat, opts->pgdata, opts->only_database);
	pwe_hash_init(&rc->txns);
	pwe_hash_init(&rc->enum_created);
	pwe_hash_init(&rc->type_created);
	pwe_hash_init(&rc->schema_created);
	pwe_hash_init(&rc->unknown_rels);
	pwe_hash_init(&rc->copy_tables);
	pwe_hash_init(&rc->seed_rels);
	pwe_hash_init(&rc->created_objs);
	pwe_hash_init(&rc->dropped_objs);
	pwe_hash_init(&rc->obj_names);
	pwe_hash_init(&rc->file_bases);
	pwe_hash_init(&rc->meta_attrs);
	pwe_hash_init(&rc->pre_indexes);
	pwe_hash_init(&rc->pre_cons);
	pwe_hash_init(&rc->pre_rels);
	if (opts->pgdata && opts->pgdata[0])
	{
		size_t		i;

		/*
		 * Snapshot the relations inherited from pgdata.  Their metadata is
		 * the state at the start of the WAL window, so a synthesized
		 * CREATE TABLE must use it rather than the catalog state at the
		 * moment the table is first referenced in the replay.
		 */
		for (i = 0; i < rc->cat->rels_by_oid.nbuckets; i++)
		{
			PweHashEntry *e;

			for (e = rc->cat->rels_by_oid.buckets[i]; e; e = e->next)
			{
				PweRel	   *rel = e->val;
				PweRelInit *ri;
				int			j;

				if (rel->relkind != 'r' && rel->relkind != 'p' &&
					rel->relkind != 'f')
					continue;
				ri = pwe_calloc(1, sizeof(PweRelInit));
				ri->rel = *rel;
				ri->rel.rows = NULL;
				ri->rel.natts = rel->natts;
				ri->rel.npk = rel->npk;
				ri->attrs = pwe_calloc((size_t) rel->natts + 1,
									   sizeof(PweAttr *));
				for (j = 0; j <= rel->natts; j++)
				{
					if (rel->attrs && rel->attrs[j])
					{
						ri->attrs[j] = pwe_malloc(sizeof(PweAttr));
						*ri->attrs[j] = *rel->attrs[j];
					}
				}
				ri->rel.attrs = ri->attrs;
				if (rel->npk > 0 && rel->pk)
				{
					ri->pk = pwe_malloc(sizeof(int16) * (size_t) rel->npk);
					memcpy(ri->pk, rel->pk, sizeof(int16) * (size_t) rel->npk);
					ri->rel.pk = ri->pk;
				}
				else
					ri->rel.pk = NULL;
				pwe_hash_put(&rc->pre_rels,
							 ((uint64) rel->db << 32) | rel->oid, ri);
			}
		}
		/* indexes and constraints that already exist in the pgdata snapshot */
		for (i = 0; i < rc->cat->indexes.nbuckets; i++)
		{
			PweHashEntry *e;

			for (e = rc->cat->indexes.buckets[i]; e; e = e->next)
			{
				PweIndex   *ix = e->val;

				pwe_hash_put(&rc->pre_indexes,
							 ((uint64) ix->db << 32) | ix->oid, (void *) 1);
			}
		}
		for (i = 0; i < rc->cat->constraints.nbuckets; i++)
		{
			PweHashEntry *e;

			for (e = rc->cat->constraints.buckets[i]; e; e = e->next)
			{
				PweCon	   *cn = e->val;

				pwe_hash_put(&rc->pre_cons,
							 ((uint64) cn->db << 32) | cn->oid, (void *) 1);
			}
		}
	}
	pwe_str_init(&rc->tmp);
	pwe_str_init(&rc->sql);
	return rc;
}

/* buckets whose entries were already freed one by one */
static void
hash_drop_freed_entries(PweHash *h)
{
	free(h->buckets);
	h->buckets = NULL;
	h->nbuckets = 0;
}

/* free every PweRelInit stored in a hash and the (already freed) buckets */
static void
free_rel_init_hash(PweHash *h)
{
	size_t		i;

	for (i = 0; i < h->nbuckets; i++)
	{
		PweHashEntry *e = h->buckets[i];

		while (e)
		{
			PweHashEntry *n = e->next;
			PweRelInit *ri = e->val;
			int			j;

			if (ri)
			{
				for (j = 0; j <= ri->rel.natts; j++)
					free(ri->attrs ? ri->attrs[j] : NULL);
				free(ri->attrs);
				free(ri->pk);
				free(ri);
			}
			free(e);
			e = n;
		}
	}
	hash_drop_freed_entries(h);
}

void
pwe_recon_free(PweReconstructor *rc)
{
	if (!rc)
		return;
	{
		size_t		i;

		for (i = 0; i < rc->txns.nbuckets; i++)
		{
			PweHashEntry *e = rc->txns.buckets[i];

			while (e)
			{
				PweHashEntry *n = e->next;

				free_txn(rc, e->val);
				free(e);
				e = n;
			}
		}
	}
	hash_drop_freed_entries(&rc->txns);
	pwe_hash_free(&rc->enum_created, NULL);
	pwe_hash_free(&rc->type_created, NULL);
	pwe_hash_free(&rc->schema_created, NULL);
	pwe_hash_free(&rc->unknown_rels, NULL);
	{
		size_t		i;

		for (i = 0; i < rc->copy_tables.nbuckets; i++)
		{
			PweHashEntry *e = rc->copy_tables.buckets[i];

			while (e)
			{
				PweHashEntry *n = e->next;
				PweCopyTable *ct = e->val;

				if (ct->f)
					fclose(ct->f);
				free(ct->qname);
				free(ct->collist);
				free(ct);
				e = n;
			}
		}
	}
	hash_drop_freed_entries(&rc->copy_tables);
	pwe_hash_free(&rc->seed_rels, NULL);
	pwe_hash_free(&rc->created_objs, NULL);
	pwe_hash_free(&rc->obj_names, NULL);
	pwe_hash_free(&rc->file_bases, NULL);
	pwe_hash_free(&rc->dropped_objs, NULL);
	pwe_hash_free(&rc->pre_indexes, NULL);
	pwe_hash_free(&rc->pre_cons, NULL);
	free_rel_init_hash(&rc->pre_rels);
	free_rel_init_hash(&rc->meta_attrs);
	pwe_catalog_destroy(rc->cat);
	pwe_str_free(&rc->tmp);
	pwe_str_free(&rc->sql);
	free(rc);
}

int
pwe_recon_version(PweReconstructor *rc)
{
	return rc->version;
}

void
pwe_recon_set_output(PweReconstructor *rc, FILE *out)
{
	if (out)
		rc->out = out;
}

PweCatalog *
pwe_recon_catalog(PweReconstructor *rc)
{
	return rc->cat;
}

void
pwe_recon_process(PweReconstructor *rc, PweRecord *rec)
{
	PweTxn	   *txn = NULL;

	if (!rc || !rec)
		return;
	if (rc->stats)
	{
		rc->stats->nrecords++;
		if (rec->rmid < 32)
			rc->stats->rmgr_count[rec->rmid]++;
		if (!rc->stats->has_first_lsn)
		{
			rc->stats->first_lsn = rec->lsn;
			rc->stats->has_first_lsn = true;
		}
		rc->stats->last_lsn = rec->lsn;
	}
	if (rc->opts.limit && rc->stats && rc->stats->nrecords > rc->opts.limit)
		return;

	if (rec->rmid == 1)			/* RM_XACT_ID */
	{
		uint8		op = rec->info & 0x70;

		txn = get_txn(rc, rec->xid, rec->lsn, false);
		if (op == 0x00 || op == 0x10 || op == 0x30)
		{
			if (txn)
			{
				/*
				 * xl_xact_commit starts with xact_time (microseconds since
				 * 2000-01-01).  Prepare records carry no such timestamp.
				 */
				txn->commit_lsn = rec->lsn;
				if ((op == 0x00 || op == 0x30) && rec->main_data &&
					rec->main_len >= 8)
				{
					txn->commit_time = (int64) pwe_read_u64(rec->main_data);
					txn->has_commit_time = true;
				}
				emit_transaction(rc, txn);
				pwe_hash_remove(&rc->txns, txn->xid);
				free_txn(rc, txn);
			}
		}
		else if (op == 0x20 || op == 0x40)
		{
			if (txn)
			{
				if (rc->opts.include_aborted && rc->opts.include_comments)
					fprintf(rc->out, "-- pg_walex: aborted transaction xid=%u\n", txn->xid);
				txn_rollback_rows(rc, txn);
				/* undo catalog changes in reverse order */
				{
					PweEvent   *list = txn->events;
					PweEvent   *rev = NULL;

					while (list)
					{
						PweEvent   *n = list->next;

						list->next = rev;
						rev = list;
						list = n;
					}
					txn->events = rev;
				}
				{
					PweEvent   *ev;

					for (ev = txn->events; ev; ev = ev->next)
						if (ev->cat)
							pwe_catalog_undo(rc->cat, ev->cat);
				}
				pwe_hash_remove(&rc->txns, txn->xid);
				free_txn(rc, txn);
			}
		}
		return;
	}
	if (rec->rmid == 10 || rec->rmid == 9)
	{
		txn = get_txn(rc, rec->xid, rec->lsn, true);
		process_heap_record(rc, rec, txn);
		if (txn && txn->bytes > 64ull * 1024 * 1024)
		{
			/* extremely large transaction: emit incrementally */
			emit_transaction(rc, txn);
			pwe_hash_remove(&rc->txns, txn->xid);
			free_txn(rc, txn);
		}
		return;
	}
	if (rec->rmid == 21)		/* RM_LOGICALMSG_ID */
	{
		process_logical_message(rc, rec);
		return;
	}
	if (rc->opts.include_non_sql)
	{
		char		lsn[32];

		pwe_format_lsn(lsn, sizeof(lsn), rec->lsn);
		fprintf(rc->out, "-- [%s] %s %s xid=%u len=%u\n",
				lsn, pwe_rmgr_name(rec->rmid),
				pwe_rmgr_info_name(rec->rmid, rec->info, rc->version),
				rec->xid, rec->tot_len);
	}
}

void
pwe_recon_finish(PweReconstructor *rc)
{
	size_t		i;

	if (!rc)
		return;
	/* report unfinished (crashed/truncated) transactions */
	for (i = 0; i < rc->txns.nbuckets; i++)
	{
		PweHashEntry *e = rc->txns.buckets[i];

		while (e)
		{
			PweHashEntry *n = e->next;
			PweTxn	   *txn = e->val;

			if (txn->events && rc->opts.include_comments)
				fprintf(rc->out,
						"-- pg_walex: WARNING: transaction xid=%u has no commit/abort record in the read range\n",
						txn->xid);
			free_txn(rc, txn);
			free(e);
			e = n;
		}
		rc->txns.buckets[i] = NULL;
	}
	rc->txns.count = 0;
	/* --tocopy: close trailing data segments and emit their \copy lines */
	if (rc->opts.tocopy)
	{
		copy_flush_segments(rc);
		fprintf(rc->out, "-- pg_walex: end of COPY output\n");
	}
	fflush(rc->out);
}
