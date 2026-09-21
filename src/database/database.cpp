#include "database/database.h"

#include "binder/binder.h"
#include "frontend/runner.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "planner/planner.h"

#include <filesystem>
#include <type_traits>

namespace nyx {

static Value extract(const ColumnVector& col, size_t row) {
    if (col.is_null(row))
        return std::monostate{};
    switch (col.type()) {
    case TypeId::INT32:
        return col.get_i32(row);
    case TypeId::INT64:
        return col.get_i64(row);
    case TypeId::VARCHAR:
        return col.get_str(row);
    case TypeId::BOOL:
        return col.get_bool(row);
    case TypeId::DATE:
        return col.get_date(row);
    case TypeId::TIMESTAMP:
        return col.get_timestamp(row);
    default:
        return col.get_f64(row);
    }
}

Database::Database(Catalog catalog)
    : catalog_(std::make_unique<Catalog>(std::move(catalog))),
      merge_worker_(std::make_unique<MergeWorker>(catalog_.get())) {
    merge_worker_->start();
}

Database::~Database() {
    if (merge_worker_)
        merge_worker_->stop();
}

Result<Database> Database::open(const std::string& data_root) {
    std::filesystem::create_directories(data_root);
    auto cat = Catalog::load(data_root);
    if (!cat.is_ok())
        return Result<Database>::err(cat.error());
    return Result<Database>::ok(Database(std::move(cat.value())));
}

Result<ExecuteResult> Database::execute(const std::string& sql) {
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

    Binder b(*catalog_);
    auto bound = b.bind(stmts.value()[0], sql);
    if (!bound.is_ok())
        return Result<ExecuteResult>::err(bound.error());

    return std::visit(
        [&](auto& stmt) -> Result<ExecuteResult> {
            using T = std::decay_t<decltype(stmt)>;

            if constexpr (std::is_same_v<T, bound::BoundCreateTable>) {
                auto r = frontend::run_create_table(*catalog_, stmt);
                if (!r.is_ok())
                    return Result<ExecuteResult>::err(r.error());
                return Result<ExecuteResult>::ok({});

            } else if constexpr (std::is_same_v<T, bound::BoundInsert>) {
                auto r = frontend::run_insert(*catalog_, stmt);
                if (!r.is_ok())
                    return Result<ExecuteResult>::err(r.error());
                return Result<ExecuteResult>::ok({{}, {}, r.value()});

            } else if constexpr (std::is_same_v<T, bound::BoundDropTable>) {
                auto r = frontend::run_drop_table(*catalog_, stmt);
                if (!r.is_ok())
                    return Result<ExecuteResult>::err(r.error());
                return Result<ExecuteResult>::ok({});

            } else if constexpr (std::is_same_v<T, bound::BoundDelete>) {
                auto r = frontend::run_delete(*catalog_, stmt);
                if (!r.is_ok())
                    return Result<ExecuteResult>::err(r.error());
                return Result<ExecuteResult>::ok({{}, {}, r.value()});

            } else if constexpr (std::is_same_v<T, bound::BoundUpdate>) {
                auto r = frontend::run_update(*catalog_, stmt);
                if (!r.is_ok())
                    return Result<ExecuteResult>::err(r.error());
                return Result<ExecuteResult>::ok({{}, {}, r.value()});

            } else if constexpr (std::is_same_v<T, bound::BoundCreateIndex>) {
                auto r = frontend::run_create_index(*catalog_, stmt);
                if (!r.is_ok())
                    return Result<ExecuteResult>::err(r.error());
                return Result<ExecuteResult>::ok({});

            } else if constexpr (std::is_same_v<T, bound::BoundDropIndex>) {
                auto r = frontend::run_drop_index(*catalog_, stmt);
                if (!r.is_ok())
                    return Result<ExecuteResult>::err(r.error());
                return Result<ExecuteResult>::ok({});

            } else if constexpr (std::is_same_v<T, bound::BoundShowIndexes>) {
                const auto& metas = catalog_->indexes_of(stmt.table_name);
                ExecuteResult result;
                result.schema = {
                    Column{"index_name", TypeId::VARCHAR, false, 255},
                    Column{"columns", TypeId::VARCHAR, false, 255},
                    Column{"is_unique", TypeId::BOOL, false, 0},
                };
                result.columns.resize(3);
                for (const auto& m : metas) {
                    result.columns[0].push_back(Value{m.name});
                    std::string col_list;
                    for (usize i = 0; i < m.col_indices.size(); ++i) {
                        if (i > 0)
                            col_list += ", ";
                        const Schema* sch = catalog_->schema_of(stmt.table_name);
                        col_list += sch ? (*sch)[m.col_indices[i]].name : "?";
                    }
                    result.columns[1].push_back(Value{col_list});
                    result.columns[2].push_back(Value{m.unique});
                }
                return Result<ExecuteResult>::ok(std::move(result));

            } else if constexpr (std::is_same_v<T, bound::BoundShowConstraints>) {
                const auto& metas = catalog_->constraints_of(stmt.table_name);
                ExecuteResult result;
                result.schema = {
                    Column{"constraint_name", TypeId::VARCHAR, false, 255},
                    Column{"kind", TypeId::VARCHAR, false, 255},
                    Column{"columns", TypeId::VARCHAR, false, 255},
                };
                result.columns.resize(3);
                for (const auto& m : metas) {
                    result.columns[0].push_back(Value{m.name});
                    result.columns[1].push_back(Value{std::string(
                        m.kind == ConstraintKind::PRIMARY_KEY ? "PRIMARY KEY" : "UNIQUE")});
                    std::string col_list;
                    const Schema* sch = catalog_->schema_of(stmt.table_name);
                    for (usize i = 0; i < m.col_indices.size(); ++i) {
                        if (i > 0)
                            col_list += ", ";
                        col_list += sch ? (*sch)[m.col_indices[i]].name : "?";
                    }
                    result.columns[2].push_back(Value{col_list});
                }
                return Result<ExecuteResult>::ok(std::move(result));

            } else {
                static_assert(std::is_same_v<T, bound::BoundSelect>);
                Planner pl(*catalog_);
                auto plan_r = pl.plan(stmt);
                if (!plan_r.is_ok())
                    return Result<ExecuteResult>::err(plan_r.error());
                auto op = std::move(plan_r.value());

                auto open_r = op->open();
                if (!open_r.is_ok())
                    return Result<ExecuteResult>::err(open_r.error());

                ExecuteResult result;
                result.schema = op->output_schema();
                result.columns.resize(result.schema.size());

                while (true) {
                    auto next_r = op->next();
                    if (!next_r.is_ok())
                        return Result<ExecuteResult>::err(next_r.error());
                    if (!next_r.value())
                        break;
                    auto& chunk = *next_r.value();
                    for (size_t col = 0; col < chunk.column_count(); col++) {
                        const auto& cv = chunk.column(col);
                        for (size_t row = 0; row < chunk.row_count(); row++)
                            result.columns[col].push_back(extract(cv, row));
                    }
                }

                return Result<ExecuteResult>::ok(std::move(result));
            }
        },
        bound.value());
}

} // namespace nyx
