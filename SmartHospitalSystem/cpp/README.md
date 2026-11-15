# C++ engine — data structures

Each header is self-contained and independently readable. Suggested viva reading order:

1. **`models.hpp`** — `Patient`, `Doctor`, `Ward`, `Severity`, `Status`, plus the
   severity → weight mapping (`Critical 4 … Standard 1`).
2. **`priority_queue.hpp`** — `ArrayMaxHeap`. 0-based array heap, `siftUp` / `siftDown`,
   `removeById` for re-triage, `validateHeap()` for the invariant check.
3. **`hash_table.hpp`** — `PatientHashTable`. Knuth multiplicative hash, separate
   chaining, automatic rehash at load factor 0.75, chain-length histogram.
4. **`search.hpp`** — `SortedIndex` (binary search on IDs and name prefixes) and
   `NameBst` (pointer-based BST with in-order traversal and height).
5. **`sparse_matrix.hpp`** — `SparseMatrix`. Row-major sorted `(row, col, value)`
   triplets, binary-search `get`, first-fit / column / spread allocation.
6. **`hospital.hpp`** — the integration layer. Owns every structure, keeps them in sync
   through register → triage → consult → admit → discharge, and drives the simulation clock.
7. **`main.cpp`** — HTTP JSON server, CLI commands, benchmarks and the self-test suite.

## Build

```bash
g++ -std=c++17 -O2 -pthread -Wall -Wextra -o ../bin/hospital *.cpp
```

## Verify

```bash
../bin/hospital selftest    # 25 assertions
../bin/hospital bench       # empirical complexity numbers
```
