/*
 * pg_walex - internal shared declarations (not installed)
 */
#ifndef PWE_INTERNAL_H
#define PWE_INTERNAL_H

#include "pg_walex.h"

/* catalog ids, must match pwe_version.c */
enum
{
	PWE_CAT_NONE = -1,
	PWE_CAT_PG_CLASS = 0,
	PWE_CAT_PG_ATTRIBUTE,
	PWE_CAT_PG_NAMESPACE,
	PWE_CAT_PG_TYPE,
	PWE_CAT_PG_INDEX,
	PWE_CAT_PG_CONSTRAINT,
	PWE_CAT_PG_ENUM,
	PWE_CAT_PG_SEQUENCE,
	PWE_CAT_PG_DATABASE,
	PWE_CAT_PG_AUTHID,
	PWE_CAT_PG_TABLESPACE,
	PWE_CAT_PG_ATTRDEF,
	PWE_CAT_COUNT
};

int			pwe_cat_lookup_oid(uint32 reloid);
const char *pwe_cat_name(int cat);
uint32		pwe_cat_oid(int cat);
int			pwe_cat_col(int cat, int major, int field);
const char *pwe_cat_typecodes(int cat, int major);
int			pwe_cat_natts(int cat, int major);
int			pwe_cat_oid_col(int cat, int major);

/* field indexes for pwe_cat_col() */
enum
{
	/* pg_class */
	REL_NAME = 0, REL_NSP, REL_TYPE, REL_KIND, REL_NATTS, REL_TOAST,
	REL_PERSIST, REL_FILENODE, REL_ISPART, REL_REWRITE, REL_ACL,
	REL_OPTIONS, REL_PARTBOUND, REL_AM, REL_TABLESPACE, REL_OWNER,
	/* pg_attribute */
	ATT_RELID = 0, ATT_NAME, ATT_TYPID, ATT_LEN, ATT_NUM, ATT_TYPMOD,
	ATT_NDIMS, ATT_BYVAL, ATT_ALIGN, ATT_NOTNULL, ATT_DROPPED, ATT_HASDEF,
	ATT_IDENTITY, ATT_GENERATED, ATT_COLLATION, ATT_ISLOCAL,
	/* pg_namespace */
	NS_NAME = 0, NS_OWNER, NS_ACL,
	/* pg_type */
	TY_NAME = 0, TY_NSP, TY_LEN, TY_BYVAL, TY_TYPE, TY_CATEGORY, TY_RELID,
	TY_ELEM, TY_ARRAY, TY_ALIGN, TY_STORAGE, TY_BASETYPE, TY_TYPMOD,
	TY_NDIMS, TY_COLLATION, TY_NOTNULL, TY_DEFINED,
	/* pg_index */
	IDX_INDEXRELID = 0, IDX_INDRELID, IDX_NATTS, IDX_NKEYATTS, IDX_UNIQUE,
	IDX_PRIMARY, IDX_KEY, IDX_COLLATION, IDX_CLASS, IDX_OPTION, IDX_EXPRS,
	IDX_PRED,
	/* pg_constraint */
	CON_NAME = 0, CON_NSP, CON_TYPE, CON_DEFERRABLE, CON_DEFERRED,
	CON_VALIDATED, CON_RELID, CON_TYPID, CON_INDID, CON_FRELID, CON_KEY,
	CON_FKEY, CON_BIN, CON_SRC,
	/* pg_enum */
	ENUM_TYPID = 0, ENUM_SORT, ENUM_LABEL,
	/* pg_sequence */
	SEQ_RELID = 0, SEQ_TYPID, SEQ_START, SEQ_INC, SEQ_MAX, SEQ_MIN,
	SEQ_CACHE, SEQ_CYCLE,
	/* pg_database */
	DB_NAME = 0, DB_DBA, DB_ENCODING, DB_ISTEMPLATE, DB_ALLOWCONN, DB_CONNLIMIT,
	/* pg_authid */
	ROLE_NAME = 0, ROLE_SUPER, ROLE_LOGIN,
	/* pg_tablespace */
	SPC_NAME = 0, SPC_OWNER,
	/* pg_attrdef */
	DEF_RELID = 0, DEF_NUM, DEF_BIN, DEF_SRC
};

/* built-in type fallback */
const void *pwe_builtin_type_lookup(uint32 oid, const char **name, int16 *typlen,
									bool *byval, char *align, char *typtype,
									uint32 *typelem, uint32 *typarray);
uint32		pwe_builtin_array_element(uint32 oid);

/* ------------------------------------------------------------------ */
/* catalog data model                                                 */
/* ------------------------------------------------------------------ */
typedef struct PweType
{
	uint32		oid;
	uint32		db;
	uint32		nsp;
	char		name[PWE_NAMEDATALEN];
	int16		typlen;
	int32		typtypmod;
	bool		byval;
	bool		notnull;
	bool		defined;
	char		align;
	char		typtype;
	char		category;
	uint32		relid;
	uint32		elem;
	uint32		array;
	uint32		basetype;
} PweType;

typedef struct PweAttr
{
	int16		num;
	char		name[PWE_NAMEDATALEN];
	uint32		typid;
	int16		len;
	int32		typmod;
	int16		ndims;
	char		align;
	bool		byval;
	bool		notnull;
	bool		dropped;
	bool		hasdef;
	char		identity;
	char		generated;
	uint32		collation;
} PweAttr;

struct PweHash;

typedef struct PweRel
{
	uint32		oid;
	uint32		db;
	uint32		nsp;
	char		name[PWE_NAMEDATALEN];
	char		nspname[PWE_NAMEDATALEN];
	char		relkind;
	char		persistence;
	uint32		filenode;
	uint32		toastrelid;
	uint32		reltype;
	uint32		owner;
	uint32		am;
	bool		ispartition;
	PweAttr   **attrs;			/* index = attnum, 1..natts */
	int			natts;
	int			livecols;
	struct PweHash *rows;		/* ctid -> PweRowVals */
	bool		rows_disabled;
	bool		toast_disk_loaded;
	int16	   *pk;				/* primary key attnums */
	int			npk;
} PweRel;

typedef struct PweRowVals
{
	int			natts;
	char	  **vals;			/* index 0..natts, NULL = null/unknown */
	char	  **copyvals;		/* COPY raw text per column (only with --tocopy) */
	uint8	   *raw;			/* xl_heap_header + tuple bytes (for prefix undo) */
	uint32		rawlen;
} PweRowVals;

typedef struct PweCon
{
	uint32		oid;
	uint32		db;
	uint32		relid;
	uint32		typid;
	uint32		indid;
	char		contype;
	char		name[PWE_NAMEDATALEN];
	int16	   *keys;
	int			nkeys;
	bool		deferrable;
	bool		deferred;
	bool		validated;
} PweCon;

typedef struct PweIndex
{
	uint32		oid;
	uint32		db;
	uint32		relid;
	int16		natts;
	int16		nkeyatts;
	bool		unique;
	bool		primary;
	bool		valid;
	bool		partial;
	uint32		am;
	char		name[PWE_NAMEDATALEN];
	int16		keys[32];
	int			nkeys;
} PweIndex;

typedef struct PweEnumVal
{
	uint32		oid;
	uint32		db;
	uint32		typid;
	float		sortorder;
	char		label[PWE_NAMEDATALEN];
} PweEnumVal;

typedef struct PweSeq
{
	uint32		relid;
	uint32		db;
	uint32		typid;
	int64		start;
	int64		inc;
	int64		maxv;
	int64		minv;
	int64		cache;
	bool		cycle;
} PweSeq;

typedef struct PweTypeInfo
{
	uint32		oid;
	uint32		nsp;
	char		name[PWE_NAMEDATALEN];
	int16		typlen;
	bool		byval;
	char		align;
	char		typtype;
	char		category;
	uint32		relid;
	uint32		elem;
	uint32		array;
	uint32		basetype;
	int32		typtypmod;
	bool		notnull;
	bool		found;
} PweTypeInfo;

/* ------------------------------------------------------------------ */
/* tuple decoding                                                     */
/* ------------------------------------------------------------------ */
typedef struct PweToastRef
{
	bool		valid;
	int32		rawsize;
	uint32		extinfo;		/* va_extinfo (PG14+) / va_extsize (PG11-13) */
	uint32		valueid;
	uint32		toastrelid;
} PweToastRef;

typedef struct PweValue
{
	bool		isnull;
	uint8		typecode;		/* catalog typecode used */
	uint32		typid;
	int16		typlen;
	bool		byval;
	char		align;
	uint64		u;				/* scalar value */
	const uint8 *data;			/* varlena payload (detoasted if possible) */
	int			len;
	uint8	   *owned;
	PweToastRef toast;
} PweValue;

typedef struct PweTuple
{
	int			natts;
	bool		has_null;
	const uint8 *nullbits;
	const uint8 *data;			/* start of attribute data */
	uint32		datalen;
	uint16		infomask;
	uint16		infomask2;
	bool		has_oid;
	uint32		oid;
} PweTuple;

#define PWE_HEAP_HASNULL 0x0001
#define PWE_HEAP_HASVARWIDTH 0x0002
#define PWE_HEAP_HASEXTERNAL 0x0004
#define PWE_HEAP_HASOID_OLD 0x0008
#define PWE_HEAP_XMAX_KEYSHR_LOCK 0x0010
#define PWE_HEAP_COMBOCID 0x0020
#define PWE_HEAP_XMAX_EXCL_LOCK 0x0040
#define PWE_HEAP_XMAX_LOCK_ONLY 0x0080
#define PWE_HEAP_XMIN_COMMITTED 0x0100
#define PWE_HEAP_XMIN_INVALID 0x0200
#define PWE_HEAP_XMIN_FROZEN 0x0300
#define PWE_HEAP_XMAX_COMMITTED 0x0400
#define PWE_HEAP_XMAX_INVALID 0x0800
#define PWE_HEAP_XMAX_IS_MULTI 0x1000
#define PWE_HEAP_UPDATED 0x2000
#define PWE_HEAP_MOVED_IN 0x4000
#define PWE_HEAP_MOVED_OFF 0x8000
#define PWE_HEAP_HASOID 0x0008	/* t_infomask, PG11 only */

/* payload = xl_heap_header (5 bytes) followed by tuple body */
bool		pwe_tuple_parse(const uint8 *payload, uint32 payload_len, PweTuple *t,
							char *err, size_t errlen);
/* decode all attributes described by typecodes (per catalog relation) */
bool		pwe_tuple_decode_catalog(const PweTuple *t, const char *typecodes,
									 int natts, PweValue **vals,
									 char *err, size_t errlen);
void		pwe_values_free(PweValue *vals, int natts);
void		pwe_value_free(PweValue *v);

/* user table row decode; attrs array index 1..nattrs */
bool		pwe_tuple_decode_row(const PweTuple *t, PweAttr **attrs, int nattrs,
								 PweValue **vals, char *err, size_t errlen);

/* resolve an external/compressed datum; detoast callback may be NULL */
typedef bool (*PweDetoastFn) (void *arg, PweValue *v);
void		pwe_value_resolve_inline(PweValue *v);

/* scalar helpers */
uint16		pwe_read_u16(const uint8 *p);
uint32		pwe_read_u32(const uint8 *p);
uint64		pwe_read_u64(const uint8 *p);
int16		pwe_read_i16(const uint8 *p);
int32		pwe_read_i32(const uint8 *p);
int64		pwe_read_i64(const uint8 *p);

/* literal rendering */
void		pwe_value_literal(struct PweCatalog *cat, uint32 db, PweValue *v,
							  const PweTypeInfo *ti, PweStr *out, int version);
/* restore the pre-bulk-output array rendering for the plain SQL stream */
void		pwe_value_set_legacy_arrays(bool on);
/* COPY text-format rendering (raw, unescaped); false = not renderable */
bool		pwe_value_copy(struct PweCatalog *cat, uint32 db, PweValue *v,
						   const PweTypeInfo *ti, PweStr *out);
void		pwe_type_display_name(const PweTypeInfo *ti, const char *nspname,
								  PweStr *out, bool qualified);
void		pwe_format_typmod(const PweTypeInfo *ti, int32 typmod, PweStr *out);

/* ------------------------------------------------------------------ */
/* recon internals                                                    */
/* ------------------------------------------------------------------ */
typedef struct PweRelSnap
{
	uint32		oid, db, nsp, typid, filenode, toastrelid, am;
	int16		natts;
	char		relkind;
	char		persistence;
	char		name[PWE_NAMEDATALEN];
} PweRelSnap;

typedef struct PweAttrSnap
{
	uint32		db;
	uint32		relid, typid, collation;
	int16		num, len;
	int32		typmod;
	char		align;
	bool		byval, notnull, dropped, hasdef;
	char		identity, generated;
	char		name[PWE_NAMEDATALEN];
} PweAttrSnap;

typedef struct PweIndexSnap
{
	uint32		oid, db, relid;
	int16		natts, nkeyatts;
	bool		unique, primary, valid;
	bool		partial;
	uint32		am;
	char		name[PWE_NAMEDATALEN];
	int16		keys[32];
	int			nkeys;
} PweIndexSnap;

typedef struct PweConSnap
{
	uint32		oid, db, relid, typid, indid;
	char		contype;
	char		name[PWE_NAMEDATALEN];
	int16		keys[32];
	int			nkeys;
	bool		deferrable, deferred, validated;
} PweConSnap;

typedef struct PweTypeSnap
{
	uint32		oid, db, nsp, relid, elem, array, basetype;
	int16		typlen;
	int32		typtypmod;
	char		name[PWE_NAMEDATALEN];
	char		typtype;
	char		align;
	bool		byval, notnull, defined;
} PweTypeSnap;

typedef struct PweNsSnap
{
	uint32		oid, db;
	char		name[PWE_NAMEDATALEN];
} PweNsSnap;

typedef struct PweEnumSnap
{
	uint32		oid, db, typid;
	float		sortorder;
	char		label[PWE_NAMEDATALEN];
} PweEnumSnap;

typedef struct PweSeqSnap
{
	uint32		relid, db, typid;
	int64		start, inc, maxv, minv, cache;
	bool		cycle;
} PweSeqSnap;

typedef struct PweCatalog PweCatalog;

/* events */
enum
{
	PWE_EV_SQL = 0,
	PWE_EV_CAT,
	PWE_EV_COPYDATA				/* escaped COPY data line (--tocopy) */
};

enum
{
	PWE_OP_INSERT = 0,
	PWE_OP_UPDATE,
	PWE_OP_DELETE
};

/* statement classification used by the SQL comment header */
enum
{
	PWE_STMT_UNKNOWN = 0,
	PWE_STMT_INSERT,
	PWE_STMT_UPDATE,
	PWE_STMT_DELETE,
	PWE_STMT_TRUNCATE,
	PWE_STMT_CREATE,
	PWE_STMT_ALTER,
	PWE_STMT_DROP,
	PWE_STMT_ROLLBACK
};

typedef struct PweCatPayload
{
	int			cat;
	int			op;
	union
	{
		PweRelSnap rel;
		PweAttrSnap attr;
		PweIndexSnap idx;
		PweConSnap con;
		PweTypeSnap typ;
		PweNsSnap ns;
		PweEnumSnap en;
		PweSeqSnap seq;
	}			n;
	union
	{
		PweRelSnap rel;
		PweAttrSnap attr;
		PweIndexSnap idx;
		PweConSnap con;
		PweTypeSnap typ;
		PweNsSnap ns;
		PweEnumSnap en;
		PweSeqSnap seq;
	}			o;
	void	   *saved_rel;		/* detached PweRel kept for abort undo */
	uint8	   *raw;			/* raw tuple payload (5-byte header + data) */
	uint32		rawlen;
} PweCatPayload;

struct PweEvent
{
	struct PweEvent *next;
	uint64		lsn;
	uint32		xid;
	uint8		kind;
	uint8		stmt;			/* PWE_STMT_* classification */
	uint16		nfields;		/* PWE_EV_COPYDATA: field count of the line */
	uint64		relkey;			/* PWE_EV_COPYDATA: (db<<32)|reloid */
	char	   *collist;		/* PWE_EV_COPYDATA: "(c1, c2)" at decode time */
	char	   *sql;			/* PWE_EV_SQL / PWE_EV_COPYDATA */
	PweCatPayload *cat;			/* PWE_EV_CAT */
};

struct PweTxn
{
	uint32		xid;
	uint32		db;
	uint64		first_lsn;
	uint64		commit_lsn;		/* LSN of the commit/abort record */
	int64		commit_time;	/* commit timestamp (us since 2000-01-01) */
	bool		has_commit_time;
	PweEvent  *events;
	PweEvent  *tail;
	uint64		bytes;
	struct PweUndo *undo;		/* row cache undo, reverse order */
	bool		too_big;
};

typedef struct PweUndo
{
	struct PweUndo *next;
	uint64		relkey;
	uint64		ctid;
	PweRowVals *oldrow;			/* NULL = row did not exist */
} PweUndo;

struct PweCatalog
{
	int			version;
	PweHash		rels_by_oid;	/* key (db<<32|oid) -> PweRel* */
	PweHash		rels_by_file;	/* key (db<<32|filenode) -> PweRel* */
	PweHash		types;			/* key (db<<32|oid) -> PweType* */
	PweHash		namespaces;		/* key (db<<32|oid) -> char* */
	PweHash		dbs;			/* key oid -> char* */
	PweHash		roles;			/* key oid -> char* */
	PweHash		indexes;		/* key (db<<32|oid) -> PweIndex* */
	PweHash		constraints;	/* key (db<<32|oid) -> PweCon* */
	PweHash		enums;			/* key (db<<32|oid) -> PweEnumVal* */
	PweHash		enums_by_type;	/* key (db<<32|typid) -> PweEnumVal* list */
	PweHash		sequences;		/* key (db<<32|relid) -> PweSeq* */
	PweHash		pending_attrs;	/* key (db<<32|relid) -> PweHash* (attnum->PweAttr) */
	PweHash		toast_chunks;	/* key hash(db,toastrelid,valueid) -> PweToastChunk* */
	PweHash		cat_rows;		/* (db,catreloid) -> PweHash* ctid -> PweCatPayload* */
	uint64		toast_bytes;
	uint64		toast_limit;
	uint64		row_count;
	uint64		row_limit;
	bool		row_limit_hit;
	uint64		relations_known;
	bool		catalog_incomplete;
	char	   *pgdata;			/* optional offline data directory */
	int			xact_fd;
	uint32		xact_seg;
	bool		xact_ok;
	uint8		xact_buf[8192];
};

/* toast chunk */
typedef struct PweToastChunk
{
	struct PweToastChunk *next;
	int32		seq;
	uint32		len;
	uint8	   *data;
} PweToastChunk;

typedef struct PweToastEntry
{
	uint32		db, relid, valueid;
	PweToastChunk *chunks;
	struct PweToastEntry *hnext;
} PweToastEntry;

/* catalog API (pwe_catalog.c) */
PweCatalog *pwe_catalog_create(int version, uint64 row_limit, uint64 toast_limit);
void		pwe_catalog_destroy(PweCatalog *cat);
PweRel	   *pwe_catalog_rel_by_oid(PweCatalog *cat, uint32 db, uint32 oid);
PweRel	   *pwe_catalog_rel_by_file(PweCatalog *cat, uint32 db, uint32 filenode);
PweTypeInfo pwe_catalog_type_info(PweCatalog *cat, uint32 db, uint32 typid);
const char *pwe_catalog_ns_name(PweCatalog *cat, uint32 db, uint32 oid);
const char *pwe_catalog_db_name(PweCatalog *cat, uint32 oid);
const char *pwe_catalog_role_name(PweCatalog *cat, uint32 oid);
PweIndex   *pwe_catalog_index(PweCatalog *cat, uint32 db, uint32 oid);
PweCon	   *pwe_catalog_constraint(PweCatalog *cat, uint32 db, uint32 oid);
PweSeq	   *pwe_catalog_sequence(PweCatalog *cat, uint32 db, uint32 relid);
const char *pwe_catalog_enum_label(PweCatalog *cat, uint32 db, uint32 valueoid);
PweRowVals *pwe_row_get(PweRel *rel, uint32 blkno, uint16 offnum);
void		pwe_row_set(PweRel *rel, uint32 blkno, uint16 offnum, PweRowVals *row);
PweRowVals *pwe_row_remove(PweRel *rel, uint32 blkno, uint16 offnum);
PweRowVals *pwe_rowvals_create(int natts);
void		pwe_rowvals_free(PweRowVals *r);
void		pwe_rel_clear_rows(PweRel *rel);
bool		pwe_catalog_detoast(PweCatalog *cat, uint32 db, PweValue *v);
void		pwe_catalog_add_toast_chunk(PweCatalog *cat, uint32 db, uint32 relid,
									   uint32 valueid, int32 seq,
									   const uint8 *data, uint32 len);
/* offline catalog seeding */
void		pwe_catalog_seed_from_pgdata(PweCatalog *cat, const char *pgdata,
									 uint32 only_db);
bool		pwe_catalog_tuple_live(PweCatalog *cat, const uint8 *tuple,
								   uint16 infomask);
/* catalog row maintenance */
void		pwe_catalog_process_tuple(PweCatalog *cat, PweRecord *rec, PweTxn *txn,
									  uint32 db, uint32 relfilenode,
									  uint32 blkno, uint16 offnum,
									  uint32 old_blkno, uint16 old_offnum,
									  const uint8 *payload, uint32 len,
									  bool is_delete, bool fill_missing);
void		pwe_catalog_undo(PweCatalog *cat, PweCatPayload *p);
void		pwe_catalog_payload_cleanup(PweCatalog *cat, PweCatPayload *p);
bool		pwe_reconstruct_update(const uint8 *old_raw, uint32 old_rawlen,
									const uint8 *data, uint32 datalen, uint8 flags,
									uint8 **out, uint32 *outlen);
bool		pwe_catalog_reconstruct_update(PweCatalog *cat, uint32 db, uint32 relfilenode,
										   uint32 old_blkno, uint16 old_offnum,
										   const uint8 *data, uint32 datalen,
										   uint8 flags, uint8 **out, uint32 *outlen);
PweCatalog *pwe_recon_catalog(struct PweReconstructor *rc);
/* DDL rendering at commit */
void		pwe_ddl_emit(PweCatalog *cat, PweTxn *txn, FILE *out,
						 const PweSqlOptions *opts, PweStats *stats,
						 bool *first_stmt);

#endif							/* PWE_INTERNAL_H */
