#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/disk_manager.h"
#include "storage/disk/index_page.h"
#include "storage/disk/page.h"
#include "storage/memory/buffer_pool.h"

#include <memory>
#include <string>
#include <vector>

namespace nyx {

class IndexFile {
  public:
    class PageHandle {
      public:
        PageHandle() = default;
        PageHandle(BufferPool* pool, PageId id, Page* page);
        ~PageHandle();
        PageHandle(const PageHandle&) = delete;
        PageHandle& operator=(const PageHandle&) = delete;
        PageHandle(PageHandle&& o) noexcept;
        PageHandle& operator=(PageHandle&& o) noexcept;

        Page* get() const { return page_; }
        Page& operator*() const { return *page_; }
        Page* operator->() const { return page_; }

      private:
        BufferPool* pool_ = nullptr;
        PageId id_ = 0;
        Page* page_ = nullptr;
    };

    static Result<IndexFile> create(const std::string& path, std::vector<IndexColSpec> cols,
                                    std::vector<u8> col_indices, bool is_unique);
    static Result<IndexFile> open(const std::string& path);

    ~IndexFile() = default;
    IndexFile(const IndexFile&) = delete;
    IndexFile& operator=(const IndexFile&) = delete;
    IndexFile(IndexFile&&) noexcept = default;
    IndexFile& operator=(IndexFile&&) noexcept = default;

    Result<PageId> allocate_node(IndexPageType type);
    PageId reserve_page_id();
    Result<PageHandle> fetch_page(PageId id);
    Result<void> write_page(const Page& page);

    const std::vector<IndexColSpec>& col_specs() const { return cols_; }
    const std::vector<u8>& col_indices() const { return col_indices_; }
    usize key_size() const { return key_size_; }
    u16 node_capacity() const { return capacity_; }
    bool is_unique() const { return is_unique_; }
    PageId root_page_id() const { return root_page_id_; }
    u64 entry_count() const { return entry_count_; }
    bool is_dirty() const { return dirty_; }

    void set_root_page_id(PageId id) { root_page_id_ = id; }
    void set_entry_count(u64 n) { entry_count_ = n; }
    Result<void> set_dirty(bool d);

    Result<void> flush();
    Result<void> fsync();

    Result<void> reset_tree();

  private:
    IndexFile(std::unique_ptr<DiskManager> disk, std::unique_ptr<BufferPool> pool,
              std::vector<IndexColSpec> cols, std::vector<u8> col_indices, bool is_unique,
              PageId root_page_id, u64 entry_count, bool dirty, usize key_size, u16 capacity,
              Page meta_page);

    Result<void> write_meta_page_();

    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<BufferPool> pool_;
    std::vector<IndexColSpec> cols_;
    std::vector<u8> col_indices_;
    bool is_unique_ = false;
    PageId root_page_id_ = 0;
    u64 entry_count_ = 0;
    bool dirty_ = false;
    usize key_size_ = 0;
    u16 capacity_ = 0;
    Page meta_page_;
};

} // namespace nyx
