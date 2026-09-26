#include "server/coordinator.h"

#include "binder/binder.h"
#include "frontend/runner.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "storage/disk/shard_map_file.h"

#include <filesystem>
#include <future>
#include <type_traits>

namespace nyx::server {

namespace fs = std::filesystem;

Coordinator::Coordinator(Catalog cat, std::string token)
    : catalog_(std::move(cat)), token_(std::move(token)),
      pool_(std::make_unique<ShardClientPool>()) {}

Result<Coordinator> Coordinator::open(const std::string& data_root, std::string token) {
    fs::create_directories(data_root);
    auto cat = Catalog::load(data_root);
    if (!cat.is_ok())
        return Result<Coordinator>::err(cat.error().message);
    return Result<Coordinator>::ok(Coordinator(std::move(cat.value()), std::move(token)));
}

static ExecuteResult merge_results(std::vector<ExecuteResult> parts) {
    if (parts.empty())
        return {};
    ExecuteResult out;
    out.schema = parts[0].schema;
    out.columns.resize(out.schema.size());
    for (auto& part : parts) {
        out.rows_affected += part.rows_affected;
        for (size_t c = 0; c < part.columns.size() && c < out.columns.size(); ++c) {
            for (auto& v : part.columns[c])
                out.columns[c].push_back(std::move(v));
        }
    }
    return out;
}

Result<ExecuteResult> Coordinator::execute(const std::string& sql) {
    Lexer lex(sql);
    auto toks = lex.tokenize();
    if (!toks.is_ok())
        return Result<ExecuteResult>::err(toks.error());

    Parser p(sql, std::move(toks.value()));
    auto stmts = p.parse();
    if (!stmts.is_ok())
        return Result<ExecuteResult>::err(stmts.error());
    if (stmts.value().empty())
        return Result<ExecuteResult>::ok({});

    Binder b(catalog_);
    auto bound = b.bind(stmts.value()[0], sql);
    if (!bound.is_ok())
        return Result<ExecuteResult>::err(bound.error());

    return std::visit(
        [&](auto& stmt) -> Result<ExecuteResult> {
            using T = std::decay_t<decltype(stmt)>;

            if constexpr (std::is_same_v<T, bound::BoundCreateTable>) {
                auto r = frontend::run_create_table(catalog_, stmt);
                if (!r.is_ok())
                    return Result<ExecuteResult>::err(r.error());

                if (stmt.shard_map.has_value()) {
                    const auto& sm = *stmt.shard_map;
                    for (const auto& pd : sm.partitions) {
                        auto sr = pool_->execute(pd.node_addr, token_, sql);
                        if (!sr.is_ok())
                            return Result<ExecuteResult>::err(
                                "coordinator: CREATE TABLE on shard " + pd.node_addr + ": " +
                                sr.error().message);
                    }
                }
                return Result<ExecuteResult>::ok({});

            } else if constexpr (std::is_same_v<T, bound::BoundDropTable>) {
                const ShardMapMeta* sm = catalog_.shard_map_of(stmt.table_name);
                if (sm) {
                    for (const auto& pd : sm->partitions) {
                        pool_->execute(pd.node_addr, token_, sql);
                    }
                }
                auto r = frontend::run_drop_table(catalog_, stmt);
                if (!r.is_ok())
                    return Result<ExecuteResult>::err(r.error());
                return Result<ExecuteResult>::ok({});

            } else if constexpr (std::is_same_v<T, bound::BoundAlterAddPartition>) {
                return handle_alter_partition_(stmt);

            } else if constexpr (std::is_same_v<T, bound::BoundVacuum>) {
                const ShardMapMeta* sm = catalog_.shard_map_of(stmt.table_name);
                if (sm) {
                    for (const auto& pd : sm->partitions)
                        pool_->execute(pd.node_addr, token_, sql);
                }
                return Result<ExecuteResult>::ok({});

            } else if constexpr (std::is_same_v<T, bound::BoundInsert>) {
                if (!catalog_.has_shard_map(stmt.table_name))
                    return Result<ExecuteResult>::err("coordinator: no shard map for table " +
                                                      stmt.table_name);
                return route_insert_(stmt, sql);

            } else if constexpr (std::is_same_v<T, bound::BoundDelete>) {
                if (!catalog_.has_shard_map(stmt.table_name))
                    return Result<ExecuteResult>::err("coordinator: no shard map for table " +
                                                      stmt.table_name);
                return route_delete_(stmt, sql);

            } else if constexpr (std::is_same_v<T, bound::BoundUpdate>) {
                if (!catalog_.has_shard_map(stmt.table_name))
                    return Result<ExecuteResult>::err("coordinator: no shard map for table " +
                                                      stmt.table_name);
                return route_update_(stmt, sql);

            } else if constexpr (std::is_same_v<T, bound::BoundSelect>) {
                const ShardMapMeta* sm = catalog_.shard_map_of(
                    stmt.bindings.empty() ? std::string{} : stmt.bindings[0].table_name);
                if (!sm)
                    return Result<ExecuteResult>::err(
                        "coordinator: no shard map for queried table");
                return fan_out_select_(stmt, sql, *sm);

            } else {
                const ShardMapMeta* sm = nullptr;
                if constexpr (std::is_same_v<T, bound::BoundCreateIndex>)
                    sm = catalog_.shard_map_of(stmt.table_name);
                else if constexpr (std::is_same_v<T, bound::BoundDropIndex>)
                    sm = catalog_.shard_map_of(stmt.table_name);

                if (sm) {
                    for (const auto& pd : sm->partitions)
                        pool_->execute(pd.node_addr, token_, sql);
                }
                return Result<ExecuteResult>::ok({});
            }
        },
        bound.value());
}

Result<ExecuteResult> Coordinator::route_insert_(const bound::BoundInsert& stmt,
                                                 const std::string& /*sql*/) {
    const ShardMapMeta* sm = catalog_.shard_map_of(stmt.table_name);
    if (!sm)
        return Result<ExecuteResult>::err("route_insert: no shard map");

    const Schema* sch = catalog_.schema_of(stmt.table_name);
    if (!sch)
        return Result<ExecuteResult>::err("route_insert: schema not found");

    std::unordered_map<size_t, std::vector<std::vector<Value>>> by_shard;
    for (const auto& row : stmt.rows) {
        Value pk_val = std::monostate{};
        if (sm->partition_col_idx < static_cast<u8>(row.size()))
            pk_val = row[sm->partition_col_idx];
        size_t shard = shard_for_value(*sm, pk_val);
        by_shard[shard].push_back(row);
    }

    u64 total = 0;
    for (auto& [shard_idx, rows] : by_shard) {
        const std::string& addr = sm->partitions[shard_idx].node_addr;

        std::string isql = "INSERT INTO " + stmt.table_name + " VALUES ";
        for (size_t ri = 0; ri < rows.size(); ++ri) {
            if (ri > 0)
                isql += ", ";
            isql += "(";
            const auto& row = rows[ri];
            for (size_t ci = 0; ci < row.size(); ++ci) {
                if (ci > 0)
                    isql += ", ";
                std::visit(
                    [&](const auto& v) {
                        using V = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<V, std::monostate>)
                            isql += "NULL";
                        else if constexpr (std::is_same_v<V, i32>)
                            isql += std::to_string(v);
                        else if constexpr (std::is_same_v<V, i64>)
                            isql += std::to_string(v);
                        else if constexpr (std::is_same_v<V, f64>)
                            isql += std::to_string(v);
                        else if constexpr (std::is_same_v<V, bool>)
                            isql += v ? "TRUE" : "FALSE";
                        else if constexpr (std::is_same_v<V, std::string>) {
                            isql += "'";
                            isql += v;
                            isql += "'";
                        } else if constexpr (std::is_same_v<V, Date>) {
                            isql += "DATE '";
                            isql += std::to_string(v.days);
                            isql += "'";
                        } else if constexpr (std::is_same_v<V, Timestamp>) {
                            isql += "TIMESTAMP '";
                            isql += std::to_string(v.micros);
                            isql += "'";
                        }
                    },
                    row[ci]);
            }
            isql += ")";
        }

        auto r = pool_->execute(addr, token_, isql);
        if (!r.is_ok())
            return Result<ExecuteResult>::err("coordinator INSERT on shard " + addr + ": " +
                                              r.error().message);
        total += r.value().rows_affected;
    }
    return Result<ExecuteResult>::ok({{}, {}, total});
}

Result<ExecuteResult> Coordinator::route_delete_(const bound::BoundDelete& /*stmt*/,
                                                 const std::string& sql) {
    std::vector<std::string> visited;
    u64 total = 0;
    for (const auto& tname : catalog_.table_names()) {
        const ShardMapMeta* tsm = catalog_.shard_map_of(tname);
        if (!tsm)
            continue;
        for (const auto& pd : tsm->partitions) {
            bool already = false;
            for (const auto& a : visited)
                if (a == pd.node_addr) {
                    already = true;
                    break;
                }
            if (already)
                continue;
            visited.push_back(pd.node_addr);
            auto r = pool_->execute(pd.node_addr, token_, sql);
            if (!r.is_ok())
                return Result<ExecuteResult>::err("coordinator DELETE on shard " + pd.node_addr +
                                                  ": " + r.error().message);
            total += r.value().rows_affected;
        }
    }
    return Result<ExecuteResult>::ok({{}, {}, total});
}

Result<ExecuteResult> Coordinator::route_update_(const bound::BoundUpdate& /*stmt*/,
                                                 const std::string& sql) {
    u64 total = 0;
    std::vector<std::string> visited;
    for (const auto& tname : catalog_.table_names()) {
        const ShardMapMeta* tsm = catalog_.shard_map_of(tname);
        if (!tsm)
            continue;
        for (const auto& pd : tsm->partitions) {
            bool already = false;
            for (const auto& a : visited)
                if (a == pd.node_addr) {
                    already = true;
                    break;
                }
            if (already)
                continue;
            visited.push_back(pd.node_addr);
            auto r = pool_->execute(pd.node_addr, token_, sql);
            if (!r.is_ok())
                return Result<ExecuteResult>::err("coordinator UPDATE on shard " + pd.node_addr +
                                                  ": " + r.error().message);
            total += r.value().rows_affected;
        }
    }
    return Result<ExecuteResult>::ok({{}, {}, total});
}

Result<ExecuteResult> Coordinator::fan_out_select_(const bound::BoundSelect& stmt,
                                                   const std::string& sql, const ShardMapMeta& sm) {
    std::vector<size_t> target_shards;
    const bound::BoundExpr* where = stmt.where.get();

    if (where) {
        if (const auto* bop = std::get_if<bound::BoundBinaryOp>(&where->node)) {
            if (const auto* colref = std::get_if<bound::BoundColumnRef>(&bop->left->node)) {
                if (colref->ref.column_idx == sm.partition_col_idx) {
                    Value lit_val = std::monostate{};
                    if (const auto* ilit = std::get_if<bound::BoundIntLit>(&bop->right->node))
                        lit_val = ilit->type == TypeId::INT32
                                      ? Value{static_cast<i32>(ilit->value)}
                                      : Value{static_cast<i64>(ilit->value)};
                    else if (const auto* dlit =
                                 std::get_if<bound::BoundDoubleLit>(&bop->right->node))
                        lit_val = dlit->value;
                    else if (const auto* slit =
                                 std::get_if<bound::BoundStringLit>(&bop->right->node))
                        lit_val = slit->value;
                    else if (const auto* dtlit =
                                 std::get_if<bound::BoundDateLit>(&bop->right->node))
                        lit_val = Date{dtlit->days};
                    else if (const auto* tslit =
                                 std::get_if<bound::BoundTimestampLit>(&bop->right->node))
                        lit_val = Timestamp{tslit->micros};

                    if (!is_null(lit_val)) {
                        target_shards =
                            prune_partitions(sm, static_cast<u8>(colref->ref.column_idx),
                                             static_cast<int>(bop->op), lit_val);
                    }
                }
            }
        }
    }

    if (target_shards.empty()) {
        for (size_t i = 0; i < sm.partitions.size(); ++i)
            target_shards.push_back(i);
    }

    std::vector<std::future<Result<ExecuteResult>>> futures;
    futures.reserve(target_shards.size());
    for (size_t idx : target_shards) {
        const std::string addr = sm.partitions[idx].node_addr;
        futures.push_back(std::async(std::launch::async, [addr, this, &sql]() {
            return pool_->execute(addr, token_, sql);
        }));
    }

    std::vector<ExecuteResult> parts;
    parts.reserve(futures.size());
    for (auto& f : futures) {
        auto r = f.get();
        if (!r.is_ok())
            return Result<ExecuteResult>::err(r.error().message);
        parts.push_back(std::move(r.value()));
    }

    return Result<ExecuteResult>::ok(merge_results(std::move(parts)));
}

Result<ExecuteResult>
Coordinator::handle_alter_partition_(const bound::BoundAlterAddPartition& stmt) {

    const ShardMapMeta& new_sm = stmt.updated_shard_map;

    const ShardMapMeta* old_sm = catalog_.shard_map_of(stmt.table_name);
    if (!old_sm)
        return Result<ExecuteResult>::err("alter_add_partition: table is not partitioned");

    const PartitionDef* new_pd = nullptr;
    for (const auto& pd : new_sm.partitions) {
        bool found = false;
        for (const auto& opd : old_sm->partitions)
            if (opd.name == pd.name) {
                found = true;
                break;
            }
        if (!found) {
            new_pd = &pd;
            break;
        }
    }
    if (!new_pd)
        return Result<ExecuteResult>::err("alter_add_partition: could not identify new partition");

    const Schema* sch = catalog_.schema_of(stmt.table_name);
    if (!sch)
        return Result<ExecuteResult>::err("alter_add_partition: schema not found");

    std::string ct_sql = "CREATE TABLE " + stmt.table_name + " (";
    for (size_t i = 0; i < sch->size(); ++i) {
        if (i > 0)
            ct_sql += ", ";
        const Column& col = (*sch)[i];
        ct_sql += col.name + " ";
        switch (col.type) {
        case TypeId::INT32:
            ct_sql += "INT";
            break;
        case TypeId::INT64:
            ct_sql += "BIGINT";
            break;
        case TypeId::DOUBLE:
            ct_sql += "DOUBLE";
            break;
        case TypeId::VARCHAR:
            ct_sql += "VARCHAR(" + std::to_string(col.max_len) + ")";
            break;
        case TypeId::BOOL:
            ct_sql += "BOOL";
            break;
        case TypeId::DATE:
            ct_sql += "DATE";
            break;
        case TypeId::TIMESTAMP:
            ct_sql += "TIMESTAMP";
            break;
        default:
            ct_sql += "INT";
            break;
        }
        if (!col.nullable)
            ct_sql += " NOT NULL";
    }
    ct_sql += ")";

    auto cr = pool_->execute(new_pd->node_addr, token_, ct_sql);
    if (!cr.is_ok())
        return Result<ExecuteResult>::err("alter_add_partition: CREATE TABLE on new shard: " +
                                          cr.error().message);

    const PartitionDef* src_pd = nullptr;
    for (const auto& opd : old_sm->partitions)
        if (opd.is_maxvalue) {
            src_pd = &opd;
            break;
        }
    if (!src_pd)
        return Result<ExecuteResult>::err("alter_add_partition: no MAXVALUE source partition");

    if (!new_pd->is_maxvalue) {
        std::string pred;
        std::visit(
            [&](const auto& v) {
                using V = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<V, i32>)
                    pred = std::to_string(v);
                else if constexpr (std::is_same_v<V, i64>)
                    pred = std::to_string(v);
                else if constexpr (std::is_same_v<V, f64>)
                    pred = std::to_string(v);
                else if constexpr (std::is_same_v<V, std::string>) {
                    pred = "'";
                    pred += v;
                    pred += "'";
                } else if constexpr (std::is_same_v<V, Date>)
                    pred = "'" + std::to_string(new_pd->upper_bound.index()) + "'";
                else if constexpr (std::is_same_v<V, Timestamp>)
                    pred = std::to_string(v.micros);
            },
            new_pd->upper_bound);

        if (!pred.empty()) {
            std::string sel = "SELECT * FROM " + stmt.table_name + " WHERE " +
                              new_sm.partition_col + " < " + pred;
            auto rows_r = pool_->execute(src_pd->node_addr, token_, sel);
            if (!rows_r.is_ok())
                return Result<ExecuteResult>::err("alter_add_partition: SELECT migration rows: " +
                                                  rows_r.error().message);

            const ExecuteResult& rows = rows_r.value();
            if (rows.row_count() > 0) {
                std::string ins = "INSERT INTO " + stmt.table_name + " VALUES ";
                for (size_t ri = 0; ri < rows.row_count(); ++ri) {
                    if (ri > 0)
                        ins += ", ";
                    ins += "(";
                    for (size_t ci = 0; ci < rows.columns.size(); ++ci) {
                        if (ci > 0)
                            ins += ", ";
                        const Value& v = rows.columns[ci][ri];
                        std::visit(
                            [&](const auto& x) {
                                using V = std::decay_t<decltype(x)>;
                                if constexpr (std::is_same_v<V, std::monostate>)
                                    ins += "NULL";
                                else if constexpr (std::is_same_v<V, i32>)
                                    ins += std::to_string(x);
                                else if constexpr (std::is_same_v<V, i64>)
                                    ins += std::to_string(x);
                                else if constexpr (std::is_same_v<V, f64>)
                                    ins += std::to_string(x);
                                else if constexpr (std::is_same_v<V, bool>)
                                    ins += x ? "TRUE" : "FALSE";
                                else if constexpr (std::is_same_v<V, std::string>) {
                                    ins += "'";
                                    ins += x;
                                    ins += "'";
                                } else if constexpr (std::is_same_v<V, Date>)
                                    ins += std::to_string(x.days);
                                else if constexpr (std::is_same_v<V, Timestamp>)
                                    ins += std::to_string(x.micros);
                            },
                            v);
                    }
                    ins += ")";
                }
                auto ir = pool_->execute(new_pd->node_addr, token_, ins);
                if (!ir.is_ok())
                    return Result<ExecuteResult>::err(
                        "alter_add_partition: INSERT migrated rows: " + ir.error().message);

                std::string del = "DELETE FROM " + stmt.table_name + " WHERE " +
                                  new_sm.partition_col + " < " + pred;
                pool_->execute(src_pd->node_addr, token_, del); // best-effort
            }
        }
    }

    auto r = frontend::run_alter_add_partition(catalog_, stmt);
    if (!r.is_ok())
        return Result<ExecuteResult>::err(r.error().message);

    return Result<ExecuteResult>::ok({});
}

} // namespace nyx::server
