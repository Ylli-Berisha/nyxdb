#include "catalog/catalog.h"

#include <cctype>
#include <filesystem>
#include <utility>

namespace nyx {

namespace fs = std::filesystem;

static std::string canonicalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

Catalog::Catalog(std::string data_root) : data_root_(std::move(data_root)) {}

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

} // namespace nyx
