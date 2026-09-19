#include "planner/planner.h"

#include "executor/filter.h"
#include "executor/hash_aggregate.h"
#include "executor/hash_join.h"
#include "executor/index_scan.h"
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
            } else if constexpr (std::is_same_v<T, bound::BoundStringLit>) {
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
            } else if constexpr (std::is_same_v<T, bound::BoundBoolLit>) {
                return std::make_unique<Literal>(Value{n.value});
            } else if constexpr (std::is_same_v<T, bound::BoundDateLit>) {
                return std::make_unique<Literal>(Value{Date{n.days}});
            } else if constexpr (std::is_same_v<T, bound::BoundTimestampLit>) {
                return std::make_unique<Literal>(Value{Timestamp{n.micros}});
            } else {
                static_assert(std::is_same_v<T, bound::BoundNullCheck>);
                return std::make_unique<NullCheckOp>(n.kind, lower_expr_(*n.child, ctx, projs));
            }
        },
        e.node);
}

static std::pair<Table*, std::unique_ptr<TableScan>> make_scan(Catalog& cat,
                                                               const bound::BoundBinding& b) {
    Table* t = cat.table(b.table_name);
    if (!t)
        return {nullptr, nullptr};
    size_t n = b.schema->size();
    std::vector<size_t> proj(n);
    std::iota(proj.begin(), proj.end(), 0);
    return {t, std::make_unique<TableScan>(t, std::move(proj))};
}

static std::optional<Value> extract_literal_(const bound::BoundExpr& e) {
    return std::visit([](const auto& n) -> std::optional<Value> {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, bound::BoundIntLit>) {
            if (n.type == TypeId::INT32) return Value{i32(n.value)};
            return Value{i64(n.value)};
        } else if constexpr (std::is_same_v<T, bound::BoundDoubleLit>) {
            return Value{n.value};
        } else if constexpr (std::is_same_v<T, bound::BoundStringLit>) {
            return Value{n.value};
        } else if constexpr (std::is_same_v<T, bound::BoundBoolLit>) {
            return Value{n.value};
        } else if constexpr (std::is_same_v<T, bound::BoundDateLit>) {
            return Value{Date{n.days}};
        } else if constexpr (std::is_same_v<T, bound::BoundTimestampLit>) {
            return Value{Timestamp{n.micros}};
        } else {
            return std::nullopt;
        }
    }, e.node);
}

std::optional<Planner::IndexScanChoice> Planner::try_index_scan_(
    const bound::BoundBinding& binding, const bound::BoundExpr& where) {

    const auto* bop = std::get_if<bound::BoundBinaryOp>(&where.node);
    if (!bop) return std::nullopt;

    bool is_range_op = (bop->op == BinaryOpKind::EQ ||
                        bop->op == BinaryOpKind::LT  ||
                        bop->op == BinaryOpKind::LE  ||
                        bop->op == BinaryOpKind::GT  ||
                        bop->op == BinaryOpKind::GE);
    if (!is_range_op) return std::nullopt;

    const auto* left_cr  = std::get_if<bound::BoundColumnRef>(&bop->left->node);
    const auto* right_cr = std::get_if<bound::BoundColumnRef>(&bop->right->node);

    u32   col_idx;
    Value lit_val;
    bool  col_is_left;

    if (left_cr && left_cr->ref.binding_id == 0) {
        auto lit = extract_literal_(*bop->right);
        if (!lit) return std::nullopt;
        col_idx     = left_cr->ref.column_idx;
        lit_val     = *lit;
        col_is_left = true;
    } else if (right_cr && right_cr->ref.binding_id == 0) {
        auto lit = extract_literal_(*bop->left);
        if (!lit) return std::nullopt;
        col_idx     = right_cr->ref.column_idx;
        lit_val     = *lit;
        col_is_left = false;
    } else {
        return std::nullopt;
    }

    BTreeIndex* found = nullptr;
    for (const auto& meta : catalog_.indexes_of(binding.table_name)) {
        if (meta.col_indices.size() == 1 && meta.col_indices[0] == static_cast<u8>(col_idx)) {
            found = catalog_.btree_index(binding.table_name, meta.name);
            break;
        }
    }
    if (!found) return std::nullopt;

    std::vector<Value> key = {lit_val};
    auto make_bnd = [&](bool incl) { return IndexScan::Bound{key, incl}; };

    std::optional<IndexScan::Bound> lo, hi;
    if (col_is_left) {
        switch (bop->op) {
        case BinaryOpKind::EQ: lo = make_bnd(true);  hi = make_bnd(true);  break;
        case BinaryOpKind::LT: hi = make_bnd(false); break;
        case BinaryOpKind::LE: hi = make_bnd(true);  break;
        case BinaryOpKind::GT: lo = make_bnd(false); break;
        case BinaryOpKind::GE: lo = make_bnd(true);  break;
        default: return std::nullopt;
        }
    } else {
        switch (bop->op) {
        case BinaryOpKind::EQ: lo = make_bnd(true);  hi = make_bnd(true);  break;
        case BinaryOpKind::LT: lo = make_bnd(false); break;
        case BinaryOpKind::LE: lo = make_bnd(true);  break;
        case BinaryOpKind::GT: hi = make_bnd(false); break;
        case BinaryOpKind::GE: hi = make_bnd(true);  break;
        default: return std::nullopt;
        }
    }

    return IndexScanChoice{found, std::move(lo), std::move(hi)};
}

Result<std::unique_ptr<Operator>> Planner::build_scans_(const bound::BoundSelect& stmt,
                                                        ColCtx& ctx, bool& where_consumed) {
    ctx.binding_offsets.resize(stmt.bindings.size(), 0u);

    auto [t0, scan0] = make_scan(catalog_, stmt.bindings[0]);
    if (!scan0)
        return Result<std::unique_ptr<Operator>>::err("unknown table: " +
                                                      stmt.bindings[0].table_name);

    if (stmt.bindings.size() == 1) {
        if (stmt.where) {
            auto choice = try_index_scan_(stmt.bindings[0], *stmt.where);
            if (choice) {
                size_t n = stmt.bindings[0].schema->size();
                std::vector<size_t> proj(n);
                std::iota(proj.begin(), proj.end(), 0);
                where_consumed = true;
                return Result<std::unique_ptr<Operator>>::ok(
                    std::make_unique<IndexScan>(t0, choice->index, std::move(proj),
                                                std::move(choice->lo), std::move(choice->hi)));
            }
        }
        return Result<std::unique_ptr<Operator>>::ok(std::move(scan0));
    }

    auto [t1, scan1] = make_scan(catalog_, stmt.bindings[1]);
    if (!scan1)
        return Result<std::unique_ptr<Operator>>::err("unknown table: " +
                                                      stmt.bindings[1].table_name);

    u32 ncols0 = static_cast<u32>(stmt.bindings[0].schema->size());
    u32 ncols1 = static_cast<u32>(stmt.bindings[1].schema->size());

    std::unique_ptr<Operator> op;

    if (t0->row_count() <= t1->row_count()) {
        ctx.binding_offsets[0] = ncols1;
        ctx.binding_offsets[1] = 0;
        auto keys = decompose_on_(*stmt.join_predicates[0], ctx, 0);
        if (!keys.is_ok())
            return Result<std::unique_ptr<Operator>>::err(keys.error());
        auto [pk, bk] = std::move(keys.value());
        op = std::make_unique<HashJoin>(std::move(scan0), std::move(scan1), std::move(bk),
                                        std::move(pk));
    } else {
        ctx.binding_offsets[0] = 0;
        ctx.binding_offsets[1] = ncols0;
        auto keys = decompose_on_(*stmt.join_predicates[0], ctx, 1);
        if (!keys.is_ok())
            return Result<std::unique_ptr<Operator>>::err(keys.error());
        auto [pk, bk] = std::move(keys.value());
        op = std::make_unique<HashJoin>(std::move(scan1), std::move(scan0), std::move(bk),
                                        std::move(pk));
    }

    u32 total_cols = ncols0 + ncols1;
    for (size_t i = 2; i < stmt.bindings.size(); i++) {
        auto r = build_join_(std::move(op), stmt.bindings[i], *stmt.join_predicates[i - 1],
                             static_cast<u32>(i), total_cols, ctx);
        if (!r.is_ok())
            return r;
        total_cols += static_cast<u32>(stmt.bindings[i].schema->size());
        op = std::move(r.value());
    }

    return Result<std::unique_ptr<Operator>>::ok(std::move(op));
}

Result<std::unique_ptr<Operator>> Planner::build_join_(std::unique_ptr<Operator> left,
                                                       const bound::BoundBinding& right_binding,
                                                       const bound::BoundExpr& on, u32 right_bid,
                                                       u32 left_col_count, ColCtx& ctx) {
    Table* t = catalog_.table(right_binding.table_name);
    if (!t)
        return Result<std::unique_ptr<Operator>>::err("unknown table: " + right_binding.table_name);

    size_t ncols = right_binding.schema->size();
    std::vector<size_t> proj(ncols);
    std::iota(proj.begin(), proj.end(), 0);
    auto scan = std::make_unique<TableScan>(t, std::move(proj));

    ctx.binding_offsets[right_bid] = left_col_count;

    auto keys = decompose_on_(on, ctx, right_bid);
    if (!keys.is_ok())
        return Result<std::unique_ptr<Operator>>::err(keys.error());
    auto [pk, bk] = std::move(keys.value());

    return Result<std::unique_ptr<Operator>>::ok(
        std::make_unique<HashJoin>(std::move(scan), std::move(left), std::move(bk), std::move(pk)));
}

Result<Planner::KeyVecs> Planner::decompose_on_(const bound::BoundExpr& on, const ColCtx& ctx,
                                                u32 build_bid) {
    if (auto* lop = std::get_if<bound::BoundLogicalOp>(&on.node)) {
        if (lop->op == LogicalOpKind::AND) {
            auto lr = decompose_on_(*lop->left, ctx, build_bid);
            if (!lr.is_ok())
                return lr;
            auto rr = decompose_on_(*lop->right, ctx, build_bid);
            if (!rr.is_ok())
                return rr;
            auto [lp, lb] = std::move(lr.value());
            auto [rp, rb] = std::move(rr.value());
            for (auto& k : rp)
                lp.push_back(std::move(k));
            for (auto& k : rb)
                lb.push_back(std::move(k));
            return Result<KeyVecs>::ok({std::move(lp), std::move(lb)});
        }
    }

    auto* bop = std::get_if<bound::BoundBinaryOp>(&on.node);
    if (!bop || bop->op != BinaryOpKind::EQ)
        return Result<KeyVecs>::err("only equijoin ON predicates supported");

    auto* lcr = std::get_if<bound::BoundColumnRef>(&bop->left->node);
    auto* rcr = std::get_if<bound::BoundColumnRef>(&bop->right->node);
    if (!lcr || !rcr)
        return Result<KeyVecs>::err("only equijoin ON predicates supported");

    const bound::BoundColumnRef* probe_cr;
    const bound::BoundColumnRef* build_cr;
    if (lcr->ref.binding_id == build_bid && rcr->ref.binding_id != build_bid) {
        build_cr = lcr;
        probe_cr = rcr;
    } else if (rcr->ref.binding_id == build_bid && lcr->ref.binding_id != build_bid) {
        build_cr = rcr;
        probe_cr = lcr;
    } else {
        return Result<KeyVecs>::err("ON predicate is not an equijoin");
    }

    u32 probe_flat = ctx.binding_offsets[probe_cr->ref.binding_id] + probe_cr->ref.column_idx;
    std::vector<std::unique_ptr<Expression>> probe_keys, build_keys;
    probe_keys.push_back(std::make_unique<ColumnRef>(probe_flat, probe_cr->ref.type));
    build_keys.push_back(std::make_unique<ColumnRef>(build_cr->ref.column_idx, build_cr->ref.type));

    return Result<KeyVecs>::ok({std::move(probe_keys), std::move(build_keys)});
}

Result<std::unique_ptr<Operator>> Planner::build_aggregate_(std::unique_ptr<Operator> child,
                                                            const bound::BoundSelect& stmt,
                                                            ColCtx& ctx) {
    std::vector<std::unique_ptr<Expression>> group_keys;
    for (const auto& gb : stmt.group_by)
        group_keys.push_back(lower_expr_(*gb, ctx));

    std::vector<AggregateSpec> agg_specs;
    for (const auto& agg : stmt.aggregates) {
        std::unique_ptr<Expression> arg;
        if (agg.arg)
            arg = lower_expr_(*agg.arg, ctx);
        agg_specs.push_back({agg.kind, std::move(arg)});
    }

    if (agg_specs.empty())
        agg_specs.push_back({AggregateKind::COUNT_STAR, nullptr});

    auto op = std::make_unique<HashAggregate>(std::move(child), std::move(group_keys),
                                              std::move(agg_specs));

    ctx.post_aggregate = true;
    ctx.num_group_keys = static_cast<u32>(stmt.group_by.size());
    for (u32 i = 0; i < static_cast<u32>(stmt.group_by.size()); i++) {
        const auto& cr = std::get<bound::BoundColumnRef>(stmt.group_by[i]->node);
        u64 key = (u64(cr.ref.binding_id) << 32) | cr.ref.column_idx;
        ctx.group_col_map[key] = i;
    }

    return Result<std::unique_ptr<Operator>>::ok(std::move(op));
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
    bool where_consumed = false;

    auto scans = build_scans_(stmt, ctx, where_consumed);
    if (!scans.is_ok())
        return scans;
    auto op = std::move(scans.value());

    if (stmt.where && !where_consumed)
        op = build_filter_(std::move(op), *stmt.where, ctx);

    if (stmt.is_aggregated) {
        auto r = build_aggregate_(std::move(op), stmt, ctx);
        if (!r.is_ok())
            return r;
        op = std::move(r.value());
    }

    if (stmt.having)
        op = build_filter_(std::move(op), *stmt.having, ctx);

    if (!stmt.order_by.empty())
        op = build_sort_(std::move(op), stmt, ctx);

    op = build_project_(std::move(op), stmt, ctx);

    if (stmt.limit.has_value())
        op = build_limit_(std::move(op), stmt);

    return Result<std::unique_ptr<Operator>>::ok(std::move(op));
}

} // namespace nyx
