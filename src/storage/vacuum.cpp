#include "storage/vacuum.h"

#include "storage/disk/column_file.h"
#include "storage/disk/column_page.h"
#include "storage/disk/segment.h"
#include "storage/disk/table.h"

#include <cerrno>
#include <cstring>
#include <filesystem>

namespace nyx {

namespace fs = std::filesystem;

VacuumStats table_dead_row_stats(const Table* t) {
    VacuumStats stats{0, 0};
    auto lk = t->lock_shared();
    for (const auto& seg : t->segments()) {
        const auto& bm = seg.deleted_bitmap();
        u64 dead = 0;
        for (u64 r = 0; r < seg.meta().row_count; ++r) {
            if (!bm.empty() && ((bm[r / 8] >> (r % 8)) & 1u))
                ++dead;
        }
        stats.dead_rows += dead;
        stats.total_rows += seg.meta().row_count;
    }
    const auto& wb_bm = t->wb_deleted_ref();
    const auto& wb_cols = t->wb_columns_ref();
    u64 wb_total = wb_cols.empty() ? 0 : wb_cols[0].row_count();
    u64 wb_dead = 0;
    for (u64 r = 0; r < wb_total; ++r) {
        if (!wb_bm.empty() && ((wb_bm[r / 8] >> (r % 8)) & 1u))
            ++wb_dead;
    }
    stats.dead_rows += wb_dead;
    stats.total_rows += wb_total;
    return stats;
}

static Result<void> copy_column_filtered(ColumnFile& src, ColumnFile& dst,
                                         const std::vector<u8>& deleted_bm, u64 row_count) {
    TypeId t = src.type();
    Result<void> err = Result<void>::ok();
    u64 row_idx = 0;

    auto scan_r = src.scan([&](const ColumnPage& p) {
        if (err.is_err())
            return;
        u16 n = p.value_count();
        for (u16 i = 0; i < n; ++i, ++row_idx) {
            if (row_idx >= row_count)
                break;
            bool deleted = !deleted_bm.empty() && ((deleted_bm[row_idx / 8] >> (row_idx % 8)) & 1u);
            if (deleted)
                continue;

            if (src.nullable() && p.is_null(i)) {
                err = dst.append_null();
                if (err.is_err())
                    return;
                continue;
            }

            Result<void> r = Result<void>::ok();
            switch (t) {
            case TypeId::INT32: {
                auto v = p.get_i32(i);
                if (v.is_ok())
                    r = dst.append_i32(v.value());
                break;
            }
            case TypeId::INT64: {
                auto v = p.get_i64(i);
                if (v.is_ok())
                    r = dst.append_i64(v.value());
                break;
            }
            case TypeId::DOUBLE: {
                auto v = p.get_f64(i);
                if (v.is_ok())
                    r = dst.append_f64(v.value());
                break;
            }
            case TypeId::BOOL: {
                auto v = p.get_bool(i);
                if (v.is_ok())
                    r = dst.append_bool(v.value());
                break;
            }
            case TypeId::DATE: {
                auto v = p.get_date(i);
                if (v.is_ok())
                    r = dst.append_date(v.value());
                break;
            }
            case TypeId::TIMESTAMP: {
                auto v = p.get_timestamp(i);
                if (v.is_ok())
                    r = dst.append_timestamp(v.value());
                break;
            }
            default: {
                r = dst.append_str(p.get_str(i));
                break;
            }
            }
            if (r.is_err()) {
                err = r;
                return;
            }
        }
    });

    if (scan_r.is_err())
        return scan_r;
    return err;
}

u64 vacuum_table(Table* t) {
    struct SegSnap {
        std::string dir;
        u64 base_row_id;
        u64 row_count;
        std::vector<u8> deleted_bm;
        usize idx;
    };

    std::vector<SegSnap> snaps;
    std::string table_dir;
    Schema schema;
    u64 total_dead = 0;
    u64 total_live = 0;

    {
        auto lk = t->lock_shared();
        const auto& segs = t->segments();
        if (segs.empty())
            return 0;

        table_dir = t->dir();
        schema = t->schema();

        for (usize i = 0; i < segs.size(); ++i) {
            const Segment& s = segs[i];
            const auto& bm = s.deleted_bitmap();
            u64 dead = 0;
            for (u64 r = 0; r < s.meta().row_count; ++r) {
                if (!bm.empty() && ((bm[r / 8] >> (r % 8)) & 1u))
                    ++dead;
            }
            total_dead += dead;
            total_live += s.meta().row_count - dead;
            snaps.push_back({s.dir(), s.meta().base_row_id, s.meta().row_count, bm, i});
        }
    }

    if (total_dead == 0)
        return 0;

    std::string tmp_dir = table_dir + "/seg_vacuum_tmp";
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);
    if (ec)
        return 0;

    for (usize ci = 0; ci < schema.size(); ++ci) {
        const auto& col_schema = schema[ci];
        std::string dst_path = tmp_dir + "/" + col_schema.name + ".col";

        auto dst_r =
            ColumnFile::create(dst_path, col_schema.type, col_schema.nullable, col_schema.max_len);
        if (dst_r.is_err()) {
            fs::remove_all(tmp_dir, ec);
            return 0;
        }
        ColumnFile dst = std::move(dst_r.value());

        for (auto& snap : snaps) {
            std::string src_path = snap.dir + "/" + col_schema.name + ".col";
            auto src_r = ColumnFile::open(src_path);
            if (src_r.is_err()) {
                fs::remove_all(tmp_dir, ec);
                return 0;
            }
            ColumnFile src = std::move(src_r.value());
            auto cr = copy_column_filtered(src, dst, snap.deleted_bm, snap.row_count);
            if (cr.is_err()) {
                fs::remove_all(tmp_dir, ec);
                return 0;
            }
        }

        auto fr = dst.flush();
        if (fr.is_err()) {
            fs::remove_all(tmp_dir, ec);
            return 0;
        }
    }

    u64 new_base = snaps[0].base_row_id;
    std::vector<usize> indices;
    indices.reserve(snaps.size());
    for (const auto& s : snaps)
        indices.push_back(s.idx);

    auto rr = t->replace_segments(indices, tmp_dir, new_base, total_live);
    if (rr.is_err()) {
        fs::remove_all(tmp_dir, ec);
        return 0;
    }

    for (const auto& s : snaps)
        fs::remove_all(s.dir, ec);

    return total_dead;
}

} // namespace nyx
