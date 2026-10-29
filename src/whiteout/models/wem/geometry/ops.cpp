// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/geometry/shading.h>

#include "rebuild.h"

#include <whiteout/models/wem/geometry/interpolate.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/triangulation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

// ============================================================================
// Element allocation — topology and attributes grow together
// ============================================================================

VertexId allocVertex(Mesh& mesh, u32 copySkinFrom) {
    const VertexId v = mesh.topology().addVertex();
    mesh.attributes.appendElement(Domain::Vertex);
    if (!mesh.skin.empty()) {
        if (copySkinFrom < mesh.skin.vertexCount()) {
            mesh.skin.appendCopyOf(copySkinFrom);
        } else {
            mesh.skin.appendVertex({});
        }
    }
    return v;
}

HalfedgeId allocEdge(Mesh& mesh) {
    const HalfedgeId h = mesh.topology().addEdgePair();
    mesh.attributes.appendElement(Domain::Halfedge);
    mesh.attributes.appendElement(Domain::Halfedge);
    mesh.attributes.appendElement(Domain::Edge);
    return h;
}

FaceId allocFace(Mesh& mesh, u32 copyFrom) {
    const FaceId f = mesh.topology().addFaceSlot();
    const u32 index = mesh.attributes.appendElement(Domain::Face);
    if (copyFrom != kInvalidId) {
        mesh.attributes.copyElement(Domain::Face, copyFrom, index);
    }
    mesh.triangulation.appendFace();
    return f;
}

/// A face whose corner cycle changed drops its row (§2.3): the tool's commit
/// materialises a fresh one.
void clearRow(Mesh& mesh, FaceId face) {
    if (face.valid() && !mesh.triangulation.row(face.value()).empty()) {
        mesh.triangulation.setRow(face.value(), {}, std::as_const(mesh).topology().faceCount());
    }
}

/// Face @p face's loop from its first halfedge, as vertex ids.
std::vector<u32> loopOf(const Topology& topology, FaceId face) {
    std::vector<u32> loop;
    for (const HalfedgeId h : topology.fh(face)) {
        loop.push_back(topology.from(h).value());
    }
    return loop;
}

/// Face @p face as it is drawn (`TriangulateFace`): vertex ids, three per triangle.
std::vector<u32> drawnTriangles(const Mesh& mesh, FaceId face) {
    const std::vector<u32> loop = loopOf(mesh.topology(), face);
    std::vector<u32> cut;
    TriangulateFace(loop, mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex),
                    mesh.triangulation.row(face.value()), cut);
    for (u32& corner : cut) {
        corner = loop[corner];
    }
    return cut;
}

void copyHalfedgeAttrs(AttributeSet& attributes, u32 from, u32 to) {
    attributes.copyElement(Domain::Halfedge, from, to);
}

/// Edge @p from's values folded into edge @p into's by type, the rule for an
/// edge two old ones became: Bool ORs, F32 takes the maximum, anything else
/// keeps @p into's.
void combineEdgeInto(AttributeSet& attributes, u32 from, u32 into) {
    for (const AttrLayer& layer : attributes.layers()) {
        if (layer.domain != Domain::Edge) {
            continue;
        }
        AttrLayer& target = *attributes.layer(layer.name, Domain::Edge);
        const std::size_t stride = AttrTypeSize(target.type);
        if (stride * (std::max(from, into) + 1) > target.data.size()) {
            continue;
        }
        u8* out = target.data.data() + stride * into;
        const u8* in = target.data.data() + stride * from;
        if (target.type == AttrType::Bool) {
            out[0] = (out[0] != 0 || in[0] != 0) ? 1 : 0;
        } else if (target.type == AttrType::F32) {
            f32 a = 0.0f;
            f32 b = 0.0f;
            std::memcpy(&a, out, sizeof(f32));
            std::memcpy(&b, in, sizeof(f32));
            const f32 larger = std::max(a, b);
            std::memcpy(out, &larger, sizeof(f32));
        }
    }
}

} // namespace

using detail::identityMapping;
using detail::RebuildMapping;
using detail::rebuild;
using detail::snapshotCorners;
using detail::snapshotCornersBuilt;

// ============================================================================
// SplitEdge
// ============================================================================

VertexId SplitEdge(Mesh& mesh, EdgeId edge, f32 t) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return VertexId();
    }
    Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge)) {
        return VertexId();
    }
    t = std::clamp(t, 0.0f, 1.0f);

    const HalfedgeId h0 = Topology::halfedge(edge, 0);
    const HalfedgeId h1 = Topology::halfedge(edge, 1);
    const VertexId a = topology.from(h0);
    const VertexId b = topology.to(h0);
    const HalfedgeId n0 = topology.next(h0);
    const HalfedgeId n1 = topology.next(h1);
    const HalfedgeId p1 = topology.prev(h1);
    const FaceId f0 = topology.face(h0);
    const FaceId f1 = topology.face(h1);

    // Each loop's two corners at a and b, in that order, captured before the
    // relink moves them: both loops blend one ordered pair with one set of
    // weights, so a continuous edge splits to bit-equal corners.
    const std::array<f32, 2> weights{1.0f - t, t};
    const std::array<HalfedgeId, 2> cornersA{h0, n0}; // f0: at a, at b
    const std::array<HalfedgeId, 2> cornersB{n1, h1}; // f1: at a, at b
    const SourcePolygon loopA = CaptureCorners(mesh, cornersA);
    const SourcePolygon loopB = CaptureCorners(mesh, cornersB);

    const u32 group = FreshMergeGroup(mesh);
    const VertexId v = allocVertex(mesh, static_cast<u32>(a.index()));
    {
        const std::array<u32, 2> ends{static_cast<u32>(a.index()), static_cast<u32>(b.index())};
        BlendVertex(mesh, ends, weights, static_cast<u32>(v.index()));
        const std::span<Vector3f> positions =
            mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        if (v.index() < positions.size() && a.index() < positions.size() &&
            b.index() < positions.size()) {
            const Vector3f& pa = positions[a.index()];
            const Vector3f& pb = positions[b.index()];
            positions[v.index()] = Vector3f{pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t,
                                            pa.z + (pb.z - pa.z) * t};
        }
        const std::span<u32> groups = mesh.attributes.get<u32>(names::kMergeGroup, Domain::Vertex);
        if (v.index() < groups.size()) {
            groups[v.index()] = group;
        }
    }

    const HalfedgeId g0 = allocEdge(mesh); // v -> b, joins h0's loop
    const HalfedgeId g1 = Topology::opposite(g0);
    // Both halves lie along the old edge, so both keep its `sharp`, `crease`
    // and `seam` (EDIT_MODE_MODELLING_DESIGN.md §3.12).
    mesh.attributes.copyElement(Domain::Edge, edge.value(), Topology::edge(g0).value());

    topology.setTo(h0, v); // h0: a -> v, so h1 is v -> a
    topology.setTo(g0, b); // g0: v -> b
    topology.setTo(g1, v); // g1: b -> v

    topology.setNext(g0, n0);
    topology.setNext(h0, g0);
    topology.setFace(g0, f0);

    topology.setNext(p1, g1);
    topology.setNext(g1, h1);
    topology.setFace(g1, f1);

    // h1's loop: g1 now leaves b, so it takes the corner h1 had there; the
    // corner at v (h1, now v -> a) is the blend. h0's loop: g0 leaves v.
    copyHalfedgeAttrs(mesh.attributes, static_cast<u32>(h1.index()), static_cast<u32>(g1.index()));
    const std::array<HalfedgeId, 1> atA{g0};
    BlendCorners(mesh, loopA, weights, atA);
    if (f1.valid()) {
        const std::array<HalfedgeId, 1> atB{h1};
        BlendCorners(mesh, loopB, weights, atB);
    }

    if (f0.valid()) {
        topology.setFaceHalfedge(f0, h0);
    }
    if (f1.valid()) {
        topology.setFaceHalfedge(f1, h1);
    }
    clearRow(mesh, f0);
    clearRow(mesh, f1);
    topology.setOutgoing(v, h1);
    if (topology.outgoing(b) == h1) {
        topology.setOutgoing(b, g1);
    }
    topology.adjustOutgoing(v);
    topology.adjustOutgoing(a);
    topology.adjustOutgoing(b);
    return v;
}

// ============================================================================
// SplitFace
// ============================================================================

FaceId SplitFaceAt(Mesh& mesh, HalfedgeId a, HalfedgeId b) {
    Topology& topology = mesh.topology();
    if (!a.valid() || !b.valid() || a == b) {
        return FaceId();
    }
    const FaceId face = topology.face(a);
    if (!face.valid() || topology.face(b) != face) {
        return FaceId();
    }
    if (topology.next(a) == b || topology.next(b) == a) {
        return FaceId(); // adjacent corners — the diagonal is an existing edge
    }

    const HalfedgeId pa = topology.prev(a);
    const HalfedgeId pb = topology.prev(b);
    const VertexId va = topology.from(a);
    const VertexId vb = topology.from(b);
    // Joined elsewhere already (a cap over two triangles, split along the
    // diagonal they share): a second a-b edge is a duplicate no rebuild takes.
    if (topology.findHalfedge(va, vb).valid()) {
        return FaceId();
    }

    const FaceId newFace = allocFace(mesh, static_cast<u32>(face.index()));
    const HalfedgeId g0 = allocEdge(mesh); // va -> vb, on the new face
    const HalfedgeId g1 = Topology::opposite(g0);
    topology.setTo(g0, vb);
    topology.setTo(g1, va);

    // Loop kept by `face`: a .. pb, then g1 (vb -> va).
    topology.setNext(pb, g1);
    topology.setNext(g1, a);
    topology.setFace(g1, face);
    topology.setFaceHalfedge(face, a);

    // Loop taken by `newFace`: b .. pa, then g0 (va -> vb).
    topology.setNext(pa, g0);
    topology.setNext(g0, b);
    topology.setFaceHalfedge(newFace, b);
    HalfedgeId walk = b;
    do {
        topology.setFace(walk, newFace);
        walk = topology.next(walk);
    } while (walk != b);

    copyHalfedgeAttrs(mesh.attributes, static_cast<u32>(a.index()), static_cast<u32>(g0.index()));
    copyHalfedgeAttrs(mesh.attributes, static_cast<u32>(b.index()), static_cast<u32>(g1.index()));
    clearRow(mesh, face);

    topology.setOutgoing(va, g0);
    topology.setOutgoing(vb, g1);
    topology.adjustOutgoing(va);
    topology.adjustOutgoing(vb);
    return newFace;
}

FaceId SplitFace(Mesh& mesh, FaceId face, u32 cornerA, u32 cornerB) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return FaceId();
    }
    const Topology& topology = mesh.topology();
    if (face.index() >= topology.faceCount() || topology.isDeleted(face)) {
        return FaceId();
    }
    const u32 valence = topology.valence(face);
    if (cornerA >= valence || cornerB >= valence) {
        return FaceId();
    }
    HalfedgeId a = topology.halfedge(face);
    for (u32 i = 0; i < cornerA; ++i) {
        a = topology.next(a);
    }
    HalfedgeId b = topology.halfedge(face);
    for (u32 i = 0; i < cornerB; ++i) {
        b = topology.next(b);
    }
    return SplitFaceAt(mesh, a, b);
}

// ============================================================================
// Triangulate
// ============================================================================

u32 Triangulate(Mesh& mesh, FaceId face) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return 0;
    }
    if (face.index() >= mesh.topology().faceCount() || mesh.topology().isDeleted(face)) {
        return 0;
    }
    const Mesh& readable = mesh;
    if (readable.topology().valence(face) < 4) {
        return 0;
    }
    // The face is cut along exactly the diagonals it is drawn with, so the
    // triangles it becomes are the ones it showed.
    const std::vector<u32> loop = loopOf(readable.topology(), face);
    const u32 valence = static_cast<u32>(loop.size());
    std::vector<u32> cut;
    TriangulateFace(loop, readable.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex),
                    readable.triangulation.row(face.value()), cut);
    std::vector<std::pair<u32, u32>> diagonals;
    for (std::size_t t = 0; t + 2 < cut.size(); t += 3) {
        for (u32 side = 0; side < 3; ++side) {
            const u32 a = cut[t + side];
            const u32 b = cut[t + (side + 1) % 3];
            if (a < b && b != a + 1 && !(a == 0 && b == valence - 1)) {
                diagonals.push_back({a, b});
            }
        }
    }
    // Each diagonal lies inside one piece: the one holding both its ends.
    std::vector<FaceId> pieces{face};
    u32 added = 0;
    for (const auto& [a, b] : diagonals) {
        HalfedgeId at;
        HalfedgeId to;
        for (std::size_t p = 0; p < pieces.size() && !to.valid(); ++p) {
            at = HalfedgeId();
            for (const HalfedgeId h : readable.topology().fh(pieces[p])) {
                if (readable.topology().from(h).value() == loop[a]) {
                    at = h;
                }
            }
            for (const HalfedgeId h : readable.topology().fh(pieces[p])) {
                if (at.valid() && readable.topology().from(h).value() == loop[b]) {
                    to = h;
                }
            }
        }
        const FaceId piece = to.valid() ? SplitFaceAt(mesh, at, to) : FaceId();
        if (piece.valid()) {
            pieces.push_back(piece);
            ++added;
        }
    }
    return added;
}

u32 TriangulateAll(Mesh& mesh) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return 0;
    }
    u32 added = 0;
    const u32 faceCount = mesh.topology().faceCount();
    for (u32 f = 0; f < faceCount; ++f) {
        if (!mesh.topology().isDeleted(FaceId(f))) {
            added += Triangulate(mesh, FaceId(f));
        }
    }
    return added;
}

// ============================================================================
// FlipEdge
// ============================================================================

bool FlipEdge(Mesh& mesh, EdgeId edge) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return false;
    }
    Topology& topology = mesh.topology();
    if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge) ||
        topology.isBoundary(edge)) {
        return false;
    }
    const HalfedgeId h0 = Topology::halfedge(edge, 0);
    const HalfedgeId h1 = Topology::halfedge(edge, 1);
    const FaceId f0 = topology.face(h0);
    const FaceId f1 = topology.face(h1);
    if (topology.valence(f0) != 3 || topology.valence(f1) != 3) {
        return false;
    }

    const HalfedgeId n0 = topology.next(h0);
    const HalfedgeId nn0 = topology.next(n0);
    const HalfedgeId n1 = topology.next(h1);
    const HalfedgeId nn1 = topology.next(n1);
    const VertexId a = topology.from(h0);
    const VertexId b = topology.to(h0);
    const VertexId c = topology.to(n0);
    const VertexId d = topology.to(n1);
    if (c == d || topology.findHalfedge(c, d).valid()) {
        return false; // the new diagonal already exists
    }

    topology.setTo(h0, d); // h0: c -> d
    topology.setTo(h1, c); // h1: d -> c

    topology.setNext(h0, nn1);
    topology.setNext(nn1, n0);
    topology.setNext(n0, h0);
    topology.setFace(h0, f0);
    topology.setFace(nn1, f0);
    topology.setFace(n0, f0);
    topology.setFaceHalfedge(f0, h0);

    topology.setNext(h1, nn0);
    topology.setNext(nn0, n1);
    topology.setNext(n1, h1);
    topology.setFace(h1, f1);
    topology.setFace(nn0, f1);
    topology.setFace(n1, f1);
    topology.setFaceHalfedge(f1, h1);

    topology.setOutgoing(a, n1);
    topology.setOutgoing(b, n0);
    topology.setOutgoing(c, h0);
    topology.setOutgoing(d, h1);
    topology.adjustOutgoing(a);
    topology.adjustOutgoing(b);
    topology.adjustOutgoing(c);
    topology.adjustOutgoing(d);
    return true;
}

// ============================================================================
// CollapseEdge
// ============================================================================

namespace {

/// The loop `h` belongs to, as a halfedge count. Boundary loops count too.
u32 loopLength(const Topology& topology, HalfedgeId h) {
    u32 count = 0;
    HalfedgeId walk = h;
    do {
        ++count;
        walk = topology.next(walk);
        if (count > 1u << 24) {
            return count; // corrupt; caller refuses
        }
    } while (walk != h);
    return count;
}

bool sideIsCollapsible(const Topology& topology, HalfedgeId h) {
    const u32 length = loopLength(topology, h);
    if (length < 3) {
        return false;
    }
    if (length == 3) {
        // A triangular *face* is spliced away; a boundary loop of three would be
        // left with two halfedges between one vertex pair, which is degenerate.
        return topology.face(h).valid();
    }
    return true;
}

} // namespace

bool IsCollapseLegal(const Mesh& mesh, HalfedgeId h) {
    if (!mesh.hasConnectivity()) {
        return false;
    }
    const Topology& topology = mesh.topology();
    if (!h.valid() || h.index() >= topology.halfedgeCount() || topology.isDeleted(h)) {
        return false;
    }
    const HalfedgeId o = Topology::opposite(h);
    const VertexId v0 = topology.from(h);
    const VertexId v1 = topology.to(h);
    if (v0 == v1) {
        return false;
    }
    if (!sideIsCollapsible(topology, h) || !sideIsCollapsible(topology, o)) {
        return false;
    }
    // A boundary vertex may only collapse along the boundary; otherwise the
    // collapse pinches two boundary loops together.
    if (topology.isBoundary(v0) && topology.isBoundary(v1) &&
        !topology.isBoundary(Topology::edge(h))) {
        return false;
    }

    // The link condition for polygons: the one-rings of v0 and v1 meet exactly
    // at the apexes of the TRIANGULAR side faces. A quad or an n-gon beside the
    // edge has no apex -- it just loses a corner -- so counting one per side
    // face refused every collapse next to a quad.
    std::vector<VertexId> apexes;
    for (const HalfedgeId side : {h, o}) {
        if (topology.face(side).valid() && loopLength(topology, side) == 3) {
            apexes.push_back(topology.to(topology.next(side)));
        }
    }
    std::vector<VertexId> ring0;
    for (VertexId n : topology.vv(v0)) {
        ring0.push_back(n);
    }
    std::vector<VertexId> shared;
    for (VertexId n : topology.vv(v1)) {
        if (std::find(ring0.begin(), ring0.end(), n) != ring0.end() &&
            std::find(shared.begin(), shared.end(), n) == shared.end()) {
            shared.push_back(n);
        }
    }
    std::sort(apexes.begin(), apexes.end());
    std::sort(shared.begin(), shared.end());
    if (shared != apexes) {
        return false;
    }
    // No face but the two side faces may hold both ends: a hexagon
    // (v0, p, r, v1, s, t) passes the ring test and would repeat a vertex.
    const FaceId sideA = topology.face(h);
    const FaceId sideB = topology.face(o);
    for (const FaceId f : topology.vf(v0)) {
        if (!f.valid() || f == sideA || f == sideB) {
            continue;
        }
        for (const VertexId v : topology.fv(f)) {
            if (v == v1) {
                return false;
            }
        }
    }
    return true;
}

bool CollapseEdge(Mesh& mesh, HalfedgeId h) {
    if (!IsCollapseLegal(mesh, h)) {
        return false;
    }
    {
        // Rows first, while the loops are whole: a side face longer than a
        // triangle loses a corner; every other face around v0 only renames it.
        const Topology& before = std::as_const(mesh).topology();
        const HalfedgeId other = Topology::opposite(h);
        const std::array<u32, 1> from{before.from(h).value()};
        const std::array<u32, 1> to{before.to(h).value()};
        std::vector<FaceId> around;
        for (const FaceId f : before.vf(before.from(h))) {
            around.push_back(f);
        }
        for (const FaceId f : around) {
            if (!f.valid()) {
                continue;
            }
            if (f == before.face(h) || f == before.face(other)) {
                clearRow(mesh, f);
            } else {
                mesh.triangulation.rewriteRow(f.value(), from, to);
            }
        }
    }
    Topology& topology = mesh.topology();
    const HalfedgeId o = Topology::opposite(h);
    const VertexId v0 = topology.from(h);
    const VertexId v1 = topology.to(h);

    // Everything that pointed at v0 points at v1 from here on.
    std::vector<HalfedgeId> incoming;
    for (HalfedgeId oh : topology.voh(v0)) {
        incoming.push_back(Topology::opposite(oh));
    }
    // v1's own fan, before the splice: where a live outgoing is found when the
    // one it had goes (below).
    std::vector<HalfedgeId> leaving;
    for (HalfedgeId oh : topology.voh(v1)) {
        leaving.push_back(oh);
    }
    for (HalfedgeId ih : incoming) {
        topology.setTo(ih, v1);
    }

    // Each side is either spliced (a triangle collapsing to one edge) or simply
    // unlinked (a longer loop that just loses a corner).
    const auto handleSide = [&](HalfedgeId side) {
        const HalfedgeId sideNext = topology.next(side);
        const HalfedgeId sidePrev = topology.prev(side);
        const FaceId face = topology.face(side);
        if (face.valid() && loopLength(topology, side) == 3) {
            const HalfedgeId keep = sideNext;                    // v1 -> w
            const HalfedgeId drop = topology.next(sideNext);     // w  -> v1
            const HalfedgeId dropOpp = Topology::opposite(drop); // v1 -> w, other side
            const VertexId w = topology.to(keep);

            // `keep` takes over `dropOpp`'s place, so the two surviving
            // neighbours meet across one edge — attributes ride along, because
            // `keep` now occupies that loop's corner.
            const HalfedgeId dropOppPrev = topology.prev(dropOpp);
            const HalfedgeId dropOppNext = topology.next(dropOpp);
            const FaceId dropOppFace = topology.face(dropOpp);
            copyHalfedgeAttrs(mesh.attributes, static_cast<u32>(dropOpp.index()),
                              static_cast<u32>(keep.index()));
            topology.setNext(dropOppPrev, keep);
            topology.setNext(keep, dropOppNext);
            topology.setFace(keep, dropOppFace);
            if (dropOppFace.valid()) {
                topology.setFaceHalfedge(dropOppFace, keep);
            }
            topology.setTo(keep, w);

            topology.setStatus(face, topology.status(face) | Status::Deleted);
            topology.setStatus(Topology::edge(drop),
                               topology.status(Topology::edge(drop)) | Status::Deleted);
            topology.setStatus(drop, topology.status(drop) | Status::Deleted);
            topology.setStatus(dropOpp, topology.status(dropOpp) | Status::Deleted);

            if (topology.outgoing(w) == drop || topology.outgoing(w) == dropOpp) {
                topology.setOutgoing(w, Topology::opposite(keep));
            }
            if (topology.outgoing(v1) == dropOpp || topology.outgoing(v1) == drop) {
                topology.setOutgoing(v1, keep);
            }
            leaving.push_back(keep);
        } else {
            topology.setNext(sidePrev, sideNext);
            if (face.valid() && topology.halfedge(face) == side) {
                topology.setFaceHalfedge(face, sideNext);
            }
        }
    };
    handleSide(h);
    handleSide(o);

    // Any surviving halfedge leaving v1 will do; `adjustOutgoing` then prefers
    // a boundary one. The candidates are v0's fan and v1's own, and each
    // spliced triangle's surviving side: when v0 had valence 2 and its triangle
    // was spliced away, everything v0 brought is deleted, and only `keep` is
    // left to point at.
    const auto live = [&](HalfedgeId candidate) {
        return candidate.valid() && !topology.isDeleted(candidate) && candidate != h &&
               candidate != o && topology.from(candidate) == v1;
    };
    if (!live(topology.outgoing(v1))) {
        HalfedgeId replacement;
        for (HalfedgeId ih : incoming) {
            if (live(Topology::opposite(ih))) {
                replacement = Topology::opposite(ih);
                break;
            }
        }
        for (std::size_t i = 0; !replacement.valid() && i < leaving.size(); ++i) {
            if (live(leaving[i])) {
                replacement = leaving[i];
            }
        }
        topology.setOutgoing(v1, replacement);
    }
    topology.setStatus(Topology::edge(h), topology.status(Topology::edge(h)) | Status::Deleted);
    topology.setStatus(h, topology.status(h) | Status::Deleted);
    topology.setStatus(o, topology.status(o) | Status::Deleted);
    topology.setStatus(v0, topology.status(v0) | Status::Deleted);
    if (topology.outgoing(v1).valid()) {
        topology.adjustOutgoing(v1);
    }
    return true;
}

// ============================================================================
// Dissolve
// ============================================================================

detail::Dissolved detail::DissolveEdgeDeferred(Mesh& mesh, EdgeId edge, FaceId survivor) {
    Dissolved out;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return out;
    }
    {
        // Every refusal asked of a const view, so nothing is marked edited.
        const Topology& topology = std::as_const(mesh).topology();
        if (edge.index() >= topology.edgeCount() || topology.isDeleted(edge) ||
            topology.isBoundary(edge)) {
            return out;
        }
        const FaceId fa = topology.face(Topology::halfedge(edge, 0));
        const FaceId fb = topology.face(Topology::halfedge(edge, 1));
        if (fa == fb || (survivor.valid() && survivor != fa && survivor != fb)) {
            return out;
        }
        const std::span<const u32> sections = std::as_const(mesh).faceSections();
        if (fa.index() < sections.size() && fb.index() < sections.size() &&
            sections[fa.index()] != sections[fb.index()]) {
            return out;
        }
        // The merged loop is both loops less the edge: it repeats a vertex
        // whenever the two faces share one besides the edge's ends.
        std::vector<VertexId> merged;
        for (const VertexId v : topology.fv(fa)) {
            merged.push_back(v);
        }
        u32 shared = 0;
        for (const VertexId v : topology.fv(fb)) {
            if (std::find(merged.begin(), merged.end(), v) != merged.end()) {
                ++shared;
            } else {
                merged.push_back(v);
            }
        }
        if (shared != 2) {
            return out;
        }
        // A face with these corners already there would be its duplicate.
        std::sort(merged.begin(), merged.end());
        const VertexId anchor = topology.from(Topology::halfedge(edge, 0));
        for (const FaceId g : topology.vf(anchor)) {
            if (!g.valid() || g == fa || g == fb || topology.valence(g) != merged.size()) {
                continue;
            }
            std::vector<VertexId> corners;
            for (const VertexId v : topology.fv(g)) {
                corners.push_back(v);
            }
            std::sort(corners.begin(), corners.end());
            if (corners == merged) {
                return out;
            }
        }
    }

    // The merged face is drawn as both faces were, so the file's diagonal is
    // kept: the union of their triangles, lower-numbered face first (§2.3).
    std::vector<u32> joined;
    {
        const Topology& before = std::as_const(mesh).topology();
        FaceId first = before.face(Topology::halfedge(edge, 0));
        FaceId second = before.face(Topology::halfedge(edge, 1));
        if (second.value() < first.value()) {
            std::swap(first, second);
        }
        joined = drawnTriangles(mesh, first);
        const std::vector<u32> rest = drawnTriangles(mesh, second);
        joined.insert(joined.end(), rest.begin(), rest.end());
    }

    Topology& topology = mesh.topology();
    // The survivor's side is `h0`: its loop is the one kept.
    const u32 keepSide = survivor.valid() && topology.face(Topology::halfedge(edge, 1)) == survivor
                             ? 1u
                             : 0u;
    const HalfedgeId h0 = Topology::halfedge(edge, keepSide);
    const HalfedgeId h1 = Topology::halfedge(edge, keepSide ^ 1u);
    const FaceId f0 = topology.face(h0);
    const FaceId f1 = topology.face(h1);
    const HalfedgeId p0 = topology.prev(h0);
    const HalfedgeId n0 = topology.next(h0);
    const HalfedgeId p1 = topology.prev(h1);
    const HalfedgeId n1 = topology.next(h1);
    const VertexId a = topology.from(h0);
    const VertexId b = topology.from(h1);

    topology.setNext(p0, n1);
    topology.setNext(p1, n0);
    HalfedgeId walk = n0;
    do {
        topology.setFace(walk, f0);
        walk = topology.next(walk);
    } while (walk != n0);
    topology.setFaceHalfedge(f0, n0);

    topology.setStatus(f1, topology.status(f1) | Status::Deleted);
    topology.setStatus(edge, topology.status(edge) | Status::Deleted);
    topology.setStatus(h0, topology.status(h0) | Status::Deleted);
    topology.setStatus(h1, topology.status(h1) | Status::Deleted);
    out.ok = true;
    out.kept = f0;
    out.gone = f1;
    out.joined = std::move(joined);

    if (topology.outgoing(a) == h0) {
        topology.setOutgoing(a, n1);
    }
    if (topology.outgoing(b) == h1) {
        topology.setOutgoing(b, n0);
    }
    topology.adjustOutgoing(a);
    topology.adjustOutgoing(b);
    return out;
}

bool DissolveEdge(Mesh& mesh, EdgeId edge, FaceId survivor) {
    detail::Dissolved dissolved = detail::DissolveEdgeDeferred(mesh, edge, survivor);
    if (!dissolved.ok) {
        return false;
    }
    clearRow(mesh, dissolved.gone);
    mesh.triangulation.setRow(dissolved.kept.value(), dissolved.joined,
                              std::as_const(mesh).topology().faceCount());
    return true;
}

bool DissolveVertex(Mesh& mesh, VertexId vertex) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return false;
    }
    Topology& topology = mesh.topology();
    if (vertex.index() >= topology.vertexCount() || topology.isDeleted(vertex)) {
        return false;
    }
    if (topology.valence(vertex) != 2) {
        return false;
    }
    const HalfedgeId ha = topology.outgoing(vertex); // v -> a
    const HalfedgeId oa = Topology::opposite(ha);    // a -> v
    const HalfedgeId hb = topology.next(oa);         // v -> b
    const HalfedgeId ob = Topology::opposite(hb);    // b -> v
    if (hb == ha) {
        return false;
    }
    const VertexId b = topology.to(hb);
    // A face beside it losing its third corner, or an a-b edge already there:
    // either leaves a degenerate face or a duplicate edge behind.
    for (const HalfedgeId side : {oa, ha}) {
        const FaceId face = topology.face(side);
        if (face.valid() && topology.valence(face) <= 3) {
            return false;
        }
    }
    if (topology.findHalfedge(topology.to(ha), b).valid()) {
        return false;
    }

    // Re-purpose ha's edge as a <-> b: `oa` becomes a -> b, and `ha`, being its
    // opposite, is b -> a without touching its `to`.
    const HalfedgeId obPrev = topology.prev(ob);
    const HalfedgeId hbNext = topology.next(hb);
    topology.setTo(oa, b);
    topology.setNext(obPrev, ha);
    topology.setNext(oa, hbNext);

    const FaceId faceA = topology.face(oa);
    const FaceId faceB = topology.face(ha);
    if (faceA.valid() && topology.halfedge(faceA) == hb) {
        topology.setFaceHalfedge(faceA, oa);
    }
    if (faceB.valid() && topology.halfedge(faceB) == ob) {
        topology.setFaceHalfedge(faceB, ha);
    }

    // The merged edge runs along both old ones: it keeps either's flag and the
    // larger crease, as a weld's closed edge does.
    combineEdgeInto(mesh.attributes, Topology::edge(hb).value(), Topology::edge(ha).value());
    clearRow(mesh, faceA);
    clearRow(mesh, faceB);

    topology.setStatus(Topology::edge(hb), topology.status(Topology::edge(hb)) | Status::Deleted);
    topology.setStatus(hb, topology.status(hb) | Status::Deleted);
    topology.setStatus(ob, topology.status(ob) | Status::Deleted);
    topology.setStatus(vertex, topology.status(vertex) | Status::Deleted);

    topology.setOutgoing(b, ha);
    topology.setOutgoing(topology.from(oa), oa);
    topology.adjustOutgoing(b);
    topology.adjustOutgoing(topology.from(oa));
    return true;
}

// ============================================================================
// WeldVertices
// ============================================================================

WeldResult WeldVertices(Mesh& mesh, f32 epsilon, bool respectMergeGroups) {
    WeldResult result;
    // Connectivity where the face set builds, so the corners and edges can be
    // carried; compacted, so a face-set index is a slot and no Face layer is
    // misread (§2.1). A face set that does not build is welded as it stands.
    if (mesh.hasConnectivity() || mesh.ensureConnectivity().ok()) {
        GarbageCollect(mesh);
    }
    const std::vector<u32> snapshot = snapshotCorners(mesh);
    const FaceSet& faces = mesh.faceSet();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<const u32> groups =
        mesh.attributes.get<const u32>(names::kMergeGroup, Domain::Vertex);
    if (positions.empty()) {
        return result;
    }

    // Quantise to an epsilon lattice, then confirm with a real distance test
    // against the bucket's representative, so the result does not depend on
    // which of two nearly equal points was seen first.
    const f32 cell = epsilon > 0.0f ? epsilon : 1e-6f;
    struct Key {
        i64 x, y, z;
        u32 group;
        bool operator==(const Key& other) const {
            return x == other.x && y == other.y && z == other.z && group == other.group;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            u64 hash = 1469598103934665603ull;
            const auto mix = [&hash](u64 value) {
                hash ^= value;
                hash *= 1099511628211ull;
            };
            mix(static_cast<u64>(k.x));
            mix(static_cast<u64>(k.y));
            mix(static_cast<u64>(k.z));
            mix(k.group);
            return static_cast<std::size_t>(hash);
        }
    };
    std::unordered_map<Key, std::vector<u32>, KeyHash> buckets; // looked up, never iterated

    std::vector<u32> newIndex(positions.size(), kInvalidId);
    std::vector<u32> vertexSource;
    const f32 epsilonSq = epsilon * epsilon;
    for (u32 v = 0; v < positions.size(); ++v) {
        const Vector3f& p = positions[v];
        const u32 group = respectMergeGroups ? (v < groups.size() ? groups[v] : v) : 0u;
        u32 match = kInvalidId;
        for (i64 dx = -1; dx <= 1 && match == kInvalidId; ++dx) {
            for (i64 dy = -1; dy <= 1 && match == kInvalidId; ++dy) {
                for (i64 dz = -1; dz <= 1 && match == kInvalidId; ++dz) {
                    const Key key{static_cast<i64>(std::floor(p.x / cell)) + dx,
                                  static_cast<i64>(std::floor(p.y / cell)) + dy,
                                  static_cast<i64>(std::floor(p.z / cell)) + dz, group};
                    const auto bucket = buckets.find(key);
                    if (bucket == buckets.end()) {
                        continue;
                    }
                    for (u32 candidate : bucket->second) {
                        const Vector3f& q = positions[candidate];
                        const f32 ddx = p.x - q.x;
                        const f32 ddy = p.y - q.y;
                        const f32 ddz = p.z - q.z;
                        if (ddx * ddx + ddy * ddy + ddz * ddz <= epsilonSq) {
                            match = candidate;
                            break;
                        }
                    }
                }
            }
        }
        if (match != kInvalidId) {
            newIndex[v] = newIndex[match];
            ++result.verticesMerged;
            continue;
        }
        const Key key{static_cast<i64>(std::floor(p.x / cell)),
                      static_cast<i64>(std::floor(p.y / cell)),
                      static_cast<i64>(std::floor(p.z / cell)), group};
        buckets[key].push_back(v);
        newIndex[v] = static_cast<u32>(vertexSource.size());
        vertexSource.push_back(v);
    }
    if (result.verticesMerged == 0) {
        return result;
    }

    RebuildMapping mapping;
    mapping.vertexSource = std::move(vertexSource);
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        std::vector<u32> rewritten;
        rewritten.reserve(valence);
        bool degenerate = false;
        for (u32 i = 0; i < valence; ++i) {
            const u32 mapped = newIndex[faces.cornerVertex[corner + i]];
            if (std::find(rewritten.begin(), rewritten.end(), mapped) != rewritten.end()) {
                degenerate = true;
                break;
            }
            rewritten.push_back(mapped);
        }
        if (degenerate || rewritten.size() < 3) {
            ++result.facesDropped;
            corner += valence;
            continue;
        }
        mapping.faceSource.push_back(static_cast<u32>(f));
        mapping.faces.faceValence.push_back(valence);
        for (u32 i = 0; i < valence; ++i) {
            mapping.faces.cornerVertex.push_back(rewritten[i]);
            mapping.cornerSource.push_back(static_cast<u32>(corner + i));
        }
        corner += valence;
    }

    rebuild(mesh, std::move(mapping), snapshot);
    return result;
}

// ============================================================================
// SplitVertexByHalfedgeAttr
// ============================================================================

u32 SplitVertexByHalfedgeAttr(Mesh& mesh, std::span<const std::string> layers) {
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        return 0;
    }
    GarbageCollect(mesh);
    const std::vector<u32> snapshot = snapshotCorners(mesh);
    const FaceSet& faces = mesh.faceSet();

    std::vector<const AttrLayer*> resolved;
    for (const std::string& name : layers) {
        if (const AttrLayer* layer = mesh.attributes.layer(name, Domain::Halfedge)) {
            resolved.push_back(layer);
        }
    }
    if (resolved.empty()) {
        return 0;
    }

    // One new vertex per (vertex, attribute tuple) — the same grouping the render
    // view does, just materialised into the document.
    struct KeyHash {
        std::size_t operator()(const std::string& s) const {
            return std::hash<std::string>()(s);
        }
    };
    std::unordered_map<std::string, u32, KeyHash> seen; // looked up, never iterated
    std::vector<u32> vertexSource;
    RebuildMapping mapping;
    mapping.faces.faceValence = faces.faceValence;
    mapping.faceSource.resize(faces.faceCount());
    for (std::size_t i = 0; i < mapping.faceSource.size(); ++i) {
        mapping.faceSource[i] = static_cast<u32>(i);
    }
    mapping.cornerSource.resize(faces.cornerVertex.size());
    for (std::size_t i = 0; i < mapping.cornerSource.size(); ++i) {
        mapping.cornerSource[i] = static_cast<u32>(i);
    }
    mapping.faces.cornerVertex.resize(faces.cornerVertex.size());

    // Pass one assigns provisional ids in corner order; pass two renumbers them
    // source-vertex-major, so a vertex's copies sit together and right where the
    // original was. Corner order alone is just as deterministic but permutes the
    // whole array for the sake of one seam.
    std::vector<u32> provisional(faces.cornerVertex.size(), kInvalidId);
    std::string key;
    for (std::size_t c = 0; c < faces.cornerVertex.size(); ++c) {
        const u32 vertex = faces.cornerVertex[c];
        const u32 halfedge = c < snapshot.size() ? snapshot[c] : 0;
        key.clear();
        key.append(reinterpret_cast<const char*>(&vertex), sizeof(vertex));
        for (const AttrLayer* layer : resolved) {
            const std::size_t stride = AttrTypeSize(layer->type);
            const std::size_t offset = stride * halfedge;
            if (offset + stride <= layer->data.size()) {
                key.append(reinterpret_cast<const char*>(layer->data.data() + offset), stride);
            } else {
                key.append(stride, '\0');
            }
        }
        const auto found = seen.find(key);
        if (found != seen.end()) {
            provisional[c] = found->second;
            continue;
        }
        const u32 index = static_cast<u32>(vertexSource.size());
        vertexSource.push_back(vertex);
        seen.emplace(key, index);
        provisional[c] = index;
    }

    if (vertexSource.size() == faces.vertexCount) {
        return 0;
    }

    std::vector<u32> order(vertexSource.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<u32>(i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](u32 lhs, u32 rhs) { return vertexSource[lhs] < vertexSource[rhs]; });
    std::vector<u32> finalOf(vertexSource.size(), kInvalidId);
    mapping.vertexSource.resize(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        finalOf[order[i]] = static_cast<u32>(i);
        mapping.vertexSource[i] = vertexSource[order[i]];
    }
    for (std::size_t c = 0; c < provisional.size(); ++c) {
        mapping.faces.cornerVertex[c] = finalOf[provisional[c]];
    }

    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    const u32 before = mesh.vertexCount();
    rebuild(mesh, std::move(mapping), snapshot);
    // Not `vertexSource.size() - faces.vertexCount`: separating a vertex's fans
    // can leave two faces meeting at a single vertex, and the rebuild's repair
    // then splits that bowtie too. Report what actually happened.
    return mesh.vertexCount() - before;
}

// ============================================================================
// UnifyWinding
// ============================================================================

u32 UnifyWinding(Mesh& mesh) {
    // As WeldVertices: a face set wound against itself does not build, and is
    // exactly what this op is for.
    if (mesh.hasConnectivity() || mesh.ensureConnectivity().ok()) {
        GarbageCollect(mesh);
    }
    const std::vector<u32> snapshot = snapshotCorners(mesh);
    const FaceSet& faces = mesh.faceSet();
    const std::size_t faceCount = faces.faceCount();
    if (faceCount == 0) {
        return 0;
    }

    std::vector<u32> base(faceCount + 1, 0);
    for (std::size_t f = 0; f < faceCount; ++f) {
        base[f + 1] = base[f] + faces.faceValence[f];
    }

    // Directed corner pairs, so two faces agree exactly when they traverse a
    // shared edge in opposite directions.
    std::unordered_map<u64, std::vector<u32>> byEdge; // looked up, never iterated
    const auto undirected = [](u32 a, u32 b) {
        const u64 lo = std::min(a, b);
        const u64 hi = std::max(a, b);
        return (lo << 32) | hi;
    };
    for (std::size_t f = 0; f < faceCount; ++f) {
        const u32 valence = faces.faceValence[f];
        for (u32 i = 0; i < valence; ++i) {
            const u32 a = faces.cornerVertex[base[f] + i];
            const u32 b = faces.cornerVertex[base[f] + (i + 1) % valence];
            byEdge[undirected(a, b)].push_back(static_cast<u32>(f));
        }
    }

    std::vector<i8> orientation(faceCount, 0); // 0 unvisited, 1 keep, -1 flip
    std::vector<u32> stack;
    u32 flipped = 0;
    const auto traversesForward = [&](std::size_t f, u32 a, u32 b) {
        const u32 valence = faces.faceValence[f];
        for (u32 i = 0; i < valence; ++i) {
            const u32 x = faces.cornerVertex[base[f] + i];
            const u32 y = faces.cornerVertex[base[f] + (i + 1) % valence];
            if (x == a && y == b) {
                return true;
            }
        }
        return false;
    };
    for (std::size_t seed = 0; seed < faceCount; ++seed) {
        if (orientation[seed] != 0) {
            continue;
        }
        orientation[seed] = 1;
        stack.push_back(static_cast<u32>(seed));
        while (!stack.empty()) {
            const u32 f = stack.back();
            stack.pop_back();
            const u32 valence = faces.faceValence[f];
            for (u32 i = 0; i < valence; ++i) {
                const u32 a = faces.cornerVertex[base[f] + i];
                const u32 b = faces.cornerVertex[base[f] + (i + 1) % valence];
                const auto bucket = byEdge.find(undirected(a, b));
                if (bucket == byEdge.end()) {
                    continue;
                }
                for (u32 other : bucket->second) {
                    if (other == f || orientation[other] != 0) {
                        continue;
                    }
                    // `f` traverses a->b in its *effective* winding; a neighbour
                    // agrees when it traverses b->a in its own.
                    const bool fReversed = orientation[f] < 0;
                    const bool otherAgrees =
                        traversesForward(other, fReversed ? a : b, fReversed ? b : a);
                    orientation[other] = otherAgrees ? static_cast<i8>(1) : static_cast<i8>(-1);
                    if (!otherAgrees) {
                        ++flipped;
                    }
                    stack.push_back(other);
                }
            }
        }
    }
    if (flipped == 0) {
        return 0;
    }

    RebuildMapping mapping = identityMapping(mesh);
    for (std::size_t f = 0; f < faceCount; ++f) {
        if (orientation[f] >= 0) {
            continue;
        }
        const u32 valence = faces.faceValence[f];
        for (u32 i = 0; i < valence / 2; ++i) {
            std::swap(mapping.faces.cornerVertex[base[f] + i],
                      mapping.faces.cornerVertex[base[f] + valence - 1 - i]);
            std::swap(mapping.cornerSource[base[f] + i],
                      mapping.cornerSource[base[f] + valence - 1 - i]);
        }
    }
    rebuild(mesh, std::move(mapping), snapshot);
    return flipped;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
