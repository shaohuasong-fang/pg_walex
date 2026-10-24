
**Features**
> Does not start the database, does not connect to libpq, does not depend on logical replication, developed in pure C, directly reads WAL segment files, and completely parses everything from table creation to inserts, updates, deletes, and type changes in the database into executable SQL.

> A killer feature: row-by-row `INSERT` recovery for large amounts of data is very slow. pg_walex provides two "bulk load" outputs, `--tocopy` and `--batch-inserts`, which encapsulate single-row `INSERT` statements into structures that support the `COPY` command and `INSERT INTO table_name VALUES(?),(?)....` capabilities. Tens of millions of data can be loaded in seconds.

> As long as you have enough archives, full database recovery is a piece of cake, ensuring loading performance. Exporting only a specific database / a specific table / a specific transaction from numerous WAL files is not a problem.

## Download Link
```Bash
git clone https://github.com/shaohuasong-fang/pg_walex.git

```

---

## I. Why is there a need for "parsing WAL log files"

The scenarios DBA fear the most are usually not just "the database crashed", but also the following:
- The database can still start, but someone accidentally deleted data and the audit logs were not enabled;
- Recover DML/DDL from a crashed, deleted, or unbootable instance;
- Need to synchronize changes over a period of time to another environment, but do not want to use logical replication;
- Audit and forensic analysis: what changes actually happened in the database;
- Replicate changes to another system without logical replication or online connection;
- Send WAL contents in SQL or JSON format to external pipelines.

PostgreSQL's WAL is a physical log: pages, tuples, transactions, system catalogs, all are recorded in it in binary format. Reading it out and restoring it to SQL has a high technical threshold—you need to parse record headers, cross-page continuations, CRC32C, Full-Page Images (FPI), TOAST, system catalogs, transaction visibility, etc. So `pg_walex` (PostgreSQL WAL Extractor) does exactly this: pure offline, zero dependencies, single binary, a WAL log file parsing tool supporting PostgreSQL 11 to 18.

---

## II. Environment Preparation

```env
Database version: PostgreSQL 18
Database: db_app
wal_level: logical or replica
```

### 2.1 Using Simulated Data
Simulated data can be generated using `deepseek`
```SQL
CREATE DATABASE db_app;
\c db_app;
CREATE TABLE user_profile (
    id         bigserial PRIMARY KEY,
    username   text NOT NULL,
    age        int,
    bio        text,
    created_at timestamptz DEFAULT now()
);
db_app=# INSERT INTO user_profile (username, age, bio)
SELECT
    'user_' || g,
    (random() * 100)::int,
    substr(md5(g::text), 1,8)
FROM generate_series(1, 5) g;
INSERT 0 5
-- View data
db_app=# SELECT * FROM user_profile;
 id | username | age |   bio    |          created_at           
----+----------+-----+----------+-------------------------------
  1 | user_1   |   9 | c4ca4238 | 2026-10-24 20:58:58.490958+08
  2 | user_2   |  19 | c81e728d | 2026-10-24 20:58:58.490958+08
  3 | user_3   |  26 | eccbc87e | 2026-10-24 20:58:58.490958+08
  4 | user_4   |  23 | a87ff679 | 2026-10-24 20:58:58.490958+08
  5 | user_5   |  98 | e4da3b7f | 2026-10-24 20:58:58.490958+08
(5 rows)
```
### 2.2 Compiling the Tool
Because `pg_walex` is independently developed and has completely written the WAL format of PostgreSQL into the code, it does not need to depend on any PostgreSQL instance. After downloading the source code, you can directly `make` and `make install`.
- **Prerequisites**
```shell
Requirements: C11 compiler and POSIX environment (macOS / Linux).
Optional: `liblz4`, `libzstd`.
```
- **Compile Source Code**
```bash
[root@PGServer pg_walex]# make -j$nproc
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc -DPWE_WITH_LZ4 -I/opt/homebrew/include -I/usr/local/include -DPWE_WITH_ZSTD -I/opt/homebrew/include -I/usr/local/include -c -o src/pwe_util.o src/pwe_util.c
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc -DPWE_WITH_LZ4 -I/opt/homebrew/include -I/usr/local/include -DPWE_WITH_ZSTD -I/opt/homebrew/include -I/usr/local/include -c -o src/pwe_version.o src/pwe_version.c
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc -DPWE_WITH_LZ4 -I/opt/homebrew/include -I/usr/local/include -DPWE_WITH_ZSTD -I/opt/homebrew/include -I/usr/local/include -c -o src/pwe_wal.o src/pwe_wal.c
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc -DPWE_WITH_LZ4 -I/opt/homebrew/include -I/usr/local/include -DPWE_WITH_ZSTD -I/opt/homebrew/include -I/usr/local/include -c -o src/pwe_tuple.o src/pwe_tuple.c
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc -DPWE_WITH_LZ4 -I/opt/homebrew/include -I/usr/local/include -DPWE_WITH_ZSTD -I/opt/homebrew/include -I/usr/local/include -c -o src/pwe_catalog.o src/pwe_catalog.c
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc -DPWE_WITH_LZ4 -I/opt/homebrew/include -I/usr/local/include -DPWE_WITH_ZSTD -I/opt/homebrew/include -I/usr/local/include -c -o src/pwe_recon.o src/pwe_recon.c
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc -DPWE_WITH_LZ4 -I/opt/homebrew/include -I/usr/local/include -DPWE_WITH_ZSTD -I/opt/homebrew/include -I/usr/local/include -c -o src/pwe_output.o src/pwe_output.c
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc -DPWE_WITH_LZ4 -I/opt/homebrew/include -I/usr/local/include -DPWE_WITH_ZSTD -I/opt/homebrew/include -I/usr/local/include -c -o src/pwe_main.o src/pwe_main.c
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc -DPWE_WITH_LZ4 -I/opt/homebrew/include -I/usr/local/include -DPWE_WITH_ZSTD -I/opt/homebrew/include -I/usr/local/include -c -o src/pwe_http.o src/pwe_http.c
cc  -L/opt/homebrew/lib -L/usr/local/lib -L/opt/homebrew/lib -L/usr/local/lib -o pg_walex src/pwe_util.o src/pwe_version.o src/pwe_wal.o src/pwe_tuple.o src/pwe_catalog.o src/pwe_recon.o src/pwe_output.o src/pwe_main.o src/pwe_http.o  -llz4 -lzstd
```
- **Install**
```Bash
[root@PGServer pg_walex]# make install PREFIX=/usr/local
install -d /usr/local/bin
install -m 0755 pg_walex /usr/local/bin/pg_walex
```
-- **Check Version**
```bash
[root@PGServer pg_walex]# ./pg_walex --version
pg_walex 1.0.0
```

## III. Verify Parsing
### 3.1 Check the current log LSN and its corresponding file

```SQL
[root@PGServer pg_walex]# psql -U postgres -d db_app -p6432
psql (18.4)
Type "help" for help.

db_app=# select pg_current_wal_lsn();
 pg_current_wal_lsn 
--------------------
 0/28009BE8
(1 row)

db_app=# SELECT pg_walfile_name(pg_current_wal_lsn());
     pg_walfile_name      
--------------------------
 000000010000000000000028
(1 row)
```
### 3.2 Modify data and check log sequence number
```SQL
db_app=# update user_profile set bio = 'updated by me' where id = 1;
UPDATE 1
db_app=# select pg_current_wal_lsn();
 pg_current_wal_lsn 
--------------------
 0/28009E80
(1 row)
```
### 3.3 Delete data and check log sequence number
```SQL
db_app=# delete from user_profile where id = 3;
DELETE 1
db_app=# select pg_current_wal_lsn();
 pg_current_wal_lsn 
--------------------
 0/2800A210
(1 row)
```
### 3.4 Check the log file corresponding to the log sequence number
```SQL
-- Log file corresponding to the updated LSN
db_app=# SELECT pg_walfile_name('0/28009E80');
     pg_walfile_name      
--------------------------
 000000010000000000000028
(1 row)

-- Log file corresponding to the LSN after deletion
db_app=# SELECT pg_walfile_name('0/2800A210');
     pg_walfile_name      
--------------------------
 000000010000000000000028
(1 row)
```

## IV. Parse Log Files

**Confirm program location**
```Bash
[root@PGServer ~]# whereis pg_walex
pg_walex: /usr/local/bin/pg_walex
```
### 4.1 Parse via the entire log file
```Bash
[root@PGServer ~]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 
\connect db_app
-- pg_walex: xid=1182
BEGIN;
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (1, 'user_1', 9, 'c4ca4238', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (2, 'user_2', 19, 'c81e728d', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (3, 'user_3', 26, 'eccbc87e', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (4, 'user_4', 23, 'a87ff679', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (5, 'user_5', 98, 'e4da3b7f', '2026-10-24 12:58:58.490958+00'::timestamptz);
COMMIT;

-- pg_walex: xid=1183
BEGIN;
UPDATE public.user_profile SET bio = 'updated by me' WHERE id IS NOT DISTINCT FROM 1;
COMMIT;

-- pg_walex: xid=1184
BEGIN;
DELETE FROM public.user_profile WHERE id IS NOT DISTINCT FROM 3;
COMMIT;
```
Found that all have been parsed out
### 4.2 Parse via LSN start and end positions

**`0/28009BE8`** is the LSN before the update
**`0/2800A210`** is the LSN after the deletion

```Bash
[root@PGServer ~]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 -s   0/28009BE8 -e 0/2800A210
\connect db_app
-- pg_walex: xid=1183
BEGIN;
UPDATE public.user_profile SET bio = 'updated by me' WHERE id IS NOT DISTINCT FROM 1;
COMMIT;

-- pg_walex: xid=1184
BEGIN;
DELETE FROM public.user_profile WHERE id IS NOT DISTINCT FROM 3;
COMMIT;
```
### 4.3 copy to data file
Why design the copy mode? Because with a large amount of data, `insert` is too slow. So by parsing the wal log into metadata plus data, the data loading capability is greatly greatly greatly greatly greatly greatly greatly improved.
**result** is a custom output directory
```Bash
[root@PGServer ~]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 -o result --tocopy 

-- Check the output directory result
[root@PGServer result]# ls
manifest.txt  public.user_profile.data.sql  public.user_profile.meta.sql
-- Check manifest.txt, this is the script to directly load the data
[root@PGServer result]# cat manifest.txt 
-- pg_walex manifest (COPY mode)
-- replay with: cd result && psql -d <dbname> -f manifest.txt

\connect db_app
CREATE SCHEMA IF NOT EXISTS public;
\i 'public.user_profile.meta.sql'
\copy public.user_profile (id, username, age, bio, created_at) FROM 'public.user_profile.data.sql';
-- pg_walex: xid=1183
BEGIN;
UPDATE public.user_profile SET bio = 'updated by me' WHERE id IS NOT DISTINCT FROM 1;
COMMIT;

-- pg_walex: xid=1184
BEGIN;
DELETE FROM public.user_profile WHERE id IS NOT DISTINCT FROM 3;
COMMIT;

-- pg_walex: end of COPY output
-- public.user_profile.meta.sql this is the table structure metadata
[root@PGServer result]# cat public.user_profile.meta.sql 
CREATE TABLE IF NOT EXISTS public.user_profile (
  id int8 NOT NULL,
  username text NOT NULL,
  age int4,
  bio text,
  created_at timestamptz,
  PRIMARY KEY (id)
);

-- public.user_profile.data.sql this is the data
[root@PGServer result]# cat public.user_profile.data.sql 
1       user_1  9       c4ca4238        2026-10-24 12:58:58.490958+00
2       user_2  19      c81e728d        2026-10-24 12:58:58.490958+00
3       user_3  26      eccbc87e        2026-10-24 12:58:58.490958+00
4       user_4  23      a87ff679        2026-10-24 12:58:58.490958+00
5       user_5  98      e4da3b7f        2026-10-24 12:58:58.490958+00
\.
```
### 4.4 batch insert mode
Considering this mode is also to greatly greatly greatly greatly greatly improve data loading capability
**batchresult** is the bulk INSERT output **file**, note, it is a file haha
```Bash
[root@PGServer result]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 -o batchresult --batch-inserts
-- Check the file content
[root@PGServer ~]# cat batchresult 
\connect db_app
-- pg_walex: xid=1182
BEGIN;
CREATE SCHEMA IF NOT EXISTS public;
-- pg_walex: table metadata synthesized from catalog
CREATE TABLE IF NOT EXISTS public.user_profile (
  id int8 NOT NULL,
  username text NOT NULL,
  age int4,
  bio text,
  created_at timestamptz,
  PRIMARY KEY (id)
);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (1, 'user_1', 9, 'c4ca4238', '2026-10-24 12:58:58.490958+00'::timestamptz), (2, 'user_2', 19, 'c81e728d', '2026-10-24 12:58:58.490958+00'::timestamptz), (3, 'user_3', 26, 'eccbc87e', '2026-10-24 12:58:58.490958+00'::timestamptz), (4, 'user_4', 23, 'a87ff679', '2026-10-24 12:58:58.490958+00'::timestamptz), (5, 'user_5', 98, 'e4da3b7f', '2026-10-24 12:58:58.490958+00'::timestamptz);
COMMIT;

-- pg_walex: xid=1183
BEGIN;
UPDATE public.user_profile SET bio = 'updated by me' WHERE id IS NOT DISTINCT FROM 1;
COMMIT;

-- pg_walex: xid=1184
BEGIN;
DELETE FROM public.user_profile WHERE id IS NOT DISTINCT FROM 3;
COMMIT;
```
### 4.5 Output as JSON mode
JSON mode only requires `-F json` to get it done
**JSON** mode is actually formatting the output of pg_waldump, of course, it also supports the original text format.
```json
[root@PGServer ~]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 -F json
{"lsn":"0/28000028","xid":0,"rmgr":"XLOG","info":"CHECKPOINT_SHUTDOWN","rmgr_id":0,"info_id":0,"total_len":114,"prev_lsn":"0/27C9A818","main_len":88,"blocks":[]}
{"lsn":"0/280000A0","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28000028","main_len":24,"blocks":[]}
{"lsn":"0/280000D8","xid":0,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":1765,"prev_lsn":"0/280000A0","blocks":[{"id":0,"spc":1663,"db":17961,"rel":1259,"fork":"main","blkno":0,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/280007C0","xid":0,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":8193,"prev_lsn":"0/280000D8","blocks":[{"id":0,"spc":1663,"db":17961,"rel":1249,"fork":"main","blkno":16,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/280027E0","xid":0,"rmgr":"Heap2","info":"PRUNE_ON_ACCESS","rmgr_id":9,"info_id":16,"total_len":54,"prev_lsn":"0/280007C0","main_len":2,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":1249,"fork":"main","blkno":16,"data_len":6,"has_image":false,"will_init":false}]}
{"lsn":"0/28002818","xid":0,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":8181,"prev_lsn":"0/280027E0","blocks":[{"id":0,"spc":1663,"db":17961,"rel":2610,"fork":"main","blkno":0,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/28004828","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28002818","main_len":24,"blocks":[]}
{"lsn":"0/28004860","xid":0,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":8193,"prev_lsn":"0/28004828","blocks":[{"id":0,"spc":1663,"db":17961,"rel":1249,"fork":"main","blkno":54,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/28006880","xid":0,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":5629,"prev_lsn":"0/28004860","blocks":[{"id":0,"spc":1663,"db":17961,"rel":2659,"fork":"main","blkno":10,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/28007E80","xid":0,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":609,"prev_lsn":"0/28006880","blocks":[{"id":0,"spc":1663,"db":17961,"rel":2604,"fork":"main","blkno":0,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/28008100","xid":0,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":2917,"prev_lsn":"0/28007E80","blocks":[{"id":0,"spc":1663,"db":17961,"rel":2606,"fork":"main","blkno":4,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/28008C68","xid":0,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":241,"prev_lsn":"0/28008100","blocks":[{"id":0,"spc":1663,"db":17961,"rel":2224,"fork":"main","blkno":0,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/28008D60","xid":1182,"rmgr":"Sequence","info":"LOG","rmgr_id":15,"info_id":0,"total_len":99,"prev_lsn":"0/28008C68","main_len":53,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17962,"fork":"main","blkno":0,"data_len":0,"has_image":false,"will_init":true}]}
{"lsn":"0/28008DC8","xid":1182,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":1613,"prev_lsn":"0/28008D60","blocks":[{"id":0,"spc":1663,"db":17961,"rel":1249,"fork":"main","blkno":55,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/28009418","xid":1182,"rmgr":"Heap","info":"INSERT","rmgr_id":10,"info_id":128,"total_len":95,"prev_lsn":"0/28008DC8","main_len":3,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17963,"fork":"main","blkno":0,"data_len":46,"has_image":false,"will_init":true}]}
{"lsn":"0/28009478","xid":1182,"rmgr":"Btree","info":"INSERT_META","rmgr_id":11,"info_id":160,"total_len":90,"prev_lsn":"0/28009418","main_len":8,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17972,"fork":"main","blkno":1,"data_len":0,"has_image":false,"will_init":true},{"id":2,"spc":1663,"db":17961,"rel":17972,"fork":"main","blkno":0,"data_len":28,"has_image":false,"will_init":true}]}
{"lsn":"0/280094D8","xid":1182,"rmgr":"Btree","info":"INSERT_LEAF","rmgr_id":11,"info_id":0,"total_len":64,"prev_lsn":"0/28009478","main_len":2,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17972,"fork":"main","blkno":1,"data_len":16,"has_image":false,"will_init":false}]}
{"lsn":"0/28009518","xid":1182,"rmgr":"Heap","info":"INSERT","rmgr_id":10,"info_id":0,"total_len":95,"prev_lsn":"0/280094D8","main_len":3,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17963,"fork":"main","blkno":0,"data_len":46,"has_image":false,"will_init":false}]}
{"lsn":"0/28009578","xid":1182,"rmgr":"Btree","info":"INSERT_LEAF","rmgr_id":11,"info_id":0,"total_len":64,"prev_lsn":"0/28009518","main_len":2,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17972,"fork":"main","blkno":1,"data_len":16,"has_image":false,"will_init":false}]}
{"lsn":"0/280095B8","xid":1182,"rmgr":"Heap","info":"INSERT","rmgr_id":10,"info_id":0,"total_len":95,"prev_lsn":"0/28009578","main_len":3,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17963,"fork":"main","blkno":0,"data_len":46,"has_image":false,"will_init":false}]}
{"lsn":"0/28009618","xid":1182,"rmgr":"Btree","info":"INSERT_LEAF","rmgr_id":11,"info_id":0,"total_len":64,"prev_lsn":"0/280095B8","main_len":2,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17972,"fork":"main","blkno":1,"data_len":16,"has_image":false,"will_init":false}]}
{"lsn":"0/28009658","xid":1182,"rmgr":"Heap","info":"INSERT","rmgr_id":10,"info_id":0,"total_len":95,"prev_lsn":"0/28009618","main_len":3,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17963,"fork":"main","blkno":0,"data_len":46,"has_image":false,"will_init":false}]}
{"lsn":"0/280096B8","xid":1182,"rmgr":"Btree","info":"INSERT_LEAF","rmgr_id":11,"info_id":0,"total_len":64,"prev_lsn":"0/28009658","main_len":2,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17972,"fork":"main","blkno":1,"data_len":16,"has_image":false,"will_init":false}]}
{"lsn":"0/280096F8","xid":1182,"rmgr":"Heap","info":"INSERT","rmgr_id":10,"info_id":0,"total_len":95,"prev_lsn":"0/280096B8","main_len":3,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17963,"fork":"main","blkno":0,"data_len":46,"has_image":false,"will_init":false}]}
{"lsn":"0/28009758","xid":1182,"rmgr":"Btree","info":"INSERT_LEAF","rmgr_id":11,"info_id":0,"total_len":64,"prev_lsn":"0/280096F8","main_len":2,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17972,"fork":"main","blkno":1,"data_len":16,"has_image":false,"will_init":false}]}
{"lsn":"0/28009798","xid":1182,"rmgr":"Transaction","info":"COMMIT","rmgr_id":1,"info_id":128,"total_len":46,"prev_lsn":"0/28009758","main_len":20,"blocks":[]}
{"lsn":"0/280097C8","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28009798","main_len":24,"blocks":[]}
{"lsn":"0/28009800","xid":0,"rmgr":"XLOG","info":"CHECKPOINT_REDO","rmgr_id":0,"info_id":224,"total_len":30,"prev_lsn":"0/280097C8","main_len":4,"blocks":[]}
{"lsn":"0/28009820","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28009800","main_len":24,"blocks":[]}
{"lsn":"0/28009858","xid":0,"rmgr":"XLOG","info":"CHECKPOINT_ONLINE","rmgr_id":0,"info_id":16,"total_len":114,"prev_lsn":"0/28009820","main_len":88,"blocks":[]}
{"lsn":"0/280098D0","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28009858","main_len":24,"blocks":[]}
{"lsn":"0/28009908","xid":0,"rmgr":"XLOG","info":"FPI_FOR_HINT","rmgr_id":0,"info_id":160,"total_len":413,"prev_lsn":"0/280098D0","blocks":[{"id":0,"spc":1663,"db":17961,"rel":17963,"fork":"main","blkno":0,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/28009AA8","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28009908","main_len":24,"blocks":[]}
{"lsn":"0/28009AE0","xid":0,"rmgr":"XLOG","info":"CHECKPOINT_REDO","rmgr_id":0,"info_id":224,"total_len":30,"prev_lsn":"0/28009AA8","main_len":4,"blocks":[]}
{"lsn":"0/28009B00","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28009AE0","main_len":24,"blocks":[]}
{"lsn":"0/28009B38","xid":0,"rmgr":"XLOG","info":"CHECKPOINT_ONLINE","rmgr_id":0,"info_id":16,"total_len":114,"prev_lsn":"0/28009B00","main_len":88,"blocks":[]}
{"lsn":"0/28009BB0","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28009B38","main_len":24,"blocks":[]}
{"lsn":"0/28009BE8","xid":1183,"rmgr":"Heap","info":"HOT_UPDATE","rmgr_id":10,"info_id":64,"total_len":559,"prev_lsn":"0/28009BB0","main_len":14,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17963,"fork":"main","blkno":0,"data_len":54,"has_image":true,"will_init":false}]}
{"lsn":"0/28009E18","xid":1183,"rmgr":"Transaction","info":"COMMIT","rmgr_id":1,"info_id":128,"total_len":46,"prev_lsn":"0/28009BE8","main_len":20,"blocks":[]}
{"lsn":"0/28009E48","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28009E18","main_len":24,"blocks":[]}
{"lsn":"0/28009E80","xid":0,"rmgr":"XLOG","info":"CHECKPOINT_REDO","rmgr_id":0,"info_id":224,"total_len":30,"prev_lsn":"0/28009E48","main_len":4,"blocks":[]}
{"lsn":"0/28009EA0","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28009E80","main_len":24,"blocks":[]}
{"lsn":"0/28009ED8","xid":0,"rmgr":"XLOG","info":"CHECKPOINT_ONLINE","rmgr_id":0,"info_id":16,"total_len":114,"prev_lsn":"0/28009EA0","main_len":88,"blocks":[]}
{"lsn":"0/28009F50","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/28009ED8","main_len":24,"blocks":[]}
{"lsn":"0/28009F88","xid":1184,"rmgr":"Heap","info":"DELETE","rmgr_id":10,"info_id":16,"total_len":513,"prev_lsn":"0/28009F50","main_len":22,"blocks":[{"id":0,"spc":1663,"db":17961,"rel":17963,"fork":"main","blkno":0,"data_len":0,"has_image":true,"will_init":false}]}
{"lsn":"0/2800A1A8","xid":1184,"rmgr":"Transaction","info":"COMMIT","rmgr_id":1,"info_id":128,"total_len":46,"prev_lsn":"0/28009F88","main_len":20,"blocks":[]}
{"lsn":"0/2800A1D8","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/2800A1A8","main_len":24,"blocks":[]}
{"lsn":"0/2800A210","xid":0,"rmgr":"XLOG","info":"CHECKPOINT_REDO","rmgr_id":0,"info_id":224,"total_len":30,"prev_lsn":"0/2800A1D8","main_len":4,"blocks":[]}
{"lsn":"0/2800A230","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/2800A210","main_len":24,"blocks":[]}
{"lsn":"0/2800A268","xid":0,"rmgr":"XLOG","info":"CHECKPOINT_ONLINE","rmgr_id":0,"info_id":16,"total_len":114,"prev_lsn":"0/2800A230","main_len":88,"blocks":[]}
{"lsn":"0/2800A2E0","xid":0,"rmgr":"Standby","info":"LOCK","rmgr_id":8,"info_id":16,"total_len":50,"prev_lsn":"0/2800A268","main_len":24,"blocks":[]}
```
etc., etc., etc., etc., etc., etc., don't want to write anymore, the features are too complete.

### 4.7 Export only a specific database / a specific table / a specific transaction
```SQL
[root@PGServer ~]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 --db-oid 17961 /data/tmp/data/pg_wal
-- pg_walex: xid=1182
BEGIN;
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (1, 'user_1', 9, 'c4ca4238', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (2, 'user_2', 19, 'c81e728d', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (3, 'user_3', 26, 'eccbc87e', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (4, 'user_4', 23, 'a87ff679', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (5, 'user_5', 98, 'e4da3b7f', '2026-10-24 12:58:58.490958+00'::timestamptz);
COMMIT;

-- pg_walex: xid=1183
BEGIN;
UPDATE public.user_profile SET bio = 'updated by me' WHERE id IS NOT DISTINCT FROM 1;
COMMIT;

-- pg_walex: xid=1184
BEGIN;
DELETE FROM public.user_profile WHERE id IS NOT DISTINCT FROM 3;
COMMIT;

-- Export only table
[root@PGServer ~]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 --db-oid 17961 --table public.user_profile /data/tmp/data/pg_wal
-- pg_walex: xid=1182
BEGIN;
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (1, 'user_1', 9, 'c4ca4238', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (2, 'user_2', 19, 'c81e728d', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (3, 'user_3', 26, 'eccbc87e', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (4, 'user_4', 23, 'a87ff679', '2026-10-24 12:58:58.490958+00'::timestamptz);
INSERT INTO public.user_profile (id, username, age, bio, created_at) VALUES (5, 'user_5', 98, 'e4da3b7f', '2026-10-24 12:58:58.490958+00'::timestamptz);
COMMIT;

-- pg_walex: xid=1183
BEGIN;
UPDATE public.user_profile SET bio = 'updated by me' WHERE id IS NOT DISTINCT FROM 1;
COMMIT;

-- pg_walex: xid=1184
BEGIN;
DELETE FROM public.user_profile WHERE id IS NOT DISTINCT FROM 3;
COMMIT;

-- Export only a specific transaction xid
[root@PGServer ~]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 --db-oid 17961 --xid 1183 /data/tmp/data/pg_wal
-- pg_walex: xid=1183
BEGIN;
UPDATE public.user_profile SET bio = 'updated by me' WHERE id IS NOT DISTINCT FROM 1;
COMMIT;
```
## V. Nagging Words
1. **WAL is not just the patent of "crash recovery"**. Under the premise of not starting the database, `pg_walex` is powerful enough to completely restore transactions, statements, and data containing TOAST/arrays/jsonb/Chinese/emoji into SQL.
2. **Recovery methods should be selected according to the scenario**. For auditing and viewing changes, use the default SQL; to load an empty database, use `--no-pgdata`; for speed, use `--tocopy` or `--batch-inserts`.
Rest assured, for the extreme scenario of "the database won't start, and only WAL remains", `pg_walex` can easily help you fish the data back.
