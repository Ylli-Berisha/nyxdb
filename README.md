# nyxdb

A columnar analytical database written from scratch in C++17. Persistent immutable segment storage, a write-ahead log, B+ tree indexes, PRIMARY KEY / UNIQUE constraints, leader-follower replication over QUIC, and a background merge + vacuum engine.

---

## Contents

- [Features](#features)
- [Architecture](#architecture)
- [SQL Reference](#sql-reference)
- [Getting Started](#getting-started)
- [Server Mode](#server-mode)
- [Replication](#replication)
- [Storage Internals](#storage-internals)
- [Building](#building)
- [Testing](#testing)

---

## Features

| Category | What's there |
|---|---|
| **Storage** | Columnar immutable segments, LRU-K buffer pool, 8 KB pages with xxHash checksums |
| **Write path** | In-memory write buffer → sealed to immutable segment at 65 536 rows or on flush |
| **WAL** | Append-only write-ahead log, replayed on startup for crash recovery |
| **Indexes** | B+ tree per column, point lookup and range scan, unique enforcement |
| **Constraints** | `NOT NULL`, `PRIMARY KEY`, `UNIQUE`, composite keys |
| **Maintenance** | Background merge worker (consolidates small segments), `VACUUM` for dead-row compaction |
| **Query engine** | Volcano-model executor: `TableScan`, `IndexScan`, `Filter`, `Project`, `Sort`, `Limit`, `HashAggregate`, `HashJoin` (equi-join only) |
| **Server** | QUIC transport (msquic), binary frame protocol, token authentication |
| **Replication** | Leader-follower, Raft-like election, WAL streaming, snapshot bootstrap |
| **Language** | C++17, ASan + UBSan in debug builds |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         SQL string                              │
└──────────────────────────────┬──────────────────────────────────┘
                               │
              ┌────────────────▼────────────────┐
              │           Lexer / Parser        │
              │        (token.h  ast.h)         │
              └────────────────┬────────────────┘
                               │  ast::Statement
              ┌────────────────▼────────────────┐
              │              Binder             │
              │    (bound_ast.h  binder.cpp)    │
              │  name resolution, type checking │
              └────────────────┬────────────────┘
                               │  bound::BoundStatement
              ┌────────────────▼─────────────────┐
              │             Planner              │
              │         (planner.cpp)            │
              │  logical → physical operator tree│
              └────────────────┬─────────────────┘
                               │  Operator*
              ┌────────────────▼─────────────────┐
              │            Executor              │
              │  TableScan / IndexScan / Filter  │
              │  Sort / HashJoin (equi) / HashAgg│
              └────────────────┬─────────────────┘
                               │  Chunk (columnar batch)
              ┌────────────────▼────────────────┐
              │            Catalog              │
              │  table registry, WAL, indexes,  │
              │  constraint enforcement         │
              └────────────────┬────────────────┘
                               │
        ┌──────────────────────┼───────────────────────┐
        │                      │                       │
┌───────▼──────┐   ┌───────────▼──────────┐   ┌───────▼──────┐
│  Table (RW)  │   │    BTreeIndex        │   │    WAL       │
│  write buf   │   │  (btree_index.cpp)   │   │  wal_writer  │
│  + segments  │   └──────────────────────┘   └──────────────┘
└───────┬──────┘
        │
┌───────▼───────────────────────────────────────┐
│              Immutable Segments               │
│  seg_0/  seg_1/  …  seg_N/                    │
│  ├── <col>.col   (ColumnFile, 8 KB pages)     │
│  ├── deleted.bin (tombstone bitmap)           │
│  └── meta.bin    (id, base_row_id, row_count) │
│                                               │
│  manifest.bin  (ordered segment list)         │
│  schema.bin    (column definitions)           │
└───────────────────────────────────────────────┘
```

### Key design choices

**Columnar storage.** Each column lives in its own `ColumnFile`. A scan that reads only two columns out of ten touches roughly 20% of the disk. Pages are fixed at 8 KB and carry an xxHash-64 checksum in the header.

**Immutable segments.** Once a write buffer is sealed it becomes an immutable `seg_N/` directory. Writes never mutate existing segment files. This makes reads lock-free at the segment level (shared-mutex, reader-writer).

**Copy-on-write mutations.** `UPDATE` is delete + insert. `VACUUM` rewrites segments without tombstoned rows into a temporary directory then atomically renames it into place via `Table::replace_segments`.

**WAL for crash recovery.** Every insert, delete, and update is logged before the data write. On startup `Catalog::load` replays the WAL up to the last checkpoint LSN, then truncates to consistent state.

**Background workers.** A `MergeWorker` thread wakes every 5 seconds. It runs vacuum on any table whose dead-row ratio exceeds 20%, then merges segments when there are more than 8.

---

## SQL Reference

### Data Types

| SQL type | Storage | Notes |
|---|---|---|
| `INT` | 4 bytes | 32-bit signed integer |
| `BIGINT` | 8 bytes | 64-bit signed integer |
| `DOUBLE` | 8 bytes | IEEE 754 double |
| `VARCHAR(n)` | n + 2 bytes | Fixed max-length string |
| `BOOL` | 1 byte | `TRUE` / `FALSE` |
| `DATE` | 4 bytes | Days since Unix epoch, literal `DATE '2024-01-15'` |
| `TIMESTAMP` | 8 bytes | Microseconds since Unix epoch, literal `TIMESTAMP '2024-01-15 10:30:00'` |

### DDL

```sql
-- Create a table
CREATE TABLE orders (
    id     INT      NOT NULL,
    price  DOUBLE,
    label  VARCHAR(64),
    placed DATE
);

-- Constraints inline
CREATE TABLE users (
    id    INT PRIMARY KEY,
    email VARCHAR(128) UNIQUE,
    name  VARCHAR(64)  NOT NULL
);

-- Composite primary key
CREATE TABLE line_items (
    order_id INT NOT NULL,
    sku      INT NOT NULL,
    qty      INT,
    PRIMARY KEY (order_id, sku)
);

-- B+ tree index
CREATE INDEX idx_price ON orders (price);
CREATE UNIQUE INDEX idx_email ON users (email);

-- Introspection
SHOW INDEXES     FROM orders;
SHOW CONSTRAINTS FROM users;

-- Drop
DROP INDEX  idx_price ON orders;
DROP TABLE  orders;
DROP TABLE IF EXISTS ghost;
```

### DML

```sql
-- Insert one row
INSERT INTO users VALUES (1, 'alice@example.com', 'Alice');

-- Insert multiple rows
INSERT INTO orders VALUES
    (1, 9.99,  'widget', DATE '2024-03-01'),
    (2, 49.50, 'gadget', DATE '2024-03-02');

-- Select with filter and sort
SELECT id, price
FROM   orders
WHERE  price > 10.0
ORDER BY price DESC
LIMIT 5;

-- Aggregation
SELECT label, count(*), sum(price)
FROM   orders
GROUP BY label
HAVING count(*) > 1
ORDER BY label ASC;

-- Join (equi-join only; hash join)
SELECT u.name, o.price
FROM   users  u
JOIN   orders o ON u.id = o.user_id
WHERE  o.price < 100.0;

-- Update
UPDATE orders SET price = price * 0.9 WHERE label = 'widget';

-- Delete with predicate
DELETE FROM orders WHERE placed < DATE '2024-01-01';

-- Delete all rows
DELETE FROM orders;
```

### Maintenance

```sql
-- Compact a table: rewrites all segments, removing tombstoned rows.
-- Returns the number of dead rows reclaimed.
VACUUM orders;
```

`VACUUM` also checkpoints the WAL, so the table's full on-disk state is self-consistent immediately after.

### Expressions

| Category | Operators |
|---|---|
| Arithmetic | `+` `-` `*` `/` |
| Comparison | `=` `<>` `<` `<=` `>` `>=` |
| Logical | `AND` `OR` `NOT` |
| Null | `IS NULL` `IS NOT NULL` |
| Aggregates | `count(*)` `count(col)` `sum(col)` `avg(col)` `min(col)` `max(col)` |
| Date literals | `DATE 'YYYY-MM-DD'` |
| Timestamp literals | `TIMESTAMP 'YYYY-MM-DD HH:MM:SS'` |

---

## Getting Started

### Prerequisites

- CMake ≥ 3.20
- GCC or Clang with C++17 support
- Linux (uses `pread`/`pwrite`, QUIC server requires msquic)

### Build

```bash
git clone --recurse-submodules https://github.com/you/nyxdb
cd nyxdb
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Debug build enables AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Embedded REPL

```bash
./build/nyxdb --data-dir ./mydata
```

```
nyxdb  data=./mydata  (exit or Ctrl-D to quit)

nyx> CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR(64), price DOUBLE);
OK
nyx> INSERT INTO products VALUES (1, 'Widget', 9.99), (2, 'Gadget', 49.50);
2 rows affected
nyx> SELECT name, price FROM products ORDER BY price DESC;
 name   | price
--------+-------
 Gadget | 49.5
 Widget | 9.99
(2 rows)
nyx> exit
```

Data is persisted to `--data-dir` and survives restarts. The WAL is replayed automatically on open.

### Embedded API (C++)

```cpp
#include "database/database.h"

auto db = nyx::Database::open("./mydata").value();

db.execute("CREATE TABLE events (id INT, ts TIMESTAMP)");

db.execute("INSERT INTO events VALUES (1, TIMESTAMP '2024-06-01 09:00:00')");

auto r = db.execute("SELECT id, ts FROM events WHERE id = 1");
// r.value().columns[0][0]  →  Value holding i32(1)
// r.value().columns[1][0]  →  Value holding Timestamp{...}
```

`Database::execute` is thread-safe for concurrent readers. A single `std::shared_mutex` serialises write paths.

---

## Server Mode

The server speaks a compact binary protocol over QUIC (msquic). All connections are authenticated with a shared token before queries are accepted.

### Start a standalone server

```bash
./build/nyxdb_server \
    --data-dir /var/lib/nyxdb \
    --token    supersecret \
    --port     4433
```

| Flag | Default | Description |
|---|---|---|
| `--data-dir` | required | Directory for segments, WAL, indexes |
| `--token` | required | Shared auth token for all clients |
| `--port` | `4433` | UDP port (QUIC) |
| `--log-level` | `warn` | `trace` `debug` `info` `warn` `error` |

### Wire protocol

Frames have a 9-byte header (`u32` length, `u8` type, `u32` query-id) followed by a variable payload.

| Frame | Direction | Description |
|---|---|---|
| `AUTH_REQ` | client → server | Send token |
| `AUTH_OK` / `AUTH_ERR` | server → client | Auth result |
| `QUERY` | client → server | UTF-8 SQL string |
| `RESULT_META` | server → client | Column names and types |
| `RESULT_COL` | server → client | One column's data, all rows |
| `RESULT_END` | server → client | Query complete, `rows_affected` count |
| `QUERY_ERR` | server → client | Error message |

---

## Replication

nyxdb supports asynchronous leader-follower replication. The leader streams its WAL to followers in real time. A Raft-inspired election protocol handles leader failure.

### Start a three-node cluster

```bash
# Node A — leader
./build/nyxdb_server \
    --data-dir /data/node-a \
    --token    clustertoken \
    --port     4433 \
    --node-id  node-a \
    --role     leader \
    --peers    node-a:4433,node-b:4434,node-c:4435

# Node B — follower
./build/nyxdb_server \
    --data-dir /data/node-b \
    --token    clustertoken \
    --port     4434 \
    --node-id  node-b \
    --role     follower \
    --peers    node-a:4433,node-b:4434,node-c:4435 \
    --leader-addr node-a:4433

# Node C — follower
./build/nyxdb_server \
    --data-dir /data/node-c \
    --token    clustertoken \
    --port     4435 \
    --node-id  node-c \
    --role     follower \
    --peers    node-a:4433,node-b:4434,node-c:4435 \
    --leader-addr node-a:4433
```

| Flag | Description |
|---|---|
| `--node-id` | Unique string identifier for this node |
| `--role` | `leader` or `follower` |
| `--peers` | Comma-separated `host:port` list of all nodes including self |
| `--leader-addr` | Current leader address (followers only, used for initial WAL pull) |
| `--max-wal-lag-mb` | Max WAL lag before a follower is considered stale (default 4096 MB) |

### How it works

- The leader appends every write to a `wal.bin` file and streams new records to followers over QUIC.
- Followers replay incoming WAL records in order, staying behind the leader by a bounded amount.
- **Write forwarding.** Clients that send a write to a follower have it transparently forwarded to the current leader, which then replicates the result back.
- **Snapshots.** A follower that is too far behind (or brand new) requests a full snapshot: the leader sends a consistent copy of all segment files and the current WAL offset.
- **Leader election.** If a follower does not receive a heartbeat within a randomised election timeout (default 3–5 s), it starts an election. Nodes vote for the candidate with the highest WAL LSN. The winner becomes leader for the new term. Election state (term, voted-for) is persisted to survive restarts.

---

## Storage Internals

### Segment layout

```
<table_dir>/
├── schema.bin          column names, types, nullability, max-len
├── manifest.bin        ordered list of sealed segment IDs + row ranges
├── constraints.bin     PRIMARY KEY / UNIQUE definitions
├── lsn.bin             WAL checkpoint (offset, row_count)
├── <col>.col           write-buffer ColumnFile (one per column)
├── deleted.bin         write-buffer tombstone bitmap (optional)
│
├── seg_0/
│   ├── <col>.col       sealed ColumnFile
│   ├── deleted.bin     tombstone bitmap (optional)
│   └── meta.bin        {id, base_row_id, row_count}
├── seg_1/
│   └── …
└── …
```

### ColumnFile format

Each `.col` file is a sequence of 8 KB pages managed by `DiskManager`. Every page carries:

- a 16-byte header: `page_id (u64)`, `checksum (u64 xxHash-64)`
- a payload area encoding fixed-width values plus an optional null-bitmap

`ColumnFile::row_count()` = `current_page_id × capacity + current_page.value_count()`. No separate metadata file needed.

### Buffer pool

A per-`ColumnFile` `BufferPool` (LRU-K eviction, default capacity 64 frames) sits between `ColumnFile` and `DiskManager`. Scans pin pages, the pool evicts cold frames when capacity is exceeded.

### Merge worker

Every 5 seconds the background `MergeWorker` iterates all tables:

1. **Vacuum check.** If `dead_rows / total_rows > 0.20` it seals the write buffer and calls `vacuum_table`, which rewrites segments without tombstoned rows into a temp directory then atomically swaps via `replace_segments`.
2. **Segment merge.** If there are ≥ 8 sealed segments it merges the oldest half into a single new segment (copying all rows including tombstoned ones — vacuum handles that separately).

Both operations are non-blocking: readers hold only a shared lock during the copy phase; the exclusive lock is held only for the brief `rename` + manifest rewrite.

### WAL and crash recovery

On every mutating operation the catalog appends a typed record to `wal.bin`:

| Record type | Payload |
|---|---|
| `CreateTable` | table name, schema |
| `Insert` | table name, rows |
| `Delete` | table name, physical row indices |
| `Update` | table name, old indices + new rows |
| `SegmentFlush` | table name, sealed row count, WAL offset |

On `Database::open` / `Catalog::load`:

1. Tables are opened from their segment files (the durable on-disk state).
2. The WAL is replayed from each table's last checkpoint LSN (`lsn.bin`) to recover writes that had not yet been flushed to segments.
3. `VACUUM` and `flush_all` delete the WAL after writing a fresh checkpoint, so restarts after a vacuum are instant.

---

## Building

```bash
# Release (optimised, -O3 -march=native)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Debug (ASan + UBSan)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)
```

### Dependencies (all vendored under `third_party/`)

| Library | Use |
|---|---|
| [msquic](https://github.com/microsoft/msquic) | QUIC transport for server and replication |
| [spdlog](https://github.com/gabime/spdlog) | Structured logging |
| [googletest](https://github.com/google/googletest) | Unit and integration tests |

---

## Testing

```bash
# Run everything
ctest --test-dir build --output-on-failure -j$(nproc)

# Run a single suite
ctest --test-dir build -R vacuum_test --output-on-failure
```

There are 47 test suites spanning every layer:

| Layer | Suites |
|---|---|
| Storage (disk) | `disk_manager`, `column_page`, `column_file`, `segment`, `table`, `truncate` |
| Storage (memory) | `buffer_pool`, `lru_k_replacer` |
| WAL | `wal_writer` |
| Executor | `chunk`, `column_vector`, `expression`, `filter`, `project`, `sort`, `limit`, `hash_join`, `nested_loop_join`, `hash_aggregate`, `selection_vector`, `table_scan` |
| Parser | `lexer`, `parser_select`, `parser_select_full`, `parser_expr`, `parser_ddl_dml`, `parse_error` |
| Binder | `bind_create_table`, `bind_insert`, `bind_select_from`, `bind_select_full`, `bind_expression` |
| Planner | `planner`, `planner_agg`, `planner_join` |
| Database | `database`, `database_segment`, `database_wal`, `database_index`, `database_constraint`, `vacuum` |
| Catalog | `catalog` |
| Integration | `scan_e2e`, `server`, `replication` |
