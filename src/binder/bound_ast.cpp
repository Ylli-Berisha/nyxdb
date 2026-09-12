#include "binder/bound_ast.h"

#include <type_traits>

namespace nyx::bound {

const char* type_name(TypeId t) {
    switch (t) {
    case TypeId::INT32:
        return "INT32";
    case TypeId::INT64:
        return "INT64";
    case TypeId::DOUBLE:
        return "DOUBLE";
    default:
        return "?";
    }
}

TypeId bound_expr_type(const BoundExpr& e) {
    return std::visit(
        [](const auto& n) -> TypeId {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, BoundIntLit>)
                return n.type;
            else if constexpr (std::is_same_v<T, BoundDoubleLit>)
                return TypeId::DOUBLE;
            else if constexpr (std::is_same_v<T, BoundNullLit>)
                return n.type;
            else if constexpr (std::is_same_v<T, BoundColumnRef>)
                return n.ref.type;
            else if constexpr (std::is_same_v<T, BoundAggregateRef>)
                return n.type;
            else if constexpr (std::is_same_v<T, BoundProjectionRef>)
                return n.type;
            else if constexpr (std::is_same_v<T, BoundBinaryOp>)
                return n.result_type;
            else if constexpr (std::is_same_v<T, BoundLogicalOp>)
                return TypeId::INT32;
            else if constexpr (std::is_same_v<T, BoundNotOp>)
                return TypeId::INT32;
            else if constexpr (std::is_same_v<T, BoundNullCheck>)
                return TypeId::INT32;
        },
        e.node);
}

bool bound_expr_nullable(const BoundExpr& e) {
    return std::visit(
        [](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, BoundIntLit>)
                return false;
            else if constexpr (std::is_same_v<T, BoundDoubleLit>)
                return false;
            else if constexpr (std::is_same_v<T, BoundNullLit>)
                return true;
            else if constexpr (std::is_same_v<T, BoundColumnRef>)
                return n.ref.nullable;
            else if constexpr (std::is_same_v<T, BoundAggregateRef>)
                return true;
            else if constexpr (std::is_same_v<T, BoundProjectionRef>)
                return false;
            else if constexpr (std::is_same_v<T, BoundBinaryOp>)
                return bound_expr_nullable(*n.left) || bound_expr_nullable(*n.right);
            else if constexpr (std::is_same_v<T, BoundLogicalOp>)
                return bound_expr_nullable(*n.left) || bound_expr_nullable(*n.right);
            else if constexpr (std::is_same_v<T, BoundNotOp>)
                return bound_expr_nullable(*n.child);
            else if constexpr (std::is_same_v<T, BoundNullCheck>)
                return false;
        },
        e.node);
}

} // namespace nyx::bound
