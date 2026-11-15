// hash_table.hpp — separate-chaining hash table for O(1) patient lookup
//
// DSA NOTES
//  * Keys are patient IDs. The hash function mixes the key with a large prime
//    (Knuth's multiplicative hashing) so sequential IDs do not clump.
//  * Collisions are resolved with singly-linked chains; chains automatically
//    rehash (load factor > 0.75) so average lookup stays O(1).
//  * Used by: register / get / admit / discharge / delete / update.
#pragma once
#include <vector>
#include <functional>
#include <cstddef>
#include "models.hpp"

class PatientHashTable {
public:
    explicit PatientHashTable(std::size_t capacity = 64) { buckets_.resize(nextPow2(capacity)); }

    // average O(1)
    void put(int key, const Patient& value) {
        if ((size_ + 1) * 4 > buckets_.size() * 3) rehash(buckets_.size() * 2);
        std::size_t idx = hash(key);
        for (auto& e : buckets_[idx]) {
            if (e.key == key) { e.value = value; return; }
        }
        buckets_[idx].push_back({key, value});
        ++size_;
    }

    // average O(1)
    Patient* get(int key) {
        std::size_t idx = hash(key);
        for (auto& e : buckets_[idx])
            if (e.key == key) return &e.value;
        return nullptr;
    }

    const Patient* get(int key) const {
        std::size_t idx = hash(key);
        for (const auto& e : buckets_[idx])
            if (e.key == key) return &e.value;
        return nullptr;
    }

    // average O(1 + chain)
    bool remove(int key) {
        std::size_t idx = hash(key);
        auto& chain = buckets_[idx];
        for (std::size_t i = 0; i < chain.size(); ++i) {
            if (chain[i].key == key) {
                chain.erase(chain.begin() + static_cast<long>(i));
                --size_;
                return true;
            }
        }
        return false;
    }

    bool contains(int key) const { return get(key) != nullptr; }

    std::size_t size() const { return size_; }
    std::size_t bucketCount() const { return buckets_.size(); }
    double loadFactor() const { return buckets_.empty() ? 0.0 : static_cast<double>(size_) / buckets_.size(); }

    std::vector<Patient> values() const {
        std::vector<Patient> out;
        out.reserve(size_);
        for (const auto& chain : buckets_)
            for (const auto& e : chain) out.push_back(e.value);
        return out;
    }

    // chain-length histogram, used by the dashboard to visualise collisions
    std::vector<int> chainHistogram() const {
        std::vector<int> hist;
        for (const auto& chain : buckets_)
            if (!chain.empty()) hist.push_back(static_cast<int>(chain.size()));
        return hist;
    }

    std::size_t longestChain() const {
        std::size_t best = 0;
        for (const auto& chain : buckets_) best = std::max(best, chain.size());
        return best;
    }

private:
    struct Entry { int key; Patient value; };

    std::vector<std::vector<Entry>> buckets_;
    std::size_t size_ = 0;

    static std::size_t nextPow2(std::size_t v) {
        std::size_t p = 8;
        while (p < v) p <<= 1;
        return p;
    }

    std::size_t hash(int key) const {
        // Knuth multiplicative hash + fibonacci mixing
        std::uint64_t k = static_cast<std::uint64_t>(key);   // handles negative ids too
        k *= 2654435761ULL;
        k ^= (k >> 16);
        return static_cast<std::size_t>(k) & (buckets_.size() - 1);
    }

    void rehash(std::size_t newSize) {
        std::vector<std::vector<Entry>> old = std::move(buckets_);
        buckets_.clear();
        buckets_.resize(newSize);
        size_ = 0;
        for (auto& chain : old)
            for (auto& e : chain) put(e.key, e.value);
    }
};
