#pragma once

#include "common/types.h"
#include "executor/expression.h"
#include "executor/hash_aggregate.h"
#include "parser/source_loc.h"
#include "storage/disk/schema.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nyx::bound {

struct BoundExpr;
using BoundExprPtr = std::unique_ptr<BoundExpr>;

struct BindingRef {
    u32 binding_id;
    u32 column_idx;
    TypeId type;
    bool nullable;
};

struct BoundIntLit {
    i64 value;
    TypeId type;
};

struct BoundDoubleLit {
    f64 value;
};

struct BoundNullLit {
    TypeId type;
};

struct BoundColumnRef {
    BindingRef ref;
};

struct BoundAggregateRef {
    u32 aggregate_idx;
    TypeId type;
};

struct BoundBinaryOp {
    BinaryOpKind op;
    BoundExprPtr left;
    BoundExprPtr right;
    TypeId operand_type;
    TypeId result_type;
};

struct BoundLogicalOp {
    LogicalOpKind op;
    BoundExprPtr left;
    BoundExprPtr right;
};

struct BoundNotOp {
    BoundExprPtr child;
};

struct BoundNullCheck {
    NullCheckKind kind;
    BoundExprPtr child;
};

struct BoundExpr {
    std::variant<BoundIntLit, BoundDoubleLit, BoundNullLit, BoundColumnRef, BoundAggregateRef,
                 BoundBinaryOp, BoundLogicalOp, BoundNotOp, BoundNullCheck>
        node;
};

struct BoundBinding {
    std::string table_name;
    std::string alias;
    const Schema* schema;
};

TypeId bound_expr_type(const BoundExpr& e);
bool bound_expr_nullable(const BoundExpr& e);
const char* type_name(TypeId t);

template <typename T> BoundExprPtr make_bound(T&& n) {
    return std::make_unique<BoundExpr>(BoundExpr{std::forward<T>(n)});
}

} // namespace nyx::bound
