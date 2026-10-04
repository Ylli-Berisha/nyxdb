# nyxdb

A columnar analytical database written from scratch in C++17. Persistent immutable segment storage, a write-ahead log, B+ tree indexes, PRIMARY KEY / UNIQUE constraints, leader-follower replication over QUIC, horizontal range sharding, per-shard replica coordinators for read scaling, and a background merge + vacuum engine.

---

## Contents

- [Features](#features)
- [Architecture](#architecture)
- [SQL Reference](#sql-reference)
- [Getting Started](#getting-started)
- [Server Mode](#server-mode)
- [Replication](#replication)
- [Sharding](#sharding)
- [Read Scaling](#read-scaling)
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
| **Sharding** | Range-based horizontal partitioning, shard coordinator routes SQL, automatic leader-failover notification |
| **Read scaling** | `ReplicaCoordinator` proxy per shard group; SELECTs are round-robin distributed across all replicas, writes forwarded to the current leader |
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
- [Task](https://taskfile.dev) — `sudo snap install task --classic`

### Build

```bash
git clone --recurse-submodules https://github.com/you/nyxdb
cd nyxdb
task build
```

Without Task, or for a debug build (AddressSanitizer + UndefinedBehaviorSanitizer):

```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)
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
task start                              # default token + /tmp/nyxdb data dir
task start TOKEN=supersecret DATA=/var/lib/nyxdb
```

Or directly:

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
| `NOTIFY_LEADER` | node → coordinator/replica-coord | Signals a leader change; payload carries old and new `host:port` |
| `REGISTER_NODE` | node → replica-coord | Registers a node's address and whether it is the current leader |

---

## Replication

nyxdb supports asynchronous leader-follower replication. The leader streams its WAL to followers in real time. A Raft-inspired election protocol handles leader failure.

### Start a replica cluster

```bash
# 3-node cluster (default) — leader on :5001, followers on :5002 :5003
task start-replica-cluster

# 5-node cluster starting at port 6000
task start-replica-cluster NODES=5 BASE_PORT=6000
```

`NODES` controls the total number of nodes. The first is always the initial leader; the rest start as followers. Node `i` gets port `BASE_PORT+i-1` and node-id `127.0.0.i`. The `--peers` list is built automatically from these, so quorum size is always correct.

To start nodes manually:</p>

```bash
# peers must list every node including self
PEERS=node-a:4433,node-b:4434,node-c:4435

./build/nyxdb_server \
    --data-dir /data/node-a --token clustertoken --port 4433 \
    --role leader --node-id node-a --peers $PEERS

./build/nyxdb_server \
    --data-dir /data/node-b --token clustertoken --port 4434 \
    --role follower --node-id node-b --peers $PEERS --leader-addr node-a:4433

./build/nyxdb_server \
    --data-dir /data/node-c --token clustertoken --port 4435 \
    --role follower --node-id node-c --peers $PEERS --leader-addr node-a:4433
```

| Flag | Description |
|---|---|
| `--node-id` | Unique string identifier for this node |
| `--role` | `leader` or `follower` |
| `--peers` | Comma-separated `host:port` list of all nodes including self |
| `--leader-addr` | Current leader address (followers only, used for initial WAL pull) |
| `--max-wal-lag-mb` | Max WAL lag before a follower is considered stale (default 4096 MB) |
| `--coordinator-addr` | Shard coordinator address; when set the node sends `NOTIFY_LEADER` to the coordinator on promotion |
| `--read-coordinator-addr` | Replica coordinator address; when set the node registers itself on startup and notifies the replica coordinator on promotion |

### How it works

- The leader appends every write to a `wal.bin` file and streams new records to followers over QUIC.
- Followers replay incoming WAL records in order, staying behind the leader by a bounded amount.
- **Write forwarding.** Clients that send a write to a follower have it transparently forwarded to the current leader, which then replicates the result back.
- **Snapshots.** A follower that is too far behind (or brand new) requests a full snapshot: the leader sends a consistent copy of all segment files and the current WAL offset.
- **Leader election.** If a follower does not receive a heartbeat within a randomised election timeout (default 3–5 s), it starts an election. Nodes vote for the candidate with the highest WAL LSN. The winner becomes leader for the new term. Election state (term, voted-for) is persisted to survive restarts.

---

## Sharding

nyxdb supports horizontal range sharding. A shard coordinator node holds the partition map and routes SQL to the appropriate shard nodes. Each shard can itself be a replicated leader-follower pair.

### Topology

```
clients
   │
   ▼
shard-coordinator  (--role coordinator)
   │   routes INSERT/SELECT/UPDATE/DELETE by partition key range
   ├── shard-0-leader   (--role leader)
   │   └── shard-0-follower (--role follower)
   └── shard-1-leader   (--role leader)
       └── shard-1-follower (--role follower)
```

### Create a sharded table

```sql
-- Run against the coordinator
CREATE TABLE orders (
    id    INT    NOT NULL,
    price DOUBLE
)
PARTITION BY RANGE (id) (
    PARTITION p0 VALUES LESS THAN (1000) ON '127.0.0.1:4433',
    PARTITION p1 VALUES LESS THAN (MAXVALUE) ON '127.0.0.1:4434'
);
```

The shard coordinator stores the partition map, propagates `CREATE TABLE` to every shard node, and from then on routes every write and read to the correct shard automatically. A `SELECT` without a partition-key predicate fans out to all shards and merges the results.

### Start a sharded cluster with replication

```bash
# 2 shards x 2 replicas (default) — coordinator :5020, shards :5021-:5024
task start-sharded-replica-cluster

# 3 shards x 3 replicas
task start-sharded-replica-cluster SHARDS=3 REPLICAS=3

# Shard-only, no replication (standalone shards)
task start-shard-cluster SHARDS=3
```

`SHARDS` controls the number of shard groups and `REPLICAS` the number of nodes per group. The coordinator always starts at `BASE_PORT`. Shard `s`, replica `r` gets port `BASE_PORT+(s-1)*REPLICAS+r` and node-id `127.s.0.r`, so each shard group has its own isolated `--peers` list. The first replica in each group starts as leader; the rest start as followers pointed at it.

| Flag | Description |
|---|---|
| `--role coordinator` | Node acts as shard coordinator; no local database, routes all SQL |
| `--coordinator-addr` | Address of the shard coordinator (shard nodes only); enables failover notification |

### Automatic leader failover

When a shard follower wins an election it sends a `NOTIFY_LEADER` frame to the shard coordinator (and to the replica coordinator if `--read-coordinator-addr` is set). The coordinator updates the affected partition addresses in-place and evicts the stale shard connection from its pool. Subsequent writes to that partition are routed to the new leader without operator intervention or restart.

---

## Read Scaling

A `ReplicaCoordinator` (`--role read-coordinator`) is a lightweight proxy that sits between the shard coordinator and a shard's replica set. It holds a list of all registered nodes and their roles, distributes `SELECT` queries round-robin across every replica, and forwards writes to the current leader. The shard coordinator's partition map stores the replica coordinator address; it never needs to know which node is the leader.

### Topology with read scaling

```
clients
   │
   ▼
shard-coordinator  (--role coordinator)
   │
   ├── replica-coordinator-0  (--role read-coordinator)
   │       ├── shard-0-leader   (--role leader)
   │       └── shard-0-follower (--role follower)
   └── replica-coordinator-1  (--role read-coordinator)
           ├── shard-1-leader   (--role leader)
           └── shard-1-follower (--role follower)
```

### Create a sharded table routed through replica coordinators

```sql
-- Partition addresses now point to the replica coordinators, not the shard leaders
CREATE TABLE orders (
    id    INT    NOT NULL,
    price DOUBLE
)
PARTITION BY RANGE (id) (
    PARTITION p0 VALUES LESS THAN (1000) ON '127.0.0.1:4437',
    PARTITION p1 VALUES LESS THAN (MAXVALUE) ON '127.0.0.1:4438'
);
```

### Start a cluster with replica coordinators

Replica coordinators are started as separate standalone processes before the shard nodes. Each shard node is then given `--read-coordinator-addr` pointing to its group's replica coordinator, in addition to `--coordinator-addr` for the shard coordinator. There is no dedicated Taskfile task for this topology yet; start the processes manually:

```bash
# One replica coordinator per shard group
./build/nyxdb_server \
    --data-dir /data/rc0 --token shardtoken --port 4437 --role read-coordinator

./build/nyxdb_server \
    --data-dir /data/rc1 --token shardtoken --port 4438 --role read-coordinator

# Shard nodes pass both coordinator addresses
PEERS_S0=127.0.0.1:4433,127.0.0.2:4434

./build/nyxdb_server \
    --data-dir /data/s0-leader --token shardtoken --port 4433 \
    --role leader --node-id 127.0.0.1 --peers $PEERS_S0 \
    --coordinator-addr 127.0.0.1:4440 --read-coordinator-addr 127.0.0.1:4437

./build/nyxdb_server \
    --data-dir /data/s0-follower --token shardtoken --port 4434 \
    --role follower --node-id 127.0.0.2 --peers $PEERS_S0 \
    --leader-addr 127.0.0.1:4433 --read-coordinator-addr 127.0.0.1:4437
```

Each shard node sends a `REGISTER_NODE` frame to its replica coordinator on startup, declaring its address and whether it is currently the leader. On leader election, the new leader sends `NOTIFY_LEADER` to both the shard coordinator and the replica coordinator so both are updated atomically.

| Flag | Description |
|---|---|
| `--role read-coordinator` | Node acts as replica coordinator; no local database, proxies reads and writes |
| `--read-coordinator-addr` | Replica coordinator address (shard nodes only); the node registers on startup and sends `NOTIFY_LEADER` on promotion |

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
task build          # Release (-O3 -march=native)
task format         # clang-format all sources in-place
task test           # run all 65 test suites in parallel
task test FILTER=sharding_replication_mp_test   # run one suite by name
```

Docker:

```bash
task docker-build                              # builds image tagged 'nyxdb'
task docker-build IMAGE=myregistry/nyxdb:1.0
task docker-push  IMAGE=myregistry/nyxdb:1.0
```

For a debug build (ASan + UBSan), invoke cmake directly — there is no task for it since sanitizer builds are incompatible with the release binary used by integration tests:

```bash
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
task test                        # run all suites in parallel
task test FILTER=vacuum_test     # run one suite by name
task test FILTER=sharding        # run all suites whose name contains "sharding"
```

There are 65 test suites spanning every layer:

| Layer | Suites |
|---|---|
| Storage (disk) | `disk_manager`, `column_page`, `column_file`, `segment`, `table`, `truncate`, `schema`, `constraint_file`, `index_file`, `btree_index` |
| Storage (memory) | `buffer_pool`, `lru_k_replacer` |
| WAL | `wal_writer`, `wal_reader` |
| Executor | `chunk`, `column_vector`, `expression`, `filter`, `project`, `sort`, `limit`, `hash_join`, `nested_loop_join`, `hash_aggregate`, `selection_vector`, `table_scan`, `index_scan` |
| Parser | `lexer`, `parser_select`, `parser_select_full`, `parser_expr`, `parser_ddl_dml`, `parse_error` |
| Binder | `bind_create_table`, `bind_insert`, `bind_select_from`, `bind_select_full`, `bind_expression` |
| Planner | `planner`, `planner_agg`, `planner_join` |
| Frontend | `runner` |
| Database | `database`, `database_segment`, `database_wal`, `database_index`, `database_constraint`, `vacuum` |
| Catalog | `catalog` |
| Replication | `follower_registry`, `wal_streamer` |
| Server | `wire`, `session_logic`, `replication_session`, `replica_coordinator` |
| Integration | `scan_e2e`, `pipeline_e2e`, `server`, `replication`, `replication_mp`, `sharding_mp`, `sharding_replication_mp`, `replica_coordinator_mp` |

The `replication_mp` suite spawns real server processes with `fork`/`exec` and exercises leader election, write forwarding, crash recovery, and follower rejoin using `SIGTERM`/`SIGKILL`. The `sharding_replication_mp` suite combines both layers: it runs a full shard coordinator + two replicated shard pairs and verifies that a leader failover triggers a `NOTIFY_LEADER` frame that updates the coordinator's partition map so subsequent writes route to the newly elected leader. The `replica_coordinator_mp` suite adds two `ReplicaCoordinator` processes and verifies round-robin read distribution across replicas, write routing to the leader, and that leader failover propagates to both the shard coordinator and the replica coordinator.
