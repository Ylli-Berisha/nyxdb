#pragma once

#include "common/types.h"
#include "executor/expression.h"
#include "parser/source_loc.h"
#include "storage/disk/value.h"

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

struct StringLit {
    std::string value;
    SourceLoc loc;
};

struct NullLit {
    SourceLoc loc;
};

struct BoolLit {
    bool value;
    SourceLoc loc;
};

struct DateLit {
    std::string value;
    SourceLoc loc;
};

struct TimestampLit {
    std::string value;
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
    std::variant<IntLit, DoubleLit, StringLit, NullLit, BoolLit, DateLit, TimestampLit, ColumnRef,
                 FuncCall, BinaryOp, LogicalOp, NotOp, NullCheck>
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

struct JoinClause {
    TableRef right;
    ExprPtr on;
};

struct OrderByItem {
    ExprPtr expr;
    bool ascending = true;
};

struct SelectStmt {
    std::vector<SelectItem> projections;
    bool star_projection = false;
    TableRef from;
    std::vector<JoinClause> joins;
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    ExprPtr having;
    std::vector<OrderByItem> order_by;
    std::optional<i64> limit;
    std::optional<i64> offset;
};

struct ColumnDef {
    std::string name;
    TypeId type;
    bool nullable = true;
    u16 max_len = 255;
    bool is_primary_key = false;
    bool is_unique = false;
    std::optional<Value> default_value;
};

struct TableConstraint {
    enum Kind { PRIMARY_KEY, UNIQUE } kind;
    std::string name;
    std::vector<std::string> columns;
};

struct CreateTableStmt {
    std::string table_name;
    std::vector<ColumnDef> columns;
    std::vector<TableConstraint> constraints;
};

struct InsertStmt {
    std::string table_name;
    std::vector<std::string> columns;
    std::vector<std::vector<ExprPtr>> rows;
};

struct DropTableStmt {
    std::string table_name;
    bool if_exists = false;
};

struct DeleteStmt {
    std::string table_name;
    ExprPtr where;
};

struct Assignment {
    std::string column;
    ExprPtr value;
};

struct UpdateStmt {
    std::string table_name;
    std::vector<Assignment> assignments;
    ExprPtr where;
};

struct CreateIndexStmt {
    std::string index_name;
    std::string table_name;
    std::vector<std::string> column_names;
    bool unique = false;
};

struct DropIndexStmt {
    std::string index_name;
    std::string table_name;
};

struct ShowIndexesStmt {
    std::string table_name;
};

struct ShowConstraintsStmt {
    std::string table_name;
};

using Statement =
    std::variant<SelectStmt, CreateTableStmt, InsertStmt, DropTableStmt, DeleteStmt, UpdateStmt,
                 CreateIndexStmt, DropIndexStmt, ShowIndexesStmt, ShowConstraintsStmt>;

} // namespace nyx::ast
