# nyxdb — Task List

---

## Phase 1 — Foundation

### Scaffold
- [x] `git init` + `.gitignore`
- [x] Vendor spdlog `v1.15.3` → `third_party/spdlog`
- [x] Vendor GoogleTest `v1.15.2` → `third_party/googletest`
- [x] Root `CMakeLists.txt` (C++17, ASan/Debug, Ninja, spdlog + gtest wired)
- [x] `src/CMakeLists.txt`, `tests/CMakeLists.txt` hierarchy
- [x] Verify build configures and compiles clean
- [x] `.github/workflows/ci.yml` (build + test + clang-format check on push)

### Common Utilities
- [x] `src/common/types.h` — fixed-size type aliases
- [x] `src/common/result.h` — `Result<T, Error>`
- [x] `tests/unit/common/result_test.cpp` — 5 passing tests
- [x] `src/common/logger.h` + `logger.cpp` — spdlog wrapper, `NYX_*` macros

### Storage Primitives
- [x] `src/storage/page.h` — `Page` struct, `PageId`, `PAGE_SIZE`
- [x] `src/storage/disk_manager.h` + `disk_manager.cpp` — raw page read/write/allocate
- [x] `tests/unit/storage/disk_manager_test.cpp`

### Entry Point
- [x] `src/main.cpp` — parse config, init logger, placeholder server start

### Exit Criteria
- [x] `cmake --build` clean, all tests pass
- [x] Page written to disk and read back correctly

---

## Phase 2 — Storage Engine

### Buffer Pool Manager — Dual-Pool Design (fresh + dirty, pointer shuffle)

**Layer 1 — Two-pool structure (synchronous)**
- [x] `src/storage/replacer.h` — `Replacer` interface
- [ ] `src/storage/lru_k_replacer.h` + `.cpp` — LRU-K eviction (for fresh pool)
- [ ] `src/storage/buffer_pool.h` + `.cpp` — dual-pool (fresh + dirty), pointer shuffle on dirty, synchronous flush when dirty pool full
- [ ] `tests/unit/storage/lru_k_replacer_test.cpp`
- [ ] `tests/unit/storage/buffer_pool_test.cpp`

**Layer 2 — Double-buffered dirty pool (memtable pattern)**
- [ ] Split dirty pool into `active` + `immutable` halves
- [ ] Swap on threshold, flush `immutable` while `active` accepts new dirties

**Layer 3 — Background flush thread**
- [ ] Watermarks (high/low) on dirty pool
- [ ] Background writer thread, sort dirty pages by offset before flushing
- [ ] Writers only block at critical watermark

**Layer 4 — Bulk insert bypass**
- [ ] Direct-append path for bulk inserts (write straight to new file, skip pool)

### Lock Manager (IS / IX / S / X hierarchical)
- [ ] `src/storage/lock_manager.h` + `.cpp` — table/page/row-level locks, deadlock detection
- [ ] `tests/unit/storage/lock_manager_test.cpp`

### PAX Page Layout
- [ ] `src/storage/pax_page.h` + `.cpp` — column stripes, null bitmaps, row insert/scan
- [ ] `tests/unit/storage/pax_page_test.cpp`

### Heap File
- [ ] `src/storage/heap_file.h` + `.cpp` — ordered PAX pages, insert, full scan iterator
- [ ] `tests/unit/storage/heap_file_test.cpp`

### Zone Maps
- [ ] `src/storage/zone_map.h` + `.cpp` — per-page min/max per column
- [ ] `tests/unit/storage/zone_map_test.cpp`

### B+ Tree Index
- [ ] `src/index/btree_node.h` — internal + leaf node layout
- [ ] `src/index/btree.h` + `.cpp` — insert, lookup, range scan, split/merge
- [ ] `tests/unit/index/btree_test.cpp`

### Write-Ahead Log
- [ ] `src/wal/log_record.h` — BEGIN, COMMIT, ABORT, INSERT, UPDATE, DELETE
- [ ] `src/wal/log_manager.h` + `.cpp` — append, flush, fsync
- [ ] `src/wal/recovery_manager.h` + `.cpp` — redo replay on startup
- [ ] `tests/unit/wal/log_manager_test.cpp`
- [ ] `tests/unit/wal/recovery_test.cpp`

---

## Phase 3 — Catalog
- [ ] `src/catalog/type.h` — `TypeId` enum
- [ ] `src/catalog/column.h`
- [ ] `src/catalog/table_schema.h`
- [ ] `src/catalog/index_info.h`
- [ ] `src/catalog/table_stats.h`
- [ ] `src/catalog/catalog.h` + `.cpp`
- [ ] `src/catalog/catalog_serializer.h` + `.cpp`
- [ ] `tests/unit/catalog/catalog_test.cpp`

---

## Phase 4 — SQL Frontend
- [ ] `src/sql/token.h`
- [ ] `src/sql/lexer.h` + `.cpp`
- [ ] `tests/unit/sql/lexer_test.cpp`
- [ ] `src/sql/ast.h`
- [ ] `src/sql/parser.h` + `.cpp`
- [ ] `tests/unit/sql/parser_test.cpp`
- [ ] `src/binder/binder.h` + `.cpp`
- [ ] `tests/unit/binder/binder_test.cpp`

---

## Phase 5 — Vectorized Executor
- [ ] `src/executor/chunk.h`
- [ ] `src/executor/operator.h`
- [ ] `src/executor/expression.h` + `.cpp`
- [ ] `src/executor/column_scan.h` + `.cpp`
- [ ] `src/executor/filter.h` + `.cpp`
- [ ] `src/executor/project.h` + `.cpp`
- [ ] `src/executor/hash_aggregate.h` + `.cpp`
- [ ] `src/executor/hash_join.h` + `.cpp`
- [ ] `src/executor/sort.h` + `.cpp`
- [ ] `src/executor/limit.h` + `.cpp`
- [ ] `tests/unit/executor/` — per operator
- [ ] `tests/integration/executor/query_test.cpp`

---

## Phase 6 — Query Optimizer
- [ ] `src/optimizer/logical_plan.h`
- [ ] `src/optimizer/physical_plan.h`
- [ ] `src/optimizer/rules/predicate_pushdown.h` + `.cpp`
- [ ] `src/optimizer/rules/projection_pruning.h` + `.cpp`
- [ ] `src/optimizer/rules/constant_folding.h` + `.cpp`
- [ ] `src/optimizer/rules/join_reorder.h` + `.cpp`
- [ ] `src/optimizer/cost_model.h` + `.cpp`
- [ ] `src/optimizer/optimizer.h` + `.cpp`
- [ ] `tests/unit/optimizer/`

---

## Phase 7 — Network Layer
- [ ] `src/network/pg_wire.h` + `.cpp`
- [ ] `src/network/connection.h` + `.cpp`
- [ ] `src/network/server.h` + `.cpp`
- [ ] `tests/integration/network/`

---

## Phase 8 — Integration & Hardening
- [ ] `tools/tpch_loader.cpp`
- [ ] `tests/integration/tpch/` — all 22 queries
- [ ] `EXPLAIN` support
- [ ] WAL stress tests
- [ ] ASan + Valgrind clean pass
- [ ] TPC-H SF1 performance baseline
