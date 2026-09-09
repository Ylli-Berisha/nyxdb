#pragma once

#include "common/result.h"
#include "storage/disk/schema.h"
#include "storage/disk/table.h"

#include <string>
#include <unordered_map>

namespace nyx {

class Catalog {
  public:
    static Result<Catalog> load(const std::string& data_root);

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    Catalog(Catalog&&) noexcept = default;
    Catalog& operator=(Catalog&&) noexcept = default;

    bool has_table(const std::string& name) const;
    const Schema* schema_of(const std::string& name) const;
    Table* table(const std::string& name);
    const std::string& data_root() const { return data_root_; }
    usize size() const { return tables_.size(); }

    Result<void> add_table(const std::string& name, Schema schema);

  private:
    explicit Catalog(std::string data_root);

    std::string data_root_;
    std::unordered_map<std::string, Table> tables_;
};

} // namespace nyx
