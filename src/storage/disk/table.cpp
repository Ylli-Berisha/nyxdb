#include "storage/disk/table.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace nyx {

namespace fs = std::filesystem;

static std::string wb_deleted_path(const std::string& dir) {
    return dir + "/deleted.bin";
}

static std::vector<u8> load_wb_deleted(const std::string& dir) {
    int fd = ::open(wb_deleted_path(dir).c_str(), O_RDONLY);
    if (fd < 0)
        return {};
    struct stat st {};
    ::fstat(fd, &st);
    std::vector<u8> bm(static_cast<size_t>(st.st_size));
    if (!bm.empty()) {
        ssize_t n = ::read(fd, bm.data(), bm.size());
        if (n != static_cast<ssize_t>(bm.size()))
            bm.clear();
    }
    ::close(fd);
    return bm;
}

static Result<void> write_wb_deleted(const std::string& dir, const std::vector<u8>& bm) {
    std::string path = wb_deleted_path(dir);
    if (bm.empty()) {
        ::unlink(path.c_str());
        return Result<void>::ok();
    }
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Result<void>::err("write_wb_deleted: " + std::string(strerror(errno)));
    ssize_t n = ::write(fd, bm.data(), bm.size());
    ::fsync(fd);
    ::close(fd);
    if (n != static_cast<ssize_t>(bm.size()))
        return Result<void>::err("write_wb_deleted: short write");
    return Result<void>::ok();
}

static std::string manifest_path(const std::string& dir) {
    return dir + "/manifest.bin";
}

static std::string manifest_tmp_path(const std::string& dir) {
    return dir + "/manifest.tmp";
}

static constexpr u32 MANIFEST_VERSION = 1;
static constexpr usize MANIFEST_ENTRY_SIZE = 24;

static u32 read_u32_le(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

static u64 read_u64_le(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<u64>(p[i]) << (i * 8);
    return v;
}

static void write_u32_le(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i)
        p[i] = static_cast<u8>((v >> (i * 8)) & 0xFF);
}

static void write_u64_le(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<u8>((v >> (i * 8)) & 0xFF);
}

static std::vector<SegmentMeta> read_manifest(const std::string& dir) {
    std::string path = manifest_path(dir);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return {};
    struct stat st {};
    ::fstat(fd, &st);
    if (st.st_size < 8) {
        ::close(fd);
        return {};
    }
    std::vector<u8> buf(static_cast<usize>(st.st_size));
    ssize_t n = ::read(fd, buf.data(), buf.size());
    ::close(fd);
    if (n != static_cast<ssize_t>(buf.size()))
        return {};
    u32 version = read_u32_le(buf.data());
    if (version != MANIFEST_VERSION)
        return {};
    u32 count = read_u32_le(buf.data() + 4);
    if (static_cast<usize>(8 + count * MANIFEST_ENTRY_SIZE) > buf.size())
        return {};
    std::vector<SegmentMeta> metas;
    metas.reserve(count);
    const u8* p = buf.data() + 8;
    for (u32 i = 0; i < count; ++i, p += MANIFEST_ENTRY_SIZE) {
        SegmentMeta m;
        m.id = read_u64_le(p);
        m.base_row_id = read_u64_le(p + 8);
        m.row_count = read_u64_le(p + 16);
        metas.push_back(m);
    }
    return metas;
}

static Result<void> write_manifest(const std::string& dir, const std::vector<SegmentMeta>& metas) {
    usize buf_size = 8 + metas.size() * MANIFEST_ENTRY_SIZE;
    std::vector<u8> buf(buf_size, 0);
    write_u32_le(buf.data(), MANIFEST_VERSION);
    write_u32_le(buf.data() + 4, static_cast<u32>(metas.size()));
    u8* p = buf.data() + 8;
    for (const auto& m : metas) {
        write_u64_le(p, m.id);
        write_u64_le(p + 8, m.base_row_id);
        write_u64_le(p + 16, m.row_count);
        p += MANIFEST_ENTRY_SIZE;
    }

    std::string tmp = manifest_tmp_path(dir);
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Result<void>::err("write_manifest: " + std::string(strerror(errno)));
    ssize_t n = ::write(fd, buf.data(), buf.size());
    ::fsync(fd);
    ::close(fd);
    if (n != static_cast<ssize_t>(buf.size()))
        return Result<void>::err("write_manifest: short write");
    if (::rename(tmp.c_str(), manifest_path(dir).c_str()) != 0)
        return Result<void>::err("write_manifest: rename failed: " + std::string(strerror(errno)));
    return Result<void>::ok();
}

static Result<void> write_seg_meta(const std::string& seg_dir, const SegmentMeta& m) {
    std::string path = seg_dir + "/meta.bin";
    u8 buf[24];
    write_u64_le(buf, m.id);
    write_u64_le(buf + 8, m.base_row_id);
    write_u64_le(buf + 16, m.row_count);
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Result<void>::err("write_seg_meta: " + std::string(strerror(errno)));
    ssize_t n = ::write(fd, buf, 24);
    ::fsync(fd);
    ::close(fd);
    if (n != 24)
        return Result<void>::err("write_seg_meta: short write");
    return Result<void>::ok();
}

Table::Table(std::string dir, std::string name, Schema schema, std::vector<ColumnFile> wb_columns,
             std::vector<u8> wb_deleted, u64 wb_base_row_id, std::vector<Segment> segments,
             u64 next_segment_id, u64 flush_threshold)
    : dir_(std::move(dir)), name_(std::move(name)), schema_(std::move(schema)),
      wb_columns_(std::move(wb_columns)), wb_deleted_(std::move(wb_deleted)),
      wb_base_row_id_(wb_base_row_id), segments_(std::move(segments)),
      next_segment_id_(next_segment_id), flush_threshold_(flush_threshold),
      rwlock_(std::make_unique<std::shared_mutex>()) {}

static Result<void> validate_schema(const Schema& schema) {
    if (schema.empty())
        return Result<void>::err("schema is empty");

    std::unordered_set<std::string> seen;
    for (const auto& col : schema) {
        if (type_size(col.type, col.max_len) == 0)
            return Result<void>::err("schema column '" + col.name + "' has invalid type");
        if (col.name.empty())
            return Result<void>::err("schema has a column with empty name");
        if (!seen.insert(col.name).second)
            return Result<void>::err("schema has duplicate column name '" + col.name + "'");
    }
    return Result<void>::ok();
}

Result<Table> Table::create(const std::string& data_root, const std::string& name, Schema schema,
                            u64 flush_threshold) {
    auto v = validate_schema(schema);
    if (v.is_err())
        return Result<Table>::err("create: " + v.error().message);

    std::error_code ec;
    fs::path dir_path = fs::path(data_root) / name;
    fs::create_directories(dir_path, ec);
    if (ec)
        return Result<Table>::err("create: cannot mkdir " + dir_path.string() + ": " +
                                  ec.message());

    fs::path schema_path = dir_path / "schema.bin";
    if (fs::exists(schema_path))
        return Result<Table>::err("create: table '" + name + "' already exists");

    auto write_res = SchemaFile::write(schema_path.string(), schema);
    if (write_res.is_err())
        return Result<Table>::err(write_res.error().message);

    std::vector<ColumnFile> columns;
    columns.reserve(schema.size());
    std::vector<fs::path> created_paths;

    for (const auto& col : schema) {
        fs::path col_path = dir_path / (col.name + ".col");
        auto cf_res = ColumnFile::create(col_path.string(), col.type, col.nullable, col.max_len);
        if (cf_res.is_err()) {
            for (const auto& p : created_paths)
                fs::remove(p, ec);
            fs::remove(schema_path, ec);
            return Result<Table>::err("create: " + cf_res.error().message);
        }
        created_paths.push_back(col_path);
        columns.push_back(std::move(cf_res.value()));
    }

    return Result<Table>::ok(Table(dir_path.string(), name, std::move(schema), std::move(columns),
                                   {}, 0, {}, 0, flush_threshold));
}

Result<Table> Table::open(const std::string& data_root, const std::string& name) {
    fs::path dir_path = fs::path(data_root) / name;
    if (!fs::exists(dir_path))
        return Result<Table>::err("open: table dir " + dir_path.string() + " does not exist");

    fs::path schema_path = dir_path / "schema.bin";
    auto schema_res = SchemaFile::read(schema_path.string());
    if (schema_res.is_err())
        return Result<Table>::err("open: " + schema_res.error().message);

    Schema schema = std::move(schema_res.value());

    std::vector<SegmentMeta> metas = read_manifest(dir_path.string());
    std::vector<Segment> segments;
    segments.reserve(metas.size());
    u64 wb_base_row_id = 0;
    u64 next_segment_id = 0;

    for (const auto& m : metas) {
        std::string seg_dir = dir_path.string() + "/seg_" + std::to_string(m.id);
        auto seg_r = Segment::open(seg_dir, schema, m);
        if (seg_r.is_err())
            return Result<Table>::err("open: segment " + std::to_string(m.id) + ": " +
                                      seg_r.error().message);
        wb_base_row_id += m.row_count;
        if (m.id >= next_segment_id)
            next_segment_id = m.id + 1;
        segments.push_back(std::move(seg_r.value()));
    }

    std::vector<ColumnFile> wb_columns;
    wb_columns.reserve(schema.size());
    for (const auto& col : schema) {
        fs::path col_path = dir_path / (col.name + ".col");
        auto cf_res = ColumnFile::open(col_path.string());
        if (cf_res.is_err())
            return Result<Table>::err("open: " + cf_res.error().message);

        ColumnFile cf = std::move(cf_res.value());
        if (cf.type() != col.type || cf.nullable() != col.nullable || cf.max_len() != col.max_len)
            return Result<Table>::err("open: column '" + col.name +
                                      "' file header does not match schema");
        wb_columns.push_back(std::move(cf));
    }

    auto wb_deleted = load_wb_deleted(dir_path.string());
    return Result<Table>::ok(Table(dir_path.string(), name, std::move(schema),
                                   std::move(wb_columns), std::move(wb_deleted), wb_base_row_id,
                                   std::move(segments), next_segment_id));
}

u64 Table::row_count() const {
    std::shared_lock lk(*rwlock_);
    u64 wb = wb_columns_.empty() ? 0 : wb_columns_[0].row_count();
    return wb_base_row_id_ + wb;
}

Result<u64> Table::insert(const std::vector<Value>& row) {
    std::unique_lock lk(*rwlock_);

    if (row.size() != wb_columns_.size())
        return Result<u64>::err("insert: row size " + std::to_string(row.size()) +
                                " does not match column count " +
                                std::to_string(wb_columns_.size()));

    u64 wb_count = wb_columns_.empty() ? 0 : wb_columns_[0].row_count();
    u64 row_id = wb_base_row_id_ + wb_count;

    for (size_t i = 0; i < row.size(); ++i) {
        const auto& v = row[i];
        auto& col = wb_columns_[i];
        const auto& col_schema = schema_[i];

        if (is_null(v)) {
            auto r = col.append_null();
            if (r.is_err())
                return Result<u64>::err("insert col " + col_schema.name + ": " + r.error().message);
            continue;
        }

        Result<void> r = Result<void>::ok();
        if (col_schema.type == TypeId::INT32 && std::holds_alternative<i32>(v)) {
            r = col.append_i32(std::get<i32>(v));
        } else if (col_schema.type == TypeId::INT64 && std::holds_alternative<i64>(v)) {
            r = col.append_i64(std::get<i64>(v));
        } else if (col_schema.type == TypeId::DOUBLE && std::holds_alternative<f64>(v)) {
            r = col.append_f64(std::get<f64>(v));
        } else if (col_schema.type == TypeId::BOOL && std::holds_alternative<bool>(v)) {
            r = col.append_bool(std::get<bool>(v));
        } else if (col_schema.type == TypeId::DATE && std::holds_alternative<Date>(v)) {
            r = col.append_date(std::get<Date>(v).days);
        } else if (col_schema.type == TypeId::TIMESTAMP && std::holds_alternative<Timestamp>(v)) {
            r = col.append_timestamp(std::get<Timestamp>(v).micros);
        } else {
            return Result<u64>::err("insert col " + col_schema.name +
                                    ": value type does not match schema type");
        }

        if (r.is_err())
            return Result<u64>::err("insert col " + col_schema.name + ": " + r.error().message);
    }

    auto fr = maybe_flush_();
    if (fr.is_err())
        return Result<u64>::err(fr.error().message);

    return Result<u64>::ok(row_id);
}

Result<u64> Table::insert_many_nolock_(const std::vector<std::vector<Value>>& rows) {
    if (rows.empty())
        return Result<u64>::ok(0);

    for (const auto& row : rows) {
        if (row.size() != wb_columns_.size())
            return Result<u64>::err("insert_many: row size " + std::to_string(row.size()) +
                                    " does not match column count " +
                                    std::to_string(wb_columns_.size()));
    }

    std::vector<std::vector<Value>> column_batches(wb_columns_.size());
    for (auto& b : column_batches)
        b.reserve(rows.size());
    for (const auto& row : rows)
        for (size_t i = 0; i < row.size(); ++i)
            column_batches[i].push_back(row[i]);

    for (size_t i = 0; i < wb_columns_.size(); ++i) {
        auto r = wb_columns_[i].append_bulk(column_batches[i]);
        if (r.is_err())
            return Result<u64>::err("insert_many col " + schema_[i].name + ": " +
                                    r.error().message);
    }

    auto fr = maybe_flush_();
    if (fr.is_err())
        return Result<u64>::err(fr.error().message);

    return Result<u64>::ok(static_cast<u64>(rows.size()));
}

Result<u64> Table::insert_many(const std::vector<std::vector<Value>>& rows) {
    std::unique_lock lk(*rwlock_);
    return insert_many_nolock_(rows);
}

Result<Table::UpdateMeta> Table::update_rows(const std::vector<u64>& old_indices,
                                             const std::vector<std::vector<Value>>& new_rows) {
    std::unique_lock lk(*rwlock_);

    auto dr = mark_deleted_nolock_(old_indices);
    if (dr.is_err())
        return Result<UpdateMeta>::err(dr.error().message);

    u64 first_row_id = wb_base_row_id_ + (wb_columns_.empty() ? 0 : wb_columns_[0].row_count());
    u64 wb_base_before = wb_base_row_id_;

    auto ir = insert_many_nolock_(new_rows);
    if (ir.is_err())
        return Result<UpdateMeta>::err(ir.error().message);

    return Result<UpdateMeta>::ok({first_row_id, wb_base_before, wb_base_row_id_});
}

bool Table::is_row_deleted(u64 global_id) const {
    std::shared_lock lk(*rwlock_);
    if (global_id >= wb_base_row_id_) {
        u64 local = global_id - wb_base_row_id_;
        usize byte_idx = local / 8;
        if (byte_idx >= wb_deleted_.size())
            return false;
        return (wb_deleted_[byte_idx] >> (local % 8)) & 1u;
    }
    for (const auto& seg : segments_) {
        u64 base = seg.meta().base_row_id;
        u64 count = seg.meta().row_count;
        if (global_id < base || global_id >= base + count)
            continue;
        u64 local = global_id - base;
        const auto& bm = seg.deleted_bitmap();
        usize byte_idx = local / 8;
        if (byte_idx >= bm.size())
            return false;
        return (bm[byte_idx] >> (local % 8)) & 1u;
    }
    return false;
}

Result<void> Table::mark_deleted_nolock_(const std::vector<u64>& row_indices) {
    if (row_indices.empty())
        return Result<void>::ok();

    std::vector<u64> wb_ids;
    std::vector<std::vector<u64>> seg_ids(segments_.size());

    for (u64 gid : row_indices) {
        if (gid >= wb_base_row_id_) {
            wb_ids.push_back(gid - wb_base_row_id_);
        } else if (!segments_.empty()) {
            usize lo = 0, hi = segments_.size();
            while (lo + 1 < hi) {
                usize mid = (lo + hi) / 2;
                if (segments_[mid].meta().base_row_id <= gid)
                    lo = mid;
                else
                    hi = mid;
            }
            u64 local = gid - segments_[lo].meta().base_row_id;
            seg_ids[lo].push_back(local);
        }
    }

    if (!wb_ids.empty()) {
        u64 total_wb = wb_columns_.empty() ? 0 : wb_columns_[0].row_count();
        usize bytes_needed = static_cast<usize>((total_wb + 7) / 8);
        if (wb_deleted_.size() < bytes_needed)
            wb_deleted_.resize(bytes_needed, 0);
        for (u64 idx : wb_ids) {
            if (idx < total_wb)
                wb_deleted_[idx / 8] |= static_cast<u8>(1u << (idx % 8));
        }
        auto r = write_wb_deleted(dir_, wb_deleted_);
        if (r.is_err())
            return r;
    }

    for (usize si = 0; si < segments_.size(); ++si) {
        if (!seg_ids[si].empty()) {
            auto r = segments_[si].mark_deleted(seg_ids[si]);
            if (r.is_err())
                return r;
        }
    }

    return Result<void>::ok();
}

Result<void> Table::mark_deleted(const std::vector<u64>& row_indices) {
    std::unique_lock lk(*rwlock_);
    return mark_deleted_nolock_(row_indices);
}

Result<void> Table::clear_deletions() {
    std::unique_lock lk(*rwlock_);
    wb_deleted_.clear();
    return write_wb_deleted(dir_, wb_deleted_);
}

Result<void> Table::truncate(u64 target_rows) {
    std::unique_lock lk(*rwlock_);
    u64 wb_target = target_rows > wb_base_row_id_ ? target_rows - wb_base_row_id_ : 0;
    for (auto& col : wb_columns_) {
        auto r = col.truncate(wb_target);
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

Result<void> Table::flush() {
    std::unique_lock lk(*rwlock_);
    for (auto& col : wb_columns_) {
        auto r = col.flush();
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

Result<void> Table::seal() {
    std::unique_lock lk(*rwlock_);
    return flush_write_buffer_();
}

Result<void> Table::fsync() {
    std::unique_lock lk(*rwlock_);
    for (auto& col : wb_columns_) {
        auto r = col.fsync();
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

Result<void> Table::maybe_flush_() {
    u64 wb_row_count = wb_columns_.empty() ? 0 : wb_columns_[0].row_count();
    if (wb_row_count >= flush_threshold_)
        return flush_write_buffer_();
    return Result<void>::ok();
}

Result<void> Table::flush_write_buffer_() {
    u64 wb_row_count = wb_columns_.empty() ? 0 : wb_columns_[0].row_count();
    if (wb_row_count == 0)
        return Result<void>::ok();

    for (auto& col : wb_columns_) {
        auto r = col.flush();
        if (r.is_err())
            return r;
    }

    u64 seg_id = next_segment_id_;
    std::string seg_dir = dir_ + "/seg_" + std::to_string(seg_id);

    std::error_code ec;
    fs::create_directories(seg_dir, ec);
    if (ec)
        return Result<void>::err("flush_write_buffer: mkdir " + seg_dir + ": " + ec.message());

    for (const auto& col : schema_) {
        std::string src = dir_ + "/" + col.name + ".col";
        std::string dst = seg_dir + "/" + col.name + ".col";
        if (::rename(src.c_str(), dst.c_str()) != 0)
            return Result<void>::err("flush_write_buffer: rename " + src + ": " +
                                     std::string(strerror(errno)));
    }

    {
        std::string src = wb_deleted_path(dir_);
        std::string dst = seg_dir + "/deleted.bin";
        if (fs::exists(src)) {
            if (::rename(src.c_str(), dst.c_str()) != 0)
                return Result<void>::err("flush_write_buffer: rename deleted.bin: " +
                                         std::string(strerror(errno)));
        }
    }

    SegmentMeta meta{seg_id, wb_base_row_id_, wb_row_count};
    auto mr = write_seg_meta(seg_dir, meta);
    if (mr.is_err())
        return mr;

    auto seg_r = Segment::open(seg_dir, schema_, meta);
    if (seg_r.is_err())
        return Result<void>::err("flush_write_buffer: open new segment: " + seg_r.error().message);

    std::vector<ColumnFile> new_wb;
    new_wb.reserve(schema_.size());
    for (const auto& col : schema_) {
        std::string col_path = dir_ + "/" + col.name + ".col";
        auto cf_r = ColumnFile::create(col_path, col.type, col.nullable, col.max_len);
        if (cf_r.is_err())
            return Result<void>::err("flush_write_buffer: create new col '" + col.name +
                                     "': " + cf_r.error().message);
        new_wb.push_back(std::move(cf_r.value()));
    }

    wb_base_row_id_ += wb_row_count;
    wb_deleted_.clear();
    wb_columns_ = std::move(new_wb);
    segments_.push_back(std::move(seg_r.value()));
    ++next_segment_id_;

    std::vector<SegmentMeta> all_metas;
    all_metas.reserve(segments_.size());
    for (const auto& s : segments_)
        all_metas.push_back(s.meta());

    return write_manifest(dir_, all_metas);
}

Result<void> Table::replace_segments(const std::vector<usize>& indices, const std::string& tmp_dir,
                                     u64 merged_base_row_id, u64 merged_row_count) {
    std::unique_lock lk(*rwlock_);

    u64 new_id = next_segment_id_++;
    std::string final_dir = dir_ + "/seg_" + std::to_string(new_id);

    if (::rename(tmp_dir.c_str(), final_dir.c_str()) != 0)
        return Result<void>::err("replace_segments: rename: " + std::string(strerror(errno)));

    SegmentMeta new_meta{new_id, merged_base_row_id, merged_row_count};
    auto mr = write_seg_meta(final_dir, new_meta);
    if (mr.is_err())
        return mr;

    auto seg_r = Segment::open(final_dir, schema_, new_meta);
    if (seg_r.is_err())
        return Result<void>::err("replace_segments: " + seg_r.error().message);

    std::vector<usize> sorted = indices;
    std::sort(sorted.rbegin(), sorted.rend());
    for (usize idx : sorted)
        segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(idx));

    usize insert_pos = sorted.back();
    segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                     std::move(seg_r.value()));

    std::vector<SegmentMeta> all_metas;
    all_metas.reserve(segments_.size());
    for (const auto& s : segments_)
        all_metas.push_back(s.meta());

    return write_manifest(dir_, all_metas);
}

} // namespace nyx
