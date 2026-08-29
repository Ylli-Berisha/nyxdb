#include "storage/disk/table.h"

#include <filesystem>
#include <unordered_set>
#include <utility>

namespace nyx {

namespace fs = std::filesystem;

Table::Table(std::string dir, std::string name, Schema schema, std::vector<ColumnFile> columns)
    : dir_(std::move(dir)), name_(std::move(name)), schema_(std::move(schema)),
      columns_(std::move(columns)) {}

static Result<void> validate_schema(const Schema& schema) {
    if (schema.empty())
        return Result<void>::err("schema is empty");

    std::unordered_set<std::string> seen;
    for (const auto& col : schema) {
        if (type_size(col.type) == 0)
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
        auto cf_res = ColumnFile::create(col_path.string(), col.type, col.nullable);
        if (cf_res.is_err()) {
            for (const auto& p : created_paths)
                fs::remove(p, ec);
            fs::remove(schema_path, ec);
            return Result<Table>::err("create: " + cf_res.error().message);
        }
        created_paths.push_back(col_path);
        columns.push_back(std::move(cf_res.value()));
    }

    return Result<Table>::ok(Table(dir_path.string(), name, std::move(schema), std::move(columns)));
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
        if (cf.type() != col.type || cf.nullable() != col.nullable)
            return Result<Table>::err("open: column '" + col.name +
                                      "' file header does not match schema");
        columns.push_back(std::move(cf));
    }

    return Result<Table>::ok(Table(dir_path.string(), name, std::move(schema), std::move(columns)));
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
        } else {
            return Result<u64>::err("insert col " + col_schema.name +
                                    ": value type does not match schema type");
        }

        if (r.is_err())
            return Result<u64>::err("insert col " + col_schema.name + ": " + r.error().message);
    }

    return Result<u64>::ok(row_id);
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
