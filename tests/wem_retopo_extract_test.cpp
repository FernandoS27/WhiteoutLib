// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// R4/R5 (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.7, §1.8): quantization and the
/// first quads, on the shapes whose layouts are rectangles throughout.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/retopo/quantize.h>

#include "whiteout/models/wem/geometry/retopo/cross_field.h"
#include "whiteout/models/wem/geometry/retopo/extract.h"
#include "whiteout/models/wem/geometry/retopo/layout.h"
#include "whiteout/models/wem/geometry/retopo/surface.h"
#include "whiteout/models/wem/geometry/retopo/work_mesh.h"

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace whiteout::models::wem::geom::retopo;

namespace {

Mesh Prepared(Mesh mesh) {
    geom::PrepareForModelling(mesh);
    mesh.ensureConnectivity();
    return mesh;
}

/// An L of three unit squares, flat.
Mesh MakeL() {
    geom::MeshBuilder builder;
    std::vector<geom::VertexId> v;
    const u32 n = 9;
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            v.push_back(builder.addVertex({i * 0.25f, j * 0.25f, 0.0f}));
        }
    }
    for (u32 j = 0; j + 1 < n; ++j) {
        for (u32 i = 0; i + 1 < n; ++i) {
            if (i >= 4 && j >= 4) {
                continue;
            }
            builder.addTriangle(v[j * n + i], v[j * n + i + 1], v[(j + 1) * n + i + 1]);
            builder.addTriangle(v[j * n + i], v[(j + 1) * n + i + 1], v[(j + 1) * n + i]);
        }
    }
    Mesh mesh = builder.build().mesh;
    mesh.ensureConnectivity();
    return mesh;
}

Mesh MakeTorus(f32 major, f32 minor, u32 around, u32 tube) {
    geom::MeshBuilder builder;
    std::vector<geom::VertexId> v;
    for (u32 i = 0; i < around; ++i) {
        const f32 a = 2.0f * 3.14159265f * i / around;
        for (u32 j = 0; j < tube; ++j) {
            const f32 b = 2.0f * 3.14159265f * j / tube;
            const f32 r = major + minor * std::cos(b);
            v.push_back(builder.addVertex({r * std::cos(a), r * std::sin(a), minor * std::sin(b)}));
        }
    }
    for (u32 i = 0; i < around; ++i) {
        for (u32 j = 0; j < tube; ++j) {
            const u32 i1 = (i + 1) % around;
            const u32 j1 = (j + 1) % tube;
            builder.addTriangle(v[i * tube + j], v[i1 * tube + j], v[i1 * tube + j1]);
            builder.addTriangle(v[i * tube + j], v[i1 * tube + j1], v[i * tube + j1]);
        }
    }
    Mesh mesh = builder.build().mesh;
    mesh.ensureConnectivity();
    return mesh;
}

struct Result {
    QuadMesh quads;
    ExtractStats stats;
    Layout layout;
    QuantizeSolution solution;
    f64 diagonal = 0.0;
};

Result Run(const Mesh& source, f64 quadEdge) {
    Result r;
    const Surface surface = BuildSurface(source, FeatureOptions{});
    r.diagonal = surface.diagonal;
    WorkMesh mesh = SeedWorkMesh(surface, 0);
    Remesh(mesh, surface, RemeshOptions{quadEdge / 3.0, 6, 0.5});
    SplitFeatureCorners(mesh, surface);
    mesh.compact();
    FieldOptions field;
    field.scale = quadEdge;
    CrossField cross = SolveCrossField(mesh, surface, field);
    LayoutOptions options;
    options.chord = 2.0 * quadEdge / 3.0;
    r.layout = BuildLayout(mesh, cross, surface, options);
    std::vector<f64> targets;
    for (const LayoutArc& arc : r.layout.arcs) {
        targets.push_back(arc.length / quadEdge);
    }
    // As `Retopologize` states it: no bound, and a price on every unit under
    // one that only a layout which cannot close pays.
    QuantizeProblem problem = BuildQuantizeProblem(r.layout, targets, 0, 4.0);
    for (QuantizeArc& arc : problem.arcs) {
        arc.zeroPenalty = 1000.0;
    }
    r.solution = SolveQuantizeDoubleCover(problem);
    if (!r.solution.solved) {
        std::vector<u32> heads(problem.nodeCount, 0);
        std::vector<u32> tails(problem.nodeCount, 0);
        for (const QuantizeArc& arc : problem.arcs) {
            (arc.headA ? heads : tails)[arc.nodeA] += 1;
            (arc.headB ? heads : tails)[arc.nodeB] += 1;
        }
        {
            QuantizeProblem zero = problem;
            for (QuantizeArc& arc : zero.arcs) {
                arc.lower = 0;
            }
            const QuantizeSolution z = SolveQuantizeDoubleCover(zero);
            if (z.lengths.empty()) { return r; }
            WARN("with lower 0: solved " << z.solved << " arcs " << problem.arcs.size() << " nodes "
                                         << problem.nodeCount);
            for (u32 p = 0; p < r.layout.patches.size(); ++p) {
                const LayoutPatch& patch = r.layout.patches[p];
                std::string text = "patch " + std::to_string(p) + (patch.rectangle ? "" : " (no rectangle)") + ":";
                for (u32 s = 0; s < 4; ++s) {
                    text += " [";
                    for (const SideArc& run : patch.sides[s]) {
                        text += (run.forward ? "+" : "-") + std::to_string(run.arc) + " ";
                    }
                    text += "]";
                }
                WARN(text);
            }
            for (u32 a = 0; a < r.layout.arcs.size(); ++a) {
                const LayoutArc& arc = r.layout.arcs[a];
                WARN("arc " << a << " " << arc.vertices.front() << "->" << arc.vertices.back() << " left " << arc.left
                            << "/" << int(arc.leftSide) << " right " << arc.right << "/" << int(arc.rightSide)
                            << " zero-solution " << z.lengths[a]);
            }
        }
        for (u32 n = 0; n < problem.nodeCount; ++n) {
            if (n != problem.freeNode && (heads[n] == 0 || tails[n] == 0)) {
                const LayoutPatch& patch = r.layout.patches[n / 2];
                WARN("node " << n << " heads " << heads[n] << " tails " << tails[n] << " rect " << patch.rectangle
                             << " sides " << patch.sides[0].size() << "," << patch.sides[1].size() << ","
                             << patch.sides[2].size() << "," << patch.sides[3].size());
            }
        }
        return r;
    }
    r.quads = ExtractQuads(mesh, surface, r.layout, r.solution.lengths, r.stats);
    RemoveDoublets(r.quads);
    CompactQuads(r.quads);
    RelaxQuads(r.quads, surface, 10);
    // With RETOPO_DUMP set, the quads as OBJ for a look.
    if (const char* directory = std::getenv("RETOPO_DUMP")) {
        static int index = 0;
        std::FILE* file = std::fopen((std::string(directory) + "/quads" + std::to_string(index++) + ".obj").c_str(), "w");
        if (file != nullptr) {
            for (const V3& p : r.quads.positions) {
                std::fprintf(file, "v %.9g %.9g %.9g\n", p.x, p.y, p.z);
            }
            for (u32 f = 0; f < r.quads.faceCount(); ++f) {
                std::fprintf(file, "f");
                for (u32 v : r.quads.face(f)) {
                    std::fprintf(file, " %u", v + 1);
                }
                std::fprintf(file, "\n");
            }
            std::fclose(file);
        }
    }
    return r;
}

void CheckQuadMesh(const Result& r, bool allQuads) {
    INFO("faces " << r.quads.faceCount() << " quads " << r.stats.quads << " filled " << r.stats.patchesFilled
                  << " skipped " << r.stats.patchesSkipped << " merged " << r.stats.mergedVertices);
    REQUIRE(r.solution.solved);
    const QuadValidity validity = CheckQuads(r.quads);
    CHECK(validity.repeated == 0);
    CHECK(validity.nonManifoldEdges == 0);
    CHECK(validity.flippedEdges == 0);
    if (allQuads) {
        CHECK(r.stats.quads == r.quads.faceCount());
        CHECK(r.stats.patchesSkipped == 0);
    }
}

} // namespace

TEST_CASE("retopo quantize: the double cover honours every patch", "[retopo][extract]") {
    // Two rectangles side by side on one long side, sharing their middle arc:
    // nodes 0/1 the first patch, 2/3 the second, 4 the border.
    QuantizeProblem problem;
    problem.nodeCount = 5;
    problem.freeNode = 4;
    auto arc = [&](u32 a, bool ha, u32 b, bool hb, f64 target) {
        problem.arcs.push_back(QuantizeArc{a, b, ha, hb, target, 1.0 / std::max(target, 1.0), 1, kUnbounded});
    };
    arc(0, true, 4, true, 3.2);   // first, side 0
    arc(0, false, 4, true, 2.6);  // first, side 2
    arc(1, true, 3, false, 1.4);  // shared: first side 1, second side 3
    arc(1, false, 4, true, 1.6);  // first side 3
    arc(2, true, 4, true, 4.1);   // second side 0
    arc(2, false, 4, true, 3.7);  // second side 2
    arc(3, true, 4, true, 1.5);   // second side 1
    const QuantizeSolution solution = SolveQuantizeDoubleCover(problem);
    REQUIRE(solution.solved);
    CHECK(QuantizeFeasible(problem, solution.lengths));
    CHECK(solution.lengths[0] == solution.lengths[1]);
    CHECK(solution.lengths[4] == solution.lengths[5]);
    CHECK(solution.lengths[2] == solution.lengths[3]);
    CHECK(solution.lengths[2] == solution.lengths[6]);
}

TEST_CASE("retopo extract: a box comes out as quads", "[retopo][extract]") {
    const Result r = Run(Prepared(geom::MakeBox({})), 0.5);
    CheckQuadMesh(r, true);
    CHECK(r.quads.faceCount() == 6 * 16);
}

TEST_CASE("retopo extract: a plane and an L come out as quads", "[retopo][extract]") {
    geom::PrimitiveParams params;
    params.segments = 2;
    const Result plane = Run(Prepared(geom::MakePlane(params)), 0.25);
    CheckQuadMesh(plane, true);
    CHECK(plane.quads.faceCount() == 64);
    const Result l = Run(MakeL(), 0.25);
    CheckQuadMesh(l, true);
    CHECK(l.quads.faceCount() == 48);
}

TEST_CASE("retopo extract: a torus comes out as quads", "[retopo][extract]") {
    // Its layout winds round the torus and does not close: a few patches'
    // sides differ, filled with a triangle or two along the shorter.
    const Result r = Run(MakeTorus(2.0f, 0.7f, 32, 16), 0.4);
    CheckQuadMesh(r, false);
    CHECK(r.stats.quads >= r.quads.faceCount() * 95 / 100);
}

TEST_CASE("retopo extract: the cylinder and the sphere close up", "[retopo][extract]") {
    geom::PrimitiveParams params;
    params.sides = 24;
    params.size = Vector3f{0.5f, 0.5f, 1.0f};
    const Result cylinder = Run(Prepared(geom::MakeCylinder(params)), 0.2);
    CheckQuadMesh(cylinder, false);
    geom::PrimitiveParams sphere;
    sphere.sides = 24;
    sphere.segments = 12;
    const Result s = Run(Prepared(geom::MakeSphere(sphere)), 0.25);
    CheckQuadMesh(s, false);
}
