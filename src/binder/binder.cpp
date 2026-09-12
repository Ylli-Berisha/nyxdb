#include "binder/binder.h"

#include "common/source_error.h"

#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nyx {

namespace {

const std::unordered_set<std::string>& aggregate_names() {
    static const std::unordered_set<std::string> names = {"count", "sum", "avg", "min", "max"};
    return names;
}

} // namespace

Binder::Binder(Catalog& catalog) : catalog_(catalog) {}

std::string Binder::err_msg_(const std::string& msg, SourceLoc loc) const {
    return render_source_error(source_, loc, "bind error", msg);
}

Result<bound::BoundExprPtr>
Binder::bind_expression(const ast::Expr& expr, const std::vector<bound::BoundBinding>& bindings,
                        std::string_view source) {
    bindings_ = &bindings;
    source_ = source;
    return bind_expr_(expr);
}

Result<bound::BoundExprPtr> Binder::bind_expr_(const ast::Expr& e) {
    return std::visit(
        [this](const auto& n) -> Result<bound::BoundExprPtr> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::IntLit>)
                return bind_int_lit_(n);
            else if constexpr (std::is_same_v<T, ast::DoubleLit>)
                return bind_double_lit_(n);
            else if constexpr (std::is_same_v<T, ast::NullLit>)
                return bind_null_lit_(n);
            else if constexpr (std::is_same_v<T, ast::ColumnRef>)
                return bind_column_ref_(n);
            else if constexpr (std::is_same_v<T, ast::FuncCall>)
                return bind_func_call_(n);
            else if constexpr (std::is_same_v<T, ast::BinaryOp>)
                return bind_binary_op_(n);
            else if constexpr (std::is_same_v<T, ast::LogicalOp>)
                return bind_logical_op_(n);
            else if constexpr (std::is_same_v<T, ast::NotOp>)
                return bind_not_op_(n);
            else if constexpr (std::is_same_v<T, ast::NullCheck>)
                return bind_null_check_(n);
        },
        e.node);
}

Result<bound::BoundExprPtr> Binder::bind_int_lit_(const ast::IntLit& lit) {
    TypeId t = (lit.value >= std::numeric_limits<i32>::min() &&
                lit.value <= std::numeric_limits<i32>::max())
                   ? TypeId::INT32
                   : TypeId::INT64;
    return Result<bound::BoundExprPtr>::ok(bound::make_bound(bound::BoundIntLit{lit.value, t}));
}

Result<bound::BoundExprPtr> Binder::bind_double_lit_(const ast::DoubleLit& lit) {
    return Result<bound::BoundExprPtr>::ok(bound::make_bound(bound::BoundDoubleLit{lit.value}));
}

Result<bound::BoundExprPtr> Binder::bind_null_lit_(const ast::NullLit& lit) {
    (void)lit;
    return Result<bound::BoundExprPtr>::ok(bound::make_bound(bound::BoundNullLit{TypeId::INT32}));
}

Result<bound::BoundExprPtr> Binder::bind_column_ref_(const ast::ColumnRef& ref) {
    std::pair<u32, u32> found{0, 0};
    u32 match_count = 0;
    for (u32 b = 0; b < bindings_->size(); ++b) {
        const auto& binding = (*bindings_)[b];
        if (ref.table.has_value() && *ref.table != binding.alias &&
            *ref.table != binding.table_name)
            continue;
        for (u32 c = 0; c < binding.schema->size(); ++c) {
            if ((*binding.schema)[c].name == ref.column) {
                found = {b, c};
                ++match_count;
            }
        }
    }
    if (match_count == 0) {
        std::string what = ref.table.has_value() ? *ref.table + "." + ref.column : ref.column;
        return Result<bound::BoundExprPtr>::err(err_msg_("unknown column: " + what, ref.loc));
    }
    if (match_count > 1) {
        return Result<bound::BoundExprPtr>::err(
            err_msg_("ambiguous column: " + ref.column, ref.loc));
    }
    const auto& col = (*(*bindings_)[found.first].schema)[found.second];
    return Result<bound::BoundExprPtr>::ok(bound::make_bound(bound::BoundColumnRef{
        bound::BindingRef{found.first, found.second, col.type, col.nullable}}));
}

Result<bound::BoundExprPtr> Binder::bind_func_call_(const ast::FuncCall& fc) {
    if (aggregate_names().count(fc.name) > 0) {
        if (aggregates_ != nullptr)
            return bind_aggregate_(fc);
        return Result<bound::BoundExprPtr>::err(
            err_msg_("aggregate function '" + fc.name + "' not allowed here", fc.loc));
    }
    return Result<bound::BoundExprPtr>::err(err_msg_("unknown function: " + fc.name, fc.loc));
}

Result<bound::BoundExprPtr> Binder::bind_aggregate_(const ast::FuncCall& fc) {
    AggregateKind kind;
    TypeId output_type;
    bound::BoundExprPtr arg;

    if (fc.name == "count") {
        kind = fc.star ? AggregateKind::COUNT_STAR : AggregateKind::COUNT;
        output_type = TypeId::INT64;
        if (!fc.star) {
            if (fc.args.empty())
                return Result<bound::BoundExprPtr>::err(
                    err_msg_("count requires an argument", fc.loc));
            auto ar = bind_expr_(*fc.args[0]);
            if (ar.is_err())
                return ar;
            arg = std::move(ar.value());
        }
    } else if (fc.name == "sum") {
        if (fc.args.empty())
            return Result<bound::BoundExprPtr>::err(err_msg_("sum requires an argument", fc.loc));
        auto ar = bind_expr_(*fc.args[0]);
        if (ar.is_err())
            return ar;
        TypeId t = bound::bound_expr_type(*ar.value());
        output_type = (t == TypeId::DOUBLE) ? TypeId::DOUBLE : TypeId::INT64;
        kind = AggregateKind::SUM;
        arg = std::move(ar.value());
    } else if (fc.name == "avg") {
        if (fc.args.empty())
            return Result<bound::BoundExprPtr>::err(err_msg_("avg requires an argument", fc.loc));
        auto ar = bind_expr_(*fc.args[0]);
        if (ar.is_err())
            return ar;
        output_type = TypeId::DOUBLE;
        kind = AggregateKind::AVG;
        arg = std::move(ar.value());
    } else {
        if (fc.args.empty())
            return Result<bound::BoundExprPtr>::err(
                err_msg_(fc.name + " requires an argument", fc.loc));
        auto ar = bind_expr_(*fc.args[0]);
        if (ar.is_err())
            return ar;
        output_type = bound::bound_expr_type(*ar.value());
        kind = (fc.name == "min") ? AggregateKind::MIN : AggregateKind::MAX;
        arg = std::move(ar.value());
    }

    u32 idx = static_cast<u32>(aggregates_->size());
    aggregates_->push_back({kind, std::move(arg), output_type});
    return Result<bound::BoundExprPtr>::ok(
        bound::make_bound(bound::BoundAggregateRef{idx, output_type}));
}

void Binder::retype_lit_if_possible_(bound::BoundExpr& e, TypeId target) {
    if (auto* lit = std::get_if<bound::BoundIntLit>(&e.node)) {
        if (target == TypeId::INT32) {
            if (lit->value >= std::numeric_limits<i32>::min() &&
                lit->value <= std::numeric_limits<i32>::max())
                lit->type = TypeId::INT32;
        } else if (target == TypeId::INT64) {
            lit->type = TypeId::INT64;
        }
    } else if (auto* lit = std::get_if<bound::BoundNullLit>(&e.node)) {
        lit->type = target;
    }
}

Result<bound::BoundExprPtr> Binder::bind_binary_op_(const ast::BinaryOp& bop) {
    auto lr = bind_expr_(*bop.left);
    if (lr.is_err())
        return lr;
    auto rr = bind_expr_(*bop.right);
    if (rr.is_err())
        return rr;
    auto left = std::move(lr.value());
    auto right = std::move(rr.value());

    TypeId lt = bound::bound_expr_type(*left);
    TypeId rt = bound::bound_expr_type(*right);
    if (lt != rt) {
        retype_lit_if_possible_(*left, rt);
        retype_lit_if_possible_(*right, lt);
        lt = bound::bound_expr_type(*left);
        rt = bound::bound_expr_type(*right);
    }
    if (lt != rt) {
        return Result<bound::BoundExprPtr>::err(err_msg_(
            std::string("type mismatch: ") + bound::type_name(lt) + " vs " + bound::type_name(rt),
            bop.loc));
    }

    TypeId result_type;
    switch (bop.op) {
    case BinaryOpKind::ADD:
    case BinaryOpKind::SUB:
    case BinaryOpKind::MUL:
    case BinaryOpKind::DIV:
        result_type = lt;
        break;
    default:
        result_type = TypeId::INT32;
        break;
    }

    return Result<bound::BoundExprPtr>::ok(bound::make_bound(
        bound::BoundBinaryOp{bop.op, std::move(left), std::move(right), lt, result_type}));
}

Result<bound::BoundExprPtr> Binder::bind_logical_op_(const ast::LogicalOp& lop) {
    auto lr = bind_expr_(*lop.left);
    if (lr.is_err())
        return lr;
    auto rr = bind_expr_(*lop.right);
    if (rr.is_err())
        return rr;
    auto left = std::move(lr.value());
    auto right = std::move(rr.value());

    const char* op = lop.op == LogicalOpKind::AND ? "AND" : "OR";
    if (bound::bound_expr_type(*left) != TypeId::INT32) {
        return Result<bound::BoundExprPtr>::err(
            err_msg_(std::string(op) + " operand must be boolean (INT32), got " +
                         bound::type_name(bound::bound_expr_type(*left)),
                     lop.loc));
    }
    if (bound::bound_expr_type(*right) != TypeId::INT32) {
        return Result<bound::BoundExprPtr>::err(
            err_msg_(std::string(op) + " operand must be boolean (INT32), got " +
                         bound::type_name(bound::bound_expr_type(*right)),
                     lop.loc));
    }
    return Result<bound::BoundExprPtr>::ok(
        bound::make_bound(bound::BoundLogicalOp{lop.op, std::move(left), std::move(right)}));
}

Result<bound::BoundExprPtr> Binder::bind_not_op_(const ast::NotOp& nop) {
    auto cr = bind_expr_(*nop.child);
    if (cr.is_err())
        return cr;
    auto child = std::move(cr.value());
    if (bound::bound_expr_type(*child) != TypeId::INT32) {
        return Result<bound::BoundExprPtr>::err(
            err_msg_("NOT operand must be boolean (INT32), got " +
                         std::string(bound::type_name(bound::bound_expr_type(*child))),
                     nop.loc));
    }
    return Result<bound::BoundExprPtr>::ok(bound::make_bound(bound::BoundNotOp{std::move(child)}));
}

Result<bound::BoundStatement> Binder::bind(const ast::Statement& stmt, std::string_view source) {
    return std::visit(
        [&](const auto& s) -> Result<bound::BoundStatement> {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, ast::SelectStmt>) {
                auto r = bind_select(s, source);
                if (r.is_err())
                    return Result<bound::BoundStatement>::err(r.error());
                return Result<bound::BoundStatement>::ok(std::move(r.value()));
            } else if constexpr (std::is_same_v<T, ast::CreateTableStmt>) {
                auto r = bind_create_table(s, source);
                if (r.is_err())
                    return Result<bound::BoundStatement>::err(r.error());
                return Result<bound::BoundStatement>::ok(std::move(r.value()));
            } else if constexpr (std::is_same_v<T, ast::InsertStmt>) {
                auto r = bind_insert(s, source);
                if (r.is_err())
                    return Result<bound::BoundStatement>::err(r.error());
                return Result<bound::BoundStatement>::ok(std::move(r.value()));
            }
        },
        stmt);
}

Result<bound::BoundInsert> Binder::bind_insert(const ast::InsertStmt& stmt,
                                               std::string_view source) {
    source_ = source;

    const Schema* schema = catalog_.schema_of(stmt.table_name);
    if (!schema)
        return Result<bound::BoundInsert>::err(
            err_msg_("unknown table: " + stmt.table_name, SourceLoc{0, 0}));

    u32 schema_size = static_cast<u32>(schema->size());

    std::vector<u32> target_cols;
    if (stmt.columns.empty()) {
        for (u32 i = 0; i < schema_size; ++i)
            target_cols.push_back(i);
    } else {
        std::unordered_set<std::string> seen;
        for (const auto& col_name : stmt.columns) {
            if (!seen.insert(col_name).second)
                return Result<bound::BoundInsert>::err(
                    err_msg_("duplicate column in INSERT: " + col_name, SourceLoc{0, 0}));
            bool found = false;
            for (u32 j = 0; j < schema_size; ++j) {
                if ((*schema)[j].name == col_name) {
                    target_cols.push_back(j);
                    found = true;
                    break;
                }
            }
            if (!found)
                return Result<bound::BoundInsert>::err(
                    err_msg_("unknown column: " + col_name, SourceLoc{0, 0}));
        }
    }

    static const std::vector<bound::BoundBinding> no_bindings;
    bindings_ = &no_bindings;

    std::vector<std::vector<Value>> rows;
    for (const auto& row_exprs : stmt.rows) {
        if (row_exprs.size() != target_cols.size())
            return Result<bound::BoundInsert>::err(
                err_msg_("arity mismatch: expected " + std::to_string(target_cols.size()) +
                             " values, got " + std::to_string(row_exprs.size()),
                         SourceLoc{0, 0}));

        std::vector<Value> schema_row(schema_size, std::monostate{});
        for (u32 i = 0; i < static_cast<u32>(row_exprs.size()); ++i) {
            auto bnd = bind_expr_(*row_exprs[i]);
            if (bnd.is_err())
                return Result<bound::BoundInsert>::err(bnd.error());
            auto val = fold_constant_expr_(*bnd.value(), (*schema)[target_cols[i]]);
            if (val.is_err())
                return Result<bound::BoundInsert>::err(val.error());
            schema_row[target_cols[i]] = std::move(val.value());
        }

        for (u32 i = 0; i < schema_size; ++i) {
            if (!(*schema)[i].nullable && is_null(schema_row[i]))
                return Result<bound::BoundInsert>::err(
                    err_msg_("NULL into NOT NULL column: " + (*schema)[i].name, SourceLoc{0, 0}));
        }

        rows.push_back(std::move(schema_row));
    }

    return Result<bound::BoundInsert>::ok({stmt.table_name, std::move(rows)});
}

Result<Value> Binder::fold_constant_expr_(const bound::BoundExpr& e, const Column& col) {
    if (const auto* lit = std::get_if<bound::BoundIntLit>(&e.node)) {
        if (col.type == TypeId::INT64)
            return Result<Value>::ok(static_cast<i64>(lit->value));
        if (col.type == TypeId::INT32 && lit->type == TypeId::INT32)
            return Result<Value>::ok(static_cast<i32>(lit->value));
        return Result<Value>::err(
            err_msg_("type mismatch for column: " + col.name, SourceLoc{0, 0}));
    }
    if (const auto* lit = std::get_if<bound::BoundDoubleLit>(&e.node)) {
        if (col.type != TypeId::DOUBLE)
            return Result<Value>::err(
                err_msg_("type mismatch for column: " + col.name, SourceLoc{0, 0}));
        return Result<Value>::ok(lit->value);
    }
    if (std::holds_alternative<bound::BoundNullLit>(e.node))
        return Result<Value>::ok(std::monostate{});
    return Result<Value>::err(err_msg_("INSERT values must be constants", SourceLoc{0, 0}));
}

Result<bound::BoundCreateTable> Binder::bind_create_table(const ast::CreateTableStmt& stmt,
                                                          std::string_view source) {
    source_ = source;
    std::unordered_set<std::string> seen;
    Schema schema;
    for (const auto& col : stmt.columns) {
        if (!seen.insert(col.name).second)
            return Result<bound::BoundCreateTable>::err(
                err_msg_("duplicate column name: " + col.name, SourceLoc{0, 0}));
        schema.push_back({col.name, col.type, col.nullable});
    }
    return Result<bound::BoundCreateTable>::ok({stmt.table_name, std::move(schema)});
}

Result<bound::BoundSelect> Binder::bind_select(const ast::SelectStmt& stmt,
                                               std::string_view source) {
    source_ = source;
    bound::BoundSelect result;

    auto from = bind_table_ref_(stmt.from);
    if (from.is_err())
        return Result<bound::BoundSelect>::err(from.error());
    result.bindings.push_back(std::move(from.value()));

    for (const auto& join : stmt.joins) {
        auto jb = bind_table_ref_(join.right);
        if (jb.is_err())
            return Result<bound::BoundSelect>::err(jb.error());
        result.bindings.push_back(std::move(jb.value()));

        bindings_ = &result.bindings;
        auto on = bind_expr_(*join.on);
        if (on.is_err())
            return Result<bound::BoundSelect>::err(on.error());
        result.join_predicates.push_back(std::move(on.value()));
    }

    bindings_ = &result.bindings;

    if (stmt.where) {
        auto wr = bind_expr_(*stmt.where);
        if (wr.is_err())
            return Result<bound::BoundSelect>::err(wr.error());
        result.where = std::move(wr.value());
    }

    for (const auto& gb : stmt.group_by) {
        auto gr = bind_expr_(*gb);
        if (gr.is_err())
            return Result<bound::BoundSelect>::err(gr.error());
        result.group_by.push_back(std::move(gr.value()));
    }

    aggregates_ = &result.aggregates;

    auto projs = bind_projections_(stmt);
    if (projs.is_err()) {
        aggregates_ = nullptr;
        return Result<bound::BoundSelect>::err(projs.error());
    }
    result.projections = std::move(projs.value());

    if (stmt.having) {
        auto hr = bind_expr_(*stmt.having);
        if (hr.is_err()) {
            aggregates_ = nullptr;
            return Result<bound::BoundSelect>::err(hr.error());
        }
        result.having = std::move(hr.value());
    }

    aggregates_ = nullptr;

    result.is_aggregated = !result.group_by.empty() || !result.aggregates.empty();

    if (result.is_aggregated) {
        for (const auto& proj : result.projections) {
            if (has_ungrouped_col_(*proj.expr, result.group_by)) {
                return Result<bound::BoundSelect>::err(
                    err_msg_("non-aggregated column in aggregated query", SourceLoc{0, 0}));
            }
        }
    }

    aggregates_ = &result.aggregates;
    auto obs = bind_order_by_(stmt, result.projections);
    aggregates_ = nullptr;
    if (obs.is_err())
        return Result<bound::BoundSelect>::err(obs.error());
    result.order_by = std::move(obs.value());

    result.limit = stmt.limit;
    result.offset = stmt.offset;

    return Result<bound::BoundSelect>::ok(std::move(result));
}

Result<std::vector<bound::BoundOrderBy>>
Binder::bind_order_by_(const ast::SelectStmt& stmt,
                       const std::vector<bound::BoundProjection>& projections) {

    std::unordered_map<std::string, u32> alias_map;
    for (u32 i = 0; i < projections.size(); ++i) {
        if (projections[i].alias.has_value())
            alias_map[*projections[i].alias] = i;
    }

    std::vector<bound::BoundOrderBy> result;
    for (const auto& item : stmt.order_by) {
        if (const auto* col = std::get_if<ast::ColumnRef>(&item.expr->node)) {
            if (!col->table.has_value()) {
                auto it = alias_map.find(col->column);
                if (it != alias_map.end()) {
                    TypeId t = bound::bound_expr_type(*projections[it->second].expr);
                    result.push_back({bound::make_bound(bound::BoundProjectionRef{it->second, t}),
                                      item.ascending});
                    continue;
                }
            }
        }
        auto er = bind_expr_(*item.expr);
        if (er.is_err())
            return Result<std::vector<bound::BoundOrderBy>>::err(er.error());
        result.push_back({std::move(er.value()), item.ascending});
    }

    return Result<std::vector<bound::BoundOrderBy>>::ok(std::move(result));
}

bool Binder::has_ungrouped_col_(const bound::BoundExpr& e,
                                const std::vector<bound::BoundExprPtr>& group_by) const {
    return std::visit(
        [&](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, bound::BoundColumnRef>) {
                for (const auto& gb : group_by) {
                    if (const auto* gc = std::get_if<bound::BoundColumnRef>(&gb->node)) {
                        if (gc->ref.binding_id == n.ref.binding_id &&
                            gc->ref.column_idx == n.ref.column_idx)
                            return false;
                    }
                }
                return true;
            } else if constexpr (std::is_same_v<T, bound::BoundAggregateRef> ||
                                 std::is_same_v<T, bound::BoundProjectionRef> ||
                                 std::is_same_v<T, bound::BoundIntLit> ||
                                 std::is_same_v<T, bound::BoundDoubleLit> ||
                                 std::is_same_v<T, bound::BoundNullLit>) {
                return false;
            } else if constexpr (std::is_same_v<T, bound::BoundBinaryOp>) {
                return has_ungrouped_col_(*n.left, group_by) ||
                       has_ungrouped_col_(*n.right, group_by);
            } else if constexpr (std::is_same_v<T, bound::BoundLogicalOp>) {
                return has_ungrouped_col_(*n.left, group_by) ||
                       has_ungrouped_col_(*n.right, group_by);
            } else if constexpr (std::is_same_v<T, bound::BoundNotOp>) {
                return has_ungrouped_col_(*n.child, group_by);
            } else {
                return has_ungrouped_col_(*n.child, group_by);
            }
        },
        e.node);
}

Result<bound::BoundBinding> Binder::bind_table_ref_(const ast::TableRef& ref) {
    const Schema* schema = catalog_.schema_of(ref.table_name);
    if (!schema)
        return Result<bound::BoundBinding>::err(
            err_msg_("unknown table: " + ref.table_name, ref.loc));
    std::string alias = ref.alias.value_or(ref.table_name);
    return Result<bound::BoundBinding>::ok({ref.table_name, alias, schema});
}

Result<std::vector<bound::BoundProjection>> Binder::bind_projections_(const ast::SelectStmt& stmt) {
    std::vector<bound::BoundProjection> result;

    if (stmt.star_projection) {
        for (u32 b = 0; b < bindings_->size(); ++b) {
            const auto& binding = (*bindings_)[b];
            for (u32 c = 0; c < binding.schema->size(); ++c) {
                const auto& col = (*binding.schema)[c];
                result.push_back({bound::make_bound(bound::BoundColumnRef{
                                      bound::BindingRef{b, c, col.type, col.nullable}}),
                                  std::nullopt});
            }
        }
        return Result<std::vector<bound::BoundProjection>>::ok(std::move(result));
    }

    for (const auto& item : stmt.projections) {
        auto er = bind_expr_(*item.expr);
        if (er.is_err())
            return Result<std::vector<bound::BoundProjection>>::err(er.error());
        result.push_back({std::move(er.value()), item.alias});
    }

    return Result<std::vector<bound::BoundProjection>>::ok(std::move(result));
}

Result<bound::BoundExprPtr> Binder::bind_null_check_(const ast::NullCheck& nc) {
    auto cr = bind_expr_(*nc.child);
    if (cr.is_err())
        return cr;
    return Result<bound::BoundExprPtr>::ok(
        bound::make_bound(bound::BoundNullCheck{nc.kind, std::move(cr.value())}));
}

} // namespace nyx
