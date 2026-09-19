#include "storage/disk/table.h"

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

static std::string deleted_bin_path(const std::string& dir) {
    return dir + "/deleted.bin";
}

static std::vector<u8> load_deleted_bitmap(const std::string& dir) {
    int fd = ::open(deleted_bin_path(dir).c_str(), O_RDONLY);
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

static Result<void> write_deleted_bitmap(const std::string& dir, const std::vector<u8>& bm) {
    std::string path = deleted_bin_path(dir);
    if (bm.empty()) {
        ::unlink(path.c_str());
        return Result<void>::ok();
    }
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Result<void>::err("write_deleted_bitmap: open failed: " +
                                 std::string(strerror(errno)));
    ssize_t n = ::write(fd, bm.data(), bm.size());
    if (::fsync(fd) != 0) {
        ::close(fd);
        return Result<void>::err("write_deleted_bitmap: fsync failed");
    }
    ::close(fd);
    if (n != static_cast<ssize_t>(bm.size()))
        return Result<void>::err("write_deleted_bitmap: short write");
    return Result<void>::ok();
}

Table::Table(std::string dir, std::string name, Schema schema, std::vector<ColumnFile> columns,
             std::vector<u8> deleted_bitmap)
    : dir_(std::move(dir)), name_(std::move(name)), schema_(std::move(schema)),
      columns_(std::move(columns)), deleted_bitmap_(std::move(deleted_bitmap)) {}

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

Result<Table> Table::create(const std::string& data_root, const std::string& name, Schema schema) {
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

    return Result<Table>::ok(
        Table(dir_path.string(), name, std::move(schema), std::move(columns), {}));
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

    std::vector<ColumnFile> columns;
    columns.reserve(schema.size());
    for (const auto& col : schema) {
        fs::path col_path = dir_path / (col.name + ".col");
        auto cf_res = ColumnFile::open(col_path.string());
        if (cf_res.is_err())
            return Result<Table>::err("open: " + cf_res.error().message);

        ColumnFile cf = std::move(cf_res.value());
        if (cf.type() != col.type || cf.nullable() != col.nullable || cf.max_len() != col.max_len)
            return Result<Table>::err("open: column '" + col.name +
                                      "' file header does not match schema");
        columns.push_back(std::move(cf));
    }

    auto bm = load_deleted_bitmap(dir_path.string());
    return Result<Table>::ok(
        Table(dir_path.string(), name, std::move(schema), std::move(columns), std::move(bm)));
}

u64 Table::row_count() const {
    return columns_.empty() ? 0 : columns_[0].row_count();
}

Result<u64> Table::insert(const std::vector<Value>& row) {
    if (row.size() != columns_.size())
        return Result<u64>::err("insert: row size " + std::to_string(row.size()) +
                                " does not match column count " + std::to_string(columns_.size()));

    u64 row_id = row_count();

    for (size_t i = 0; i < row.size(); ++i) {
        const auto& v = row[i];
        auto& col = columns_[i];
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

    return Result<u64>::ok(row_id);
}

Result<u64> Table::insert_many(const std::vector<std::vector<Value>>& rows) {
    if (rows.empty())
        return Result<u64>::ok(0);

    for (const auto& row : rows) {
        if (row.size() != columns_.size())
            return Result<u64>::err("insert_many: row size " + std::to_string(row.size()) +
                                    " does not match column count " +
                                    std::to_string(columns_.size()));
    }

    std::vector<std::vector<Value>> column_batches(columns_.size());
    for (auto& b : column_batches)
        b.reserve(rows.size());
    for (const auto& row : rows)
        for (size_t i = 0; i < row.size(); ++i)
            column_batches[i].push_back(row[i]);

    for (size_t i = 0; i < columns_.size(); ++i) {
        auto r = columns_[i].append_bulk(column_batches[i]);
        if (r.is_err())
            return Result<u64>::err("insert_many col " + schema_[i].name + ": " +
                                    r.error().message);
    }

    return Result<u64>::ok(static_cast<u64>(rows.size()));
}

Result<void> Table::mark_deleted(const std::vector<u64>& row_indices) {
    if (row_indices.empty())
        return Result<void>::ok();
    u64 total = row_count();
    size_t bytes_needed = static_cast<size_t>((total + 7) / 8);
    if (deleted_bitmap_.size() < bytes_needed)
        deleted_bitmap_.resize(bytes_needed, 0);
    for (u64 idx : row_indices) {
        if (idx < total)
            deleted_bitmap_[idx / 8] |= static_cast<u8>(1u << (idx % 8));
    }
    return write_deleted_bitmap(dir_, deleted_bitmap_);
}

Result<void> Table::clear_deletions() {
    deleted_bitmap_.clear();
    return write_deleted_bitmap(dir_, deleted_bitmap_);
}

Result<void> Table::truncate(u64 target_rows) {
    for (auto& col : columns_) {
        auto r = col.truncate(target_rows);
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

Result<void> Table::flush() {
    for (auto& col : columns_) {
        auto r = col.flush();
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

Result<void> Table::fsync() {
    for (auto& col : columns_) {
        auto r = col.fsync();
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

} // namespace nyx
