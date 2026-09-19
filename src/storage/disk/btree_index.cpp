#include "storage/disk/btree_index.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace nyx {

BTreeIndex::BTreeIndex(IndexFile file) : file_(std::move(file)), key_size_(file_.key_size()) {}

Result<BTreeIndex> BTreeIndex::create(const std::string& path, std::vector<IndexColSpec> cols,
                                      std::vector<u8> col_indices, bool is_unique) {
    auto f = IndexFile::create(path, std::move(cols), std::move(col_indices), is_unique);
    if (!f.is_ok())
        return Result<BTreeIndex>::err(f.error().message);
    return Result<BTreeIndex>::ok(BTreeIndex(std::move(f.value())));
}

Result<BTreeIndex> BTreeIndex::open(const std::string& path) {
    auto f = IndexFile::open(path);
    if (!f.is_ok())
        return Result<BTreeIndex>::err(f.error().message);
    return Result<BTreeIndex>::ok(BTreeIndex(std::move(f.value())));
}

Result<void> BTreeIndex::flush() {
    return file_.flush();
}
Result<void> BTreeIndex::reset_tree() {
    return file_.reset_tree();
}

void BTreeIndex::encode_key(const std::vector<Value>& vals, byte* out) const {
    const auto& specs = file_.col_specs();
    usize off = 0;
    for (usize i = 0; i < specs.size(); ++i) {
        const auto& s = specs[i];
        const Value& v = (i < vals.size()) ? vals[i] : Value{};
        usize vb = type_size(s.type, s.max_len);

        if (nyx::is_null(v)) {
            out[off++] = 0x00u;
            std::memset(out + off, 0, vb);
            off += vb;
            continue;
        }
        out[off++] = 0x01u;
        switch (s.type) {
        case TypeId::INT32: {
            i32 val = std::get<i32>(v);
            std::memcpy(out + off, &val, 4);
            break;
        }
        case TypeId::DATE: {
            i32 val = std::get<Date>(v).days;
            std::memcpy(out + off, &val, 4);
            break;
        }
        case TypeId::INT64: {
            i64 val = std::get<i64>(v);
            std::memcpy(out + off, &val, 8);
            break;
        }
        case TypeId::TIMESTAMP: {
            i64 val = std::get<Timestamp>(v).micros;
            std::memcpy(out + off, &val, 8);
            break;
        }
        case TypeId::DOUBLE: {
            f64 val = std::get<f64>(v);
            std::memcpy(out + off, &val, 8);
            break;
        }
        case TypeId::VARCHAR: {
            const auto& str = std::get<std::string>(v);
            u16 len = static_cast<u16>(std::min(str.size(), static_cast<usize>(s.max_len)));
            std::memcpy(out + off, &len, 2);
            std::memcpy(out + off + 2, str.data(), len);
            if (len < s.max_len)
                std::memset(out + off + 2 + len, 0, s.max_len - len);
            break;
        }
        default:
            break;
        }
        off += vb;
    }
}

int BTreeIndex::compare_keys(const byte* a, const byte* b) const {
    const auto& specs = file_.col_specs();
    usize off = 0;
    for (const auto& s : specs) {
        u8 an = a[off], bn = b[off];
        off += 1;
        usize vb = type_size(s.type, s.max_len);

        if (an != bn) {
            off += vb;
            return (an < bn) ? -1 : 1;
        }
        if (an == 0x00u) {
            off += vb;
            continue;
        }

        int cmp = 0;
        switch (s.type) {
        case TypeId::INT32:
        case TypeId::DATE: {
            i32 va, vb2;
            std::memcpy(&va, a + off, 4);
            std::memcpy(&vb2, b + off, 4);
            cmp = (va < vb2) ? -1 : (va > vb2) ? 1 : 0;
            break;
        }
        case TypeId::INT64:
        case TypeId::TIMESTAMP: {
            i64 va, vb2;
            std::memcpy(&va, a + off, 8);
            std::memcpy(&vb2, b + off, 8);
            cmp = (va < vb2) ? -1 : (va > vb2) ? 1 : 0;
            break;
        }
        case TypeId::DOUBLE: {
            f64 va, vb2;
            std::memcpy(&va, a + off, 8);
            std::memcpy(&vb2, b + off, 8);
            cmp = (va < vb2) ? -1 : (va > vb2) ? 1 : 0;
            break;
        }
        case TypeId::VARCHAR: {
            u16 la, lb;
            std::memcpy(&la, a + off, 2);
            std::memcpy(&lb, b + off, 2);
            u16 mn = std::min(la, lb);
            cmp = std::memcmp(a + off + 2, b + off + 2, mn);
            if (cmp == 0)
                cmp = (la < lb) ? -1 : (la > lb) ? 1 : 0;
            break;
        }
        default:
            break;
        }
        off += vb;
        if (cmp != 0)
            return cmp;
    }
    return 0;
}

byte* BTreeIndex::leaf_entry_at(byte* p, u16 slot) const {
    return p + INDEX_PAGE_HDR_SIZE + 8u + static_cast<usize>(slot) * (key_size_ + 8u);
}

const byte* BTreeIndex::leaf_entry_at(const byte* p, u16 slot) const {
    return p + INDEX_PAGE_HDR_SIZE + 8u + static_cast<usize>(slot) * (key_size_ + 8u);
}

byte* BTreeIndex::leaf_rowid_at(byte* p, u16 slot) const {
    return leaf_entry_at(p, slot) + key_size_;
}

u64 BTreeIndex::leaf_right_sibling(const byte* p) const {
    u64 v;
    std::memcpy(&v, p + INDEX_PAGE_HDR_SIZE, 8);
    return v;
}

void BTreeIndex::set_leaf_right_sibling(byte* p, u64 sib) const {
    std::memcpy(p + INDEX_PAGE_HDR_SIZE, &sib, 8);
}

byte* BTreeIndex::internal_child_at(byte* p, u16 i) const {
    return p + INDEX_PAGE_HDR_SIZE + static_cast<usize>(i) * 8u;
}

const byte* BTreeIndex::internal_child_at(const byte* p, u16 i) const {
    return p + INDEX_PAGE_HDR_SIZE + static_cast<usize>(i) * 8u;
}

byte* BTreeIndex::internal_key_at(byte* p, u16 i) const {
    return p + INDEX_PAGE_HDR_SIZE + (static_cast<usize>(file_.node_capacity()) + 1u) * 8u +
           static_cast<usize>(i) * key_size_;
}

const byte* BTreeIndex::internal_key_at(const byte* p, u16 i) const {
    return p + INDEX_PAGE_HDR_SIZE + (static_cast<usize>(file_.node_capacity()) + 1u) * 8u +
           static_cast<usize>(i) * key_size_;
}

u64 BTreeIndex::get_child(const byte* p, u16 i) const {
    u64 v;
    std::memcpy(&v, internal_child_at(p, i), 8);
    return v;
}

void BTreeIndex::set_child(byte* p, u16 i, u64 child) const {
    std::memcpy(internal_child_at(p, i), &child, 8);
}

u16 BTreeIndex::find_leaf_insert_slot(const byte* p, u16 count, const byte* key) const {
    u16 lo = 0, hi = count;
    while (lo < hi) {
        u16 mid = static_cast<u16>((lo + hi) / 2u);
        if (compare_keys(leaf_key_at(p, mid), key) <= 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

u16 BTreeIndex::find_child_slot(const byte* p, u16 n_keys, const byte* key) const {
    u16 lo = 0, hi = n_keys;
    while (lo < hi) {
        u16 mid = static_cast<u16>((lo + hi) / 2u);
        if (compare_keys(internal_key_at(p, mid), key) <= 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

u16 BTreeIndex::find_leaf_scan_start(const byte* p, u16 count, const byte* lo) const {
    u16 left = 0, right = count;
    while (left < right) {
        u16 mid = static_cast<u16>((left + right) / 2u);
        if (compare_keys(leaf_key_at(p, mid), lo) < 0)
            left = mid + 1;
        else
            right = mid;
    }
    return left;
}

void BTreeIndex::insert_leaf_entry(byte* p, u16 slot, const byte* key, u64 row_id,
                                   u16 count) const {
    usize entry_sz = key_size_ + 8u;
    byte* base = leaf_entry_at(p, 0);
    if (slot < count)
        std::memmove(base + (slot + 1) * entry_sz, base + slot * entry_sz,
                     static_cast<usize>(count - slot) * entry_sz);
    std::memcpy(base + slot * entry_sz, key, key_size_);
    std::memcpy(base + slot * entry_sz + key_size_, &row_id, 8);
}

void BTreeIndex::insert_internal_entry(byte* p, u16 slot, const byte* sep_key, u64 right_child,
                                       u16 n_keys) const {
    if (slot <= n_keys) {
        std::memmove(internal_child_at(p, slot + 2), internal_child_at(p, slot + 1),
                     static_cast<usize>(n_keys - slot) * 8u);
    }
    set_child(p, slot + 1, right_child);

    if (slot < n_keys) {
        std::memmove(internal_key_at(p, slot + 1), internal_key_at(p, slot),
                     static_cast<usize>(n_keys - slot) * key_size_);
    }
    std::memcpy(internal_key_at(p, slot), sep_key, key_size_);
}

Result<std::pair<PageId, std::vector<BTreeIndex::PathEntry>>>
BTreeIndex::find_leaf_(const byte* key) {
    std::vector<PathEntry> path;
    PageId cur = file_.root_page_id();

    while (true) {
        auto h_r = file_.fetch_page(cur);
        if (!h_r.is_ok())
            return Result<std::pair<PageId, std::vector<PathEntry>>>::err(h_r.error().message);
        auto h = std::move(h_r.value());

        const auto* hdr = reinterpret_cast<const IndexPageHeader*>(h.get()->payload());
        if (hdr->page_type == IDX_PAGE_LEAF)
            return Result<std::pair<PageId, std::vector<PathEntry>>>::ok({cur, std::move(path)});

        u16 n_keys = hdr->entry_count;
        u16 slot = find_child_slot(h.get()->payload(), n_keys, key);
        path.push_back({cur, slot});
        u64 child = get_child(h.get()->payload(), slot);
        cur = child;
    }
}

Result<std::pair<std::vector<byte>, PageId>>
BTreeIndex::split_leaf_(PageId leaf_id, u16 insert_slot, const byte* key, u64 row_id) {
    usize entry_sz = key_size_ + 8u;
    usize N = file_.node_capacity();
    usize total = N + 1u;

    auto lh_r = file_.fetch_page(leaf_id);
    if (!lh_r.is_ok())
        return Result<std::pair<std::vector<byte>, PageId>>::err(lh_r.error().message);
    auto lh = std::move(lh_r.value());
    Page& lp = *lh;

    u64 old_sibling = leaf_right_sibling(lp.payload());

    std::vector<byte> tmp(total * entry_sz, 0);

    usize before = insert_slot;
    usize after = N - insert_slot;

    std::memcpy(tmp.data(), leaf_entry_at(lp.payload(), 0), before * entry_sz);

    byte* ins = tmp.data() + before * entry_sz;
    std::memcpy(ins, key, key_size_);
    std::memcpy(ins + key_size_, &row_id, 8);

    if (after > 0)
        std::memcpy(tmp.data() + (before + 1u) * entry_sz, leaf_entry_at(lp.payload(), insert_slot),
                    after * entry_sz);

    usize mid = total / 2u;

    auto* lhdr = reinterpret_cast<IndexPageHeader*>(lp.payload());
    lhdr->entry_count = static_cast<u16>(mid);
    std::memcpy(leaf_entry_at(lp.payload(), 0), tmp.data(), mid * entry_sz);
    if (N > mid)
        std::memset(leaf_entry_at(lp.payload(), static_cast<u16>(mid)), 0, (N - mid) * entry_sz);

    PageId right_id = file_.reserve_page_id();

    set_leaf_right_sibling(lp.payload(), right_id);

    auto wl = file_.write_page(lp);
    if (!wl.is_ok())
        return Result<std::pair<std::vector<byte>, PageId>>::err(wl.error().message);

    Page rp{};
    rp.reset(right_id);
    auto* rhdr = reinterpret_cast<IndexPageHeader*>(rp.payload());
    rhdr->page_type = IDX_PAGE_LEAF;
    rhdr->flags = 0;
    rhdr->entry_count = static_cast<u16>(total - mid);
    rhdr->capacity = file_.node_capacity();

    set_leaf_right_sibling(rp.payload(), old_sibling);
    std::memcpy(leaf_entry_at(rp.payload(), 0), tmp.data() + mid * entry_sz,
                (total - mid) * entry_sz);

    auto wr = file_.write_page(rp);
    if (!wr.is_ok())
        return Result<std::pair<std::vector<byte>, PageId>>::err(wr.error().message);

    std::vector<byte> sep(key_size_);
    std::memcpy(sep.data(), tmp.data() + mid * entry_sz, key_size_);

    return Result<std::pair<std::vector<byte>, PageId>>::ok({std::move(sep), right_id});
}

Result<std::pair<std::vector<byte>, PageId>>
BTreeIndex::split_internal_(PageId page_id, u16 slot, const byte* sep_key, u64 right_child) {
    usize cap = file_.node_capacity();

    auto ih_r = file_.fetch_page(page_id);
    if (!ih_r.is_ok())
        return Result<std::pair<std::vector<byte>, PageId>>::err(ih_r.error().message);
    auto ih = std::move(ih_r.value());
    Page& ip = *ih;

    usize total_k = cap + 1u;
    usize total_c = cap + 2u;

    std::vector<byte> tmp_keys(total_k * key_size_, 0);
    std::vector<u64> tmp_children(total_c, 0);

    auto* hdr = reinterpret_cast<const IndexPageHeader*>(ip.payload());
    usize n_keys = hdr->entry_count;

    for (usize i = 0; i <= n_keys; ++i)
        tmp_children[i] = get_child(ip.payload(), static_cast<u16>(i));

    for (usize i = 0; i < n_keys; ++i)
        std::memcpy(tmp_keys.data() + i * key_size_,
                    internal_key_at(ip.payload(), static_cast<u16>(i)), key_size_);

    if (slot < n_keys)
        std::memmove(tmp_keys.data() + (slot + 1u) * key_size_, tmp_keys.data() + slot * key_size_,
                     (n_keys - slot) * key_size_);
    std::memcpy(tmp_keys.data() + slot * key_size_, sep_key, key_size_);

    for (usize i = total_c - 1; i > slot + 1u; --i)
        tmp_children[i] = tmp_children[i - 1u];
    tmp_children[slot + 1u] = right_child;

    usize mid = total_k / 2u;

    auto* lhdr = reinterpret_cast<IndexPageHeader*>(ip.payload());
    lhdr->entry_count = static_cast<u16>(mid);
    std::memset(internal_child_at(ip.payload(), 0), 0, (cap + 1u) * 8u);
    std::memset(internal_key_at(ip.payload(), 0), 0, cap * key_size_);

    for (usize i = 0; i <= mid; ++i)
        set_child(ip.payload(), static_cast<u16>(i), tmp_children[i]);
    for (usize i = 0; i < mid; ++i)
        std::memcpy(internal_key_at(ip.payload(), static_cast<u16>(i)),
                    tmp_keys.data() + i * key_size_, key_size_);

    auto wl = file_.write_page(ip);
    if (!wl.is_ok())
        return Result<std::pair<std::vector<byte>, PageId>>::err(wl.error().message);

    std::vector<byte> pushed(key_size_);
    std::memcpy(pushed.data(), tmp_keys.data() + mid * key_size_, key_size_);

    PageId right_id = file_.reserve_page_id();
    Page rp{};
    rp.reset(right_id);

    usize right_keys = total_k - mid - 1u;
    auto* rhdr = reinterpret_cast<IndexPageHeader*>(rp.payload());
    rhdr->page_type = IDX_PAGE_INTERNAL;
    rhdr->flags = 0;
    rhdr->entry_count = static_cast<u16>(right_keys);
    rhdr->capacity = file_.node_capacity();

    for (usize i = 0; i <= right_keys; ++i)
        set_child(rp.payload(), static_cast<u16>(i), tmp_children[mid + 1u + i]);
    for (usize i = 0; i < right_keys; ++i)
        std::memcpy(internal_key_at(rp.payload(), static_cast<u16>(i)),
                    tmp_keys.data() + (mid + 1u + i) * key_size_, key_size_);

    auto wr = file_.write_page(rp);
    if (!wr.is_ok())
        return Result<std::pair<std::vector<byte>, PageId>>::err(wr.error().message);

    return Result<std::pair<std::vector<byte>, PageId>>::ok({std::move(pushed), right_id});
}

Result<void> BTreeIndex::insert(const std::vector<Value>& key_vals, u64 row_id) {
    std::vector<byte> key(file_.key_size());
    encode_key(key_vals, key.data());

    if (file_.root_page_id() == 0) {
        auto dr = file_.set_dirty(true);
        if (!dr.is_ok())
            return dr;

        PageId leaf_id = file_.reserve_page_id();
        Page lp{};
        lp.reset(leaf_id);

        auto* hdr = reinterpret_cast<IndexPageHeader*>(lp.payload());
        hdr->page_type = IDX_PAGE_LEAF;
        hdr->flags = IDX_FLAG_IS_ROOT;
        hdr->entry_count = 1;
        hdr->capacity = file_.node_capacity();

        set_leaf_right_sibling(lp.payload(), INVALID_PAGE_ID);
        std::memcpy(leaf_entry_at(lp.payload(), 0), key.data(), key_size_);
        std::memcpy(leaf_rowid_at(lp.payload(), 0), &row_id, 8);

        auto wr = file_.write_page(lp);
        if (!wr.is_ok())
            return wr;

        file_.set_root_page_id(leaf_id);
        file_.set_entry_count(1);
        return Result<void>::ok();
    }

    if (!file_.is_dirty()) {
        auto dr = file_.set_dirty(true);
        if (!dr.is_ok())
            return dr;
    }

    auto fp_r = find_leaf_(key.data());
    if (!fp_r.is_ok())
        return Result<void>::err(fp_r.error().message);
    auto [leaf_id, path] = std::move(fp_r.value());

    u16 insert_slot;
    bool need_split;
    {
        auto lh_r = file_.fetch_page(leaf_id);
        if (!lh_r.is_ok())
            return Result<void>::err(lh_r.error().message);
        auto lh = std::move(lh_r.value());

        const auto* hdr = reinterpret_cast<const IndexPageHeader*>(lh.get()->payload());
        u16 count = hdr->entry_count;
        insert_slot = find_leaf_insert_slot(lh.get()->payload(), count, key.data());

        if (file_.is_unique() && insert_slot > 0) {
            const byte* prev = leaf_key_at(lh.get()->payload(), insert_slot - 1);
            if (compare_keys(prev, key.data()) == 0)
                return Result<void>::err("unique constraint violated: duplicate key");
        }

        need_split = (count >= hdr->capacity);

        if (!need_split) {
            auto* mhdr = reinterpret_cast<IndexPageHeader*>(lh.get()->payload());
            insert_leaf_entry(lh.get()->payload(), insert_slot, key.data(), row_id, count);
            mhdr->entry_count = count + 1;
            auto wr = file_.write_page(*lh.get());
            if (!wr.is_ok())
                return wr;
            file_.set_entry_count(file_.entry_count() + 1);
            return Result<void>::ok();
        }
    }

    auto sr = split_leaf_(leaf_id, insert_slot, key.data(), row_id);
    if (!sr.is_ok())
        return Result<void>::err(sr.error().message);
    auto [sep_key, right_id] = std::move(sr.value());

    PageId cur_right = right_id;
    std::vector<byte> cur_sep = std::move(sep_key);

    while (!path.empty()) {
        auto [parent_id, child_slot] = path.back();
        path.pop_back();

        auto ph_r = file_.fetch_page(parent_id);
        if (!ph_r.is_ok())
            return Result<void>::err(ph_r.error().message);
        auto ph = std::move(ph_r.value());

        auto* phdr = reinterpret_cast<IndexPageHeader*>(ph.get()->payload());
        u16 n_keys = phdr->entry_count;

        if (n_keys < phdr->capacity) {
            insert_internal_entry(ph.get()->payload(), child_slot, cur_sep.data(), cur_right,
                                  n_keys);
            phdr->entry_count = n_keys + 1;
            auto wr = file_.write_page(*ph.get());
            if (!wr.is_ok())
                return wr;
            return Result<void>::ok();
        }

        auto ist_r = split_internal_(parent_id, child_slot, cur_sep.data(), cur_right);
        if (!ist_r.is_ok())
            return Result<void>::err(ist_r.error().message);
        auto [new_sep, new_right] = std::move(ist_r.value());
        cur_sep = std::move(new_sep);
        cur_right = new_right;
    }

    PageId old_root = file_.root_page_id();
    PageId new_root_id = file_.reserve_page_id();

    Page nrp{};
    nrp.reset(new_root_id);
    auto* nrhdr = reinterpret_cast<IndexPageHeader*>(nrp.payload());
    nrhdr->page_type = IDX_PAGE_INTERNAL;
    nrhdr->flags = IDX_FLAG_IS_ROOT;
    nrhdr->entry_count = 1;
    nrhdr->capacity = file_.node_capacity();

    set_child(nrp.payload(), 0, old_root);
    set_child(nrp.payload(), 1, cur_right);
    std::memcpy(internal_key_at(nrp.payload(), 0), cur_sep.data(), key_size_);

    auto wr = file_.write_page(nrp);
    if (!wr.is_ok())
        return wr;

    auto orp_r = file_.fetch_page(old_root);
    if (!orp_r.is_ok())
        return Result<void>::err(orp_r.error().message);
    auto orp = std::move(orp_r.value());
    reinterpret_cast<IndexPageHeader*>(orp.get()->payload())->flags &= ~IDX_FLAG_IS_ROOT;
    auto wr2 = file_.write_page(*orp.get());
    if (!wr2.is_ok())
        return wr2;

    file_.set_root_page_id(new_root_id);
    return Result<void>::ok();
}

void BTreeIndex::range_scan(const byte* lo, bool lo_incl, const byte* hi, bool hi_incl,
                            const std::function<bool(u64)>& cb) {
    if (file_.root_page_id() == 0)
        return;

    PageId cur = file_.root_page_id();
    while (true) {
        auto h_r = file_.fetch_page(cur);
        if (!h_r.is_ok())
            return;
        auto h = std::move(h_r.value());

        const auto* hdr = reinterpret_cast<const IndexPageHeader*>(h.get()->payload());
        if (hdr->page_type == IDX_PAGE_LEAF) {
            u16 count = hdr->entry_count;
            u16 start = 0;
            if (lo != nullptr)
                start = find_leaf_scan_start(h.get()->payload(), count, lo);

            for (u16 i = start; i < count; ++i) {
                const byte* k = leaf_key_at(h.get()->payload(), i);

                if (lo != nullptr && !lo_incl && i == start) {
                    if (compare_keys(k, lo) == 0)
                        continue;
                }

                if (hi != nullptr) {
                    int c = compare_keys(k, hi);
                    if (c > 0 || (!hi_incl && c == 0))
                        return;
                }

                u64 rid;
                std::memcpy(&rid, leaf_rowid_at(h.get()->payload(), i), 8);
                if (!cb(rid))
                    return;
            }

            u64 sib = leaf_right_sibling(h.get()->payload());
            if (sib == INVALID_PAGE_ID)
                return;
            cur = sib;

            while (cur != INVALID_PAGE_ID) {
                auto sh_r = file_.fetch_page(cur);
                if (!sh_r.is_ok())
                    return;
                auto sh = std::move(sh_r.value());

                const auto* shdr = reinterpret_cast<const IndexPageHeader*>(sh.get()->payload());
                u16 sc = shdr->entry_count;
                for (u16 i = 0; i < sc; ++i) {
                    const byte* k = leaf_key_at(sh.get()->payload(), i);
                    if (hi != nullptr) {
                        int c = compare_keys(k, hi);
                        if (c > 0 || (!hi_incl && c == 0))
                            return;
                    }
                    u64 rid;
                    std::memcpy(&rid, leaf_rowid_at(sh.get()->payload(), i), 8);
                    if (!cb(rid))
                        return;
                }
                u64 next_sib = leaf_right_sibling(sh.get()->payload());
                cur = next_sib;
            }
            return;
        }

        u16 n_keys = hdr->entry_count;
        u16 slot = (lo != nullptr) ? find_child_slot(h.get()->payload(), n_keys, lo) : 0;
        u64 child = get_child(h.get()->payload(), slot);
        cur = child;
    }
}

Result<void> BTreeIndex::bulk_build(std::vector<std::pair<std::vector<byte>, u64>>& entries) {
    if (entries.empty())
        return Result<void>::ok();

    auto dr = file_.set_dirty(true);
    if (!dr.is_ok())
        return dr;

    usize cap = file_.node_capacity();
    usize fill = std::max<usize>(1u, cap * 3u / 4u);
    usize total = entries.size();

    struct LevelEntry {
        PageId page_id;
        std::vector<byte> first_key;
    };

    std::vector<LevelEntry> level;
    usize i = 0;
    PageId prev_leaf = INVALID_PAGE_ID;

    while (i < total) {
        usize take = std::min(fill, total - i);
        PageId lid = file_.reserve_page_id();

        Page lp{};
        lp.reset(lid);
        auto* lhdr = reinterpret_cast<IndexPageHeader*>(lp.payload());
        lhdr->page_type = IDX_PAGE_LEAF;
        lhdr->flags = 0;
        lhdr->entry_count = static_cast<u16>(take);
        lhdr->capacity = static_cast<u16>(cap);
        set_leaf_right_sibling(lp.payload(), INVALID_PAGE_ID);

        for (usize j = 0; j < take; ++j) {
            byte* ep = leaf_entry_at(lp.payload(), static_cast<u16>(j));
            std::memcpy(ep, entries[i + j].first.data(), key_size_);
            std::memcpy(ep + key_size_, &entries[i + j].second, 8);
        }

        auto wr = file_.write_page(lp);
        if (!wr.is_ok())
            return wr;

        if (prev_leaf != INVALID_PAGE_ID) {
            auto plh_r = file_.fetch_page(prev_leaf);
            if (!plh_r.is_ok())
                return Result<void>::err(plh_r.error().message);
            auto plh = std::move(plh_r.value());
            set_leaf_right_sibling(plh.get()->payload(), lid);
            auto pw = file_.write_page(*plh.get());
            if (!pw.is_ok())
                return pw;
        }

        LevelEntry le;
        le.page_id = lid;
        le.first_key = entries[i].first;
        level.push_back(std::move(le));

        prev_leaf = lid;
        i += take;
    }

    while (level.size() > 1) {
        std::vector<LevelEntry> next_level;
        usize n_children = level.size();
        usize ci = 0;

        while (ci < n_children) {
            usize nc = std::min(cap + 1u, n_children - ci);
            usize nk = nc - 1u;

            PageId nid = file_.reserve_page_id();
            Page np{};
            np.reset(nid);
            auto* nhdr = reinterpret_cast<IndexPageHeader*>(np.payload());
            nhdr->page_type = IDX_PAGE_INTERNAL;
            nhdr->flags = 0;
            nhdr->entry_count = static_cast<u16>(nk);
            nhdr->capacity = static_cast<u16>(cap);

            for (usize j = 0; j < nc; ++j)
                set_child(np.payload(), static_cast<u16>(j), level[ci + j].page_id);
            for (usize j = 0; j < nk; ++j)
                std::memcpy(internal_key_at(np.payload(), static_cast<u16>(j)),
                            level[ci + j + 1u].first_key.data(), key_size_);

            auto wr = file_.write_page(np);
            if (!wr.is_ok())
                return wr;

            LevelEntry le;
            le.page_id = nid;
            le.first_key = level[ci].first_key;
            next_level.push_back(std::move(le));

            ci += nc;
        }

        level = std::move(next_level);
    }

    PageId root_id = level[0].page_id;
    {
        auto rh_r = file_.fetch_page(root_id);
        if (!rh_r.is_ok())
            return Result<void>::err(rh_r.error().message);
        auto rh = std::move(rh_r.value());
        reinterpret_cast<IndexPageHeader*>(rh.get()->payload())->flags |= IDX_FLAG_IS_ROOT;
        auto wr = file_.write_page(*rh.get());
        if (!wr.is_ok())
            return wr;
    }

    file_.set_root_page_id(root_id);
    file_.set_entry_count(static_cast<u64>(total));

    return Result<void>::ok();
}

} // namespace nyx
