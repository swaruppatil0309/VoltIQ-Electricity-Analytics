// shunting_yard.hpp — infix → postfix (Reverse Polish) conversion, with a
// recording of every stack operation so the process can be replayed visually.
#pragma once

#include <string>
#include <vector>

#include "ds.hpp"
#include "token.hpp"

struct ConvertStep {
    int step = 0;
    std::string token;   // token being handled
    std::string action;  // output | push | pop | match | comma | drop
    std::string note;    // plain-English explanation
    std::vector<std::string> stack;   // operator stack, bottom → top
    std::vector<std::string> output;  // postfix produced so far
    int tokenIndex = -1;              // index into the token list (-1 for implicit)
};

struct ConvertResult {
    bool ok = false;
    std::string error;
    int errorPos = 0;
    std::vector<Token> rpn;
    std::vector<ConvertStep> steps;
    int peakStack = 0;
};

namespace shunting {

inline std::string stackLabel(const Token& t) {
    if (t.type == TokType::Func) return t.text + "()";
    if (t.type == TokType::LParen) return "(";
    if (t.type == TokType::Op && t.unary) return std::string(1, t.op) + "u";
    return t.text;
}

inline ConvertResult run(const std::vector<Token>& tokens, bool wantTrace = true) {
    ConvertResult r;
    ArrayStack<Token> ops;
    ArrayStack<int> argCounts;  // commas seen inside the current parenthesis
    std::vector<Token> out;
    out.reserve(tokens.size());

    auto snap = [&](ConvertStep& s) {
        for (const Token& t : ops.toVector()) s.stack.push_back(stackLabel(t));
        for (const Token& t : out) s.output.push_back(t.text);
        if (ops.size() > static_cast<std::size_t>(r.peakStack)) r.peakStack = static_cast<int>(ops.size());
    };

    auto record = [&](const std::string& token, const std::string& action,
                      const std::string& note, int tokenIndex) {
        if (!wantTrace) return;  // the compiler does not pay for snapshots
        ConvertStep s;
        s.step = static_cast<int>(r.steps.size()) + 1;
        s.token = token;
        s.action = action;
        s.note = note;
        s.tokenIndex = tokenIndex;
        snap(s);
        r.steps.push_back(s);
    };

    auto fail = [&](const std::string& msg, int pos) {
        r.ok = false;
        r.error = msg;
        r.errorPos = pos;
        return r;
    };

    for (std::size_t ti = 0; ti < tokens.size(); ++ti) {
        const Token& t = tokens[ti];
        const int idx = static_cast<int>(ti);
        const std::string label = (t.implicit ? "* (implicit)" : tokenLabel(t));

        switch (t.type) {
            case TokType::Number:
            case TokType::Variable: {
                out.push_back(t);
                record(label, "output",
                       t.implicit ? "folded constant goes straight to the output" : "operand → output queue",
                       idx);
                break;
            }
            case TokType::Func: {
                ops.push(t);
                record(label, "push", "function waits on the stack for its arguments", idx);
                break;
            }
            case TokType::Comma: {
                if (ops.empty()) return fail("',' outside a function call", t.pos);
                while (!ops.empty() && ops.top().type != TokType::LParen) {
                    out.push_back(ops.popValue());
                    record("", "pop", "',' closes the previous argument: pop until '('",
                           idx);
                }
                if (ops.empty()) return fail("',' outside a function call", t.pos);
                if (argCounts.empty()) return fail("',' outside a function call", t.pos);
                argCounts.push(argCounts.popValue() + 1);
                record(label, "comma", "argument boundary recorded", idx);
                break;
            }
            case TokType::LParen: {
                ops.push(t);
                argCounts.push(0);
                record(label, "push", "left parenthesis marks the start of a group", idx);
                break;
            }
            case TokType::RParen: {
                while (!ops.empty() && ops.top().type != TokType::LParen) {
                    out.push_back(ops.popValue());
                    record("", "pop", "')' forces pending operators out to the output", idx);
                }
                if (ops.empty()) return fail("unmatched ')'", t.pos);
                ops.pop();  // the '(' itself
                int commas = 0;
                if (!argCounts.empty()) commas = argCounts.popValue();
                record(label, "match", "parenthesis matched — '(' is discarded", idx);

                if (!ops.empty() && ops.top().type == TokType::Func) {
                    Token fn = ops.popValue();
                    const int args = commas + 1;
                    if (args != fn.arity) {
                        return fail(fn.text + "() expects " + std::to_string(fn.arity) +
                                        (fn.arity == 1 ? " argument" : " arguments") + " but got " +
                                        std::to_string(args),
                                    fn.pos);
                    }
                    out.push_back(fn);
                    record(fn.text + "()", "output",
                           "arguments are complete — the function moves to the output", idx);
                }
                break;
            }
            case TokType::Op: {
                const int prec = t.unary ? kUnaryPrecedence : t.prec;
                // A unary operator is a prefix operator: it never causes a pop.
                // Skipping the loop is what makes -2^2 == -4 and 2^-3 work.
                while (!t.unary && !ops.empty()) {
                    const Token& top = ops.top();
                    if (top.type == TokType::Func) {
                        out.push_back(ops.popValue());
                        record("", "pop", "functions bind tightest — pop it first", idx);
                        continue;
                    }
                    if (top.type != TokType::Op) break;
                    const int topPrec = top.unary ? kUnaryPrecedence : top.prec;
                    const bool topRight = top.unary ? true : top.rightAssoc;
                    (void)topRight;
                    const bool shouldPop = topPrec > prec || (topPrec == prec && !t.rightAssoc);
                    if (!shouldPop) break;
                    out.push_back(ops.popValue());
                    record("", "pop",
                           std::string("operator '") + stackLabel(out.back()) +
                               "' binds tighter or is left-associative — pop it first",
                           idx);
                }
                ops.push(t);
                record(label, "push",
                       t.unary ? "unary operator waits on the stack"
                               : std::string("'") + t.op + "' waits: higher precedence stays on top",
                       idx);
                break;
            }
        }
    }

    while (!ops.empty()) {
        if (ops.top().type == TokType::LParen) return fail("unclosed '('", ops.top().pos);
        out.push_back(ops.popValue());
        record("", "pop", "end of input — flush the remaining stack", -1);
    }

    r.ok = true;
    r.rpn = out;
    return r;
}

inline std::string rpnText(const std::vector<Token>& rpn) {
    std::string s;
    for (const Token& t : rpn) {
        if (!s.empty()) s += ' ';
        if (t.type == TokType::Func) s += t.text + "()";
        else if (t.unary) s += std::string(1, t.op) + "u";
        else s += t.text;  // implicit × prints as * so the program stays ASCII
    }
    return s;
}

}  // namespace shunting
