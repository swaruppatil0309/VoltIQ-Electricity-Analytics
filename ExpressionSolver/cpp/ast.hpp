// ast.hpp — expression tree built from the postfix program.
//
// Two jobs:
//   1. give the visualiser a tree (pre-order / in-order / post-order walks),
//   2. host the compiler optimisation: constant folding. A subtree whose
//      leaves are all literals is evaluated once at compile time and replaced
//      by a single number, so the runtime program is shorter.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "token.hpp"

struct AstNode {
    TokType type = TokType::Number;
    std::string name;   // variable or function name
    std::string text;   // literal text as typed
    double value = 0.0; // literal value (or folded value)
    char op = 0;
    bool unary = false;
    bool implicit = false;
    int left = -1;
    int right = -1;
    int pos = 0;
    int len = 1;
    bool folded = false;      // this node (or subtree) was replaced by a literal
    int foldCount = 0;        // instructions removed by folding this subtree
    std::string foldText;     // text of the folded constant
};

struct Ast {
    std::vector<AstNode> nodes;
    int root = -1;
    int height = 0;
    int foldedNodes = 0;
    int foldedConstants = 0;
};

namespace ast {

inline int addNode(Ast& tree, const AstNode& n) {
    tree.nodes.push_back(n);
    return static_cast<int>(tree.nodes.size()) - 1;
}

// build an expression tree from a postfix token list
inline Ast fromRpn(const std::vector<Token>& rpn) {
    Ast tree;
    std::vector<int> st;
    st.reserve(rpn.size());

    for (const Token& t : rpn) {
        if (t.type == TokType::Number || t.type == TokType::Variable) {
            AstNode n;
            n.type = t.type;
            n.name = t.text;
            n.text = t.text;
            n.value = t.value;
            n.pos = t.pos;
            n.len = t.len;
            st.push_back(addNode(tree, n));
        } else if (t.type == TokType::Op) {
            AstNode n;
            n.type = TokType::Op;
            n.op = t.op;
            n.unary = t.unary;
            n.implicit = t.implicit;
            n.pos = t.pos;
            n.len = t.len;
            if (!st.empty()) {
                n.right = st.back();
                st.pop_back();
            }
            if (t.unary) {
                n.left = n.right;
                n.right = -1;
            } else if (!st.empty()) {
                n.left = st.back();
                st.pop_back();
            }
            st.push_back(addNode(tree, n));
        } else if (t.type == TokType::Func) {
            AstNode n;
            n.type = TokType::Func;
            n.name = t.text;
            n.pos = t.pos;
            n.len = t.len;
            if (t.arity == 2) {
                if (!st.empty()) {
                    n.right = st.back();
                    st.pop_back();
                }
                if (!st.empty()) {
                    n.left = st.back();
                    st.pop_back();
                }
            } else {
                if (!st.empty()) {
                    n.left = st.back();
                    st.pop_back();
                }
            }
            st.push_back(addNode(tree, n));
        }
    }
    if (!st.empty()) tree.root = st.back();
    return tree;
}

inline double applyOp(char op, bool unary, double a, double b) {
    if (unary) return op == '-' ? -b : b;
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return a / b;
        case '%': return std::fmod(a, b);
        case '^': return std::pow(a, b);
        default: return 0.0;
    }
}

inline bool applyFunction(const std::string& name, const double* args, int arity, double& out) {
    const double a = args[0];
    const double b = arity > 1 ? args[1] : 0.0;
    if (name == "sin") out = std::sin(a);
    else if (name == "cos") out = std::cos(a);
    else if (name == "tan") out = std::tan(a);
    else if (name == "asin") out = std::asin(a);
    else if (name == "acos") out = std::acos(a);
    else if (name == "atan") out = std::atan(a);
    else if (name == "sqrt") out = std::sqrt(a);
    else if (name == "cbrt") out = std::cbrt(a);
    else if (name == "abs") out = std::fabs(a);
    else if (name == "log" || name == "ln") out = std::log(a);
    else if (name == "log10") out = std::log10(a);
    else if (name == "exp") out = std::exp(a);
    else if (name == "floor") out = std::floor(a);
    else if (name == "ceil") out = std::ceil(a);
    else if (name == "round") out = std::round(a);
    else if (name == "sign") out = (a > 0) - (a < 0);
    else if (name == "min") out = a < b ? a : b;
    else if (name == "max") out = a > b ? a : b;
    else if (name == "pow") out = std::pow(a, b);
    else if (name == "mod") out = std::fmod(a, b);
    else return false;
    return true;
}

// --- constant folding -------------------------------------------------------
// Returns true when the subtree is a compile-time constant.
inline bool fold(Ast& tree, int idx) {
    if (idx < 0) return false;
    AstNode& n = tree.nodes[static_cast<std::size_t>(idx)];

    if (n.type == TokType::Number) return true;
    if (n.type == TokType::Variable) return false;

    const bool leftConst = fold(tree, n.left);
    const bool rightConst = n.right >= 0 ? fold(tree, n.right) : true;
    if (!leftConst || !rightConst) return false;

    const double a = n.left >= 0 ? tree.nodes[static_cast<std::size_t>(n.left)].value : 0.0;
    const double b = n.right >= 0 ? tree.nodes[static_cast<std::size_t>(n.right)].value : 0.0;
    double result = 0.0;

    if (n.type == TokType::Op) {
        if (!n.unary && (n.op == '/' || n.op == '%') && b == 0.0) return false;  // leave to runtime
        // a unary node keeps its single operand in `left`
        result = n.unary ? applyOp(n.op, true, 0.0, a) : applyOp(n.op, false, a, b);
    } else if (n.type == TokType::Func) {
        double args[2] = {a, b};
        double tmp = 0.0;
        if (!applyFunction(n.name, args, n.right >= 0 ? 2 : 1, tmp)) return false;
        result = tmp;
    } else {
        return false;
    }

    if (!std::isfinite(result)) return false;  // keep sqrt(-1) etc. as runtime errors

    n.type = TokType::Number;
    n.value = result;
    n.folded = true;
    n.left = n.right = -1;
    tree.foldedNodes++;
    return true;
}

// Emit the postfix program for a (possibly folded) tree.
inline void emitRpn(const Ast& tree, int idx, std::vector<Token>& out, int& foldedEmitted) {
    if (idx < 0) return;
    const AstNode& n = tree.nodes[static_cast<std::size_t>(idx)];

    if (n.type == TokType::Number) {
        Token t;
        t.type = TokType::Number;
        t.value = n.value;
        t.pos = n.pos;
        t.len = n.len;
        // format without trailing zeros
        t.text = formatNumber(n.value);
        t.folded = n.folded;
        if (n.folded) ++foldedEmitted;
        out.push_back(t);
        return;
    }
    if (n.type == TokType::Variable) {
        Token t;
        t.type = TokType::Variable;
        t.text = n.name;
        t.pos = n.pos;
        t.len = n.len;
        out.push_back(t);
        return;
    }
    emitRpn(tree, n.left, out, foldedEmitted);
    emitRpn(tree, n.right, out, foldedEmitted);
    Token t;
    t.type = n.type;
    t.text = n.type == TokType::Func ? n.name : std::string(1, n.op);
    t.op = n.op;
    t.unary = n.unary;
    t.implicit = n.implicit;
    t.arity = n.type == TokType::Func ? (n.right >= 0 ? 2 : 1) : 0;
    t.prec = n.type == TokType::Func ? 5 : (n.unary ? kUnaryPrecedence : precedenceOf(n.op));
    t.pos = n.pos;
    t.len = n.len;
    out.push_back(t);
}

// ---------------------------------------------------------------------------
// notation for display
// ---------------------------------------------------------------------------
inline std::string infix(const Ast& tree, int idx) {
    if (idx < 0) return "";
    const AstNode& n = tree.nodes[static_cast<std::size_t>(idx)];
    if (n.type == TokType::Number) return n.folded || n.text.empty() ? formatNumber(n.value) : n.text;
    if (n.type == TokType::Variable) return n.name;
    if (n.type == TokType::Op && n.unary) return std::string(1, n.op) + infix(tree, n.left);
    if (n.type == TokType::Func) {
        if (n.right >= 0)
            return n.name + "(" + infix(tree, n.left) + ", " + infix(tree, n.right) + ")";
        return n.name + "(" + infix(tree, n.left) + ")";
    }
    return "(" + infix(tree, n.left) + " " + std::string(1, n.op) + " " + infix(tree, n.right) + ")";
}

inline std::string prefix(const Ast& tree, int idx) {
    if (idx < 0) return "";
    const AstNode& n = tree.nodes[static_cast<std::size_t>(idx)];
    if (n.type == TokType::Number) return n.folded || n.text.empty() ? formatNumber(n.value) : n.text;
    if (n.type == TokType::Variable) return n.name;
    if (n.type == TokType::Op && n.unary) return std::string(1, n.op) + " " + prefix(tree, n.left);
    if (n.type == TokType::Func) {
        std::string args = prefix(tree, n.left);
        if (n.right >= 0) args += " " + prefix(tree, n.right);
        return n.name + (args.empty() ? "" : " " + args);
    }
    return std::string(1, n.op) + " " + prefix(tree, n.left) + " " + prefix(tree, n.right);
}

inline int computeHeight(const Ast& tree, int idx) {
    if (idx < 0) return 0;
    const AstNode& n = tree.nodes[static_cast<std::size_t>(idx)];
    const int l = computeHeight(tree, n.left);
    const int r = computeHeight(tree, n.right);
    return 1 + (l > r ? l : r);
}

inline int countNodes(const Ast& tree, int idx) {
    if (idx < 0) return 0;
    const AstNode& n = tree.nodes[static_cast<std::size_t>(idx)];
    return 1 + countNodes(tree, n.left) + countNodes(tree, n.right);
}

}  // namespace ast
