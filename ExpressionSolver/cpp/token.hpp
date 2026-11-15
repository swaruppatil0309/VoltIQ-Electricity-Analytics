// token.hpp — the token model shared by the tokenizer, the shunting-yard
// converter, the evaluator and the tracer.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

enum class TokType {
    Number,    // 3, 4.5, .25, 1e3
    Variable,  // x, rate, total_1
    Op,        // + - * / % ^
    LParen,    // (
    RParen,    // )
    Func,      // sin, sqrt, max
    Comma      // ,
};

struct Token {
    TokType type = TokType::Number;
    std::string text;   // exactly as it appeared in the source
    double value = 0.0; // numeric literal value
    char op = 0;        // operator character
    int prec = 0;       // operator / function precedence
    bool rightAssoc = false;
    bool unary = false; // unary minus / plus
    bool implicit = false; // a '*' that the tokenizer inserted (e.g. 2x, 3(4+5))
    bool folded = false;   // a literal produced by compile-time constant folding
    int pos = 0;        // 0-based column in the source string
    int len = 0;
    int arity = 1;      // functions: 1 or 2 arguments
};

// ---------------------------------------------------------------------------
// operator metadata
// ---------------------------------------------------------------------------
inline int precedenceOf(char op) {
    switch (op) {
        case '+':
        case '-': return 1;
        case '*':
        case '/':
        case '%': return 2;
        case '^': return 4;
        default: return 0;
    }
}

inline bool rightAssoc(char op) { return op == '^'; }

inline bool isOperatorChar(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '^';
}

// unary minus binds tighter than * / but looser than ^: -2^2 == -(2^2) == -4
constexpr int kUnaryPrecedence = 3;

inline bool isFunctionName(const std::string& name, int& arity) {
    struct Fn { const char* name; int arity; };
    static const Fn kFuncs[] = {
        {"sin", 1}, {"cos", 1}, {"tan", 1},
        {"asin", 1}, {"acos", 1}, {"atan", 1},
        {"sqrt", 1}, {"cbrt", 1}, {"abs", 1},
        {"log", 1}, {"ln", 1}, {"log10", 1},
        {"exp", 1}, {"floor", 1}, {"ceil", 1}, {"round", 1},
        {"sign", 1},
        {"min", 2}, {"max", 2}, {"pow", 2}, {"mod", 2},
    };
    for (const Fn& f : kFuncs) {
        if (name == f.name) {
            arity = f.arity;
            return true;
        }
    }
    return false;
}

inline std::string tokenTypeName(TokType t) {
    switch (t) {
        case TokType::Number: return "number";
        case TokType::Variable: return "variable";
        case TokType::Op: return "operator";
        case TokType::LParen: return "lparen";
        case TokType::RParen: return "rparen";
        case TokType::Func: return "function";
        case TokType::Comma: return "comma";
    }
    return "unknown";
}

// compact, round-trippable-ish rendering of a computed value: 11, 3.5, 0.333333333333
inline std::string formatNumber(double v) {
    if (v == 0.0) return "0";  // avoid "-0"
    char buf[64];
    if (std::isfinite(v) && std::fabs(v) < 1e15 && v == std::floor(v)) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.12g", v);
    return buf;
}

// human readable label used by the CLI trace tables and the web UI
inline std::string tokenLabel(const Token& t) {
    switch (t.type) {
        case TokType::Op:
            if (t.unary) return std::string(t.text) + " (unary)";
            return std::string(t.text);
        case TokType::Func: return std::string(t.text) + "()";
        case TokType::Number: return t.text;
        case TokType::Variable: return t.text;
        default: return t.text;
    }
}
