#pragma once

#include "common/types.h"
#include "parser/source_loc.h"

#include <string>

namespace nyx {

enum class TokenKind : u8 {
    // Literals
    INT_LITERAL,
    DOUBLE_LITERAL,
    STRING_LITERAL,
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
    KW_VARCHAR,
    KW_NVARCHAR,
    KW_DROP,
    KW_IF,
    KW_EXISTS,
    KW_DELETE,
    KW_UPDATE,
    KW_SET,
    KW_BOOL,
    KW_BOOLEAN,
    KW_TRUE,
    KW_FALSE,
    KW_DATE,
    KW_TIMESTAMP,
    KW_INDEX,
    KW_UNIQUE,
    KW_SHOW,
    KW_INDEXES,
    KW_PRIMARY,
    KW_KEY,
    KW_DEFAULT,
    KW_CONSTRAINT,
    KW_CONSTRAINTS,
    KW_VACUUM,
    KW_PARTITION,
    KW_RANGE,
    KW_LESS,
    KW_THAN,
    KW_MAXVALUE,
    KW_ALTER,
    KW_ADD,

    END_OF_FILE,
};

struct Token {
    TokenKind kind;
    SourceLoc loc;
    std::string text;
};

const char* token_kind_name(TokenKind kind);

} // namespace nyx
