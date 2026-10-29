// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/prepare.h>
#include <whiteout/models/wem/geometry/kernel.h>

#include "modelling_common.h"
#include "rebuild.h"
#include "weld.h"

#include <whiteout/models/wem/geometry/repair.h>
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

using detail::identityMapping;
using detail::RebuildMapping;
using detail::rebuild;
using detail::snapshotCorners;
using detail::snapshotCornersBuilt;

using modellingdetail::angleBetween;
using modellingdetail::positionsOf;
using modellingdetail::sortedUnique;
using modellingdetail::toArray;

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
            layer.name == names::kBinormal || names::IsCornerMark(layer.name)) {
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

namespace welddetail {

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

/// `sharp` on every edge whose two sides' normals break (the weld's test),
/// whether the weld closed it or it was interior already: an OBJ or FBX cube
/// shares its points and holds a normal per face. A modelled mesh shades by
/// its flags alone, so an edge left unflagged here is smooth at the next
/// re-shade. Returns how many it set.
u32 markNormalBreaks(Mesh& mesh) {
    if (!mesh.hasConnectivity()) {
        return 0;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const u8> flagged = std::as_const(mesh).attributes.get<const u8>(names::kSharp, Domain::Edge);
    std::vector<u32> sharps;
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (topology.isDeleted(EdgeId(e)) || (e < flagged.size() && flagged[e] != 0)) {
            continue;
        }
        if (NormalsBreakAcross(mesh, Topology::halfedge(EdgeId(e), 0), kWeldSharpAngle)) {
            sharps.push_back(e);
        }
    }
    if (!sharps.empty()) {
        const std::span<u8> sharp = mesh.attributes.getOrCreate<u8>(names::kSharp, Domain::Edge, AttrType::Bool);
        for (const u32 e : sharps) {
            sharp[e] = 1;
        }
    }
    return static_cast<u32>(sharps.size());
}

} // namespace welddetail

using namespace welddetail;

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
        report.sharpMarked = markNormalBreaks(mesh);
        setMarker(mesh);
        return report;
    }

    const detail::RebuildResult rebuilt =
        commit(mesh, settled, static_cast<u32>(set.faceCount()), &report);
    report.verticesWelded = vertexCount - std::as_const(mesh).vertexCount();

    // Edges the weld closed: seam where an authored corner value differs. An
    // edge already interior keeps its seam flag. Sharp is every edge whose
    // normals break, closed here or not.
    if (rebuilt.ok && mesh.hasConnectivity()) {
        // `seam` has one writer, so the weld and a UV commit mark by the same
        // rule and neither can drift from the other (EDIT_MODE_UV_PLAN.md P1).
        report.seamsMarked = uv::MarkDelimitSeams(mesh, rebuilt.closedEdges);
        report.sharpMarked = markNormalBreaks(mesh);
    }
    setMarker(mesh);
    mesh.recomputeBounds();
    return report;
}

namespace uv {

u32 MarkDelimitSeams(Mesh& mesh) {
    if (!mesh.hasConnectivity()) {
        return 0;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const u32 edgeCount = topology.edgeCount();
    if (edgeCount == 0) {
        return 0;
    }
    const std::span<u8> seam =
        mesh.attributes.getOrCreate<u8>(names::kSeam, Domain::Edge, AttrType::Bool);
    u32 marked = 0;
    for (u32 e = 0; e < edgeCount && e < seam.size(); ++e) {
        const EdgeId edge(e);
        if (topology.isDeleted(edge) || topology.isBoundary(edge)) {
            // A border does not delimit anything: there is only one side of it.
            continue;
        }
        const u8 value = SeamBetween(mesh, Topology::halfedge(edge, 0)) ? 1 : 0;
        seam[e] = value;
        marked += value;
    }
    return marked;
}

u32 MarkDelimitSeams(Mesh& mesh, std::span<const u32> edges) {
    if (!mesh.hasConnectivity() || edges.empty()) {
        return 0;
    }
    std::vector<u32> marked;
    marked.reserve(edges.size());
    for (const u32 e : edges) {
        if (SeamBetween(mesh, Topology::halfedge(EdgeId(e), 0))) {
            marked.push_back(e);
        }
    }
    if (marked.empty()) {
        return 0;
    }
    const std::span<u8> seam =
        mesh.attributes.getOrCreate<u8>(names::kSeam, Domain::Edge, AttrType::Bool);
    for (const u32 e : marked) {
        if (e < seam.size()) {
            seam[e] = 1;
        }
    }
    return static_cast<u32>(marked.size());
}

} // namespace uv

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
