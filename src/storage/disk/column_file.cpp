#include "storage/disk/column_file.h"

#include <stdexcept>
#include <utility>

namespace nyx {

static constexpr usize POOL_FRESH_CAPACITY = 64;

ColumnFile::PageHandle::PageHandle(BufferPool* pool, PageId id, Page* page)
    : pool_(pool), id_(id), page_(page) {}

ColumnFile::PageHandle::~PageHandle() {
    if (pool_ != nullptr)
        (void)pool_->unpin_page(id_);
}

ColumnFile::PageHandle::PageHandle(PageHandle&& other) noexcept
    : pool_(other.pool_), id_(other.id_), page_(other.page_) {
    other.pool_ = nullptr;
    other.page_ = nullptr;
}

ColumnFile::PageHandle& ColumnFile::PageHandle::operator=(PageHandle&& other) noexcept {
    if (this != &other) {
        if (pool_ != nullptr)
            (void)pool_->unpin_page(id_);
        pool_ = other.pool_;
        id_ = other.id_;
        page_ = other.page_;
        other.pool_ = nullptr;
        other.page_ = nullptr;
    }
    return *this;
}

ColumnFile::ColumnFile(std::unique_ptr<DiskManager> disk, std::unique_ptr<BufferPool> pool,
                       TypeId type, bool nullable, u16 capacity, Page current, PageId current_id,
                       bool current_dirty)
    : disk_(std::move(disk)), pool_(std::move(pool)), type_(type), nullable_(nullable),
      capacity_(capacity), current_page_(std::move(current)), current_page_id_(current_id),
      current_dirty_(current_dirty) {}

Result<ColumnFile> ColumnFile::create(const std::string& path, TypeId type, bool nullable) {
    if (type_size(type) == 0)
        return Result<ColumnFile>::err("create: invalid type");

    try {
        auto disk = std::make_unique<DiskManager>(path);
        if (disk->page_count() > 0)
            return Result<ColumnFile>::err("create: file " + path + " already has pages");

        u16 capacity = static_cast<u16>(column_page_capacity(type, nullable));

        auto id_res = disk->allocate_page();
        if (id_res.is_err())
            return Result<ColumnFile>::err(id_res.error().message);

        Page page{};
        page.reset(id_res.value());
        ColumnPage::init(page, type, nullable);

        auto pool = std::make_unique<BufferPool>(POOL_FRESH_CAPACITY, *disk);

        return Result<ColumnFile>::ok(ColumnFile(std::move(disk), std::move(pool), type, nullable,
                                                 capacity, std::move(page), id_res.value(), true));
    } catch (const std::exception& e) {
        return Result<ColumnFile>::err(e.what());
    }
}

Result<ColumnFile> ColumnFile::open(const std::string& path) {
    try {
        auto disk = std::make_unique<DiskManager>(path);
        if (disk->page_count() == 0)
            return Result<ColumnFile>::err("open: file " + path + " is empty");

        PageId last = disk->page_count() - 1;
        Page page{};
        auto read_res = disk->read_page(last, page);
        if (read_res.is_err())
            return Result<ColumnFile>::err(read_res.error().message);

        ColumnPage view(page);
        TypeId type = view.type();
        bool nullable = view.nullable();
        u16 capacity = view.capacity();

        if (type_size(type) == 0)
            return Result<ColumnFile>::err("open: page header has invalid type");

        auto pool = std::make_unique<BufferPool>(POOL_FRESH_CAPACITY, *disk);

        return Result<ColumnFile>::ok(ColumnFile(std::move(disk), std::move(pool), type, nullable,
                                                 capacity, std::move(page), last, false));
    } catch (const std::exception& e) {
        return Result<ColumnFile>::err(e.what());
    }
}

u64 ColumnFile::row_count() const {
    ColumnPage view(const_cast<Page&>(current_page_));
    return static_cast<u64>(current_page_id_) * capacity_ + view.value_count();
}

Result<void> ColumnFile::rotate_page() {
    if (current_dirty_) {
        auto res = disk_->write_page(current_page_);
        if (res.is_err())
            return res;
        current_dirty_ = false;
    }

    PageId new_id = disk_->reserve_page_id();
    current_page_.reset(new_id);
    ColumnPage::init(current_page_, type_, nullable_);
    current_page_id_ = new_id;
    current_dirty_ = true;
    return Result<void>::ok();
}

Result<void> ColumnFile::ensure_room_for_append() {
    ColumnPage view(current_page_);
    if (view.is_full())
        return rotate_page();
    return Result<void>::ok();
}

Result<void> ColumnFile::append_i32(i32 v) {
    auto r = ensure_room_for_append();
    if (r.is_err())
        return r;
    ColumnPage view(current_page_);
    auto res = view.append_i32(v);
    if (res.is_err())
        return res;
    current_dirty_ = true;
    return Result<void>::ok();
}

Result<void> ColumnFile::append_i64(i64 v) {
    auto r = ensure_room_for_append();
    if (r.is_err())
        return r;
    ColumnPage view(current_page_);
    auto res = view.append_i64(v);
    if (res.is_err())
        return res;
    current_dirty_ = true;
    return Result<void>::ok();
}

Result<void> ColumnFile::append_f64(f64 v) {
    auto r = ensure_room_for_append();
    if (r.is_err())
        return r;
    ColumnPage view(current_page_);
    auto res = view.append_f64(v);
    if (res.is_err())
        return res;
    current_dirty_ = true;
    return Result<void>::ok();
}

Result<void> ColumnFile::append_null() {
    auto r = ensure_room_for_append();
    if (r.is_err())
        return r;
    ColumnPage view(current_page_);
    auto res = view.append_null();
    if (res.is_err())
        return res;
    current_dirty_ = true;
    return Result<void>::ok();
}

Result<void> ColumnFile::append_bulk(const std::vector<Value>& values) {
    for (const auto& v : values) {
        auto rr = ensure_room_for_append();
        if (rr.is_err())
            return rr;

        ColumnPage view(current_page_);
        Result<void> ar = Result<void>::ok();

        if (nyx::is_null(v)) {
            ar = view.append_null();
        } else if (type_ == TypeId::INT32 && std::holds_alternative<i32>(v)) {
            ar = view.append_i32(std::get<i32>(v));
        } else if (type_ == TypeId::INT64 && std::holds_alternative<i64>(v)) {
            ar = view.append_i64(std::get<i64>(v));
        } else if (type_ == TypeId::DOUBLE && std::holds_alternative<f64>(v)) {
            ar = view.append_f64(std::get<f64>(v));
        } else {
            return Result<void>::err("append_bulk: value type does not match column type");
        }

        if (ar.is_err())
            return ar;
        current_dirty_ = true;
    }
    return Result<void>::ok();
}

template <typename T, typename Getter>
static Result<T> get_typed(ColumnFile& self, u64 row_id, u16 capacity, Getter getter) {
    PageId page_num = row_id / capacity;
    u16 slot = static_cast<u16>(row_id % capacity);

    auto h = self.read_page(page_num);
    if (h.is_err())
        return Result<T>::err(h.error().message);

    ColumnPage view(*h.value());
    return getter(view, slot);
}

Result<i32> ColumnFile::get_i32(u64 row_id) {
    return get_typed<i32>(*this, row_id, capacity_,
                          [](ColumnPage& v, u16 s) { return v.get_i32(s); });
}

Result<i64> ColumnFile::get_i64(u64 row_id) {
    return get_typed<i64>(*this, row_id, capacity_,
                          [](ColumnPage& v, u16 s) { return v.get_i64(s); });
}

Result<f64> ColumnFile::get_f64(u64 row_id) {
    return get_typed<f64>(*this, row_id, capacity_,
                          [](ColumnPage& v, u16 s) { return v.get_f64(s); });
}

bool ColumnFile::is_null(u64 row_id) {
    PageId page_num = row_id / capacity_;
    u16 slot = static_cast<u16>(row_id % capacity_);

    if (page_num >= disk_->page_count())
        return false;

    auto h = read_page(page_num);
    if (h.is_err())
        return false;
    ColumnPage view(*h.value());
    return view.is_null(slot);
}

Result<void> ColumnFile::scan(std::function<void(const ColumnPage&)> fn) {
    u64 total = disk_->page_count();
    for (PageId id = 0; id < total; ++id) {
        auto h = read_page(id);
        if (h.is_err())
            return Result<void>::err(h.error().message);
        ColumnPage view(*h.value());
        fn(view);
    }
    return Result<void>::ok();
}

Result<ColumnFile::PageHandle> ColumnFile::read_page(PageId id) {
    if (id == current_page_id_)
        return Result<PageHandle>::ok(PageHandle(nullptr, id, &current_page_));
    auto p = pool_->fetch_page(id);
    if (p.is_err())
        return Result<PageHandle>::err(p.error().message);
    return Result<PageHandle>::ok(PageHandle(pool_.get(), id, p.value()));
}

Result<void> ColumnFile::flush() {
    if (!current_dirty_)
        return Result<void>::ok();
    auto res = disk_->write_page(current_page_);
    if (res.is_err())
        return res;
    current_dirty_ = false;
    return Result<void>::ok();
}

Result<void> ColumnFile::fsync() {
    return disk_->fsync();
}

} // namespace nyx
