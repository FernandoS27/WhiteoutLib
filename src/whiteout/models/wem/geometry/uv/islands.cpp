// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/uv/islands.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

namespace {

/// The classic disjoint set, by rank with path halving. Small enough to keep
/// here twice over rather than reach for a shared one: the faces and the
/// corners are two different populations and neither wants the other's.
class UnionFind {
public:
    explicit UnionFind(u32 count) : parent_(count), rank_(count, 0) {
        for (u32 i = 0; i < count; ++i) {
            parent_[i] = i;
        }
    }

    u32 find(u32 a) {
        while (parent_[a] != a) {
            parent_[a] = parent_[parent_[a]];
            a = parent_[a];
        }
        return a;
    }

    void join(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        if (rank_[a] < rank_[b]) {
            std::swap(a, b);
        }
        parent_[b] = a;
        if (rank_[a] == rank_[b]) {
            ++rank_[a];
        }
    }

private:
    std::vector<u32> parent_;
    std::vector<u8> rank_;
};

/// The two corner pairs an interior edge brings face to face: corner `h` of one
/// face against the corner of the other at the same vertex, at both ends. The
/// same pairing `SeamBetween` makes, spelled again here because this one runs
/// over every edge of every mesh and cannot afford the layer walk.
struct AcrossPair {
    HalfedgeId a, b;
};

bool acrossPairs(const Topology& topology, EdgeId edge, AcrossPair (&out)[2]) {
    const HalfedgeId h0 = Topology::halfedge(edge, 0);
    const HalfedgeId h1 = Topology::halfedge(edge, 1);
    if (!topology.face(h0).valid() || !topology.face(h1).valid()) {
        return false;
    }
    out[0] = {h0, topology.next(h1)};
    out[1] = {topology.next(h0), h1};
    return true;
}

bool uvDiffers(std::span<const Vector2f> uvs, HalfedgeId a, HalfedgeId b) {
    if (a.index() >= uvs.size() || b.index() >= uvs.size()) {
        return false;
    }
    // Bytes, not a tolerance: two corners the file wrote the same way are one
    // UV vertex, and two it did not are two, however close they look (§3).
    return std::memcmp(&uvs[a.index()], &uvs[b.index()], sizeof(Vector2f)) != 0;
}

} // namespace

std::span<const u32> UvIslands::facesOf(u32 island) const {
    if (island + 1 >= faceOffsets.size()) {
        return {};
    }
    return std::span<const u32>(faces.data() + faceOffsets[island],
                                faceOffsets[island + 1] - faceOffsets[island]);
}

std::span<const HalfedgeId> UvIslands::boundaryOf(u32 island) const {
    if (island + 1 >= boundaryOffsets.size()) {
        return {};
    }
    return std::span<const HalfedgeId>(boundary.data() + boundaryOffsets[island],
                                       boundaryOffsets[island + 1] - boundaryOffsets[island]);
}

std::span<const u32> UvIslands::cornersOf(u32 wedge) const {
    if (wedge + 1 >= wedgeOffsets.size()) {
        return {};
    }
    return std::span<const u32>(wedgeCorners.data() + wedgeOffsets[wedge],
                                wedgeOffsets[wedge + 1] - wedgeOffsets[wedge]);
}

std::vector<u8> CutsOf(const Mesh& mesh, u32 set) {
    std::vector<u8> cuts;
    if (!mesh.hasConnectivity()) {
        return cuts;
    }
    const Topology& topology = mesh.topology();
    cuts.assign(topology.edgeCount(), 0);
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
    const std::span<const u8> marks =
        mesh.attributes.get<const u8>(names::uvSeam(set), Domain::Edge);
    const std::span<const u32> sections = mesh.faceSections();

    for (u32 e = 0; e < cuts.size(); ++e) {
        const EdgeId edge(e);
        if (topology.isDeleted(edge)) {
            continue;
        }
        if (e < marks.size() && marks[e] != 0) {
            cuts[e] = 1;
            continue;
        }
        AcrossPair pairs[2];
        if (!acrossPairs(topology, edge, pairs)) {
            // A mesh border is a cut: an island cannot continue past the edge
            // of the surface.
            cuts[e] = 1;
            continue;
        }
        const FaceId fa = topology.face(Topology::halfedge(edge, 0));
        const FaceId fb = topology.face(Topology::halfedge(edge, 1));
        if (fa.index() < sections.size() && fb.index() < sections.size() &&
            sections[fa.index()] != sections[fb.index()]) {
            cuts[e] = 1;
            continue;
        }
        if (uvs.empty()) {
            // No set to disagree in: the surface is one island until a mark
            // says otherwise, which is what a set born empty should be.
            continue;
        }
        if (uvDiffers(uvs, pairs[0].a, pairs[0].b) || uvDiffers(uvs, pairs[1].a, pairs[1].b)) {
            cuts[e] = 1;
        }
    }
    return cuts;
}

UvIslands BuildUvIslands(const Mesh& mesh, u32 set) {
    UvIslands out;
    out.set = set;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    const u32 faceCount = topology.faceCount();
    const u32 halfedgeCount = topology.halfedgeCount();
    const std::vector<u8> cuts = CutsOf(mesh, set);

    // --- faces into islands --------------------------------------------------
    UnionFind faceSets(faceCount);
    UnionFind cornerSets(halfedgeCount);
    for (u32 e = 0; e < cuts.size(); ++e) {
        if (cuts[e] != 0) {
            continue;
        }
        const EdgeId edge(e);
        AcrossPair pairs[2];
        if (!acrossPairs(topology, edge, pairs)) {
            continue;
        }
        faceSets.join(topology.face(Topology::halfedge(edge, 0)).value(),
                      topology.face(Topology::halfedge(edge, 1)).value());
        // The two corners at each end are the same UV vertex. A cut at one end
        // of an edge separates that end's corners and leaves the other end's
        // joined, which is what makes a dart two wedges and not four.
        cornerSets.join(pairs[0].a.value(), pairs[0].b.value());
        cornerSets.join(pairs[1].a.value(), pairs[1].b.value());
    }

    out.islandOfFace.assign(faceCount, kInvalidId);
    std::vector<u32> islandOfRoot(faceCount, kInvalidId);
    std::vector<u32> countPerIsland;
    for (u32 f = 0; f < faceCount; ++f) {
        const FaceId face(f);
        if (topology.isDeleted(face)) {
            continue;
        }
        const u32 root = faceSets.find(f);
        if (islandOfRoot[root] == kInvalidId) {
            islandOfRoot[root] = out.count++;
            countPerIsland.push_back(0);
        }
        out.islandOfFace[f] = islandOfRoot[root];
        ++countPerIsland[islandOfRoot[root]];
    }

    out.faceOffsets.assign(out.count + 1, 0);
    for (u32 i = 0; i < out.count; ++i) {
        out.faceOffsets[i + 1] = out.faceOffsets[i] + countPerIsland[i];
    }
    out.faces.assign(out.faceOffsets.empty() ? 0 : out.faceOffsets.back(), 0);
    {
        std::vector<u32> cursor(out.faceOffsets.begin(), out.faceOffsets.end() - (out.count ? 1 : 0));
        for (u32 f = 0; f < faceCount; ++f) {
            const u32 island = out.islandOfFace[f];
            if (island != kInvalidId) {
                out.faces[cursor[island]++] = f;
            }
        }
    }

    // --- corners into wedges -------------------------------------------------
    out.wedgeOfCorner.assign(halfedgeCount, kInvalidId);
    std::vector<u32> wedgeOfRoot(halfedgeCount, kInvalidId);
    std::vector<u32> perWedge;
    for (u32 h = 0; h < halfedgeCount; ++h) {
        const HalfedgeId corner(h);
        // A boundary halfedge is not a corner: it has no face, so it carries no
        // corner attributes and belongs to no wedge.
        if (topology.isDeleted(corner) || !topology.face(corner).valid()) {
            continue;
        }
        const u32 root = cornerSets.find(h);
        if (wedgeOfRoot[root] == kInvalidId) {
            wedgeOfRoot[root] = out.wedgeCount++;
            perWedge.push_back(0);
            out.wedgeVertex.push_back(topology.from(corner).value());
            out.wedgeIsland.push_back(out.islandOf(topology.face(corner).value()));
        }
        out.wedgeOfCorner[h] = wedgeOfRoot[root];
        ++perWedge[wedgeOfRoot[root]];
    }
    out.wedgeOffsets.assign(out.wedgeCount + 1, 0);
    for (u32 w = 0; w < out.wedgeCount; ++w) {
        out.wedgeOffsets[w + 1] = out.wedgeOffsets[w] + perWedge[w];
    }
    out.wedgeCorners.assign(out.wedgeOffsets.empty() ? 0 : out.wedgeOffsets.back(), 0);
    {
        std::vector<u32> cursor(out.wedgeOffsets.begin(),
                                out.wedgeOffsets.end() - (out.wedgeCount ? 1 : 0));
        for (u32 h = 0; h < halfedgeCount; ++h) {
            const u32 wedge = out.wedgeOfCorner[h];
            if (wedge != kInvalidId) {
                out.wedgeCorners[cursor[wedge]++] = h;
            }
        }
    }

    // --- boundaries ----------------------------------------------------------
    //
    // A boundary halfedge is a corner whose edge is cut. They are walked into
    // loops the way an edge loop is walked: from a corner, round its far vertex
    // through the fan, until the next cut edge. A loop closes when the walk
    // returns to where it started, and an island's loops are laid end to end.
    out.boundaryOffsets.assign(out.count + 1, 0);
    out.closed.assign(out.count, 0);
    out.loops.assign(out.count, 0);
    std::vector<u8> visited(halfedgeCount, 0);
    std::vector<std::vector<HalfedgeId>> perIsland(out.count);
    for (u32 h = 0; h < halfedgeCount; ++h) {
        const HalfedgeId start(h);
        if (visited[h] != 0 || topology.isDeleted(start) || !topology.face(start).valid()) {
            continue;
        }
        const u32 edgeIndex = Topology::edge(start).index();
        if (edgeIndex >= cuts.size() || cuts[edgeIndex] == 0) {
            continue;
        }
        const u32 island = out.islandOf(topology.face(start).value());
        if (island == kInvalidId) {
            continue;
        }
        HalfedgeId walk = start;
        do {
            visited[walk.index()] = 1;
            perIsland[island].push_back(walk);
            HalfedgeId step = topology.next(walk);
            // Round the far vertex through the fan to the next cut. Bounded by
            // the fan: a mesh whose cuts a walk cannot find is broken, and a
            // broken mesh is not a reason to hang.
            for (u32 guard = 0; guard < halfedgeCount; ++guard) {
                const u32 stepEdge = Topology::edge(step).index();
                if (stepEdge < cuts.size() && cuts[stepEdge] != 0) {
                    break;
                }
                const HalfedgeId across = Topology::opposite(step);
                if (!topology.face(across).valid()) {
                    break;
                }
                step = topology.next(across);
            }
            walk = step;
        } while (walk != start && visited[walk.index()] == 0);
        ++out.loops[island];
    }
    for (u32 i = 0; i < out.count; ++i) {
        out.boundaryOffsets[i + 1] =
            out.boundaryOffsets[i] + static_cast<u32>(perIsland[i].size());
        out.closed[i] = perIsland[i].empty() ? 1 : 0;
    }
    out.boundary.reserve(out.boundaryOffsets.empty() ? 0 : out.boundaryOffsets.back());
    for (u32 i = 0; i < out.count; ++i) {
        out.boundary.insert(out.boundary.end(), perIsland[i].begin(), perIsland[i].end());
    }
    return out;
}

std::vector<u32> IslandWedges(const UvIslands& islands, const Mesh& mesh, u32 island) {
    std::vector<u32> out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    for (const u32 face : islands.facesOf(island)) {
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            const u32 wedge = islands.wedgeOf(h);
            if (wedge != kInvalidId) {
                out.push_back(wedge);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

UvFirstVisit EnsureUvSet(Mesh& mesh, u32 set) {
    UvFirstVisit out;
    const std::string uvName = names::uv(set);
    out.wroteUvs = !mesh.attributes.has(uvName, Domain::Halfedge);
    out.bornFree = out.wroteUvs;
    if (out.wroteUvs) {
        mesh.attributes.getOrCreate<Vector2f>(uvName, Domain::Halfedge, AttrType::F32x2);
    }

    const std::string seamName = names::uvSeam(set);
    out.wroteSeams = !mesh.attributes.has(seamName, Domain::Edge);
    if (out.wroteSeams) {
        // The file's own layout, read back as the cuts that made it. Without
        // this a first visit would see one island where the artist shipped
        // forty (§4).
        const std::vector<u8> cuts = CutsOf(mesh, set);
        const std::span<u8> marks =
            mesh.attributes.getOrCreate<u8>(seamName, Domain::Edge, AttrType::Bool);
        const std::size_t n = std::min(marks.size(), cuts.size());
        for (std::size_t e = 0; e < n; ++e) {
            // A mesh border needs no mark: it cuts by being a border, and
            // marking it would write a bit that says nothing and would have to
            // be kept true through every edit of the shape.
            if (cuts[e] != 0 &&
                !std::as_const(mesh).topology().isBoundary(EdgeId(static_cast<u32>(e)))) {
                marks[e] = 1;
            }
        }
    }

    const std::string freeName = names::uvFree(set);
    out.wroteFree = !mesh.attributes.has(freeName, Domain::Face);
    if (out.wroteFree) {
        const std::span<u8> free =
            mesh.attributes.getOrCreate<u8>(freeName, Domain::Face, AttrType::Bool);
        if (out.bornFree) {
            std::fill(free.begin(), free.end(), static_cast<u8>(1));
        }
    }

    if (!mesh.attributes.has(names::uvPin(set), Domain::Halfedge)) {
        mesh.attributes.getOrCreate<u8>(names::uvPin(set), Domain::Halfedge, AttrType::Bool);
    }
    return out;
}

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
