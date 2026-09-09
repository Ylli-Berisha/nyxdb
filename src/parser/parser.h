#pragma once

#include "common/result.h"
#include "common/types.h"
#include "parser/ast.h"
#include "parser/token.h"

#include <string_view>
#include <vector>

namespace nyx {

class Parser {
  public:
    Parser(std::string_view source, std::vector<Token> tokens);

    Result<ast::ExprPtr> parse_expression();
    Result<ast::Statement> parse_statement();

  private:
    const Token& peek_(usize ahead = 0) const;
    const Token& consume_();
    bool match_(TokenKind kind);

    Result<ast::ExprPtr> parse_expr_();
    Result<ast::ExprPtr> parse_binary_(int min_prec);
    Result<ast::ExprPtr> parse_unary_();
    Result<ast::ExprPtr> parse_primary_();
    Result<ast::ExprPtr> parse_ident_or_call_();

    Result<ast::SelectStmt> parse_select_();
    Result<ast::SelectItem> parse_select_item_();
    Result<ast::TableRef> parse_table_ref_();
    Result<ast::JoinClause> parse_join_clause_();
    Result<ast::OrderByItem> parse_order_by_item_();
    Result<i64> parse_int_literal_(const std::string& what);

    std::string err_msg_(const std::string& msg, const Token& tok) const;

    std::string_view source_;
    std::vector<Token> tokens_;
    usize pos_ = 0;
};

} // namespace nyx
