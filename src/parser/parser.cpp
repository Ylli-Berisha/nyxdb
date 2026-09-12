#include "parser/parser.h"

#include "common/source_error.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace nyx {

namespace {

struct OpInfo {
    int prec;
    bool is_logical;
    BinaryOpKind binop;
    LogicalOpKind logop;
};

std::optional<OpInfo> lookup_op(TokenKind k) {
    switch (k) {
    case TokenKind::KW_OR:
        return OpInfo{1, true, BinaryOpKind::ADD, LogicalOpKind::OR};
    case TokenKind::KW_AND:
        return OpInfo{2, true, BinaryOpKind::ADD, LogicalOpKind::AND};
    case TokenKind::LT:
        return OpInfo{4, false, BinaryOpKind::LT, LogicalOpKind::AND};
    case TokenKind::LE:
        return OpInfo{4, false, BinaryOpKind::LE, LogicalOpKind::AND};
    case TokenKind::EQ:
        return OpInfo{4, false, BinaryOpKind::EQ, LogicalOpKind::AND};
    case TokenKind::NE:
        return OpInfo{4, false, BinaryOpKind::NE, LogicalOpKind::AND};
    case TokenKind::GE:
        return OpInfo{4, false, BinaryOpKind::GE, LogicalOpKind::AND};
    case TokenKind::GT:
        return OpInfo{4, false, BinaryOpKind::GT, LogicalOpKind::AND};
    case TokenKind::PLUS:
        return OpInfo{5, false, BinaryOpKind::ADD, LogicalOpKind::AND};
    case TokenKind::MINUS:
        return OpInfo{5, false, BinaryOpKind::SUB, LogicalOpKind::AND};
    case TokenKind::STAR:
        return OpInfo{6, false, BinaryOpKind::MUL, LogicalOpKind::AND};
    case TokenKind::SLASH:
        return OpInfo{6, false, BinaryOpKind::DIV, LogicalOpKind::AND};
    default:
        return std::nullopt;
    }
}

SourceLoc combine_loc(SourceLoc a, SourceLoc b) {
    u32 start = a.offset < b.offset ? a.offset : b.offset;
    u32 end_a = a.offset + a.length;
    u32 end_b = b.offset + b.length;
    u32 end = end_a > end_b ? end_a : end_b;
    return SourceLoc{start, end - start};
}

} // namespace

Parser::Parser(std::string_view source, std::vector<Token> tokens)
    : source_(source), tokens_(std::move(tokens)) {}

const Token& Parser::peek_(usize ahead) const {
    usize idx = pos_ + ahead;
    if (idx >= tokens_.size())
        return tokens_.back();
    return tokens_[idx];
}

const Token& Parser::consume_() {
    const Token& t = peek_();
    if (pos_ + 1 < tokens_.size())
        ++pos_;
    return t;
}

bool Parser::match_(TokenKind kind) {
    if (peek_().kind != kind)
        return false;
    consume_();
    return true;
}

std::string Parser::err_msg_(const std::string& msg, const Token& tok) const {
    return render_source_error(source_, tok.loc, "parse error", msg);
}

Result<ast::ExprPtr> Parser::parse_expression() {
    auto r = parse_expr_();
    if (r.is_err())
        return r;
    if (peek_().kind != TokenKind::END_OF_FILE)
        return Result<ast::ExprPtr>::err(err_msg_("unexpected token after expression", peek_()));
    return r;
}

Result<ast::ExprPtr> Parser::parse_expr_() {
    return parse_binary_(1);
}

Result<ast::ExprPtr> Parser::parse_binary_(int min_prec) {
    ast::ExprPtr left;
    if (min_prec <= 3 && peek_().kind == TokenKind::KW_NOT) {
        const Token& not_tok = consume_();
        auto r = parse_binary_(3);
        if (r.is_err())
            return r;
        SourceLoc combined = combine_loc(not_tok.loc, ast::expr_loc(*r.value()));
        left = ast::make_expr(ast::NotOp{std::move(r.value()), combined});
    } else {
        auto lhs_r = parse_unary_();
        if (lhs_r.is_err())
            return lhs_r;
        left = std::move(lhs_r.value());
    }

    while (true) {
        const Token& tok = peek_();

        if (tok.kind == TokenKind::KW_IS && 4 >= min_prec) {
            consume_();
            bool is_not = match_(TokenKind::KW_NOT);
            if (peek_().kind != TokenKind::KW_NULL)
                return Result<ast::ExprPtr>::err(err_msg_("expected NULL after IS", peek_()));
            const Token& null_tok = consume_();
            SourceLoc combined = combine_loc(ast::expr_loc(*left), null_tok.loc);
            NullCheckKind k = is_not ? NullCheckKind::IS_NOT_NULL : NullCheckKind::IS_NULL;
            left = ast::make_expr(ast::NullCheck{k, std::move(left), combined});
            continue;
        }

        auto info = lookup_op(tok.kind);
        if (!info || info->prec < min_prec)
            break;
        consume_();
        auto rhs_r = parse_binary_(info->prec + 1);
        if (rhs_r.is_err())
            return rhs_r;
        ast::ExprPtr right = std::move(rhs_r.value());
        SourceLoc combined = combine_loc(ast::expr_loc(*left), ast::expr_loc(*right));
        if (info->is_logical) {
            left = ast::make_expr(
                ast::LogicalOp{info->logop, std::move(left), std::move(right), combined});
        } else {
            left = ast::make_expr(
                ast::BinaryOp{info->binop, std::move(left), std::move(right), combined});
        }
    }
    return Result<ast::ExprPtr>::ok(std::move(left));
}

Result<ast::ExprPtr> Parser::parse_unary_() {
    const Token& tok = peek_();
    if (tok.kind == TokenKind::MINUS) {
        consume_();
        auto r = parse_unary_();
        if (r.is_err())
            return r;
        ast::ExprPtr child = std::move(r.value());
        SourceLoc combined = combine_loc(tok.loc, ast::expr_loc(*child));
        if (std::holds_alternative<ast::IntLit>(child->node)) {
            i64 v = std::get<ast::IntLit>(child->node).value;
            return Result<ast::ExprPtr>::ok(ast::make_expr(ast::IntLit{-v, combined}));
        }
        if (std::holds_alternative<ast::DoubleLit>(child->node)) {
            f64 v = std::get<ast::DoubleLit>(child->node).value;
            return Result<ast::ExprPtr>::ok(ast::make_expr(ast::DoubleLit{-v, combined}));
        }
        ast::ExprPtr zero = ast::make_expr(ast::IntLit{0, tok.loc});
        return Result<ast::ExprPtr>::ok(ast::make_expr(
            ast::BinaryOp{BinaryOpKind::SUB, std::move(zero), std::move(child), combined}));
    }
    if (tok.kind == TokenKind::PLUS) {
        consume_();
        return parse_unary_();
    }
    return parse_primary_();
}

Result<ast::ExprPtr> Parser::parse_primary_() {
    const Token& tok = peek_();
    switch (tok.kind) {
    case TokenKind::INT_LITERAL: {
        consume_();
        i64 v = std::strtoll(tok.text.c_str(), nullptr, 10);
        return Result<ast::ExprPtr>::ok(ast::make_expr(ast::IntLit{v, tok.loc}));
    }
    case TokenKind::DOUBLE_LITERAL: {
        consume_();
        f64 v = std::strtod(tok.text.c_str(), nullptr);
        return Result<ast::ExprPtr>::ok(ast::make_expr(ast::DoubleLit{v, tok.loc}));
    }
    case TokenKind::KW_NULL: {
        consume_();
        return Result<ast::ExprPtr>::ok(ast::make_expr(ast::NullLit{tok.loc}));
    }
    case TokenKind::LPAREN: {
        consume_();
        auto r = parse_expr_();
        if (r.is_err())
            return r;
        if (!match_(TokenKind::RPAREN))
            return Result<ast::ExprPtr>::err(err_msg_("expected ')'", peek_()));
        return r;
    }
    case TokenKind::IDENTIFIER:
        return parse_ident_or_call_();
    default:
        return Result<ast::ExprPtr>::err(err_msg_("expected expression", tok));
    }
}

Result<ast::ExprPtr> Parser::parse_ident_or_call_() {
    Token ident = consume_();

    if (peek_().kind == TokenKind::DOT) {
        consume_();
        if (peek_().kind != TokenKind::IDENTIFIER)
            return Result<ast::ExprPtr>::err(err_msg_("expected identifier after '.'", peek_()));
        Token col = consume_();
        SourceLoc combined = combine_loc(ident.loc, col.loc);
        return Result<ast::ExprPtr>::ok(ast::make_expr(
            ast::ColumnRef{std::optional<std::string>{ident.text}, col.text, combined}));
    }

    if (peek_().kind == TokenKind::LPAREN) {
        consume_();
        std::vector<ast::ExprPtr> args;
        bool star = false;
        if (peek_().kind == TokenKind::STAR) {
            consume_();
            star = true;
        } else if (peek_().kind != TokenKind::RPAREN) {
            while (true) {
                auto a = parse_expr_();
                if (a.is_err())
                    return a;
                args.push_back(std::move(a.value()));
                if (!match_(TokenKind::COMMA))
                    break;
            }
        }
        if (peek_().kind != TokenKind::RPAREN)
            return Result<ast::ExprPtr>::err(err_msg_("expected ')'", peek_()));
        Token rparen = consume_();
        SourceLoc combined = combine_loc(ident.loc, rparen.loc);
        return Result<ast::ExprPtr>::ok(
            ast::make_expr(ast::FuncCall{ident.text, std::move(args), star, combined}));
    }

    return Result<ast::ExprPtr>::ok(
        ast::make_expr(ast::ColumnRef{std::nullopt, ident.text, ident.loc}));
}

Result<ast::Statement> Parser::parse_one_statement_() {
    switch (peek_().kind) {
    case TokenKind::KW_SELECT: {
        auto r = parse_select_();
        if (r.is_err())
            return Result<ast::Statement>::err(r.error().message);
        return Result<ast::Statement>::ok(ast::Statement{std::move(r.value())});
    }
    case TokenKind::KW_CREATE: {
        auto r = parse_create_table_();
        if (r.is_err())
            return Result<ast::Statement>::err(r.error().message);
        return Result<ast::Statement>::ok(ast::Statement{std::move(r.value())});
    }
    case TokenKind::KW_INSERT: {
        auto r = parse_insert_();
        if (r.is_err())
            return Result<ast::Statement>::err(r.error().message);
        return Result<ast::Statement>::ok(ast::Statement{std::move(r.value())});
    }
    default:
        return Result<ast::Statement>::err(err_msg_("expected statement", peek_()));
    }
}

Result<ast::Statement> Parser::parse_statement() {
    auto r = parse_one_statement_();
    if (r.is_err())
        return r;
    match_(TokenKind::SEMICOLON);
    if (peek_().kind != TokenKind::END_OF_FILE)
        return Result<ast::Statement>::err(err_msg_("unexpected token after statement", peek_()));
    return r;
}

Result<std::vector<ast::Statement>> Parser::parse() {
    std::vector<ast::Statement> stmts;
    while (peek_().kind != TokenKind::END_OF_FILE) {
        auto r = parse_one_statement_();
        if (r.is_err())
            return Result<std::vector<ast::Statement>>::err(r.error().message);
        stmts.push_back(std::move(r.value()));
        if (peek_().kind == TokenKind::END_OF_FILE)
            break;
        if (!match_(TokenKind::SEMICOLON))
            return Result<std::vector<ast::Statement>>::err(
                err_msg_("expected ';' between statements", peek_()));
    }
    return Result<std::vector<ast::Statement>>::ok(std::move(stmts));
}

Result<ast::SelectStmt> Parser::parse_select_() {
    if (!match_(TokenKind::KW_SELECT))
        return Result<ast::SelectStmt>::err(err_msg_("expected SELECT", peek_()));

    ast::SelectStmt stmt;

    if (peek_().kind == TokenKind::STAR) {
        consume_();
        stmt.star_projection = true;
    } else {
        while (true) {
            auto item = parse_select_item_();
            if (item.is_err())
                return Result<ast::SelectStmt>::err(item.error().message);
            stmt.projections.push_back(std::move(item.value()));
            if (!match_(TokenKind::COMMA))
                break;
        }
    }

    if (!match_(TokenKind::KW_FROM))
        return Result<ast::SelectStmt>::err(err_msg_("expected FROM", peek_()));

    auto table = parse_table_ref_();
    if (table.is_err())
        return Result<ast::SelectStmt>::err(table.error().message);
    stmt.from = std::move(table.value());

    while (peek_().kind == TokenKind::KW_INNER || peek_().kind == TokenKind::KW_JOIN) {
        auto j = parse_join_clause_();
        if (j.is_err())
            return Result<ast::SelectStmt>::err(j.error().message);
        stmt.joins.push_back(std::move(j.value()));
    }

    if (match_(TokenKind::KW_WHERE)) {
        auto expr = parse_expr_();
        if (expr.is_err())
            return Result<ast::SelectStmt>::err(expr.error().message);
        stmt.where = std::move(expr.value());
    }

    if (match_(TokenKind::KW_GROUP)) {
        if (!match_(TokenKind::KW_BY))
            return Result<ast::SelectStmt>::err(err_msg_("expected BY after GROUP", peek_()));
        while (true) {
            auto e = parse_expr_();
            if (e.is_err())
                return Result<ast::SelectStmt>::err(e.error().message);
            stmt.group_by.push_back(std::move(e.value()));
            if (!match_(TokenKind::COMMA))
                break;
        }
    }

    if (match_(TokenKind::KW_HAVING)) {
        auto e = parse_expr_();
        if (e.is_err())
            return Result<ast::SelectStmt>::err(e.error().message);
        stmt.having = std::move(e.value());
    }

    if (match_(TokenKind::KW_ORDER)) {
        if (!match_(TokenKind::KW_BY))
            return Result<ast::SelectStmt>::err(err_msg_("expected BY after ORDER", peek_()));
        while (true) {
            auto item = parse_order_by_item_();
            if (item.is_err())
                return Result<ast::SelectStmt>::err(item.error().message);
            stmt.order_by.push_back(std::move(item.value()));
            if (!match_(TokenKind::COMMA))
                break;
        }
    }

    if (match_(TokenKind::KW_LIMIT)) {
        auto n = parse_int_literal_("LIMIT");
        if (n.is_err())
            return Result<ast::SelectStmt>::err(n.error().message);
        stmt.limit = n.value();
    }

    if (match_(TokenKind::KW_OFFSET)) {
        auto n = parse_int_literal_("OFFSET");
        if (n.is_err())
            return Result<ast::SelectStmt>::err(n.error().message);
        stmt.offset = n.value();
    }

    return Result<ast::SelectStmt>::ok(std::move(stmt));
}

Result<ast::JoinClause> Parser::parse_join_clause_() {
    match_(TokenKind::KW_INNER);
    if (!match_(TokenKind::KW_JOIN))
        return Result<ast::JoinClause>::err(err_msg_("expected JOIN", peek_()));
    auto right = parse_table_ref_();
    if (right.is_err())
        return Result<ast::JoinClause>::err(right.error().message);
    if (!match_(TokenKind::KW_ON))
        return Result<ast::JoinClause>::err(err_msg_("expected ON after join table", peek_()));
    auto on = parse_expr_();
    if (on.is_err())
        return Result<ast::JoinClause>::err(on.error().message);
    ast::JoinClause j;
    j.right = std::move(right.value());
    j.on = std::move(on.value());
    return Result<ast::JoinClause>::ok(std::move(j));
}

Result<ast::OrderByItem> Parser::parse_order_by_item_() {
    auto e = parse_expr_();
    if (e.is_err())
        return Result<ast::OrderByItem>::err(e.error().message);
    ast::OrderByItem item;
    item.expr = std::move(e.value());
    item.ascending = true;
    if (match_(TokenKind::KW_DESC))
        item.ascending = false;
    else
        match_(TokenKind::KW_ASC);
    return Result<ast::OrderByItem>::ok(std::move(item));
}

Result<i64> Parser::parse_int_literal_(const std::string& what) {
    if (peek_().kind != TokenKind::INT_LITERAL)
        return Result<i64>::err(err_msg_("expected integer after " + what, peek_()));
    const Token& tok = consume_();
    return Result<i64>::ok(std::strtoll(tok.text.c_str(), nullptr, 10));
}

Result<ast::SelectItem> Parser::parse_select_item_() {
    auto e = parse_expr_();
    if (e.is_err())
        return Result<ast::SelectItem>::err(e.error().message);

    ast::SelectItem item;
    item.expr = std::move(e.value());

    if (match_(TokenKind::KW_AS)) {
        if (peek_().kind != TokenKind::IDENTIFIER)
            return Result<ast::SelectItem>::err(err_msg_("expected alias after AS", peek_()));
        item.alias = consume_().text;
    } else if (peek_().kind == TokenKind::IDENTIFIER) {
        item.alias = consume_().text;
    }
    return Result<ast::SelectItem>::ok(std::move(item));
}

Result<ast::TableRef> Parser::parse_table_ref_() {
    if (peek_().kind != TokenKind::IDENTIFIER)
        return Result<ast::TableRef>::err(err_msg_("expected table name", peek_()));
    const Token& name_tok = consume_();

    ast::TableRef ref;
    ref.table_name = name_tok.text;
    ref.loc = name_tok.loc;

    if (match_(TokenKind::KW_AS)) {
        if (peek_().kind != TokenKind::IDENTIFIER)
            return Result<ast::TableRef>::err(err_msg_("expected alias after AS", peek_()));
        const Token& alias_tok = consume_();
        ref.alias = alias_tok.text;
        ref.loc = combine_loc(ref.loc, alias_tok.loc);
    } else if (peek_().kind == TokenKind::IDENTIFIER) {
        const Token& alias_tok = consume_();
        ref.alias = alias_tok.text;
        ref.loc = combine_loc(ref.loc, alias_tok.loc);
    }
    return Result<ast::TableRef>::ok(std::move(ref));
}

Result<ast::CreateTableStmt> Parser::parse_create_table_() {
    if (!match_(TokenKind::KW_CREATE))
        return Result<ast::CreateTableStmt>::err(err_msg_("expected CREATE", peek_()));
    if (!match_(TokenKind::KW_TABLE))
        return Result<ast::CreateTableStmt>::err(err_msg_("expected TABLE", peek_()));
    if (peek_().kind != TokenKind::IDENTIFIER)
        return Result<ast::CreateTableStmt>::err(err_msg_("expected table name", peek_()));
    const Token& name_tok = consume_();

    ast::CreateTableStmt stmt;
    stmt.table_name = name_tok.text;

    if (!match_(TokenKind::LPAREN))
        return Result<ast::CreateTableStmt>::err(err_msg_("expected '('", peek_()));
    while (true) {
        auto col = parse_column_def_();
        if (col.is_err())
            return Result<ast::CreateTableStmt>::err(col.error().message);
        stmt.columns.push_back(std::move(col.value()));
        if (!match_(TokenKind::COMMA))
            break;
    }
    if (!match_(TokenKind::RPAREN))
        return Result<ast::CreateTableStmt>::err(err_msg_("expected ')'", peek_()));
    return Result<ast::CreateTableStmt>::ok(std::move(stmt));
}

Result<ast::ColumnDef> Parser::parse_column_def_() {
    if (peek_().kind != TokenKind::IDENTIFIER)
        return Result<ast::ColumnDef>::err(err_msg_("expected column name", peek_()));
    const Token& name_tok = consume_();

    ast::ColumnDef def;
    def.name = name_tok.text;
    def.nullable = true;

    switch (peek_().kind) {
    case TokenKind::KW_INT:
    case TokenKind::KW_INTEGER:
        def.type = TypeId::INT32;
        consume_();
        break;
    case TokenKind::KW_BIGINT:
        def.type = TypeId::INT64;
        consume_();
        break;
    case TokenKind::KW_DOUBLE:
        def.type = TypeId::DOUBLE;
        consume_();
        break;
    default:
        return Result<ast::ColumnDef>::err(err_msg_("expected column type", peek_()));
    }

    if (match_(TokenKind::KW_NOT)) {
        if (!match_(TokenKind::KW_NULL))
            return Result<ast::ColumnDef>::err(err_msg_("expected NULL after NOT", peek_()));
        def.nullable = false;
    }
    return Result<ast::ColumnDef>::ok(std::move(def));
}

Result<ast::InsertStmt> Parser::parse_insert_() {
    if (!match_(TokenKind::KW_INSERT))
        return Result<ast::InsertStmt>::err(err_msg_("expected INSERT", peek_()));
    if (!match_(TokenKind::KW_INTO))
        return Result<ast::InsertStmt>::err(err_msg_("expected INTO", peek_()));
    if (peek_().kind != TokenKind::IDENTIFIER)
        return Result<ast::InsertStmt>::err(err_msg_("expected table name", peek_()));
    const Token& name_tok = consume_();

    ast::InsertStmt stmt;
    stmt.table_name = name_tok.text;

    if (match_(TokenKind::LPAREN)) {
        while (true) {
            if (peek_().kind != TokenKind::IDENTIFIER)
                return Result<ast::InsertStmt>::err(err_msg_("expected column name", peek_()));
            stmt.columns.push_back(consume_().text);
            if (!match_(TokenKind::COMMA))
                break;
        }
        if (!match_(TokenKind::RPAREN))
            return Result<ast::InsertStmt>::err(err_msg_("expected ')'", peek_()));
    }

    if (!match_(TokenKind::KW_VALUES))
        return Result<ast::InsertStmt>::err(err_msg_("expected VALUES", peek_()));

    while (true) {
        if (!match_(TokenKind::LPAREN))
            return Result<ast::InsertStmt>::err(err_msg_("expected '('", peek_()));
        std::vector<ast::ExprPtr> row;
        while (true) {
            auto e = parse_expr_();
            if (e.is_err())
                return Result<ast::InsertStmt>::err(e.error().message);
            row.push_back(std::move(e.value()));
            if (!match_(TokenKind::COMMA))
                break;
        }
        if (!match_(TokenKind::RPAREN))
            return Result<ast::InsertStmt>::err(err_msg_("expected ')'", peek_()));
        stmt.rows.push_back(std::move(row));
        if (!match_(TokenKind::COMMA))
            break;
    }
    return Result<ast::InsertStmt>::ok(std::move(stmt));
}

} // namespace nyx
