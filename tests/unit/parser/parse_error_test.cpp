#include "parser/lexer.h"
#include "parser/parse_error.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

using namespace nyx;

TEST(ParseErrorTest, RenderLine1Col1) {
    std::string src = "SELECT";
    SourceLoc loc{0, 6};
    auto msg = render_parse_error(src, loc, "hello");
    EXPECT_NE(msg.find("line 1, col 1"), std::string::npos);
    EXPECT_NE(msg.find("hello"), std::string::npos);
    EXPECT_NE(msg.find("SELECT"), std::string::npos);
    EXPECT_NE(msg.find("^^^^^^"), std::string::npos);
}

TEST(ParseErrorTest, CaretPositionMidLine) {
    std::string src = "SELECT id";
    SourceLoc loc{7, 2};
    auto msg = render_parse_error(src, loc, "err");
    EXPECT_NE(msg.find("line 1, col 8"), std::string::npos);
    EXPECT_NE(msg.find("       ^^"), std::string::npos);
}

TEST(ParseErrorTest, LineTwoAfterNewline) {
    std::string src = "line1\nline2 here";
    SourceLoc loc{6, 5};
    auto msg = render_parse_error(src, loc, "boom");
    EXPECT_NE(msg.find("line 2, col 1"), std::string::npos);
    EXPECT_NE(msg.find("line2 here"), std::string::npos);
    EXPECT_EQ(msg.find("line1"), std::string::npos);
}

TEST(ParseErrorTest, SnippetContainsOnlyOffendingLine) {
    std::string src = "alpha\nbravo\ngamma\n";
    SourceLoc loc{6, 5};
    auto msg = render_parse_error(src, loc, "x");
    EXPECT_NE(msg.find("line 2"), std::string::npos);
    EXPECT_NE(msg.find("bravo"), std::string::npos);
    EXPECT_EQ(msg.find("alpha"), std::string::npos);
    EXPECT_EQ(msg.find("gamma"), std::string::npos);
}

TEST(ParseErrorTest, ZeroLengthLocRendersSingleCaret) {
    std::string src = "abc";
    SourceLoc loc{3, 0};
    auto msg = render_parse_error(src, loc, "eof");
    EXPECT_NE(msg.find("line 1, col 4"), std::string::npos);
    EXPECT_NE(msg.find("^"), std::string::npos);
    EXPECT_EQ(msg.find("^^"), std::string::npos);
}

TEST(ParseErrorTest, ParserErrorGetsRendered) {
    Lexer lex("SELECT id");
    auto toks = lex.tokenize();
    ASSERT_TRUE(toks.is_ok());
    Parser p("SELECT id", std::move(toks.value()));
    auto r = p.parse_statement();
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("line 1"), std::string::npos);
    EXPECT_NE(r.error().message.find("FROM"), std::string::npos);
}

static Result<std::vector<ast::Statement>> parse_all(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    if (toks.is_err())
        return Result<std::vector<ast::Statement>>::err(toks.error().message);
    Parser p(src, std::move(toks.value()));
    return p.parse();
}

TEST(ParseErrorTest, ParseEmptyInputReturnsEmptyVector) {
    auto r = parse_all("");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().size(), 0u);
}

TEST(ParseErrorTest, ParseSingleStatementNoSemicolon) {
    auto r = parse_all("SELECT id FROM t");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().size(), 1u);
}

TEST(ParseErrorTest, ParseSingleStatementTrailingSemicolon) {
    auto r = parse_all("SELECT id FROM t;");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().size(), 1u);
}

TEST(ParseErrorTest, ParseTwoStatementsSeparatedBySemicolon) {
    auto r = parse_all("SELECT a FROM t; SELECT b FROM u");
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 2u);
    EXPECT_TRUE(std::holds_alternative<ast::SelectStmt>(r.value()[0]));
    EXPECT_TRUE(std::holds_alternative<ast::SelectStmt>(r.value()[1]));
}

TEST(ParseErrorTest, ParseMixedDdlDmlSelect) {
    auto r = parse_all("CREATE TABLE t (a INT); INSERT INTO t VALUES (1); SELECT * FROM t;");
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 3u);
    EXPECT_TRUE(std::holds_alternative<ast::CreateTableStmt>(r.value()[0]));
    EXPECT_TRUE(std::holds_alternative<ast::InsertStmt>(r.value()[1]));
    EXPECT_TRUE(std::holds_alternative<ast::SelectStmt>(r.value()[2]));
}

TEST(ParseErrorTest, ParseErrorMissingSemicolonBetween) {
    auto r = parse_all("SELECT 1 FROM t SELECT 2 FROM u");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find(";"), std::string::npos);
}
