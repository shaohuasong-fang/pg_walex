
[English](README_EN.md) | [简体中文](README.md)

# pg_walex

**PostgreSQL WAL Extractor** —— 一个离线、无依赖的 PostgreSQL 预写日志读取工具，
可以把物理 WAL 记录重新还原为可移植的 SQL。

`pg_walex` 直接读取 WAL 段文件。它从不启动数据库服务，不链接 `libpq`，也不连接
任何正在运行的实例。通过边解析物理记录边重建系统目录状态，它把堆表变更还原为
可执行的通用 SQL 文本，也可以通过内置的 RESTful API 提供同样的结果。

支持的 PostgreSQL 大版本：**11 / 12 / 13 / 14 / 15 / 16 / 17 / 18**。

典型使用场景：

- 审计与取证分析：数据库实际发生了哪些变更；
- 从崩溃、被删除或无法启动的实例中恢复 DML/DDL；
- 在没有逻辑复制或在线连接的情况下把变更复制到另一个系统；
- 把 WAL 内容以 SQL 或 JSON 形式送入外部流水线。

---

## 1. 功能特性

- **直接访问 WAL**：解析段文件、页头、跨页续写（`contrecord`）、记录头、
  块引用和主数据。
- **完整的记录解析**：CRC32C 校验；PGLZ / LZ4 / Zstandard 压缩的整页镜像（FPI）
  会被解压。
- **系统目录重建**：根据 WAL 中出现的 `pg_class`、`pg_attribute`、
  `pg_namespace`、`pg_type`、`pg_index`、`pg_constraint`、`pg_enum`、
  `pg_sequence`、`pg_database`、`pg_authid` 和 `pg_attrdef` 变更重建对象名、
  列名、类型、主键、唯一约束和枚举；目录元组也可以从 FPI 页中抢救出来。
- **TOAST 还原**：收集并解压 toast 数据块（PGLZ/LZ4/ZSTD）。WAL 窗口之外的
  数据块会按 valueid 精确地从数据目录中的 TOAST 关系文件读取，因此大字段
  （jsonb、text 等）可以完整恢复。
- **离线目录预热**：当 WAL 目录是 `<pgdata>/pg_wal`（或
  `<pgdata>/pgdata/pg_wal`）时，工具会读取其旁边的 `base/`、`global/` 和
  `pg_xact/`，离线判断元组可见性，无需启动服务即可获得完整的表和列元数据。
  这也是为什么即使只保留最近的 WAL 窗口，`INSERT`/`UPDATE` 语句仍然带有完整
  的列清单和值。可以用 `--pgdata PATH` 显式指定数据目录，或用 `--no-pgdata`
  关闭该行为。
- **SQL 重建**：
  - DML：语句重建
  - DDL：语句重建
  - 事务：语句按 xid 分组并包裹在 `BEGIN/COMMIT` 中
  - 语句注释：每个事务前输出 `-- pg_walex: type=...,xid=...,lsn=...,timestamp=...`。
  - 逆操作：`--rollback` 为每条 `UPDATE`/`DELETE` 追加可回放的逆操作语句，一般仅 'wal_level = logical' 时有效。
- **值渲染**：支持数据库内部的数据类型渲染，无法渲染的类型会输出为`NULL` 并附注释，保证脚本可执行。
- **多数据库**：`--db-oid` 只输出一个数据库；否则按数据库输出 psql
  `\connect` 语句。
- **预热**：使用 `--start-lsn` 时，工具先静默回放最早可用的 WAL 以重建目录和
  行缓存，只在到达指定 LSN 后输出 SQL，从而保证表名、列和键正确。
- **前缀/后缀压缩的 UPDATE**：行缓存保留原始元组字节，因此压缩的 `UPDATE`
  （`wal_level=replica`）仍会被展开为完整新行和按主键定位的 `UPDATE`。
- **输出格式**：`sql` / `json`（每行一条记录）/ `text`（类似 `pg_waldump`）/
  `stats`。
- **大容量装载输出**：`--tocopy` 写出 COPY 格式的数据文件以及 `manifest.txt`
  回放脚本（每张表一个 `<schema>.<table>.meta.sql` / `.data.sql`）；
  `--batch-inserts[=N]` 把同一张表连续的 INSERT 合并为多 `VALUES` 语句，
  使大 WAL 量的恢复比逐行 `INSERT` 快多个数量级。两个选项都是附加功能：
  不使用它们时 SQL 输出保持不变。
- **RESTful API**：健康检查、WAL 信息、记录列表、SQL 重建、统计和目录查询，
  支持可选的 Bearer Token 认证。
- **实时 WAL 监控**：使用 `--serve --monitor` 启动后，后台线程会持续跟随

---

## 2. 构建

要求：C11 编译器和 POSIX 环境（macOS / Linux）。
可选：`liblz4`、`libzstd`。

```bash
cd pg_walex
make                 # 自动检测系统 lz4/zstd
# 或显式禁用：make LDFLAGS= LDLIBS=
```

如果这个目录是从别的机器（例如 macOS arm64）整包拷过来的，里面会残留
其他平台的 `src/*.o`，链接时会报 `file not recognized: File format not
recognized`，先清理再构建即可：

```bash
make clean && make
```

构建 `pg_walex`。安装：

```bash
make install PREFIX=/usr/local
whereis pg_walex
pg_walex: /usr/local/bin/pg_walex
```

> 使用 LZ4 或 ZSTD 压缩的 FPI/TOAST 数据需要对应的共享库；PGLZ 由内部实现，
> 不需要外部依赖。

---

## 3. 命令行

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


