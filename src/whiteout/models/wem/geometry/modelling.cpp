// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/modelling.h>

#include "rebuild.h"

#include <whiteout/models/wem/geometry/repair.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/geometry/uv/seams.h>
#include <whiteout/models/wem/skinning/points.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

f32 CoincidenceTolerance(std::span<const Vector3f> positions) {
    if (positions.empty()) {
        return 0.0f;
    }
    Vector3f low = positions[0];
    Vector3f high = positions[0];
    for (const Vector3f& p : positions) {
        low.x = std::min(low.x, p.x);
        low.y = std::min(low.y, p.y);
        low.z = std::min(low.z, p.z);
        high.x = std::max(high.x, p.x);
        high.y = std::max(high.y, p.y);
        high.z = std::max(high.z, p.z);
    }
    const Vector3f span{high.x - low.x, high.y - low.y, high.z - low.z};
    return std::sqrt(span.x * span.x + span.y * span.y + span.z * span.z) * 1e-5f;
}

f32 CoincidenceTolerance(const Mesh& mesh) {
    return CoincidenceTolerance(
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex));
}

bool IsModelled(const Mesh& mesh) {
    const std::span<const u8> marker = mesh.attributes.get<const u8>(names::kModelled, Domain::Mesh);
    return !marker.empty() && marker[0] != 0;
}

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

f64 angleBetween(const std::array<f64, 3>& a, const std::array<f64, 3>& b) {
    const f64 cx = a[1] * b[2] - a[2] * b[1];
    const f64 cy = a[2] * b[0] - a[0] * b[2];
    const f64 cz = a[0] * b[1] - a[1] * b[0];
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

std::array<f64, 3> toArray(const Vector3f& v) {
    return {static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}

std::span<const Vector3f> positionsOf(const Mesh& mesh) {
    return mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
}

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

/// Face @p face as it is drawn, vertex ids, three per triangle; @p pending
/// overrides the stored row for a face a tool already re-cut.
std::vector<u32> drawnOf(const Mesh& mesh, FaceId face,
                         const std::unordered_map<u32, std::vector<u32>>* pending = nullptr) {
    const std::vector<u32> loop = loopOf(mesh.topology(), face);
    std::span<const u32> row = mesh.triangulation.row(face.value());
    if (pending != nullptr) {
        const auto found = pending->find(face.value());
        if (found != pending->end()) {
            row = found->second;
        }
    }
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
        // (EDIT_MODE_UV_DESIGN.md §3).
        if (names::IsUvPin(layer.name)) {
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

/// The union row of a join across @p edge, lower-numbered face first (§2.3).
std::vector<u32> unionRow(const Mesh& mesh, EdgeId edge,
                          const std::unordered_map<u32, std::vector<u32>>* pending = nullptr) {
    const Topology& topology = mesh.topology();
    FaceId first = topology.face(Topology::halfedge(edge, 0));
    FaceId second = topology.face(Topology::halfedge(edge, 1));
    if (second.value() < first.value()) {
        std::swap(first, second);
    }
    std::vector<u32> row = drawnOf(mesh, first, pending);
    const std::vector<u32> rest = drawnOf(mesh, second, pending);
    row.insert(row.end(), rest.begin(), rest.end());
    return row;
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

std::vector<u32> sortedUnique(std::vector<u32> list) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    return list;
}

} // namespace

// ============================================================================
// Seams and sharp edges
// ============================================================================

namespace {

/// The corner pairs across @p h's edge: (side A at a, side B at a) and
/// (side A at b, side B at b). False on a border.
bool acrossPairs(const Topology& topology, HalfedgeId h,
                 std::array<std::pair<HalfedgeId, HalfedgeId>, 2>& out) {
    const HalfedgeId o = Topology::opposite(h);
    if (!topology.face(h).valid() || !topology.face(o).valid()) {
        return false;
    }
    out[0] = {h, topology.next(o)};
    out[1] = {topology.next(h), o};
    return true;
}

} // namespace

bool SeamBetween(const Mesh& mesh, HalfedgeId h) {
    if (!mesh.hasConnectivity()) {
        return false;
    }
    std::array<std::pair<HalfedgeId, HalfedgeId>, 2> pairs;
    if (!acrossPairs(mesh.topology(), h, pairs)) {
        return false;
    }
    for (const AttrLayer& layer : mesh.attributes.layers()) {
        // A pin is authoring state on one corner, not a difference in what the
        // surface holds there: pinning one side of an edge must not mark it
        // (EDIT_MODE_UV_DESIGN.md §3).
        if (layer.domain != Domain::Halfedge || layer.name == names::kNormal ||
            layer.name == names::kBinormal || names::IsUvPin(layer.name)) {
            continue;
        }
        const std::size_t stride = AttrTypeSize(layer.type);
        for (const auto& [a, b] : pairs) {
            const std::size_t at = stride * a.index();
            const std::size_t bt = stride * b.index();
            if (at + stride > layer.data.size() || bt + stride > layer.data.size()) {
                continue;
            }
            if (layer.name == names::kTangent) {
                // Derived shading never makes a seam: only the handedness counts.
                if (layer.type != AttrType::F32x4) {
                    continue;
                }
                f32 wa = 0.0f;
                f32 wb = 0.0f;
                std::memcpy(&wa, layer.data.data() + at + 3 * sizeof(f32), sizeof(f32));
                std::memcpy(&wb, layer.data.data() + bt + 3 * sizeof(f32), sizeof(f32));
                if ((wa < 0.0f) != (wb < 0.0f)) {
                    return true;
                }
                continue;
            }
            if (std::memcmp(layer.data.data() + at, layer.data.data() + bt, stride) != 0) {
                return true;
            }
        }
    }
    return false;
}

bool NormalsBreakAcross(const Mesh& mesh, HalfedgeId h, f32 angle) {
    if (!mesh.hasConnectivity()) {
        return false;
    }
    std::array<std::pair<HalfedgeId, HalfedgeId>, 2> pairs;
    if (!acrossPairs(mesh.topology(), h, pairs)) {
        return false;
    }
    const std::span<const Vector3f> normals =
        mesh.attributes.get<const Vector3f>(names::kNormal, Domain::Halfedge);
    if (normals.empty()) {
        return false;
    }
    for (const auto& [a, b] : pairs) {
        if (a.index() >= normals.size() || b.index() >= normals.size()) {
            continue;
        }
        if (angleBetween(toArray(normals[a.index()]), toArray(normals[b.index()])) > angle) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// The weld engine: rule 3's fixed point, shared by the prepare and Weld
// ============================================================================

namespace {

/// The mesh's face set, indexed for the weld.
struct FaceTable {
    const FaceSet* set = nullptr;
    std::vector<u32> base;                  ///< Per face, its first corner.
    std::vector<std::vector<u32>> facesOf;  ///< Per vertex, the faces holding it, ascending.
};

FaceTable tableOf(const FaceSet& set) {
    FaceTable out;
    out.set = &set;
    out.base.resize(set.faceCount() + 1, 0);
    for (std::size_t f = 0; f < set.faceCount(); ++f) {
        out.base[f + 1] = out.base[f] + set.faceValence[f];
    }
    out.facesOf.resize(set.vertexCount);
    for (std::size_t f = 0; f < set.faceCount(); ++f) {
        for (u32 c = out.base[f]; c < out.base[f + 1]; ++c) {
            const u32 v = set.cornerVertex[c];
            if (v < out.facesOf.size() &&
                (out.facesOf[v].empty() || out.facesOf[v].back() != f)) {
                out.facesOf[v].push_back(static_cast<u32>(f));
            }
        }
    }
    return out;
}

/// A partition of the vertices into merge groups, each group's members ascending.
struct Partition {
    std::vector<u32> groupOf;
    std::vector<std::vector<u32>> groups;

    explicit Partition(u32 vertexCount) : groupOf(vertexCount), groups(vertexCount) {
        for (u32 v = 0; v < vertexCount; ++v) {
            groupOf[v] = v;
            groups[v] = {v};
        }
    }

    /// Makes @p members (ascending, all currently singletons) one group.
    void join(const std::vector<u32>& members) {
        if (members.size() < 2) {
            return;
        }
        const u32 target = groupOf[members[0]];
        for (std::size_t i = 1; i < members.size(); ++i) {
            const u32 from = groupOf[members[i]];
            for (const u32 v : groups[from]) {
                groupOf[v] = target;
                groups[target].push_back(v);
            }
            groups[from].clear();
        }
        std::sort(groups[target].begin(), groups[target].end());
    }

    /// Moves @p movers out of their group into a new one of their own.
    void split(const std::vector<u32>& movers) {
        if (movers.empty()) {
            return;
        }
        const u32 from = groupOf[movers[0]];
        const u32 fresh = static_cast<u32>(groups.size());
        groups.emplace_back();
        for (const u32 v : movers) {
            groupOf[v] = fresh;
            groups.back().push_back(v);
        }
        std::vector<u32>& rest = groups[from];
        rest.erase(std::remove_if(rest.begin(), rest.end(),
                                  [&](u32 v) { return groupOf[v] != from; }),
                   rest.end());
        std::sort(groups.back().begin(), groups.back().end());
    }
};

/// The face set a partition welds to, in survivors' old ids.
struct Candidate {
    FaceSet faces;
    std::vector<u32> sections;
    std::vector<u32> sourceFace;        ///< Per candidate face.
    std::vector<u32> sourceCorner;      ///< Per candidate corner, into the original corners.
    std::vector<u32> candidateOfCorner; ///< Per original corner, or kInvalidId (its face went).
    std::vector<u32> faceOfCandidateCorner;
};

/// @p collapse: consecutive corners that become one vertex merge, and a face
/// left with fewer than three goes (Weld); otherwise every corner stays.
Candidate candidateOf(const FaceTable& table, std::span<const u32> sections,
                      std::span<const u32> survivorOf, bool collapse) {
    const FaceSet& set = *table.set;
    Candidate out;
    out.faces.vertexCount = set.vertexCount;
    out.candidateOfCorner.assign(set.cornerVertex.size(), kInvalidId);
    std::vector<u32> mapped;
    std::vector<u32> origin;
    for (std::size_t f = 0; f < set.faceCount(); ++f) {
        mapped.clear();
        origin.clear();
        const u32 base = table.base[f];
        const u32 valence = set.faceValence[f];
        std::vector<u32> runOf(valence, 0);
        for (u32 i = 0; i < valence; ++i) {
            const u32 v = survivorOf[set.cornerVertex[base + i]];
            if (collapse && !mapped.empty() && mapped.back() == v) {
                runOf[i] = static_cast<u32>(mapped.size() - 1);
                continue;
            }
            runOf[i] = static_cast<u32>(mapped.size());
            mapped.push_back(v);
            origin.push_back(base + i);
        }
        if (collapse && mapped.size() > 1 && mapped.front() == mapped.back()) {
            mapped.pop_back();
            origin.pop_back();
            for (u32& run : runOf) {
                if (run == mapped.size()) {
                    run = 0;
                }
            }
        }
        if (mapped.size() < 3) {
            continue;
        }
        const u32 first = static_cast<u32>(out.faces.cornerVertex.size());
        const u32 index = static_cast<u32>(out.sourceFace.size());
        out.sourceFace.push_back(static_cast<u32>(f));
        out.sections.push_back(f < sections.size() ? sections[f] : 0u);
        out.faces.faceValence.push_back(static_cast<u32>(mapped.size()));
        for (std::size_t k = 0; k < mapped.size(); ++k) {
            out.faces.cornerVertex.push_back(mapped[k]);
            out.sourceCorner.push_back(origin[k]);
            out.faceOfCandidateCorner.push_back(index);
        }
        for (u32 i = 0; i < valence; ++i) {
            out.candidateOfCorner[base + i] = first + runOf[i];
        }
    }
    return out;
}

struct Settled {
    std::vector<u32> survivorOf;  ///< Per vertex: its group's survivor.
    std::vector<Vector3f> places; ///< Per vertex: where it ends up.
    Candidate candidate;
    u32 keptNonManifold = 0;
};

using Chooser = std::function<u32(const std::vector<u32>&)>;
using Placer = std::function<void(const Partition&, std::span<const u8>, std::vector<Vector3f>&)>;

/// Rule 3 (§2.2): trial `Repair` runs on the welded face set until one changes
/// nothing, each failure refining the partition along the repair's own
/// partition, and a dropped face's corners split out at their own positions.
Settled settle(const FaceTable& table, std::span<const u32> sections, Partition& partition,
               const Chooser& choose, const Placer& place, bool collapse) {
    const FaceSet& set = *table.set;
    const u32 vertexCount = set.vertexCount;
    std::vector<u8> own(vertexCount, 0);
    Settled out;
    for (;;) {
        out.survivorOf.resize(vertexCount);
        for (const std::vector<u32>& group : partition.groups) {
            if (group.empty()) {
                continue;
            }
            const u32 survivor = choose(group);
            for (const u32 v : group) {
                out.survivorOf[v] = survivor;
            }
        }
        place(partition, own, out.places);
        out.candidate = candidateOf(table, sections, out.survivorOf, collapse);
        const RepairResult trial = Repair(out.candidate.faces, out.candidate.sections, out.places);
        if (!trial.changed) {
            return out;
        }

        const Candidate& candidate = out.candidate;
        std::vector<u8> dropped(candidate.sourceFace.size(), 0);
        for (const FaceRecord& record : trial.log.droppedFaces) {
            if (record.index < dropped.size()) {
                dropped[record.index] = 1;
            }
        }
        // Each candidate corner's vertex in the repaired set.
        std::vector<u32> resultOf(candidate.faces.cornerVertex.size(), kInvalidId);
        {
            std::size_t cursor = 0;
            std::size_t corner = 0;
            for (std::size_t j = 0; j < candidate.sourceFace.size(); ++j) {
                const u32 valence = candidate.faces.faceValence[j];
                if (dropped[j] == 0) {
                    for (u32 k = 0; k < valence && cursor + k < trial.faces.cornerVertex.size(); ++k) {
                        resultOf[corner + k] = trial.faces.cornerVertex[cursor + k];
                    }
                    cursor += valence;
                }
                corner += valence;
            }
        }
        // Each vertex's corners, as the repaired set numbers them: a member
        // the repair split, even partly, receives more than one.
        std::vector<std::vector<u32>> received(vertexCount);
        std::vector<u8> fromDropped(vertexCount, 0);
        for (std::size_t c = 0; c < set.cornerVertex.size(); ++c) {
            const u32 cc = candidate.candidateOfCorner[c];
            if (cc == kInvalidId) {
                continue;
            }
            const u32 v = set.cornerVertex[c];
            if (dropped[candidate.faceOfCandidateCorner[cc]] != 0) {
                fromDropped[v] = 1;
            } else {
                received[v].push_back(resultOf[cc]);
            }
        }
        for (std::vector<u32>& ids : received) {
            ids = sortedUnique(std::move(ids));
        }

        bool progress = false;
        // A group the repair pulled apart splits the way the repair did: its
        // members keyed by what their corners received, the first member's
        // key keeping the group.
        const std::size_t groupCount = partition.groups.size();
        for (std::size_t g = 0; g < groupCount; ++g) {
            const std::vector<u32> members = partition.groups[g];
            if (members.size() < 2) {
                continue;
            }
            std::vector<std::vector<u32>> keys;
            for (const u32 m : members) {
                if (std::find(keys.begin(), keys.end(), received[m]) == keys.end()) {
                    keys.push_back(received[m]);
                }
            }
            for (std::size_t k = 1; k < keys.size(); ++k) {
                std::vector<u32> movers;
                for (const u32 m : members) {
                    if (received[m] == keys[k]) {
                        movers.push_back(m);
                    }
                }
                out.keptNonManifold += static_cast<u32>(movers.size());
                partition.split(movers);
                progress = true;
            }
        }
        // A face the weld would drop keeps its corners apart, where they were.
        for (u32 v = 0; v < vertexCount; ++v) {
            if (fromDropped[v] == 0) {
                continue;
            }
            if (partition.groups[partition.groupOf[v]].size() > 1) {
                partition.split({v});
                ++out.keptNonManifold;
                progress = true;
            }
            if (own[v] == 0) {
                own[v] = 1;
                progress = true;
            }
        }
        if (!progress) {
            // The repair's partition did not separate anything: every group a
            // split touched comes apart whole, which cannot fail.
            for (const VertexSplit& split : trial.log.splits) {
                if (split.original >= vertexCount) {
                    continue;
                }
                const std::vector<u32> members = partition.groups[partition.groupOf[split.original]];
                for (std::size_t i = 1; i < members.size(); ++i) {
                    partition.split({members[i]});
                    ++out.keptNonManifold;
                    progress = true;
                }
            }
        }
        if (!progress) {
            return out;
        }
    }
}

/// Rebuilds @p mesh as @p settled welds it. Returns the rebuild's result and
/// fills the report's maps.
detail::RebuildResult commit(Mesh& mesh, const Settled& settled, u32 faceCount, PrepareReport* report) {
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const u32 vertexCount = static_cast<u32>(settled.survivorOf.size());
    {
        const std::span<Vector3f> positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        for (u32 v = 0; v < vertexCount && v < positions.size() && v < settled.places.size(); ++v) {
            positions[v] = settled.places[v];
        }
    }
    std::vector<u32> newId(vertexCount, kInvalidId);
    RebuildMapping mapping;
    for (u32 v = 0; v < vertexCount; ++v) {
        if (settled.survivorOf[v] == v) {
            newId[v] = static_cast<u32>(mapping.vertexSource.size());
            mapping.vertexSource.push_back(v);
        }
    }
    const Candidate& candidate = settled.candidate;
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    mapping.faces.faceValence = candidate.faces.faceValence;
    mapping.faces.cornerVertex.reserve(candidate.faces.cornerVertex.size());
    for (const u32 v : candidate.faces.cornerVertex) {
        mapping.faces.cornerVertex.push_back(newId[v]);
    }
    mapping.faceSource = candidate.sourceFace;
    mapping.cornerSource = candidate.sourceCorner;
    if (report != nullptr) {
        report->vertexOf.assign(vertexCount, kInvalidId);
        for (u32 v = 0; v < vertexCount; ++v) {
            report->vertexOf[v] = newId[settled.survivorOf[v]];
        }
        report->cornerOf = candidate.candidateOfCorner;
        report->faceOf.assign(faceCount, kInvalidId);
        for (u32 j = 0; j < candidate.sourceFace.size(); ++j) {
            report->faceOf[candidate.sourceFace[j]] = j;
        }
    }
    detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    // The import's log names vertices that are gone, and `rebuild` keeps it
    // whenever its own repair changed nothing, which the fixed point ensures.
    mesh.repairLog = result.repair.log;
    return result;
}

/// Rule 1's effective binding (§2.2): the same bones with weights within
/// `kSkinWeightTolerance`, the same rigid node, and the same `classicBones` pin.
bool sameBinding(const Mesh& mesh, std::span<const std::optional<u32>> rigidOf,
                 std::span<const u16> pins, u32 a, u32 b) {
    if (!mesh.skin.empty()) {
        const std::span<const Influence> left =
            a < mesh.skin.vertexCount() ? mesh.skin.forVertex(a) : std::span<const Influence>{};
        const std::span<const Influence> right =
            b < mesh.skin.vertexCount() ? mesh.skin.forVertex(b) : std::span<const Influence>{};
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (left[i].bone != right[i].bone ||
                std::abs(left[i].weight - right[i].weight) > kSkinWeightTolerance) {
                return false;
            }
        }
    }
    if (a < rigidOf.size() && b < rigidOf.size() && rigidOf[a] != rigidOf[b]) {
        return false;
    }
    if (a < pins.size() && b < pins.size() && pins[a] != pins[b]) {
        return false;
    }
    return true;
}

/// Whether @p a and @p b are corners of one face.
bool shareFace(const FaceTable& table, u32 a, u32 b) {
    const std::vector<u32>& fa = table.facesOf[a];
    const std::vector<u32>& fb = table.facesOf[b];
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < fa.size() && j < fb.size()) {
        if (fa[i] == fb[j]) {
            return true;
        }
        fa[i] < fb[j] ? ++i : ++j;
    }
    return false;
}

/// Whether @p a and @p b are two NON-consecutive corners of one face: a weld of
/// them would repeat a vertex rather than collapse an edge.
bool shareFaceApart(const FaceTable& table, u32 a, u32 b) {
    const FaceSet& set = *table.set;
    for (const u32 f : table.facesOf[a]) {
        if (!std::binary_search(table.facesOf[b].begin(), table.facesOf[b].end(), f)) {
            continue;
        }
        const u32 base = table.base[f];
        const u32 valence = set.faceValence[f];
        for (u32 i = 0; i < valence; ++i) {
            if (set.cornerVertex[base + i] != a) {
                continue;
            }
            const u32 next = set.cornerVertex[base + (i + 1) % valence];
            const u32 prev = set.cornerVertex[base + (i + valence - 1) % valence];
            if (next != b && prev != b) {
                return true;
            }
        }
    }
    return false;
}

/// Greedy sub-groups (§2.2): in ascending order each member joins the first
/// sub-group none of whose members @p conflicts with it.
std::vector<std::vector<u32>> greedy(const std::vector<u32>& members,
                                     const std::function<bool(u32, u32)>& conflicts) {
    std::vector<std::vector<u32>> out;
    for (const u32 m : members) {
        bool placed = false;
        for (std::vector<u32>& group : out) {
            bool clash = false;
            for (const u32 other : group) {
                clash = clash || conflicts(m, other);
            }
            if (!clash) {
                group.push_back(m);
                placed = true;
                break;
            }
        }
        if (!placed) {
            out.push_back({m});
        }
    }
    return out;
}

void setMarker(Mesh& mesh) {
    const std::span<u8> marker =
        mesh.attributes.getOrCreate<u8>(names::kModelled, Domain::Mesh, AttrType::Bool);
    if (!marker.empty()) {
        marker[0] = 1;
    }
}

void identityReport(const Mesh& mesh, PrepareReport& report) {
    const FaceSet& set = mesh.faceSet();
    report.vertexOf.resize(set.vertexCount);
    std::iota(report.vertexOf.begin(), report.vertexOf.end(), 0u);
    report.faceOf.resize(set.faceCount());
    std::iota(report.faceOf.begin(), report.faceOf.end(), 0u);
    report.cornerOf.resize(set.cornerVertex.size());
    std::iota(report.cornerOf.begin(), report.cornerOf.end(), 0u);
}

/// Whether the partition merges anything.
bool mergesAny(const Partition& partition) {
    for (const std::vector<u32>& group : partition.groups) {
        if (group.size() > 1) {
            return true;
        }
    }
    return false;
}

} // namespace

// ============================================================================
// PrepareForModelling (§2.2)
// ============================================================================

PrepareReport PrepareForModelling(Mesh& mesh) {
    PrepareReport report;
    report.tolerance = CoincidenceTolerance(mesh);
    if (IsModelled(mesh)) {
        identityReport(mesh, report);
        return report;
    }
    // Connectivity where the face set builds, so corners and edges carry;
    // compacted, so a face-set index is a slot (§2.1).
    if (mesh.hasConnectivity() || mesh.ensureConnectivity().ok()) {
        GarbageCollect(mesh);
    }
    const FaceSet set = mesh.faceSet();
    const u32 vertexCount = set.vertexCount;
    const std::span<const Vector3f> positions = positionsOf(mesh);
    if (vertexCount == 0 || positions.size() != vertexCount) {
        identityReport(mesh, report);
        setMarker(mesh);
        return report;
    }
    const FaceTable table = tableOf(set);
    const std::vector<u32> sections(mesh.faceSections().begin(), mesh.faceSections().end());

    // The effective binding's two non-skin parts, per vertex.
    std::vector<std::optional<u32>> rigidOf(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v) {
        if (!table.facesOf[v].empty()) {
            const u32 f = table.facesOf[v][0];
            const u32 s = f < sections.size() ? sections[f] : 0u;
            if (s < mesh.sections.size()) {
                rigidOf[v] = mesh.sections[s].rigidNode;
            }
        }
    }
    const std::span<const u16> pins = mesh.attributes.get<const u16>(names::kClassicBones, Domain::Vertex);

    // Clusters: the point table's points, so the two agree (D13).
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    Partition partition(vertexCount);
    std::vector<u32> clusterLead(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v) {
        clusterLead[v] = v;
    }
    for (u32 p = 0; p < points.pointCount; ++p) {
        const std::span<const u32> span = points.membersOf(p);
        std::vector<u32> members(span.begin(), span.end());
        std::sort(members.begin(), members.end());
        for (const u32 m : members) {
            clusterLead[m] = members[0];
        }
        if (members.size() < 2) {
            continue;
        }
        // Rule 1: by binding, each member joining the first group whose first
        // member it agrees with.
        std::vector<std::vector<u32>> byBinding;
        for (const u32 m : members) {
            bool placed = false;
            for (std::vector<u32>& group : byBinding) {
                if (sameBinding(mesh, rigidOf, pins, group[0], m)) {
                    group.push_back(m);
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                byBinding.push_back({m});
            }
        }
        report.keptBinding += static_cast<u32>(byBinding.size() - 1);
        // Rule 2: never two corners of one face.
        for (const std::vector<u32>& group : byBinding) {
            const std::vector<std::vector<u32>> apart =
                greedy(group, [&](u32 a, u32 b) { return shareFace(table, a, b); });
            report.keptFaceCorners += static_cast<u32>(apart.size() - 1);
            for (const std::vector<u32>& sub : apart) {
                partition.join(sub);
            }
        }
    }

    const Chooser lowest = [](const std::vector<u32>& group) { return group[0]; };
    // Every member of a point at its lowest-numbered vertex's position, merged
    // or not; a corner a face would lose its area over keeps its own.
    const Placer snap = [&](const Partition&, std::span<const u8> own, std::vector<Vector3f>& out) {
        out.resize(vertexCount);
        for (u32 v = 0; v < vertexCount; ++v) {
            out[v] = own[v] != 0 ? positions[v] : positions[clusterLead[v]];
        }
    };
    const Settled settled = settle(table, sections, partition, lowest, snap, false);
    report.keptNonManifold = settled.keptNonManifold;

    if (!mergesAny(partition)) {
        // Nothing merges; the snap still lands, so the point table sees the
        // same points after as the weld did.
        const std::span<Vector3f> written = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        for (u32 v = 0; v < vertexCount; ++v) {
            written[v] = settled.places[v];
        }
        // The import's log is cleared whatever the weld did: no exporter of a
        // modelled mesh runs `Unrepair`.
        mesh.repairLog = {};
        identityReport(mesh, report);
        setMarker(mesh);
        return report;
    }

    const detail::RebuildResult rebuilt =
        commit(mesh, settled, static_cast<u32>(set.faceCount()), &report);
    report.verticesWelded = vertexCount - std::as_const(mesh).vertexCount();

    // Edges the weld closed: seam where an authored corner value differs,
    // sharp where the normals break. An edge already interior keeps its flags.
    if (rebuilt.ok && mesh.hasConnectivity()) {
        std::vector<u32> sharps;
        for (const u32 e : rebuilt.closedEdges) {
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            if (NormalsBreakAcross(mesh, h, kWeldSharpAngle)) {
                sharps.push_back(e);
            }
        }
        // `seam` has one writer, so the weld and a UV commit mark by the same
        // rule and neither can drift from the other (EDIT_MODE_UV_PLAN.md P1).
        report.seamsMarked = uv::MarkDelimitSeams(mesh, rebuilt.closedEdges);
        if (!sharps.empty()) {
            const std::span<u8> sharp = mesh.attributes.getOrCreate<u8>(names::kSharp, Domain::Edge, AttrType::Bool);
            for (const u32 e : sharps) {
                sharp[e] = 1;
            }
        }
        report.sharpMarked = static_cast<u32>(sharps.size());
    }
    setMarker(mesh);
    mesh.recomputeBounds();
    return report;
}

// ============================================================================
// The contract (§3.1)
// ============================================================================

std::vector<Resample> ResamplesAt(const Mesh& mesh, VertexId vertex, const Vector3f& to) {
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
    std::unordered_map<u32, SourcePolygon> captured;
    std::unordered_map<u32, bool> holding;
    const auto captureOf = [&](FaceId f) -> const SourcePolygon& {
        auto found = captured.find(f.value());
        if (found == captured.end()) {
            found = captured.emplace(f.value(), CapturePolygon(mesh, f)).first;
        }
        return found->second;
    };
    const auto holds = [&](FaceId f) {
        const auto found = holding.find(f.value());
        if (found != holding.end()) {
            return found->second;
        }
        const SourcePolygon& polygon = captureOf(f);
        std::vector<f32> weights(polygon.cornerCount());
        const bool inside = SampleWeights(polygon.positions, polygon.cut, to, weights);
        holding.emplace(f.value(), inside);
        return inside;
    };
    FaceId vertexSource = topology.face(fan[0]);
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
            for (const HalfedgeId h : group) {
                if (holds(topology.face(h))) {
                    source = topology.face(h);
                    break;
                }
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
                    plan.amountMax = std::min(plan.amountMax, static_cast<f32>(r));
                } else if (r < 0.0) {
                    plan.amountMin = std::max(plan.amountMin, static_cast<f32>(r));
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
    for (const Resample& resample : plan.resamples) {
        if (resample.targetCorners.empty()) {
            continue;
        }
        const u32 vertex = mesh.topology().from(resample.targetCorners[0]).value();
        if (vertex >= positions.size()) {
            continue;
        }
        std::vector<f32> weights(resample.source.cornerCount());
        SampleWeights(resample.source.positions, resample.source.cut, positions[vertex], weights);
        BlendCorners(mesh, resample.source, weights, resample.targetCorners);
        if (resample.targetVertex != kInvalidId && !mesh.skin.empty() &&
            resample.targetVertex < mesh.skin.vertexCount()) {
            const std::vector<Influence> blended = BlendInfluences(resample.source, weights);
            mesh.skin.assignVertex(resample.targetVertex, blended);
        }
    }
    std::vector<u32> movers;
    for (const VertexMotion& m : plan.motions) {
        movers.push_back(m.vertex);
    }
    reshade(mesh, facesAround(mesh, movers));
}

void FinishTool(Mesh& mesh, ModelPlan& plan, f32 creaseAngle) {
    if (plan.refused()) {
        return;
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
            // The crease rule, then the seam derivation (§3.12).
            std::vector<u32> hard;
            std::vector<std::pair<u32, u8>> seams;
            for (const u32 e : live) {
                const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
                const FaceId a = topology.face(h);
                const FaceId b = topology.face(Topology::opposite(h));
                if (a.valid() && b.valid()) {
                    const std::vector<Vector3f> pa = placesOf(positions, loopOf(topology, a));
                    const std::vector<Vector3f> pb = placesOf(positions, loopOf(topology, b));
                    if (angleBetween(newellOf(pa), newellOf(pb)) > creaseAngle) {
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
                const std::span<u8> seam = mesh.attributes.getOrCreate<u8>(names::kSeam, Domain::Edge, AttrType::Bool);
                for (const auto& [e, value] : seams) {
                    seam[e] = value;
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

} // namespace

ModelPlan PlanJoinTriangles(Mesh& mesh, const PointTable& points, const ElementSet& faces,
                            const JoinTrianglesParams& params) {
    (void)points;
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
    const std::vector<u8> chosen = faceMask(readable, faces);
    const std::span<const Vector3f> positions = positionsOf(readable);
    struct Join {
        f64 cost;
        u32 edge;
    };
    std::vector<Join> candidates;
    {
        const Topology& topology = readable.topology();
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            const EdgeId edge(e);
            if (topology.isDeleted(edge) || topology.isBoundary(edge)) {
                continue;
            }
            const HalfedgeId h0 = Topology::halfedge(edge, 0);
            const HalfedgeId h1 = Topology::halfedge(edge, 1);
            const FaceId fa = topology.face(h0);
            const FaceId fb = topology.face(h1);
            if (chosen[fa.index()] == 0 || chosen[fb.index()] == 0 || topology.valence(fa) != 3 ||
                topology.valence(fb) != 3 || !joinable(readable, edge, params.delimitFlagged)) {
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
    }
    std::sort(candidates.begin(), candidates.end(), [](const Join& x, const Join& y) {
        return x.cost != y.cost ? x.cost < y.cost : x.edge < y.edge;
    });

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
    const auto normalOf = [&](FaceId f) {
        return newellOf(placesOf(positions, loopOf(readable.topology(), f)));
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

    std::unordered_map<u32, std::vector<u32>> pending;
    std::vector<u32> cleared;
    std::vector<u32> merged;
    // Refused now, refused later too: the faces only grow.
    const auto tryDissolve = [&](u32 e, bool commitIt, f64& penalty) {
        const Topology& topology = readable.topology();
        const EdgeId edge(e);
        if (topology.isDeleted(edge) || topology.isBoundary(edge)) {
            return false;
        }
        const FaceId fa = topology.face(Topology::halfedge(edge, 0));
        const FaceId fb = topology.face(Topology::halfedge(edge, 1));
        if (fa == fb) {
            return false;
        }
        const std::vector<u32> loop = mergedLoop(topology, edge);
        std::vector<u32> sorted = loop;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            return false;
        }
        const std::vector<u32> row = unionRow(readable, edge, &pending);
        if (!RowValid(loop, positions, row)) {
            return false;
        }
        // The cumulative flatness cap: no triangle of the union strays more
        // than the angle from the merged face's normal.
        const std::array<f64, 3> n = newellOf(placesOf(positions, loop));
        for (std::size_t t = 0; t + 2 < row.size(); t += 3) {
            if (angleBetween(n, triangleNormal(positions[row[t]], positions[row[t + 1]],
                                               positions[row[t + 2]])) > params.angle) {
                return false;
            }
        }
        // Among equal costs: prefer leaving each end's two last edges straight.
        penalty = 0.0;
        for (u32 side = 0; side < 2; ++side) {
            const VertexId v = side == 0 ? topology.from(Topology::halfedge(edge, 0))
                                         : topology.to(Topology::halfedge(edge, 0));
            std::vector<VertexId> others;
            for (const HalfedgeId h : topology.voh(v)) {
                if (Topology::edge(h) != edge) {
                    others.push_back(topology.to(h));
                }
            }
            if (others.size() == 2) {
                const std::array<f64, 3> u = toArray(sub(positions[others[0].value()], positions[v.value()]));
                const std::array<f64, 3> w = toArray(sub(positions[others[1].value()], positions[v.value()]));
                penalty += std::abs(3.14159265358979323846 - angleBetween(u, w));
            }
        }
        if (!commitIt) {
            return true;
        }
        const FaceId lower = fa.value() < fb.value() ? fa : fb;
        detail::Dissolved dissolved = detail::DissolveEdgeDeferred(mesh, edge, lower);
        if (!dissolved.ok) {
            return false;
        }
        pending[dissolved.kept.value()] = row;
        pending.erase(dissolved.gone.value());
        cleared.push_back(dissolved.gone.value());
        merged.push_back(dissolved.kept.value());
        plan.dissolvedEdges.push_back(e);
        return true;
    };
    for (std::size_t first = 0; first < candidates.size();) {
        std::size_t last = first + 1;
        while (last < candidates.size() && candidates[last].cost - candidates[first].cost <= 1e-6) {
            ++last;
        }
        std::vector<u32> bucket;
        for (std::size_t i = first; i < last; ++i) {
            bucket.push_back(candidates[i].edge);
        }
        for (;;) {
            u32 best = kInvalidId;
            f64 bestPenalty = 0.0;
            std::vector<u32> alive;
            for (const u32 e : bucket) {
                f64 penalty = 0.0;
                if (!tryDissolve(e, false, penalty)) {
                    continue;
                }
                alive.push_back(e);
                if (best == kInvalidId || penalty < bestPenalty - 1e-9 ||
                    (std::abs(penalty - bestPenalty) <= 1e-9 && e < best)) {
                    best = e;
                    bestPenalty = penalty;
                }
            }
            if (best == kInvalidId) {
                break;
            }
            f64 penalty = 0.0;
            tryDissolve(best, true, penalty);
            alive.erase(std::remove(alive.begin(), alive.end(), best), alive.end());
            bucket = alive;
        }
        first = last;
    }
    if (!merged.empty()) {
        std::vector<u32> gone = sortedUnique(cleared);
        std::unordered_map<u32, std::vector<u32>> rows;
        for (auto& [face, row] : pending) {
            if (!readable.topology().isDeleted(FaceId(face))) {
                rows[face] = row;
            }
        }
        storeRows(mesh, rows, gone);
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

/// The face set with every corner @p rename[v] instead of v (kInvalidId keeps
/// it), rebuilt without the renamed-away vertices, when a trial repair of it
/// changes nothing. False, the mesh untouched, when it would.
bool renameAndRebuild(Mesh& mesh, const std::vector<u32>& rename) {
    const FaceSet set = mesh.faceSet();
    const u32 vertexCount = set.vertexCount;
    std::vector<u32> survivorOf(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v) {
        survivorOf[v] = v < rename.size() && rename[v] != kInvalidId ? rename[v] : v;
    }
    const FaceTable table = tableOf(set);
    const std::vector<u32> sections(mesh.faceSections().begin(), mesh.faceSections().end());
    Settled settled;
    settled.survivorOf = survivorOf;
    const std::span<const Vector3f> positions = positionsOf(mesh);
    settled.places.assign(positions.begin(), positions.end());
    settled.candidate = candidateOf(table, sections, survivorOf, false);
    if (settled.candidate.sourceFace.size() != set.faceCount()) {
        return false;
    }
    for (std::size_t f = 0; f < set.faceCount(); ++f) {
        std::vector<u32> corners(settled.candidate.faces.cornerVertex.begin() + table.base[f],
                                 settled.candidate.faces.cornerVertex.begin() + table.base[f + 1]);
        std::sort(corners.begin(), corners.end());
        if (std::adjacent_find(corners.begin(), corners.end()) != corners.end()) {
            return false;
        }
    }
    const RepairResult trial = Repair(settled.candidate.faces, settled.candidate.sections, settled.places);
    if (trial.changed) {
        return false;
    }
    commit(mesh, settled, static_cast<u32>(set.faceCount()), nullptr);
    return true;
}

/// Target Weld's notch (§3.6): u and v both on one border loop with one vertex x
/// between them, sharing no other neighbour and no face, on a loop of more than
/// three edges.
bool isNotch(const Topology& topology, VertexId u, VertexId v) {
    for (const HalfedgeId h : topology.voh(u)) {
        const HalfedgeId back = Topology::opposite(h);
        // Either way round the loop: u -> x -> v, or v -> x -> u.
        for (const HalfedgeId step : {h, back}) {
            if (topology.face(step).valid()) {
                continue;
            }
            const HalfedgeId onward = step == h ? topology.next(h) : topology.prev(back);
            const VertexId far = step == h ? topology.to(onward) : topology.from(onward);
            if (topology.face(onward).valid() || far != v) {
                continue;
            }
            const VertexId x = topology.to(h);
            // x is the only shared neighbour, and no face holds both.
            for (const VertexId n : topology.vv(u)) {
                if (n == x) {
                    continue;
                }
                for (const VertexId m : topology.vv(v)) {
                    if (m == n) {
                        return false;
                    }
                }
            }
            for (const FaceId f : topology.vf(u)) {
                if (!f.valid()) {
                    continue;
                }
                for (const VertexId w : topology.fv(f)) {
                    if (w == v) {
                        return false;
                    }
                }
            }
            u32 length = 0;
            HalfedgeId walk = step;
            do {
                ++length;
                walk = topology.next(walk);
            } while (walk != step && length < (1u << 24));
            return length > 3;
        }
    }
    return false;
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
    if (std::find(from.begin(), from.end(), target.value()) != from.end()) {
        plan.refusal = ModelRefusal::NotAdjacent;
        return plan;
    }
    // Every source member along an edge: collapse each onto its target member.
    std::vector<std::pair<u32, u32>> pairs;
    for (const u32 s : from) {
        for (const u32 t : onto) {
            if (topology.findHalfedge(VertexId(s), VertexId(t)).valid()) {
                pairs.push_back({s, t});
                break;
            }
        }
    }
    if (pairs.size() == from.size()) {
        // Refusal is total: on a copy, then kept only when every collapse held.
        Mesh copy = readable;
        for (const auto& [s, t] : pairs) {
            const HalfedgeId h = std::as_const(copy).topology().findHalfedge(VertexId(s), VertexId(t));
            if (!h.valid() || !IsCollapseLegal(copy, h) || !CollapseEdge(copy, h)) {
                plan.refusal = ModelRefusal::TargetCollapseIllegal;
                return plan;
            }
        }
        mesh = std::move(copy);
        plan.selection.vertices = onto;
        plan.changed = static_cast<u32>(pairs.size());
        return plan;
    }
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
    if (from.size() == 1 && onto.size() == 1 &&
        !topology.findHalfedge(source, target).valid()) {
        // The notch, when it is one: manifold in one step.
        bool onOneLoop = false;
        for (const HalfedgeId h : topology.voh(source)) {
            for (const HalfedgeId side : {h, Topology::opposite(h)}) {
                if (topology.face(side).valid()) {
                    continue;
                }
                const HalfedgeId onward = side == h ? topology.next(h) : topology.prev(side);
                const VertexId far = side == h ? topology.to(onward) : topology.from(onward);
                onOneLoop = onOneLoop || (!topology.face(onward).valid() && far == target);
            }
        }
        if (onOneLoop) {
            if (!isNotch(topology, source, target)) {
                plan.refusal = ModelRefusal::WouldPinch;
                return plan;
            }
            std::vector<u32> rename(topology.vertexCount(), kInvalidId);
            rename[source.value()] = target.value();
            if (!renameAndRebuild(mesh, rename)) {
                plan.refusal = ModelRefusal::WouldPinch;
                return plan;
            }
            plan.selection.vertices = {target.value() > source.value() ? target.value() - 1 : target.value()};
            plan.changed = 1;
            return plan;
        }
    }
    plan.refusal = ModelRefusal::CrackNeedsWeld;
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
    std::vector<u32> rename(topology.vertexCount(), kInvalidId);
    if (s0 != t1) {
        rename[s0] = t1;
    }
    if (s1 != t0) {
        rename[s1] = t0;
    }
    if (s0 == t0 || s1 == t1) {
        plan.refusal = ModelRefusal::WindingDisagrees;
        return plan;
    }
    // The notch's condition at each end: no face may hold both vertices a
    // weld joins, or the crack's last edge would make a pillow.
    for (u32 v = 0; v < rename.size(); ++v) {
        if (rename[v] == kInvalidId) {
            continue;
        }
        for (const FaceId f : topology.vf(VertexId(v))) {
            if (!f.valid()) {
                continue;
            }
            for (const VertexId w : topology.fv(f)) {
                if (w.value() == rename[v]) {
                    plan.refusal = ModelRefusal::WouldPinch;
                    return plan;
                }
            }
        }
    }
    if (!renameAndRebuild(mesh, rename)) {
        plan.refusal = ModelRefusal::WouldPinch;
        return plan;
    }
    plan.changed = 1;
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
    for (const std::vector<u32>& cluster : clusters) {
        const std::vector<std::vector<u32>> apart =
            greedy(cluster, [&](u32 a, u32 b) { return shareFaceApart(table, a, b); });
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
    return plan;
}

/// Clusters over @p vertices: pairs within @p threshold, joined transitively,
/// and every member of one point together.
std::vector<std::vector<u32>> clustersOf(const Mesh& mesh, const PointTable& points,
                                         const std::vector<u32>& vertices, f32 threshold) {
    const std::span<const Vector3f> positions = positionsOf(mesh);
    std::unordered_map<u32, u32> parent;
    const std::function<u32(u32)> find = [&](u32 v) {
        u32 root = v;
        while (parent[root] != root) {
            root = parent[root];
        }
        while (parent[v] != root) {
            const u32 up = parent[v];
            parent[v] = root;
            v = up;
        }
        return root;
    };
    const auto unite = [&](u32 a, u32 b) {
        const u32 ra = find(a);
        const u32 rb = find(b);
        if (ra != rb) {
            parent[std::max(ra, rb)] = std::min(ra, rb);
        }
    };
    for (const u32 v : vertices) {
        parent[v] = v;
    }
    const u32 vertexCount = mesh.topology().vertexCount();
    const bool covers = points.pointOf.size() == vertexCount;
    for (const u32 v : vertices) {
        if (covers) {
            for (const u32 m : points.membersOf(points.pointOf[v])) {
                if (parent.count(m) != 0) {
                    unite(v, m);
                }
            }
        }
    }
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
        std::unordered_map<Key, std::vector<u32>, KeyHash> grid; // looked up, never iterated
        const auto cellOf = [&](const Vector3f& p) {
            return Key{static_cast<i64>(std::floor(p.x / threshold)), static_cast<i64>(std::floor(p.y / threshold)),
                       static_cast<i64>(std::floor(p.z / threshold))};
        };
        for (const u32 v : vertices) {
            const Key key = cellOf(positions[v]);
            for (i64 dx = -1; dx <= 1; ++dx) {
                for (i64 dy = -1; dy <= 1; ++dy) {
                    for (i64 dz = -1; dz <= 1; ++dz) {
                        const auto found = grid.find(Key{key.x + dx, key.y + dy, key.z + dz});
                        if (found == grid.end()) {
                            continue;
                        }
                        for (const u32 other : found->second) {
                            if (length64(sub(positions[v], positions[other])) <= threshold) {
                                unite(v, other);
                            }
                        }
                    }
                }
            }
            grid[key].push_back(v);
        }
    }
    std::unordered_map<u32, std::vector<u32>> byRoot;
    std::vector<u32> roots;
    for (const u32 v : vertices) {
        const u32 root = find(v);
        if (byRoot.count(root) == 0) {
            roots.push_back(root);
        }
        byRoot[root].push_back(v);
    }
    std::sort(roots.begin(), roots.end());
    std::vector<std::vector<u32>> out;
    for (const u32 root : roots) {
        std::vector<u32> cluster = sortedUnique(byRoot[root]);
        if (cluster.size() > 1) {
            out.push_back(std::move(cluster));
        }
    }
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
    if (mesh.hasConnectivity() || mesh.ensureConnectivity().ok()) {
        GarbageCollect(mesh);
    } else {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const std::vector<u32> chosen = pointClosed(mesh, points, selection.vertices);
    return weldClusters(mesh, clustersOf(mesh, points, chosen, threshold));
}

ModelPlan PlanWeld(Mesh& mesh, const PointTable& points, const ElementSet& sources,
                   const ElementSet& targets, f32 threshold) {
    ModelPlan plan;
    if (sources.vertices.empty() || targets.vertices.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (mesh.hasConnectivity() || mesh.ensureConnectivity().ok()) {
        GarbageCollect(mesh);
    } else {
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
    // One to one: each source vertex, in ascending order, onto the target
    // vertex that shares its binding, then the nearest, within the threshold
    // and not yet taken. Never two sources together, so the twins of a crease
    // moved as one point stay apart (each onto its own twin, or one onto the
    // target and the rest beside it).
    const std::span<const u16> pins = mesh.attributes.get<const u16>(names::kClassicBones, Domain::Vertex);
    std::vector<u8> taken(positions.size(), 0);
    std::vector<std::vector<u32>> clusters;
    for (const u32 s : from) {
        u32 best = kInvalidId;
        bool bestBound = false;
        f64 bestDistance = 0.0;
        for (const u32 t : onto) {
            if (taken[t] != 0) {
                continue;
            }
            const f64 d = length64(sub(positions[s], positions[t]));
            if (d > threshold) {
                continue;
            }
            const bool bound = sameBinding(mesh, {}, pins, s, t);
            if (best == kInvalidId || (bound && !bestBound) ||
                (bound == bestBound && d < bestDistance)) {
                best = t;
                bestBound = bound;
                bestDistance = d;
            }
        }
        if (best == kInvalidId) {
            continue;
        }
        taken[best] = 1;
        clusters.push_back(sortedUnique({s, best}));
    }
    return weldClusters(mesh, clusters);
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

/// Why `DissolveEdge` refused @p edge, in §3.7's words.
ModelRefusal whyNotDissolved(const Mesh& mesh, EdgeId edge) {
    const Topology& topology = mesh.topology();
    if (topology.isBoundary(edge)) {
        return ModelRefusal::BorderEdgeRemove;
    }
    const HalfedgeId h = Topology::halfedge(edge, 0);
    const FaceId a = topology.face(h), b = topology.face(Topology::opposite(h));
    if (a == b) {
        return ModelRefusal::WouldFold; // both sides are one face
    }
    const std::span<const u32> sections = mesh.faceSections();
    if (a.value() < sections.size() && b.value() < sections.size() && sections[a.value()] != sections[b.value()]) {
        return ModelRefusal::CrossesSection;
    }
    const std::vector<u32> loop = mergedLoop(topology, edge);
    std::vector<u32> sorted = loop;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return ModelRefusal::WouldRepeatVertex;
    }
    if (faceExists(mesh, loop, a, b)) {
        return ModelRefusal::WouldDuplicateFace;
    }
    return ModelRefusal::WouldFold;
}

/// Collapse's UV rule (§3.7): the corners at @p vertices, grouped into islands
/// -- two corners of one fan are one island while their UVs agree across the
/// edge between them -- and each island's corners set to the centre of its own
/// UV bounding box, so the seams stay where they were.
void averageIslandUvs(Mesh& mesh, const std::vector<u32>& vertices) {
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 set = 0; set < 8; ++set) {
        const std::string name = names::uv(set);
        if (!mesh.attributes.has(name, Domain::Halfedge)) {
            break;
        }
        const std::span<Vector2f> uv = mesh.attributes.get<Vector2f>(name, Domain::Halfedge);
        std::vector<u32> corners;
        for (const u32 v : vertices) {
            if (v >= topology.vertexCount() || topology.isDeleted(VertexId(v))) {
                continue;
            }
            for (const HalfedgeId h : topology.voh(VertexId(v))) {
                if (!topology.isBoundary(h) && h.index() < uv.size()) {
                    corners.push_back(static_cast<u32>(h.index()));
                }
            }
        }
        corners = sortedUnique(std::move(corners));
        std::unordered_map<u32, u32> parent;
        for (const u32 c : corners) {
            parent[c] = c;
        }
        const std::function<u32(u32)> find = [&](u32 c) {
            u32 root = c;
            while (parent[root] != root) {
                root = parent[root];
            }
            while (parent[c] != root) {
                const u32 next = parent[c];
                parent[c] = root;
                c = next;
            }
            return root;
        };
        // Around one vertex: two corners across an edge are one island while
        // their UVs agree there.
        for (const u32 c : corners) {
            const HalfedgeId opposite = Topology::opposite(HalfedgeId(c));
            if (topology.isBoundary(opposite)) {
                continue;
            }
            const u32 other = static_cast<u32>(topology.next(opposite).index());
            if (parent.find(other) == parent.end() || other >= uv.size()) {
                continue;
            }
            if (uv[c] == uv[other]) {
                parent[find(c)] = find(other);
            }
        }
        // And within one face: its corners are one patch of the map, so the
        // corners it holds at two of the part's vertices are one island.
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            u32 first = kInvalidId;
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                const u32 c = static_cast<u32>(h.index());
                if (parent.find(c) == parent.end()) {
                    continue;
                }
                if (first == kInvalidId) {
                    first = c;
                } else {
                    parent[find(c)] = find(first);
                }
            }
        }
        std::unordered_map<u32, std::vector<u32>> islands;
        for (const u32 c : corners) {
            islands[find(c)].push_back(c);
        }
        for (const auto& entry : islands) {
            Vector2f lo = uv[entry.second.front()], hi = lo;
            for (const u32 c : entry.second) {
                lo = {std::min(lo.x, uv[c].x), std::min(lo.y, uv[c].y)};
                hi = {std::max(hi.x, uv[c].x), std::max(hi.y, uv[c].y)};
            }
            const Vector2f centre{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f};
            for (const u32 c : entry.second) {
                uv[c] = centre;
            }
        }
    }
}

/// The parts of @p chosen, joined across the mesh's own edges whose two ends
/// are both chosen and across the members of one point (§3.7).
std::vector<std::vector<u32>> partsOf(const Mesh& mesh, const PointTable& points,
                                      const std::vector<u32>& chosen) {
    std::unordered_map<u32, u32> parent;
    for (const u32 v : chosen) {
        parent[v] = v;
    }
    const std::function<u32(u32)> find = [&](u32 v) {
        u32 root = v;
        while (parent[root] != root) {
            root = parent[root];
        }
        while (parent[v] != root) {
            const u32 next = parent[v];
            parent[v] = root;
            v = next;
        }
        return root;
    };
    const auto join = [&](u32 a, u32 b) {
        if (parent.count(a) != 0 && parent.count(b) != 0) {
            parent[find(a)] = find(b);
        }
    };
    const Topology& topology = mesh.topology();
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (topology.isDeleted(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        join(topology.from(h).value(), topology.to(h).value());
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
        if (entry.second.size() > 1) {
            parts.push_back(sortedUnique(std::move(entry.second)));
        }
    }
    std::sort(parts.begin(), parts.end());
    return parts;
}

} // namespace

ModelPlan PlanCollapse(Mesh& mesh, const PointTable& points, const ElementSet& selection) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const std::vector<u32> chosen = verticesOf(mesh, points, selection);
    if (chosen.empty()) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    const std::vector<std::vector<u32>> parts = partsOf(mesh, points, chosen);
    if (parts.empty()) {
        return plan; // one vertex is already at its own centroid
    }
    // Planned on a copy: an illegal step refuses the whole (§3.7).
    Mesh copy = mesh;
    for (const std::vector<u32>& part : parts) {
        const std::span<const Vector3f> places = positionsOf(copy);
        Vector3f centre{0, 0, 0};
        for (const u32 v : part) {
            centre = centre + places[v];
        }
        centre = centre * (1.0f / static_cast<f32>(part.size()));
        u32 survivor = part.front();
        f64 nearest = length64(sub(places[survivor], centre));
        for (const u32 v : part) {
            const f64 d = length64(sub(places[v], centre));
            if (d < nearest) {
                nearest = d;
                survivor = v;
            }
        }
        // The part's skin and its islands' UVs, written on every member first,
        // so the survivor carries them however the collapses go.
        if (!copy.skin.empty()) {
            std::vector<std::vector<Influence>> sources;
            std::vector<f32> weights;
            for (const u32 v : part) {
                const std::span<const Influence> theirs = std::as_const(copy).skin.forVertex(v);
                sources.emplace_back(theirs.begin(), theirs.end());
                weights.push_back(1.0f);
            }
            const std::vector<Influence> blended = BlendInfluences(sources, weights);
            for (const u32 v : part) {
                copy.skin.assignVertex(v, blended);
            }
        }
        averageIslandUvs(copy, part);
        // Every other member collapses into the survivor, along an edge, the
        // nearest first; a member with no legal step left refuses the whole.
        std::vector<u32> left;
        for (const u32 v : part) {
            if (v != survivor) {
                left.push_back(v);
            }
        }
        while (!left.empty()) {
            bool moved = false;
            for (std::size_t i = 0; i < left.size() && !moved; ++i) {
                const Topology& topology = std::as_const(copy).topology();
                const u32 v = left[i];
                if (topology.isDeleted(VertexId(v))) {
                    left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
                    moved = true;
                    break;
                }
                // Toward the survivor where they share an edge, else toward
                // another member: the part comes together either way.
                HalfedgeId step = topology.findHalfedge(VertexId(v), VertexId(survivor));
                if (!step.valid()) {
                    for (const u32 other : part) {
                        if (other != v && !topology.isDeleted(VertexId(other)) && !step.valid() &&
                            std::find(left.begin(), left.end(), other) == left.end()) {
                            step = topology.findHalfedge(VertexId(v), VertexId(other));
                        }
                    }
                }
                if (!step.valid() || !IsCollapseLegal(copy, step) || !CollapseEdge(copy, step)) {
                    continue;
                }
                left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
                ++plan.changed;
                moved = true;
            }
            if (!moved) {
                plan = ModelPlan{};
                plan.refusal = ModelRefusal::CollapseIllegal;
                return plan;
            }
        }
        // The survivor sits at the part's centroid.
        const std::span<Vector3f> written = copy.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        written[survivor] = centre;
        plan.selection.vertices.push_back(survivor);
    }
    mesh = std::move(copy);
    plan.selection.normalise();
    plan.renumbers = true;
    return plan;
}

/// The outer ring of @p vertex's fan, in loop order, and the faces around it:
/// what Remove replaces them with (§3.7). `ok` is false for a vertex whose fan
/// is not a simple walk.
struct Ring {
    bool ok = false;
    std::vector<u32> loop;    ///< The ring's vertices, in order.
    std::vector<u32> corners; ///< Per ring vertex, the corner it keeps (a halfedge).
    std::vector<u32> faces;   ///< The fan's faces, ascending.
};

Ring ringOf(const Mesh& mesh, VertexId vertex) {
    Ring ring;
    const Topology& topology = mesh.topology();
    // The fan's first face: the one after the hole on a border vertex, any on
    // an interior one.
    HalfedgeId start;
    for (const HalfedgeId h : topology.voh(vertex)) {
        if (topology.isBoundary(h)) {
            continue;
        }
        if (!start.valid()) {
            start = h;
        }
        if (topology.isBoundary(Topology::opposite(topology.prev(h)))) {
            start = h; // nothing before it: the fan begins here
        }
    }
    if (!start.valid()) {
        return ring;
    }
    HalfedgeId h = start;
    std::vector<u32> faces;
    do {
        faces.push_back(topology.face(h).value());
        // This face's share of the ring: from the corner after the vertex
        // round to the one before it, each keeping its own corner.
        HalfedgeId step = topology.next(h);
        while (step.valid() && topology.from(step).value() != vertex.value()) {
            const u32 at = topology.from(step).value();
            if (ring.loop.empty() || ring.loop.back() != at) {
                ring.loop.push_back(at);
                ring.corners.push_back(static_cast<u32>(step.index()));
            }
            step = topology.next(step);
        }
        // `step` leaves the vertex again: the next face is across the edge the
        // one before it arrived on.
        const HalfedgeId arriving = topology.prev(h);
        h = Topology::opposite(arriving);
        if (topology.isBoundary(h)) {
            break; // a border vertex's fan ends here
        }
    } while (h.valid() && h != start && faces.size() <= topology.faceCount());
    if (ring.loop.size() > 1 && ring.loop.front() == ring.loop.back()) {
        ring.loop.pop_back();
        ring.corners.pop_back();
    }
    ring.faces = sortedUnique(std::move(faces));
    ring.ok = ring.loop.size() >= 3 && !ring.faces.empty();
    return ring;
}

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
    ElementSet asked;
    asked.vertices = selection.vertices;
    const std::vector<u32> vertices = verticesOf(mesh, points, asked);
    Mesh copy = mesh;
    std::vector<u32> left; // the vertices a dissolve left behind, for Clean vertices
    // The edges: a dissolve each; a border edge has nothing to close.
    for (const u32 e : sortedUnique(selection.edges)) {
        const Topology& topology = std::as_const(copy).topology();
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e))) {
            continue;
        }
        if (topology.isBoundary(EdgeId(e))) {
            plan.refusal = ModelRefusal::BorderEdgeRemove;
            return plan;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        left.push_back(topology.from(h).value());
        left.push_back(topology.to(h).value());
        if (!DissolveEdge(copy, EdgeId(e))) {
            plan.refusal = whyNotDissolved(copy, EdgeId(e));
            return plan;
        }
        ++plan.changed;
    }
    // The vertices: the faces around each replaced by one face over its ring,
    // every outer corner keeping its own values (§3.7).
    for (const u32 v : vertices) {
        const Topology& topology = std::as_const(copy).topology();
        if (v >= topology.vertexCount() || topology.isDeleted(VertexId(v))) {
            continue;
        }
        if (topology.valence(VertexId(v)) == 2) {
            // A valence-2 vertex is its own case: its two edges merge.
            if (!DissolveVertex(copy, VertexId(v))) {
                plan.refusal = ModelRefusal::DissolveVertexIllegal;
                return plan;
            }
            ++plan.changed;
            continue;
        }
        const Ring ring = ringOf(copy, VertexId(v));
        if (!ring.ok) {
            plan.refusal = ModelRefusal::DissolveVertexIllegal;
            return plan;
        }
        {
            std::vector<u32> sorted = ring.loop;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
                plan.refusal = ModelRefusal::WouldRepeatVertex;
                return plan;
            }
        }
        {
            const std::span<const u32> sections = copy.faceSections();
            for (const u32 f : ring.faces) {
                if (f < sections.size() && sections[f] != sections[ring.faces.front()]) {
                    plan.refusal = ModelRefusal::CrossesSection;
                    return plan;
                }
            }
        }
        {
            const std::span<const Vector3f> places = positionsOf(copy);
            std::vector<Vector3f> shape;
            for (const u32 r : ring.loop) {
                shape.push_back(places[r]);
            }
            if (foldsOver(shape)) {
                plan.refusal = ModelRefusal::WouldFold; // a knife-edge crease
                return plan;
            }
        }
        // The rebuild: the fan's faces out, one face over the ring in, and
        // every other face as it was.
        const std::vector<u32> snapshot = detail::snapshotCornersBuilt(copy);
        std::unordered_map<u32, u32> cornerOfHalfedge;
        for (u32 c = 0; c < snapshot.size(); ++c) {
            cornerOfHalfedge[snapshot[c]] = c;
        }
        detail::RebuildMapping mapping;
        std::vector<u32> newId(topology.vertexCount(), kInvalidId);
        for (u32 x = 0; x < topology.vertexCount(); ++x) {
            if (!topology.isDeleted(VertexId(x)) && x != v) {
                newId[x] = static_cast<u32>(mapping.vertexSource.size());
                mapping.vertexSource.push_back(x);
            }
        }
        mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
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
            if (std::binary_search(ring.faces.begin(), ring.faces.end(), f)) {
                continue;
            }
            mapping.faces.faceValence.push_back(static_cast<u32>(loop.size()));
            for (std::size_t i = 0; i < loop.size(); ++i) {
                mapping.faces.cornerVertex.push_back(newId[loop[i]]);
                mapping.cornerSource.push_back(mine[i]);
            }
            mapping.faceSource.push_back(f);
        }
        const u32 slot = static_cast<u32>(mapping.faces.faceValence.size());
        mapping.faces.faceValence.push_back(static_cast<u32>(ring.loop.size()));
        for (std::size_t i = 0; i < ring.loop.size(); ++i) {
            mapping.faces.cornerVertex.push_back(newId[ring.loop[i]]);
            const auto at = cornerOfHalfedge.find(ring.corners[i]);
            mapping.cornerSource.push_back(at == cornerOfHalfedge.end() ? kInvalidId : at->second);
        }
        mapping.faceSource.push_back(ring.faces.front()); // its section and smoothing group
        const detail::RebuildResult result = detail::rebuild(copy, std::move(mapping), snapshot);
        if (!result.ok || result.repair.changed) {
            plan.refusal = ModelRefusal::WouldFold;
            return plan;
        }
        plan.changedFaces.push_back(slot);
        ++plan.changed;
        left.clear(); // the ids the clean pass held are another mesh's now
    }
    // Clean vertices (3ds Max's Ctrl+Backspace): the valence-2 vertices the
    // dissolves left, and only those.
    if (params.cleanVertices) {
        for (const u32 v : sortedUnique(std::move(left))) {
            const Topology& now = std::as_const(copy).topology();
            if (v < now.vertexCount() && !now.isDeleted(VertexId(v)) && now.valence(VertexId(v)) == 2) {
                plan.changed += DissolveVertex(copy, VertexId(v)) ? 1u : 0u;
            }
        }
    }
    if (plan.changed == 0) {
        return plan;
    }
    mesh = std::move(copy);
    plan.renumbers = true;
    return plan;
}

ModelPlan PlanDelete(Mesh& mesh, const ElementSet& faces) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u8> gone(topology.faceCount(), 0);
    u32 chosen = 0, live = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        live += topology.isDeleted(FaceId(f)) ? 0u : 1u;
    }
    for (const u32 f : sortedUnique(faces.faces)) {
        if (f < gone.size() && !topology.isDeleted(FaceId(f)) && gone[f] == 0) {
            gone[f] = 1;
            ++chosen;
        }
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
    plan.changed = chosen;
    return plan;
}

ModelPlan PlanCap(Mesh& mesh, const PointTable& points, EdgeId edge) {
    ModelPlan plan;
    (void)points;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    if (edge.value() >= topology.edgeCount() || topology.isDeleted(edge) || !topology.isBoundary(edge)) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    // The border loop, walked from its boundary halfedge: its vertices in the
    // loop's own direction, and the rim corner beside each.
    HalfedgeId start = Topology::halfedge(edge, 0);
    if (!topology.isBoundary(start)) {
        start = Topology::opposite(start);
    }
    std::vector<u32> loop;
    std::vector<u32> rim;
    std::vector<u32> rimFaces;
    HalfedgeId walk = start;
    do {
        loop.push_back(topology.from(walk).value());
        // The rim corner at that vertex: the halfedge leaving it inside the
        // face beside the border edge.
        const HalfedgeId inside = topology.next(Topology::opposite(walk));
        rim.push_back(static_cast<u32>(inside.index()));
        rimFaces.push_back(topology.face(Topology::opposite(walk)).value());
        walk = topology.next(walk);
    } while (walk.valid() && walk != start && loop.size() <= topology.halfedgeCount());
    if (loop.size() < 3) {
        plan.refusal = ModelRefusal::CapTooShort;
        return plan;
    }
    {
        const std::span<const Vector3f> places = positionsOf(mesh);
        std::vector<Vector3f> shape;
        for (const u32 v : loop) {
            shape.push_back(places[v]);
        }
        if (foldsOver(shape)) {
            plan.refusal = ModelRefusal::WouldFold;
            return plan;
        }
    }
    if (faceExists(mesh, loop)) {
        // Capping a lone card would make a two-face pillow, which the next
        // rebuild's duplicate-face rule splits apart again.
        plan.refusal = ModelRefusal::WouldDuplicateFace;
        return plan;
    }
    // The cap closes the loop, so its corners run the other way; each copies
    // the rim corner at its vertex, and the face takes the lowest-numbered rim
    // face's section and smoothing group.
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    std::unordered_map<u32, u32> cornerOfHalfedge;
    for (u32 c = 0; c < snapshot.size(); ++c) {
        cornerOfHalfedge[snapshot[c]] = c;
    }
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    // The boundary loop runs the other way from the faces beside it, so the
    // cap takes it as it is and faces outward with them.
    mapping.faces.faceValence.push_back(static_cast<u32>(loop.size()));
    for (std::size_t i = 0; i < loop.size(); ++i) {
        mapping.faces.cornerVertex.push_back(loop[i]);
        const auto at = cornerOfHalfedge.find(rim[i]);
        mapping.cornerSource.push_back(at == cornerOfHalfedge.end() ? kInvalidId : at->second);
    }
    mapping.faceSource.push_back(*std::min_element(rimFaces.begin(), rimFaces.end()));
    const u32 slot = static_cast<u32>(mapping.faces.faceValence.size() - 1);
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changed = 1;
    plan.changedFaces.push_back(slot); // its row is the finish's to materialise
    plan.selection.faces.push_back(slot);
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
    std::unordered_map<u64, u32> copyOfFan;
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (topology.isDeleted(VertexId(v))) {
            continue;
        }
        // A vertex every one of whose faces flips stays one vertex, on the
        // mesh's own border or not: there is nothing to detach it from.
        std::vector<u32> around;
        bool border = false;
        for (const HalfedgeId h : topology.voh(VertexId(v))) {
            if (topology.isBoundary(h)) {
                continue;
            }
            const u32 f = topology.face(h).value();
            around.push_back(f);
            border = border || flipped[f] == 0;
        }
        if (!border || around.empty()) {
            continue;
        }
        // The fan's runs: rotate so one never wraps, then a copy per run. The
        // flipped faces take the copies and everything else keeps the vertex,
        // which is what detaches the region along its border.
        std::size_t start = 0;
        for (std::size_t i = 0; i < around.size(); ++i) {
            if (flipped[around[i]] == 0) {
                start = (i + 1) % around.size();
                break;
            }
        }
        u32 current = kInvalidId;
        for (std::size_t i = 0; i < around.size(); ++i) {
            const u32 f = around[(start + i) % around.size()];
            if (flipped[f] == 0) {
                current = kInvalidId;
                continue;
            }
            if (current == kInvalidId) {
                current = static_cast<u32>(mapping.vertexSource.size());
                mapping.vertexSource.push_back(v);
                freshOf.push_back(current);
            }
            copyOfFan[(static_cast<u64>(v) << 32) | f] = current;
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
        // Its stored triangles, renamed and reversed (§3.10).
        const std::span<const u32> row = mesh.triangulation.row(f);
        if (!row.empty()) {
            std::vector<u32> made;
            for (std::size_t t = 0; t + 2 < row.size(); t += 3) {
                made.push_back(nameOf(row[t + 2]));
                made.push_back(nameOf(row[t + 1]));
                made.push_back(nameOf(row[t]));
            }
            rows.emplace_back(slot, std::move(made));
        }
        plan.changedFaces.push_back(slot);
    }
    const u32 slotCount = static_cast<u32>(mapping.faces.faceValence.size());
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold; // the region would not detach
        return plan;
    }
    // The copies are their own points: a fresh `mergeGroup` each, so nothing
    // welds them back together.
    if (!freshOf.empty()) {
        const std::span<u32> groups = MergeGroupsOf(mesh);
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
        const Topology& now = std::as_const(mesh).topology();
        const std::span<Vector3f> normals = mesh.attributes.get<Vector3f>(names::kNormal, Domain::Halfedge);
        const std::span<Vector4f> tangents = mesh.attributes.get<Vector4f>(names::kTangent, Domain::Halfedge);
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
            }
        }
    }
    for (const auto& entry : rows) {
        mesh.triangulation.setRow(entry.first, entry.second, slotCount);
    }
    plan.changed = chosen;
    plan.selection.faces = sortedUnique(plan.changedFaces);
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

/// The mesh's border edges keyed by their ends' points: two on one key are
/// twins, which is all the weld left of a crease it kept apart (§2.2).
class Twins {
public:
    Twins(const Mesh& mesh, const PointTable& points) {
        const Topology& topology = mesh.topology();
        keyOf_.assign(topology.edgeCount(), 0);
        const auto pointOf = [&](u32 vertex) {
            return points.pointOf.size() == topology.vertexCount() ? points.pointOf[vertex] : vertex;
        };
        for (u32 e = 0; e < topology.edgeCount(); ++e) {
            if (topology.isDeleted(EdgeId(e)) || !topology.isBoundary(EdgeId(e))) {
                continue;
            }
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            const u64 key = pointPair(pointOf(topology.from(h).value()), pointOf(topology.to(h).value()));
            keyOf_[e] = key;
            byPair_[key].push_back(e);
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
            if (other != edge) {
                out.push_back(other);
            }
        }
        return out;
    }

private:
    std::vector<u64> keyOf_;
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
    std::vector<u8> onBoundary(topology.vertexCount(), 0);
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
            onBoundary[topology.from(h).value()] = 1;
            onBoundary[topology.to(h).value()] = 1;
        }
    }
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
        if (onBoundary[v] == 0) {
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

/// The unit normals of the selected faces around each member of every point,
/// summed and normalised: one direction per point, so a crease's twins move
/// together (§3.8's Twins).
std::vector<Vector3f> localNormals(const Mesh& mesh, const PointTable& points, const std::vector<u8>& chosen) {
    const Topology& topology = mesh.topology();
    std::vector<Vector3f> sums(topology.vertexCount(), Vector3f{0.0f, 0.0f, 0.0f});
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (f >= chosen.size() || chosen[f] == 0 || topology.isDeleted(FaceId(f))) {
            continue;
        }
        const Vector3f normal = unitOr(faceArea(mesh, FaceId(f)), Vector3f{0.0f, 0.0f, 1.0f});
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            sums[topology.from(h).value()] = sums[topology.from(h).value()] + normal;
        }
    }
    // One sum per point, shared out again.
    std::vector<Vector3f> out(sums.size(), Vector3f{0.0f, 0.0f, 1.0f});
    std::vector<u8> done(sums.size(), 0);
    for (u32 v = 0; v < sums.size(); ++v) {
        if (done[v] != 0 || topology.isDeleted(VertexId(v))) {
            continue;
        }
        const std::vector<u32> members = membersOfPoint(mesh, points, v);
        Vector3f sum{0.0f, 0.0f, 0.0f};
        for (const u32 m : members) {
            sum = sum + sums[m];
        }
        const Vector3f unit = unitOr(sum, Vector3f{0.0f, 0.0f, 1.0f});
        for (const u32 m : members) {
            out[m] = unit;
            done[m] = 1;
        }
    }
    return out;
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
/// what it came from.
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
    const std::vector<Vector3f> local = localNormals(mesh, points, inRegion);
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
        const auto directionOf = [&](u32 vertex) {
            return group ? aim : (vertex < local.size() ? local[vertex] : aim);
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

    // Extrude's regions, copies and walls (Group: the selection as one), with
    // the hinge's two vertices pinned -- the wall on the hinge edge is then
    // nothing and its neighbours are triangles (`appendWall`).
    const Twins twins(mesh, points);
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
/// @p pinned is the vertex a hinge keeps in place -- its own copy, so the two
/// walls at it are triangles and it takes no motion -- or `kInvalidId`.
static ModelPlan planBorderStrip(Mesh& mesh, const PointTable& points, const ElementSet& edges, u32 pinned) {
    ModelPlan plan;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    // The border halfedges asked for: the side with no face, which is the run
    // the strip grows from.
    std::vector<HalfedgeId> border;
    std::vector<u8> taken(topology.edgeCount(), 0);
    for (const u32 e : sortedUnique(edges.edges)) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e)) || !topology.isBoundary(EdgeId(e)) ||
            taken[e] != 0) {
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
    // OUTWARD directions: for each edge, from the centre of the face behind it
    // to the centre of the edge, flattened into that face's plane.
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
        const Vector3f normal = unitOr(faceArea(mesh, face), Vector3f{0.0f, 0.0f, 1.0f});
        const u32 from = topology.from(h).value();
        const u32 to = topology.to(h).value();
        Vector3f outward = normal; // a face with no centre to speak of
        if (from < placed.size() && to < placed.size()) {
            Vector3f centre{0.0f, 0.0f, 0.0f};
            u32 corners = 0;
            for (const HalfedgeId c : topology.fh(face)) {
                const u32 v = topology.from(c).value();
                if (v < placed.size()) {
                    centre = centre + placed[v];
                    ++corners;
                }
            }
            if (corners != 0) {
                centre = centre * (1.0f / static_cast<f32>(corners));
                const Vector3f mid = (placed[from] + placed[to]) * 0.5f;
                Vector3f out = mid - centre;
                // Into the face's plane, so a bowed n-gon's strip still lies in
                // the surface rather than tilting out of it...
                out = out - normal * out.dot(normal);
                // ...and square to the edge. Without this the direction leans
                // along the edge by however far the face's centre sits to one
                // side of it, which on a TRIANGLE is always -- every Warcraft
                // III mesh is triangles, and a border run of them would grow a
                // sawtooth instead of a strip.
                const Vector3f along = unitOr(placed[to] - placed[from], Vector3f{1.0f, 0.0f, 0.0f});
                out = out - along * out.dot(along);
                outward = unitOr(out, normal);
            }
        }
        for (const u32 v : {from, to}) {
            if (v == pinned) {
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
    const auto copyOrSelf = [&](u32 v) { return v == pinned ? v : copyOf[v]; };
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
        // A crack's twins are one point (§2.2), so they take one aim between
        // them and the strip grows without parting them.
        Vector3f aim{0.0f, 0.0f, 0.0f};
        for (const u32 member : membersOfPoint(mesh, points, vertex)) {
            const auto found = aimOf.find(member);
            if (found != aimOf.end()) {
                aim = aim + found->second;
            }
        }
        VertexMotion motion;
        motion.vertex = copy;
        motion.base = copy < now.size() ? now[copy] : Vector3f{0.0f, 0.0f, 0.0f};
        motion.direction = unitOr(aim, Vector3f{0.0f, 0.0f, 1.0f});
        plan.motions.push_back(motion);
    }
    std::sort(plan.motions.begin(), plan.motions.end(),
              [](const VertexMotion& a, const VertexMotion& b) { return a.vertex < b.vertex; });
    const Topology& built = std::as_const(mesh).topology();
    const auto isCopy = [&](VertexId v) { return std::binary_search(copies.begin(), copies.end(), v.value()); };
    const auto onFarSide = [&](VertexId v) { return isCopy(v) || v.value() == pinned; };
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
    AmountRange(mesh, plan);
    return plan;
}

ModelPlan PlanExtrudeBorder(Mesh& mesh, const PointTable& points, const ElementSet& edges) {
    return planBorderStrip(mesh, points, edges, kInvalidId);
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
    plan = planBorderStrip(mesh, points, edges, hingeVertex);
    if (plan.refused()) {
        return plan;
    }
    // The strip's aims are the extrude's; a hinge turns instead, about the
    // pinned vertex along the axis the caller chose, and is unbounded.
    for (VertexMotion& motion : plan.motions) {
        motion.direction = Vector3f{0.0f, 0.0f, 0.0f};
    }
    const std::span<const Vector3f> now = positionsOf(mesh);
    HingeAxis hinge;
    hinge.origin = hingeVertex < now.size() ? now[hingeVertex] : Vector3f{0.0f, 0.0f, 0.0f};
    hinge.axis = unitOr(axis, Vector3f{0.0f, 0.0f, 1.0f});
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
    plan.changed = static_cast<u32>(chosen.size());
    AmountRange(mesh, plan);
    // §2.4: each inner vertex's corners and skin are re-sampled from the face
    // its new place lands in, so the texture and the binding stay on the
    // surface. The place the plan samples at is half of what it allows.
    const f32 sample = plan.amountMax > 0.0f && plan.amountMax < FLT_MAX ? plan.amountMax * 0.5f : 1.0f;
    for (const VertexMotion& motion : plan.motions) {
        const std::vector<Resample> made =
            ResamplesAt(mesh, VertexId(motion.vertex), motion.base + motion.direction * sample);
        plan.resamples.insert(plan.resamples.end(), made.begin(), made.end());
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

/// One point the chamfer places: where it starts, how fast it leaves, and the
/// vertex it became.
struct Placed {
    u32 from = kInvalidId; ///< The vertex it replaces.
    Vector3f direction{0.0f, 0.0f, 0.0f};
    u32 id = kInvalidId;
};

} // namespace

ModelPlan PlanChamferEdges(Mesh& mesh, const PointTable& points, const ElementSet& edges,
                           const ChamferParams& params) {
    ModelPlan plan;
    (void)points;
    if (params.segments != 1 || params.open) {
        plan.refusal = ModelRefusal::NotBuiltYet; // one flat segment, for now
        return plan;
    }
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u8> chosen(topology.edgeCount(), 0);
    u32 count = 0;
    for (const u32 e : sortedUnique(edges.edges)) {
        if (e >= topology.edgeCount() || topology.isDeleted(EdgeId(e)) || chosen[e] != 0) {
            continue;
        }
        if (topology.isBoundary(EdgeId(e))) {
            // The rules below want a face on each side (§3.9); `open` is what
            // would chamfer a border edge, and it is not built.
            plan.refusal = ModelRefusal::BorderEdgeChamfer;
            return plan;
        }
        chosen[e] = 1;
        ++count;
    }
    if (count == 0) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
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
    std::vector<u8> touched(topology.vertexCount(), 0);
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (chosen[e] == 0) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        touched[topology.from(h).value()] = 1;
        touched[topology.to(h).value()] = 1;
    }
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
        // An unselected edge's point: `amount / sin` of the sharpest offset any
        // face beside it asks for, and plain `amount` when none does.
        for (std::size_t i = 0; i < n; ++i) {
            const u32 e = edgeAt(i);
            if (chosen[e] != 0) {
                continue; // a selected edge puts no point of its own
            }
            f32 reach = 0.0f;
            for (const std::size_t f : {i, (i + n - 1) % n}) {
                // The two faces beside this edge: the one after it in the fan
                // and the one before it. A border vertex has only one of them.
                if (!faceAt(f).valid()) {
                    continue;
                }
                const std::size_t other = f == i ? (i + 1) % n : f;
                if (chosen[edgeAt(other)] == 0) {
                    continue; // this face holds no selected edge at v
                }
                const f32 cosine = std::clamp(along[i].dot(along[other]), -1.0f, 1.0f);
                const f32 sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
                if (sine < 0.0174524f) {
                    continue; // outside [1, 179] degrees: the face gives a point instead
                }
                // The nearer offset line wins, so two of them never put their
                // points past each other.
                const f32 asked = 1.0f / sine;
                reach = reach == 0.0f ? asked : std::min(reach, asked);
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
            const auto take = [&](u32 id) {
                cornerVertex.push_back(id);
                cornerSource.push_back(source);
                ++valence;
            };
            if (both) {
                if (inner != pointInFace.end()) {
                    take(inner->second);
                }
                continue;
            }
            if (fromArriving != pointOfEdge.end()) {
                take(fromArriving->second);
            }
            if (inner != pointInFace.end()) {
                take(inner->second);
            }
            if (fromLeaving != pointOfEdge.end()) {
                take(fromLeaving->second);
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
        u32 strip;  ///< The strip's slot.
        u32 source; ///< The face all four of its corners read.
        u32 first;  ///< Its two corners on the far side of the seam.
        u32 second;
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
        // A seam's strip samples one face on both sides, so the seam ends up on
        // its far long edge and stays crisp (§3.9). Its far corners are not in
        // that face's fan, so they are re-sampled from it by hand below.
        const bool seam = e < seams.size() && seams[e] != 0;
        const HalfedgeId sources[4] = {topology.next(left), left, seam ? left : right,
                                       seam ? topology.next(left) : topology.next(right)};
        if (seam) {
            seamStrips.push_back({static_cast<u32>(faceValence.size()), std::min(leftFace, rightFace),
                                  loop[2], loop[3]});
        }
        for (u32 i = 0; i < 4; ++i) {
            if (loop[i] == kInvalidId) {
                plan.refusal = ModelRefusal::WouldFold; // a side put no point there
                return plan;
            }
        }
        faceValence.push_back(4);
        for (u32 i = 0; i < 4; ++i) {
            cornerVertex.push_back(loop[i]);
            const auto ordinal = ordinalOf.find(static_cast<u32>(sources[i].index()));
            cornerSource.push_back(ordinal == ordinalOf.end() ? kInvalidId : ordinal->second);
        }
        faceSource.push_back(std::min(leftFace, rightFace));
        strips.push_back(static_cast<u32>(faceValence.size() - 1));
        plan.changedFaces.push_back(strips.back());
    }
    // One patch per ring of three or more, wound with the faces around it.
    std::vector<u32> ringed;
    for (const auto& entry : ringOfVertex) {
        ringed.push_back(entry.first);
    }
    std::sort(ringed.begin(), ringed.end());
    for (const u32 vertex : ringed) {
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
        // The ring's own places, at the amount the plan is sampled at: at 0
        // they all sit on the vertex and say nothing about which way round it
        // goes.
        std::vector<Vector3f> shape;
        for (const u32 id : ring) {
            const Placed& point = placed[id - firstPlaced];
            shape.push_back(at(point.from) + point.direction);
        }
        std::vector<u32> loop(ring.begin(), ring.end());
        {
            Vector3f normal{0.0f, 0.0f, 0.0f};
            for (std::size_t i = 0; i < shape.size(); ++i) {
                normal = normal + cross(shape[i], shape[(i + 1) % shape.size()]);
            }
            if (normal.dot(outward) < 0.0f) {
                std::reverse(loop.begin(), loop.end());
            }
        }
        faceValence.push_back(static_cast<u32>(loop.size()));
        for (const u32 id : loop) {
            cornerVertex.push_back(id);
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
            cornerSource.push_back(source);
        }
        faceSource.push_back(lowest == kInvalidId ? 0u : lowest);
        plan.changedFaces.push_back(static_cast<u32>(faceValence.size() - 1));
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
    plan.selection.faces = strips;
    const Topology& built = std::as_const(mesh).topology();
    touchFaces(mesh, plan);
    plan.changed = count;
    AmountRange(mesh, plan);
    // §2.4 for every point that moves: its corners and its skin come from the
    // face its new place lands in.
    const f32 sample = plan.amountMax > 0.0f && plan.amountMax < FLT_MAX ? plan.amountMax * 0.5f : 1.0f;
    for (const VertexMotion& motion : plan.motions) {
        const std::vector<Resample> made =
            ResamplesAt(mesh, VertexId(motion.vertex), motion.base + motion.direction * sample);
        plan.resamples.insert(plan.resamples.end(), made.begin(), made.end());
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
            if (vertex == seam.first || vertex == seam.second) {
                resample.targetCorners.push_back(h);
            }
        }
        if (!resample.targetCorners.empty()) {
            plan.resamples.push_back(std::move(resample)); // its skin stays its point's
        }
    }
    return plan;
}

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
    const VertexId made = SplitEdge(mesh, edge, std::clamp(t, 0.0f, 1.0f));
    if (!made.valid()) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changed = 1;
    plan.selection.vertices.push_back(made.value());
    // Its two halves are new, and every face it touches is drawn again.
    const Topology& built = std::as_const(mesh).topology();
    for (const HalfedgeId h : built.voh(made)) {
        plan.touchedEdges.push_back(Topology::edge(h).value());
        if (const FaceId face = built.face(h); face.valid() && !built.isDeleted(face)) {
            plan.changedFaces.push_back(face.value());
        }
    }
    plan.touchedEdges = sortedUnique(std::move(plan.touchedEdges));
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
    const Topology& built = std::as_const(mesh).topology();
    for (const u32 v : plan.selection.vertices) {
        for (const HalfedgeId h : built.voh(VertexId(v))) {
            plan.touchedEdges.push_back(Topology::edge(h).value());
            if (const FaceId face = built.face(h); face.valid() && !built.isDeleted(face)) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
    plan.touchedEdges = sortedUnique(std::move(plan.touchedEdges));
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
    if (chosen.size() < 2) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
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
    const Topology& built = std::as_const(mesh).topology();
    for (const u32 v : plan.selection.vertices) {
        for (const HalfedgeId h : built.voh(VertexId(v))) {
            plan.touchedEdges.push_back(Topology::edge(h).value());
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
    const Vector3f normal = unitOr(plane, Vector3f{0.0f, 0.0f, 1.0f});
    // Each end as a vertex: a click on one is itself, a click along an edge is
    // `SplitEdge`'s, and a click inside a face is where the chain left it.
    const auto realise = [&](const CutPoint& point) -> u32 {
        mesh.ensureConnectivity();
        const Topology& topology = std::as_const(mesh).topology();
        if (point.vertex != kInvalidId && point.vertex < topology.vertexCount() &&
            !topology.isDeleted(VertexId(point.vertex))) {
            return point.vertex;
        }
        if (point.edge != kInvalidId && point.edge < topology.edgeCount() &&
            !topology.isDeleted(EdgeId(point.edge))) {
            const VertexId made = SplitEdge(mesh, EdgeId(point.edge), std::clamp(point.t, 0.0f, 1.0f));
            return made.valid() ? made.value() : kInvalidId;
        }
        return kInvalidId;
    };
    // The far end first: splitting an edge renumbers the near end's, but a
    // vertex keeps its id, so the near end is taken against what is left.
    const u32 target = realise(to);
    const u32 start = realise(from);
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
                nearest = FLT_MAX;
                break;
            }
            // A corner of this face that the plane already runs through is the
            // crossing: stepping onto it cuts no sliver beside it (§3.14).
            for (const HalfedgeId other : topology.fh(face)) {
                const u32 v = topology.from(other).value();
                if (v == here || std::abs((at[v] - anchor).dot(normal)) > tolerance) {
                    continue;
                }
                const f32 reach = (at[v] - at[here]).dot(towards);
                if (reach > 0.0f && reach > nearest) {
                    nearest = reach;
                    best = face;
                    crossed = kInvalidId;
                    straight = v;
                }
            }
            if (straight != kInvalidId) {
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
            const HalfedgeId a = leaving(topology, best, here);
            const HalfedgeId b = leaving(topology, best, straight);
            if (a.valid() && b.valid() && !joined(topology, here, straight)) {
                SplitFaceAt(mesh, a, b);
            }
            chain.push_back(straight);
            here = straight;
            continue;
        }
        if (crossed == kInvalidId) {
            // The target is in this face: one cut across it, unless an edge
            // already runs between them — a segment along an existing edge is
            // that edge, and nothing is made (§3.14).
            if (!joined(topology, here, target)) {
                const HalfedgeId a = leaving(topology, best, here);
                const HalfedgeId b = leaving(topology, best, target);
                if (a.valid() && b.valid()) {
                    SplitFaceAt(mesh, a, b);
                }
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
        const VertexId made = SplitEdge(mesh, EdgeId(crossed), span == 0.0f ? 0.5f : first / span);
        if (!made.valid()) {
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
                    if (built.from(other).value() == made.value()) {
                        face = candidate;
                    }
                }
                if (face.valid()) {
                    break;
                }
            }
            if (face.valid() && !joined(built, here, made.value())) {
                const HalfedgeId one = leaving(built, face, here);
                const HalfedgeId two = leaving(built, face, made.value());
                if (one.valid() && two.valid()) {
                    SplitFaceAt(mesh, one, two);
                }
            }
        }
        chain.push_back(made.value());
        here = made.value();
    }
    if (chain.size() < 2) {
        plan.refusal = ModelRefusal::NothingToSlice;
        return plan;
    }
    mesh.ensureConnectivity();
    const Topology& built = std::as_const(mesh).topology();
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        if (const HalfedgeId h = built.findHalfedge(VertexId(chain[i]), VertexId(chain[i + 1]));
            h.valid()) {
            plan.selection.edges.push_back(Topology::edge(h).value());
            plan.touchedEdges.push_back(Topology::edge(h).value());
            ++plan.changed;
        }
    }
    if (plan.changed == 0) {
        plan.refusal = ModelRefusal::NothingToSlice;
        return plan;
    }
    for (const u32 e : plan.touchedEdges) {
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
    const Vector3f normal = unitOr(params.normal, Vector3f{0.0f, 0.0f, 1.0f});
    const f32 tolerance = CoincidenceTolerance(mesh);
    const auto distanceOf = [&](const Vector3f& at) { return (at - params.origin).dot(normal); };
    // Which faces the slice may touch: the selection, or all of them.
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
    // A vertex within the tolerance is put on the plane and used as a crossing:
    // that is what keeps a sliver from being cut off beside it. A refusal puts
    // them back, since a refused plan leaves the mesh as it was (§3.1).
    std::vector<std::pair<u32, Vector3f>> snapped;
    {
        const std::span<Vector3f> at = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        const Topology& topology = std::as_const(mesh).topology();
        for (u32 v = 0; v < at.size() && v < topology.vertexCount(); ++v) {
            if (topology.isDeleted(VertexId(v))) {
                continue;
            }
            const f32 distance = distanceOf(at[v]);
            if (distance != 0.0f && std::abs(distance) <= tolerance) {
                snapped.emplace_back(v, at[v]);
                at[v] = at[v] - normal * distance;
            }
        }
    }
    const auto unsnap = [&] {
        const std::span<Vector3f> at = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        for (const auto& [vertex, place] : snapped) {
            if (vertex < at.size()) {
                at[vertex] = place;
            }
        }
    };
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
            for (u32 side = 0; side < 2 && !touched; ++side) {
                const FaceId face = topology.face(Topology::halfedge(EdgeId(e), side));
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
            // paired two by two, so a concave face cut four times gets two
            // edges and not a cross.
            const Vector3f line = unitOr(cross(normal, faceArea(mesh, FaceId(f))), Vector3f{1.0f, 0.0f, 0.0f});
            std::sort(on.begin(), on.end(), [&](u32 a, u32 b) {
                return at[a].dot(line) < at[b].dot(line);
            });
            for (std::size_t i = 0; i + 1 < on.size(); i += 2) {
                cuts.emplace_back(on[i], on[i + 1]);
            }
        }
    }
    if (cuts.empty() && onPlane.empty()) {
        unsnap();
        plan.refusal = ModelRefusal::NothingToSlice;
        return plan;
    }
    if (cuts.empty()) {
        // The plane ran along edges that were already there: there is nothing
        // to cut, but the snap stands as what it found.
        unsnap();
        plan.refusal = ModelRefusal::NothingToSlice;
        return plan;
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
    if (plan.changed == 0) {
        unsnap();
        plan.refusal = ModelRefusal::NothingToSlice;
        return plan;
    }
    mesh.ensureConnectivity();
    if (params.mode == SliceMode::Remove) {
        // One side goes, through Delete's own rebuild (§3.7).
        ElementSet gone;
        const Topology& topology = std::as_const(mesh).topology();
        const std::span<const Vector3f> at = positionsOf(mesh);
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            f32 sum = 0.0f;
            u32 corners = 0;
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                sum += distanceOf(at[topology.from(h).value()]);
                ++corners;
            }
            const f32 side = corners == 0 ? 0.0f : sum / static_cast<f32>(corners);
            if (params.keepPositive ? side < 0.0f : side > 0.0f) {
                gone.faces.push_back(f);
            }
        }
        if (!gone.faces.empty()) {
            ModelPlan dropped = PlanDelete(mesh, gone);
            if (dropped.refused()) {
                plan.refusal = dropped.refusal;
                return plan;
            }
            plan.selection.edges.clear(); // the rebuild renumbered them
            plan.touchedEdges.clear();
            plan.changedFaces.clear();
        }
    } else if (params.mode == SliceMode::Split) {
        // The two sides part: every vertex on the cut is doubled, and the faces
        // below the plane take the copy.
        const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
        detail::RebuildMapping mapping = detail::identityMapping(mesh);
        const Topology& topology = std::as_const(mesh).topology();
        const std::span<const Vector3f> at = positionsOf(mesh);
        std::unordered_map<u32, u32> copyOf;
        for (const u32 v : onPlane) {
            if (v < topology.vertexCount() && !topology.isDeleted(VertexId(v))) {
                copyOf[v] = static_cast<u32>(mapping.vertexSource.size());
                mapping.vertexSource.push_back(v);
            }
        }
        const std::vector<u32> base = cornerBases(mapping.faces);
        bool parted = false;
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            f32 sum = 0.0f;
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                sum += distanceOf(at[topology.from(h).value()]);
            }
            if (sum >= 0.0f) {
                continue; // this side keeps the vertices it has
            }
            u32 corner = base[f];
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                const auto copy = copyOf.find(topology.from(h).value());
                if (copy != copyOf.end()) {
                    mapping.faces.cornerVertex[corner] = copy->second;
                    parted = true;
                }
                ++corner;
            }
        }
        if (parted) {
            mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
            const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
            if (!result.ok) {
                plan.refusal = ModelRefusal::WouldFold;
                return plan;
            }
            plan.selection.edges.clear(); // the rebuild renumbered them
            plan.touchedEdges.clear();
            plan.changedFaces.clear();
            // The copies are their own points, so nothing welds them back.
            const std::span<u32> groups = MergeGroupsOf(mesh);
            u32 next = FreshMergeGroup(mesh);
            for (const auto& [vertex, copy] : copyOf) {
                (void)vertex;
                if (copy < groups.size()) {
                    groups[copy] = next++;
                }
            }
        }
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

ModelPlan PlanTurnEdge(Mesh& mesh, EdgeId edge) {
    ModelPlan plan;
    plan.renumbers = true;
    if (!mesh.hasConnectivity() && !mesh.ensureConnectivity().ok()) {
        plan.refusal = ModelRefusal::NotBuiltYet;
        return plan;
    }
    {
        const Topology& topology = std::as_const(mesh).topology();
        if (edge.value() >= topology.edgeCount() || topology.isDeleted(edge) ||
            topology.isBoundary(edge)) {
            plan.refusal = ModelRefusal::EmptySelection;
            return plan;
        }
        // The corners the edge would turn onto: the one after it in each face.
        const HalfedgeId left = Topology::halfedge(edge, 0);
        const HalfedgeId right = Topology::opposite(left);
        const u32 a = topology.to(topology.next(left)).value();
        const u32 b = topology.to(topology.next(right)).value();
        if (joined(topology, a, b)) {
            plan.refusal = ModelRefusal::DiagonalExists;
            return plan;
        }
    }
    if (!FlipEdge(mesh, edge)) {
        // Not two triangles, or the flip would not stay manifold: the general
        // rotation over n-gons is `PlanEditTriangulation`'s business (§3.4).
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changed = 1;
    const Topology& built = std::as_const(mesh).topology();
    if (edge.value() < built.edgeCount() && !built.isDeleted(edge)) {
        plan.touchedEdges.push_back(edge.value());
        plan.selection.edges.push_back(edge.value());
        for (u32 side = 0; side < 2; ++side) {
            if (const FaceId face = built.face(Topology::halfedge(edge, side));
                face.valid() && !built.isDeleted(face)) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
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
    // The copies are a shell of their own: every vertex they use is copied.
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    std::unordered_map<u32, u32> copyOf;
    std::vector<u32> made;
    for (const u32 f : chosen) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const u32 v = topology.from(h).value();
            if (copyOf.find(v) == copyOf.end()) {
                copyOf[v] = static_cast<u32>(mapping.vertexSource.size());
                mapping.vertexSource.push_back(v);
                made.push_back(copyOf[v]);
            }
        }
    }
    for (const u32 f : chosen) {
        u32 valence = 0;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            mapping.faces.cornerVertex.push_back(copyOf[topology.from(h).value()]);
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
        plan.refusal = ModelRefusal::AllFaces; // a whole mesh moves at the Mesh level
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
    return plan;
}

ModelPlan PlanDetachToElement(Mesh& mesh, const ElementSet& faces) {
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
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
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
        if (rim[i] != kInvalidId) {
            lowest = std::min(lowest, rim[i]);
            for (const HalfedgeId h : topology.fh(FaceId(rim[i]))) {
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

ModelPlan PlanBridge(Mesh& mesh, const PointTable& points, EdgeId first, EdgeId second,
                     const BridgeParams& params) {
    ModelPlan plan;
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
    const auto loopOfEdge = [&](EdgeId edge, std::vector<u32>& vertices, std::vector<HalfedgeId>& rim) {
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
            rim.push_back(Topology::opposite(walk));
            walk = topology.next(walk);
        } while (walk.valid() && walk != start && vertices.size() <= topology.halfedgeCount());
        return vertices.size() >= 3;
    };
    std::vector<u32> here;
    std::vector<u32> there;
    std::vector<HalfedgeId> hereRim;
    std::vector<HalfedgeId> thereRim;
    if (!loopOfEdge(first, here, hereRim) || !loopOfEdge(second, there, thereRim)) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (here.size() != there.size()) {
        plan.refusal = ModelRefusal::LoopCountsDiffer;
        return plan;
    }
    for (const u32 v : here) {
        if (std::find(there.begin(), there.end(), v) != there.end()) {
            plan.refusal = ModelRefusal::SameLoop;
            return plan;
        }
    }
    // The loops are matched the other way round, as a bridge that does not
    // twist needs, and the pair they start from is the one whose connecting
    // edges are shortest in sum.
    const std::span<const Vector3f> at = positionsOf(mesh);
    const std::size_t n = here.size();
    std::size_t best = 0;
    f32 shortest = FLT_MAX;
    for (std::size_t offset = 0; offset < n; ++offset) {
        f32 sum = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const u32 a = here[i];
            const u32 b = there[(offset + n - i) % n];
            sum += (at[b] - at[a]).length();
        }
        if (sum < shortest) {
            shortest = sum;
            best = offset;
        }
    }
    best = (best + static_cast<std::size_t>((params.twist % static_cast<i32>(n)) + static_cast<i32>(n))) % n;
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
    for (std::size_t i = 0; i < n; ++i) {
        const u32 a = here[i];
        const u32 b = here[(i + 1) % n];
        const u32 c = there[(best + n - i) % n];
        const u32 d = there[(best + n - i - 1 + n) % n];
        // One quad per edge pair, wound against both borders.
        const u32 loop[4] = {a, b, d, c};
        const HalfedgeId sources[4] = {hereRim[i], hereRim[(i + 1) % n],
                                       thereRim[(best + n - i - 1 + n) % n],
                                       thereRim[(best + n - i) % n]};
        mapping.faces.faceValence.push_back(4);
        for (u32 k = 0; k < 4; ++k) {
            mapping.faces.cornerVertex.push_back(loop[k]);
            mapping.cornerSource.push_back(cornerOf(sources[k], loop[k]));
        }
        mapping.faceSource.push_back(topology.face(hereRim[i]).valid() ? topology.face(hereRim[i]).value() : 0u);
        plan.selection.faces.push_back(static_cast<u32>(mapping.faces.faceValence.size() - 1));
        plan.changedFaces.push_back(plan.selection.faces.back());
    }
    mapping.faces.vertexCount = static_cast<u32>(mapping.vertexSource.size());
    const detail::RebuildResult result = detail::rebuild(mesh, std::move(mapping), snapshot);
    if (!result.ok || result.repair.changed) {
        plan.refusal = ModelRefusal::WouldFold;
        return plan;
    }
    plan.changed = static_cast<u32>(n);
    touchFaces(mesh, plan);
    return plan;
}


ModelPlan PlanBridgeFaces(Mesh& mesh, const PointTable& points, FaceId first, FaceId second,
                          const BridgeParams& params) {
    ModelPlan plan;
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
    if (here.size() != there.size()) {
        plan.refusal = ModelRefusal::LoopCountsDiffer;
        return plan;
    }
    for (const u32 v : here) {
        if (std::find(there.begin(), there.end(), v) != there.end()) {
            plan.refusal = ModelRefusal::SameLoop;
            return plan;
        }
    }
    // The loops matched the other way round, started from the pair whose
    // connecting edges are shortest in sum, `twist` stepping it round.
    const std::span<const Vector3f> at = positionsOf(mesh);
    const std::size_t n = here.size();
    std::size_t best = 0;
    f32 shortest = FLT_MAX;
    for (std::size_t offset = 0; offset < n; ++offset) {
        f32 sum = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            sum += (at[there[(offset + n - i) % n]] - at[here[i]]).length();
        }
        if (sum < shortest) {
            shortest = sum;
            best = offset;
        }
    }
    best = (best + static_cast<std::size_t>((params.twist % static_cast<i32>(n)) + static_cast<i32>(n))) % n;
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
    for (std::size_t i = 0; i < n; ++i) {
        const u32 a = here[i];
        const u32 b = here[(i + 1) % n];
        const u32 c = there[(best + n - i) % n];
        const u32 d = there[(best + n - i - 1 + n) % n];
        // One quad per edge pair, wound against both rims.
        const u32 loop[4] = {a, b, d, c};
        const HalfedgeId sources[4] = {hereRim[i], hereRim[(i + 1) % n],
                                       thereRim[(best + n - i - 1 + n) % n],
                                       thereRim[(best + n - i) % n]};
        mapping.faces.faceValence.push_back(4);
        for (u32 k = 0; k < 4; ++k) {
            mapping.faces.cornerVertex.push_back(newId[loop[k]]);
            mapping.cornerSource.push_back(ordinal(sources[k]));
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
    plan.changed = static_cast<u32>(n);
    touchFaces(mesh, plan);
    return plan;
}

// ============================================================================
// Symmetry and moving well (§3.15, §3.17)
// ============================================================================

ModelPlan PlanSymmetrize(Mesh& mesh, const PointTable& points, const SymmetrizeParams& params,
                         std::span<const u32> boneMirror) {
    ModelPlan plan;
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
    if (!sliced.refused()) {
        FinishTool(mesh, sliced);
    }
    // And the far side goes, whether the plane had to cut anything or ran along
    // edges that were already there: Slice refuses the second case, and the
    // half still has to go.
    {
        mesh.ensureConnectivity();
        const Topology& before = std::as_const(mesh).topology();
        const std::span<const Vector3f> places = positionsOf(mesh);
        ElementSet gone;
        for (u32 f = 0; f < before.faceCount(); ++f) {
            if (before.isDeleted(FaceId(f))) {
                continue;
            }
            f32 sum = 0.0f;
            u32 corners = 0;
            for (const HalfedgeId h : before.fh(FaceId(f))) {
                sum += (places[before.from(h).value()] - seam).dot(normal);
                ++corners;
            }
            if (corners != 0 && sum / static_cast<f32>(corners) < 0.0f) {
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
            FinishTool(mesh, dropped);
        }
    }
    const f32 tolerance = CoincidenceTolerance(mesh);
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector3f> at = positionsOf(mesh);
    bool any = false;
    for (u32 f = 0; f < topology.faceCount() && !any; ++f) {
        any = !topology.isDeleted(FaceId(f));
    }
    if (!any) {
        plan.refusal = ModelRefusal::NoSourceSide;
        return plan;
    }
    // The reflected copy: every vertex off the plane gets one, and every face a
    // reversed twin. A vertex on the plane is shared, which is the seam.
    const std::vector<u32> snapshot = detail::snapshotCornersBuilt(mesh);
    const std::unordered_map<u32, u32> ordinalOf = cornerOrdinals(snapshot);
    detail::RebuildMapping mapping = detail::identityMapping(mesh);
    std::vector<u32> mirrorOf(topology.vertexCount(), kInvalidId);
    std::vector<u32> made;
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (topology.isDeleted(VertexId(v))) {
            continue;
        }
        if (std::abs((at[v] - seam).dot(normal)) <= tolerance) {
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
    touchFaces(mesh, plan);
    mesh.recomputeBounds();
    return plan;
}

namespace {

/// Both forms of Make Planar once the plane is settled (§3.17): one motion per
/// chosen point that is off it, landing that point on it at amount 1.
ModelPlan planarOnto(Mesh& mesh, const std::vector<u32>& chosen, const Vector3f& origin,
                     const Vector3f& normal) {
    ModelPlan plan;
    plan.renumbers = false;
    const std::span<const Vector3f> at = positionsOf(mesh);
    for (const u32 v : chosen) {
        const f32 distance = (at[v] - origin).dot(normal);
        if (std::abs(distance) <= 0.0f) {
            continue;
        }
        VertexMotion motion;
        motion.vertex = v;
        motion.base = at[v];
        motion.direction = normal * -distance; // amount 1 lands it on the plane
        plan.motions.push_back(motion);
    }
    if (plan.motions.empty()) {
        plan.refusal = ModelRefusal::ZeroAmount; // already flat
        return plan;
    }
    plan.changed = static_cast<u32>(plan.motions.size());
    plan.amountMin = -1.0f;
    plan.amountMax = 1.0f;
    for (const u32 v : chosen) {
        for (const HalfedgeId h : std::as_const(mesh).topology().voh(VertexId(v))) {
            if (const FaceId face = std::as_const(mesh).topology().face(h); face.valid()) {
                plan.changedFaces.push_back(face.value());
            }
        }
    }
    plan.changedFaces = sortedUnique(std::move(plan.changedFaces));
    plan.selection.vertices = chosen;
    return plan;
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
        FitPlane(gathered, &centre, &normal);
    }
    return planarOnto(mesh, chosen, centre, normal);
}

bool FitPlane(std::span<const Vector3f> points, Vector3f* origin, Vector3f* normal) {
    // The plane through the centre whose normal is the smallest eigenvector of
    // the scatter, found by the power method on its adjugate -- three sweeps is
    // plenty for a face's worth of points.
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
    f64 xx = 0, xy = 0, xz = 0, yy = 0, yz = 0, zz = 0;
    for (const Vector3f& p : points) {
        const Vector3f d = p - centre;
        xx += static_cast<f64>(d.x) * d.x;
        xy += static_cast<f64>(d.x) * d.y;
        xz += static_cast<f64>(d.x) * d.z;
        yy += static_cast<f64>(d.y) * d.y;
        yz += static_cast<f64>(d.y) * d.z;
        zz += static_cast<f64>(d.z) * d.z;
    }
    const std::array<std::array<f64, 3>, 3> adjugate = {{
        {yy * zz - yz * yz, xz * yz - xy * zz, xy * yz - xz * yy},
        {xz * yz - xy * zz, xx * zz - xz * xz, xy * xz - xx * yz},
        {xy * yz - xz * yy, xy * xz - xx * yz, xx * yy - xy * xy},
    }};
    std::size_t best = 0;
    f64 largest = -1.0;
    for (std::size_t i = 0; i < 3; ++i) {
        const f64 length = adjugate[i][0] * adjugate[i][0] + adjugate[i][1] * adjugate[i][1] +
                           adjugate[i][2] * adjugate[i][2];
        if (length > largest) {
            largest = length;
            best = i;
        }
    }
    if (largest <= 1e-20) {
        return false; // one point, or a line: no smallest direction to find
    }
    if (normal != nullptr) {
        *normal = unitOr(Vector3f{static_cast<f32>(adjugate[best][0]), static_cast<f32>(adjugate[best][1]),
                                  static_cast<f32>(adjugate[best][2])},
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
    const std::vector<u32> chosen = verticesOf(mesh, points, selection);
    if (chosen.size() < 3) {
        plan.refusal = ModelRefusal::EmptySelection;
        return plan;
    }
    if (normal.length() <= 1e-12f) {
        plan.refusal = ModelRefusal::ZeroAmount; // that is no plane
        return plan;
    }
    return planarOnto(mesh, chosen, origin, normal.normalized());
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
