#include "storage/disk/index_file.h"

#include <cstring>
#include <stdexcept>

namespace nyx {

static constexpr usize INDEX_POOL_CAPACITY = 64;

IndexFile::PageHandle::PageHandle(BufferPool* pool, PageId id, Page* page)
    : pool_(pool), id_(id), page_(page) {}

IndexFile::PageHandle::~PageHandle() {
    if (pool_ != nullptr)
        (void)pool_->unpin_page(id_);
}

IndexFile::PageHandle::PageHandle(PageHandle&& o) noexcept
    : pool_(o.pool_), id_(o.id_), page_(o.page_) {
    o.pool_ = nullptr;
    o.page_ = nullptr;
}

IndexFile::PageHandle& IndexFile::PageHandle::operator=(PageHandle&& o) noexcept {
    if (this != &o) {
        if (pool_ != nullptr)
            (void)pool_->unpin_page(id_);
        pool_ = o.pool_;
        id_ = o.id_;
        page_ = o.page_;
        o.pool_ = nullptr;
        o.page_ = nullptr;
    }
    return *this;
}

IndexFile::IndexFile(std::unique_ptr<DiskManager> disk, std::unique_ptr<BufferPool> pool,
                     std::vector<IndexColSpec> cols, std::vector<u8> col_indices, bool is_unique,
                     PageId root_page_id, u64 entry_count, bool dirty, usize key_size, u16 capacity,
                     Page meta_page)
    : disk_(std::move(disk)), pool_(std::move(pool)), cols_(std::move(cols)),
      col_indices_(std::move(col_indices)), is_unique_(is_unique), root_page_id_(root_page_id),
      entry_count_(entry_count), dirty_(dirty), key_size_(key_size), capacity_(capacity),
      meta_page_(std::move(meta_page)) {}

static usize compute_key_size(const std::vector<IndexColSpec>& cols) {
    usize ks = 0;
    for (const auto& c : cols)
        ks += index_col_bytes(c.type, c.max_len);
    return ks;
}

Result<IndexFile> IndexFile::create(const std::string& path, std::vector<IndexColSpec> cols,
                                    std::vector<u8> col_indices, bool is_unique) {
    if (cols.empty())
        return Result<IndexFile>::err("IndexFile::create: no columns");
    if (col_indices.size() != cols.size())
        return Result<IndexFile>::err("IndexFile::create: col_indices size mismatch");

    usize key_size = compute_key_size(cols);
    if (key_size == 0)
        return Result<IndexFile>::err("IndexFile::create: zero key size (unsupported type?)");

    u16 capacity = index_node_capacity(key_size);
    if (capacity == 0)
        return Result<IndexFile>::err("IndexFile::create: key size too large for a single page");

    try {
        auto disk = std::make_unique<DiskManager>(path);
        if (disk->page_count() > 0)
            return Result<IndexFile>::err("IndexFile::create: file already exists");

        PageId meta_id = disk->reserve_page_id();

        Page meta{};
        meta.reset(meta_id);

        auto* m = reinterpret_cast<IndexFileMeta*>(meta.payload());
        m->magic = INDEX_FILE_MAGIC;
        m->dirty = 0;
        m->is_unique = is_unique ? 1u : 0u;
        m->col_count = static_cast<u8>(cols.size());
        m->reserved[0] = 0;
        m->root_page_id = 0;
        m->entry_count = 0;

        byte* after_meta = meta.payload() + sizeof(IndexFileMeta);
        auto* spec_ptr = reinterpret_cast<IndexColSpec*>(after_meta);
        for (usize i = 0; i < cols.size(); ++i)
            spec_ptr[i] = cols[i];

        byte* idx_ptr = after_meta + cols.size() * sizeof(IndexColSpec);
        std::memcpy(idx_ptr, col_indices.data(), col_indices.size());

        auto wr = disk->write_page(meta);
        if (wr.is_err())
            return Result<IndexFile>::err(wr.error().message);

        auto pool = std::make_unique<BufferPool>(INDEX_POOL_CAPACITY, *disk);

        return Result<IndexFile>::ok(IndexFile(std::move(disk), std::move(pool), std::move(cols),
                                               std::move(col_indices), is_unique, 0, 0, false,
                                               key_size, capacity, std::move(meta)));
    } catch (const std::exception& e) {
        return Result<IndexFile>::err(e.what());
    }
}

Result<IndexFile> IndexFile::open(const std::string& path) {
    try {
        auto disk = std::make_unique<DiskManager>(path);
        if (disk->page_count() == 0)
            return Result<IndexFile>::err("IndexFile::open: file is empty");

        Page meta{};
        auto rd = disk->read_page(0, meta);
        if (rd.is_err())
            return Result<IndexFile>::err(rd.error().message);

        const auto* m = reinterpret_cast<const IndexFileMeta*>(meta.payload());
        if (m->magic != INDEX_FILE_MAGIC)
            return Result<IndexFile>::err("IndexFile::open: bad magic");

        u8 col_count = m->col_count;
        bool is_unique = m->is_unique != 0;
        bool dirty = m->dirty != 0;
        PageId root_page = m->root_page_id;
        u64 entry_count = m->entry_count;

        const byte* after_meta = meta.payload() + sizeof(IndexFileMeta);
        const auto* spec_ptr = reinterpret_cast<const IndexColSpec*>(after_meta);
        std::vector<IndexColSpec> cols(col_count);
        for (u8 i = 0; i < col_count; ++i)
            cols[i] = spec_ptr[i];

        const byte* idx_ptr = after_meta + col_count * sizeof(IndexColSpec);
        std::vector<u8> col_indices(col_count);
        std::memcpy(col_indices.data(), idx_ptr, col_count);

        usize key_size = compute_key_size(cols);
        u16 capacity = index_node_capacity(key_size);

        auto pool = std::make_unique<BufferPool>(INDEX_POOL_CAPACITY, *disk);

        return Result<IndexFile>::ok(IndexFile(
            std::move(disk), std::move(pool), std::move(cols), std::move(col_indices), is_unique,
            root_page, entry_count, dirty, key_size, capacity, std::move(meta)));
    } catch (const std::exception& e) {
        return Result<IndexFile>::err(e.what());
    }
}

Result<PageId> IndexFile::allocate_node(IndexPageType type) {
    PageId id = disk_->reserve_page_id();

    Page page{};
    page.reset(id);

    auto* hdr = reinterpret_cast<IndexPageHeader*>(page.payload());
    hdr->page_type = static_cast<u8>(type);
    hdr->flags = 0;
    hdr->entry_count = 0;
    hdr->capacity = capacity_;
    hdr->reserved[0] = 0;
    hdr->reserved[1] = 0;

    if (type == IDX_PAGE_LEAF) {
        byte* payload_after_hdr = page.payload() + INDEX_PAGE_HDR_SIZE;
        u64 invalid = INVALID_PAGE_ID;
        std::memcpy(payload_after_hdr, &invalid, 8);
    }

    auto wr = disk_->write_page(page);
    if (wr.is_err())
        return Result<PageId>::err(wr.error().message);

    return Result<PageId>::ok(id);
}

PageId IndexFile::reserve_page_id() {
    return disk_->reserve_page_id();
}

Result<IndexFile::PageHandle> IndexFile::fetch_page(PageId id) {
    auto p = pool_->fetch_page(id);
    if (p.is_err())
        return Result<PageHandle>::err(p.error().message);
    return Result<PageHandle>::ok(PageHandle(pool_.get(), id, p.value()));
}

Result<void> IndexFile::write_page(const Page& page) {
    return disk_->write_page(page);
}

Result<void> IndexFile::write_meta_page_() {
    auto* m = reinterpret_cast<IndexFileMeta*>(meta_page_.payload());
    m->dirty = dirty_ ? 1u : 0u;
    m->root_page_id = root_page_id_;
    m->entry_count = entry_count_;
    return disk_->write_page(meta_page_);
}

Result<void> IndexFile::set_dirty(bool d) {
    dirty_ = d;
    return write_meta_page_();
}

Result<void> IndexFile::flush() {
    dirty_ = false;
    auto r = write_meta_page_();
    if (r.is_err())
        return r;
    return disk_->fsync();
}

Result<void> IndexFile::fsync() {
    return disk_->fsync();
}

Result<void> IndexFile::reset_tree() {
    auto r = disk_->truncate(1);
    if (r.is_err())
        return r;

    root_page_id_ = 0;
    entry_count_ = 0;
    dirty_ = false;

    pool_ = std::make_unique<BufferPool>(INDEX_POOL_CAPACITY, *disk_);

    return write_meta_page_();
}

} // namespace nyx
