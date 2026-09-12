#pragma once

#include "binder/bound_ast.h"
#include "catalog/catalog.h"
#include "common/result.h"
#include "parser/ast.h"

#include <string>
#include <string_view>
#include <vector>

namespace nyx {

class Binder {
  public:
    explicit Binder(Catalog& catalog);

    Result<bound::BoundExprPtr> bind_expression(const ast::Expr& expr,
                                                const std::vector<bound::BoundBinding>& bindings,
                                                std::string_view source);

    Result<bound::BoundSelect> bind_select(const ast::SelectStmt& stmt, std::string_view source);

  private:
    Result<bound::BoundExprPtr> bind_expr_(const ast::Expr& e);
    Result<bound::BoundExprPtr> bind_int_lit_(const ast::IntLit& lit);
    Result<bound::BoundExprPtr> bind_double_lit_(const ast::DoubleLit& lit);
    Result<bound::BoundExprPtr> bind_null_lit_(const ast::NullLit& lit);
    Result<bound::BoundExprPtr> bind_column_ref_(const ast::ColumnRef& ref);
    Result<bound::BoundExprPtr> bind_func_call_(const ast::FuncCall& fc);
    Result<bound::BoundExprPtr> bind_binary_op_(const ast::BinaryOp& bop);
    Result<bound::BoundExprPtr> bind_logical_op_(const ast::LogicalOp& lop);
    Result<bound::BoundExprPtr> bind_not_op_(const ast::NotOp& nop);
    Result<bound::BoundExprPtr> bind_null_check_(const ast::NullCheck& nc);

    Result<bound::BoundBinding> bind_table_ref_(const ast::TableRef& ref);
    Result<std::vector<bound::BoundProjection>> bind_projections_(const ast::SelectStmt& stmt);
    Result<bound::BoundExprPtr> bind_aggregate_(const ast::FuncCall& fc);
    Result<std::vector<bound::BoundOrderBy>> bind_order_by_(
        const ast::SelectStmt& stmt,
        const std::vector<bound::BoundProjection>& projections);
    bool has_ungrouped_col_(const bound::BoundExpr& e,
                            const std::vector<bound::BoundExprPtr>& group_by) const;

    void retype_lit_if_possible_(bound::BoundExpr& e, TypeId target);
    std::string err_msg_(const std::string& msg, SourceLoc loc) const;

    Catalog& catalog_;
    const std::vector<bound::BoundBinding>* bindings_ = nullptr;
    std::vector<bound::BoundAggregate>* aggregates_ = nullptr;
    std::string_view source_;
};

} // namespace nyx
