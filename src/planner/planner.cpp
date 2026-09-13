#include "planner/planner.h"

#include "executor/filter.h"
#include "executor/limit.h"
#include "executor/project.h"
#include "executor/sort.h"
#include "executor/table_scan.h"

#include <numeric>
#include <type_traits>

namespace nyx {

Planner::Planner(Catalog& catalog) : catalog_(catalog) {}

std::unique_ptr<Expression> Planner::lower_expr_(const bound::BoundExpr& e, const ColCtx& ctx,
                                                 const std::vector<bound::BoundProjection>* projs) {

    return std::visit(
        [&](const auto& n) -> std::unique_ptr<Expression> {
            using T = std::decay_t<decltype(n)>;

            if constexpr (std::is_same_v<T, bound::BoundIntLit>) {
                if (n.type == TypeId::INT32)
                    return std::make_unique<Literal>(Value{i32(n.value)});
                return std::make_unique<Literal>(Value{i64(n.value)});
            } else if constexpr (std::is_same_v<T, bound::BoundDoubleLit>) {
                return std::make_unique<Literal>(Value{n.value});
            } else if constexpr (std::is_same_v<T, bound::BoundNullLit>) {
                return std::make_unique<Literal>(Value{std::monostate{}});
            } else if constexpr (std::is_same_v<T, bound::BoundColumnRef>) {
                if (ctx.post_aggregate) {
                    u64 key = (u64(n.ref.binding_id) << 32) | n.ref.column_idx;
                    u32 idx = ctx.group_col_map.at(key);
                    return std::make_unique<ColumnRef>(idx, n.ref.type);
                }
                u32 flat = ctx.binding_offsets[n.ref.binding_id] + n.ref.column_idx;
                return std::make_unique<ColumnRef>(flat, n.ref.type);
            } else if constexpr (std::is_same_v<T, bound::BoundAggregateRef>) {
                return std::make_unique<ColumnRef>(ctx.num_group_keys + n.aggregate_idx, n.type);
            } else if constexpr (std::is_same_v<T, bound::BoundProjectionRef>) {
                return lower_expr_(*(*projs)[n.proj_idx].expr, ctx, projs);
            } else if constexpr (std::is_same_v<T, bound::BoundBinaryOp>) {
                return std::make_unique<BinaryOp>(n.op, lower_expr_(*n.left, ctx, projs),
                                                  lower_expr_(*n.right, ctx, projs));
            } else if constexpr (std::is_same_v<T, bound::BoundLogicalOp>) {
                return std::make_unique<LogicalOp>(n.op, lower_expr_(*n.left, ctx, projs),
                                                   lower_expr_(*n.right, ctx, projs));
            } else if constexpr (std::is_same_v<T, bound::BoundNotOp>) {
                return std::make_unique<NotOp>(lower_expr_(*n.child, ctx, projs));
            } else {
                static_assert(std::is_same_v<T, bound::BoundNullCheck>);
                return std::make_unique<NullCheckOp>(n.kind, lower_expr_(*n.child, ctx, projs));
            }
        },
        e.node);
}

Result<std::unique_ptr<Operator>> Planner::build_scans_(const bound::BoundSelect& stmt,
                                                        ColCtx& ctx) {
    const auto& binding = stmt.bindings[0];
    Table* t = catalog_.table(binding.table_name);
    if (!t)
        return Result<std::unique_ptr<Operator>>::err("unknown table: " + binding.table_name);

    size_t ncols = binding.schema->size();
    std::vector<size_t> projected(ncols);
    std::iota(projected.begin(), projected.end(), 0);

    ctx.binding_offsets.resize(stmt.bindings.size(), 0u);

    return Result<std::unique_ptr<Operator>>::ok(
        std::make_unique<TableScan>(t, std::move(projected)));
}

std::unique_ptr<Operator> Planner::build_filter_(std::unique_ptr<Operator> child,
                                                 const bound::BoundExpr& pred, const ColCtx& ctx) {
    return std::make_unique<Filter>(std::move(child), lower_expr_(pred, ctx));
}

std::unique_ptr<Operator> Planner::build_project_(std::unique_ptr<Operator> child,
                                                  const bound::BoundSelect& stmt,
                                                  const ColCtx& ctx) {
    std::vector<ProjectItem> items;
    for (const auto& proj : stmt.projections) {
        std::string name;
        if (proj.alias.has_value()) {
            name = *proj.alias;
        } else if (auto* cr = std::get_if<bound::BoundColumnRef>(&proj.expr->node)) {
            u32 bid = cr->ref.binding_id;
            u32 cidx = cr->ref.column_idx;
            name = (*stmt.bindings[bid].schema)[cidx].name;
        }
        items.push_back({lower_expr_(*proj.expr, ctx, &stmt.projections), std::move(name),
                         bound::bound_expr_nullable(*proj.expr)});
    }
    return std::make_unique<Project>(std::move(child), std::move(items));
}

std::unique_ptr<Operator> Planner::build_sort_(std::unique_ptr<Operator> child,
                                               const bound::BoundSelect& stmt, const ColCtx& ctx) {
    std::vector<SortKey> keys;
    for (const auto& ob : stmt.order_by) {
        keys.push_back({lower_expr_(*ob.expr, ctx, &stmt.projections),
                        ob.ascending ? SortDirection::ASC : SortDirection::DESC});
    }
    return std::make_unique<Sort>(std::move(child), std::move(keys));
}

std::unique_ptr<Operator> Planner::build_limit_(std::unique_ptr<Operator> child,
                                                const bound::BoundSelect& stmt) {
    auto lim = static_cast<size_t>(*stmt.limit);
    if (stmt.offset.has_value())
        return std::make_unique<Limit>(std::move(child), static_cast<size_t>(*stmt.offset), lim);
    return std::make_unique<Limit>(std::move(child), lim);
}

Result<std::unique_ptr<Operator>> Planner::plan(const bound::BoundSelect& stmt) {
    ColCtx ctx;

    auto scans = build_scans_(stmt, ctx);
    if (!scans.is_ok())
        return scans;
    auto op = std::move(scans.value());

    if (stmt.where)
        op = build_filter_(std::move(op), *stmt.where, ctx);

    if (!stmt.order_by.empty())
        op = build_sort_(std::move(op), stmt, ctx);

    op = build_project_(std::move(op), stmt, ctx);

    if (stmt.limit.has_value())
        op = build_limit_(std::move(op), stmt);

    return Result<std::unique_ptr<Operator>>::ok(std::move(op));
}

} // namespace nyx
