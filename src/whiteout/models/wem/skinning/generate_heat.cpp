// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Bone heat (EDIT_MODE_SKIN_DESIGN.md §8.4): the Laplacian over the welded
/// ring, the visibility test, and the conjugate gradients that solve one system
/// per bone. `SolveHeat` in `generate.h` says what is solved and why.

#include <whiteout/models/wem/skinning/generate.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>

#include <whiteout/models/wem/geometry/bvh.h>

#include "generate_common.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

using detail::PositionOf;

/// How many of a point's nearest segments are tried before it is taken to see
/// no bone. Pinocchio tries every one; past the first few, a visible bone is so
/// far away that `1/d²` gives it next to nothing, and a plate that sees no bone
/// at all would otherwise cast a ray at each of a hundred.
constexpr u32 kVisibilityTries = 16;

/// Points per visibility job: enough that handing jobs out costs nothing next
/// to the rays, few enough that the bar moves.
constexpr u32 kVisibilityChunk = 512;

/// The residual, relative to the right-hand side, at which a solve stops. The
/// weights are written at 1/255 at best, so this is far past what shows.
constexpr f64 kSolveTolerance = 1e-6;

/// How often a solve looks at the stop, in iterations.
constexpr u32 kStopCheckEvery = 32;

/// A solved weight under this is taken as none. CG leaves crumbs of the order
/// of its tolerance on every point of a piece, and writing them would hand each
/// point every bone of its piece.
constexpr f32 kSolvedFloor = 1e-5f;

/// Union-find over one mesh's points.
class Unions {
public:
    explicit Unions(u32 count) : parent_(count) {
        std::iota(parent_.begin(), parent_.end(), 0u);
    }
    u32 find(u32 item) {
        while (parent_[item] != item) {
            parent_[item] = parent_[parent_[item]];
            item = parent_[item];
        }
        return item;
    }
    void join(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent_[std::max(a, b)] = std::min(a, b);
        }
    }

private:
    std::vector<u32> parent_;
};

/// The ring slot of the edge @p from -> @p to: an index into `PointTable::ring`,
/// or `kInvalidIndex` when the two are not ring neighbours. The ring is sorted
/// per point, so this is a binary search.
u32 SlotOf(const PointTable& points, u32 from, u32 to) {
    const std::span<const u32> ring = points.ringOf(from);
    const auto found = std::lower_bound(ring.begin(), ring.end(), to);
    if (found == ring.end() || *found != to) {
        return kInvalidIndex;
    }
    return points.ringOffsets[from] + static_cast<u32>(found - ring.begin());
}

/// One piece's matrix `L + A·H`, over its own points in ascending order.
struct PieceMatrix {
    std::vector<u32> offsets;  ///< Per local row, one past the end included.
    std::vector<u32> columns;  ///< Local.
    std::vector<f64> values;   ///< The off-diagonal: minus the edge weight.
    std::vector<f64> diagonal; ///< The row's edge weights summed, plus `A·H`.

    void multiply(std::span<const f64> x, std::span<f64> y) const {
        for (std::size_t row = 0; row < diagonal.size(); ++row) {
            f64 sum = diagonal[row] * x[row];
            for (u32 k = offsets[row]; k < offsets[row + 1]; ++k) {
                sum += values[k] * x[columns[k]];
            }
            y[row] = sum;
        }
    }
};

/**
 * @brief Jacobi-preconditioned conjugate gradients on @p matrix.
 *
 * Every matrix this is asked about is symmetric positive definite: `L` is the
 * weighted graph Laplacian of a CONNECTED piece, and `A·H` is positive on at
 * least one of its points. False when the run was stopped.
 */
bool SolveConjugate(const PieceMatrix& matrix, std::span<const f64> rhs, std::vector<f64>& x,
                    const GenerateControl& control) {
    const std::size_t n = matrix.diagonal.size();
    x.assign(n, 0.0);
    std::vector<f64> r(rhs.begin(), rhs.end());
    std::vector<f64> z(n);
    std::vector<f64> p(n);
    std::vector<f64> q(n);

    f64 target = 0.0;
    for (const f64 value : rhs) {
        target += value * value;
    }
    target *= kSolveTolerance * kSolveTolerance;
    if (!(target > 0.0)) {
        return true; // a zero right-hand side solves to zero
    }

    f64 rz = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        z[i] = r[i] / matrix.diagonal[i];
        p[i] = z[i];
        rz += r[i] * z[i];
    }
    // CG reaches the answer in at most n steps in exact arithmetic. The cap is
    // for the arithmetic that is not, and is never the stop that matters.
    const u32 cap = static_cast<u32>(std::clamp<std::size_t>(n * 2, 200, 20000));
    for (u32 iteration = 0; iteration < cap; ++iteration) {
        if (iteration % kStopCheckEvery == kStopCheckEvery - 1 && control.stopped()) {
            return false;
        }
        matrix.multiply(p, q);
        f64 pq = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            pq += p[i] * q[i];
        }
        if (!(pq > 0.0)) {
            break;
        }
        const f64 alpha = rz / pq;
        f64 residual = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * q[i];
            residual += r[i] * r[i];
        }
        if (residual <= target) {
            break;
        }
        f64 next = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            z[i] = r[i] / matrix.diagonal[i];
            next += r[i] * z[i];
        }
        const f64 beta = next / rz;
        rz = next;
        for (std::size_t i = 0; i < n; ++i) {
            p[i] = z[i] + beta * p[i];
        }
    }
    return true;
}

/// Everything one mesh contributes to a run.
struct MeshSystem {
    const Mesh* mesh = nullptr;
    const PointTable* points = nullptr;
    std::span<const Vector3f> positions;
    std::vector<u32> scope;

    std::vector<f64> edge; ///< Per ring slot: the clamped cotangent weight.
    std::vector<f64> area; ///< Per point: a third of each face it holds.

    std::vector<u32> pieceOf;             ///< Per point.
    std::vector<u32> localOf;             ///< Per point: its row in its piece.
    std::vector<std::vector<u32>> pieces; ///< Their points, ascending.
    std::vector<u8> wanted;               ///< Per piece: it holds a scope point.

    std::vector<u32> sees; ///< Per point: its nearest visible bone.
    std::vector<f64> pull; ///< Per point: `A·H`, 0 when it sees none.

    geom::TriangleBvh bvh;
    f32 rayLift = 0.0f;  ///< How far off its own surface a ray starts.
    f32 nearest = 0.0f;  ///< The least distance `H` is taken at.

    std::vector<PieceMatrix> matrices; ///< Per piece; empty for one not solved.
    std::vector<u32> vote;             ///< Per piece: its bone when it is singular.
};

/// §8.4's Laplacian: `(cot α + cot β) / 2` per edge of the welded ring, a
/// uniform `1/2` per edge of a degenerate face, clamped at 0; and each point's
/// barycentric area.
void BuildLaplacian(MeshSystem& system) {
    const PointTable& points = *system.points;
    system.edge.assign(points.ring.size(), 0.0);
    system.area.assign(points.pointCount, 0.0);

    const auto add = [&](u32 from, u32 to, f64 weight) {
        // A drawn diagonal of a polygon is a ring edge (the point table adds
        // them); a pair with no slot is one a weld folded, and is dropped.
        const u32 there = SlotOf(points, from, to);
        const u32 back = SlotOf(points, to, from);
        if (there != kInvalidIndex && back != kInvalidIndex) {
            system.edge[there] += weight;
            system.edge[back] += weight;
        }
    };

    const std::vector<u32> triangles = detail::TrianglesOf(*system.mesh);
    for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
        const u32 va = triangles[t];
        const u32 vb = triangles[t + 1];
        const u32 vc = triangles[t + 2];
        if (va >= system.positions.size() || vb >= system.positions.size() ||
            vc >= system.positions.size()) {
            continue;
        }
        const u32 pa = points.pointOfVertex(va);
        const u32 pb = points.pointOfVertex(vb);
        const u32 pc = points.pointOfVertex(vc);
        if (pa == kInvalidIndex || pb == kInvalidIndex || pc == kInvalidIndex || pa == pb ||
            pb == pc || pc == pa) {
            continue; // a face the weld folded flat
        }
        const Vector3f a = system.positions[va];
        const Vector3f b = system.positions[vb];
        const Vector3f c = system.positions[vc];
        const Vector3f ab = b - a;
        const Vector3f ac = c - a;
        const Vector3f bc = c - b;
        const f64 twice = static_cast<f64>(cross(ab, ac).length());
        const f64 longest = static_cast<f64>(
            std::max({ab.length_squared(), ac.length_squared(), bc.length_squared()}));
        if (!(twice > 1e-12 * longest)) {
            add(pa, pb, 0.5);
            add(pb, pc, 0.5);
            add(pc, pa, 0.5);
            continue;
        }
        const f64 third = twice / 6.0;
        system.area[pa] += third;
        system.area[pb] += third;
        system.area[pc] += third;
        // The cotangent at a corner weighs the edge opposite it.
        const f64 cotA = static_cast<f64>(ab.dot(ac)) / twice;
        const f64 cotB = static_cast<f64>((a - b).dot(bc)) / twice;
        const f64 cotC = static_cast<f64>(ac.dot(bc)) / twice;
        add(pb, pc, 0.5 * cotA);
        add(pc, pa, 0.5 * cotB);
        add(pa, pb, 0.5 * cotC);
    }
    for (f64& weight : system.edge) {
        weight = std::max(weight, 0.0);
    }
}

/// The pieces the clamped Laplacian falls into: islands, or parts of one where
/// a clamp cut the last edge between them. Each is solved alone, and a piece
/// with no pull at all is exactly the singular system §8.4 names.
void BuildPieces(MeshSystem& system) {
    const PointTable& points = *system.points;
    Unions unions(points.pointCount);
    for (u32 point = 0; point < points.pointCount; ++point) {
        const std::span<const u32> ring = points.ringOf(point);
        for (std::size_t k = 0; k < ring.size(); ++k) {
            if (system.edge[points.ringOffsets[point] + k] > 0.0) {
                unions.join(point, ring[k]);
            }
        }
    }
    system.pieceOf.assign(points.pointCount, kInvalidIndex);
    system.localOf.assign(points.pointCount, kInvalidIndex);
    std::vector<u32> pieceOfRoot(points.pointCount, kInvalidIndex);
    for (u32 point = 0; point < points.pointCount; ++point) {
        const u32 root = unions.find(point);
        if (pieceOfRoot[root] == kInvalidIndex) {
            pieceOfRoot[root] = static_cast<u32>(system.pieces.size());
            system.pieces.emplace_back();
        }
        const u32 piece = pieceOfRoot[root];
        system.pieceOf[point] = piece;
        system.localOf[point] = static_cast<u32>(system.pieces[piece].size());
        system.pieces[piece].push_back(point);
    }
    system.wanted.assign(system.pieces.size(), 0);
    for (const u32 point : system.scope) {
        if (point < points.pointCount) {
            system.wanted[system.pieceOf[point]] = 1;
        }
    }
}

/// Each point of a wanted piece: its nearest bone it can see, and `A·H`.
void SeeBones(MeshSystem& system, const BoneSegments& segments, u32 first, u32 last) {
    const PointTable& points = *system.points;
    struct Candidate {
        f32 signedDistance;
        u32 segment;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(segments.segments.size());
    for (u32 point = first; point < last; ++point) {
        if (system.wanted[system.pieceOf[point]] == 0) {
            continue;
        }
        const Vector3f at = PositionOf(system.positions, points, point);
        candidates.clear();
        for (u32 s = 0; s < segments.segments.size(); ++s) {
            const BoneSegments::Segment& segment = segments.segments[s];
            candidates.push_back(
                {DistanceToSegment(at, segment.start, segment.end) - segment.radius, s});
        }
        const std::size_t tries = std::min<std::size_t>(kVisibilityTries, candidates.size());
        // Nearest first, as `NearestBone` measures, with its tie to the lower
        // bone; only the few that will be tried need to be in order.
        const auto nearer = [&](const Candidate& a, const Candidate& b) {
            if (a.signedDistance != b.signedDistance) {
                return a.signedDistance < b.signedDistance;
            }
            return segments.segments[a.segment].bone < segments.segments[b.segment].bone;
        };
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(tries),
                          candidates.end(), nearer);
        for (std::size_t i = 0; i < tries; ++i) {
            const BoneSegments::Segment& segment = segments.segments[candidates[i].segment];
            // The bone's own nearest point: the segment's, or a leaf's joint.
            const Vector3f along = segment.end - segment.start;
            const f32 span = along.length_squared();
            f32 t = 0.0f;
            if (span > 0.0f) {
                t = std::clamp((at - segment.start).dot(along) / span, 0.0f, 1.0f);
            }
            const Vector3f target = segment.start + along * t;
            const Vector3f toward = target - at;
            const f32 distance = toward.length();
            bool visible = distance <= system.rayLift * 2.0f;
            if (!visible) {
                const f32 lift = system.rayLift / distance;
                visible = !system.bvh.raycast(at, toward, lift, 1.0f - lift).hit();
            }
            if (visible) {
                const f64 d = static_cast<f64>(std::max(distance, system.nearest));
                system.sees[point] = segment.bone;
                system.pull[point] = system.area[point] / (d * d);
                break;
            }
        }
    }
}

/// A wanted piece's matrix, or none when it is singular -- in which case its
/// vote is taken instead.
void BuildPieceMatrix(MeshSystem& system, const BoneSegments& segments, u32 piece) {
    const PointTable& points = *system.points;
    const std::vector<u32>& members = system.pieces[piece];
    bool pulled = false;
    for (const u32 point : members) {
        pulled = pulled || system.pull[point] > 0.0;
    }
    if (!pulled) {
        system.vote[piece] = detail::VoteBone(segments, system.positions, points, members);
        return;
    }
    PieceMatrix& matrix = system.matrices[piece];
    matrix.offsets.assign(1, 0);
    matrix.diagonal.assign(members.size(), 0.0);
    for (std::size_t row = 0; row < members.size(); ++row) {
        const u32 point = members[row];
        const std::span<const u32> ring = points.ringOf(point);
        f64 total = 0.0;
        for (std::size_t k = 0; k < ring.size(); ++k) {
            const f64 weight = system.edge[points.ringOffsets[point] + k];
            if (!(weight > 0.0)) {
                continue;
            }
            // A positive edge never leaves its piece: that is how pieces are
            // made.
            matrix.columns.push_back(system.localOf[ring[k]]);
            matrix.values.push_back(-weight);
            total += weight;
        }
        matrix.diagonal[row] = total + system.pull[point];
        matrix.offsets.push_back(static_cast<u32>(matrix.columns.size()));
    }
    // A point with no positive edge and no pull is a piece of its own, and a
    // piece of one with no pull is singular and was voted on above -- so every
    // diagonal left here is positive, and Jacobi never divides by zero.
}

/// One solve: a piece of a mesh, and a bone some point of it sees.
struct Solve {
    u32 mesh = 0;
    u32 piece = 0;
    u32 bone = 0;
    std::vector<f32> weights; ///< Per local row of the piece.
};

} // namespace

GeneratedRun SolveHeat(std::span<const MeshScope> meshes, const NodeTree& nodes,
                       const GenerateOptions& options, const GenerateControl& control) {
    GeneratedRun run;
    const BoneSegments segments = BuildBoneSegments(nodes, options.bones);

    std::vector<MeshSystem> systems(meshes.size());
    u32 visibilityJobs = 0;
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        MeshSystem& system = systems[m];
        if (meshes[m].mesh == nullptr || meshes[m].points == nullptr) {
            continue;
        }
        system.mesh = meshes[m].mesh;
        system.points = meshes[m].points;
        system.positions =
            system.mesh->attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        system.scope = detail::ScopeOrEvery(*system.points, meshes[m].scope);
        visibilityJobs += (system.points->pointCount + kVisibilityChunk - 1) / kVisibilityChunk;
    }
    if (segments.empty()) {
        // No bone to write to: every point is unreached, and nothing is
        // decided -- the honest answer, not an empty skin.
        for (const MeshSystem& system : systems) {
            GeneratedWeights none;
            none.unreached = static_cast<u32>(system.scope.size());
            run.meshes.push_back(std::move(none));
        }
        return run;
    }

    // ---- 1. Each mesh's Laplacian, its pieces and its rays ----------------
    for (MeshSystem& system : systems) {
        if (system.points == nullptr || system.points->pointCount == 0) {
            continue;
        }
        BuildLaplacian(system);
        BuildPieces(system);
        system.sees.assign(system.points->pointCount, kInvalidNode);
        system.pull.assign(system.points->pointCount, 0.0);
        const std::vector<u32> triangles = detail::TrianglesOf(*system.mesh);
        system.bvh.build(triangles, system.positions);
        // The weld tolerance is 1e-5 of the mesh's diagonal: ten of it lifts a
        // ray off its own face, and a hundred is closer to a bone than the
        // mesh can say anything about.
        const f32 weld = system.points->weldTolerance > 0.0f ? system.points->weldTolerance : 1e-6f;
        system.rayLift = weld * 10.0f;
        system.nearest = weld * 100.0f;
        if (control.stopped()) {
            run.cancelled = true;
            return run;
        }
    }

    // ---- 2. Who sees which bone, in chunks over every mesh -----------------
    struct Chunk {
        u32 mesh;
        u32 first;
        u32 last;
    };
    std::vector<Chunk> chunks;
    for (u32 m = 0; m < systems.size(); ++m) {
        if (systems[m].points == nullptr) {
            continue;
        }
        const u32 count = systems[m].points->pointCount;
        for (u32 first = 0; first < count; first += kVisibilityChunk) {
            chunks.push_back({m, first, std::min(count, first + kVisibilityChunk)});
        }
    }
    u32 done = 0;
    u32 total = visibilityJobs;
    if (!detail::RunJobs(static_cast<u32>(chunks.size()), control, done, total,
                         [&](u32 index) {
                             const Chunk& chunk = chunks[index];
                             SeeBones(systems[chunk.mesh], segments, chunk.first, chunk.last);
                         })) {
        run.cancelled = true;
        return run;
    }

    // ---- 3. The matrices, and one solve per piece and bone -----------------
    std::vector<Solve> solves;
    for (u32 m = 0; m < systems.size(); ++m) {
        MeshSystem& system = systems[m];
        if (system.points == nullptr) {
            continue;
        }
        system.matrices.resize(system.pieces.size());
        system.vote.assign(system.pieces.size(), kInvalidNode);
        for (u32 piece = 0; piece < system.pieces.size(); ++piece) {
            if (system.wanted[piece] == 0) {
                continue;
            }
            BuildPieceMatrix(system, segments, piece);
            if (system.matrices[piece].diagonal.empty()) {
                continue;
            }
            // One right-hand side per bone some point of the piece sees. A
            // bone nobody sees solves to zero exactly, so it is not solved.
            std::vector<u32> bones;
            for (const u32 point : system.pieces[piece]) {
                if (system.sees[point] != kInvalidNode) {
                    bones.push_back(system.sees[point]);
                }
            }
            std::sort(bones.begin(), bones.end());
            bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
            for (const u32 bone : bones) {
                solves.push_back({m, piece, bone, {}});
            }
        }
    }
    total = visibilityJobs + static_cast<u32>(solves.size());
    std::atomic<bool> stoppedInside{false};
    if (!detail::RunJobs(static_cast<u32>(solves.size()), control, done, total,
                         [&](u32 index) {
                             Solve& solve = solves[index];
                             const MeshSystem& system = systems[solve.mesh];
                             const std::vector<u32>& members = system.pieces[solve.piece];
                             std::vector<f64> rhs(members.size(), 0.0);
                             for (std::size_t row = 0; row < members.size(); ++row) {
                                 if (system.sees[members[row]] == solve.bone) {
                                     rhs[row] = system.pull[members[row]];
                                 }
                             }
                             std::vector<f64> x;
                             if (!SolveConjugate(system.matrices[solve.piece], rhs, x, control)) {
                                 stoppedInside.store(true, std::memory_order_release);
                                 return;
                             }
                             solve.weights.resize(x.size());
                             for (std::size_t row = 0; row < x.size(); ++row) {
                                 solve.weights[row] = static_cast<f32>(std::max(x[row], 0.0));
                             }
                         }) ||
        stoppedInside.load(std::memory_order_acquire)) {
        run.cancelled = true;
        return run;
    }

    // ---- 4. Gather, in bone order, point by point --------------------------
    // Per mesh and piece, where its solves start in `solves`: they were made
    // piece by piece with the bones ascending, so a piece's are contiguous.
    std::vector<std::vector<u32>> firstSolve(systems.size());
    std::vector<std::vector<u32>> solveCount(systems.size());
    for (u32 m = 0; m < systems.size(); ++m) {
        firstSolve[m].assign(systems[m].pieces.size(), 0);
        solveCount[m].assign(systems[m].pieces.size(), 0);
    }
    for (u32 index = 0; index < solves.size(); ++index) {
        const Solve& solve = solves[index];
        if (solveCount[solve.mesh][solve.piece] == 0) {
            firstSolve[solve.mesh][solve.piece] = index;
        }
        ++solveCount[solve.mesh][solve.piece];
    }

    std::vector<geom::Influence> weights;
    for (u32 m = 0; m < systems.size(); ++m) {
        const MeshSystem& system = systems[m];
        GeneratedWeights out;
        if (system.points == nullptr) {
            run.meshes.push_back(std::move(out));
            continue;
        }
        for (u32 piece = 0; piece < system.pieces.size(); ++piece) {
            out.islands += system.wanted[piece] != 0 ? 1u : 0u;
        }
        for (const u32 point : system.scope) {
            if (point >= system.points->pointCount) {
                continue;
            }
            const u32 piece = system.pieceOf[point];
            weights.clear();
            if (system.matrices[piece].diagonal.empty()) {
                // Singular: the whole piece goes to its vote (§8.3).
                if (system.vote[piece] != kInvalidNode) {
                    weights.push_back({system.vote[piece], 1.0f});
                }
            } else {
                const u32 row = system.localOf[point];
                f32 sum = 0.0f;
                for (u32 k = 0; k < solveCount[m][piece]; ++k) {
                    const Solve& solve = solves[firstSolve[m][piece] + k];
                    const f32 value = solve.weights[row];
                    if (value > kSolvedFloor) {
                        weights.push_back({solve.bone, value});
                        sum += value;
                    }
                }
                for (geom::Influence& influence : weights) {
                    influence.weight /= sum;
                }
            }
            if (weights.empty() || system.matrices[piece].diagonal.empty()) {
                ++out.unreached;
            }
            if (weights.empty()) {
                // No heat reached it: §8.1's fallback, rigid to the nearest.
                const u32 bone = NearestBone(segments, PositionOf(system.positions,
                                                                  *system.points, point));
                if (bone == kInvalidNode) {
                    continue;
                }
                weights.push_back({bone, 1.0f});
            }
            out.points.push_back(point);
            out.given.add(weights);
        }
        run.meshes.push_back(std::move(out));
    }
    return run;
}

GenerateResult HeatWeights(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                           std::span<const u32> scope, const GenerateOptions& options,
                           const GenerateControl& control) {
    const MeshScope one[1]{{&mesh, &points, scope}};
    return detail::WriteOneMesh(mesh, nodes, points, SolveHeat(one, nodes, options, control),
                                options);
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
