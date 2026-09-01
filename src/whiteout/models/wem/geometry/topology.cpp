// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/topology.h>

#include <algorithm>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

const char* ToString(BuildError error) {
    switch (error) {
    case BuildError::None:
        return "none";
    case BuildError::NonManifoldEdge:
        return "non_manifold_edge";
    case BuildError::NonManifoldVertex:
        return "non_manifold_vertex";
    case BuildError::InconsistentWinding:
        return "inconsistent_winding";
    case BuildError::DegenerateFace:
        return "degenerate_face";
    case BuildError::VertexOutOfRange:
        return "vertex_out_of_range";
    }
    return "invalid";
}

// ============================================================================
// FaceSet
// ============================================================================

void FaceSet::addTriangle(u32 a, u32 b, u32 c) {
    faceValence.push_back(3);
    cornerVertex.push_back(a);
    cornerVertex.push_back(b);
    cornerVertex.push_back(c);
}

void FaceSet::addFace(std::span<const u32> corners) {
    faceValence.push_back(static_cast<u32>(corners.size()));
    cornerVertex.insert(cornerVertex.end(), corners.begin(), corners.end());
}

void FaceSet::clear() {
    vertexCount = 0;
    faceValence.clear();
    cornerVertex.clear();
}

// ============================================================================
// Build
// ============================================================================

namespace {

/// One directed edge of one face corner, as the sort sees it.
struct CornerKey {
    u32 low;     ///< min(from, to)
    u32 high;    ///< max(from, to)
    u8 reversed; ///< 1 when the face traverses high -> low
    u32 corner;  ///< index into the flattened corner array
};

/// Total order: key, then direction, then corner index. Including the corner
/// index makes the order total even for the malformed inputs we are about to
/// reject, which is what keeps the *error* deterministic too.
bool cornerLess(const CornerKey& a, const CornerKey& b) {
    if (a.low != b.low) {
        return a.low < b.low;
    }
    if (a.high != b.high) {
        return a.high < b.high;
    }
    if (a.reversed != b.reversed) {
        return a.reversed < b.reversed;
    }
    return a.corner < b.corner;
}

} // namespace

BuildResult Topology::build(const FaceSet& faces) {
    clear();

    const std::size_t faceCountIn = faces.faceCount();
    std::size_t cornerTotal = 0;
    for (u32 valence : faces.faceValence) {
        cornerTotal += valence;
    }
    if (cornerTotal != faces.cornerVertex.size()) {
        return BuildResult{BuildError::DegenerateFace, kInvalidId, kInvalidId};
    }

    // --- pass 1: validate corners and collect keys --------------------------

    std::vector<CornerKey> keys;
    keys.reserve(cornerTotal);
    std::vector<u32> faceCornerBase(faceCountIn, 0);

    std::size_t cursor = 0;
    for (std::size_t f = 0; f < faceCountIn; ++f) {
        const u32 valence = faces.faceValence[f];
        faceCornerBase[f] = static_cast<u32>(cursor);
        if (valence < 3) {
            return BuildResult{BuildError::DegenerateFace, static_cast<u32>(f), kInvalidId};
        }
        for (u32 i = 0; i < valence; ++i) {
            const u32 a = faces.cornerVertex[cursor + i];
            const u32 b = faces.cornerVertex[cursor + ((i + 1) % valence)];
            if (a >= faces.vertexCount || b >= faces.vertexCount) {
                return BuildResult{BuildError::VertexOutOfRange, static_cast<u32>(f),
                                   a >= faces.vertexCount ? a : b};
            }
            if (a == b) {
                return BuildResult{BuildError::DegenerateFace, static_cast<u32>(f), a};
            }
            CornerKey key;
            key.low = a < b ? a : b;
            key.high = a < b ? b : a;
            key.reversed = a < b ? 0u : 1u;
            key.corner = static_cast<u32>(cursor + i);
            keys.push_back(key);
        }
        cursor += valence;
    }

    std::sort(keys.begin(), keys.end(), cornerLess);

    // --- pass 2: allocate one edge per distinct key -------------------------
    //
    // Edge numbering follows the sorted key order, which is what makes the whole
    // build deterministic: the same face set produces the same edge and halfedge
    // handles everywhere.

    std::vector<u32> cornerHalfedge(cornerTotal, kInvalidId);
    u32 edgeTotal = 0;

    for (std::size_t i = 0; i < keys.size();) {
        std::size_t groupEnd = i + 1;
        while (groupEnd < keys.size() && keys[groupEnd].low == keys[i].low &&
               keys[groupEnd].high == keys[i].high) {
            ++groupEnd;
        }
        const std::size_t groupSize = groupEnd - i;
        if (groupSize > 2) {
            return BuildResult{BuildError::NonManifoldEdge, kInvalidId, keys[i].low};
        }
        if (groupSize == 2 && keys[i].reversed == keys[i + 1].reversed) {
            // Two faces traverse the edge the same way: one of them is wound
            // against the other. Repair splits rather than flips (§5.3), because
            // flipping changes the normal and the source authored it that way.
            return BuildResult{BuildError::InconsistentWinding, kInvalidId, keys[i].low};
        }

        const u32 e = edgeTotal++;
        for (std::size_t k = i; k < groupEnd; ++k) {
            // Halfedge 2e runs low -> high, 2e + 1 runs high -> low. The pair is
            // what makes `opposite(h) == h ^ 1` true by construction (C1).
            cornerHalfedge[keys[k].corner] = (e << 1u) | keys[k].reversed;
        }
        i = groupEnd;
    }

    // --- pass 3: fill the arrays --------------------------------------------

    const std::size_t halfedgeTotal = static_cast<std::size_t>(edgeTotal) * 2;
    vertexOutgoing_.assign(faces.vertexCount, HalfedgeId());
    halfedgeTo_.assign(halfedgeTotal, VertexId());
    halfedgeFace_.assign(halfedgeTotal, FaceId());
    halfedgeNext_.assign(halfedgeTotal, HalfedgeId());
    halfedgePrev_.assign(halfedgeTotal, HalfedgeId());
    faceHalfedge_.assign(faceCountIn, HalfedgeId());
    vertexStatus_.assign(faces.vertexCount, Status::None);
    halfedgeStatus_.assign(halfedgeTotal, Status::None);
    edgeStatus_.assign(edgeTotal, Status::None);
    faceStatus_.assign(faceCountIn, Status::None);

    // `to` is known from the key alone: the even halfedge of edge e points at the
    // higher-numbered endpoint.
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const u32 h = cornerHalfedge[keys[k].corner];
        halfedgeTo_[h] = VertexId((h & 1u) == 0 ? keys[k].high : keys[k].low);
        halfedgeTo_[h ^ 1u] = VertexId((h & 1u) == 0 ? keys[k].low : keys[k].high);
    }

    for (std::size_t f = 0; f < faceCountIn; ++f) {
        const u32 valence = faces.faceValence[f];
        const u32 base = faceCornerBase[f];
        faceHalfedge_[f] = HalfedgeId(cornerHalfedge[base]);
        for (u32 i = 0; i < valence; ++i) {
            const u32 h = cornerHalfedge[base + i];
            const u32 hNext = cornerHalfedge[base + ((i + 1) % valence)];
            halfedgeFace_[h] = FaceId(static_cast<u32>(f));
            halfedgeNext_[h] = HalfedgeId(hNext);
            halfedgePrev_[hNext] = HalfedgeId(h);
            // Any outgoing halfedge will do for now; `linkBoundaryLoops` moves
            // the entry point onto a boundary halfedge where one exists.
            vertexOutgoing_[halfedgeTo_[h ^ 1u].index()] = HalfedgeId(h);
        }
    }

    const BuildResult boundary = linkBoundaryLoops();
    if (!boundary.ok()) {
        clear();
        return boundary;
    }
    return BuildResult{};
}

BuildResult Topology::linkBoundaryLoops() {
    // In a manifold mesh each boundary vertex has exactly one outgoing and one
    // incoming boundary halfedge, so the chaining is a pair of per-vertex slots
    // rather than a search. A second candidate on either side is a bowtie.
    const u32 vertices = vertexCount();
    std::vector<u32> outgoingBoundary(vertices, kInvalidId);
    std::vector<u32> incomingBoundary(vertices, kInvalidId);

    for (u32 h = 0; h < halfedgeCount(); ++h) {
        if (halfedgeFace_[h].valid()) {
            continue;
        }
        const u32 fromVertex = halfedgeTo_[h ^ 1u].value();
        const u32 toVertex = halfedgeTo_[h].value();
        if (outgoingBoundary[fromVertex] != kInvalidId) {
            return BuildResult{BuildError::NonManifoldVertex, kInvalidId, fromVertex};
        }
        if (incomingBoundary[toVertex] != kInvalidId) {
            return BuildResult{BuildError::NonManifoldVertex, kInvalidId, toVertex};
        }
        outgoingBoundary[fromVertex] = h;
        incomingBoundary[toVertex] = h;
    }

    for (u32 v = 0; v < vertices; ++v) {
        const u32 out = outgoingBoundary[v];
        const u32 in = incomingBoundary[v];
        if (out == kInvalidId && in == kInvalidId) {
            continue;
        }
        if (out == kInvalidId || in == kInvalidId) {
            // A boundary chain that enters a vertex and never leaves it cannot
            // close: the vertex's fan is broken.
            return BuildResult{BuildError::NonManifoldVertex, kInvalidId, v};
        }
        halfedgeNext_[in] = HalfedgeId(out);
        halfedgePrev_[out] = HalfedgeId(in);
        // C3's entry point: a boundary vertex's outgoing halfedge is the boundary
        // one, so `isBoundary(v)` is a single lookup and a fan circulation that
        // starts there covers the whole fan.
        vertexOutgoing_[v] = HalfedgeId(out);
    }

    // An interior vertex whose fan is more than one cycle is the other bowtie
    // case, and it survives the boundary check above because such a vertex has
    // no boundary halfedge at all. Circulating and comparing against the true
    // degree catches it.
    std::vector<u32> degree(vertices, 0);
    for (u32 h = 0; h < halfedgeCount(); ++h) {
        ++degree[halfedgeTo_[h ^ 1u].index()];
    }
    for (u32 v = 0; v < vertices; ++v) {
        const HalfedgeId start = vertexOutgoing_[v];
        if (!start.valid()) {
            continue;
        }
        u32 walked = 0;
        HalfedgeId h = start;
        do {
            ++walked;
            h = opposite(prev(h));
            if (walked > degree[v]) {
                break;
            }
        } while (h != start);
        if (walked != degree[v]) {
            return BuildResult{BuildError::NonManifoldVertex, kInvalidId, v};
        }
    }

    return BuildResult{};
}

void Topology::clear() {
    vertexOutgoing_.clear();
    halfedgeTo_.clear();
    halfedgeFace_.clear();
    halfedgeNext_.clear();
    halfedgePrev_.clear();
    faceHalfedge_.clear();
    vertexStatus_.clear();
    halfedgeStatus_.clear();
    edgeStatus_.clear();
    faceStatus_.clear();
}

FaceSet Topology::toFaceSet() const {
    FaceSet out;
    out.vertexCount = vertexCount();
    out.faceValence.reserve(faceCount());
    for (u32 f = 0; f < faceCount(); ++f) {
        const FaceId face(f);
        if (isDeleted(face) || !faceHalfedge_[f].valid()) {
            continue;
        }
        // `from(h)`, not `fv()`: the loop's first halfedge leaves the face's
        // first corner, so `from` recovers the corner order the build was given
        // while `fv()` (which yields `to`, as OpenMesh does) rotates it by one.
        u32 valence = 0;
        for (HalfedgeId h : fh(face)) {
            out.cornerVertex.push_back(from(h).value());
            ++valence;
        }
        out.faceValence.push_back(valence);
    }
    return out;
}

// ============================================================================
// Queries
// ============================================================================

bool Topology::isBoundary(VertexId v) const {
    const HalfedgeId h = vertexOutgoing_[v.index()];
    if (!h.valid()) {
        return true; // An isolated vertex is on the boundary of nothing.
    }
    return isBoundary(h);
}

u32 Topology::valence(VertexId v) const {
    u32 count = 0;
    for (HalfedgeId h : voh(v)) {
        (void)h;
        ++count;
    }
    return count;
}

u32 Topology::valence(FaceId f) const {
    u32 count = 0;
    for (HalfedgeId h : fh(f)) {
        (void)h;
        ++count;
    }
    return count;
}

HalfedgeId Topology::findHalfedge(VertexId a, VertexId b) const {
    for (HalfedgeId h : voh(a)) {
        if (to(h) == b) {
            return h;
        }
    }
    return HalfedgeId();
}

Topology::VertexOHalfedgeRange Topology::voh(VertexId v) const {
    return VertexOHalfedgeRange(this, vertexOutgoing_[v.index()]);
}
Topology::VertexVertexRange Topology::vv(VertexId v) const {
    return VertexVertexRange(this, vertexOutgoing_[v.index()]);
}
Topology::VertexFaceRange Topology::vf(VertexId v) const {
    return VertexFaceRange(this, vertexOutgoing_[v.index()]);
}
Topology::FaceHalfedgeRange Topology::fh(FaceId f) const {
    return FaceHalfedgeRange(this, faceHalfedge_[f.index()]);
}
Topology::FaceVertexRange Topology::fv(FaceId f) const {
    return FaceVertexRange(this, faceHalfedge_[f.index()]);
}
Topology::FaceFaceRange Topology::ff(FaceId f) const {
    return FaceFaceRange(this, faceHalfedge_[f.index()]);
}
Topology::HalfedgeLoopRange Topology::hl(HalfedgeId h) const {
    return HalfedgeLoopRange(this, h);
}

// ============================================================================
// Allocation
// ============================================================================

VertexId Topology::addVertex() {
    const u32 index = static_cast<u32>(vertexOutgoing_.size());
    vertexOutgoing_.push_back(HalfedgeId());
    vertexStatus_.push_back(Status::None);
    return VertexId(index);
}

FaceId Topology::addFaceSlot() {
    const u32 index = static_cast<u32>(faceHalfedge_.size());
    faceHalfedge_.push_back(HalfedgeId());
    faceStatus_.push_back(Status::None);
    return FaceId(index);
}

HalfedgeId Topology::addEdgePair() {
    const u32 index = static_cast<u32>(halfedgeTo_.size());
    halfedgeTo_.resize(index + 2, VertexId());
    halfedgeFace_.resize(index + 2, FaceId());
    halfedgeNext_.resize(index + 2, HalfedgeId());
    halfedgePrev_.resize(index + 2, HalfedgeId());
    halfedgeStatus_.resize(index + 2, Status::None);
    edgeStatus_.push_back(Status::None);
    return HalfedgeId(index);
}

void Topology::adjustOutgoing(VertexId v) {
    const HalfedgeId start = vertexOutgoing_[v.index()];
    if (!start.valid()) {
        return;
    }
    HalfedgeId h = start;
    do {
        if (isBoundary(h)) {
            vertexOutgoing_[v.index()] = h;
            return;
        }
        h = opposite(prev(h));
    } while (h != start);
}

// ============================================================================
// Lazy deletion
// ============================================================================

void Topology::deleteFace(FaceId f, bool deleteIsolatedVertices) {
    if (isDeleted(f)) {
        return;
    }
    faceStatus_[f.index()] |= Status::Deleted;

    // The loop's halfedges become boundary halfedges. Their `next` chains are
    // then rebuilt for the vertices involved, which is the only part of the
    // structure a face removal can invalidate.
    std::vector<HalfedgeId> loop;
    for (HalfedgeId h : fh(f)) {
        loop.push_back(h);
    }
    for (HalfedgeId h : loop) {
        halfedgeFace_[h.index()] = FaceId();
    }

    for (HalfedgeId h : loop) {
        const EdgeId e = edge(h);
        if (isBoundary(opposite(h))) {
            // Both sides free: the edge is gone.
            edgeStatus_[e.index()] |= Status::Deleted;
            halfedgeStatus_[h.index()] |= Status::Deleted;
            halfedgeStatus_[opposite(h).index()] |= Status::Deleted;
        }
    }

    // Re-chain the boundary through every vertex the face touched. A vertex may
    // now have several free outgoing halfedges (a pinch); the first one found in
    // handle order wins, which keeps the result deterministic.
    for (HalfedgeId h : loop) {
        const VertexId v = to(h);
        HalfedgeId incoming;
        HalfedgeId outgoing;
        for (u32 raw = 0; raw < halfedgeCount(); ++raw) {
            const HalfedgeId candidate(raw);
            if (hasStatus(halfedgeStatus_[raw], Status::Deleted) || face(candidate).valid()) {
                continue;
            }
            if (to(candidate) == v && !incoming.valid()) {
                incoming = candidate;
            }
            if (from(candidate) == v && !outgoing.valid()) {
                outgoing = candidate;
            }
        }
        if (incoming.valid() && outgoing.valid()) {
            halfedgeNext_[incoming.index()] = outgoing;
            halfedgePrev_[outgoing.index()] = incoming;
            vertexOutgoing_[v.index()] = outgoing;
        } else if (!incoming.valid() && !outgoing.valid()) {
            vertexOutgoing_[v.index()] = HalfedgeId();
            if (deleteIsolatedVertices) {
                vertexStatus_[v.index()] |= Status::Deleted;
            }
        }
    }
}

void Topology::deleteEdge(EdgeId e, bool deleteIsolatedVertices) {
    const HalfedgeId h0 = halfedge(e, 0);
    const HalfedgeId h1 = halfedge(e, 1);
    if (face(h0).valid()) {
        deleteFace(face(h0), deleteIsolatedVertices);
    }
    if (face(h1).valid()) {
        deleteFace(face(h1), deleteIsolatedVertices);
    }
    edgeStatus_[e.index()] |= Status::Deleted;
    halfedgeStatus_[h0.index()] |= Status::Deleted;
    halfedgeStatus_[h1.index()] |= Status::Deleted;
}

void Topology::deleteVertex(VertexId v) {
    std::vector<FaceId> faces;
    for (FaceId f : vf(v)) {
        if (f.valid()) {
            faces.push_back(f);
        }
    }
    for (FaceId f : faces) {
        deleteFace(f, false);
    }
    vertexStatus_[v.index()] |= Status::Deleted;
    vertexOutgoing_[v.index()] = HalfedgeId();
}

bool Topology::hasDeleted() const {
    for (Status s : vertexStatus_) {
        if (hasStatus(s, Status::Deleted)) {
            return true;
        }
    }
    for (Status s : faceStatus_) {
        if (hasStatus(s, Status::Deleted)) {
            return true;
        }
    }
    for (Status s : edgeStatus_) {
        if (hasStatus(s, Status::Deleted)) {
            return true;
        }
    }
    return false;
}

Topology::Remap Topology::garbageCollect() {
    Remap remap;
    remap.vertices.assign(vertexCount(), kInvalidId);
    remap.halfedges.assign(halfedgeCount(), kInvalidId);
    remap.edges.assign(edgeCount(), kInvalidId);
    remap.faces.assign(faceCount(), kInvalidId);

    // Edges move as pairs, so the halfedge remap is derived from the edge one and
    // C1 survives collection.
    for (u32 e = 0; e < edgeCount(); ++e) {
        if (hasStatus(edgeStatus_[e], Status::Deleted)) {
            continue;
        }
        const u32 fresh = remap.newEdgeCount++;
        remap.edges[e] = fresh;
        remap.halfedges[e * 2] = fresh * 2;
        remap.halfedges[e * 2 + 1] = fresh * 2 + 1;
    }
    remap.newHalfedgeCount = remap.newEdgeCount * 2;

    for (u32 v = 0; v < vertexCount(); ++v) {
        if (!hasStatus(vertexStatus_[v], Status::Deleted)) {
            remap.vertices[v] = remap.newVertexCount++;
        }
    }
    for (u32 f = 0; f < faceCount(); ++f) {
        if (!hasStatus(faceStatus_[f], Status::Deleted)) {
            remap.faces[f] = remap.newFaceCount++;
        }
    }

    const auto mapVertex = [&](VertexId v) {
        return v.valid() && remap.vertices[v.index()] != kInvalidId
                   ? VertexId(remap.vertices[v.index()])
                   : VertexId();
    };
    const auto mapHalfedge = [&](HalfedgeId h) {
        return h.valid() && remap.halfedges[h.index()] != kInvalidId
                   ? HalfedgeId(remap.halfedges[h.index()])
                   : HalfedgeId();
    };
    const auto mapFace = [&](FaceId f) {
        return f.valid() && remap.faces[f.index()] != kInvalidId ? FaceId(remap.faces[f.index()])
                                                                 : FaceId();
    };

    std::vector<HalfedgeId> newVertexOutgoing(remap.newVertexCount);
    std::vector<Status> newVertexStatus(remap.newVertexCount, Status::None);
    for (u32 v = 0; v < vertexCount(); ++v) {
        const u32 fresh = remap.vertices[v];
        if (fresh == kInvalidId) {
            continue;
        }
        newVertexOutgoing[fresh] = mapHalfedge(vertexOutgoing_[v]);
        newVertexStatus[fresh] = vertexStatus_[v] & ~Status::Deleted;
    }

    std::vector<VertexId> newTo(remap.newHalfedgeCount);
    std::vector<FaceId> newFace(remap.newHalfedgeCount);
    std::vector<HalfedgeId> newNext(remap.newHalfedgeCount);
    std::vector<HalfedgeId> newPrev(remap.newHalfedgeCount);
    std::vector<Status> newHalfedgeStatus(remap.newHalfedgeCount, Status::None);
    for (u32 h = 0; h < halfedgeCount(); ++h) {
        const u32 fresh = remap.halfedges[h];
        if (fresh == kInvalidId) {
            continue;
        }
        newTo[fresh] = mapVertex(halfedgeTo_[h]);
        newFace[fresh] = mapFace(halfedgeFace_[h]);
        newNext[fresh] = mapHalfedge(halfedgeNext_[h]);
        newPrev[fresh] = mapHalfedge(halfedgePrev_[h]);
        newHalfedgeStatus[fresh] = halfedgeStatus_[h] & ~Status::Deleted;
    }

    std::vector<Status> newEdgeStatus(remap.newEdgeCount, Status::None);
    for (u32 e = 0; e < edgeCount(); ++e) {
        const u32 fresh = remap.edges[e];
        if (fresh != kInvalidId) {
            newEdgeStatus[fresh] = edgeStatus_[e] & ~Status::Deleted;
        }
    }

    std::vector<HalfedgeId> newFaceHalfedge(remap.newFaceCount);
    std::vector<Status> newFaceStatus(remap.newFaceCount, Status::None);
    for (u32 f = 0; f < faceCount(); ++f) {
        const u32 fresh = remap.faces[f];
        if (fresh == kInvalidId) {
            continue;
        }
        newFaceHalfedge[fresh] = mapHalfedge(faceHalfedge_[f]);
        newFaceStatus[fresh] = faceStatus_[f] & ~Status::Deleted;
    }

    vertexOutgoing_ = std::move(newVertexOutgoing);
    vertexStatus_ = std::move(newVertexStatus);
    halfedgeTo_ = std::move(newTo);
    halfedgeFace_ = std::move(newFace);
    halfedgeNext_ = std::move(newNext);
    halfedgePrev_ = std::move(newPrev);
    halfedgeStatus_ = std::move(newHalfedgeStatus);
    edgeStatus_ = std::move(newEdgeStatus);
    faceHalfedge_ = std::move(newFaceHalfedge);
    faceStatus_ = std::move(newFaceStatus);

    return remap;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
