// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/points.h>

#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/triangulation.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

/// Union-find over vertices, path-halving.
struct Unions {
    std::vector<u32> parent;

    explicit Unions(u32 count) : parent(count) {
        for (u32 i = 0; i < count; ++i) {
            parent[i] = i;
        }
    }
    u32 find(u32 a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    }
    void join(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            // The lower index wins, so a point's id is its lowest vertex and the
            // table is the same whatever order the joins came in.
            (a < b ? parent[b] : parent[a]) = a < b ? a : b;
        }
    }
};

/// A hash grid cell, keyed by the quantised position.
struct CellKey {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;

    bool operator==(const CellKey&) const = default;
};

struct CellHash {
    std::size_t operator()(const CellKey& key) const {
        std::size_t h = 1469598103934665603ull;
        for (const i64 word : {key.x, key.y, key.z}) {
            h = (h ^ static_cast<std::size_t>(word)) * 1099511628211ull;
        }
        return h;
    }
};

CellKey CellOf(const Vector3f& p, f32 cell) {
    return CellKey{static_cast<i64>(std::floor(p.x / cell)), static_cast<i64>(std::floor(p.y / cell)),
                   static_cast<i64>(std::floor(p.z / cell))};
}

/// The CSR of a set of (key, value) pairs, sorted and de-duplicated per key.
void BuildCsr(u32 keys, std::vector<std::pair<u32, u32>>& pairs, std::vector<u32>& offsets,
              std::vector<u32>& values) {
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    offsets.assign(static_cast<std::size_t>(keys) + 1, 0);
    for (const auto& [key, value] : pairs) {
        ++offsets[key + 1];
    }
    for (u32 k = 0; k < keys; ++k) {
        offsets[k + 1] += offsets[k];
    }
    values.resize(pairs.size());
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        values[i] = pairs[i].second;
    }
}

} // namespace

std::span<const u32> PointTable::membersOf(u32 point) const {
    if (point + 1 >= memberOffsets.size()) {
        return {};
    }
    return std::span<const u32>(members.data() + memberOffsets[point],
                                memberOffsets[point + 1] - memberOffsets[point]);
}

std::span<const u32> PointTable::ringOf(u32 point) const {
    if (point + 1 >= ringOffsets.size()) {
        return {};
    }
    return std::span<const u32>(ring.data() + ringOffsets[point],
                                ringOffsets[point + 1] - ringOffsets[point]);
}

u32 PointTable::pointOfVertex(u32 vertex) const {
    return vertex < pointOf.size() ? pointOf[vertex] : kInvalidIndex;
}

PointTable BuildPointTable(const Mesh& mesh) {
    PointTable table;
    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    table.vertexCount = static_cast<u32>(positions.size());
    if (table.vertexCount == 0) {
        return table;
    }

    // --- the joins: seam twins, then repair twins ----------------------------
    table.weldTolerance = geom::CoincidenceTolerance(positions);
    Unions unions(table.vertexCount);
    if (table.weldTolerance > 0.0f) {
        const f32 cell = table.weldTolerance * 2.0f;
        const f32 tolerance = table.weldTolerance;
        std::unordered_map<CellKey, std::vector<u32>, CellHash> grid;
        grid.reserve(table.vertexCount);
        for (u32 v = 0; v < table.vertexCount; ++v) {
            const CellKey key = CellOf(positions[v], cell);
            // The 27 neighbouring cells, because two positions a hair apart can
            // sit either side of a cell boundary.
            for (i64 dx = -1; dx <= 1; ++dx) {
                for (i64 dy = -1; dy <= 1; ++dy) {
                    for (i64 dz = -1; dz <= 1; ++dz) {
                        const auto found =
                            grid.find(CellKey{key.x + dx, key.y + dy, key.z + dz});
                        if (found == grid.end()) {
                            continue;
                        }
                        for (const u32 other : found->second) {
                            const Vector3f& a = positions[v];
                            const Vector3f& b = positions[other];
                            const f32 dxp = a.x - b.x;
                            const f32 dyp = a.y - b.y;
                            const f32 dzp = a.z - b.z;
                            if (dxp * dxp + dyp * dyp + dzp * dzp <= tolerance * tolerance) {
                                unions.join(v, other);
                            }
                        }
                    }
                }
            }
            grid[key].push_back(v);
        }
    }
    const std::span<const u32> mergeGroups =
        mesh.attributes.get<u32>(geom::names::kMergeGroup, geom::Domain::Vertex);
    if (mergeGroups.size() == table.vertexCount) {
        std::unordered_map<u32, u32> firstOfGroup;
        firstOfGroup.reserve(table.vertexCount);
        for (u32 v = 0; v < table.vertexCount; ++v) {
            const auto [entry, added] = firstOfGroup.try_emplace(mergeGroups[v], v);
            if (!added) {
                unions.join(v, entry->second);
            }
        }
    }

    // --- number the points ---------------------------------------------------
    table.pointOf.assign(table.vertexCount, kInvalidIndex);
    std::vector<std::pair<u32, u32>> memberPairs;
    memberPairs.reserve(table.vertexCount);
    for (u32 v = 0; v < table.vertexCount; ++v) {
        const u32 root = unions.find(v);
        if (table.pointOf[root] == kInvalidIndex) {
            table.pointOf[root] = table.pointCount++;
        }
        table.pointOf[v] = table.pointOf[root];
        memberPairs.emplace_back(table.pointOf[v], v);
    }
    BuildCsr(table.pointCount, memberPairs, table.memberOffsets, table.members);

    // --- the welded ring, and the islands ------------------------------------
    //
    // Both come off the face set: a face's corners are ring neighbours of each
    // other, and the faces that share a point are one island. Neither needs the
    // half-edge arrays, which is why a table can be built for a mesh nobody has
    // asked for connectivity on. A polygon's drawn diagonals are ring edges too,
    // so joining triangles into quads changes no neighbourhood Grow and Smooth
    // see (EDIT_MODE_MODELLING_DESIGN.md §2.3).
    const geom::FaceSet& faces = mesh.faceSet();
    const std::vector<u32> slots = geom::FaceSetSlots(mesh);
    std::vector<std::pair<u32, u32>> ringPairs;
    Unions faceUnions(static_cast<u32>(faces.faceCount()));
    std::vector<u32> firstFaceOfPoint(table.pointCount, kInvalidIndex);
    std::vector<u32> cut;
    u32 corner = 0;
    for (u32 f = 0; f < faces.faceCount(); ++f) {
        const u32 valence = faces.faceValence[f];
        if (valence > 3) {
            const std::span<const u32> loop(faces.cornerVertex.data() + corner, valence);
            cut.clear();
            geom::TriangulateFace(loop, positions,
                                  mesh.triangulation.row(f < slots.size() ? slots[f] : f), cut);
            for (std::size_t t = 0; t + 2 < cut.size(); t += 3) {
                for (u32 side = 0; side < 3; ++side) {
                    const u32 a = loop[cut[t + side]];
                    const u32 b = loop[cut[t + (side + 1) % 3]];
                    if (a < table.vertexCount && b < table.vertexCount &&
                        table.pointOf[a] != table.pointOf[b]) {
                        ringPairs.emplace_back(table.pointOf[a], table.pointOf[b]);
                    }
                }
            }
        }
        for (u32 k = 0; k < valence; ++k) {
            const u32 vertex = faces.cornerVertex[corner + k];
            const u32 next = faces.cornerVertex[corner + (k + 1) % valence];
            if (vertex >= table.vertexCount || next >= table.vertexCount) {
                continue;
            }
            const u32 point = table.pointOf[vertex];
            const u32 other = table.pointOf[next];
            if (point != other) {
                ringPairs.emplace_back(point, other);
                ringPairs.emplace_back(other, point);
            }
            if (firstFaceOfPoint[point] == kInvalidIndex) {
                firstFaceOfPoint[point] = f;
            } else {
                faceUnions.join(f, firstFaceOfPoint[point]);
            }
        }
        corner += valence;
    }
    BuildCsr(table.pointCount, ringPairs, table.ringOffsets, table.ring);

    table.faceIsland.assign(faces.faceCount(), kInvalidIndex);
    std::vector<u32> islandOfRoot(faces.faceCount(), kInvalidIndex);
    for (u32 f = 0; f < faces.faceCount(); ++f) {
        const u32 root = faceUnions.find(f);
        if (islandOfRoot[root] == kInvalidIndex) {
            islandOfRoot[root] = table.islandCount++;
        }
        table.faceIsland[f] = islandOfRoot[root];
    }
    // A point takes the island of the first face that holds it; a point no face
    // holds has none.
    table.islandOf.assign(table.pointCount, kInvalidIndex);
    for (u32 p = 0; p < table.pointCount; ++p) {
        if (firstFaceOfPoint[p] != kInvalidIndex) {
            table.islandOf[p] = table.faceIsland[firstFaceOfPoint[p]];
        }
    }
    return table;
}

std::vector<u32> DisagreeingPoints(const Mesh& mesh, const PointTable& points) {
    std::vector<u32> out;
    if (mesh.skin.empty()) {
        return out;
    }
    const auto same = [&](u32 a, u32 b) {
        const std::span<const geom::Influence> left = mesh.skin.forVertex(a);
        const std::span<const geom::Influence> right = mesh.skin.forVertex(b);
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (left[i].bone != right[i].bone ||
                std::abs(left[i].weight - right[i].weight) > geom::kSkinWeightTolerance) {
                return false;
            }
        }
        return true;
    };
    for (u32 p = 0; p < points.pointCount; ++p) {
        const std::span<const u32> members = points.membersOf(p);
        for (std::size_t i = 1; i < members.size(); ++i) {
            if (!same(members[0], members[i])) {
                out.push_back(p);
                break;
            }
        }
    }
    return out;
}

std::vector<u32> GrowPoints(const PointTable& points, std::span<const u32> seed, u32 rings) {
    std::vector<u8> inside(points.pointCount, 0);
    std::vector<u32> front;
    for (const u32 point : seed) {
        if (point < points.pointCount && inside[point] == 0) {
            inside[point] = 1;
            front.push_back(point);
        }
    }
    for (u32 ring = 0; ring < rings && !front.empty(); ++ring) {
        std::vector<u32> next;
        for (const u32 point : front) {
            for (const u32 neighbour : points.ringOf(point)) {
                if (inside[neighbour] == 0) {
                    inside[neighbour] = 1;
                    next.push_back(neighbour);
                }
            }
        }
        front = std::move(next);
    }
    std::vector<u32> out;
    for (u32 p = 0; p < points.pointCount; ++p) {
        if (inside[p] != 0) {
            out.push_back(p);
        }
    }
    return out;
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
