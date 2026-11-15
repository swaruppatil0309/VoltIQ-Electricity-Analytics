// tokenizer.hpp — one left-to-right lexer that also validates the grammar.
//
// It is a single pass, O(n) in the source length, and it is the only place that
// ever looks at raw characters again. Everything downstream (optimizer, RPN,
// evaluator, visualiser) works on tokens / instructions.
//
// Extra feature: implicit multiplication.  2x, 3(4+5), (1+2)(3+4), 2sin(x)
// are tokenized as if the missing '*' had been typed. The inserted token is
// flagged so the UI can show it in a different colour.
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "token.hpp"

struct TokenizeResult {
    bool ok = false;
    std::string error;
    int errorPos = 0;
    int errorLen = 1;
    std::vector<Token> tokens;
    int implicitInserts = 0;

    // convenience: pretty "3 + 4 · 2" style rendering (implicit * shown as ·)
    std::string normalized() const {
        std::string out;
        for (const Token& t : tokens) {
            if (!out.empty()) out += ' ';
            out += (t.implicit ? "\xC2\xB7" : t.text);
        }
        return out;
    }
};

namespace lexer {

inline bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

inline bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

inline TokenizeResult run(const std::string& src) {
    TokenizeResult r;
    const std::size_t n = src.size();
    std::size_t i = 0;
    bool expectOperand = true;
    int openParens = 0;

    auto fail = [&](const std::string& msg, std::size_t pos, int len) {
        r.ok = false;
        r.error = msg;
        r.errorPos = static_cast<int>(pos);
        r.errorLen = len < 1 ? 1 : len;
        return r;
    };

    auto pushToken = [&](const Token& t) { r.tokens.push_back(t); };

    auto pushImplicitMul = [&](std::size_t pos) {
        Token mul;
        mul.type = TokType::Op;
        mul.text = "*";
        mul.op = '*';
        mul.prec = precedenceOf('*');
        mul.pos = static_cast<int>(pos);
        mul.len = 0;
        mul.implicit = true;
        pushToken(mul);
        ++r.implicitInserts;
    };

    while (i < n) {
        char c = src[i];

        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        // ---- number -----------------------------------------------------
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            const char* begin = src.c_str() + i;
            char* end = nullptr;
            double value = std::strtod(begin, &end);
            std::size_t len = static_cast<std::size_t>(end - begin);
            // 12 34 is a typo, not a multiplication — implicit × is only
            // inserted in front of variables, functions and '('.
            if (!expectOperand)
                return fail("unexpected number — two values in a row", i, static_cast<int>(len));
            Token t;
            t.type = TokType::Number;
            t.text = src.substr(i, len);
            t.value = value;
            t.pos = static_cast<int>(i);
            t.len = static_cast<int>(len);
            pushToken(t);
            i += len;
            expectOperand = false;
            continue;
        }

        // ---- identifier: function or variable ---------------------------
        if (isIdentStart(c)) {
            std::size_t start = i;
            while (i < n && isIdentChar(src[i])) ++i;
            std::string name = src.substr(start, i - start);

            // look ahead (through spaces) for '(' to detect a function call
            std::size_t j = i;
            while (j < n && std::isspace(static_cast<unsigned char>(src[j]))) ++j;
            int arity = 0;
            bool call = (j < n && src[j] == '(' && isFunctionName(name, arity));

            if (!expectOperand) {
                pushImplicitMul(start);
                expectOperand = true;
            }

            Token t;
            t.pos = static_cast<int>(start);
            t.len = static_cast<int>(i - start);
            if (call) {
                t.type = TokType::Func;
                t.text = name;
                t.arity = arity;
                t.prec = 5;
            } else {
                t.type = TokType::Variable;
                t.text = name;
            }
            pushToken(t);
            // a function is still waiting for its '(' and arguments; a bare
            // variable is already a complete operand
            expectOperand = call;
            continue;
        }

        // ---- ( ----------------------------------------------------------
        if (c == '(') {
            if (!expectOperand) pushImplicitMul(i);
            Token t;
            t.type = TokType::LParen;
            t.text = "(";
            t.pos = static_cast<int>(i);
            t.len = 1;
            pushToken(t);
            ++openParens;
            ++i;
            expectOperand = true;
            continue;
        }

        // ---- ) ----------------------------------------------------------
        if (c == ')') {
            if (expectOperand) {
                return fail(r.tokens.empty() ? "empty parentheses" : "expected a value before ')'", i, 1);
            }
            if (openParens == 0) return fail("unmatched ')'", i, 1);
            Token t;
            t.type = TokType::RParen;
            t.text = ")";
            t.pos = static_cast<int>(i);
            t.len = 1;
            pushToken(t);
            --openParens;
            ++i;
            expectOperand = false;
            continue;
        }

        // ---- , ----------------------------------------------------------
        if (c == ',') {
            if (expectOperand) return fail("expected a value before ','", i, 1);
            Token t;
            t.type = TokType::Comma;
            t.text = ",";
            t.pos = static_cast<int>(i);
            t.len = 1;
            pushToken(t);
            ++i;
            expectOperand = true;
            continue;
        }

        // ---- operator ---------------------------------------------------
        if (isOperatorChar(c)) {
            if (expectOperand) {
                if (c == '+' || c == '-') {
                    Token t;
                    t.type = TokType::Op;
                    t.text = std::string(1, c);
                    t.op = c;
                    t.unary = true;
                    t.prec = kUnaryPrecedence;
                    t.rightAssoc = true;
                    t.pos = static_cast<int>(i);
                    t.len = 1;
                    pushToken(t);
                    ++i;
                            expectOperand = true;
                    continue;
                }
                return fail(std::string("operator '") + c + "' needs a value on its left", i, 1);
            }
            Token t;
            t.type = TokType::Op;
            t.text = std::string(1, c);
            t.op = c;
            t.prec = precedenceOf(c);
            t.rightAssoc = rightAssoc(c);
            t.pos = static_cast<int>(i);
            t.len = 1;
            pushToken(t);
            ++i;
            expectOperand = true;
            continue;
        }

        return fail(std::string("unexpected character '") + c + "'", i, 1);
    }

    if (r.tokens.empty()) return fail("expression is empty", 0, 1);
    if (openParens > 0) {
        // point at the last unclosed '('
        int pos = 0;
        for (const Token& t : r.tokens)
            if (t.type == TokType::LParen) pos = t.pos;
        return fail("unclosed '(' — add a matching ')'", static_cast<std::size_t>(pos), 1);
    }
    if (expectOperand) {
        const Token& last = r.tokens.back();
        return fail("expression ends where a value was expected", static_cast<std::size_t>(last.pos), last.len);
    }

    r.ok = true;
    return r;
}

}  // namespace lexer
