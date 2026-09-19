#pragma once

#include "common/result.h"
#include "storage/disk/btree_index.h"
#include "storage/disk/schema.h"
#include "storage/disk/table.h"
#include "storage/disk/value.h"
#include "storage/wal/wal_writer.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nyx {

struct IndexMeta {
    std::string name;
    std::vector<u8> col_indices;
    bool unique = false;
};

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
    Result<u64> insert(const std::string& table_name, const std::vector<std::vector<Value>>& rows);
    Result<void> drop_table(const std::string& name, bool if_exists = false);
    Result<u64> delete_rows(const std::string& name, const std::vector<u64>& row_indices);
    Result<u64> delete_all(const std::string& name);
    Result<u64> update_rows(const std::string& name, const std::vector<u64>& old_indices,
                            const Schema& schema, const std::vector<std::vector<Value>>& new_rows);
    Result<void> flush_all();

    const std::vector<IndexMeta>& indexes_of(const std::string& table_name) const;
    BTreeIndex* btree_index(const std::string& table_name, const std::string& index_name);
    Result<void> add_index(const std::string& table_name, const std::string& index_name,
                           const std::vector<u8>& col_indices, bool unique);
    Result<void> drop_index(const std::string& table_name, const std::string& index_name);

  private:
    explicit Catalog(std::string data_root);

    Result<void> create_table_(const std::string& name, Schema schema);
    Result<void> ensure_wal_();
    Result<void> bulk_build_index_(BTreeIndex& idx, Table& tbl);
    Result<void> rebuild_index_(BTreeIndex& idx, Table& tbl);

    std::string data_root_;
    std::unordered_map<std::string, Table> tables_;
    std::optional<WalWriter> wal_;
    std::unordered_map<std::string, std::vector<IndexMeta>> index_meta_;
    std::unordered_map<std::string, std::vector<BTreeIndex>> indexes_;
};

} // namespace nyx
