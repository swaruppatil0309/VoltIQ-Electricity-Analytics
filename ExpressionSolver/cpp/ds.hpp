// ds.hpp — hand-written data structures for the expression engine.
//
// Nothing here wraps STL containers: the stack, the queue, the hash map and the
// LRU cache are implemented from raw arrays so the algorithmic behaviour is
// visible (capacity growth, probe counts, eviction, ...).
//
//   ArrayStack<T>     dynamic array stack            push/pop O(1) amortised
//   RingQueue<T>      circular buffer queue          enqueue/dequeue O(1)
//   VarMap            open-addressing hash map       get/put O(1) average
//   LruCache<V>       hash map + intrusive LRU list  get/put O(1)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ===========================================================================
// ArrayStack<T> — LIFO on a doubling dynamic array
// ===========================================================================
template <typename T>
class ArrayStack {
public:
    ArrayStack() = default;
    ~ArrayStack() { delete[] buf_; }

    ArrayStack(const ArrayStack& other) { copyFrom(other); }

    ArrayStack& operator=(const ArrayStack& other) {
        if (this != &other) {
            delete[] buf_;
            buf_ = nullptr;
            cap_ = 0;
            n_ = 0;
            peak_ = 0;
            copyFrom(other);
        }
        return *this;
    }

    void push(const T& value) {
        if (n_ == cap_) grow(cap_ ? cap_ * 2 : 8);
        buf_[n_++] = value;
        if (n_ > peak_) peak_ = n_;
    }

    void pop() {
        if (n_ > 0) --n_;
    }

    // pop() that also returns the removed value (pop is only called when the
    // algorithm has already validated the stack is non-empty).
    T popValue() {
        T value = buf_[--n_];
        return value;
    }

    const T& top() const { return buf_[n_ - 1]; }
    bool empty() const { return n_ == 0; }
    std::size_t size() const { return n_; }
    std::size_t capacity() const { return cap_; }
    std::size_t peak() const { return peak_; }

    void clear() { n_ = 0; }

    const T& at(std::size_t i) const { return buf_[i]; }

    // deep-copy the live contents (used for snapshots / visualisation)
    std::vector<T> toVector() const {
        std::vector<T> out;
        out.reserve(n_);
        for (std::size_t i = 0; i < n_; ++i) out.push_back(buf_[i]);
        return out;
    }

private:
    void grow(std::size_t newCap) {
        T* fresh = new T[newCap];
        for (std::size_t i = 0; i < n_; ++i) fresh[i] = buf_[i];
        delete[] buf_;
        buf_ = fresh;
        cap_ = newCap;
    }

    void copyFrom(const ArrayStack& other) {
        cap_ = other.cap_;
        n_ = other.n_;
        peak_ = other.peak_;
        if (cap_) {
            buf_ = new T[cap_];
            for (std::size_t i = 0; i < n_; ++i) buf_[i] = other.buf_[i];
        }
    }

    T* buf_ = nullptr;
    std::size_t cap_ = 0;
    std::size_t n_ = 0;
    std::size_t peak_ = 0;
};

// ===========================================================================
// RingQueue<T> — circular buffer, used as the token stream
// ===========================================================================
template <typename T>
class RingQueue {
public:
    RingQueue() = default;
    ~RingQueue() { delete[] buf_; }

    RingQueue(const RingQueue& other) { copyFrom(other); }

    RingQueue& operator=(const RingQueue& other) {
        if (this != &other) {
            delete[] buf_;
            buf_ = nullptr;
            cap_ = 0;
            head_ = 0;
            n_ = 0;
            copyFrom(other);
        }
        return *this;
    }

    void enqueue(const T& value) {
        if (n_ == cap_) grow(cap_ ? cap_ * 2 : 8);
        buf_[(head_ + n_) % cap_] = value;
        ++n_;
    }

    T dequeue() {
        T value = buf_[head_];
        head_ = (head_ + 1) % cap_;
        --n_;
        return value;
    }

    const T& front() const { return buf_[head_]; }
    bool empty() const { return n_ == 0; }
    std::size_t size() const { return n_; }
    std::size_t capacity() const { return cap_; }

    std::vector<T> toVector() const {
        std::vector<T> out;
        out.reserve(n_);
        for (std::size_t i = 0; i < n_; ++i) out.push_back(buf_[(head_ + i) % cap_]);
        return out;
    }

private:
    void grow(std::size_t newCap) {
        T* fresh = new T[newCap];
        for (std::size_t i = 0; i < n_; ++i) fresh[i] = buf_[(head_ + i) % cap_];
        delete[] buf_;
        buf_ = fresh;
        cap_ = newCap;
        head_ = 0;
    }

    void copyFrom(const RingQueue& other) {
        cap_ = other.cap_;
        n_ = other.n_;
        head_ = 0;
        if (cap_) {
            buf_ = new T[cap_];
            for (std::size_t i = 0; i < n_; ++i) buf_[i] = other.buf_[(other.head_ + i) % cap_];
        }
    }

    T* buf_ = nullptr;
    std::size_t cap_ = 0;
    std::size_t head_ = 0;
    std::size_t n_ = 0;
};

// ===========================================================================
// VarMap — open addressing (linear probing) string -> double
// ===========================================================================
class VarMap {
public:
    VarMap() { rehash(16); }
    VarMap(const VarMap& other) { copyFrom(other); }

    VarMap& operator=(const VarMap& other) {
        if (this != &other) copyFrom(other);
        return *this;
    }

    // returns true when the key was present
    bool get(const std::string& key, double& out) const;
    void put(const std::string& key, double value);
    bool erase(const std::string& key);
    void clear();
    std::size_t size() const { return used_; }

    // longest probe run since construction — a live load-factor health check
    std::size_t maxProbe() const { return maxProbe_; }
    std::size_t capacity() const { return cap_; }
    double loadFactor() const { return cap_ ? static_cast<double>(used_) / static_cast<double>(cap_) : 0.0; }

    std::vector<std::pair<std::string, double>> entries() const;

private:
    enum : unsigned char { EMPTY = 0, USED = 1, TOMB = 2 };

    static std::uint64_t hash(const std::string& s) {
        std::uint64_t h = 1469598103934665603ULL;  // FNV-1a
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    void rehash(std::size_t newCap);
    void copyFrom(const VarMap& other);

    std::vector<std::string> keys_;
    std::vector<double> values_;
    std::vector<unsigned char> state_;
    std::size_t cap_ = 0;
    std::size_t used_ = 0;
    std::size_t tombs_ = 0;
    std::size_t maxProbe_ = 0;
};

// ===========================================================================
// LruCache<V> — hash buckets (chaining) + intrusive doubly linked LRU list
// ===========================================================================
template <typename V>
class LruCache {
public:
    explicit LruCache(std::size_t capacity = 32) : capacity_(capacity ? capacity : 1) {
        entries_.resize(capacity_);
        buckets_.assign(bucketCount(capacity_), -1);
        free_.reserve(capacity_);
        for (std::size_t i = capacity_; i-- > 0;) free_.push_back(static_cast<int>(i));
    }

    bool get(const std::string& key, V& out) {
        int slot = find(key);
        if (slot < 0) {
            ++misses_;
            return false;
        }
        ++hits_;
        touch(slot);
        out = entries_[static_cast<std::size_t>(slot)].value;
        return true;
    }

    void put(const std::string& key, const V& value) {
        int slot = find(key);
        if (slot >= 0) {
            entries_[static_cast<std::size_t>(slot)].value = value;
            touch(slot);
            return;
        }
        if (free_.empty()) evictTail();
        slot = free_.back();
        free_.pop_back();
        Entry& e = entries_[static_cast<std::size_t>(slot)];
        e.key = key;
        e.value = value;
        e.hash = hash(key);
        e.next = buckets_[bucketIndex(e.hash)];
        buckets_[bucketIndex(e.hash)] = slot;
        e.nextLru = head_;
        e.prevLru = -1;
        if (head_ >= 0) entries_[static_cast<std::size_t>(head_)].prevLru = slot;
        head_ = slot;
        if (tail_ < 0) tail_ = slot;
        ++size_;
    }

    std::size_t hits() const { return hits_; }
    std::size_t misses() const { return misses_; }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return capacity_; }

    void clear() {
        buckets_.assign(bucketCount(capacity_), -1);
        free_.clear();
        for (std::size_t i = capacity_; i-- > 0;) {
            entries_[i] = Entry{};
            free_.push_back(static_cast<int>(i));
        }
        head_ = tail_ = -1;
        size_ = 0;
        hits_ = misses_ = 0;
    }

    // keys ordered most-recently-used first
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        for (int i = head_; i >= 0; i = entries_[static_cast<std::size_t>(i)].nextLru)
            out.push_back(entries_[static_cast<std::size_t>(i)].key);
        return out;
    }

private:
    struct Entry {
        std::string key;
        V value{};
        std::uint64_t hash = 0;
        int next = -1;     // next slot in the same hash bucket
        int prevLru = -1;  // intrusive LRU list
        int nextLru = -1;
    };

    static std::size_t bucketCount(std::size_t cap) {
        std::size_t n = 8;
        while (n < cap * 2) n *= 2;
        return n;
    }

    static std::uint64_t hash(const std::string& s) {
        std::uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    std::size_t bucketIndex(std::uint64_t h) const { return static_cast<std::size_t>(h & (buckets_.size() - 1)); }

    int find(const std::string& key) const {
        std::uint64_t h = hash(key);
        for (int i = buckets_[bucketIndex(h)]; i >= 0; i = entries_[static_cast<std::size_t>(i)].next) {
            const Entry& e = entries_[static_cast<std::size_t>(i)];
            if (e.hash == h && e.key == key) return i;
        }
        return -1;
    }

    void touch(int slot) {
        if (head_ == slot) return;
        Entry& e = entries_[static_cast<std::size_t>(slot)];
        if (e.prevLru >= 0) entries_[static_cast<std::size_t>(e.prevLru)].nextLru = e.nextLru;
        if (e.nextLru >= 0) entries_[static_cast<std::size_t>(e.nextLru)].prevLru = e.prevLru;
        if (tail_ == slot) tail_ = e.prevLru;
        e.prevLru = -1;
        e.nextLru = head_;
        if (head_ >= 0) entries_[static_cast<std::size_t>(head_)].prevLru = slot;
        head_ = slot;
        if (tail_ < 0) tail_ = slot;
    }

    void evictTail() {
        int slot = tail_;
        if (slot < 0) return;
        Entry& e = entries_[static_cast<std::size_t>(slot)];
        // unlink from the LRU list
        tail_ = e.prevLru;
        if (e.prevLru >= 0) entries_[static_cast<std::size_t>(e.prevLru)].nextLru = -1;
        if (head_ == slot) head_ = -1;
        // unlink from the hash bucket
        int* link = &buckets_[bucketIndex(e.hash)];
        while (*link >= 0) {
            if (*link == slot) {
                *link = entries_[static_cast<std::size_t>(slot)].next;
                break;
            }
            link = &entries_[static_cast<std::size_t>(*link)].next;
        }
        e = Entry{};
        free_.push_back(slot);
        --size_;
    }

    std::vector<Entry> entries_;
    std::vector<int> buckets_;
    std::vector<int> free_;
    std::size_t capacity_ = 1;
    std::size_t size_ = 0;
    int head_ = -1;
    int tail_ = -1;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

// ===========================================================================
// VarMap implementation (header-only, kept below the classes for readability)
// ===========================================================================
inline bool VarMap::get(const std::string& key, double& out) const {
    if (cap_ == 0) return false;
    std::size_t i = static_cast<std::size_t>(hash(key) & (cap_ - 1));
    std::size_t probe = 1;
    while (state_[i] != EMPTY) {
        if (state_[i] == USED && keys_[i] == key) {
            out = values_[i];
            return true;
        }
        i = (i + 1) & (cap_ - 1);
        ++probe;
    }
    return false;
}

inline void VarMap::put(const std::string& key, double value) {
    if ((used_ + tombs_ + 1) * 10 >= cap_ * 7) rehash(cap_ * 2);
    std::size_t i = static_cast<std::size_t>(hash(key) & (cap_ - 1));
    std::size_t probe = 1;
    std::size_t firstTomb = static_cast<std::size_t>(-1);
    while (state_[i] != EMPTY) {
        if (state_[i] == USED && keys_[i] == key) {
            values_[i] = value;
            if (probe > maxProbe_) maxProbe_ = probe;
            return;
        }
        if (state_[i] == TOMB && firstTomb == static_cast<std::size_t>(-1)) firstTomb = i;
        i = (i + 1) & (cap_ - 1);
        ++probe;
    }
    if (firstTomb != static_cast<std::size_t>(-1)) i = firstTomb;
    keys_[i] = key;
    values_[i] = value;
    if (state_[i] == TOMB) --tombs_;
    state_[i] = USED;
    ++used_;
    if (probe > maxProbe_) maxProbe_ = probe;
}

inline bool VarMap::erase(const std::string& key) {
    if (cap_ == 0) return false;
    std::size_t i = static_cast<std::size_t>(hash(key) & (cap_ - 1));
    while (state_[i] != EMPTY) {
        if (state_[i] == USED && keys_[i] == key) {
            state_[i] = TOMB;
            keys_[i].clear();
            --used_;
            ++tombs_;
            return true;
        }
        i = (i + 1) & (cap_ - 1);
    }
    return false;
}

inline void VarMap::clear() {
    keys_.assign(cap_, std::string());
    values_.assign(cap_, 0.0);
    state_.assign(cap_, EMPTY);
    used_ = tombs_ = 0;
    maxProbe_ = 0;
}

inline void VarMap::rehash(std::size_t newCap) {
    std::vector<std::string> oldKeys = keys_;
    std::vector<double> oldValues = values_;
    std::vector<unsigned char> oldState = state_;
    std::size_t oldCap = cap_;

    cap_ = newCap;
    keys_.assign(cap_, std::string());
    values_.assign(cap_, 0.0);
    state_.assign(cap_, EMPTY);
    used_ = tombs_ = 0;

    for (std::size_t i = 0; i < oldCap; ++i) {
        if (oldState[i] == USED) put(oldKeys[i], oldValues[i]);
    }
}

inline void VarMap::copyFrom(const VarMap& other) {
    keys_ = other.keys_;
    values_ = other.values_;
    state_ = other.state_;
    cap_ = other.cap_;
    used_ = other.used_;
    tombs_ = other.tombs_;
    maxProbe_ = other.maxProbe_;
}

inline std::vector<std::pair<std::string, double>> VarMap::entries() const {
    std::vector<std::pair<std::string, double>> out;
    for (std::size_t i = 0; i < cap_; ++i)
        if (state_[i] == USED) out.emplace_back(keys_[i], values_[i]);
    return out;
}
