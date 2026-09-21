#pragma once

#include "catalog/catalog.h"
#include "common/result.h"
#include "storage/disk/schema.h"
#include "storage/disk/value.h"
#include "storage/merge_worker.h"

#include <memory>
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

    ~Database();

  private:
    explicit Database(Catalog catalog);
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<MergeWorker> merge_worker_;
};

} // namespace nyx
