// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/fit.h>

#include <whiteout/models/wem/skinning/quantize.h>

#include "../geometry/fracture/parallel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

using geom::fracture::Parallel;
using V3 = std::array<f64, 3>;

/// `q = m·p + t`, column convention, in f64.
struct Affine {
    f64 m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    f64 t[3] = {0, 0, 0};

    V3 operator()(const V3& p) const {
        return {m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + t[0],
                m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + t[1],
                m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + t[2]};
    }
};

V3 Of(const Vector3f& v) {
    return {v.x, v.y, v.z};
}

f64 DistanceSquared(const V3& a, const V3& b) {
    return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
}

/// The eigenvector of @p n's largest eigenvalue (cyclic Jacobi; @p n is
/// symmetric).
std::array<f64, 4> LargestEigenvector(std::array<std::array<f64, 4>, 4> n) {
    std::array<std::array<f64, 4>, 4> v{};
    for (int i = 0; i < 4; ++i) {
        v[i][i] = 1.0;
    }
    for (int sweep = 0; sweep < 32; ++sweep) {
        f64 off = 0.0;
        for (int p = 0; p < 4; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                off += n[p][q] * n[p][q];
            }
        }
        if (off < 1e-24) {
            break;
        }
        for (int p = 0; p < 4; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                if (std::fabs(n[p][q]) < 1e-30) {
                    continue;
                }
                const f64 theta = (n[q][q] - n[p][p]) / (2.0 * n[p][q]);
                const f64 t = (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const f64 c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 4; ++k) {
                    const f64 a = n[k][p], b = n[k][q];
                    n[k][p] = c * a - s * b;
                    n[k][q] = s * a + c * b;
                }
                for (int k = 0; k < 4; ++k) {
                    const f64 a = n[p][k], b = n[q][k];
                    n[p][k] = c * a - s * b;
                    n[q][k] = s * a + c * b;
                }
                for (int k = 0; k < 4; ++k) {
                    const f64 a = v[k][p], b = v[k][q];
                    v[k][p] = c * a - s * b;
                    v[k][q] = s * a + c * b;
                }
            }
        }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i) {
        if (n[i][i] > n[best][best]) {
            best = i;
        }
    }
    return {v[0][best], v[1][best], v[2][best], v[3][best]};
}

/// Horn's unit quaternion `(w, x, y, z)` for a cross-covariance; zero length
/// when there is none.
std::array<f64, 4> HornQuaternion(const f64 (&s)[3][3]) {
    const std::array<std::array<f64, 4>, 4> n{{
        {s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0]},
        {s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2]},
        {s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1]},
        {s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2]},
    }};
    return LargestEigenvector(n);
}

/// Weighted point pairs, summed: what a rigid fit needs of them.
struct Moments {
    f64 weight = 0.0;
    V3 from{}, to{};
    f64 cross[3][3] = {};
    u32 count = 0;

    /// @p p, weighing @p w squared, toward `q / w`: a bone that carries a
    /// point by @p w fitted to what it must add, @p q.
    void add(const V3& p, const V3& q, f64 w) {
        weight += w * w;
        for (int r = 0; r < 3; ++r) {
            from[r] += w * w * p[r];
            to[r] += w * q[r];
            for (int c = 0; c < 3; ++c) {
                cross[r][c] += w * p[r] * q[c];
            }
        }
        ++count;
    }

    /// The best rigid motion; false with fewer than three points.
    bool solve(Affine& out) const {
        if (count < 3 || !(weight > 0.0)) {
            return false;
        }
        V3 a, b;
        f64 s[3][3];
        for (int r = 0; r < 3; ++r) {
            a[r] = from[r] / weight;
            b[r] = to[r] / weight;
        }
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                s[r][c] = cross[r][c] - weight * a[r] * b[c];
            }
        }
        const std::array<f64, 4> q = HornQuaternion(s);
        const f64 length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!(length > 0.0)) {
            return false;
        }
        const f64 w = q[0] / length, x = q[1] / length, y = q[2] / length, z = q[3] / length;
        out.m[0][0] = 1 - 2 * (y * y + z * z), out.m[0][1] = 2 * (x * y - w * z), out.m[0][2] = 2 * (x * z + w * y);
        out.m[1][0] = 2 * (x * y + w * z), out.m[1][1] = 1 - 2 * (x * x + z * z), out.m[1][2] = 2 * (y * z - w * x);
        out.m[2][0] = 2 * (x * z - w * y), out.m[2][1] = 2 * (y * z + w * x), out.m[2][2] = 1 - 2 * (x * x + y * y);
        for (int r = 0; r < 3; ++r) {
            out.t[r] = b[r] - (out.m[r][0] * a[0] + out.m[r][1] * a[1] + out.m[r][2] * a[2]);
        }
        return true;
    }
};

/// Solves @p a·x = @p b in place (@p n square, row-major, partial pivoting).
bool SolveDense(std::vector<f64>& a, std::vector<f64>& b, u32 n) {
    for (u32 col = 0; col < n; ++col) {
        u32 pivot = col;
        for (u32 r = col + 1; r < n; ++r) {
            if (std::fabs(a[r * n + col]) > std::fabs(a[pivot * n + col])) {
                pivot = r;
            }
        }
        if (!(std::fabs(a[pivot * n + col]) > 1e-14)) {
            return false;
        }
        if (pivot != col) {
            for (u32 c = 0; c < n; ++c) {
                std::swap(a[col * n + c], a[pivot * n + c]);
            }
            std::swap(b[col], b[pivot]);
        }
        for (u32 r = col + 1; r < n; ++r) {
            const f64 f = a[r * n + col] / a[col * n + col];
            for (u32 c = col; c < n; ++c) {
                a[r * n + c] -= f * a[col * n + c];
            }
            b[r] -= f * b[col];
        }
    }
    for (u32 r = n; r-- > 0;) {
        f64 sum = b[r];
        for (u32 c = r + 1; c < n; ++c) {
            sum -= a[r * n + c] * b[c];
        }
        b[r] = sum / a[r * n + r];
    }
    return true;
}

/// `min ½wᵀGw − hᵀw` with the weights on the simplex (`Σw = 1`, `w ≥ 0`), by
/// active sets: the optimum on a support with the sum held, its most negative
/// weight dropped, and a weight whose multiplier says it would help let back.
std::vector<f64> SimplexLeastSquares(const std::vector<f64>& g, const std::vector<f64>& h, u32 n) {
    std::vector<u8> in(n, 1);
    std::vector<f64> w(n, 0.0);
    std::vector<f64> a, x;
    std::vector<u32> support;
    for (u32 round = 0; round < 4 * n + 4; ++round) {
        support.clear();
        for (u32 j = 0; j < n; ++j) {
            if (in[j] != 0) {
                support.push_back(j);
            }
        }
        const u32 m = static_cast<u32>(support.size());
        if (m == 0) {
            break;
        }
        a.assign((m + 1) * (m + 1), 0.0);
        x.assign(m + 1, 0.0);
        for (u32 r = 0; r < m; ++r) {
            for (u32 c = 0; c < m; ++c) {
                a[r * (m + 1) + c] = g[support[r] * n + support[c]];
            }
            a[r * (m + 1) + m] = 1.0;
            a[m * (m + 1) + r] = 1.0;
            x[r] = h[support[r]];
        }
        x[m] = 1.0;
        if (!SolveDense(a, x, m + 1)) {
            if (m == 1) {
                std::fill(w.begin(), w.end(), 0.0);
                w[support[0]] = 1.0;
                break;
            }
            in[support.back()] = 0;
            continue;
        }
        u32 lowest = m;
        for (u32 r = 0; r < m; ++r) {
            if (x[r] < -1e-12 && (lowest == m || x[r] < x[lowest])) {
                lowest = r;
            }
        }
        if (lowest < m) {
            in[support[lowest]] = 0;
            continue;
        }
        std::fill(w.begin(), w.end(), 0.0);
        for (u32 r = 0; r < m; ++r) {
            w[support[r]] = std::max(0.0, x[r]);
        }
        // An excluded weight helps where its multiplier is negative.
        u32 add = n;
        f64 most = -1e-9;
        for (u32 j = 0; j < n; ++j) {
            if (in[j] != 0) {
                continue;
            }
            f64 lambda = x[m] - h[j];
            for (u32 k = 0; k < n; ++k) {
                lambda += g[j * n + k] * w[k];
            }
            if (lambda < most) {
                most = lambda;
                add = j;
            }
        }
        if (add == n) {
            break;
        }
        in[add] = 1;
    }
    f64 total = 0.0;
    for (const f64 value : w) {
        total += value;
    }
    for (f64& value : w) {
        value = total > 0.0 ? value / total : 0.0;
    }
    return w;
}

/// A point's weight on a bone: new (`fixed` false) or given.
struct Weight {
    u32 bone = 0;
    bool fixed = false;
    f64 w = 0.0;
};

class Fitter {
public:
    Fitter(const FitMotion& motion, const FitFixedBones& fixed, const FitOptions& options)
        : options_(options), points_(static_cast<u32>(motion.rest.size())), frames_(motion.frameCount),
          fixedCount_(fixed.count), allowed_(fixed.allowed) {
        // Centred, the rest on its centroid and each frame on its own, so the
        // sums a solve takes stay near the figure's size.
        rest_.resize(points_);
        for (u32 i = 0; i < points_; ++i) {
            rest_[i] = Of(motion.rest[i]);
            for (int r = 0; r < 3; ++r) {
                restCentre_[r] += rest_[i][r] / points_;
            }
        }
        for (V3& p : rest_) {
            for (int r = 0; r < 3; ++r) {
                p[r] -= restCentre_[r];
            }
        }
        target_.resize(static_cast<std::size_t>(frames_) * points_);
        frameCentre_.assign(frames_, V3{});
        for (u32 f = 0; f < frames_; ++f) {
            for (u32 i = 0; i < points_; ++i) {
                const V3 q = Of(motion.frames[static_cast<std::size_t>(f) * points_ + i]);
                target_[index(f, i)] = q;
                for (int r = 0; r < 3; ++r) {
                    frameCentre_[f][r] += q[r] / points_;
                }
            }
            for (u32 i = 0; i < points_; ++i) {
                for (int r = 0; r < 3; ++r) {
                    target_[index(f, i)][r] -= frameCentre_[f][r];
                }
            }
        }
        fixed_.resize(static_cast<std::size_t>(frames_) * fixedCount_);
        for (u32 f = 0; f < frames_; ++f) {
            for (u32 b = 0; b < fixedCount_; ++b) {
                const Matrix44f& m = fixed.skinning[static_cast<std::size_t>(f) * fixedCount_ + b];
                Affine& a = fixed_[static_cast<std::size_t>(f) * fixedCount_ + b];
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        a.m[r][c] = m.data[c][r];
                    }
                    a.t[r] = m.data[3][r];
                }
                // Into the centred frames: x' = A(p' + c₀) + t − m_f.
                for (int r = 0; r < 3; ++r) {
                    a.t[r] += a.m[r][0] * restCentre_[0] + a.m[r][1] * restCentre_[1] + a.m[r][2] * restCentre_[2] -
                              frameCentre_[f][r];
                }
            }
        }
        recon_.assign(target_.size(), V3{});
        error_.assign(points_, 0.0);
    }

    FitResult run() {
        FitResult out;
        if (points_ == 0 || frames_ == 0) {
            return out;
        }
        const u32 most = std::max(1u, std::min(options_.maxBones, points_));
        // One bone for everything, then twice as many at each stage until the
        // budget or the target.
        motion_.assign(1, std::vector<Affine>(frames_));
        owner_.assign(points_, 0);
        refit(0);
        // A few rounds a stage to judge it; the full count on the one kept.
        const u32 polish = std::max(1u, options_.iterations);
        u32 stage = std::min(most, 8u);
        bool stopped = false;
        for (;;) {
            grow(stage);
            oneHot();
            const bool last = stage == most;
            stopped = solve(last ? polish : 4u);
            if (stopped || last) {
                break;
            }
            if (options_.rmsTarget > 0.0f && rms_ <= options_.rmsTarget) {
                stopped = solve(polish);
                break;
            }
            takeOwners();
            stage = std::min(most, stage * 2);
        }
        if (options_.weights != FitWeights::Exact) {
            roundWeights();
            reconstruct();
            // A few rounds of the bones alone take up what the rounding moved.
            for (u32 round = 0; round < 3 && !stopped; ++round) {
                solveBones();
            }
            measure();
        }
        return finish(stopped);
    }

private:
    std::size_t index(u32 f, u32 i) const {
        return static_cast<std::size_t>(f) * points_ + i;
    }

    u32 bones() const {
        return static_cast<u32>(motion_.size());
    }

    /// Each new bone's frames, fitted to the points it owns.
    void refit(u32 bone) {
        std::vector<u32> members;
        for (u32 i = 0; i < points_; ++i) {
            if (owner_[i] == bone) {
                members.push_back(i);
            }
        }
        fitTo(bone, members);
    }

    void fitTo(u32 bone, const std::vector<u32>& members) {
        for (u32 f = 0; f < frames_; ++f) {
            Moments m;
            for (const u32 i : members) {
                m.add(rest_[i], target_[index(f, i)], 1.0);
            }
            Affine a;
            if (m.solve(a)) {
                motion_[bone][f] = a;
            } else if (!members.empty()) {
                // Too few to turn: carried by their centre alone.
                Affine moved;
                for (int r = 0; r < 3; ++r) {
                    moved.t[r] = m.to[r] / m.weight - m.from[r] / m.weight;
                }
                motion_[bone][f] = moved;
            }
        }
    }

    void updatePivots() {
        pivots_.assign(bones(), V3{});
        std::vector<f64> mass(bones(), 0.0);
        if (weights_.empty()) {
            for (u32 i = 0; i < points_; ++i) {
                for (int r = 0; r < 3; ++r) {
                    pivots_[owner_[i]][r] += rest_[i][r];
                }
                mass[owner_[i]] += 1.0;
            }
        } else {
            for (u32 i = 0; i < points_; ++i) {
                for (const Weight& w : weights_[i]) {
                    if (!w.fixed) {
                        for (int r = 0; r < 3; ++r) {
                            pivots_[w.bone][r] += w.w * rest_[i][r];
                        }
                        mass[w.bone] += w.w;
                    }
                }
            }
        }
        for (u32 k = 0; k < bones(); ++k) {
            for (int r = 0; r < 3; ++r) {
                pivots_[k][r] = mass[k] > 0.0 ? pivots_[k][r] / mass[k] : (k < seeds_.size() ? seeds_[k][r] : 0.0);
            }
        }
    }

    /// Up to @p count new bones nearest @p i by their pivots, nearest first.
    std::vector<u32> nearest(u32 i, u32 count) const {
        std::vector<std::pair<f64, u32>> by;
        by.reserve(bones());
        for (u32 k = 0; k < bones(); ++k) {
            by.emplace_back(DistanceSquared(rest_[i], pivots_[k]), k);
        }
        const std::size_t kept = std::min<std::size_t>(count, by.size());
        std::partial_sort(by.begin(), by.begin() + static_cast<std::ptrdiff_t>(kept), by.end());
        std::vector<u32> out;
        for (std::size_t k = 0; k < kept; ++k) {
            out.push_back(by[k].second);
        }
        return out;
    }

    /// How far bone @p k alone puts point @p i, squared, over every frame.
    f64 aloneError(u32 i, u32 k) const {
        f64 sum = 0.0;
        for (u32 f = 0; f < frames_; ++f) {
            sum += DistanceSquared(motion_[k][f](rest_[i]), target_[index(f, i)]);
        }
        return sum;
    }

    /// Splits the worst-fitted groups until @p wanted bones, each a few rounds
    /// of every point going to the bone that carries it best.
    void grow(u32 wanted) {
        weights_.clear();
        while (bones() < wanted) {
            updatePivots();
            Parallel(points_, options_.threads, [&](std::size_t i) {
                error_[i] = aloneError(static_cast<u32>(i), owner_[i]);
            });
            std::vector<f64> groupError(bones(), 0.0);
            std::vector<std::vector<u32>> members(bones());
            for (u32 i = 0; i < points_; ++i) {
                groupError[owner_[i]] += error_[i];
                members[owner_[i]].push_back(i);
            }
            std::vector<u32> order(bones());
            for (u32 k = 0; k < bones(); ++k) {
                order[k] = k;
            }
            std::stable_sort(order.begin(), order.end(),
                             [&](u32 a, u32 b) { return groupError[a] > groupError[b]; });
            const u32 before = bones();
            for (const u32 group : order) {
                if (bones() >= wanted) {
                    break;
                }
                const std::vector<u32>& list = members[group];
                if (list.size() < 6) {
                    continue;
                }
                // The worst point and the nearest third of its group start the
                // new bone.
                u32 seed = list.front();
                for (const u32 i : list) {
                    if (error_[i] > error_[seed]) {
                        seed = i;
                    }
                }
                std::vector<std::pair<f64, u32>> by;
                for (const u32 i : list) {
                    by.emplace_back(DistanceSquared(rest_[i], rest_[seed]), i);
                }
                std::sort(by.begin(), by.end());
                const std::size_t taken = std::max<std::size_t>(3, list.size() / 3);
                std::vector<u32> chosen;
                for (std::size_t k = 0; k < taken && k < by.size(); ++k) {
                    chosen.push_back(by[k].second);
                }
                const u32 made = bones();
                motion_.emplace_back(frames_);
                fitTo(made, chosen);
                for (const u32 i : chosen) {
                    owner_[i] = made;
                }
            }
            if (bones() == before) {
                break;
            }
            for (int round = 0; round < 3; ++round) {
                reassign();
            }
        }
        if (seeds_.size() < bones()) {
            updatePivots();
            seeds_ = pivots_;
        }
    }

    /// Every point to the bone among the nearest that carries it best, then
    /// every bone refitted; a bone left with nothing goes.
    void reassign() {
        updatePivots();
        std::vector<u32> next(points_);
        Parallel(points_, options_.threads, [&](std::size_t i) {
            std::vector<u32> near = nearest(static_cast<u32>(i), 2 * std::max(1u, options_.candidates));
            if (std::find(near.begin(), near.end(), owner_[i]) == near.end()) {
                near.push_back(owner_[i]);
            }
            std::sort(near.begin(), near.end());
            u32 best = owner_[i];
            f64 lowest = aloneError(static_cast<u32>(i), best);
            for (const u32 k : near) {
                const f64 e = aloneError(static_cast<u32>(i), k);
                if (e < lowest) {
                    lowest = e;
                    best = k;
                }
            }
            next[i] = best;
        });
        owner_ = next;
        // Bones nobody chose go, the rest keep their order.
        std::vector<u32> count(bones(), 0), renumber(bones(), 0);
        for (const u32 k : owner_) {
            ++count[k];
        }
        u32 kept = 0;
        for (u32 k = 0; k < bones(); ++k) {
            renumber[k] = kept;
            if (count[k] != 0) {
                motion_[kept] = std::move(motion_[k]);
                ++kept;
            }
        }
        motion_.resize(kept);
        for (u32& k : owner_) {
            k = renumber[k];
        }
        Parallel(bones(), options_.threads, [&](std::size_t k) { refit(static_cast<u32>(k)); });
    }

    void oneHot() {
        weights_.assign(points_, {});
        for (u32 i = 0; i < points_; ++i) {
            weights_[i].push_back(Weight{owner_[i], false, 1.0});
        }
    }

    /// Each point's heaviest new bone, or the nearest where it leans on fixed
    /// ones alone: the groups the next stage splits.
    void takeOwners() {
        updatePivots();
        for (u32 i = 0; i < points_; ++i) {
            f64 best = 0.0;
            bool found = false;
            for (const Weight& w : weights_[i]) {
                if (!w.fixed && w.w > best) {
                    best = w.w;
                    owner_[i] = w.bone;
                    found = true;
                }
            }
            if (!found) {
                owner_[i] = nearest(i, 1).front();
            }
        }
    }

    V3 apply(const Weight& w, u32 f, const V3& p) const {
        return w.fixed ? fixed_[static_cast<std::size_t>(f) * fixedCount_ + w.bone](p) : motion_[w.bone][f](p);
    }

    void reconstruct() {
        Parallel(frames_, options_.threads, [&](std::size_t f) {
            for (u32 i = 0; i < points_; ++i) {
                V3 sum{};
                for (const Weight& w : weights_[i]) {
                    const V3 q = apply(w, static_cast<u32>(f), rest_[i]);
                    for (int r = 0; r < 3; ++r) {
                        sum[r] += w.w * q[r];
                    }
                }
                recon_[index(static_cast<u32>(f), i)] = sum;
            }
        });
    }

    /// The RMS and the worst distance, and each point's summed square.
    void measure() {
        std::vector<f64> frameSum(frames_, 0.0), frameWorst(frames_, 0.0);
        Parallel(frames_, options_.threads, [&](std::size_t f) {
            for (u32 i = 0; i < points_; ++i) {
                const f64 d = DistanceSquared(recon_[index(static_cast<u32>(f), i)], target_[index(static_cast<u32>(f), i)]);
                frameSum[f] += d;
                frameWorst[f] = std::max(frameWorst[f], d);
            }
        });
        Parallel(points_, options_.threads, [&](std::size_t i) {
            f64 sum = 0.0;
            for (u32 f = 0; f < frames_; ++f) {
                sum += DistanceSquared(recon_[index(f, static_cast<u32>(i))], target_[index(f, static_cast<u32>(i))]);
            }
            error_[i] = sum;
        });
        f64 total = 0.0, worst = 0.0;
        for (u32 f = 0; f < frames_; ++f) {
            total += frameSum[f];
            worst = std::max(worst, frameWorst[f]);
        }
        rms_ = static_cast<f32>(std::sqrt(total / (static_cast<f64>(frames_) * points_)));
        worst_ = static_cast<f32>(std::sqrt(worst));
    }

    /// One point's weights over the nearest bones and the fixed ones it may
    /// use: least squares on the simplex, then the heaviest few solved again.
    std::vector<Weight> solveWeights(u32 i) const {
        std::vector<Weight> candidates;
        for (const u32 k : nearest(i, std::max(1u, options_.candidates))) {
            candidates.push_back(Weight{k, false, 0.0});
        }
        for (const Weight& w : weights_[i]) {
            if (!w.fixed && std::none_of(candidates.begin(), candidates.end(), [&](const Weight& c) {
                    return !c.fixed && c.bone == w.bone;
                })) {
                candidates.push_back(w);
            }
        }
        if (i < allowed_.size()) {
            for (const u32 b : allowed_[i]) {
                if (b < fixedCount_) {
                    candidates.push_back(Weight{b, true, 0.0});
                }
            }
        }
        const auto solveOver = [&](std::vector<Weight>& set) {
            const u32 n = static_cast<u32>(set.size());
            std::vector<f64> g(static_cast<std::size_t>(n) * n, 0.0), h(n, 0.0);
            std::vector<V3> y(n);
            for (u32 f = 0; f < frames_; ++f) {
                const V3& x = target_[index(f, i)];
                for (u32 c = 0; c < n; ++c) {
                    y[c] = apply(set[c], f, rest_[i]);
                    h[c] += y[c][0] * x[0] + y[c][1] * x[1] + y[c][2] * x[2];
                }
                for (u32 a = 0; a < n; ++a) {
                    for (u32 b = a; b < n; ++b) {
                        g[a * n + b] += y[a][0] * y[b][0] + y[a][1] * y[b][1] + y[a][2] * y[b][2];
                    }
                }
            }
            f64 scale = 0.0;
            for (u32 a = 0; a < n; ++a) {
                scale += g[a * n + a];
                for (u32 b = 0; b < a; ++b) {
                    g[a * n + b] = g[b * n + a];
                }
            }
            scale = scale > 0.0 ? scale / n : 1.0;
            for (f64& value : g) {
                value /= scale;
            }
            for (f64& value : h) {
                value /= scale;
            }
            for (u32 a = 0; a < n; ++a) {
                g[a * n + a] += 1e-9;
            }
            const std::vector<f64> w = SimplexLeastSquares(g, h, n);
            for (u32 c = 0; c < n; ++c) {
                set[c].w = w[c];
            }
        };
        solveOver(candidates);
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Weight& a, const Weight& b) { return a.w > b.w; });
        while (!candidates.empty() && !(candidates.back().w > 1e-6)) {
            candidates.pop_back();
        }
        const std::size_t most = std::max<std::size_t>(1, options_.maxInfluences);
        if (candidates.size() > most) {
            candidates.resize(most);
            solveOver(candidates);
            std::stable_sort(candidates.begin(), candidates.end(),
                             [](const Weight& a, const Weight& b) { return a.w > b.w; });
            while (candidates.size() > 1 && !(candidates.back().w > 1e-6)) {
                candidates.pop_back();
            }
        }
        if (candidates.empty()) {
            candidates.push_back(weights_[i].front());
            candidates.front().w = 1.0;
        }
        return candidates;
    }

    /// Bone by bone, each frame fitted to what the others leave its points.
    void solveBones() {
        std::vector<std::vector<std::pair<u32, f64>>> members(bones());
        for (u32 i = 0; i < points_; ++i) {
            for (const Weight& w : weights_[i]) {
                if (!w.fixed && w.w > 0.0) {
                    members[w.bone].emplace_back(i, w.w);
                }
            }
        }
        for (u32 k = 0; k < bones(); ++k) {
            if (members[k].size() < 3) {
                continue;
            }
            Parallel(frames_, options_.threads, [&](std::size_t frame) {
                const u32 f = static_cast<u32>(frame);
                Moments m;
                for (const auto& [i, w] : members[k]) {
                    const V3 old = motion_[k][f](rest_[i]);
                    const V3& x = target_[index(f, i)];
                    const V3& now = recon_[index(f, i)];
                    m.add(rest_[i], V3{x[0] - now[0] + w * old[0], x[1] - now[1] + w * old[1], x[2] - now[2] + w * old[2]},
                          w);
                }
                Affine fitted;
                if (!m.solve(fitted)) {
                    return;
                }
                for (const auto& [i, w] : members[k]) {
                    const V3 old = motion_[k][f](rest_[i]);
                    const V3 moved = fitted(rest_[i]);
                    V3& now = recon_[index(f, i)];
                    for (int r = 0; r < 3; ++r) {
                        now[r] += w * (moved[r] - old[r]);
                    }
                }
                motion_[k][f] = fitted;
            });
        }
    }

    /// A bone nearly nobody leans on starts again at the worst-fitted point.
    void reseed() {
        std::vector<f64> mass(bones(), 0.0);
        for (const std::vector<Weight>& list : weights_) {
            for (const Weight& w : list) {
                if (!w.fixed) {
                    mass[w.bone] += w.w;
                }
            }
        }
        std::vector<u8> used(points_, 0);
        for (u32 k = 0; k < bones(); ++k) {
            if (mass[k] >= 0.5) {
                continue;
            }
            u32 seed = points_;
            for (u32 i = 0; i < points_; ++i) {
                if (used[i] == 0 && (seed == points_ || error_[i] > error_[seed])) {
                    seed = i;
                }
            }
            if (seed == points_) {
                return;
            }
            std::vector<std::pair<f64, u32>> by;
            for (u32 i = 0; i < points_; ++i) {
                by.emplace_back(DistanceSquared(rest_[i], rest_[seed]), i);
            }
            std::sort(by.begin(), by.end());
            std::vector<u32> chosen;
            for (std::size_t n = 0; n < by.size() && n < 12; ++n) {
                chosen.push_back(by[n].second);
                used[by[n].second] = 1;
            }
            fitTo(k, chosen);
            if (k < seeds_.size()) {
                seeds_[k] = rest_[seed];
            }
        }
    }

    /// Each point's weights as `FitOptions::weights` holds them, by the rounding
    /// the file's writer makes, so that it finds nothing left to round.
    void roundWeights() {
        Parallel(points_, options_.threads, [&](std::size_t i) {
            std::vector<Weight>& list = weights_[i];
            if (options_.weights == FitWeights::Bytes) {
                const std::size_t count = std::min<std::size_t>(list.size(), 4);
                std::array<f32, 4> shares{};
                for (std::size_t k = 0; k < count; ++k) {
                    shares[k] = static_cast<f32>(list[k].w);
                }
                const std::array<u8, 4> bytes = QuantizeWeights(shares, count);
                list.resize(count);
                for (std::size_t k = 0; k < count; ++k) {
                    list[k].w = bytes[k] / 255.0;
                }
            } else {
                // Named by place in the list: the group is a set of them.
                std::vector<std::vector<geom::Influence>> one(1);
                for (u32 k = 0; k < list.size(); ++k) {
                    one[0].push_back(geom::Influence{k, static_cast<f32>(list[k].w)});
                }
                ClassicLimits limits;
                limits.maxBones = std::max(1u, options_.maxInfluences);
                const ClassicSkin snapped = QuantizeClassic(one, {}, limits);
                if (snapped.groupOf.empty()) {
                    return;
                }
                const std::vector<u32>& group = snapped.groups[snapped.groupOf.front()];
                for (u32 k = 0; k < list.size(); ++k) {
                    list[k].w = std::binary_search(group.begin(), group.end(), k) ? 1.0 / group.size() : 0.0;
                }
            }
            std::erase_if(list, [](const Weight& w) { return !(w.w > 0.0); });
            std::stable_sort(list.begin(), list.end(), [](const Weight& a, const Weight& b) { return a.w > b.w; });
        });
    }

    /// Up to @p rounds rounds of weights then bones; true when cancelled.
    bool solve(u32 rounds) {
        f32 before = 0.0f;
        for (u32 round = 0; round < rounds; ++round) {
            if (options_.cancelled && options_.cancelled()) {
                reconstruct();
                measure();
                return true;
            }
            updatePivots();
            std::vector<std::vector<Weight>> next(points_);
            Parallel(points_, options_.threads, [&](std::size_t i) { next[i] = solveWeights(static_cast<u32>(i)); });
            weights_ = std::move(next);
            reconstruct();
            solveBones();
            measure();
            if (round > 0 && before - rms_ < 0.01f * before) {
                break;
            }
            before = rms_;
            if (round + 1 < rounds) {
                reseed();
            }
        }
        return false;
    }

    FitResult finish(bool cancelled) {
        FitResult out;
        out.cancelled = cancelled;
        // Bones nobody leans on go.
        std::vector<f64> mass(bones(), 0.0);
        for (const std::vector<Weight>& list : weights_) {
            for (const Weight& w : list) {
                if (!w.fixed) {
                    mass[w.bone] += w.w;
                }
            }
        }
        std::vector<u32> renumber(bones(), 0);
        u32 kept = 0;
        for (u32 k = 0; k < bones(); ++k) {
            renumber[k] = kept;
            kept += mass[k] > 0.0 ? 1u : 0u;
        }
        out.bones = kept;
        updatePivots();
        out.pivots.resize(kept);
        out.skinning.resize(static_cast<std::size_t>(frames_) * kept);
        for (u32 k = 0; k < bones(); ++k) {
            if (!(mass[k] > 0.0)) {
                continue;
            }
            const u32 to = renumber[k];
            out.pivots[to] = Vector3f{static_cast<f32>(pivots_[k][0] + restCentre_[0]),
                                      static_cast<f32>(pivots_[k][1] + restCentre_[1]),
                                      static_cast<f32>(pivots_[k][2] + restCentre_[2])};
            for (u32 f = 0; f < frames_; ++f) {
                const Affine& a = motion_[k][f];
                Matrix44f m = Matrix44f::identity();
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        m.data[c][r] = static_cast<f32>(a.m[r][c]);
                    }
                    // Back out of the centred frames: t = t' + m_f − R·c₀.
                    m.data[3][r] = static_cast<f32>(a.t[r] + frameCentre_[f][r] -
                                                    (a.m[r][0] * restCentre_[0] + a.m[r][1] * restCentre_[1] +
                                                     a.m[r][2] * restCentre_[2]));
                }
                out.skinning[static_cast<std::size_t>(f) * kept + to] = m;
            }
        }
        out.weights.resize(points_);
        for (u32 i = 0; i < points_; ++i) {
            for (const Weight& w : weights_[i]) {
                if (w.fixed || mass[w.bone] > 0.0) {
                    out.weights[i].push_back(
                        geom::Influence{w.fixed ? kept + w.bone : renumber[w.bone], static_cast<f32>(w.w)});
                }
            }
        }
        out.rms = rms_;
        out.worst = worst_;
        return out;
    }

    const FitOptions& options_;
    u32 points_ = 0;
    u32 frames_ = 0;
    u32 fixedCount_ = 0;
    std::span<const std::vector<u32>> allowed_;
    V3 restCentre_{};
    std::vector<V3> rest_;
    std::vector<V3> target_;      ///< Frame-major, each frame on its centre.
    std::vector<V3> frameCentre_;
    std::vector<Affine> fixed_;   ///< Frame-major, into the centred frames.
    std::vector<std::vector<Affine>> motion_; ///< Per new bone, per frame.
    std::vector<u32> owner_;
    std::vector<std::vector<Weight>> weights_;
    std::vector<V3> pivots_;
    std::vector<V3> seeds_;
    std::vector<V3> recon_;
    std::vector<f64> error_;
    f32 rms_ = 0.0f;
    f32 worst_ = 0.0f;
};

} // namespace

bool SolveRigid(const f64 (&from)[3], const f64 (&to)[3], const f64 (&cross)[3][3], Quaternion& rotation,
                Vector3f& translation) {
    const std::array<f64, 4> q = HornQuaternion(cross);
    const f64 length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 0.0)) {
        return false;
    }
    rotation = Quaternion{static_cast<f32>(q[1] / length), static_cast<f32>(q[2] / length),
                          static_cast<f32>(q[3] / length), static_cast<f32>(q[0] / length)};
    const Vector3f turned = rotation.rotate_vector(
        Vector3f{static_cast<f32>(from[0]), static_cast<f32>(from[1]), static_cast<f32>(from[2])});
    translation = Vector3f{static_cast<f32>(to[0]) - turned.x, static_cast<f32>(to[1]) - turned.y,
                           static_cast<f32>(to[2]) - turned.z};
    return true;
}

PointFit FitPointWeights(const Vector3f& rest, std::span<const geom::Influence> truth,
                         std::span<const Matrix44f> palette, u32 nodes, u32 frames,
                         std::span<const u32> candidates, u32 maxInfluences) {
    PointFit out;
    const u32 count = static_cast<u32>(candidates.size());
    out.weights.assign(count, 0.0f);
    if (count == 0 || palette.size() < static_cast<std::size_t>(frames) * nodes) {
        return out;
    }
    const auto place = [&](u32 f, u32 node) {
        return Of(transform_point(rest, palette[static_cast<std::size_t>(f) * nodes + node]));
    };
    const auto target = [&](u32 f) {
        V3 sum{};
        for (const geom::Influence& influence : truth) {
            if (influence.bone < nodes && influence.weight > 0.0f) {
                const V3 q = place(f, influence.bone);
                for (int r = 0; r < 3; ++r) {
                    sum[r] += influence.weight * q[r];
                }
            }
        }
        return sum;
    };
    // With the weights summing to one, the residual is Σ w (y − x): each
    // candidate's miss from where the point should be, so the sums stay the
    // size of the misses wherever the point is.
    const auto gram = [&](const std::vector<u32>& set) {
        const u32 n = static_cast<u32>(set.size());
        std::vector<f64> g(static_cast<std::size_t>(n) * n, 0.0);
        std::vector<V3> z(n);
        for (u32 f = 0; f < frames; ++f) {
            const V3 x = target(f);
            for (u32 c = 0; c < n; ++c) {
                const V3 y = candidates[set[c]] < nodes ? place(f, candidates[set[c]]) : x;
                z[c] = {y[0] - x[0], y[1] - x[1], y[2] - x[2]};
            }
            for (u32 a = 0; a < n; ++a) {
                for (u32 b = a; b < n; ++b) {
                    g[a * n + b] += z[a][0] * z[b][0] + z[a][1] * z[b][1] + z[a][2] * z[b][2];
                }
            }
        }
        for (u32 a = 0; a < n; ++a) {
            for (u32 b = 0; b < a; ++b) {
                g[a * n + b] = g[b * n + a];
            }
        }
        return g;
    };
    const auto solveOver = [&](const std::vector<u32>& set, f64& error) {
        const u32 n = static_cast<u32>(set.size());
        std::vector<f64> g = gram(set);
        f64 scale = 0.0;
        for (u32 a = 0; a < n; ++a) {
            scale += g[a * n + a];
        }
        scale = scale > 0.0 ? scale / n : 1.0;
        std::vector<f64> scaled = g;
        for (f64& value : scaled) {
            value /= scale;
        }
        for (u32 a = 0; a < n; ++a) {
            scaled[a * n + a] += 1e-12;
        }
        const std::vector<f64> w = SimplexLeastSquares(scaled, std::vector<f64>(n, 0.0), n);
        error = 0.0;
        for (u32 a = 0; a < n; ++a) {
            for (u32 b = 0; b < n; ++b) {
                error += w[a] * g[a * n + b] * w[b];
            }
        }
        return w;
    };
    std::vector<u32> set(count);
    for (u32 c = 0; c < count; ++c) {
        set[c] = c;
    }
    f64 error = 0.0;
    std::vector<f64> w = solveOver(set, error);
    if (count > maxInfluences) {
        std::vector<u32> order = set;
        std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return w[a] > w[b]; });
        order.resize(std::max(1u, maxInfluences));
        std::sort(order.begin(), order.end());
        const std::vector<f64> kept = solveOver(order, error);
        std::fill(w.begin(), w.end(), 0.0);
        for (std::size_t k = 0; k < order.size(); ++k) {
            w[order[k]] = kept[k];
        }
    }
    for (u32 c = 0; c < count; ++c) {
        out.weights[c] = static_cast<f32>(w[c]);
    }
    out.error = std::max(0.0, error);
    f64 worst = 0.0;
    for (u32 f = 0; f < frames; ++f) {
        const V3 x = target(f);
        V3 miss{};
        for (u32 c = 0; c < count; ++c) {
            if (w[c] > 0.0 && candidates[c] < nodes) {
                const V3 y = place(f, candidates[c]);
                for (int r = 0; r < 3; ++r) {
                    miss[r] += w[c] * (y[r] - x[r]);
                }
            }
        }
        worst = std::max(worst, miss[0] * miss[0] + miss[1] * miss[1] + miss[2] * miss[2]);
    }
    out.worst = static_cast<f32>(std::sqrt(worst));
    return out;
}

FitResult FitBones(const FitMotion& motion, const FitFixedBones& fixed, const FitOptions& options) {
    if (motion.frames.size() < static_cast<std::size_t>(motion.frameCount) * motion.rest.size() ||
        fixed.skinning.size() < static_cast<std::size_t>(motion.frameCount) * fixed.count) {
        return {};
    }
    Fitter fitter(motion, fixed, options);
    return fitter.run();
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
