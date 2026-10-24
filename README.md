
[ [English](README_EN.md) | 简体中文 ]
[ English | [简体中文](README.md) ]

**特性**
> 不启动数据库、不连 libpq、不依赖逻辑复制，纯C开发，直接读 WAL 段文件，把数据库中从建表到插入、更新、删除、改类型，完整解析为可执行的 SQL。

>大杀器，逐行 `INSERT` 恢复大数据量非常慢。pg_walex 提供了两种"批量装载"输出，`--tocopy` 和 `--batch-inserts`，将单行`INSERT`语句封装为支持`COPY`命令的结构和 `INSERT INTO 表名 VALUES(?),(?)....`的能力，千万数据，尽在秒级加。

> 只要你的归档够多，全库恢复不在话下，保证加载性能，众多wal只导出某个库 / 某张表 / 某个事务都不是问题。

## 下载地址
```Bash
git clone https://github.com/shaohuasong-fang/pg_walex.git

```

---

## 一、为什么会有"解析WAL 日志文件"这种需求

DBA 最怕的场景通常不仅仅是"库挂了"，而且还有以下场景：
- 库还能启动，但有人误删了数据，审计日志又没开；
- 从崩溃、被删除或无法启动的实例中恢复 DML/DDL；
- 需要把某段时间的变更同步到另一套环境，但不想上逻辑复制；
- 审计与取证分析：数据库实际发生了哪些变更；
- 在没有逻辑复制或在线连接的情况下把变更复制到另一个系统；
- 把 WAL 内容以 SQL 或 JSON 形式送入外部流水线。

PostgreSQL 的 WAL 是物理日志：页、元组、事务、系统目录，全都以二进制形式记录在里面。把它读出来并还原成 SQL，技术门槛不低——要解析记录头、跨页续写、CRC32C、整页镜像（FPI）、TOAST、系统目录、事务可见性等，那么`pg_walex`（PostgreSQL WAL Extractor）做的就是这件事：**纯离线、零依赖、单二进制**，支持 PostgreSQL 11 到 18的 `wal`日志文件解析工具。

---

## 二、环境准备

```env
数据库版本：PostgreSQL 18
数据库:db_app
wal_level: logical 或者 replica
```

### 2.1 使用模拟数据
模拟数据用 `deepseek` 生成即可
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
-- 查看数据
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
### 2.2 编译工具
因 `pg_walex` 独立开发，已经将 `PostgreSQL` 中的 `wal` 格式完整写入到代码里面，所以不需要依赖任何 `PostgreSQL` 实例，下载源码后直接 `make` 和 `make install` 即可。
- **先决条件**
```shell
要求：C11 编译器和 POSIX 环境（macOS / Linux）。
可选：`liblz4`、`libzstd`。
```
- **编译源码**
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
- **安装**
```Bash
[root@PGServer pg_walex]# make install PREFIX=/usr/local
install -d /usr/local/bin
install -m 0755 pg_walex /usr/local/bin/pg_walex
```
-- **查看版本**
```bash
[root@PGServer pg_walex]# ./pg_walex --version
pg_walex 1.0.0
```

## 三、验证解析
### 3.1 查看当前日志`LSN`和其对应的文件

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
### 3.2 修改数据并查看日志序列号
```SQL
db_app=# update user_profile set bio = 'updated by me' where id = 1;
UPDATE 1
db_app=# select pg_current_wal_lsn();
 pg_current_wal_lsn 
--------------------
 0/28009E80
(1 row)
```
### 3.3 删除数据并查看日志序列号
```SQL
db_app=# delete from user_profile where id = 3;
DELETE 1
db_app=# select pg_current_wal_lsn();
 pg_current_wal_lsn 
--------------------
 0/2800A210
(1 row)
```
### 3.4 查看日志序列编号对应的日志文件
```SQL
-- 更新后的LSN对应的日志文件
db_app=# SELECT pg_walfile_name('0/28009E80');
     pg_walfile_name      
--------------------------
 000000010000000000000028
(1 row)

-- 删除后的LSN对应的日志文件
db_app=# SELECT pg_walfile_name('0/2800A210');
     pg_walfile_name      
--------------------------
 000000010000000000000028
(1 row)
```

## 四、解析日志文件

**确认程序位置**
```Bash
[root@PGServer ~]# whereis pg_walex
pg_walex: /usr/local/bin/pg_walex
```
### 4.1 通过整个日志文件解析
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
发现都解析出来了
### 4.2 通过`LSN`的起始位置和终止位置解析

**`0/28009BE8`** 是更新前的`LSN`
**`0/2800A210`** 是删除后的`LSN`

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
### 4.3 copy到数据文件
为什么设计`copy`模式呢，因为数据量大的情况下，`insert` 太慢了，所以通过解析 `wal` 日志为**元数据**加**数据**的方式，来**大大大大大大大**幅度的提高数据的加载能力。
**result**是自定义的输出目录
```Bash
[root@PGServer ~]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 -o result --tocopy 

-- 查看输出目录 result
[root@PGServer result]# ls
manifest.txt  public.user_profile.data.sql  public.user_profile.meta.sql
-- 查看manifest.txt，这个就是直接加载到数据的脚本
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
-- public.user_profile.meta.sql  这个就是表结构元数据
[root@PGServer result]# cat public.user_profile.meta.sql 
CREATE TABLE IF NOT EXISTS public.user_profile (
  id int8 NOT NULL,
  username text NOT NULL,
  age int4,
  bio text,
  created_at timestamptz,
  PRIMARY KEY (id)
);

-- public.user_profile.data.sql 这个就是数据
[root@PGServer result]# cat public.user_profile.data.sql 
1       user_1  9       c4ca4238        2026-10-24 12:58:58.490958+00
2       user_2  19      c81e728d        2026-10-24 12:58:58.490958+00
3       user_3  26      eccbc87e        2026-10-24 12:58:58.490958+00
4       user_4  23      a87ff679        2026-10-24 12:58:58.490958+00
5       user_5  98      e4da3b7f        2026-10-24 12:58:58.490958+00
\.
```
### 4.4 batch insert 模式
考虑这个模式也是为了**大大大大大大**幅度提高数据加载的能力
**batchresult** 为批量INSERT输出 **文件**，注意，是文件哈
```Bash
[root@PGServer result]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 -o batchresult --batch-inserts
-- 查看文件内容
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
### 4.5 输出为 `JSON` 模式
JSON 模式仅需 `-F json`就搞定
**JSON** 模式实际上就是对pg_waldump的输出进行了格式化,当然也支持原始的文本格式。
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
等等等等等等，不想写了，功能太全了。

### 4.7 只导出某个库 / 某张表 / 某个事务
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

-- 仅导出表
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

-- 仅导出某个事务xid
[root@PGServer ~]# pg_walex -f /data/tmp/data/pg_wal/000000010000000000000028 --db-oid 17961 --xid 1183 /data/tmp/data/pg_wal
-- pg_walex: xid=1183
BEGIN;
UPDATE public.user_profile SET bio = 'updated by me' WHERE id IS NOT DISTINCT FROM 1;
COMMIT;
```
## 五、啰嗦语
1. **WAL 不只是"崩溃恢复"的专利**。在不启动数据库的前提下，pg_walex 强大到能把事务、语句、包含 TOAST/数组/jsonb/中文/emoji 的数据完整还原成 SQL。
2. **恢复方式要按场景选**。要审计、要看变更，用默认 SQL；要灌空库，用 `--no-pgdata`；要快，用 `--tocopy` 或 `--batch-inserts`。
放心，对于"库起不来、只剩 WAL"的极端场景，pg_walex能轻松帮你把数据捞回来。

