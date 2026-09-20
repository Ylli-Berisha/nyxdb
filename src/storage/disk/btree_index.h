#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/index_file.h"
#include "storage/disk/value.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nyx {

class BTreeIndex {
  public:
    static Result<BTreeIndex> create(const std::string& path, std::vector<IndexColSpec> cols,
                                     std::vector<u8> col_indices, bool is_unique);
    static Result<BTreeIndex> open(const std::string& path);

    ~BTreeIndex() = default;
    BTreeIndex(const BTreeIndex&) = delete;
    BTreeIndex& operator=(const BTreeIndex&) = delete;
    BTreeIndex(BTreeIndex&&) noexcept = default;
    BTreeIndex& operator=(BTreeIndex&&) noexcept = default;

    void encode_key(const std::vector<Value>& vals, byte* out) const;
    int compare_keys(const byte* a, const byte* b) const;

    Result<void> insert(const std::vector<Value>& key, u64 row_id);

    Result<void> bulk_build(std::vector<std::pair<std::vector<byte>, u64>>& entries);

    void range_scan(const byte* lo, bool lo_incl, const byte* hi, bool hi_incl,
                    const std::function<bool(u64)>& cb);

    const std::vector<IndexColSpec>& col_specs() const { return file_.col_specs(); }
    const std::vector<u8>& col_indices() const { return file_.col_indices(); }
    usize key_size() const { return file_.key_size(); }
    u16 capacity() const { return file_.node_capacity(); }
    bool is_dirty() const { return file_.is_dirty(); }
    bool is_unique() const { return file_.is_unique(); }
    u64 entry_count() const { return file_.entry_count(); }

    Result<void> flush();
    Result<void> reset_tree();

  private:
    explicit BTreeIndex(IndexFile file);

    struct PathEntry {
        PageId page_id;
        u16 child_slot;
    };

    byte* leaf_entry_at(byte* p, u16 slot) const;
    const byte* leaf_entry_at(const byte* p, u16 slot) const;
    byte* leaf_key_at(byte* p, u16 slot) const { return leaf_entry_at(p, slot); }
    const byte* leaf_key_at(const byte* p, u16 slot) const { return leaf_entry_at(p, slot); }
    byte* leaf_rowid_at(byte* p, u16 slot) const;

    u64 leaf_right_sibling(const byte* p) const;
    void set_leaf_right_sibling(byte* p, u64 sib) const;

    byte* internal_child_at(byte* p, u16 i) const;
    const byte* internal_child_at(const byte* p, u16 i) const;
    byte* internal_key_at(byte* p, u16 i) const;
    const byte* internal_key_at(const byte* p, u16 i) const;

    u64 get_child(const byte* p, u16 i) const;
    void set_child(byte* p, u16 i, u64 child) const;

    u16 find_leaf_insert_slot(const byte* payload, u16 count, const byte* key) const;
    u16 find_child_slot(const byte* payload, u16 n_keys, const byte* key) const;
    u16 find_leaf_scan_start(const byte* payload, u16 count, const byte* lo) const;

    Result<std::pair<PageId, std::vector<PathEntry>>> find_leaf_(const byte* key);

    void insert_leaf_entry(byte* payload, u16 slot, const byte* key, u64 row_id, u16 count) const;
    void insert_internal_entry(byte* payload, u16 slot, const byte* sep_key, u64 right_child,
                               u16 n_keys) const;

    Result<void> write_new_leaf(Page& page, u16 entry_count, u64 right_sibling,
                                const byte* entries_src, bool is_root);

    Result<std::pair<std::vector<byte>, PageId>> split_leaf_(PageId leaf_id, u16 insert_slot,
                                                             const byte* key, u64 row_id);

    Result<std::pair<std::vector<byte>, PageId>>
    split_internal_(PageId page_id, u16 slot, const byte* sep_key, u64 right_child);

    IndexFile file_;
    usize key_size_ = 0;
};

} // namespace nyx
