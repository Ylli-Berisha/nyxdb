#include "storage/column_file.h"

#include <stdexcept>
#include <utility>

namespace nyx {

ColumnFile::ColumnFile(DiskManager disk, TypeId type, bool nullable, u16 capacity, Page current,
                       PageId current_id, bool current_dirty)
    : disk_(std::move(disk)), type_(type), nullable_(nullable), capacity_(capacity),
      current_page_(std::move(current)), current_page_id_(current_id),
      current_dirty_(current_dirty) {}

Result<ColumnFile> ColumnFile::create(const std::string& path, TypeId type, bool nullable) {
    if (type_size(type) == 0)
        return Result<ColumnFile>::err("create: invalid type");

    try {
        DiskManager disk(path);
        if (disk.page_count() > 0)
            return Result<ColumnFile>::err("create: file " + path + " already has pages");

        u16 capacity = static_cast<u16>(column_page_capacity(type, nullable));

        auto id_res = disk.allocate_page();
        if (id_res.is_err())
            return Result<ColumnFile>::err(id_res.error().message);

        Page page{};
        page.reset(id_res.value());
        ColumnPage::init(page, type, nullable);

        return Result<ColumnFile>::ok(ColumnFile(std::move(disk), type, nullable, capacity,
                                                 std::move(page), id_res.value(), true));
    } catch (const std::exception& e) {
        return Result<ColumnFile>::err(e.what());
    }
}

Result<ColumnFile> ColumnFile::open(const std::string& path) {
    try {
        DiskManager disk(path);
        if (disk.page_count() == 0)
            return Result<ColumnFile>::err("open: file " + path + " is empty");

        PageId last = disk.page_count() - 1;
        Page page{};
        auto read_res = disk.read_page(last, page);
        if (read_res.is_err())
            return Result<ColumnFile>::err(read_res.error().message);

        ColumnPage view(page);
        TypeId type = view.type();
        bool nullable = view.nullable();
        u16 capacity = view.capacity();

        if (type_size(type) == 0)
            return Result<ColumnFile>::err("open: page header has invalid type");

        return Result<ColumnFile>::ok(
            ColumnFile(std::move(disk), type, nullable, capacity, std::move(page), last, false));
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
        auto res = disk_.write_page(current_page_);
        if (res.is_err())
            return res;
        current_dirty_ = false;
    }

    auto id_res = disk_.allocate_page();
    if (id_res.is_err())
        return Result<void>::err(id_res.error().message);

    current_page_.reset(id_res.value());
    ColumnPage::init(current_page_, type_, nullable_);
    current_page_id_ = id_res.value();
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

template <typename T, typename Getter>
static Result<T> get_typed(ColumnFile& self, u64 row_id, u16 capacity, PageId current_page_id,
                           Page& current_page, DiskManager& disk, Getter getter) {
    PageId page_num = row_id / capacity;
    u16 slot = static_cast<u16>(row_id % capacity);

    if (page_num >= disk.page_count())
        return Result<T>::err("get: row_id " + std::to_string(row_id) + " out of range");

    if (page_num == current_page_id) {
        ColumnPage view(current_page);
        return getter(view, slot);
    }

    Page temp{};
    auto res = disk.read_page(page_num, temp);
    if (res.is_err())
        return Result<T>::err(res.error().message);

    ColumnPage view(temp);
    (void)self;
    return getter(view, slot);
}

Result<i32> ColumnFile::get_i32(u64 row_id) {
    return get_typed<i32>(*this, row_id, capacity_, current_page_id_, current_page_, disk_,
                          [](ColumnPage& v, u16 s) { return v.get_i32(s); });
}

Result<i64> ColumnFile::get_i64(u64 row_id) {
    return get_typed<i64>(*this, row_id, capacity_, current_page_id_, current_page_, disk_,
                          [](ColumnPage& v, u16 s) { return v.get_i64(s); });
}

Result<f64> ColumnFile::get_f64(u64 row_id) {
    return get_typed<f64>(*this, row_id, capacity_, current_page_id_, current_page_, disk_,
                          [](ColumnPage& v, u16 s) { return v.get_f64(s); });
}

bool ColumnFile::is_null(u64 row_id) {
    PageId page_num = row_id / capacity_;
    u16 slot = static_cast<u16>(row_id % capacity_);

    if (page_num >= disk_.page_count())
        return false;

    if (page_num == current_page_id_) {
        ColumnPage view(current_page_);
        return view.is_null(slot);
    }

    Page temp{};
    if (disk_.read_page(page_num, temp).is_err())
        return false;
    ColumnPage view(temp);
    return view.is_null(slot);
}

Result<void> ColumnFile::scan(std::function<void(const ColumnPage&)> fn) {
    u64 total = disk_.page_count();
    Page temp{};
    for (PageId id = 0; id < total; ++id) {
        if (id == current_page_id_) {
            ColumnPage view(current_page_);
            fn(view);
        } else {
            auto res = disk_.read_page(id, temp);
            if (res.is_err())
                return res;
            ColumnPage view(temp);
            fn(view);
        }
    }
    return Result<void>::ok();
}

Result<void> ColumnFile::flush() {
    if (!current_dirty_)
        return Result<void>::ok();
    auto res = disk_.write_page(current_page_);
    if (res.is_err())
        return res;
    current_dirty_ = false;
    return Result<void>::ok();
}

Result<void> ColumnFile::fsync() {
    return disk_.fsync();
}

} // namespace nyx
