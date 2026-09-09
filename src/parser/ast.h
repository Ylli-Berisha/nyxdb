#pragma once

#include "common/types.h"
#include "executor/expression.h"
#include "parser/source_loc.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nyx::ast {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct IntLit {
    i64 value;
    SourceLoc loc;
};

struct DoubleLit {
    f64 value;
    SourceLoc loc;
};

struct NullLit {
    SourceLoc loc;
};

struct ColumnRef {
    std::optional<std::string> table;
    std::string column;
    SourceLoc loc;
};

struct FuncCall {
    std::string name;
    std::vector<ExprPtr> args;
    bool star;
    SourceLoc loc;
};

struct BinaryOp {
    BinaryOpKind op;
    ExprPtr left;
    ExprPtr right;
    SourceLoc loc;
};

struct LogicalOp {
    LogicalOpKind op;
    ExprPtr left;
    ExprPtr right;
    SourceLoc loc;
};

struct NotOp {
    ExprPtr child;
    SourceLoc loc;
};

struct NullCheck {
    NullCheckKind kind;
    ExprPtr child;
    SourceLoc loc;
};

struct Expr {
    std::variant<IntLit, DoubleLit, NullLit, ColumnRef, FuncCall, BinaryOp, LogicalOp, NotOp,
                 NullCheck>
        node;
};

inline SourceLoc expr_loc(const Expr& e) {
    return std::visit([](const auto& n) -> SourceLoc { return n.loc; }, e.node);
}

template <typename T> ExprPtr make_expr(T&& node) {
    return std::make_unique<Expr>(Expr{std::forward<T>(node)});
}

struct SelectItem {
    ExprPtr expr;
    std::optional<std::string> alias;
};

struct TableRef {
    std::string table_name;
    std::optional<std::string> alias;
    SourceLoc loc;
};

struct SelectStmt {
    std::vector<SelectItem> projections;
    bool star_projection = false;
    TableRef from;
    ExprPtr where;
};

using Statement = std::variant<SelectStmt>;

} // namespace nyx::ast
