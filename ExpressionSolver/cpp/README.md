# C++ engine — data structures & algorithms

Each header is self-contained and readable on its own. Suggested viva reading order:

1. **`token.hpp`** — `Token`, `TokType`, the precedence table (`+ - = 1`, `* / % = 2`,
   unary `- = 3`, `^ = 4` right-associative) and the function table (`sqrt`, `sin`,
   `max`, … with their arity).
2. **`tokenizer.hpp`** — one left-to-right pass: numbers, identifiers, functions,
   parentheses, commas and operators. Validates the grammar while it lexes (so
   `3 + * 4` fails with a column) and inserts the implicit multiplication in `2x`
   or `3(4+5)`.
3. **`shunting_yard.hpp`** — infix → postfix (`ArrayStack<Token>` + argument counter for
   function arity), recording every push/pop as a `ConvertStep`.
4. **`ast.hpp`** — expression tree from the postfix program, recursive constant folding,
   re-emission of the shorter postfix program, prefix/infix rendering.
5. **`evaluator.hpp`** — the stack machine: `ArrayStack<double>`, per-instruction
   `EvalStep` trace, runtime errors (division by zero, unknown variable).
6. **`ds.hpp`** — `ArrayStack<T>`, `RingQueue<T>`, `VarMap` (open addressing with
   tombstones) and `LruCache<V>` (hash buckets + intrusive LRU list). No STL containers.
7. **`engine.hpp`** — the pipeline plus the LRU program cache, the variable environment
   and counters. `setFolding(true)` enables the optimiser.
8. **`main.cpp`** — HTTP JSON server, CLI, 60 self-test assertions and benchmarks.

## Build

```bash
g++ -std=c++17 -O2 -pthread -Wall -Wextra -o ../bin/xpr *.cpp
```

## Verify

```bash
../bin/xpr selftest        # 60 assertions
../bin/xpr bench           # empirical numbers
```

## Pipeline

```
source → tokenizer → shunting-yard → postfix ──▶ AST ──▶ fold ──▶ shorter postfix
                                        │                              │
                                   convert trace                  eval trace + value
```
