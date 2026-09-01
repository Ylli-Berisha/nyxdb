# nyxdb — Task List

**Direction:** ClickHouse-style OLAP engine. One file per column, immutable parts (planned), background merges, snapshot isolation over the parts list. Not a Postgres-style row-store with MVCC/WAL/undo.

---

## Phase 1 — Foundation ✅

### Scaffold
- [x] `git init` + `.gitignore`
- [x] Vendor spdlog `v1.15.3` → `third_party/spdlog`
- [x] Vendor GoogleTest `v1.15.2` → `third_party/googletest`
- [x] Root `CMakeLists.txt` (C++17, Ninja, ASan/UBSan in Debug)
- [x] `.github/workflows/ci.yml` — build + test + clang-format check

### Common Utilities
- [x] `src/common/types.h` — fixed-size aliases (u8..u64, i8..i64, f32/f64, byte, usize)
- [x] `src/common/result.h` — `Result<T, Error>` for exception-free error handling
- [x] `src/common/logger.h/.cpp` — spdlog wrapper, `NYX_*` macros
- [x] `src/common/xxhash.h` — XXHash64 (page checksums)

### Storage Primitives
- [x] `src/storage/disk/page.h` — `Page`, `PageHeader` (24B), 8 KB pages, `INVALID_PAGE_ID`
- [x] `src/storage/disk/disk_manager.h/.cpp` — `pread`/`pwrite` per-page I/O, `allocate_page`, `reserve_page_id`, `fsync`, move semantics

### Entry Point
- [x] `src/main.cpp` — parses `--port` (1404) / `--data-dir` / `--log-level`, inits logger

---

## Phase 2 — Storage Engine ✅ (mostly)

### Buffer Pool (unused by column-file path, kept for future)
Dual-pool design (fresh + dirty, pointer shuffle, LSM memtable pattern).

- [x] `src/storage/memory/replacer.h` — `Replacer` interface + `FrameId`
- [x] `src/storage/memory/lru_k_replacer.h/.cpp` — LRU-K (K=2) for fresh pool
- [x] `src/storage/memory/buffer_pool.h/.cpp` — Layers 1–3
  - [x] Layer 1: fresh + dirty pools, pointer shuffle, sync flush
  - [x] Layer 2: dirty pool split into A/B halves, 75% high watermark rotation
  - [x] Layer 3: background flush thread, condvar-driven, backpressure on active-empty + flushing
- [x] Tests: `lru_k_replacer_test`, `buffer_pool_test`

Note: `ColumnFile` currently bypasses the buffer pool (uses `DiskManager` directly). The pool sits ready for a future refactor when multiple files share cache memory.

### Columnar Page Format
- [x] `src/storage/disk/type_id.h` — `TypeId` (INT32/INT64/DOUBLE), `type_size()`
- [x] `src/storage/disk/column_page.h/.cpp` — `ColumnPageHeader` (32B), `ColumnPage` view
  - [x] Typed append/get (i32/i64/f64) + append_null / is_null
  - [x] Column stats: `min_i32/i64/f64`, `max_*`, `null_count`, `has_nulls` flag
- [x] Tests: `column_page_test` (17 tests)

### Column File
- [x] `src/storage/disk/column_file.h/.cpp` — one file per column
  - [x] `create` / `open` factories, cached current page, `flush` / `fsync`
  - [x] `rotate_page` uses `reserve_page_id` (skips zero-write per rotation)
  - [x] `append_bulk(vector<Value>)` — variant dispatched once per column
  - [x] `scan(callback)` — page-at-a-time vectorized callback
- [x] Tests: `column_file_test` (14 tests)

### Table (single-part per table, current)
- [x] `src/storage/disk/value.h` — `Value = variant<monostate, i32, i64, f64>`
- [x] `src/storage/disk/schema.h/.cpp` — `Column`, `Schema`, `SchemaFile` binary serializer (`schema.bin`)
- [x] `src/storage/disk/table.h/.cpp`
  - [x] `create` / `open` factories, `data_root/{name}/` layout
  - [x] `insert(row)` and `insert_many(rows)` (transpose + per-column bulk)
  - [x] `column(idx)`, `flush`, `fsync`
- [x] Tests: `table_test` (14 tests)

### Deferred to later phases (see below)
- Multi-part refactor (Phase 2b, ClickHouse-style)
- B+ tree index (Phase 3 or later)
- Lock manager — minimized to parts-list mutex; not a full IS/IX/S/X hierarchy for now
- Zone maps as a separate concept — folded into ColumnPage column stats
- WAL — deferred, may be replaced entirely by atomic-rename part commits

---

## Phase 2b — Multi-Part Refactor (ClickHouse-style)

Refactor `Table` so it owns a collection of immutable **parts**, not a single set of column files.

- [ ] Directory layout: `data_root/{table}/{part_id}/schema.bin + *.col`
- [ ] `Part` = self-contained snapshot of a set of rows (own schema copy + column files)
- [ ] `Table::insert*` — creates a new part per call (or per batch)
- [ ] `Table` maintains an in-memory list of visible parts + a mutex
- [ ] Snapshot isolation: `Table::scan_snapshot()` freezes the visible-parts list
- [ ] Directory-atomic commit: write part to `.tmp` dir, rename to final on completion
- [ ] Tests: create multi-part table, insert into new parts, snapshot mid-insert

---

## Phase 3 — Vectorized Executor (see `EXECUTOR.md` for detail)

The big unlock — turns nyxdb from a storage library into a database.

### v0 — minimum viable query engine
- [ ] `src/executor/chunk.h` — `Chunk` (column-major batch, ~1024 rows)
- [ ] `src/executor/operator.h` — `Operator` interface (pull-based iterator)
- [ ] `src/executor/expression.h/.cpp` — expression tree for filter/project
- [ ] `src/executor/table_scan.h/.cpp` — reads a `Table` into chunks
- [ ] `src/executor/filter.h/.cpp` — predicate on chunks (with zone-map pruning)
- [ ] `src/executor/project.h/.cpp` — column selection + expression eval
- [ ] `src/executor/limit.h/.cpp`
- [ ] Tests: end-to-end `SELECT col1 FROM t WHERE col2 > 5 LIMIT 10`

### v1 — aggregation + joins
- [ ] `src/executor/hash_aggregate.h/.cpp`
- [ ] `src/executor/sort.h/.cpp`
- [ ] `src/executor/hash_join.h/.cpp`
- [ ] Merge scans across parts (once multi-part is in)

---

## Phase 4 — SQL Frontend
- [ ] `src/sql/token.h`, `src/sql/lexer.h/.cpp`
- [ ] `src/sql/ast.h`, `src/sql/parser.h/.cpp`
- [ ] `src/binder/binder.h/.cpp` — resolves table/column names against schema

---

## Phase 5 — Query Optimizer
- [ ] `src/optimizer/logical_plan.h`, `physical_plan.h`
- [ ] Rewrites: predicate pushdown, projection pruning, constant folding, join reorder
- [ ] `cost_model.h/.cpp`, `optimizer.h/.cpp`

---

## Phase 6 — Concurrency & Durability

Only when the executor is real and running concurrent queries.

- [ ] Per-page S/X latches (`std::shared_mutex` on `Page`) — for concurrent scans of hot pages
- [ ] Merge/mutation scheduler — background job to consolidate small parts, apply mutations
- [ ] Atomic-rename part commits (durability primitive) — a fully-written part directory is atomically visible via rename
- [ ] WAL (optional, if we want stronger durability than atomic-rename gives us)
- [ ] Recovery — scan parts dir on startup, discard partial `.tmp` parts

---

## Phase 7 — Network Layer
- [ ] `src/network/pg_wire.h/.cpp` — PostgreSQL wire protocol (client compat)
- [ ] `src/network/connection.h/.cpp`, `server.h/.cpp`
- [ ] Wire main.cpp to listen on port 1404

---

## Phase 8 — Integration & Hardening
- [ ] `tools/tpch_loader.cpp`
- [ ] `tests/integration/tpch/` — TPC-H queries
- [ ] `EXPLAIN` support
- [ ] TPC-H SF1 performance baseline
- [ ] Valgrind + extended ASan runs

---

## Explicitly Deferred / Reconsidered

- **Full MVCC (Postgres-style)** — not needed. ClickHouse-style snapshot isolation over parts is enough.
- **Full LockManager (IS/IX/S/X hierarchy)** — over-engineered for immutable-parts. A parts-list mutex is enough.
- **Undo log** — not needed with immutable parts.
- **ARIES-style WAL** — deferred indefinitely. Atomic-rename may replace it entirely.
- **UPDATE / DELETE** — pushed to "mutations" via async part rewrites (much later).
