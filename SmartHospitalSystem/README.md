# VoltIQ Care — Smart Hospital Management System

A complete **Data Structures & Algorithms (C++17)** project: a hospital needs to manage
patient flow, prioritise emergency cases, and allocate resources like beds and doctors.
This repository implements that problem end-to-end — a hand-written C++17 data-structure
engine, plus an interactive dashboard that visualises every structure live.

> **Every data structure is written from scratch.** No `std::priority_queue`, no
> `unordered_map`, no `std::map`. The heap, hash table, BST, binary search index and
> sparse matrix are all implemented in `cpp/`.

---

## The problem → the solution

| Requirement from the brief | Data structure | Where | Complexity |
|---|---|---|---|
| Design structures to manage patient queues with **priority handling** | Custom **binary max-heap** (array based) | `cpp/priority_queue.hpp` | `push` / `pop` **O(log n)**, `peek` **O(1)** |
| Implement **searching techniques** for quick patient lookup | **Hash table** (separate chaining) + **binary search** + **BST** | `cpp/hash_table.hpp`, `cpp/search.hpp` | lookup **O(1)**, search **O(log n)** |
| Use **sparse matrix** concepts for bed allocation | Sparse matrix as row-major **triplets** | `cpp/sparse_matrix.hpp` | `get` **O(log nnz)**, memory **O(nnz)** |
| **Optimise operations** for faster emergency handling | Triage key `(severity ↓, wait ↓)` + lazy deletion | `cpp/hospital.hpp` | registration **O(log n)** total |

### Triage ordering — how emergencies win

```
priority(a) > priority(b)
  iff  severityWeight(a) > severityWeight(b)
  else arrivalTick(a) < arrivalTick(b)      // longest-waiting wins
  else seq(a) < seq(b)                      // FIFO tie-break
```

* `Critical = 4`, `Emergency = 3`, `Urgent = 2`, `Standard = 1`
* A critical patient can never sit behind a routine case.
* Inside one severity band the longest-waiting patient goes first, which **prevents
  starvation** — the classic problem with naive priority queues.

### Bed allocation — why sparse, not dense

A ward is a `rows × cols` grid, but only a fraction of beds are occupied. Storing the
whole grid wastes memory, so each ward keeps only occupied cells:

```
(row, col, patientId)  → sorted row-major → binary search for get/set
```

| Ward | Grid | Dense | Sparse (typical) |
|---|---|---|---|
| ICU | 3 × 8 | 96 B | ~60 B |
| Emergency | 4 × 10 | 160 B | ~84 B |
| General Ward A | 4 × 14 | 224 B | ~108 B |
| **Total (6 wards)** | **224 beds** | **896 B** | **~432 B** |

Three allocation strategies are implemented: **first-fit** (lowest row-major free cell),
**column stack** (compact block, nurse friendly) and **spread** (fewest occupied neighbours).

---

## Project structure

```
SmartHospitalSystem/
├── cpp/                       ← the DSA engine (pure C++17, zero dependencies)
│   ├── models.hpp             Patient, Doctor, Ward, Severity, Status
│   ├── priority_queue.hpp     ArrayMaxHeap — sift-up / sift-down from scratch
│   ├── hash_table.hpp         PatientHashTable — chaining + Knuth hashing + rehash
│   ├── search.hpp             SortedIndex (binary search) + NameBst (BST)
│   ├── sparse_matrix.hpp      SparseMatrix — triplets, first-fit allocation
│   ├── hospital.hpp           integration: register, triage, admit, simulate
│   ├── json.hpp               minimal JSON escaping helpers
│   └── main.cpp               HTTP JSON server, CLI, benchmarks, self-test
├── web/
│   ├── server.js              static host + /api/* reverse proxy (no npm deps)
│   └── public/                dashboard (hand-written HTML/CSS/JS)
├── standalone/
│   ├── engine.js              JS mirror of the C++ engine (same /api surface)
│   └── build-standalone.js    bundles everything into one HTML file
├── VoltIQ-Care-Standalone.html  ← single-file build: double-click to open
├── docs/index.html            same file, served by GitHub Pages
├── run.sh                     one-command launcher (Linux / macOS / sandbox)
├── run.bat                    one-command launcher (Windows)
└── bin/hospital               compiled engine
```

---

## Three ways to open it

| Option | How | Needs |
|---|---|---|
| **1 · Single file** (easiest) | Double-click `VoltIQ-Care-Standalone.html` | nothing — works offline |
| **2 · Local full stack** | `./run.sh` → http://localhost:3000 | g++ + Node |
| **3 · GitHub Pages** | enable Pages → `/SmartHospitalSystem/docs` | one toggle in repo settings |

The single-file build contains a JavaScript mirror of the engine so the dashboard works
with no server and no compiler. The C++ engine in `cpp/` remains the canonical
implementation — `standalone/engine.js` exists only for convenient distribution.

```bash
cd standalone && node build-standalone.js   # regenerate the single file
```

### Option 2 — one command

```bash
./run.sh                     # compiles, starts engine + dashboard
# open http://localhost:3000
```

```bat
run.bat                      :: Windows
```

### Option 2 — manual

```bash
# 1. build the C++ engine
g++ -std=c++17 -O2 -pthread -o bin/hospital cpp/*.cpp

# 2. start the engine (port 8001)
./bin/hospital serve 8001

# 3. start the dashboard (port 3000, proxies /api/* to 8001)
cd web && node server.js
```

The dashboard has **zero npm dependencies** — `server.js` uses only Node's standard library.

### Windows (MSVC alternative)

```bat
cl /std:c++17 /O2 /EHsc /Fe:bin\hospital.exe cpp\main.cpp
```

---

## Command-line mode

The engine is a first-class CLI project too — useful for a viva / lab demo.

```bash
./bin/hospital selftest        # 25 invariant assertions across all structures
./bin/hospital bench           # empirical complexity benchmarks
./bin/hospital demo            # dump the whole hospital state as JSON
./bin/hospital search 1004     # hash-table lookup with probe count
./bin/hospital search sunita   # BST + prefix binary search
./bin/hospital callnext        # pop the highest-priority patient
./bin/hospital add '{"name":"Ravi","age":30,"severity":"critical"}'
```

---

## Dashboard features

| View | What it proves |
|---|---|
| **Overview** | Live KPIs: waiting, critical count, bed density, doctors busy, average wait |
| **Triage Queue** | The heap drawn as an **actual array** with indices — root is highlighted "next". Service order is produced by really popping a copy of the heap. |
| **Patients** | Search box reports algorithm, **probe count** and **µs** for each query |
| **Bed Allocator** | Per-ward sparse matrix grid, live triplets, dense-vs-sparse memory bars, switchable allocation strategy |
| **Data Structures** | Complexity cards + a full operation/complexity table |
| **Benchmarks** | Hash vs linear scan, binary vs linear search, heap vs sorted-vector insert — timed in C++ |

Interactive actions: register patient (with triage severity picker), call next patient,
escalate to critical, admit / release bed, mass-casualty drill, advance the virtual clock,
auto-play simulation, delete patient.

---

## Measured results

From `./bin/hospital bench` (60,000 records, 6,000 queries):

| Operation | Fast structure | Naive alternative | Speed-up |
|---|---|---|---|
| Patient lookup | Hash table **0.033 µs** | Linear scan 90.6 µs | **~2,700×** |
| Search in register | Binary search **0.144 µs** | Linear scan 90.6 µs | **~600×** |
| Triage insertion | Binary heap **0.029 µs** | Sorted vector 2.86 µs | **~100×** |
| Ward grid memory | Sparse **5,400 B** | Dense 14,400 B | **62.5% saved** |

---

## Verified

```
./bin/hospital selftest   →  25 / 25 assertions pass
headless UI test          →  32 / 32 checks pass (live C++ server)
offline single-file test  →  30 / 30 checks pass (network blocked)
g++ -Wall -Wextra         →  zero warnings
```

Self-test coverage: hash insert/get/delete + load factor, max-heap property + pop order,
binary search hit/miss + probe bound, BST exact search + in-order traversal, sparse
set/overwrite/clear/allocation, and full engine integration.

---

## API reference

All endpoints are served by the C++ binary and proxied by `server.js`.

| Method | Endpoint | Purpose |
|---|---|---|
| `GET` | `/api/state` | full hospital snapshot (stats, patients, doctors, wards, heap) |
| `GET` | `/api/search?q=` | hash / BST / binary-search lookup with probe count + µs |
| `GET` | `/api/benchmark` | empirical complexity benchmarks |
| `GET` | `/api/health` | engine liveness |
| `POST` | `/api/register` | `{name, age, gender, phone, severity, condition, department}` |
| `POST` | `/api/admit` | `{id, ward?, row?, col?}` — first-fit when coordinates are omitted |
| `POST` | `/api/discharge-bed` | `{id}` |
| `POST` | `/api/call-next` | pop the heap top and assign a doctor |
| `POST` | `/api/escalate` | `{id, severity}` — re-triage, re-heapify |
| `POST` | `/api/complete` | `{id}` — finish consultation |
| `POST` | `/api/delete` | `{id}` — remove from hash table, index, BST, heap, beds |
| `POST` | `/api/tick` | `{minutes}` — advance the simulation |
| `POST` | `/api/strategy` | `{strategy: first \| column \| spread}` |
| `POST` | `/api/mass-casualty` | `{count}` — inject emergency arrivals |

---

## Design notes / viva questions

**Why not `std::priority_queue`?**
It cannot delete or re-prioritise an arbitrary element. Triage needs "escalate patient
#1017 to critical", so the heap is hand-written with `removeById` + re-sift.

**Why is hash lookup O(1) and not O(n) worst case?**
Separate chaining gives O(1) *average*. Rehashing above a 0.75 load factor keeps chains
short (the engine reports the longest chain live in the UI). Worst case stays O(n) if every
key collides — that is the honest answer.

**Why a BST *and* a sorted array?**
The sorted array gives cache-friendly binary search on IDs and name prefixes. The BST
gives O(log n) insertion with in-order alphabetical traversal. The dashboard shows both.

**Why does escalate cost O(n)?**
`removeById` scans the heap array to locate the entry. It happens rarely (a triage
upgrade), so paying O(n) there keeps every other operation optimal. A production system
would add a position index to make it O(log n).

**How is starvation prevented?**
Equal severities fall back to arrival time, so a stream of critical arrivals cannot
indefinitely postpone an urgent patient *within its own band*. Cross-band starvation is
inherent to strict priority and is mitigated by the visible average-wait KPI.

---

## Tech

C++17 (engine, zero dependencies) · Node.js ≥ 18 (static host + proxy) ·
vanilla HTML/CSS/JS (dashboard, no framework, no CDN).

Built as a DSA course project — priority queues, hashing, searching and sparse matrices
applied to a real hospital workflow.
