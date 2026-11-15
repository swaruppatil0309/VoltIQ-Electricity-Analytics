// main.cpp — VoltIQ Expr · Expression Solver & Visualizer (DSA, C++17)
//
// CLI
//   ./xpr convert "3+4*2"      infix → postfix with a stack trace
//   ./xpr eval    "3+4*2"      evaluate the postfix program
//   ./xpr trace   "3+4*2"      both traces, side by side
//   ./xpr tree    "3+4*2"      the expression tree + walks
//   ./xpr analyze "3+4*2"      full analysis as JSON (used by the parity test)
//   ./xpr repl                 interactive shell
//   ./xpr bench                empirical complexity benchmarks
//   ./xpr selftest             30+ invariant assertions
//   ./xpr serve [port]         JSON HTTP engine for the dashboard (default 8003)
//
// Build:  g++ -std=c++17 -O2 -pthread -Wall -Wextra -o bin/xpr *.cpp
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "engine.hpp"
#include "json.hpp"

using Clock = std::chrono::steady_clock;

static double microsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// ===========================================================================
// JSON serialisation
// ===========================================================================
static std::string jsonTokens(const std::vector<Token>& tokens) {
    std::ostringstream o;
    o << "[";
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (i) o << ",";
        o << "{\"i\":" << i
          << ",\"text\":" << js::q(t.implicit ? "\xC2\xB7" : t.text)
          << ",\"type\":" << js::q(tokenTypeName(t.type))
          << ",\"pos\":" << t.pos
          << ",\"len\":" << t.len
          << ",\"prec\":" << (t.unary ? kUnaryPrecedence : t.prec)
          << ",\"implicit\":" << (t.implicit ? "true" : "false")
          << ",\"folded\":" << (t.folded ? "true" : "false")
          << ",\"unary\":" << (t.unary ? "true" : "false")
          << ",\"arity\":" << t.arity
          << ",\"value\":" << js::value(t.value) << "}";
    }
    o << "]";
    return o.str();
}

static std::string jsonStringList(const std::vector<std::string>& items) {
    std::ostringstream o;
    o << "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) o << ",";
        o << js::q(items[i]);
    }
    o << "]";
    return o.str();
}

static std::string jsonConvert(const ConvertResult& c) {
    std::ostringstream o;
    o << "{\"ok\":" << (c.ok ? "true" : "false")
      << ",\"rpn\":" << js::q(shunting::rpnText(c.rpn))
      << ",\"peakStack\":" << c.peakStack
      << ",\"steps\":[";
    for (std::size_t i = 0; i < c.steps.size(); ++i) {
        const ConvertStep& s = c.steps[i];
        if (i) o << ",";
        o << "{\"step\":" << s.step
          << ",\"token\":" << js::q(s.token)
          << ",\"action\":" << js::q(s.action)
          << ",\"note\":" << js::q(s.note)
          << ",\"stack\":" << jsonStringList(s.stack)
          << ",\"output\":" << jsonStringList(s.output)
          << ",\"tokenIndex\":" << s.tokenIndex << "}";
    }
    o << "]}";
    return o.str();
}

static std::string jsonEval(const EvalResult& e) {
    std::ostringstream o;
    o << "{\"ok\":" << (e.ok ? "true" : "false")
      << ",\"value\":" << (e.ok ? js::value(e.value) : "null")
      << ",\"valueText\":" << js::q(e.ok ? formatNumber(e.value) : "")
      << ",\"peakStack\":" << e.peakStack
      << ",\"opsApplied\":" << e.opsApplied
      << ",\"steps\":[";
    for (std::size_t i = 0; i < e.trace.size(); ++i) {
        const EvalStep& s = e.trace[i];
        if (i) o << ",";
        o << "{\"step\":" << s.step
          << ",\"tokenIndex\":" << s.tokenIndex
          << ",\"token\":" << js::q(s.token)
          << ",\"action\":" << js::q(s.action)
          << ",\"note\":" << js::q(s.note)
          << ",\"value\":" << js::q(s.value)
          << ",\"stack\":" << jsonStringList(s.stack) << "}";
    }
    o << "]}";
    return o.str();
}

static std::string jsonAst(const Ast& tree) {
    std::ostringstream o;
    o << "{\"root\":" << tree.root
      << ",\"nodeCount\":" << ast::countNodes(tree, tree.root)
      << ",\"height\":" << ast::computeHeight(tree, tree.root)
      << ",\"foldedNodes\":" << tree.foldedNodes
      << ",\"prefix\":" << js::q(ast::prefix(tree, tree.root))
      << ",\"infix\":" << js::q(ast::infix(tree, tree.root))
      << ",\"nodes\":[";
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        const AstNode& n = tree.nodes[i];
        if (i) o << ",";
        std::string label;
        if (n.type == TokType::Number) label = n.folded || n.text.empty() ? formatNumber(n.value) : n.text;
        else if (n.type == TokType::Variable) label = n.name;
        else if (n.type == TokType::Func) label = n.name;
        else label = n.unary ? std::string(1, n.op) + "u" : std::string(1, n.op);

        o << "{\"id\":" << i
          << ",\"type\":" << js::q(tokenTypeName(n.type))
          << ",\"label\":" << js::q(label)
          << ",\"left\":" << n.left
          << ",\"right\":" << n.right
          << ",\"folded\":" << (n.folded ? "true" : "false")
          << ",\"unary\":" << (n.unary ? "true" : "false")
          << ",\"implicit\":" << (n.implicit ? "true" : "false")
          << ",\"pos\":" << n.pos << "}";
    }
    o << "]}";
    return o.str();
}

static std::string jsonStats(const AnalysisStats& s) {
    std::ostringstream o;
    o << "{\"tokens\":" << s.tokens
      << ",\"rawInstructions\":" << s.rawInstructions
      << ",\"instructions\":" << s.instructions
      << ",\"foldedConstants\":" << s.foldedConstants
      << ",\"foldedSubtrees\":" << s.foldedSubtrees
      << ",\"implicitMuls\":" << s.implicitMuls
      << ",\"astNodes\":" << s.astNodes
      << ",\"astHeight\":" << s.astHeight
      << ",\"peakOpStack\":" << s.peakOpStack
      << ",\"peakEvalStack\":" << s.peakEvalStack
      << ",\"traceSteps\":" << s.traceSteps
      << ",\"opsApplied\":" << s.opsApplied
      << ",\"compileUs\":" << js::num(s.compileUs, 3)
      << ",\"evalUs\":" << js::num(s.evalUs, 3)
      << ",\"cacheHit\":" << (s.cacheHit ? "true" : "false") << "}";
    return o.str();
}

static std::string jsonEngineState(const Engine& engine) {
    const EngineStats& s = engine.stats();
    std::ostringstream o;
    o << "{\"compiles\":" << s.compiles
      << ",\"cacheHits\":" << s.cacheHits
      << ",\"cacheMisses\":" << s.cacheMisses
      << ",\"evals\":" << s.evals
      << ",\"foldedConstants\":" << s.foldedConstants
      << ",\"tokensLexed\":" << s.tokensLexed
      << ",\"cacheSize\":" << engine.cache().size()
      << ",\"cacheCapacity\":" << engine.cache().capacity()
      << ",\"cacheOrder\":" << jsonStringList(engine.cache().keys())
      << ",\"vars\":{";
    auto entries = engine.vars().entries();
    std::sort(entries.begin(), entries.end());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) o << ",";
        o << js::q(entries[i].first) << ":" << js::value(entries[i].second);
    }
    o << "}}";
    return o.str();
}

static std::string jsonAnalysis(const Engine& engine, const Analysis& a) {
    std::ostringstream o;
    o << "{\"ok\":" << (a.ok ? "true" : "false")
      << ",\"expr\":" << js::q(a.expr)
      << ",\"normalized\":" << js::q(a.ok ? a.normalized : "")
      << ",\"tokens\":" << jsonTokens(a.tokens)
      // note: tokens are still reported when only evaluation failed
      << ",\"program\":{\"raw\":" << js::q(shunting::rpnText(a.rawRpn))
      << ",\"active\":" << js::q(shunting::rpnText(a.rpn))
      << ",\"folded\":" << js::q(shunting::rpnText(a.foldedRpn))
      << ",\"foldingActive\":" << (a.foldingActive ? "true" : "false") << "}"
      << ",\"convert\":" << jsonConvert(a.convert)
      << ",\"eval\":" << jsonEval(a.eval)
      << ",\"ast\":" << jsonAst(a.tree)
      << ",\"stats\":" << jsonStats(a.stats)
      << ",\"engine\":" << jsonEngineState(engine)
      << ",\"error\":";
    if (a.ok) o << "null";
    else
        o << "{\"message\":" << js::q(a.error) << ",\"pos\":" << a.errorPos
          << ",\"len\":" << a.errorLen << "}";
    o << "}";
    return o.str();
}

// a caret line under the offending character — used by the CLI
static std::string caretLine(const std::string& expr, int pos, int len) {
    std::string line;
    for (int i = 0; i < pos && i < static_cast<int>(expr.size()); ++i) line += (expr[static_cast<std::size_t>(i)] == '\t' ? '\t' : ' ');
    line += '^';
    for (int i = 1; i < (len > 0 ? len : 1); ++i) line += '~';
    return line;
}

// ===========================================================================
// CLI rendering
// ===========================================================================
static const char* kRule = "------------------------------------------------------------------------------";

static void printTokens(const Analysis& a) {
    std::cout << "Tokens (" << a.tokens.size() << ")\n";
    std::cout << "  +------+------------+\n";
    for (std::size_t i = 0; i < a.tokens.size(); ++i) {
        const Token& t = a.tokens[i];
        std::string text = t.implicit ? "*" : t.text;
        std::cout << "  | " << std::setw(4) << i << " | " << std::left << std::setw(10) << text
                  << std::right << " | " << tokenTypeName(t.type) << "\n";
    }
    std::cout << "  +------+------------+\n";
}

static void printConvert(const Analysis& a) {
    const ConvertResult& c = a.convert;
    if (!c.ok) return;
    std::cout << "\nInfix → Postfix  (shunting-yard, " << c.steps.size() << " stack operations)\n";
    std::cout << "  " << std::left << std::setw(5) << "step" << std::setw(16) << "token"
              << std::setw(9) << "action" << std::setw(26) << "operator stack" << "output queue\n";
    std::cout << "  " << kRule << "\n";
    for (const ConvertStep& s : c.steps) {
        std::string stack;
        for (std::size_t i = 0; i < s.stack.size(); ++i) stack += (i ? " " : "") + s.stack[i];
        if (stack.empty()) stack = "—";
        std::string out;
        for (std::size_t i = 0; i < s.output.size(); ++i) out += (i ? " " : "") + s.output[i];
        if (out.empty()) out = "—";
        std::cout << "  " << std::left << std::setw(5) << s.step << std::setw(16)
                  << (s.token.empty() ? "(auto)" : s.token) << std::setw(9) << s.action
                  << std::setw(26) << stack << out << "\n";
    }
    std::cout << "  " << kRule << "\n";
    std::cout << "  postfix: " << shunting::rpnText(c.rpn) << "   (peak stack depth " << c.peakStack << ")\n";
}

static void printEval(const Analysis& a) {
    const EvalResult& e = a.eval;
    if (e.trace.empty()) return;
    std::cout << "\nPostfix evaluation  (stack machine, " << e.trace.size() << " operations)\n";
    std::cout << "  " << std::left << std::setw(5) << "step" << std::setw(16) << "token"
              << std::setw(28) << "operand stack" << "what happened\n";
    std::cout << "  " << kRule << "\n";
    for (const EvalStep& s : e.trace) {
        std::string stack;
        for (std::size_t i = 0; i < s.stack.size(); ++i) stack += (i ? " " : "") + s.stack[i];
        if (stack.empty()) stack = "—";
        std::cout << "  " << std::left << std::setw(5) << s.step << std::setw(16) << s.token
                  << std::setw(28) << stack << s.note << "\n";
    }
    std::cout << "  " << kRule << "\n";
    std::cout << (e.ok ? "  result: " : "  error: ")
              << (e.ok ? formatNumber(e.value) : e.error) << "\n";
}

static void printTreeRec(const Ast& tree, int idx, const std::string& prefix, bool last, bool root) {
    if (idx < 0) return;
    const AstNode& n = tree.nodes[static_cast<std::size_t>(idx)];
    std::string label;
    if (n.type == TokType::Number) label = n.folded || n.text.empty() ? formatNumber(n.value) : n.text;
    else if (n.type == TokType::Variable) label = n.name;
    else if (n.type == TokType::Func) label = n.name + "()";
    else label = n.unary ? std::string(1, n.op) + " (unary)" : std::string(1, n.op);

    if (root) std::cout << "  " << label << "\n";
    else std::cout << "  " << prefix << (last ? "└── " : "├── ") << label
                   << (n.folded ? "   ← folded at compile time" : "") << "\n";

    const std::string childPrefix = root ? "  " : prefix + (last ? "    " : "│   ");
    const bool hasLeft = n.left >= 0;
    const bool hasRight = n.right >= 0;
    if (hasLeft) printTreeRec(tree, n.left, childPrefix, !hasRight, false);
    if (hasRight) printTreeRec(tree, n.right, childPrefix, true, false);
}

static void printTree(const Analysis& a) {
    if (a.tree.root < 0) return;
    std::cout << "\nExpression tree (" << ast::countNodes(a.tree, a.tree.root) << " nodes, height "
              << ast::computeHeight(a.tree, a.tree.root) << ")\n";
    printTreeRec(a.tree, a.tree.root, "", true, true);
    std::cout << "\n  infix   : " << ast::infix(a.tree, a.tree.root) << "\n"
              << "  prefix  : " << ast::prefix(a.tree, a.tree.root) << "\n"
              << "  postfix : " << shunting::rpnText(a.rpn) << "\n";
}

static void printProgram(const Analysis& a) {
    std::cout << "\nCompiled program\n"
              << "  shunting-yard output : " << shunting::rpnText(a.rawRpn) << "   ("
              << a.stats.rawInstructions << " instructions)\n"
              << "  constant folding     : " << shunting::rpnText(a.foldedRpn) << "   ("
              << static_cast<int>(a.foldedRpn.size()) << " instructions";
    if (a.stats.foldedConstants > 0)
        std::cout << ", " << a.stats.foldedConstants << " removed";
    std::cout << ")\n"
              << "  program in use       : " << shunting::rpnText(a.rpn) << "   ("
              << a.stats.instructions << " instructions, folding "
              << (a.foldingActive ? "ON" : "OFF") << ")\n";
    if (a.stats.implicitMuls > 0)
        std::cout << "  implicit ×           : " << a.stats.implicitMuls << " inserted\n";
}

static void printError(const Analysis& a) {
    std::cout << "\n  error: " << a.error << "\n";
    if (!a.expr.empty()) {
        std::cout << "    " << a.expr << "\n    " << caretLine(a.expr, a.errorPos, a.errorLen) << "\n";
    }
}

static void printFullAnalysis(const Analysis& a, bool withSteps) {
    std::cout << "\n  expression : " << a.expr << "\n";
    if (!a.ok && a.tokens.empty()) {
        printError(a);
        return;
    }
    printTokens(a);
    if (withSteps) printConvert(a);
    else if (a.convert.ok) std::cout << "\n  postfix: " << shunting::rpnText(a.rawRpn) << "\n";
    printProgram(a);
    if (withSteps) printEval(a);
    else if (a.eval.ok) std::cout << "\n  result: " << formatNumber(a.eval.value) << "\n";
    printTree(a);
    if (!a.ok) printError(a);
    std::cout << "\n  cache: " << (a.stats.cacheHit ? "HIT" : "MISS")
              << " · compile " << std::fixed << std::setprecision(2) << a.stats.compileUs
              << " µs · evaluate " << a.stats.evalUs << " µs\n";
}

// ===========================================================================
// interactive shell
// ===========================================================================
static void repl(Engine& engine) {
    bool steps = true;
    std::cout << "VoltIQ Expr REPL — type an expression, or :help\n";
    std::string line;
    while (true) {
        std::cout << (steps ? "expr> " : "expr(quiet)> ");
        std::cout.flush();
        if (!std::getline(std::cin, line)) break;
        // trim
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line = line.substr(first, line.find_last_not_of(" \t") - first + 1);

        if (line == ":quit" || line == ":q" || line == "quit" || line == "exit") break;
        if (line == ":help") {
            std::cout << "  <expression>        evaluate (e.g. 3+4*2, sin(x), 2(3+4)^2)\n"
                         "  :steps              toggle the step-by-step traces\n"
                         "  :vars               list variables\n"
                         "  :set x=2 y=3        define variables\n"
                         "  :tree <expression>  show the expression tree\n"
                         "  :cache              show the LRU cache state\n"
                         "  :clear              clear variables and the cache\n"
                         "  :quit\n";
            continue;
        }
        if (line == ":steps") {
            steps = !steps;
            std::cout << "  traces " << (steps ? "on" : "off") << "\n";
            continue;
        }
        if (line == ":vars") {
            auto entries = engine.vars().entries();
            std::sort(entries.begin(), entries.end());
            if (entries.empty()) std::cout << "  (no variables)\n";
            for (const auto& e : entries) std::cout << "  " << e.first << " = " << formatNumber(e.second) << "\n";
            continue;
        }
        if (line.rfind(":set", 0) == 0) {
            std::istringstream is(line.substr(4));
            std::string assign;
            int count = 0;
            while (is >> assign) {
                const auto eq = assign.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                try {
                    engine.setVar(assign.substr(0, eq), std::stod(assign.substr(eq + 1)));
                    ++count;
                } catch (...) {
                    std::cout << "  could not parse " << assign << "\n";
                }
            }
            std::cout << "  " << count << " variable(s) set\n";
            continue;
        }
        if (line == ":cache") {
            std::cout << "  size " << engine.cache().size() << "/" << engine.cache().capacity()
                      << " · hits " << engine.cache().hits() << " · misses " << engine.cache().misses() << "\n";
            for (const std::string& k : engine.cache().keys()) std::cout << "    " << k << "\n";
            continue;
        }
        if (line == ":clear") {
            engine.clearVars();
            engine.resetCache();
            std::cout << "  cleared\n";
            continue;
        }
        if (line.rfind(":tree", 0) == 0) {
            Analysis a = engine.analyze(line.substr(5), false);
            if (a.tree.root >= 0) printTree(a);
            else printError(a);
            continue;
        }
        if (line[0] == ':') {
            std::cout << "  unknown command, try :help\n";
            continue;
        }

        Analysis a = engine.analyze(line, steps);
        if (a.ok) {
            std::cout << "  = " << formatNumber(a.eval.value)
                      << "    [" << a.stats.instructions << " instructions, postfix "
                      << shunting::rpnText(a.rpn) << "]\n";
            if (steps) {
                printConvert(a);
                printEval(a);
            }
            if (a.stats.foldedConstants > 0)
                std::cout << "  constant folding " << (a.foldingActive ? "removed " : "would remove ")
                          << a.stats.foldedConstants << " instruction(s)\n";
        } else {
            printError(a);
            if (!a.eval.trace.empty()) printEval(a);
        }
    }
    std::cout << "\nbye\n";
}

// ===========================================================================
// self test
// ===========================================================================
static int selfTest() {
    int failures = 0;
    auto check = [&](bool cond, const std::string& label) {
        std::cout << (cond ? "  PASS  " : "  FAIL  ") << label << "\n";
        if (!cond) ++failures;
    };

    std::cout << "VoltIQ Expr — expression solver self test\n";
    std::cout << kRule << "\n";

    auto postfixOf = [](const std::string& expr, std::string& out) {
        TokenizeResult tk = lexer::run(expr);
        if (!tk.ok) return false;
        ConvertResult cv = shunting::run(tk.tokens);
        if (!cv.ok) return false;
        out = shunting::rpnText(cv.rpn);
        return true;
    };
    auto valueOf = [](Engine& e, const std::string& expr, double& out) {
        std::string err;
        return e.evaluate(expr, out, err);
    };

    // ---- shunting-yard ---------------------------------------------------
    Engine engine(4);  // deliberately tiny cache for the eviction test
    std::string pf;
    check(postfixOf("3+4*2", pf) && pf == "3 4 2 * +", "3+4*2  →  3 4 2 * +");
    check(postfixOf("(3+4)*2", pf) && pf == "3 4 + 2 *", "(3+4)*2  →  3 4 + 2 *");
    check(postfixOf("2^3^2", pf) && pf == "2 3 2 ^ ^", "right-associative ^  →  2 3 2 ^ ^");
    check(postfixOf("-2^2", pf) && pf == "2 2 ^ -u", "unary minus binds looser than ^");
    check(postfixOf("2^-2", pf) && pf == "2 2 -u ^", "2^-2 survives the unary/right-assoc corner");
    check(postfixOf("(1+2)(3+4)", pf) && pf == "1 2 + 3 4 + *", "implicit × between parentheses");
    check(postfixOf("3(4+5)", pf) && pf == "3 4 5 + *", "implicit × before '('");
    check(postfixOf("sqrt(81)+max(2,7)", pf) && pf == "81 sqrt() 2 7 max() +", "function calls with arity");
    check(postfixOf("-3*2", pf) && pf == "3 -u 2 *", "unary minus applies before *");

    // ---- evaluation ------------------------------------------------------
    double v = 0;
    check(valueOf(engine, "3+4*2", v) && v == 11, "3+4*2 = 11");
    check(valueOf(engine, "(3+4)*2", v) && v == 14, "(3+4)*2 = 14");
    check(valueOf(engine, "2^3^2", v) && v == 512, "2^3^2 = 512");
    check(valueOf(engine, "-2^2", v) && v == -4, "-2^2 = -4");
    check(valueOf(engine, "10/4", v) && v == 2.5, "10/4 = 2.5");
    check(valueOf(engine, "10%3", v) && v == 1, "10%3 = 1");
    check(valueOf(engine, "sqrt(81)+abs(-4)", v) && v == 13, "sqrt(81)+abs(-4) = 13");
    check(valueOf(engine, "2^10", v) && v == 1024, "2^10 = 1024");
    check(valueOf(engine, "min(3,8)*max(2,5)", v) && v == 15, "nested two-argument functions");

    engine.setVar("x", 5);
    check(valueOf(engine, "2x+1", v) && v == 11, "variable lookup: 2x+1 = 11 with x=5");
    check(valueOf(engine, "x(x+1)", v) && v == 30, "implicit × with a variable: x(x+1) = 30");
    engine.setVar("rate", 0.18);
    check(valueOf(engine, "1000(1+rate)", v) && std::fabs(v - 1180.0) < 1e-9, "multi-character variable name");
    engine.vars().erase("rate");
    check(valueOf(engine, "1000(1+rate)", v) == false, "deleted variable reports an error");

    // ---- error handling --------------------------------------------------
    Engine errs(4);
    Analysis bad = errs.analyze("3+", false);
    check(!bad.ok && !bad.error.empty(), "trailing operator is rejected");
    bad = errs.analyze("(3+4", false);
    check(!bad.ok && bad.error.find("unclosed") != std::string::npos, "unclosed '(' is reported");
    bad = errs.analyze("3/0", false);
    check(!bad.ok && bad.error.find("division by zero") != std::string::npos, "division by zero is reported");
    bad = errs.analyze("q+1", false);
    check(!bad.ok && bad.error.find("unknown variable") != std::string::npos, "unknown variable is reported");
    bad = errs.analyze("sin(1,2)", false);
    check(!bad.ok && bad.error.find("expects 1 argument") != std::string::npos, "function arity is checked");
    bad = errs.analyze("3 4", false);
    check(!bad.ok, "two numbers in a row are rejected");
    bad = errs.analyze("3 $ 4", false);
    check(!bad.ok && bad.errorPos == 2, "invalid character reports its column");
    bad = errs.analyze("", false);
    check(!bad.ok, "empty input is rejected");

    // ---- constant folding ------------------------------------------------
    Engine fold(8);
    fold.setFolding(true);
    fold.setVar("x", 4);
    Analysis folded = fold.analyze("2*3 + x", false);
    check(folded.ok && folded.stats.foldedConstants == 2, "2*3 + x folds 5 instructions into 3");
    check(shunting::rpnText(folded.rpn) == "6 x +", "folded program is 6 x +");
    Analysis allConst = fold.analyze("2+3*4", false);
    check(allConst.stats.instructions == 1 && allConst.eval.ok && allConst.eval.value == 14,
          "a fully constant expression collapses to a single instruction");
    check(allConst.stats.foldedSubtrees >= 2, "folding is applied bottom-up over several nodes");
    Analysis noFold = fold.analyze("x + y", false);
    check(noFold.stats.foldedConstants == 0, "nothing is folded when only variables are present");
    Engine noFoldEngine(8);
    Analysis off = noFoldEngine.analyze("2*3 + x", false);
    check(!off.foldingActive && shunting::rpnText(off.rpn) == "2 3 * x +",
          "with folding off the shunting-yard program runs unchanged");
    check(shunting::rpnText(off.foldedRpn) == "6 x +", "the folded preview is still available");

    // folding must never change the answer
    {
        struct Case { const char* expr; double x; double y; };
        const Case cases[] = {
            {"x^2 + 2x + 1", 3, 0},   {"(x+y)*(x-y)", 7, 4},
            {"2*3*x + 4*5*y", 2, 3},  {"sqrt(16)*x + 10/2", 3, 0},
            {"-x^2 + 100", 4, 0},     {"max(x, 2*3) + min(y, 1+1)", 9, 1},
        };
        Engine probe(32);
        probe.setFolding(true);
        probe.setVar("x", 0);
        probe.setVar("y", 0);
        bool stable = true;
        for (const Case& c : cases) {
            probe.setVar("x", c.x);
            probe.setVar("y", c.y);
            Analysis a = probe.analyze(c.expr, false);
            bool naiveOk = true;
            const double naive = probe.evaluateNaive(c.expr, naiveOk);
            if (!a.ok || !naiveOk || std::fabs(a.eval.value - naive) > 1e-9) stable = false;
        }
        check(stable, "folded and naive pipelines agree on 6 mixed expressions");
    }

    // ---- LRU cache -------------------------------------------------------
    Engine cache(2);
    cache.analyze("1+1", false);
    Analysis second = cache.analyze("1+1", false);
    check(second.stats.cacheHit, "the second analysis of the same text is a cache hit");
    cache.analyze("2+2", false);
    cache.analyze("3+3", false);  // evicts 1+1 (LRU)
    check(cache.cache().size() == 2, "LRU cache honours its capacity");
    check(cache.cache().keys().front() == "3+3", "most recently used expression is at the front");
    Analysis evicted = cache.analyze("1+1", false);
    check(!evicted.stats.cacheHit, "evicted expression is recompiled");

    // ---- cache must not cache errors wrongly -----------------------------
    Engine ec(4);
    Analysis e1 = ec.analyze("1/0", false);
    Analysis e2 = ec.analyze("1/0", false);
    check(!e1.ok && !e2.ok, "runtime errors are reported consistently (cached program)");
    ec.setVar("z", 0);
    Analysis e3 = ec.analyze("1/z", false);
    check(!e3.ok, "division by zero through a variable is caught");
    ec.setVar("z", 4);
    Analysis e4 = ec.analyze("1/z", false);
    check(e4.ok && std::fabs(e4.eval.value - 0.25) < 1e-12, "the same program re-evaluates with the new variable");

    // ---- hand-written data structures ------------------------------------
    ArrayStack<int> stack;
    for (int i = 0; i < 1000; ++i) stack.push(i);
    check(stack.size() == 1000 && stack.capacity() >= 1000, "ArrayStack grows past 1000 elements");
    check(stack.top() == 999, "ArrayStack top is the last pushed value");
    int sum = 0;
    while (!stack.empty()) sum += stack.popValue();
    check(sum == 499500 && stack.empty(), "ArrayStack pops in LIFO order");

    RingQueue<int> queue;
    for (int i = 0; i < 100; ++i) queue.enqueue(i);
    bool fifo = true;
    for (int i = 0; i < 100; ++i) fifo = fifo && (queue.dequeue() == i);
    check(fifo && queue.empty(), "RingQueue is FIFO across a growth boundary");

    VarMap map;
    for (int i = 0; i < 2000; ++i) map.put("v" + std::to_string(i), i * 0.5);
    bool allFound = true;
    for (int i = 0; i < 2000; ++i) {
        double got = 0;
        if (!map.get("v" + std::to_string(i), got) || std::fabs(got - i * 0.5) > 1e-12) allFound = false;
    }
    check(allFound, "hash map returns all 2000 inserted variables");
    check(map.loadFactor() <= 0.7, "hash map keeps its load factor below 0.7");
    check(!map.get("nope", v), "hash map reports a miss for an unknown key");
    map.erase("v7");
    check(!map.get("v7", v) && map.size() == 1999, "hash map delete works with tombstones");
    map.put("v7", 1.0);
    check(map.get("v7", v) && v == 1.0, "hash map reuses a tombstone slot");

    // ---- trace integrity -------------------------------------------------
    Engine tracer(8);
    Analysis t = tracer.analyze("3+4*2", true);
    check(t.convert.ok && t.convert.steps.size() >= 6, "conversion trace records every stack operation");
    check(t.eval.ok && t.eval.trace.size() == 5, "evaluation trace has one step per instruction");
    check(!t.foldingActive && shunting::rpnText(t.rpn) == "3 4 2 * +", "the traced program is the raw one");
    check(t.stats.peakOpStack == 2 && t.stats.peakEvalStack == 3, "peak stack depths are reported");
    check(t.eval.trace.back().stack.size() == 1, "a finished evaluation leaves exactly one value");

    std::cout << kRule << "\n";
    std::cout << (failures == 0 ? "ALL TESTS PASSED" : std::to_string(failures) + " TEST(S) FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}

// ===========================================================================
// benchmarks
// ===========================================================================
static std::string runBenchmark() {
    Engine engine(256);
    engine.setFolding(true);
    engine.setVar("x", 6);
    engine.setVar("y", 2);

    const int N = 50000;
    const std::string expr = "((3+4)*2 - 5) / 7 + 2^3*2 - sqrt(81) + x";
    const std::string foldedExpr =
        "2*3 + 4*5 - 6/2 + 7*8 - 9*1 + 2^5 + sqrt(16) - abs(-3) + max(2,7) + x";

    double sink = 0.0;
    // warm up
    for (int i = 0; i < 2000; ++i) {
        bool ok = true;
        sink += engine.evaluateNaive(expr, ok);
    }

    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        bool ok = true;
        sink += engine.evaluateNaive(expr, ok);
    }
    const double naiveUs = microsSince(t0) / N;

    auto t1 = Clock::now();
    for (int i = 0; i < N; ++i) {
        double val = 0.0;
        std::string err;
        if (engine.evaluate(expr, val, err)) sink += val;
    }
    const double cachedUs = microsSince(t1) / N;

    Analysis a = engine.analyze(expr, false);
    Analysis f = engine.analyze(foldedExpr, false);

    // folding benchmark: unfurled pipeline vs compiled program
    auto t3 = Clock::now();
    for (int i = 0; i < N; ++i) {
        bool ok = true;
        sink += engine.evaluateNaive(foldedExpr, ok);
    }
    const double foldNaiveUs = microsSince(t3) / N;

    auto t4 = Clock::now();
    for (int i = 0; i < N; ++i) {
        double val = 0.0;
        std::string err;
        if (engine.evaluate(foldedExpr, val, err)) sink += val;
    }
    const double foldFastUs = microsSince(t4) / N;

    // LRU cache: 400 distinct expressions, two passes
    std::vector<std::string> pool;
    for (int i = 0; i < 400; ++i) pool.push_back("(" + std::to_string(i) + "+1)*2 - sqrt(16) + x");
    auto t5 = Clock::now();
    for (int pass = 0; pass < 3; ++pass) {
        for (const std::string& e : pool) {
            double val = 0.0;
            std::string err;
            if (engine.evaluate(e, val, err)) sink += val;
        }
    }
    const double poolUs = microsSince(t5) / (3.0 * static_cast<double>(pool.size()));

    // tokenizer scaling
    std::ostringstream scaling;
    scaling << "[";
    bool firstScale = true;
    for (int terms : {16, 256, 4096}) {
        std::string longExpr = "1";
        for (int i = 0; i < terms; ++i) longExpr += "+1";
        Engine scaleEngine(8);
        const int reps = terms <= 256 ? 600 : 40;
        auto s0 = Clock::now();
        volatile double acc = 0;
        for (int i = 0; i < reps; ++i) {
            bool ok = true;
            acc = acc + scaleEngine.evaluateNaive(longExpr, ok);
        }
        const double us = microsSince(s0) / reps;
        if (!firstScale) scaling << ",";
        firstScale = false;
        scaling << "{\"terms\":" << (terms + 1) << ",\"tokens\":" << (2 * terms + 1)
                << ",\"micros\":" << js::num(us, 2) << ",\"perTokenUs\":"
                << js::num(us / static_cast<double>(2 * terms + 1), 4) << "}";
    }
    scaling << "]";

    (void)sink;
    std::ostringstream o;
    o << "{\"ok\":true,\"iterations\":" << N
      << ",\"rows\":["
      << "{\"label\":\"Evaluate one expression " << N
      << " times\",\"fastLabel\":\"cached compiled program\",\"fastUs\":" << js::num(cachedUs, 4)
      << ",\"slowLabel\":\"re-tokenize + re-parse every time\",\"slowUs\":" << js::num(naiveUs, 4)
      << ",\"speedup\":" << js::num(naiveUs / (cachedUs > 0 ? cachedUs : 1e-9), 2)
      << ",\"note\":\"LRU cache removes lexing and parsing from the hot path\"},"
      << "{\"label\":\"Constant-heavy expression\",\"fastLabel\":\"compiled + constant folding\",\"fastUs\":"
      << js::num(foldFastUs, 4) << ",\"slowLabel\":\"naive pipeline\",\"slowUs\":" << js::num(foldNaiveUs, 4)
      << ",\"speedup\":" << js::num(foldNaiveUs / (foldFastUs > 0 ? foldFastUs : 1e-9), 2)
      << ",\"note\":\"" << f.stats.rawInstructions << " instructions folded down to "
      << f.stats.instructions << "\"},"
      << "{\"label\":\"400 distinct expressions, cache warm\",\"fastLabel\":\"cached program\",\"fastUs\":"
      << js::num(poolUs, 4) << ",\"slowLabel\":\"re-parse\",\"slowUs\":"
      << js::num(naiveUs, 4) << ",\"speedup\":" << js::num(naiveUs / (poolUs > 0 ? poolUs : 1e-9), 2)
      << ",\"note\":\"LRU capacity " << engine.cache().capacity() << ", hit rate "
      << js::num(100.0 * static_cast<double>(engine.cache().hits()) /
                     static_cast<double>(engine.cache().hits() + engine.cache().misses() + 1e-9), 1)
      << "%\"}"
      << "]"
      << ",\"example\":{\"expr\":" << js::q(expr)
      << ",\"raw\":" << js::q(shunting::rpnText(a.rawRpn))
      << ",\"optimized\":" << js::q(shunting::rpnText(a.rpn))
      << ",\"result\":" << js::q(formatNumber(a.eval.value)) << "}"
      << ",\"foldedExample\":{\"expr\":" << js::q(foldedExpr)
      << ",\"raw\":" << js::q(shunting::rpnText(f.rawRpn))
      << ",\"optimized\":" << js::q(shunting::rpnText(f.rpn))
      << ",\"result\":" << js::q(formatNumber(f.eval.value)) << "}"
      << ",\"scaling\":" << scaling.str()
      << ",\"folding\":{\"enabled\":true,\"rawInstructions\":" << f.stats.rawInstructions
      << ",\"optimizedInstructions\":" << f.stats.instructions
      << ",\"foldedSubtrees\":" << f.stats.foldedSubtrees << "}}";
    return o.str();
}

// ===========================================================================
// tiny HTTP server (POSIX sockets)
// ===========================================================================
namespace http {

struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

inline Request parse(const std::string& raw) {
    Request r;
    const std::size_t lineEnd = raw.find("\r\n");
    std::string first = raw.substr(0, lineEnd == std::string::npos ? raw.size() : lineEnd);
    std::istringstream is(first);
    std::string target;
    is >> r.method >> target;
    const std::size_t q = target.find('?');
    if (q != std::string::npos) {
        r.path = target.substr(0, q);
        r.query = target.substr(q + 1);
    } else {
        r.path = target;
    }
    const std::size_t bodyStart = raw.find("\r\n\r\n");
    if (bodyStart != std::string::npos) r.body = raw.substr(bodyStart + 4);
    return r;
}

inline std::string queryParam(const std::string& query, const std::string& key) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t eq = query.find('=', pos);
        std::size_t amp = query.find('&', pos);
        if (eq == std::string::npos) break;
        const std::string k = query.substr(pos, eq - pos);
        const std::string v = query.substr(eq + 1, (amp == std::string::npos ? query.size() : amp) - eq - 1);
        if (k == key) return v;
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

// --- minimal JSON field readers (the dashboard only sends flat objects) -----
inline std::string strField(const std::string& body, const std::string& key, const std::string& def = "") {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = body.find(needle);
    if (p == std::string::npos) return def;
    p = body.find(':', p + needle.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
    if (p >= body.size() || body[p] != '"') return def;
    ++p;
    std::string out;
    while (p < body.size() && body[p] != '"') {
        if (body[p] == '\\' && p + 1 < body.size()) {
            const char esc = body[p + 1];
            if (esc == 'n') out += '\n';
            else if (esc == 't') out += '\t';
            else if (esc == 'r') out += '\r';
            else out += esc;
            p += 2;
        } else {
            out += body[p++];
        }
    }
    return out;
}

inline std::vector<std::pair<std::string, double>> numberObject(const std::string& body,
                                                                const std::string& key) {
    std::vector<std::pair<std::string, double>> out;
    const std::string needle = "\"" + key + "\"";
    std::size_t p = body.find(needle);
    if (p == std::string::npos) return out;
    p = body.find('{', p);
    if (p == std::string::npos) return out;
    ++p;
    while (p < body.size() && body[p] != '}') {
        while (p < body.size() && (std::isspace(static_cast<unsigned char>(body[p])) || body[p] == ',')) ++p;
        if (p >= body.size() || body[p] != '"') break;
        const std::size_t keyStart = ++p;
        while (p < body.size() && body[p] != '"') ++p;
        const std::string name = body.substr(keyStart, p - keyStart);
        ++p;
        while (p < body.size() && (std::isspace(static_cast<unsigned char>(body[p])) || body[p] == ':')) ++p;
        if (p >= body.size()) break;
        char* end = nullptr;
        const double val = std::strtod(body.c_str() + p, &end);
        if (end == body.c_str() + p) break;
        out.emplace_back(name, val);
        p = static_cast<std::size_t>(end - body.c_str());
    }
    return out;
}

inline void respond(int fd, int code, const std::string& type, const std::string& body) {
    std::ostringstream o;
    o << "HTTP/1.1 " << code << (code == 200 ? " OK" : " ERROR") << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Access-Control-Allow-Headers: Content-Type\r\n"
      << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      << "Cache-Control: no-store\r\n"
      << "Connection: close\r\n\r\n"
      << body;
    const std::string out = o.str();
    std::size_t sent = 0;
    while (sent < out.size()) {
        const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

}  // namespace http

static std::atomic<bool> g_running{true};
static void onSignal(int) { g_running = false; }

static std::string examplesJson() {
    static const char* kExamples[][2] = {
        {"Basic precedence", "3 + 4 * 2"},
        {"Parentheses", "(3 + 4) * 2"},
        {"Right-associative power", "2 ^ 3 ^ 2"},
        {"Unary minus vs power", "-2 ^ 2"},
        {"Implicit multiply", "2(3 + 4) ^ 2"},
        {"Variables", "2x^2 - 3x + 1"},
        {"Functions", "sqrt(81) + max(2, 7)"},
        {"Constant folding", "2*3 + 4*5 - 6/2 + 7*8"},
        {"Bigger program", "((3+4)*2 - 5) / 7 + 2^3*2 - sqrt(81) + x"},
        {"Percentage", "1250 * 18 % 100"},
        {"Circle area", "3.14159 * r ^ 2"},
        {"Error demo", "3 + * 4"},
    };
    std::ostringstream o;
    o << "[";
    for (std::size_t i = 0; i < sizeof(kExamples) / sizeof(kExamples[0]); ++i) {
        if (i) o << ",";
        o << "{\"label\":" << js::q(kExamples[i][0]) << ",\"expr\":" << js::q(kExamples[i][1]) << "}";
    }
    o << "]";
    return o.str();
}

static int serve(Engine& engine, int port) {
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }
    int opt = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // 0.0.0.0 — required by the preview proxy
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed on port " << port << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    if (::listen(server, 64) < 0) {
        std::cerr << "listen() failed\n";
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "VoltIQ Expr engine listening on 0.0.0.0:" << port << std::endl;

    while (g_running) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        const int fd = ::accept(server, reinterpret_cast<sockaddr*>(&client), &len);
        if (fd < 0) continue;

        std::string raw;
        char buf[8192];
        ssize_t n;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
            raw.append(buf, static_cast<std::size_t>(n));
            const std::size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::size_t need = 0;
                const std::size_t clPos = raw.find("Content-Length:");
                if (clPos != std::string::npos && clPos < headerEnd)
                    need = static_cast<std::size_t>(std::stoul(raw.substr(clPos + 15, 12)));
                if (raw.size() >= headerEnd + 4 + need) break;
            }
            if (raw.size() > 2 * 1024 * 1024) break;
        }
        if (raw.empty()) {
            ::close(fd);
            continue;
        }

        const http::Request req = http::parse(raw);

        try {
            if (req.method == "OPTIONS") {
                http::respond(fd, 200, "text/plain", "");
            } else if (req.method == "GET" && (req.path == "/api/health" || req.path == "/api/ping")) {
                http::respond(fd, 200, "application/json",
                              "{\"ok\":true,\"service\":\"voltiq-expr-dsa\",\"engine\":\"c++17\","
                              "\"version\":\"1.0\"}");
            } else if (req.method == "GET" && req.path == "/api/examples") {
                http::respond(fd, 200, "application/json", "{\"ok\":true,\"examples\":" + examplesJson() + "}");
            } else if (req.method == "GET" && req.path == "/api/bench") {
                http::respond(fd, 200, "application/json", runBenchmark());
            } else if (req.method == "GET" && req.path == "/api/state") {
                http::respond(fd, 200, "application/json",
                              "{\"ok\":true,\"engine\":" + jsonEngineState(engine) + "}");
            } else if (req.method == "POST" && req.path == "/api/analyze") {
                const std::string expr = http::strField(req.body, "expr");
                // a request that carries "vars" defines the whole environment,
                // so deleting a variable in the UI really makes it unknown
                if (req.body.find("\"vars\"") != std::string::npos) engine.clearVars();
                for (const auto& kv : http::numberObject(req.body, "vars")) engine.setVar(kv.first, kv.second);
                const bool wantTrace = req.body.find("\"trace\":false") == std::string::npos;
                const Analysis a = engine.analyze(expr, wantTrace);
                http::respond(fd, 200, "application/json", jsonAnalysis(engine, a));
            } else if (req.method == "POST" && req.path == "/api/eval") {
                const std::string expr = http::strField(req.body, "expr");
                if (req.body.find("\"vars\"") != std::string::npos) engine.clearVars();
                for (const auto& kv : http::numberObject(req.body, "vars")) engine.setVar(kv.first, kv.second);
                double value = 0.0;
                std::string error;
                const bool ok = engine.evaluate(expr, value, error);
                std::ostringstream o;
                o << "{\"ok\":" << (ok ? "true" : "false")
                  << ",\"value\":" << (ok ? js::value(value) : "null")
                  << ",\"valueText\":" << js::q(ok ? formatNumber(value) : "")
                  << ",\"error\":" << (ok ? "null" : js::q(error))
                  << ",\"engine\":" << jsonEngineState(engine) << "}";
                http::respond(fd, 200, "application/json", o.str());
            } else if (req.method == "POST" && req.path == "/api/vars") {
                for (const auto& kv : http::numberObject(req.body, "vars")) engine.setVar(kv.first, kv.second);
                http::respond(fd, 200, "application/json",
                              "{\"ok\":true,\"engine\":" + jsonEngineState(engine) + "}");
            } else if (req.method == "POST" && req.path == "/api/reset") {
                engine.clearVars();
                engine.resetCache();
                http::respond(fd, 200, "application/json",
                              "{\"ok\":true,\"engine\":" + jsonEngineState(engine) + "}");
            } else {
                http::respond(fd, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
            }
        } catch (const std::exception& e) {
            http::respond(fd, 500, "application/json",
                          std::string("{\"ok\":false,\"error\":") + js::q(e.what()) + "}");
        }
        ::close(fd);
    }

    ::close(server);
    return 0;
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "help";
    const std::string arg = argc > 2 ? argv[2] : "";

    if (cmd == "serve") {
        Engine engine(128);
        engine.setVar("x", 6);
        engine.setVar("y", 3);
        engine.setVar("r", 5);
        return serve(engine, argc > 2 ? std::atoi(argv[2]) : 8003);
    }
    if (cmd == "selftest") return selfTest();
    if (cmd == "bench") {
        std::cout << runBenchmark() << std::endl;
        return 0;
    }
    if (cmd == "repl") {
        Engine engine(64);
        engine.setVar("x", 2);
        repl(engine);
        return 0;
    }
    if (cmd == "analyze") {
        Engine engine(16);
        engine.setVar("x", 6);
        engine.setVar("y", 3);
        engine.setVar("r", 5);
        const Analysis a = engine.analyze(arg, true);
        std::cout << jsonAnalysis(engine, a) << std::endl;
        return a.ok ? 0 : 1;
    }

    if (cmd == "convert" || cmd == "eval" || cmd == "trace" || cmd == "tree") {
        if (arg.empty()) {
            std::cerr << "usage: xpr " << cmd << " \"<expression>\"\n";
            return 2;
        }
        Engine engine(16);
        engine.setVar("x", 6);
        engine.setVar("y", 3);
        engine.setVar("r", 5);
        const Analysis a = engine.analyze(arg, true);
        std::cout << std::string(78, '=') << "\n"
                  << "VoltIQ Expr · " << cmd << "\n"
                  << std::string(78, '=') << "\n";

        if (cmd == "convert") {
            if (a.tokens.empty()) {
                printError(a);
                return 1;
            }
            printTokens(a);
            printConvert(a);
            return a.convert.ok ? 0 : 1;
        }
        if (cmd == "eval") {
            if (a.tokens.empty()) {
                printError(a);
                return 1;
            }
            printProgram(a);
            printEval(a);
            return a.eval.ok ? 0 : 1;
        }
        if (cmd == "trace") {
            printFullAnalysis(a, true);
            return a.ok ? 0 : 1;
        }
        printFullAnalysis(a, false);
        return a.ok ? 0 : 1;
    }

    std::cout <<
        "VoltIQ Expr — Expression Solver & Visualizer (C++17 DSA project)\n\n"
        "  xpr convert \"3+4*2\"     infix → postfix with a step-by-step stack trace\n"
        "  xpr eval    \"3+4*2\"     evaluate the postfix program on a stack\n"
        "  xpr trace   \"3+4*2\"     both traces + expression tree\n"
        "  xpr tree    \"3+4*2\"     the expression tree and its walks\n"
        "  xpr analyze \"3+4*2\"     the full analysis as JSON\n"
        "  xpr repl                 interactive shell (:help)\n"
        "  xpr bench                empirical benchmarks\n"
        "  xpr selftest             invariant tests for every structure\n"
        "  xpr serve [port]         JSON HTTP engine (default 8003)\n";
    return 0;
}
