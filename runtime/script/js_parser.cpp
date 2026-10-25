#include "js_parser.hpp"

namespace jetscript {
namespace detail {

namespace {
enum Prec { P_NONE = 0, P_OR = 1, P_AND, P_NOT, P_IN, P_IS, P_CMP, P_BOR, P_BXOR, P_BAND, P_SHIFT, P_ADD, P_MUL, P_POW };
}

uint32_t Ast::intern(std::string_view s) {
    auto it = name_map.find(std::string(s));
    if (it != name_map.end()) return it->second;
    uint32_t id = uint32_t(names.size());
    names.emplace_back(s);
    name_map.emplace(names.back(), id);
    return id;
}

Parser::Parser(std::string_view src, Ast& ast, std::vector<Diagnostic>& errors)
    : lex_(src), ast_(ast), errors_(errors) {}

const Token& Parser::peek(size_t k) {
    while (la_.size() <= k) {
        Token t = lex_.next();
        if (t.type == Tok::Error) {
            error_at(t.line, t.col, t.text);
            continue;
        }
        la_.push_back(std::move(t));
    }
    return la_[k];
}

void Parser::skip() {
    peek();
    la_.pop_front();
}

Token Parser::advance() {
    peek();
    Token t = std::move(la_.front());
    la_.pop_front();
    return t;
}

bool Parser::match(Tok t) {
    if (!check(t)) return false;
    skip();
    return true;
}

bool Parser::expect(Tok t, const char* message) {
    if (match(t)) return true;
    error_here(message);
    return false;
}

std::string Parser::describe(const Token& t) const {
    switch (t.type) {
    case Tok::Ident: return "Identifier \"" + t.text + "\"";
    case Tok::Int:
    case Tok::Float:
    case Tok::String: return "Literal";
    case Tok::Newline: return "Newline";
    case Tok::Indent: return "Indent";
    case Tok::Dedent: return "Dedent";
    case Tok::Eof: return "end of file";
    case Tok::Annotation: return "Annotation \"@" + t.text + "\"";
    case Tok::NodePath: return "\"$\"";
    default: return std::string("\"") + tok_name(t.type) + "\"";
    }
}

void Parser::error_at(int line, int col, const std::string& msg) {
    if (abort_) return;
    if (!errors_.empty() && errors_.back().line == line) return; // one error per line, no cascades
    if (int(errors_.size()) >= kMaxErrors) {
        abort_ = true;
        return;
    }
    Diagnostic d;
    d.line = line;
    d.column = col;
    d.message = msg;
    errors_.push_back(std::move(d));
}

void Parser::error_at(int line, int col, const char* msg) { error_at(line, col, std::string(msg)); }

void Parser::error_here(const std::string& msg) {
    const Token& t = peek();
    error_at(t.line, t.col, msg);
}

void Parser::error_here(const char* msg) { error_here(std::string(msg)); }

void Parser::depth_error() {
    if (abort_) return;
    const Token& t = peek();
    error_at(t.line, t.col, "Maximum nesting depth exceeded (" + std::to_string(kMaxNesting) +
                                " levels of blocks and expressions).");
    abort_ = true;
}

void Parser::sync_line() {
    for (;;) {
        Tok t = peek().type;
        if (t == Tok::Eof || t == Tok::Dedent || t == Tok::Indent) return;
        skip();
        if (t == Tok::Newline) return;
    }
}

bool Parser::end_statement() {
    if (match(Tok::Semicolon)) {
        last_terminator_ = Tok::Semicolon;
        if (match(Tok::Newline)) last_terminator_ = Tok::Newline;
        return true;
    }
    last_terminator_ = Tok::Newline;
    if (match(Tok::Newline)) return true;
    if (check(Tok::Eof) || check(Tok::Dedent)) return true;
    end_statement_error();
    return false;
}

void Parser::end_statement_error() {
    error_here("Expected end of statement, found " + describe(peek()) + " instead.");
    sync_line();
}

void Parser::skip_block() {
    if (!match(Tok::Indent)) return;
    int level = 1;
    while (level > 0) {
        Tok t = peek().type;
        skip();
        if (t == Tok::Eof) return;
        if (t == Tok::Indent) ++level;
        else if (t == Tok::Dedent) --level;
    }
}

Node* Parser::new_node(NK kind, int line, int col) {
    ast_.nodes.emplace_back();
    Node* n = &ast_.nodes.back();
    n->kind = kind;
    n->line = line;
    n->col = col;
    return n;
}

void Parser::finish(Node* n) {
    int d = 0;
    if (n->a && n->a->depth > d) d = n->a->depth;
    if (n->b && n->b->depth > d) d = n->b->depth;
    if (n->c && n->c->depth > d) d = n->c->depth;
    for (Node* x : n->list)
        if (x && x->depth > d) d = x->depth;
    ++d;
    if (d > 255) d = 255;
    n->depth = uint8_t(d);
    if (d + block_depth_ > kMaxNesting) complexity_error(n);
}

void Parser::complexity_error(Node* n) {
    if (abort_) return;
    error_at(n->line, n->col, "Expression is too complex (maximum nesting depth is " + std::to_string(kMaxNesting) +
                                  "); split it into several statements.");
    abort_ = true;
}

Stmt* Parser::new_stmt(SK kind, int line, int col) {
    ast_.stmts.emplace_back();
    Stmt* s = &ast_.stmts.back();
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

void Parser::parse_script() {
    while (!abort_ && !check(Tok::Eof)) parse_class_item();
}

void Parser::unsupported_statement(Tok type, int line, int col) {
    const char* msg = "This construct is not supported yet in JetScript.";
    switch (type) {
    case Tok::KwSignal: msg = "Signals (\"signal\") are not supported yet in JetScript."; break;
    case Tok::KwClass: msg = "Inner classes (\"class\") are not supported yet in JetScript."; break;
    case Tok::KwClassName: msg = "\"class_name\" is not supported yet in JetScript."; break;
    case Tok::KwStatic: msg = "Static functions and variables (\"static\") are not supported yet in JetScript."; break;
    case Tok::KwEnum: msg = "Enums can only be declared at the top level of the script."; break;
    case Tok::KwFunc: msg = "Lambdas and nested functions are not supported yet in JetScript."; break;
    default: break;
    }
    error_at(line, col, msg);
    sync_line();
    if (check(Tok::Indent)) skip_block();
}

void Parser::parse_annotation(std::vector<Token>& annotations, std::vector<std::vector<Node*>>& args) {
    Token t = advance();
    std::vector<Node*> a;
    if (match(Tok::LParen)) {
        while (!abort_ && !check(Tok::RParen) && !check(Tok::Eof) && !check(Tok::Newline)) {
            a.push_back(parse_expr());
            if (!match(Tok::Comma)) break;
        }
        expect(Tok::RParen, "Expected \")\" after annotation arguments.");
    }
    if (t.text != "export" && t.text != "export_range" && t.text != "onready" && t.text != "warning_ignore") {
        error_at(t.line, t.col, "Annotation \"@" + t.text + "\" is not supported yet in JetScript.");
    } else if (t.text == "export_range" && (a.size() < 2 || a.size() > 4)) {
        error_at(t.line, t.col, "Annotation \"@export_range\" expects (min, max) or (min, max, step).");
    } else if ((t.text == "export" || t.text == "onready") && !a.empty()) {
        error_at(t.line, t.col, "Annotation \"@" + t.text + "\" does not take arguments.");
    }
    annotations.push_back(std::move(t));
    args.push_back(std::move(a));
}

void Parser::parse_annotated() {
    std::vector<Token> ann;
    std::vector<std::vector<Node*>> args;
    while (!abort_ && check(Tok::Annotation)) {
        parse_annotation(ann, args);
        while (match(Tok::Newline)) {}
    }
    if (check(Tok::KwVar)) {
        parse_member_var(ann, args);
        return;
    }
    bool only_ignore = true;
    for (const Token& a : ann)
        if (a.text != "warning_ignore") only_ignore = false;
    if (!only_ignore && !ann.empty())
        error_here("Annotation \"@" + ann.front().text + "\" must be followed by a variable declaration.");
}

void Parser::parse_extends() {
    Token kw = advance();
    if (ast_.has_extends) error_at(kw.line, kw.col, "\"extends\" can only be used once.");
    else if (seen_items_) error_at(kw.line, kw.col, "\"extends\" must be the first statement of the script.");
    if (check(Tok::Ident)) {
        Token n = advance();
        if (!ast_.has_extends) {
            ast_.has_extends = true;
            ast_.extends_name = ast_.intern(n.text);
            ast_.extends_line = n.line;
            ast_.extends_col = n.col;
        }
        if (check(Tok::Dot)) error_here("Extending inner classes is not supported yet in JetScript.");
    } else if (check(Tok::String)) {
        error_here("Extending scripts by path is not supported yet in JetScript.");
        skip();
    } else {
        error_here("Expected class name after \"extends\".");
    }
    seen_items_ = true;
    end_statement();
}

void Parser::unexpected_class_item() {
    Token t = peek();
    error_at(t.line, t.col, "Unexpected " + describe(t) + " in class body.");
    if (t.type == Tok::Eof) return;
    skip();
    sync_line();
    if (check(Tok::Indent)) skip_block();
}

void Parser::parse_class_item() {
    const Token& t = peek();
    switch (t.type) {
    case Tok::Newline:
    case Tok::Semicolon:
    case Tok::Dedent: skip(); return;
    case Tok::Indent:
        error_here("Unexpected indentation.");
        skip_block();
        return;
    case Tok::Annotation: parse_annotated(); return;
    case Tok::KwExtends: parse_extends(); return;
    case Tok::KwVar: {
        std::vector<Token> ann;
        std::vector<std::vector<Node*>> args;
        parse_member_var(ann, args);
        return;
    }
    case Tok::KwConst: parse_const_decl(); return;
    case Tok::KwEnum: parse_enum(); return;
    case Tok::KwFunc: parse_func(); return;
    case Tok::KwStatic:
    case Tok::KwSignal:
    case Tok::KwClass:
    case Tok::KwClassName: {
        Tok type = t.type;
        int line = t.line, col = t.col;
        skip();
        unsupported_statement(type, line, col);
        return;
    }
    case Tok::KwPass:
        skip();
        end_statement();
        return;
    default: unexpected_class_item(); return;
    }
}

bool Parser::parse_type(TypeRef& type, bool allow_void) {
    const Token& t = peek();
    type.line = t.line;
    type.col = t.col;
    if (t.type == Tok::KwVoid) {
        if (!allow_void) error_here("\"void\" is only allowed as a function return type.");
        skip();
        type.present = true;
        type.name = ast_.intern("void");
        return true;
    }
    if (t.type != Tok::Ident) {
        error_here("Expected type after \":\" or \"->\".");
        return false;
    }
    type.present = true;
    type.name = ast_.intern(t.text);
    skip();
    if (check(Tok::LBracket)) {
        error_here("Typed arrays are not supported yet in JetScript.");
        int depth = 0;
        while (!check(Tok::Eof) && !check(Tok::Newline)) {
            Tok k = peek().type;
            skip();
            if (k == Tok::LBracket) ++depth;
            else if (k == Tok::RBracket && --depth <= 0) break;
        }
    }
    if (check(Tok::Dot)) {
        error_here("Nested types are not supported yet in JetScript.");
        skip();
        if (check(Tok::Ident)) skip();
    }
    return true;
}

void Parser::parse_member_var(std::vector<Token>& annotations, std::vector<std::vector<Node*>>& args) {
    skip(); // var
    seen_items_ = true;
    VarDecl d;
    if (!check(Tok::Ident)) {
        error_here("Expected variable name after \"var\".");
        sync_line();
        return;
    }
    Token n = advance();
    d.name = ast_.intern(n.text);
    d.line = n.line;
    d.col = n.col;
    if (match(Tok::Colon)) {
        if (match(Tok::Assign)) {
            d.infer = true;
            d.init = parse_expr();
        } else {
            parse_type(d.type, false);
        }
    }
    if (!d.init && match(Tok::Assign)) d.init = parse_expr();
    if (check(Tok::Colon)) {
        error_here("Property setters and getters (set/get) are not supported yet in JetScript.");
        skip();
        sync_line();
        if (check(Tok::Indent)) skip_block();
        return;
    }
    for (size_t i = 0; i < annotations.size(); ++i) {
        const std::string& a = annotations[i].text;
        if (a == "export") {
            d.exported = true;
        } else if (a == "export_range") {
            d.exported = true;
            d.has_range = true;
            d.range_args = args[i];
        } else if (a == "onready") {
            d.onready = true;
        }
    }
    ast_.order.push_back({ItemKind::Var, uint32_t(ast_.vars.size())});
    ast_.vars.push_back(std::move(d));
    end_statement();
}

void Parser::parse_const_decl() {
    skip(); // const
    seen_items_ = true;
    ConstDecl d;
    if (!check(Tok::Ident)) {
        error_here("Expected constant name after \"const\".");
        sync_line();
        return;
    }
    Token n = advance();
    d.name = ast_.intern(n.text);
    d.line = n.line;
    d.col = n.col;
    bool infer = false;
    if (match(Tok::Colon)) {
        if (match(Tok::Assign)) infer = true;
        else parse_type(d.type, false);
    }
    if (infer || match(Tok::Assign)) {
        d.init = parse_expr();
    } else {
        error_here("Expected initializer after constant name.");
        sync_line();
        return;
    }
    ast_.order.push_back({ItemKind::Const, uint32_t(ast_.consts.size())});
    ast_.consts.push_back(std::move(d));
    end_statement();
}

void Parser::parse_enum() {
    Token kw = advance();
    seen_items_ = true;
    EnumDecl e;
    e.line = kw.line;
    e.col = kw.col;
    if (check(Tok::Ident)) {
        Token n = advance();
        e.name = ast_.intern(n.text);
        e.line = n.line;
        e.col = n.col;
    }
    if (!expect(Tok::LBrace, "Expected \"{\" after \"enum\".")) {
        sync_line();
        return;
    }
    while (!abort_ && !check(Tok::RBrace) && !check(Tok::Eof)) {
        if (!check(Tok::Ident)) {
            error_here("Expected identifier for enum key.");
            break;
        }
        Token k = advance();
        Node* value = nullptr;
        if (match(Tok::Assign)) value = parse_expr();
        e.values.emplace_back(ast_.intern(k.text), value);
        e.positions.emplace_back(k.line, k.col);
        if (!match(Tok::Comma)) break;
    }
    if (!expect(Tok::RBrace, "Expected closing \"}\" for enum.")) {
        while (!check(Tok::Eof) && !check(Tok::RBrace) && !check(Tok::Newline)) skip();
        match(Tok::RBrace);
    }
    ast_.order.push_back({ItemKind::Enum, uint32_t(ast_.enums.size())});
    ast_.enums.push_back(std::move(e));
    end_statement();
}

void Parser::parse_func() {
    Token kw = advance();
    seen_items_ = true;
    FuncDecl f;
    f.line = kw.line;
    f.col = kw.col;
    if (!check(Tok::Ident)) {
        error_here("Expected function name after \"func\".");
        sync_line();
        if (check(Tok::Indent)) skip_block();
        return;
    }
    Token n = advance();
    f.name = ast_.intern(n.text);
    f.line = n.line;
    f.col = n.col;
    if (!expect(Tok::LParen, "Expected \"(\" after function name.")) {
        sync_line();
        if (check(Tok::Indent)) skip_block();
        return;
    }
    while (!abort_ && !check(Tok::RParen) && !check(Tok::Eof)) {
        if (check(Tok::DotDot) || check(Tok::Dot)) {
            error_here("Variadic functions are not supported yet in JetScript.");
            break;
        }
        if (!check(Tok::Ident)) {
            error_here("Expected parameter name.");
            break;
        }
        Token pn = advance();
        Param p;
        p.name = ast_.intern(pn.text);
        p.line = pn.line;
        p.col = pn.col;
        if (match(Tok::Colon)) {
            if (match(Tok::Assign)) {
                p.infer = true;
                p.def = parse_expr();
            } else {
                parse_type(p.type, false);
            }
        }
        if (!p.def && match(Tok::Assign)) p.def = parse_expr();
        f.params.push_back(p);
        if (!match(Tok::Comma)) break;
    }
    if (!expect(Tok::RParen, "Expected closing \")\" after function parameters.")) {
        while (!check(Tok::Eof) && !check(Tok::Newline) && !check(Tok::Colon)) skip();
        match(Tok::RParen);
    }
    if (match(Tok::Arrow)) parse_type(f.ret, true);
    if (!expect(Tok::Colon, "Expected \":\" after function declaration.")) {
        sync_line();
        if (check(Tok::Indent)) skip_block();
        return;
    }
    loop_depth_ = 0;
    parse_block(f.body, "function declaration");
    ast_.order.push_back({ItemKind::Func, uint32_t(ast_.funcs.size())});
    ast_.funcs.push_back(std::move(f));
}

void Parser::parse_block(std::vector<Stmt*>& out, const char* what) {
    DepthGuard g(*this);
    if (!g.ok) return;
    ++block_depth_;
    if (match(Tok::Newline)) {
        if (!match(Tok::Indent)) {
            block_error(what);
        } else {
            while (!abort_ && !check(Tok::Dedent) && !check(Tok::Eof)) {
                Stmt* s = parse_statement();
                if (s) out.push_back(s);
            }
            match(Tok::Dedent);
        }
    } else if (check(Tok::Eof)) {
        block_error(what);
    } else {
        // one-line block: `if x: return`, `while c: a(); b()`
        do {
            Stmt* s = parse_statement();
            if (s) out.push_back(s);
        } while (!abort_ && last_terminator_ == Tok::Semicolon && !check(Tok::Newline) && !check(Tok::Eof) &&
                 !check(Tok::Dedent));
    }
    --block_depth_;
}

void Parser::block_error(const char* what) { error_here(std::string("Expected indented block after ") + what + "."); }

Stmt* Parser::parse_statement() {
    switch (peek().type) {
    case Tok::KwIf: return parse_if();
    case Tok::KwWhile: return parse_while();
    case Tok::KwFor: return parse_for();
    case Tok::KwMatch: return parse_match();
    default: return parse_other_statement();
    }
}

Stmt* Parser::parse_var_stmt() {
    skip(); // var
    if (!check(Tok::Ident)) {
        error_here("Expected variable name after \"var\".");
        sync_line();
        last_terminator_ = Tok::Newline;
        return nullptr;
    }
    const Token& n = peek();
    Stmt* s = new_stmt(SK::Var, n.line, n.col);
    s->name = ast_.intern(n.text);
    skip();
    if (match(Tok::Colon)) {
        if (match(Tok::Assign)) {
            s->infer = true;
            s->a = parse_expr();
        } else {
            parse_type(s->type, false);
        }
    }
    if (!s->a && match(Tok::Assign)) s->a = parse_expr();
    end_statement();
    return s;
}

Stmt* Parser::parse_const_stmt() {
    skip(); // const
    if (!check(Tok::Ident)) {
        error_here("Expected constant name after \"const\".");
        sync_line();
        return nullptr;
    }
    const Token& n = peek();
    Stmt* s = new_stmt(SK::Const, n.line, n.col);
    s->name = ast_.intern(n.text);
    skip();
    bool infer = false;
    if (match(Tok::Colon)) {
        if (match(Tok::Assign)) infer = true;
        else parse_type(s->type, false);
    }
    if (infer || match(Tok::Assign)) {
        s->a = parse_expr();
    } else {
        error_here("Expected initializer after constant name.");
        sync_line();
        return nullptr;
    }
    end_statement();
    return s;
}

Stmt* Parser::parse_other_statement() {
    Token t = peek();
    switch (t.type) {
    case Tok::KwVar: return parse_var_stmt();
    case Tok::KwConst: return parse_const_stmt();
    case Tok::KwPass: {
        skip();
        Stmt* s = new_stmt(SK::Pass, t.line, t.col);
        end_statement();
        return s;
    }
    case Tok::KwBreak:
    case Tok::KwContinue: {
        skip();
        if (loop_depth_ == 0)
            error_at(t.line, t.col, std::string("\"") + tok_name(t.type) + "\" can only be used inside a loop.");
        Stmt* s = new_stmt(t.type == Tok::KwBreak ? SK::Break : SK::Continue, t.line, t.col);
        end_statement();
        return s;
    }
    case Tok::KwReturn: {
        skip();
        Stmt* s = new_stmt(SK::Return, t.line, t.col);
        if (!check(Tok::Newline) && !check(Tok::Semicolon) && !check(Tok::Eof) && !check(Tok::Dedent)) s->a = parse_expr();
        end_statement();
        return s;
    }
    case Tok::Indent:
        error_at(t.line, t.col, "Unexpected indentation.");
        skip_block();
        last_terminator_ = Tok::Newline;
        return nullptr;
    case Tok::Dedent:
    case Tok::Newline:
    case Tok::Semicolon:
        skip();
        last_terminator_ = Tok::Newline;
        return nullptr;
    case Tok::Annotation: {
        std::vector<Token> ann;
        std::vector<std::vector<Node*>> args;
        parse_annotation(ann, args);
        if (ann.back().text != "warning_ignore")
            error_at(t.line, t.col, "Annotation \"@" + t.text + "\" is not allowed inside a function.");
        match(Tok::Newline);
        last_terminator_ = Tok::Newline;
        return nullptr;
    }
    case Tok::KwElif:
    case Tok::KwElse:
        error_at(t.line, t.col, std::string("\"") + tok_name(t.type) + "\" without a matching \"if\".");
        skip();
        sync_line();
        if (check(Tok::Indent)) skip_block();
        last_terminator_ = Tok::Newline;
        return nullptr;
    case Tok::KwFunc:
    case Tok::KwSignal:
    case Tok::KwClass:
    case Tok::KwClassName:
    case Tok::KwStatic:
    case Tok::KwEnum:
        skip();
        unsupported_statement(t.type, t.line, t.col);
        last_terminator_ = Tok::Newline;
        return nullptr;
    case Tok::KwExtends:
        error_at(t.line, t.col, "\"extends\" can only be used at the top of the script.");
        sync_line();
        last_terminator_ = Tok::Newline;
        return nullptr;
    case Tok::KwBreakpoint:
        error_at(t.line, t.col, "\"breakpoint\" is not supported yet in JetScript.");
        skip();
        end_statement();
        return nullptr;
    default: return parse_simple_statement();
    }
}

static int assign_op(Tok t) {
    switch (t) {
    case Tok::Assign: return 0;
    case Tok::PlusEq: return int(BinOp::Add) + 1;
    case Tok::MinusEq: return int(BinOp::Sub) + 1;
    case Tok::StarEq: return int(BinOp::Mul) + 1;
    case Tok::SlashEq: return int(BinOp::Div) + 1;
    case Tok::PercentEq: return int(BinOp::Mod) + 1;
    case Tok::StarStarEq: return int(BinOp::Pow) + 1;
    case Tok::AmpEq: return int(BinOp::BitAnd) + 1;
    case Tok::PipeEq: return int(BinOp::BitOr) + 1;
    case Tok::CaretEq: return int(BinOp::BitXor) + 1;
    case Tok::ShlEq: return int(BinOp::Shl) + 1;
    case Tok::ShrEq: return int(BinOp::Shr) + 1;
    default: return -1;
    }
}

Stmt* Parser::parse_simple_statement() {
    int line = peek().line, col = peek().col;
    Node* e = parse_expr();
    if (abort_) return nullptr;
    int op = assign_op(peek().type);
    Stmt* s;
    if (op >= 0) {
        skip();
        s = new_stmt(SK::Assign, line, col);
        s->op = uint8_t(op);
        s->a = e;
        s->b = parse_expr();
    } else {
        s = new_stmt(SK::Expr, line, col);
        s->a = e;
    }
    end_statement();
    return s;
}

Stmt* Parser::parse_if() {
    Stmt* s = new_stmt(SK::If, peek().line, peek().col);
    skip();
    s->a = parse_expr();
    expect(Tok::Colon, "Expected \":\" after \"if\" condition.");
    parse_block(s->body, "\"if\"");
    while (!abort_ && check(Tok::KwElif)) {
        Stmt* e = new_stmt(SK::If, peek().line, peek().col);
        skip();
        e->a = parse_expr();
        expect(Tok::Colon, "Expected \":\" after \"elif\" condition.");
        parse_block(e->body, "\"elif\"");
        s->elifs.push_back(e);
    }
    if (!abort_ && check(Tok::KwElse)) {
        skip();
        expect(Tok::Colon, "Expected \":\" after \"else\".");
        parse_block(s->orelse, "\"else\"");
    }
    last_terminator_ = Tok::Newline;
    return s;
}

Stmt* Parser::parse_while() {
    Stmt* s = new_stmt(SK::While, peek().line, peek().col);
    skip();
    s->a = parse_expr();
    expect(Tok::Colon, "Expected \":\" after \"while\" condition.");
    ++loop_depth_;
    parse_block(s->body, "\"while\"");
    --loop_depth_;
    last_terminator_ = Tok::Newline;
    return s;
}

bool Parser::parse_for_header(Stmt* s) {
    if (!check(Tok::Ident)) {
        error_here("Expected loop variable name after \"for\".");
        sync_line();
        if (check(Tok::Indent)) skip_block();
        return false;
    }
    s->name = ast_.intern(peek().text);
    skip();
    if (match(Tok::Colon)) parse_type(s->type, false);
    if (!expect(Tok::KwIn, "Expected \"in\" after \"for\" variable name.")) {
        sync_line();
        if (check(Tok::Indent)) skip_block();
        return false;
    }
    return true;
}

Stmt* Parser::parse_for() {
    Stmt* s = new_stmt(SK::For, peek().line, peek().col);
    skip();
    if (!parse_for_header(s)) {
        last_terminator_ = Tok::Newline;
        return nullptr;
    }
    s->a = parse_expr();
    expect(Tok::Colon, "Expected \":\" after \"for\" condition.");
    ++loop_depth_;
    parse_block(s->body, "\"for\"");
    --loop_depth_;
    last_terminator_ = Tok::Newline;
    return s;
}

void Parser::parse_match_pattern(MatchBranch& br) {
    const Token& p = peek();
    if (p.type == Tok::Ident && p.text == "_") {
        br.patterns.push_back(new_node(NK::Wildcard, p.line, p.col));
        skip();
    } else if (p.type == Tok::KwVar) {
        skip();
        if (!check(Tok::Ident)) {
            error_here("Expected bind name after \"var\".");
            return;
        }
        const Token& name = peek();
        Node* n = new_node(NK::Binding, name.line, name.col);
        n->name = ast_.intern(name.text);
        skip();
        br.patterns.push_back(n);
    } else if (p.type == Tok::LBracket || p.type == Tok::LBrace || p.type == Tok::DotDot) {
        error_here("Array and dictionary patterns are not supported yet in JetScript.");
        br.patterns.push_back(parse_expr());
    } else {
        br.patterns.push_back(parse_expr());
    }
}

Stmt* Parser::parse_match() {
    Stmt* s = new_stmt(SK::Match, peek().line, peek().col);
    skip();
    s->a = parse_expr();
    expect(Tok::Colon, "Expected \":\" after \"match\" expression.");
    last_terminator_ = Tok::Newline;
    if (!match(Tok::Newline) || !match(Tok::Indent)) {
        error_here("Expected an indented block of patterns after \"match\".");
        sync_line();
        return s;
    }
    DepthGuard g(*this);
    if (!g.ok) return s;
    ++block_depth_;
    while (!abort_ && !check(Tok::Dedent) && !check(Tok::Eof)) {
        if (match(Tok::Newline)) continue;
        if (check(Tok::Indent)) {
            error_here("Unexpected indentation.");
            skip_block();
            continue;
        }
        s->branches.emplace_back();
        MatchBranch& br = s->branches.back();
        br.line = peek().line;
        br.col = peek().col;
        for (;;) {
            parse_match_pattern(br);
            if (!match(Tok::Comma)) break;
        }
        if (check(Tok::Ident) && peek().text == "when") error_here("Pattern guards (\"when\") are not supported yet in JetScript.");
        if (!expect(Tok::Colon, "Expected \":\" after match patterns.")) {
            sync_line();
            if (check(Tok::Indent)) skip_block();
            continue;
        }
        parse_block(s->branches.back().body, "match pattern");
    }
    match(Tok::Dedent);
    --block_depth_;
    last_terminator_ = Tok::Newline;
    return s;
}

Node* Parser::parse_expr() {
    DepthGuard g(*this);
    if (!g.ok) return new_here(NK::Bad);
    Node* n = parse_binary(P_OR);
    if (!abort_ && (check(Tok::KwIf) || check(Tok::KwAs))) n = parse_expr_suffix(n);
    return n;
}

// `x if cond else y` (right associative) and `x as Type`.
Node* Parser::parse_expr_suffix(Node* n) {
    if (check(Tok::KwIf)) {
        Node* r = new_here(NK::Ternary);
        skip();
        r->a = n;
        r->b = parse_binary(P_OR);
        if (!expect(Tok::KwElse, "Expected \"else\" after ternary operator condition.")) return n;
        r->c = parse_expr();
        finish(r);
        n = r;
    }
    while (!abort_ && check(Tok::KwAs)) {
        Node* r = new_here(NK::As);
        skip();
        r->a = n;
        parse_type(r->type, false);
        finish(r);
        n = r;
    }
    return n;
}

int Parser::binary_prec(BinOp& op, NK& kind, int& ntok) {
    const Token& t = peek();
    kind = NK::Binary;
    ntok = 1;
    switch (t.type) {
    case Tok::KwOr:
    case Tok::PipePipe: kind = NK::Or; return P_OR;
    case Tok::KwAnd:
    case Tok::AmpAmp: kind = NK::And; return P_AND;
    case Tok::KwIn: op = BinOp::In; return P_IN;
    case Tok::KwNot:
        if (peek(1).type == Tok::KwIn) {
            op = BinOp::NotIn;
            ntok = 2;
            return P_IN;
        }
        return P_NONE;
    case Tok::KwIs: kind = NK::Is; return P_IS;
    case Tok::Eq: op = BinOp::Eq; return P_CMP;
    case Tok::Ne: op = BinOp::Ne; return P_CMP;
    case Tok::Lt: op = BinOp::Lt; return P_CMP;
    case Tok::Le: op = BinOp::Le; return P_CMP;
    case Tok::Gt: op = BinOp::Gt; return P_CMP;
    case Tok::Ge: op = BinOp::Ge; return P_CMP;
    case Tok::Pipe: op = BinOp::BitOr; return P_BOR;
    case Tok::Caret: op = BinOp::BitXor; return P_BXOR;
    case Tok::Amp: op = BinOp::BitAnd; return P_BAND;
    case Tok::Shl: op = BinOp::Shl; return P_SHIFT;
    case Tok::Shr: op = BinOp::Shr; return P_SHIFT;
    case Tok::Plus: op = BinOp::Add; return P_ADD;
    case Tok::Minus: op = BinOp::Sub; return P_ADD;
    case Tok::Star: op = BinOp::Mul; return P_MUL;
    case Tok::Slash: op = BinOp::Div; return P_MUL;
    case Tok::Percent: op = BinOp::Mod; return P_MUL;
    case Tok::StarStar: op = BinOp::Pow; return P_POW;
    default: return P_NONE;
    }
}

Node* Parser::parse_binary(int min_prec) {
    Node* left = (min_prec <= P_NOT && (check(Tok::KwNot) || check(Tok::Bang))) ? parse_not() : parse_operand();
    for (;;) {
        if (abort_) return left;
        BinOp op = BinOp::Add;
        NK kind = NK::Binary;
        int ntok = 1;
        int prec = binary_prec(op, kind, ntok);
        if (prec == P_NONE || prec < min_prec) break;
        Node* n = new_here(kind);
        skip();
        if (ntok == 2) skip();
        n->a = left;
        left = n;
        if (kind == NK::Is) {
            if (match(Tok::KwNot)) n->negate = true;
            parse_type(n->type, false);
            finish(n);
            continue;
        }
        n->op = uint8_t(op);
        DepthGuard g(*this);
        if (!g.ok) return left;
        n->b = parse_binary(prec + 1);
        finish(n);
    }
    return left;
}

Node* Parser::parse_not() {
    Node* n = new_here(NK::Not);
    skip();
    DepthGuard g(*this);
    if (!g.ok) return n;
    n->a = parse_binary(P_NOT);
    finish(n);
    return n;
}

// Prefix operators (- + ~ not !) are chained in a loop and the operand is attached afterwards, so they don't recurse.
Node* Parser::parse_operand() {
    Node* head = nullptr;
    Node* tail = nullptr;
    int count = 0;
    for (;;) {
        Tok t = peek().type;
        NK kind = NK::Unary;
        uint8_t op = 0;
        if (t == Tok::Minus) op = uint8_t(UnOp::Neg);
        else if (t == Tok::Plus) op = uint8_t(UnOp::Pos);
        else if (t == Tok::Tilde) op = uint8_t(UnOp::BitNot);
        else if (t == Tok::KwNot || t == Tok::Bang) kind = NK::Not;
        else break;
        Node* u = new_here(kind);
        u->op = op;
        skip();
        if (tail) tail->a = u;
        else head = u;
        tail = u;
        if (++count + depth_ > kMaxNesting) {
            depth_error();
            return head;
        }
    }
    Node* n = parse_primary();
    for (;;) {
        if (abort_) break;
        Tok t = peek().type;
        if (t == Tok::Dot) n = parse_member(n);
        else if (t == Tok::LBracket) n = parse_index(n);
        else if (t == Tok::LParen) {
            bad_call();
            break;
        } else break;
    }
    return head ? attach_prefix(head, n) : n;
}

Node* Parser::attach_prefix(Node* head, Node* operand) {
    std::vector<Node*> chain;
    for (Node* x = head; x; x = x->a) chain.push_back(x);
    Node* inner = operand;
    // fold `-<number literal>` into the literal
    while (!chain.empty() && inner && inner->kind == NK::Literal && inner->value.is_number() &&
           chain.back()->kind == NK::Unary && UnOp(chain.back()->op) == UnOp::Neg) {
        std::string err;
        Value v;
        if (!unary_op(UnOp::Neg, inner->value, v, err)) break;
        inner->value = v;
        inner->line = chain.back()->line;
        inner->col = chain.back()->col;
        chain.pop_back();
    }
    for (size_t i = chain.size(); i-- > 0;) {
        chain[i]->a = inner;
        finish(chain[i]);
        inner = chain[i];
    }
    return inner;
}

void Parser::bad_call() {
    error_here("Calling the result of an expression is not supported (callables are not supported yet in JetScript).");
    Node* d = new_here(NK::Bad);
    parse_args(d);
}

Node* Parser::parse_member(Node* base) {
    skip(); // '.'
    if (!check(Tok::Ident)) {
        error_here("Expected identifier after \".\".");
        return base;
    }
    const Token& name = peek();
    Node* m = new_node(NK::Member, name.line, name.col);
    m->a = base;
    m->name = ast_.intern(name.text);
    skip();
    if (check(Tok::LParen)) {
        m->kind = NK::MethodCall;
        parse_args(m);
    }
    finish(m);
    return m;
}

Node* Parser::parse_index(Node* base) {
    Node* m = new_here(NK::Index);
    skip(); // '['
    m->a = base;
    m->b = parse_expr();
    expect(Tok::RBracket, "Expected closing \"]\" after subscription index.");
    finish(m);
    return m;
}

void Parser::parse_args(Node* call) {
    skip(); // '('
    while (!abort_ && !check(Tok::RParen) && !check(Tok::Eof)) {
        call->list.push_back(parse_expr());
        if (!match(Tok::Comma)) break;
    }
    if (call->list.size() > 255) error_at(call->line, call->col, "Too many arguments (the maximum is 255).");
    expect(Tok::RParen, "Expected closing \")\" after call arguments.");
}

Node* Parser::parse_call() {
    const Token& k = peek();
    Node* n = new_node(NK::Call, k.line, k.col);
    n->name = ast_.intern(k.text);
    skip();
    parse_args(n);
    finish(n);
    return n;
}

Node* Parser::parse_array() {
    Node* n = new_here(NK::ArrayLit);
    skip(); // '['
    while (!abort_ && !check(Tok::RBracket) && !check(Tok::Eof)) {
        n->list.push_back(parse_expr());
        if (!match(Tok::Comma)) break;
    }
    expect(Tok::RBracket, "Expected closing \"]\" after array elements.");
    finish(n);
    return n;
}

Node* Parser::dict_key_shorthand() {
    const Token& kt = peek();
    Node* key = new_node(NK::Literal, kt.line, kt.col);
    key->value = Value(kt.text);
    skip(); // identifier
    skip(); // '='
    return key;
}

Node* Parser::parse_dict() {
    Node* n = new_here(NK::DictLit);
    skip(); // '{'
    while (!abort_ && !check(Tok::RBrace) && !check(Tok::Eof)) {
        Node* key;
        if (check(Tok::Ident) && peek(1).type == Tok::Assign) {
            key = dict_key_shorthand();
        } else {
            key = parse_expr();
            if (!expect(Tok::Colon, "Expected \":\" after dictionary key.")) break;
        }
        n->list.push_back(key);
        n->list.push_back(parse_expr());
        if (!match(Tok::Comma)) break;
    }
    expect(Tok::RBrace, "Expected closing \"}\" after dictionary elements.");
    finish(n);
    return n;
}

Node* Parser::parse_primary() {
    switch (peek().type) {
    case Tok::LParen: {
        skip();
        Node* n = parse_expr();
        expect(Tok::RParen, "Expected closing \")\" after grouping expression.");
        return n;
    }
    case Tok::LBracket: return parse_array();
    case Tok::LBrace: return parse_dict();
    case Tok::Ident:
        if (peek(1).type == Tok::LParen) return parse_call();
        return parse_leaf();
    default: return parse_leaf();
    }
}

// Non-recursive primary expressions (literals, identifiers, self, $paths) and errors.
Node* Parser::parse_leaf() {
    const Token& t = peek();
    Node* n;
    switch (t.type) {
    case Tok::Int:
        n = new_node(NK::Literal, t.line, t.col);
        n->value = Value(t.ival);
        skip();
        return n;
    case Tok::Float:
        n = new_node(NK::Literal, t.line, t.col);
        n->value = Value(Real(t.fval));
        skip();
        return n;
    case Tok::String:
        n = new_node(NK::Literal, t.line, t.col);
        n->value = Value(t.text);
        skip();
        return n;
    case Tok::KwTrue:
    case Tok::KwFalse:
        n = new_node(NK::Literal, t.line, t.col);
        n->value = Value(t.type == Tok::KwTrue);
        skip();
        return n;
    case Tok::KwNull:
        n = new_node(NK::Literal, t.line, t.col);
        skip();
        return n;
    case Tok::KwSelf:
        n = new_node(NK::Self, t.line, t.col);
        skip();
        return n;
    case Tok::Ident:
        n = new_node(NK::Ident, t.line, t.col);
        n->name = ast_.intern(t.text);
        skip();
        return n;
    case Tok::NodePath:
        n = new_node(NK::GetNode, t.line, t.col);
        n->value = Value(t.text);
        skip();
        return n;
    default: break;
    }
    Token k = t;
    const char* msg = nullptr;
    switch (k.type) {
    case Tok::KwFunc: msg = "Lambdas are not supported yet in JetScript."; break;
    case Tok::KwAwait: msg = "\"await\" is not supported yet in JetScript."; break;
    case Tok::KwYield: msg = "\"yield\" is not supported in JetScript."; break;
    case Tok::KwPreload: msg = "\"preload\" is not supported yet in JetScript."; break;
    case Tok::KwSuper: msg = "\"super\" is not supported yet in JetScript."; break;
    case Tok::KwSignal:
    case Tok::KwClass:
    case Tok::KwClassName:
    case Tok::KwStatic:
    case Tok::KwBreakpoint:
        error_at(k.line, k.col, std::string("\"") + tok_name(k.type) + "\" is not supported yet in JetScript.");
        skip();
        return new_node(NK::Bad, k.line, k.col);
    default: break;
    }
    if (msg) {
        error_at(k.line, k.col, msg);
        skip();
        return new_node(NK::Bad, k.line, k.col);
    }
    error_at(k.line, k.col, "Expected expression, found " + describe(k) + " instead.");
    if (k.type != Tok::Newline && k.type != Tok::Eof && k.type != Tok::Indent && k.type != Tok::Dedent &&
        k.type != Tok::Semicolon && k.type != Tok::Colon && k.type != Tok::RParen && k.type != Tok::RBracket &&
        k.type != Tok::RBrace && k.type != Tok::Comma)
        skip();
    return new_node(NK::Bad, k.line, k.col);
}

} // namespace detail
} // namespace jetscript
