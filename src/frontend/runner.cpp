#include "frontend/runner.h"

#include "executor/chunk.h"
#include "executor/expression.h"
#include "executor/table_scan.h"

#include <numeric>
#include <type_traits>

namespace nyx::frontend {

Result<void> run_create_table(Catalog& catalog, const bound::BoundCreateTable& stmt) {
    return catalog.add_table(stmt.table_name, stmt.schema);
}

Result<u64> run_insert(Catalog& catalog, const bound::BoundInsert& stmt) {
    return catalog.insert(stmt.table_name, stmt.rows);
}

Result<void> run_drop_table(Catalog& catalog, const bound::BoundDropTable& stmt) {
    return catalog.drop_table(stmt.table_name, stmt.if_exists);
}

static std::unique_ptr<Expression> to_expr(const bound::BoundExpr& e, u32 binding_offset) {
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
                return std::make_unique<ColumnRef>(binding_offset + n.ref.column_idx, n.ref.type);
            } else if constexpr (std::is_same_v<T, bound::BoundAggregateRef> ||
                                 std::is_same_v<T, bound::BoundProjectionRef>) {
                return nullptr;
            } else if constexpr (std::is_same_v<T, bound::BoundBinaryOp>) {
                return std::make_unique<BinaryOp>(n.op, to_expr(*n.left, binding_offset),
                                                  to_expr(*n.right, binding_offset));
            } else if constexpr (std::is_same_v<T, bound::BoundLogicalOp>) {
                return std::make_unique<LogicalOp>(n.op, to_expr(*n.left, binding_offset),
                                                   to_expr(*n.right, binding_offset));
            } else if constexpr (std::is_same_v<T, bound::BoundNotOp>) {
                return std::make_unique<NotOp>(to_expr(*n.child, binding_offset));
            } else {
                static_assert(std::is_same_v<T, bound::BoundNullCheck>);
                return std::make_unique<NullCheckOp>(n.kind, to_expr(*n.child, binding_offset));
            }
        },
        e.node);
}

Result<u64> run_delete(Catalog& catalog, const bound::BoundDelete& stmt) {
    if (!stmt.where)
        return catalog.delete_all(stmt.table_name);

    Table* tbl = catalog.table(stmt.table_name);
    if (!tbl)
        return Result<u64>::err("delete: table not found: " + stmt.table_name);

    std::vector<size_t> all_cols(stmt.schema.size());
    std::iota(all_cols.begin(), all_cols.end(), 0u);

    TableScan scan(tbl, std::move(all_cols));
    auto open_r = scan.open();
    if (!open_r.is_ok())
        return Result<u64>::err(open_r.error().message);

    auto pred_expr = to_expr(*stmt.where, 0);

    std::vector<u64> to_delete;
    u64 chunk_start = 0;

    while (true) {
        auto nr = scan.next();
        if (!nr.is_ok())
            return Result<u64>::err(nr.error().message);
        if (!nr.value())
            break;
        Chunk& chunk = *nr.value();

        auto pred = pred_expr->evaluate(chunk);
        if (!pred.is_ok())
            return Result<u64>::err(pred.error().message);

        for (size_t r = 0; r < chunk.row_count(); ++r) {
            if (!pred.value().is_null(r) && pred.value().get_i32(r) != 0)
                to_delete.push_back(chunk_start + static_cast<u64>(r));
        }

        chunk_start += chunk.row_count();
    }

    return catalog.delete_rows(stmt.table_name, to_delete);
}

} // namespace nyx::frontend
