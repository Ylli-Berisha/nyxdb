#include "parser/lexer.h"

#include <cctype>
#include <string>
#include <unordered_map>

namespace nyx {

const char* token_kind_name(TokenKind kind) {
    switch (kind) {
    case TokenKind::INT_LITERAL:
        return "INT_LITERAL";
    case TokenKind::DOUBLE_LITERAL:
        return "DOUBLE_LITERAL";
    case TokenKind::IDENTIFIER:
        return "IDENTIFIER";
    case TokenKind::LPAREN:
        return "(";
    case TokenKind::RPAREN:
        return ")";
    case TokenKind::COMMA:
        return ",";
    case TokenKind::SEMICOLON:
        return ";";
    case TokenKind::DOT:
        return ".";
    case TokenKind::STAR:
        return "*";
    case TokenKind::PLUS:
        return "+";
    case TokenKind::MINUS:
        return "-";
    case TokenKind::SLASH:
        return "/";
    case TokenKind::LT:
        return "<";
    case TokenKind::LE:
        return "<=";
    case TokenKind::EQ:
        return "=";
    case TokenKind::NE:
        return "<>";
    case TokenKind::GE:
        return ">=";
    case TokenKind::GT:
        return ">";
    case TokenKind::KW_SELECT:
        return "SELECT";
    case TokenKind::KW_FROM:
        return "FROM";
    case TokenKind::KW_WHERE:
        return "WHERE";
    case TokenKind::KW_GROUP:
        return "GROUP";
    case TokenKind::KW_BY:
        return "BY";
    case TokenKind::KW_HAVING:
        return "HAVING";
    case TokenKind::KW_ORDER:
        return "ORDER";
    case TokenKind::KW_ASC:
        return "ASC";
    case TokenKind::KW_DESC:
        return "DESC";
    case TokenKind::KW_LIMIT:
        return "LIMIT";
    case TokenKind::KW_OFFSET:
        return "OFFSET";
    case TokenKind::KW_JOIN:
        return "JOIN";
    case TokenKind::KW_INNER:
        return "INNER";
    case TokenKind::KW_ON:
        return "ON";
    case TokenKind::KW_AS:
        return "AS";
    case TokenKind::KW_AND:
        return "AND";
    case TokenKind::KW_OR:
        return "OR";
    case TokenKind::KW_NOT:
        return "NOT";
    case TokenKind::KW_IS:
        return "IS";
    case TokenKind::KW_NULL:
        return "NULL";
    case TokenKind::KW_CREATE:
        return "CREATE";
    case TokenKind::KW_TABLE:
        return "TABLE";
    case TokenKind::KW_INSERT:
        return "INSERT";
    case TokenKind::KW_INTO:
        return "INTO";
    case TokenKind::KW_VALUES:
        return "VALUES";
    case TokenKind::KW_INT:
        return "INT";
    case TokenKind::KW_INTEGER:
        return "INTEGER";
    case TokenKind::KW_BIGINT:
        return "BIGINT";
    case TokenKind::KW_DOUBLE:
        return "DOUBLE";
    case TokenKind::END_OF_FILE:
        return "EOF";
    }
    return "?";
}

static const std::unordered_map<std::string, TokenKind>& keyword_table() {
    static const std::unordered_map<std::string, TokenKind> table = {
        {"select", TokenKind::KW_SELECT},   {"from", TokenKind::KW_FROM},
        {"where", TokenKind::KW_WHERE},     {"group", TokenKind::KW_GROUP},
        {"by", TokenKind::KW_BY},           {"having", TokenKind::KW_HAVING},
        {"order", TokenKind::KW_ORDER},     {"asc", TokenKind::KW_ASC},
        {"desc", TokenKind::KW_DESC},       {"limit", TokenKind::KW_LIMIT},
        {"offset", TokenKind::KW_OFFSET},   {"join", TokenKind::KW_JOIN},
        {"inner", TokenKind::KW_INNER},     {"on", TokenKind::KW_ON},
        {"as", TokenKind::KW_AS},           {"and", TokenKind::KW_AND},
        {"or", TokenKind::KW_OR},           {"not", TokenKind::KW_NOT},
        {"is", TokenKind::KW_IS},           {"null", TokenKind::KW_NULL},
        {"create", TokenKind::KW_CREATE},   {"table", TokenKind::KW_TABLE},
        {"insert", TokenKind::KW_INSERT},   {"into", TokenKind::KW_INTO},
        {"values", TokenKind::KW_VALUES},   {"int", TokenKind::KW_INT},
        {"integer", TokenKind::KW_INTEGER}, {"bigint", TokenKind::KW_BIGINT},
        {"double", TokenKind::KW_DOUBLE},
    };
    return table;
}

Lexer::Lexer(std::string_view source) : source_(source) {}

char Lexer::peek_(usize ahead) const {
    usize p = pos_ + ahead;
    if (p >= source_.size())
        return '\0';
    return source_[p];
}

bool Lexer::at_end_() const {
    return pos_ >= source_.size();
}

void Lexer::advance_() {
    if (pos_ < source_.size())
        ++pos_;
}

void Lexer::skip_trivia_() {
    while (!at_end_()) {
        char c = source_[pos_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            advance_();
        } else if (c == '-' && peek_(1) == '-') {
            while (!at_end_() && source_[pos_] != '\n')
                advance_();
        } else if (c == '/' && peek_(1) == '*') {
            advance_();
            advance_();
            while (!at_end_() && !(source_[pos_] == '*' && peek_(1) == '/'))
                advance_();
            if (!at_end_()) {
                advance_();
                advance_();
            }
        } else {
            break;
        }
    }
}

Result<Token> Lexer::error_(const std::string& message, u32 start) const {
    return Result<Token>::err("lex error at offset " + std::to_string(start) + ": " + message);
}

Result<Token> Lexer::lex_ident_or_keyword_() {
    u32 start = static_cast<u32>(pos_);
    std::string text;
    while (!at_end_()) {
        char c = source_[pos_];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            advance_();
        } else {
            break;
        }
    }
    u32 length = static_cast<u32>(pos_) - start;

    const auto& table = keyword_table();
    auto it = table.find(text);
    Token tok;
    tok.loc = SourceLoc{start, length};
    if (it != table.end()) {
        tok.kind = it->second;
        tok.text = std::string();
    } else {
        tok.kind = TokenKind::IDENTIFIER;
        tok.text = std::move(text);
    }
    return Result<Token>::ok(std::move(tok));
}

Result<Token> Lexer::lex_number_() {
    u32 start = static_cast<u32>(pos_);
    bool is_double = false;

    while (!at_end_() && std::isdigit(static_cast<unsigned char>(source_[pos_])))
        advance_();

    if (!at_end_() && source_[pos_] == '.' && std::isdigit(static_cast<unsigned char>(peek_(1)))) {
        is_double = true;
        advance_();
        while (!at_end_() && std::isdigit(static_cast<unsigned char>(source_[pos_])))
            advance_();
    }

    if (!at_end_() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
        is_double = true;
        advance_();
        if (!at_end_() && (source_[pos_] == '+' || source_[pos_] == '-'))
            advance_();
        if (at_end_() || !std::isdigit(static_cast<unsigned char>(source_[pos_])))
            return error_("expected digit after exponent", start);
        while (!at_end_() && std::isdigit(static_cast<unsigned char>(source_[pos_])))
            advance_();
    }

    u32 length = static_cast<u32>(pos_) - start;
    Token tok;
    tok.kind = is_double ? TokenKind::DOUBLE_LITERAL : TokenKind::INT_LITERAL;
    tok.loc = SourceLoc{start, length};
    tok.text = std::string(source_.substr(start, length));
    return Result<Token>::ok(std::move(tok));
}

Result<Token> Lexer::lex_symbol_() {
    u32 start = static_cast<u32>(pos_);
    char c = source_[pos_];
    Token tok;
    tok.loc = SourceLoc{start, 1};

    auto one = [&](TokenKind k) {
        advance_();
        tok.kind = k;
        tok.loc.length = 1;
        return Result<Token>::ok(std::move(tok));
    };
    auto two = [&](TokenKind k) {
        advance_();
        advance_();
        tok.kind = k;
        tok.loc.length = 2;
        return Result<Token>::ok(std::move(tok));
    };

    switch (c) {
    case '(':
        return one(TokenKind::LPAREN);
    case ')':
        return one(TokenKind::RPAREN);
    case ',':
        return one(TokenKind::COMMA);
    case ';':
        return one(TokenKind::SEMICOLON);
    case '.':
        return one(TokenKind::DOT);
    case '*':
        return one(TokenKind::STAR);
    case '+':
        return one(TokenKind::PLUS);
    case '-':
        return one(TokenKind::MINUS);
    case '/':
        return one(TokenKind::SLASH);
    case '=':
        return one(TokenKind::EQ);
    case '<':
        if (peek_(1) == '=')
            return two(TokenKind::LE);
        if (peek_(1) == '>')
            return two(TokenKind::NE);
        return one(TokenKind::LT);
    case '>':
        if (peek_(1) == '=')
            return two(TokenKind::GE);
        return one(TokenKind::GT);
    case '!':
        if (peek_(1) == '=')
            return two(TokenKind::NE);
        return error_(std::string("unexpected character '") + c + "'", start);
    default:
        return error_(std::string("unexpected character '") + c + "'", start);
    }
}

Result<Token> Lexer::next_token_() {
    char c = source_[pos_];
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        return lex_ident_or_keyword_();
    if (std::isdigit(static_cast<unsigned char>(c)))
        return lex_number_();
    return lex_symbol_();
}

Result<std::vector<Token>> Lexer::tokenize() {
    std::vector<Token> out;
    while (true) {
        skip_trivia_();
        if (at_end_())
            break;
        auto res = next_token_();
        if (res.is_err())
            return Result<std::vector<Token>>::err(res.error().message);
        out.push_back(std::move(res.value()));
    }
    Token eof;
    eof.kind = TokenKind::END_OF_FILE;
    eof.loc = SourceLoc{static_cast<u32>(source_.size()), 0};
    out.push_back(std::move(eof));
    return Result<std::vector<Token>>::ok(std::move(out));
}

} // namespace nyx
