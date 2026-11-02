#pragma once
#include "js_internal.hpp"

#include <deque>

namespace jetscript{
    namespace detail{

        enum class Tok : uint8_t{
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

        const char* tokName(Tok t);

        struct Token{
            Tok type = Tok::Eof;
            int line = 1, col = 1;
            Int ival = 0;
            double fval = 0;
            // identifier, string value, node path, annotation name or error message
            std::string text;
        };

        // Never fails: problems come back as Error tokens with the message in `text`.
        class Lexer{
            public:
                explicit Lexer(std::string_view src);
                Token next();

            private:
                char peekc(size_t k = 0) const{ return pos_ + k < src_.size() ? src_[pos_ + k] : '\0'; }
                int column(size_t pos) const;
                Token make(Tok t, size_t start) const;
                Token error(std::string msg, size_t start) const;
                bool handleLineStart(Token& out);
                Token lexNumber(size_t start);
                Token lexString(size_t start, char quote, Tok type);
                Token lexNodePath(size_t start);
                Token lexIdentifier(size_t start);
                void newlineAt(size_t posAfter){ ++line_; lineStart_ = posAfter; }

                std::string_view src_;
                size_t pos_ = 0;
                int line_ = 1;
                size_t lineStart_ = 0;
                // column() counts on from the last token, so a very long line is not scanned again for each one
                mutable size_t colLine_ = 0, colPos_ = 0;
                mutable int colCount_ = 0;
                // indentation widths of open blocks (0 at bottom)
                std::vector<int> indents_;
                // ' ' or '\t', fixed by the first indented line
                char indentChar_ = 0;
                std::vector<char> brackets_;
                std::deque<Token> pending_;
                bool atLineStart_ = true;
                bool lineHasTokens_ = false;
                bool eof_ = false;
        };

    }
}
