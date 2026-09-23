// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/transfer.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <whiteout/models/wem/geometry/bvh.h>
#include <whiteout/models/wem/geometry/triangulation.h>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

/// A mesh's triangles as it is drawn: `bvh.h` indexes three corners at a time,
/// and a quad in a source mesh is two triangles for this purpose like any other.
std::vector<u32> TrianglesOf(const Mesh& mesh) {
    std::vector<u32> out;
    geom::TriangulateMesh(mesh, out);
    return out;
}

Vector3f PositionOf(std::span<const Vector3f> positions, const PointTable& points, u32 point) {
    const std::span<const u32> members = points.membersOf(point);
    if (members.empty() || members[0] >= positions.size()) {
        return Vector3f{0, 0, 0};
    }
    return positions[members[0]];
}

void AddWeighted(std::vector<geom::Influence>& into, std::span<const geom::Influence> from,
                 f32 share) {
    if (!(share > 0.0f)) {
        return;
    }
    for (const geom::Influence& influence : from) {
        if (!(influence.weight > 0.0f) || !std::isfinite(influence.weight)) {
            continue;
        }
        bool merged = false;
        for (geom::Influence& held : into) {
            if (held.bone == influence.bone) {
                held.weight += influence.weight * share;
                merged = true;
                break;
            }
        }
        if (!merged) {
            into.push_back({influence.bone, influence.weight * share});
        }
    }
}

} // namespace

TransferResult Transfer(Mesh& target, const NodeTree& nodes, const PointTable& points,
                        std::span<const u32> scope, std::span<const Mesh* const> sources,
                        const TransferOptions& options) {
    TransferResult result;

    std::vector<u32> all;
    if (scope.empty()) {
        all.resize(points.pointCount);
        for (u32 point = 0; point < points.pointCount; ++point) {
            all[point] = point;
        }
        scope = all;
    }
    if (scope.empty()) {
        return result;
    }

    const std::span<const Vector3f> here =
        target.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);

    // One tree per source rather than one over all of them, so each keeps its
    // own vertex numbering and the corner lookup stays a plain index.
    struct Source {
        const Mesh* mesh = nullptr;
        std::vector<u32> indices;
        geom::TriangleBvh bvh;
    };
    std::vector<Source> built;
    for (const Mesh* source : sources) {
        if (source == nullptr || source == &target) {
            continue;
        }
        const std::span<const Vector3f> positions =
            source->attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        if (positions.empty() || source->skin.offsets.empty()) {
            // A source with no skin has nothing to give, and one with no
            // positions has nowhere to be.
            continue;
        }
        Source& made = built.emplace_back();
        made.mesh = source;
        made.indices = TrianglesOf(*source);
        if (made.indices.empty()) {
            built.pop_back();
            continue;
        }
        made.bvh.build(made.indices, positions);
    }
    if (built.empty()) {
        result.unreached = static_cast<u32>(scope.size());
        return result;
    }

    const f32 limit =
        options.maxDistance > 0.0f ? options.maxDistance : std::numeric_limits<f32>::max();

    // The bones outside the set are held, exactly as a generator holds them, so
    // "transfer the arm without touching the chest" is a scope and three bones.
    std::vector<u32> held;
    if (!options.bones.empty()) {
        std::vector<u8> inSet(nodes.size(), 0);
        for (const u32 bone : options.bones) {
            if (bone < nodes.size()) {
                inSet[bone] = 1;
            }
        }
        for (u32 node = 0; node < nodes.size(); ++node) {
            if (inSet[node] == 0 && nodes.nodes[node].kind == NodeKind::Bone) {
                held.push_back(node);
            }
        }
    }

    std::vector<u32> wrote;
    PointWeights given;
    for (const u32 point : scope) {
        if (point >= points.pointCount) {
            continue;
        }
        const Vector3f at = PositionOf(here, points, point);
        const Source* bestSource = nullptr;
        geom::PointHit best;
        f32 bestDistance = limit;
        for (const Source& source : built) {
            const geom::PointHit hit = source.bvh.closestPoint(at, bestDistance);
            if (!hit.hit() || hit.distance > bestDistance) {
                continue;
            }
            bestDistance = hit.distance;
            best = hit;
            bestSource = &source;
        }
        if (bestSource == nullptr) {
            ++result.unreached;
            continue;
        }
        const u32 corner = best.triangle * 3;
        if (corner + 2 >= bestSource->indices.size()) {
            ++result.unreached;
            continue;
        }
        // The three corners' influences, by the hit's barycentric weights.
        std::vector<geom::Influence> blended;
        const f32 shares[3] = {1.0f - best.u - best.v, best.u, best.v};
        for (u32 i = 0; i < 3; ++i) {
            AddWeighted(blended, bestSource->mesh->skin.forVertex(bestSource->indices[corner + i]),
                        shares[i]);
        }
        if (blended.empty()) {
            // The nearest surface is there but unskinned: nothing to give, so
            // the point keeps what it has, as an out-of-reach one does.
            ++result.unreached;
            continue;
        }
        wrote.push_back(point);
        given.add(blended);
        ++result.transferred;
    }

    if (wrote.empty()) {
        return result;
    }
    SkinScope span;
    span.points = wrote;
    span.heldBones = held;
    result.weights = Assign(target, nodes, points, span, given);
    return result;
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
