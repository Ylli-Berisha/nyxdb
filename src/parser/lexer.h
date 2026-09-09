#pragma once

#include "common/result.h"
#include "common/types.h"
#include "parser/token.h"

#include <string_view>
#include <vector>

namespace nyx {

class Lexer {
  public:
    explicit Lexer(std::string_view source);

    Result<std::vector<Token>> tokenize();

  private:
    void skip_trivia_();
    Result<Token> next_token_();
    Result<Token> lex_ident_or_keyword_();
    Result<Token> lex_number_();
    Result<Token> lex_symbol_();

    char peek_(usize ahead = 0) const;
    bool at_end_() const;
    void advance_();

    Result<Token> error_(const std::string& message, u32 start) const;

    std::string_view source_;
    usize pos_ = 0;
};

} // namespace nyx
