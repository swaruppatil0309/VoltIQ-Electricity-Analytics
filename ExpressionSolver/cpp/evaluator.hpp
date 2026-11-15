// evaluator.hpp — postfix evaluation on an explicit stack, step by step.
//
// This is the "runtime" of the engine: it consumes the compiled instruction
// list (the postfix program) and never re-parses the source text.
#pragma once

#include <string>
#include <vector>

#include "ast.hpp"
#include "ds.hpp"
#include "token.hpp"

struct EvalStep {
    int step = 0;
    int tokenIndex = -1;
    std::string token;
    std::string action;  // push | apply | function
    std::vector<std::string> stack;  // operand stack, bottom → top
    std::string note;
    std::string value;  // value produced by this step ("" when pushing)
};

struct EvalResult {
    bool ok = false;
    std::string error;
    int errorPos = 0;
    double value = 0.0;
    int peakStack = 0;
    int opsApplied = 0;
    std::vector<EvalStep> trace;
};

namespace evalr {

inline EvalResult run(const std::vector<Token>& rpn, const VarMap& vars, bool wantTrace = true) {
    EvalResult r;
    ArrayStack<double> stack;
    std::string unknownVar;

    auto snap = [&](EvalStep& s) {
        for (double v : stack.toVector()) s.stack.push_back(formatNumber(v));
        if (stack.size() > static_cast<std::size_t>(r.peakStack))
            r.peakStack = static_cast<int>(stack.size());
    };

    auto record = [&](int tokenIndex, const std::string& token, const std::string& action,
                      const std::string& note, const std::string& value) {
        if (!wantTrace) return;  // hot path: no snapshotting, no formatting
        EvalStep s;
        s.step = static_cast<int>(r.trace.size()) + 1;
        s.tokenIndex = tokenIndex;
        s.token = token;
        s.action = action;
        s.note = note;
        s.value = value;
        snap(s);
        r.trace.push_back(s);
    };

    auto fail = [&](const std::string& msg, int pos) {
        r.ok = false;
        r.error = msg;
        r.errorPos = pos;
        return r;
    };

    for (std::size_t i = 0; i < rpn.size(); ++i) {
        const Token& t = rpn[i];
        const int idx = static_cast<int>(i);
        const std::string label =
            t.type == TokType::Func ? t.text + "()" : (t.implicit ? "* (implicit)" : tokenLabel(t));

        if (t.type == TokType::Number) {
            stack.push(t.value);
            record(idx, label, "push", "push the literal value", formatNumber(t.value));
            continue;
        }
        if (t.type == TokType::Variable) {
            double v = 0.0;
            if (!vars.get(t.text, v)) {
                return fail("unknown variable '" + t.text + "' — give it a value first", t.pos);
            }
            stack.push(v);
            record(idx, label, "push",
                   "look the variable up in the hash map and push its value", formatNumber(v));
            continue;
        }

        if (t.type == TokType::Op) {
            const int need = t.unary ? 1 : 2;
            if (static_cast<int>(stack.size()) < need)
                return fail(std::string("operator '") + t.op + "' is missing an operand", t.pos);

            const double b = stack.popValue();
            const double a = t.unary ? 0.0 : stack.popValue();
            if (!t.unary && (t.op == '/' || t.op == '%') && b == 0.0)
                return fail(t.op == '/' ? "division by zero" : "modulo by zero", t.pos);

            const double result = ast::applyOp(t.op, t.unary, a, b);
            stack.push(result);
            ++r.opsApplied;

            std::string note;
            if (t.unary) {
                note = std::string("unary '") + t.op + "' applied to " + formatNumber(b);
            } else {
                note = formatNumber(a) + " " + std::string(1, t.op) + " " + formatNumber(b) +
                       " = " + formatNumber(result);
            }
            record(idx, label, "apply", note, formatNumber(result));
            continue;
        }

        if (t.type == TokType::Func) {
            const int arity = t.arity > 0 ? t.arity : 1;
            if (static_cast<int>(stack.size()) < arity)
                return fail("function " + t.text + "() is missing an argument", t.pos);

            double args[2] = {0.0, 0.0};
            for (int k = arity - 1; k >= 0; --k) args[k] = stack.popValue();

            double result = 0.0;
            if (!ast::applyFunction(t.text, args, arity, result))
                return fail("unknown function " + t.text + "()", t.pos);
            stack.push(result);
            ++r.opsApplied;

            std::string call = t.text + "(" + formatNumber(args[0]);
            if (arity == 2) call += ", " + formatNumber(args[1]);
            call += ") = " + formatNumber(result);
            record(idx, label, "function", call, formatNumber(result));
            continue;
        }
    }

    if (stack.empty()) return fail("the expression produced no value", 0);
    if (stack.size() > 1)
        return fail(std::to_string(stack.size()) + " values left on the stack — expression is incomplete", 0);

    r.value = stack.top();
    r.ok = true;
    return r;
}

}  // namespace evalr
