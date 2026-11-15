// main.cpp — VoltIQ Care · Smart Hospital Management System (DSA, C++)
//
// Usage
//   ./hospital serve [port]     start the JSON HTTP server (dashboard backend)
//   ./hospital demo             seed demo data and print the state as JSON
//   ./hospital add <json>       register a patient from a JSON object
//   ./hospital search <query>   hash / BST / binary-search patient lookup
//   ./hospital callnext         pop the highest-priority patient
//   ./hospital bench            run the complexity benchmarks
//   ./hospital selftest         verify every data structure invariant
//
// Build:  g++ -std=c++17 -O2 -pthread -o hospital *.cpp
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstring>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "models.hpp"
#include "hash_table.hpp"
#include "priority_queue.hpp"
#include "search.hpp"
#include "sparse_matrix.hpp"
#include "hospital.hpp"
#include "json.hpp"

// ===========================================================================
// JSON serialisation
// ===========================================================================
static std::string jsonPatient(const Patient& p, const std::vector<Ward>& wards,
                               const std::vector<Doctor>& doctors) {
    std::ostringstream o;
    o << "{"
      << "\"id\":" << p.id
      << ",\"name\":" << js::q(p.name)
      << ",\"age\":" << p.age
      << ",\"gender\":" << js::q(p.gender)
      << ",\"phone\":" << js::q(p.phone)
      << ",\"severity\":" << js::q(severityCode(p.severity))
      << ",\"severityLabel\":" << js::q(severityName(p.severity))
      << ",\"weight\":" << severityWeight(p.severity)
      << ",\"status\":" << js::q(statusCode(p.status))
      << ",\"statusLabel\":" << js::q(statusName(p.status))
      << ",\"condition\":" << js::q(p.condition)
      << ",\"department\":" << js::q(p.department)
      << ",\"arrival\":" << js::q(p.arrivalReal)
      << ",\"arrivalTick\":" << p.arrivalTick
      << ",\"waitMinutes\":" << p.waitTicks
      << ",\"ward\":" << (p.wardIndex >= 0 && p.wardIndex < static_cast<int>(wards.size())
                          ? js::q(wards[static_cast<std::size_t>(p.wardIndex)].name) : "null")
      << ",\"bedRow\":" << p.bedRow
      << ",\"bedCol\":" << p.bedCol
      << ",\"doctor\":" << (p.doctorId >= 0 && p.doctorId < static_cast<int>(doctors.size())
                            ? js::q(doctors[static_cast<std::size_t>(p.doctorId)].name) : "null")
      << "}";
    return o.str();
}

static std::string jsonDoctor(const Doctor& d) {
    std::ostringstream o;
    o << "{"
      << "\"id\":" << d.id
      << ",\"name\":" << js::q(d.name)
      << ",\"specialty\":" << js::q(d.specialty)
      << ",\"qualification\":" << js::q(d.qualification)
      << ",\"shift\":" << js::q(d.shift)
      << ",\"busy\":" << (d.busy ? "true" : "false")
      << ",\"currentPatient\":" << (d.busy ? std::to_string(d.currentPatient) : "null")
      << ",\"treated\":" << d.treatedCount
      << "}";
    return o.str();
}

// full state document consumed by the dashboard
static std::string jsonState(Hospital& hospital, const Hospital::Snapshot& snap) {
    std::ostringstream o;
    const EngineStats& s = snap.stats;

    o << "{\"generatedAt\":" << js::q([]{
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
            return std::string(buf);
        }())
      << ",\"stats\":{"
      << "\"totalPatients\":" << s.totalPatients
      << ",\"waiting\":" << s.waiting
      << ",\"admitted\":" << s.admitted
      << ",\"treated\":" << s.treated
      << ",\"criticalWaiting\":" << s.criticalWaiting
      << ",\"doctors\":" << s.doctors
      << ",\"doctorsBusy\":" << s.doctorsBusy
      << ",\"bedsTotal\":" << s.bedsTotal
      << ",\"bedsOccupied\":" << s.bedsOccupied
      << ",\"bedDensity\":" << js::num(s.bedDensity, 2)
      << ",\"queueSize\":" << snap.queue.size()
      << ",\"hashLoadFactor\":" << js::num(s.hashLoadFactor * 100.0, 1)
      << ",\"hashBuckets\":" << s.hashBuckets
      << ",\"longestChain\":" << s.longestChain
      << ",\"heapHeight\":" << s.heapHeight
      << ",\"bstHeight\":" << s.bstHeight
      << ",\"avgWaitMinutes\":" << js::num(s.avgWaitMinutes, 1)
      << ",\"shift\":" << js::q(s.shift)
      << ",\"clock\":" << js::q([&]{
              long long total = 8 * 60 + s.elapsedTicks;
              char b[8];
              std::snprintf(b, sizeof(b), "%02d:%02d", (int)((total / 60) % 24), (int)(total % 60));
              return std::string(b);
          }())
      << "}";

    // patients
    o << ",\"patients\":[";
    for (std::size_t i = 0; i < snap.patients.size(); ++i) {
        if (i) o << ",";
        o << jsonPatient(snap.patients[i], snap.wards, snap.doctors);
    }
    o << "]";

    // doctors
    o << ",\"doctors\":[";
    for (std::size_t i = 0; i < snap.doctors.size(); ++i) {
        if (i) o << ",";
        o << jsonDoctor(snap.doctors[i]);
    }
    o << "]";

    // wards + sparse matrices
    o << ",\"wards\":[";
    for (std::size_t wi = 0; wi < snap.wards.size(); ++wi) {
        const Ward& w = snap.wards[wi];
        if (wi) o << ",";
        o << "{\"index\":" << w.index
          << ",\"name\":" << js::q(w.name)
          << ",\"rows\":" << w.rows
          << ",\"cols\":" << w.cols
          << ",\"capacity\":" << w.capacity
          << ",\"occupied\":" << w.occupied
          << ",\"free\":" << (w.capacity - w.occupied)
          << ",\"occupancy\":" << js::num(occupancyPct(w), 1)
          << ",\"denseBytes\":" << (static_cast<long long>(w.capacity) * 4)
          << ",\"sparseBytes\":" << (static_cast<long long>(w.occupied) * static_cast<long long>(sizeof(Triplet)))
          << ",\"triplets\":[";
        const auto& trips = snap.wardTriplets[wi];
        for (std::size_t i = 0; i < trips.size(); ++i) {
            if (i) o << ",";
            o << "{\"row\":" << trips[i].row << ",\"col\":" << trips[i].col
              << ",\"patient\":" << trips[i].value << "}";
        }
        o << "]}";
    }
    o << "]";

    // live heap layout (index 0 = next to be served)
    o << ",\"heapArray\":[";
    for (std::size_t i = 0; i < snap.queue.size(); ++i) {
        if (i) o << ",";
        const Patient* p = nullptr;
        for (const auto& cand : snap.patients)
            if (cand.id == snap.queue[i].patientId) { p = &cand; break; }
        o << "{\"id\":" << snap.queue[i].patientId
          << ",\"weight\":" << snap.queue[i].severityWeight
          << ",\"name\":" << js::q(p ? p->name : std::string("?"))
          << ",\"severity\":" << js::q(p ? severityCode(p->severity) : "standard")
          << ",\"arrivalTick\":" << snap.queue[i].arrivalTick << "}";
    }
    o << "]";

    // true service order — pop a copy of the heap
    o << ",\"serviceOrder\":[";
    {
        struct CopyHeap {
            std::vector<QueueEntry> v;
            void push(const QueueEntry& e) {
                v.push_back(e);
                std::size_t i = v.size() - 1;
                while (i > 0) {
                    std::size_t parent = (i - 1) / 2;
                    if (!higherPriority(v[i], v[parent])) break;
                    std::swap(v[i], v[parent]);
                    i = parent;
                }
            }
            QueueEntry pop() {
                QueueEntry top = v.front();
                v.front() = v.back();
                v.pop_back();
                std::size_t i = 0, n = v.size();
                while (true) {
                    std::size_t l = 2 * i + 1, r = 2 * i + 2, best = i;
                    if (l < n && higherPriority(v[l], v[best])) best = l;
                    if (r < n && higherPriority(v[r], v[best])) best = r;
                    if (best == i) break;
                    std::swap(v[i], v[best]);
                    i = best;
                }
                return top;
            }
            bool empty() const { return v.empty(); }
        } copy;
        for (const auto& e : snap.queue) copy.push(e);
        bool first = true;
        while (!copy.empty()) {
            QueueEntry e = copy.pop();
            const Patient* p = nullptr;
            for (const auto& cand : snap.patients)
                if (cand.id == e.patientId) { p = &cand; break; }
            if (!first) o << ",";
            first = false;
            o << "{\"id\":" << e.patientId
              << ",\"name\":" << js::q(p ? p->name : std::string("?"))
              << ",\"severity\":" << js::q(p ? severityCode(p->severity) : "standard")
              << ",\"weight\":" << e.severityWeight << "}";
        }
    }
    o << "]";

    o << ",\"heapValid\":" << (snap.heapValid ? "true" : "false")
      << ",\"allocationStrategy\":" << js::q(snap.allocationStrategy);

    o << ",\"events\":[";
    for (std::size_t i = 0; i < snap.events.size(); ++i) {
        if (i) o << ",";
        o << js::q(snap.events[i]);
    }
    o << "]";

    (void)hospital;
    o << "}";
    return o.str();
}

// ===========================================================================
// tiny JSON field reader (flat objects only — enough for the CLI)
// ===========================================================================
static std::string jsonField(const std::string& body, const std::string& key,
                             const std::string& fallback = "") {
    std::string k = "\"" + key + "\"";
    std::size_t p = body.find(k);
    if (p == std::string::npos) return fallback;
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return fallback;
    ++p;
    while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
    if (p >= body.size()) return fallback;
    if (body[p] == '"') {
        ++p;
        std::string out;
        while (p < body.size() && body[p] != '"') {
            if (body[p] == '\\' && p + 1 < body.size()) { ++p; }
            out += body[p++];
        }
        return out;
    }
    std::string out;
    while (p < body.size() && body[p] != ',' && body[p] != '}') out += body[p++];
    return out;
}

// ===========================================================================
// HTTP plumbing
// ===========================================================================
static std::string urlDecode(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') out += ' ';
        else if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            out += static_cast<char>(hex(in[i + 1]) * 16 + hex(in[i + 2]));
            i += 2;
        } else out += in[i];
    }
    return out;
}

static std::string queryParam(const std::string& query, const std::string& key) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t amp = query.find('&', pos);
        std::string piece = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        std::size_t eq = piece.find('=');
        std::string k = urlDecode(piece.substr(0, eq));
        std::string v = eq == std::string::npos ? "" : urlDecode(piece.substr(eq + 1));
        if (k == key) return v;
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

static HttpRequest parseRequest(const std::string& raw) {
    HttpRequest req;
    std::size_t headerEnd = raw.find("\r\n\r\n");
    std::string head = headerEnd == std::string::npos ? raw : raw.substr(0, headerEnd);
    if (headerEnd != std::string::npos) req.body = raw.substr(headerEnd + 4);

    std::istringstream hs(head);
    std::string line;
    std::getline(hs, line);
    std::istringstream first(line);
    std::string target;
    first >> req.method >> target;
    std::size_t qm = target.find('?');
    if (qm == std::string::npos) req.path = target;
    else { req.path = target.substr(0, qm); req.query = target.substr(qm + 1); }

    // content-length guard (already read in the socket loop, but be safe)
    return req;
}

static void sendResponse(int fd, int status, const std::string& contentType,
                         const std::string& body, bool cors = true) {
    std::ostringstream o;
    o << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Error") << "\r\n"
      << "Content-Type: " << contentType << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n";
    if (cors) {
        o << "Access-Control-Allow-Origin: *\r\n"
          << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
          << "Access-Control-Allow-Headers: Content-Type\r\n";
    }
    o << "Connection: close\r\n\r\n" << body;
    std::string out = o.str();
    std::size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

static std::string okJson(const std::string& extra = "") {
    std::ostringstream o;
    o << "{\"ok\":true";
    if (!extra.empty()) o << "," << extra;
    o << "}";
    return o.str();
}

static std::string errJson(const std::string& msg) {
    return "{\"ok\":false,\"error\":" + js::q(msg) + "}";
}

static int findWardByName(const std::vector<Ward>& wards, const std::string& name) {
    for (const auto& w : wards)
        if (lowerStr(w.name).find(lowerStr(name)) != std::string::npos) return w.index;
    return -1;
}

// ===========================================================================
// benchmarks — empirical proof of the complexity classes
// ===========================================================================
static volatile long long g_benchSink = 0;

static std::string runBenchmark() {
    const int N = 60000;
    const int probes = 6000;

    // ---------------------------------------------------------------- hash vs linear
    PatientHashTable table(1024);
    for (int i = 0; i < N; ++i) { Patient p; p.id = i; p.name = "P" + std::to_string(i); table.put(i, p); }

    long long acc = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < probes; ++i) {
        int target = static_cast<int>((i * 7919 + g_benchSink) % N);
        const Patient* hit = table.get(target);
        acc += hit ? hit->id : -1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double hashUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / probes;

    std::vector<Patient> vec(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) vec[static_cast<std::size_t>(i)].id = i;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < probes; ++i) {
        int target = static_cast<int>((i * 7919 + g_benchSink) % N);
        for (const auto& p : vec) if (p.id == target) { acc += p.id; break; }
    }
    t1 = std::chrono::steady_clock::now();
    double linearUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / probes;

    // ---------------------------------------------------------------- binary search
    std::vector<int> sortedKeys(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) sortedKeys[static_cast<std::size_t>(i)] = i;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < probes; ++i) {
        int target = static_cast<int>((i * 7919 + g_benchSink) % N);
        auto it = std::lower_bound(sortedKeys.begin(), sortedKeys.end(), target);
        acc += (it != sortedKeys.end() && *it == target) ? 1 : 0;
    }
    t1 = std::chrono::steady_clock::now();
    double binaryUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / probes;

    // ---------------------------------------------------------------- heap vs sorted vector
    ArrayMaxHeap heap;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i)
        heap.push(QueueEntry{i, 1 + (i % 4), i, i, Severity::STANDARD});
    t1 = std::chrono::steady_clock::now();
    double heapUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / N;

    const int vecN = 20000;   // capped: O(n^2) would otherwise dominate the request
    std::vector<QueueEntry> sortedVec;
    sortedVec.reserve(static_cast<std::size_t>(vecN));
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < vecN; ++i) {
        QueueEntry e{i, 1 + (i % 4), i, i, Severity::STANDARD};
        auto it = std::lower_bound(sortedVec.begin(), sortedVec.end(), e,
                                   [](const QueueEntry& a, const QueueEntry& b) {
                                       return higherPriority(a, b);
                                   });
        sortedVec.insert(it, e);
    }
    t1 = std::chrono::steady_clock::now();
    double vecUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / vecN;

    // ---------------------------------------------------------------- sparse vs dense memory
    const int R = 60, C = 60;
    SparseMatrix sparse(R, C);
    for (int i = 0; i < R * C / 8; ++i) sparse.set(i / C, i % C, 1);
    std::vector<int> dense(static_cast<std::size_t>(R * C), 0);
    for (int i = 0; i < R * C / 8; ++i) dense[static_cast<std::size_t>(i)] = 1;

    const long long denseBytes = static_cast<long long>(R) * C * 4;
    const long long sparseBytes = static_cast<long long>(sparse.nonZero()) * static_cast<long long>(sizeof(Triplet));
    acc += sparse.nonZero() + dense[0];

    g_benchSink += acc;

    std::ostringstream o;
    o << "{\"n\":" << N
      << ",\"probes\":" << probes
      << ",\"rows\":["
      << "{\"operation\":\"Patient lookup\"" 
      << ",\"detail\":\"" << (probes / 1000) << ",000 queries over " << (N / 1000) << ",000 records\""
      << ",\"fast\":\"Hash table\",\"fastBigO\":\"O(1)\",\"fastValue\":" << js::num(hashUs, 4)
      << ",\"slow\":\"Linear scan\",\"slowBigO\":\"O(n)\",\"slowValue\":" << js::num(linearUs, 4)
      << ",\"unit\":\"µs / query\"}"
      << ",{\"operation\":\"Search in sorted register\""
      << ",\"detail\":\"lower-bound over " << (N / 1000) << ",000 sorted ids\""
      << ",\"fast\":\"Binary search\",\"fastBigO\":\"O(log n)\",\"fastValue\":" << js::num(binaryUs, 4)
      << ",\"slow\":\"Linear scan\",\"slowBigO\":\"O(n)\",\"slowValue\":" << js::num(linearUs, 4)
      << ",\"unit\":\"µs / query\"}"
      << ",{\"operation\":\"Triage insertion\""
      << ",\"detail\":\"" << (N / 1000) << ",000 emergency arrivals\""
      << ",\"fast\":\"Binary heap\",\"fastBigO\":\"O(log n)\",\"fastValue\":" << js::num(heapUs, 5)
      << ",\"slow\":\"Sorted vector insert\",\"slowBigO\":\"O(n)\",\"slowValue\":" << js::num(vecUs, 5)
      << ",\"unit\":\"µs / insert\"}"
      << ",{\"operation\":\"Ward grid memory\""
      << ",\"detail\":\"60 × 60 beds, 12.5% occupied\""
      << ",\"fast\":\"Sparse triplets\",\"fastBigO\":\"O(nnz)\",\"fastValue\":" << sparseBytes
      << ",\"slow\":\"Dense grid\",\"slowBigO\":\"O(rows × cols)\",\"slowValue\":" << denseBytes
      << ",\"unit\":\"bytes\"}"
      << "]"
      << ",\"memory\":{\"denseBytes\":" << denseBytes
      << ",\"sparseBytes\":" << sparseBytes
      << ",\"density\":" << js::num(sparse.sparseDensity(), 2)
      << ",\"savedPercent\":" << js::num(100.0 * (1.0 - static_cast<double>(sparseBytes) / static_cast<double>(denseBytes)), 1)
      << "}"
      << "}";
    return o.str();
}

// ===========================================================================
// self test — verifies invariants of every custom data structure
// ===========================================================================
static int selfTest() {
    int failures = 0;
    auto check = [&](bool cond, const std::string& label) {
        std::cout << (cond ? "  PASS  " : "  FAIL  ") << label << "\n";
        if (!cond) ++failures;
    };

    std::cout << "Smart Hospital Management System — data-structure self test\n";
    std::cout << "------------------------------------------------------------\n";

    // hash table
    PatientHashTable table(8);
    for (int i = 0; i < 500; ++i) {
        Patient p; p.id = i; p.name = "Patient " + std::to_string(i);
        table.put(i, p);
    }
    check(table.size() == 500, "hash table stores 500 patients");
    check(table.get(499) != nullptr && table.get(499)->name == "Patient 499", "hash lookup is exact");
    check(table.get(9999) == nullptr, "missing key returns nullptr");
    check(table.loadFactor() <= 0.75, "hash table keeps load factor <= 0.75");
    table.remove(250);
    check(table.get(250) == nullptr && table.size() == 499, "hash delete works");

    // heap
    ArrayMaxHeap heap;
    for (int i = 0; i < 300; ++i)
        heap.push(QueueEntry{i, 1 + (i % 4), i, i, Severity::STANDARD});
    check(heap.validateHeap(), "binary heap satisfies the max-heap property");
    int lastWeight = 99;
    bool ordered = true;
    while (!heap.empty()) {
        int w = heap.pop().severityWeight;
        if (w > lastWeight) ordered = false;
        lastWeight = w;
    }
    check(ordered, "heap pops in non-increasing priority order");

    // binary search
    SortedIndex idx;
    for (int i = 0; i < 100; ++i) idx.insert(i * 2, "name" + std::to_string(i));
    int probes = 0;
    check(idx.binarySearchId(64, probes) != nullptr, "binary search finds an existing id");
    check(idx.binarySearchId(65, probes) == nullptr, "binary search rejects a missing id");
    check(probes <= 8, "binary search over 100 keys uses <= 8 probes");

    // BST
    NameBst bst;
    bst.insert("Ramesh Patil", 1001);
    bst.insert("Sunita Deshpande", 1002);
    bst.insert("Arjun Mehta", 1003);
    int cmp = 0;
    check(bst.find("Arjun Mehta", cmp) == 1003, "BST exact name search");
    check(bst.inOrder().size() == 3, "BST in-order traversal visits every node");

    // sparse matrix
    SparseMatrix m(4, 4);
    m.set(1, 2, 77);
    m.set(1, 2, 78);            // overwrite, not duplicate
    check(m.nonZero() == 1 && m.get(1, 2) == 78, "sparse matrix updates in place");
    check(m.get(0, 0) == 0, "unset sparse cell reads as zero");
    auto alloc = m.allocate("first");
    check(alloc.found && alloc.row == 0 && alloc.col == 0, "first-fit allocation picks [0,0]");
    m.clearCell(1, 2);
    check(m.nonZero() == 0, "sparse cell clears correctly");

    // engine integration
    Hospital h;
    h.seedDemo();
    auto snap = h.snapshot();
    check(snap.stats.totalPatients == 48, "engine registers all 48 demo patients");
    check(snap.stats.bedsOccupied == 36, "engine allocates 36 beds through sparse matrices");
    check(snap.stats.waiting > 0, "triage queue holds waiting patients");
    check(snap.stats.bedsOccupied < snap.stats.bedsTotal, "bed grid stays sparse (density < 20%)");
    check(snap.stats.bedDensity < 20.0, "sparse density reported correctly");
    check(snap.heapValid, "engine priority queue stays a valid heap");
    check(!snap.doctors.empty() && snap.doctors.size() == 12, "12 doctors on the roster");

    std::cout << "------------------------------------------------------------\n";
    std::cout << (failures == 0 ? "ALL TESTS PASSED\n" : std::to_string(failures) + " TEST(S) FAILED\n");
    return failures == 0 ? 0 : 1;
}

// ===========================================================================
// HTTP server
// ===========================================================================
static std::atomic<bool> g_running{true};
static void onSignal(int) { g_running = false; }

static int serve(Hospital& hospital, int port) {
    int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { std::cerr << "socket() failed\n"; return 1; }
    int opt = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;       // 0.0.0.0 — required for the preview proxy
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed on port " << port << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    if (::listen(server, 64) < 0) { std::cerr << "listen() failed\n"; return 1; }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "SmartHospital DSA engine listening on 0.0.0.0:" << port << std::endl;

    while (g_running) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        int fd = ::accept(server, reinterpret_cast<sockaddr*>(&client), &len);
        if (fd < 0) continue;

        // read request (headers + optional body)
        std::string raw;
        char buf[8192];
        ssize_t n;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
            raw.append(buf, static_cast<std::size_t>(n));
            std::size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::size_t clPos = raw.find("Content-Length:");
                std::size_t need = 0;
                if (clPos != std::string::npos && clPos < headerEnd) {
                    need = static_cast<std::size_t>(std::stoul(raw.substr(clPos + 15, 12)));
                }
                if (raw.size() >= headerEnd + 4 + need) break;
            }
            if (raw.size() > 1024 * 1024) break;
        }
        if (raw.empty()) { ::close(fd); continue; }

        HttpRequest req = parseRequest(raw);

        if (req.method == "OPTIONS") {
            sendResponse(fd, 200, "text/plain", "");
            ::close(fd);
            continue;
        }

        try {
            if (req.method == "GET" && (req.path == "/api/health" || req.path == "/api/ping")) {
                sendResponse(fd, 200, "application/json", "{\"ok\":true,\"service\":\"voltiq-hospital-dsa\",\"engine\":\"c++17\"}");
            } else if (req.method == "GET" && req.path == "/api/state") {
                auto snap = hospital.snapshot();
                sendResponse(fd, 200, "application/json", jsonState(hospital, snap));
            } else if (req.method == "GET" && req.path == "/api/search") {
                std::string qv = queryParam(req.query, "q");
                auto report = hospital.search(qv);
                std::ostringstream o;
                o << "{\"ok\":true,\"query\":" << js::q(qv)
                  << ",\"algorithm\":" << js::q(report.algorithm)
                  << ",\"probes\":" << report.probes
                  << ",\"microseconds\":" << js::num(report.microseconds, 3)
                  << ",\"count\":" << report.results.size()
                  << ",\"results\":[";
                auto snap = hospital.snapshot();
                for (std::size_t i = 0; i < report.results.size(); ++i) {
                    if (i) o << ",";
                    o << jsonPatient(report.results[i], snap.wards, snap.doctors);
                }
                o << "]}";
                sendResponse(fd, 200, "application/json", o.str());
            } else if (req.method == "GET" && req.path == "/api/queue-order") {
                auto entries = hospital.priorityQueue();
                std::ostringstream o;
                o << "{\"ok\":true,\"queue\":[";
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    if (i) o << ",";
                    o << "{\"id\":" << entries[i].patientId
                      << ",\"weight\":" << entries[i].severityWeight << "}";
                }
                o << "]}";
                sendResponse(fd, 200, "application/json", o.str());
            } else if (req.method == "GET" && req.path == "/api/benchmark") {
                sendResponse(fd, 200, "application/json", runBenchmark());
            } else if (req.method == "GET" && req.path == "/api/download") {
                // dump the entire state as a JSON file
                auto snap = hospital.snapshot();
                sendResponse(fd, 200, "application/json", jsonState(hospital, snap));
            } else if (req.method == "POST" && req.path == "/api/register") {
                std::string name = jsonField(req.body, "name");
                if (name.empty()) {
                    sendResponse(fd, 400, "application/json", errJson("Patient name is required"));
                } else {
                    Severity sev = Severity::STANDARD;
                    parseSeverity(jsonField(req.body, "severity", "standard"), sev);
                    int age = 0;
                    try { age = std::stoi(jsonField(req.body, "age", "0")); } catch (...) {}
                    int id = hospital.registerPatient(
                        name, age, jsonField(req.body, "gender", "Other"),
                        jsonField(req.body, "phone"), sev,
                        jsonField(req.body, "condition"), jsonField(req.body, "department", "General Medicine"),
                        "");
                    auto snap = hospital.snapshot();
                    std::string patJson = "{}";
                    for (const auto& p : snap.patients) if (p.id == id) patJson = jsonPatient(p, snap.wards, snap.doctors);
                    sendResponse(fd, 200, "application/json",
                                 okJson("\"id\":" + std::to_string(id) + ",\"patient\":" + patJson));
                }
            } else if (req.method == "POST" && req.path == "/api/admit") {
                int id = 0, ward = -1, row = -1, col = -1;
                try { id = std::stoi(jsonField(req.body, "id", "0")); } catch (...) {}
                std::string wardName = jsonField(req.body, "ward");
                auto snap = hospital.snapshot();
                if (!wardName.empty()) ward = findWardByName(snap.wards, wardName);
                try { row = std::stoi(jsonField(req.body, "row", "-1")); } catch (...) {}
                try { col = std::stoi(jsonField(req.body, "col", "-1")); } catch (...) {}
                bool ok = hospital.admit(id, ward, row, col);
                sendResponse(fd, ok ? 200 : 400, "application/json",
                             ok ? okJson() : errJson("Unable to admit patient — ward may be full or the bed is taken"));
            } else if (req.method == "POST" && req.path == "/api/discharge-bed") {
                int id = 0;
                try { id = std::stoi(jsonField(req.body, "id", "0")); } catch (...) {}
                bool ok = hospital.dischargeBed(id);
                sendResponse(fd, ok ? 200 : 400, "application/json",
                             ok ? okJson() : errJson("Patient does not occupy a bed"));
            } else if (req.method == "POST" && req.path == "/api/call-next") {
                bool ok = hospital.callNext();
                sendResponse(fd, ok ? 200 : 400, "application/json",
                             ok ? okJson() : errJson("No doctor is free right now"));
            } else if (req.method == "POST" && req.path == "/api/escalate") {
                int id = 0;
                try { id = std::stoi(jsonField(req.body, "id", "0")); } catch (...) {}
                Severity sev = Severity::CRITICAL;
                parseSeverity(jsonField(req.body, "severity", "critical"), sev);
                bool ok = hospital.escalate(id, sev);
                sendResponse(fd, ok ? 200 : 400, "application/json",
                             ok ? okJson() : errJson("Patient is not waiting or severity did not increase"));
            } else if (req.method == "POST" && req.path == "/api/complete") {
                int id = 0;
                try { id = std::stoi(jsonField(req.body, "id", "0")); } catch (...) {}
                bool ok = hospital.finishConsultation(id);
                sendResponse(fd, ok ? 200 : 400, "application/json",
                             ok ? okJson() : errJson("Patient not found"));
            } else if (req.method == "POST" && req.path == "/api/delete") {
                int id = 0;
                try { id = std::stoi(jsonField(req.body, "id", "0")); } catch (...) {}
                bool ok = hospital.deletePatient(id);
                sendResponse(fd, ok ? 200 : 400, "application/json",
                             ok ? okJson() : errJson("Patient not found"));
            } else if (req.method == "POST" && req.path == "/api/tick") {
                int minutes = 1;
                try { minutes = std::stoi(jsonField(req.body, "minutes", "1")); } catch (...) {}
                minutes = std::max(1, std::min(minutes, 600));
                hospital.tick(minutes, false);
                sendResponse(fd, 200, "application/json", okJson());
            } else if (req.method == "POST" && req.path == "/api/strategy") {
                std::string s = jsonField(req.body, "strategy", "first");
                bool ok = hospital.setAllocationStrategy(s);
                sendResponse(fd, ok ? 200 : 400, "application/json",
                             ok ? okJson() : errJson("Unknown allocation strategy"));
            } else if (req.method == "POST" && req.path == "/api/mass-casualty") {
                int count = 10;
                try { count = std::stoi(jsonField(req.body, "count", "10")); } catch (...) {}
                count = std::max(1, std::min(count, 60));
                hospital.startMassCasualty(count);
                hospital.tick(1, true);
                sendResponse(fd, 200, "application/json", okJson("\"incoming\":" + std::to_string(count)));
            } else {
                sendResponse(fd, 404, "application/json", errJson("Unknown endpoint " + req.path));
            }
        } catch (const std::exception& e) {
            sendResponse(fd, 500, "application/json", errJson(std::string("engine error: ") + e.what()));
        }
        ::close(fd);
    }

    ::close(server);
    return 0;
}

// ===========================================================================
// CLI
// ===========================================================================
int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "help";

    if (cmd == "serve") {
        int port = argc > 2 ? std::atoi(argv[2]) : 8001;
        Hospital hospital;
        hospital.seedDemo();
        return serve(hospital, port);
    }
    if (cmd == "selftest") return selfTest();
    if (cmd == "bench") { std::cout << runBenchmark() << std::endl; return 0; }
    if (cmd == "demo") {
        Hospital hospital;
        hospital.seedDemo();
        auto snap = hospital.snapshot();
        std::cout << jsonState(hospital, snap) << std::endl;
        return 0;
    }
    if (cmd == "add") {
        std::string body = argc > 2 ? argv[2] : "{}";
        Hospital hospital;
        hospital.seedDemo();
        Severity sev = Severity::STANDARD;
        parseSeverity(jsonField(body, "severity", "standard"), sev);
        int id = hospital.registerPatient(jsonField(body, "name", "CLI Patient"),
                                          std::stoi(jsonField(body, "age", "30")),
                                          jsonField(body, "gender", "Other"),
                                          jsonField(body, "phone"), sev,
                                          jsonField(body, "condition"),
                                          jsonField(body, "department", "General Medicine"), "");
        std::cout << "Registered patient #" << id << "\n";
        return 0;
    }
    if (cmd == "search") {
        Hospital hospital;
        hospital.seedDemo();
        auto report = hospital.search(argc > 2 ? argv[2] : "");
        std::cout << "Algorithm : " << report.algorithm << "\n"
                  << "Probes    : " << report.probes << "\n"
                  << "Time      : " << report.microseconds << " µs\n"
                  << "Matches   : " << report.results.size() << "\n";
        for (const auto& p : report.results)
            std::cout << "  #" << p.id << "  " << p.name << "  "
                      << severityName(p.severity) << "  " << statusName(p.status) << "\n";
        return 0;
    }
    if (cmd == "callnext") {
        Hospital hospital;
        hospital.seedDemo();
        int before = static_cast<int>(hospital.priorityQueue().size());
        bool ok = hospital.callNext();
        std::cout << (ok ? "Called the highest-priority patient.\n" : "No free doctor.\n");
        std::cout << "Queue size: " << before << " -> " << hospital.priorityQueue().size() << "\n";
        return 0;
    }

    std::cout <<
        "VoltIQ Care — Smart Hospital Management System (C++17 DSA project)\n\n"
        "  hospital serve [port]     run the HTTP JSON engine (default 8001)\n"
        "  hospital demo             seed demo data and print state JSON\n"
        "  hospital add '<json>'     register a patient\n"
        "  hospital search <query>   id / name search with probe count\n"
        "  hospital callnext         pop the highest-priority patient\n"
        "  hospital bench            empirical complexity benchmarks\n"
        "  hospital selftest         validate every data structure\n";
    return 0;
}
