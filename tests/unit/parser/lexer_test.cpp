#include "parser/lexer.h"

#include <gtest/gtest.h>

using namespace nyx;

static std::vector<Token> lex(const std::string& src) {
    Lexer lex(src);
    auto r = lex.tokenize();
    EXPECT_TRUE(r.is_ok()) << r.error().message;
    return r.value();
}

static std::vector<TokenKind> kinds(const std::vector<Token>& tokens) {
    std::vector<TokenKind> out;
    out.reserve(tokens.size());
    for (const auto& t : tokens)
        out.push_back(t.kind);
    return out;
}

TEST(LexerTest, EmptyInputProducesOnlyEof) {
    auto toks = lex("");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].kind, TokenKind::END_OF_FILE);
}

TEST(LexerTest, WhitespaceOnly) {
    auto toks = lex("   \t\n  \r\n");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].kind, TokenKind::END_OF_FILE);
}

TEST(LexerTest, SingleKeywordSelect) {
    auto toks = lex("SELECT");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::KW_SELECT);
    EXPECT_EQ(toks[0].loc.offset, 0u);
    EXPECT_EQ(toks[0].loc.length, 6u);
}

TEST(LexerTest, KeywordCaseInsensitive) {
    auto toks = lex("select Select SeLeCt");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::KW_SELECT);
    EXPECT_EQ(toks[1].kind, TokenKind::KW_SELECT);
    EXPECT_EQ(toks[2].kind, TokenKind::KW_SELECT);
    EXPECT_EQ(toks[3].kind, TokenKind::END_OF_FILE);
}

TEST(LexerTest, IdentifierFoldsToLowercase) {
    auto toks = lex("MyTable other_col _x1");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::IDENTIFIER);
    EXPECT_EQ(toks[0].text, "mytable");
    EXPECT_EQ(toks[1].kind, TokenKind::IDENTIFIER);
    EXPECT_EQ(toks[1].text, "other_col");
    EXPECT_EQ(toks[2].kind, TokenKind::IDENTIFIER);
    EXPECT_EQ(toks[2].text, "_x1");
}

TEST(LexerTest, IdentifierVsKeyword) {
    auto toks = lex("selected select_col");
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_EQ(toks[0].kind, TokenKind::IDENTIFIER);
    EXPECT_EQ(toks[0].text, "selected");
    EXPECT_EQ(toks[1].kind, TokenKind::IDENTIFIER);
    EXPECT_EQ(toks[1].text, "select_col");
}

TEST(LexerTest, AllKeywords) {
    auto toks = lex("SELECT FROM WHERE GROUP BY HAVING ORDER ASC DESC LIMIT OFFSET "
                    "JOIN INNER ON AS AND OR NOT IS NULL "
                    "CREATE TABLE INSERT INTO VALUES INT INTEGER BIGINT DOUBLE");
    std::vector<TokenKind> expected = {
        TokenKind::KW_SELECT, TokenKind::KW_FROM,     TokenKind::KW_WHERE,   TokenKind::KW_GROUP,
        TokenKind::KW_BY,     TokenKind::KW_HAVING,   TokenKind::KW_ORDER,   TokenKind::KW_ASC,
        TokenKind::KW_DESC,   TokenKind::KW_LIMIT,    TokenKind::KW_OFFSET,  TokenKind::KW_JOIN,
        TokenKind::KW_INNER,  TokenKind::KW_ON,       TokenKind::KW_AS,      TokenKind::KW_AND,
        TokenKind::KW_OR,     TokenKind::KW_NOT,      TokenKind::KW_IS,      TokenKind::KW_NULL,
        TokenKind::KW_CREATE, TokenKind::KW_TABLE,    TokenKind::KW_INSERT,  TokenKind::KW_INTO,
        TokenKind::KW_VALUES, TokenKind::KW_INT,      TokenKind::KW_INTEGER, TokenKind::KW_BIGINT,
        TokenKind::KW_DOUBLE, TokenKind::END_OF_FILE,
    };
    EXPECT_EQ(kinds(toks), expected);
}

TEST(LexerTest, IntLiteral) {
    auto toks = lex("42 0 12345");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::INT_LITERAL);
    EXPECT_EQ(toks[0].text, "42");
    EXPECT_EQ(toks[1].kind, TokenKind::INT_LITERAL);
    EXPECT_EQ(toks[1].text, "0");
    EXPECT_EQ(toks[2].kind, TokenKind::INT_LITERAL);
    EXPECT_EQ(toks[2].text, "12345");
}

TEST(LexerTest, DoubleLiteralFraction) {
    auto toks = lex("3.14 0.5 100.0");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::DOUBLE_LITERAL);
    EXPECT_EQ(toks[0].text, "3.14");
    EXPECT_EQ(toks[1].kind, TokenKind::DOUBLE_LITERAL);
    EXPECT_EQ(toks[1].text, "0.5");
    EXPECT_EQ(toks[2].kind, TokenKind::DOUBLE_LITERAL);
    EXPECT_EQ(toks[2].text, "100.0");
}

TEST(LexerTest, DoubleLiteralExponent) {
    auto toks = lex("1e10 2.5E-3 6e+2");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::DOUBLE_LITERAL);
    EXPECT_EQ(toks[0].text, "1e10");
    EXPECT_EQ(toks[1].kind, TokenKind::DOUBLE_LITERAL);
    EXPECT_EQ(toks[1].text, "2.5E-3");
    EXPECT_EQ(toks[2].kind, TokenKind::DOUBLE_LITERAL);
    EXPECT_EQ(toks[2].text, "6e+2");
}

TEST(LexerTest, TrailingDotNotConsumedAsDouble) {
    auto toks = lex("5.foo");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::INT_LITERAL);
    EXPECT_EQ(toks[0].text, "5");
    EXPECT_EQ(toks[1].kind, TokenKind::DOT);
    EXPECT_EQ(toks[2].kind, TokenKind::IDENTIFIER);
    EXPECT_EQ(toks[2].text, "foo");
}

TEST(LexerTest, Punctuation) {
    auto toks = lex("( ) , ; . *");
    std::vector<TokenKind> expected = {
        TokenKind::LPAREN, TokenKind::RPAREN, TokenKind::COMMA,       TokenKind::SEMICOLON,
        TokenKind::DOT,    TokenKind::STAR,   TokenKind::END_OF_FILE,
    };
    EXPECT_EQ(kinds(toks), expected);
}

TEST(LexerTest, Arithmetic) {
    auto toks = lex("+ - * /");
    std::vector<TokenKind> expected = {
        TokenKind::PLUS,  TokenKind::MINUS,       TokenKind::STAR,
        TokenKind::SLASH, TokenKind::END_OF_FILE,
    };
    EXPECT_EQ(kinds(toks), expected);
}

TEST(LexerTest, ComparisonOperators) {
    auto toks = lex("< <= = <> != >= >");
    std::vector<TokenKind> expected = {
        TokenKind::LT, TokenKind::LE, TokenKind::EQ, TokenKind::NE,
        TokenKind::NE, TokenKind::GE, TokenKind::GT, TokenKind::END_OF_FILE,
    };
    EXPECT_EQ(kinds(toks), expected);
}

TEST(LexerTest, TwoCharOperatorSpans) {
    auto toks = lex("<=");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::LE);
    EXPECT_EQ(toks[0].loc.offset, 0u);
    EXPECT_EQ(toks[0].loc.length, 2u);
}

TEST(LexerTest, LineComment) {
    auto toks = lex("SELECT -- this is a comment\nFROM t");
    std::vector<TokenKind> expected = {
        TokenKind::KW_SELECT,
        TokenKind::KW_FROM,
        TokenKind::IDENTIFIER,
        TokenKind::END_OF_FILE,
    };
    EXPECT_EQ(kinds(toks), expected);
}

TEST(LexerTest, LineCommentAtEofNoNewline) {
    auto toks = lex("SELECT -- trailing comment no newline");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::KW_SELECT);
    EXPECT_EQ(toks[1].kind, TokenKind::END_OF_FILE);
}

TEST(LexerTest, BlockComment) {
    auto toks = lex("SELECT /* a\nb\nc */ FROM t");
    std::vector<TokenKind> expected = {
        TokenKind::KW_SELECT,
        TokenKind::KW_FROM,
        TokenKind::IDENTIFIER,
        TokenKind::END_OF_FILE,
    };
    EXPECT_EQ(kinds(toks), expected);
}

TEST(LexerTest, BlockCommentBetweenTokens) {
    auto toks = lex("a/*x*/b");
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_EQ(toks[0].kind, TokenKind::IDENTIFIER);
    EXPECT_EQ(toks[0].text, "a");
    EXPECT_EQ(toks[1].kind, TokenKind::IDENTIFIER);
    EXPECT_EQ(toks[1].text, "b");
}

TEST(LexerTest, PositionsTrackedAcrossWhitespace) {
    auto toks = lex("  foo   bar");
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_EQ(toks[0].loc.offset, 2u);
    EXPECT_EQ(toks[0].loc.length, 3u);
    EXPECT_EQ(toks[1].loc.offset, 8u);
    EXPECT_EQ(toks[1].loc.length, 3u);
}

TEST(LexerTest, EofLocAtEndOfSource) {
    std::string src = "abc";
    auto toks = lex(src);
    ASSERT_EQ(toks.back().kind, TokenKind::END_OF_FILE);
    EXPECT_EQ(toks.back().loc.offset, src.size());
    EXPECT_EQ(toks.back().loc.length, 0u);
}

TEST(LexerTest, MinusIsMinusNotUnary) {
    auto toks = lex("a-b");
    std::vector<TokenKind> expected = {
        TokenKind::IDENTIFIER,
        TokenKind::MINUS,
        TokenKind::IDENTIFIER,
        TokenKind::END_OF_FILE,
    };
    EXPECT_EQ(kinds(toks), expected);
}

TEST(LexerTest, DottedColumnRef) {
    auto toks = lex("t.col");
    std::vector<TokenKind> expected = {
        TokenKind::IDENTIFIER,
        TokenKind::DOT,
        TokenKind::IDENTIFIER,
        TokenKind::END_OF_FILE,
    };
    EXPECT_EQ(kinds(toks), expected);
}

TEST(LexerTest, SimpleSelectShape) {
    auto toks = lex("SELECT id, name FROM users WHERE id = 5;");
    std::vector<TokenKind> expected = {
        TokenKind::KW_SELECT, TokenKind::IDENTIFIER,  TokenKind::COMMA,     TokenKind::IDENTIFIER,
        TokenKind::KW_FROM,   TokenKind::IDENTIFIER,  TokenKind::KW_WHERE,  TokenKind::IDENTIFIER,
        TokenKind::EQ,        TokenKind::INT_LITERAL, TokenKind::SEMICOLON, TokenKind::END_OF_FILE,
    };
    EXPECT_EQ(kinds(toks), expected);
}

TEST(LexerTest, UnexpectedCharErrors) {
    Lexer l("SELECT @");
    auto r = l.tokenize();
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unexpected character"), std::string::npos);
}

TEST(LexerTest, LoneBangErrors) {
    Lexer l("a ! b");
    auto r = l.tokenize();
    ASSERT_TRUE(r.is_err());
}

TEST(LexerTest, ExponentWithoutDigitsErrors) {
    Lexer l("1e");
    auto r = l.tokenize();
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("exponent"), std::string::npos);
}

TEST(LexerTest, NullIsKeyword) {
    auto toks = lex("null NULL");
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_EQ(toks[0].kind, TokenKind::KW_NULL);
    EXPECT_EQ(toks[1].kind, TokenKind::KW_NULL);
}
