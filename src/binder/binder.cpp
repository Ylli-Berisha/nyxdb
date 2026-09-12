#include "binder/binder.h"

#include "common/source_error.h"

#include <cstdint>
#include <limits>
#include <type_traits>
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
        return Result<bound::BoundExprPtr>::err(
            err_msg_("aggregate function '" + fc.name + "' not allowed here", fc.loc));
    }
    return Result<bound::BoundExprPtr>::err(err_msg_("unknown function: " + fc.name, fc.loc));
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
    auto projs = bind_projections_(stmt);
    if (projs.is_err())
        return Result<bound::BoundSelect>::err(projs.error());
    result.projections = std::move(projs.value());

    return Result<bound::BoundSelect>::ok(std::move(result));
}

Result<bound::BoundBinding> Binder::bind_table_ref_(const ast::TableRef& ref) {
    const Schema* schema = catalog_.schema_of(ref.table_name);
    if (!schema)
        return Result<bound::BoundBinding>::err(
            err_msg_("unknown table: " + ref.table_name, ref.loc));
    std::string alias = ref.alias.value_or(ref.table_name);
    return Result<bound::BoundBinding>::ok({ref.table_name, alias, schema});
}

Result<std::vector<bound::BoundProjection>> Binder::bind_projections_(
    const ast::SelectStmt& stmt) {
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
