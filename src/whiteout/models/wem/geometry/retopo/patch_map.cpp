// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "patch_map.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "sparse.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

namespace {

f64 Cross2(const P2& a, const P2& b, const P2& c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

u64 Key(u32 a, u32 b) {
    return (static_cast<u64>(a) << 32) | b;
}

} // namespace

u32 PatchMap::locate(const P2& point, f64 weights[3]) const {
    u32 best = kNone;
    f64 bestScore = -std::numeric_limits<f64>::infinity();
    for (u32 t = 0; t < triangleCount(); ++t) {
        const P2& a = uv[corners[3 * t]];
        const P2& b = uv[corners[3 * t + 1]];
        const P2& c = uv[corners[3 * t + 2]];
        const f64 area = Cross2(a, b, c);
        if (std::abs(area) < 1e-300) {
            continue;
        }
        const f64 w0 = Cross2(point, b, c) / area;
        const f64 w1 = Cross2(a, point, c) / area;
        const f64 w2 = 1.0 - w0 - w1;
        const f64 score = std::min({w0, w1, w2});
        if (score > bestScore) {
            bestScore = score;
            best = t;
            weights[0] = w0;
            weights[1] = w1;
            weights[2] = w2;
        }
    }
    if (best != kNone && bestScore < 0.0) {
        // Just outside every triangle (a grid point on a degenerate border):
        // the nearest one, its weights clamped back inside.
        f64 sum = 0.0;
        for (u32 i = 0; i < 3; ++i) {
            weights[i] = std::max(weights[i], 0.0);
            sum += weights[i];
        }
        for (u32 i = 0; i < 3; ++i) {
            weights[i] = sum > 0.0 ? weights[i] / sum : 1.0 / 3.0;
        }
    }
    return best;
}

std::vector<P2> CircleBorder(const WorkMesh& mesh, const LayoutPatch& patch) {
    std::vector<f64> at(patch.loop.size() + 1, 0.0);
    for (std::size_t i = 0; i < patch.loop.size(); ++i) {
        const u32 h = patch.loop[i];
        at[i + 1] = at[i] + Distance(mesh.positions[mesh.from(h)], mesh.positions[mesh.to(h)]);
    }
    std::vector<P2> border(patch.loop.size());
    const f64 total = at.back() > 0.0 ? at.back() : 1.0;
    for (std::size_t i = 0; i < patch.loop.size(); ++i) {
        const f64 angle = 2.0 * kPi * at[i] / total;
        border[i] = {std::cos(angle), std::sin(angle)};
    }
    return border;
}

std::vector<P2> CornerCircleBorder(const WorkMesh& mesh, const LayoutPatch& patch,
                                   const std::vector<u32>& corners) {
    const u32 size = static_cast<u32>(patch.loop.size());
    const u32 n = static_cast<u32>(corners.size());
    if (n == 0) {
        return CircleBorder(mesh, patch);
    }
    std::vector<P2> border(size);
    for (u32 s = 0; s < n; ++s) {
        const u32 begin = corners[s];
        const u32 span = (corners[(s + 1) % n] + size - begin) % size;
        const u32 count = span == 0 ? size : span;
        std::vector<f64> at(count + 1, 0.0);
        for (u32 k = 0; k < count; ++k) {
            const u32 h = patch.loop[(begin + k) % size];
            at[k + 1] = at[k] + Distance(mesh.positions[mesh.from(h)], mesh.positions[mesh.to(h)]);
        }
        const f64 total = at[count] > 0.0 ? at[count] : 1.0;
        for (u32 k = 0; k < count; ++k) {
            const f64 angle = 2.0 * kPi * (s + at[k] / total) / n;
            border[(begin + k) % size] = {std::cos(angle), std::sin(angle)};
        }
    }
    return border;
}

bool BuildPatchMap(const WorkMesh& mesh, const LayoutPatch& patch, const std::vector<P2>& border,
                   PatchMap& map) {
    map = PatchMap{};
    if (patch.loop.empty() || patch.loops != 1 || patch.euler != 1 || border.size() != patch.loop.size()) {
        return false;
    }
    std::unordered_map<u32, u32> local;
    for (u32 i = 0; i < patch.triangles.size(); ++i) {
        local[patch.triangles[i]] = i;
    }
    // Border copies: each loop position owns the wedge of triangles round its
    // vertex from its outgoing loop edge counter-clockwise to the next one.
    std::unordered_map<u64, u32> cornerCopy; // (work triangle, work vertex) -> map vertex
    for (u32 i = 0; i < patch.loop.size(); ++i) {
        const u32 h = patch.loop[i];
        const u32 v = mesh.from(h);
        map.vertices.push_back(v);
        u32 o = h;
        for (u32 guard = 0; guard < 4096; ++guard) {
            cornerCopy[Key(o / 3, v)] = i;
            const u32 in = WorkMesh::Prev(o);
            if (mesh.isLayout(in) || mesh.twins[in] == kNone) {
                break;
            }
            o = mesh.twins[in];
            if (!local.count(o / 3)) {
                break;
            }
        }
    }
    map.borderCount = static_cast<u32>(patch.loop.size());
    map.uv.assign(map.borderCount, P2{0.0, 0.0});
    for (u32 i = 0; i < map.borderCount; ++i) {
        map.uv[i] = border[i];
    }
    std::unordered_map<u32, u32> inside;
    map.corners.resize(3 * patch.triangles.size());
    map.triangles = patch.triangles;
    for (u32 t = 0; t < patch.triangles.size(); ++t) {
        const u32 work = patch.triangles[t];
        for (u32 j = 0; j < 3; ++j) {
            const u32 v = mesh.corners[3 * work + j];
            const auto copy = cornerCopy.find(Key(work, v));
            if (copy != cornerCopy.end()) {
                map.corners[3 * t + j] = copy->second;
                continue;
            }
            auto it = inside.find(v);
            if (it == inside.end()) {
                it = inside.emplace(v, static_cast<u32>(map.vertices.size())).first;
                map.vertices.push_back(v);
                map.uv.push_back({0.0, 0.0});
            }
            map.corners[3 * t + j] = it->second;
        }
    }
    // Adjacency across the patch's inside edges.
    map.across.assign(3 * patch.triangles.size(), kNone);
    for (u32 t = 0; t < patch.triangles.size(); ++t) {
        const u32 work = patch.triangles[t];
        for (u32 j = 0; j < 3; ++j) {
            const u32 h = 3 * work + j;
            const u32 twin = mesh.twins[h];
            if (twin == kNone || mesh.isLayout(h)) {
                continue;
            }
            const auto other = local.find(twin / 3);
            if (other != local.end()) {
                map.across[3 * t + j] = 3 * other->second + twin % 3;
            }
        }
    }

    // Tutte with clamped cotangent weights: symmetric and positive, so the map
    // of a disk onto a convex border is one to one.
    const u32 count = static_cast<u32>(map.vertices.size());
    const u32 unknowns = count - map.borderCount;
    if (unknowns == 0) {
        return true;
    }
    std::unordered_map<u64, f64> weight;
    f64 sum = 0.0;
    for (u32 t = 0; t < patch.triangles.size(); ++t) {
        for (u32 j = 0; j < 3; ++j) {
            const u32 a = map.corners[3 * t + j];
            const u32 b = map.corners[3 * t + (j + 1) % 3];
            const u32 c = map.corners[3 * t + (j + 2) % 3];
            const V3& pa = mesh.positions[map.vertices[a]];
            const V3& pb = mesh.positions[map.vertices[b]];
            const V3& pc = mesh.positions[map.vertices[c]];
            const V3 u = pa - pc;
            const V3 w = pb - pc;
            const f64 cross = Length(Cross(u, w));
            const f64 cot = cross > 1e-300 ? Dot(u, w) / cross : 0.0;
            weight[Key(std::min(a, b), std::max(a, b))] += 0.5 * cot;
            sum += std::abs(0.5 * cot);
        }
    }
    const f64 floor = std::max(1e-6, 0.05 * sum / std::max<std::size_t>(1, weight.size()));
    // In key order, so the sums come out the same whatever the hash order.
    std::vector<std::pair<u64, f64>> weights(weight.begin(), weight.end());
    std::sort(weights.begin(), weights.end());
    SparseBuilder builder(unknowns);
    std::vector<f64> bx(unknowns, 0.0);
    std::vector<f64> by(unknowns, 0.0);
    for (auto& [key, w] : weights) {
        w = std::max(w, floor);
        const u32 a = static_cast<u32>(key >> 32);
        const u32 b = static_cast<u32>(key & 0xFFFFFFFFu);
        const bool aFree = a >= map.borderCount;
        const bool bFree = b >= map.borderCount;
        if (aFree) {
            builder.add(a - map.borderCount, a - map.borderCount, w);
        }
        if (bFree) {
            builder.add(b - map.borderCount, b - map.borderCount, w);
        }
        if (aFree && bFree) {
            builder.add(a - map.borderCount, b - map.borderCount, -w);
            builder.add(b - map.borderCount, a - map.borderCount, -w);
        } else if (aFree) {
            bx[a - map.borderCount] += w * map.uv[b][0];
            by[a - map.borderCount] += w * map.uv[b][1];
        } else if (bFree) {
            bx[b - map.borderCount] += w * map.uv[a][0];
            by[b - map.borderCount] += w * map.uv[a][1];
        }
    }
    const SparseMatrix matrix = builder.build();
    std::vector<f64> x(unknowns, 0.0);
    std::vector<f64> y(unknowns, 0.0);
    // A map the solve left far from settled need not be one-to-one.
    for (const CgResult& solve : {SolveConjugateGradient(matrix, bx, x, 1e-10, 10 * unknowns + 200),
                                  SolveConjugateGradient(matrix, by, y, 1e-10, 10 * unknowns + 200)}) {
        if (!solve.converged && !(solve.residual < 1e-6)) {
            return false;
        }
    }
    for (u32 k = 0; k < unknowns; ++k) {
        map.uv[map.borderCount + k] = {x[k], y[k]};
    }
    return true;
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
