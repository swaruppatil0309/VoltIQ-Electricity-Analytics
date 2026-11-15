# VoltIQ Expr — Expression Solver &amp; Visualizer

A complete **Data Structures &amp; Algorithms (C++17)** project. Students need to turn
infix expressions into postfix notation, evaluate them with a stack, and *see* every
push and pop. This repository does exactly that — a hand-written C++17 engine plus an
interactive visualiser that animates both algorithms.

> **Every structure is written from scratch.** No `std::stack`, no `std::queue`, no
> `std::unordered_map`, no `std::list`. The dynamic-array stack, ring-buffer queue,
> open-addressing hash map and LRU cache live in `cpp/ds.hpp`.

**Live demo:** <https://swaruppatil0309.github.io/VoltIQ-Electricity-Analytics/>
(also works offline — see *Three ways to open it* below).

---

## The problem → the solution

| Requirement from the brief | How it is done | Where | Complexity |
|---|---|---|---|
| Convert **infix → postfix** | Shunting-yard algorithm with an operator stack | `cpp/shunting_yard.hpp` | **O(n)** |
| **Evaluate postfix** using stacks | Stack machine: push operands, pop on operators | `cpp/evaluator.hpp` | **O(n)** |
| Show **step-by-step evaluation** | Every stack operation is recorded as a trace frame | `cpp/shunting_yard.hpp`, `cpp/evaluator.hpp` | **O(n)** extra |
| **Optimise expression handling** | Compile once → expression tree → **constant folding** → **LRU program cache** | `cpp/ast.hpp`, `cpp/engine.hpp`, `cpp/ds.hpp` | compile **O(n)**, cached evaluate **O(n)** |

### Worked example (`2(3+4)^2 - sqrt(81)/3 + x`, `x = 6`)

```
infix      2 ( 3 + 4 ) ^ 2 - sqrt ( 81 ) / 3 + x
postfix    2 3 4 + 2 ^ * 81 sqrt() 3 / - x +

conversion trace (excerpt)
 step  token   action   operator stack   output queue
   1   2       output   —                2
   2   *       push     *                2
   3   (       push     * (              2
   …
   7   )       match    * (              2 3 4 +
   8   )       match    *                2 3 4 +
   9   ^       push     * ^              2 3 4 +

evaluation trace (excerpt)
 step  token   operand stack   what happened
   3   4       2 3 4           3 + 4 = 7
   6   ^       2 49            7 ^ 2 = 49
   7   *       98              2 * 49 = 98
   …
  14   +       101             95 + 6 = 101        ← result
```

The visualiser replays both tables frame by frame: the token being read glows, the
operator stack grows and shrinks, the output queue fills up, then the operand stack
consumes the postfix program one instruction at a time.

---

## Why postfix at all?

Infix needs parentheses and precedence rules; postfix needs neither. Once the program
is in postfix, evaluation is a **single left-to-right pass with one stack** — no
recursion, no lookahead, no grammar. That is why the conversion step exists: it moves
all the thinking to compile time.

```
infix  : 2 + 3 * 4          ──shunting-yard──▶   postfix : 2 3 4 * +
                                                   push 2, push 3, push 4,
                                                   * → 12, + → 14
```

---

## The five data structures (all hand-written)

| Structure | File | Used for | Cost |
|---|---|---|---|
| `ArrayStack<T>` | `cpp/ds.hpp` | operator stack, operand stack | push/pop **O(1)** amortised, doubling growth |
| `RingQueue<T>` | `cpp/ds.hpp` | token stream (FIFO) | enqueue/dequeue **O(1)** |
| `VarMap` | `cpp/ds.hpp` | variables — open addressing, linear probing, tombstones | lookup **O(1)** average |
| `LruCache<V>` | `cpp/ds.hpp` | compiled programs — hash buckets + intrusive doubly-linked list | get/put **O(1)** |
| `Ast` (node arena) | `cpp/ast.hpp` | expression tree as indices into a vector (no pointers) | build **O(n)** |

Togglable constant folding and the program cache are the two "optimise expression
handling" requirements; both are visible in the dashboard with live counters.

---

## Correctness details that make or break a converter

| Case | Result | Why |
|---|---|---|
| `2 ^ 3 ^ 2` | `2 3 2 ^ ^` = **512** | `^` is right-associative, so it does not pop an equal-precedence operator |
| `-2 ^ 2` | `2 2 ^ -u` = **-4** | unary minus binds *looser* than `^` |
| `2 ^ -3` | `2 3 -u ^` = **0.125** | a prefix operator never triggers a pop |
| `2x`, `3(4+5)`, `(1+2)(3+4)` | implicit `*` inserted | the tokenizer inserts the multiplication nobody typed and flags it |
| `sin(1,2)` | error, column of `sin` | function arity is checked while matching `)` |
| `3 4` | error, column 2 | two numbers in a row are a typo, not a product |
| `3/0` | error at runtime, `x/0` too | the compiler refuses to fold it, the stack machine reports it |
| `3 + * 4` | error with a caret under `*` | the lexer validates the grammar in one pass |

Errors carry a column, so the CLI and the dashboard both render a caret line:

```
  error: operator '*' needs a value on its left
    3 + * 4
        ^
```

---

## Optimisations (both measurable)

1. **Compile once, evaluate many times.** `tokenize → shunting-yard → postfix` is done
   once per distinct expression and stored in an **LRU cache** keyed by the source text.
   Re-evaluation is just the stack machine.
2. **Constant folding.** The postfix program is parsed into an expression tree; any
   subtree made only of literals is evaluated at compile time and replaced by one
   number. The runtime program gets shorter — e.g. 36 instructions become **3**, and a
   fully constant expression collapses to a single instruction.

```
before folding : 2 3 * 4 5 * + 6 2 / - 7 8 * + 9 1 * - 2 5 ^ + 16 sqrt() + 3 -u abs() - 2 7 max() + x +
after  folding : 110 x +
```

Both are on by default only in the benchmark; the visualiser starts with folding
**off** so the teaching trace matches the textbook algorithm, and the toggle shows the
reduction live.

---

## Project structure

```
ExpressionSolver/
├── cpp/                          ← the DSA engine (pure C++17, zero dependencies)
│   ├── token.hpp                 token model, precedence table, number formatting
│   ├── tokenizer.hpp             one-pass lexer + grammar check + implicit ×
│   ├── shunting_yard.hpp         infix → postfix with a recorded stack trace
│   ├── ast.hpp                   expression tree, constant folding, RPN emission
│   ├── evaluator.hpp             stack machine with a recorded evaluation trace
│   ├── engine.hpp                pipeline, LRU program cache, variables, stats
│   ├── ds.hpp                    ArrayStack, RingQueue, VarMap, LruCache
│   ├── json.hpp                  minimal JSON writer
│   └── main.cpp                  HTTP JSON server, CLI, self-tests, benchmarks
├── web/
│   ├── server.js                 static host + /api/* reverse proxy (no npm deps)
│   └── public/                   dashboard (hand-written HTML/CSS/JS)
├── standalone/
│   ├── engine.js                 JS mirror of the C++ engine (identical JSON)
│   └── build-standalone.js       bundles everything into one HTML file
├── tests/
│   ├── parity.js                 C++ ↔ JS equivalence (34 expressions)
│   └── ui-test.js                headless jsdom test of the real dashboard
├── VoltIQ-Expr-Standalone.html   ← single-file build: double-click to open
├── run.sh / run.bat              one-command launcher
└── bin/xpr                       compiled engine
```

---

## Three ways to open it

| Option | How | Needs |
|---|---|---|
| **1 · Single file** (easiest) | Open `VoltIQ-Expr-Standalone.html` | nothing — works offline |
| **2 · Local full stack** | `./run.sh` → <http://localhost:3000> | g++ + Node |
| **3 · GitHub Pages** | <https://swaruppatil0309.github.io/VoltIQ-Electricity-Analytics/> | nothing |

The single-file build embeds a JavaScript mirror of the engine, so the dashboard works
with no server and no compiler. The C++ engine in `cpp/` remains the canonical
implementation — `standalone/engine.js` exists only for convenient distribution, and
`tests/parity.js` proves the two agree.

```bash
cd ExpressionSolver
./run.sh                      # compiles the engine, starts it, opens the dashboard
node standalone/build-standalone.js   # rebuild the single-file build
```

```bat
run.bat                       :: Windows
```

Manual:

```bash
g++ -std=c++17 -O2 -pthread -Wall -Wextra -o bin/xpr cpp/*.cpp
./bin/xpr serve 8003          # JSON engine
cd web && node server.js      # dashboard on :3000, proxies /api/* to :8003
```

---

## Command-line mode (great for a viva)

```bash
./bin/xpr convert "3+4*2"     # tokens + infix → postfix table
./bin/xpr eval    "3+4*2"     # compiled program + evaluation table
./bin/xpr trace   "2(3+4)^2 - sqrt(81)/3 + x"   # both traces + expression tree
./bin/xpr tree    "2^3^2"     # tree, prefix / infix / postfix walks
./bin/xpr analyze "3+4*2"     # the whole analysis as JSON
./bin/xpr repl                # interactive shell  (:help, :set x=2, :tree …)
./bin/xpr bench               # benchmarks
./bin/xpr selftest            # 60 assertions
./bin/xpr serve [port]        # JSON HTTP engine (default 8003)
```

Example:

```
$ ./bin/xpr eval "3+4*2"
Compiled program
  shunting-yard output : 3 4 2 * +   (5 instructions)
  constant folding     : 14   (1 instructions, 4 removed)
  program in use       : 3 4 2 * +   (5 instructions, folding OFF)

Postfix evaluation  (stack machine, 5 operations)
  step token   operand stack   what happened
   1   3       3               push the literal value
   2   4       3 4             push the literal value
   3   2       3 4 2           push the literal value
   4   *       3 8             4 * 2 = 8
   5   +       11              3 + 8 = 11
  result: 11
```

---

## Dashboard features

| View | What it proves |
|---|---|
| **Input + variables** | Live parsing as you type, editable variables (`x`, `rate`, …), examples, exact error column with a caret |
| **Result strip** | Value, compiled postfix program, tokens / instructions / folded-away / peak stack / compile µs / evaluate µs / cache hit |
| **Shunting-yard pane** | Token strip highlights the token being read; operator stack and output queue update every single push and pop |
| **Evaluation pane** | Postfix instructions run one at a time on the operand stack; the current value and operation are shown |
| **Playback** | Play / pause / step / jump, 0.5×–4× speed, arrow-key control, click any token to jump to its frame |
| **Compiler &amp; optimiser** | Raw program vs constant-folded program, instruction counts, live toggle |
| **Expression tree** | SVG tree with folded subtrees marked, plus prefix / infix / postfix walks |
| **Benchmarks** | Cached + folded pipeline vs naive pipeline, and tokenizer scaling (µs per token stays flat) |
| **Engine state** | Compiles, cache hits/misses, hit rate, tokens lexed, and the live LRU cache contents |

Shareable: the expression, variables and folding mode live in the URL hash, so
**Copy link** produces a link that opens exactly the same view.

---

## Measured results

From `./bin/xpr bench` (50,000 iterations per row, one sandbox run):

| Operation | Fast path | Naive path | Speed-up |
|---|---|---|---|
| Evaluate one expression (cache warm) | **3.46 µs** | 18.08 µs (re-tokenize + re-parse) | **5.2×** |
| Constant-heavy expression | **2.83 µs** | 24.62 µs | **8.7×** |
| 400 distinct expressions, cache warm | **8.08 µs** | 18.08 µs | **2.2×** |
| Program size after folding | **3 instructions** | 36 instructions | **12× shorter** |

Tokenizer + parser scaling — cost per token is flat, so the pipeline is linear:

| Tokens | Total parse + evaluate | Per token |
|---|---|---|
| 33 | 19.7 µs | 0.596 µs |
| 513 | 322.6 µs | 0.629 µs |
| 8,193 | 4,914 µs | 0.600 µs |

---

## Verified

```
./bin/xpr selftest        →  60 / 60 assertions pass
node tests/parity.js      →  34 / 34 expressions match between C++ and JS
node tests/ui-test.js     →  26 / 26 UI checks pass (headless jsdom, offline build)
g++ -Wall -Wextra         →  zero warnings
```

Coverage: precedence and associativity corners (unary minus, `^`, `2^-2`), implicit
multiplication, functions and arity errors, every error path with its column, constant
folding (including "folding must never change the answer" over mixed expressions), LRU
eviction, hash-map growth/tombstones, stack growth, queue FIFO order, trace integrity,
and the rendered dashboard (playback, errors, folding, examples, keyboard, benchmarks).

---

## API reference

Served by the C++ binary, proxied by `web/server.js`.

| Method | Endpoint | Purpose |
|---|---|---|
| `GET` | `/api/health` | engine liveness + name |
| `GET` | `/api/examples` | example expressions |
| `GET` | `/api/bench` | benchmark JSON (same data as `xpr bench`) |
| `GET` | `/api/state` | engine counters, LRU contents, variables |
| `POST` | `/api/analyze` | `{expr, vars?, fold?, trace?}` → tokens, both traces, tree, stats. Passing `vars` replaces the variable environment. |
| `POST` | `/api/eval` | `{expr, vars?}` → value only (fast path) |
| `POST` | `/api/vars` | `{vars:{x:6}}` → set variables |
| `POST` | `/api/reset` | clear variables and the compile cache |

`/api/analyze` returns the two traces the visualiser replays:

```json
{
  "ok": true,
  "program": { "raw": "2 3 * x +", "folded": "6 x +", "active": "2 3 * x +", "foldingActive": false },
  "convert": { "rpn": "2 3 * x +", "peakStack": 2, "steps": [
      { "step": 1, "token": "2", "action": "output", "stack": [], "output": ["2"], "tokenIndex": 0 }
  ]},
  "eval": { "ok": true, "valueText": "10", "peakStack": 2, "steps": [
      { "step": 1, "token": "2", "action": "push", "stack": ["2"], "note": "push the literal value" }
  ]},
  "ast": { "root": 4, "nodeCount": 5, "height": 3, "prefix": "+ * 2 3 x" },
  "stats": { "tokens": 5, "rawInstructions": 5, "instructions": 5, "compileUs": 3.2, "evalUs": 0.4 }
}
```

---

## Design notes / viva questions

**Why a stack for conversion and evaluation?**
Both problems are LIFO by nature: the most recent operator is the first one that can be
emitted, and the most recent operand is the first one an operator needs.

**Why does the converter need precedence *and* associativity?**
Precedence says `*` beats `+`; associativity says `2-3-4` is `(2-3)-4` and `2^3^2` is
`2^(3^2)`. Getting associativity wrong produces a program that is still valid postfix
but evaluates to the wrong number — the self-test pins both.

**Why does unary minus get its own precedence?**
`-2^2` must mean `-(2^2)`. Treating `-` as a normal high-precedence operator gives `4`.
Prefix operators therefore never pop anything, which also makes `2^-3` work.

**Is the trace free?**
No — a snapshot of the stack per step is O(n), so a full trace is O(n²) in the worst
case. The compiler never builds a trace (`wantTrace = false` is threaded through), only
the visualiser does, and traces are skipped automatically for very long inputs.

**Where is the O(n) evaluation hidden?**
Nowhere: each token is pushed or popped exactly once, so evaluation is linear in the
number of instructions. The interesting cost is compilation, which is why the compiled
program is cached.

**Why an expression tree if postfix already evaluates?**
The tree is what makes optimisation safe and simple: folding is a post-order walk that
replaces constant subtrees, and re-emitting postfix from the tree is another post-order
walk. It also gives free prefix/infix/postfix traversals for the visualiser.

**What does the LRU cache buy?**
Re-evaluating a repeated expression skips lexing, parsing, tree building and folding —
a 5× speed-up here. The trade-off is memory: the cache is bounded (128 programs) and
evicts the least recently used entry in O(1).

**Limits / honest answers.**
Recursive tree walks mean a pathological expression with thousands of nested operators
could exhaust the C++ stack — the analysis path guards against huge inputs (> 2000
characters) instead of pretending otherwise. Numbers are `double`, so `0.1+0.2` is not
exactly `0.3`. The JS mirror re-implements the engine, so it is verified against the C++
binary rather than assumed equal.

---

## Tech

C++17 (engine, zero dependencies) · Node.js ≥ 18 (static host + proxy) · vanilla
HTML/CSS/JS (dashboard, no framework, no CDN).

Built as a DSA course project — stacks, queues, trees, hashing and caching applied to
expression solving.
