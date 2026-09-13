#pragma once

#include "catalog/catalog.h"
#include "common/result.h"
#include "storage/disk/schema.h"
#include "storage/disk/value.h"

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

  private:
    explicit Database(Catalog catalog);
    Catalog catalog_;
};

} // namespace nyx
