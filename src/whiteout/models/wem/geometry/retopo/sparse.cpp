// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "sparse.h"

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

void SparseMatrix::multiply(std::span<const f64> x, std::span<f64> out) const {
    for (u32 r = 0; r < rows; ++r) {
        f64 sum = 0.0;
        for (u32 k = offsets[r]; k < offsets[r + 1]; ++k) {
            sum += values[k] * x[columns[k]];
        }
        out[r] = sum;
    }
}

f64 SparseMatrix::diagonal(u32 row) const {
    for (u32 k = offsets[row]; k < offsets[row + 1]; ++k) {
        if (columns[k] == row) {
            return values[k];
        }
    }
    return 0.0;
}

SparseMatrix SparseBuilder::build() const {
    std::vector<Entry> sorted = entries_;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Entry& a, const Entry& b) {
        return a.row != b.row ? a.row < b.row : a.column < b.column;
    });
    SparseMatrix matrix;
    matrix.rows = rows_;
    matrix.offsets.assign(rows_ + 1, 0);
    for (std::size_t i = 0; i < sorted.size();) {
        std::size_t j = i;
        f64 value = 0.0;
        while (j < sorted.size() && sorted[j].row == sorted[i].row &&
               sorted[j].column == sorted[i].column) {
            value += sorted[j].value;
            ++j;
        }
        matrix.columns.push_back(sorted[i].column);
        matrix.values.push_back(value);
        matrix.offsets[sorted[i].row + 1] += 1;
        i = j;
    }
    for (u32 r = 0; r < rows_; ++r) {
        matrix.offsets[r + 1] += matrix.offsets[r];
    }
    return matrix;
}

CgResult SolveConjugateGradient(const SparseMatrix& a, std::span<const f64> b, std::span<f64> x,
                                f64 tolerance, u32 maxIterations) {
    CgResult result;
    const u32 n = a.rows;
    std::vector<f64> r(n), z(n), p(n), q(n), inverse(n);
    for (u32 i = 0; i < n; ++i) {
        const f64 d = a.diagonal(i);
        inverse[i] = d > 0.0 ? 1.0 / d : 1.0;
    }
    a.multiply(x, q);
    f64 bNorm = 0.0;
    for (u32 i = 0; i < n; ++i) {
        r[i] = b[i] - q[i];
        bNorm += b[i] * b[i];
    }
    bNorm = std::sqrt(bNorm);
    if (bNorm == 0.0) {
        bNorm = 1.0;
    }
    f64 rz = 0.0;
    for (u32 i = 0; i < n; ++i) {
        z[i] = r[i] * inverse[i];
        p[i] = z[i];
        rz += r[i] * z[i];
    }
    for (u32 iteration = 0; iteration < maxIterations; ++iteration) {
        f64 rNorm = 0.0;
        for (u32 i = 0; i < n; ++i) {
            rNorm += r[i] * r[i];
        }
        result.residual = std::sqrt(rNorm) / bNorm;
        if (result.residual <= tolerance) {
            result.converged = true;
            return result;
        }
        a.multiply(p, q);
        f64 pq = 0.0;
        for (u32 i = 0; i < n; ++i) {
            pq += p[i] * q[i];
        }
        if (pq <= 0.0) {
            return result;
        }
        const f64 alpha = rz / pq;
        f64 next = 0.0;
        for (u32 i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * q[i];
            z[i] = r[i] * inverse[i];
            next += r[i] * z[i];
        }
        const f64 beta = next / rz;
        rz = next;
        for (u32 i = 0; i < n; ++i) {
            p[i] = z[i] + beta * p[i];
        }
        result.iterations = iteration + 1;
    }
    f64 rNorm = 0.0;
    for (u32 i = 0; i < n; ++i) {
        rNorm += r[i] * r[i];
    }
    result.residual = std::sqrt(rNorm) / bNorm;
    result.converged = result.residual <= tolerance;
    return result;
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
