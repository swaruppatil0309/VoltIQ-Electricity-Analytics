// priority_queue.hpp — custom binary max-heap for emergency triage
//
// DSA NOTES
//  * `ArrayMaxHeap` is implemented from scratch (no std::priority_queue) to
//    demonstrate sift-up / sift-down, 0-based indexing and stable ordering.
//  * Ordering key:  (severity weight DESC, waiting time DESC)
//      - critical patients always outrank standard patients
//      - inside the same severity band, the patient who waited longest wins
//        (prevents starvation — textbook priority-queue fairness).
//  * Every push / pop is O(log n); peek is O(1).
#pragma once
#include <vector>
#include <stdexcept>
#include "models.hpp"

struct QueueEntry {
    int       patientId = 0;
    int       severityWeight = 1;
    long long arrivalTick = 0;
    long long seq = 0;
    Severity  severity = Severity::STANDARD;
};

// true when `a` must be served before `b`
inline bool higherPriority(const QueueEntry& a, const QueueEntry& b) {
    if (a.severityWeight != b.severityWeight)
        return a.severityWeight > b.severityWeight;
    if (a.arrivalTick != b.arrivalTick)
        return a.arrivalTick < b.arrivalTick;   // earlier arrival = longer wait
    return a.seq < b.seq;                        // FIFO tie-break
}

class ArrayMaxHeap {
public:
    void clear() { heap_.clear(); }

    bool empty() const { return heap_.empty(); }
    std::size_t size() const { return heap_.size(); }

    const QueueEntry& peek() const {
        if (heap_.empty()) throw std::out_of_range("priority queue is empty");
        return heap_.front();
    }

    // O(log n)
    void push(const QueueEntry& e) {
        heap_.push_back(e);
        siftUp(heap_.size() - 1);
    }

    // O(log n)
    QueueEntry pop() {
        if (heap_.empty()) throw std::out_of_range("priority queue is empty");
        QueueEntry top = heap_.front();
        heap_.front() = heap_.back();
        heap_.pop_back();
        if (!heap_.empty()) siftDown(0);
        return top;
    }

    // O(n) — used after a severity upgrade so the patient is re-triaged
    bool removeById(int patientId) {
        for (std::size_t i = 0; i < heap_.size(); ++i) {
            if (heap_[i].patientId == patientId) {
                std::size_t last = heap_.size() - 1;
                if (i != last) {
                    heap_[i] = heap_[last];
                    heap_.pop_back();
                    if (i < heap_.size()) { siftUp(i); siftDown(i); }
                } else {
                    heap_.pop_back();
                }
                return true;
            }
        }
        return false;
    }

    // raw internal layout (index 0 = highest priority) — for the dashboard view
    std::vector<QueueEntry> snapshot() const { return heap_; }

    bool validateHeap() const {
        for (std::size_t i = 1; i < heap_.size(); ++i) {
            std::size_t parent = (i - 1) / 2;
            if (higherPriority(heap_[i], heap_[parent])) return false;
        }
        return true;
    }

private:
    std::vector<QueueEntry> heap_;

    void siftUp(std::size_t i) {
        while (i > 0) {
            std::size_t parent = (i - 1) / 2;
            if (!higherPriority(heap_[i], heap_[parent])) break;
            std::swap(heap_[i], heap_[parent]);
            i = parent;
        }
    }

    void siftDown(std::size_t i) {
        const std::size_t n = heap_.size();
        while (true) {
            std::size_t left = 2 * i + 1;
            std::size_t right = 2 * i + 2;
            std::size_t best = i;
            if (left < n && higherPriority(heap_[left], heap_[best])) best = left;
            if (right < n && higherPriority(heap_[right], heap_[best])) best = right;
            if (best == i) break;
            std::swap(heap_[i], heap_[best]);
            i = best;
        }
    }
};
