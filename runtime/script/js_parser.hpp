#pragma once
#include "js_lexer.hpp"

#include <deque>
#include <unordered_map>

namespace jetscript {
namespace detail {

enum class NK : uint8_t {
    Literal, Ident, Self, Unary, Binary, And, Or, Not, Ternary, Is, As,
    Member, Index, Call, MethodCall, ArrayLit, DictLit, GetNode, Wildcard, Binding, Bad
};

struct TypeRef {
    bool present = false;
    uint32_t name = 0;         // interned type name
    int line = 0, col = 0;
};

struct Node {
    NK kind = NK::Bad;
    uint8_t op = 0;            // BinOp / UnOp
    uint8_t depth = 1;
    int8_t cstate = 0;         // constant folding memo: 0 unknown, 1 constant, 2 not constant
    bool negate = false;       // `is not`
    int line = 0, col = 0;
    Node* a = nullptr;
    Node* b = nullptr;
    Node* c = nullptr;
    uint32_t name = 0;         // identifier / member name
    TypeRef type;              // `is` / `as` target
    Value value;               // literal or folded constant
    std::vector<Node*> list;   // call arguments, array items, dictionary key/value pairs
};

enum class SK : uint8_t { Expr, Var, Const, Assign, If, While, For, Match, Break, Continue, Pass, Return };

struct Stmt;

struct MatchBranch {
    std::vector<Node*> patterns;
    std::vector<Stmt*> body;
    int line = 0, col = 0;
};

struct Stmt {
    SK kind = SK::Pass;
    uint8_t op = 0;            // Assign: 0 for '=', otherwise BinOp + 1
    bool infer = false;        // `:=`
    int line = 0, col = 0;
    uint32_t name = 0;
    TypeRef type;
    Node* a = nullptr;         // condition / value / target / iterable
    Node* b = nullptr;         // assignment value
    std::vector<Stmt*> body;
    std::vector<Stmt*> elifs;  // If: elif branches (each an If statement)
    std::vector<Stmt*> orelse;
    std::vector<MatchBranch> branches;
};

struct VarDecl {
    uint32_t name = 0;
    TypeRef type;
    bool infer = false;
    Node* init = nullptr;
    bool exported = false, onready = false, has_range = false;
    std::vector<Node*> range_args;
    int line = 0, col = 0;
};

struct ConstDecl {
    uint32_t name = 0;
    TypeRef type;
    Node* init = nullptr;
    int line = 0, col = 0;
};

struct EnumDecl {
    uint32_t name = 0;         // 0: unnamed
    std::vector<std::pair<uint32_t, Node*>> values;
    std::vector<std::pair<int, int>> positions;
    int line = 0, col = 0;
};

struct Param {
    uint32_t name = 0;
    TypeRef type;
    bool infer = false;
    Node* def = nullptr;
    int line = 0, col = 0;
};

struct FuncDecl {
    uint32_t name = 0;
    std::vector<Param> params;
    TypeRef ret;
    std::vector<Stmt*> body;
    int line = 0, col = 0;
};

struct SignalDecl {
    uint32_t name = 0;
    std::vector<uint32_t> params; // names; their types are only documentation
    int line = 0, col = 0;
};

enum class ItemKind : uint8_t { Var, Const, Enum, Func, Signal };
struct ClassItem { ItemKind kind; uint32_t index; };

// Nodes and statements live in arenas here, so freeing a deep tree never recurses.
struct Ast {
    std::deque<Node> nodes;
    std::deque<Stmt> stmts;
    std::vector<std::string> names; // names[0] == ""
    std::unordered_map<std::string, uint32_t> name_map;

    bool has_extends = false;
    uint32_t extends_name = 0;
    int extends_line = 0, extends_col = 0;
    std::vector<VarDecl> vars;
    std::vector<ConstDecl> consts;
    std::vector<EnumDecl> enums;
    std::vector<FuncDecl> funcs;
    std::vector<SignalDecl> signals;
    std::vector<ClassItem> order;

    Ast() { names.push_back(std::string()); }
    uint32_t intern(std::string_view s);
    const std::string& name(uint32_t id) const { return names[id < names.size() ? id : 0]; }
};

#define JS_NOINLINE [[gnu::noinline]]

// Depth is capped at kMaxNesting, and recursive functions keep only scalar locals so each level costs little stack.
// Anything that needs strings or tokens goes into a JS_NOINLINE helper.
class Parser {
public:
    Parser(std::string_view src, Ast& ast, std::vector<Diagnostic>& errors);
    void parse_script();

private:
    const Token& peek(size_t k = 0);
    void skip();
    JS_NOINLINE Token advance();
    bool check(Tok t) { return peek().type == t; }
    bool match(Tok t);
    bool expect(Tok t, const char* message);
    JS_NOINLINE void error_at(int line, int col, const char* msg);
    JS_NOINLINE void error_at(int line, int col, const std::string& msg);
    JS_NOINLINE void error_here(const char* msg);
    JS_NOINLINE void error_here(const std::string& msg);
    JS_NOINLINE void depth_error();
    JS_NOINLINE void sync_line();
    bool end_statement();
    JS_NOINLINE void end_statement_error();
    std::string describe(const Token& t) const;

    Node* new_node(NK kind, int line, int col);
    Node* new_here(NK kind) { const Token& t = peek(); return new_node(kind, t.line, t.col); }
    void finish(Node* n);
    JS_NOINLINE void complexity_error(Node* n);
    Stmt* new_stmt(SK kind, int line, int col);

    void parse_class_item();
    JS_NOINLINE void parse_annotation(std::vector<Token>& annotations, std::vector<std::vector<Node*>>& args);
    JS_NOINLINE void parse_annotated();
    JS_NOINLINE void parse_extends();
    JS_NOINLINE void parse_member_var(std::vector<Token>& annotations, std::vector<std::vector<Node*>>& args);
    JS_NOINLINE void parse_const_decl();
    JS_NOINLINE void parse_enum();
    JS_NOINLINE void parse_func();
    JS_NOINLINE void parse_signal();
    JS_NOINLINE bool parse_type(TypeRef& t, bool allow_void);
    JS_NOINLINE void skip_block();
    JS_NOINLINE void unsupported_statement(Tok type, int line, int col);
    JS_NOINLINE void unexpected_class_item();

    // statements (recursive path: parse_block -> parse_statement -> parse_if/... -> parse_block)
    void parse_block(std::vector<Stmt*>& out, const char* what);
    JS_NOINLINE void block_error(const char* what);
    Stmt* parse_statement();
    JS_NOINLINE Stmt* parse_other_statement();
    JS_NOINLINE Stmt* parse_var_stmt();
    JS_NOINLINE Stmt* parse_const_stmt();
    JS_NOINLINE Stmt* parse_simple_statement();
    Stmt* parse_if();
    Stmt* parse_while();
    Stmt* parse_for();
    JS_NOINLINE bool parse_for_header(Stmt* s);
    Stmt* parse_match();
    JS_NOINLINE void parse_match_pattern(MatchBranch& br);

    // expressions (recursive path: parse_expr -> parse_binary -> parse_operand -> parse_primary -> parse_expr)
    Node* parse_expr();
    Node* parse_expr_suffix(Node* n);
    Node* parse_binary(int min_prec);
    Node* parse_not();
    Node* parse_operand();
    JS_NOINLINE Node* attach_prefix(Node* head, Node* operand);
    Node* parse_primary();
    JS_NOINLINE Node* parse_leaf();
    Node* parse_call();
    Node* parse_array();
    Node* parse_dict();
    JS_NOINLINE Node* dict_key_shorthand();
    Node* parse_member(Node* base);
    Node* parse_index(Node* base);
    JS_NOINLINE void bad_call();
    void parse_args(Node* call);
    int binary_prec(BinOp& op, NK& kind, int& ntok);

    struct DepthGuard {
        Parser& p;
        bool ok;
        explicit DepthGuard(Parser& parser) : p(parser), ok(++parser.depth_ <= kMaxNesting) {
            if (!ok) parser.depth_error();
        }
        ~DepthGuard() { --p.depth_; }
    };

    Lexer lex_;
    Ast& ast_;
    std::vector<Diagnostic>& errors_;
    std::deque<Token> la_;
    Tok last_terminator_ = Tok::Newline;
    int depth_ = 0;
    int block_depth_ = 0;
    bool abort_ = false;
    bool seen_items_ = false;
    int loop_depth_ = 0;
};

} // namespace detail
} // namespace jetscript
