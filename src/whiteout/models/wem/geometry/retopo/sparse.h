// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file sparse.h
 * @brief The retopology's linear algebra: a symmetric sparse matrix and
 *        Jacobi-preconditioned conjugate gradients.
 *
 * The cross field and the patch maps are both symmetric positive definite
 * solves, so CG is enough and no factorisation is needed (the library takes no
 * dependency for one; see uv/flatten.h).
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

/// Compressed rows; duplicates summed by the builder.
struct SparseMatrix {
    u32 rows = 0;
    std::vector<u32> offsets;
    std::vector<u32> columns;
    std::vector<f64> values;

    void multiply(std::span<const f64> x, std::span<f64> out) const;
    f64 diagonal(u32 row) const;
};

class SparseBuilder {
public:
    explicit SparseBuilder(u32 rows) : rows_(rows) {}
    void add(u32 row, u32 column, f64 value) {
        entries_.push_back({row, column, value});
    }
    SparseMatrix build() const;

private:
    struct Entry {
        u32 row;
        u32 column;
        f64 value;
    };
    u32 rows_;
    std::vector<Entry> entries_;
};

struct CgResult {
    u32 iterations = 0;
    f64 residual = 0.0; ///< Relative to the right-hand side.
    bool converged = false;
};

/// Solves `A x = b` from the @p x given, stopping at @p tolerance relative to
/// `|b|` or after @p maxIterations.
CgResult SolveConjugateGradient(const SparseMatrix& a, std::span<const f64> b, std::span<f64> x,
                                f64 tolerance, u32 maxIterations);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
