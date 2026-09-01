// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/repair.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

// The edge and face-key tables below are hash maps, which the determinism rule
// permits for one reason only: they are *looked up*, never iterated. Every
// ordered decision the repair makes — which face is processed first, which
// vertex is duplicated next — comes from the input's own order.

constexpr u64 pairKey(u32 a, u32 b) {
    return (static_cast<u64>(a) << 32) | static_cast<u64>(b);
}

constexpr u64 undirectedKey(u32 a, u32 b) {
    return a < b ? pairKey(a, b) : pairKey(b, a);
}

/// The sorted corner set of a face, as one hashable key. Faces here have at most
/// a handful of corners, so a sorted vector hashed by FNV is cheaper than any
/// clever alternative and — unlike a sum or an xor — does not collide on
/// permutations of different sets.
u64 faceKey(std::span<const u32> corners) {
    u32 sorted[16];
    const std::size_t count = corners.size() < 16 ? corners.size() : 16;
    for (std::size_t i = 0; i < count; ++i) {
        sorted[i] = corners[i];
    }
    std::sort(sorted, sorted + count);
    u64 hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= sorted[i];
        hash *= 1099511628211ull;
    }
    // Mix the valence in so a 3-corner and a 4-corner face cannot share a key
    // when the extra corner happens to hash away.
    hash ^= static_cast<u64>(corners.size()) << 56;
    return hash;
}

bool hasRepeatedCorner(std::span<const u32> corners) {
    for (std::size_t i = 0; i < corners.size(); ++i) {
        for (std::size_t j = i + 1; j < corners.size(); ++j) {
            if (corners[i] == corners[j]) {
                return true;
            }
        }
    }
    return false;
}

/// Twice the area of the polygon's Newell normal. Zero (to a relative epsilon)
/// means the face has no surface to shade.
bool isZeroArea(std::span<const u32> corners, std::span<const Vector3f> positions) {
    if (positions.empty()) {
        return false;
    }
    Vector3f normal{0, 0, 0};
    f32 scale = 0;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        if (corners[i] >= positions.size()) {
            return false;
        }
        const Vector3f& a = positions[corners[i]];
        const Vector3f& b = positions[corners[(i + 1) % corners.size()]];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
        const f32 extent = std::max(std::max(std::abs(a.x), std::abs(a.y)), std::abs(a.z));
        scale = std::max(scale, extent);
    }
    const f32 magnitude = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
    // Relative to the face's own coordinate magnitude, so a tiny-but-real face in
    // a model authored in centimetres is not mistaken for a degenerate one.
    const f32 epsilon = std::max(scale * scale, 1.0f) * 1e-12f;
    return magnitude <= epsilon;
}

/// Union-find over global corner indices, for the bowtie pass.
class DisjointSet {
public:
    explicit DisjointSet(std::size_t count) : parent_(count) {
        for (std::size_t i = 0; i < count; ++i) {
            parent_[i] = static_cast<u32>(i);
        }
    }
    u32 find(u32 x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }
    void unite(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            // Always attach the higher root to the lower one, so the surviving
            // root of a group is its smallest member and the choice of "which
            // fan keeps the original vertex" is deterministic.
            if (a < b) {
                parent_[b] = a;
            } else {
                parent_[a] = b;
            }
        }
    }

private:
    std::vector<u32> parent_;
};

} // namespace

// ============================================================================

RepairResult Repair(const FaceSet& faces, std::span<const u32> sections,
                    std::span<const Vector3f> positions) {
    RepairResult result;
    result.faces.vertexCount = faces.vertexCount;

    u32 nextVertex = faces.vertexCount;

    // --- pass 1: per-face edge conflicts ------------------------------------
    //
    // Faces are visited in input order and each is made compatible with what is
    // already there, so the result never depends on a global view.

    std::unordered_map<u64, u32> directedEdgeOwner; // (from, to) -> output face
    std::unordered_map<u64, u32> undirectedEdgeUses;
    std::unordered_set<u64> faceKeysSeen;

    std::vector<std::vector<u32>> outFaces;
    outFaces.reserve(faces.faceCount());
    result.sections.reserve(faces.faceCount());

    std::vector<u32> corners;
    std::vector<u8> marked;

    std::size_t cursor = 0;
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        const u32 section = f < sections.size() ? sections[f] : 0;
        corners.assign(faces.cornerVertex.begin() + static_cast<std::ptrdiff_t>(cursor),
                       faces.cornerVertex.begin() + static_cast<std::ptrdiff_t>(cursor + valence));
        cursor += valence;

        // Degenerate: a repeated corner, too few corners, or no area at all.
        if (valence < 3 || hasRepeatedCorner(corners) || isZeroArea(corners, positions)) {
            FaceRecord dropped;
            dropped.corners = corners;
            dropped.section = section;
            dropped.index = static_cast<u32>(f);
            result.log.droppedFaces.push_back(std::move(dropped));
            result.changed = true;
            continue;
        }

        marked.assign(valence, 0);

        // A face on a vertex set another face already used is the cheap
        // two-sided trick; every corner is duplicated so the two surfaces come
        // apart cleanly.
        if (faceKeysSeen.count(faceKey(corners)) != 0) {
            for (u32 i = 0; i < valence; ++i) {
                marked[i] = 1;
            }
        }

        for (u32 i = 0; i < valence; ++i) {
            const u32 a = corners[i];
            const u32 b = corners[(i + 1) % valence];
            const bool sameDirection = directedEdgeOwner.count(pairKey(a, b)) != 0;
            const auto uses = undirectedEdgeUses.find(undirectedKey(a, b));
            const bool full = uses != undirectedEdgeUses.end() && uses->second >= 2;
            if (sameDirection || full) {
                // Both endpoints, per §5.3: a third fan gets its own copy of the
                // edge, and a face wound against its neighbour is split rather
                // than flipped.
                marked[i] = 1;
                marked[(i + 1) % valence] = 1;
            }
        }

        for (u32 i = 0; i < valence; ++i) {
            if (marked[i] == 0) {
                continue;
            }
            const u32 original = corners[i];
            const u32 created = nextVertex++;
            result.log.splits.push_back(VertexSplit{original, created});
            corners[i] = created;
            result.changed = true;
        }

        const u32 outIndex = static_cast<u32>(outFaces.size());
        for (u32 i = 0; i < valence; ++i) {
            const u32 a = corners[i];
            const u32 b = corners[(i + 1) % valence];
            directedEdgeOwner[pairKey(a, b)] = outIndex;
            ++undirectedEdgeUses[undirectedKey(a, b)];
        }
        faceKeysSeen.insert(faceKey(corners));
        outFaces.push_back(corners);
        result.sections.push_back(section);
    }

    // --- pass 2: bowtie vertices --------------------------------------------
    //
    // Every edge now carries at most two faces with opposite orientation, but a
    // vertex may still be the meeting point of several fans. Two corners at a
    // vertex belong to the same fan exactly when a two-faced edge joins them, so
    // the fans are the connected components of that relation.

    std::vector<u32> cornerBase(outFaces.size(), 0);
    u32 cornerTotal = 0;
    for (std::size_t f = 0; f < outFaces.size(); ++f) {
        cornerBase[f] = cornerTotal;
        cornerTotal += static_cast<u32>(outFaces[f].size());
    }

    if (cornerTotal != 0) {
        // corner -> (face, position), so a rewrite is a lookup and not a scan.
        std::vector<u32> cornerFace(cornerTotal, 0);
        for (std::size_t f = 0; f < outFaces.size(); ++f) {
            for (u32 i = 0; i < outFaces[f].size(); ++i) {
                cornerFace[cornerBase[f] + i] = static_cast<u32>(f);
            }
        }

        DisjointSet fans(cornerTotal);

        // Keyed by the *directed* pair (thisEndpoint, otherEndpoint), so the two
        // faces of an edge agree on which endpoint they are talking about even
        // though they traverse it in opposite directions. The second claim on a
        // slot unites its corner with the first: the two are the same fan.
        std::unordered_map<u64, u32> edgeCornerAt;
        const auto claim = [&](u32 vertex, u32 other, u32 corner) {
            const u64 slot = pairKey(vertex, other);
            const auto found = edgeCornerAt.find(slot);
            if (found == edgeCornerAt.end()) {
                edgeCornerAt.emplace(slot, corner);
            } else {
                fans.unite(found->second, corner);
            }
        };

        for (std::size_t f = 0; f < outFaces.size(); ++f) {
            const auto& face = outFaces[f];
            const u32 valence = static_cast<u32>(face.size());
            for (u32 i = 0; i < valence; ++i) {
                const u32 next = (i + 1) % valence;
                claim(face[i], face[next], cornerBase[f] + i);
                claim(face[next], face[i], cornerBase[f] + next);
            }
        }

        // Corners grouped per vertex, in ascending corner order — which is what
        // makes "the first fan keeps the original vertex" a deterministic rule.
        std::vector<std::vector<u32>> cornersOfVertex(nextVertex);
        for (std::size_t f = 0; f < outFaces.size(); ++f) {
            const auto& face = outFaces[f];
            for (u32 i = 0; i < face.size(); ++i) {
                cornersOfVertex[face[i]].push_back(cornerBase[f] + i);
            }
        }

        const u32 vertexLimit = nextVertex;
        for (u32 v = 0; v < vertexLimit; ++v) {
            const auto& owned = cornersOfVertex[v];
            if (owned.size() < 2) {
                continue;
            }
            const u32 firstRoot = fans.find(owned[0]);
            // Roots seen after the first, each mapped to the vertex it got.
            std::vector<std::pair<u32, u32>> extraFans;
            for (u32 corner : owned) {
                const u32 root = fans.find(corner);
                if (root == firstRoot) {
                    continue;
                }
                u32 replacement = kInvalidId;
                for (const auto& entry : extraFans) {
                    if (entry.first == root) {
                        replacement = entry.second;
                        break;
                    }
                }
                if (replacement == kInvalidId) {
                    replacement = nextVertex++;
                    result.log.splits.push_back(VertexSplit{v, replacement});
                    extraFans.emplace_back(root, replacement);
                    result.changed = true;
                }
                const u32 face = cornerFace[corner];
                outFaces[face][corner - cornerBase[face]] = replacement;
            }
        }
    }

    // --- assemble ------------------------------------------------------------

    result.faces.vertexCount = nextVertex;
    result.faces.faceValence.reserve(outFaces.size());
    for (const auto& face : outFaces) {
        result.faces.faceValence.push_back(static_cast<u32>(face.size()));
        result.faces.cornerVertex.insert(result.faces.cornerVertex.end(), face.begin(), face.end());
    }
    return result;
}

// ============================================================================

std::vector<u32> BuildMergeGroups(u32 vertexCount, const RepairLog& log) {
    std::vector<u32> groups(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v) {
        groups[v] = v;
    }
    // Splits are in application order, so a split whose original is itself a
    // created vertex already has its group resolved.
    for (const VertexSplit& split : log.splits) {
        if (split.created < vertexCount && split.original < vertexCount) {
            groups[split.created] = groups[split.original];
        }
    }
    return groups;
}

FaceSet Unrepair(const FaceSet& faces, const RepairLog& log) {
    FaceSet out;
    const u32 originalCount = faces.vertexCount >= static_cast<u32>(log.splits.size())
                                  ? faces.vertexCount - static_cast<u32>(log.splits.size())
                                  : faces.vertexCount;
    out.vertexCount = originalCount;

    const std::vector<u32> groups = BuildMergeGroups(faces.vertexCount, log);

    // Walk the surviving faces and the dropped ones together, so each dropped
    // face lands back at the index it came from.
    std::size_t droppedCursor = 0;
    std::size_t cursor = 0;
    std::size_t survivor = 0;
    const std::size_t totalFaces = faces.faceCount() + log.droppedFaces.size();

    for (std::size_t index = 0; index < totalFaces; ++index) {
        if (droppedCursor < log.droppedFaces.size() &&
            log.droppedFaces[droppedCursor].index == index) {
            const FaceRecord& record = log.droppedFaces[droppedCursor++];
            out.faceValence.push_back(static_cast<u32>(record.corners.size()));
            out.cornerVertex.insert(out.cornerVertex.end(), record.corners.begin(),
                                    record.corners.end());
            continue;
        }
        if (survivor >= faces.faceCount()) {
            break;
        }
        const u32 valence = faces.faceValence[survivor];
        out.faceValence.push_back(valence);
        for (u32 i = 0; i < valence; ++i) {
            const u32 v = faces.cornerVertex[cursor + i];
            out.cornerVertex.push_back(v < groups.size() ? groups[v] : v);
        }
        cursor += valence;
        ++survivor;
    }

    return out;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
