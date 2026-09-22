#pragma once

#include "catalog/catalog.h"
#include "common/result.h"
#include "storage/disk/schema.h"
#include "storage/disk/value.h"
#include "storage/merge_worker.h"
#include "storage/wal/wal_reader.h"

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace nyx {

struct ExecuteResult {
    Schema schema;
    std::vector<std::vector<Value>> columns;
    u64 rows_affected = 0;

    size_t row_count() const { return columns.empty() ? 0 : columns[0].size(); }
};

class Database {
  public:
    static Result<Database> open(const std::string& data_root);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept = default;
    Database& operator=(Database&&) noexcept = default;

    Result<ExecuteResult> execute(const std::string& sql);
    Result<void> flush() { return catalog_->flush_all(); }

    u64 current_wal_lsn() const { return catalog_->wal_offset(); }
    std::string wal_path() const { return catalog_->data_root() + "/wal.bin"; }
    std::string data_root() const { return catalog_->data_root(); }
    void set_compaction_gate(std::function<u64()> fn) {
        catalog_->set_compaction_gate(std::move(fn));
    }
    Result<void> apply_replicated_record(const WalRecord& rec);
    Result<void> reopen();

    ~Database();

  private:
    explicit Database(Catalog catalog);
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<MergeWorker> merge_worker_;
    mutable std::unique_ptr<std::shared_mutex> rw_mu_;
};

} // namespace nyx
