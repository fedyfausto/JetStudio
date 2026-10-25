#include "js_lexer.hpp"

#include <cstdlib>

namespace jetscript {
namespace detail {

const char* tok_name(Tok t) {
    switch (t) {
    case Tok::Eof: return "end of file";
    case Tok::Newline: return "Newline";
    case Tok::Indent: return "Indent";
    case Tok::Dedent: return "Dedent";
    case Tok::Error: return "Error";
    case Tok::Ident: return "Identifier";
    case Tok::Int:
    case Tok::Float:
    case Tok::String: return "Literal";
    case Tok::NodePath: return "$";
    case Tok::Annotation: return "Annotation";
    case Tok::KwIf: return "if";
    case Tok::KwElif: return "elif";
    case Tok::KwElse: return "else";
    case Tok::KwFor: return "for";
    case Tok::KwWhile: return "while";
    case Tok::KwBreak: return "break";
    case Tok::KwContinue: return "continue";
    case Tok::KwPass: return "pass";
    case Tok::KwReturn: return "return";
    case Tok::KwMatch: return "match";
    case Tok::KwVar: return "var";
    case Tok::KwConst: return "const";
    case Tok::KwFunc: return "func";
    case Tok::KwExtends: return "extends";
    case Tok::KwAnd: return "and";
    case Tok::KwOr: return "or";
    case Tok::KwNot: return "not";
    case Tok::KwIn: return "in";
    case Tok::KwIs: return "is";
    case Tok::KwSelf: return "self";
    case Tok::KwSignal: return "signal";
    case Tok::KwClass: return "class";
    case Tok::KwClassName: return "class_name";
    case Tok::KwStatic: return "static";
    case Tok::KwEnum: return "enum";
    case Tok::KwAwait: return "await";
    case Tok::KwYield: return "yield";
    case Tok::KwPreload: return "preload";
    case Tok::KwBreakpoint: return "breakpoint";
    case Tok::KwSuper: return "super";
    case Tok::KwAs: return "as";
    case Tok::KwVoid: return "void";
    case Tok::KwTrue: return "true";
    case Tok::KwFalse: return "false";
    case Tok::KwNull: return "null";
    case Tok::LParen: return "(";
    case Tok::RParen: return ")";
    case Tok::LBracket: return "[";
    case Tok::RBracket: return "]";
    case Tok::LBrace: return "{";
    case Tok::RBrace: return "}";
    case Tok::Comma: return ",";
    case Tok::Colon: return ":";
    case Tok::Semicolon: return ";";
    case Tok::Dot: return ".";
    case Tok::Arrow: return "->";
    case Tok::Plus: return "+";
    case Tok::Minus: return "-";
    case Tok::Star: return "*";
    case Tok::Slash: return "/";
    case Tok::Percent: return "%";
    case Tok::StarStar: return "**";
    case Tok::Tilde: return "~";
    case Tok::Amp: return "&";
    case Tok::Pipe: return "|";
    case Tok::Caret: return "^";
    case Tok::Shl: return "<<";
    case Tok::Shr: return ">>";
    case Tok::Eq: return "==";
    case Tok::Ne: return "!=";
    case Tok::Lt: return "<";
    case Tok::Le: return "<=";
    case Tok::Gt: return ">";
    case Tok::Ge: return ">=";
    case Tok::Bang: return "!";
    case Tok::AmpAmp: return "&&";
    case Tok::PipePipe: return "||";
    case Tok::Assign: return "=";
    case Tok::PlusEq: return "+=";
    case Tok::MinusEq: return "-=";
    case Tok::StarEq: return "*=";
    case Tok::SlashEq: return "/=";
    case Tok::PercentEq: return "%=";
    case Tok::StarStarEq: return "**=";
    case Tok::AmpEq: return "&=";
    case Tok::PipeEq: return "|=";
    case Tok::CaretEq: return "^=";
    case Tok::ShlEq: return "<<=";
    case Tok::ShrEq: return ">>=";
    case Tok::DotDot: return "..";
    }
    return "?";
}

static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Lexer::Lexer(std::string_view src) : src_(src) {
    if (src_.size() >= 3 && static_cast<unsigned char>(src_[0]) == 0xEF && static_cast<unsigned char>(src_[1]) == 0xBB &&
        static_cast<unsigned char>(src_[2]) == 0xBF) {
        pos_ = 3;
        line_start_ = 3;
    }
    indents_.push_back(0);
}

int Lexer::column(size_t pos) const {
    if (pos > src_.size()) pos = src_.size();
    if (pos < line_start_) return 1;
    return int(utf8_count(src_.data() + line_start_, pos - line_start_)) + 1;
}

Token Lexer::make(Tok t, size_t start) const {
    Token k;
    k.type = t;
    k.line = line_;
    k.col = column(start);
    return k;
}

Token Lexer::error(std::string msg, size_t start) const {
    Token k = make(Tok::Error, start);
    k.text = std::move(msg);
    return k;
}

// Called at each logical line start outside brackets. Returns true when it put an Indent, Dedent or Error in `out`.
bool Lexer::handle_line_start(Token& out) {
    size_t p = pos_;
    int spaces = 0, tabs = 0;
    while (p < src_.size() && (src_[p] == ' ' || src_[p] == '\t')) {
        if (src_[p] == ' ') ++spaces;
        else ++tabs;
        ++p;
    }
    char c = p < src_.size() ? src_[p] : '\0';
    bool blank = p >= src_.size() || c == '\n' || c == '#' ||
                 (c == '\r' && (p + 1 >= src_.size() || src_[p + 1] == '\n'));
    pos_ = p;
    at_line_start_ = false;
    if (blank) return false; // blank and comment-only lines don't count for indentation
    int width = spaces + tabs;
    size_t line_begin = p - size_t(width);
    if (spaces && tabs) {
        pending_.push_back(error("Mixed use of tabs and spaces for indentation.", line_begin));
    } else if (width > 0) {
        char ch = tabs ? '\t' : ' ';
        if (!indent_char_) indent_char_ = ch;
        else if (indent_char_ != ch)
            pending_.push_back(error("Mixed use of tabs and spaces for indentation.", line_begin));
    }
    if (width == 0) indent_char_ = 0;
    int cur = indents_.back();
    if (width > cur) {
        indents_.push_back(width);
        pending_.push_back(make(Tok::Indent, p));
    } else if (width < cur) {
        while (indents_.size() > 1 && indents_.back() > width) {
            indents_.pop_back();
            pending_.push_back(make(Tok::Dedent, p));
        }
        if (indents_.back() != width)
            pending_.push_back(error("Unindent doesn't match the previous indentation level.", p));
    }
    if (pending_.empty()) return false;
    out = std::move(pending_.front());
    pending_.pop_front();
    return true;
}

Token Lexer::lex_identifier(size_t start) {
    while (is_ident_char(peekc())) ++pos_;
    std::string_view w = src_.substr(start, pos_ - start);
    static const struct { const char* s; Tok t; } kw[] = {
        {"if", Tok::KwIf},           {"elif", Tok::KwElif},         {"else", Tok::KwElse},
        {"for", Tok::KwFor},         {"while", Tok::KwWhile},       {"break", Tok::KwBreak},
        {"continue", Tok::KwContinue}, {"pass", Tok::KwPass},       {"return", Tok::KwReturn},
        {"match", Tok::KwMatch},     {"var", Tok::KwVar},           {"const", Tok::KwConst},
        {"func", Tok::KwFunc},       {"extends", Tok::KwExtends},   {"and", Tok::KwAnd},
        {"or", Tok::KwOr},           {"not", Tok::KwNot},           {"in", Tok::KwIn},
        {"is", Tok::KwIs},           {"self", Tok::KwSelf},         {"signal", Tok::KwSignal},
        {"class", Tok::KwClass},     {"class_name", Tok::KwClassName}, {"static", Tok::KwStatic},
        {"enum", Tok::KwEnum},       {"await", Tok::KwAwait},       {"yield", Tok::KwYield},
        {"preload", Tok::KwPreload}, {"breakpoint", Tok::KwBreakpoint}, {"super", Tok::KwSuper},
        {"as", Tok::KwAs},           {"void", Tok::KwVoid},         {"true", Tok::KwTrue},
        {"false", Tok::KwFalse},     {"null", Tok::KwNull},
    };
    for (const auto& k : kw)
        if (w == k.s) return make(k.t, start);
    Token t = make(Tok::Ident, start);
    t.text.assign(w.data(), w.size());
    return t;
}

Token Lexer::lex_number(size_t start) {
    std::string digits;
    char c0 = peekc(), c1 = peekc(1);
    if (c0 == '0' && (c1 == 'x' || c1 == 'X' || c1 == 'b' || c1 == 'B')) {
        unsigned base = (c1 == 'x' || c1 == 'X') ? 16 : 2;
        pos_ += 2;
        uint64_t v = 0;
        bool any = false, overflow = false;
        for (;;) {
            char c = peekc();
            if (c == '_') { ++pos_; continue; }
            int d = hex_digit(c);
            if (d < 0 || unsigned(d) >= base) break;
            if (v > (UINT64_MAX >> (base == 16 ? 4 : 1))) overflow = true;
            v = v * base + unsigned(d);
            any = true;
            ++pos_;
        }
        if (is_ident_char(peekc())) {
            while (is_ident_char(peekc())) ++pos_;
            return error("Invalid numeric notation.", start);
        }
        if (!any) return error("Invalid numeric notation.", start);
        if (overflow) return error("Integer literal is too large.", start);
        Token t = make(Tok::Int, start);
        t.ival = Int(v);
        return t;
    }
    bool is_float = false;
    while (is_digit(peekc()) || peekc() == '_') {
        if (peekc() != '_') digits.push_back(peekc());
        ++pos_;
    }
    if (peekc() == '.' && peekc(1) != '.' && !is_ident_start(peekc(1))) {
        is_float = true;
        digits.push_back('.');
        ++pos_;
        while (is_digit(peekc()) || peekc() == '_') {
            if (peekc() != '_') digits.push_back(peekc());
            ++pos_;
        }
    }
    if (peekc() == 'e' || peekc() == 'E') {
        size_t save = pos_;
        std::string exp = "e";
        ++pos_;
        if (peekc() == '+' || peekc() == '-') exp.push_back(src_[pos_++]);
        if (is_digit(peekc())) {
            while (is_digit(peekc()) || peekc() == '_') {
                if (peekc() != '_') exp.push_back(peekc());
                ++pos_;
            }
            digits += exp;
            is_float = true;
        } else {
            pos_ = save;
        }
    }
    if (is_ident_char(peekc())) {
        while (is_ident_char(peekc())) ++pos_;
        return error("Invalid numeric notation.", start);
    }
    if (digits.empty() || digits == ".") return error("Invalid numeric notation.", start);
    if (is_float) {
        if (digits[0] == '.') digits.insert(digits.begin(), '0');
        Token t = make(Tok::Float, start);
        t.fval = std::strtod(digits.c_str(), nullptr);
        return t;
    }
    uint64_t v = 0;
    for (char c : digits) {
        uint64_t d = uint64_t(c - '0');
        if (v > (uint64_t(INT64_MAX) - d) / 10) return error("Integer literal is too large.", start);
        v = v * 10 + d;
    }
    Token t = make(Tok::Int, start);
    t.ival = Int(v);
    return t;
}

Token Lexer::lex_string(size_t start, char quote, Tok type) {
    bool triple = peekc() == quote && peekc(1) == quote && peekc(2) == quote;
    pos_ += triple ? 3 : 1;
    std::string val;
    const char* bad = nullptr;
    int start_line = line_;
    int start_col = column(start);
    for (;;) {
        if (pos_ >= src_.size()) {
            Token e = error("Unterminated string.", start);
            e.line = start_line;
            e.col = start_col;
            return e;
        }
        char c = src_[pos_];
        if (c == quote) {
            if (!triple) {
                ++pos_;
                break;
            }
            if (peekc(1) == quote && peekc(2) == quote) {
                pos_ += 3;
                break;
            }
            val.push_back(c);
            ++pos_;
            continue;
        }
        if (c == '\n') {
            if (!triple) {
                Token e = error("Unterminated string.", start);
                e.line = start_line;
                e.col = start_col;
                return e;
            }
            val.push_back('\n');
            ++pos_;
            newline_at(pos_);
            continue;
        }
        if (c == '\r' && peekc(1) == '\n') {
            ++pos_;
            continue;
        }
        if (c != '\\') {
            val.push_back(c);
            ++pos_;
            continue;
        }
        ++pos_;
        char e = peekc();
        ++pos_;
        switch (e) {
        case 'n': val.push_back('\n'); break;
        case 't': val.push_back('\t'); break;
        case 'r': val.push_back('\r'); break;
        case 'a': val.push_back('\a'); break;
        case 'b': val.push_back('\b'); break;
        case 'f': val.push_back('\f'); break;
        case 'v': val.push_back('\v'); break;
        case '\\': val.push_back('\\'); break;
        case '"': val.push_back('"'); break;
        case '\'': val.push_back('\''); break;
        case '\n': newline_at(pos_); break; // backslash-newline continues the string
        case '\r':
            if (peekc() == '\n') {
                ++pos_;
                newline_at(pos_);
            }
            break;
        case 'u':
        case 'U': {
            int n = e == 'u' ? 4 : 6;
            uint32_t cp = 0;
            bool ok = true;
            for (int i = 0; i < n; ++i) {
                int d = hex_digit(peekc());
                if (d < 0) { ok = false; break; }
                cp = cp * 16 + uint32_t(d);
                ++pos_;
            }
            if (!ok) {
                bad = "Invalid hexadecimal escape in string.";
                break;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && peekc() == '\\' && peekc(1) == 'u') {
                uint32_t lo = 0;
                bool ok2 = true;
                for (int i = 0; i < 4; ++i) {
                    int d = hex_digit(peekc(2 + size_t(i)));
                    if (d < 0) { ok2 = false; break; }
                    lo = lo * 16 + uint32_t(d);
                }
                if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                    pos_ += 6;
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                }
            }
            utf8_append(val, cp);
            break;
        }
        default:
            if (e == '\0' && pos_ > src_.size()) pos_ = src_.size();
            bad = "Invalid escape in string.";
            break;
        }
    }
    if (bad) return error(bad, start);
    Token t = make(type, start);
    t.text = std::move(val);
    t.line = start_line;
    t.col = start_col;
    return t;
}

Token Lexer::lex_node_path(size_t start) {
    ++pos_; // '$'
    char q = peekc();
    if (q == '"' || q == '\'') return lex_string(start, q, Tok::NodePath);
    std::string path;
    if (peekc() == '/') {
        path.push_back('/');
        ++pos_;
    }
    for (;;) {
        if (peekc() == '.' && peekc(1) == '.') {
            path += "..";
            pos_ += 2;
        } else if (peekc() == '.' && peekc(1) == '/') {
            path += ".";
            pos_ += 1;
        } else if (is_ident_char(peekc())) {
            while (is_ident_char(peekc())) path.push_back(src_[pos_++]);
        } else {
            break;
        }
        if (peekc() == '/' && (is_ident_char(peekc(1)) || peekc(1) == '.')) {
            path.push_back('/');
            ++pos_;
            continue;
        }
        break;
    }
    if (path.empty() || path == "/") return error("Expected node path after \"$\".", start);
    Token t = make(Tok::NodePath, start);
    t.text = std::move(path);
    return t;
}

Token Lexer::next() {
    if (!pending_.empty()) {
        Token t = std::move(pending_.front());
        pending_.pop_front();
        return t;
    }
    for (;;) {
        if (at_line_start_ && brackets_.empty()) {
            Token t;
            if (handle_line_start(t)) return t;
        }
        if (pos_ >= src_.size()) {
            if (!eof_) {
                eof_ = true;
                if (!brackets_.empty()) {
                    std::string m = "Unclosed \"";
                    m.push_back(brackets_.back());
                    m += "\" at end of file.";
                    pending_.push_back(error(m, pos_));
                    brackets_.clear();
                }
                if (line_has_tokens_) pending_.push_back(make(Tok::Newline, pos_));
                line_has_tokens_ = false;
                while (indents_.size() > 1) {
                    indents_.pop_back();
                    pending_.push_back(make(Tok::Dedent, pos_));
                }
                pending_.push_back(make(Tok::Eof, pos_));
                Token t = std::move(pending_.front());
                pending_.pop_front();
                return t;
            }
            return make(Tok::Eof, pos_);
        }
        char c = src_[pos_];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++pos_;
            continue;
        }
        if (c == '#') {
            while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            continue;
        }
        if (c == '\\') {
            size_t p = pos_ + 1;
            while (p < src_.size() && (src_[p] == ' ' || src_[p] == '\t' || src_[p] == '\r')) ++p;
            if (p >= src_.size()) {
                pos_ = p;
                continue;
            }
            if (src_[p] == '\n') {
                pos_ = p + 1;
                newline_at(pos_);
                continue;
            }
            size_t start = pos_++;
            line_has_tokens_ = true;
            return error("Expected newline after \"\\\" (line continuation).", start);
        }
        if (c == '\n') {
            size_t start = pos_;
            Token nl = make(Tok::Newline, start);
            ++pos_;
            newline_at(pos_);
            if (!brackets_.empty()) continue;
            at_line_start_ = true;
            if (line_has_tokens_) {
                line_has_tokens_ = false;
                return nl;
            }
            continue;
        }
        line_has_tokens_ = true;
        size_t start = pos_;
        if (is_ident_start(c)) return lex_identifier(start);
        if (is_digit(c) || (c == '.' && is_digit(peekc(1)))) return lex_number(start);
        if (c == '"' || c == '\'') return lex_string(start, c, Tok::String);
        if (c == '$') return lex_node_path(start);
        if (c == '@') {
            ++pos_;
            if (!is_ident_start(peekc())) return error("Expected annotation name after \"@\".", start);
            size_t ns = pos_;
            while (is_ident_char(peekc())) ++pos_;
            Token t = make(Tok::Annotation, start);
            t.text.assign(src_.data() + ns, pos_ - ns);
            return t;
        }
        char c1 = peekc(1), c2 = peekc(2);
        Tok t = Tok::Error;
        size_t len = 1;
        switch (c) {
        case '(': t = Tok::LParen; break;
        case ')': t = Tok::RParen; break;
        case '[': t = Tok::LBracket; break;
        case ']': t = Tok::RBracket; break;
        case '{': t = Tok::LBrace; break;
        case '}': t = Tok::RBrace; break;
        case ',': t = Tok::Comma; break;
        case ':': t = Tok::Colon; break;
        case ';': t = Tok::Semicolon; break;
        case '.':
            if (c1 == '.') { t = Tok::DotDot; len = 2; }
            else t = Tok::Dot;
            break;
        case '~': t = Tok::Tilde; break;
        case '+':
            if (c1 == '=') { t = Tok::PlusEq; len = 2; }
            else t = Tok::Plus;
            break;
        case '-':
            if (c1 == '>') { t = Tok::Arrow; len = 2; }
            else if (c1 == '=') { t = Tok::MinusEq; len = 2; }
            else t = Tok::Minus;
            break;
        case '*':
            if (c1 == '*' && c2 == '=') { t = Tok::StarStarEq; len = 3; }
            else if (c1 == '*') { t = Tok::StarStar; len = 2; }
            else if (c1 == '=') { t = Tok::StarEq; len = 2; }
            else t = Tok::Star;
            break;
        case '/':
            if (c1 == '=') { t = Tok::SlashEq; len = 2; }
            else t = Tok::Slash;
            break;
        case '%':
            if (c1 == '=') { t = Tok::PercentEq; len = 2; }
            else t = Tok::Percent;
            break;
        case '&':
            if (c1 == '&') { t = Tok::AmpAmp; len = 2; }
            else if (c1 == '=') { t = Tok::AmpEq; len = 2; }
            else t = Tok::Amp;
            break;
        case '|':
            if (c1 == '|') { t = Tok::PipePipe; len = 2; }
            else if (c1 == '=') { t = Tok::PipeEq; len = 2; }
            else t = Tok::Pipe;
            break;
        case '^':
            if (c1 == '=') { t = Tok::CaretEq; len = 2; }
            else t = Tok::Caret;
            break;
        case '<':
            if (c1 == '<' && c2 == '=') { t = Tok::ShlEq; len = 3; }
            else if (c1 == '<') { t = Tok::Shl; len = 2; }
            else if (c1 == '=') { t = Tok::Le; len = 2; }
            else t = Tok::Lt;
            break;
        case '>':
            if (c1 == '>' && c2 == '=') { t = Tok::ShrEq; len = 3; }
            else if (c1 == '>') { t = Tok::Shr; len = 2; }
            else if (c1 == '=') { t = Tok::Ge; len = 2; }
            else t = Tok::Gt;
            break;
        case '=':
            if (c1 == '=') { t = Tok::Eq; len = 2; }
            else t = Tok::Assign;
            break;
        case '!':
            if (c1 == '=') { t = Tok::Ne; len = 2; }
            else t = Tok::Bang;
            break;
        default: break;
        }
        if (t == Tok::Error) {
            ++pos_;
            while (pos_ < src_.size() && (static_cast<unsigned char>(src_[pos_]) & 0xC0u) == 0x80u) ++pos_;
            std::string m = "Invalid character \"";
            m.append(src_.data() + start, pos_ - start);
            m += "\".";
            return error(m, start);
        }
        pos_ += len;
        if (t == Tok::LParen || t == Tok::LBracket || t == Tok::LBrace) {
            brackets_.push_back(c);
        } else if (t == Tok::RParen || t == Tok::RBracket || t == Tok::RBrace) {
            char open = t == Tok::RParen ? '(' : (t == Tok::RBracket ? '[' : '{');
            if (brackets_.empty()) {
                std::string m = "Closing \"";
                m.push_back(c);
                m += "\" doesn't have an opening counterpart.";
                return error(m, start);
            }
            char top = brackets_.back();
            brackets_.pop_back();
            if (top != open) {
                std::string m = "Closing \"";
                m.push_back(c);
                m += "\" doesn't match the opening \"";
                m.push_back(top);
                m += "\".";
                return error(m, start);
            }
        }
        return make(t, start);
    }
}

} // namespace detail
} // namespace jetscript
