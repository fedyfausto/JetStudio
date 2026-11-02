#include "js_parser.hpp"

namespace jetscript{
    namespace detail{

        namespace{
            enum Prec{ P_NONE = 0, P_OR = 1, P_AND, P_NOT, P_IN, P_IS, P_CMP, P_BOR, P_BXOR, P_BAND, P_SHIFT, P_ADD, P_MUL, P_POW };
        }

        uint32_t Ast::intern(std::string_view s){
            auto it = nameMap.find(std::string(s));

            if(it != nameMap.end()) return it->second;

            uint32_t id = uint32_t(names.size());
            names.emplace_back(s);
            nameMap.emplace(names.back(), id);
            return id;
        }

        Parser::Parser(std::string_view src, Ast& ast, std::vector<Diagnostic>& errors)
            : lex_(src), ast_(ast), errors_(errors){}

        const Token& Parser::peek(size_t k){
            while(la_.size() <= k){
                Token t = lex_.next();

                if(t.type == Tok::Error){
                    errorAt(t.line, t.col, t.text);
                    continue;
                }

                la_.push_back(std::move(t));
            }

            return la_[k];
        }

        void Parser::skip(){
            peek();
            la_.pop_front();
        }

        Token Parser::advance(){
            peek();
            Token t = std::move(la_.front());
            la_.pop_front();
            return t;
        }

        bool Parser::match(Tok t){
            if(!check(t)) return false;

            skip();
            return true;
        }

        bool Parser::expect(Tok t, const char* message){
            if(match(t)) return true;

            errorHere(message);
            return false;
        }

        std::string Parser::describe(const Token& t) const{
            switch(t.type){
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
                default: return std::string("\"") + tokName(t.type) + "\"";
            }
        }

        void Parser::errorAt(int line, int col, const std::string& msg){
            if(abort_) return;

            //one error per line, no cascades
            if(!errors_.empty() && errors_.back().line == line) return;

            if(int(errors_.size()) >= kMaxErrors){
                abort_ = true;
                return;
            }

            Diagnostic d;
            d.line = line;
            d.column = col;
            d.message = msg;
            errors_.push_back(std::move(d));
        }

        void Parser::errorAt(int line, int col, const char* msg){ errorAt(line, col, std::string(msg)); }

        void Parser::errorHere(const std::string& msg){
            const Token& t = peek();
            errorAt(t.line, t.col, msg);
        }

        void Parser::errorHere(const char* msg){ errorHere(std::string(msg)); }

        void Parser::depthError(){
            if(abort_) return;

            const Token& t = peek();
            errorAt(t.line, t.col, "Maximum nesting depth exceeded (" + std::to_string(kMaxNesting) +
                                        " levels of blocks and expressions).");
            abort_ = true;
        }

        void Parser::syncLine(){
            for(;;){
                Tok t = peek().type;

                if(t == Tok::Eof || t == Tok::Dedent || t == Tok::Indent) return;

                skip();

                if(t == Tok::Newline) return;
            }
        }

        bool Parser::endStatement(){
            if(match(Tok::Semicolon)){
                lastTerminator_ = Tok::Semicolon;

                if(match(Tok::Newline)) lastTerminator_ = Tok::Newline;

                return true;
            }

            lastTerminator_ = Tok::Newline;

            if(match(Tok::Newline)) return true;

            if(check(Tok::Eof) || check(Tok::Dedent)) return true;

            endStatementError();
            return false;
        }

        void Parser::endStatementError(){
            errorHere("Expected end of statement, found " + describe(peek()) + " instead.");
            syncLine();
        }

        void Parser::skipBlock(){
            if(!match(Tok::Indent)) return;

            int level = 1;

            while(level > 0){
                Tok t = peek().type;
                skip();

                if(t == Tok::Eof) return;

                if(t == Tok::Indent) ++level;
                else if(t == Tok::Dedent) --level;
            }
        }

        Node* Parser::new_node(NK kind, int line, int col){
            ast_.nodes.emplace_back();
            Node* n = &ast_.nodes.back();
            n->kind = kind;
            n->line = line;
            n->col = col;
            return n;
        }

        void Parser::finish(Node* n){
            int d = 0;

            if(n->a && n->a->depth > d) d = n->a->depth;

            if(n->b && n->b->depth > d) d = n->b->depth;

            if(n->c && n->c->depth > d) d = n->c->depth;

            for(Node* x : n->list)
                if(x && x->depth > d) d = x->depth;

            ++d;

            if(d > 255) d = 255;

            n->depth = uint8_t(d);

            if(d + blockDepth_ > kMaxNesting) complexityError(n);
        }

        void Parser::complexityError(Node* n){
            if(abort_) return;

            errorAt(n->line, n->col, "Expression is too complex (maximum nesting depth is " + std::to_string(kMaxNesting) +
                                          "); split it into several statements.");
            abort_ = true;
        }

        Stmt* Parser::newStmt(SK kind, int line, int col){
            ast_.stmts.emplace_back();
            Stmt* s = &ast_.stmts.back();
            s->kind = kind;
            s->line = line;
            s->col = col;
            return s;
        }

        void Parser::parseScript(){
            while(!abort_ && !check(Tok::Eof)) parseClassItem();
        }

        void Parser::unsupportedStatement(Tok type, int line, int col){
            const char* msg = "This construct is not supported yet in JetScript.";

            switch(type){
                case Tok::KwSignal: msg = "Signals can only be declared at the top level of the script."; break;
                case Tok::KwClass: msg = "Inner classes (\"class\") are not supported yet in JetScript."; break;
                case Tok::KwClassName: msg = "\"class_name\" is not supported yet in JetScript."; break;
                case Tok::KwStatic: msg = "Static functions and variables (\"static\") are not supported yet in JetScript."; break;
                case Tok::KwEnum: msg = "Enums can only be declared at the top level of the script."; break;
                case Tok::KwFunc: msg = "Lambdas and nested functions are not supported yet in JetScript."; break;
                default: break;
            }

            errorAt(line, col, msg);
            syncLine();

            if(check(Tok::Indent)) skipBlock();
        }

        void Parser::parseAnnotation(std::vector<Token>& annotations, std::vector<std::vector<Node*>>& args){
            Token t = advance();
            std::vector<Node*> a;

            if(match(Tok::LParen)){
                while(!abort_ && !check(Tok::RParen) && !check(Tok::Eof) && !check(Tok::Newline)){
                    a.push_back(parseExpr());

                    if(!match(Tok::Comma)) break;
                }

                expect(Tok::RParen, "Expected \")\" after annotation arguments.");
            }

            if(t.text != "export" && t.text != "export_range" && t.text != "onready" && t.text != "warning_ignore"){
                errorAt(t.line, t.col, "Annotation \"@" + t.text + "\" is not supported yet in JetScript.");
            } else if(t.text == "export_range" && (a.size() < 2 || a.size() > 4)){
                errorAt(t.line, t.col, "Annotation \"@export_range\" expects (min, max) or (min, max, step).");
            } else if((t.text == "export" || t.text == "onready") && !a.empty()){
                errorAt(t.line, t.col, "Annotation \"@" + t.text + "\" does not take arguments.");
            }

            annotations.push_back(std::move(t));
            args.push_back(std::move(a));
        }

        void Parser::parseAnnotated(){
            std::vector<Token> ann;
            std::vector<std::vector<Node*>> args;

            while(!abort_ && check(Tok::Annotation)){
                parseAnnotation(ann, args);

                while(match(Tok::Newline)){}
            }

            if(check(Tok::KwVar)){
                parseMemberVar(ann, args);
                return;
            }

            bool onlyIgnore = true;

            for(const Token& a : ann)
                if(a.text != "warning_ignore") onlyIgnore = false;

            if(!onlyIgnore && !ann.empty())
                errorHere("Annotation \"@" + ann.front().text + "\" must be followed by a variable declaration.");
        }

        void Parser::parseExtends(){
            Token kw = advance();

            if(ast_.hasExtends) errorAt(kw.line, kw.col, "\"extends\" can only be used once.");
            else if(seenItems_) errorAt(kw.line, kw.col, "\"extends\" must be the first statement of the script.");

            if(check(Tok::Ident)){
                Token n = advance();

                if(!ast_.hasExtends){
                    ast_.hasExtends = true;
                    ast_.extendsName = ast_.intern(n.text);
                    ast_.extendsLine = n.line;
                    ast_.extendsCol = n.col;
                }

                if(check(Tok::Dot)) errorHere("Extending inner classes is not supported yet in JetScript.");
            } else if(check(Tok::String)){
                errorHere("Extending scripts by path is not supported yet in JetScript.");
                skip();
            } else {
                errorHere("Expected class name after \"extends\".");
            }

            seenItems_ = true;
            endStatement();
        }

        void Parser::unexpectedClassItem(){
            Token t = peek();
            errorAt(t.line, t.col, "Unexpected " + describe(t) + " in class body.");

            if(t.type == Tok::Eof) return;

            skip();
            syncLine();

            if(check(Tok::Indent)) skipBlock();
        }

        void Parser::parseClassItem(){
            const Token& t = peek();

            switch(t.type){
                case Tok::Newline:
                case Tok::Semicolon:
                case Tok::Dedent: skip(); return;
                case Tok::Indent:
                    errorHere("Unexpected indentation.");
                    skipBlock();
                    return;
                case Tok::Annotation: parseAnnotated(); return;
                case Tok::KwExtends: parseExtends(); return;
                case Tok::KwVar:{
                    std::vector<Token> ann;
                    std::vector<std::vector<Node*>> args;
                    parseMemberVar(ann, args);
                    return;
                }
                case Tok::KwConst: parseConstDecl(); return;
                case Tok::KwEnum: parseEnum(); return;
                case Tok::KwFunc: parseFunc(); return;
                case Tok::KwSignal: parseSignal(); return;
                case Tok::KwStatic:
                case Tok::KwClass:
                case Tok::KwClassName:{
                    Tok type = t.type;
                    int line = t.line, col = t.col;
                    skip();
                    unsupportedStatement(type, line, col);
                    return;
                }
                case Tok::KwPass:
                    skip();
                    endStatement();
                    return;
                default: unexpectedClassItem(); return;
            }
        }

        bool Parser::parseType(TypeRef& type, bool allowVoid){
            const Token& t = peek();
            type.line = t.line;
            type.col = t.col;

            if(t.type == Tok::KwVoid){
                if(!allowVoid) errorHere("\"void\" is only allowed as a function return type.");

                skip();
                type.present = true;
                type.name = ast_.intern("void");
                return true;
            }

            if(t.type != Tok::Ident){
                errorHere("Expected type after \":\" or \"->\".");
                return false;
            }

            type.present = true;
            type.name = ast_.intern(t.text);
            skip();

            if(check(Tok::LBracket)){
                errorHere("Typed arrays are not supported yet in JetScript.");
                int depth = 0;

                while(!check(Tok::Eof) && !check(Tok::Newline)){
                    Tok k = peek().type;
                    skip();

                    if(k == Tok::LBracket) ++depth;
                    else if(k == Tok::RBracket && --depth <= 0) break;
                }
            }

            if(check(Tok::Dot)){
                errorHere("Nested types are not supported yet in JetScript.");
                skip();

                if(check(Tok::Ident)) skip();
            }

            return true;
        }

        void Parser::parseMemberVar(std::vector<Token>& annotations, std::vector<std::vector<Node*>>& args){
            skip();
            seenItems_ = true;
            VarDecl d;

            if(!check(Tok::Ident)){
                errorHere("Expected variable name after \"var\".");
                syncLine();
                return;
            }

            Token n = advance();
            d.name = ast_.intern(n.text);
            d.line = n.line;
            d.col = n.col;

            if(match(Tok::Colon)){
                if(match(Tok::Assign)){
                    d.infer = true;
                    d.init = parseExpr();
                } else {
                    parseType(d.type, false);
                }
            }

            if(!d.init && match(Tok::Assign)) d.init = parseExpr();

            if(check(Tok::Colon)){
                errorHere("Property setters and getters (set/get) are not supported yet in JetScript.");
                skip();
                syncLine();

                if(check(Tok::Indent)) skipBlock();

                return;
            }

            for(size_t i = 0; i < annotations.size(); ++i){
                const std::string& a = annotations[i].text;

                if(a == "export"){
                    d.exported = true;
                } else if(a == "export_range"){
                    d.exported = true;
                    d.hasRange = true;
                    d.rangeArgs = args[i];
                } else if(a == "onready"){
                    d.onready = true;
                }
            }

            ast_.order.push_back({ItemKind::Var, uint32_t(ast_.vars.size())});
            ast_.vars.push_back(std::move(d));
            endStatement();
        }

        void Parser::parseConstDecl(){
            skip();
            seenItems_ = true;
            ConstDecl d;

            if(!check(Tok::Ident)){
                errorHere("Expected constant name after \"const\".");
                syncLine();
                return;
            }

            Token n = advance();
            d.name = ast_.intern(n.text);
            d.line = n.line;
            d.col = n.col;
            bool infer = false;

            if(match(Tok::Colon)){
                if(match(Tok::Assign)) infer = true;
                else parseType(d.type, false);
            }

            if(infer || match(Tok::Assign)){
                d.init = parseExpr();
            } else {
                errorHere("Expected initializer after constant name.");
                syncLine();
                return;
            }

            ast_.order.push_back({ItemKind::Const, uint32_t(ast_.consts.size())});
            ast_.consts.push_back(std::move(d));
            endStatement();
        }

        void Parser::parseEnum(){
            Token kw = advance();
            seenItems_ = true;
            EnumDecl e;
            e.line = kw.line;
            e.col = kw.col;

            if(check(Tok::Ident)){
                Token n = advance();
                e.name = ast_.intern(n.text);
                e.line = n.line;
                e.col = n.col;
            }

            if(!expect(Tok::LBrace, "Expected \"{\" after \"enum\".")){
                syncLine();
                return;
            }

            while(!abort_ && !check(Tok::RBrace) && !check(Tok::Eof)){
                if(!check(Tok::Ident)){
                    errorHere("Expected identifier for enum key.");
                    break;
                }

                Token k = advance();
                Node* value = nullptr;

                if(match(Tok::Assign)) value = parseExpr();

                e.values.emplace_back(ast_.intern(k.text), value);
                e.positions.emplace_back(k.line, k.col);

                if(!match(Tok::Comma)) break;
            }

            if(!expect(Tok::RBrace, "Expected closing \"}\" for enum.")){
                while(!check(Tok::Eof) && !check(Tok::RBrace) && !check(Tok::Newline)) skip();

                match(Tok::RBrace);
            }

            ast_.order.push_back({ItemKind::Enum, uint32_t(ast_.enums.size())});
            ast_.enums.push_back(std::move(e));
            endStatement();
        }

        // signal name, or signal name(a, b: int)
        void Parser::parseSignal(){
            skip();
            seenItems_ = true;
            SignalDecl d;

            if(!check(Tok::Ident)){
                errorHere("Expected signal name after \"signal\".");
                syncLine();
                return;
            }

            Token n = advance();
            d.name = ast_.intern(n.text);
            d.line = n.line;
            d.col = n.col;

            if(match(Tok::LParen)){
                while(!abort_ && !check(Tok::RParen) && !check(Tok::Eof)){
                    if(!check(Tok::Ident)){
                        errorHere("Expected parameter name.");
                        break;
                    }

                    d.params.push_back(ast_.intern(advance().text));

                    if(match(Tok::Colon)){
                        TypeRef type;
                        parseType(type, false);
                    }

                    if(check(Tok::Assign)){
                        errorHere("Signal parameters can not have default values.");
                        break;
                    }

                    if(!match(Tok::Comma)) break;
                }

                if(!expect(Tok::RParen, "Expected closing \")\" after signal parameters.")){
                    syncLine();
                    return;
                }
            }

            ast_.order.push_back({ItemKind::Signal, uint32_t(ast_.signals.size())});
            ast_.signals.push_back(std::move(d));
            endStatement();
        }

        void Parser::parseFunc(){
            Token kw = advance();
            seenItems_ = true;
            FuncDecl f;
            f.line = kw.line;
            f.col = kw.col;

            if(!check(Tok::Ident)){
                errorHere("Expected function name after \"func\".");
                syncLine();

                if(check(Tok::Indent)) skipBlock();

                return;
            }

            Token n = advance();
            f.name = ast_.intern(n.text);
            f.line = n.line;
            f.col = n.col;

            if(!expect(Tok::LParen, "Expected \"(\" after function name.")){
                syncLine();

                if(check(Tok::Indent)) skipBlock();

                return;
            }

            while(!abort_ && !check(Tok::RParen) && !check(Tok::Eof)){
                if(check(Tok::DotDot) || check(Tok::Dot)){
                    errorHere("Variadic functions are not supported yet in JetScript.");
                    break;
                }

                if(!check(Tok::Ident)){
                    errorHere("Expected parameter name.");
                    break;
                }

                Token pn = advance();
                Param p;
                p.name = ast_.intern(pn.text);
                p.line = pn.line;
                p.col = pn.col;

                if(match(Tok::Colon)){
                    if(match(Tok::Assign)){
                        p.infer = true;
                        p.def = parseExpr();
                    } else {
                        parseType(p.type, false);
                    }
                }

                if(!p.def && match(Tok::Assign)) p.def = parseExpr();

                f.params.push_back(p);

                if(!match(Tok::Comma)) break;
            }

            if(!expect(Tok::RParen, "Expected closing \")\" after function parameters.")){
                while(!check(Tok::Eof) && !check(Tok::Newline) && !check(Tok::Colon)) skip();

                match(Tok::RParen);
            }

            if(match(Tok::Arrow)) parseType(f.ret, true);

            if(!expect(Tok::Colon, "Expected \":\" after function declaration.")){
                syncLine();

                if(check(Tok::Indent)) skipBlock();

                return;
            }

            loopDepth_ = 0;
            parseBlock(f.body, "function declaration");
            ast_.order.push_back({ItemKind::Func, uint32_t(ast_.funcs.size())});
            ast_.funcs.push_back(std::move(f));
        }

        void Parser::parseBlock(std::vector<Stmt*>& out, const char* what){
            DepthGuard g(*this);

            if(!g.ok) return;

            ++blockDepth_;

            if(match(Tok::Newline)){
                if(!match(Tok::Indent)){
                    blockError(what);
                } else {
                    while(!abort_ && !check(Tok::Dedent) && !check(Tok::Eof)){
                        Stmt* s = parseStatement();

                        if(s) out.push_back(s);
                    }

                    match(Tok::Dedent);
                }
            } else if(check(Tok::Eof)){
                blockError(what);
            } else {
                //one-line block: `if x: return`, `while c: a(); b()`
                do{
                    Stmt* s = parseStatement();

                    if(s) out.push_back(s);
                } while(!abort_ && lastTerminator_ == Tok::Semicolon && !check(Tok::Newline) && !check(Tok::Eof) &&
                         !check(Tok::Dedent));
            }

            --blockDepth_;
        }

        void Parser::blockError(const char* what){ errorHere(std::string("Expected indented block after ") + what + "."); }

        Stmt* Parser::parseStatement(){
            switch(peek().type){
                case Tok::KwIf: return parseIf();
                case Tok::KwWhile: return parseWhile();
                case Tok::KwFor: return parseFor();
                case Tok::KwMatch: return parseMatch();
                default: return parseOtherStatement();
            }
        }

        Stmt* Parser::parseVarStmt(){
            skip();

            if(!check(Tok::Ident)){
                errorHere("Expected variable name after \"var\".");
                syncLine();
                lastTerminator_ = Tok::Newline;
                return nullptr;
            }

            const Token& n = peek();
            Stmt* s = newStmt(SK::Var, n.line, n.col);
            s->name = ast_.intern(n.text);
            skip();

            if(match(Tok::Colon)){
                if(match(Tok::Assign)){
                    s->infer = true;
                    s->a = parseExpr();
                } else {
                    parseType(s->type, false);
                }
            }

            if(!s->a && match(Tok::Assign)) s->a = parseExpr();

            endStatement();
            return s;
        }

        Stmt* Parser::parseConstStmt(){
            skip();

            if(!check(Tok::Ident)){
                errorHere("Expected constant name after \"const\".");
                syncLine();
                return nullptr;
            }

            const Token& n = peek();
            Stmt* s = newStmt(SK::Const, n.line, n.col);
            s->name = ast_.intern(n.text);
            skip();
            bool infer = false;

            if(match(Tok::Colon)){
                if(match(Tok::Assign)) infer = true;
                else parseType(s->type, false);
            }

            if(infer || match(Tok::Assign)){
                s->a = parseExpr();
            } else {
                errorHere("Expected initializer after constant name.");
                syncLine();
                return nullptr;
            }

            endStatement();
            return s;
        }

        Stmt* Parser::parseOtherStatement(){
            Token t = peek();

            switch(t.type){
                case Tok::KwVar: return parseVarStmt();
                case Tok::KwConst: return parseConstStmt();
                case Tok::KwPass:{
                    skip();
                    Stmt* s = newStmt(SK::Pass, t.line, t.col);
                    endStatement();
                    return s;
                }
                case Tok::KwBreak:
                case Tok::KwContinue:{
                    skip();

                    if(loopDepth_ == 0)
                        errorAt(t.line, t.col, std::string("\"") + tokName(t.type) + "\" can only be used inside a loop.");
                    Stmt* s = newStmt(t.type == Tok::KwBreak ? SK::Break : SK::Continue, t.line, t.col);
                    endStatement();
                    return s;
                }
                case Tok::KwReturn:{
                    skip();
                    Stmt* s = newStmt(SK::Return, t.line, t.col);

                    if(!check(Tok::Newline) && !check(Tok::Semicolon) && !check(Tok::Eof) && !check(Tok::Dedent)) s->a = parseExpr();

                    endStatement();
                    return s;
                }
                case Tok::Indent:
                    errorAt(t.line, t.col, "Unexpected indentation.");
                    skipBlock();
                    lastTerminator_ = Tok::Newline;
                    return nullptr;
                case Tok::Dedent:
                case Tok::Newline:
                case Tok::Semicolon:
                    skip();
                    lastTerminator_ = Tok::Newline;
                    return nullptr;
                case Tok::Annotation:{
                    std::vector<Token> ann;
                    std::vector<std::vector<Node*>> args;
                    parseAnnotation(ann, args);

                    if(ann.back().text != "warning_ignore")
                        errorAt(t.line, t.col, "Annotation \"@" + t.text + "\" is not allowed inside a function.");
                    match(Tok::Newline);
                    lastTerminator_ = Tok::Newline;
                    return nullptr;
                }
                case Tok::KwElif:
                case Tok::KwElse:
                    errorAt(t.line, t.col, std::string("\"") + tokName(t.type) + "\" without a matching \"if\".");
                    skip();
                    syncLine();

                    if(check(Tok::Indent)) skipBlock();

                    lastTerminator_ = Tok::Newline;
                    return nullptr;
                case Tok::KwFunc:
                case Tok::KwSignal:
                case Tok::KwClass:
                case Tok::KwClassName:
                case Tok::KwStatic:
                case Tok::KwEnum:
                    skip();
                    unsupportedStatement(t.type, t.line, t.col);
                    lastTerminator_ = Tok::Newline;
                    return nullptr;
                case Tok::KwExtends:
                    errorAt(t.line, t.col, "\"extends\" can only be used at the top of the script.");
                    syncLine();
                    lastTerminator_ = Tok::Newline;
                    return nullptr;
                case Tok::KwBreakpoint:
                    errorAt(t.line, t.col, "\"breakpoint\" is not supported yet in JetScript.");
                    skip();
                    endStatement();
                    return nullptr;
                default: return parseSimpleStatement();
            }
        }

        static int assignOp(Tok t){
            switch(t){
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

        Stmt* Parser::parseSimpleStatement(){
            int line = peek().line, col = peek().col;
            Node* e = parseExpr();

            if(abort_) return nullptr;

            int op = assignOp(peek().type);
            Stmt* s;

            if(op >= 0){
                skip();
                s = newStmt(SK::Assign, line, col);
                s->op = uint8_t(op);
                s->a = e;
                s->b = parseExpr();
            } else {
                s = newStmt(SK::Expr, line, col);
                s->a = e;
            }

            endStatement();
            return s;
        }

        Stmt* Parser::parseIf(){
            Stmt* s = newStmt(SK::If, peek().line, peek().col);
            skip();
            s->a = parseExpr();
            expect(Tok::Colon, "Expected \":\" after \"if\" condition.");
            parseBlock(s->body, "\"if\"");

            while(!abort_ && check(Tok::KwElif)){
                Stmt* e = newStmt(SK::If, peek().line, peek().col);
                skip();
                e->a = parseExpr();
                expect(Tok::Colon, "Expected \":\" after \"elif\" condition.");
                parseBlock(e->body, "\"elif\"");
                s->elifs.push_back(e);
            }

            if(!abort_ && check(Tok::KwElse)){
                skip();
                expect(Tok::Colon, "Expected \":\" after \"else\".");
                parseBlock(s->orelse, "\"else\"");
            }

            lastTerminator_ = Tok::Newline;
            return s;
        }

        Stmt* Parser::parseWhile(){
            Stmt* s = newStmt(SK::While, peek().line, peek().col);
            skip();
            s->a = parseExpr();
            expect(Tok::Colon, "Expected \":\" after \"while\" condition.");
            ++loopDepth_;
            parseBlock(s->body, "\"while\"");
            --loopDepth_;
            lastTerminator_ = Tok::Newline;
            return s;
        }

        bool Parser::parseForHeader(Stmt* s){
            if(!check(Tok::Ident)){
                errorHere("Expected loop variable name after \"for\".");
                syncLine();

                if(check(Tok::Indent)) skipBlock();

                return false;
            }

            s->name = ast_.intern(peek().text);
            skip();

            if(match(Tok::Colon)) parseType(s->type, false);

            if(!expect(Tok::KwIn, "Expected \"in\" after \"for\" variable name.")){
                syncLine();

                if(check(Tok::Indent)) skipBlock();

                return false;
            }

            return true;
        }

        Stmt* Parser::parseFor(){
            Stmt* s = newStmt(SK::For, peek().line, peek().col);
            skip();

            if(!parseForHeader(s)){
                lastTerminator_ = Tok::Newline;
                return nullptr;
            }

            s->a = parseExpr();
            expect(Tok::Colon, "Expected \":\" after \"for\" condition.");
            ++loopDepth_;
            parseBlock(s->body, "\"for\"");
            --loopDepth_;
            lastTerminator_ = Tok::Newline;
            return s;
        }

        void Parser::parseMatchPattern(MatchBranch& br){
            const Token& p = peek();

            if(p.type == Tok::Ident && p.text == "_"){
                br.patterns.push_back(new_node(NK::Wildcard, p.line, p.col));
                skip();
            } else if(p.type == Tok::KwVar){
                skip();

                if(!check(Tok::Ident)){
                    errorHere("Expected bind name after \"var\".");
                    return;
                }

                const Token& name = peek();
                Node* n = new_node(NK::Binding, name.line, name.col);
                n->name = ast_.intern(name.text);
                skip();
                br.patterns.push_back(n);
            } else if(p.type == Tok::LBracket || p.type == Tok::LBrace || p.type == Tok::DotDot){
                errorHere("Array and dictionary patterns are not supported yet in JetScript.");
                br.patterns.push_back(parseExpr());
            } else {
                br.patterns.push_back(parseExpr());
            }
        }

        Stmt* Parser::parseMatch(){
            Stmt* s = newStmt(SK::Match, peek().line, peek().col);
            skip();
            s->a = parseExpr();
            expect(Tok::Colon, "Expected \":\" after \"match\" expression.");
            lastTerminator_ = Tok::Newline;

            if(!match(Tok::Newline) || !match(Tok::Indent)){
                errorHere("Expected an indented block of patterns after \"match\".");
                syncLine();
                return s;
            }

            DepthGuard g(*this);

            if(!g.ok) return s;

            ++blockDepth_;

            while(!abort_ && !check(Tok::Dedent) && !check(Tok::Eof)){
                if(match(Tok::Newline)) continue;

                if(check(Tok::Indent)){
                    errorHere("Unexpected indentation.");
                    skipBlock();
                    continue;
                }

                s->branches.emplace_back();
                MatchBranch& br = s->branches.back();
                br.line = peek().line;
                br.col = peek().col;

                for(;;){
                    parseMatchPattern(br);

                    if(!match(Tok::Comma)) break;
                }

                if(check(Tok::Ident) && peek().text == "when") errorHere("Pattern guards (\"when\") are not supported yet in JetScript.");

                if(!expect(Tok::Colon, "Expected \":\" after match patterns.")){
                    syncLine();

                    if(check(Tok::Indent)) skipBlock();

                    continue;
                }

                parseBlock(s->branches.back().body, "match pattern");
            }

            match(Tok::Dedent);
            --blockDepth_;
            lastTerminator_ = Tok::Newline;
            return s;
        }

        Node* Parser::parseExpr(){
            DepthGuard g(*this);

            if(!g.ok) return newHere(NK::Bad);

            Node* n = parseBinary(P_OR);

            if(!abort_ && (check(Tok::KwIf) || check(Tok::KwAs))) n = parseExprSuffix(n);

            return n;
        }

        // `x if cond else y` (right associative) and `x as Type`.
        Node* Parser::parseExprSuffix(Node* n){
            if(check(Tok::KwIf)){
                Node* r = newHere(NK::Ternary);
                skip();
                r->a = n;
                r->b = parseBinary(P_OR);

                if(!expect(Tok::KwElse, "Expected \"else\" after ternary operator condition.")) return n;

                r->c = parseExpr();
                finish(r);
                n = r;
            }

            while(!abort_ && check(Tok::KwAs)){
                Node* r = newHere(NK::As);
                skip();
                r->a = n;
                parseType(r->type, false);
                finish(r);
                n = r;
            }

            return n;
        }

        int Parser::binaryPrec(BinOp& op, NK& kind, int& ntok){
            const Token& t = peek();
            kind = NK::Binary;
            ntok = 1;

            switch(t.type){
                case Tok::KwOr:
                case Tok::PipePipe: kind = NK::Or; return P_OR;
                case Tok::KwAnd:
                case Tok::AmpAmp: kind = NK::And; return P_AND;
                case Tok::KwIn: op = BinOp::In; return P_IN;
                case Tok::KwNot:
                    if(peek(1).type == Tok::KwIn){
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

        Node* Parser::parseBinary(int minPrec){
            Node* left = (minPrec <= P_NOT && (check(Tok::KwNot) || check(Tok::Bang))) ? parseNot() : parseOperand();

            for(;;){
                if(abort_) return left;

                BinOp op = BinOp::Add;
                NK kind = NK::Binary;
                int ntok = 1;
                int prec = binaryPrec(op, kind, ntok);

                if(prec == P_NONE || prec < minPrec) break;

                Node* n = newHere(kind);
                skip();

                if(ntok == 2) skip();

                n->a = left;
                left = n;

                if(kind == NK::Is){
                    if(match(Tok::KwNot)) n->negate = true;

                    parseType(n->type, false);
                    finish(n);
                    continue;
                }

                n->op = uint8_t(op);
                DepthGuard g(*this);

                if(!g.ok) return left;

                n->b = parseBinary(prec + 1);
                finish(n);
            }

            return left;
        }

        Node* Parser::parseNot(){
            Node* n = newHere(NK::Not);
            skip();
            DepthGuard g(*this);

            if(!g.ok) return n;

            n->a = parseBinary(P_NOT);
            finish(n);
            return n;
        }

        // Prefix operators (- + ~ not !) are chained in a loop and the operand is attached afterwards, so they do not recurse.
        Node* Parser::parseOperand(){
            Node* head = nullptr;
            Node* tail = nullptr;
            int count = 0;

            for(;;){
                Tok t = peek().type;
                NK kind = NK::Unary;
                uint8_t op = 0;

                if(t == Tok::Minus) op = uint8_t(UnOp::Neg);
                else if(t == Tok::Plus) op = uint8_t(UnOp::Pos);
                else if(t == Tok::Tilde) op = uint8_t(UnOp::BitNot);
                else if(t == Tok::KwNot || t == Tok::Bang) kind = NK::Not;
                else break;
                Node* u = newHere(kind);
                u->op = op;
                skip();

                if(tail) tail->a = u;
                else head = u;
                tail = u;

                if(++count + depth_ > kMaxNesting){
                    depthError();
                    return head;
                }
            }

            Node* n = parsePrimary();

            for(;;){
                if(abort_) break;

                Tok t = peek().type;

                if(t == Tok::Dot) n = parseMember(n);
                else if(t == Tok::LBracket) n = parseIndex(n);
                else if(t == Tok::LParen){
                    badCall();
                    break;
                } else break;
            }

            return head ? attachPrefix(head, n) : n;
        }

        Node* Parser::attachPrefix(Node* head, Node* operand){
            std::vector<Node*> chain;

            for(Node* x = head; x; x = x->a) chain.push_back(x);

            Node* inner = operand;
            //fold `-<number literal>` into the literal
            while(!chain.empty() && inner && inner->kind == NK::Literal && inner->value.isNumber() &&
                   chain.back()->kind == NK::Unary && UnOp(chain.back()->op) == UnOp::Neg){
                std::string err;
                Value v;

                if(!unary_op(UnOp::Neg, inner->value, v, err)) break;

                inner->value = v;
                inner->line = chain.back()->line;
                inner->col = chain.back()->col;
                chain.pop_back();
            }

            for(size_t i = chain.size(); i-- > 0;){
                chain[i]->a = inner;
                finish(chain[i]);
                inner = chain[i];
            }

            return inner;
        }

        void Parser::badCall(){
            errorHere("Calling the result of an expression is not supported (callables are not supported yet in JetScript).");
            Node* d = newHere(NK::Bad);
            parse_args(d);
        }

        Node* Parser::parseMember(Node* base){
            skip();

            if(!check(Tok::Ident)){
                errorHere("Expected identifier after \".\".");
                return base;
            }

            const Token& name = peek();
            Node* m = new_node(NK::Member, name.line, name.col);
            m->a = base;
            m->name = ast_.intern(name.text);
            skip();

            if(check(Tok::LParen)){
                m->kind = NK::MethodCall;
                parse_args(m);
            }

            finish(m);
            return m;
        }

        Node* Parser::parseIndex(Node* base){
            Node* m = newHere(NK::Index);
            skip();
            m->a = base;
            m->b = parseExpr();
            expect(Tok::RBracket, "Expected closing \"]\" after subscription index.");
            finish(m);
            return m;
        }

        void Parser::parse_args(Node* call){
            skip();

            while(!abort_ && !check(Tok::RParen) && !check(Tok::Eof)){
                call->list.push_back(parseExpr());

                if(!match(Tok::Comma)) break;
            }

            if(call->list.size() > 255) errorAt(call->line, call->col, "Too many arguments (the maximum is 255).");

            expect(Tok::RParen, "Expected closing \")\" after call arguments.");
        }

        Node* Parser::parseCall(){
            const Token& k = peek();
            Node* n = new_node(NK::Call, k.line, k.col);
            n->name = ast_.intern(k.type == Tok::KwPreload ? std::string("preload") : k.text);
            skip();
            parse_args(n);
            finish(n);
            return n;
        }

        Node* Parser::parseArray(){
            Node* n = newHere(NK::ArrayLit);
            skip();

            while(!abort_ && !check(Tok::RBracket) && !check(Tok::Eof)){
                n->list.push_back(parseExpr());

                if(!match(Tok::Comma)) break;
            }

            expect(Tok::RBracket, "Expected closing \"]\" after array elements.");
            finish(n);
            return n;
        }

        Node* Parser::dictKeyShorthand(){
            const Token& kt = peek();
            Node* key = new_node(NK::Literal, kt.line, kt.col);
            key->value = Value(kt.text);
            skip();
            skip();
            return key;
        }

        Node* Parser::parseDict(){
            Node* n = newHere(NK::DictLit);
            skip();

            while(!abort_ && !check(Tok::RBrace) && !check(Tok::Eof)){
                Node* key;

                if(check(Tok::Ident) && peek(1).type == Tok::Assign){
                    key = dictKeyShorthand();
                } else {
                    key = parseExpr();

                    if(!expect(Tok::Colon, "Expected \":\" after dictionary key.")) break;
                }

                n->list.push_back(key);
                n->list.push_back(parseExpr());

                if(!match(Tok::Comma)) break;
            }

            expect(Tok::RBrace, "Expected closing \"}\" after dictionary elements.");
            finish(n);
            return n;
        }

        Node* Parser::parsePrimary(){
            switch(peek().type){
                case Tok::LParen:{
                    skip();
                    Node* n = parseExpr();
                    expect(Tok::RParen, "Expected closing \")\" after grouping expression.");
                    return n;
                }
                case Tok::LBracket: return parseArray();
                case Tok::LBrace: return parseDict();
                case Tok::Ident:
                //a keyword in GDScript, a function here: preload("res://bullet.jetscene")
                case Tok::KwPreload:
                    if(peek(1).type == Tok::LParen) return parseCall();

                    return parseLeaf();
                default: return parseLeaf();
            }
        }

        // Non-recursive primary expressions (literals, identifiers, self, $paths) and errors.
        Node* Parser::parseLeaf(){
            const Token& t = peek();
            Node* n;

            switch(t.type){
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

            switch(k.type){
                case Tok::KwFunc: msg = "Lambdas are not supported yet in JetScript."; break;
                case Tok::KwAwait: msg = "\"await\" is not supported yet in JetScript."; break;
                case Tok::KwYield: msg = "\"yield\" is not supported in JetScript."; break;
                case Tok::KwPreload: msg = "\"preload\" needs a path: preload(\"res://scene.jetscene\")."; break;
                case Tok::KwSuper: msg = "\"super\" is not supported yet in JetScript."; break;
                case Tok::KwSignal:
                case Tok::KwClass:
                case Tok::KwClassName:
                case Tok::KwStatic:
                case Tok::KwBreakpoint:
                    errorAt(k.line, k.col, std::string("\"") + tokName(k.type) + "\" is not supported yet in JetScript.");
                    skip();
                    return new_node(NK::Bad, k.line, k.col);
                default: break;
            }

            if(msg){
                errorAt(k.line, k.col, msg);
                skip();
                return new_node(NK::Bad, k.line, k.col);
            }

            errorAt(k.line, k.col, "Expected expression, found " + describe(k) + " instead.");

            if(k.type != Tok::Newline && k.type != Tok::Eof && k.type != Tok::Indent && k.type != Tok::Dedent &&
                k.type != Tok::Semicolon && k.type != Tok::Colon && k.type != Tok::RParen && k.type != Tok::RBracket &&
                k.type != Tok::RBrace && k.type != Tok::Comma)
                skip();
            return new_node(NK::Bad, k.line, k.col);
        }

    }
}
