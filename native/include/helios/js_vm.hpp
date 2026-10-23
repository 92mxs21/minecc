#pragma once
// HeliosJS: Zero-Dependency ECMAScript Lexer, Recursive-Descent AST Parser, Closure Interpreter & Live DOM Bridge

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "native/include/helios/css_engine.hpp"
#include "native/include/helios/html_dom.hpp"

namespace helios::js {

struct JsValue;
struct JsObject;
struct JsArray;
struct JsFunction;
class JsRuntime;

using NativeFn = std::function<JsValue(JsRuntime&, const JsValue& this_val, const std::vector<JsValue>& args)>;

enum class ValKind : uint8_t {
    Undefined = 0,
    Null = 1,
    Bool = 2,
    Number = 3,
    String = 4,
    Object = 5,
    Array = 6,
    Function = 7,
    DomElement = 8,
};

struct JsValue {
    ValKind kind = ValKind::Undefined;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::shared_ptr<JsObject> obj;
    std::shared_ptr<JsArray> arr;
    std::shared_ptr<JsFunction> fn;
    std::shared_ptr<DomNode> dom;

    static JsValue undef() { return JsValue{}; }
    static JsValue null_v() { JsValue v; v.kind = ValKind::Null; return v; }
    static JsValue boolean(bool x) { JsValue v; v.kind = ValKind::Bool; v.b = x; return v; }
    static JsValue number(double x) { JsValue v; v.kind = ValKind::Number; v.num = x; return v; }
    static JsValue string(std::string s) { JsValue v; v.kind = ValKind::String; v.str = std::move(s); return v; }
    static JsValue element(std::shared_ptr<DomNode> d) {
        if (!d) return null_v();
        JsValue v; v.kind = ValKind::DomElement; v.dom = std::move(d); return v;
    }

    bool truthy() const {
        switch (kind) {
            case ValKind::Undefined:
            case ValKind::Null: return false;
            case ValKind::Bool: return b;
            case ValKind::Number: return num != 0.0 && !std::isnan(num);
            case ValKind::String: return !str.empty();
            default: return true;
        }
    }

    double to_num() const {
        if (kind == ValKind::Number) return num;
        if (kind == ValKind::Bool) return b ? 1.0 : 0.0;
        if (kind == ValKind::String) return str.empty() ? 0.0 : std::strtod(str.c_str(), nullptr);
        return 0.0;
    }

    std::string to_str() const {
        switch (kind) {
            case ValKind::Undefined: return "undefined";
            case ValKind::Null: return "null";
            case ValKind::Bool: return b ? "true" : "false";
            case ValKind::Number: {
                if (std::isnan(num)) return "NaN";
                double ip = 0.0;
                if (std::modf(num, &ip) == 0.0 && std::fabs(num) < 9e15) {
                    return std::to_string(static_cast<long long>(ip));
                }
                std::ostringstream oss;
                oss << num;
                return oss.str();
            }
            case ValKind::String: return str;
            case ValKind::Array: return "[Array]";
            case ValKind::Object: return "[object Object]";
            case ValKind::Function: return "[Function]";
            case ValKind::DomElement: return dom ? ("<" + dom->tag + ">") : "null";
        }
        return "";
    }
};

struct JsObject {
    std::unordered_map<std::string, JsValue> props;
};

struct JsArray {
    std::vector<JsValue> items;
};

inline JsValue make_object() {
    JsValue v;
    v.kind = ValKind::Object;
    v.obj = std::make_shared<JsObject>();
    return v;
}

inline JsValue make_array(std::vector<JsValue> items = {}) {
    JsValue v;
    v.kind = ValKind::Array;
    v.arr = std::make_shared<JsArray>();
    v.arr->items = std::move(items);
    return v;
}

// ============================================================================
// Lexer
// ============================================================================
enum class TokKind : uint8_t {
    Eof = 0, Ident, Number, String, TemplateStr,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semi, Colon, Dot, Question, Arrow,
    Plus, Minus, Star, Slash, Percent,
    PlusPlus, MinusMinus, PlusEq, MinusEq,
    Assign, Eq, StrictEq, Neq, StrictNeq,
    Lt, Le, Gt, Ge, And, Or, Nullish, Bang,
    KwLet, KwConst, KwVar, KwFunction, KwReturn,
    KwIf, KwElse, KwFor, KwOf, KwIn, KwWhile,
    KwBreak, KwContinue, KwTrue, KwFalse, KwNull, KwUndefined, KwTypeof
};

struct Token {
    TokKind kind = TokKind::Eof;
    std::string text;
    double num = 0.0;
    int line = 1;
};

inline std::vector<Token> tokenize_js(std::string_view src) {
    std::vector<Token> out;
    size_t i = 0;
    const size_t n = src.size();
    int line = 1;

    // Helper: expand template literal `` `hello ${expr} world` `` directly into `("hello " + (expr) + " world")`!
    auto emit_template = [&](size_t& pos) {
        ++pos; // skip opening `
        out.push_back({TokKind::LParen, "(", 0.0, line});
        out.push_back({TokKind::String, "", 0.0, line});
        std::string buf;
        while (pos < n && src[pos] != '`') {
            if (src[pos] == '\n') ++line;
            if (src[pos] == '\\' && pos + 1 < n) {
                char nx = src[pos + 1];
                if (nx == 'n') buf.push_back('\n');
                else if (nx == 't') buf.push_back('\t');
                else buf.push_back(nx);
                pos += 2;
                continue;
            }
            if (src[pos] == '$' && pos + 1 < n && src[pos + 1] == '{') {
                if (!buf.empty()) {
                    out.push_back({TokKind::Plus, "+", 0.0, line});
                    out.push_back({TokKind::String, buf, 0.0, line});
                    buf.clear();
                }
                pos += 2;
                int brace_depth = 1;
                size_t expr_start = pos;
                while (pos < n && brace_depth > 0) {
                    if (src[pos] == '\n') ++line;
                    if (src[pos] == '\'' || src[pos] == '"') {
                        char q = src[pos++];
                        while (pos < n && src[pos] != q) {
                            if (src[pos] == '\\') ++pos;
                            ++pos;
                        }
                    } else if (src[pos] == '{') ++brace_depth;
                    else if (src[pos] == '}') {
                        --brace_depth;
                        if (brace_depth == 0) break;
                    }
                    ++pos;
                }
                std::string_view inner = src.substr(expr_start, pos - expr_start);
                if (pos < n && src[pos] == '}') ++pos;
                out.push_back({TokKind::Plus, "+", 0.0, line});
                out.push_back({TokKind::LParen, "(", 0.0, line});
                auto sub = tokenize_js(inner);
                for (auto& st : sub) {
                    if (st.kind != TokKind::Eof) out.push_back(std::move(st));
                }
                out.push_back({TokKind::RParen, ")", 0.0, line});
                continue;
            }
            buf.push_back(src[pos++]);
        }
        if (pos < n && src[pos] == '`') ++pos;
        if (!buf.empty()) {
            out.push_back({TokKind::Plus, "+", 0.0, line});
            out.push_back({TokKind::String, buf, 0.0, line});
        }
        out.push_back({TokKind::RParen, ")", 0.0, line});
    };

    while (i < n) {
        char c = src[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') ++line;
                ++i;
            }
            i += 2;
            continue;
        }
        if (c == '`') {
            emit_template(i);
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = src[i++];
            std::string s;
            while (i < n && src[i] != q) {
                if (src[i] == '\\' && i + 1 < n) {
                    char nx = src[i + 1];
                    if (nx == 'n') s.push_back('\n');
                    else if (nx == 'r') s.push_back('\r');
                    else if (nx == 't') s.push_back('\t');
                    else s.push_back(nx);
                    i += 2;
                } else {
                    if (src[i] == '\n') ++line;
                    s.push_back(src[i++]);
                }
            }
            if (i < n) ++i;
            out.push_back({TokKind::String, std::move(s), 0.0, line});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i;
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                while (i < n && std::isxdigit(static_cast<unsigned char>(src[i]))) ++i;
                double hv = static_cast<double>(std::strtoull(std::string(src.substr(start, i - start)).c_str(), nullptr, 16));
                out.push_back({TokKind::Number, std::string(src.substr(start, i - start)), hv, line});
                continue;
            }
            while (i < n && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.')) ++i;
            double v = std::strtod(std::string(src.substr(start, i - start)).c_str(), nullptr);
            out.push_back({TokKind::Number, std::string(src.substr(start, i - start)), v, line});
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            size_t start = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_' || src[i] == '$')) ++i;
            std::string id(src.substr(start, i - start));
            TokKind k = TokKind::Ident;
            if (id == "let") k = TokKind::KwLet;
            else if (id == "const") k = TokKind::KwConst;
            else if (id == "var") k = TokKind::KwVar;
            else if (id == "function") k = TokKind::KwFunction;
            else if (id == "return") k = TokKind::KwReturn;
            else if (id == "if") k = TokKind::KwIf;
            else if (id == "else") k = TokKind::KwElse;
            else if (id == "for") k = TokKind::KwFor;
            else if (id == "of") k = TokKind::KwOf;
            else if (id == "in") k = TokKind::KwIn;
            else if (id == "while") k = TokKind::KwWhile;
            else if (id == "break") k = TokKind::KwBreak;
            else if (id == "continue") k = TokKind::KwContinue;
            else if (id == "true") k = TokKind::KwTrue;
            else if (id == "false") k = TokKind::KwFalse;
            else if (id == "null") k = TokKind::KwNull;
            else if (id == "undefined") k = TokKind::KwUndefined;
            else if (id == "typeof") k = TokKind::KwTypeof;
            out.push_back({k, std::move(id), 0.0, line});
            continue;
        }

        // Operators & punctuation
        std::string_view rem = src.substr(i);
        if (rem.substr(0, 3) == "===") { out.push_back({TokKind::StrictEq, "===", 0, line}); i += 3; }
        else if (rem.substr(0, 3) == "!==") { out.push_back({TokKind::StrictNeq, "!==", 0, line}); i += 3; }
        else if (rem.substr(0, 2) == "=>") { out.push_back({TokKind::Arrow, "=>", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "==") { out.push_back({TokKind::Eq, "==", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "!=") { out.push_back({TokKind::Neq, "!=", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "<=") { out.push_back({TokKind::Le, "<=", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == ">=") { out.push_back({TokKind::Ge, ">=", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "&&") { out.push_back({TokKind::And, "&&", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "||") { out.push_back({TokKind::Or, "||", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "??") { out.push_back({TokKind::Nullish, "??", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "++") { out.push_back({TokKind::PlusPlus, "++", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "--") { out.push_back({TokKind::MinusMinus, "--", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "+=") { out.push_back({TokKind::PlusEq, "+=", 0, line}); i += 2; }
        else if (rem.substr(0, 2) == "-=") { out.push_back({TokKind::MinusEq, "-=", 0, line}); i += 2; }
        else {
            ++i;
            switch (c) {
                case '(': out.push_back({TokKind::LParen, "(", 0, line}); break;
                case ')': out.push_back({TokKind::RParen, ")", 0, line}); break;
                case '{': out.push_back({TokKind::LBrace, "{", 0, line}); break;
                case '}': out.push_back({TokKind::RBrace, "}", 0, line}); break;
                case '[': out.push_back({TokKind::LBracket, "[", 0, line}); break;
                case ']': out.push_back({TokKind::RBracket, "]", 0, line}); break;
                case ',': out.push_back({TokKind::Comma, ",", 0, line}); break;
                case ';': out.push_back({TokKind::Semi, ";", 0, line}); break;
                case ':': out.push_back({TokKind::Colon, ":", 0, line}); break;
                case '.': out.push_back({TokKind::Dot, ".", 0, line}); break;
                case '?': out.push_back({TokKind::Question, "?", 0, line}); break;
                case '+': out.push_back({TokKind::Plus, "+", 0, line}); break;
                case '-': out.push_back({TokKind::Minus, "-", 0, line}); break;
                case '*': out.push_back({TokKind::Star, "*", 0, line}); break;
                case '/': out.push_back({TokKind::Slash, "/", 0, line}); break;
                case '%': out.push_back({TokKind::Percent, "%", 0, line}); break;
                case '=': out.push_back({TokKind::Assign, "=", 0, line}); break;
                case '<': out.push_back({TokKind::Lt, "<", 0, line}); break;
                case '>': out.push_back({TokKind::Gt, ">", 0, line}); break;
                case '!': out.push_back({TokKind::Bang, "!", 0, line}); break;
                default: break;
            }
        }
    }
    out.push_back({TokKind::Eof, "", 0.0, line});
    return out;
}

// ============================================================================
// AST Nodes & Parser
// ============================================================================
struct Stmt;
struct Expr;
using StmtPtr = std::shared_ptr<Stmt>;
using ExprPtr = std::shared_ptr<Expr>;

enum class ExprKind : uint8_t {
    Literal, Ident, ArrayLit, ObjectLit, Unary, Binary, Ternary,
    Assign, Member, Index, Call, FnExpr
};

struct Expr {
    ExprKind kind = ExprKind::Literal;
    JsValue lit;
    std::string op;
    std::string ident;
    ExprPtr a, b, c;
    std::vector<ExprPtr> list;
    std::vector<std::pair<std::string, ExprPtr>> kv;
    std::vector<std::string> params;
    StmtPtr fn_body;
};

enum class StmtKind : uint8_t {
    Block, VarDecl, ExprStmt, If, While, ForC, ForOf, FnDecl, Return, Break, Continue
};

struct Stmt {
    StmtKind kind = StmtKind::Block;
    std::string name;
    std::vector<std::string> params;
    ExprPtr init_expr, cond_expr, step_expr;
    StmtPtr init_stmt, then_branch, else_branch;
    std::vector<StmtPtr> list;
};

class Parser {
public:
    std::vector<Token> toks;
    size_t pos = 0;

    explicit Parser(std::vector<Token> t) : toks(std::move(t)) {}
    const Token& peek(size_t off = 0) const {
        return (pos + off < toks.size()) ? toks[pos + off] : toks.back();
    }
    Token advance() {
        Token t = peek();
        if (pos + 1 < toks.size()) ++pos;
        return t;
    }
    bool match(TokKind k) {
        if (peek().kind == k) { advance(); return true; }
        return false;
    }
    Token expect(TokKind k, const char* msg) {
        if (peek().kind != k) {
            throw std::runtime_error("JS SyntaxError line " + std::to_string(peek().line) + ": expected " + msg + ", got '" + peek().text + "'");
        }
        return advance();
    }

    StmtPtr parse_program() {
        auto root = std::make_shared<Stmt>();
        root->kind = StmtKind::Block;
        while (peek().kind != TokKind::Eof) {
            if (match(TokKind::Semi)) continue;
            root->list.push_back(parse_stmt());
        }
        return root;
    }

    StmtPtr parse_block() {
        expect(TokKind::LBrace, "{");
        auto s = std::make_shared<Stmt>();
        s->kind = StmtKind::Block;
        while (peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
            if (match(TokKind::Semi)) continue;
            s->list.push_back(parse_stmt());
        }
        expect(TokKind::RBrace, "}");
        return s;
    }

    StmtPtr parse_var_decl(bool consume_semi = true) {
        advance(); // let/const/var
        auto block = std::make_shared<Stmt>();
        block->kind = StmtKind::Block;
        do {
            std::string nm = expect(TokKind::Ident, "identifier").text;
            auto decl = std::make_shared<Stmt>();
            decl->kind = StmtKind::VarDecl;
            decl->name = nm;
            if (match(TokKind::Assign)) decl->init_expr = parse_expr();
            block->list.push_back(decl);
        } while (match(TokKind::Comma));
        if (consume_semi) match(TokKind::Semi);
        return block->list.size() == 1 ? block->list[0] : block;
    }

    StmtPtr parse_stmt() {
        TokKind k = peek().kind;
        if (k == TokKind::LBrace) return parse_block();
        if (k == TokKind::KwLet || k == TokKind::KwConst || k == TokKind::KwVar) return parse_var_decl(true);
        if (k == TokKind::KwFunction) {
            advance();
            auto s = std::make_shared<Stmt>();
            s->kind = StmtKind::FnDecl;
            s->name = expect(TokKind::Ident, "function name").text;
            expect(TokKind::LParen, "(");
            if (peek().kind != TokKind::RParen) {
                do { s->params.push_back(expect(TokKind::Ident, "parameter").text); } while (match(TokKind::Comma));
            }
            expect(TokKind::RParen, ")");
            s->then_branch = parse_block();
            return s;
        }
        if (k == TokKind::KwIf) {
            advance();
            expect(TokKind::LParen, "(");
            auto s = std::make_shared<Stmt>();
            s->kind = StmtKind::If;
            s->cond_expr = parse_expr();
            expect(TokKind::RParen, ")");
            s->then_branch = parse_stmt();
            if (match(TokKind::KwElse)) s->else_branch = parse_stmt();
            return s;
        }
        if (k == TokKind::KwWhile) {
            advance();
            expect(TokKind::LParen, "(");
            auto s = std::make_shared<Stmt>();
            s->kind = StmtKind::While;
            s->cond_expr = parse_expr();
            expect(TokKind::RParen, ")");
            s->then_branch = parse_stmt();
            return s;
        }
        if (k == TokKind::KwFor) {
            advance();
            expect(TokKind::LParen, "(");
            // Check `for (const/let x of expr)`
            if ((peek().kind == TokKind::KwConst || peek().kind == TokKind::KwLet || peek().kind == TokKind::KwVar) &&
                peek(1).kind == TokKind::Ident && peek(2).kind == TokKind::KwOf)
            {
                advance();
                std::string var_nm = advance().text;
                advance(); // of
                auto s = std::make_shared<Stmt>();
                s->kind = StmtKind::ForOf;
                s->name = var_nm;
                s->init_expr = parse_expr();
                expect(TokKind::RParen, ")");
                s->then_branch = parse_stmt();
                return s;
            }
            auto s = std::make_shared<Stmt>();
            s->kind = StmtKind::ForC;
            if (!match(TokKind::Semi)) {
                if (peek().kind == TokKind::KwLet || peek().kind == TokKind::KwConst || peek().kind == TokKind::KwVar) {
                    s->init_stmt = parse_var_decl(true);
                } else {
                    auto es = std::make_shared<Stmt>();
                    es->kind = StmtKind::ExprStmt;
                    es->init_expr = parse_expr();
                    expect(TokKind::Semi, ";");
                    s->init_stmt = es;
                }
            }
            if (!match(TokKind::Semi)) {
                s->cond_expr = parse_expr();
                expect(TokKind::Semi, ";");
            }
            if (!match(TokKind::RParen)) {
                s->step_expr = parse_expr();
                expect(TokKind::RParen, ")");
            }
            s->then_branch = parse_stmt();
            return s;
        }
        if (k == TokKind::KwReturn) {
            advance();
            auto s = std::make_shared<Stmt>();
            s->kind = StmtKind::Return;
            if (peek().kind != TokKind::Semi && peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
                s->init_expr = parse_expr();
            }
            match(TokKind::Semi);
            return s;
        }
        if (k == TokKind::KwBreak) {
            advance(); match(TokKind::Semi);
            auto s = std::make_shared<Stmt>(); s->kind = StmtKind::Break; return s;
        }
        if (k == TokKind::KwContinue) {
            advance(); match(TokKind::Semi);
            auto s = std::make_shared<Stmt>(); s->kind = StmtKind::Continue; return s;
        }

        auto s = std::make_shared<Stmt>();
        s->kind = StmtKind::ExprStmt;
        s->init_expr = parse_expr();
        match(TokKind::Semi);
        return s;
    }

    bool is_arrow_ahead() const {
        if (peek().kind == TokKind::Ident && peek(1).kind == TokKind::Arrow) return true;
        if (peek().kind == TokKind::LParen) {
            size_t j = pos + 1;
            int depth = 1;
            while (j < toks.size() && depth > 0) {
                if (toks[j].kind == TokKind::LParen) ++depth;
                else if (toks[j].kind == TokKind::RParen) --depth;
                ++j;
            }
            return j < toks.size() && toks[j].kind == TokKind::Arrow;
        }
        return false;
    }

    ExprPtr parse_arrow() {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::FnExpr;
        if (peek().kind == TokKind::Ident) {
            e->params.push_back(advance().text);
        } else {
            expect(TokKind::LParen, "(");
            if (peek().kind != TokKind::RParen) {
                do { e->params.push_back(expect(TokKind::Ident, "arrow param").text); } while (match(TokKind::Comma));
            }
            expect(TokKind::RParen, ")");
        }
        expect(TokKind::Arrow, "=>");
        if (peek().kind == TokKind::LBrace) {
            e->fn_body = parse_block();
        } else {
            auto ret = std::make_shared<Stmt>();
            ret->kind = StmtKind::Return;
            ret->init_expr = parse_assign();
            e->fn_body = ret;
        }
        return e;
    }

    ExprPtr parse_expr() { return parse_assign(); }

    ExprPtr parse_assign() {
        if (is_arrow_ahead()) return parse_arrow();
        ExprPtr lhs = parse_ternary();
        TokKind k = peek().kind;
        if (k == TokKind::Assign || k == TokKind::PlusEq || k == TokKind::MinusEq) {
            std::string op = advance().text;
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Assign;
            e->op = op;
            e->a = lhs;
            e->b = parse_assign();
            return e;
        }
        return lhs;
    }

    ExprPtr parse_ternary() {
        ExprPtr cond = parse_or();
        if (match(TokKind::Question)) {
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Ternary;
            e->a = cond;
            e->b = parse_assign();
            expect(TokKind::Colon, ":");
            e->c = parse_assign();
            return e;
        }
        return cond;
    }

    ExprPtr parse_or() {
        ExprPtr lhs = parse_and();
        while (peek().kind == TokKind::Or || peek().kind == TokKind::Nullish) {
            std::string op = advance().text;
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Binary; e->op = op; e->a = lhs; e->b = parse_and();
            lhs = e;
        }
        return lhs;
    }

    ExprPtr parse_and() {
        ExprPtr lhs = parse_eq();
        while (peek().kind == TokKind::And) {
            std::string op = advance().text;
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Binary; e->op = op; e->a = lhs; e->b = parse_eq();
            lhs = e;
        }
        return lhs;
    }

    ExprPtr parse_eq() {
        ExprPtr lhs = parse_rel();
        while (peek().kind == TokKind::Eq || peek().kind == TokKind::StrictEq ||
               peek().kind == TokKind::Neq || peek().kind == TokKind::StrictNeq)
        {
            std::string op = advance().text;
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Binary; e->op = op; e->a = lhs; e->b = parse_rel();
            lhs = e;
        }
        return lhs;
    }

    ExprPtr parse_rel() {
        ExprPtr lhs = parse_add();
        while (peek().kind == TokKind::Lt || peek().kind == TokKind::Le ||
               peek().kind == TokKind::Gt || peek().kind == TokKind::Ge)
        {
            std::string op = advance().text;
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Binary; e->op = op; e->a = lhs; e->b = parse_add();
            lhs = e;
        }
        return lhs;
    }

    ExprPtr parse_add() {
        ExprPtr lhs = parse_mul();
        while (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus) {
            std::string op = advance().text;
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Binary; e->op = op; e->a = lhs; e->b = parse_mul();
            lhs = e;
        }
        return lhs;
    }

    ExprPtr parse_mul() {
        ExprPtr lhs = parse_unary();
        while (peek().kind == TokKind::Star || peek().kind == TokKind::Slash || peek().kind == TokKind::Percent) {
            std::string op = advance().text;
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Binary; e->op = op; e->a = lhs; e->b = parse_unary();
            lhs = e;
        }
        return lhs;
    }

    ExprPtr parse_unary() {
        TokKind k = peek().kind;
        if (k == TokKind::Bang || k == TokKind::Minus || k == TokKind::Plus ||
            k == TokKind::KwTypeof || k == TokKind::PlusPlus || k == TokKind::MinusMinus)
        {
            std::string op = advance().text;
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Unary; e->op = op; e->a = parse_unary();
            return e;
        }
        return parse_postfix();
    }

    ExprPtr parse_postfix() {
        ExprPtr cur = parse_primary();
        while (true) {
            if (match(TokKind::Dot)) {
                auto e = std::make_shared<Expr>();
                e->kind = ExprKind::Member;
                e->a = cur;
                e->ident = advance().text;
                cur = e;
            } else if (match(TokKind::LBracket)) {
                auto e = std::make_shared<Expr>();
                e->kind = ExprKind::Index;
                e->a = cur;
                e->b = parse_expr();
                expect(TokKind::RBracket, "]");
                cur = e;
            } else if (match(TokKind::LParen)) {
                auto e = std::make_shared<Expr>();
                e->kind = ExprKind::Call;
                e->a = cur;
                if (peek().kind != TokKind::RParen) {
                    do { e->list.push_back(parse_assign()); } while (match(TokKind::Comma));
                }
                expect(TokKind::RParen, ")");
                cur = e;
            } else if (peek().kind == TokKind::PlusPlus || peek().kind == TokKind::MinusMinus) {
                std::string op = advance().text;
                auto e = std::make_shared<Expr>();
                e->kind = ExprKind::Unary;
                e->op = "post" + op;
                e->a = cur;
                cur = e;
            } else {
                break;
            }
        }
        return cur;
    }

    ExprPtr parse_primary() {
        TokKind k = peek().kind;
        if (k == TokKind::Number) {
            auto e = std::make_shared<Expr>(); e->lit = JsValue::number(advance().num); return e;
        }
        if (k == TokKind::String) {
            auto e = std::make_shared<Expr>(); e->lit = JsValue::string(advance().text); return e;
        }
        if (k == TokKind::KwTrue) {
            advance(); auto e = std::make_shared<Expr>(); e->lit = JsValue::boolean(true); return e;
        }
        if (k == TokKind::KwFalse) {
            advance(); auto e = std::make_shared<Expr>(); e->lit = JsValue::boolean(false); return e;
        }
        if (k == TokKind::KwNull) {
            advance(); auto e = std::make_shared<Expr>(); e->lit = JsValue::null_v(); return e;
        }
        if (k == TokKind::KwUndefined) {
            advance(); auto e = std::make_shared<Expr>(); e->lit = JsValue::undef(); return e;
        }
        if (k == TokKind::KwFunction) {
            advance();
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::FnExpr;
            if (peek().kind == TokKind::Ident) e->ident = advance().text;
            expect(TokKind::LParen, "(");
            if (peek().kind != TokKind::RParen) {
                do { e->params.push_back(expect(TokKind::Ident, "param").text); } while (match(TokKind::Comma));
            }
            expect(TokKind::RParen, ")");
            e->fn_body = parse_block();
            return e;
        }
        if (k == TokKind::LBracket) {
            advance();
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::ArrayLit;
            while (peek().kind != TokKind::RBracket && peek().kind != TokKind::Eof) {
                e->list.push_back(parse_assign());
                if (!match(TokKind::Comma)) break;
            }
            expect(TokKind::RBracket, "]");
            return e;
        }
        if (k == TokKind::LBrace) {
            advance();
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::ObjectLit;
            while (peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
                Token kt = advance();
                std::string key = kt.text;
                ExprPtr val;
                if (match(TokKind::Colon)) {
                    val = parse_assign();
                } else {
                    auto id = std::make_shared<Expr>();
                    id->kind = ExprKind::Ident;
                    id->ident = key;
                    val = id;
                }
                e->kv.emplace_back(std::move(key), std::move(val));
                if (!match(TokKind::Comma)) break;
            }
            expect(TokKind::RBrace, "}");
            return e;
        }
        if (match(TokKind::LParen)) {
            ExprPtr inner = parse_expr();
            expect(TokKind::RParen, ")");
            return inner;
        }
        if (k == TokKind::Ident) {
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::Ident;
            e->ident = advance().text;
            return e;
        }
        throw std::runtime_error("JS SyntaxError line " + std::to_string(peek().line) + ": unexpected token '" + peek().text + "'");
    }
};

// ============================================================================
// Lexical Scope, Functions & Live Runtime
// ============================================================================
struct Env {
    std::shared_ptr<Env> parent;
    std::unordered_map<std::string, JsValue> vars;

    explicit Env(std::shared_ptr<Env> p = nullptr) : parent(std::move(p)) {}

    void define(const std::string& k, JsValue v) { vars[k] = std::move(v); }

    JsValue get(const std::string& k) const {
        auto it = vars.find(k);
        if (it != vars.end()) return it->second;
        if (parent) return parent->get(k);
        return JsValue::undef();
    }

    bool set(const std::string& k, JsValue v) {
        auto it = vars.find(k);
        if (it != vars.end()) { it->second = std::move(v); return true; }
        if (parent) return parent->set(k, std::move(v));
        vars[k] = std::move(v);
        return true;
    }
};

struct JsFunction {
    bool is_native = false;
    NativeFn native;
    std::vector<std::string> params;
    StmtPtr body;
    std::shared_ptr<Env> closure;
};

inline JsValue make_native_fn(NativeFn fn) {
    JsValue v;
    v.kind = ValKind::Function;
    v.fn = std::make_shared<JsFunction>();
    v.fn->is_native = true;
    v.fn->native = std::move(fn);
    return v;
}

struct ExecSignal {
    enum Type { None, Return, Break, Continue } type = None;
    JsValue val;
};

struct DomListener {
    uint32_t node_uid = 0;
    std::string event_type;
    JsValue callback;
};

class JsRuntime {
public:
    std::shared_ptr<DomNode> doc_root;
    uint32_t next_uid = 1000;
    bool dom_dirty = true;
    std::shared_ptr<Env> global_env;
    std::vector<DomListener> listeners;

    explicit JsRuntime(std::shared_ptr<DomNode> root = nullptr, uint32_t start_uid = 1000)
        : doc_root(std::move(root)), next_uid(start_uid), global_env(std::make_shared<Env>())
    {
        install_builtins();
    }

    std::shared_ptr<DomNode> html_element() const {
        if (!doc_root) return nullptr;
        for (const auto& c : doc_root->children) {
            if (c->tag == "html") return c;
        }
        return doc_root;
    }

    void install_builtins() {
        // console.log
        auto console_obj = make_object();
        console_obj.obj->props["log"] = make_native_fn([](JsRuntime&, const JsValue&, const std::vector<JsValue>& args) {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) std::cerr << ' ';
                std::cerr << args[i].to_str();
            }
            std::cerr << '\n';
            return JsValue::undef();
        });
        global_env->define("console", console_obj);

        // Math
        auto math_obj = make_object();
        math_obj.obj->props["max"] = make_native_fn([](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
            return JsValue::number(a.size() >= 2 ? std::max(a[0].to_num(), a[1].to_num()) : 0.0);
        });
        math_obj.obj->props["min"] = make_native_fn([](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
            return JsValue::number(a.size() >= 2 ? std::min(a[0].to_num(), a[1].to_num()) : 0.0);
        });
        math_obj.obj->props["floor"] = make_native_fn([](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
            return JsValue::number(a.empty() ? 0.0 : std::floor(a[0].to_num()));
        });
        math_obj.obj->props["round"] = make_native_fn([](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
            return JsValue::number(a.empty() ? 0.0 : std::round(a[0].to_num()));
        });
        global_env->define("Math", math_obj);

        global_env->define("parseInt", make_native_fn([](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
            if (a.empty()) return JsValue::number(0.0);
            int base = a.size() >= 2 ? static_cast<int>(a[1].to_num()) : 10;
            return JsValue::number(static_cast<double>(std::strtoll(a[0].to_str().c_str(), nullptr, base)));
        }));

        // document object
        auto doc_obj = make_object();
        doc_obj.obj->props["getElementById"] = make_native_fn([](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
            if (a.empty() || !rt.doc_root) return JsValue::null_v();
            return JsValue::element(rt.doc_root->get_element_by_id(a[0].to_str()));
        });
        doc_obj.obj->props["documentElement"] = JsValue::element(html_element());
        doc_obj.obj->props["addEventListener"] = make_native_fn([](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
            if (a.size() >= 2 && a[1].kind == ValKind::Function && rt.doc_root) {
                rt.listeners.push_back({rt.doc_root->uid, a[0].to_str(), a[1]});
            }
            return JsValue::undef();
        });
        global_env->define("document", doc_obj);
        global_env->define("window", global_env->vars["document"]);
    }

    void refresh_document_refs() {
        auto it = global_env->vars.find("document");
        if (it != global_env->vars.end() && it->second.obj) {
            it->second.obj->props["documentElement"] = JsValue::element(html_element());
        }
    }

    JsValue get_member(const JsValue& target, const std::string& prop) {
        if (target.kind == ValKind::Object && target.obj) {
            auto it = target.obj->props.find(prop);
            return it == target.obj->props.end() ? JsValue::undef() : it->second;
        }
        if (target.kind == ValKind::Array && target.arr) {
            if (prop == "length") return JsValue::number(static_cast<double>(target.arr->items.size()));
            if (prop == "push") {
                return make_native_fn([arr = target.arr](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    for (const auto& item : a) arr->items.push_back(item);
                    return JsValue::number(static_cast<double>(arr->items.size()));
                });
            }
            if (prop == "join") {
                return make_native_fn([arr = target.arr](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    std::string sep = a.empty() ? "," : a[0].to_str();
                    std::string out;
                    for (size_t i = 0; i < arr->items.size(); ++i) {
                        if (i) out += sep;
                        out += arr->items[i].to_str();
                    }
                    return JsValue::string(std::move(out));
                });
            }
            if (prop == "map") {
                return make_native_fn([arr = target.arr](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
                    std::vector<JsValue> res;
                    if (!a.empty()) {
                        for (size_t i = 0; i < arr->items.size(); ++i) {
                            res.push_back(rt.call_fn(a[0], JsValue::undef(), {arr->items[i], JsValue::number(static_cast<double>(i))}));
                        }
                    }
                    return make_array(std::move(res));
                });
            }
            if (prop == "filter") {
                return make_native_fn([arr = target.arr](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
                    std::vector<JsValue> res;
                    if (!a.empty()) {
                        for (size_t i = 0; i < arr->items.size(); ++i) {
                            if (rt.call_fn(a[0], JsValue::undef(), {arr->items[i], JsValue::number(static_cast<double>(i))}).truthy()) {
                                res.push_back(arr->items[i]);
                            }
                        }
                    }
                    return make_array(std::move(res));
                });
            }
            if (prop == "forEach") {
                return make_native_fn([arr = target.arr](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
                    if (!a.empty()) {
                        for (size_t i = 0; i < arr->items.size(); ++i) {
                            rt.call_fn(a[0], JsValue::undef(), {arr->items[i], JsValue::number(static_cast<double>(i))});
                        }
                    }
                    return JsValue::undef();
                });
            }
            if (prop == "slice") {
                return make_native_fn([arr = target.arr](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    int n = static_cast<int>(arr->items.size());
                    int s = a.empty() ? 0 : static_cast<int>(a[0].to_num());
                    int e = a.size() >= 2 ? static_cast<int>(a[1].to_num()) : n;
                    if (s < 0) s = std::max(0, n + s);
                    if (e < 0) e = std::max(0, n + e);
                    s = std::clamp(s, 0, n);
                    e = std::clamp(e, s, n);
                    return make_array(std::vector<JsValue>(arr->items.begin() + s, arr->items.begin() + e));
                });
            }
            if (prop == "includes") {
                return make_native_fn([arr = target.arr](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.empty()) return JsValue::boolean(false);
                    std::string needle = a[0].to_str();
                    for (const auto& item : arr->items) {
                        if (item.to_str() == needle && item.kind == a[0].kind) return JsValue::boolean(true);
                    }
                    return JsValue::boolean(false);
                });
            }
        }
        if (target.kind == ValKind::String) {
            const std::string s = target.str;
            if (prop == "length") return JsValue::number(static_cast<double>(s.size()));
            if (prop == "toLowerCase") {
                return make_native_fn([s](JsRuntime&, const JsValue&, const std::vector<JsValue>&) {
                    return JsValue::string(detail::to_lower_ascii(s));
                });
            }
            if (prop == "toUpperCase") {
                return make_native_fn([s](JsRuntime&, const JsValue&, const std::vector<JsValue>&) {
                    std::string u = s;
                    for (char& c : u) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                    return JsValue::string(std::move(u));
                });
            }
            if (prop == "trim") {
                return make_native_fn([s](JsRuntime&, const JsValue&, const std::vector<JsValue>&) {
                    return JsValue::string(css_detail::trim(s));
                });
            }
            if (prop == "includes") {
                return make_native_fn([s](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    return JsValue::boolean(!a.empty() && s.find(a[0].to_str()) != std::string::npos);
                });
            }
            if (prop == "indexOf") {
                return make_native_fn([s](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.empty()) return JsValue::number(-1);
                    size_t p = s.find(a[0].to_str());
                    return JsValue::number(p == std::string::npos ? -1.0 : static_cast<double>(p));
                });
            }
            if (prop == "startsWith") {
                return make_native_fn([s](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.empty()) return JsValue::boolean(false);
                    std::string pfx = a[0].to_str();
                    return JsValue::boolean(s.rfind(pfx, 0) == 0);
                });
            }
            if (prop == "endsWith") {
                return make_native_fn([s](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.empty()) return JsValue::boolean(false);
                    std::string sfx = a[0].to_str();
                    return JsValue::boolean(s.size() >= sfx.size() && s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0);
                });
            }
            if (prop == "split") {
                return make_native_fn([s](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    std::string sep = a.empty() ? "" : a[0].to_str();
                    std::vector<JsValue> parts;
                    if (sep.empty()) {
                        for (char c : s) parts.push_back(JsValue::string(std::string(1, c)));
                        return make_array(std::move(parts));
                    }
                    size_t start = 0;
                    while (true) {
                        size_t p = s.find(sep, start);
                        if (p == std::string::npos) {
                            parts.push_back(JsValue::string(s.substr(start)));
                            break;
                        }
                        parts.push_back(JsValue::string(s.substr(start, p - start)));
                        start = p + sep.size();
                    }
                    return make_array(std::move(parts));
                });
            }
            if (prop == "slice" || prop == "substring") {
                return make_native_fn([s](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    int n = static_cast<int>(s.size());
                    int st = a.empty() ? 0 : static_cast<int>(a[0].to_num());
                    int en = a.size() >= 2 ? static_cast<int>(a[1].to_num()) : n;
                    if (st < 0) st = std::max(0, n + st);
                    if (en < 0) en = std::max(0, n + en);
                    st = std::clamp(st, 0, n);
                    en = std::clamp(en, st, n);
                    return JsValue::string(s.substr(static_cast<size_t>(st), static_cast<size_t>(en - st)));
                });
            }
            if (prop == "replace" || prop == "replaceAll") {
                return make_native_fn([s, all = (prop == "replaceAll")](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.size() < 2) return JsValue::string(s);
                    std::string from = a[0].to_str();
                    std::string to = a[1].to_str();
                    if (from.empty()) return JsValue::string(s);
                    std::string out = s;
                    size_t pos = 0;
                    while ((pos = out.find(from, pos)) != std::string::npos) {
                        out.replace(pos, from.size(), to);
                        if (!all) break;
                        pos += to.size();
                    }
                    return JsValue::string(std::move(out));
                });
            }
        }
        if (target.kind == ValKind::DomElement && target.dom) {
            auto el = target.dom;
            if (prop == "id") return JsValue::string(el->id);
            if (prop == "tagName") return JsValue::string(el->tag);
            if (prop == "value") return JsValue::string(el->get_attr("value"));
            if (prop == "textContent") return JsValue::string(el->text_content());
            if (prop == "scrollTop") return JsValue::number(el->scroll_y);
            if (prop == "dataset") {
                auto ds = make_object();
                for (const auto& [k, v] : el->attrs) {
                    if (k.rfind("data-", 0) == 0) {
                        // Expose both camelCase and kebab tail (`data-line="12"` -> `dataset.line`)
                        std::string tail = k.substr(5);
                        std::string camel;
                        for (size_t i = 0; i < tail.size(); ++i) {
                            if (tail[i] == '-' && i + 1 < tail.size()) {
                                camel.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(tail[++i]))));
                            } else camel.push_back(tail[i]);
                        }
                        ds.obj->props[camel] = JsValue::string(v);
                        ds.obj->props[tail] = JsValue::string(v);
                    }
                }
                return ds;
            }
            if (prop == "classList") {
                auto cl = make_object();
                cl.obj->props["add"] = make_native_fn([el](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
                    if (!a.empty()) { el->add_class(a[0].to_str()); rt.dom_dirty = true; }
                    return JsValue::undef();
                });
                cl.obj->props["remove"] = make_native_fn([el](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
                    if (!a.empty()) { el->remove_class(a[0].to_str()); rt.dom_dirty = true; }
                    return JsValue::undef();
                });
                cl.obj->props["toggle"] = make_native_fn([el](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.empty()) return JsValue::boolean(false);
                    std::string c = a[0].to_str();
                    bool on = a.size() >= 2 ? a[1].truthy() : !el->has_class(c);
                    if (on) el->add_class(c); else el->remove_class(c);
                    rt.dom_dirty = true;
                    return JsValue::boolean(on);
                });
                return cl;
            }
            if (prop == "getAttribute") {
                return make_native_fn([el](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.empty()) return JsValue::null_v();
                    auto it = el->attrs.find(detail::to_lower_ascii(a[0].to_str()));
                    return it == el->attrs.end() ? JsValue::null_v() : JsValue::string(it->second);
                });
            }
            if (prop == "setAttribute") {
                return make_native_fn([el](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.size() >= 2) {
                        el->set_attr(detail::to_lower_ascii(a[0].to_str()), a[1].to_str());
                        rt.dom_dirty = true;
                    }
                    return JsValue::undef();
                });
            }
            if (prop == "closest") {
                return make_native_fn([el](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.empty()) return JsValue::null_v();
                    uint32_t dummy_i = 0, dummy_c = 0, dummy_t = 0;
                    SimpleSelector sel = css_detail::parse_simple_selector(a[0].to_str(), dummy_i, dummy_c, dummy_t);
                    std::shared_ptr<DomNode> cur = el;
                    while (cur) {
                        if (css_detail::matches_simple(*cur, sel)) return JsValue::element(cur);
                        cur = cur->parent.lock();
                    }
                    return JsValue::null_v();
                });
            }
            if (prop == "addEventListener") {
                return make_native_fn([el](JsRuntime& rt, const JsValue&, const std::vector<JsValue>& a) {
                    if (a.size() >= 2 && a[1].kind == ValKind::Function) {
                        rt.listeners.push_back({el->uid, a[0].to_str(), a[1]});
                    }
                    return JsValue::undef();
                });
            }
        }
        return JsValue::undef();
    }

    void set_member(JsValue& target, const std::string& prop, JsValue val) {
        if (target.kind == ValKind::Object && target.obj) {
            target.obj->props[prop] = std::move(val);
            return;
        }
        if (target.kind == ValKind::DomElement && target.dom) {
            auto el = target.dom;
            if (prop == "innerHTML") {
                el->children.clear();
                parse_html_into(val.to_str(), el, next_uid, nullptr, nullptr, nullptr, nullptr, el->tag == "pre" || el->tag == "code");
                dom_dirty = true;
            } else if (prop == "textContent") {
                el->set_text_content(val.to_str(), next_uid);
                dom_dirty = true;
            } else if (prop == "value") {
                el->set_attr("value", val.to_str());
                dom_dirty = true;
            } else if (prop == "scrollTop") {
                el->scroll_y = static_cast<float>(val.to_num());
                dom_dirty = true;
            }
        }
    }

    JsValue call_fn(const JsValue& callee, const JsValue& this_val, const std::vector<JsValue>& args) {
        if (callee.kind != ValKind::Function || !callee.fn) return JsValue::undef();
        if (callee.fn->is_native) return callee.fn->native(*this, this_val, args);
        auto call_env = std::make_shared<Env>(callee.fn->closure);
        call_env->define("this", this_val);
        for (size_t i = 0; i < callee.fn->params.size(); ++i) {
            call_env->define(callee.fn->params[i], i < args.size() ? args[i] : JsValue::undef());
        }
        ExecSignal sig = exec_stmt(callee.fn->body, call_env);
        return sig.type == ExecSignal::Return ? sig.val : JsValue::undef();
    }

    JsValue eval_expr(const ExprPtr& e, const std::shared_ptr<Env>& env) {
        if (!e) return JsValue::undef();
        switch (e->kind) {
            case ExprKind::Literal: return e->lit;
            case ExprKind::Ident: return env->get(e->ident);
            case ExprKind::ArrayLit: {
                std::vector<JsValue> items;
                items.reserve(e->list.size());
                for (const auto& item : e->list) items.push_back(eval_expr(item, env));
                return make_array(std::move(items));
            }
            case ExprKind::ObjectLit: {
                auto o = make_object();
                for (const auto& [k, ve] : e->kv) o.obj->props[k] = eval_expr(ve, env);
                return o;
            }
            case ExprKind::FnExpr: {
                JsValue v;
                v.kind = ValKind::Function;
                v.fn = std::make_shared<JsFunction>();
                v.fn->params = e->params;
                v.fn->body = e->fn_body;
                v.fn->closure = env;
                return v;
            }
            case ExprKind::Unary: {
                if (e->op == "!") return JsValue::boolean(!eval_expr(e->a, env).truthy());
                if (e->op == "-") return JsValue::number(-eval_expr(e->a, env).to_num());
                if (e->op == "+") return JsValue::number(+eval_expr(e->a, env).to_num());
                if (e->op == "typeof") {
                    JsValue v = eval_expr(e->a, env);
                    switch (v.kind) {
                        case ValKind::Undefined: return JsValue::string("undefined");
                        case ValKind::Bool: return JsValue::string("boolean");
                        case ValKind::Number: return JsValue::string("number");
                        case ValKind::String: return JsValue::string("string");
                        case ValKind::Function: return JsValue::string("function");
                        default: return JsValue::string("object");
                    }
                }
                if (e->op == "++" || e->op == "--" || e->op == "post++" || e->op == "post--") {
                    double delta = (e->op.find("++") != std::string::npos) ? 1.0 : -1.0;
                    double old_v = eval_expr(e->a, env).to_num();
                    double new_v = old_v + delta;
                    if (e->a->kind == ExprKind::Ident) env->set(e->a->ident, JsValue::number(new_v));
                    return JsValue::number(e->op.rfind("post", 0) == 0 ? old_v : new_v);
                }
                return JsValue::undef();
            }
            case ExprKind::Ternary:
                return eval_expr(e->a, env).truthy() ? eval_expr(e->b, env) : eval_expr(e->c, env);
            case ExprKind::Binary: {
                if (e->op == "&&") {
                    JsValue l = eval_expr(e->a, env);
                    return l.truthy() ? eval_expr(e->b, env) : l;
                }
                if (e->op == "||") {
                    JsValue l = eval_expr(e->a, env);
                    return l.truthy() ? l : eval_expr(e->b, env);
                }
                if (e->op == "??") {
                    JsValue l = eval_expr(e->a, env);
                    return (l.kind == ValKind::Null || l.kind == ValKind::Undefined) ? eval_expr(e->b, env) : l;
                }
                JsValue l = eval_expr(e->a, env);
                JsValue r = eval_expr(e->b, env);
                if (e->op == "+") {
                    if (l.kind == ValKind::String || r.kind == ValKind::String) {
                        return JsValue::string(l.to_str() + r.to_str());
                    }
                    return JsValue::number(l.to_num() + r.to_num());
                }
                if (e->op == "-") return JsValue::number(l.to_num() - r.to_num());
                if (e->op == "*") return JsValue::number(l.to_num() * r.to_num());
                if (e->op == "/") return JsValue::number(l.to_num() / r.to_num());
                if (e->op == "%") return JsValue::number(std::fmod(l.to_num(), r.to_num()));
                if (e->op == "<") return JsValue::boolean(l.kind == ValKind::String && r.kind == ValKind::String ? l.str < r.str : l.to_num() < r.to_num());
                if (e->op == "<=") return JsValue::boolean(l.kind == ValKind::String && r.kind == ValKind::String ? l.str <= r.str : l.to_num() <= r.to_num());
                if (e->op == ">") return JsValue::boolean(l.kind == ValKind::String && r.kind == ValKind::String ? l.str > r.str : l.to_num() > r.to_num());
                if (e->op == ">=") return JsValue::boolean(l.kind == ValKind::String && r.kind == ValKind::String ? l.str >= r.str : l.to_num() >= r.to_num());
                bool eq = false;
                if (l.kind == r.kind) {
                    if (l.kind == ValKind::Undefined || l.kind == ValKind::Null) eq = true;
                    else if (l.kind == ValKind::Bool) eq = (l.b == r.b);
                    else if (l.kind == ValKind::Number) eq = (l.num == r.num);
                    else if (l.kind == ValKind::String) eq = (l.str == r.str);
                    else if (l.kind == ValKind::DomElement) eq = (l.dom == r.dom);
                    else eq = (l.obj == r.obj && l.arr == r.arr && l.fn == r.fn);
                } else if (e->op == "==" || e->op == "!=") {
                    if ((l.kind == ValKind::Null && r.kind == ValKind::Undefined) ||
                        (l.kind == ValKind::Undefined && r.kind == ValKind::Null)) eq = true;
                    else eq = (l.to_str() == r.to_str());
                }
                return JsValue::boolean((e->op == "==" || e->op == "===") ? eq : !eq);
            }
            case ExprKind::Assign: {
                JsValue rhs = eval_expr(e->b, env);
                auto apply_op = [&](const JsValue& cur) -> JsValue {
                    if (e->op == "+=") {
                        if (cur.kind == ValKind::String || rhs.kind == ValKind::String)
                            return JsValue::string(cur.to_str() + rhs.to_str());
                        return JsValue::number(cur.to_num() + rhs.to_num());
                    }
                    if (e->op == "-=") return JsValue::number(cur.to_num() - rhs.to_num());
                    return rhs;
                };
                if (e->a->kind == ExprKind::Ident) {
                    JsValue res = (e->op == "=") ? rhs : apply_op(env->get(e->a->ident));
                    env->set(e->a->ident, res);
                    return res;
                }
                if (e->a->kind == ExprKind::Member) {
                    JsValue obj = eval_expr(e->a->a, env);
                    JsValue res = (e->op == "=") ? rhs : apply_op(get_member(obj, e->a->ident));
                    set_member(obj, e->a->ident, res);
                    return res;
                }
                if (e->a->kind == ExprKind::Index) {
                    JsValue container = eval_expr(e->a->a, env);
                    JsValue idx = eval_expr(e->a->b, env);
                    if (container.kind == ValKind::Array && container.arr) {
                        size_t k = static_cast<size_t>(std::max(0.0, idx.to_num()));
                        if (k >= container.arr->items.size()) container.arr->items.resize(k + 1);
                        container.arr->items[k] = (e->op == "=") ? rhs : apply_op(container.arr->items[k]);
                        return container.arr->items[k];
                    }
                    if (container.kind == ValKind::Object && container.obj) {
                        std::string key = idx.to_str();
                        container.obj->props[key] = (e->op == "=") ? rhs : apply_op(container.obj->props[key]);
                        return container.obj->props[key];
                    }
                }
                return rhs;
            }
            case ExprKind::Member: {
                JsValue obj = eval_expr(e->a, env);
                return get_member(obj, e->ident);
            }
            case ExprKind::Index: {
                JsValue obj = eval_expr(e->a, env);
                JsValue idx = eval_expr(e->b, env);
                if (obj.kind == ValKind::Array && obj.arr) {
                    int k = static_cast<int>(idx.to_num());
                    if (k >= 0 && k < static_cast<int>(obj.arr->items.size())) return obj.arr->items[static_cast<size_t>(k)];
                    return JsValue::undef();
                }
                if (obj.kind == ValKind::String) {
                    int k = static_cast<int>(idx.to_num());
                    if (k >= 0 && k < static_cast<int>(obj.str.size())) return JsValue::string(std::string(1, obj.str[static_cast<size_t>(k)]));
                    return JsValue::undef();
                }
                return get_member(obj, idx.to_str());
            }
            case ExprKind::Call: {
                JsValue this_val = JsValue::undef();
                JsValue fn_val;
                if (e->a->kind == ExprKind::Member) {
                    this_val = eval_expr(e->a->a, env);
                    fn_val = get_member(this_val, e->a->ident);
                } else {
                    fn_val = eval_expr(e->a, env);
                }
                std::vector<JsValue> args;
                args.reserve(e->list.size());
                for (const auto& a : e->list) args.push_back(eval_expr(a, env));
                return call_fn(fn_val, this_val, args);
            }
        }
        return JsValue::undef();
    }

    ExecSignal exec_stmt(const StmtPtr& s, const std::shared_ptr<Env>& env) {
        if (!s) return {};
        switch (s->kind) {
            case StmtKind::Block: {
                auto block_env = std::make_shared<Env>(env);
                // Hoist function declarations in scope per ES semantics
                for (const auto& st : s->list) {
                    if (st->kind == StmtKind::FnDecl) {
                        JsValue fn;
                        fn.kind = ValKind::Function;
                        fn.fn = std::make_shared<JsFunction>();
                        fn.fn->params = st->params;
                        fn.fn->body = st->then_branch;
                        fn.fn->closure = block_env;
                        block_env->define(st->name, std::move(fn));
                    }
                }
                for (const auto& st : s->list) {
                    if (st->kind == StmtKind::FnDecl) continue;
                    ExecSignal sig = exec_stmt(st, block_env);
                    if (sig.type != ExecSignal::None) return sig;
                }
                return {};
            }
            case StmtKind::FnDecl: {
                JsValue fn;
                fn.kind = ValKind::Function;
                fn.fn = std::make_shared<JsFunction>();
                fn.fn->params = s->params;
                fn.fn->body = s->then_branch;
                fn.fn->closure = env;
                env->define(s->name, std::move(fn));
                return {};
            }
            case StmtKind::VarDecl:
                env->define(s->name, s->init_expr ? eval_expr(s->init_expr, env) : JsValue::undef());
                return {};
            case StmtKind::ExprStmt:
                eval_expr(s->init_expr, env);
                return {};
            case StmtKind::If:
                if (eval_expr(s->cond_expr, env).truthy()) return exec_stmt(s->then_branch, env);
                if (s->else_branch) return exec_stmt(s->else_branch, env);
                return {};
            case StmtKind::While: {
                while (eval_expr(s->cond_expr, env).truthy()) {
                    ExecSignal sig = exec_stmt(s->then_branch, env);
                    if (sig.type == ExecSignal::Return) return sig;
                    if (sig.type == ExecSignal::Break) break;
                }
                return {};
            }
            case StmtKind::ForC: {
                auto loop_env = std::make_shared<Env>(env);
                if (s->init_stmt) exec_stmt(s->init_stmt, loop_env);
                while (!s->cond_expr || eval_expr(s->cond_expr, loop_env).truthy()) {
                    ExecSignal sig = exec_stmt(s->then_branch, loop_env);
                    if (sig.type == ExecSignal::Return) return sig;
                    if (sig.type == ExecSignal::Break) break;
                    if (s->step_expr) eval_expr(s->step_expr, loop_env);
                }
                return {};
            }
            case StmtKind::ForOf: {
                JsValue seq = eval_expr(s->init_expr, env);
                if (seq.kind == ValKind::Array && seq.arr) {
                    for (const auto& item : seq.arr->items) {
                        auto it_env = std::make_shared<Env>(env);
                        it_env->define(s->name, item);
                        ExecSignal sig = exec_stmt(s->then_branch, it_env);
                        if (sig.type == ExecSignal::Return) return sig;
                        if (sig.type == ExecSignal::Break) break;
                    }
                }
                return {};
            }
            case StmtKind::Return:
                return {ExecSignal::Return, s->init_expr ? eval_expr(s->init_expr, env) : JsValue::undef()};
            case StmtKind::Break:
                return {ExecSignal::Break, JsValue::undef()};
            case StmtKind::Continue:
                return {ExecSignal::Continue, JsValue::undef()};
        }
        return {};
    }

    void eval_script(std::string_view code) {
        refresh_document_refs();
        Parser parser(tokenize_js(code));
        StmtPtr prog = parser.parse_program();
        // Execute top-level statements directly in `global_env`
        for (const auto& st : prog->list) {
            if (st->kind == StmtKind::FnDecl) exec_stmt(st, global_env);
        }
        for (const auto& st : prog->list) {
            if (st->kind != StmtKind::FnDecl) exec_stmt(st, global_env);
        }
    }

    void dispatch_dom_event(const std::string& type, const std::shared_ptr<DomNode>& target, const std::string& key = "") {
        if (!target) return;
        auto ev = make_object();
        ev.obj->props["type"] = JsValue::string(type);
        ev.obj->props["target"] = JsValue::element(target);
        ev.obj->props["key"] = JsValue::string(key);

        // Bubble up the DOM tree from `target` to `document` per W3C UI Events specification
        std::shared_ptr<DomNode> cur = target;
        while (cur) {
            ev.obj->props["currentTarget"] = JsValue::element(cur);
            // Copy matching listeners first so handlers mutating DOM safely iterate
            std::vector<JsValue> cbs;
            for (const auto& l : listeners) {
                if (l.node_uid == cur->uid && l.event_type == type) cbs.push_back(l.callback);
            }
            for (const auto& cb : cbs) {
                call_fn(cb, JsValue::element(cur), {ev});
            }
            cur = cur->parent.lock();
        }
    }
};

} // namespace helios::js
