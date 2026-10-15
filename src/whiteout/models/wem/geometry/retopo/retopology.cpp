// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/retopo/retopology.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/interpolate.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/geometry/triangulation.h>

#include "cross_field.h"
#include "extract.h"
#include "field_extract.h"
#include "layout.h"
#include "position_field.h"
#include "surface.h"
#include "work_mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

const char* ToString(RetopoReport::Failure failure) {
    switch (failure) {
    case RetopoReport::Failure::None:
        return "none";
    case RetopoReport::Failure::Cancelled:
        return "cancelled";
    case RetopoReport::Failure::EmptyMesh:
        return "empty mesh";
    case RetopoReport::Failure::NotManifold:
        return "not manifold";
    case RetopoReport::Failure::Unsolved:
        return "unsolved";
    }
    return "unknown";
}

namespace {

/// The fewest work triangles a piece is refined to, whatever the quad size:
/// tracing needs room to keep separatrices apart.
constexpr f64 kMinimumTriangles = 2500.0;
/// And the most, so a dense target cannot run away.
constexpr f64 kMaximumTriangles = 250000.0;
/// What an arc pays for every unit under one quad (§1.7). Every bound is zero,
/// so no solver is ever handed a problem without an answer -- libSatsuma would
/// throw on one -- and "at least one" is this price instead: one an inconsistent
/// layout can still pay, and the validity loop then sees.
constexpr f64 kKeepArc = 1000.0;
/// What a collapse costs where collapsing is allowed: about a quad of error.
constexpr f64 kCollapseArc = 0.5;
/// What opposite sides of a patch pay, squared, for each quad they differ by:
/// dearer than stretching arcs to match, cheaper than an arc kept at zero.
constexpr f64 kMismatch = 4.0;

/// The field method's pure quads come out uneven past this median worst-corner
/// turn from square (25 degrees), and the unsplit lattice must beat them by
/// this (5 degrees) to be taken instead.
constexpr f64 kUneven = 0.436;
constexpr f64 kBetter = 0.087;

/// A piece of fewer of the pure lattice's cells than this is solved unsplit:
/// below it the corpus's pieces lost a tenth of their area and more to it.
constexpr f64 kFewCells = 60.0;
/// Where neither lattice holds a piece, finer ones at these shares of the quad
/// edge, the nearer first: each step costs the piece's share more quads.
constexpr f64 kFinerLattices[2] = {0.8, 0.6};

/// What a piece's result may get wrong and still be taken (§9, third pass):
/// a folded face draws black, a lost stretch is a hole in the silhouette and
/// a face across a seam smears the texture. The tries are taken in order of
/// evenness, the first within the bar; where none is, the best within twice
/// it or so; where none is that either, the piece stays as it was.
struct Bar {
    f64 folded;   ///< Share of the faces.
    f64 lost;     ///< Share of the source farther than half a quad edge...
    f64 lostFar;  ///< ...and than a whole one: a strap or a blade gone.
    f64 straddle; ///< Share of the faces with a corner inside another island.
    f64 others;   ///< Share of the faces that are no quads: a layout's fills.
    /// Share of the faces laid over a strip of texture narrower than a quad,
    /// a decal's outline or a trim, which smear it: no quad that size fits it.
    f64 spans;
};
constexpr Bar kStrict{0.01, 0.02, 0.005, 0.005, 0.25, 0.05};
constexpr Bar kLenient{0.03, 0.04, 0.01, 0.01, 0.4, 0.1};

/// The shares of @p triangles' source area whose centres lie farther than
/// @p near, and than @p far, from every face of @p quads.
void LostShares(const Surface& surface, std::span<const u32> triangles, const QuadMesh& quads, f64 near, f64 far,
                f64& lostNear, f64& lostFar) {
    const f64 reach = far;
    // The faces as fans of triangles, in a grid of `reach` cells.
    struct Cell {
        i64 x, y, z;
        bool operator==(const Cell& o) const {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct CellHash {
        std::size_t operator()(const Cell& c) const {
            return static_cast<std::size_t>(c.x * 73856093) ^ static_cast<std::size_t>(c.y * 19349663) ^
                   static_cast<std::size_t>(c.z * 83492791);
        }
    };
    auto cellOf = [&](const V3& p) {
        return Cell{static_cast<i64>(std::floor(p.x / reach)), static_cast<i64>(std::floor(p.y / reach)),
                    static_cast<i64>(std::floor(p.z / reach))};
    };
    std::vector<std::array<u32, 3>> fans;
    std::unordered_map<Cell, std::vector<u32>, CellHash> grid;
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        const std::span<const u32> face = quads.face(f);
        for (std::size_t k = 1; k + 1 < face.size(); ++k) {
            const std::array<u32, 3> t = {face[0], face[k], face[k + 1]};
            V3 lo = quads.positions[t[0]];
            V3 hi = lo;
            for (const u32 v : t) {
                const V3& p = quads.positions[v];
                lo = V3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = V3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            const Cell a = cellOf(lo);
            const Cell b = cellOf(hi);
            const u32 id = static_cast<u32>(fans.size());
            fans.push_back(t);
            for (i64 x = a.x; x <= b.x; ++x) {
                for (i64 y = a.y; y <= b.y; ++y) {
                    for (i64 z = a.z; z <= b.z; ++z) {
                        grid[Cell{x, y, z}].push_back(id);
                    }
                }
            }
        }
    }
    f64 total = 0.0;
    f64 farOff = 0.0;
    f64 nearOff = 0.0;
    for (const u32 t : triangles) {
        const V3 centre = (surface.positions[surface.corners[3 * t]] + surface.positions[surface.corners[3 * t + 1]] +
                           surface.positions[surface.corners[3 * t + 2]]) *
                          (1.0 / 3.0);
        total += surface.triangleAreas[t];
        f64 nearest = std::numeric_limits<f64>::infinity();
        const Cell c = cellOf(centre);
        for (i64 x = c.x - 1; x <= c.x + 1 && nearest > near; ++x) {
            for (i64 y = c.y - 1; y <= c.y + 1 && nearest > near; ++y) {
                for (i64 z = c.z - 1; z <= c.z + 1 && nearest > near; ++z) {
                    const auto it = grid.find(Cell{x, y, z});
                    if (it == grid.end()) {
                        continue;
                    }
                    for (const u32 id : it->second) {
                        const std::array<u32, 3>& tri = fans[id];
                        const Nearest n = NearestOnTriangle(centre, quads.positions[tri[0]], quads.positions[tri[1]],
                                                            quads.positions[tri[2]]);
                        nearest = std::min(nearest, n.distance);
                        if (nearest <= near) {
                            break;
                        }
                    }
                }
            }
        }
        nearOff += nearest > near ? surface.triangleAreas[t] : 0.0;
        farOff += nearest > far ? surface.triangleAreas[t] : 0.0;
    }
    lostNear = total > 0.0 ? nearOff / total : 0.0;
    lostFar = total > 0.0 ? farOff / total : 0.0;
}

/// Parts of @p quads: faces joined through their corners.
u32 PartsOf(const QuadMesh& quads) {
    std::vector<u32> parent(quads.vertexCount());
    for (u32 v = 0; v < parent.size(); ++v) {
        parent[v] = v;
    }
    auto root = [&](u32 v) {
        while (parent[v] != v) {
            parent[v] = parent[parent[v]];
            v = parent[v];
        }
        return v;
    };
    std::vector<u8> used(quads.vertexCount(), 0);
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        const std::span<const u32> face = quads.face(f);
        for (const u32 v : face) {
            used[v] = 1;
            parent[root(v)] = root(face[0]);
        }
    }
    u32 parts = 0;
    for (u32 v = 0; v < parent.size(); ++v) {
        parts += used[v] && root(v) == v ? 1 : 0;
    }
    return parts;
}

/// V - E + F over the faces' vertices and sides.
i64 EulerOf(const QuadMesh& quads) {
    std::vector<u8> used(quads.vertexCount(), 0);
    std::unordered_map<u64, u8> sides;
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        const std::span<const u32> face = quads.face(f);
        for (std::size_t i = 0; i < face.size(); ++i) {
            const u32 a = face[i];
            const u32 b = face[(i + 1) % face.size()];
            used[a] = 1;
            sides[(static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b)] = 1;
        }
    }
    i64 vertices = 0;
    for (const u8 u : used) {
        vertices += u;
    }
    return vertices - static_cast<i64>(sides.size()) + quads.faceCount();
}

/// Over the quads, the median of each one's worst corner turn from square.
f64 MedianCornerTurn(const QuadMesh& quads) {
    std::vector<f64> worst;
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        const std::span<const u32> face = quads.face(f);
        if (face.size() != 4) {
            continue;
        }
        f64 w = 0.0;
        for (u32 i = 0; i < 4; ++i) {
            const V3& p = quads.positions[face[i]];
            w = std::max(w, std::abs(Angle(quads.positions[face[(i + 1) % 4]] - p, quads.positions[face[(i + 3) % 4]] - p) -
                                     0.5 * kPi));
        }
        worst.push_back(w);
    }
    if (worst.empty()) {
        return 0.0;
    }
    std::nth_element(worst.begin(), worst.begin() + worst.size() / 2, worst.end());
    return worst[worst.size() / 2];
}

/// How a try at a piece holds its source: what `CheckQuads`, which reads only
/// how the faces join, cannot see.
struct Assessment {
    bool valid = false; ///< Manifold and oriented, one part, the source piece's Euler characteristic.
    u32 faces = 0;
    u32 degenerate = 0; ///< Faces the mesh build would drop: holes.
    u32 folded = 0;     ///< Faces turned against the surface.
    u32 straddling = 0; ///< Faces with a corner inside another region: across a seam.
    u32 others = 0;     ///< Faces of other than four corners.
    u32 spanning = 0;   ///< Faces with a corner on a seam beyond a strip of another region.
    bool blown = false; ///< Twice the faces the piece had triangles, and far over its share: no simplification.
    f64 lost = 0.0;     ///< The source piece's share farther than half a quad edge from them.
    f64 lostFar = 0.0;  ///< And farther than a whole one.
    f64 turn = 0.0;     ///< `MedianCornerTurn`.

    bool holds(const Bar& bar = kStrict) const {
        const f64 n = static_cast<f64>(faces);
        return valid && !blown && degenerate == 0 && folded <= bar.folded * n && straddling <= bar.straddle * n &&
               others <= bar.others * n && spanning <= bar.spans * n && lost <= bar.lost && lostFar <= bar.lostFar;
    }
    /// How far it misses, for choosing among tries that miss the strict bar.
    f64 shortfall() const {
        const f64 n = std::max<f64>(faces, 1.0);
        return folded / n / kStrict.folded + lost / kStrict.lost + lostFar / kStrict.lostFar +
               straddling / n / kStrict.straddle + spanning / n / kStrict.spans;
    }
};

/// The best try at a piece that misses the strict bar but not the lenient one.
struct Fallback {
    QuadMesh quads;
    Assessment assessment;
    bool any = false;
    u32 from = 0; ///< 0 the field, 1 the layout.

    /// The field's tries before the layout's, as for the strict bar.
    void offer(const QuadMesh& q, const Assessment& a, u32 method) {
        if (a.holds(kLenient) &&
            (!any || method < from || (method == from && a.shortfall() < assessment.shortfall()))) {
            quads = q;
            assessment = a;
            any = true;
            from = method;
        }
    }
};

/// One connected piece of the surface, through the pipeline.
struct Piece {
    u32 component = kNone;
    f64 area = 0.0;
    f64 share = 0.0;   ///< Its share of the quads asked for, by area.
    u32 triangles = 0; ///< Source triangles.
    std::vector<u32> sourceTriangles;
    i64 euler = 0;     ///< The source piece's Euler characteristic.
    WorkMesh mesh;
    CrossField field;
    Layout layout;
    std::vector<f64> penalties; ///< Per arc: what each unit under one costs.
    /// Per arc, its segments' lengths and how far the authored normals turn
    /// along each, for an adaptive size.
    std::vector<std::vector<std::pair<f64, f64>>> bends;
    std::vector<i32> lengths;
    f64 cost = 0.0; ///< The last quantization's.
    QuadMesh quads;
    ExtractStats stats;
    bool solved = false;
    bool valid = false;
    u32 rounds = 0;
    QuadValidity validity; ///< The last extraction's, for the note when it fails.
    PositionField frames;  ///< The field method's frames, before any lattice.
    f64 workEdge = 0.0;    ///< The field method's work mesh edge.
    bool split = false;    ///< The field method's lattice at twice the edge, split in four.
    FieldExtractStats fieldStats;
};

/// A vertex of the output, wherever it came from.
struct OutVertex {
    V3 position{0.0, 0.0, 0.0};
    SurfacePoint home;
    VertexKind kind = VertexKind::Free;
    FeaturePoint onCurve;
    u32 curve = kNone;
};

class Retopology {
public:
    Retopology(const Mesh& source, const RetopoOptions& options, const QuantizeSolver& solver,
               const RetopoControl& control)
        : source_(source), options_(options), solver_(solver), control_(control) {}

    RetopoResult run();

private:
    bool stopped() {
        if (control_.stopped()) {
            report_.failure = RetopoReport::Failure::Cancelled;
            return true;
        }
        return false;
    }
    void progress(u32 done, u32 total) {
        if (control_.progress) {
            control_.progress(done, total);
        }
    }

    void preparePiece(Piece& piece);
    QuantizeSolution solve(const QuantizeProblem& problem) const {
        return solver_ ? solver_(problem) : SolveQuantizeDoubleCover(problem);
    }
    /// Quantizes @p piece at quad edge @p edge with its current bounds.
    bool quantize(Piece& piece, f64 edge);
    u64 quadCount(const Piece& piece) const;
    void extract(Piece& piece, f64 edge);
    /// The layout method over every piece (§1.6-§1.8).
    void runLayout(std::vector<Piece>& pieces, f64 edge, f64 quads, f64 remeshedArea);
    /// One piece by the layout alone at quad edge @p edge: the field
    /// method's fallback. Returns whether its quads hold the piece.
    bool layoutAlone(Piece& piece, f64 edge, Assessment& assessment, Fallback& fallback);
    /// Prepares and assesses @p piece's layout quads, just extracted; where
    /// they do not hold and arcs may collapse, extracts them again with every
    /// arc kept. Returns whether the quads hold the piece; each try is
    /// offered to @p fallback.
    bool holdLayout(Piece& piece, f64 edge, Assessment& assessment, Fallback& fallback);

    /// Readies a try as it would be taken -- doublets out, relaxed on the
    /// source -- and returns its connectivity's verdict. Relaxing moves
    /// points, so it comes before the checks that read where they are.
    QuadValidity prepare(QuadMesh& quads) const;
    Assessment assess(const Piece& piece, const QuadMesh& quads, const QuadValidity& validity) const;

    // --- the field method (§6) ---
    void prepareFieldPiece(Piece& piece, f64 lattice);
    /// Solves the lattice at @p lattice and extracts it, every face split in
    /// four with @p split; returns the faces.
    u64 solveField(Piece& piece, f64 lattice, bool split);
    /// The lattice at the quad edge @p edge, unsplit, the work mesh refined
    /// for it first where it is too coarse.
    void solveFine(Piece& piece, f64 edge);
    void runField(std::vector<Piece>& pieces, f64 edge, f64 quads, f64 remeshedArea);

    /// Adds @p piece's prepared quads, unless they came out far over its share
    /// of the count; then its source is kept. Returns whether added.
    bool finishPiece(Piece& piece, f64 share, const Assessment& assessment);
    /// Keeps @p piece as it was because no try held it.
    void failPiece(const Piece& piece);

    // --- the output ---
    u32 addVertex(const OutVertex& vertex) {
        vertices_.push_back(vertex);
        return static_cast<u32>(vertices_.size() - 1);
    }
    void addPieceQuads(const Piece& piece);
    void keepComponent(u32 component);
    Mesh build();
    /// @p vertex's home re-expressed in the triangle holding the same point on
    /// the side of any seam that @p face (its face's home) is on.
    SurfacePoint homeIn(const SurfacePoint& home, const SurfacePoint& face) const;
    /// The source edge of the feature output vertices @p a and @p b run along,
    /// or none.
    HalfedgeId alongFeature(u32 a, u32 b) const;
    const SourcePolygon& polygon(u32 face);

    const Mesh& source_;
    RetopoOptions options_;
    const QuantizeSolver& solver_;
    const RetopoControl& control_;
    RetopoReport report_;
    Mesh welded_;
    Surface surface_;
    std::vector<OutVertex> vertices_;
    std::vector<std::vector<u32>> faces_;
    std::vector<u32> faceRegions_; ///< Per face, the source region it lies in; kNone where unknown.
    std::unordered_map<u32, SourcePolygon> polygons_;
    mutable std::vector<u32> seen_; ///< `homeIn`'s walk, by stamp.
    mutable u32 seenStamp_ = 0;
    u32 steps_ = 2; ///< Progress steps: each piece prepared, the count, each extracted, the build.
};

void Retopology::preparePiece(Piece& piece) {
    piece.mesh = SeedWorkMesh(surface_, piece.component);
    // The layout is traced at a third of the quad edge, never coarser than a
    // few thousand triangles for the piece nor finer than a cap.
    const f64 edge = report_.targetEdge;
    const f64 perTriangle = std::sqrt(3.0) / 4.0;
    const f64 coarsest = std::sqrt(piece.area / (perTriangle * kMinimumTriangles));
    const f64 finest = std::sqrt(piece.area / (perTriangle * kMaximumTriangles));
    const f64 length = std::clamp(edge / 3.0, finest, std::max(finest, coarsest));
    Remesh(piece.mesh, surface_, RemeshOptions{length, 6, 0.5});
    SplitFeatureCorners(piece.mesh, surface_);
    piece.mesh.compact();
    if (stopped()) {
        return;
    }
    FieldOptions field;
    field.scale = edge;
    piece.field = SolveCrossField(piece.mesh, surface_, field);
    if (stopped()) {
        return;
    }
    LayoutOptions layout;
    layout.chord = 2.0 * length;
    piece.layout = BuildLayout(piece.mesh, piece.field, surface_, layout);
    piece.penalties.assign(piece.layout.arcs.size(), options_.collapseArcs ? kCollapseArc : kKeepArc);
    if (options_.adaptivity > 0.0f) {
        piece.bends.resize(piece.layout.arcs.size());
        for (u32 a = 0; a < piece.layout.arcs.size(); ++a) {
            const std::vector<u32>& vertices = piece.layout.arcs[a].vertices;
            for (std::size_t k = 1; k < vertices.size(); ++k) {
                const u32 u = vertices[k - 1];
                const u32 v = vertices[k];
                const f64 turn = Angle(surface_.normal(piece.mesh.homes[u]), surface_.normal(piece.mesh.homes[v]));
                piece.bends[a].push_back({Distance(piece.mesh.positions[u], piece.mesh.positions[v]), turn});
            }
        }
    }
    report_.singularities += piece.field.singularityCount();
    report_.patches += static_cast<u32>(piece.layout.patches.size());
    report_.patchesUnfilled += piece.layout.stats.unfilled;
}

bool Retopology::quantize(Piece& piece, f64 edge) {
    std::vector<f64> targets;
    targets.reserve(piece.layout.arcs.size());
    for (u32 a = 0; a < piece.layout.arcs.size(); ++a) {
        if (piece.bends.empty()) {
            targets.push_back(piece.layout.arcs[a].length / edge);
            continue;
        }
        // Adaptive: each stretch counts for more where the surface turns, by
        // how far it turns over a quad edge; the count loop rescales the rest.
        f64 target = 0.0;
        for (const auto& [length, turn] : piece.bends[a]) {
            const f64 bend = length > 0.0 ? std::min(turn / length * edge, 1.5) : 0.0;
            target += length * (1.0 + 2.0 * options_.adaptivity * bend) / edge;
        }
        targets.push_back(target);
    }
    QuantizeProblem problem = BuildQuantizeProblem(piece.layout, targets, 0, kMismatch);
    for (u32 a = 0; a < piece.penalties.size(); ++a) {
        // An arc two quads long or more never folds: a blade or a strap that
        // wide would go with it.
        problem.arcs[a].zeroPenalty = piece.layout.arcs[a].length >= 2.0 * edge ? kKeepArc : piece.penalties[a];
    }
    const QuantizeSolution solution = solve(problem);
    piece.solved = solution.solved;
    if (solution.solved) {
        piece.lengths.assign(solution.lengths.begin(), solution.lengths.begin() + piece.layout.arcs.size());
        piece.cost = solution.cost;
    }
    return solution.solved;
}

u64 Retopology::quadCount(const Piece& piece) const {
    u64 count = 0;
    for (const LayoutPatch& patch : piece.layout.patches) {
        if (!patch.rectangle) {
            continue;
        }
        i64 side[4] = {0, 0, 0, 0};
        for (u32 s = 0; s < 4; ++s) {
            for (const SideArc& run : patch.sides[s]) {
                side[s] += piece.lengths[run.arc];
            }
        }
        count += static_cast<u64>(std::max(side[0], side[2]) * std::max(side[1], side[3]));
    }
    return count;
}

void Retopology::extract(Piece& piece, f64 edge) {
    // Collapse, look, and raise the penalty of every collapsed arc that made a
    // bad face, until the quads are a manifold or nothing is left to raise.
    const u64 wanted = quadCount(piece);
    for (u32 round = 0; round < 12; ++round) {
        if (round > 0 && (stopped() || !quantize(piece, edge))) {
            break;
        }
        // An arc kept costs the quads round it: the edge grows to keep the
        // piece's count where it was.
        for (u32 again = 0; round > 0 && again < 3 && wanted > 0; ++again) {
            const f64 ratio = static_cast<f64>(quadCount(piece)) / static_cast<f64>(wanted);
            if (ratio < 1.1 || stopped()) {
                break;
            }
            edge *= std::sqrt(ratio);
            if (!quantize(piece, edge)) {
                break;
            }
        }
        piece.stats = {};
        std::vector<u32> zeroArcVertex;
        piece.quads = ExtractQuads(piece.mesh, surface_, piece.layout, piece.lengths, piece.stats, zeroArcVertex);
        RemoveDoublets(piece.quads);
        piece.rounds = round;
        piece.validity = CheckQuads(piece.quads);
        const QuadValidity& validity = piece.validity;
        // Every strip folded away is no answer either.
        if (validity.ok() && piece.quads.faceCount() > 0) {
            piece.valid = true;
            return;
        }
        std::unordered_map<u32, u8> bad;
        for (u32 v : validity.badVertices) {
            bad[v] = 1;
        }
        u32 raised = 0;
        for (u32 a = 0; a < zeroArcVertex.size(); ++a) {
            if (zeroArcVertex[a] != kNone && bad.count(zeroArcVertex[a]) && piece.penalties[a] < kKeepArc) {
                piece.penalties[a] = kKeepArc;
                ++raised;
            }
        }
        if (raised == 0) {
            // Nothing collapsed made it: forbid every collapse, once.
            for (u32 a = 0; a < zeroArcVertex.size(); ++a) {
                if (zeroArcVertex[a] != kNone && piece.penalties[a] < kKeepArc) {
                    piece.penalties[a] = kKeepArc;
                    ++raised;
                }
            }
        }
        if (raised == 0) {
            break;
        }
    }
    piece.valid = false;
}

void Retopology::addPieceQuads(const Piece& piece) {
    const QuadMesh& q = piece.quads;
    const u32 base = static_cast<u32>(vertices_.size());
    for (u32 v = 0; v < q.vertexCount(); ++v) {
        addVertex(OutVertex{q.positions[v], q.homes[v], q.kinds[v], q.onCurve[v], q.curve[v]});
    }
    for (u32 f = 0; f < q.faceCount(); ++f) {
        std::vector<u32> corners;
        for (u32 v : q.face(f)) {
            corners.push_back(base + v);
        }
        faces_.push_back(std::move(corners));
        faceRegions_.push_back(f < q.faceRegion.size() ? q.faceRegion[f] : kNone);
    }
}

void Retopology::keepComponent(u32 component) {
    // As it was: its own vertices and faces, every corner reading itself.
    std::unordered_map<u32, u32> vertexOf;
    std::unordered_map<u32, u8> faceSeen;
    const Topology& topology = welded_.topology();
    for (u32 t = 0; t < surface_.triangleCount(); ++t) {
        if (surface_.components[t] != component || faceSeen.count(surface_.faces[t])) {
            continue;
        }
        const u32 face = surface_.faces[t];
        faceSeen[face] = 1;
        std::vector<u32> corners;
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            const u32 v = topology.from(h).value();
            auto it = vertexOf.find(v);
            if (it == vertexOf.end()) {
                OutVertex vertex;
                vertex.position = surface_.positions[v];
                vertex.home = surface_.atVertex(v);
                vertex.kind = VertexKind::Corner;
                it = vertexOf.emplace(v, addVertex(vertex)).first;
            }
            corners.push_back(it->second);
        }
        faces_.push_back(std::move(corners));
        faceRegions_.push_back(surface_.regions[t]);
    }
    ++report_.piecesKept;
}

SurfacePoint Retopology::homeIn(const SurfacePoint& home, const SurfacePoint& face) const {
    if (!home.valid() || !face.valid()) {
        return home;
    }
    const u32 t = home.triangle;
    std::vector<u32> candidates;
    const f64 w[3] = {home.weight(0), home.weight(1), home.weight(2)};
    for (u32 i = 0; i < 3; ++i) {
        // On a vertex: its whole fan; on an edge: the triangle across it.
        if (w[i] > 1.0 - 1e-9) {
            surface_.fan(surface_.corners[3 * t + i], candidates);
        }
    }
    for (u32 i = 0; i < 3; ++i) {
        const u32 opposite = 3 * t + (i + 1) % 3; // the edge without corner i
        if (std::abs(w[i]) < 1e-9 && surface_.twins[opposite] != kNone) {
            candidates.push_back(surface_.twins[opposite] / 3);
        }
    }
    if (candidates.empty()) {
        return home; // inside its triangle: one side only
    }
    candidates.push_back(t);
    // The face's side is the one its own home reaches without crossing a
    // feature. A region cannot tell: a seam that does not cut the surface
    // apart (a sleeve's) has the same region on both sides.
    constexpr std::size_t kReach = 16384;
    if (seen_.size() != surface_.triangleCount() || ++seenStamp_ == 0) {
        seen_.assign(surface_.triangleCount(), 0);
        seenStamp_ = 1;
    }
    u32 found = kNone;
    std::vector<u32> queue{face.triangle};
    seen_[face.triangle] = seenStamp_;
    for (std::size_t at = 0; at < queue.size() && at < kReach; ++at) {
        const u32 x = queue[at];
        if (std::find(candidates.begin(), candidates.end(), x) != candidates.end()) {
            found = x;
            break;
        }
        for (u32 i = 0; i < 3; ++i) {
            const u32 twin = surface_.twins[3 * x + i];
            if (twin != kNone && !surface_.isFeature(3 * x + i) && seen_[twin / 3] != seenStamp_) {
                seen_[twin / 3] = seenStamp_;
                queue.push_back(twin / 3);
            }
        }
    }
    if (found == kNone) {
        // Not reached nearby, as a face across a seam is not: its region's.
        const u32 region = surface_.regions[face.triangle];
        for (const u32 c : candidates) {
            if (surface_.regions[c] == region) {
                found = c;
                break;
            }
        }
    }
    if (found == kNone || found == t) {
        return home;
    }
    const Nearest n = NearestOnTriangle(surface_.position(home), surface_.positions[surface_.corners[3 * found]],
                                        surface_.positions[surface_.corners[3 * found + 1]],
                                        surface_.positions[surface_.corners[3 * found + 2]]);
    return Surface::Make(found, n.w0, n.w1, n.w2);
}

const SourcePolygon& Retopology::polygon(u32 face) {
    auto it = polygons_.find(face);
    if (it == polygons_.end()) {
        it = polygons_.emplace(face, CapturePolygon(welded_, FaceId(face))).first;
    }
    return it->second;
}

HalfedgeId Retopology::alongFeature(u32 a, u32 b) const {
    if (vertices_[a].kind == VertexKind::Free || vertices_[b].kind == VertexKind::Free) {
        return {};
    }
    const V3 middle = Lerp(vertices_[a].position, vertices_[b].position, 0.5);
    const f64 length = Distance(vertices_[a].position, vertices_[b].position);
    // Starts on the curves the ends are on.
    std::vector<FeaturePoint> starts;
    for (u32 end : {a, b}) {
        if (vertices_[end].kind == VertexKind::Feature && vertices_[end].onCurve.valid()) {
            starts.push_back(vertices_[end].onCurve);
        } else if (vertices_[end].kind == VertexKind::Corner) {
            const SurfacePoint& home = vertices_[end].home;
            for (u32 i = 0; i < 3; ++i) {
                if (home.weight(i) > 1.0 - 1e-9) {
                    const u32 sv = surface_.corners[3 * home.triangle + i];
                    for (u32 fh : surface_.featureEdges(sv)) {
                        starts.push_back(FeaturePoint{fh, surface_.from(fh) == sv ? 0.0 : 1.0});
                    }
                }
            }
        }
    }
    FeaturePoint found;
    f64 nearest = std::numeric_limits<f64>::infinity();
    for (const FeaturePoint& start : starts) {
        const FeaturePoint at = surface_.locateOnCurve(start, middle, length);
        const f64 d = Distance(surface_.position(at), middle);
        if (d < nearest) {
            nearest = d;
            found = at;
        }
    }
    if (!found.valid() || nearest > 0.25 * length + 1e-9) {
        return {};
    }
    const u32 sh = found.halfedge;
    return welded_.topology().findHalfedge(VertexId(surface_.from(sh)), VertexId(surface_.to(sh)));
}

Mesh Retopology::build() {
    const Topology& sourceTopology = welded_.topology();
    const std::span<const u32> sections = welded_.faceSections();

    // Each face's home: its centroid on the surface, from its corners' homes.
    std::vector<SurfacePoint> faceHome(faces_.size());
    for (std::size_t f = 0; f < faces_.size(); ++f) {
        V3 centroid{0.0, 0.0, 0.0};
        std::vector<u32> starts;
        for (u32 v : faces_[f]) {
            centroid = centroid + vertices_[v].position;
            starts.push_back(vertices_[v].home.triangle);
        }
        centroid = centroid * (1.0 / static_cast<f64>(faces_[f].size()));
        f64 reach = 0.0;
        LocateFilter filter{faceRegions_[f], V3{0.0, 0.0, 0.0}};
        u32 homed = 0;
        for (u32 v : faces_[f]) {
            reach = std::max(reach, Distance(centroid, vertices_[v].position));
            if (vertices_[v].home.valid()) {
                filter.normal = filter.normal + surface_.normal(vertices_[v].home);
                ++homed;
            }
        }
        filter.normal = AgreedNormal(filter.normal, homed);
        faceHome[f] = surface_.locate(starts, centroid, reach + 1e-12, filter);
        if (!faceHome[f].valid()) {
            faceHome[f] = surface_.locate(starts, centroid, reach + 1e-12);
        }
    }

    MeshBuilder builder;
    for (const MeshSection& section : welded_.sections) {
        builder.addSection(section);
    }
    for (const OutVertex& vertex : vertices_) {
        builder.addVertex(ToV3f(vertex.position));
    }
    for (std::size_t f = 0; f < faces_.size(); ++f) {
        std::vector<VertexId> corners;
        for (u32 v : faces_[f]) {
            corners.push_back(VertexId(v));
        }
        const u32 sourceFace = faceHome[f].valid() ? surface_.faces[faceHome[f].triangle] : 0;
        builder.addFace(corners, sourceFace < sections.size() ? sections[sourceFace] : 0);
    }
    MeshBuilder::BuildOutcome outcome = builder.build();
    Mesh out = std::move(outcome.mesh);
    if (outcome.refused || !out.hasConnectivity()) {
        report_.notes.push_back("the output would not build");
        return out;
    }
    out.name = source_.name;
    out.lodLevel = source_.lodLevel;
    const Topology& topology = out.topology();
    // Faces come out in input order less any the repair dropped.
    std::vector<u32> outputFace;
    {
        std::size_t dropped = 0;
        const auto& droppedFaces = out.repairLog.droppedFaces;
        for (u32 f = 0; f < faces_.size(); ++f) {
            if (dropped < droppedFaces.size() && droppedFaces[dropped].index == f) {
                ++dropped;
                continue;
            }
            outputFace.push_back(f);
        }
        if (outputFace.size() != topology.faceCount()) {
            report_.notes.push_back("faces were lost building the output");
            outputFace.resize(std::min<std::size_t>(outputFace.size(), topology.faceCount()));
        }
    }

    // A vertex the build split where faces pinch it reads the one it was
    // split from.
    std::vector<u32> outOf(out.vertexCount());
    for (u32 v = 0; v < outOf.size(); ++v) {
        outOf[v] = v;
    }
    for (const VertexSplit& split : out.repairLog.splits) {
        if (split.created < outOf.size() && split.original < outOf.size()) {
            outOf[split.created] = outOf[split.original];
        }
    }

    // Layers: every one the source has, made on the output. A cloth binding
    // names source vertices, which the output has none of: it goes, its
    // weights with it.
    for (const AttrLayer& layer : welded_.attributes.layers()) {
        if (layer.name == names::kPosition || layer.name == names::kMergeGroup ||
            layer.name == names::kSection || IsVertexReferenceLayer(layer) ||
            layer.name == names::kClothBindWeight) {
            continue;
        }
        AttrLayer& made = out.attributes.create(layer.name, layer.domain, layer.type, layer.storage);
        // One value for the mesh, such as how its normals are weighted.
        if (layer.domain == Domain::Mesh && made.data.size() == layer.data.size()) {
            made.data = layer.data;
        }
    }

    // Corners: each from its own face's side of any seam.
    for (u32 slot = 0; slot < outputFace.size(); ++slot) {
        const u32 f = outputFace[slot];
        if (!faceHome[f].valid()) {
            continue;
        }
        for (const HalfedgeId h : topology.fh(FaceId(slot))) {
            const u32 v = outOf[topology.from(h).value()];
            if (v >= vertices_.size()) {
                continue;
            }
            const SurfacePoint home = homeIn(vertices_[v].home, faceHome[f]);
            if (!home.valid()) {
                continue;
            }
            const u32 t = home.triangle;
            const SourcePolygon& source = polygon(surface_.faces[t]);
            std::vector<f32> weights(source.cornerCount(), 0.0f);
            for (u32 i = 0; i < 3; ++i) {
                const u32 sv = surface_.corners[3 * t + i];
                for (u32 c = 0; c < source.cornerCount(); ++c) {
                    if (source.vertices[c] == sv) {
                        weights[c] += static_cast<f32>(home.weight(i));
                    }
                }
            }
            const HalfedgeId target[1] = {h};
            BlendCorners(out, source, weights, target);
        }
    }

    // Vertices: skin blended at the home, every other layer the heaviest
    // corner's.
    const bool skinned = !welded_.skin.empty();
    if (skinned) {
        out.skin.reset(out.vertexCount());
    }
    for (u32 v = 0; v < out.vertexCount(); ++v) {
        if (outOf[v] >= vertices_.size() || !vertices_[outOf[v]].home.valid()) {
            continue;
        }
        const SurfacePoint& home = vertices_[outOf[v]].home;
        const u32 t = home.triangle;
        u32 heaviest = surface_.corners[3 * t];
        f64 weight = -1.0;
        std::vector<std::vector<Influence>> skins(3);
        f32 weights[3];
        for (u32 i = 0; i < 3; ++i) {
            const u32 sv = surface_.corners[3 * t + i];
            weights[i] = static_cast<f32>(home.weight(i));
            if (home.weight(i) > weight) {
                weight = home.weight(i);
                heaviest = sv;
            }
            if (skinned) {
                const std::span<const Influence> list = welded_.skin.forVertex(sv);
                skins[i].assign(list.begin(), list.end());
            }
        }
        if (skinned) {
            const std::vector<Influence> blended = BlendInfluences(skins, weights);
            out.skin.assignVertex(v, blended);
        }
        for (const AttrLayer& layer : welded_.attributes.layers()) {
            if (layer.domain != Domain::Vertex || layer.name == names::kPosition ||
                layer.name == names::kMergeGroup) {
                continue;
            }
            AttrLayer* target = out.attributes.layer(layer.name, Domain::Vertex);
            const u32 stride = AttrTypeSize(layer.type);
            if (target != nullptr && (heaviest + 1) * stride <= layer.data.size() &&
                (v + 1) * stride <= target->data.size()) {
                std::memcpy(target->data.data() + v * stride, layer.data.data() + heaviest * stride, stride);
            }
        }
    }

    // Faces: every layer but the section, from the home face.
    for (u32 slot = 0; slot < outputFace.size(); ++slot) {
        const SurfacePoint& home = faceHome[outputFace[slot]];
        if (!home.valid()) {
            continue;
        }
        const u32 sourceFace = surface_.faces[home.triangle];
        for (const AttrLayer& layer : welded_.attributes.layers()) {
            if (layer.domain != Domain::Face || layer.name == names::kSection) {
                continue;
            }
            AttrLayer* target = out.attributes.layer(layer.name, Domain::Face);
            const u32 stride = AttrTypeSize(layer.type);
            if (target != nullptr && (sourceFace + 1) * stride <= layer.data.size() &&
                (slot + 1) * stride <= target->data.size()) {
                std::memcpy(target->data.data() + slot * stride, layer.data.data() + sourceFace * stride, stride);
            }
        }
    }

    // Edges: an output edge that runs along a source feature takes that
    // feature edge's marks -- sharp, seam, crease, the UV cuts -- and one
    // between the two ends of a source edge, as a kept piece's are, takes
    // whatever that edge marks.
    auto onSourceVertex = [&](const SurfacePoint& home) {
        for (u32 i = 0; i < 3; ++i) {
            if (home.valid() && home.weight(i) > 1.0 - 1e-9) {
                return surface_.corners[3 * home.triangle + i];
            }
        }
        return kNone;
    };
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const u32 a = outOf[topology.from(h).value()];
        const u32 b = outOf[topology.to(h).value()];
        if (a >= vertices_.size() || b >= vertices_.size()) {
            continue;
        }
        const u32 sa = onSourceVertex(vertices_[a].home);
        const u32 sb = onSourceVertex(vertices_[b].home);
        HalfedgeId wem;
        if (sa != kNone && sb != kNone) {
            wem = sourceTopology.findHalfedge(VertexId(sa), VertexId(sb));
            if (!wem.valid()) {
                wem = sourceTopology.findHalfedge(VertexId(sb), VertexId(sa));
            }
        }
        if (!wem.valid()) {
            wem = alongFeature(a, b);
        }
        if (!wem.valid()) {
            continue;
        }
        const u32 sourceEdge = Topology::edge(wem).value();
        for (const AttrLayer& layer : welded_.attributes.layers()) {
            if (layer.domain != Domain::Edge) {
                continue;
            }
            AttrLayer* target = out.attributes.layer(layer.name, Domain::Edge);
            const u32 stride = AttrTypeSize(layer.type);
            if (target != nullptr && (sourceEdge + 1) * stride <= layer.data.size() &&
                (e + 1) * stride <= target->data.size()) {
                std::memcpy(target->data.data() + e * stride, layer.data.data() + sourceEdge * stride, stride);
            }
        }
    }

    // Prepared for modelling already: one point per vertex, welded for good.
    out.attributes.getOrCreate<u8>(names::kModelled, Domain::Mesh, AttrType::Bool)[0] = 1;
    if (out.attributes.has(names::kNormal, Domain::Halfedge)) {
        RecomputeNormals(out, ShadingAngle(out));
    }
    if (out.attributes.has(names::kTangent, Domain::Halfedge)) {
        RecomputeTangents(out, 0);
    }
    MaterialiseRows(out);
    out.recomputeBounds();
    return out;
}

bool Retopology::finishPiece(Piece& piece, f64 share, const Assessment& assessment) {
    // Twice the faces it had triangles and far over its share of the count:
    // nothing simplified it (each facet of a low-poly part wants patches of
    // its own), and the source is the better mesh.
    if (piece.quads.faceCount() > std::max(2.0 * piece.triangles, 4.0 * share)) {
        report_.notes.push_back("a piece would have come out with twice the faces it had triangles and was "
                                "kept as it was");
        keepComponent(piece.component);
        return false;
    }
    addPieceQuads(piece);
    ++report_.piecesRemeshed;
    report_.worstLost = std::max(report_.worstLost, assessment.lost);
    return true;
}

void Retopology::failPiece(const Piece& piece) {
    keepComponent(piece.component);
    ++report_.piecesFailed;
}

QuadValidity Retopology::prepare(QuadMesh& quads) const {
    RemoveDoublets(quads);
    const QuadValidity validity = CheckQuads(quads);
    if (validity.ok() && quads.faceCount() > 0) {
        CompactQuads(quads);
        RelaxQuads(quads, surface_, options_.relaxIterations);
        UnfoldQuads(quads, surface_, 6);
        if (RepairDegenerate(quads) > 0) {
            CompactQuads(quads);
            return CheckQuads(quads);
        }
    }
    return validity;
}

Assessment Retopology::assess(const Piece& piece, const QuadMesh& quads, const QuadValidity& validity) const {
    Assessment a;
    a.faces = quads.faceCount();
    // Each facet of a low-poly part wanting patches of its own: the source is
    // the better mesh (`finishPiece`).
    a.blown = a.faces > std::max(2.0 * piece.triangles, 4.0 * piece.share);
    a.valid = validity.ok() && a.faces > 0 && EulerOf(quads) == piece.euler && PartsOf(quads) == 1;
    if (!a.valid) {
        return a;
    }
    const bool regions = quads.faceRegion.size() == quads.faceCount();
    // A corner belongs to a face's region when a triangle at it is in it:
    // inside a triangle, its own; on an edge, either side's; on a point, any
    // round it. A corner a collapse moved onto another seam touches none.
    std::vector<u32> fan;
    auto touches = [&](const SurfacePoint& home, u32 region) {
        if (!home.valid() || surface_.regions[home.triangle] == region) {
            return true;
        }
        const u32 t = home.triangle;
        for (u32 i = 0; i < 3; ++i) {
            if (home.weight(i) > 1.0 - 1e-9) {
                surface_.fan(surface_.corners[3 * t + i], fan);
                return std::any_of(fan.begin(), fan.end(), [&](u32 x) { return surface_.regions[x] == region; });
            }
        }
        for (u32 i = 0; i < 3; ++i) {
            const u32 twin = surface_.twins[3 * t + (i + 1) % 3];
            if (home.weight(i) < 1e-9 && twin != kNone && surface_.regions[twin / 3] == region) {
                return true;
            }
        }
        return false;
    };
    std::vector<u32> starts;
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        const std::span<const u32> face = quads.face(f);
        V3 newell{0.0, 0.0, 0.0};
        V3 normal{0.0, 0.0, 0.0};
        V3 centroid{0.0, 0.0, 0.0};
        bool across = false;
        bool spans = false;
        starts.clear();
        for (std::size_t k = 0; k < face.size(); ++k) {
            const SurfacePoint& home = quads.homes[face[k]];
            newell = newell + Cross(quads.positions[face[k]], quads.positions[face[(k + 1) % face.size()]]);
            normal = normal + surface_.normal(home);
            centroid = centroid + quads.positions[face[k]];
            starts.push_back(home.triangle);
            if (regions && quads.faceRegion[f] != kNone && !touches(home, quads.faceRegion[f])) {
                // Inside another triangle: across a seam. On a seam: past a strip.
                const bool inside = home.weight(0) > 1e-9 && home.u > 1e-9 && home.v > 1e-9;
                across = across || inside;
                spans = spans || !inside;
            }
        }
        // A face wrapped round a fold tighter than itself, a hem between a
        // cloth and its lining, has corners facing apart: it is judged by the
        // surface under its middle instead.
        if (Length(AgreedNormal(normal, static_cast<u32>(face.size()))) == 0.0) {
            centroid = centroid * (1.0 / static_cast<f64>(face.size()));
            const SurfacePoint under = surface_.locate(starts, centroid, std::sqrt(Length(newell)) + 1e-12);
            normal = under.valid() ? surface_.normal(under) : normal;
        }
        a.folded += Dot(newell, normal) < 0.0 ? 1 : 0;
        a.straddling += across ? 1 : 0;
        a.spanning += spans && !across ? 1 : 0;
        a.others += face.size() != 4 ? 1 : 0;
        a.degenerate += DegenerateFace(quads, f) ? 1 : 0;
    }
    LostShares(surface_, piece.sourceTriangles, quads, 0.5 * report_.targetEdge, report_.targetEdge, a.lost,
               a.lostFar);
    a.turn = MedianCornerTurn(quads);
    return a;
}

bool Retopology::holdLayout(Piece& piece, f64 edge, Assessment& assessment, Fallback& fallback) {
    if (piece.valid) {
        assessment = assess(piece, piece.quads, prepare(piece.quads));
        if (assessment.holds()) {
            return true;
        }
        fallback.offer(piece.quads, assessment, 1);
    }
    if (!options_.collapseArcs) {
        return false;
    }
    // The collapses folded part of it away, or left no valid quads: every arc
    // kept, at the price of a count over the request.
    report_.notes.push_back("a piece's simplified layout did not hold its shape and was made with every arc kept");
    std::fill(piece.penalties.begin(), piece.penalties.end(), kKeepArc);
    if (stopped() || !quantize(piece, edge)) {
        return false;
    }
    extract(piece, edge);
    report_.validityRounds += piece.rounds;
    if (!piece.valid) {
        return false;
    }
    assessment = assess(piece, piece.quads, prepare(piece.quads));
    fallback.offer(piece.quads, assessment, 1);
    return assessment.holds();
}

void Retopology::runLayout(std::vector<Piece>& pieces, f64 edge, f64 quads, f64 remeshedArea) {
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        if (stopped()) {
            return;
        }
        progress(static_cast<u32>(i), steps_);
        preparePiece(pieces[i]);
    }
    if (stopped()) {
        return;
    }

    // The layout's floor: every arc as short as it may be.
    for (Piece& piece : pieces) {
        if (stopped()) {
            return;
        }
        const std::vector<f64> penalties = piece.penalties;
        std::fill(piece.penalties.begin(), piece.penalties.end(), kKeepArc);
        if (quantize(piece, 1e30)) {
            report_.minimumQuads += static_cast<u32>(quadCount(piece));
        }
        piece.penalties = penalties;
    }

    // The quad edge that lands the count on the request: the layout is fixed,
    // so each guess only re-quantizes.
    for (u32 iteration = 0; iteration < 4; ++iteration) {
        if (stopped()) {
            return;
        }
        u64 count = 0;
        for (Piece& piece : pieces) {
            if (quantize(piece, edge)) {
                count += quadCount(piece);
            }
        }
        if (count == 0) {
            break;
        }
        const f64 ratio = static_cast<f64>(count) / quads;
        if (std::abs(ratio - 1.0) < 0.04) {
            break;
        }
        edge *= std::sqrt(ratio);
    }
    report_.targetEdge = edge;
    progress(static_cast<u32>(pieces.size()), steps_);

    for (std::size_t i = 0; i < pieces.size(); ++i) {
        Piece& piece = pieces[i];
        if (stopped()) {
            return;
        }
        progress(static_cast<u32>(pieces.size() + 1 + i), steps_);
        piece.share = quads * piece.area / remeshedArea;
        if (!quantize(piece, edge)) {
            report_.notes.push_back("a piece found no quantization and was kept as it was");
            failPiece(piece);
            continue;
        }
        extract(piece, edge);
        report_.validityRounds += piece.rounds;
        Assessment assessment;
        Fallback fallback;
        bool held = holdLayout(piece, edge, assessment, fallback);
        if (!held && fallback.any) {
            report_.notes.push_back("a piece's quads missed the bar narrowly and the nearest try was taken");
            piece.quads = std::move(fallback.quads);
            assessment = fallback.assessment;
            held = true;
        }
        if (!held && piece.valid) {
            report_.notes.push_back("a piece's quads did not hold its shape (folded " + std::to_string(assessment.folded) +
                                    " of " + std::to_string(assessment.faces) + ", lost " +
                                    std::to_string(assessment.lost) + ", across seams " +
                                    std::to_string(assessment.straddling) + ") and it was kept as it was");
            failPiece(piece);
            continue;
        }
        if (!held) {
            u32 zero = 0;
            u32 kept = 0;
            for (u32 a = 0; a < piece.lengths.size(); ++a) {
                zero += piece.lengths[a] == 0 ? 1 : 0;
                kept += piece.penalties[a] >= kKeepArc ? 1 : 0;
            }
            const QuadValidity& v = piece.validity;
            report_.notes.push_back("a piece made no valid quads and was kept as it was (repeated " +
                                    std::to_string(v.repeated) + ", non-manifold " + std::to_string(v.nonManifoldEdges) +
                                    ", flipped " + std::to_string(v.flippedEdges) + ", thin " +
                                    std::to_string(v.thinVertices) + "; arcs " + std::to_string(piece.lengths.size()) +
                                    ", zero " + std::to_string(zero) + ", kept " + std::to_string(kept) +
                                    ", unfilled " + std::to_string(piece.layout.stats.unfilled) + ", rounds " +
                                    std::to_string(piece.rounds) + ")");
            failPiece(piece);
            continue;
        }
        if (!finishPiece(piece, piece.share, assessment)) {
            continue;
        }
        for (i32 length : piece.lengths) {
            report_.arcsCollapsed += length == 0 ? 1 : 0;
        }
        report_.patchesMismatched += piece.stats.patchesMismatched;
        report_.quantizeCost += piece.cost;
    }
}

bool Retopology::layoutAlone(Piece& piece, f64 edge, Assessment& assessment, Fallback& fallback) {
    report_.singularities -= std::min(report_.singularities, piece.field.singularityCount());
    preparePiece(piece);
    // At the run's quad edge, as every piece of a layout run is: an edge
    // re-aimed at this piece's share alone grew until a blade's arcs were
    // short enough to fold away.
    if (stopped() || !quantize(piece, edge)) {
        return false;
    }
    extract(piece, edge);
    report_.validityRounds += piece.rounds;
    return holdLayout(piece, edge, assessment, fallback);
}

void Retopology::prepareFieldPiece(Piece& piece, f64 lattice) {
    piece.mesh = SeedWorkMesh(surface_, piece.component);
    // A third of the lattice step, as the layout's work mesh is of its quad.
    const f64 perTriangle = std::sqrt(3.0) / 4.0;
    const f64 coarsest = std::sqrt(piece.area / (perTriangle * kMinimumTriangles));
    const f64 finest = std::sqrt(piece.area / (perTriangle * kMaximumTriangles));
    const f64 length = std::clamp(lattice / 3.0, finest, std::max(finest, coarsest));
    piece.workEdge = length;
    Remesh(piece.mesh, surface_, RemeshOptions{length, 6, 0.5});
    SplitFeatureCorners(piece.mesh, surface_);
    piece.mesh.compact();
    if (stopped()) {
        return;
    }
    FieldOptions field;
    field.scale = lattice;
    piece.field = SolveCrossField(piece.mesh, surface_, field);
    report_.singularities += piece.field.singularityCount();
    piece.frames = VertexFrames(piece.mesh, surface_, piece.field);
}

u64 Retopology::solveField(Piece& piece, f64 lattice, bool split) {
    PositionField field = piece.frames;
    PositionOptions options;
    options.scale = lattice;
    SolvePositions(piece.mesh, field, options);
    WorkMesh mesh = piece.mesh;
    piece.fieldStats = {};
    LatticeSteps steps = MeasureSteps(piece.mesh, field);
    piece.quads = ExtractFieldQuads(mesh, surface_, field, std::move(steps), split, piece.fieldStats);
    return piece.quads.faceCount();
}

void Retopology::runField(std::vector<Piece>& pieces, f64 edge, f64 quads, f64 remeshedArea) {
    // Pure quads: the lattice at twice the edge, each face split in four. A
    // piece only a few of its cells big cannot be held by it: the lattice at
    // the quad edge there, its leftover faces as they are.
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        if (stopped()) {
            return;
        }
        progress(static_cast<u32>(i), steps_);
        Piece& piece = pieces[i];
        piece.split = options_.pureQuads && piece.area >= kFewCells * 4.0 * edge * edge;
        prepareFieldPiece(piece, piece.split ? 2.0 * edge : edge);
    }
    // The edge that lands the count on the request, re-solved once.
    for (u32 attempt = 0;; ++attempt) {
        u64 count = 0;
        for (Piece& piece : pieces) {
            if (stopped()) {
                return;
            }
            count += solveField(piece, piece.split ? 2.0 * edge : edge, piece.split);
        }
        const f64 ratio = static_cast<f64>(count) / quads;
        if (count == 0 || std::abs(ratio - 1.0) < 0.08 || attempt == 1) {
            break;
        }
        edge *= std::sqrt(ratio);
    }
    report_.targetEdge = edge;
    progress(static_cast<u32>(pieces.size()), steps_);

    const f64 fine = edge;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        Piece& piece = pieces[i];
        if (stopped()) {
            return;
        }
        progress(static_cast<u32>(pieces.size() + 1 + i), steps_);
        piece.share = quads * piece.area / remeshedArea;
        const f64 share = piece.share;
        // Each try is prepared and measured as it would be taken, and the
        // first that holds the piece is: pure quads, the lattice at the quad
        // edge, the layout, the piece as it was.
        Assessment taken;
        Fallback fallback;
        bool good = false;
        bool pureHolds = false;
        QuadMesh pure;
        if (piece.split) {
            taken = assess(piece, piece.quads, prepare(piece.quads));
            pureHolds = taken.holds();
            good = pureHolds && taken.turn <= kUneven;
            fallback.offer(piece.quads, taken, 0);
        }
        if (!good) {
            // A lattice twice the quad edge has too few cells across a thin
            // part; where pure quads fold, lose part of the piece or come out
            // uneven, the lattice at the quad edge, if that does better.
            if (piece.split) {
                pure = std::move(piece.quads);
                solveFine(piece, fine);
            }
            const Assessment fineTry = assess(piece, piece.quads, prepare(piece.quads));
            fallback.offer(piece.quads, fineTry, 0);
            if (fineTry.holds() && (!pureHolds || fineTry.turn + kBetter <= taken.turn)) {
                taken = fineTry;
                good = true;
            } else if (pureHolds) {
                piece.quads = std::move(pure);
                good = true;
            }
        }
        if (!good && fallback.any) {
            // Near the bar at the count asked: kept over a finer lattice's
            // count, or the layout's floor.
            report_.notes.push_back("a piece's lattice missed the bar narrowly and was taken");
            piece.quads = std::move(fallback.quads);
            taken = fallback.assessment;
            fallback = Fallback{};
            good = true;
        }
        // A part a few cells across -- a buckle, a strap, a plate's rim --
        // folds in a lattice that coarse: one finer, at some quads over its
        // share, still beats the layout's floor.
        for (const f64 finer : kFinerLattices) {
            if (good || stopped()) {
                break;
            }
            solveFine(piece, finer * fine);
            const Assessment finerTry = assess(piece, piece.quads, prepare(piece.quads));
            fallback.offer(piece.quads, finerTry, 0);
            if (finerTry.holds()) {
                taken = finerTry;
                good = true;
            }
        }
        if (!good) {
            report_.notes.push_back("a piece's lattice did not hold its shape, and the layout took it");
            if (layoutAlone(piece, fine, taken, fallback)) {
                ++report_.piecesFellBack;
            } else if (fallback.any) {
                report_.notes.push_back("a piece's quads missed the bar narrowly and the nearest try was taken");
                piece.quads = std::move(fallback.quads);
                taken = fallback.assessment;
                report_.piecesFellBack += fallback.from;
            } else {
                report_.notes.push_back("a piece's quads did not hold its shape either way and it was kept as it was");
                failPiece(piece);
                continue;
            }
        }
        finishPiece(piece, share, taken);
    }
}

void Retopology::solveFine(Piece& piece, f64 edge) {
    if (piece.workEdge > edge / 2.5) {
        report_.singularities -= std::min(report_.singularities, piece.field.singularityCount());
        prepareFieldPiece(piece, edge);
    }
    solveField(piece, edge, false);
}

RetopoResult Retopology::run() {
    RetopoResult result;
    report_.sourceTriangles = 0;
    welded_ = source_;
    PrepareForModelling(welded_);
    if (!welded_.ensureConnectivity().ok()) {
        report_.failure = RetopoReport::Failure::NotManifold;
        result.report = report_;
        return result;
    }
    surface_ = BuildSurface(welded_, options_.features);
    report_.sourceTriangles = surface_.triangleCount();
    if (surface_.triangleCount() == 0) {
        report_.failure = RetopoReport::Failure::EmptyMesh;
        result.report = report_;
        return result;
    }

    // Pieces, and which are too small to hold a patch.
    std::vector<f64> area(surface_.componentCount, 0.0);
    std::vector<u32> triangles(surface_.componentCount, 0);
    std::vector<std::vector<u32>> trianglesOf(surface_.componentCount);
    for (u32 t = 0; t < surface_.triangleCount(); ++t) {
        area[surface_.components[t]] += surface_.triangleAreas[t];
        ++triangles[surface_.components[t]];
        trianglesOf[surface_.components[t]].push_back(t);
    }
    f64 total = 0.0;
    for (f64 a : area) {
        total += a;
    }
    if (!(total > 0.0)) {
        // No area: no quad edge to size anything by.
        report_.failure = RetopoReport::Failure::EmptyMesh;
        result.report = report_;
        return result;
    }
    const f64 quads = std::max<f64>(1.0, options_.targetQuads);
    f64 edge = std::sqrt(total / quads);
    std::vector<u8> keep(surface_.componentCount, 0);
    for (u32 pass = 0; pass < 2; ++pass) {
        f64 remeshed = 0.0;
        for (u32 c = 0; c < surface_.componentCount; ++c) {
            keep[c] = area[c] < options_.keepBelowQuads * edge * edge ? 1 : 0;
            remeshed += keep[c] ? 0.0 : area[c];
        }
        if (remeshed <= 0.0) {
            break;
        }
        edge = std::sqrt(remeshed / quads);
    }
    report_.targetEdge = edge;
    ScaleFeatures(surface_, edge, options_.features.cornerAngle);

    // Each piece's Euler characteristic: its vertices, edges and triangles.
    std::vector<i64> euler(surface_.componentCount, 0);
    {
        std::vector<u32> vertexPiece(surface_.vertexCount(), kNone);
        for (u32 t = 0; t < surface_.triangleCount(); ++t) {
            const u32 c = surface_.components[t];
            euler[c] += 1;
            for (u32 i = 0; i < 3; ++i) {
                const u32 h = 3 * t + i;
                if (surface_.twins[h] == kNone || surface_.twins[h] > h) {
                    euler[c] -= 1;
                }
                if (vertexPiece[surface_.corners[h]] == kNone) {
                    vertexPiece[surface_.corners[h]] = c;
                    euler[c] += 1;
                }
            }
        }
    }
    std::vector<Piece> pieces;
    f64 remeshedArea = 0.0;
    for (u32 c = 0; c < surface_.componentCount; ++c) {
        if (!keep[c]) {
            Piece piece;
            piece.component = c;
            piece.area = area[c];
            piece.triangles = triangles[c];
            piece.sourceTriangles = std::move(trianglesOf[c]);
            piece.euler = euler[c];
            remeshedArea += area[c];
            pieces.push_back(std::move(piece));
        }
    }
    // Each piece prepared, the count, each piece extracted, the build.
    steps_ = 2 * static_cast<u32>(pieces.size()) + 2;
    if (options_.method == RetopoMethod::Field) {
        runField(pieces, edge, quads, remeshedArea);
    } else {
        runLayout(pieces, edge, quads, remeshedArea);
    }
    if (report_.failure == RetopoReport::Failure::Cancelled) {
        result.report = report_;
        return result;
    }
    for (u32 c = 0; c < surface_.componentCount; ++c) {
        if (keep[c]) {
            keepComponent(c);
        }
    }
    progress(steps_ - 1, steps_);
    if (stopped()) {
        result.report = report_;
        return result;
    }
    if (!pieces.empty() && report_.piecesRemeshed == 0) {
        // Every piece was kept: the output would be the source again.
        report_.failure = RetopoReport::Failure::Unsolved;
        result.report = report_;
        return result;
    }

    result.mesh = build();
    // The count and the irregular vertices, as built.
    if (result.mesh.hasConnectivity()) {
        const Topology& topology = result.mesh.topology();
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            if (topology.valence(FaceId(f)) == 4) {
                ++report_.quads;
            } else {
                ++report_.otherFaces;
            }
        }
        for (u32 v = 0; v < topology.vertexCount(); ++v) {
            if (!topology.isBoundary(VertexId(v)) && topology.valence(VertexId(v)) != 4) {
                ++report_.irregularVertices;
            }
        }
    }
    progress(steps_, steps_);
    result.report = report_;
    return result;
}

} // namespace

RetopoResult Retopologize(const Mesh& source, const RetopoOptions& options, const QuantizeSolver& solver,
                          const RetopoControl& control) {
    Retopology retopology(source, options, solver, control);
    return retopology.run();
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
