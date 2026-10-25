#pragma once
#include "js_internal.hpp"

#include <deque>

namespace jetscript {
namespace detail {

enum class Tok : uint8_t {
    Eof, Newline, Indent, Dedent, Error,
    Ident, Int, Float, String, NodePath, Annotation,
    KwIf, KwElif, KwElse, KwFor, KwWhile, KwBreak, KwContinue, KwPass, KwReturn, KwMatch,
    KwVar, KwConst, KwFunc, KwExtends, KwAnd, KwOr, KwNot, KwIn, KwIs, KwSelf, KwSignal,
    KwClass, KwClassName, KwStatic, KwEnum, KwAwait, KwYield, KwPreload, KwBreakpoint, KwSuper,
    KwAs, KwVoid, KwTrue, KwFalse, KwNull,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace, Comma, Colon, Semicolon, Dot, Arrow,
    Plus, Minus, Star, Slash, Percent, StarStar, Tilde, Amp, Pipe, Caret, Shl, Shr,
    Eq, Ne, Lt, Le, Gt, Ge, Bang, AmpAmp, PipePipe,
    Assign, PlusEq, MinusEq, StarEq, SlashEq, PercentEq, StarStarEq, AmpEq, PipeEq, CaretEq, ShlEq, ShrEq,
    DotDot,
};

const char* tok_name(Tok t);

struct Token {
    Tok type = Tok::Eof;
    int line = 1, col = 1;
    Int ival = 0;
    double fval = 0;
    std::string text; // identifier, string value, node path, annotation name or error message
};

// Never fails: problems come back as Error tokens with the message in `text`.
class Lexer {
public:
    explicit Lexer(std::string_view src);
    Token next();

private:
    char peekc(size_t k = 0) const { return pos_ + k < src_.size() ? src_[pos_ + k] : '\0'; }
    int column(size_t pos) const;
    Token make(Tok t, size_t start) const;
    Token error(std::string msg, size_t start) const;
    bool handle_line_start(Token& out);
    Token lex_number(size_t start);
    Token lex_string(size_t start, char quote, Tok type);
    Token lex_node_path(size_t start);
    Token lex_identifier(size_t start);
    void newline_at(size_t pos_after) { ++line_; line_start_ = pos_after; }

    std::string_view src_;
    size_t pos_ = 0;
    int line_ = 1;
    size_t line_start_ = 0;
    std::vector<int> indents_;   // indentation widths of open blocks (0 at bottom)
    char indent_char_ = 0;       // ' ' or '\t', fixed by the first indented line
    std::vector<char> brackets_;
    std::deque<Token> pending_;
    bool at_line_start_ = true;
    bool line_has_tokens_ = false;
    bool eof_ = false;
};

} // namespace detail
} // namespace jetscript
