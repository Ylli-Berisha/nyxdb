#include "catalog/catalog.h"

#include "storage/wal/wal_reader.h"
#include "storage/wal/wal_record.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <numeric>
#include <unistd.h>
#include <utility>

namespace nyx {

namespace fs = std::filesystem;

struct LsnCheckpoint {
    u64 lsn;
    u64 rows;
};

static std::string canonicalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

static std::string lsn_path(const std::string& table_dir) {
    return table_dir + "/lsn.bin";
}

static LsnCheckpoint read_lsn(const std::string& table_dir) {
    int fd = ::open(lsn_path(table_dir).c_str(), O_RDONLY);
    if (fd < 0)
        return {0, 0};
    u8 buf[16];
    ssize_t n = ::read(fd, buf, 16);
    ::close(fd);
    if (n != 16)
        return {0, 0};
    return {wal_read_u64(buf), wal_read_u64(buf + 8)};
}

static Result<void> write_lsn(const std::string& table_dir, LsnCheckpoint cp) {
    int fd = ::open(lsn_path(table_dir).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Result<void>::err("write_lsn: cannot open " + lsn_path(table_dir) + ": " +
                                 strerror(errno));
    u8 buf[16];
    wal_put_u64(buf, cp.lsn);
    wal_put_u64(buf + 8, cp.rows);
    ssize_t n = ::write(fd, buf, 16);
    ::close(fd);
    if (n != 16)
        return Result<void>::err("write_lsn: short write");
    return Result<void>::ok();
}

Catalog::Catalog(std::string data_root) : data_root_(std::move(data_root)) {}

Result<void> Catalog::ensure_wal_() {
    if (wal_.has_value())
        return Result<void>::ok();
    auto r = WalWriter::open(data_root_ + "/wal.bin");
    if (r.is_err())
        return Result<void>::err(r.error());
    wal_ = std::move(r.value());
    return Result<void>::ok();
}

Result<void> Catalog::create_table_(const std::string& name, Schema schema) {
    std::string canonical = canonicalize(name);
    if (tables_.count(canonical) > 0)
        return Result<void>::err("catalog: table '" + name + "' already exists");
    auto t = Table::create(data_root_, canonical, std::move(schema));
    if (t.is_err())
        return Result<void>::err("catalog: cannot create table '" + name +
                                 "': " + t.error().message);
    auto flush = t.value().flush();
    if (flush.is_err())
        return Result<void>::err("catalog: cannot flush table '" + name +
                                 "': " + flush.error().message);
    tables_.emplace(canonical, std::move(t.value()));
    return Result<void>::ok();
}

Result<Catalog> Catalog::load(const std::string& data_root) {
    Catalog cat(data_root);

    std::error_code ec;
    if (!fs::exists(data_root, ec))
        return Result<Catalog>::ok(std::move(cat));
    if (!fs::is_directory(data_root, ec))
        return Result<Catalog>::err("catalog: data_root is not a directory: " + data_root);

    for (const auto& entry : fs::directory_iterator(data_root, ec)) {
        if (!entry.is_directory())
            continue;
        fs::path schema_path = entry.path() / "schema.bin";
        if (!fs::exists(schema_path))
            continue;
        std::string name = entry.path().filename().string();
        auto t = Table::open(data_root, name);
        if (t.is_err())
            return Result<Catalog>::err("catalog: cannot open table '" + name +
                                        "': " + t.error().message);
        cat.tables_.emplace(canonicalize(name), std::move(t.value()));
    }
    if (ec)
        return Result<Catalog>::err("catalog: directory iteration error: " + ec.message());

    std::string wal_path = data_root + "/wal.bin";
    if (!fs::exists(wal_path))
        return Result<Catalog>::ok(std::move(cat));

    auto rr = WalReader::open(wal_path);
    if (rr.is_err())
        return Result<Catalog>::err("catalog: wal open: " + rr.error().message);
    auto rec_r = rr.value().read_all();
    if (rec_r.is_err())
        return Result<Catalog>::err("catalog: wal read: " + rec_r.error().message);
    const auto& records = rec_r.value();

    for (const auto& rec : records) {
        if (rec.type == WalRecord::Type::CreateTable) {
            std::string canonical = canonicalize(rec.table_name);
            if (cat.tables_.count(canonical) == 0) {
                auto r = cat.create_table_(rec.table_name, rec.schema);
                if (r.is_err())
                    return Result<Catalog>::err("catalog: wal replay create '" + rec.table_name +
                                                "': " + r.error().message);
            }
        }
    }

    std::unordered_map<std::string, LsnCheckpoint> lsns;
    for (auto& [name, tbl] : cat.tables_) {
        LsnCheckpoint cp = read_lsn(tbl.dir());
        lsns[name] = cp;
        auto tr = tbl.truncate(cp.rows);
        if (tr.is_err())
            return Result<Catalog>::err("catalog: truncate '" + name + "': " + tr.error().message);
    }

    for (const auto& rec : records) {
        if (rec.type == WalRecord::Type::CreateTable)
            continue;
        std::string canonical = canonicalize(rec.table_name);
        auto it = cat.tables_.find(canonical);
        if (it == cat.tables_.end())
            continue;
        auto lsn_it = lsns.find(canonical);
        if (lsn_it != lsns.end() && rec.byte_offset < lsn_it->second.lsn)
            continue;

        if (rec.type == WalRecord::Type::Insert) {
            auto r = it->second.insert_many(rec.rows);
            if (r.is_err())
                return Result<Catalog>::err("catalog: wal replay insert '" + rec.table_name +
                                            "': " + r.error().message);
        } else if (rec.type == WalRecord::Type::Update) {
            auto r = it->second.mark_deleted(rec.row_indices);
            if (r.is_err())
                return Result<Catalog>::err("catalog: wal replay update (delete) '" +
                                            rec.table_name + "': " + r.error().message);
            auto r2 = it->second.insert_many(rec.rows);
            if (r2.is_err())
                return Result<Catalog>::err("catalog: wal replay update (insert) '" +
                                            rec.table_name + "': " + r2.error().message);
        } else if (rec.type == WalRecord::Type::Delete) {
            auto r = it->second.mark_deleted(rec.row_indices);
            if (r.is_err())
                return Result<Catalog>::err("catalog: wal replay delete '" + rec.table_name +
                                            "': " + r.error().message);
        }
    }

    for (auto& [name, tbl] : cat.tables_) {
        auto r = tbl.flush();
        if (r.is_err())
            return Result<Catalog>::err("catalog: post-recovery flush '" + name +
                                        "': " + r.error().message);
        auto wl = write_lsn(tbl.dir(), {0, tbl.row_count()});
        if (wl.is_err())
            return Result<Catalog>::err(wl.error());
    }

    fs::remove(wal_path);
    return Result<Catalog>::ok(std::move(cat));
}

bool Catalog::has_table(const std::string& name) const {
    return tables_.find(canonicalize(name)) != tables_.end();
}

const Schema* Catalog::schema_of(const std::string& name) const {
    auto it = tables_.find(canonicalize(name));
    if (it == tables_.end())
        return nullptr;
    return &it->second.schema();
}

Table* Catalog::table(const std::string& name) {
    auto it = tables_.find(canonicalize(name));
    if (it == tables_.end())
        return nullptr;
    return &it->second;
}

Result<void> Catalog::add_table(const std::string& name, Schema schema) {
    auto r = ensure_wal_();
    if (r.is_err())
        return r;
    auto lw = wal_->log_create_table(canonicalize(name), schema);
    if (lw.is_err())
        return lw;
    return create_table_(name, std::move(schema));
}

Result<u64> Catalog::insert(const std::string& table_name,
                            const std::vector<std::vector<Value>>& rows) {
    std::string canonical = canonicalize(table_name);
    auto it = tables_.find(canonical);
    if (it == tables_.end())
        return Result<u64>::err("insert: table not found: " + table_name);

    auto r = ensure_wal_();
    if (r.is_err())
        return Result<u64>::err(r.error().message);

    auto lw = wal_->log_insert(canonical, it->second.schema(), rows);
    if (lw.is_err())
        return Result<u64>::err(lw.error().message);

    auto ir = it->second.insert_many(rows);
    if (ir.is_err())
        return ir;

    if (wal_->current_offset() > WAL_CHECKPOINT_BYTES) {
        auto fr = flush_all();
        if (fr.is_err())
            return Result<u64>::err(fr.error().message);
    }

    return ir;
}

Result<u64> Catalog::delete_rows(const std::string& name, const std::vector<u64>& row_indices) {
    if (row_indices.empty())
        return Result<u64>::ok(0);
    std::string canonical = canonicalize(name);
    auto it = tables_.find(canonical);
    if (it == tables_.end())
        return Result<u64>::err("delete: table not found: " + name);

    auto ew = ensure_wal_();
    if (ew.is_err())
        return Result<u64>::err(ew.error().message);

    auto lw = wal_->log_delete(canonical, row_indices);
    if (lw.is_err())
        return Result<u64>::err(lw.error().message);

    auto r = it->second.mark_deleted(row_indices);
    if (r.is_err())
        return Result<u64>::err(r.error().message);

    return Result<u64>::ok(static_cast<u64>(row_indices.size()));
}

Result<u64> Catalog::update_rows(const std::string& name, const std::vector<u64>& old_indices,
                                 const Schema& schema,
                                 const std::vector<std::vector<Value>>& new_rows) {
    if (old_indices.empty())
        return Result<u64>::ok(0);
    std::string canonical = canonicalize(name);
    auto it = tables_.find(canonical);
    if (it == tables_.end())
        return Result<u64>::err("update: table not found: " + name);

    auto ew = ensure_wal_();
    if (ew.is_err())
        return Result<u64>::err(ew.error().message);

    auto lw = wal_->log_update(canonical, old_indices, schema, new_rows);
    if (lw.is_err())
        return Result<u64>::err(lw.error().message);

    auto dr = it->second.mark_deleted(old_indices);
    if (dr.is_err())
        return Result<u64>::err(dr.error().message);

    auto ir = it->second.insert_many(new_rows);
    if (ir.is_err())
        return Result<u64>::err(ir.error().message);

    return Result<u64>::ok(static_cast<u64>(old_indices.size()));
}

Result<u64> Catalog::delete_all(const std::string& name) {
    std::string canonical = canonicalize(name);
    auto it = tables_.find(canonical);
    if (it == tables_.end())
        return Result<u64>::err("delete: table not found: " + name);
    u64 count = it->second.row_count();
    if (count == 0)
        return Result<u64>::ok(0);
    std::vector<u64> all(static_cast<size_t>(count));
    std::iota(all.begin(), all.end(), u64{0});
    return delete_rows(name, all);
}

Result<void> Catalog::drop_table(const std::string& name, bool if_exists) {
    std::string canonical = canonicalize(name);
    auto it = tables_.find(canonical);
    if (it == tables_.end()) {
        if (if_exists)
            return Result<void>::ok();
        return Result<void>::err("catalog: table '" + name + "' does not exist");
    }
    std::string dir = it->second.dir();
    auto r = flush_all();
    if (r.is_err())
        return r;
    tables_.erase(canonical);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec)
        return Result<void>::err("drop_table: remove_all failed: " + ec.message());
    return Result<void>::ok();
}

Result<void> Catalog::flush_all() {
    for (auto& [name, tbl] : tables_) {
        auto r = tbl.flush();
        if (r.is_err())
            return Result<void>::err("flush_all: table '" + name + "': " + r.error().message);
    }

    u64 wal_offset = wal_.has_value() ? wal_->current_offset() : 0;
    for (auto& [name, tbl] : tables_) {
        auto r = write_lsn(tbl.dir(), {wal_offset, tbl.row_count()});
        if (r.is_err())
            return r;
    }

    if (wal_.has_value()) {
        auto r = wal_->checkpoint();
        if (r.is_err())
            return r;
        wal_.reset();
    }

    return Result<void>::ok();
}

} // namespace nyx
