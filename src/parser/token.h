#pragma once

#include "common/types.h"
#include "parser/source_loc.h"

#include <string>

namespace nyx {

enum class TokenKind : u8 {
    // Literals
    INT_LITERAL,
    DOUBLE_LITERAL,
    IDENTIFIER,

    // Punctuation
    LPAREN,
    RPAREN,
    COMMA,
    SEMICOLON,
    DOT,
    STAR,

    // Arithmetic
    PLUS,
    MINUS,
    SLASH,

    // Comparison
    LT,
    LE,
    EQ,
    NE,
    GE,
    GT,

    // Keywords
    KW_SELECT,
    KW_FROM,
    KW_WHERE,
    KW_GROUP,
    KW_BY,
    KW_HAVING,
    KW_ORDER,
    KW_ASC,
    KW_DESC,
    KW_LIMIT,
    KW_OFFSET,
    KW_JOIN,
    KW_INNER,
    KW_ON,
    KW_AS,
    KW_AND,
    KW_OR,
    KW_NOT,
    KW_IS,
    KW_NULL,
    KW_CREATE,
    KW_TABLE,
    KW_INSERT,
    KW_INTO,
    KW_VALUES,
    KW_INT,
    KW_INTEGER,
    KW_BIGINT,
    KW_DOUBLE,

    END_OF_FILE,
};

struct Token {
    TokenKind kind;
    SourceLoc loc;
    std::string text;
};

const char* token_kind_name(TokenKind kind);

} // namespace nyx
