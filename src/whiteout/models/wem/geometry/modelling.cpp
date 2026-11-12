// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/shading.h>

#include "modelling_common.h"
#include "rebuild.h"
#include "weld.h"

#include <whiteout/models/wem/geometry/repair.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/geometry/uv/seams.h>
#include <whiteout/models/wem/skinning/points.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

const char* ToString(ModelRefusal refusal) {
    switch (refusal) {
    case ModelRefusal::None: return "None";
    case ModelRefusal::EmptySelection: return "EmptySelection";
    case ModelRefusal::DifferentMeshes: return "DifferentMeshes";
    case ModelRefusal::WouldPinch: return "WouldPinch";
    case ModelRefusal::CrackNeedsWeld: return "CrackNeedsWeld";
    case ModelRefusal::NotAdjacent: return "NotAdjacent";
    case ModelRefusal::BorderEdgeRemove: return "BorderEdgeRemove";
    case ModelRefusal::BorderEdgeChamfer: return "BorderEdgeChamfer";
    case ModelRefusal::WouldRepeatVertex: return "WouldRepeatVertex";
    case ModelRefusal::WouldDuplicateFace: return "WouldDuplicateFace";
    case ModelRefusal::WouldFold: return "WouldFold";
    case ModelRefusal::CollapseIllegal: return "CollapseIllegal";
    case ModelRefusal::TargetCollapseIllegal: return "TargetCollapseIllegal";
    case ModelRefusal::AllFaces: return "AllFaces";
    case ModelRefusal::ZeroAmount: return "ZeroAmount";
    case ModelRefusal::ConcaveTurn: return "ConcaveTurn";
    case ModelRefusal::DiagonalOutside: return "DiagonalOutside";
    case ModelRefusal::AdjacentCorners: return "AdjacentCorners";
    case ModelRefusal::NotOneFace: return "NotOneFace";
    case ModelRefusal::DiagonalExists: return "DiagonalExists";
    case ModelRefusal::CapTooShort: return "CapTooShort";
    case ModelRefusal::CrossesSection: return "CrossesSection";
    case ModelRefusal::DissolveVertexIllegal: return "DissolveVertexIllegal";
    case ModelRefusal::NothingToSlice: return "NothingToSlice";
    case ModelRefusal::NoSourceSide: return "NoSourceSide";
    case ModelRefusal::WindingDisagrees: return "WindingDisagrees";
    case ModelRefusal::LoopCountsDiffer: return "LoopCountsDiffer";
    case ModelRefusal::SameLoop: return "SameLoop";
    case ModelRefusal::HingeNotOnBoundary: return "HingeNotOnBoundary";
    case ModelRefusal::NotBuiltYet: return "NotBuiltYet";
    case ModelRefusal::NoPlane: return "NoPlane";
    case ModelRefusal::PinnedBorders: return "PinnedBorders";
    }
    return "Unknown";
}

// ============================================================================
// Shared pieces
// ============================================================================

namespace {

using detail::RebuildMapping;

Vector3f sub(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.x - b.x, a.y - b.y, a.z - b.z};
}

f64 length64(const Vector3f& v) {
    return std::sqrt(static_cast<f64>(v.x) * v.x + static_cast<f64>(v.y) * v.y +
                     static_cast<f64>(v.z) * v.z);
}

/// The Newell normal of @p loop, unit length, or zero for a line.
std::array<f64, 3> newellOf(std::span<const Vector3f> loop) {
    std::array<f64, 3> n{0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const Vector3f& a = loop[i];
        const Vector3f& b = loop[(i + 1) % loop.size()];
        n[0] += (static_cast<f64>(a.y) - b.y) * (static_cast<f64>(a.z) + b.z);
        n[1] += (static_cast<f64>(a.z) - b.z) * (static_cast<f64>(a.x) + b.x);
        n[2] += (static_cast<f64>(a.x) - b.x) * (static_cast<f64>(a.y) + b.y);
    }
    const f64 length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (length > 0.0) {
        for (f64& c : n) {
            c /= length;
        }
    }
    return n;
}

using namespace welddetail;
using modellingdetail::angleBetween;
using modellingdetail::positionsOf;
using modellingdetail::sortedUnique;
using modellingdetail::toArray;

/// Face @p face's loop from its first halfedge, as vertex ids.
std::vector<u32> loopOf(const Topology& topology, FaceId face) {
    std::vector<u32> out;
    for (const HalfedgeId h : topology.fh(face)) {
        out.push_back(topology.from(h).value());
    }
    return out;
}

std::vector<Vector3f> placesOf(std::span<const Vector3f> positions, std::span<const u32> loop) {
    std::vector<Vector3f> out;
    out.reserve(loop.size());
    for (const u32 v : loop) {
        out.push_back(v < positions.size() ? positions[v] : Vector3f{0.0f, 0.0f, 0.0f});
    }
    return out;
}

/// Face @p face as it is drawn, vertex ids, three per triangle.
std::vector<u32> drawnOf(const Mesh& mesh, FaceId face) {
    const std::vector<u32> loop = loopOf(mesh.topology(), face);
    const std::span<const u32> row = mesh.triangulation.row(face.value());
    std::vector<u32> cut;
    TriangulateFace(loop, positionsOf(mesh), row, cut);
    for (u32& corner : cut) {
        corner = loop[corner];
    }
    return cut;
}

/// Whether every Halfedge layer holds the same bytes at corners @p a and @p b.
bool sameCorner(const Mesh& mesh, HalfedgeId a, HalfedgeId b) {
    for (const AttrLayer& layer : mesh.attributes.layers()) {
        if (layer.domain != Domain::Halfedge) {
            continue;
        }
        // A pin says "hold this corner where it is", which is a thing about one
        // corner and never a reason to keep two apart: two corners that agree in
        // everything else are one corner, pinned or not
        // (EDIT_MODE_UV_DESIGN.md §3). A custom-normal mark is the same.
        if (names::IsCornerMark(layer.name)) {
            continue;
        }
        const std::size_t stride = AttrTypeSize(layer.type);
        const std::size_t at = stride * a.index();
        const std::size_t bt = stride * b.index();
        if (at + stride > layer.data.size() || bt + stride > layer.data.size()) {
            continue;
        }
        if (std::memcmp(layer.data.data() + at, layer.data.data() + bt, stride) != 0) {
            return false;
        }
    }
    return true;
}

/// §3.2's delimit for joining across interior edge @p edge: the same section
/// and smoothing group, every corner layer equal at both ends, and (when
/// @p flagged) no `seam` or `sharp` on the edge.
bool joinable(const Mesh& mesh, EdgeId edge, bool flagged) {
    const Topology& topology = mesh.topology();
    const HalfedgeId h0 = Topology::halfedge(edge, 0);
    const HalfedgeId h1 = Topology::halfedge(edge, 1);
    const FaceId fa = topology.face(h0);
    const FaceId fb = topology.face(h1);
    if (!fa.valid() || !fb.valid() || fa == fb) {
        return false;
    }
    const std::span<const u32> sections = mesh.faceSections();
    if (fa.index() < sections.size() && fb.index() < sections.size() &&
        sections[fa.index()] != sections[fb.index()]) {
        return false;
    }
    const std::span<const u32> smoothing = mesh.attributes.get<const u32>(names::kSmoothGroup, Domain::Face);
    if (fa.index() < smoothing.size() && fb.index() < smoothing.size() &&
        smoothing[fa.index()] != smoothing[fb.index()]) {
        return false;
    }
    if (flagged) {
        const std::span<const u8> seam = mesh.attributes.get<const u8>(names::kSeam, Domain::Edge);
        const std::span<const u8> sharp = mesh.attributes.get<const u8>(names::kSharp, Domain::Edge);
        if ((edge.index() < seam.size() && seam[edge.index()] != 0) ||
            (edge.index() < sharp.size() && sharp[edge.index()] != 0)) {
            return false;
        }
    }
    // One corner per vertex survives a join: both sides must already agree.
    return sameCorner(mesh, h0, topology.next(h1)) && sameCorner(mesh, topology.next(h0), h1);
}

/// The loop a join across @p edge would leave, as vertex ids: @p edge's side-0
/// face from past the edge round to it, then the other face the same way.
std::vector<u32> mergedLoop(const Topology& topology, EdgeId edge) {
    std::vector<u32> out;
    for (u32 side = 0; side < 2; ++side) {
        const HalfedgeId h = Topology::halfedge(edge, side);
        for (HalfedgeId walk = topology.next(h); walk != h; walk = topology.next(walk)) {
            out.push_back(topology.from(walk).value());
        }
    }
    return out;
}

/// Builds every row once: the table as it stands, with @p rows written over it
/// and @p cleared emptied.
void storeRows(Mesh& mesh, const std::unordered_map<u32, std::vector<u32>>& rows,
               const std::vector<u32>& cleared) {
    const u32 slots = std::as_const(mesh).topology().faceCount();
    FaceTriangulationBuilder builder(slots);
    for (u32 f = 0; f < slots; ++f) {
        builder.set(f, mesh.triangulation.row(f));
    }
    for (const u32 f : cleared) {
        builder.set(f, {});
    }
    for (const auto& [face, row] : rows) {
        builder.set(face, row);
    }
    mesh.triangulation = builder.build();
}

/// §3.1's promise for a tool that must write before it knows whether it can
/// finish (a split, then a rebuild that may fold): the mesh as it came in, put
/// back when the plan it watches ends refused. Declare it after the plan, and
/// return that plan on every path.
class Rollback {
public:
    Rollback(Mesh& mesh, const ModelPlan& plan)
        : mesh_(mesh), plan_(plan), before_(std::make_shared<const Mesh>(mesh)) {}
    ~Rollback() {
        if (plan_.refused()) {
            mesh_ = *before_;
        }
    }
    Rollback(const Rollback&) = delete;
    Rollback& operator=(const Rollback&) = delete;

    /// The mesh as it came in, for a plan that keeps it (Chamfer, Q3).
    const std::shared_ptr<const Mesh>& before() const {
        return before_;
    }

private:
    Mesh& mesh_;
    const ModelPlan& plan_;
    std::shared_ptr<const Mesh> before_;
};

} // namespace

// ============================================================================
// The contract (§3.1)
// ============================================================================

namespace {

/// `ResamplesAt` over captures a tool shares between all its points: each is
/// of the mesh as Plan left it, so one per face serves every corner of it. A
/// whole-mesh press took most of its time capturing each face four times.
std::vector<Resample> resamplesAt(const Mesh& mesh, VertexId vertex, const Vector3f& to,
                                  std::unordered_map<u32, SourcePolygon>& captured) {
    std::vector<Resample> out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    if (vertex.index() >= topology.vertexCount() || topology.isDeleted(vertex) ||
        !topology.outgoing(vertex).valid()) {
        return out;
    }
    std::vector<HalfedgeId> fan;
    for (const HalfedgeId h : topology.voh(vertex)) {
        if (topology.face(h).valid()) {
            fan.push_back(h);
        }
    }
    if (fan.empty()) {
        return out;
    }
    std::unordered_map<u32, bool> holding;
    const auto captureOf = [&](FaceId f) -> const SourcePolygon& {
        auto found = captured.find(f.value());
        if (found == captured.end()) {
            found = captured.emplace(f.value(), CapturePolygon(mesh, f)).first;
        }
        return found->second;
    };
    // A face a tool made with no area yet (a strip or wall at amount 0) holds
    // nothing and is read by no class that has another face: it would hand
    // every point its nearest corner (AMT-2).
    const auto hasArea = [&](FaceId f) {
        const std::array<f64, 3> n = newellOf(captureOf(f).positions);
        return n[0] * n[0] + n[1] * n[1] + n[2] * n[2] > 0.5;
    };
    const auto holds = [&](FaceId f) {
        const auto found = holding.find(f.value());
        if (found != holding.end()) {
            return found->second;
        }
        const SourcePolygon& polygon = captureOf(f);
        std::vector<f32> weights(polygon.cornerCount());
        const bool inside = hasArea(f) && SampleWeights(polygon.positions, polygon.cut, to, weights);
        holding.emplace(f.value(), inside);
        return inside;
    };
    FaceId vertexSource = topology.face(fan[0]);
    for (const HalfedgeId h : fan) {
        if (hasArea(topology.face(h))) {
            vertexSource = topology.face(h);
            break;
        }
    }
    for (const HalfedgeId h : fan) {
        if (holds(topology.face(h))) {
            vertexSource = topology.face(h);
            break;
        }
    }
    // Classes: bit-equal corners, in fan order of first appearance.
    std::vector<std::vector<HalfedgeId>> classes;
    for (const HalfedgeId h : fan) {
        bool placed = false;
        for (std::vector<HalfedgeId>& group : classes) {
            if (sameCorner(mesh, group[0], h)) {
                group.push_back(h);
                placed = true;
                break;
            }
        }
        if (!placed) {
            classes.push_back({h});
        }
    }
    for (const std::vector<HalfedgeId>& group : classes) {
        FaceId source = topology.face(group[0]);
        bool carriesVertex = false;
        for (const HalfedgeId h : group) {
            carriesVertex = carriesVertex || topology.face(h) == vertexSource;
        }
        if (carriesVertex) {
            source = vertexSource;
        } else {
            bool found = false;
            for (const HalfedgeId h : group) {
                if (holds(topology.face(h))) {
                    source = topology.face(h);
                    found = true;
                    break;
                }
            }
            for (const HalfedgeId h : group) {
                if (!found && hasArea(topology.face(h))) {
                    source = topology.face(h);
                    found = true;
                }
            }
            if (!found) {
                source = vertexSource;
            }
        }
        Resample resample;
        resample.targetVertex = carriesVertex ? vertex.value() : kInvalidId;
        resample.targetCorners = group;
        resample.source = captureOf(source);
        out.push_back(std::move(resample));
    }
    return out;
}

} // namespace

std::vector<Resample> ResamplesAt(const Mesh& mesh, VertexId vertex, const Vector3f& to) {
    std::unordered_map<u32, SourcePolygon> captured;
    return resamplesAt(mesh, vertex, to, captured);
}

namespace {

/// The faces around the vertices of @p motions, live, sorted.
std::vector<u32> facesAround(const Mesh& mesh, std::span<const u32> vertices) {
    std::vector<u32> out;
    const Topology& topology = mesh.topology();
    for (const u32 v : vertices) {
        if (v >= topology.vertexCount() || topology.isDeleted(VertexId(v)) ||
            !topology.outgoing(VertexId(v)).valid()) {
            continue;
        }
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            const FaceId f = topology.face(h);
            if (f.valid() && !topology.isDeleted(f)) {
                out.push_back(f.value());
            }
        }
    }
    return sortedUnique(std::move(out));
}

/// A custom normal turns with its face: each one @p plan's motions reach is
/// put back as it began and turned from the motions' bases to where the
/// vertices are now.
void turnCustomNormals(Mesh& mesh, const ModelPlan& plan, const std::vector<u32>& faces) {
    const std::span<const u8> custom = std::as_const(mesh).attributes.get<const u8>(names::kNormalCustom, Domain::Halfedge);
    const std::span<Vector3f> normals = mesh.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
    if (custom.empty() || normals.empty() || faces.empty()) {
        return;
    }
    const Topology& topology = std::as_const(mesh).topology();
    if (!plan.customNormalsHeld) {
        plan.customNormalsHeld = true;
        for (const u32 f : faces) {
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                if (h.index() < custom.size() && custom[h.index()] != 0 && h.index() < normals.size()) {
                    plan.customNormals.emplace_back(h.value(), normals[h.index()]);
                }
            }
        }
    }
    if (plan.customNormals.empty()) {
        return;
    }
    for (const auto& [h, normal] : plan.customNormals) {
        if (h < normals.size()) {
            normals[h] = normal;
        }
    }
    const std::span<const Vector3f> now = positionsOf(mesh);
    std::vector<Vector3f> before(now.begin(), now.end());
    for (const VertexMotion& m : plan.motions) {
        if (m.vertex < before.size()) {
            before[m.vertex] = m.base;
        }
    }
    std::vector<FaceId> ids;
    ids.reserve(faces.size());
    for (const u32 f : faces) {
        ids.push_back(FaceId(f));
    }
    shading::TurnCustomNormals(mesh, ids, before);
}

void reshade(Mesh& mesh, const std::vector<u32>& faces) {
    if (faces.empty()) {
        return;
    }
    std::vector<FaceId> ids;
    ids.reserve(faces.size());
    for (const u32 f : faces) {
        ids.push_back(FaceId(f));
    }
    if (mesh.attributes.has(names::kNormal, Domain::Halfedge)) {
        RecomputeNormals(mesh, ids, ShadingAngle(mesh));
    }
    if (mesh.attributes.has(names::kTangent, Domain::Halfedge)) {
        RecomputeTangents(mesh, ids, 0);
    }
}

/// X4: how far towards a collapse an amount may go. The root itself is a face
/// of no area or an edge of no length, so the range stops a hundredth short.
constexpr f32 kShortOfCollapse = 0.99f;

std::vector<u32> remapList(const std::vector<u32>& list, const std::vector<u32>& table) {
    if (table.empty()) {
        return list;
    }
    std::vector<u32> out;
    for (const u32 v : list) {
        if (v < table.size() && table[v] != kInvalidId) {
            out.push_back(table[v]);
        }
    }
    return sortedUnique(std::move(out));
}

} // namespace

void AmountRange(const Mesh& mesh, ModelPlan& plan) {
    if (plan.motions.empty() || !mesh.hasConnectivity() || plan.hinge) {
        return; // a hinge's points are not linear in the amount: unbounded
    }
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> positions = positionsOf(mesh);
    std::unordered_map<u32, const VertexMotion*> moving;
    std::vector<u32> movers;
    for (const VertexMotion& motion : plan.motions) {
        moving[motion.vertex] = &motion;
        movers.push_back(motion.vertex);
    }
    const auto at = [&](u32 v, f64 t) {
        const auto found = moving.find(v);
        if (found == moving.end()) {
            return toArray(positions[v]);
        }
        const VertexMotion& m = *found->second;
        return std::array<f64, 3>{m.base.x + t * m.direction.x, m.base.y + t * m.direction.y,
                                  m.base.z + t * m.direction.z};
    };
    for (const u32 f : facesAround(mesh, movers)) {
        const std::vector<u32> loop = loopOf(topology, FaceId(f));
        std::vector<Vector3f> base;
        for (const u32 v : loop) {
            const std::array<f64, 3> p = at(v, 0.0);
            base.push_back(Vector3f{static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2])});
        }
        std::array<f64, 3> n = newellOf(base);
        // A face the tool made (§3.8's walls and ring) has no area at 0 yet, so
        // it is measured from the side the amount opens it on: its normal is
        // taken just off zero, where it is still opening whatever it does
        // later, and the root at 0 is its own beginning, not a fold.
        const bool opening = n[0] * n[0] + n[1] * n[1] + n[2] * n[2] <= 1e-24;
        if (opening) {
            std::vector<Vector3f> open;
            for (const u32 v : loop) {
                const std::array<f64, 3> p = at(v, 1e-3);
                open.push_back(Vector3f{static_cast<f32>(p[0]), static_cast<f32>(p[1]),
                                        static_cast<f32>(p[2])});
            }
            n = newellOf(open);
        }
        std::vector<u32> cut;
        TriangulateFace(loop, positions, mesh.triangulation.row(f), cut);
        if (opening && loop.size() == 4) {
            // A face with no area yet has no cut worth the name: its corners
            // are on top of each other, so which diagonal comes back is a coin
            // toss. Both are tested, and a quad that twists the other way (an
            // inset corner running past the edge beside it) is caught either
            // way round.
            cut = {0, 1, 2, 0, 2, 3, 0, 1, 3, 1, 2, 3};
        }
        for (std::size_t k = 0; k + 2 < cut.size(); k += 3) {
            const auto area = [&](f64 t) {
                const std::array<f64, 3> a = at(loop[cut[k]], t);
                const std::array<f64, 3> b = at(loop[cut[k + 1]], t);
                const std::array<f64, 3> c = at(loop[cut[k + 2]], t);
                const std::array<f64, 3> u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                const std::array<f64, 3> w{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                return n[0] * (u[1] * w[2] - u[2] * w[1]) + n[1] * (u[2] * w[0] - u[0] * w[2]) +
                       n[2] * (u[0] * w[1] - u[1] * w[0]);
            };
            const f64 c0 = area(0.0);
            if (!opening && !(c0 > 0.0)) {
                continue;
            }
            const f64 plus = area(1.0);
            const f64 minus = area(-1.0);
            const f64 c1 = (plus - minus) * 0.5;
            const f64 c2 = (plus + minus) * 0.5 - c0;
            std::vector<f64> roots;
            if (std::abs(c2) <= 1e-12 * (std::abs(c1) + std::abs(c0))) {
                if (c1 != 0.0) {
                    roots.push_back(-c0 / c1);
                }
            } else {
                const f64 disc = c1 * c1 - 4.0 * c2 * c0;
                if (disc >= 0.0) {
                    const f64 root = std::sqrt(disc);
                    // The stable pair: no cancellation in either root.
                    const f64 q = -0.5 * (c1 + (c1 >= 0.0 ? root : -root));
                    if (q != 0.0) {
                        roots.push_back(q / c2);
                        roots.push_back(c0 / q);
                    }
                }
            }
            for (const f64 r : roots) {
                if (opening && std::abs(r) <= 1e-9) {
                    continue; // where it opened from, not where it folds
                }
                if (r > 0.0) {
                    plan.amountMax = std::min(plan.amountMax, static_cast<f32>(r) * kShortOfCollapse);
                } else if (r < 0.0) {
                    plan.amountMin = std::max(plan.amountMin, static_cast<f32>(r) * kShortOfCollapse);
                }
            }
        }
    }
}

void ApplyAmount(Mesh& mesh, const ModelPlan& plan, f32 amount) {
    if (plan.refused() || plan.motions.empty()) {
        return;
    }
    amount = std::clamp(amount, plan.amountMin, plan.amountMax);
    {
        const std::span<Vector3f> positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        if (plan.hinge) {
            // An angle, in degrees (§3.8): every motion turned about the hinge
            // by Rodrigues' formula. At exactly 0 the base goes back untouched,
            // so a hinge that was never swung leaves no rounding behind it and
            // a drag that went nowhere reads as nowhere.
            const f32 radians = amount * (3.14159265358979f / 180.0f);
            const f32 c = std::cos(radians);
            const f32 s = std::sin(radians);
            Vector3f k = plan.hinge->axis;
            const f32 length = k.length();
            k = length > 1e-12f ? k * (1.0f / length) : Vector3f{0.0f, 0.0f, 1.0f};
            for (const VertexMotion& m : plan.motions) {
                if (m.vertex >= positions.size()) {
                    continue;
                }
                if (amount == 0.0f) {
                    positions[m.vertex] = m.base;
                    continue;
                }
                const Vector3f r = m.base - plan.hinge->origin;
                positions[m.vertex] = plan.hinge->origin + r * c + cross(k, r) * s + k * (k.dot(r) * (1.0f - c));
            }
        } else {
            for (const VertexMotion& m : plan.motions) {
                if (m.vertex < positions.size()) {
                    positions[m.vertex] = Vector3f{m.base.x + amount * m.direction.x,
                                                   m.base.y + amount * m.direction.y,
                                                   m.base.z + amount * m.direction.z};
                }
            }
        }
    }
    if (!mesh.hasConnectivity()) {
        return;
    }
    const std::span<const Vector3f> positions = positionsOf(mesh);
    std::vector<HalfedgeId> corners;
    for (const Resample& resample : plan.resamples) {
        // Each vertex the class's corners sit at reads its own place: a seam
        // strip's far corners are two vertices (EXT-8).
        std::vector<u32> at;
        for (const HalfedgeId h : resample.targetCorners) {
            at.push_back(mesh.topology().from(h).value());
        }
        at = sortedUnique(std::move(at));
        for (const u32 vertex : at) {
            if (vertex >= positions.size()) {
                continue;
            }
            corners.clear();
            for (const HalfedgeId h : resample.targetCorners) {
                if (mesh.topology().from(h).value() == vertex) {
                    corners.push_back(h);
                }
            }
            std::vector<f32> weights(resample.source.cornerCount());
            SampleWeights(resample.source.positions, resample.source.cut, positions[vertex], weights);
            BlendCorners(mesh, resample.source, weights, corners);
            if (resample.targetVertex == vertex && !mesh.skin.empty() && vertex < mesh.skin.vertexCount()) {
                const std::vector<Influence> blended = BlendInfluences(resample.source, weights);
                mesh.skin.assignVertex(vertex, blended);
            }
        }
    }
    std::vector<u32> movers;
    for (const VertexMotion& m : plan.motions) {
        movers.push_back(m.vertex);
    }
    const std::vector<u32> around = facesAround(mesh, movers);
    turnCustomNormals(mesh, plan, around);
    reshade(mesh, around);
}

void FinishTool(Mesh& mesh, ModelPlan& plan, f32 creaseAngle) {
    if (plan.refused()) {
        return;
    }
    if (plan.unchangedAtZero && !plan.motions.empty()) {
        const std::span<const Vector3f> positions = positionsOf(mesh);
        bool moved = false;
        for (const VertexMotion& m : plan.motions) {
            moved = moved || (m.vertex < positions.size() && !(positions[m.vertex] == m.base));
        }
        if (!moved) {
            mesh = *plan.unchangedAtZero;
            plan.refusal = ModelRefusal::ZeroAmount;
            plan.selection = {}; // it named the elements just put back
            return;
        }
    }
    if (mesh.hasConnectivity()) {
        const Topology& topology = std::as_const(mesh).topology();
        const std::span<const Vector3f> positions = positionsOf(mesh);
        std::vector<u32> live;
        for (const u32 e : plan.touchedEdges) {
            if (e < topology.edgeCount() && !topology.isDeleted(EdgeId(e))) {
                live.push_back(e);
            }
        }
        if (!live.empty()) {
            // The crease rule, then the seam derivation (§3.12). Q7: the rule acts
            // on what the tool made or bent. Where the motions began, a made face
            // has no area yet; an edge whose two faces both had area there and
            // already met over the angle existed so, and keeps its own flag (a
            // soft upright inside an extruded region, EXT-3).
            std::vector<Vector3f> base;
            if (!plan.motions.empty()) {
                base.assign(positions.begin(), positions.end());
                for (const VertexMotion& m : plan.motions) {
                    if (m.vertex < base.size()) {
                        base[m.vertex] = m.base;
                    }
                }
            }
            const auto facing = [](std::span<const Vector3f> loop) {
                f64 longest = 0.0;
                std::array<f64, 3> n{0.0, 0.0, 0.0};
                for (std::size_t i = 0; i < loop.size(); ++i) {
                    const std::array<f64, 3> a = toArray(loop[i]);
                    const std::array<f64, 3> b = toArray(loop[(i + 1) % loop.size()]);
                    n[0] += (a[1] - b[1]) * (a[2] + b[2]);
                    n[1] += (a[2] - b[2]) * (a[0] + b[0]);
                    n[2] += (a[0] - b[0]) * (a[1] + b[1]);
                    longest = std::max(longest, (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                                                    (a[2] - b[2]) * (a[2] - b[2]));
                }
                return std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) > 1e-9 * longest;
            };
            std::vector<u32> hard;
            std::vector<std::pair<u32, u8>> seams;
            for (const u32 e : live) {
                const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
                const FaceId a = topology.face(h);
                const FaceId b = topology.face(Topology::opposite(h));
                if (a.valid() && b.valid()) {
                    const std::vector<u32> la = loopOf(topology, a);
                    const std::vector<u32> lb = loopOf(topology, b);
                    bool existed = false;
                    if (!base.empty()) {
                        const std::vector<Vector3f> ba = placesOf(base, la);
                        const std::vector<Vector3f> bb = placesOf(base, lb);
                        existed = facing(ba) && facing(bb) && angleBetween(newellOf(ba), newellOf(bb)) > creaseAngle;
                    }
                    if (!existed && angleBetween(newellOf(placesOf(positions, la)), newellOf(placesOf(positions, lb))) >
                                        creaseAngle) {
                        hard.push_back(e);
                    }
                }
                seams.push_back({e, SeamBetween(mesh, h) ? u8{1} : u8{0}});
            }
            if (!hard.empty()) {
                const std::span<u8> sharp = mesh.attributes.getOrCreate<u8>(names::kSharp, Domain::Edge, AttrType::Bool);
                for (const u32 e : hard) {
                    sharp[e] = 1;
                }
            }
            bool anySeam = mesh.attributes.has(names::kSeam, Domain::Edge);
            for (const auto& entry : seams) {
                anySeam = anySeam || entry.second != 0;
            }
            if (anySeam) {
                // Added where the corners now split, never cleared: a seam marked
                // by hand ahead of an unwrap has no split to derive it from.
                const std::span<u8> seam = mesh.attributes.getOrCreate<u8>(names::kSeam, Domain::Edge, AttrType::Bool);
                for (const auto& [e, value] : seams) {
                    seam[e] = seam[e] != 0 || value != 0 ? 1 : 0;
                }
            }
        }
        std::vector<u32> around;
        for (const u32 e : live) {
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            around.push_back(topology.from(h).value());
            around.push_back(topology.to(h).value());
        }
        for (const VertexMotion& m : plan.motions) {
            around.push_back(m.vertex);
        }
        around.insert(around.end(), plan.reshade.begin(), plan.reshade.end());
        reshade(mesh, facesAround(mesh, sortedUnique(std::move(around))));
    }
    const CanonicalRemap remap = Canonicalize(mesh);
    if (!remap.identity()) {
        plan.selection.vertices = remapList(plan.selection.vertices, remap.vertices);
        plan.selection.edges = remapList(plan.selection.edges, remap.edges);
        plan.selection.faces = remapList(plan.selection.faces, remap.faces);
        plan.touchedEdges = remapList(plan.touchedEdges, remap.edges);
        plan.changedFaces = remapList(plan.changedFaces, remap.faces);
    }
    MaterialiseRows(mesh);
    mesh.recomputeBounds();
}

// ============================================================================
// To Quads and To Polygons (§3.2, §3.3)
// ============================================================================

namespace {

std::vector<u8> faceMask(const Mesh& mesh, const ElementSet& faces) {
    std::vector<u8> mask(mesh.topology().faceCount(), 0);
    for (const u32 f : faces.faces) {
        if (f < mask.size() && !mesh.topology().isDeleted(FaceId(f))) {
            mask[f] = 1;
        }
    }
    return mask;
}

/// The cost of joining triangles (a, b, c) and (b, a, d) across a-b into the
/// quad a, d, b, c: how far it is from flat, from rectangular, and from
/// convex, each on [0, 1]. The three-term sum Blender's join ranks by (R§2.2).
struct QuadShape {
    bool convex = false;
    f64 cost = 0.0;
    f64 worstCorner = 0.0; ///< The largest |corner angle - 90 degrees|.
    f64 bend = 0.0;        ///< The angle between the two triangles' normals.
};

std::array<f64, 3> triangleNormal(const Vector3f& a, const Vector3f& b, const Vector3f& c) {
    const Vector3f u = sub(b, a);
    const Vector3f w = sub(c, a);
    std::array<f64, 3> n{static_cast<f64>(u.y) * w.z - static_cast<f64>(u.z) * w.y,
                         static_cast<f64>(u.z) * w.x - static_cast<f64>(u.x) * w.z,
                         static_cast<f64>(u.x) * w.y - static_cast<f64>(u.y) * w.x};
    const f64 l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0.0) {
        for (f64& x : n) {
            x /= l;
        }
    }
    return n;
}

f64 triangleArea(const Vector3f& a, const Vector3f& b, const Vector3f& c) {
    const Vector3f u = sub(b, a);
    const Vector3f w = sub(c, a);
    const f64 x = static_cast<f64>(u.y) * w.z - static_cast<f64>(u.z) * w.y;
    const f64 y = static_cast<f64>(u.z) * w.x - static_cast<f64>(u.x) * w.z;
    const f64 z = static_cast<f64>(u.x) * w.y - static_cast<f64>(u.y) * w.x;
    return 0.5 * std::sqrt(x * x + y * y + z * z);
}

QuadShape quadShape(const std::array<Vector3f, 4>& q) {
    constexpr f64 pi = 3.14159265358979323846;
    QuadShape out;
    const std::array<f64, 3> n = newellOf(q);
    // Convex, no bowtie: every corner turns the Newell normal's way.
    out.convex = true;
    for (u32 i = 0; i < 4; ++i) {
        const Vector3f& p = q[(i + 3) % 4];
        const Vector3f& c = q[i];
        const Vector3f& x = q[(i + 1) % 4];
        const std::array<f64, 3> t = triangleNormal(p, c, x);
        out.convex = out.convex && (t[0] * n[0] + t[1] * n[1] + t[2] * n[2]) > 0.0;
    }
    // Flatness: the two ways of splitting it, each triangle pair's bend.
    const f64 bend02 = angleBetween(triangleNormal(q[0], q[1], q[2]), triangleNormal(q[0], q[2], q[3]));
    const f64 bend13 = angleBetween(triangleNormal(q[1], q[2], q[3]), triangleNormal(q[1], q[3], q[0]));
    out.bend = std::max(bend02, bend13);
    const f64 flatness = (bend02 + bend13) / (2.0 * pi);
    // Rectangularity: each corner's distance from a right angle.
    f64 right = 0.0;
    for (u32 i = 0; i < 4; ++i) {
        const Vector3f a = sub(q[(i + 3) % 4], q[i]);
        const Vector3f b = sub(q[(i + 1) % 4], q[i]);
        const f64 angle = angleBetween(toArray(a), toArray(b));
        const f64 off = std::abs(angle - pi / 2.0);
        out.worstCorner = std::max(out.worstCorner, off);
        right += off;
    }
    right /= 2.0 * pi;
    // Concavity: how unequal the two splits' areas are.
    const f64 area02 = triangleArea(q[0], q[1], q[2]) + triangleArea(q[0], q[2], q[3]);
    const f64 area13 = triangleArea(q[1], q[2], q[3]) + triangleArea(q[1], q[3], q[0]);
    const f64 larger = std::max(area02, area13);
    const f64 concave = larger > 0.0 ? 1.0 - std::min(area02, area13) / larger : 1.0;
    out.cost = flatness + right + concave;
    return out;
}

struct Join {
    f64 cost;
    u32 edge;
};

/// Every edge whose two triangles To Quads may join (§3.2), cheapest first.
/// @p chosen, per face, keeps it to those faces; empty is every face.
std::vector<Join> joinCandidates(const Mesh& mesh, const std::vector<u8>& chosen, const JoinTrianglesParams& params) {
    const std::span<const Vector3f> positions = positionsOf(mesh);
    const Topology& topology = mesh.topology();
    const auto wanted = [&](FaceId f) { return chosen.empty() || chosen[f.index()] != 0; };
    std::vector<Join> candidates;
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        const EdgeId edge(e);
        if (topology.isDeleted(edge) || topology.isBoundary(edge)) {
            continue;
        }
        const HalfedgeId h0 = Topology::halfedge(edge, 0);
        const HalfedgeId h1 = Topology::halfedge(edge, 1);
        const FaceId fa = topology.face(h0);
        const FaceId fb = topology.face(h1);
        if (!wanted(fa) || !wanted(fb) || topology.valence(fa) != 3 || topology.valence(fb) != 3 ||
            !joinable(mesh, edge, params.delimitFlagged)) {
            continue;
        }
        const u32 a = topology.from(h0).value();
        const u32 b = topology.to(h0).value();
        const u32 c = topology.to(topology.next(h0)).value();
        const u32 d = topology.to(topology.next(h1)).value();
        const std::array<Vector3f, 4> quad{positions[a], positions[d], positions[b], positions[c]};
        const QuadShape shape = quadShape(quad);
        const f64 faceBend = angleBetween(triangleNormal(positions[a], positions[b], positions[c]),
                                          triangleNormal(positions[b], positions[a], positions[d]));
        if (!shape.convex || faceBend > params.faceAngle || shape.worstCorner > params.shapeAngle) {
            continue;
        }
        candidates.push_back({shape.cost, e});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Join& x, const Join& y) {
        return x.cost != y.cost ? x.cost < y.cost : x.edge < y.edge;
    });
    return candidates;
}

} // namespace

std::vector<u8> JoinDiagonals(const Mesh& mesh) {
    const JoinTrianglesParams params;
    std::vector<u8> out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    // A mask even where nothing pairs: it is what tells a walker the mesh has
    // triangles to pass.
    for (u32 f = 0; f < topology.faceCount() && out.empty(); ++f) {
        if (!topology.isDeleted(FaceId(f)) && topology.valence(FaceId(f)) == 3) {
            out.assign(topology.edgeCount(), 0);
        }
    }
    std::vector<u8> taken(topology.faceCount(), 0);
    for (const Join& candidate : joinCandidates(mesh, {}, params)) {
        const HalfedgeId h0 = Topology::halfedge(EdgeId(candidate.edge), 0);
        const HalfedgeId h1 = Topology::halfedge(EdgeId(candidate.edge), 1);
        const FaceId fa = topology.face(h0);
        const FaceId fb = topology.face(h1);
        // Two triangles on the same three vertices make no quad: the join skips them too.
        if (taken[fa.index()] != 0 || taken[fb.index()] != 0 ||
            topology.to(topology.next(h0)) == topology.to(topology.next(h1))) {
            continue;
        }
        taken[fa.index()] = 1;
        taken[fb.index()] = 1;
        out[candidate.edge] = 1;
    }
    return out;
}

ModelPlan PlanJoinTriangles(Mesh& mesh, const PointTable& points, const ElementSet& faces,
                            const JoinTrianglesParams& params) {
    (void)points;
    ModelPlan plan;
    if (faces.faces.empty()) {
        // A mesh with no face has nothing to join: a no-op, so the Mesh level
        // running it on every mesh is not refused (JQ-2).
        if (mesh.faceSet().faceCount() == 0) {
            plan.renumbers = false;
            return plan;
        }
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Mesh& readable = mesh;
    const std::vector<Join> candidates = joinCandidates(readable, faceMask(readable, faces), params);

    std::vector<u8> taken(readable.topology().faceCount(), 0);
    std::unordered_map<u32, std::vector<u32>> rows;
    std::vector<u32> cleared;
    std::vector<u32> joined;
    for (const Join& candidate : candidates) {
        const Topology& topology = readable.topology();
        const EdgeId edge(candidate.edge);
        if (topology.isDeleted(edge)) {
            continue;
        }
        const FaceId fa = topology.face(Topology::halfedge(edge, 0));
        const FaceId fb = topology.face(Topology::halfedge(edge, 1));
        if (taken[fa.index()] != 0 || taken[fb.index()] != 0) {
            continue;
        }
        const FaceId lower = fa.value() < fb.value() ? fa : fb;
        detail::Dissolved dissolved = detail::DissolveEdgeDeferred(mesh, edge, lower);
        if (!dissolved.ok) {
            continue; // a duplicate face, or a repeated vertex: skipped
        }
        taken[fa.index()] = 1;
        taken[fb.index()] = 1;
        rows[dissolved.kept.value()] = std::move(dissolved.joined);
        cleared.push_back(dissolved.gone.value());
        joined.push_back(dissolved.kept.value());
        plan.dissolvedEdges.push_back(edge.value());
    }
    if (!joined.empty()) {
        storeRows(mesh, rows, cleared);
    }
    plan.changed = static_cast<u32>(joined.size());
    if (params.deselectJoined) {
        for (const u32 f : faces.faces) {
            if (f < taken.size() && taken[f] == 0 && !readable.topology().isDeleted(FaceId(f))) {
                plan.selection.faces.push_back(f);
            }
        }
    } else {
        plan.selection.faces = joined;
    }
    plan.selection.normalise();
    plan.renumbers = !joined.empty();
    return plan;
}

ModelPlan PlanLimitedDissolve(Mesh& mesh, const PointTable& points, const ElementSet& faces,
                              const LimitedDissolveParams& params) {
    (void)points;
    ModelPlan plan;
    if (faces.faces.empty()) {
        // A mesh with no face has nothing to join: a no-op, so the Mesh level
        // running it on every mesh is not refused (JQ-2).
        if (mesh.faceSet().faceCount() == 0) {
            plan.renumbers = false;
            return plan;
        }
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Mesh& readable = mesh;
    std::vector<u8> chosen = faceMask(readable, faces);
    const std::span<const Vector3f> positions = positionsOf(readable);
    std::vector<std::array<f64, 3>> normals(readable.topology().faceCount());
    std::vector<u8> measured(normals.size(), 0);
    const auto normalOf = [&](FaceId f) {
        if (measured[f.index()] == 0) {
            normals[f.index()] = newellOf(placesOf(positions, loopOf(readable.topology(), f)));
            measured[f.index()] = 1;
        }
        return normals[f.index()];
    };
    // Costs are static (the dihedral between two faces does not change when a
    // third merges elsewhere); equal costs are decided as the region grows.
    struct Dissolve {
        f64 cost;
        u32 edge;
    };
    std::vector<Dissolve> candidates;
    {
        const Topology& topology = readable.topology();
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            const EdgeId edge(e);
            if (topology.isDeleted(edge) || topology.isBoundary(edge)) {
                continue;
            }
            const FaceId fa = topology.face(Topology::halfedge(edge, 0));
            const FaceId fb = topology.face(Topology::halfedge(edge, 1));
            if (chosen[fa.index()] == 0 || chosen[fb.index()] == 0 ||
                !joinable(readable, edge, params.delimitFlagged)) {
                continue;
            }
            const f64 cost = angleBetween(normalOf(fa), normalOf(fb));
            if (cost <= params.angle) {
                candidates.push_back({cost, e});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Dissolve& x, const Dissolve& y) {
        return x.cost != y.cost ? x.cost < y.cost : x.edge < y.edge;
    });

    std::vector<u32> cleared;
    std::vector<u32> merged;
    // Among equal costs: prefer leaving each end's two last edges straight.
    const auto penaltyOf = [&](EdgeId edge) {
        const Topology& topology = readable.topology();
        f64 penalty = 0.0;
        for (u32 side = 0; side < 2; ++side) {
            const VertexId v = side == 0 ? topology.from(Topology::halfedge(edge, 0))
                                         : topology.to(Topology::halfedge(edge, 0));
            std::array<VertexId, 2> others;
            u32 count = 0;
            for (const HalfedgeId h : topology.voh(v)) {
                if (Topology::edge(h) != edge && count++ < 2) {
                    others[count - 1] = topology.to(h);
                }
            }
            if (count == 2) {
                const std::array<f64, 3> u = toArray(sub(positions[others[0].value()], positions[v.value()]));
                const std::array<f64, 3> w = toArray(sub(positions[others[1].value()], positions[v.value()]));
                penalty += std::abs(3.14159265358979323846 - angleBetween(u, w));
            }
        }
        return penalty;
    };
    // The joins (JP-1) as a union-find over face slots: a region's root is the
    // face each join keeps, the lower slot. An absorbed face's halfedges keep
    // its old slot until the end, so reads go through `faceOf`; a region's row
    // is its faces' own rows chained lower first (§2.3) and its halfedges are
    // named for it once, when every join is done.
    const u32 slots = readable.topology().faceCount();
    std::vector<u32> parent(slots);
    std::iota(parent.begin(), parent.end(), 0u);
    const auto rootOf = [&](u32 f) {
        while (parent[f] != f) {
            parent[f] = parent[parent[f]];
            f = parent[f];
        }
        return f;
    };
    const auto faceOf = [&](HalfedgeId h) {
        const FaceId f = readable.topology().face(h);
        return f.valid() ? FaceId(rootOf(f.value())) : f;
    };
    std::vector<std::vector<u32>> own(slots);      // per slot, its own drawn row
    std::vector<u32> chainNext(slots, kInvalidId); // the next slot of its region's row
    std::vector<u32> chainTail(slots);             // per root, its row's last slot
    std::iota(chainTail.begin(), chainTail.end(), 0u);
    const auto rowOf = [&](u32 root, std::vector<u32>& out) {
        for (u32 f = root; f != kInvalidId; f = chainNext[f]) {
            out.insert(out.end(), own[f].begin(), own[f].end());
        }
    };
    const auto unionOf = [&](u32 a, u32 b) {
        std::vector<u32> row;
        rowOf(std::min(a, b), row);
        rowOf(std::max(a, b), row);
        return row;
    };
    // Per root, the ranking's cheap facts: its corners, sorted, for a repeat;
    // its Newell vector, which a join adds; a cone round its normal holding
    // every drawn triangle, so the flatness cap is a bound unless the bound is
    // not enough; and what says its row stays a valid cut (`RowValid`) without
    // measuring it -- the row is one, its thinnest triangle, its longest side.
    std::vector<std::vector<u32>> cornerSet(slots);
    std::vector<std::array<f64, 3>> newell(slots, {0.0, 0.0, 0.0});
    std::vector<std::array<f64, 3>> axis(slots, {0.0, 0.0, 1.0});
    std::vector<f64> spread(slots, 0.0);
    std::vector<u8> sound(slots, 0);
    std::vector<f64> thinnest(slots, 0.0); // the least |cross| of a triangle of its row
    std::vector<f64> longest(slots, 0.0);  // its loop's longest side, squared
    const auto unit = [](const std::array<f64, 3>& v) {
        const f64 length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return length > 0.0 ? std::array<f64, 3>{v[0] / length, v[1] / length, v[2] / length} : v;
    };
    for (u32 f = 0; f < slots; ++f) {
        if (readable.topology().isDeleted(FaceId(f))) {
            continue;
        }
        const std::vector<u32> loop = loopOf(readable.topology(), FaceId(f));
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const Vector3f& a = positions[loop[i]];
            const Vector3f& b = positions[loop[(i + 1) % loop.size()]];
            newell[f][0] += (static_cast<f64>(a.y) - b.y) * (static_cast<f64>(a.z) + b.z);
            newell[f][1] += (static_cast<f64>(a.z) - b.z) * (static_cast<f64>(a.x) + b.x);
            newell[f][2] += (static_cast<f64>(a.x) - b.x) * (static_cast<f64>(a.y) + b.y);
            const std::array<f64, 3> side = toArray(sub(b, a));
            longest[f] = std::max(longest[f], side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        }
        cornerSet[f] = loop;
        std::sort(cornerSet[f].begin(), cornerSet[f].end());
        own[f] = loop.size() == 3 ? loop : drawnOf(readable, FaceId(f));
        axis[f] = unit(newell[f]);
        thinnest[f] = FLT_MAX;
        for (std::size_t t = 0; t + 2 < own[f].size(); t += 3) {
            const Vector3f& a = positions[own[f][t]];
            const Vector3f& b = positions[own[f][t + 1]];
            const Vector3f& c = positions[own[f][t + 2]];
            spread[f] = std::max(spread[f], angleBetween(axis[f], triangleNormal(a, b, c)));
            const std::array<f64, 3> u = toArray(sub(b, a));
            const std::array<f64, 3> w = toArray(sub(c, a));
            const f64 x = u[1] * w[2] - u[2] * w[1];
            const f64 y = u[2] * w[0] - u[0] * w[2];
            const f64 z = u[0] * w[1] - u[1] * w[0];
            thinnest[f] = std::min(thinnest[f], std::sqrt(x * x + y * y + z * z));
        }
        sound[f] = loop.size() == 3 ? (thinnest[f] > 0.0 ? 1 : 0) : (RowValid(loop, positions, own[f]) ? 1 : 0);
    }
    // Refused now, refused later too: the faces only grow. @p bound: how far
    // the union's triangles may stray from its normal, exact when the cones
    // were not enough.
    const auto quick = [&](u32 e, f64& penalty, f64& bound) {
        const Topology& topology = readable.topology();
        const EdgeId edge(e);
        if (topology.isDeleted(edge) || topology.isBoundary(edge)) {
            return false;
        }
        const u32 a = faceOf(Topology::halfedge(edge, 0)).value();
        const u32 b = faceOf(Topology::halfedge(edge, 1)).value();
        if (a == b) {
            return false;
        }
        const std::vector<u32>& fewer = cornerSet[a].size() <= cornerSet[b].size() ? cornerSet[a] : cornerSet[b];
        const std::vector<u32>& more = cornerSet[a].size() <= cornerSet[b].size() ? cornerSet[b] : cornerSet[a];
        u32 shared = 0;
        for (const u32 v : fewer) {
            shared += std::binary_search(more.begin(), more.end(), v) ? 1u : 0u;
        }
        if (shared > 2) {
            return false; // the joined loop would repeat a vertex
        }
        const std::array<f64, 3> n =
            unit({newell[a][0] + newell[b][0], newell[a][1] + newell[b][1], newell[a][2] + newell[b][2]});
        bound = std::max(angleBetween(axis[a], n) + spread[a], angleBetween(axis[b], n) + spread[b]);
        if (!(bound <= params.angle)) {
            const std::vector<u32> row = unionOf(a, b);
            bound = 0.0;
            for (std::size_t t = 0; t + 2 < row.size(); t += 3) {
                const f64 off =
                    angleBetween(n, triangleNormal(positions[row[t]], positions[row[t + 1]], positions[row[t + 2]]));
                if (off > params.angle) {
                    return false;
                }
                bound = std::max(bound, off);
            }
        }
        penalty = penaltyOf(edge);
        return true;
    };
    // The join `quick` just passed: the row checked, then the edge's two
    // halfedges unlinked as `DissolveEdgeDeferred` does, the faces' pointers
    // left for the end.
    const auto join = [&](u32 e, f64 bound) {
        const EdgeId edge(e);
        const u32 a = faceOf(Topology::halfedge(edge, 0)).value();
        const u32 b = faceOf(Topology::halfedge(edge, 1)).value();
        const u32 kept = std::min(a, b);
        const u32 gone = std::max(a, b);
        // Two valid cuts sharing one side are one valid cut of the joined loop,
        // that side a diagonal, as long as every triangle still faces the
        // joined plane by `RowValid`'s own margin; else it is measured.
        const f64 least = std::min(thinnest[a], thinnest[b]);
        const f64 side = std::max(longest[a], longest[b]);
        const bool sure = sound[a] != 0 && sound[b] != 0 && bound < 1.5 &&
                          0.5 * least * std::cos(bound) > 1.001e-6 * side;
        if (!sure && !RowValid(mergedLoop(readable.topology(), edge), positions, unionOf(a, b))) {
            return false;
        }
        std::vector<u32> joined;
        joined.reserve(cornerSet[a].size() + cornerSet[b].size());
        std::set_union(cornerSet[a].begin(), cornerSet[a].end(), cornerSet[b].begin(), cornerSet[b].end(),
                       std::back_inserter(joined));
        {
            // A face with these corners already there would be its duplicate.
            const Topology& topology = readable.topology();
            for (const HalfedgeId h : topology.voh(topology.from(Topology::halfedge(edge, 0)))) {
                const FaceId g = faceOf(h);
                if (g.valid() && g.value() != a && g.value() != b && cornerSet[g.value()].size() == joined.size() &&
                    cornerSet[g.value()] == joined) {
                    return false;
                }
            }
        }
        Topology& topology = mesh.topology();
        const u32 keepSide = faceOf(Topology::halfedge(edge, 1)).value() == kept ? 1u : 0u;
        const HalfedgeId h0 = Topology::halfedge(edge, keepSide);
        const HalfedgeId h1 = Topology::halfedge(edge, keepSide ^ 1u);
        const HalfedgeId p0 = topology.prev(h0);
        const HalfedgeId n0 = topology.next(h0);
        const HalfedgeId p1 = topology.prev(h1);
        const HalfedgeId n1 = topology.next(h1);
        const VertexId va = topology.from(h0);
        const VertexId vb = topology.from(h1);
        topology.setNext(p0, n1);
        topology.setNext(p1, n0);
        topology.setFaceHalfedge(FaceId(kept), n0);
        topology.setStatus(FaceId(gone), topology.status(FaceId(gone)) | Status::Deleted);
        topology.setStatus(edge, topology.status(edge) | Status::Deleted);
        topology.setStatus(h0, topology.status(h0) | Status::Deleted);
        topology.setStatus(h1, topology.status(h1) | Status::Deleted);
        if (topology.outgoing(va) == h0) {
            topology.setOutgoing(va, n1);
        }
        if (topology.outgoing(vb) == h1) {
            topology.setOutgoing(vb, n0);
        }
        topology.adjustOutgoing(va);
        topology.adjustOutgoing(vb);
        parent[gone] = kept;
        chainNext[chainTail[kept]] = gone;
        chainTail[kept] = chainTail[gone];
        cornerSet[kept] = std::move(joined);
        cornerSet[gone] = {};
        for (u32 k = 0; k < 3; ++k) {
            newell[kept][k] += newell[gone][k];
        }
        axis[kept] = unit(newell[kept]);
        spread[kept] = bound;
        sound[kept] = 1;
        thinnest[kept] = least;
        longest[kept] = side;
        cleared.push_back(gone);
        merged.push_back(kept);
        plan.dissolvedEdges.push_back(e);
        return true;
    };
    std::vector<u8> waiting(readable.topology().edgeCount(), 0); // queued in the bucket's heap
    for (std::size_t first = 0; first < candidates.size();) {
        std::size_t last = first + 1;
        while (last < candidates.size() && candidates[last].cost - candidates[first].cost <= 1e-6) {
            ++last;
        }
        // The straightest of the bucket first, lowest id on a tie, from a lazy
        // heap rather than a re-scan of the whole bucket per dissolve (JP-1):
        // an edge's penalty changes only when a dissolve at one of its ends
        // does, and an edge refused once stays refused.
        const auto keyOf = [](f64 penalty) { return static_cast<i64>(std::llround(penalty * 1e9)); };
        using Entry = std::pair<i64, u32>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
        for (std::size_t i = first; i < last; ++i) {
            f64 penalty = 0.0;
            f64 bound = 0.0;
            if (quick(candidates[i].edge, penalty, bound)) {
                heap.push({keyOf(penalty), candidates[i].edge});
                waiting[candidates[i].edge] = 1;
            }
        }
        while (!heap.empty()) {
            const auto [stored, e] = heap.top();
            heap.pop();
            if (waiting[e] == 0) {
                continue;
            }
            f64 penalty = 0.0;
            f64 bound = 0.0;
            if (!quick(e, penalty, bound)) {
                waiting[e] = 0;
                continue;
            }
            if (keyOf(penalty) > stored) {
                heap.push({keyOf(penalty), e});
                continue;
            }
            const Topology& topology = readable.topology();
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            const VertexId ends[2] = {topology.from(h), topology.to(h)};
            waiting[e] = 0;
            if (!join(e, bound)) {
                continue;
            }
            for (const VertexId end : ends) {
                if (readable.topology().isDeleted(end)) {
                    continue;
                }
                for (const HalfedgeId out : readable.topology().voh(end)) {
                    const u32 near = Topology::edge(out).value();
                    f64 now = 0.0;
                    f64 ignored = 0.0;
                    if (waiting[near] != 0 && quick(near, now, ignored)) {
                        heap.push({keyOf(now), near});
                    }
                }
            }
        }
        first = last;
    }
    if (!merged.empty()) {
        Topology& topology = mesh.topology();
        std::unordered_map<u32, std::vector<u32>> rows;
        for (const u32 f : sortedUnique(merged)) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            const HalfedgeId start = topology.halfedge(FaceId(f));
            HalfedgeId walk = start;
            do {
                topology.setFace(walk, FaceId(f));
                walk = topology.next(walk);
            } while (walk != start);
            rowOf(f, rows[f]);
        }
        storeRows(mesh, rows, sortedUnique(cleared));
    }
    for (const u32 f : sortedUnique(merged)) {
        if (!readable.topology().isDeleted(FaceId(f))) {
            plan.selection.faces.push_back(f);
        }
    }
    plan.changed = static_cast<u32>(cleared.size());
    plan.renumbers = !merged.empty();
    return plan;
}

// ============================================================================
// Turn, Edit Triangulation, Retriangulate, Triangulate (§3.4)
// ============================================================================

namespace {

/// @p loop split by non-crossing @p diagonals (ordinal pairs) into cells, each a
/// list of ordinals in loop order.
void splitCells(const std::vector<u32>& cell, std::vector<std::pair<u32, u32>> diagonals,
                std::vector<std::vector<u32>>& out) {
    for (std::size_t d = 0; d < diagonals.size(); ++d) {
        const auto [x, y] = diagonals[d];
        const auto ix = std::find(cell.begin(), cell.end(), x);
        const auto iy = std::find(cell.begin(), cell.end(), y);
        if (ix == cell.end() || iy == cell.end()) {
            continue;
        }
        std::size_t i = static_cast<std::size_t>(ix - cell.begin());
        std::size_t j = static_cast<std::size_t>(iy - cell.begin());
        if (i > j) {
            std::swap(i, j);
        }
        if (j - i < 2 || (i == 0 && j == cell.size() - 1)) {
            continue; // a side of this cell
        }
        std::vector<u32> left(cell.begin() + static_cast<std::ptrdiff_t>(i),
                              cell.begin() + static_cast<std::ptrdiff_t>(j) + 1);
        std::vector<u32> right(cell.begin() + static_cast<std::ptrdiff_t>(j), cell.end());
        right.insert(right.end(), cell.begin(), cell.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        diagonals.erase(diagonals.begin() + static_cast<std::ptrdiff_t>(d));
        splitCells(left, diagonals, out);
        splitCells(right, diagonals, out);
        return;
    }
    out.push_back(cell);
}

/// Whether chords (a, b) and (c, d) of an n-gon cross, by ordinals.
bool chordsCross(u32 a, u32 b, u32 c, u32 d) {
    if (a > b) {
        std::swap(a, b);
    }
    if (a == c || a == d || b == c || b == d) {
        return false;
    }
    const bool cInside = c > a && c < b;
    const bool dInside = d > a && d < b;
    return cInside != dInside;
}

/// @p cut's diagonals, as ordinal pairs (low, high).
std::vector<std::pair<u32, u32>> diagonalsOf(std::span<const u32> cut, u32 n) {
    std::vector<std::pair<u32, u32>> out;
    for (std::size_t t = 0; t + 2 < cut.size(); t += 3) {
        for (u32 side = 0; side < 3; ++side) {
            const u32 a = cut[t + side];
            const u32 b = cut[t + (side + 1) % 3];
            if (a < b && b != a + 1 && !(a == 0 && b == n - 1)) {
                out.push_back({a, b});
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool liveFace(const Mesh& mesh, FaceId face) {
    return mesh.hasConnectivity() && face.valid() && face.index() < mesh.topology().faceCount() &&
           !mesh.topology().isDeleted(face);
}

} // namespace

ModelPlan PlanTurn(Mesh& mesh, FaceId face, VertexId a, VertexId b) {
    ModelPlan plan;
    plan.renumbers = false;
    if (!liveFace(mesh, face)) {
        plan.refusal = ModelRefusal::NotOneFace;
        return plan;
    }
    const Mesh& readable = mesh;
    const std::vector<u32> loop = loopOf(readable.topology(), face);
    if (std::find(loop.begin(), loop.end(), a.value()) == loop.end() ||
        std::find(loop.begin(), loop.end(), b.value()) == loop.end()) {
        plan.refusal = ModelRefusal::NotOneFace;
        return plan;
    }
    std::vector<u32> row = drawnOf(readable, face);
    // The two triangles on the diagonal: (a, b, x) and (b, a, y).
    std::size_t first = row.size();
    std::size_t second = row.size();
    u32 x = kInvalidId;
    u32 y = kInvalidId;
    for (std::size_t t = 0; t + 2 < row.size(); t += 3) {
        for (u32 s = 0; s < 3; ++s) {
            const u32 p = row[t + s];
            const u32 q = row[t + (s + 1) % 3];
            const u32 r = row[t + (s + 2) % 3];
            if (p == a.value() && q == b.value()) {
                first = t;
                x = r;
            } else if (p == b.value() && q == a.value()) {
                second = t;
                y = r;
            }
        }
    }
    if (first == row.size() || second == row.size()) {
        plan.refusal = ModelRefusal::DiagonalOutside;
        return plan;
    }
    row[first] = x;
    row[first + 1] = a.value();
    row[first + 2] = y;
    row[second] = y;
    row[second + 1] = b.value();
    row[second + 2] = x;
    if (!RowValid(loop, positionsOf(readable), row)) {
        plan.refusal = ModelRefusal::ConcaveTurn;
        return plan;
    }
    mesh.triangulation.setRow(face.value(), row, readable.topology().faceCount());
    plan.changed = 1;
    plan.selection.faces = {face.value()};
    return plan;
}

ModelPlan PlanEditTriangulation(Mesh& mesh, FaceId face, VertexId a, VertexId b) {
    ModelPlan plan;
    plan.renumbers = false;
    if (!liveFace(mesh, face)) {
        plan.refusal = ModelRefusal::NotOneFace;
        return plan;
    }
    const Mesh& readable = mesh;
    const std::vector<u32> loop = loopOf(readable.topology(), face);
    const u32 n = static_cast<u32>(loop.size());
    const auto ia = std::find(loop.begin(), loop.end(), a.value());
    const auto ib = std::find(loop.begin(), loop.end(), b.value());
    if (ia == loop.end() || ib == loop.end()) {
        plan.refusal = ModelRefusal::NotOneFace;
        return plan;
    }
    u32 i = static_cast<u32>(ia - loop.begin());
    u32 j = static_cast<u32>(ib - loop.begin());
    if (i > j) {
        std::swap(i, j);
    }
    if (i == j || j == i + 1 || (i == 0 && j == n - 1)) {
        plan.refusal = ModelRefusal::AdjacentCorners;
        return plan;
    }
    const std::span<const Vector3f> positions = positionsOf(readable);
    std::vector<u32> cut;
    TriangulateFace(loop, positions, readable.triangulation.row(face.value()), cut);
    std::vector<std::pair<u32, u32>> kept{{i, j}};
    for (const auto& [c, d] : diagonalsOf(cut, n)) {
        if ((c != i || d != j) && !chordsCross(i, j, c, d)) {
            kept.push_back({c, d});
        }
    }
    std::vector<u32> whole(n);
    std::iota(whole.begin(), whole.end(), 0u);
    std::vector<std::vector<u32>> cells;
    splitCells(whole, kept, cells);
    std::vector<u32> row;
    for (const std::vector<u32>& cell : cells) {
        std::vector<u32> cellLoop;
        for (const u32 o : cell) {
            cellLoop.push_back(loop[o]);
        }
        std::vector<u32> cellCut;
        TriangulateFace(cellLoop, positions, {}, cellCut);
        for (const u32 o : cellCut) {
            row.push_back(cellLoop[o]);
        }
    }
    if (!RowValid(loop, positions, row)) {
        plan.refusal = ModelRefusal::DiagonalOutside;
        return plan;
    }
    mesh.triangulation.setRow(face.value(), row, readable.topology().faceCount());
    plan.changed = 1;
    plan.selection.faces = {face.value()};
    return plan;
}

ModelPlan PlanRetriangulate(Mesh& mesh, const ElementSet& faces) {
    ModelPlan plan;
    plan.renumbers = false;
    if (faces.faces.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Mesh& readable = mesh;
    const std::span<const Vector3f> positions = positionsOf(readable);
    std::unordered_map<u32, std::vector<u32>> rows;
    for (const u32 f : faces.faces) {
        if (!liveFace(readable, FaceId(f)) || readable.topology().valence(FaceId(f)) < 4) {
            continue;
        }
        const std::vector<u32> loop = loopOf(readable.topology(), FaceId(f));
        std::vector<u32> cut;
        TriangulateFace(loop, positions, {}, cut);
        for (u32& corner : cut) {
            corner = loop[corner];
        }
        rows[f] = std::move(cut);
    }
    storeRows(mesh, rows, {});
    plan.changed = static_cast<u32>(rows.size());
    plan.selection.faces = faces.faces;
    plan.selection.normalise();
    return plan;
}

ModelPlan PlanTriangulate(Mesh& mesh, const ElementSet& faces) {
    ModelPlan plan;
    if (faces.faces.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Mesh& readable = mesh;
    for (const u32 f : sortedUnique(faces.faces)) {
        if (!liveFace(readable, FaceId(f))) {
            continue;
        }
        plan.selection.faces.push_back(f);
        if (readable.topology().valence(FaceId(f)) < 4) {
            continue;
        }
        // A diagonal that already is an edge elsewhere cannot be cut (§2.7.8):
        // the face stays a polygon with its row.
        const std::vector<u32> loop = loopOf(readable.topology(), FaceId(f));
        std::vector<u32> cut;
        TriangulateFace(loop, positionsOf(readable), readable.triangulation.row(f), cut);
        bool blocked = false;
        for (const auto& [x, y] : diagonalsOf(cut, static_cast<u32>(loop.size()))) {
            blocked = blocked ||
                      readable.topology().findHalfedge(VertexId(loop[x]), VertexId(loop[y])).valid();
        }
        if (blocked) {
            ++plan.kept;
            continue;
        }
        const u32 before = readable.topology().faceCount();
        plan.changed += Triangulate(mesh, FaceId(f));
        for (u32 added = before; added < readable.topology().faceCount(); ++added) {
            plan.selection.faces.push_back(added);
        }
    }
    plan.selection.normalise();
    plan.renumbers = plan.changed != 0;
    return plan;
}

// ============================================================================
// Target Weld and Weld (§3.6)
// ============================================================================

namespace {

std::vector<u32> membersOfPoint(const Mesh& mesh, const PointTable& points, u32 vertex) {
    const u32 vertexCount = mesh.topology().vertexCount();
    if (points.pointOf.size() != vertexCount || vertex >= vertexCount) {
        return {vertex};
    }
    const std::span<const u32> span = points.membersOf(points.pointOf[vertex]);
    std::vector<u32> out;
    for (const u32 m : span) {
        if (!mesh.topology().isDeleted(VertexId(m))) {
            out.push_back(m);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Why a part whose @p survivors the trial repair named is left: `WouldPinch`
/// when one of them would hold two fans (Q6), `WouldDuplicateFace` when two
/// faces round one would hold the same corners, else the general case.
ModelRefusal whyNotCollapsed(const Candidate& candidate, const std::vector<u32>& survivors) {
    const FaceSet& faces = candidate.faces;
    std::vector<u32> base(faces.faceCount() + 1, 0);
    for (u32 f = 0; f < faces.faceCount(); ++f) {
        base[f + 1] = base[f] + faces.faceValence[f];
    }
    bool duplicate = false;
    for (const u32 s : survivors) {
        // The faces round the survivor, each with the corners before and after it.
        std::vector<std::array<u32, 3>> round;
        for (u32 f = 0; f < faces.faceCount(); ++f) {
            const u32 valence = faces.faceValence[f];
            for (u32 k = 0; k < valence; ++k) {
                if (faces.cornerVertex[base[f] + k] == s) {
                    round.push_back({f, faces.cornerVertex[base[f] + (k + valence - 1) % valence],
                                     faces.cornerVertex[base[f] + (k + 1) % valence]});
                }
            }
        }
        std::vector<u32> parent(round.size());
        std::iota(parent.begin(), parent.end(), 0u);
        const auto find = [&](u32 i) {
            while (parent[i] != i) {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }
            return i;
        };
        for (u32 i = 0; i < round.size(); ++i) {
            for (u32 j = 0; j < round.size(); ++j) {
                if (i != j && round[i][2] == round[j][1]) {
                    parent[find(i)] = find(j); // the two faces either side of one edge at s
                }
            }
        }
        u32 fans = 0;
        for (u32 i = 0; i < round.size(); ++i) {
            fans += find(i) == i ? 1u : 0u;
        }
        if (fans > 1) {
            return ModelRefusal::WouldPinch;
        }
        for (u32 i = 0; i < round.size() && !duplicate; ++i) {
            for (u32 j = i + 1; j < round.size() && !duplicate; ++j) {
                const u32 a = round[i][0];
                const u32 b = round[j][0];
                if (a == b || faces.faceValence[a] != faces.faceValence[b]) {
                    continue;
                }
                std::vector<u32> left(faces.cornerVertex.begin() + base[a], faces.cornerVertex.begin() + base[a + 1]);
                std::vector<u32> right(faces.cornerVertex.begin() + base[b], faces.cornerVertex.begin() + base[b + 1]);
                std::sort(left.begin(), left.end());
                std::sort(right.begin(), right.end());
                duplicate = left == right;
            }
        }
    }
    return duplicate ? ModelRefusal::WouldDuplicateFace : ModelRefusal::CollapseIllegal;
}

/// The edges a rename folds together: a face it shrinks to two corners goes,
/// and its sides become one edge between them. The rebuild combines a closed
/// edge's two sides, but a side on the border has no corner to carry it, so
/// each fold's Edge layers are gathered here (Bool ORs, F32 takes the maximum)
/// and written on the edge the rebuild made (COL-8).
struct Fold {
    u32 a = kInvalidId;
    u32 b = kInvalidId;
    std::vector<std::vector<u8>> values; ///< Per layer of `Folds::layers`.
};

struct Folds {
    std::vector<std::string> layers; ///< The Edge layers, by name.
    std::vector<Fold> folds;
};

Folds foldsOf(const Mesh& mesh, const std::vector<u32>& survivorOf, const Candidate& candidate) {
    const Topology& topology = mesh.topology();
    std::vector<u8> kept(topology.faceCount(), 0);
    for (const u32 f : candidate.sourceFace) {
        if (f < kept.size()) {
            kept[f] = 1;
        }
    }
    std::vector<const AttrLayer*> layers;
    Folds folds;
    for (const AttrLayer& layer : mesh.attributes.layers()) {
        if (layer.domain == Domain::Edge) {
            layers.push_back(&layer);
            folds.layers.push_back(layer.name);
        }
    }
    std::vector<Fold>& out = folds.folds;
    if (layers.empty()) {
        return folds;
    }
    std::unordered_map<u64, u32> foldOf;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (kept[f] != 0 || topology.isDeleted(FaceId(f))) {
            continue;
        }
        std::vector<u32> distinct;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            distinct.push_back(survivorOf[topology.from(h).value()]);
        }
        distinct = sortedUnique(std::move(distinct));
        if (distinct.size() != 2) {
            continue;
        }
        const u64 key = (static_cast<u64>(distinct[0]) << 32) | distinct[1];
        const auto [at, added] = foldOf.try_emplace(key, static_cast<u32>(out.size()));
        if (added) {
            Fold fold;
            fold.a = distinct[0];
            fold.b = distinct[1];
            for (const AttrLayer* layer : layers) {
                fold.values.emplace_back(AttrTypeSize(layer->type), u8{0});
            }
            out.push_back(std::move(fold));
        }
        Fold& fold = out[at->second];
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            if (survivorOf[topology.from(h).value()] == survivorOf[topology.to(h).value()]) {
                continue;
            }
            const std::size_t e = Topology::edge(h).index();
            for (std::size_t l = 0; l < layers.size(); ++l) {
                const std::size_t stride = fold.values[l].size();
                if (stride * (e + 1) > layers[l]->data.size()) {
                    continue;
                }
                const u8* in = layers[l]->data.data() + stride * e;
                u8* acc = fold.values[l].data();
                if (layers[l]->type == AttrType::Bool) {
                    acc[0] = acc[0] != 0 || in[0] != 0 ? 1 : 0;
                } else if (layers[l]->type == AttrType::F32) {
                    f32 x = 0.0f, y = 0.0f;
                    std::memcpy(&x, acc, sizeof(f32));
                    std::memcpy(&y, in, sizeof(f32));
                    const f32 larger = std::max(x, y);
                    std::memcpy(acc, &larger, sizeof(f32));
                }
            }
        }
    }
    return folds;
}

void writeFolds(Mesh& mesh, const Folds& folds, const std::vector<u32>& newId) {
    const Topology& topology = std::as_const(mesh).topology();
    for (const Fold& fold : folds.folds) {
        const u32 a = newId[fold.a];
        const u32 b = newId[fold.b];
        if (a == kInvalidId || b == kInvalidId) {
            continue;
        }
        const HalfedgeId h = topology.findHalfedge(VertexId(a), VertexId(b));
        if (!h.valid()) {
            continue;
        }
        const std::size_t e = Topology::edge(h).index();
        for (std::size_t l = 0; l < folds.layers.size(); ++l) {
            AttrLayer* found = mesh.attributes.layer(folds.layers[l], Domain::Edge);
            if (found == nullptr) {
                continue;
            }
            AttrLayer& target = *found;
            const std::vector<u8>& value = fold.values[l];
            if (value.size() != AttrTypeSize(target.type) || value.size() * (e + 1) > target.data.size()) {
                continue;
            }
            u8* out = target.data.data() + value.size() * e;
            if (target.type == AttrType::Bool) {
                out[0] = out[0] != 0 || value[0] != 0 ? 1 : 0;
            } else if (target.type == AttrType::F32) {
                f32 x = 0.0f, y = 0.0f;
                std::memcpy(&x, out, sizeof(f32));
                std::memcpy(&y, value.data(), sizeof(f32));
                const f32 larger = std::max(x, y);
                std::memcpy(out, &larger, sizeof(f32));
            }
        }
    }
}

void settleChains(std::vector<u32>& into);

/// A rename's survivor that lands on a vertex of its own point and now sits
/// beside it in a face takes it in (Collapse, Target Weld, Cap): the two would
/// be an edge of no length, and they are one point to the user (Q5, Q8).
/// Only a pair the rename or a move made; the landed one is kept, of two the
/// lower. Beside a vertex kept apart (another stamp) nothing is joined: the
/// landed vertices there are returned, for the caller to refuse (Q8).
std::vector<u32> joinLanded(const Mesh& mesh, const FaceSet& set, const FaceTable& table, std::vector<u32>& into,
                            const std::vector<Vector3f>& places) {
    std::vector<u32> blocked;
    settleChains(into);
    const std::span<const Vector3f> was = positionsOf(mesh);
    const std::span<const u32> stamps = mesh.attributes.get<const u32>(names::kApart, Domain::Vertex);
    const f32 tolerance = CoincidenceTolerance(mesh);
    const auto stampOf = [&](u32 v) { return v < stamps.size() ? stamps[v] : 0u; };
    std::vector<u8> landed(into.size(), 0);
    for (u32 v = 0; v < into.size() && v < places.size() && v < was.size(); ++v) {
        if (into[v] != v) {
            landed[into[v]] = 1;
        } else if ((places[v] - was[v]).length() > 0.0f) {
            landed[v] = 1;
        }
    }
    for (bool again = true; again;) {
        again = false;
        for (u32 f = 0; f < set.faceCount(); ++f) {
            const u32 n = set.faceValence[f];
            for (u32 k = 0; k < n; ++k) {
                const u32 a = into[set.cornerVertex[table.base[f] + k]];
                const u32 b = into[set.cornerVertex[table.base[f] + (k + 1) % n]];
                if (a == b || (landed[a] == 0 && landed[b] == 0) || (places[a] - places[b]).length() > tolerance) {
                    continue;
                }
                if (stampOf(a) != stampOf(b)) {
                    blocked.push_back(landed[a] != 0 ? a : b);
                    continue;
                }
                const u32 keep = landed[a] != landed[b] ? (landed[a] != 0 ? a : b) : std::min(a, b);
                const u32 gone = keep == a ? b : a;
                for (u32& to : into) {
                    to = to == gone ? keep : to;
                }
                landed[keep] = 1;
                again = true;
            }
        }
    }
    return sortedUnique(std::move(blocked));
}

/// A rename, rebuilt -- Target Weld's engine, as Weld's and Collapse's: each
/// vertex becomes `into[v]` (itself when not renamed) at `places`; a face's
/// runs merge, a face it empties goes, and the edges it folds keep their flags.
/// The new id of each old vertex, or a refusal: the trial repair would change
/// the result (`WouldPinch` when a vertex of @p watched would hold two fans,
/// Q6), every face would go, or a face it moves would turn over (TWL-2).
/// @p mesh is a compacted copy; on a refusal the caller drops it.
struct Renamed {
    ModelRefusal refusal = ModelRefusal::None;
    std::vector<u32> newId;
};

Renamed renameRebuild(Mesh& mesh, std::vector<u32> into, const std::vector<Vector3f>& places,
                      const std::vector<u32>& watched) {
    Renamed out;
    const FaceSet set = mesh.faceSet();
    const FaceTable table = tableOf(set);
    const std::vector<u32> sections(mesh.faceSections().begin(), mesh.faceSections().end());
    if (!joinLanded(mesh, set, table, into, places).empty()) {
        out.refusal = ModelRefusal::TargetCollapseIllegal; // onto a point kept apart: no edge of no length
        return out;
    }
    Candidate candidate = candidateOf(table, sections, into, true);
    if (candidate.faces.faceCount() == 0) {
        out.refusal = ModelRefusal::AllFaces;
        return out;
    }
    if (Repair(candidate.faces, candidate.sections, places).changed) {
        out.refusal = whyNotCollapsed(candidate, watched);
        if (out.refusal == ModelRefusal::CollapseIllegal) {
            out.refusal = ModelRefusal::TargetCollapseIllegal;
        }
        return out;
    }
    {
        const std::span<const Vector3f> before = positionsOf(mesh);
        std::vector<Vector3f> was;
        std::vector<Vector3f> now;
        u32 corner = 0;
        for (u32 j = 0; j < candidate.sourceFace.size(); ++j) {
            const u32 f = candidate.sourceFace[j];
            was.clear();
            now.clear();
            for (u32 k = 0; k < set.faceValence[f]; ++k) {
                was.push_back(before[set.cornerVertex[table.base[f] + k]]);
            }
            for (u32 k = 0; k < candidate.faces.faceValence[j]; ++k) {
                now.push_back(places[candidate.faces.cornerVertex[corner + k]]);
            }
            corner += candidate.faces.faceValence[j];
            const std::array<f64, 3> a = newellOf(was);
            const std::array<f64, 3> b = newellOf(now);
            if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] < 0.0) {
                out.refusal = ModelRefusal::WouldFold;
                return out;
            }
        }
    }
    const Folds folds = foldsOf(mesh, into, candidate);
    Settled settled;
    settled.survivorOf = into;
    settled.places = places;
    settled.candidate = std::move(candidate);
    const detail::RebuildResult result = commit(mesh, settled, static_cast<u32>(set.faceCount()), nullptr);
    if (!result.ok || result.repair.changed) {
        out.refusal = ModelRefusal::TargetCollapseIllegal;
        return out;
    }
    out.newId.assign(into.size(), kInvalidId);
    u32 next = 0;
    for (u32 v = 0; v < into.size(); ++v) {
        if (into[v] == v) {
            out.newId[v] = next++;
        }
    }
    for (u32 v = 0; v < into.size(); ++v) {
        out.newId[v] = out.newId[into[v]];
    }
    writeFolds(mesh, folds, out.newId);
    return out;
}

/// Q5 for a point a tool moves: a crack's twins act as one vertex. Each border
/// edge at a member of @p members whose twin -- a border edge between the same
/// two points, running the other way -- lies across a crack is zipped in
/// @p into, both ends' members renamed into their lowest.
void zipAround(const Mesh& mesh, const PointTable& points, const std::vector<u32>& members, std::vector<u32>& into) {
    const Topology& topology = mesh.topology();
    std::unordered_map<u32, u32> parent;
    const std::function<u32(u32)> find = [&](u32 v) {
        const auto at = parent.find(v);
        if (at == parent.end() || at->second == v) {
            return v;
        }
        const u32 root = find(at->second);
        parent[v] = root;
        return root;
    };
    const auto join = [&](u32 a, u32 b) {
        const u32 ra = find(a);
        const u32 rb = find(b);
        if (ra != rb) {
            parent[std::max(ra, rb)] = std::min(ra, rb);
        }
    };
    for (const u32 m : members) {
        for (const HalfedgeId h : topology.voh(VertexId(m))) {
            for (const HalfedgeId side : {h, Topology::opposite(h)}) {
                if (topology.isBoundary(side) || !topology.isBoundary(Topology::opposite(side))) {
                    continue; // not the face side of a border edge
                }
                const u32 a = topology.from(side).value();
                const u32 b = topology.to(side).value();
                for (const u32 b2 : membersOfPoint(mesh, points, b)) {
                    for (const u32 a2 : membersOfPoint(mesh, points, a)) {
                        if (a2 == a && b2 == b) {
                            continue;
                        }
                        const HalfedgeId twin = topology.findHalfedge(VertexId(b2), VertexId(a2));
                        if (twin.valid() && !topology.isBoundary(twin) &&
                            topology.isBoundary(Topology::opposite(twin))) {
                            join(a, a2);
                            join(b, b2);
                        }
                    }
                }
            }
        }
    }
    for (const auto& entry : parent) {
        into[entry.first] = find(entry.first);
    }
}

/// @p into with every chain followed to its end, so each vertex names a survivor.
void settleChains(std::vector<u32>& into) {
    for (u32 v = 0; v < into.size(); ++v) {
        u32 at = into[v];
        for (u32 guard = 0; into[at] != at && guard < into.size(); ++guard) {
            at = into[at];
        }
        into[v] = at;
    }
}

/// @p into and @p places, named in @p mesh's ids, onto @p work: a compacted
/// copy when @p mesh holds lazy deletes, else @p mesh's own numbering.
void compactInto(Mesh& work, std::vector<u32>& into, std::vector<Vector3f>& places, std::vector<u32>& named) {
    if (!std::as_const(work).topology().hasDeleted()) {
        return;
    }
    const Topology::Remap remap = GarbageCollect(work);
    const auto to = [&](u32 v) { return v < remap.vertices.size() ? remap.vertices[v] : kInvalidId; };
    std::vector<u32> moved(remap.newVertexCount);
    std::vector<Vector3f> placed(remap.newVertexCount);
    for (u32 v = 0; v < into.size(); ++v) {
        if (to(v) != kInvalidId) {
            moved[to(v)] = to(into[v]);
            placed[to(v)] = places[v];
        }
    }
    into = std::move(moved);
    places = std::move(placed);
    for (u32& v : named) {
        v = to(v);
    }
}

} // namespace

ModelPlan PlanTargetWeld(Mesh& mesh, const PointTable& points, VertexId source, VertexId target) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Mesh& readable = mesh;
    const Topology& topology = readable.topology();
    if (source.index() >= topology.vertexCount() || target.index() >= topology.vertexCount() ||
        topology.isDeleted(source) || topology.isDeleted(target)) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    const std::vector<u32> from = membersOfPoint(readable, points, source.value());
    const std::vector<u32> onto = membersOfPoint(readable, points, target.value());
    // The point moves as one (Q5): onto the target member it shares an edge
    // with, else onto the target. A twin onto its own twin zips the crack
    // there; two border points with no edge join as Max joins them (Q14).
    const bool samePoint = std::find(from.begin(), from.end(), target.value()) != from.end();
    u32 goal = target.value();
    bool adjacent = false;
    for (const u32 s : from) {
        for (const u32 t : onto) {
            if (!samePoint && !adjacent && topology.findHalfedge(VertexId(s), VertexId(t)).valid()) {
                goal = t;
                adjacent = true;
            }
        }
    }
    if (!samePoint && !adjacent) {
        bool sourceBorder = false;
        bool targetBorder = false;
        for (const u32 s : from) {
            sourceBorder = sourceBorder || topology.isBoundary(VertexId(s));
        }
        for (const u32 t : onto) {
            targetBorder = targetBorder || topology.isBoundary(VertexId(t));
        }
        if (!sourceBorder || !targetBorder) {
            plan.refusal = ModelRefusal::NotAdjacent;
            return plan;
        }
    }
    std::vector<u32> into(topology.vertexCount());
    std::iota(into.begin(), into.end(), 0u);
    zipAround(readable, points, from, into);
    for (const u32 s : from) {
        into[s] = goal;
    }
    into[goal] = goal;
    settleChains(into);
    const std::span<const Vector3f> positions = positionsOf(readable);
    std::vector<Vector3f> places(positions.begin(), positions.end());
    for (const u32 s : from) {
        places[s] = positions[goal];
    }
    std::vector<u32> watched;
    for (u32 v = 0; v < into.size(); ++v) {
        if (into[v] != v) {
            watched.push_back(into[v]);
        }
    }
    watched = sortedUnique(std::move(watched));
    std::vector<u32> named = {goal};
    named.insert(named.end(), watched.begin(), watched.end());
    Mesh work = readable;
    compactInto(work, into, places, named);
    watched.assign(named.begin() + 1, named.end());
    const Renamed renamed = renameRebuild(work, into, places, watched);
    if (renamed.refusal != ModelRefusal::None) {
        plan.refusal = renamed.refusal;
        return plan;
    }
    plan.selection.vertices = {renamed.newId[named[0]]};
    for (const u32 v : watched) {
        plan.reshade.push_back(renamed.newId[v]);
    }
    plan.changed = static_cast<u32>(into.size()) - std::as_const(work).topology().vertexCount();
    mesh = std::move(work);
    return plan;
}

ModelPlan PlanTargetWeldEdges(Mesh& mesh, const PointTable& points, EdgeId source, EdgeId target) {
    (void)points;
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Mesh& readable = mesh;
    const Topology& topology = readable.topology();
    const auto live = [&](EdgeId e) {
        return e.index() < topology.edgeCount() && !topology.isDeleted(e);
    };
    if (!live(source) || !live(target) || source == target) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (!topology.isBoundary(source) || !topology.isBoundary(target)) {
        plan.refusal = ModelRefusal::NotAdjacent;
        return plan;
    }
    const auto borderSide = [&](EdgeId e) {
        const HalfedgeId h = Topology::halfedge(e, 0);
        return topology.face(h).valid() ? Topology::halfedge(e, 1) : h;
    };
    // Facing across a crack, the two border edges run opposite ways.
    const HalfedgeId s = borderSide(source);
    const HalfedgeId t = borderSide(target);
    const u32 s0 = topology.from(s).value();
    const u32 s1 = topology.to(s).value();
    const u32 t0 = topology.from(t).value();
    const u32 t1 = topology.to(t).value();
    if (s0 == t0 || s1 == t1) {
        plan.refusal = ModelRefusal::WindingDisagrees;
        return plan;
    }
    std::vector<u32> into(topology.vertexCount());
    std::iota(into.begin(), into.end(), 0u);
    into[s0] = t1;
    into[s1] = t0;
    // The notch's condition at each end: no face may hold both vertices a
    // weld joins, or the crack's last edge would make a pillow.
    for (const u32 v : {s0, s1}) {
        if (into[v] == v) {
            continue;
        }
        for (const FaceId f : topology.vf(VertexId(v))) {
            if (!f.valid()) {
                continue;
            }
            for (const VertexId w : topology.fv(f)) {
                if (w.value() == into[v]) {
                    plan.refusal = ModelRefusal::WouldPinch;
                    return plan;
                }
            }
        }
    }
    const std::span<const Vector3f> positions = positionsOf(readable);
    std::vector<Vector3f> places(positions.begin(), positions.end());
    places[s0] = positions[t1];
    places[s1] = positions[t0];
    std::vector<u32> named = {t0, t1};
    Mesh work = readable;
    compactInto(work, into, places, named);
    const Renamed renamed = renameRebuild(work, into, places, named);
    if (renamed.refusal != ModelRefusal::None) {
        plan.refusal = renamed.refusal;
        return plan;
    }
    const u32 a = renamed.newId[named[0]];
    const u32 b = renamed.newId[named[1]];
    const HalfedgeId welded = std::as_const(work).topology().findHalfedge(VertexId(a), VertexId(b));
    if (welded.valid()) {
        plan.selection.edges = {Topology::edge(welded).value()};
    }
    plan.reshade = {a, b};
    plan.changed = 1;
    mesh = std::move(work);
    return plan;
}

f32 DefaultWeldThreshold(const Mesh& mesh) {
    return CoincidenceTolerance(mesh) * 100.0f;
}

namespace {

/// Weld's engine (§3.6): @p unions joins vertices into clusters; rule 2 splits a
/// cluster that would repeat a non-consecutive corner; rule 3's fixed point
/// keeps the rest manifold. Each cluster keeps its most central vertex, at its
/// centroid.
ModelPlan weldClusters(Mesh& mesh, const std::vector<std::vector<u32>>& clusters) {
    ModelPlan plan;
    const FaceSet set = mesh.faceSet();
    const u32 vertexCount = set.vertexCount;
    const FaceTable table = tableOf(set);
    const std::vector<u32> sections(mesh.faceSections().begin(), mesh.faceSections().end());
    const std::span<const Vector3f> positions = positionsOf(mesh);
    const std::vector<Vector3f> original(positions.begin(), positions.end());
    Partition partition(vertexCount);
    std::vector<u8> inCluster(vertexCount, 0);
    for (const std::vector<u32>& cluster : clusters) {
        // Rule 2 reads only the faces the cluster leaves standing: one it
        // empties (the four corners of a tiny quad, WLD-1) simply goes.
        for (const u32 v : cluster) {
            inCluster[v] = 1;
        }
        const auto empties = [&](u32 f) {
            const u32 base = table.base[f];
            const u32 valence = set.faceValence[f];
            u32 runs = 0;
            u32 outside = 0;
            for (u32 i = 0; i < valence; ++i) {
                const bool in = inCluster[set.cornerVertex[base + i]] != 0;
                const bool before = inCluster[set.cornerVertex[base + (i + valence - 1) % valence]] != 0;
                outside += in ? 0u : 1u;
                runs += in && !before ? 1u : 0u;
            }
            return outside + (runs == 0 && outside < valence ? 1u : runs) < 3u;
        };
        const auto apartIn = [&](u32 a, u32 b) {
            for (const u32 f : table.facesOf[a]) {
                if (!std::binary_search(table.facesOf[b].begin(), table.facesOf[b].end(), f) || empties(f)) {
                    continue;
                }
                const u32 base = table.base[f];
                const u32 valence = set.faceValence[f];
                for (u32 i = 0; i < valence; ++i) {
                    if (set.cornerVertex[base + i] == a &&
                        set.cornerVertex[base + (i + 1) % valence] != b &&
                        set.cornerVertex[base + (i + valence - 1) % valence] != b) {
                        return true;
                    }
                }
            }
            return false;
        };
        const std::vector<std::vector<u32>> apart = greedy(cluster, apartIn);
        for (const u32 v : cluster) {
            inCluster[v] = 0;
        }
        plan.kept += static_cast<u32>(apart.size() - 1);
        for (const std::vector<u32>& sub : apart) {
            partition.join(sub);
        }
    }
    const auto centroidOf = [&](const std::vector<u32>& group) {
        f64 x = 0.0;
        f64 y = 0.0;
        f64 z = 0.0;
        for (const u32 v : group) {
            x += original[v].x;
            y += original[v].y;
            z += original[v].z;
        }
        const f64 n = static_cast<f64>(group.size());
        return Vector3f{static_cast<f32>(x / n), static_cast<f32>(y / n), static_cast<f32>(z / n)};
    };
    const Chooser central = [&](const std::vector<u32>& group) {
        const Vector3f centre = centroidOf(group);
        u32 best = group[0];
        f64 bestDistance = length64(sub(original[best], centre));
        for (const u32 v : group) {
            const f64 d = length64(sub(original[v], centre));
            if (d < bestDistance) {
                best = v;
                bestDistance = d;
            }
        }
        return best;
    };
    const Placer place = [&](const Partition& parts, std::span<const u8> own, std::vector<Vector3f>& out) {
        out = original;
        for (const std::vector<u32>& group : parts.groups) {
            if (group.size() < 2) {
                continue;
            }
            const Vector3f centre = centroidOf(group);
            for (const u32 v : group) {
                if (own[v] == 0) {
                    out[v] = centre;
                }
            }
        }
    };
    const Settled settled = settle(table, sections, partition, central, place, true);
    plan.kept += settled.keptNonManifold;
    if (!mergesAny(partition)) {
        return plan;
    }
    if (settled.candidate.faces.faceCount() == 0 && set.faceCount() != 0) {
        plan = ModelPlan{};
        plan.refusal = ModelRefusal::AllFaces; // as Delete: a mesh keeps a polygon (Q10)
        return plan;
    }
    commit(mesh, settled, static_cast<u32>(set.faceCount()), nullptr);
    plan.changed = vertexCount - std::as_const(mesh).vertexCount();
    // The survivors, renumbered as the rebuild numbered them.
    std::vector<u32> newId(vertexCount, kInvalidId);
    u32 next = 0;
    for (u32 v = 0; v < vertexCount; ++v) {
        if (settled.survivorOf[v] == v) {
            newId[v] = next++;
        }
    }
    for (const std::vector<u32>& group : partition.groups) {
        if (group.size() > 1) {
            plan.selection.vertices.push_back(newId[settled.survivorOf[group[0]]]);
        }
    }
    plan.selection.normalise();
    plan.reshade = plan.selection.vertices;
    return plan;
}

/// @p clusters, named in @p mesh's own ids, welded. A mesh with lazy deletes is
/// compacted first, on a copy so that a refusal leaves it as it came, and the
/// clusters follow the compaction (WLD-5).
ModelPlan weldNamed(Mesh& mesh, std::vector<std::vector<u32>> clusters) {
    if (!std::as_const(mesh).topology().hasDeleted()) {
        return weldClusters(mesh, clusters);
    }
    Mesh copy = mesh;
    const Topology::Remap remap = GarbageCollect(copy);
    std::vector<std::vector<u32>> renamed;
    for (const std::vector<u32>& cluster : clusters) {
        std::vector<u32> now;
        for (const u32 v : cluster) {
            const u32 to = v < remap.vertices.size() ? remap.vertices[v] : kInvalidId;
            if (to != kInvalidId) {
                now.push_back(to);
            }
        }
        now = sortedUnique(std::move(now));
        if (now.size() > 1) {
            renamed.push_back(std::move(now));
        }
    }
    ModelPlan plan = weldClusters(copy, renamed);
    if (!plan.refused()) {
        mesh = std::move(copy);
    }
    return plan;
}

/// Clusters over @p vertices, Blender's Merge by Distance (Q9): the members of
/// one point always together; then, in order of x + y + z, each point not yet
/// taken takes every later one within @p threshold of it, and a point taken
/// takes nothing, so a row spaced under the threshold does not chain.
std::vector<std::vector<u32>> clustersOf(const Mesh& mesh, const PointTable& points,
                                         const std::vector<u32>& vertices, f32 threshold) {
    const std::span<const Vector3f> positions = positionsOf(mesh);
    const bool covers = points.pointOf.size() == mesh.topology().vertexCount();
    std::vector<std::vector<u32>> units; // one per point, its members among `vertices`
    {
        std::unordered_map<u32, u32> unitOf;
        for (const u32 v : vertices) {
            const auto [at, added] = unitOf.try_emplace(covers ? points.pointOf[v] : v, static_cast<u32>(units.size()));
            if (added) {
                units.emplace_back();
            }
            units[at->second].push_back(v);
        }
    }
    const auto placeOf = [&](u32 unit) { return positions[units[unit].front()]; };
    std::vector<u32> order(units.size());
    for (u32 u = 0; u < order.size(); ++u) {
        order[u] = u;
    }
    const auto sumOf = [&](u32 unit) {
        const Vector3f p = placeOf(unit);
        return static_cast<f64>(p.x) + static_cast<f64>(p.y) + static_cast<f64>(p.z);
    };
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const f64 sa = sumOf(a);
        const f64 sb = sumOf(b);
        return sa != sb ? sa < sb : units[a].front() < units[b].front();
    });
    std::vector<u32> rank(units.size());
    for (u32 i = 0; i < order.size(); ++i) {
        rank[order[i]] = i;
    }
    std::vector<u32> targetOf(units.size(), kInvalidId);
    if (threshold > 0.0f) {
        struct Key {
            i64 x, y, z;
            bool operator==(const Key& o) const {
                return x == o.x && y == o.y && z == o.z;
            }
        };
        struct KeyHash {
            std::size_t operator()(const Key& k) const {
                u64 h = 1469598103934665603ull;
                for (const i64 c : {k.x, k.y, k.z}) {
                    h ^= static_cast<u64>(c);
                    h *= 1099511628211ull;
                }
                return static_cast<std::size_t>(h);
            }
        };
        // Clamped, so a tiny threshold far from the origin shares a cell
        // rather than overflowing (WLD-3); the distance test stays exact.
        const auto cell = [&](f32 c) {
            return static_cast<i64>(std::clamp(std::floor(static_cast<f64>(c) / threshold), -4e18, 4e18));
        };
        const auto cellOf = [&](const Vector3f& p) { return Key{cell(p.x), cell(p.y), cell(p.z)}; };
        std::unordered_map<Key, std::vector<u32>, KeyHash> grid; // looked up, never iterated
        for (u32 u = 0; u < units.size(); ++u) {
            grid[cellOf(placeOf(u))].push_back(u);
        }
        std::vector<u8> taken(units.size(), 0);
        for (const u32 u : order) {
            if (taken[u] != 0) {
                continue;
            }
            const Vector3f at = placeOf(u);
            const Key key = cellOf(at);
            for (i64 dx = -1; dx <= 1; ++dx) {
                for (i64 dy = -1; dy <= 1; ++dy) {
                    for (i64 dz = -1; dz <= 1; ++dz) {
                        const auto found = grid.find(Key{key.x + dx, key.y + dy, key.z + dz});
                        if (found == grid.end()) {
                            continue;
                        }
                        for (const u32 w : found->second) {
                            if (rank[w] > rank[u] && taken[w] == 0 && length64(sub(at, placeOf(w))) <= threshold) {
                                taken[w] = 1;
                                targetOf[w] = u;
                            }
                        }
                    }
                }
            }
        }
    }
    std::vector<std::vector<u32>> merged(units.size());
    for (u32 u = 0; u < units.size(); ++u) {
        std::vector<u32>& into = merged[targetOf[u] == kInvalidId ? u : targetOf[u]];
        into.insert(into.end(), units[u].begin(), units[u].end());
    }
    std::vector<std::vector<u32>> out;
    for (std::vector<u32>& cluster : merged) {
        if (cluster.size() > 1) {
            out.push_back(sortedUnique(std::move(cluster)));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<u32> pointClosed(const Mesh& mesh, const PointTable& points, const std::vector<u32>& vertices) {
    std::vector<u32> out;
    for (const u32 v : vertices) {
        if (v >= mesh.topology().vertexCount() || mesh.topology().isDeleted(VertexId(v))) {
            continue;
        }
        for (const u32 m : membersOfPoint(mesh, points, v)) {
            out.push_back(m);
        }
    }
    return sortedUnique(std::move(out));
}

} // namespace

ModelPlan PlanWeld(Mesh& mesh, const PointTable& points, const ElementSet& selection, f32 threshold) {
    ModelPlan plan;
    if (selection.vertices.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const std::vector<u32> chosen = pointClosed(mesh, points, selection.vertices);
    return weldNamed(mesh, clustersOf(mesh, points, chosen, threshold));
}

ModelPlan PlanWeld(Mesh& mesh, const PointTable& points, const ElementSet& sources,
                   const ElementSet& targets, f32 threshold) {
    ModelPlan plan;
    if (sources.vertices.empty() || targets.vertices.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    (void)points;
    const std::span<const Vector3f> positions = positionsOf(mesh);
    // The caller names whole points on both sides: after a Snap the sources
    // sit on the target, and the point table would make them one point.
    const auto liveOf = [&](const std::vector<u32>& list) {
        std::vector<u32> out;
        for (const u32 v : list) {
            if (v < mesh.topology().vertexCount() && !mesh.topology().isDeleted(VertexId(v))) {
                out.push_back(v);
            }
        }
        return sortedUnique(std::move(out));
    };
    const std::vector<u32> onto = liveOf(targets.vertices);
    std::vector<u32> from;
    for (const u32 v : liveOf(sources.vertices)) {
        if (!std::binary_search(onto.begin(), onto.end(), v)) {
            from.push_back(v);
        }
    }
    // One to one, never two sources together, so the twins of a crease moved
    // as one point stay apart. Each source lists the targets within the
    // threshold, the one bound alike first, then the nearest; the matching
    // takes as many sources as it can (WLD-4), each its best target that an
    // augmenting path leaves free.
    const std::span<const u16> pins = mesh.attributes.get<const u16>(names::kClassicBones, Domain::Vertex);
    std::vector<std::vector<u32>> choices(from.size());
    for (std::size_t i = 0; i < from.size(); ++i) {
        std::vector<std::tuple<bool, f64, u32>> ranked;
        for (const u32 t : onto) {
            const f64 d = length64(sub(positions[from[i]], positions[t]));
            if (d <= threshold) {
                ranked.emplace_back(!sameBinding(mesh, {}, pins, from[i], t), d, t);
            }
        }
        std::sort(ranked.begin(), ranked.end());
        for (const auto& entry : ranked) {
            choices[i].push_back(std::get<2>(entry));
        }
    }
    std::unordered_map<u32, u32> holder; // target -> index into `from`
    std::vector<u32> matched(from.size(), kInvalidId);
    for (std::size_t i = 0; i < from.size(); ++i) {
        std::unordered_map<u32, u8> seen;
        const std::function<bool(std::size_t)> augment = [&](std::size_t s) {
            for (const u32 t : choices[s]) {
                if (seen[t] != 0) {
                    continue;
                }
                seen[t] = 1;
                const auto at = holder.find(t);
                if (at == holder.end() || augment(at->second)) {
                    holder[t] = static_cast<u32>(s);
                    matched[s] = t;
                    return true;
                }
            }
            return false;
        };
        augment(i);
    }
    std::vector<std::vector<u32>> clusters;
    for (std::size_t i = 0; i < from.size(); ++i) {
        if (matched[i] != kInvalidId) {
            clusters.push_back(sortedUnique({from[i], matched[i]}));
        }
    }
    return weldNamed(mesh, clusters);
}

// ============================================================================
// Collapse, Remove, Delete, Cap, Flip, Hard and Soft (§3.7, §3.10)
// ============================================================================

namespace {

/// Every vertex @p selection names -- an edge's ends, a face's corners -- whole
/// points, live and ascending.
std::vector<u32> verticesOf(const Mesh& mesh, const PointTable& points, const ElementSet& selection) {
    const Topology& topology = mesh.topology();
    std::vector<u32> out = selection.vertices;
    for (const u32 e : selection.edges) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        out.push_back(topology.from(h).value());
        out.push_back(topology.to(h).value());
    }
    for (const u32 f : selection.faces) {
        if (f >= topology.faceCount() || topology.isDeleted(FaceId(f))) {
            continue;
        }
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            out.push_back(topology.from(h).value());
        }
    }
    std::vector<u32> live;
    for (const u32 v : sortedUnique(std::move(out))) {
        if (v < topology.vertexCount() && !topology.isDeleted(VertexId(v))) {
            live.push_back(v);
        }
    }
    return pointClosed(mesh, points, live);
}

/// @p places on the plane of their own Newell normal, as 2-D points.
std::vector<std::array<f64, 2>> flattened(std::span<const Vector3f> places) {
    const std::array<f64, 3> n = newellOf(places);
    std::array<f64, 3> u{1.0, 0.0, 0.0};
    if (std::abs(n[0]) > 0.9) {
        u = {0.0, 1.0, 0.0};
    }
    const f64 along = u[0] * n[0] + u[1] * n[1] + u[2] * n[2];
    u = {u[0] - n[0] * along, u[1] - n[1] * along, u[2] - n[2] * along};
    const f64 length = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(length > 1e-12)) {
        return {};
    }
    u = {u[0] / length, u[1] / length, u[2] / length};
    const std::array<f64, 3> v{n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
    std::vector<std::array<f64, 2>> out;
    out.reserve(places.size());
    for (const Vector3f& p : places) {
        const std::array<f64, 3> q{static_cast<f64>(p.x), static_cast<f64>(p.y), static_cast<f64>(p.z)};
        out.push_back({q[0] * u[0] + q[1] * u[1] + q[2] * u[2], q[0] * v[0] + q[1] * v[1] + q[2] * v[2]});
    }
    return out;
}

f64 turnOf(const std::array<f64, 2>& a, const std::array<f64, 2>& b, const std::array<f64, 2>& c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

/// Whether the closed polygon @p places crosses itself on its own plane: a
/// knife-edge crease (§3.7) or a rim that is not a simple loop (§3.10). Two
/// sides that only meet at a shared end do not count.
bool foldsOver(std::span<const Vector3f> places) {
    const std::vector<std::array<f64, 2>> flat = flattened(places);
    const std::size_t n = flat.size();
    if (n < 4) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if ((i + 1) % n == j || (j + 1) % n == i) {
                continue; // they share an end
            }
            const std::array<f64, 2>& a = flat[i];
            const std::array<f64, 2>& b = flat[(i + 1) % n];
            const std::array<f64, 2>& c = flat[j];
            const std::array<f64, 2>& d = flat[(j + 1) % n];
            const bool straddles = (turnOf(a, b, c) > 0.0) != (turnOf(a, b, d) > 0.0);
            const bool crosses = (turnOf(c, d, a) > 0.0) != (turnOf(c, d, b) > 0.0);
            if (straddles && crosses) {
                return true;
            }
        }
    }
    return false;
}

/// Whether a live face of @p mesh other than @p except has exactly @p corners
/// as its vertices.
bool faceExists(const Mesh& mesh, std::vector<u32> corners, FaceId except = FaceId(),
                FaceId alsoExcept = FaceId()) {
    std::sort(corners.begin(), corners.end());
    const Topology& topology = mesh.topology();
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        const FaceId face(f);
        if (topology.isDeleted(face) || face == except || face == alsoExcept) {
            continue;
        }
        std::vector<u32> theirs;
        for (const HalfedgeId h : topology.fh(face)) {
            theirs.push_back(topology.from(h).value());
        }
        if (theirs.size() != corners.size()) {
            continue;
        }
        std::sort(theirs.begin(), theirs.end());
        if (theirs == corners) {
            return true;
        }
    }
    return false;
}

/// Collapse's corner rule (§3.7): the corners at @p vertices, per `uv` and
/// `color` layer, grouped into islands -- two corners of one fan are one
/// island while the layer agrees across the edge between them, and the corners
/// one face holds are one patch of it -- and each island set to the centre of
/// its own bounding box. The seams stay where they were, and a colour blends
/// as a UV does (COL-7).
void averageIslands(Mesh& mesh, const std::vector<u32>& vertices) {
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u32> corners;
    for (const u32 v : vertices) {
        if (v >= topology.vertexCount() || topology.isDeleted(VertexId(v))) {
            continue;
        }
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            if (!topology.isBoundary(h)) {
                corners.push_back(static_cast<u32>(h.index()));
            }
        }
    }
    corners = sortedUnique(std::move(corners));
    std::unordered_map<u32, u32> slotOf;
    for (u32 i = 0; i < corners.size(); ++i) {
        slotOf[corners[i]] = i;
    }
    std::vector<std::pair<u32, u32>> oneFace;
    {
        std::unordered_map<u32, u32> firstOfFace;
        for (u32 i = 0; i < corners.size(); ++i) {
            const auto [at, added] = firstOfFace.try_emplace(topology.face(HalfedgeId(corners[i])).value(), i);
            if (!added) {
                oneFace.emplace_back(i, at->second);
            }
        }
    }
    for (u32 set = 0; set < 8; ++set) {
        for (const std::string& name : {names::uv(set), names::color(set)}) {
            AttrLayer* layer = mesh.attributes.layer(name, Domain::Halfedge);
            if (layer == nullptr) {
                continue;
            }
            const bool bytes = layer->type == AttrType::U8x4;
            if (!bytes && layer->type != AttrType::F32x2 && layer->type != AttrType::F32x3 &&
                layer->type != AttrType::F32x4) {
                continue;
            }
            const std::size_t stride = AttrTypeSize(layer->type);
            const u32 components = AttrTypeComponents(layer->type);
            u8* data = layer->data.data();
            const auto has = [&](u32 corner) {
                return stride * (static_cast<std::size_t>(corner) + 1) <= layer->data.size();
            };
            std::vector<u32> parent(corners.size());
            std::iota(parent.begin(), parent.end(), 0u);
            const auto find = [&](u32 i) {
                while (parent[i] != i) {
                    parent[i] = parent[parent[i]];
                    i = parent[i];
                }
                return i;
            };
            for (u32 i = 0; i < corners.size(); ++i) {
                const HalfedgeId opposite = Topology::opposite(HalfedgeId(corners[i]));
                if (topology.isBoundary(opposite)) {
                    continue;
                }
                const u32 other = static_cast<u32>(topology.next(opposite).index());
                const auto at = slotOf.find(other);
                if (at != slotOf.end() && has(corners[i]) && has(other) &&
                    std::memcmp(data + stride * corners[i], data + stride * other, stride) == 0) {
                    parent[find(i)] = find(at->second);
                }
            }
            for (const auto& [a, b] : oneFace) {
                parent[find(a)] = find(b);
            }
            std::unordered_map<u32, std::vector<u32>> islands;
            for (u32 i = 0; i < corners.size(); ++i) {
                if (has(corners[i])) {
                    islands[find(i)].push_back(corners[i]);
                }
            }
            for (const auto& entry : islands) {
                for (u32 k = 0; k < components; ++k) {
                    const auto read = [&](u32 corner) {
                        if (bytes) {
                            return static_cast<f32>(data[stride * corner + k]);
                        }
                        f32 value = 0.0f;
                        std::memcpy(&value, data + stride * corner + 4 * k, sizeof(f32));
                        return value;
                    };
                    f32 lo = read(entry.second.front());
                    f32 hi = lo;
                    for (const u32 c : entry.second) {
                        lo = std::min(lo, read(c));
                        hi = std::max(hi, read(c));
                    }
                    for (const u32 c : entry.second) {
                        if (bytes) {
                            data[stride * c + k] =
                                static_cast<u8>((static_cast<u32>(lo) + static_cast<u32>(hi) + 1u) / 2u);
                        } else {
                            const f32 centre = (lo + hi) * 0.5f;
                            std::memcpy(data + stride * c + 4 * k, &centre, sizeof(f32));
                        }
                    }
                }
            }
        }
    }
}

/// One part of a Collapse (§3.7): its vertices, whole points; the centroid of
/// its points, each counted once however many vertices it holds (COL-6); and,
/// per side -- its vertices joined by the mesh's own edges, so the two sides
/// of a crack stay two (Q5) -- the most central vertex, which every other
/// vertex of the side becomes.
struct Gathered {
    std::vector<u32> vertices;
    std::vector<u32> into;  ///< Per vertex of `vertices`: its side's survivor.
    std::vector<u32> point; ///< Per vertex of `vertices`: its point.
    std::vector<u32> survivors;
    Vector3f centre{0.0f, 0.0f, 0.0f};
};

Gathered gather(const Mesh& mesh, const PointTable& points, const std::vector<u32>& part) {
    Gathered out;
    out.vertices = part;
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> places = positionsOf(mesh);
    const bool covers = points.pointOf.size() == topology.vertexCount();
    std::unordered_set<u32> seen;
    f64 x = 0.0, y = 0.0, z = 0.0;
    for (const u32 v : part) {
        out.point.push_back(covers ? points.pointOf[v] : v);
        if (seen.insert(out.point.back()).second) {
            x += places[v].x;
            y += places[v].y;
            z += places[v].z;
        }
    }
    const f64 n = static_cast<f64>(std::max<std::size_t>(seen.size(), 1));
    out.centre = Vector3f{static_cast<f32>(x / n), static_cast<f32>(y / n), static_cast<f32>(z / n)};
    std::unordered_map<u32, u32> indexOf;
    for (u32 i = 0; i < part.size(); ++i) {
        indexOf[part[i]] = i;
    }
    std::vector<u32> parent(part.size());
    std::iota(parent.begin(), parent.end(), 0u);
    const auto find = [&](u32 i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (u32 i = 0; i < part.size(); ++i) {
        for (const HalfedgeId h : topology.voh(VertexId(part[i]))) {
            const auto at = indexOf.find(topology.to(h).value());
            if (at != indexOf.end()) {
                parent[find(i)] = find(at->second);
            }
        }
    }
    std::unordered_map<u32, u32> best; // a side's root -> its survivor
    for (u32 i = 0; i < part.size(); ++i) {
        const auto [at, added] = best.try_emplace(find(i), part[i]);
        if (!added && length64(sub(places[part[i]], out.centre)) < length64(sub(places[at->second], out.centre))) {
            at->second = part[i];
        }
    }
    out.into.resize(part.size());
    for (u32 i = 0; i < part.size(); ++i) {
        out.into[i] = best[find(i)];
        if (out.into[i] == part[i]) {
            out.survivors.push_back(part[i]);
        }
    }
    return out;
}

} // namespace

ElementSet PointClosed(const Mesh& mesh, const PointTable& points, const ElementSet& selection) {
    ElementSet out = selection;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    for (const u32 v : selection.vertices) {
        if (v < topology.vertexCount() && !topology.isDeleted(VertexId(v))) {
            const std::vector<u32> members = membersOfPoint(mesh, points, v);
            out.vertices.insert(out.vertices.end(), members.begin(), members.end());
        }
    }
    for (const u32 e : selection.edges) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const std::vector<u32> from = membersOfPoint(mesh, points, topology.from(h).value());
        const std::vector<u32> to = membersOfPoint(mesh, points, topology.to(h).value());
        for (const u32 a : from) {
            for (const u32 b : to) {
                const HalfedgeId twin = topology.findHalfedge(VertexId(a), VertexId(b));
                if (twin.valid()) {
                    out.edges.push_back(Topology::edge(twin).value());
                }
            }
        }
    }
    out.normalise();
    return out;
}

std::vector<std::vector<u32>> SelectionParts(const Mesh& mesh, const PointTable& points,
                                             const ElementSet& selection) {
    if (!mesh.hasConnectivity()) {
        return {};
    }
    // Joined across what was selected, not across any edge between two of its
    // points: two groups of polygons with an unselected one between them are
    // two parts, though that polygon's edges run from one to the other.
    const Topology& topology = mesh.topology();
    const std::vector<u32> chosen = verticesOf(mesh, points, selection);
    std::unordered_map<u32, u32> parent;
    for (const u32 v : chosen) {
        parent[v] = v;
    }
    const auto find = [&](u32 v) {
        while (parent[v] != v) {
            parent[v] = parent[parent[v]];
            v = parent[v];
        }
        return v;
    };
    const auto join = [&](u32 a, u32 b) {
        if (parent.count(a) != 0 && parent.count(b) != 0) {
            parent[find(a)] = find(b);
        }
    };
    for (const u32 f : selection.faces) {
        if (f < topology.faceCount() && !topology.isDeleted(FaceId(f))) {
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                join(topology.from(h).value(), topology.to(h).value());
            }
        }
    }
    for (const u32 e : selection.edges) {
        if (e < topology.edgeCount() && !topology.isDeleted(EdgeId(e))) {
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            join(topology.from(h).value(), topology.to(h).value());
        }
    }
    // Vertices picked as vertices are joined by the mesh's own edges.
    const std::vector<u32> picked = sortedUnique(selection.vertices);
    for (const u32 v : picked) {
        if (v >= topology.vertexCount() || topology.isDeleted(VertexId(v))) {
            continue;
        }
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            if (std::binary_search(picked.begin(), picked.end(), topology.to(h).value())) {
                join(v, topology.to(h).value());
            }
        }
    }
    for (const u32 v : chosen) {
        for (const u32 member : membersOfPoint(mesh, points, v)) {
            join(v, member);
        }
    }
    std::unordered_map<u32, std::vector<u32>> groups;
    for (const u32 v : chosen) {
        groups[find(v)].push_back(v);
    }
    std::vector<std::vector<u32>> parts;
    for (auto& entry : groups) {
        parts.push_back(sortedUnique(std::move(entry.second)));
    }
    std::sort(parts.begin(), parts.end());
    return parts;
}

ModelPlan PlanCollapse(Mesh& mesh, const PointTable& points, const ElementSet& selection) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    if (verticesOf(mesh, points, selection).empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    // Grouped by what was selected (Q1), each part to its points' centroid. A
    // part whose every side is one vertex already is (a crack's twins alone).
    std::vector<Gathered> parts;
    for (const std::vector<u32>& part : SelectionParts(mesh, points, selection)) {
        Gathered gathered = gather(mesh, points, part);
        if (gathered.survivors.size() < gathered.vertices.size()) {
            parts.push_back(std::move(gathered));
        }
    }
    if (parts.empty()) {
        return plan;
    }
    // Planned on a copy, compacted: the trial and the rebuild index every slot.
    Mesh work = mesh;
    if (std::as_const(work).topology().hasDeleted()) {
        const Topology::Remap remap = GarbageCollect(work);
        for (Gathered& part : parts) {
            for (std::vector<u32>* ids : {&part.vertices, &part.into, &part.survivors}) {
                for (u32& v : *ids) {
                    v = v < remap.vertices.size() ? remap.vertices[v] : v;
                }
            }
        }
    }
    const FaceSet set = work.faceSet();
    const u32 vertexCount = set.vertexCount;
    const FaceTable table = tableOf(set);
    const std::vector<u32> sections(work.faceSections().begin(), work.faceSections().end());
    const std::span<const Vector3f> start = positionsOf(work);
    std::vector<u32> partOf(vertexCount, kInvalidId); // per survivor
    for (u32 p = 0; p < parts.size(); ++p) {
        for (const u32 s : parts[p].survivors) {
            partOf[s] = p;
        }
    }
    // All parts at once, as one rename; a part the trial repair names is left
    // (X1, Q2) and the rest tried again, so the legal parts are done.
    std::vector<ModelRefusal> left(parts.size(), ModelRefusal::None);
    std::vector<u32> survivorOf(vertexCount);
    std::vector<Vector3f> places;
    Candidate candidate;
    bool any = false;
    for (;;) {
        std::iota(survivorOf.begin(), survivorOf.end(), 0u);
        places.assign(start.begin(), start.end());
        any = false;
        for (u32 p = 0; p < parts.size(); ++p) {
            if (left[p] != ModelRefusal::None) {
                continue;
            }
            any = true;
            for (u32 i = 0; i < parts[p].vertices.size(); ++i) {
                survivorOf[parts[p].vertices[i]] = parts[p].into[i];
            }
            for (const u32 s : parts[p].survivors) {
                places[s] = parts[p].centre;
            }
        }
        if (!any) {
            break;
        }
        // A part landing beside a point kept apart is left (Q8, X1).
        bool leftBeside = false;
        for (const u32 v : joinLanded(work, set, table, survivorOf, places)) {
            if (v < vertexCount && partOf[v] != kInvalidId && left[partOf[v]] == ModelRefusal::None) {
                left[partOf[v]] = ModelRefusal::CollapseIllegal;
                leftBeside = true;
            }
        }
        if (leftBeside) {
            continue;
        }
        candidate = candidateOf(table, sections, survivorOf, true);
        if (candidate.faces.faceCount() == 0) {
            // Every face would go: refused whole, as Delete refuses every face.
            for (u32 p = 0; p < parts.size(); ++p) {
                if (left[p] == ModelRefusal::None) {
                    left[p] = ModelRefusal::AllFaces;
                }
            }
            continue;
        }
        const RepairResult trial = Repair(candidate.faces, candidate.sections, places);
        if (!trial.changed) {
            break;
        }
        std::vector<u8> named(parts.size(), 0);
        const auto name = [&](u32 v) {
            if (v < vertexCount && partOf[v] != kInvalidId && survivorOf[v] == v) {
                named[partOf[v]] = 1;
            }
        };
        for (const VertexSplit& split : trial.log.splits) {
            name(split.original);
        }
        for (const FaceRecord& dropped : trial.log.droppedFaces) {
            for (const u32 v : dropped.corners) {
                name(v);
            }
        }
        bool leftAny = false;
        for (u32 p = 0; p < parts.size(); ++p) {
            if (left[p] == ModelRefusal::None && named[p] != 0) {
                left[p] = whyNotCollapsed(candidate, parts[p].survivors);
                leftAny = true;
            }
        }
        if (!leftAny) {
            for (u32 p = 0; p < parts.size(); ++p) {
                if (left[p] == ModelRefusal::None) {
                    left[p] = ModelRefusal::CollapseIllegal;
                }
            }
        }
    }
    for (u32 p = 0; p < parts.size(); ++p) {
        if (left[p] != ModelRefusal::None) {
            plan.kept += 1;
            if (plan.keptBecause == ModelRefusal::None) {
                plan.keptBecause = left[p];
            }
        }
    }
    if (!any) {
        const ModelRefusal why = plan.keptBecause;
        plan = ModelPlan{};
        plan.refusal = why;
        return plan;
    }
    // The parts kept: the skin blended over their points at equal weight, and
    // each island's corners to its centre, on every member, so the survivor
    // carries them; then the rename, rebuilt.
    for (u32 p = 0; p < parts.size(); ++p) {
        if (left[p] != ModelRefusal::None) {
            continue;
        }
        const std::vector<u32>& vertices = parts[p].vertices;
        if (!work.skin.empty()) {
            std::unordered_map<u32, u32> share; // the part's members of each point
            for (const u32 point : parts[p].point) {
                ++share[point];
            }
            std::vector<std::vector<Influence>> sources;
            std::vector<f32> weights;
            for (u32 i = 0; i < vertices.size(); ++i) {
                const std::span<const Influence> theirs = std::as_const(work).skin.forVertex(vertices[i]);
                sources.emplace_back(theirs.begin(), theirs.end());
                weights.push_back(1.0f / static_cast<f32>(share[parts[p].point[i]]));
            }
            const std::vector<Influence> blended = BlendInfluences(sources, weights);
            for (const u32 v : vertices) {
                work.skin.assignVertex(v, blended);
            }
        }
        averageIslands(work, vertices);
    }
    const Folds folds = foldsOf(work, survivorOf, candidate);
    Settled settled;
    settled.survivorOf = survivorOf;
    settled.places = places;
    settled.candidate = std::move(candidate);
    const detail::RebuildResult result = commit(work, settled, static_cast<u32>(set.faceCount()), nullptr);
    if (!result.ok || result.repair.changed) {
        plan = ModelPlan{};
        plan.refusal = ModelRefusal::CollapseIllegal;
        return plan;
    }
    std::vector<u32> newId(vertexCount, kInvalidId);
    u32 next = 0;
    for (u32 v = 0; v < vertexCount; ++v) {
        if (survivorOf[v] == v) {
            newId[v] = next++;
        }
    }
    writeFolds(work, folds, newId);
    for (u32 p = 0; p < parts.size(); ++p) {
        if (left[p] == ModelRefusal::None) {
            for (const u32 s : parts[p].survivors) {
                plan.selection.vertices.push_back(newId[survivorOf[s]]); // joinLanded may have renamed it
            }
        }
    }
    plan.selection.normalise();
    plan.reshade = plan.selection.vertices;
    plan.changed = vertexCount - next;
    plan.renumbers = true;
    mesh = std::move(work);
    return plan;
}

namespace {

/// Remove's face set (§3.7): each face of a renamed face set, and the regions
/// the dissolves join, each of which becomes one face over its border.
struct Dissolving {
    const Candidate* candidate = nullptr;
    std::vector<u32> base;                       ///< Per face, its first corner.
    std::unordered_map<u64, u32> faceOfSide;     ///< Halfedge a -> b -> its face.
    std::vector<u32> parent;                     ///< Per face, union-find.
    std::vector<std::vector<u32>> members;       ///< Per region root, its faces.
    std::vector<u8> dropped;                     ///< Per vertex: taken out of every loop.
    std::vector<std::vector<u32>> facesOf;       ///< Per vertex, the faces holding it.
    std::span<const Vector3f> places;
    std::vector<f64> area;                       ///< Per face, twice its area.

    static u64 side(u32 a, u32 b) {
        return (static_cast<u64>(a) << 32) | b;
    }

    /// Twice the area @p loop spans (its Newell vector's length).
    f64 spanOf(const std::vector<std::pair<u32, u32>>& loop) const {
        f64 x = 0.0, y = 0.0, z = 0.0;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const Vector3f& a = places[loop[i].first];
            const Vector3f& b = places[loop[(i + 1) % loop.size()].first];
            x += (static_cast<f64>(a.y) - b.y) * (static_cast<f64>(a.z) + b.z);
            y += (static_cast<f64>(a.z) - b.z) * (static_cast<f64>(a.x) + b.x);
            z += (static_cast<f64>(a.x) - b.x) * (static_cast<f64>(a.y) + b.y);
        }
        return std::sqrt(x * x + y * y + z * z);
    }

    Dissolving(const Candidate& made, std::span<const Vector3f> at) : candidate(&made), places(at) {
        const FaceSet& faces = made.faces;
        base.assign(faces.faceCount() + 1, 0);
        for (u32 f = 0; f < faces.faceCount(); ++f) {
            base[f + 1] = base[f] + faces.faceValence[f];
            for (u32 k = 0; k < faces.faceValence[f]; ++k) {
                faceOfSide[side(corner(f, k), corner(f, k + 1))] = f;
            }
        }
        parent.resize(faces.faceCount());
        std::iota(parent.begin(), parent.end(), 0u);
        members.resize(faces.faceCount());
        for (u32 f = 0; f < faces.faceCount(); ++f) {
            members[f] = {f};
        }
        dropped.assign(faces.vertexCount, 0);
        facesOf.resize(faces.vertexCount);
        area.resize(faces.faceCount());
        std::vector<std::pair<u32, u32>> loop;
        for (u32 f = 0; f < faces.faceCount(); ++f) {
            loop.clear();
            for (u32 k = 0; k < faces.faceValence[f]; ++k) {
                facesOf[corner(f, k)].push_back(f);
                loop.emplace_back(corner(f, k), 0u);
            }
            area[f] = spanOf(loop);
        }
    }

    u32 corner(u32 f, u32 k) const {
        return candidate->faces.cornerVertex[base[f] + k % candidate->faces.faceValence[f]];
    }
    u32 find(u32 f) {
        while (parent[f] != f) {
            parent[f] = parent[parent[f]];
            f = parent[f];
        }
        return f;
    }
    /// The face across a -> b, or kInvalidId on the border.
    u32 across(u32 a, u32 b) const {
        const auto at = faceOfSide.find(side(b, a));
        return at == faceOfSide.end() ? kInvalidId : at->second;
    }

    /// The border of @p faces (one region, or two about to be one) as a single
    /// loop of (vertex, candidate corner), the dropped vertices left out; or
    /// why it is not one face's: two loops or a pinch (`WouldFold`), a vertex
    /// twice (`WouldRepeatVertex`).
    ModelRefusal loopOf(const std::vector<u32>& faces, std::vector<std::pair<u32, u32>>& loop) const {
        std::unordered_set<u32> inside(faces.begin(), faces.end());
        std::unordered_map<u32, std::pair<u32, u32>> leaving; // a -> (b, corner at a)
        u32 sides = 0;
        for (const u32 f : faces) {
            const u32 valence = candidate->faces.faceValence[f];
            for (u32 k = 0; k < valence; ++k) {
                const u32 a = corner(f, k);
                const u32 b = corner(f, k + 1);
                const u32 other = across(a, b);
                if (other != kInvalidId && inside.count(other) != 0) {
                    continue;
                }
                if (!leaving.emplace(a, std::make_pair(b, base[f] + k)).second) {
                    return ModelRefusal::WouldFold; // two border runs through one vertex
                }
                ++sides;
            }
        }
        loop.clear();
        if (leaving.empty()) {
            return ModelRefusal::WouldFold; // a closed shell has no border to be a face
        }
        u32 at = leaving.begin()->first;
        for (u32 walked = 0; walked < sides; ++walked) {
            const auto next = leaving.find(at);
            if (next == leaving.end()) {
                return ModelRefusal::WouldFold;
            }
            if (dropped[at] == 0) {
                loop.emplace_back(at, next->second.second);
            }
            at = next->second.first;
            if (at == leaving.begin()->first && walked + 1 < sides) {
                return ModelRefusal::WouldFold; // the border is more than one loop
            }
        }
        std::vector<u32> seen;
        for (const auto& entry : loop) {
            seen.push_back(entry.first);
        }
        std::sort(seen.begin(), seen.end());
        if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) {
            return ModelRefusal::WouldRepeatVertex;
        }
        if (loop.size() < 3) {
            return ModelRefusal::WouldFold;
        }
        // A region wrapped round a tube or a shell spans next to nothing as
        // one polygon: it would fold over itself, and the repair drop it.
        f64 covered = 0.0;
        for (const u32 f : faces) {
            covered += area[f];
        }
        return spanOf(loop) > 1e-3 * covered ? ModelRefusal::None : ModelRefusal::WouldFold;
    }

    void join(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        if (members[a].size() < members[b].size()) {
            std::swap(a, b);
        }
        members[a].insert(members[a].end(), members[b].begin(), members[b].end());
        members[b].clear();
        parent[b] = a;
    }
};

} // namespace

ModelPlan PlanRemove(Mesh& mesh, const PointTable& points, const ElementSet& selection,
                     const RemoveParams& params) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    if (selection.edges.empty() && selection.vertices.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    // A crack's twins are one vertex (Q5): a vertex or edge on one is taken on
    // both sides, the crack zipped where it runs through what is removed.
    const Mesh& readable = mesh;
    const Topology& topology = readable.topology();
    const ElementSet closed = PointClosed(readable, points, selection);
    std::vector<u32> into(topology.vertexCount());
    std::iota(into.begin(), into.end(), 0u);
    for (const u32 v : closed.vertices) {
        if (v < topology.vertexCount() && !topology.isDeleted(VertexId(v))) {
            zipAround(readable, points, membersOfPoint(readable, points, v), into);
        }
    }
    const auto rootOf = [&](u32 v) {
        for (u32 guard = 0; into[v] != v && guard < into.size(); ++guard) {
            v = into[v];
        }
        return v;
    };
    const auto zip = [&](u32 a, u32 b) {
        const u32 ra = rootOf(a);
        const u32 rb = rootOf(b);
        into[std::max(ra, rb)] = std::min(ra, rb);
    };
    std::vector<std::pair<u32, u32>> asked; // the edges, as their two ends
    for (const u32 e : closed.edges) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const u32 a = topology.from(h).value();
        const u32 b = topology.to(h).value();
        if (topology.isBoundary(EdgeId(e))) {
            // A crack's edge: zipped with its twin, it is an inner edge.
            for (const u32 b2 : membersOfPoint(readable, points, b)) {
                for (const u32 a2 : membersOfPoint(readable, points, a)) {
                    const HalfedgeId twin = topology.findHalfedge(VertexId(b2), VertexId(a2));
                    if ((a2 != a || b2 != b) && twin.valid() && topology.isBoundary(Topology::edge(twin))) {
                        zip(a, a2);
                        zip(b, b2);
                    }
                }
            }
        }
        asked.emplace_back(a, b);
    }
    settleChains(into);
    std::vector<u32> removing; // vertices, as survivors
    for (const u32 v : closed.vertices) {
        if (v < topology.vertexCount() && !topology.isDeleted(VertexId(v))) {
            removing.push_back(into[v]);
        }
    }
    removing = sortedUnique(std::move(removing));
    std::vector<Vector3f> places(positionsOf(readable).begin(), positionsOf(readable).end());
    std::vector<u32> named = removing;
    for (const auto& [a, b] : asked) {
        named.push_back(a);
        named.push_back(b);
    }
    Mesh work = readable;
    compactInto(work, into, places, named);
    for (u32 i = 0; i < removing.size(); ++i) {
        removing[i] = into[named[i]];
    }
    for (u32 i = 0; i < asked.size(); ++i) {
        asked[i] = {into[named[removing.size() + 2 * i]], into[named[removing.size() + 2 * i + 1]]};
    }
    const FaceSet set = work.faceSet();
    const FaceTable table = tableOf(set);
    const std::vector<u32> sections(work.faceSections().begin(), work.faceSections().end());
    const Candidate candidate = candidateOf(table, sections, into, true);
    Dissolving dissolve(candidate, places);
    const auto refuseOne = [&](ModelRefusal why) {
        ++plan.kept;
        if (plan.keptBecause == ModelRefusal::None) {
            plan.keptBecause = why;
        }
    };
    const auto sectionOf = [&](u32 f) { return f < candidate.sections.size() ? candidate.sections[f] : 0u; };
    std::vector<u32> left; // the ends of dissolved edges, for Clean vertices
    std::vector<std::pair<u32, u32>> loop;
    // The edges: the faces either side become one, while the region's border
    // stays one loop; a border edge has nothing to close (X1: the rest go on).
    for (const auto& [a, b] : asked) {
        const u32 one = dissolve.across(b, a);
        const u32 two = dissolve.across(a, b);
        if (one == kInvalidId || two == kInvalidId) {
            refuseOne(ModelRefusal::BorderEdgeRemove);
            continue;
        }
        const u32 ra = dissolve.find(one);
        const u32 rb = dissolve.find(two);
        if (ra == rb) {
            ++plan.changed; // already inside one region: it goes with it
            continue;
        }
        if (sectionOf(one) != sectionOf(two)) {
            refuseOne(ModelRefusal::CrossesSection);
            continue;
        }
        std::vector<u32> both = dissolve.members[ra];
        both.insert(both.end(), dissolve.members[rb].begin(), dissolve.members[rb].end());
        const ModelRefusal why = dissolve.loopOf(both, loop);
        if (why != ModelRefusal::None) {
            refuseOne(why);
            continue;
        }
        dissolve.join(ra, rb);
        left.push_back(a);
        left.push_back(b);
        ++plan.changed;
    }
    // The vertices: the faces round each one face over its ring, every outer
    // corner keeping its own values; a valence-2 vertex just leaves its two faces.
    for (const u32 v : removing) {
        const std::vector<u32> fan = v < dissolve.facesOf.size() ? dissolve.facesOf[v] : std::vector<u32>{};
        std::unordered_set<u32> neighbours;
        for (const u32 f : fan) {
            for (u32 k = 0; k < candidate.faces.faceValence[f]; ++k) {
                if (dissolve.corner(f, k) == v) {
                    neighbours.insert(dissolve.corner(f, k + 1));
                    neighbours.insert(dissolve.corner(f, k + candidate.faces.faceValence[f] - 1));
                }
            }
        }
        if (fan.empty()) {
            continue;
        }
        if (neighbours.size() == 2) {
            bool tooShort = false;
            for (const u32 f : fan) {
                tooShort = tooShort || candidate.faces.faceValence[f] < 4;
            }
            if (tooShort) {
                refuseOne(ModelRefusal::DissolveVertexIllegal);
            } else {
                dissolve.dropped[v] = 1;
                ++plan.changed;
            }
            continue;
        }
        std::vector<u32> region;
        for (const u32 f : fan) {
            const u32 root = dissolve.find(f);
            region.insert(region.end(), dissolve.members[root].begin(), dissolve.members[root].end());
        }
        region = sortedUnique(std::move(region));
        bool mixed = false;
        for (const u32 f : region) {
            mixed = mixed || sectionOf(f) != sectionOf(region.front());
        }
        if (mixed) {
            refuseOne(ModelRefusal::CrossesSection);
            continue;
        }
        dissolve.dropped[v] = 1;
        ModelRefusal why = dissolve.loopOf(region, loop);
        if (why == ModelRefusal::None) {
            std::vector<Vector3f> shape;
            for (const auto& entry : loop) {
                shape.push_back(places[entry.first]);
            }
            why = foldsOver(shape) ? ModelRefusal::WouldFold : ModelRefusal::None; // a knife-edge crease
        }
        if (why != ModelRefusal::None) {
            dissolve.dropped[v] = 0;
            refuseOne(why);
            continue;
        }
        for (const u32 f : fan) {
            dissolve.join(fan.front(), f);
        }
        ++plan.changed;
    }
    // Clean vertices (3ds Max's Ctrl+Backspace): the valence-2 vertices the
    // dissolves left, and only those.
    if (params.cleanVertices) {
        for (const u32 v : sortedUnique(std::move(left))) {
            std::unordered_set<u32> neighbours;
            std::unordered_set<u32> regions;
            for (const u32 f : dissolve.facesOf[v]) {
                const u32 valence = candidate.faces.faceValence[f];
                for (u32 k = 0; k < valence; ++k) {
                    if (dissolve.corner(f, k) != v) {
                        continue;
                    }
                    regions.insert(dissolve.find(f));
                    for (const u32 n : {dissolve.corner(f, k + 1), dissolve.corner(f, k + valence - 1)}) {
                        // A neighbour across an edge inside one region is gone with it.
                        const u32 out = dissolve.across(v, n);
                        const u32 in = dissolve.across(n, v);
                        if (out == kInvalidId || in == kInvalidId || dissolve.find(out) != dissolve.find(in)) {
                            neighbours.insert(n);
                        }
                    }
                }
            }
            if (neighbours.size() == 2 && regions.size() <= 2) {
                dissolve.dropped[v] = 1;
                ++plan.changed;
            }
        }
    }
    if (plan.changed == 0) {
        const ModelRefusal why = plan.keptBecause;
        plan = ModelPlan{};
        plan.refusal = why;
        return plan;
    }
    // The face set: each region one face over its border, every other face as
    // it was less its dropped corners, through the rebuild.
    detail::RebuildMapping mapping;
    std::vector<u32> newId(candidate.faces.vertexCount, kInvalidId);
    const auto use = [&](u32 v) {
        if (newId[v] == kInvalidId) {
            newId[v] = static_cast<u32>(mapping.vertexSource.size());
            mapping.vertexSource.push_back(v);
        }
        return newId[v];
    };
    std::vector<u32> made; // new face slots whose rows the finish materialises
    std::vector<u32> around;
    for (u32 f = 0; f < candidate.faces.faceCount(); ++f) {
        if (dissolve.find(f) != f) {
            continue; // emitted with its region's root
        }
        const std::vector<u32>& region = dissolve.members[f];
        std::vector<std::pair<u32, u32>> corners;
        bool reshaped = region.size() > 1;
        if (region.size() > 1) {
            if (dissolve.loopOf(region, corners) != ModelRefusal::None) {
                plan = ModelPlan{};
                plan.refusal = ModelRefusal::WouldFold;
                return plan;
            }
        } else {
            for (u32 k = 0; k < candidate.faces.faceValence[f]; ++k) {
                const u32 v = dissolve.corner(f, k);
                if (dissolve.dropped[v] == 0) {
                    corners.emplace_back(v, dissolve.base[f] + k);
                } else {
                    reshaped = true;
                }
            }
        }
        if (corners.size() < 3) {
            plan = ModelPlan{};
            plan.refusal = ModelRefusal::DissolveVertexIllegal;
            return plan;
        }
        u32 sourceFace = candidate.sourceFace[f];
        for (const u32 g : region) {
            sourceFace = std::min(sourceFace, candidate.sourceFace[g]);
        }
        if (reshaped) {
            made.push_back(static_cast<u32>(mapping.faces.faceValence.size()));
        }
        mapping.faces.faceValence.push_back(static_cast<u32>(corners.size()));
        for (const auto& [v, c] : corners) {
            mapping.faces.cornerVertex.push_back(use(v));
            mapping.cornerSource.push_back(candidate.sourceCorner[c]);
            if (reshaped) {
                around.push_back(newId[v]);
            }
        }
        mapping.faceSource.push_back(sourceFace);
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(work);
    {
        const std::span<Vector3f> written = work.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        for (u32 v = 0; v < places.size() && v < written.size(); ++v) {
            written[v] = places[v];
        }
    }
    const detail::RebuildResult result = detail::rebuild(work, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan = ModelPlan{};
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changedFaces = std::move(made);
    plan.reshade = sortedUnique(std::move(around));
    plan.renumbers = true;
    mesh = std::move(work);
    return plan;
}

ModelPlan PlanDelete(Mesh& mesh, const ElementSet& asked) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    // A crack's twins are one vertex (Q5): a vertex or edge there takes the
    // faces on both sides.
    const ElementSet selection = asked.vertices.empty() && asked.edges.empty()
                                     ? asked
                                     : PointClosed(mesh, skinning::BuildPointTable(mesh), asked);
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u8> gone(topology.faceCount(), 0);
    for (const u32 f : selection.faces) {
        if (f < gone.size()) {
            gone[f] = 1;
        }
    }
    for (const u32 e : selection.edges) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e))) {
            continue;
        }
        for (u32 side = 0; side < 2; ++side) {
            const FaceId f = topology.face(Topology::halfedge(EdgeId(e), side));
            if (f.valid() && f.value() < gone.size()) {
                gone[f.value()] = 1;
            }
        }
    }
    std::vector<u8> at(topology.vertexCount(), 0);
    for (const u32 v : selection.vertices) {
        if (v < at.size()) {
            at[v] = 1;
        }
    }
    u32 chosen = 0, live = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (topology.isDeleted(FaceId(f))) {
            gone[f] = 0;
            continue;
        }
        ++live;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            gone[f] |= at[topology.from(h).value()];
        }
        chosen += gone[f];
    }
    if (chosen == 0) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (chosen == live) {
        plan.refusal = ModelRefusal::AllFaces; // a mesh needs a polygon
        return plan;
    }
    // The face set without them, through the rebuild: the repair splits every
    // vertex the deletion pinched, one per fan, and the twins keep the
    // original's `mergeGroup` (§3.7).
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    std::vector<u8> kept(topology.vertexCount(), 0);
    std::vector<u8> rim(topology.vertexCount(), 0); // the corners of the faces that go
    std::vector<std::vector<u32>> loops;
    std::vector<std::vector<u32>> corners;
    std::vector<u32> sourceFace;
    u32 corner = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        const FaceId face(f);
        if (topology.isDeleted(face)) {
            continue;
        }
        std::vector<u32> loop;
        std::vector<u32> mine;
        for (const HalfedgeId h : topology.fh(face)) {
            loop.push_back(topology.from(h).value());
            mine.push_back(corner++);
        }
        if (gone[f] != 0) {
            for (const u32 v : loop) {
                rim[v] = 1;
            }
            continue;
        }
        for (const u32 v : loop) {
            kept[v] = 1;
        }
        loops.push_back(std::move(loop));
        corners.push_back(std::move(mine));
        sourceFace.push_back(f);
    }
    detail::RebuildMapping mapping;
    std::vector<u32> newId(kept.size(), kInvalidId);
    for (u32 v = 0; v < kept.size(); ++v) {
        if (kept[v] != 0) {
            newId[v] = static_cast<u32>(mapping.vertexSource.size());
            mapping.vertexSource.push_back(v);
        }
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    for (std::size_t i = 0; i < loops.size(); ++i) {
        mapping.faces.faceValence.push_back(static_cast<u32>(loops[i].size()));
        for (const u32 v : loops[i]) {
            mapping.faces.cornerVertex.push_back(newId[v]);
        }
        mapping.cornerSource.insert(mapping.cornerSource.end(), corners[i].begin(), corners[i].end());
    }
    mapping.faceSource = std::move(sourceFace);
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    // The faces left round the hole are shaded afresh (X10), the repair's
    // splits of a pinched vertex with them.
    for (u32 v = 0; v < rim.size(); ++v) {
        if (rim[v] != 0 && newId[v] != kInvalidId) {
            plan.reshade.push_back(newId[v]);
        }
    }
    for (const VertexSplit& split : result.repair.log.splits) {
        plan.reshade.push_back(split.created);
    }
    plan.changed = chosen;
    return plan;
}

ModelPlan PlanCap(Mesh& mesh, const PointTable& given, EdgeId edge) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    {
        const Topology& topology = std::as_const(mesh).topology();
        if (edge.value() >= topology.edgeCount() || topology.isDeleted(edge) || !topology.isBoundary(edge)) {
            plan.refusal = ModelRefusal::EmptySelection;
            return plan;
        }
    }
    // Planned on a compacted copy, its own points then.
    Mesh work = mesh;
    PointTable compacted;
    const PointTable* points = &given;
    if (std::as_const(work).topology().hasDeleted()) {
        const Topology::Remap remap = GarbageCollect(work);
        edge = EdgeId(edge.value() < remap.edges.size() ? remap.edges[edge.value()] : edge.value());
        compacted = skinning::BuildPointTable(work);
        points = &compacted;
    }
    const Topology& topology = std::as_const(work).topology();
    // The hole's loop over points (Q5): a border edge whose twin runs back the
    // other way between the same two points is a crack, not the hole's, and
    // the walk steps over it to the twin side, which is zipped where it does.
    const auto cracked = [&](HalfedgeId h) {
        const u32 a = topology.from(h).value();
        const u32 b = topology.to(h).value();
        for (const u32 b2 : membersOfPoint(work, *points, b)) {
            for (const u32 a2 : membersOfPoint(work, *points, a)) {
                if (a2 == a && b2 == b) {
                    continue;
                }
                const HalfedgeId back = topology.findHalfedge(VertexId(b2), VertexId(a2));
                if (back.valid() && topology.isBoundary(back)) {
                    return true;
                }
            }
        }
        return false;
    };
    HalfedgeId start = Topology::halfedge(edge, 0);
    if (!topology.isBoundary(start)) {
        start = Topology::opposite(start);
    }
    std::vector<HalfedgeId> run;
    {
        bool crossed = false;
        HalfedgeId walk = start;
        do {
            run.push_back(walk);
            HalfedgeId next = topology.next(walk);
            if (!cracked(start) && cracked(next)) {
                // The next border edge of the point that is not a crack's.
                HalfedgeId found;
                u32 count = 0;
                for (const u32 m : membersOfPoint(work, *points, topology.to(walk).value())) {
                    for (const HalfedgeId h : topology.voh(VertexId(m))) {
                        if (topology.isBoundary(h) && !cracked(h)) {
                            found = h;
                            ++count;
                        }
                    }
                }
                if (count == 1) {
                    next = found;
                    crossed = true;
                }
            }
            walk = next;
        } while (walk.valid() && walk != start && run.size() <= topology.halfedgeCount());
        if (crossed && walk != start) {
            run.clear(); // the crossing did not come round: the loop as built
            walk = start;
            do {
                run.push_back(walk);
                walk = topology.next(walk);
            } while (walk.valid() && walk != start && run.size() <= topology.halfedgeCount());
        }
    }
    // Where the walk stepped across, the twins it joined become one vertex.
    std::vector<u32> into(topology.vertexCount());
    std::iota(into.begin(), into.end(), 0u);
    for (std::size_t i = 0; i < run.size(); ++i) {
        const u32 arrived = topology.to(run[i]).value();
        const u32 leaves = topology.from(run[(i + 1) % run.size()]).value();
        if (arrived != leaves) {
            into[std::max(arrived, leaves)] = std::min(arrived, leaves);
        }
    }
    const std::span<const Vector3f> places = positionsOf(work);
    const FaceSet set = work.faceSet();
    const FaceTable table = tableOf(set);
    if (!joinLanded(work, set, table, into, std::vector<Vector3f>(places.begin(), places.end())).empty()) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    std::vector<u32> loop;
    std::vector<u32> rim;
    std::vector<u32> rimFaces;
    for (const HalfedgeId walk : run) {
        const u32 v = into[topology.from(walk).value()];
        if (!loop.empty() && (loop.back() == v || loop.front() == v)) {
            continue; // a twin the join took in: one corner
        }
        loop.push_back(v);
        // The rim corner at that vertex: the halfedge leaving it inside the
        // face beside the border edge.
        rim.push_back(static_cast<u32>(topology.next(Topology::opposite(walk)).index()));
        rimFaces.push_back(topology.face(Topology::opposite(walk)).value());
    }
    if (loop.size() < 3) {
        plan.refusal = ModelRefusal::CapTooShort;
        return plan;
    }
    {
        std::vector<Vector3f> shape;
        for (const u32 v : loop) {
            shape.push_back(places[v]);
        }
        if (foldsOver(shape)) {
            plan.refusal = ModelRefusal::WouldFold;
            return plan;
        }
    }
    const std::vector<u32> sections(work.faceSections().begin(), work.faceSections().end());
    const Candidate candidate = candidateOf(table, sections, into, true);
    {
        // Capping a lone card would make a two-face pillow, which the next
        // rebuild's duplicate-face rule splits apart again.
        std::vector<u32> sorted = loop;
        std::sort(sorted.begin(), sorted.end());
        u32 corner = 0;
        for (u32 f = 0; f < candidate.faces.faceCount(); ++f) {
            const u32 valence = candidate.faces.faceValence[f];
            std::vector<u32> theirs(candidate.faces.cornerVertex.begin() + corner,
                                    candidate.faces.cornerVertex.begin() + corner + valence);
            corner += valence;
            std::sort(theirs.begin(), theirs.end());
            if (theirs == sorted) {
                plan.refusal = ModelRefusal::WouldDuplicateFace;
                return plan;
            }
        }
    }
    // The cap closes the loop, so its corners run the other way; each copies
    // the rim corner at its vertex, and the face takes the lowest-numbered rim
    // face's section and smoothing group.
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(work);
    std::unordered_map<u32, u32> cornerOfHalfedge;
    for (u32 c = 0; c < snapshot.size(); ++c) {
        cornerOfHalfedge[snapshot[c]] = c;
    }
    detail::RebuildMapping mapping;
    std::vector<u32> newId(into.size(), kInvalidId);
    for (u32 v = 0; v < into.size(); ++v) {
        if (into[v] == v && !topology.isDeleted(VertexId(v))) {
            newId[v] = static_cast<u32>(mapping.vertexSource.size());
            mapping.vertexSource.push_back(v);
        }
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    mapping.faces.faceValence = candidate.faces.faceValence;
    for (const u32 v : candidate.faces.cornerVertex) {
        mapping.faces.cornerVertex.push_back(newId[v]);
    }
    mapping.cornerSource = candidate.sourceCorner;
    mapping.faceSource = candidate.sourceFace;
    // The boundary loop runs the other way from the faces beside it, so the
    // cap takes it as it is and faces outward with them.
    mapping.faces.faceValence.push_back(static_cast<u32>(loop.size()));
    for (std::size_t i = 0; i < loop.size(); ++i) {
        mapping.faces.cornerVertex.push_back(newId[loop[i]]);
        const auto at = cornerOfHalfedge.find(rim[i]);
        mapping.cornerSource.push_back(at == cornerOfHalfedge.end() ? kInvalidId : at->second);
    }
    mapping.faceSource.push_back(*std::min_element(rimFaces.begin(), rimFaces.end()));
    const u32 slot = static_cast<u32>(mapping.faces.faceValence.size() - 1);
    const detail::RebuildResult result = detail::rebuild(work, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    // The rim is an edge the cap bent: the crease rule and the re-shade (X10,
    // CAP-1) read it.
    const Topology& built = std::as_const(work).topology();
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const HalfedgeId h =
            built.findHalfedge(VertexId(newId[loop[i]]), VertexId(newId[loop[(i + 1) % loop.size()]]));
        if (h.valid()) {
            plan.touchedEdges.push_back(Topology::edge(h).value());
        }
    }
    plan.touchedEdges = sortedUnique(std::move(plan.touchedEdges));
    plan.changed = 1;
    plan.changedFaces.push_back(slot); // its row is the finish's to materialise
    plan.selection.faces.push_back(slot);
    mesh = std::move(work);
    return plan;
}

ModelPlan PlanFlip(Mesh& mesh, const ElementSet& faces) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u8> flipped(topology.faceCount(), 0);
    u32 chosen = 0;
    for (const u32 f : sortedUnique(faces.faces)) {
        if (f < flipped.size() && !topology.isDeleted(FaceId(f)) && flipped[f] == 0) {
            flipped[f] = 1;
            ++chosen;
        }
    }
    if (chosen == 0) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    // Every border vertex of the region is duplicated once per fan of flipped
    // faces, so the region detaches cleanly rather than being shattered by the
    // repair (§3.10). A vertex every one of whose faces flips stays one.
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    detail::RebuildMapping mapping;
    std::vector<u32> newId(topology.vertexCount(), kInvalidId);
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (!topology.isDeleted(VertexId(v))) {
            newId[v] = static_cast<u32>(mapping.vertexSource.size());
            mapping.vertexSource.push_back(v);
        }
    }
    const u32 fresh = FreshMergeGroup(mesh);
    std::vector<u32> freshOf; // per new vertex past the originals: its group
    std::vector<u32> parted;  // the originals that gave copies, as the rebuild numbers them
    std::unordered_map<u64, u32> copyOfFan;
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (topology.isDeleted(VertexId(v)) || !topology.outgoing(VertexId(v)).valid()) {
            continue;
        }
        // The fan in order, from the face after the hole on a border vertex:
        // a run of flipped faces never runs across the hole (FLP-1).
        HalfedgeId first = topology.outgoing(VertexId(v));
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            if (!topology.isBoundary(h) && topology.isBoundary(Topology::opposite(h))) {
                first = h;
            }
        }
        std::vector<u32> around;
        bool border = false;
        bool open = false;
        HalfedgeId h = first;
        do {
            if (topology.isBoundary(h)) {
                open = true;
                break;
            }
            const u32 f = topology.face(h).value();
            around.push_back(f);
            border = border || flipped[f] == 0;
            h = Topology::opposite(topology.prev(h));
        } while (h.valid() && h != first && around.size() <= topology.halfedgeCount());
        open = open || topology.isBoundary(h);
        // A vertex every one of whose faces flips stays one vertex, on the
        // mesh's own border or not: there is nothing to detach it from.
        if (!border || around.empty()) {
            continue;
        }
        // The fan's runs, a closed fan's rotated so none wraps: the first run
        // left as it was keeps the vertex and every other run takes a copy, so
        // the region detaches along its border and no vertex is left with two
        // fans (FLP-4: two flipped faces touching at a corner).
        std::size_t start = 0;
        for (std::size_t i = 0; i < around.size() && !open; ++i) {
            if (flipped[around[i]] != flipped[around[(i + around.size() - 1) % around.size()]]) {
                start = i;
                break;
            }
        }
        u32 current = kInvalidId;
        bool kept = false;
        u8 state = 2;
        for (std::size_t i = 0; i < around.size(); ++i) {
            const u32 f = around[(start + i) % around.size()];
            if (flipped[f] != state) {
                state = flipped[f];
                current = kInvalidId;
                if (state != 0 || kept) {
                    current = static_cast<u32>(mapping.vertexSource.size());
                    mapping.vertexSource.push_back(v);
                    parted.push_back(newId[v]);
                    if (state != 0) {
                        freshOf.push_back(current);
                    }
                }
                kept = kept || state == 0;
            }
            if (current != kInvalidId) {
                copyOfFan[(static_cast<u64>(v) << 32) | f] = current;
            }
        }
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    std::vector<std::pair<u32, std::vector<u32>>> rows;
    u32 corner = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        const FaceId face(f);
        if (topology.isDeleted(face)) {
            continue;
        }
        std::vector<u32> loop;
        std::vector<u32> mine;
        for (const HalfedgeId h : topology.fh(face)) {
            loop.push_back(topology.from(h).value());
            mine.push_back(corner++);
        }
        const u32 slot = static_cast<u32>(mapping.faces.faceValence.size());
        mapping.faces.faceValence.push_back(static_cast<u32>(loop.size()));
        mapping.faceSource.push_back(f);
        const auto nameOf = [&](u32 v) {
            const auto at = copyOfFan.find((static_cast<u64>(v) << 32) | f);
            return at != copyOfFan.end() ? at->second : newId[v];
        };
        if (flipped[f] == 0) {
            for (std::size_t i = 0; i < loop.size(); ++i) {
                mapping.faces.cornerVertex.push_back(nameOf(loop[i]));
                mapping.cornerSource.push_back(mine[i]);
            }
            continue;
        }
        for (std::size_t i = loop.size(); i-- > 0;) {
            mapping.faces.cornerVertex.push_back(nameOf(loop[i]));
            mapping.cornerSource.push_back(mine[i]);
        }
        // Its drawn triangles, renamed and reversed (§3.10): the stored row,
        // or the cut it is drawn with now, so the reversed face draws the same
        // triangles rather than re-cutting the other way (FLP-5).
        if (loop.size() > 3) {
            const std::vector<u32> drawn = drawnOf(mesh, face);
            std::vector<u32> made;
            for (std::size_t t = 0; t + 2 < drawn.size(); t += 3) {
                made.push_back(nameOf(drawn[t + 2]));
                made.push_back(nameOf(drawn[t + 1]));
                made.push_back(nameOf(drawn[t]));
            }
            rows.emplace_back(slot, std::move(made));
        }
        plan.changedFaces.push_back(slot);
    }
    const u32 slotCount = static_cast<u32>(mapping.faces.faceValence.size());
    // On a copy: a region the repair would shatter is refused with the mesh as
    // it was (FLP-3).
    Mesh work = mesh;
    const detail::RebuildResult result = detail::rebuild(work, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan = ModelPlan{};
        plan.refusal = ModelRefusal::WouldFold; // the region would not detach
        return plan;
    }
    // The copies are their own GPU vertices: a fresh `mergeGroup` each. They
    // stay one point with their sources, as a flipped face stays attached in
    // Max and Blender (a split the kernel needs, like a crack's, not a copy).
    if (!freshOf.empty()) {
        const std::span<u32> groups = MergeGroupsOf(work);
        u32 next = fresh;
        for (const u32 v : freshOf) {
            if (v < groups.size()) {
                groups[v] = next++;
            }
        }
    }
    // The flipped corners: their normals and their tangents' `w` negated, so
    // the bitangent `w * cross(n, t)` is kept. Face order stood.
    {
        const Topology& now = std::as_const(work).topology();
        const std::span<Vector3f> normals = work.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
        const std::span<Vector4f> tangents = work.attributes.get<Vector4f>(names::kTangent, Domain::Halfedge);
        for (const u32 f : plan.changedFaces) {
            if (f >= now.faceCount() || now.isDeleted(FaceId(f))) {
                continue;
            }
            for (const HalfedgeId h : now.fh(FaceId(f))) {
                const u32 c = static_cast<u32>(h.index());
                if (c < normals.size()) {
                    normals[c] = {-normals[c].x, -normals[c].y, -normals[c].z};
                }
                if (c < tangents.size()) {
                    tangents[c].w = -tangents[c].w;
                }
                plan.reshade.push_back(now.from(h).value());
            }
        }
    }
    for (const auto& entry : rows) {
        work.triangulation.setRow(entry.first, entry.second, slotCount);
    }
    plan.changed = chosen;
    plan.selection.faces = sortedUnique(plan.changedFaces);
    // The faces left beside the border it opened shade afresh too.
    plan.reshade.insert(plan.reshade.end(), parted.begin(), parted.end());
    plan.reshade = sortedUnique(std::move(plan.reshade));
    mesh = std::move(work);
    return plan;
}

ModelPlan PlanSharpen(Mesh& mesh, const ElementSet& edges, bool hard) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    if (edges.edges.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    const std::span<u8> sharp = mesh.attributes.getOrCreate<u8>(names::kSharp, Domain::Edge, AttrType::Bool);
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> places = positionsOf(mesh);
    for (const u32 e : sortedUnique(edges.edges)) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e)) || e >= sharp.size()) {
            continue;
        }
        if ((sharp[e] != 0) == hard) {
            continue;
        }
        sharp[e] = hard ? 1 : 0;
        ++plan.changed;
        // Named as motions rather than touched edges: the finish's crease rule
        // would harden a soft edge again (§3.12). What is wanted here is its
        // re-shade, which motions ask for too.
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        for (const u32 v : {topology.from(h).value(), topology.to(h).value()}) {
            VertexMotion motion;
            motion.vertex = v;
            motion.base = v < places.size() ? places[v] : Vector3f{0, 0, 0};
            plan.motions.push_back(motion);
        }
    }
    plan.renumbers = false; // nothing moved but the flag
    plan.selection.edges = sortedUnique(edges.edges);
    return plan;
}

// ============================================================================
// Extrude, Border Extrude and Inset (§3.8)
// ============================================================================

namespace {

u64 pointPair(u32 a, u32 b) {
    return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
}

Vector3f faceArea(const Mesh& mesh, FaceId face);
Vector3f unitOr(const Vector3f& v, const Vector3f& fallback);

/// The mesh's border edges keyed by their ends' points: two on one key are
/// twins, which is all the weld left of a crease it kept apart (§2.2). Two
/// faces folded flat onto each other -- a double-sided card's front and back --
/// are no crease: they are two surfaces, and their edges no twins (EXT-2, X13).
class Twins {
public:
    Twins(const Mesh& mesh, const PointTable& points) {
        const Topology& topology = mesh.topology();
        const std::span<const Vector3f> places = positionsOf(mesh);
        keyOf_.assign(topology.edgeCount(), 0);
        normal_.assign(topology.edgeCount(), Vector3f{0.0f, 0.0f, 0.0f});
        inward_.assign(topology.edgeCount(), Vector3f{0.0f, 0.0f, 0.0f});
        const auto pointOf = [&](u32 vertex) {
            return points.pointOf.size() == topology.vertexCount() ? points.pointOf[vertex] : vertex;
        };
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            if (topology.isDeleted(EdgeId(e)) || !topology.isBoundary(EdgeId(e))) {
                continue;
            }
            HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            const u64 key = pointPair(pointOf(topology.from(h).value()), pointOf(topology.to(h).value()));
            keyOf_[e] = key;
            byPair_[key].push_back(e);
            if (!topology.face(h).valid()) {
                h = Topology::opposite(h);
            }
            const u32 a = topology.from(h).value();
            const u32 b = topology.to(h).value();
            if (topology.face(h).valid() && a < places.size() && b < places.size()) {
                normal_[e] = unitOr(faceArea(mesh, topology.face(h)), Vector3f{0.0f, 0.0f, 0.0f});
                inward_[e] = cross(normal_[e], places[b] - places[a]); // the face is left of h
            }
        }
    }

    /// The border edges across the crack from @p edge, @p edge aside.
    std::vector<u32> of(u32 edge) const {
        std::vector<u32> out;
        if (edge >= keyOf_.size() || keyOf_[edge] == 0) {
            return out;
        }
        const auto found = byPair_.find(keyOf_[edge]);
        if (found == byPair_.end()) {
            return out;
        }
        for (const u32 other : found->second) {
            const bool folded = normal_[edge].dot(normal_[other]) < -0.99f && inward_[edge].dot(inward_[other]) > 0.0f;
            if (other != edge && !folded) {
                out.push_back(other);
            }
        }
        return out;
    }

private:
    std::vector<u64> keyOf_;
    std::vector<Vector3f> normal_; ///< Each border edge's face's unit normal.
    std::vector<Vector3f> inward_; ///< Into that face, square to the edge.
    std::unordered_map<u64, std::vector<u32>> byPair_;
};

/// A face's area-weighted normal: twice the area, times the unit normal, from
/// Newell's formula, which holds for a polygon of any valence.
Vector3f faceArea(const Mesh& mesh, FaceId face) {
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> places = positionsOf(mesh);
    Vector3f sum{0.0f, 0.0f, 0.0f};
    for (const HalfedgeId h : topology.fh(face)) {
        const u32 a = topology.from(h).value();
        const u32 b = topology.to(h).value();
        if (a >= places.size() || b >= places.size()) {
            continue;
        }
        sum = sum + cross(places[a], places[b]);
    }
    return sum * 0.5f;
}

Vector3f unitOr(const Vector3f& v, const Vector3f& fallback) {
    const f32 length = v.length();
    return length > 1e-12f ? v * (1.0f / length) : fallback;
}

/// The selected faces grouped into regions (§3.8): connected across edges and
/// across twin edges. `perFace` puts each face in a region of its own.
std::vector<std::vector<u32>> regionsOf(const Mesh& mesh, const Twins& twins, const std::vector<u32>& chosen,
                                        bool perFace) {
    std::vector<std::vector<u32>> regions;
    if (perFace) {
        for (const u32 f : chosen) {
            regions.push_back({f});
        }
        return regions;
    }
    const Topology& topology = mesh.topology();
    std::unordered_map<u32, u32> parent;
    for (const u32 f : chosen) {
        parent[f] = f;
    }
    const std::function<u32(u32)> find = [&](u32 f) {
        u32 root = f;
        while (parent[root] != root) {
            root = parent[root];
        }
        while (parent[f] != root) {
            const u32 next = parent[f];
            parent[f] = root;
            f = next;
        }
        return root;
    };
    const auto join = [&](u32 a, u32 b) {
        if (parent.count(a) != 0 && parent.count(b) != 0) {
            parent[find(a)] = find(b);
        }
    };
    for (const u32 f : chosen) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const FaceId across = topology.face(Topology::opposite(h));
            if (across.valid() && !topology.isDeleted(across)) {
                join(f, across.value());
                continue;
            }
            // A crack: the face on the far side of the twin edge is as good as
            // a neighbour, so a crease the weld kept apart is one region.
            for (const u32 other : twins.of(Topology::edge(h).value())) {
                for (u32 side = 0; side < 2; ++side) {
                    const FaceId beside = topology.face(Topology::halfedge(EdgeId(other), side));
                    if (beside.valid() && !topology.isDeleted(beside)) {
                        join(f, beside.value());
                    }
                }
            }
        }
    }
    std::unordered_map<u32, std::vector<u32>> groups;
    for (const u32 f : chosen) {
        groups[find(f)].push_back(f);
    }
    for (auto& entry : groups) {
        regions.push_back(sortedUnique(std::move(entry.second)));
    }
    std::sort(regions.begin(), regions.end());
    return regions;
}

/// What a region grows: its boundary halfedges (the side inside it), the
/// vertices on them, and one copy of each such vertex per fan of its faces.
struct Grown {
    std::vector<HalfedgeId> boundary;  ///< One per boundary edge, face inside the region.
    std::vector<u32> inside;           ///< Vertices of the region that are not on its boundary.
    std::unordered_map<u64, u32> copyOfFan; ///< (vertex << 32 | face) -> the copy it uses.
    std::vector<u32> copies;           ///< Every copy made, in the order it was made.
    std::vector<u32> copiedFrom;       ///< Per copy, the vertex it came from.
};

/// @p region's boundary and its per-fan copies, appended to @p mapping's
/// vertices (§3.8). A boundary edge has exactly one side in *this* region; a
/// twin edge with both sides in it has none. `regionOf` names every face's
/// region, so By Polygon walls the edge between two chosen faces twice.
Grown growRegion(const Mesh& mesh, const Twins& twins, const std::vector<u32>& region,
                 const std::vector<u32>& regionOf, u32 index, detail::RebuildMapping& mapping,
                 const std::vector<u8>& pinned) {
    Grown grown;
    const Topology& topology = mesh.topology();
    const auto selected = [&](FaceId f) {
        return f.valid() && f.value() < regionOf.size() && regionOf[f.value()] == index;
    };
    // Sorted ids, not a flag per mesh vertex: By Polygon grows a region per
    // face, and a whole-mesh press would clear the mesh's worth each time.
    std::vector<u32> onBoundary;
    for (const u32 f : region) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const FaceId across = topology.face(Topology::opposite(h));
            if (selected(across)) {
                continue; // an inside edge
            }
            bool twinInside = false;
            if (!across.valid()) {
                for (const u32 other : twins.of(Topology::edge(h).value())) {
                    for (u32 side = 0; side < 2; ++side) {
                        twinInside = twinInside || selected(topology.face(Topology::halfedge(EdgeId(other), side)));
                    }
                }
            }
            if (twinInside) {
                continue; // a crease the region holds both sides of
            }
            grown.boundary.push_back(h);
            onBoundary.push_back(topology.from(h).value());
            onBoundary.push_back(topology.to(h).value());
        }
    }
    onBoundary = sortedUnique(std::move(onBoundary));
    // Every vertex of the region: the ones on its boundary are copied once per
    // fan of its faces, the rest move as they are.
    std::vector<u32> vertices;
    for (const u32 f : region) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            vertices.push_back(topology.from(h).value());
        }
    }
    for (const u32 v : sortedUnique(std::move(vertices))) {
        if (v < pinned.size() && pinned[v] != 0) {
            continue; // a hinge's vertex (§3.8): neither copied nor moved
        }
        if (!std::binary_search(onBoundary.begin(), onBoundary.end(), v)) {
            grown.inside.push_back(v);
            continue;
        }
        // The fan's runs, rotated so none wraps: one copy per run, which keeps
        // a region that touches itself at a vertex manifold.
        std::vector<u32> around;
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            if (!topology.isBoundary(h)) {
                around.push_back(topology.face(h).value());
            }
        }
        if (around.empty()) {
            continue;
        }
        std::size_t start = 0;
        for (std::size_t i = 0; i < around.size(); ++i) {
            if (!selected(FaceId(around[i]))) {
                start = (i + 1) % around.size();
                break;
            }
        }
        u32 current = kInvalidId;
        for (std::size_t i = 0; i < around.size(); ++i) {
            const u32 f = around[(start + i) % around.size()];
            if (!selected(FaceId(f))) {
                current = kInvalidId;
                continue;
            }
            if (current == kInvalidId) {
                current = static_cast<u32>(mapping.vertexSource.size());
                mapping.vertexSource.push_back(v);
                grown.copies.push_back(current);
                grown.copiedFrom.push_back(v);
            }
            grown.copyOfFan[(static_cast<u64>(v) << 32) | f] = current;
        }
    }
    return grown;
}

/// The corner ordinal of each halfedge in @p snapshot, for `cornerSource`.
std::unordered_map<u32, u32> cornerOrdinals(const std::vector<u32>& snapshot) {
    std::unordered_map<u32, u32> out;
    for (u32 c = 0; c < snapshot.size(); ++c) {
        out[snapshot[c]] = c;
    }
    return out;
}

/// Where each face's corners begin in a face set's `cornerVertex`.
std::vector<u32> cornerBases(const FaceSet& faces) {
    std::vector<u32> base(faces.faceCount() + 1, 0);
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        base[f + 1] = base[f] + faces.faceValence[f];
    }
    return base;
}

/// The new vertex @p face uses at @p vertex: its copy when the fan has one,
/// else the vertex itself.
u32 useAt(const Grown& grown, u32 vertex, u32 face) {
    const auto found = grown.copyOfFan.find((static_cast<u64>(vertex) << 32) | face);
    return found == grown.copyOfFan.end() ? vertex : found->second;
}

/// One wall of a strip or a ring: @p loop's four corners, with a corner that
/// repeats folded away -- a pinned end (§3.8's hinge) is its own copy, so a
/// wall pinned at one end is a triangle, and one pinned at both is the hinge
/// edge itself and is nothing. Returns the new face's slot, or `kInvalidId`
/// when there was no wall to add.
u32 appendWall(detail::RebuildMapping& mapping, const std::unordered_map<u32, u32>& ordinalOf,
               const u32 (&loop)[4], const HalfedgeId (&sources)[4], u32 sourceFace) {
    u32 corners[4] = {};
    HalfedgeId froms[4] = {sources[0], sources[1], sources[2], sources[3]};
    u32 n = 0;
    for (u32 i = 0; i < 4; ++i) {
        if (n != 0 && corners[n - 1] == loop[i]) {
            continue;
        }
        corners[n] = loop[i];
        froms[n] = sources[i];
        ++n;
    }
    if (n > 1 && corners[n - 1] == corners[0]) {
        --n;
    }
    if (n < 3) {
        return kInvalidId;
    }
    const u32 slot = static_cast<u32>(mapping.faces.faceValence.size());
    mapping.faces.faceValence.push_back(n);
    for (u32 i = 0; i < n; ++i) {
        mapping.faces.cornerVertex.push_back(corners[i]);
        const auto found = ordinalOf.find(static_cast<u32>(froms[i].index()));
        mapping.cornerSource.push_back(found == ordinalOf.end() ? kInvalidId : found->second);
    }
    mapping.faceSource.push_back(sourceFace);
    return slot;
}

/// The rings Extrude and Inset both build (§3.8): every region's faces moved
/// onto their per-fan copies, and one quad on each boundary edge, wound the way
/// the region face's own halfedge runs and taking that face's two corners at
/// the edge (§3.12's zero-width strip). Nothing moves here: the amount does
/// that. The regions' `Grown` come back for the caller to aim.
std::vector<Grown> growRings(const Mesh& mesh, const Twins& twins,
                             const std::vector<std::vector<u32>>& regions,
                             const std::vector<u32>& regionOf,
                             const std::unordered_map<u32, u32>& ordinalOf, const std::vector<u32>& base,
                             detail::RebuildMapping& mapping, ModelPlan& plan,
                             const std::vector<u8>& pinned) {
    const Topology& topology = mesh.topology();
    std::vector<Grown> grown;
    for (u32 r = 0; r < regions.size(); ++r) {
        grown.push_back(growRegion(mesh, twins, regions[r], regionOf, r, mapping, pinned));
        const Grown& made = grown.back();
        for (const u32 f : regions[r]) {
            u32 corner = base[f];
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                mapping.faces.cornerVertex[corner++] = useAt(made, topology.from(h).value(), f);
            }
            plan.changedFaces.push_back(f);
            plan.selection.faces.push_back(f);
        }
        for (const HalfedgeId h : made.boundary) {
            const u32 f = topology.face(h).value();
            const u32 from = topology.from(h).value();
            const u32 to = topology.to(h).value();
            const HalfedgeId next = topology.next(h);
            const u32 loop[4] = {from, to, useAt(made, to, f), useAt(made, from, f)};
            const HalfedgeId sources[4] = {h, next, next, h};
            const u32 ring = appendWall(mapping, ordinalOf, loop, sources, f);
            if (ring != kInvalidId) {
                plan.changedFaces.push_back(ring);
            }
        }
    }
    return grown;
}

/// Every edge of every face a tool changed, for §3.12's derivations.
void touchFaces(const Mesh& mesh, ModelPlan& plan) {
    const Topology& topology = std::as_const(mesh).topology();
    for (const u32 f : plan.changedFaces) {
        if (f >= topology.faceCount() || topology.isDeleted(FaceId(f))) {
            continue;
        }
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            plan.touchedEdges.push_back(Topology::edge(h).value());
        }
    }
    plan.touchedEdges = sortedUnique(std::move(plan.touchedEdges));
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    plan.selection.normalise();
}

/// The fresh merge groups @p made ask for, so nothing welds a copy back onto
/// what it came from, and a fresh `apart` stamp, so no point joins it to its
/// source either (Q8).
void freshGroups(Mesh& mesh, const std::vector<u32>& made) {
    if (made.empty()) {
        return;
    }
    const std::span<u32> groups = MergeGroupsOf(mesh);
    u32 next = FreshMergeGroup(mesh);
    for (const u32 v : made) {
        if (v < groups.size()) {
            groups[v] = next++;
        }
    }
    MarkApart(mesh, made);
}

/// Every per-fan copy @p rings made, ascending.
std::vector<u32> copiesOf(const std::vector<Grown>& rings) {
    std::vector<u32> made;
    for (const Grown& grown : rings) {
        made.insert(made.end(), grown.copies.begin(), grown.copies.end());
    }
    return sortedUnique(std::move(made));
}

} // namespace

ModelPlan PlanExtrudeFaces(Mesh& mesh, const PointTable& points, const ElementSet& faces, ExtrudeType type) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u8> inRegion(topology.faceCount(), 0);
    std::vector<u32> chosen;
    for (const u32 f : sortedUnique(faces.faces)) {
        if (f < topology.faceCount() && !topology.isDeleted(FaceId(f)) && inRegion[f] == 0) {
            inRegion[f] = 1;
            chosen.push_back(f);
        }
    }
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    const std::span<const Vector3f> places = positionsOf(mesh);
    const Twins twins(mesh, points);
    const std::vector<std::vector<u32>> regions =
        regionsOf(mesh, twins, chosen, type == ExtrudeType::ByPolygon);
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    const std::vector<u32> base = cornerBases(mapping.faces);
    std::vector<u32> regionOf(topology.faceCount(), kInvalidId);
    for (u32 r = 0; r < regions.size(); ++r) {
        for (const u32 f : regions[r]) {
            regionOf[f] = r;
        }
    }
    const std::vector<u8> nothingPinned;
    const std::vector<Grown> rings =
        growRings(mesh, twins, regions, regionOf, ordinalOf, base, mapping, plan, nothingPinned);
    std::vector<std::pair<u32, Vector3f>> moves; // vertex (new id) -> direction
    for (u32 r = 0; r < regions.size(); ++r) {
        const std::vector<u32>& region = regions[r];
        const Grown& grown = rings[r];
        // The region's own direction, and Group's two fallbacks (§3.8).
        Vector3f area{0.0f, 0.0f, 0.0f};
        f32 total = 0.0f;
        for (const u32 f : region) {
            const Vector3f weighted = faceArea(mesh, FaceId(f));
            area = area + weighted;
            total += weighted.length();
        }
        const Vector3f mean = total > 1e-12f ? area * (1.0f / total) : Vector3f{0.0f, 0.0f, 0.0f};
        bool group = type != ExtrudeType::LocalNormal;
        if (group && mean.length() < 0.1f) {
            group = false;
            plan.fellBack = true;
        }
        if (group) {
            const Vector3f aim = unitOr(mean, Vector3f{0.0f, 0.0f, 1.0f});
            for (const HalfedgeId h : grown.boundary) {
                const u32 a = topology.from(h).value();
                const u32 b = topology.to(h).value();
                if (a >= places.size() || b >= places.size()) {
                    continue;
                }
                const Vector3f along = unitOr(places[b] - places[a], Vector3f{0.0f, 0.0f, 0.0f});
                if (std::abs(along.dot(aim)) > 0.9998477f) { // within a degree: a wall of no area
                    group = false;
                    plan.fellBack = true;
                    break;
                }
            }
        }
        const Vector3f aim = unitOr(mean, Vector3f{0.0f, 0.0f, 1.0f});
        // Local Normal within this region: its own faces' normals summed per
        // point, so a crack's twins move as one, and a shell that only touches
        // the region at a point (another region) never bends it (EXT-4).
        std::unordered_map<u32, Vector3f> localSum;
        const auto pointOf = [&](u32 vertex) {
            return points.pointOf.size() == topology.vertexCount() && vertex < points.pointOf.size()
                       ? points.pointOf[vertex]
                       : vertex;
        };
        if (!group) {
            for (const u32 f : region) {
                const Vector3f normal = unitOr(faceArea(mesh, FaceId(f)), Vector3f{0.0f, 0.0f, 0.0f});
                for (const HalfedgeId h : topology.fh(FaceId(f))) {
                    Vector3f& sum = localSum[pointOf(topology.from(h).value())];
                    sum = sum + normal;
                }
            }
        }
        const auto directionOf = [&](u32 vertex) {
            if (group) {
                return aim;
            }
            const auto found = localSum.find(pointOf(vertex));
            return found == localSum.end() ? aim : unitOr(found->second, aim);
        };
        for (const u32 v : grown.inside) {
            moves.push_back({v, directionOf(v)});
        }
        for (std::size_t i = 0; i < grown.copies.size(); ++i) {
            moves.push_back({grown.copies[i], directionOf(grown.copiedFrom[i])});
        }
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    mapping.mayHaveZeroArea = true; // the walls have none until the amount lifts them
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    // A copy carries its source's `mergeGroup` across the rebuild, and the
    // amount then lifts it off that vertex: without a group of its own the two
    // would still be one point, so the walls would draw as one vertex and a
    // move of the lifted face would drag the ground with it.
    freshGroups(mesh, copiesOf(rings));
    const std::span<const Vector3f> now = positionsOf(mesh);
    for (const auto& [vertex, direction] : moves) {
        VertexMotion motion;
        motion.vertex = vertex;
        motion.base = vertex < now.size() ? now[vertex] : Vector3f{0.0f, 0.0f, 0.0f};
        motion.direction = direction;
        plan.motions.push_back(motion);
    }
    // Every edge of a wall is new or re-shaped, and so is every edge of the
    // faces that moved: §3.12 hardens the ones that turned a corner.
    touchFaces(mesh, plan);
    plan.changed = static_cast<u32>(chosen.size());
    AmountRange(mesh, plan);
    return plan;
}

ModelPlan PlanHingeFaces(Mesh& mesh, const PointTable& points, const ElementSet& faces, u32 hingeEdge) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u8> inRegion(topology.faceCount(), 0);
    std::vector<u32> chosen;
    for (const u32 f : sortedUnique(faces.faces)) {
        if (f < topology.faceCount() && !topology.isDeleted(FaceId(f)) && inRegion[f] == 0) {
            inRegion[f] = 1;
            chosen.push_back(f);
        }
    }
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    // The hinge: a live edge with exactly one side in the selection. The
    // halfedge on that side runs the way the region face is wound, so it is
    // the axis, and a positive angle lifts the region along its own normal.
    if (hingeEdge >= topology.edgeCount() || topology.isDeleted(EdgeId(hingeEdge))) {
        plan.refusal = ModelRefusal::HingeNotOnBoundary;
        return plan;
    }
    const auto chosenFace = [&](HalfedgeId h) {
        const FaceId f = topology.face(h);
        return f.valid() && f.value() < inRegion.size() && inRegion[f.value()] != 0;
    };
    HalfedgeId along = Topology::halfedge(EdgeId(hingeEdge), 0);
    const bool sideA = chosenFace(along);
    const bool sideB = chosenFace(Topology::opposite(along));
    if (sideA == sideB) {
        plan.refusal = ModelRefusal::HingeNotOnBoundary;
        return plan;
    }
    // A crack's edge whose twin's face is selected too is inside the selection,
    // however its own other side looks (Q5, HNG-1).
    const Twins twins(mesh, points);
    for (const u32 other : twins.of(hingeEdge)) {
        for (u32 side = 0; side < 2; ++side) {
            if (chosenFace(Topology::halfedge(EdgeId(other), side))) {
                plan.refusal = ModelRefusal::HingeNotOnBoundary;
                return plan;
            }
        }
    }
    if (!sideA) {
        along = Topology::opposite(along);
    }
    const u32 hingeFrom = topology.from(along).value();
    const u32 hingeTo = topology.to(along).value();
    const std::span<const Vector3f> places = positionsOf(mesh);
    if (hingeFrom >= places.size() || hingeTo >= places.size()) {
        plan.refusal = ModelRefusal::HingeNotOnBoundary;
        return plan;
    }
    HingeAxis hinge;
    hinge.origin = places[hingeFrom];
    hinge.axis = unitOr(places[hingeTo] - places[hingeFrom], Vector3f{0.0f, 0.0f, 1.0f});
    std::vector<u8> pinned(topology.vertexCount(), 0);
    pinned[hingeFrom] = 1;
    pinned[hingeTo] = 1;
    // Any other vertex of the selection on the hinge's line cannot move either:
    // a copy of it would stay on top of it, a wall of no area (FUZ-11, MTX-12).
    {
        const f32 tolerance = CoincidenceTolerance(mesh);
        for (const u32 f : chosen) {
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                const u32 v = topology.from(h).value();
                if (v < places.size() && cross(places[v] - hinge.origin, hinge.axis).length() <= tolerance) {
                    pinned[v] = 1;
                }
            }
        }
    }

    // Extrude's regions, copies and walls (Group: the selection as one), with
    // the hinge's two vertices pinned -- the wall on the hinge edge is then
    // nothing and its neighbours are triangles (`appendWall`).
    const std::vector<std::vector<u32>> regions = regionsOf(mesh, twins, chosen, false);
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    const std::vector<u32> base = cornerBases(mapping.faces);
    std::vector<u32> regionOf(topology.faceCount(), kInvalidId);
    for (u32 r = 0; r < regions.size(); ++r) {
        for (const u32 f : regions[r]) {
            regionOf[f] = r;
        }
    }
    const std::vector<Grown> rings = growRings(mesh, twins, regions, regionOf, ordinalOf, base, mapping, plan, pinned);
    std::vector<u32> movers;
    for (const Grown& grown : rings) {
        movers.insert(movers.end(), grown.inside.begin(), grown.inside.end());
        movers.insert(movers.end(), grown.copies.begin(), grown.copies.end());
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    mapping.mayHaveZeroArea = true; // the walls have none until the angle opens them
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    freshGroups(mesh, copiesOf(rings));
    const std::span<const Vector3f> now = positionsOf(mesh);
    for (const u32 v : sortedUnique(std::move(movers))) {
        VertexMotion motion;
        motion.vertex = v;
        motion.base = v < now.size() ? now[v] : Vector3f{0.0f, 0.0f, 0.0f};
        plan.motions.push_back(motion); // no direction: the hinge turns it
    }
    plan.hinge = hinge;
    touchFaces(mesh, plan);
    plan.changed = static_cast<u32>(chosen.size());
    return plan; // unbounded: `AmountRange` leaves a hinge alone
}

/// The strip both border tools grow (§3.8): one quad per border edge in
/// @p edges on copies of the run's vertices, aimed outward in the surface.
/// @p pinned (sorted) are the vertices a hinge keeps in place -- each its own
/// copy, so the walls at it are triangles and it takes no motion.
static ModelPlan planBorderStrip(Mesh& mesh, const PointTable& points, const ElementSet& edges,
                                 const std::vector<u32>& pinned) {
    const auto isPinned = [&](u32 v) { return std::binary_search(pinned.begin(), pinned.end(), v); };
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    // The border halfedges asked for: the side with no face, which is the run
    // the strip grows from.
    // A crack's edge is no border: its twins are one edge (Q5), inside the
    // surface, and dropped as an interior edge is (EXB-1, EXB-4).
    const Twins twins(mesh, points);
    std::vector<HalfedgeId> border;
    std::vector<u8> taken(topology.edgeCount(), 0);
    for (const u32 e : sortedUnique(edges.edges)) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e)) || !topology.isBoundary(EdgeId(e)) ||
            taken[e] != 0 || !twins.of(e).empty()) {
            continue;
        }
        taken[e] = 1;
        HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        if (topology.face(h).valid()) {
            h = Topology::opposite(h);
        }
        border.push_back(h);
    }
    if (border.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    // One copy per border vertex, aimed along the mean of its border edges'
    // OUTWARD directions: for each edge, away from the face behind it, square
    // to the edge in that face's plane.
    //
    // Along the face's normal instead -- which this did until 2026-09-24 --
    // the strip stands up off the surface as a collar, and a plane extruded at
    // its border grows a wall rather than getting wider. Outward, it CONTINUES
    // the surface, which is what extruding a border is for: a plane gets
    // wider, a tube's rim runs on along the tube (the face behind that rim is
    // behind it in exactly that sense), and a hole's border closes inward.
    const std::span<const Vector3f> placed = positionsOf(mesh);
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    std::unordered_map<u32, u32> copyOf;
    std::unordered_map<u32, Vector3f> aimOf;
    for (const HalfedgeId h : border) {
        const HalfedgeId inside = Topology::opposite(h);
        const FaceId face = topology.face(inside);
        // A face with no area (corners in line) has no plane of its own: it
        // takes its neighbours', never a world axis, so the strip turns with
        // the mesh (EXB-3, P15).
        Vector3f area = faceArea(mesh, face);
        const Vector3f span = placed[topology.to(h).value()] - placed[topology.from(h).value()];
        if (!(area.length() > 1e-6f * span.dot(span))) {
            for (const HalfedgeId c : topology.fh(face)) {
                if (const FaceId beside = topology.face(Topology::opposite(c)); beside.valid() && beside != face) {
                    area = area + faceArea(mesh, beside);
                }
            }
        }
        const Vector3f normal = unitOr(area, Vector3f{0.0f, 0.0f, 1.0f});
        const u32 from = topology.from(h).value();
        const u32 to = topology.to(h).value();
        Vector3f outward = normal; // an edge of no length
        if (from < placed.size() && to < placed.size()) {
            // In the face's plane, so a bowed n-gon's strip still lies in the
            // surface, and square to the edge, so a run of triangles grows a
            // strip and not a sawtooth: the left of the border halfedge, which
            // runs against its face. Taken from the face's centre instead (until
            // 2026-11-12), a concave face's centre could lie on the edge's line
            // and the aim was rounding (MTX-11, EXB-3).
            const Vector3f along = unitOr(placed[to] - placed[from], Vector3f{0.0f, 0.0f, 0.0f});
            outward = unitOr(cross(normal, along), normal);
        }
        for (const u32 v : {from, to}) {
            if (isPinned(v)) {
                continue; // the hinge: its own copy, and it goes nowhere
            }
            if (copyOf.find(v) == copyOf.end()) {
                copyOf[v] = static_cast<u32>(mapping.vertexSource.size());
                mapping.vertexSource.push_back(v);
                aimOf[v] = Vector3f{0.0f, 0.0f, 0.0f};
            }
            aimOf[v] = aimOf[v] + outward;
        }
    }
    const auto copyOrSelf = [&](u32 v) { return isPinned(v) ? v : copyOf[v]; };
    for (const HalfedgeId h : border) {
        const HalfedgeId inside = Topology::opposite(h);
        const u32 f = topology.face(inside).value();
        const u32 from = topology.from(h).value();
        const u32 to = topology.to(h).value();
        // The border halfedge runs against the face beside it, so the strip's
        // quad takes its ends the way the border does and faces outward with
        // that face. At a pinned end it is a triangle.
        const u32 loop[4] = {from, to, copyOrSelf(to), copyOrSelf(from)};
        const HalfedgeId sources[4] = {topology.next(inside), inside, inside, topology.next(inside)};
        const u32 quad = appendWall(mapping, ordinalOf, loop, sources, f);
        if (quad != kInvalidId) {
            plan.changedFaces.push_back(quad);
        }
    }
    // A crack's twins are one point (§2.2), so they take one aim between them
    // and the strip grows without parting them. Summed now: past the rebuild
    // the point table no longer matches the mesh (EXB-1).
    std::unordered_map<u32, Vector3f> aimOfPoint;
    for (const auto& [vertex, copy] : copyOf) {
        (void)copy;
        Vector3f aim{0.0f, 0.0f, 0.0f};
        for (const u32 member : membersOfPoint(mesh, points, vertex)) {
            const auto found = aimOf.find(member);
            if (found != aimOf.end()) {
                aim = aim + found->second;
            }
        }
        aimOfPoint[vertex] = aim;
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    mapping.mayHaveZeroArea = true; // the strip has none until the amount grows it
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    std::vector<u32> grew;
    for (const auto& [vertex, copy] : copyOf) {
        grew.push_back(copy);
    }
    const std::vector<u32> copies = sortedUnique(std::move(grew));
    freshGroups(mesh, copies); // the strip's far side is its own point
    const std::span<const Vector3f> now = positionsOf(mesh);
    for (const auto& [vertex, copy] : copyOf) {
        VertexMotion motion;
        const Vector3f aim = aimOfPoint[vertex];
        motion.vertex = copy;
        motion.base = copy < now.size() ? now[copy] : Vector3f{0.0f, 0.0f, 0.0f};
        motion.direction = unitOr(aim, Vector3f{0.0f, 0.0f, 1.0f});
        plan.motions.push_back(motion);
    }
    std::sort(plan.motions.begin(), plan.motions.end(),
              [](const VertexMotion& a, const VertexMotion& b) { return a.vertex < b.vertex; });
    const Topology& built = std::as_const(mesh).topology();
    const auto isCopy = [&](VertexId v) { return std::binary_search(copies.begin(), copies.end(), v.value()); };
    const auto onFarSide = [&](VertexId v) { return isCopy(v) || isPinned(v.value()); };
    for (const u32 f : plan.changedFaces) {
        if (f >= built.faceCount() || built.isDeleted(FaceId(f))) {
            continue;
        }
        for (const HalfedgeId h : built.fh(FaceId(f))) {
            plan.touchedEdges.push_back(Topology::edge(h).value());
            // The strip's far side -- the copies of the edges it grew from, a
            // quad's one edge with a copy at both ends, or a triangle's from
            // its copy to the pinned hinge -- is the selection after Apply
            // (§3.8): the gizmo's, and what the next extrude continues. NOT
            // the side edges at an open run's two ends, border though they
            // are: selected, the next extrude would grow flaps sideways off
            // the strip rather than run it on. (The whole rim, ends included,
            // until 2026-09-24.)
            if (onFarSide(built.from(h)) && onFarSide(built.to(h)) &&
                (isCopy(built.from(h)) || isCopy(built.to(h)))) {
                plan.selection.edges.push_back(Topology::edge(h).value());
            }
        }
    }
    plan.touchedEdges = sortedUnique(std::move(plan.touchedEdges));
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    plan.selection.normalise();
    plan.changed = static_cast<u32>(border.size());
    // Below 0 the strip lies back over the faces it grew from (EXB-2): it
    // stops at 0, as Q4 has Inset and Chamfer do. A hinge drops the bound.
    plan.amountMin = 0.0f;
    AmountRange(mesh, plan);
    return plan;
}

ModelPlan PlanExtrudeBorder(Mesh& mesh, const PointTable& points, const ElementSet& edges) {
    return planBorderStrip(mesh, points, edges, {});
}

ModelPlan PlanHingeBorder(Mesh& mesh, const PointTable& points, const ElementSet& edges, u32 hingeVertex,
                          const Vector3f& axis) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    // The hinge must be an end of one of the border edges given, checked
    // before the strip is grown: a refusal leaves the mesh as it came.
    const Topology& topology = std::as_const(mesh).topology();
    bool onRun = false;
    for (const u32 e : sortedUnique(edges.edges)) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e)) || !topology.isBoundary(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        onRun = onRun || topology.from(h).value() == hingeVertex || topology.to(h).value() == hingeVertex;
    }
    if (!onRun) {
        plan.refusal = ModelRefusal::HingeNotOnBoundary;
        return plan;
    }
    HingeAxis hinge;
    {
        const std::span<const Vector3f> was = positionsOf(mesh);
        hinge.origin = hingeVertex < was.size() ? was[hingeVertex] : Vector3f{0.0f, 0.0f, 0.0f};
    }
    hinge.axis = unitOr(axis, Vector3f{0.0f, 0.0f, 1.0f});
    // Every border vertex on the hinge's line stays, as the hinge vertex does:
    // a copy of it would turn on the spot, a wall of no area (MTX-12).
    std::vector<u32> pinned{hingeVertex};
    {
        const std::span<const Vector3f> was = positionsOf(mesh);
        const f32 tolerance = CoincidenceTolerance(mesh);
        for (const u32 e : sortedUnique(edges.edges)) {
            if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e)) || !topology.isBoundary(EdgeId(e))) {
                continue;
            }
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            for (const u32 v : {topology.from(h).value(), topology.to(h).value()}) {
                if (v < was.size() && cross(was[v] - hinge.origin, hinge.axis).length() <= tolerance) {
                    pinned.push_back(v);
                }
            }
        }
        pinned = sortedUnique(pinned);
    }
    bool turns = false;
    for (const u32 e : sortedUnique(edges.edges)) {
        if (e < topology.edgeCount() && !topology.isDeleted(EdgeId(e)) && topology.isBoundary(EdgeId(e))) {
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            turns = turns || !std::binary_search(pinned.begin(), pinned.end(), topology.from(h).value()) ||
                    !std::binary_search(pinned.begin(), pinned.end(), topology.to(h).value());
        }
    }
    if (!turns) {
        plan.refusal = ModelRefusal::ZeroAmount; // every edge lies on the hinge: nothing turns
        return plan;
    }
    plan = planBorderStrip(mesh, points, edges, pinned);
    if (plan.refused()) {
        return plan;
    }
    // The strip's aims are the extrude's; a hinge turns instead, about the
    // pinned vertices along the axis the caller chose, and is unbounded.
    for (VertexMotion& motion : plan.motions) {
        motion.direction = Vector3f{0.0f, 0.0f, 0.0f};
    }
    plan.hinge = hinge;
    plan.amountMin = -FLT_MAX;
    plan.amountMax = FLT_MAX;
    return plan;
}

/// How far an inset corner may reach per unit of amount: past this the two
/// edges have folded back on each other and the miter is meaningless.
constexpr f32 kInsetReach = 50.0f;

ModelPlan PlanInset(Mesh& mesh, const PointTable& points, const ElementSet& faces, InsetType type) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u8> inSelection(topology.faceCount(), 0);
    std::vector<u32> chosen;
    for (const u32 f : sortedUnique(faces.faces)) {
        if (f < topology.faceCount() && !topology.isDeleted(FaceId(f)) && inSelection[f] == 0) {
            inSelection[f] = 1;
            chosen.push_back(f);
        }
    }
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    const std::span<const Vector3f> places = positionsOf(mesh);
    const Twins twins(mesh, points);
    const std::vector<std::vector<u32>> regions = regionsOf(mesh, twins, chosen, type == InsetType::ByPolygon);
    std::vector<u32> regionOf(topology.faceCount(), kInvalidId);
    for (u32 r = 0; r < regions.size(); ++r) {
        for (const u32 f : regions[r]) {
            regionOf[f] = r;
        }
    }
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    const std::vector<u32> base = cornerBases(mapping.faces);
    const std::vector<u8> nothingPinned;
    const std::vector<Grown> rings =
        growRings(mesh, twins, regions, regionOf, ordinalOf, base, mapping, plan, nothingPinned);
    // Each copy is one inner vertex: its fan's normal, the two boundary edges
    // that end the fan, the angle the fan turns through, and where its faces
    // lie, which is the side it moves to.
    struct Corner {
        Vector3f normal{0.0f, 0.0f, 0.0f}; ///< The fan's, area weighted.
        Vector3f first{0.0f, 0.0f, 0.0f};  ///< Its two boundary edges, as their
        Vector3f second{0.0f, 0.0f, 0.0f}; ///< halfedges run inside the region.
        u32 edges = 0;
        u32 from = kInvalidId;
        u32 region = kInvalidId;
    };
    std::unordered_map<u32, Corner> corners;
    const auto at = [&](u32 vertex) {
        return vertex < places.size() ? places[vertex] : Vector3f{0.0f, 0.0f, 0.0f};
    };
    for (u32 r = 0; r < regions.size(); ++r) {
        const Grown& grown = rings[r];
        for (const u32 f : regions[r]) {
            const Vector3f area = faceArea(mesh, FaceId(f));
            Vector3f centre{0.0f, 0.0f, 0.0f};
            u32 valence = 0;
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                centre = centre + at(topology.from(h).value());
                ++valence;
            }
            centre = centre * (1.0f / static_cast<f32>(std::max(valence, 1u)));
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                const u32 v = topology.from(h).value();
                const u32 copy = useAt(grown, v, f);
                if (copy == v) {
                    continue; // inside the region: it stays where it is
                }
                Corner& corner = corners[copy];
                corner.from = v;
                corner.region = r;
                corner.normal = corner.normal + area;
            }
        }
        for (const HalfedgeId h : grown.boundary) {
            const u32 f = topology.face(h).value();
            const u32 from = topology.from(h).value();
            const u32 to = topology.to(h).value();
            // The edge as its halfedge runs inside the region: both of its ends
            // record the same vector, so the miter below has the face's own
            // sense of which side the material is on.
            const Vector3f along = at(to) - at(from);
            for (const u32 vertex : {from, to}) {
                const auto found = corners.find(useAt(grown, vertex, f));
                if (found == corners.end()) {
                    continue;
                }
                if (found->second.edges++ == 0) {
                    found->second.first = along;
                } else {
                    found->second.second = along;
                }
            }
        }
    }
    // A crack's twins are one point (Q5): their copies are one corner of the
    // ring, each twin bringing the boundary edge on its own side, so both move
    // to the one miter (INS-1).
    {
        std::map<std::pair<u32, u32>, std::vector<u32>> byPoint; // (region, point) -> copies
        for (const auto& [copy, corner] : corners) {
            const u32 point = corner.from < points.pointOf.size() ? points.pointOf[corner.from] : corner.from;
            byPoint[{corner.region, point}].push_back(copy);
        }
        for (const auto& [key, copies] : byPoint) {
            if (copies.size() < 2) {
                continue;
            }
            Corner joined;
            for (const u32 copy : copies) {
                const Corner& one = corners[copy];
                joined.normal = joined.normal + one.normal;
                for (u32 k = 0; k < one.edges && k < 2; ++k) {
                    const Vector3f along = k == 0 ? one.first : one.second;
                    (joined.edges++ == 0 ? joined.first : joined.second) = along;
                }
            }
            for (const u32 copy : copies) {
                Corner& one = corners[copy];
                one.normal = joined.normal;
                one.first = joined.first;
                one.second = joined.second;
                one.edges = joined.edges;
            }
        }
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    mapping.mayHaveZeroArea = true; // the ring has none until the amount opens it
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    freshGroups(mesh, copiesOf(rings)); // the ring's inner vertices are their own points
    const std::span<const Vector3f> now = positionsOf(mesh);
    for (const auto& [copy, corner] : corners) {
        if (corner.edges < 2) {
            continue; // a fan the boundary did not close: nothing to offset
        }
        const Vector3f normal = unitOr(corner.normal, Vector3f{0.0f, 0.0f, 1.0f});
        // Each boundary edge's inward normal, in the fan's plane: the material
        // is to the left of a halfedge inside the region.
        const Vector3f first = unitOr(cross(normal, corner.first), Vector3f{0.0f, 0.0f, 0.0f});
        const Vector3f second = unitOr(cross(normal, corner.second), Vector3f{0.0f, 0.0f, 0.0f});
        // Blender's even offset is the miter of the two: (n1 + n2) / (1 + n1.n2)
        // lands exactly `amount` from both edges, which is 1 / sin(half the
        // corner) as fast, and it turns the right way at a reflex corner
        // without being told which way that is.
        const f32 fold = 1.0f + first.dot(second);
        Vector3f direction = fold > 1e-6f ? (first + second) * (1.0f / fold)
                                          : unitOr(first + second, cross(normal, corner.first)) * kInsetReach;
        if (direction.length() > kInsetReach) {
            // Two edges that fold right back on each other would send their
            // corner to infinity: a needle reaches far, not for ever.
            direction = unitOr(direction, Vector3f{0.0f, 0.0f, 0.0f}) * kInsetReach;
        }
        VertexMotion motion;
        motion.vertex = copy;
        motion.base = copy < now.size() ? now[copy] : Vector3f{0.0f, 0.0f, 0.0f};
        motion.direction = direction;
        plan.motions.push_back(motion);
    }
    std::sort(plan.motions.begin(), plan.motions.end(),
              [](const VertexMotion& a, const VertexMotion& b) { return a.vertex < b.vertex; });
    touchFaces(mesh, plan);
    // A region with no border (a whole closed mesh) has nothing to inset from:
    // it is counted as left, not as done (INS-5).
    for (u32 r = 0; r < regions.size(); ++r) {
        (rings[r].boundary.empty() ? plan.kept : plan.changed) += static_cast<u32>(regions[r].size());
    }
    plan.amountMin = 0.0f; // Q4: below 0 the ring folds over its neighbours; Max stops there too
    AmountRange(mesh, plan);
    // §2.4: each inner vertex's corners and skin are re-sampled from the face
    // its new place lands in, so the texture and the binding stay on the
    // surface. The place the plan samples at is half of what it allows.
    const f32 sample = plan.amountMax > 0.0f && plan.amountMax < FLT_MAX ? plan.amountMax * 0.5f : 1.0f;
    std::unordered_map<u32, SourcePolygon> captured;
    for (const VertexMotion& motion : plan.motions) {
        std::vector<Resample> made =
            resamplesAt(mesh, VertexId(motion.vertex), motion.base + motion.direction * sample, captured);
        std::move(made.begin(), made.end(), std::back_inserter(plan.resamples));
    }
    return plan;
}

// ============================================================================
// Chamfer (§3.9)
// ============================================================================

namespace {

/// The miter of two offset lines inside one face: where the line `amount` in
/// from the first meets the line `amount` in from the second, per unit of
/// amount. @p first and @p second are the faces' inward perpendiculars.
Vector3f miterOf(const Vector3f& first, const Vector3f& second, const Vector3f& fallback) {
    const f32 fold = 1.0f + first.dot(second);
    if (fold <= 1e-4f) {
        return fallback; // within a degree of collinear: the lines are parallel
    }
    return (first + second) * (1.0f / fold);
}

/// The fan of outgoing halfedges at @p vertex, in order, starting after the
/// hole at a border vertex. `face(out[i])` lies between `edge(out[i])` and
/// `edge(out[i + 1])`.
std::vector<HalfedgeId> fanOf(const Topology& topology, VertexId vertex, bool& border) {
    std::vector<HalfedgeId> out;
    border = false;
    HalfedgeId start;
    for (const HalfedgeId h : topology.voh(vertex)) {
        if (topology.isBoundary(h)) {
            border = true;
        }
        if (!start.valid()) {
            start = h;
        }
        if (topology.isBoundary(Topology::opposite(h))) {
            start = h; // the first edge after the hole
        }
    }
    if (!start.valid()) {
        return out;
    }
    HalfedgeId h = start;
    do {
        out.push_back(h);
        if (topology.isBoundary(h)) {
            break; // the fan ends on the hole
        }
        h = Topology::opposite(topology.prev(h));
    } while (h.valid() && h != start && out.size() <= topology.halfedgeCount());
    return out;
}

/// A strip end's interior points, per unit of amount, from @p from to @p to:
/// the superellipse whose middle's height squared is @p profile, inside the
/// parallelogram the two directions span with the old vertex at its corner,
/// spaced evenly along the curve.
std::vector<Vector3f> profileOf(const Vector3f& from, const Vector3f& to, u32 segments, f32 profile) {
    // x = cos^k, y = sin^k: k = 1 a quarter circle, 2 the straight line.
    const f32 power = -std::log2(std::clamp(profile, 0.05f, 0.95f));
    const auto point = [&](f32 theta) {
        const f32 x = std::pow(std::max(0.0f, std::cos(theta)), power);
        const f32 y = std::pow(std::max(0.0f, std::sin(theta)), power);
        return from * (1.0f - y) + to * (1.0f - x);
    };
    const u32 samples = 32 * segments;
    std::vector<Vector3f> curve(samples + 1);
    std::vector<f32> length(samples + 1, 0.0f);
    for (u32 s = 0; s <= samples; ++s) {
        curve[s] = point(1.5707963f * static_cast<f32>(s) / static_cast<f32>(samples));
        if (s > 0) {
            length[s] = length[s - 1] + (curve[s] - curve[s - 1]).length();
        }
    }
    std::vector<Vector3f> out;
    u32 s = 1;
    for (u32 k = 1; k < segments; ++k) {
        const f32 wanted = length[samples] * static_cast<f32>(k) / static_cast<f32>(segments);
        while (s < samples && length[s] < wanted) {
            ++s;
        }
        const f32 span = length[s] - length[s - 1];
        const f32 t = span > 0.0f ? std::clamp((wanted - length[s - 1]) / span, 0.0f, 1.0f) : 0.0f;
        out.push_back(curve[s - 1] + (curve[s] - curve[s - 1]) * t);
    }
    return out;
}

/// One point the chamfer places: where it starts, how fast it leaves, and the
/// vertex it became.
struct Placed {
    u32 from = kInvalidId; ///< The vertex it replaces.
    Vector3f direction{0.0f, 0.0f, 0.0f};
    u32 id = kInvalidId;
};

ModelPlan chamferAt(Mesh& mesh, const PointTable& points, const std::vector<u8>& chosen,
                    const std::vector<u8>& touched, u32 count, const ChamferParams& params);

/// Q5: every member of a cut vertex's point is cut too, so a crack keeps a twin
/// for every vertex the chamfer makes (CHF-4). A loose member has no edge to
/// put a point on and stays.
void touchPointMembers(const Mesh& mesh, const PointTable& points, std::vector<u8>& touched) {
    const Topology& topology = mesh.topology();
    for (u32 v = 0; v < touched.size(); ++v) {
        if (touched[v] == 0) {
            continue;
        }
        for (const u32 m : membersOfPoint(mesh, points, v)) {
            for (const HalfedgeId h : topology.voh(VertexId(m))) {
                touched[m] = touched[m] != 0 || topology.face(h).valid() ? 1 : 0;
            }
        }
    }
}

} // namespace

ModelPlan PlanChamferEdges(Mesh& mesh, const PointTable& points, const ElementSet& edges,
                           const ChamferParams& params) {
    ModelPlan plan;
    if (params.open) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u8> chosen(topology.edgeCount(), 0);
    u32 count = 0;
    u32 borders = 0;
    for (const u32 e : PointClosed(mesh, points, edges).edges) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e)) || chosen[e] != 0) {
            continue;
        }
        if (topology.isBoundary(EdgeId(e))) {
            // The rules below want a face on each side (§3.9); `open` is what
            // would chamfer a border edge, and it is not built. The rest go
            // ahead and it is counted as left (Q2, CHF-6).
            ++borders;
            continue;
        }
        chosen[e] = 1;
        ++count;
    }
    if (count == 0) {
        plan.refusal = borders != 0 ? ModelRefusal::BorderEdgeChamfer : ModelRefusal::EmptySelection;
        return plan;
    }
    std::vector<u8> touched(topology.vertexCount(), 0);
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (chosen[e] == 0) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        touched[topology.from(h).value()] = 1;
        touched[topology.to(h).value()] = 1;
    }
    touchPointMembers(mesh, points, touched);
    plan = chamferAt(mesh, points, chosen, touched, count, params);
    if (!plan.refused() && borders != 0) {
        plan.kept += borders;
        plan.keptBecause = ModelRefusal::BorderEdgeChamfer;
    }
    return plan;
}

ModelPlan PlanChamferVertices(Mesh& mesh, const PointTable& points, const ElementSet& vertices,
                              const ChamferParams& params) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    // A crack's twins each cut their own side, and the two cuts meet on it.
    std::vector<u8> touched(topology.vertexCount(), 0);
    u32 count = 0;
    for (const u32 v : sortedUnique(vertices.vertices)) {
        if (v >= topology.vertexCount() || topology.isDeleted(VertexId(v)) || touched[v] != 0) {
            continue;
        }
        bool inFace = false;
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            inFace = inFace || topology.face(h).valid();
        }
        if (!inFace) {
            continue; // a loose vertex has no edge to put a point on
        }
        touched[v] = 1;
        ++count;
    }
    if (count == 0) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    touchPointMembers(mesh, points, touched);
    return chamferAt(mesh, points, std::vector<u8>(topology.edgeCount(), 0), touched, count, params);
}

namespace {

/// Chamfer itself (§3.9): every vertex @p touched marks cut off, with the edges
/// @p chosen marks replaced by strips -- none at all for the Vertex level's.
ModelPlan chamferAt(Mesh& mesh, const PointTable& points, const std::vector<u8>& chosen,
                    const std::vector<u8>& touched, u32 count, const ChamferParams& params) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    const u32 segments = std::clamp(params.segments, 1u, 64u);
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> places = positionsOf(mesh);
    const auto at = [&](u32 v) { return v < places.size() ? places[v] : Vector3f{0.0f, 0.0f, 0.0f}; };
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    detail::RebuildMapping mapping;
    std::vector<Placed> placed;
    // Per (vertex, edge) and (vertex, face): the point each put there, and the
    // ring around each vertex in fan order.
    std::unordered_map<u64, u32> pointOfEdge; // vertex << 32 | edge
    std::unordered_map<u64, u32> pointInFace; // vertex << 32 | face (miter or face point)
    std::unordered_map<u32, std::vector<u32>> ringOfVertex;
    // Past one segment: the profile's interior points between two points of
    // one ring, keyed low << 32 | high and stored from low to high.
    std::unordered_map<u64, std::vector<u32>> chainOf;
    const auto chainBetween = [&](u32 from, u32 to) {
        std::vector<u32> out;
        const auto found = chainOf.find((static_cast<u64>(std::min(from, to)) << 32) | std::max(from, to));
        if (found != chainOf.end()) {
            out = found->second;
            if (from > to) {
                std::reverse(out.begin(), out.end());
            }
        }
        return out;
    };
    // A touched vertex disappears: every one of its edges now ends at a point
    // (§3.9), so it is left out of the rebuild rather than left behind with no
    // face of its own.
    std::vector<u32> newId(topology.vertexCount(), kInvalidId);
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (!topology.isDeleted(VertexId(v)) && touched[v] == 0) {
            newId[v] = static_cast<u32>(mapping.vertexSource.size());
            mapping.vertexSource.push_back(v);
        }
    }
    const u32 firstPlaced = static_cast<u32>(mapping.vertexSource.size());
    const auto place = [&](u32 vertex, const Vector3f& direction) {
        Placed made;
        made.from = vertex;
        made.direction = direction;
        made.id = static_cast<u32>(mapping.vertexSource.size());
        mapping.vertexSource.push_back(vertex);
        placed.push_back(made);
        return made.id;
    };
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (touched[v] == 0 || topology.isDeleted(VertexId(v))) {
            continue;
        }
        bool border = false;
        const std::vector<HalfedgeId> fan = fanOf(topology, VertexId(v), border);
        if (fan.empty()) {
            continue;
        }
        const std::size_t n = fan.size();
        // The angle at v inside face `fan[i]` between its two edges, and each
        // edge's unit direction away from v.
        std::vector<Vector3f> along(n);
        for (std::size_t i = 0; i < n; ++i) {
            along[i] = unitOr(at(topology.to(fan[i]).value()) - at(v), Vector3f{1.0f, 0.0f, 0.0f});
        }
        const auto edgeAt = [&](std::size_t i) { return Topology::edge(fan[i]).value(); };
        const auto faceAt = [&](std::size_t i) { return topology.face(fan[i]); };
        const std::vector<u32> mates = membersOfPoint(mesh, points, v);
        // An unselected edge's point: `amount / sin` of the sharpest offset any
        // face beside it asks for, and plain `amount` when none does.
        for (std::size_t i = 0; i < n; ++i) {
            const u32 e = edgeAt(i);
            if (chosen[e] != 0) {
                continue; // a selected edge puts no point of its own
            }
            f32 reach = 0.0f;
            const auto ask = [&](const Vector3f& selected) {
                const f32 cosine = std::clamp(along[i].dot(selected), -1.0f, 1.0f);
                const f32 sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
                if (sine < 0.0174524f) {
                    return; // outside [1, 179] degrees: the face gives a point instead
                }
                // The nearer offset line wins, so two of them never put their
                // points past each other.
                const f32 asked = 1.0f / sine;
                reach = reach == 0.0f ? asked : std::min(reach, asked);
            };
            for (const std::size_t f : {i, (i + n - 1) % n}) {
                // The two faces beside this edge: the one after it in the fan
                // and the one before it. A border vertex has only one of them.
                if (!faceAt(f).valid()) {
                    continue;
                }
                const std::size_t other = f == i ? (i + 1) % n : f;
                if (chosen[edgeAt(other)] != 0) {
                    ask(along[other]); // this face holds a selected edge at v
                }
            }
            // A crack: the faces beside the edge's twins, in the other members'
            // fans, ask too, so both copies of the point land together (CHF-4).
            const std::vector<u32> ends =
                mates.size() > 1 ? membersOfPoint(mesh, points, topology.to(fan[i]).value()) : std::vector<u32>{};
            for (const u32 mate : mates) {
                for (const u32 end : mate == v ? std::vector<u32>{} : ends) {
                    const HalfedgeId twin = topology.findHalfedge(VertexId(mate), VertexId(end));
                    if (!twin.valid()) {
                        continue;
                    }
                    for (const HalfedgeId inFace : {twin, Topology::opposite(twin)}) {
                        if (!topology.face(inFace).valid()) {
                            continue;
                        }
                        // The face's other edge at `mate`: arriving before the
                        // twin, or leaving after its opposite.
                        const bool leaves = inFace == twin;
                        const HalfedgeId side = leaves ? topology.prev(inFace) : topology.next(inFace);
                        if (chosen[Topology::edge(side).value()] != 0) {
                            const u32 far = leaves ? topology.from(side).value() : topology.to(side).value();
                            ask(unitOr(at(far) - at(mate), along[i]));
                        }
                    }
                }
            }
            if (reach == 0.0f) {
                reach = 1.0f; // no face asked: plain `amount` along the edge
            }
            pointOfEdge[(static_cast<u64>(v) << 32) | e] = place(v, along[i] * reach);
        }
        // Each face's own point: the miter where both its edges are selected,
        // and a face point where one is and the corner is too straight or
        // reflex for an offset along the other.
        for (std::size_t i = 0; i < n; ++i) {
            const FaceId face = faceAt(i);
            if (!face.valid() || topology.isDeleted(face)) {
                continue;
            }
            const std::size_t next = (i + 1) % n;
            const bool first = chosen[edgeAt(i)] != 0;
            const bool second = chosen[edgeAt(next)] != 0;
            if (!first && !second) {
                continue;
            }
            const Vector3f normal = unitOr(faceArea(mesh, face), Vector3f{0.0f, 0.0f, 1.0f});
            // Inward perpendicular to each edge, inside this face: the face is
            // to the left of the halfedge leaving v, and to the right of the
            // one arriving.
            const Vector3f fromFirst = unitOr(cross(normal, along[i]), Vector3f{0.0f, 0.0f, 0.0f});
            const Vector3f fromSecond = unitOr(cross(normal, along[next]) * -1.0f, Vector3f{0.0f, 0.0f, 0.0f});
            if (first && second) {
                pointInFace[(static_cast<u64>(v) << 32) | face.value()] =
                    place(v, miterOf(fromFirst, fromSecond, fromFirst));
                continue;
            }
            const f32 cosine = std::clamp(along[i].dot(along[next]), -1.0f, 1.0f);
            const f32 sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
            const bool straight = sine < 0.0174524f;
            const bool reflex = cross(along[i], along[next]).dot(normal) < 0.0f;
            if (straight || reflex) {
                pointInFace[(static_cast<u64>(v) << 32) | face.value()] =
                    place(v, first ? fromFirst : fromSecond);
            }
        }
        // Each selected edge's strip end, past one segment: a chain from what
        // the face before it put to what the face after it did.
        for (std::size_t i = 0; segments > 1 && i < n; ++i) {
            if (chosen[edgeAt(i)] == 0) {
                continue;
            }
            const auto beside = [&](std::size_t face, std::size_t otherEdge) {
                const u64 key = static_cast<u64>(v) << 32;
                if (const FaceId f = faceAt(face); f.valid()) {
                    if (const auto inner = pointInFace.find(key | f.value()); inner != pointInFace.end()) {
                        return inner->second;
                    }
                }
                const auto edgePoint = pointOfEdge.find(key | edgeAt(otherEdge));
                return edgePoint == pointOfEdge.end() ? kInvalidId : edgePoint->second;
            };
            const u32 before = beside((i + n - 1) % n, (i + n - 1) % n);
            const u32 after = beside(i, (i + 1) % n);
            if (before == kInvalidId || after == kInvalidId || before == after) {
                continue; // the strip below refuses
            }
            const u32 low = std::min(before, after);
            const u32 high = std::max(before, after);
            const u64 pair = (static_cast<u64>(low) << 32) | high;
            if (chainOf.count(pair) != 0) {
                continue; // a strip meeting this one put it
            }
            const std::vector<Vector3f> profile = profileOf(placed[low - firstPlaced].direction,
                                                            placed[high - firstPlaced].direction, segments,
                                                            params.profile);
            std::vector<u32> chain;
            for (const Vector3f& direction : profile) {
                chain.push_back(place(v, direction));
            }
            chainOf[pair] = std::move(chain);
        }
        // The ring: the points round v in fan order, each edge's then the face
        // after it.
        std::vector<u32> ring;
        for (std::size_t i = 0; i < n; ++i) {
            const auto edgePoint = pointOfEdge.find((static_cast<u64>(v) << 32) | edgeAt(i));
            if (edgePoint != pointOfEdge.end()) {
                ring.push_back(edgePoint->second);
            }
            if (const FaceId face = faceAt(i); face.valid() && !topology.isDeleted(face)) {
                const auto inner = pointInFace.find((static_cast<u64>(v) << 32) | face.value());
                if (inner != pointInFace.end()) {
                    ring.push_back(inner->second);
                }
            }
        }
        ringOfVertex[v] = std::move(ring);
    }
    // Two cut vertices on one edge each put a point on it, and the two run at
    // each other: they meet before either reaches the far end, which no face's
    // area says while the face between them is still convex. Measured here, on
    // the edges as they were.
    f32 meets = FLT_MAX;
    for (const auto& [key, id] : pointOfEdge) {
        const u32 v = static_cast<u32>(key >> 32);
        const u32 e = static_cast<u32>(key & 0xffffffffu);
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const u32 w = topology.from(h).value() == v ? topology.to(h).value() : topology.from(h).value();
        const auto other = pointOfEdge.find((static_cast<u64>(w) << 32) | e);
        if (w < v || other == pointOfEdge.end()) {
            continue; // each pair once, and only a pair
        }
        const Vector3f along = at(w) - at(v);
        const f32 closing =
            (placed[other->second - firstPlaced].direction - placed[id - firstPlaced].direction).dot(along);
        if (closing < 0.0f) {
            meets = std::min(meets, -along.dot(along) / closing);
        }
    }
    // Every face's touched corners, replaced in loop order.
    std::vector<u32> cornerVertex;
    std::vector<u32> cornerSource;
    std::vector<u32> faceValence;
    std::vector<u32> faceSource;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (topology.isDeleted(FaceId(f))) {
            continue;
        }
        u32 valence = 0;
        bool reshaped = false;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const u32 v = topology.from(h).value();
            const auto ordinal = ordinalOf.find(static_cast<u32>(h.index()));
            const u32 source = ordinal == ordinalOf.end() ? kInvalidId : ordinal->second;
            if (touched[v] == 0) {
                cornerVertex.push_back(newId[v]);
                cornerSource.push_back(source);
                ++valence;
                continue;
            }
            reshaped = true;
            // In this face's loop the corner at v sits between the edge it
            // arrives on and the one it leaves on: the arriving edge's point,
            // then the face's own, then the leaving edge's.
            const u32 leaving = Topology::edge(h).value();
            const u32 arriving = Topology::edge(topology.prev(h)).value();
            const u64 key = static_cast<u64>(v) << 32;
            const auto inner = pointInFace.find(key | f);
            const auto fromArriving = pointOfEdge.find(key | arriving);
            const auto fromLeaving = pointOfEdge.find(key | leaving);
            const bool both = chosen[leaving] != 0 && chosen[arriving] != 0;
            u32 held[3];
            u32 heldCount = 0;
            if (both) {
                if (inner != pointInFace.end()) {
                    held[heldCount++] = inner->second;
                }
            } else {
                if (fromArriving != pointOfEdge.end()) {
                    held[heldCount++] = fromArriving->second;
                }
                if (inner != pointInFace.end()) {
                    held[heldCount++] = inner->second;
                }
                if (fromLeaving != pointOfEdge.end()) {
                    held[heldCount++] = fromLeaving->second;
                }
            }
            const auto take = [&](u32 id) {
                cornerVertex.push_back(id);
                cornerSource.push_back(source);
                ++valence;
            };
            for (u32 j = 0; j < heldCount; ++j) {
                take(held[j]);
                // Two of a strip end's points in one face: the end's chain is
                // this face's border between them.
                if (j + 1 < heldCount) {
                    for (const u32 id : chainBetween(held[j], held[j + 1])) {
                        take(id);
                    }
                }
            }
        }
        if (valence < 3) {
            plan.refusal = ModelRefusal::WouldFold; // nothing left of the face
            return plan;
        }
        faceValence.push_back(valence);
        faceSource.push_back(f);
        if (reshaped) {
            // Only what the tool reshaped is re-cut and re-shaded: §3.12's
            // crease rule must not run over a mesh it never touched.
            plan.changedFaces.push_back(static_cast<u32>(faceValence.size() - 1));
        }
    }
    // One strip per selected edge: the points its two side faces put at each
    // end, wound against both of them.
    const std::span<const u8> seams = mesh.attributes.get<const u8>(names::kSeam, Domain::Edge);
    std::vector<u32> strips;
    struct SeamStrip {
        u32 strip;              ///< The strip's slot.
        u32 source;             ///< The face all four of its corners read.
        std::vector<u32> moved; ///< Its corners off the seam's near side.
    };
    std::vector<SeamStrip> seamStrips;
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (chosen[e] == 0) {
            continue;
        }
        const HalfedgeId left = Topology::halfedge(EdgeId(e), 0);
        const HalfedgeId right = Topology::opposite(left);
        const u32 v = topology.from(left).value();
        const u32 w = topology.to(left).value();
        const u32 leftFace = topology.face(left).value();
        const u32 rightFace = topology.face(right).value();
        const auto put = [&](u32 vertex, u32 face, HalfedgeId here) {
            // What `face` put beside the chamfered edge at `vertex`: its own
            // point when it has one — a face point sits between the selected
            // edge and the other one, and a miter stands for both — else the
            // point on its other edge there.
            const u64 key = static_cast<u64>(vertex) << 32;
            if (const auto inner = pointInFace.find(key | face); inner != pointInFace.end()) {
                return inner->second;
            }
            const u32 other = topology.from(here).value() == vertex
                                  ? Topology::edge(topology.prev(here)).value()
                                  : Topology::edge(topology.next(here)).value();
            const auto edgePoint = pointOfEdge.find(key | other);
            return edgePoint == pointOfEdge.end() ? kInvalidId : edgePoint->second;
        };
        const u32 loop[4] = {put(w, leftFace, left), put(v, leftFace, left), put(v, rightFace, right),
                             put(w, rightFace, right)};
        for (u32 i = 0; i < 4; ++i) {
            if (loop[i] == kInvalidId) {
                plan.refusal = ModelRefusal::WouldFold; // a side put no point there
                return plan;
            }
        }
        // Each end from the left side to the right, through its chain past one
        // segment; the strip is cut across into one quad per segment.
        std::vector<u32> atW{loop[0]};
        std::vector<u32> atV{loop[1]};
        for (const u32 id : chainBetween(loop[0], loop[3])) {
            atW.push_back(id);
        }
        for (const u32 id : chainBetween(loop[1], loop[2])) {
            atV.push_back(id);
        }
        atW.push_back(loop[3]);
        atV.push_back(loop[2]);
        if (atW.size() != segments + 1 || atV.size() != segments + 1) {
            plan.refusal = ModelRefusal::WouldFold; // an end put no chain
            return plan;
        }
        // A seam's strip samples one face on both sides, so the seam ends up on
        // its far long edge and stays crisp (§3.9). Its other corners are not in
        // that face's fan, so they are re-sampled from it by hand below.
        // That face is the lower-numbered side, for the corners copied here and
        // the ones re-sampled below alike (CHF-2).
        const bool seam = e < seams.size() && seams[e] != 0;
        const auto sourceAt = [&](u32 j, bool atEndW) {
            const bool leftSide = seam ? leftFace <= rightFace : j * 2 <= segments;
            // `left` runs v to w and `right` w to v; a corner sits at its
            // halfedge's `from`.
            const HalfedgeId h = leftSide ? (atEndW ? topology.next(left) : left)
                                          : (atEndW ? right : topology.next(right));
            const auto ordinal = ordinalOf.find(static_cast<u32>(h.index()));
            return ordinal == ordinalOf.end() ? kInvalidId : ordinal->second;
        };
        for (u32 k = 0; k < segments; ++k) {
            if (seam) {
                // Every corner but the two the reading side put itself.
                const u32 own = leftFace <= rightFace ? 0u : segments;
                SeamStrip strip{static_cast<u32>(faceValence.size()), std::min(leftFace, rightFace), {}};
                for (const u32 j : {k, k + 1}) {
                    if (j != own) {
                        strip.moved.push_back(atV[j]);
                        strip.moved.push_back(atW[j]);
                    }
                }
                seamStrips.push_back(std::move(strip));
            }
            faceValence.push_back(4);
            const u32 quad[4] = {atW[k], atV[k], atV[k + 1], atW[k + 1]};
            const u32 sources[4] = {sourceAt(k, true), sourceAt(k, false), sourceAt(k + 1, false),
                                    sourceAt(k + 1, true)};
            for (u32 i = 0; i < 4; ++i) {
                cornerVertex.push_back(quad[i]);
                cornerSource.push_back(sources[i]);
            }
            faceSource.push_back(std::min(leftFace, rightFace));
            strips.push_back(static_cast<u32>(faceValence.size() - 1));
            plan.changedFaces.push_back(strips.back());
        }
    }
    // One patch per ring of three or more, wound with the faces around it --
    // unless the vertices are to leave holes.
    std::vector<u32> ringed;
    for (const auto& entry : ringOfVertex) {
        ringed.push_back(entry.first);
    }
    std::sort(ringed.begin(), ringed.end());
    std::vector<u32> patches;
    std::vector<std::pair<u32, u32>> seamPatches; // (patch slot, the face it reads)
    for (const u32 vertex : params.open ? std::vector<u32>{} : ringed) {
        const std::vector<u32>& ring = ringOfVertex[vertex];
        if (ring.size() < 3) {
            continue;
        }
        bool border = false;
        const std::vector<HalfedgeId> fan = fanOf(topology, VertexId(vertex), border);
        Vector3f outward{0.0f, 0.0f, 0.0f};
        u32 lowest = kInvalidId;
        for (const HalfedgeId h : fan) {
            if (const FaceId face = topology.face(h); face.valid() && !topology.isDeleted(face)) {
                outward = outward + faceArea(mesh, face);
                lowest = std::min(lowest, face.value());
            }
        }
        // The ring with each strip end's chain between its two points.
        std::vector<u32> loop;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            loop.push_back(ring[i]);
            for (const u32 id : chainBetween(ring[i], ring[(i + 1) % ring.size()])) {
                loop.push_back(id);
            }
        }
        // The loop's own places, at the amount the plan is sampled at: at 0
        // they all sit on the vertex and say nothing about which way round it
        // goes.
        std::vector<Vector3f> shape;
        for (const u32 id : loop) {
            const Placed& point = placed[id - firstPlaced];
            shape.push_back(at(point.from) + point.direction);
        }
        {
            Vector3f normal{0.0f, 0.0f, 0.0f};
            for (std::size_t i = 0; i < shape.size(); ++i) {
                normal = normal + cross(shape[i], shape[(i + 1) % shape.size()]);
            }
            if (normal.dot(outward) < 0.0f) {
                std::reverse(loop.begin(), loop.end());
            }
        }
        // The patch samples the lowest-numbered face around the vertex,
        // extrapolated where it must (§3.9).
        u32 source = kInvalidId;
        if (lowest != kInvalidId) {
            for (const HalfedgeId h : topology.fh(FaceId(lowest))) {
                if (topology.from(h).value() == vertex) {
                    const auto ordinal = ordinalOf.find(static_cast<u32>(h.index()));
                    source = ordinal == ordinalOf.end() ? kInvalidId : ordinal->second;
                }
            }
        }
        // A seam through the vertex: the patch's corners are read from that one
        // face below, so it lies on its island whole (CHF-5).
        bool seamed = false;
        for (const HalfedgeId h : fan) {
            seamed = seamed || SeamBetween(mesh, h);
        }
        const auto patch = [&](const std::vector<u32>& corners) {
            faceValence.push_back(static_cast<u32>(corners.size()));
            for (const u32 id : corners) {
                cornerVertex.push_back(id);
                cornerSource.push_back(source);
            }
            faceSource.push_back(lowest == kInvalidId ? 0u : lowest);
            plan.changedFaces.push_back(static_cast<u32>(faceValence.size() - 1));
            patches.push_back(plan.changedFaces.back());
            if (seamed && lowest != kInvalidId) {
                seamPatches.emplace_back(patches.back(), lowest);
            }
        };
        if (loop.size() == ring.size()) {
            patch(loop);
            continue;
        }
        // Chains in it: a fan round the loop's mean, since a flat profile lines
        // its points up along each side and no single face cuts that cleanly.
        Vector3f middle{0.0f, 0.0f, 0.0f};
        for (const u32 id : loop) {
            middle = middle + placed[id - firstPlaced].direction;
        }
        const u32 centre = place(vertex, middle * (1.0f / static_cast<f32>(loop.size())));
        for (std::size_t i = 0; i < loop.size(); ++i) {
            patch({loop[i], loop[(i + 1) % loop.size()], centre});
        }
    }
    mapping.faces.faceValence = std::move(faceValence);
    mapping.faces.cornerVertex = std::move(cornerVertex);
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    mapping.cornerSource = std::move(cornerSource);
    mapping.faceSource = std::move(faceSource);
    mapping.mayHaveZeroArea = true; // the strips and patches open with the amount
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    std::vector<u32> made;
    for (const Placed& point : placed) {
        made.push_back(point.id);
    }
    freshGroups(mesh, sortedUnique(std::move(made))); // each placed point is its own
    const std::span<const Vector3f> now = positionsOf(mesh);
    for (const Placed& point : placed) {
        VertexMotion motion;
        motion.vertex = point.id;
        motion.base = point.id < now.size() ? now[point.id] : Vector3f{0.0f, 0.0f, 0.0f};
        motion.direction = point.direction;
        plan.motions.push_back(motion);
    }
    std::sort(plan.motions.begin(), plan.motions.end(),
              [](const VertexMotion& a, const VertexMotion& b) { return a.vertex < b.vertex; });
    // The strips; or at vertices, which have none, the points and patches made.
    plan.selection.faces = strips.empty() ? patches : strips;
    if (strips.empty()) {
        for (const Placed& point : placed) {
            plan.selection.vertices.push_back(point.id);
        }
        plan.selection.normalise();
    }
    const Topology& built = std::as_const(mesh).topology();
    touchFaces(mesh, plan);
    plan.changed = count;
    plan.amountMin = 0.0f; // Q4: a negative chamfer turns the strip inside out; Max stops at 0
    AmountRange(mesh, plan);
    plan.amountMax = std::min(plan.amountMax, meets * kShortOfCollapse);
    plan.unchangedAtZero = rollback.before(); // Q3: at 0 a chamfer changes nothing
    // §2.4 for every point that moves: its corners and its skin come from the
    // face its new place lands in.
    const f32 sample = plan.amountMax > 0.0f && plan.amountMax < FLT_MAX ? plan.amountMax * 0.5f : 1.0f;
    std::unordered_map<u32, SourcePolygon> captured;
    for (const VertexMotion& motion : plan.motions) {
        std::vector<Resample> resamples =
            resamplesAt(mesh, VertexId(motion.vertex), motion.base + motion.direction * sample, captured);
        std::move(resamples.begin(), resamples.end(), std::back_inserter(plan.resamples));
    }
    // And the seam strips' far corners, from the one face the whole strip
    // reads: after the others, so they are the ones that land.
    for (const SeamStrip& seam : seamStrips) {
        if (seam.strip >= built.faceCount() || built.isDeleted(FaceId(seam.strip)) ||
            seam.source >= built.faceCount()) {
            continue;
        }
        Resample resample;
        resample.source = CapturePolygon(mesh, FaceId(seam.source));
        for (const HalfedgeId h : built.fh(FaceId(seam.strip))) {
            const u32 vertex = built.from(h).value();
            if (std::find(seam.moved.begin(), seam.moved.end(), vertex) != seam.moved.end()) {
                resample.targetCorners.push_back(h);
            }
        }
        if (!resample.targetCorners.empty()) {
            plan.resamples.push_back(std::move(resample)); // its skin stays its point's
        }
    }
    for (const auto& [slot, read] : seamPatches) {
        if (slot >= built.faceCount() || built.isDeleted(FaceId(slot)) || read >= built.faceCount()) {
            continue;
        }
        Resample resample;
        resample.source = CapturePolygon(mesh, FaceId(read));
        for (const HalfedgeId h : built.fh(FaceId(slot))) {
            resample.targetCorners.push_back(h);
        }
        plan.resamples.push_back(std::move(resample));
    }
    return plan;
}

} // namespace

// ============================================================================
// Adding topology by hand (§3.14)
// ============================================================================

namespace {

/// Every edge of @p mesh that runs between two of @p vertices, as a set of
/// point pairs, so "already joined" is asked once.
bool joined(const Topology& topology, u32 a, u32 b) {
    return topology.findHalfedge(VertexId(a), VertexId(b)).valid() ||
           topology.findHalfedge(VertexId(b), VertexId(a)).valid();
}

/// The halfedge of @p face that leaves @p vertex, or an invalid id.
HalfedgeId leaving(const Topology& topology, FaceId face, u32 vertex) {
    for (const HalfedgeId h : topology.fh(face)) {
        if (topology.from(h).value() == vertex) {
            return h;
        }
    }
    return HalfedgeId();
}

/// Whether @p at lies inside the polygon @p places (even-odd), on the plane
/// its Newell normal faces most.
bool insideLoop(std::span<const Vector3f> places, const Vector3f& at) {
    const std::array<f64, 3> n = newellOf(places);
    const u32 drop = std::abs(n[0]) >= std::abs(n[1]) && std::abs(n[0]) >= std::abs(n[2]) ? 0u
                     : std::abs(n[1]) >= std::abs(n[2])                                  ? 1u
                                                                                         : 2u;
    const auto flat = [drop](const Vector3f& p) {
        const std::array<f64, 3> q = toArray(p);
        return std::array<f64, 2>{q[(drop + 1) % 3], q[(drop + 2) % 3]};
    };
    const std::array<f64, 2> q = flat(at);
    bool inside = false;
    for (std::size_t i = 0, j = places.size() - 1; i < places.size(); j = i++) {
        const std::array<f64, 2> a = flat(places[i]);
        const std::array<f64, 2> b = flat(places[j]);
        if ((a[1] > q[1]) != (b[1] > q[1]) && q[0] < a[0] + (q[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1])) {
            inside = !inside;
        }
    }
    return inside;
}

/// Why cutting @p face between its corners at vertices @p a and @p b would be
/// wrong, or `None`: each half must keep the face's winding with area, and
/// cross no side of its own. A diagonal across a concave face's notch fails
/// (CON-3, EXT-9), and so does one that only runs along corners in line (MTX-9).
ModelRefusal whyNotCut(const Mesh& mesh, FaceId face, u32 a, u32 b) {
    const std::vector<u32> loop = loopOf(mesh.topology(), face);
    const std::vector<Vector3f> places = placesOf(positionsOf(mesh), loop);
    const auto ia = std::find(loop.begin(), loop.end(), a);
    const auto ib = std::find(loop.begin(), loop.end(), b);
    if (ia == loop.end() || ib == loop.end() || ia == ib) {
        return ModelRefusal::None;
    }
    const std::array<f64, 3> n = newellOf(places);
    f64 longest = 0.0;
    for (std::size_t i = 0; i < places.size(); ++i) {
        longest = std::max(longest, length64(sub(places[(i + 1) % places.size()], places[i])));
    }
    const std::size_t count = loop.size();
    for (const auto& [first, last] : {std::pair{ia, ib}, std::pair{ib, ia}}) {
        std::vector<Vector3f> half;
        for (std::size_t k = static_cast<std::size_t>(first - loop.begin());; k = (k + 1) % count) {
            half.push_back(places[k]);
            if (k == static_cast<std::size_t>(last - loop.begin())) {
                break;
            }
        }
        std::array<f64, 3> twice{0.0, 0.0, 0.0};
        for (std::size_t i = 0; i < half.size(); ++i) {
            const std::array<f64, 3> p = toArray(half[i]);
            const std::array<f64, 3> q = toArray(half[(i + 1) % half.size()]);
            twice[0] += p[1] * q[2] - p[2] * q[1];
            twice[1] += p[2] * q[0] - p[0] * q[2];
            twice[2] += p[0] * q[1] - p[1] * q[0];
        }
        const f64 area = 0.5 * (twice[0] * n[0] + twice[1] * n[1] + twice[2] * n[2]);
        const f64 least = 1e-6 * longest * longest;
        if (area < -least || foldsOver(half)) {
            return ModelRefusal::DiagonalOutside;
        }
        if (area <= least) {
            return ModelRefusal::WouldFold;
        }
    }
    return ModelRefusal::None;
}

} // namespace

ModelPlan PlanInsertVertex(Mesh& mesh, EdgeId edge, f32 t) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    if (edge.value() >= topology.edgeCount() || topology.isDeleted(edge)) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    {
        // A vertex on an end would be a zero-length edge and a copy in its
        // neighbour's point (IV-3).
        const std::span<const Vector3f> at = positionsOf(mesh);
        const HalfedgeId h = Topology::halfedge(edge, 0);
        const f64 length = length64(sub(at[topology.to(h).value()], at[topology.from(h).value()]));
        const f64 tolerance = CoincidenceTolerance(mesh);
        if (t * length <= tolerance || (1.0f - t) * length <= tolerance) {
            plan.refusal = ModelRefusal::ZeroAmount;
            return plan;
        }
    }
    // A crack's twin edges are this edge too (Q5): each is split at the same
    // place, so the twins stay matched and no T opens across the crack (IV-4).
    std::vector<std::pair<u32, f32>> splits{{edge.value(), t}};
    {
        const PointTable points = skinning::BuildPointTable(mesh);
        const auto pointOf = [&](u32 v) { return v < points.pointOf.size() ? points.pointOf[v] : v; };
        const u32 first = pointOf(topology.from(Topology::halfedge(edge, 0)).value());
        for (const u32 twin : Twins(mesh, points).of(edge.value())) {
            const bool same = pointOf(topology.from(Topology::halfedge(EdgeId(twin), 0)).value()) == first;
            splits.emplace_back(twin, same ? t : 1.0f - t);
        }
    }
    for (const auto& [e, at] : splits) {
        const VertexId made = SplitEdge(mesh, EdgeId(e), at);
        if (!made.valid()) {
            continue;
        }
        ++plan.changed;
        plan.selection.vertices.push_back(made.value());
        // Its two halves are the old edge's, flags and all (Q7): it makes no
        // edge, and the faces round it are drawn and shaded again.
        plan.reshade.push_back(made.value());
        const Topology& built = std::as_const(mesh).topology();
        for (const HalfedgeId h : built.voh(made)) {
            if (const FaceId face = built.face(h); face.valid() && !built.isDeleted(face)) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
    if (plan.changed == 0) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    return plan;
}

ModelPlan PlanDivide(Mesh& mesh, const ElementSet& edges, u32 segments) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    if (segments < 2) {
        plan.refusal = ModelRefusal::ZeroAmount; // nothing to divide into
        return plan;
    }
    // The edges are taken by their ends: a split renumbers, so an edge's id
    // does not survive the one before it.
    std::vector<std::pair<u32, u32>> chosen;
    {
        const Topology& topology = std::as_const(mesh).topology();
        for (const u32 e : sortedUnique(edges.edges)) {
            if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e))) {
                continue;
            }
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            chosen.emplace_back(topology.from(h).value(), topology.to(h).value());
        }
    }
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    for (const auto& [a, b] : chosen) {
        // Each cut leaves the piece towards `b`, so the next one is taken at
        // the parameter that divides what is left.
        u32 from = a;
        for (u32 i = 1; i < segments; ++i) {
            const Topology& topology = std::as_const(mesh).topology();
            const HalfedgeId h = topology.findHalfedge(VertexId(from), VertexId(b));
            if (!h.valid()) {
                break;
            }
            const f32 t = 1.0f / static_cast<f32>(segments - i + 1);
            const VertexId made = SplitEdge(mesh, Topology::edge(h), t);
            if (!made.valid()) {
                break;
            }
            plan.selection.vertices.push_back(made.value());
            ++plan.changed;
            from = made.value();
        }
    }
    if (plan.changed == 0) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    // The pieces are the old edges', flags and all (Q7): only shaded again.
    plan.reshade.insert(plan.reshade.end(), plan.selection.vertices.begin(), plan.selection.vertices.end());
    const Topology& built = std::as_const(mesh).topology();
    for (const u32 v : plan.selection.vertices) {
        for (const HalfedgeId h : built.voh(VertexId(v))) {
            if (const FaceId face = built.face(h); face.valid() && !built.isDeleted(face)) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    return plan;
}

ModelPlan PlanConnectVertices(Mesh& mesh, const PointTable& points, const ElementSet& vertices) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const std::vector<u32> chosen = verticesOf(mesh, points, vertices);
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    const Rollback rollback(mesh, plan);
    std::vector<u8> picked(std::as_const(mesh).topology().vertexCount(), 0);
    for (const u32 v : chosen) {
        if (v < picked.size()) {
            picked[v] = 1;
        }
    }
    // Every face holding two or more, by its own vertices: a cut renumbers the
    // faces, so each is found again by the pair it is cut between.
    struct Cut {
        u32 a = kInvalidId;
        u32 b = kInvalidId;
    };
    std::vector<Cut> cuts;
    bool anyFace = false;
    {
        const Topology& topology = std::as_const(mesh).topology();
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            std::vector<u32> loop;
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                const u32 v = topology.from(h).value();
                if (v < picked.size() && picked[v] != 0) {
                    loop.push_back(v);
                }
            }
            if (loop.size() < 2) {
                continue;
            }
            anyFace = true;
            // Consecutive selected corners in loop order, each pair joined
            // once — 3ds Max's rule, which turns a hexagon's 0, 2 and 4 into an
            // inner triangle. A pair of two is one cut, not a cut and its twin.
            const std::size_t pairs = loop.size() == 2 ? 1 : loop.size();
            for (std::size_t i = 0; i < pairs; ++i) {
                const u32 a = loop[i];
                const u32 b = loop[(i + 1) % loop.size()];
                if (!joined(topology, a, b)) {
                    cuts.push_back({a, b});
                }
            }
        }
    }
    if (!anyFace) {
        plan.refusal = ModelRefusal::NotOneFace;
        return plan;
    }
    if (cuts.empty()) {
        plan.refusal = ModelRefusal::DiagonalExists;
        return plan;
    }
    for (const Cut& cut : cuts) {
        mesh.ensureConnectivity();
        const Topology& topology = std::as_const(mesh).topology();
        // The face the pair still shares, if the cuts before this one left one.
        FaceId face;
        for (const HalfedgeId h : topology.voh(VertexId(cut.a))) {
            const FaceId candidate = topology.face(h);
            if (!candidate.valid() || topology.isDeleted(candidate)) {
                continue;
            }
            for (const HalfedgeId other : topology.fh(candidate)) {
                if (topology.from(other).value() == cut.b) {
                    face = candidate;
                }
            }
            if (face.valid()) {
                break;
            }
        }
        if (!face.valid() || joined(topology, cut.a, cut.b)) {
            continue;
        }
        const HalfedgeId from = leaving(topology, face, cut.a);
        const HalfedgeId to = leaving(topology, face, cut.b);
        if (!from.valid() || !to.valid()) {
            continue;
        }
        if (const ModelRefusal why = whyNotCut(mesh, face, cut.a, cut.b); why != ModelRefusal::None) {
            plan.refusal = why;
            return plan;
        }
        if (!SplitFaceAt(mesh, from, to).valid()) {
            continue; // §2.7.8's refusal: a second edge between one pair
        }
        ++plan.changed;
        const Topology& built = std::as_const(mesh).topology();
        if (const HalfedgeId made = built.findHalfedge(VertexId(cut.a), VertexId(cut.b)); made.valid()) {
            plan.selection.edges.push_back(Topology::edge(made).value());
            plan.touchedEdges.push_back(Topology::edge(made).value());
        }
    }
    if (plan.changed == 0) {
        plan.refusal = ModelRefusal::DiagonalExists;
        return plan;
    }
    const Topology& built = std::as_const(mesh).topology();
    for (const u32 e : plan.touchedEdges) {
        for (u32 side = 0; side < 2; ++side) {
            if (const FaceId face = built.face(Topology::halfedge(EdgeId(e), side));
                face.valid() && !built.isDeleted(face)) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    plan.selection.normalise();
    return plan;
}

ModelPlan PlanConnectEdges(Mesh& mesh, const PointTable& points, const ElementSet& edges,
                           const ConnectParams& params) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    (void)points;
    if (params.pinch != 0.0f) {
        plan.refusal = ModelRefusal::NotBuiltYet; // the spread is a later phase
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const u32 segments = std::max(1u, params.segments);
    // The selected edges by their ends: every split renumbers.
    std::vector<std::pair<u32, u32>> chosen;
    {
        const Topology& topology = std::as_const(mesh).topology();
        for (const u32 e : sortedUnique(edges.edges)) {
            if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e))) {
                continue;
            }
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            chosen.emplace_back(topology.from(h).value(), topology.to(h).value());
        }
    }
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    // The faces the selected edges lie in, as they are: the slide reads each
    // new vertex's corners from the face they were cut out of (CON-1).
    std::unordered_map<u32, SourcePolygon> before;
    {
        const Topology& topology = std::as_const(mesh).topology();
        for (const auto& [a, b] : chosen) {
            const HalfedgeId h = topology.findHalfedge(VertexId(a), VertexId(b));
            for (u32 side = 0; h.valid() && side < 2; ++side) {
                const FaceId face = topology.face(Topology::halfedge(Topology::edge(h), side));
                if (face.valid() && !before.count(face.value())) {
                    before.emplace(face.value(), CapturePolygon(mesh, face));
                }
            }
        }
    }
    std::unordered_map<u32, u32> parentOf; // a cut's new face -> the face it came from
    // Every selected edge's own new vertices, in order from its first end, and
    // which faces each edge was in.
    struct Split {
        std::vector<u32> made;    ///< `segments` vertices, first end first.
        std::vector<u32> faces;   ///< The faces it lay in, as they were.
        Vector3f along{0.0f, 0.0f, 0.0f}; ///< Its direction, times its length.
        u32 first = kInvalidId;   ///< Its ends, which say which way it runs.
        u32 last = kInvalidId;
    };
    std::vector<Split> splits;
    for (const auto& [a, b] : chosen) {
        mesh.ensureConnectivity();
        Split split;
        {
            const Topology& topology = std::as_const(mesh).topology();
            const std::span<const Vector3f> at = positionsOf(mesh);
            const HalfedgeId h = topology.findHalfedge(VertexId(a), VertexId(b));
            if (!h.valid()) {
                continue;
            }
            for (u32 side = 0; side < 2; ++side) {
                if (const FaceId face = topology.face(Topology::halfedge(Topology::edge(h), side));
                    face.valid() && !topology.isDeleted(face)) {
                    split.faces.push_back(face.value());
                }
            }
            if (a < at.size() && b < at.size()) {
                split.along = at[b] - at[a];
            }
            split.first = a;
            split.last = b;
        }
        u32 from = a;
        for (u32 i = 1; i <= segments; ++i) {
            const Topology& topology = std::as_const(mesh).topology();
            const HalfedgeId h = topology.findHalfedge(VertexId(from), VertexId(b));
            if (!h.valid()) {
                break;
            }
            const f32 t = 1.0f / static_cast<f32>(segments + 2 - i);
            const VertexId made = SplitEdge(mesh, Topology::edge(h), t);
            if (!made.valid()) {
                break;
            }
            split.made.push_back(made.value());
            from = made.value();
        }
        if (!split.made.empty()) {
            splits.push_back(std::move(split));
        }
    }
    if (splits.empty()) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    // Each face that held exactly two selected edges is cut between their
    // vertices, first to last so that no two cuts cross. A face that held three
    // or more keeps its vertices and is counted.
    std::unordered_map<u32, std::vector<u32>> inFace; // face -> the splits in it
    for (u32 i = 0; i < splits.size(); ++i) {
        for (const u32 f : splits[i].faces) {
            inFace[f].push_back(i);
        }
    }
    // An edge's own direction is whichever halfedge came first, so each split
    // is turned to run the way its neighbours across a face do. Otherwise a
    // face's second loop joins across its first, and the slide moves the loop
    // one way on some edges and the other way on the rest.
    {
        mesh.ensureConnectivity();
        const Topology& topology = std::as_const(mesh).topology();
        const std::span<const Vector3f> at = positionsOf(mesh);
        // Next to each other round the face the two share: the side between them.
        const auto near = [&](u32 face, u32 a, u32 b) {
            if (a == b) {
                return true;
            }
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                const u32 from = topology.from(h).value();
                const u32 to = topology.to(h).value();
                if ((from == a && to == b) || (from == b && to == a)) {
                    return true;
                }
            }
            return false;
        };
        const auto sameWay = [&](u32 face, const Split& p, const Split& q) {
            if (near(face, p.first, q.first) || near(face, p.last, q.last)) {
                return true;
            }
            if (near(face, p.first, q.last) || near(face, p.last, q.first)) {
                return false;
            }
            return (at[p.first] - at[q.first]).length() + (at[p.last] - at[q.last]).length() <=
                   (at[p.first] - at[q.last]).length() + (at[p.last] - at[q.first]).length();
        };
        std::vector<u8> seen(splits.size(), 0);
        for (u32 root = 0; root < splits.size(); ++root) {
            if (seen[root]) {
                continue;
            }
            seen[root] = 1;
            std::vector<u32> open{root};
            while (!open.empty()) {
                const u32 i = open.back();
                open.pop_back();
                for (const u32 f : splits[i].faces) {
                    for (const u32 j : inFace[f]) {
                        if (seen[j]) {
                            continue;
                        }
                        seen[j] = 1;
                        Split& next = splits[j];
                        if (!sameWay(f, splits[i], next)) {
                            std::reverse(next.made.begin(), next.made.end());
                            std::swap(next.first, next.last);
                            next.along = next.along * -1.0f;
                        }
                        open.push_back(j);
                    }
                }
            }
        }
    }
    std::vector<u32> faces;
    for (const auto& entry : inFace) {
        faces.push_back(entry.first);
    }
    std::sort(faces.begin(), faces.end());
    for (const u32 f : faces) {
        const std::vector<u32>& which = inFace[f];
        if (which.size() < 2) {
            continue;
        }
        if (which.size() > 2) {
            ++plan.kept; // three or more: its vertices only (§3.14)
            continue;
        }
        const Split& first = splits[which[0]];
        const Split& second = splits[which[1]];
        for (u32 i = 0; i < segments; ++i) {
            const u32 a = first.made[std::min<std::size_t>(i, first.made.size() - 1)];
            const u32 b = second.made[std::min<std::size_t>(i, second.made.size() - 1)];
            mesh.ensureConnectivity();
            const Topology& topology = std::as_const(mesh).topology();
            if (joined(topology, a, b)) {
                continue;
            }
            // The face the two now share: the cuts before this one split it.
            FaceId face;
            for (const HalfedgeId h : topology.voh(VertexId(a))) {
                const FaceId candidate = topology.face(h);
                if (!candidate.valid() || topology.isDeleted(candidate)) {
                    continue;
                }
                for (const HalfedgeId other : topology.fh(candidate)) {
                    if (topology.from(other).value() == b) {
                        face = candidate;
                    }
                }
                if (face.valid()) {
                    break;
                }
            }
            if (!face.valid()) {
                continue;
            }
            const HalfedgeId from = leaving(topology, face, a);
            const HalfedgeId to = leaving(topology, face, b);
            if (!from.valid() || !to.valid()) {
                continue;
            }
            const FaceId cut = SplitFaceAt(mesh, from, to);
            if (!cut.valid()) {
                continue;
            }
            parentOf[cut.value()] = face.value();
            ++plan.changed;
            const Topology& built = std::as_const(mesh).topology();
            if (const HalfedgeId made = built.findHalfedge(VertexId(a), VertexId(b)); made.valid()) {
                plan.selection.edges.push_back(Topology::edge(made).value());
                plan.touchedEdges.push_back(Topology::edge(made).value());
            }
        }
    }
    if (plan.changed == 0 && plan.kept == 0) {
        // No face held two of the edges: nothing to join, and Max adds
        // nothing either (CON-5).
        plan.refusal = ModelRefusal::NotOneFace;
        return plan;
    }
    // Each new vertex's corners, one class per face they were cut out of,
    // re-sampled from it wherever the slide takes them: along the edge, inside
    // that face.
    {
        const Topology& topology = std::as_const(mesh).topology();
        const auto rootOf = [&](u32 f) {
            for (auto up = parentOf.find(f); up != parentOf.end(); up = parentOf.find(f)) {
                f = up->second;
            }
            return f;
        };
        for (const Split& split : splits) {
            for (const u32 v : split.made) {
                std::vector<std::pair<u32, std::vector<HalfedgeId>>> classes;
                for (const HalfedgeId h : topology.voh(VertexId(v))) {
                    if (!topology.face(h).valid()) {
                        continue;
                    }
                    const u32 root = rootOf(topology.face(h).value());
                    auto found = std::find_if(classes.begin(), classes.end(),
                                              [&](const auto& c) { return c.first == root; });
                    if (found == classes.end()) {
                        classes.push_back({root, {}});
                        found = classes.end() - 1;
                    }
                    found->second.push_back(h);
                }
                for (std::size_t i = 0; i < classes.size(); ++i) {
                    const auto source = before.find(classes[i].first);
                    if (source == before.end()) {
                        continue;
                    }
                    Resample resample;
                    resample.targetVertex = i == 0 ? v : kInvalidId;
                    resample.targetCorners = std::move(classes[i].second);
                    resample.source = source->second;
                    plan.resamples.push_back(std::move(resample));
                }
            }
        }
    }
    // Slide: each new vertex runs along its own edge, over the room between the
    // neighbours it was cut between (§3.14). 0 is where it already is.
    {
        const std::span<const Vector3f> at = positionsOf(mesh);
        const f32 room = 1.0f / static_cast<f32>(segments + 1);
        for (const Split& split : splits) {
            for (const u32 v : split.made) {
                VertexMotion motion;
                motion.vertex = v;
                motion.base = v < at.size() ? at[v] : Vector3f{0.0f, 0.0f, 0.0f};
                motion.direction = split.along * room;
                plan.motions.push_back(motion);
                plan.selection.vertices.push_back(v);
            }
        }
        std::sort(plan.motions.begin(), plan.motions.end(),
                  [](const VertexMotion& a, const VertexMotion& b) { return a.vertex < b.vertex; });
    }
    // The pieces of the selected edges are theirs, flags and all (Q7): the
    // crease rule reads only the new edges across the faces.
    const Topology& built = std::as_const(mesh).topology();
    for (const u32 v : plan.selection.vertices) {
        plan.reshade.push_back(v);
        for (const HalfedgeId h : built.voh(VertexId(v))) {
            if (const FaceId face = built.face(h); face.valid() && !built.isDeleted(face)) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
    plan.touchedEdges = sortedUnique(std::move(plan.touchedEdges));
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    plan.selection.normalise();
    plan.amountMin = -1.0f;
    plan.amountMax = 1.0f;
    AmountRange(mesh, plan); // a hundredth short of a neighbour, as the other tools stop (CON-2)
    return plan;
}

ModelPlan PlanCut(Mesh& mesh, const PointTable& points, const CutPoint& from, const CutPoint& to,
                  const Vector3f& plane) {
    ModelPlan plan;
    (void)points;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Rollback rollback(mesh, plan);
    const Vector3f normal = unitOr(plane, Vector3f{0.0f, 0.0f, 1.0f});
    // Each end as a vertex: a click on one is itself, a click along an edge is
    // `SplitEdge`'s, and a click inside a face is where the chain left it.
    const auto realise = [&](const CutPoint& point, f32 t) -> u32 {
        mesh.ensureConnectivity();
        const Topology& topology = std::as_const(mesh).topology();
        if (point.vertex != kInvalidId && point.vertex < topology.vertexCount() &&
            !topology.isDeleted(VertexId(point.vertex))) {
            return point.vertex;
        }
        if (point.edge != kInvalidId && point.edge < topology.edgeCount() &&
            !topology.isDeleted(EdgeId(point.edge))) {
            const VertexId made = SplitEdge(mesh, EdgeId(point.edge), std::clamp(t, 0.0f, 1.0f));
            return made.valid() ? made.value() : kInvalidId;
        }
        return kInvalidId;
    };
    // The far end first: splitting an edge renumbers the near end's, but a
    // vertex keeps its id, so the near end is taken against what is left. Both
    // on one edge: the split keeps the edge's id on the piece from its first
    // end, so a near click past the far one lies on the other piece (CUT-2).
    const bool oneEdge = from.vertex == kInvalidId && to.vertex == kInvalidId && from.edge == to.edge &&
                         from.edge < std::as_const(mesh).topology().edgeCount();
    const u32 farEnd = oneEdge ? std::as_const(mesh).topology().to(Topology::halfedge(EdgeId(from.edge), 0)).value()
                               : kInvalidId;
    const u32 target = realise(to, to.t);
    CutPoint near = from;
    f32 nearT = from.t;
    if (oneEdge && target != kInvalidId) {
        const f32 split = std::clamp(to.t, 0.0f, 1.0f);
        const f32 t = std::clamp(from.t, 0.0f, 1.0f);
        if (t == split) {
            plan.refusal = ModelRefusal::NothingToSlice;
            return plan;
        }
        if (t < split) {
            nearT = t / split;
        } else {
            const Topology& topology = std::as_const(mesh).topology();
            const HalfedgeId piece = topology.findHalfedge(VertexId(target), VertexId(farEnd));
            near.edge = piece.valid() ? Topology::edge(piece).value() : kInvalidId;
            const f32 along = (t - split) / (1.0f - split);
            nearT = piece.valid() && Topology::halfedge(Topology::edge(piece), 0) == piece ? along : 1.0f - along;
        }
    }
    const u32 start = realise(near, nearT);
    if (start == kInvalidId || target == kInvalidId || start == target) {
        plan.refusal = ModelRefusal::NothingToSlice;
        return plan;
    }
    // The walk: from the vertex it is at, the face that carries on towards the
    // target, and the edge of that face the plane crosses.
    u32 here = start;
    // The plane is the one the two clicks and the eye made, and it stands for
    // the whole walk: re-anchoring it at each step would let it drift off the
    // line the user drew and cross an edge it had already cut.
    const Vector3f anchor = positionsOf(mesh)[start];
    std::vector<u32> chain{start};
    // Each step across a face, checked first: a step outside a concave face,
    // or along corners in line, refuses the whole cut.
    std::vector<std::pair<u32, u32>> made;
    ModelRefusal refused = ModelRefusal::None;
    const auto cutAcross = [&](FaceId face, u32 a, u32 b) {
        const Topology& topology = std::as_const(mesh).topology();
        const HalfedgeId one = leaving(topology, face, a);
        const HalfedgeId two = leaving(topology, face, b);
        if (!one.valid() || !two.valid() || joined(topology, a, b)) {
            return;
        }
        if (const ModelRefusal why = whyNotCut(mesh, face, a, b); why != ModelRefusal::None) {
            refused = why;
            return;
        }
        if (SplitFaceAt(mesh, one, two).valid()) {
            made.emplace_back(a, b);
        }
    };
    const u32 guard = std::as_const(mesh).topology().faceCount() + 2;
    for (u32 step = 0; step < guard && here != target; ++step) {
        mesh.ensureConnectivity();
        const Topology& topology = std::as_const(mesh).topology();
        const std::span<const Vector3f> at = positionsOf(mesh);
        const Vector3f towards = at[target] - at[here];
        FaceId best;
        u32 crossed = kInvalidId;  // the edge of `best` the plane crosses
        u32 straight = kInvalidId; // a vertex already on the plane, which is used as it is
        f32 nearest = -FLT_MAX;
        const f32 tolerance = CoincidenceTolerance(mesh);
        for (const HalfedgeId h : topology.voh(VertexId(here))) {
            const FaceId face = topology.face(h);
            if (!face.valid() || topology.isDeleted(face)) {
                continue;
            }
            // Does this face hold the target? Then the cut ends inside it.
            bool holds = false;
            for (const HalfedgeId other : topology.fh(face)) {
                holds = holds || topology.from(other).value() == target;
            }
            if (holds) {
                best = face;
                crossed = kInvalidId;
                straight = kInvalidId; // a corner an earlier face had on the plane is not this face's (MTX-14)
                nearest = FLT_MAX;
                break;
            }
            // A corner of this face that the plane already runs through is the
            // crossing: stepping onto it cuts no sliver beside it (§3.14).
            bool onCorner = false;
            for (const HalfedgeId other : topology.fh(face)) {
                const u32 v = topology.from(other).value();
                if (v == here || std::abs((at[v] - anchor).dot(normal)) > tolerance) {
                    continue;
                }
                const f32 reach = (at[v] - at[here]).dot(towards);
                onCorner = onCorner || reach > 0.0f;
                if (reach > 0.0f && reach > nearest) {
                    nearest = reach;
                    best = face;
                    crossed = kInvalidId;
                    straight = v;
                }
            }
            if (onCorner) {
                continue; // this face has its crossing already
            }
            // Else the edge of this face, not at `here`, that the plane crosses
            // on the far side, furthest along the way to the target.
            for (const HalfedgeId other : topology.fh(face)) {
                const u32 a = topology.from(other).value();
                const u32 b = topology.to(other).value();
                if (a == here || b == here) {
                    continue;
                }
                const f32 first = (at[a] - anchor).dot(normal);
                const f32 second = (at[b] - anchor).dot(normal);
                if ((first > 0.0f && second > 0.0f) || (first < 0.0f && second < 0.0f)) {
                    continue; // both on one side: the plane misses this edge
                }
                if (std::abs(first) <= tolerance || std::abs(second) <= tolerance) {
                    continue; // it meets the plane at an end: that corner is the crossing
                }
                const f32 span = first - second;
                const f32 t = span == 0.0f ? 0.5f : first / span;
                const Vector3f where = at[a] + (at[b] - at[a]) * t;
                const f32 reach = (where - at[here]).dot(towards);
                if (reach > nearest) {
                    nearest = reach;
                    best = face;
                    crossed = Topology::edge(other).value();
                    straight = kInvalidId;
                }
            }
        }
        if (!best.valid()) {
            break;
        }
        if (crossed == kInvalidId && straight != kInvalidId) {
            // Onto that corner: a cut across the face, and the walk carries on
            // from there.
            cutAcross(best, here, straight);
            if (refused != ModelRefusal::None) {
                break;
            }
            chain.push_back(straight);
            here = straight;
            continue;
        }
        if (crossed == kInvalidId) {
            // The target is in this face: one cut across it, unless an edge
            // already runs between them — a segment along an existing edge is
            // that edge, and nothing is made (§3.14).
            cutAcross(best, here, target);
            if (refused != ModelRefusal::None) {
                break;
            }
            chain.push_back(target);
            here = target;
            break;
        }
        // Cross it: a vertex at the crossing, then the cut from `here` to it.
        const HalfedgeId edge = Topology::halfedge(EdgeId(crossed), 0);
        const u32 a = topology.from(edge).value();
        const u32 b = topology.to(edge).value();
        const f32 first = (at[a] - anchor).dot(normal);
        const f32 second = (at[b] - anchor).dot(normal);
        const f32 span = first - second;
        const VertexId crossing = SplitEdge(mesh, EdgeId(crossed), span == 0.0f ? 0.5f : first / span);
        if (!crossing.valid()) {
            break;
        }
        mesh.ensureConnectivity();
        {
            const Topology& built = std::as_const(mesh).topology();
            FaceId face;
            for (const HalfedgeId h : built.voh(VertexId(here))) {
                const FaceId candidate = built.face(h);
                if (!candidate.valid() || built.isDeleted(candidate)) {
                    continue;
                }
                for (const HalfedgeId other : built.fh(candidate)) {
                    if (built.from(other).value() == crossing.value()) {
                        face = candidate;
                    }
                }
                if (face.valid()) {
                    break;
                }
            }
            if (face.valid()) {
                cutAcross(face, here, crossing.value());
                if (refused != ModelRefusal::None) {
                    break;
                }
            }
        }
        chain.push_back(crossing.value());
        here = crossing.value();
    }
    if (refused != ModelRefusal::None) {
        plan.refusal = refused;
        return plan;
    }
    // A walk that stopped short -- at a hole, or a border -- did not reach the
    // second click, and a cut is the line between the two (CUT-3).
    if (chain.size() < 2 || here != target) {
        plan.refusal = ModelRefusal::NothingToSlice;
        return plan;
    }
    mesh.ensureConnectivity();
    const Topology& built = std::as_const(mesh).topology();
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        if (const HalfedgeId h = built.findHalfedge(VertexId(chain[i]), VertexId(chain[i + 1]));
            h.valid()) {
            plan.selection.edges.push_back(Topology::edge(h).value());
            ++plan.changed;
        }
    }
    if (plan.changed == 0) {
        plan.refusal = ModelRefusal::NothingToSlice;
        return plan;
    }
    // The crease rule reads only the edges the cut made (Q7): one it ran along
    // keeps its flags, and the vertices it went through are shaded again.
    for (const auto& [a, b] : made) {
        if (const HalfedgeId h = built.findHalfedge(VertexId(a), VertexId(b)); h.valid()) {
            plan.touchedEdges.push_back(Topology::edge(h).value());
        }
    }
    plan.touchedEdges = sortedUnique(std::move(plan.touchedEdges));
    plan.reshade.insert(plan.reshade.end(), chain.begin(), chain.end());
    for (const u32 e : plan.selection.edges) {
        for (u32 side = 0; side < 2; ++side) {
            if (const FaceId face = built.face(Topology::halfedge(EdgeId(e), side));
                face.valid() && !built.isDeleted(face)) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    // Where the cut ended, which is where a chain's next segment starts.
    plan.selection.vertices.push_back(chain.back());
    plan.selection.normalise();
    return plan;
}

ModelPlan PlanSlice(Mesh& mesh, const PointTable& points, const ElementSet& faces,
                    const SliceParams& params) {
    ModelPlan plan;
    (void)points;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Rollback rollback(mesh, plan);
    const Vector3f normal = unitOr(params.normal, Vector3f{0.0f, 0.0f, 1.0f});
    const f32 tolerance = CoincidenceTolerance(mesh);
    const auto distanceOf = [&](const Vector3f& at) { return (at - params.origin).dot(normal); };
    // Which faces the slice may touch: the selection, or all of them. A split
    // appends its new half past the end, and that half is a wanted face's.
    std::vector<u8> wanted;
    {
        const Topology& topology = std::as_const(mesh).topology();
        wanted.assign(topology.faceCount(), faces.faces.empty() ? 1 : 0);
        for (const u32 f : faces.faces) {
            if (f < wanted.size()) {
                wanted[f] = 1;
            }
        }
    }
    const auto isWanted = [&](FaceId f) { return f.valid() && (f.index() >= wanted.size() || wanted[f.index()] != 0); };
    // A vertex of a wanted face within the tolerance is put on the plane and
    // used as a crossing: that is what keeps a sliver from being cut off beside
    // it. One no wanted face uses stays where it is (SLC-4).
    {
        const std::span<Vector3f> at = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        const Topology& topology = std::as_const(mesh).topology();
        for (u32 v = 0; v < at.size() && v < topology.vertexCount(); ++v) {
            if (topology.isDeleted(VertexId(v)) || !topology.outgoing(VertexId(v)).valid()) {
                continue;
            }
            const f32 distance = distanceOf(at[v]);
            if (distance == 0.0f || std::abs(distance) > tolerance) {
                continue;
            }
            bool used = false;
            for (const HalfedgeId h : topology.voh(VertexId(v))) {
                used = used || isWanted(topology.face(h));
            }
            if (used) {
                at[v] = at[v] - normal * distance;
            }
        }
    }
    // Which side of the plane each vertex is on, settled here and not asked
    // again: a snapped vertex is *on* it, whatever the float says afterwards.
    std::vector<i8> side;
    {
        const Topology& topology = std::as_const(mesh).topology();
        const std::span<const Vector3f> at = positionsOf(mesh);
        side.assign(topology.vertexCount(), 0);
        for (u32 v = 0; v < side.size() && v < at.size(); ++v) {
            const f32 distance = distanceOf(at[v]);
            side[v] = std::abs(distance) <= tolerance ? 0 : (distance > 0.0f ? 1 : -1);
        }
    }
    const auto sideOf = [&](u32 vertex) { return vertex < side.size() ? side[vertex] : 0; };
    // Every edge the plane crosses strictly, by its ends: each split renumbers.
    std::vector<std::pair<u32, u32>> crossing;
    {
        const Topology& topology = std::as_const(mesh).topology();
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            if (topology.isDeleted(EdgeId(e))) {
                continue;
            }
            bool touched = false;
            for (u32 half = 0; half < 2 && !touched; ++half) {
                const FaceId face = topology.face(Topology::halfedge(EdgeId(e), half));
                touched = face.valid() && face.value() < wanted.size() && wanted[face.value()] != 0;
            }
            if (!touched) {
                continue;
            }
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            const u32 a = topology.from(h).value();
            const u32 b = topology.to(h).value();
            if (sideOf(a) * sideOf(b) < 0) {
                crossing.emplace_back(a, b);
            }
        }
    }
    std::vector<u32> onPlane; // the vertices the cut runs through
    for (const auto& [a, b] : crossing) {
        mesh.ensureConnectivity();
        const Topology& topology = std::as_const(mesh).topology();
        const std::span<const Vector3f> at = positionsOf(mesh);
        const HalfedgeId h = topology.findHalfedge(VertexId(a), VertexId(b));
        if (!h.valid()) {
            continue;
        }
        const f32 first = distanceOf(at[a]);
        const f32 second = distanceOf(at[b]);
        if (first == second) {
            continue;
        }
        const VertexId made = SplitEdge(mesh, Topology::edge(h), first / (first - second));
        if (!made.valid()) {
            continue;
        }
        // The lerp lands it on the plane to the float's own precision; the
        // distance it has left is taken off, so the cut is exactly flat.
        const std::span<Vector3f> places = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        places[made.value()] = places[made.value()] - normal * distanceOf(places[made.value()]);
        onPlane.push_back(made.value());
    }
    {
        // The vertices the plane already ran through are on the cut too.
        const Topology& topology = std::as_const(mesh).topology();
        for (u32 v = 0; v < topology.vertexCount(); ++v) {
            if (!topology.isDeleted(VertexId(v)) && sideOf(v) == 0) {
                onPlane.push_back(v);
            }
        }
    }
    onPlane = sortedUnique(std::move(onPlane));
    // Each face the cut crosses, split between its crossings in order along the
    // plane's line through it.
    std::vector<std::pair<u32, u32>> cuts;
    {
        const Topology& topology = std::as_const(mesh).topology();
        const std::span<const Vector3f> at = positionsOf(mesh);
        std::vector<u8> here(topology.vertexCount(), 0);
        for (const u32 v : onPlane) {
            if (v < here.size()) {
                here[v] = 1;
            }
        }
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            if (topology.isDeleted(FaceId(f)) || (f < wanted.size() && wanted[f] == 0)) {
                continue;
            }
            std::vector<u32> on;
            bool above = false;
            bool below = false;
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                const u32 v = topology.from(h).value();
                if (v < here.size() && here[v] != 0) {
                    on.push_back(v);
                } else {
                    above = above || sideOf(v) > 0;
                    below = below || sideOf(v) < 0;
                }
            }
            if (on.size() < 2 || !above || !below) {
                continue; // it only touches the plane, or lies along it
            }
            // Along the plane's line through this face: the crossings in order,
            // and each step between two of them that runs inside the face, so
            // a concave face cut four times gets two edges and not a cross, and
            // a reflex corner the plane only grazes ends no step (MTX-6).
            const Vector3f line = unitOr(cross(normal, faceArea(mesh, FaceId(f))), Vector3f{1.0f, 0.0f, 0.0f});
            std::sort(on.begin(), on.end(), [&](u32 a, u32 b) {
                return at[a].dot(line) < at[b].dot(line);
            });
            const std::vector<Vector3f> places = placesOf(at, loopOf(topology, FaceId(f)));
            for (std::size_t i = 0; i + 1 < on.size(); ++i) {
                if (!joined(topology, on[i], on[i + 1]) &&
                    insideLoop(places, (at[on[i]] + at[on[i + 1]]) * 0.5f)) {
                    cuts.emplace_back(on[i], on[i + 1]);
                }
            }
        }
    }
    for (const auto& [a, b] : cuts) {
        mesh.ensureConnectivity();
        const Topology& topology = std::as_const(mesh).topology();
        if (joined(topology, a, b)) {
            continue;
        }
        FaceId face;
        for (const HalfedgeId h : topology.voh(VertexId(a))) {
            const FaceId candidate = topology.face(h);
            if (!isWanted(candidate) || topology.isDeleted(candidate)) {
                continue;
            }
            for (const HalfedgeId other : topology.fh(candidate)) {
                if (topology.from(other).value() == b) {
                    face = candidate;
                }
            }
            if (face.valid()) {
                break;
            }
        }
        if (!face.valid()) {
            continue;
        }
        const HalfedgeId from = leaving(topology, face, a);
        const HalfedgeId to = leaving(topology, face, b);
        if (!from.valid() || !to.valid() || !SplitFaceAt(mesh, from, to).valid()) {
            continue;
        }
        ++plan.changed;
        const Topology& built = std::as_const(mesh).topology();
        if (const HalfedgeId made = built.findHalfedge(VertexId(a), VertexId(b)); made.valid()) {
            plan.selection.edges.push_back(Topology::edge(made).value());
            plan.touchedEdges.push_back(Topology::edge(made).value());
        }
    }
    mesh.ensureConnectivity();
    const Topology& sliced = std::as_const(mesh).topology();
    const std::span<const Vector3f> slicedAt = positionsOf(mesh);
    // A face's side: where its corners off the plane are; a face no cut parted
    // (one outside the selection) goes by its mean.
    const auto faceSide = [&](FaceId f) {
        bool above = false;
        bool below = false;
        f32 sum = 0.0f;
        for (const HalfedgeId h : sliced.fh(f)) {
            const u32 v = sliced.from(h).value();
            above = above || sideOf(v) > 0;
            below = below || sideOf(v) < 0;
            sum += distanceOf(slicedAt[v]);
        }
        if (above != below) {
            return above ? 1 : -1;
        }
        return sum > 0.0f ? 1 : (sum < 0.0f ? -1 : 0);
    };
    // The cut: the edges just made, and for Split and Remove the edges already
    // lying on the plane between two wanted faces on its two sides -- a centre
    // loop the plane runs along parts there (SLC-3).
    std::vector<u8> onCut(sliced.edgeCount(), 0);
    for (const u32 e : plan.touchedEdges) {
        onCut[e] = 1;
    }
    if (params.mode != SliceMode::Cut) {
        for (u32 e = 0; e < sliced.edgeCount(); ++e) {
            if (onCut[e] != 0 || sliced.isDeleted(EdgeId(e))) {
                continue;
            }
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            const FaceId a = sliced.face(h);
            const FaceId b = sliced.face(Topology::opposite(h));
            if (sideOf(sliced.from(h).value()) == 0 && sideOf(sliced.to(h).value()) == 0 && isWanted(a) &&
                isWanted(b) && faceSide(a) * faceSide(b) < 0) {
                onCut[e] = 1;
                plan.selection.edges.push_back(e);
                plan.touchedEdges.push_back(e);
                ++plan.changed;
            }
        }
    }
    if (plan.changed == 0) {
        plan.refusal = ModelRefusal::NothingToSlice;
        return plan;
    }
    if (params.mode == SliceMode::Remove) {
        // One side of the wanted faces goes, through Delete's own rebuild
        // (§3.7); a face outside the selection stays wherever it is (SLC-1).
        ElementSet gone;
        for (u32 f = 0; f < sliced.faceCount(); ++f) {
            if (!sliced.isDeleted(FaceId(f)) && isWanted(FaceId(f)) &&
                faceSide(FaceId(f)) == (params.keepPositive ? -1 : 1)) {
                gone.faces.push_back(f);
            }
        }
        if (!gone.faces.empty()) {
            ModelPlan dropped = PlanDelete(mesh, gone);
            if (dropped.refused()) {
                plan.refusal = dropped.refusal;
                return plan;
            }
            plan.reshade = std::move(dropped.reshade); // the corners the gone faces leave (FUZ-6)
            // The rebuild renumbered the cut: it is found again as the open
            // border on the plane, which is re-shaded.
            plan.selection.edges.clear();
            plan.touchedEdges.clear();
            plan.changedFaces.clear();
            const Topology& kept = std::as_const(mesh).topology();
            const std::span<const Vector3f> keptAt = positionsOf(mesh);
            for (u32 e = 0; e < kept.edgeCount(); ++e) {
                const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
                if (!kept.isDeleted(EdgeId(e)) && kept.isBoundary(EdgeId(e)) &&
                    std::abs(distanceOf(keptAt[kept.from(h).value()])) <= tolerance &&
                    std::abs(distanceOf(keptAt[kept.to(h).value()])) <= tolerance) {
                    plan.touchedEdges.push_back(e);
                }
            }
        }
    } else if (params.mode == SliceMode::Split) {
        // The two sides part along the cut, as an edge split does: round each
        // vertex on it, the faces that meet across an edge off the cut stay
        // one group, and every group but the one on the kept (positive) side
        // takes a copy of its own. A vertex the cut only ends at stays one, and
        // nothing is copied that no face would use (SLC-5).
        const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
        detail::RebuildMapping mapping = detail::identityMapping(mesh);
        const std::vector<u32> base = cornerBases(mapping.faces);
        // Each side of each cut edge, as (face, corner ordinal), to find the
        // two borders it opens once the rebuild has renumbered the edges.
        std::vector<std::pair<u32, u32>> sides;
        std::vector<u32> ends;
        for (u32 e = 0; e < sliced.edgeCount(); ++e) {
            if (onCut[e] == 0) {
                continue;
            }
            for (u32 half = 0; half < 2; ++half) {
                const HalfedgeId h = Topology::halfedge(EdgeId(e), half);
                const FaceId f = sliced.face(h);
                if (!f.valid()) {
                    continue;
                }
                u32 ordinal = 0;
                for (const HalfedgeId w : sliced.fh(f)) {
                    if (w == h) {
                        break;
                    }
                    ++ordinal;
                }
                sides.emplace_back(f.value(), ordinal);
            }
            ends.push_back(sliced.from(Topology::halfedge(EdgeId(e), 0)).value());
            ends.push_back(sliced.to(Topology::halfedge(EdgeId(e), 0)).value());
        }
        std::vector<u32> copies;
        for (const u32 v : sortedUnique(std::move(ends))) {
            std::vector<u32> around;
            std::vector<u32> group;
            for (const HalfedgeId h : sliced.voh(VertexId(v))) {
                if (const FaceId f = sliced.face(h); f.valid()) {
                    around.push_back(f.value());
                    group.push_back(static_cast<u32>(group.size()));
                }
            }
            const auto indexOf = [&](FaceId f) {
                return static_cast<u32>(std::find(around.begin(), around.end(), f.value()) - around.begin());
            };
            const auto rootOf = [&](u32 i) {
                while (group[i] != i) {
                    i = group[i];
                }
                return i;
            };
            for (const HalfedgeId h : sliced.voh(VertexId(v))) {
                const FaceId a = sliced.face(h);
                const FaceId b = sliced.face(Topology::opposite(h));
                if (a.valid() && b.valid() && onCut[Topology::edge(h).value()] == 0) {
                    group[rootOf(indexOf(a))] = rootOf(indexOf(b));
                }
            }
            u32 keeper = rootOf(0);
            for (u32 i = 0; i < around.size(); ++i) {
                if (faceSide(FaceId(around[i])) > 0) {
                    keeper = rootOf(i);
                    break;
                }
            }
            std::unordered_map<u32, u32> copyOfGroup;
            for (u32 i = 0; i < around.size(); ++i) {
                const u32 root = rootOf(i);
                if (root == keeper) {
                    continue;
                }
                auto copy = copyOfGroup.find(root);
                if (copy == copyOfGroup.end()) {
                    copy = copyOfGroup.emplace(root, static_cast<u32>(mapping.vertexSource.size())).first;
                    mapping.vertexSource.push_back(v);
                    copies.push_back(copy->second);
                }
                u32 corner = base[around[i]];
                for (const HalfedgeId h : sliced.fh(FaceId(around[i]))) {
                    if (sliced.from(h).value() == v) {
                        mapping.faces.cornerVertex[corner] = copy->second;
                    }
                    ++corner;
                }
            }
        }
        if (!copies.empty()) {
            mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
            const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
            if (!result.ok) {
                plan.refusal = ModelRefusal::WouldFold;
                return plan;
            }
            // The copies are their own points, so nothing welds them back (Q8).
            freshGroups(mesh, copies);
            // The cut again, as the borders it opened: the faces keep their
            // places and corner order through an identity rebuild.
            plan.selection.edges.clear();
            plan.touchedEdges.clear();
            const Topology& parted = std::as_const(mesh).topology();
            for (const auto& [f, ordinal] : sides) {
                if (f >= parted.faceCount() || parted.isDeleted(FaceId(f))) {
                    continue;
                }
                u32 k = 0;
                for (const HalfedgeId h : parted.fh(FaceId(f))) {
                    if (k++ == ordinal) {
                        plan.selection.edges.push_back(Topology::edge(h).value());
                        plan.touchedEdges.push_back(Topology::edge(h).value());
                        break;
                    }
                }
            }
        }
    }
    plan.touchedEdges = sortedUnique(std::move(plan.touchedEdges));
    plan.selection.edges = sortedUnique(std::move(plan.selection.edges));
    const Topology& built = std::as_const(mesh).topology();
    for (const u32 e : plan.touchedEdges) {
        for (u32 half = 0; half < 2; ++half) {
            if (const FaceId face = built.face(Topology::halfedge(EdgeId(e), half));
                face.valid() && !built.isDeleted(face)) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    // Every corner of a face the cut parted: its far corners' fans now weigh
    // a smaller face (FUZ-6).
    for (const u32 f : plan.changedFaces) {
        for (const HalfedgeId h : built.fh(FaceId(f))) {
            plan.reshade.push_back(built.from(h).value());
        }
    }
    plan.reshade = sortedUnique(std::move(plan.reshade));
    plan.selection.normalise();
    return plan;
}

ModelPlan PlanTurnEdge(Mesh& mesh, EdgeId edge) {
    ModelPlan plan;
    plan.renumbers = true;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    Mesh work = mesh;
    if (std::as_const(work).topology().hasDeleted()) {
        const Topology::Remap remap = GarbageCollect(work);
        edge = EdgeId(edge.value() < remap.edges.size() ? remap.edges[edge.value()] : edge.value());
    }
    const Topology& topology = std::as_const(work).topology();
    if (edge.value() >= topology.edgeCount() || topology.isDeleted(edge) || topology.isBoundary(edge)) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    // Two triangles (a, b, c) and (b, a, d) about the edge a-b become (a, d, c)
    // and (b, c, d); the general rotation over n-gons is `PlanEditTriangulation`'s
    // business (§3.4).
    const HalfedgeId left = Topology::halfedge(edge, 0);
    const HalfedgeId right = Topology::opposite(left);
    const FaceId leftFace = topology.face(left);
    const FaceId rightFace = topology.face(right);
    if (topology.valence(leftFace) != 3 || topology.valence(rightFace) != 3) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    const std::span<const u32> sections = work.faceSections();
    if (leftFace.value() < sections.size() && rightFace.value() < sections.size() &&
        sections[leftFace.value()] != sections[rightFace.value()]) {
        plan.refusal = ModelRefusal::CrossesSection;
        return plan;
    }
    const u32 a = topology.from(left).value();
    const u32 b = topology.to(left).value();
    const u32 c = topology.to(topology.next(left)).value();
    const u32 d = topology.to(topology.next(right)).value();
    if (joined(topology, c, d)) {
        plan.refusal = ModelRefusal::DiagonalExists;
        return plan;
    }
    {
        // Neither new triangle may face away from the pair it replaces.
        const std::span<const Vector3f> p = positionsOf(work);
        const std::array<f64, 3> one = triangleNormal(p[a], p[b], p[c]);
        const std::array<f64, 3> two = triangleNormal(p[b], p[a], p[d]);
        const std::array<f64, 3> was{one[0] + two[0], one[1] + two[1], one[2] + two[2]};
        for (const std::array<f64, 3>& made : {triangleNormal(p[a], p[d], p[c]), triangleNormal(p[b], p[c], p[d])}) {
            if (!(made[0] * was[0] + made[1] * was[1] + made[2] * was[2] > 0.0)) {
                plan.refusal = ModelRefusal::WouldFold;
                return plan;
            }
        }
    }
    // Rebuilt with each corner taking a corner of its own vertex, so every
    // value stays where it was (TRN-1); the new edge is a new edge, its flags
    // the crease rule's.
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(work);
    std::unordered_map<u32, u32> ordinalOf;
    for (u32 i = 0; i < snapshot.size(); ++i) {
        ordinalOf[snapshot[i]] = i;
    }
    const auto ordinal = [&](HalfedgeId h) {
        const auto at = ordinalOf.find(static_cast<u32>(h.index()));
        return at == ordinalOf.end() ? kInvalidId : at->second;
    };
    const u32 atA = ordinal(left);
    const u32 atB = ordinal(right);
    const u32 atC = ordinal(topology.prev(left));
    const u32 atD = ordinal(topology.prev(right));
    detail::RebuildMapping mapping = detail::identityMapping(work);
    std::vector<u32> base(mapping.faces.faceCount() + 1, 0);
    for (u32 f = 0; f < mapping.faces.faceCount(); ++f) {
        base[f + 1] = base[f] + mapping.faces.faceValence[f];
    }
    const auto put = [&](u32 f, std::array<u32, 3> corners, std::array<u32, 3> sources) {
        for (u32 k = 0; k < 3; ++k) {
            mapping.faces.cornerVertex[base[f] + k] = corners[k];
            mapping.cornerSource[base[f] + k] = sources[k];
        }
    };
    put(leftFace.value(), {a, d, c}, {atA, atD, atC});
    put(rightFace.value(), {b, c, d}, {atB, atC, atD});
    const detail::RebuildResult result = detail::rebuild(work, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changed = 1;
    const Topology& built = std::as_const(work).topology();
    const HalfedgeId turned = built.findHalfedge(VertexId(c), VertexId(d));
    if (turned.valid()) {
        plan.touchedEdges.push_back(Topology::edge(turned).value());
        plan.selection.edges.push_back(Topology::edge(turned).value());
    }
    plan.changedFaces = {leftFace.value(), rightFace.value()};
    mesh = std::move(work);
    return plan;
}

// ============================================================================
// Parts (§3.16)
// ============================================================================

namespace {

/// The chosen live faces of @p faces, and whether they are every face there is.
std::vector<u32> chosenFaces(const Topology& topology, const ElementSet& faces, bool& all) {
    std::vector<u32> chosen;
    u32 live = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        live += topology.isDeleted(FaceId(f)) ? 0u : 1u;
    }
    for (const u32 f : sortedUnique(faces.faces)) {
        if (f < topology.faceCount() && !topology.isDeleted(FaceId(f))) {
            chosen.push_back(f);
        }
    }
    all = !chosen.empty() && chosen.size() == live;
    return chosen;
}

} // namespace

ModelPlan PlanDuplicate(Mesh& mesh, const ElementSet& faces) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    bool all = false;
    const std::vector<u32> chosen = chosenFaces(topology, faces, all);
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    // The copies are a shell of their own: every vertex they use is copied,
    // once per fan of chosen faces round it, so faces that met only at a
    // vertex do not make a pinch (DUP-2). A corner is its halfedge leaving the
    // vertex; corners across a chosen edge are one fan.
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    std::vector<u8> isChosen(topology.faceCount(), 0);
    for (const u32 f : chosen) {
        isChosen[f] = 1;
    }
    std::unordered_map<u32, u32> fanOf; // corner -> its fan's first corner
    const auto fan = [&](u32 h) {
        while (fanOf.count(h) != 0 && fanOf[h] != h) {
            h = fanOf[h];
        }
        return h;
    };
    for (const u32 f : chosen) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            fanOf.emplace(h.value(), h.value());
        }
    }
    for (const u32 f : chosen) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const HalfedgeId o = Topology::opposite(h);
            const FaceId g = topology.face(o);
            if (!g.valid() || isChosen[g.index()] == 0) {
                continue;
            }
            // o runs back along h: the other face's corner at from(h) is next(o).
            const u32 a = fan(h.value());
            const u32 b = fan(topology.next(o).value());
            if (a != b) {
                fanOf[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    std::unordered_map<u32, u32> copyOf; // fan -> its copy
    std::vector<u32> made;
    for (const u32 f : chosen) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const u32 at = fan(h.value());
            if (copyOf.find(at) == copyOf.end()) {
                copyOf[at] = static_cast<u32>(mapping.vertexSource.size());
                mapping.vertexSource.push_back(topology.from(h).value());
                made.push_back(copyOf[at]);
            }
        }
    }
    for (const u32 f : chosen) {
        u32 valence = 0;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            mapping.faces.cornerVertex.push_back(copyOf[fan(h.value())]);
            const auto ordinal = ordinalOf.find(static_cast<u32>(h.index()));
            mapping.cornerSource.push_back(ordinal == ordinalOf.end() ? kInvalidId : ordinal->second);
            ++valence;
        }
        mapping.faces.faceValence.push_back(valence);
        mapping.faceSource.push_back(f);
        plan.selection.faces.push_back(static_cast<u32>(mapping.faces.faceValence.size() - 1));
        plan.changedFaces.push_back(plan.selection.faces.back());
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    freshGroups(mesh, made);
    plan.changed = static_cast<u32>(chosen.size());
    touchFaces(mesh, plan);
    return plan;
}

ModelPlan PlanDetach(Mesh& mesh, const ElementSet& faces, Mesh& made, bool keep) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    bool all = false;
    const std::vector<u32> chosen = chosenFaces(topology, faces, all);
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (all) {
        if (!keep) {
            plan.refusal = ModelRefusal::AllFaces; // a whole mesh moves at the Mesh level
            return plan;
        }
        made = mesh; // a clone of every face is the whole mesh again (DET-1)
        plan.changed = static_cast<u32>(chosen.size());
        return plan;
    }
    // The new mesh is this one with everything else gone: Delete's own rebuild
    // carries the faces' vertices, their skin and every layer with them, and
    // copies a vertex the faces left behind still use (§3.7).
    ElementSet others;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (!topology.isDeleted(FaceId(f)) &&
            !std::binary_search(chosen.begin(), chosen.end(), f)) {
            others.faces.push_back(f);
        }
    }
    made = mesh;
    ModelPlan lifted = PlanDelete(made, others);
    if (lifted.refused()) {
        plan.refusal = lifted.refusal;
        return plan;
    }
    FinishTool(made, lifted);
    if (keep) {
        plan.changed = static_cast<u32>(chosen.size());
        return plan; // a clone: the source keeps them
    }
    ElementSet gone;
    gone.faces = chosen;
    ModelPlan dropped = PlanDelete(mesh, gone);
    if (dropped.refused()) {
        plan.refusal = dropped.refusal;
        return plan;
    }
    plan.changed = static_cast<u32>(chosen.size());
    plan.changedFaces = std::move(dropped.changedFaces);
    plan.touchedEdges = std::move(dropped.touchedEdges);
    plan.reshade = std::move(dropped.reshade);
    return plan;
}

ModelPlan PlanDetachToElement(Mesh& mesh, const ElementSet& faces) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    bool all = false;
    const std::vector<u32> chosen = chosenFaces(topology, faces, all);
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (all) {
        plan.refusal = ModelRefusal::AllFaces; // the whole mesh is already its own shell
        return plan;
    }
    // Flip's first step without the reversal: a copy per fan of chosen faces at
    // every border vertex of the region, and the region takes the copies.
    std::vector<u32> regionOf(topology.faceCount(), kInvalidId);
    for (const u32 f : chosen) {
        regionOf[f] = 0;
    }
    const Twins twins(mesh, skinning::BuildPointTable(mesh));
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    const std::vector<u32> base = cornerBases(mapping.faces);
    const std::vector<u8> nothingPinned;
    const Grown grown = growRegion(mesh, twins, chosen, regionOf, 0, mapping, nothingPinned);
    for (const u32 f : chosen) {
        u32 corner = base[f];
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            mapping.faces.cornerVertex[corner++] = useAt(grown, topology.from(h).value(), f);
        }
        plan.selection.faces.push_back(f);
        plan.changedFaces.push_back(f);
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    // Both sides of the border it opens are shaded afresh (X10, FUZ-6).
    for (const u32 copy : grown.copies) {
        plan.reshade.push_back(copy);
        if (copy < mapping.vertexSource.size()) {
            plan.reshade.push_back(mapping.vertexSource[copy]);
        }
    }
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan = ModelPlan{};
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    freshGroups(mesh, grown.copies);
    plan.changed = static_cast<u32>(chosen.size());
    touchFaces(mesh, plan);
    return plan;
}

ModelPlan PlanCreatePolygon(Mesh& mesh, const PointTable& points, const std::vector<u32>& loop) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    (void)points;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    if (loop.size() < 3) {
        plan.refusal = ModelRefusal::CapTooShort;
        return plan;
    }
    if (sortedUnique(loop).size() != loop.size()) {
        plan.refusal = ModelRefusal::WouldRepeatVertex;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    for (const u32 v : loop) {
        if (v >= topology.vertexCount() || topology.isDeleted(VertexId(v))) {
            plan.refusal = ModelRefusal::EmptySelection;
            return plan;
        }
    }
    // Every edge of the new face is either new, or a border edge used against
    // its own direction: that is what gives it a second face wound the same way
    // as the first (§3.16).
    std::vector<u32> rim(loop.size(), kInvalidId); // the face beside each vertex, for its corner
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const u32 a = loop[i];
        const u32 b = loop[(i + 1) % loop.size()];
        const HalfedgeId forward = topology.findHalfedge(VertexId(a), VertexId(b));
        if (!forward.valid()) {
            continue; // a new edge: nothing to agree with
        }
        if (topology.face(forward).valid()) {
            // A face already runs this edge the way the new one would: it would
            // be the second on that side, wound alike.
            plan.refusal = topology.isBoundary(Topology::edge(forward)) ? ModelRefusal::WindingDisagrees
                                                                        : ModelRefusal::WouldPinch;
            return plan;
        }
        // The hole's own halfedge: the new face fills it, and takes its corners
        // from the face on the other side.
        if (const FaceId face = topology.face(Topology::opposite(forward)); face.valid()) {
            rim[i] = face.value();
        }
    }
    {
        const std::span<const Vector3f> at = positionsOf(mesh);
        std::vector<Vector3f> shape;
        for (const u32 v : loop) {
            shape.push_back(at[v]);
        }
        if (foldsOver(shape)) {
            plan.refusal = ModelRefusal::WouldFold;
            return plan;
        }
    }
    if (faceExists(mesh, loop)) {
        plan.refusal = ModelRefusal::WouldDuplicateFace;
        return plan;
    }
    // Its corners copy the face beside each vertex, as Cap's do.
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    mapping.faces.faceValence.push_back(static_cast<u32>(loop.size()));
    u32 lowest = kInvalidId;
    for (std::size_t i = 0; i < loop.size(); ++i) {
        mapping.faces.cornerVertex.push_back(loop[i]);
        u32 source = kInvalidId;
        // A corner whose own edge is new reads the face beside the edge it
        // arrives on, which holds it too, else any face round it (CP-2).
        u32 beside = rim[i] != kInvalidId ? rim[i] : rim[(i + loop.size() - 1) % loop.size()];
        if (beside == kInvalidId && topology.outgoing(VertexId(loop[i])).valid()) {
            for (const HalfedgeId h : topology.voh(VertexId(loop[i]))) {
                if (topology.face(h).valid() && beside == kInvalidId) {
                    beside = topology.face(h).value();
                }
            }
        }
        if (beside != kInvalidId) {
            if (rim[i] != kInvalidId) {
                lowest = std::min(lowest, rim[i]);
            }
            for (const HalfedgeId h : topology.fh(FaceId(beside))) {
                if (topology.from(h).value() == loop[i]) {
                    const auto ordinal = ordinalOf.find(static_cast<u32>(h.index()));
                    source = ordinal == ordinalOf.end() ? kInvalidId : ordinal->second;
                }
            }
        }
        mapping.cornerSource.push_back(source);
    }
    mapping.faceSource.push_back(lowest == kInvalidId ? 0u : lowest);
    const u32 slot = static_cast<u32>(mapping.faces.faceValence.size() - 1);
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changed = 1;
    plan.selection.faces.push_back(slot);
    plan.changedFaces.push_back(slot);
    touchFaces(mesh, plan);
    return plan;
}

namespace {

/// One face of a bridge's band: its corners as (rim, edge) -- rim 0 / 2 the
/// start / end of the first loop's edge, 1 / 3 of the second's. A loop that
/// crosses a crack ends one edge on a twin of where the next starts (BRG-3).
using BandFace = std::vector<std::pair<u32, u32>>;

/// The band between two loops (§3.10), wound against both, @p there matched
/// the other way round and `twist` stepping its start. Equal counts: a quad per
/// edge pair from the pair whose rungs are shortest in sum. Different counts
/// (Q14, as Max and Blender build them): the rungs fall where the two loops
/// have run the same share of their length, with a triangle wherever one loop
/// steps on and the other does not.
std::vector<BandFace> bandBetween(std::span<const Vector3f> at, const std::vector<u32>& here,
                                  const std::vector<u32>& there, i32 twist) {
    const std::size_t n = here.size();
    const std::size_t m = there.size();
    std::size_t best = 0;
    f32 shortest = FLT_MAX;
    for (std::size_t offset = 0; offset < m; ++offset) {
        f32 sum = 0.0f;
        if (n == m) {
            for (std::size_t i = 0; i < n; ++i) {
                sum += (at[there[(offset + n - i) % n]] - at[here[i]]).length();
            }
        } else {
            sum = (at[there[offset]] - at[here[0]]).length();
        }
        if (sum < shortest) {
            shortest = sum;
            best = offset;
        }
    }
    best = (best + static_cast<std::size_t>((twist % static_cast<i32>(m)) + static_cast<i32>(m))) % m;
    const auto onThere = [&](std::size_t j) { return static_cast<u32>((best + m - j % m) % m); };
    std::vector<BandFace> band;
    // The second loop's edge between T(j + 1) and T(j), from the first.
    const auto thereEdge = [&](std::size_t j) { return onThere(j + 1); };
    if (n == m) {
        for (std::size_t i = 0; i < n; ++i) {
            band.push_back({{0u, static_cast<u32>(i)}, {2u, static_cast<u32>(i)}, {1u, thereEdge(i)},
                            {3u, thereEdge(i)}});
        }
        return band;
    }
    // Each loop's corners by the share of its length run to reach them.
    const auto shares = [&](std::size_t count, const auto& vertexAt) {
        std::vector<f64> out(count + 1, 0.0);
        for (std::size_t k = 0; k < count; ++k) {
            out[k + 1] = out[k] + length64(sub(at[vertexAt((k + 1) % count)], at[vertexAt(k)]));
        }
        for (f64& s : out) {
            s = out[count] > 0.0 ? s / out[count] : 0.0;
        }
        return out;
    };
    const std::vector<f64> a = shares(n, [&](std::size_t k) { return here[k]; });
    const std::vector<f64> b = shares(m, [&](std::size_t k) { return there[onThere(k)]; });
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n || j < m) {
        const f64 nextA = i < n ? a[i + 1] : 2.0;
        const f64 nextB = j < m ? b[j + 1] : 2.0;
        const u32 hi = static_cast<u32>(i % n);
        if (i < n && j < m && std::abs(nextA - nextB) <= 1e-6) {
            band.push_back({{0u, hi}, {2u, hi}, {1u, thereEdge(j)}, {3u, thereEdge(j)}});
            ++i;
            ++j;
        } else if (nextA < nextB) {
            band.push_back({{0u, hi}, {2u, hi}, {1u, onThere(j)}});
            ++i;
        } else {
            band.push_back({{0u, hi}, {1u, thereEdge(j)}, {3u, thereEdge(j)}});
            ++j;
        }
    }
    return band;
}

/// Whether the two loops share a point -- a vertex, or two coincident ones
/// the point table joins: a rung there has no length (MTX-10).
bool loopsTouch(const PointTable& points, const std::vector<u32>& here, const std::vector<u32>& there) {
    const auto pointOf = [&](u32 v) { return v < points.pointOf.size() ? points.pointOf[v] : v; };
    for (const u32 v : here) {
        for (const u32 w : there) {
            if (v == w || pointOf(v) == pointOf(w)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

ModelPlan PlanBridge(Mesh& mesh, const PointTable& points, EdgeId first, EdgeId second,
                     const BridgeParams& params) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    (void)points;
    if (params.segments != 1) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    // The rim over points (Q5): where the next border edge is a crack's, the
    // walk steps across to the twin side's border and on, as Cap's does, so a
    // rim that crosses a crack is one loop (BRG-3).
    const Twins twins(mesh, points);
    const auto cracked = [&](HalfedgeId h) { return !twins.of(Topology::edge(h).value()).empty(); };
    const auto loopOfEdge = [&](EdgeId edge, std::vector<u32>& vertices, std::vector<u32>& ends,
                                std::vector<HalfedgeId>& rim) {
        if (edge.value() >= topology.edgeCount() || topology.isDeleted(edge) ||
            !topology.isBoundary(edge)) {
            return false;
        }
        HalfedgeId start = Topology::halfedge(edge, 0);
        if (!topology.isBoundary(start)) {
            start = Topology::opposite(start);
        }
        HalfedgeId walk = start;
        do {
            vertices.push_back(topology.from(walk).value());
            ends.push_back(topology.to(walk).value());
            rim.push_back(Topology::opposite(walk));
            HalfedgeId next = topology.next(walk);
            if (!cracked(start) && cracked(next)) {
                HalfedgeId found;
                u32 count = 0;
                for (const u32 member : membersOfPoint(mesh, points, topology.to(walk).value())) {
                    for (const HalfedgeId h : topology.voh(VertexId(member))) {
                        if (topology.isBoundary(h) && !topology.face(h).valid() && !cracked(h)) {
                            found = h;
                            ++count;
                        }
                    }
                }
                if (count == 1) {
                    next = found;
                }
            }
            walk = next;
        } while (walk.valid() && walk != start && vertices.size() <= topology.halfedgeCount());
        return walk == start && vertices.size() >= 3;
    };
    std::vector<u32> here;
    std::vector<u32> there;
    std::vector<u32> hereEnd;
    std::vector<u32> thereEnd;
    std::vector<HalfedgeId> hereRim;
    std::vector<HalfedgeId> thereRim;
    if (!loopOfEdge(first, here, hereEnd, hereRim) || !loopOfEdge(second, there, thereEnd, thereRim)) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (loopsTouch(points, here, there)) {
        plan.refusal = ModelRefusal::SameLoop;
        return plan;
    }
    // The loops are matched the other way round, as a bridge that does not
    // twist needs (`bandBetween`).
    const std::vector<BandFace> band = bandBetween(positionsOf(mesh), here, there, params.twist);
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    const auto cornerOf = [&](HalfedgeId rim, u32 vertex) {
        // The rim face's corner at that vertex, as Extrude's walls take theirs.
        if (!rim.valid() || !topology.face(rim).valid()) {
            return kInvalidId;
        }
        for (const HalfedgeId h : topology.fh(topology.face(rim))) {
            if (topology.from(h).value() == vertex) {
                const auto ordinal = ordinalOf.find(static_cast<u32>(h.index()));
                return ordinal == ordinalOf.end() ? kInvalidId : ordinal->second;
            }
        }
        return kInvalidId;
    };
    for (const BandFace& face : band) {
        // Each corner from its own rim's face, as Extrude's walls take theirs.
        mapping.faces.faceValence.push_back(static_cast<u32>(face.size()));
        for (const auto& [rim, k] : face) {
            const u32 vertex = rim == 0 ? here[k] : rim == 1 ? there[k] : rim == 2 ? hereEnd[k] : thereEnd[k];
            mapping.faces.cornerVertex.push_back(vertex);
            mapping.cornerSource.push_back(cornerOf(rim % 2 == 0 ? hereRim[k] : thereRim[k], vertex));
        }
        const HalfedgeId wearing = hereRim[face.front().second];
        mapping.faceSource.push_back(topology.face(wearing).valid() ? topology.face(wearing).value() : 0u);
        plan.selection.faces.push_back(static_cast<u32>(mapping.faces.faceValence.size() - 1));
        plan.changedFaces.push_back(plan.selection.faces.back());
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changed = static_cast<u32>(band.size());
    touchFaces(mesh, plan);
    return plan;
}


ModelPlan PlanBridgeFaces(Mesh& mesh, const PointTable& points, FaceId first, FaceId second,
                          const BridgeParams& params) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    (void)points;
    if (params.segments != 1) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    if (first == second) {
        plan.refusal = ModelRefusal::SameLoop;
        return plan;
    }
    // Each face's loop the way the border its removal would leave runs, which is
    // the face's OWN direction: the face and its neighbour cross their shared
    // edge opposite ways, so the boundary halfedge left behind -- the neighbour's
    // twin -- runs the way the face did. The pairing below is `PlanBridge`'s,
    // written for border loops, and this is what makes the two forms one rule.
    //
    // Reversing here instead builds the band wound the other way round. Two
    // loose shells bridged like that are still a manifold, just inside out, so
    // nothing catches it; on a surface with neighbours every rim vertex ends up
    // with two fans and the repair splits all of them.
    const auto loopOfFace = [&](FaceId face, std::vector<u32>& vertices, std::vector<HalfedgeId>& rim) {
        if (face.value() >= topology.faceCount() || topology.isDeleted(face)) {
            return false;
        }
        for (const HalfedgeId h : topology.fh(face)) {
            vertices.push_back(topology.from(h).value());
            rim.push_back(h); // the face's own corner at that vertex
        }
        return vertices.size() >= 3;
    };
    std::vector<u32> here;
    std::vector<u32> there;
    std::vector<HalfedgeId> hereRim;
    std::vector<HalfedgeId> thereRim;
    if (!loopOfFace(first, here, hereRim) || !loopOfFace(second, there, thereRim)) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (loopsTouch(points, here, there)) {
        plan.refusal = ModelRefusal::SameLoop;
        return plan;
    }
    // The loops matched the other way round, `twist` stepping the start round.
    const std::vector<BandFace> band = bandBetween(positionsOf(mesh), here, there, params.twist);
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    const auto ordinal = [&](HalfedgeId h) {
        const auto found = ordinalOf.find(static_cast<u32>(h.index()));
        return found == ordinalOf.end() ? kInvalidId : found->second;
    };
    // Every face but those two, and then the band. The two go without a delete
    // of their own -- they are simply not rebuilt -- and their vertices stay,
    // because the band is what uses them now.
    std::vector<u8> kept(topology.vertexCount(), 0);
    for (const u32 v : here) {
        kept[v] = 1;
    }
    for (const u32 v : there) {
        kept[v] = 1;
    }
    std::vector<std::vector<u32>> loops;
    std::vector<std::vector<u32>> corners;
    std::vector<u32> sourceFace;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        const FaceId face(f);
        if (topology.isDeleted(face) || face == first || face == second) {
            continue;
        }
        std::vector<u32> loop;
        std::vector<u32> mine;
        for (const HalfedgeId h : topology.fh(face)) {
            loop.push_back(topology.from(h).value());
            mine.push_back(ordinal(h));
            kept[topology.from(h).value()] = 1;
        }
        loops.push_back(std::move(loop));
        corners.push_back(std::move(mine));
        sourceFace.push_back(f);
    }
    detail::RebuildMapping mapping;
    std::vector<u32> newId(kept.size(), kInvalidId);
    for (u32 v = 0; v < kept.size(); ++v) {
        if (kept[v] != 0 && !topology.isDeleted(VertexId(v))) {
            newId[v] = static_cast<u32>(mapping.vertexSource.size());
            mapping.vertexSource.push_back(v);
        }
    }
    for (std::size_t i = 0; i < loops.size(); ++i) {
        mapping.faces.faceValence.push_back(static_cast<u32>(loops[i].size()));
        for (const u32 v : loops[i]) {
            mapping.faces.cornerVertex.push_back(newId[v]);
        }
        mapping.cornerSource.insert(mapping.cornerSource.end(), corners[i].begin(), corners[i].end());
        mapping.faceSource.push_back(sourceFace[i]);
    }
    for (const BandFace& face : band) {
        mapping.faces.faceValence.push_back(static_cast<u32>(face.size()));
        for (const auto& [rim, k] : face) {
            // A face's loop has no crack in it: an edge's end is the next one's
            // start, and its corner there is the face's own.
            const std::vector<u32>& loop = rim % 2 == 0 ? here : there;
            const std::vector<HalfedgeId>& rimCorners = rim % 2 == 0 ? hereRim : thereRim;
            const u32 at = rim < 2 ? k : static_cast<u32>((k + 1) % loop.size());
            mapping.faces.cornerVertex.push_back(newId[loop[at]]);
            mapping.cornerSource.push_back(ordinal(rimCorners[at]));
        }
        // The band wears the face it grew out of, which is what carries its
        // section: a bridge that landed in another draw would be a bridge you
        // then had to go and fix.
        mapping.faceSource.push_back(first.value());
        plan.selection.faces.push_back(static_cast<u32>(mapping.faces.faceValence.size() - 1));
        plan.changedFaces.push_back(plan.selection.faces.back());
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changed = static_cast<u32>(band.size());
    touchFaces(mesh, plan);
    return plan;
}

// ============================================================================
// Symmetry and moving well (§3.15, §3.17)
// ============================================================================

ModelPlan PlanSymmetrize(Mesh& mesh, const PointTable& points, const SymmetrizeParams& params,
                         std::span<const u32> boneMirror) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    // The plane, as one point and one direction: an axis plane through the
    // origin unless the caller named its own. Every test below is then one dot
    // product, and for an axis normal through the origin it is the component
    // the axis names -- which is what this read before it could be asked for a
    // plane that is not one of the three.
    const u32 axis = std::min(2u, params.axis);
    Vector3f cardinal{0.0f, 0.0f, 0.0f};
    (&cardinal.x)[axis] = 1.0f;
    const bool placed = params.normal.length() > 1e-12f;
    // The normal points AT the source side, so the sign carries `fromPositive`.
    const Vector3f normal =
        (placed ? params.normal.normalized() : cardinal) * (params.fromPositive ? 1.0f : -1.0f);
    const Vector3f seam = placed ? params.origin : Vector3f{0.0f, 0.0f, 0.0f};
    // A reflection about it, for a point and for a direction: the direction's
    // is the same with the offset left out.
    const auto reflectPoint = [&](const Vector3f& p) {
        return p - normal * (2.0f * (p - seam).dot(normal));
    };
    const auto reflectDirection = [&](const Vector3f& d) {
        return d - normal * (2.0f * d.dot(normal));
    };
    // The far side goes first, and a vertex on the plane is snapped onto it so
    // the two halves meet exactly (§3.14's own rule).
    SliceParams cut;
    cut.origin = seam;
    cut.normal = normal;
    cut.mode = SliceMode::Cut;
    ModelPlan sliced = PlanSlice(mesh, points, ElementSet{}, cut);
    if (sliced.refused() && sliced.refusal != ModelRefusal::NothingToSlice) {
        plan.refusal = sliced.refusal;
        return plan;
    }
    // The steps finish with no crease rule: it runs once, on the seam, at the
    // caller's angle (SYM-8).
    constexpr f32 kNoCrease = 3.14159265358979f;
    if (!sliced.refused()) {
        FinishTool(mesh, sliced, kNoCrease);
    }
    // And the far side goes, whether the plane had to cut anything or ran along
    // edges that were already there: Slice refuses the second case, and the
    // half still has to go. So does a face lying in the plane: the halves meet
    // across it, so it would be inside (SYM-1).
    {
        mesh.ensureConnectivity();
        const Topology& before = std::as_const(mesh).topology();
        const std::span<const Vector3f> places = positionsOf(mesh);
        const f32 flat = CoincidenceTolerance(mesh);
        ElementSet gone;
        for (u32 f = 0; f < before.faceCount(); ++f) {
            if (before.isDeleted(FaceId(f))) {
                continue;
            }
            f32 sum = 0.0f;
            f32 farthest = 0.0f;
            u32 corners = 0;
            for (const HalfedgeId h : before.fh(FaceId(f))) {
                const f32 d = (places[before.from(h).value()] - seam).dot(normal);
                sum += d;
                farthest = std::max(farthest, std::abs(d));
                ++corners;
            }
            if (corners != 0 && (sum / static_cast<f32>(corners) < 0.0f || farthest <= flat)) {
                gone.faces.push_back(f);
            }
        }
        if (!gone.faces.empty()) {
            ModelPlan dropped = PlanDelete(mesh, gone);
            if (dropped.refused()) {
                plan.refusal = dropped.refusal == ModelRefusal::AllFaces ? ModelRefusal::NoSourceSide
                                                                        : dropped.refusal;
                return plan;
            }
            FinishTool(mesh, dropped, kNoCrease);
        }
    }
    const f32 tolerance = CoincidenceTolerance(mesh);
    const Topology& topology = std::as_const(mesh).topology();
    // The seam is welded (§3.15 step 3): a border vertex within `weld` of the
    // plane is put on it, so the halves share it (3ds Max's Weld Seam).
    {
        const std::span<Vector3f> places = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        Extent box;
        ResetExtent(box);
        for (u32 v = 0; v < topology.vertexCount(); ++v) {
            if (!topology.isDeleted(VertexId(v))) {
                GrowExtent(box, places[v]);
            }
        }
        const f32 weld = params.weld * (box.maximum - box.minimum).length();
        for (u32 v = 0; v < topology.vertexCount(); ++v) {
            if (topology.isDeleted(VertexId(v)) || !topology.isBoundary(VertexId(v))) {
                continue;
            }
            const f32 d = (places[v] - seam).dot(normal);
            if (std::abs(d) > tolerance && std::abs(d) <= weld) {
                places[v] = places[v] - normal * d;
                plan.reshade.push_back(v);
            }
        }
    }
    const std::span<const Vector3f> at = positionsOf(mesh);
    bool any = false;
    for (u32 f = 0; f < topology.faceCount() && !any; ++f) {
        any = !topology.isDeleted(FaceId(f));
    }
    if (!any) {
        plan.refusal = ModelRefusal::NoSourceSide;
        return plan;
    }
    // A rigid section whose bone has a mirror is spelled out per vertex, so the
    // copy's skin goes through the bone map below (SYM-5).
    if (!boneMirror.empty()) {
        for (u32 s = 0; s < mesh.sections.size(); ++s) {
            const std::optional<u32>& node = mesh.sections[s].rigidNode;
            if (node && *node < boneMirror.size() && boneMirror[*node] != kInvalidId && boneMirror[*node] != *node) {
                BakeRigidNode(mesh, s);
            }
        }
    }
    // The reflected copy: every vertex off the plane gets one, and every face a
    // reversed twin. A vertex on the plane is shared, which is the seam, where a
    // border edge of it runs in the plane; one that touches the plane alone keeps
    // a copy of its own, as two fans cannot meet at one vertex (SYM-11).
    std::vector<u8> onPlane(topology.vertexCount(), 0);
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        onPlane[v] = !topology.isDeleted(VertexId(v)) && std::abs((at[v] - seam).dot(normal)) <= tolerance ? 1 : 0;
    }
    const auto shared = [&](u32 v) {
        if (onPlane[v] == 0) {
            return false;
        }
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            if (topology.isBoundary(Topology::edge(h)) && onPlane[topology.to(h).value()] != 0) {
                return true;
            }
        }
        return false;
    };
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    std::vector<u32> mirrorOf(topology.vertexCount(), kInvalidId);
    std::vector<u32> made;
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (topology.isDeleted(VertexId(v))) {
            continue;
        }
        if (shared(v)) {
            mirrorOf[v] = v; // on the plane: the halves share it
            continue;
        }
        mirrorOf[v] = static_cast<u32>(mapping.vertexSource.size());
        mapping.vertexSource.push_back(v);
        made.push_back(mirrorOf[v]);
    }
    const u32 faces = topology.faceCount();
    for (u32 f = 0; f < faces; ++f) {
        if (topology.isDeleted(FaceId(f))) {
            continue;
        }
        std::vector<u32> loop;
        std::vector<HalfedgeId> corners;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            loop.push_back(mirrorOf[topology.from(h).value()]);
            corners.push_back(h);
        }
        // Reversed: a reflection turns the winding round, and the corners come
        // with their own vertices.
        std::reverse(loop.begin(), loop.end());
        std::reverse(corners.begin(), corners.end());
        mapping.faces.faceValence.push_back(static_cast<u32>(loop.size()));
        for (std::size_t i = 0; i < loop.size(); ++i) {
            mapping.faces.cornerVertex.push_back(loop[i]);
            const auto ordinal = ordinalOf.find(static_cast<u32>(corners[i].index()));
            mapping.cornerSource.push_back(ordinal == ordinalOf.end() ? kInvalidId : ordinal->second);
        }
        mapping.faceSource.push_back(f);
        plan.changedFaces.push_back(static_cast<u32>(mapping.faces.faceValence.size() - 1));
        ++plan.changed;
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    // The copy's own places, and the attributes a reflection turns round.
    {
        const std::span<Vector3f> places = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        for (const u32 v : made) {
            if (v < places.size()) {
                places[v] = reflectPoint(places[v]);
            }
        }
    }
    freshGroups(mesh, made); // across the plane, so the halves are not one point
    {
        const Topology& built = std::as_const(mesh).topology();
        const std::span<Vector3f> normals = mesh.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
        const std::span<Vector4f> tangents = mesh.attributes.get<Vector4f>(names::kTangent, Domain::Halfedge);
        for (const u32 f : plan.changedFaces) {
            if (f >= built.faceCount() || built.isDeleted(FaceId(f))) {
                continue;
            }
            for (const HalfedgeId h : built.fh(FaceId(f))) {
                const u32 c = static_cast<u32>(h.index());
                if (c < normals.size()) {
                    normals[c] = reflectDirection(normals[c]);
                }
                if (c < tangents.size()) {
                    const Vector3f turned =
                        reflectDirection(Vector3f{tangents[c].x, tangents[c].y, tangents[c].z});
                    tangents[c] = {turned.x, turned.y, turned.z, -tangents[c].w}; // a mirrored island
                }
            }
        }
    }
    // The copy's skin through the bone map: a bone with no mirror keeps itself,
    // and those are what `kept` counts.
    if (!boneMirror.empty() && !mesh.skin.empty()) {
        for (const u32 v : made) {
            const std::span<const Influence> was = mesh.skin.forVertex(v);
            if (was.empty()) {
                continue;
            }
            std::vector<Influence> now(was.begin(), was.end());
            for (Influence& one : now) {
                if (one.bone < boneMirror.size() && boneMirror[one.bone] != kInvalidId) {
                    one.bone = boneMirror[one.bone];
                } else {
                    ++plan.kept;
                }
            }
            mesh.skin.assignVertex(v, now);
        }
    }
    // The crease rule and the re-shade run on the seam alone, at the caller's
    // angle: the copy's other edges keep their sources' flags (SYM-8, X8).
    {
        const Topology& built = std::as_const(mesh).topology();
        const auto sharedAt = [&](u32 v) { return v < mirrorOf.size() && mirrorOf[v] == v; };
        for (u32 e = 0; e < built.edgeCount(); ++e) {
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            if (!built.isDeleted(EdgeId(e)) && sharedAt(built.from(h).value()) && sharedAt(built.to(h).value())) {
                plan.touchedEdges.push_back(e);
            }
        }
    }
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    plan.selection.normalise();
    mesh.recomputeBounds();
    return plan;
}

ModelPlan PlanMirror(Mesh& mesh, const MirrorParams& params, std::span<const u32> boneMirror) {
    ModelPlan plan;
    const Rollback rollback(mesh, plan);
    if (!(params.normal.length() > 1e-12f)) {
        plan.refusal = ModelRefusal::ZeroAmount;
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Vector3f normal = params.normal.normalized();
    const auto reflectDirection = [&](const Vector3f& d) { return d - normal * (2.0f * d.dot(normal)); };
    // Every face reversed, its corners with it, and nothing renumbered: the
    // same vertices and faces, wound the other way.
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    const std::vector<u32> bases = cornerBases(mapping.faces);
    for (std::size_t f = 0; f < mapping.faces.faceCount(); ++f) {
        std::reverse(mapping.faces.cornerVertex.begin() + bases[f], mapping.faces.cornerVertex.begin() + bases[f + 1]);
        std::reverse(mapping.cornerSource.begin() + bases[f], mapping.cornerSource.begin() + bases[f + 1]);
        plan.changedFaces.push_back(static_cast<u32>(f));
    }
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    for (Vector3f& place : mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex))
        place = place - normal * (2.0f * (place - params.origin).dot(normal));
    for (const Domain domain : {Domain::Halfedge, Domain::Vertex}) {
        for (Vector3f& n : mesh.attributes.get<Vector3f>(names::kNormal, domain))
            n = reflectDirection(n);
        for (Vector3f& b : mesh.attributes.get<Vector3f>(names::kBinormal, domain))
            b = reflectDirection(b);
        for (Vector4f& t : mesh.attributes.get<Vector4f>(names::kTangent, domain)) {
            const Vector3f turned = reflectDirection(Vector3f{t.x, t.y, t.z});
            t = {turned.x, turned.y, turned.z, -t.w}; // a mirrored island
        }
    }
    if (!boneMirror.empty()) {
        const auto mirrored = [&](u32 bone) {
            if (bone < boneMirror.size() && boneMirror[bone] != kInvalidId)
                return boneMirror[bone];
            ++plan.kept;
            return bone;
        };
        for (u32 v = 0; v < mesh.skin.vertexCount(); ++v) {
            const std::span<const Influence> was = mesh.skin.forVertex(v);
            if (was.empty())
                continue;
            std::vector<Influence> now(was.begin(), was.end());
            for (Influence& one : now)
                one.bone = mirrored(one.bone);
            mesh.skin.assignVertex(v, now);
        }
        for (auto& section : mesh.sections)
            if (section.rigidNode)
                section.rigidNode = mirrored(*section.rigidNode);
    }
    plan.changed = static_cast<u32>(plan.changedFaces.size());
    mesh.recomputeBounds();
    return plan;
}

namespace {

/// Every form of Make Planar once its planes are settled (§3.17): one motion
/// per point of a part that is off that part's plane, landing it there at
/// amount 1.
ModelPlan planarOnto(Mesh& mesh, std::span<const PlanarPart> parts) {
    ModelPlan plan;
    plan.renumbers = false;
    const std::span<const Vector3f> at = positionsOf(mesh);
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u32> chosen;
    // Off by no more than rounding everywhere is already flat (PLN-5).
    const f32 rounding = CoincidenceTolerance(mesh);
    f32 farthest = 0.0f;
    bool anyPlane = false;
    for (const PlanarPart& part : parts) {
        if (part.normal.length() <= 1e-12f) {
            // No plane to go onto: the rest go, and the result line counts it (Q2).
            if (!part.vertices.empty()) {
                ++plan.kept;
                plan.keptBecause = ModelRefusal::NoPlane;
            }
            continue;
        }
        anyPlane = true;
        const Vector3f normal = part.normal.normalized();
        for (const u32 v : part.vertices) {
            if (v >= at.size() || topology.isDeleted(VertexId(v))) {
                continue;
            }
            chosen.push_back(v);
            const f32 distance = (at[v] - part.origin).dot(normal);
            farthest = std::max(farthest, std::abs(distance));
            if (std::abs(distance) <= 0.0f) {
                continue;
            }
            VertexMotion motion;
            motion.vertex = v;
            motion.base = at[v];
            motion.direction = normal * -distance; // amount 1 lands it on the plane
            plan.motions.push_back(motion);
        }
    }
    if (!anyPlane) {
        plan = ModelPlan{};
        plan.renumbers = false;
        plan.refusal = ModelRefusal::NoPlane;
        return plan;
    }
    if (plan.motions.empty() || farthest <= rounding) {
        plan.motions.clear();
        plan.refusal = ModelRefusal::ZeroAmount; // already flat
        return plan;
    }
    std::sort(plan.motions.begin(), plan.motions.end(),
              [](const VertexMotion& a, const VertexMotion& b) { return a.vertex < b.vertex; });
    plan.changed = static_cast<u32>(plan.motions.size());
    plan.amountMin = -1.0f;
    plan.amountMax = 1.0f;
    chosen = sortedUnique(std::move(chosen));
    for (const u32 v : chosen) {
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            if (const FaceId face = topology.face(h); face.valid()) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    plan.selection.vertices = std::move(chosen);
    return plan;
}

/// One plane for the whole of @p chosen.
ModelPlan planarOnto(Mesh& mesh, const std::vector<u32>& chosen, const Vector3f& origin,
                     const Vector3f& normal) {
    const PlanarPart part{chosen, origin, normal};
    return planarOnto(mesh, std::span<const PlanarPart>(&part, 1));
}

} // namespace

ModelPlan PlanMakePlanar(Mesh& mesh, const PointTable& points, const ElementSet& selection, u32 axis) {
    ModelPlan plan;
    plan.renumbers = false;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const std::vector<u32> chosen = verticesOf(mesh, points, selection);
    if (chosen.size() < 3) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    const std::span<const Vector3f> at = positionsOf(mesh);
    Vector3f centre{0.0f, 0.0f, 0.0f};
    for (const u32 v : chosen) {
        centre = centre + at[v];
    }
    centre = centre * (1.0f / static_cast<f32>(chosen.size()));
    Vector3f normal{0.0f, 0.0f, 1.0f};
    if (axis < 3) {
        normal = {0.0f, 0.0f, 0.0f};
        (&normal.x)[axis] = 1.0f;
    } else {
        // The fit, which is `FitPlane`'s: the same plane the viewer draws when
        // it offers Best fit, because it is the same code and not a second one.
        std::vector<Vector3f> gathered;
        gathered.reserve(chosen.size());
        for (const u32 v : chosen) {
            gathered.push_back(at[v]);
        }
        if (!FitPlane(gathered, &centre, &normal)) {
            plan.refusal = ModelRefusal::NoPlane; // a line fits no plane (PLN-7)
            return plan;
        }
    }
    return planarOnto(mesh, chosen, centre, normal);
}

bool FitPlane(std::span<const Vector3f> points, Vector3f* origin, Vector3f* normal) {
    // The plane through the centre whose normal is the scatter's smallest
    // eigenvector, by Jacobi rotations: exact where one adjugate row is not
    // (PLN-1), and the same however the points are turned.
    Vector3f centre{0.0f, 0.0f, 0.0f};
    for (const Vector3f& p : points) {
        centre = centre + p;
    }
    if (!points.empty()) {
        centre = centre * (1.0f / static_cast<f32>(points.size()));
    }
    if (origin != nullptr) {
        *origin = centre;
    }
    if (normal != nullptr) {
        *normal = {0.0f, 0.0f, 1.0f};
    }
    if (points.size() < 3) {
        return false;
    }
    std::array<std::array<f64, 3>, 3> a{};
    for (const Vector3f& p : points) {
        const f64 d[3] = {static_cast<f64>(p.x) - centre.x, static_cast<f64>(p.y) - centre.y,
                          static_cast<f64>(p.z) - centre.z};
        for (u32 i = 0; i < 3; ++i) {
            for (u32 j = 0; j < 3; ++j) {
                a[i][j] += d[i] * d[j];
            }
        }
    }
    std::array<std::array<f64, 3>, 3> v{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    for (u32 sweep = 0; sweep < 32; ++sweep) {
        const f64 off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        const f64 diagonal = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2];
        if (!(off > 1e-30 * diagonal)) {
            break;
        }
        for (const auto [p, q] : {std::pair<u32, u32>{0, 1}, {0, 2}, {1, 2}}) {
            const f64 apq = a[p][q];
            if (apq == 0.0) {
                continue;
            }
            const f64 theta = (a[q][q] - a[p][p]) / (2.0 * apq);
            const f64 t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
            const f64 c = 1.0 / std::sqrt(t * t + 1.0);
            const f64 sn = t * c;
            const f64 tau = sn / (1.0 + c);
            a[p][p] -= t * apq;
            a[q][q] += t * apq;
            a[p][q] = a[q][p] = 0.0;
            const u32 r = 3 - p - q;
            const f64 arp = a[r][p], arq = a[r][q];
            a[r][p] = a[p][r] = arp - sn * (arq + tau * arp);
            a[r][q] = a[q][r] = arq + sn * (arp - tau * arq);
            for (u32 k = 0; k < 3; ++k) {
                const f64 vkp = v[k][p], vkq = v[k][q];
                v[k][p] = vkp - sn * (vkq + tau * vkp);
                v[k][q] = vkq + sn * (vkp - tau * vkq);
            }
        }
    }
    // Eigenvalues smallest first; a line or a point leaves the middle one at 0.
    std::array<u32, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](u32 l, u32 r) { return a[l][l] < a[r][r]; });
    if (!(a[order[1]][order[1]] > 1e-10 * a[order[2]][order[2]]) || !(a[order[2]][order[2]] > 0.0)) {
        return false; // one point, or a line: no smallest direction to find
    }
    if (normal != nullptr) {
        const u32 k = order[0];
        *normal = unitOr(Vector3f{static_cast<f32>(v[0][k]), static_cast<f32>(v[1][k]), static_cast<f32>(v[2][k])},
                         Vector3f{0.0f, 0.0f, 1.0f});
    }
    return true;
}

ModelPlan PlanMakePlanar(Mesh& mesh, const PointTable& points, const ElementSet& selection,
                         const Vector3f& origin, const Vector3f& normal) {
    ModelPlan plan;
    plan.renumbers = false;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    // A plane the caller names needs no three points to find it.
    const std::vector<u32> chosen = verticesOf(mesh, points, selection);
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (normal.length() <= 1e-12f) {
        plan.refusal = ModelRefusal::ZeroAmount; // that is no plane
        return plan;
    }
    return planarOnto(mesh, chosen, origin, normal.normalized());
}

ModelPlan PlanMakePlanar(Mesh& mesh, std::span<const PlanarPart> parts) {
    ModelPlan plan;
    plan.renumbers = false;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    bool any = false;
    for (const PlanarPart& part : parts) {
        any = any || !part.vertices.empty();
    }
    if (!any) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    return planarOnto(mesh, parts);
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
