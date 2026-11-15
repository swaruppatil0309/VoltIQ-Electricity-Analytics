// engine.hpp — the expression pipeline.
//
//   source text
//     │  tokenize (single pass, implicit × insertion, grammar check)
//     ▼
//   token list ──────────────► shunting-yard ──► postfix program (teaching
//     │                                          trace recorded here)
//     │  AST build
//     ▼
//   expression tree ──► constant folding ──► optimised postfix program
//                                              │
//                                              ▼  stack machine
//                                           value
//
// The compiled program is cached in an LRU cache keyed by the (normalised)
// source text, so evaluating the same expression twice skips compilation
// entirely. That is the "optimise expression handling" part of the brief.
#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "ast.hpp"
#include "ds.hpp"
#include "evaluator.hpp"
#include "shunting_yard.hpp"
#include "token.hpp"
#include "tokenizer.hpp"

struct Program {
    bool ok = false;
    std::string error;
    int errorPos = 0;
    int errorLen = 1;
    std::string expr;
    std::string normalized;
    std::vector<Token> tokens;
    std::vector<Token> rawRpn;
    std::vector<Token> rpn;        // the program that actually runs
    std::vector<Token> foldedRpn;  // the constant-folded variant (always built)
    Ast tree;                      // tree of the program that runs
    bool foldingActive = false;
    int implicitMuls = 0;
    int foldedInstructions = 0;  // instructions saved by folding
    int foldedSubtrees = 0;
    int peakOpStack = 0;
};

struct AnalysisStats {
    int tokens = 0;
    int rawInstructions = 0;
    int instructions = 0;   // instruction count of the program that runs
    int foldedConstants = 0;
    int foldedSubtrees = 0;
    int implicitMuls = 0;
    int astNodes = 0;
    int astHeight = 0;
    int peakOpStack = 0;
    int peakEvalStack = 0;
    int traceSteps = 0;
    int opsApplied = 0;
    double compileUs = 0.0;
    double evalUs = 0.0;
    bool cacheHit = false;
};

struct Analysis {
    bool ok = false;
    bool foldingActive = false;
    std::vector<Token> foldedRpn;
    std::string error;
    int errorPos = 0;
    int errorLen = 1;
    std::string expr;
    std::string normalized;
    std::vector<Token> tokens;
    std::vector<Token> rawRpn;
    std::vector<Token> rpn;
    ConvertResult convert;
    Ast tree;
    EvalResult eval;
    AnalysisStats stats;
};

struct EngineStats {
    long long compiles = 0;
    long long cacheHits = 0;
    long long cacheMisses = 0;
    long long evals = 0;
    long long foldedConstants = 0;
    long long tokensLexed = 0;
    double lastCompileUs = 0.0;
    double lastEvalUs = 0.0;
};

class Engine {
public:
    explicit Engine(std::size_t cacheCapacity = 64) : cache_(cacheCapacity) {}

    // Constant folding is opt-in: with it off, the visualiser shows the exact
    // program the shunting-yard algorithm produced. Turning it on collapses
    // every constant subtree into one instruction.
    void setFolding(bool on) {
        if (folding_ == on) return;
        folding_ = on;
        cache_.clear();  // cached programs depend on the mode
    }
    bool folding() const { return folding_; }

    // Full pipeline including both traces — what the visualiser shows.
    Analysis analyze(const std::string& expr, bool wantTrace = true) {
        // A trace snapshot is O(n) per step, so very long inputs run without one.
        if (expr.size() > 2000) wantTrace = false;
        const auto startCompile = std::chrono::high_resolution_clock::now();
        Program program;
        bool hit = cache_.get(expr, program);
        stats_.lastCompileUs = 0.0;

        if (hit) {
            ++stats_.cacheHits;
        } else {
            ++stats_.cacheMisses;
            program = compile(expr);
            cache_.put(expr, program);
            if (program.ok) {
                ++stats_.compiles;
                stats_.foldedConstants += program.foldedInstructions;
                stats_.tokensLexed += static_cast<long long>(program.tokens.size());
            }
        }

        Analysis a;
        a.expr = expr;
        a.stats.cacheHit = hit;
        if (!program.ok) {
            a.ok = false;
            a.error = program.error;
            a.errorPos = program.errorPos;
            a.errorLen = program.errorLen;
            a.stats.compileUs = hit ? 0.0 : micros(startCompile);
            return a;
        }

        a.ok = true;
        a.foldingActive = program.foldingActive;
        a.foldedRpn = program.foldedRpn;
        a.normalized = program.normalized;
        a.tokens = program.tokens;
        a.rawRpn = program.rawRpn;
        a.rpn = program.rpn;
        a.tree = program.tree;

        // The visualiser always replays the *original* infix → postfix pass so
        // the teaching trace matches the algorithm, not the folding shortcut.
        if (wantTrace) a.convert = shunting::run(program.tokens, true);
        else {
            a.convert.ok = true;
            a.convert.rpn = program.rawRpn;
            a.convert.peakStack = program.peakOpStack;
        }

        const auto evalStart = std::chrono::high_resolution_clock::now();
        a.eval = evalr::run(program.rpn, vars_, wantTrace);
        const double evalUs = micros(evalStart);
        ++stats_.evals;
        stats_.lastEvalUs = evalUs;

        if (!a.eval.ok) {
            a.ok = false;
            a.error = a.eval.error;
            a.errorPos = a.eval.errorPos;
            a.errorLen = 1;
        }

        a.stats.tokens = static_cast<int>(program.tokens.size());
        a.stats.rawInstructions = static_cast<int>(program.rawRpn.size());
        a.stats.instructions = static_cast<int>(program.rpn.size());
        a.stats.foldedConstants = program.foldedInstructions;
        a.stats.foldedSubtrees = program.foldedSubtrees;
        a.stats.implicitMuls = program.implicitMuls;
        a.stats.astNodes = ast::countNodes(program.tree, program.tree.root);
        a.stats.astHeight = ast::computeHeight(program.tree, program.tree.root);
        a.stats.peakOpStack = a.convert.peakStack;
        a.stats.peakEvalStack = a.eval.peakStack;
        a.stats.traceSteps = static_cast<int>(a.convert.steps.size() + a.eval.trace.size());
        a.stats.opsApplied = a.eval.opsApplied;
        a.stats.compileUs = hit ? 0.0 : micros(startCompile);
        a.stats.evalUs = evalUs;
        return a;
    }

    // Fast path used by benchmarks / bulk evaluation: compiled program only,
    // no traces at all.
    bool evaluate(const std::string& expr, double& out, std::string& error) {
        Program program;
        if (!cache_.get(expr, program)) {
            ++stats_.cacheMisses;
            program = compile(expr);
            cache_.put(expr, program);
            if (program.ok) ++stats_.compiles;
        } else {
            ++stats_.cacheHits;
        }
        if (!program.ok) {
            error = program.error;
            return false;
        }
        EvalResult r = evalr::run(program.rpn, vars_, false);
        ++stats_.evals;
        if (!r.ok) {
            error = r.error;
            return false;
        }
        out = r.value;
        return true;
    }

    // Naive reference implementation: rebuild everything, no cache, no folding.
    double evaluateNaive(const std::string& expr, bool& ok) const {
        TokenizeResult tk = lexer::run(expr);
        if (!tk.ok) {
            ok = false;
            return 0.0;
        }
        ConvertResult cv = shunting::run(tk.tokens, false);
        if (!cv.ok) {
            ok = false;
            return 0.0;
        }
        EvalResult ev = evalr::run(cv.rpn, vars_, false);
        ok = ev.ok;
        return ev.value;
    }

    VarMap& vars() { return vars_; }
    const VarMap& vars() const { return vars_; }

    void setVar(const std::string& name, double value) { vars_.put(name, value); }
    bool getVar(const std::string& name, double& out) const { return vars_.get(name, out); }
    void clearVars() { vars_.clear(); }

    void resetCache() { cache_.clear(); }

    const EngineStats& stats() const { return stats_; }
    const LruCache<Program>& cache() const { return cache_; }

    static double micros(std::chrono::high_resolution_clock::time_point start) {
        const auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::micro>(end - start).count();
    }

private:
    // Compile: tokenize → postfix → tree → fold → optimised postfix.
    Program compile(const std::string& expr) const {
        Program p;
        p.expr = expr;

        TokenizeResult tk = lexer::run(expr);
        if (!tk.ok) {
            p.ok = false;
            p.error = tk.error;
            p.errorPos = tk.errorPos;
            p.errorLen = tk.errorLen;
            return p;
        }
        p.tokens = tk.tokens;
        p.implicitMuls = tk.implicitInserts;
        p.normalized = tk.normalized();

        ConvertResult cv = shunting::run(tk.tokens, false);
        if (!cv.ok) {
            p.ok = false;
            p.error = cv.error;
            p.errorPos = cv.errorPos;
            return p;
        }
        p.rawRpn = cv.rpn;
        p.peakOpStack = cv.peakStack;

        // --- optimisation 1: build the expression tree --------------------
        Ast rawTree = ast::fromRpn(cv.rpn);
        if (rawTree.root < 0) {
            p.ok = false;
            p.error = "the expression produced no value";
            p.errorPos = 0;
            return p;
        }

        // --- optimisation 2: fold constant subtrees -----------------------
        Ast foldedTree = rawTree;
        ast::fold(foldedTree, foldedTree.root);
        std::vector<Token> foldedRpn;
        foldedRpn.reserve(cv.rpn.size());
        int foldedEmitted = 0;
        ast::emitRpn(foldedTree, foldedTree.root, foldedRpn, foldedEmitted);

        p.foldedRpn = foldedRpn;
        p.foldedSubtrees = foldedTree.foldedNodes;
        p.foldedInstructions = static_cast<int>(cv.rpn.size()) - static_cast<int>(foldedRpn.size());
        p.foldingActive = folding_;

        // --- choose the program that actually runs ------------------------
        p.tree = folding_ ? foldedTree : rawTree;
        p.rpn = folding_ ? foldedRpn : cv.rpn;
        p.ok = !p.rpn.empty();
        if (!p.ok) {
            p.error = "the expression produced no value";
            p.errorPos = 0;
        }
        return p;
    }

    VarMap vars_;
    LruCache<Program> cache_;
    EngineStats stats_;
    bool folding_ = false;
};
