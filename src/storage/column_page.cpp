#include "storage/column_page.h"

#include <cstring>

namespace nyx {

void ColumnPage::init(Page& page, TypeId type, bool nullable) {
    ColumnPageHeader* h = reinterpret_cast<ColumnPageHeader*>(page.payload());
    h->type = type;
    h->flags = nullable ? COL_PAGE_FLAG_NULLABLE : 0;
    h->value_count = 0;
    h->capacity = static_cast<u16>(column_page_capacity(type, nullable));
    h->null_bitmap_bytes = nullable ? static_cast<u16>((h->capacity + 7) / 8) : 0;
    std::memset(h->min_bytes, 0, sizeof(h->min_bytes));
    std::memset(h->max_bytes, 0, sizeof(h->max_bytes));
    std::memset(h->reserved, 0, sizeof(h->reserved));
}

ColumnPageHeader* ColumnPage::header() {
    return reinterpret_cast<ColumnPageHeader*>(page_.payload());
}

const ColumnPageHeader* ColumnPage::header() const {
    return reinterpret_cast<const ColumnPageHeader*>(page_.payload());
}

byte* ColumnPage::null_bitmap() {
    return page_.payload() + COL_PAGE_HEADER_SIZE;
}

const byte* ColumnPage::null_bitmap() const {
    return page_.payload() + COL_PAGE_HEADER_SIZE;
}

byte* ColumnPage::value_area() {
    return page_.payload() + COL_PAGE_HEADER_SIZE + header()->null_bitmap_bytes;
}

const byte* ColumnPage::value_area() const {
    return page_.payload() + COL_PAGE_HEADER_SIZE + header()->null_bitmap_bytes;
}

TypeId ColumnPage::type() const {
    return header()->type;
}

u16 ColumnPage::value_count() const {
    return header()->value_count;
}

u16 ColumnPage::capacity() const {
    return header()->capacity;
}

bool ColumnPage::nullable() const {
    return (header()->flags & COL_PAGE_FLAG_NULLABLE) != 0;
}

bool ColumnPage::is_full() const {
    return header()->value_count >= header()->capacity;
}

bool ColumnPage::is_null(u16 slot) const {
    const ColumnPageHeader* h = header();
    if (h->null_bitmap_bytes == 0)
        return false;
    if (slot >= h->value_count)
        return false;
    return (null_bitmap()[slot / 8] >> (slot % 8)) & 1u;
}

static void clear_null_bit(byte* bitmap, u16 slot) {
    bitmap[slot / 8] &= static_cast<byte>(~(1u << (slot % 8)));
}

static void set_null_bit(byte* bitmap, u16 slot) {
    bitmap[slot / 8] |= static_cast<byte>(1u << (slot % 8));
}

template <typename T>
static Result<void> append_typed(ColumnPage& self, ColumnPageHeader* h, byte* values, byte* bitmap,
                                 TypeId expected, T v) {
    if (h->type != expected)
        return Result<void>::err("append: type mismatch");
    if (h->value_count >= h->capacity)
        return Result<void>::err("append: page full");

    std::memcpy(values + h->value_count * sizeof(T), &v, sizeof(T));
    if (h->null_bitmap_bytes > 0)
        clear_null_bit(bitmap, h->value_count);
    h->value_count++;
    (void)self;
    return Result<void>::ok();
}

Result<void> ColumnPage::append_i32(i32 v) {
    return append_typed<i32>(*this, header(), value_area(), null_bitmap(), TypeId::INT32, v);
}

Result<void> ColumnPage::append_i64(i64 v) {
    return append_typed<i64>(*this, header(), value_area(), null_bitmap(), TypeId::INT64, v);
}

Result<void> ColumnPage::append_f64(f64 v) {
    return append_typed<f64>(*this, header(), value_area(), null_bitmap(), TypeId::DOUBLE, v);
}

Result<void> ColumnPage::append_null() {
    ColumnPageHeader* h = header();
    if (h->null_bitmap_bytes == 0)
        return Result<void>::err("append_null: column is not nullable");
    if (h->value_count >= h->capacity)
        return Result<void>::err("append_null: page full");

    set_null_bit(null_bitmap(), h->value_count);
    h->value_count++;
    return Result<void>::ok();
}

template <typename T>
static Result<T> get_typed(const ColumnPage& self, const ColumnPageHeader* h, const byte* values,
                           TypeId expected, u16 slot) {
    if (h->type != expected)
        return Result<T>::err("get: type mismatch");
    if (slot >= h->value_count)
        return Result<T>::err("get: slot out of range");
    if (self.is_null(slot))
        return Result<T>::err("get: value is null");

    T v;
    std::memcpy(&v, values + slot * sizeof(T), sizeof(T));
    return Result<T>::ok(v);
}

Result<i32> ColumnPage::get_i32(u16 slot) const {
    return get_typed<i32>(*this, header(), value_area(), TypeId::INT32, slot);
}

Result<i64> ColumnPage::get_i64(u16 slot) const {
    return get_typed<i64>(*this, header(), value_area(), TypeId::INT64, slot);
}

Result<f64> ColumnPage::get_f64(u16 slot) const {
    return get_typed<f64>(*this, header(), value_area(), TypeId::DOUBLE, slot);
}

} // namespace nyx
