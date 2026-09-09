#include "parser/parser.h"

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

Result<ast::ExprPtr> Parser::err_(const std::string& msg, const Token& tok) const {
    return Result<ast::ExprPtr>::err("parse error at offset " + std::to_string(tok.loc.offset) +
                                     ": " + msg);
}

Result<ast::ExprPtr> Parser::parse_expression() {
    auto r = parse_expr_();
    if (r.is_err())
        return r;
    if (peek_().kind != TokenKind::END_OF_FILE)
        return err_("unexpected token after expression", peek_());
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
                return err_("expected NULL after IS", peek_());
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
            return err_("expected ')'", peek_());
        return r;
    }
    case TokenKind::IDENTIFIER:
        return parse_ident_or_call_();
    default:
        return err_("expected expression", tok);
    }
}

Result<ast::ExprPtr> Parser::parse_ident_or_call_() {
    Token ident = consume_();

    if (peek_().kind == TokenKind::DOT) {
        consume_();
        if (peek_().kind != TokenKind::IDENTIFIER)
            return err_("expected identifier after '.'", peek_());
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
            return err_("expected ')'", peek_());
        Token rparen = consume_();
        SourceLoc combined = combine_loc(ident.loc, rparen.loc);
        return Result<ast::ExprPtr>::ok(
            ast::make_expr(ast::FuncCall{ident.text, std::move(args), star, combined}));
    }

    return Result<ast::ExprPtr>::ok(
        ast::make_expr(ast::ColumnRef{std::nullopt, ident.text, ident.loc}));
}

} // namespace nyx
