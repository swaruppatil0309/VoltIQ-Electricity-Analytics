// search.hpp — searching techniques for quick patient lookup
//
// DSA NOTES
//  * SortedIndex : keeps a sorted vector of (key, id) pairs in sync with the
//    hash table, enabling O(log n) binary search on:
//       - exact patient-ID lookup
//       - name prefix lookup (case-insensitive)
//  * NameBst : a real binary search tree (lower-cased full name → patient id)
//    used to demonstrate O(log n) average / O(n) worst-case tree search.
//    An in-order traversal gives patients sorted alphabetically.
#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <memory>
#include "models.hpp"

inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------------------
// SortedIndex — binary search over a maintained sorted array
// ---------------------------------------------------------------------------
class SortedIndex {
public:
    struct Pair { int id; std::string nameLower; };

    void insert(int id, const std::string& name) {
        Pair p{id, toLower(name)};
        auto it = std::lower_bound(vec_.begin(), vec_.end(), p,
                                   [](const Pair& a, const Pair& b) { return a.id < b.id; });
        vec_.insert(it, p);                       // O(n) insert, O(log n) search
    }

    void erase(int id) {
        auto it = std::lower_bound(vec_.begin(), vec_.end(), id,
                                   [](const Pair& a, int v) { return a.id < v; });
        if (it != vec_.end() && it->id == id) vec_.erase(it);
    }

    void clear() { vec_.clear(); }

    // O(log n) exact id lookup
    const Pair* binarySearchId(int id, int& probes) const {
        int lo = 0, hi = static_cast<int>(vec_.size()) - 1;
        probes = 0;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            ++probes;
            if (vec_[mid].id == id) return &vec_[mid];
            if (vec_[mid].id < id) lo = mid + 1; else hi = mid - 1;
        }
        return nullptr;
    }

    // O(log n) lower bound on the lower-cased name, then O(k) scan of matches
    std::vector<int> searchNamePrefix(const std::string& prefix, int& probes) const {
        probes = 0;
        std::string p = toLower(prefix);
        if (p.empty()) return {};

        std::vector<Pair> byName = vec_;
        std::sort(byName.begin(), byName.end(), [](const Pair& a, const Pair& b) {
            if (a.nameLower != b.nameLower) return a.nameLower < b.nameLower;
            return a.id < b.id;
        });

        int lo = 0, hi = static_cast<int>(byName.size());
        while (lo < hi) {                 // binary search for lower bound
            int mid = lo + (hi - lo) / 2;
            ++probes;
            if (byName[mid].nameLower.compare(0, p.size(), p) < 0) lo = mid + 1;
            else hi = mid;
        }
        std::vector<int> hits;
        for (int i = lo; i < static_cast<int>(byName.size()); ++i) {
            if (byName[i].nameLower.compare(0, p.size(), p) != 0) break;
            hits.push_back(byName[i].id);
        }
        return hits;
    }

    std::size_t size() const { return vec_.size(); }
    const std::vector<Pair>& raw() const { return vec_; }

private:
    std::vector<Pair> vec_;
};

// ---------------------------------------------------------------------------
// NameBst — binary search tree keyed by lower-cased patient name
// ---------------------------------------------------------------------------
class NameBst {
public:
    void insert(const std::string& name, int id) { root_ = insertNode(std::move(root_), toLower(name), id); }
    void clear() { root_.reset(); }

    // exact match search, returns patient id or -1
    int find(const std::string& name, int& comparisons) const {
        comparisons = 0;
        Node* cur = root_.get();
        std::string key = toLower(name);
        while (cur) {
            ++comparisons;
            if (key == cur->key) return cur->id;
            cur = (key < cur->key) ? cur->left.get() : cur->right.get();
        }
        return -1;
    }

    std::vector<std::pair<std::string, int>> inOrder() const {
        std::vector<std::pair<std::string, int>> out;
        walk(root_.get(), out);
        return out;
    }

    int height() const { return heightOf(root_.get()); }
    std::size_t size() const { return count_; }

private:
    struct Node {
        std::string key;
        int id;
        std::unique_ptr<Node> left, right;
        Node(std::string k, int i) : key(std::move(k)), id(i) {}
    };

    std::unique_ptr<Node> root_;
    std::size_t count_ = 0;

    std::unique_ptr<Node> insertNode(std::unique_ptr<Node> node, const std::string& key, int id) {
        if (!node) { ++count_; return std::make_unique<Node>(key, id); }
        if (key < node->key)      node->left  = insertNode(std::move(node->left), key, id);
        else if (key > node->key) node->right = insertNode(std::move(node->right), key, id);
        else                      node->id = id;   // duplicate name → latest id
        return node;
    }

    static void walk(const Node* n, std::vector<std::pair<std::string, int>>& out) {
        if (!n) return;
        walk(n->left.get(), out);
        out.emplace_back(n->key, n->id);
        walk(n->right.get(), out);
    }

    static int heightOf(const Node* n) {
        if (!n) return 0;
        return 1 + std::max(heightOf(n->left.get()), heightOf(n->right.get()));
    }
};
