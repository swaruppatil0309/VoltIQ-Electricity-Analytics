// sparse_matrix.hpp — sparse matrix representation of ward beds
//
// DSA NOTES
//  * A ward is a rows x cols grid of beds, but the vast majority of beds are
//    free at any time. Storing the grid densely wastes memory, so we keep only
//    the occupied cells as (row, col, patientId) triplets, sorted row-major.
//  * Density = occupied / (rows * cols). Real ward density is typically < 15%,
//    which is the textbook case for a sparse representation.
//  * Operations:
//       set / get / clear  -> O(log nnz) via binary search on the triplets
//       allocation         -> first-fit scan, O(nnz + rows) worst case
//       column occupancy   -> O(nnz) filtered walk
#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <cstddef>
#include "models.hpp"

struct Triplet {
    int row = 0;
    int col = 0;
    int value = 0;
};

struct AllocationResult {
    bool found = false;
    int  row = -1;
    int  col = -1;
    std::string strategy;
};

class SparseMatrix {
public:
    SparseMatrix() = default;
    SparseMatrix(int rows, int cols) : rows_(rows), cols_(cols) {}

    int rows() const { return rows_; }
    int cols() const { return cols_; }
    std::size_t capacity() const { return static_cast<std::size_t>(rows_) * static_cast<std::size_t>(cols_); }
    int nonZero() const { return static_cast<int>(data_.size()); }
    const std::vector<Triplet>& triplets() const { return data_; }

    double sparseDensity() const {
        if (capacity() == 0) return 0.0;
        return 100.0 * static_cast<double>(nonZero()) / static_cast<double>(capacity());
    }

    // memory saved by storing triplets instead of a dense grid
    std::size_t bytesSparse() const { return data_.size() * sizeof(Triplet); }
    std::size_t bytesDense() const { return capacity() * sizeof(int); }

    // ---- O(log nnz) lookup, binary search on row-major order -------------
    int get(int r, int c) const {
        auto it = lowerBound(r, c);
        if (it != data_.end() && it->row == r && it->col == c) return it->value;
        return 0;
    }

    bool occupied(int r, int c) const { return get(r, c) != 0; }

    // ---- O(nnz) insert / update ----------------------------------------
    void set(int r, int c, int value) {
        if (r < 0 || c < 0 || r >= rows_ || c >= cols_) return;
        auto it = lowerBound(r, c);
        if (it != data_.end() && it->row == r && it->col == c) {
            if (value == 0) data_.erase(it);
            else it->value = value;
            return;
        }
        if (value == 0) return;
        data_.insert(it, Triplet{r, c, value});
    }

    bool clearCell(int r, int c) {
        auto it = lowerBound(r, c);
        if (it != data_.end() && it->row == r && it->col == c) { data_.erase(it); return true; }
        return false;
    }

    void clearAll() { data_.clear(); }

    // ---- first-fit bed allocation, O(nnz + rows * cols) ------------------
    // strategy: "first"  -> lowest row-major free cell
    //           "column" -> fill column-major (nurses prefer a compact block)
    //           "spread" -> free cell with the fewest occupied neighbours
    AllocationResult allocate(const std::string& strategy = "first") {
        AllocationResult res;
        res.strategy = strategy;

        if (strategy == "column") {
            for (int c = 0; c < cols_; ++c)
                for (int r = 0; r < rows_; ++r)
                    if (!occupied(r, c)) { res.found = true; res.row = r; res.col = c; return res; }
            return res;
        }

        if (strategy == "spread") {
            int bestScore = 99, bestR = -1, bestC = -1;
            for (int r = 0; r < rows_; ++r) {
                for (int c = 0; c < cols_; ++c) {
                    if (occupied(r, c)) continue;
                    int score = 0;
                    const int dr[4] = {-1, 1, 0, 0};
                    const int dc[4] = {0, 0, -1, 1};
                    for (int k = 0; k < 4; ++k) {
                        int nr = r + dr[k], nc = c + dc[k];
                        if (nr >= 0 && nc >= 0 && nr < rows_ && nc < cols_ && occupied(nr, nc)) ++score;
                    }
                    if (score < bestScore) { bestScore = score; bestR = r; bestC = c; }
                }
            }
            if (bestR >= 0) { res.found = true; res.row = bestR; res.col = bestC; }
            return res;
        }

        for (int r = 0; r < rows_; ++r)
            for (int c = 0; c < cols_; ++c)
                if (!occupied(r, c)) { res.found = true; res.row = r; res.col = c; return res; }
        return res;
    }

    int occupiedInRow(int r) const {
        int count = 0;
        for (const auto& t : data_) if (t.row == r) ++count;
        return count;
    }

    int occupiedInCol(int c) const {
        int count = 0;
        for (const auto& t : data_) if (t.col == c) ++count;
        return count;
    }

    std::vector<int> rowOccupancy() const {
        std::vector<int> out(static_cast<std::size_t>(rows_), 0);
        for (const auto& t : data_) out[static_cast<std::size_t>(t.row)]++;
        return out;
    }

private:
    int rows_ = 0;
    int cols_ = 0;
    std::vector<Triplet> data_;   // row-major sorted

    std::vector<Triplet>::const_iterator lowerBound(int r, int c) const {
        return std::lower_bound(data_.begin(), data_.end(), std::make_pair(r, c),
            [](const Triplet& t, const std::pair<int, int>& key) {
                return t.row < key.first || (t.row == key.first && t.col < key.second);
            });
    }

    std::vector<Triplet>::iterator lowerBound(int r, int c) {
        return std::lower_bound(data_.begin(), data_.end(), std::make_pair(r, c),
            [](const Triplet& t, const std::pair<int, int>& key) {
                return t.row < key.first || (t.row == key.first && t.col < key.second);
            });
    }
};
