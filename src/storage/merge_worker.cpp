#include "storage/merge_worker.h"

#include "catalog/catalog.h"
#include "storage/disk/column_file.h"
#include "storage/disk/column_page.h"
#include "storage/disk/segment.h"
#include "storage/disk/table.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace nyx {

namespace fs = std::filesystem;

MergeWorker::MergeWorker(Catalog* catalog, u32 merge_threshold)
    : catalog_(catalog), merge_threshold_(merge_threshold) {}

MergeWorker::~MergeWorker() {
    stop();
}

void MergeWorker::start() {
    stop_flag_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { loop_(); });
}

void MergeWorker::stop() {
    if (!thread_.joinable())
        return;
    {
        std::lock_guard lk(mu_);
        stop_flag_.store(true, std::memory_order_relaxed);
    }
    cv_.notify_all();
    thread_.join();
}

void MergeWorker::loop_() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait_for(lk, std::chrono::seconds(5));
        if (stop_flag_.load(std::memory_order_relaxed))
            break;
        lk.unlock();
        for (const auto& name : catalog_->table_names())
            maybe_merge_table_(name);
        lk.lock();
    }
}

static Result<void> copy_column(ColumnFile& src, ColumnFile& dst) {
    TypeId t = src.type();
    Result<void> err = Result<void>::ok();

    auto scan_r = src.scan([&](const ColumnPage& p) {
        if (err.is_err())
            return;
        u16 n = p.value_count();
        for (u16 i = 0; i < n; ++i) {
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

static std::vector<u8> merge_deleted_bitmaps(const std::vector<MergeWorker::SegSnapshot>& snaps) {
    u64 total = 0;
    bool any = false;
    for (const auto& s : snaps) {
        total += s.row_count;
        if (!s.deleted_bitmap.empty())
            any = true;
    }
    if (!any || total == 0)
        return {};

    std::vector<u8> merged((total + 7) / 8, 0);
    u64 offset = 0;
    for (const auto& s : snaps) {
        for (u64 i = 0; i < s.row_count; ++i) {
            usize src_byte = i / 8;
            bool bit = (src_byte < s.deleted_bitmap.size()) &&
                       ((s.deleted_bitmap[src_byte] >> (i % 8)) & 1u);
            if (bit) {
                u64 dst_bit = offset + i;
                merged[dst_bit / 8] |= static_cast<u8>(1u << (dst_bit % 8));
            }
        }
        offset += s.row_count;
    }
    return merged;
}

void MergeWorker::maybe_merge_table_(const std::string& table_name) {
    Table* tbl = catalog_->table(table_name);
    if (!tbl)
        return;

    std::vector<SegSnapshot> snaps;
    std::string table_dir;
    Schema schema;
    {
        auto lk = tbl->lock_shared();
        const auto& segs = tbl->segments();
        if (segs.size() < merge_threshold_)
            return;

        table_dir = tbl->dir();
        schema = tbl->schema();

        usize n = merge_threshold_ / 2;
        snaps.reserve(n);
        for (usize i = 0; i < n; ++i) {
            const Segment& s = segs[i];
            snaps.push_back({s.dir(), s.meta().id, s.meta().base_row_id, s.meta().row_count,
                             s.deleted_bitmap(), i});
        }
    }

    std::string tmp_dir = table_dir + "/seg_merge_tmp";

    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);
    if (ec)
        return;

    for (usize ci = 0; ci < schema.size(); ++ci) {
        const auto& col_schema = schema[ci];
        std::string dst_path = tmp_dir + "/" + col_schema.name + ".col";

        auto dst_r =
            ColumnFile::create(dst_path, col_schema.type, col_schema.nullable, col_schema.max_len);
        if (dst_r.is_err()) {
            fs::remove_all(tmp_dir, ec);
            return;
        }
        ColumnFile dst = std::move(dst_r.value());

        for (auto& snap : snaps) {
            std::string src_path = snap.dir + "/" + col_schema.name + ".col";
            auto src_r = ColumnFile::open(src_path);
            if (src_r.is_err()) {
                fs::remove_all(tmp_dir, ec);
                return;
            }
            ColumnFile src = std::move(src_r.value());
            auto cr = copy_column(src, dst);
            if (cr.is_err()) {
                fs::remove_all(tmp_dir, ec);
                return;
            }
        }

        auto fr = dst.flush();
        if (fr.is_err()) {
            fs::remove_all(tmp_dir, ec);
            return;
        }
    }

    auto merged_bm = merge_deleted_bitmaps(snaps);
    if (!merged_bm.empty()) {
        std::string bm_path = tmp_dir + "/deleted.bin";
        int fd = ::open(bm_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fs::remove_all(tmp_dir, ec);
            return;
        }
        ssize_t n = ::write(fd, merged_bm.data(), merged_bm.size());
        ::fsync(fd);
        ::close(fd);
        if (n != static_cast<ssize_t>(merged_bm.size())) {
            fs::remove_all(tmp_dir, ec);
            return;
        }
    }

    u64 merged_base = snaps[0].base_row_id;
    u64 merged_count = 0;
    for (const auto& s : snaps)
        merged_count += s.row_count;

    std::vector<usize> indices;
    indices.reserve(snaps.size());
    for (const auto& s : snaps)
        indices.push_back(s.idx);

    auto rr = tbl->replace_segments(indices, tmp_dir, merged_base, merged_count);
    if (rr.is_err()) {
        fs::remove_all(tmp_dir, ec);
        return;
    }

    for (const auto& s : snaps)
        fs::remove_all(s.dir, ec);
}

} // namespace nyx
