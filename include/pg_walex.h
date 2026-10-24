/*
 * pg_walex - PostgreSQL WAL deep reader / SQL reconstructor
 *
 * Reads PostgreSQL 11..18 WAL segment files directly (no server, no catalog
 * files), reconstructs catalog state and transactions from WAL, and renders
 * the changes as portable SQL text.  Also provides a small RESTful API.
 */
#ifndef PG_WALEX_H
#define PG_WALEX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PostgreSQL-style fixed width type names */
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;

#define PWE_VERSION        "1.0.0"
#define PWE_NAMEDATALEN    64
#define PWE_MAX_BLOCK_ID   32
#define PWE_BLCKSZ_DEFAULT 8192
#define PWE_SEGSIZE_DEFAULT (16u * 1024u * 1024u)
#define PWE_FIRST_NORMAL_OID 16384u

#define PWE_INVALID_XID 0u

/* ------------------------------------------------------------------ */
/* per-version constants                                              */
/* ------------------------------------------------------------------ */
typedef struct PweVersionInfo
{
	int			major;			/* 11 .. 18 */
	uint16		page_magic;
	bool		catalog_has_oid;	/* explicit oid attribute in catalogs */
} PweVersionInfo;

const PweVersionInfo *pwe_version_by_major(int major);
const PweVersionInfo *pwe_version_by_magic(uint16 magic);
const char *pwe_rmgr_name(uint8 rmid);
const char *pwe_rmgr_info_name(uint8 rmid, uint8 info, int major);

/* ------------------------------------------------------------------ */
/* dynamic string                                                     */
/* ------------------------------------------------------------------ */
typedef struct PweStr
{
	char	   *data;
	size_t		len;
	size_t		cap;
} PweStr;

void		pwe_str_init(PweStr *s);
void		pwe_str_free(PweStr *s);
void		pwe_str_reset(PweStr *s);
void		pwe_str_add(PweStr *s, const char *data, size_t len);
void		pwe_str_addc(PweStr *s, char c);
void		pwe_str_adds(PweStr *s, const char *str);
void		pwe_str_addf(PweStr *s, const char *fmt,...) __attribute__((format(printf, 2, 3)));
void		pwe_str_add_quoted(PweStr *s, const char *val, size_t len);	/* SQL literal */
void		pwe_str_add_ident(PweStr *s, const char *ident);	/* SQL identifier */
void		pwe_str_add_json(PweStr *s, const char *val, size_t len);
const char *pwe_str_cstr(PweStr *s);

/* ------------------------------------------------------------------ */
/* hash map: uint64 -> void*                                          */
/* ------------------------------------------------------------------ */
typedef struct PweHashEntry
{
	uint64		key;
	void	   *val;
	struct PweHashEntry *next;
} PweHashEntry;

typedef struct PweHash
{
	PweHashEntry **buckets;
	size_t		nbuckets;
	size_t		count;
} PweHash;

void		pwe_hash_init(PweHash *h);
void		pwe_hash_free(PweHash *h, void (*freeval) (void *));
/* returns previous value or NULL */
void	   *pwe_hash_put(PweHash *h, uint64 key, void *val);
void	   *pwe_hash_get(PweHash *h, uint64 key);
void	   *pwe_hash_remove(PweHash *h, uint64 key);
void		pwe_hash_iterate(PweHash *h, void (*cb) (uint64 key, void *val, void *arg), void *arg);

/* ------------------------------------------------------------------ */
/* misc utils                                                         */
/* ------------------------------------------------------------------ */
void	   *pwe_malloc(size_t size);
void	   *pwe_calloc(size_t n, size_t size);
void	   *pwe_realloc(void *p, size_t size);
char	   *pwe_strdup(const char *s);
char	   *pwe_strndup(const char *s, size_t n);
uint32		pwe_crc32c(uint32 crc, const void *data, size_t len);
bool		pwe_pglz_decompress(const uint8 *src, size_t srclen, uint8 *dst, size_t dstlen);
bool		pwe_lz4_available(void);
bool		pwe_zstd_available(void);
bool		pwe_lz4_decompress(const uint8 *src, size_t srclen, uint8 *dst, size_t dstlen);
bool		pwe_zstd_decompress(const uint8 *src, size_t srclen, uint8 *dst, size_t dstlen);
void		pwe_format_lsn(char *buf, size_t buflen, uint64 lsn);
bool		pwe_parse_lsn(const char *str, uint64 *lsn);
void		pwe_write_u16(uint8 *p, uint16 v);
uint32		pwe_align8(uint32 x);
uint64		pwe_align8_64(uint64 x);
char	   *pwe_hex_encode(const uint8 *data, size_t len);
int			pwe_hexval(char c);
bool		pwe_parse_u64(const char *s, uint64 *out);
bool		pwe_parse_int(const char *s, int64 *out);
void		pwe_trim(char *s);
bool		pwe_guess_pgdata(const char *wal_path, char *out, size_t outlen);

/* ------------------------------------------------------------------ */
/* WAL reader                                                         */
/* ------------------------------------------------------------------ */
typedef struct PweBlock
{
	bool		in_use;
	bool		has_image;
	bool		has_data;
	bool		will_init;
	bool		apply_image;
	uint8		forknum;
	uint8		bimg_info;
	uint32		spc;
	uint32		db;
	uint32		rel;
	uint32		blkno;
	uint32		data_len;
	uint32		image_len;
	uint16		hole_offset;
	uint16		hole_length;
	const uint8 *data;			/* payload, inside record buffer */
	const uint8 *image;			/* compressed image bytes */
} PweBlock;

typedef struct PweRecord
{
	uint64		lsn;
	uint64		next_lsn;
	uint64		prev_lsn;
	uint32		xid;
	uint8		info;
	uint8		rmid;
	uint32		origin;
	uint32		toplevel_xid;
	uint32		tot_len;
	int			nblocks;
	PweBlock	blocks[PWE_MAX_BLOCK_ID + 1];
	const uint8 *main_data;
	uint32		main_len;
	uint8	   *buf;				/* assembled whole record */
	uint32		buf_len;
} PweRecord;

typedef struct PweReader PweReader;

typedef struct PweReaderOpts
{
	const char *wal_dir;
	const char *wal_file;
	int			version;		/* 0 = auto detect */
	int			timeline;		/* 0 = auto (highest) */
	uint64		start_lsn;		/* 0 = beginning */
	uint64		end_lsn;		/* 0 = until end */
	uint32		blocksize;
	uint32		segsize;
	bool		verify_crc;
} PweReaderOpts;

PweReader  *pwe_reader_open(const PweReaderOpts *opts, char *err, size_t errlen);
/* 1 = record, 0 = end of WAL, -1 = error */
int			pwe_reader_next(PweReader *r, PweRecord *rec, char *err, size_t errlen);
int			pwe_reader_version(PweReader *r);
uint32		pwe_reader_blocksize(PweReader *r);
uint32		pwe_reader_segsize(PweReader *r);
uint64		pwe_reader_position(PweReader *r);
bool		pwe_reader_version_mismatch(PweReader *r);
void		pwe_reader_close(PweReader *r);

/* helper: list WAL segment files in a directory (sorted) */
typedef struct PweWalFile
{
	char	   *path;
	char	   *name;
	uint32		tli;
	uint32		logid;
	uint32		segindex;
	uint64		segno;			/* linear segment number (after segsize is known) */
	uint64		size;
} PweWalFile;

int			pwe_list_wal_files(const char *dir, PweWalFile **files, int *nfiles,
							 char *err, size_t errlen);
void		pwe_free_wal_files(PweWalFile *files, int nfiles);

/* ------------------------------------------------------------------ */
/* catalog / SQL reconstruction                                       */
/* ------------------------------------------------------------------ */
typedef struct PweCatalog PweCatalog;
typedef struct PweEvent PweEvent;
typedef struct PweTxn PweTxn;

typedef struct PweSqlOptions
{
	bool		include_comments;
	bool		include_non_sql;
	bool		catalog_sql;
	bool		include_aborted;
	bool		logical_messages;
	uint64		start_lsn;		/* only emit changes at/after this LSN */
	const char *pgdata;			/* offline data directory used to seed the catalog */
	uint32		only_database;	/* restrict to a database OID (0 = all) */
	bool		emit_connect;	/* emit \connect for multi-database WAL */
	uint32		max_row_cache;	/* rows kept for ctid->row reconstruction */
	uint32		limit;
	const char *only_table;		/* filter: schema.table or table */
	uint32		only_xid;
	bool		tocopy;			/* emit COPY-format data files + manifest */
	const char *output_dir;		/* directory for --tocopy output */
	uint32		batch_rows;		/* merge INSERTs into multi-VALUES (0 = off) */
} PweSqlOptions;

typedef struct PweStats
{
	uint64		nrecords;
	uint64		nsql;
	uint64		ntxns;
	uint64		ninsert;
	uint64		nupdate;
	uint64		ndelete;
	uint64		ntruncate;
	uint64		nddl;
	uint64		nunknown;
	uint64		rmgr_count[32];
	uint64		first_lsn;
	uint64		last_lsn;
	bool		has_first_lsn;
} PweStats;

typedef struct PweReconstructor PweReconstructor;

PweReconstructor *pwe_recon_create(int version, const PweSqlOptions *opts,
								   FILE *out, PweStats *stats);
void		pwe_recon_free(PweReconstructor *rc);
/* feed one record; may write SQL to output */
void		pwe_recon_process(PweReconstructor *rc, PweRecord *rec);
/* flush unfinished transactions as comments */
void		pwe_recon_finish(PweReconstructor *rc);
int			pwe_recon_version(PweReconstructor *rc);

/* JSON/text description of one record (for json/text output modes). */
void		pwe_record_describe(PweRecord *rec, int version, PweStr *out, bool with_sql);
void		pwe_record_describe_text(PweRecord *rec, int version, PweStr *out);

/* ------------------------------------------------------------------ */
/* HTTP server                                                        */
/* ------------------------------------------------------------------ */
typedef struct PweHttpOptions
{
	const char *listen;
	int			port;
	const char *token;
	const char *default_wal_dir;
	int			default_version;
} PweHttpOptions;

int			pwe_http_serve(const PweHttpOptions *opts);

#endif							/* PG_WALEX_H */
