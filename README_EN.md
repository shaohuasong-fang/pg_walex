[中文](./README.md) | [English](./README_EN.md)
# pg_walex

**PostgreSQL WAL Extractor** — an offline, dependency-free PostgreSQL write-ahead log reading tool that can reconstruct physical WAL records back into portable SQL.

`pg_walex` reads WAL segment files directly. It never starts a database server, does not link against `libpq`, and does not connect to any running instance. By rebuilding system catalog state while parsing physical records, it reconstructs heap table changes into executable, generic SQL text, and can also expose the same results through a built-in RESTful API.

Supported PostgreSQL major versions: **11 / 12 / 13 / 14 / 15 / 16 / 17 / 18**.

Typical use cases:

- Auditing and forensic analysis: what changes actually occurred in the database;
- Recovering DML/DDL from a crashed, deleted, or unbootable instance;
- Replicating changes to another system without logical replication or an online connection;
- Feeding WAL contents into external pipelines as SQL or JSON.

---

## 1. Features

- **Direct WAL access**: parses segment files, page headers, cross-page continuations (`contrecord`), record headers, block references, and main data.
- **Complete record parsing**: CRC32C verification; full-page images (FPI) compressed with PGLZ / LZ4 / Zstandard are decompressed.
- **System catalog reconstruction**: rebuilds object names, column names, types, primary keys, unique constraints, and enums from `pg_class`, `pg_attribute`, `pg_namespace`, `pg_type`, `pg_index`, `pg_constraint`, `pg_enum`, `pg_sequence`, `pg_database`, `pg_authid`, and `pg_attrdef` changes seen in WAL; catalog tuples can also be salvaged from FPI pages.
- **TOAST restoration**: collects and decompresses toast data chunks (PGLZ/LZ4/ZSTD). Chunks outside the WAL window are read precisely by valueid from the TOAST relation files in the data directory, so large fields (jsonb, text, etc.) can be fully recovered.
- **Offline catalog warm-up**: when the WAL directory is `<pgdata>/pg_wal` (or `<pgdata>/pgdata/pg_wal`), the tool reads the adjacent `base/`, `global/`, and `pg_xact/` to determine tuple visibility offline, obtaining complete table and column metadata without starting the server. This is why `INSERT`/`UPDATE` statements still carry complete column lists and values even when only the most recent WAL window is retained. You can explicitly specify the data directory with `--pgdata PATH`, or disable this behavior with `--no-pgdata`.
- **SQL reconstruction**:
  - DML: statement reconstruction
  - DDL: statement reconstruction
  - Transactions: statements are grouped by xid and wrapped in `BEGIN/COMMIT`
  - Statement comments: each transaction is preceded by `-- pg_walex: type=...,xid=...,lsn=...,timestamp=...`.
  - Inverse operations: `--rollback` appends a replayable inverse statement after each `UPDATE`/`DELETE`; generally only effective with `wal_level = logical`.
- **Value rendering**: supports rendering of database-internal data types; types that cannot be rendered are output as `NULL` with a comment, ensuring the script is executable.
- **Multi-database**: `--db-oid` outputs only one database; otherwise output includes psql `\connect` statements per database.
- **Warm-up**: when using `--start-lsn`, the tool first silently replays the earliest available WAL to rebuild catalogs and row caches, and only outputs SQL after reaching the specified LSN, thereby ensuring correct table names, columns, and keys.
- **Prefix/suffix-compressed UPDATE**: the row cache retains the original tuple bytes, so compressed `UPDATE`s (`wal_level=replica`) are still expanded into full new rows and primary-key-targeted `UPDATE` statements.
- **Output formats**: `sql` / `json` (one record per line) / `text` (similar to `pg_waldump`) / `stats`.
- **Bulk-load output**: `--tocopy` writes COPY-format data files plus a `manifest.txt` replay script (one `<schema>.<table>.meta.sql` / `.data.sql` per table); `--batch-inserts[=N]` merges consecutive INSERTs of the same table into multi-`VALUES` statements, making recovery of large WAL volumes orders of magnitude faster than row-by-row `INSERT`. Both options are additive: SQL output remains unchanged when they are not used.
- **RESTful API**: health check, WAL info, record listing, SQL reconstruction, statistics, and catalog queries, with optional Bearer Token authentication.
- **Real-time WAL monitoring**: after starting with `--serve --monitor`, a background thread continuously follows

---

## 2. Building

Requirements: a C11 compiler and a POSIX environment (macOS / Linux).
Optional: `liblz4`, `libzstd`.

```bash
cd pg_walex
make                 # auto-detect system lz4/zstd
# or explicitly disable: make LDFLAGS= LDLIBS=
```

If this directory was copied wholesale from another machine (e.g., macOS arm64), it will contain leftover `src/*.o` files from the other platform, and linking will fail with `file not recognized: File format not recognized`. Simply clean before building:

```bash
make clean && make
```

Build `pg_walex`. Install:

```bash
make install PREFIX=/usr/local
whereis pg_walex
pg_walex: /usr/local/bin/pg_walex
```

> FPI/TOAST data compressed with LZ4 or ZSTD requires the corresponding shared libraries; PGLZ is implemented internally and requires no external dependencies.

---

## 3. Command line

```bash
pg_walex --help
pg_walex 1.1.0 - PostgreSQL WAL Extractor
Read PostgreSQL 11..18 WAL segments directly (no server, no libpq),
reconstruct the changes as portable SQL text and expose a RESTful API.

Usage: pg_walex [options] <wal-directory>

Options:
  -d, --dir PATH          WAL directory (pg_wal)
  -f, --file PATH         read a single WAL segment file
      --pg-version N      force PostgreSQL major version (11..18)
  -s, --start-lsn LSN     start at LSN (X/Y, inclusive)
  -e, --end-lsn LSN       stop before LSN (X/Y)
  -t, --timeline N        timeline to read (default: highest)
  -b, --blocksize N       WAL block size (default: auto/8192)
  -S, --segsize N         WAL segment size (default: auto/16MB)
  -F, --format FMT        sql | json | text | stats (default: sql)
  -o, --output PATH       write output to FILE (with --tocopy: output directory)
      --tocopy            write COPY-format output: manifest.txt plus
                          <schema>.<table>.meta.sql and .data.sql files
                          (output directory from -o, default ./pg_walex_copy)
      --batch-inserts[=N] merge consecutive INSERTs of one table into a
                          single multi-VALUES statement (default N=100)
      --rollback          also emit the inverse statement for every
                          UPDATE/DELETE (commented as rollback)
  -l, --limit N           stop after N WAL records
      --xid N             only emit the given transaction
      --table NAME        only emit DML for NAME ([schema.]table)
      --include-non-sql   add comments for non-SQL WAL records
      --include-aborted   report aborted transactions
      --logical-messages  show pg_logical_emit_message payloads
      --no-comments       emit bare SQL without comments/BEGIN/COMMIT
      --no-crc            skip WAL record CRC verification
      --db-oid N          restrict output to one database OID
      --no-warmup         start scanning exactly at --start-lsn
      --pgdata PATH       seed catalog from an offline data directory
      --no-pgdata         do not auto-detect/read the data directory
      --no-connect        do not emit psql \connect between databases
      --max-rows N        row cache size for ctid->row tracking (default 200000)
      --serve             run the RESTful API server
      --monitor           with --serve: follow the WAL directory in the
                          background and expose the latest reconstructed
                          SQL statement at /api/v1/item
      --listen ADDR       listen address (default 127.0.0.1)
      --port N            listen port (default 8900)
      --token TOKEN       require Bearer TOKEN for API requests
  -v, --verbose           verbose messages on stderr
      --version           print version and exit
  -h, --help              show this help

Examples:
  pg_walex --pg-version version_num waldirectory > changes.sql
  pg_walex -F json --start-lsn 0/16B2C60 waldirectory
  pg_walex --serve --port 8900 --dir waldirectory
Comments:
 valid version_num is : 11,12,13,14,15,16,17,18
 waldirectory: <pgdata directory>/pg_wal
```
