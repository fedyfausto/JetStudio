#pragma once
#include "js_lexer.hpp"

#include <deque>
#include <unordered_map>

namespace jetscript{
    namespace detail{

        enum class NK : uint8_t{
            Literal, Ident, Self, Unary, Binary, And, Or, Not, Ternary, Is, As,
            Member, Index, Call, MethodCall, ArrayLit, DictLit, GetNode, Wildcard, Binding, Bad
        };

        struct TypeRef{
            bool present = false;
            // interned type name
            uint32_t name = 0;
            int line = 0, col = 0;
        };

        struct Node{
            NK kind = NK::Bad;
            // BinOp / UnOp
            uint8_t op = 0;
            uint8_t depth = 1;
            // constant folding memo: 0 unknown, 1 constant, 2 not constant
            int8_t cstate = 0;
            // `is not`
            bool negate = false;
            int line = 0, col = 0;
            Node* a = nullptr;
            Node* b = nullptr;
            Node* c = nullptr;
            // identifier / member name
            uint32_t name = 0;
            // `is` / `as` target
            TypeRef type;
            // literal or folded constant
            Value value;
            // call arguments, array items, dictionary key/value pairs
            std::vector<Node*> list;
        };

        enum class SK : uint8_t{ Expr, Var, Const, Assign, If, While, For, Match, Break, Continue, Pass, Return };

        struct Stmt;

        struct MatchBranch{
            std::vector<Node*> patterns;
            std::vector<Stmt*> body;
            int line = 0, col = 0;
        };

        struct Stmt{
            SK kind = SK::Pass;
            // Assign: 0 for '=', otherwise BinOp + 1
            uint8_t op = 0;
            // `:=`
            bool infer = false;
            int line = 0, col = 0;
            uint32_t name = 0;
            TypeRef type;
            // condition / value / target / iterable
            Node* a = nullptr;
            // assignment value
            Node* b = nullptr;
            std::vector<Stmt*> body;
            // If: elif branches (each an If statement)
            std::vector<Stmt*> elifs;
            std::vector<Stmt*> orelse;
            std::vector<MatchBranch> branches;
        };

        struct VarDecl{
            uint32_t name = 0;
            TypeRef type;
            bool infer = false;
            Node* init = nullptr;
            bool exported = false, onready = false, hasRange = false;
            std::vector<Node*> rangeArgs;
            int line = 0, col = 0;
        };

        struct ConstDecl{
            uint32_t name = 0;
            TypeRef type;
            Node* init = nullptr;
            int line = 0, col = 0;
        };

        struct EnumDecl{
            // 0: unnamed
            uint32_t name = 0;
            std::vector<std::pair<uint32_t, Node*>> values;
            std::vector<std::pair<int, int>> positions;
            int line = 0, col = 0;
        };

        struct Param{
            uint32_t name = 0;
            TypeRef type;
            bool infer = false;
            Node* def = nullptr;
            int line = 0, col = 0;
        };

        struct FuncDecl{
            uint32_t name = 0;
            std::vector<Param> params;
            TypeRef ret;
            std::vector<Stmt*> body;
            int line = 0, col = 0;
        };

        struct SignalDecl{
            uint32_t name = 0;
            // names; their types are only documentation
            std::vector<uint32_t> params;
            int line = 0, col = 0;
        };

        enum class ItemKind : uint8_t{ Var, Const, Enum, Func, Signal };
        struct ClassItem{ ItemKind kind; uint32_t index; };

        // Nodes and statements live in arenas here, so freeing a deep tree never recurses.
        struct Ast{
            std::deque<Node> nodes;
            std::deque<Stmt> stmts;
            // names[0] == ""
            std::vector<std::string> names;
            std::unordered_map<std::string, uint32_t> nameMap;

            bool hasExtends = false;
            uint32_t extendsName = 0;
            int extendsLine = 0, extendsCol = 0;
            std::vector<VarDecl> vars;
            std::vector<ConstDecl> consts;
            std::vector<EnumDecl> enums;
            std::vector<FuncDecl> funcs;
            std::vector<SignalDecl> signals;
            std::vector<ClassItem> order;

            Ast(){ names.push_back(std::string()); }
            uint32_t intern(std::string_view s);
            const std::string& name(uint32_t id) const{ return names[id < names.size() ? id : 0]; }
        };

#define JS_NOINLINE [[gnu::noinline]]

        // Depth is capped at kMaxNesting, and recursive functions keep only scalar locals so each level costs little stack.
        // Anything that needs strings or tokens goes into a JS_NOINLINE helper.
        class Parser{
            public:
                Parser(std::string_view src, Ast& ast, std::vector<Diagnostic>& errors);
                void parseScript();

            private:
                const Token& peek(size_t k = 0);
                void skip();
                JS_NOINLINE Token advance();
                bool check(Tok t){ return peek().type == t; }
                bool match(Tok t);
                bool expect(Tok t, const char* message);
                JS_NOINLINE void errorAt(int line, int col, const char* msg);
                JS_NOINLINE void errorAt(int line, int col, const std::string& msg);
                JS_NOINLINE void errorHere(const char* msg);
                JS_NOINLINE void errorHere(const std::string& msg);
                JS_NOINLINE void depthError();
                JS_NOINLINE void syncLine();
                bool endStatement();
                JS_NOINLINE void endStatementError();
                std::string describe(const Token& t) const;

                Node* new_node(NK kind, int line, int col);
                Node* newHere(NK kind){ const Token& t = peek(); return new_node(kind, t.line, t.col); }
                void finish(Node* n);
                JS_NOINLINE void complexityError(Node* n);
                Stmt* newStmt(SK kind, int line, int col);

                void parseClassItem();
                JS_NOINLINE void parseAnnotation(std::vector<Token>& annotations, std::vector<std::vector<Node*>>& args);
                JS_NOINLINE void parseAnnotated();
                JS_NOINLINE void parseExtends();
                JS_NOINLINE void parseMemberVar(std::vector<Token>& annotations, std::vector<std::vector<Node*>>& args);
                JS_NOINLINE void parseConstDecl();
                JS_NOINLINE void parseEnum();
                JS_NOINLINE void parseFunc();
                JS_NOINLINE void parseSignal();
                JS_NOINLINE bool parseType(TypeRef& t, bool allowVoid);
                JS_NOINLINE void skipBlock();
                JS_NOINLINE void unsupportedStatement(Tok type, int line, int col);
                JS_NOINLINE void unexpectedClassItem();

                // statements (recursive path: parseBlock -> parseStatement -> parseIf/... -> parseBlock)
                void parseBlock(std::vector<Stmt*>& out, const char* what);
                JS_NOINLINE void blockError(const char* what);
                Stmt* parseStatement();
                JS_NOINLINE Stmt* parseOtherStatement();
                JS_NOINLINE Stmt* parseVarStmt();
                JS_NOINLINE Stmt* parseConstStmt();
                JS_NOINLINE Stmt* parseSimpleStatement();
                Stmt* parseIf();
                Stmt* parseWhile();
                Stmt* parseFor();
                JS_NOINLINE bool parseForHeader(Stmt* s);
                Stmt* parseMatch();
                JS_NOINLINE void parseMatchPattern(MatchBranch& br);

                // expressions (recursive path: parseExpr -> parseBinary -> parseOperand -> parsePrimary -> parseExpr)
                Node* parseExpr();
                Node* parseExprSuffix(Node* n);
                Node* parseBinary(int minPrec);
                Node* parseNot();
                Node* parseOperand();
                JS_NOINLINE Node* attachPrefix(Node* head, Node* operand);
                Node* parsePrimary();
                JS_NOINLINE Node* parseLeaf();
                Node* parseCall();
                Node* parseArray();
                Node* parseDict();
                JS_NOINLINE Node* dictKeyShorthand();
                Node* parseMember(Node* base);
                Node* parseIndex(Node* base);
                JS_NOINLINE void badCall();
                void parse_args(Node* call);
                int binaryPrec(BinOp& op, NK& kind, int& ntok);

                struct DepthGuard{
                    Parser& p;
                    bool ok;
                    explicit DepthGuard(Parser& parser) : p(parser), ok(++parser.depth_ <= kMaxNesting){
                        if(!ok) parser.depthError();
                    }
                    ~DepthGuard(){ --p.depth_; }
                };

                Lexer lex_;
                Ast& ast_;
                std::vector<Diagnostic>& errors_;
                std::deque<Token> la_;
                Tok lastTerminator_ = Tok::Newline;
                int depth_ = 0;
                int blockDepth_ = 0;
                bool abort_ = false;
                bool seenItems_ = false;
                int loopDepth_ = 0;
        };

    }
}
