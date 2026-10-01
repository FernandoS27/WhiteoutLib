// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/uv/layout.h>

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

namespace {

/// The corners of one island, each exactly once: what a move writes.
std::vector<u32> cornersOfIsland(const Mesh& mesh, const UvIslands& islands, u32 island) {
    std::vector<u32> out;
    const Topology& topology = std::as_const(mesh).topology();
    for (const u32 face : islands.facesOf(island)) {
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            out.push_back(h.index());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

UvBounds BoundsOf(const Mesh& mesh, const UvIslands& islands, u32 island, u32 set) {
    UvBounds out;
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
    if (uvs.empty() || !mesh.hasConnectivity()) {
        return out;
    }
    for (const u32 corner : cornersOfIsland(mesh, islands, island)) {
        if (corner >= uvs.size()) {
            continue;
        }
        const Vector2f& value = uvs[corner];
        if (out.empty) {
            out.low = out.high = value;
            out.empty = false;
            continue;
        }
        out.low.x = std::min(out.low.x, value.x);
        out.low.y = std::min(out.low.y, value.y);
        out.high.x = std::max(out.high.x, value.x);
        out.high.y = std::max(out.high.y, value.y);
    }
    return out;
}

bool IslandIsFree(const Mesh& mesh, const UvIslands& islands, u32 island, u32 set) {
    const std::span<const u8> free =
        mesh.attributes.get<const u8>(names::uvFree(set), Domain::Face);
    if (free.empty()) {
        return false;
    }
    const std::span<const u32> faces = islands.facesOf(island);
    if (faces.empty()) {
        return false;
    }
    for (const u32 face : faces) {
        if (face >= free.size() || free[face] == 0) {
            return false;
        }
    }
    return true;
}

void TransformIsland(Mesh& mesh, const UvIslands& islands, u32 island, u32 set, f32 scale,
                     const Vector2f& delta) {
    const UvBounds bounds = BoundsOf(mesh, islands, island, set);
    if (bounds.empty) {
        return;
    }
    const Vector2f centre{(bounds.low.x + bounds.high.x) * 0.5f,
                          (bounds.low.y + bounds.high.y) * 0.5f};
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);
    for (const u32 corner : cornersOfIsland(mesh, islands, island)) {
        if (corner >= uvs.size()) {
            continue;
        }
        Vector2f& value = uvs[corner];
        value.x = centre.x + (value.x - centre.x) * scale + delta.x;
        value.y = centre.y + (value.y - centre.y) * scale + delta.y;
    }
}

f32 TexelDensity(const Mesh& mesh, const UvIslands& islands, u32 island, u32 set,
                 u32 resolution) {
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
    if (positions.empty() || uvs.empty()) {
        return 0.0f;
    }
    const detail::IslandArea areas = detail::AreasOf(mesh, islands, island, positions, uvs);
    if (areas.world <= 0.0f || areas.uv <= 0.0f) {
        return 0.0f;
    }
    // Texels across, not texels squared: a density anyone can compare by eye
    // with "how many pixels per metre".
    return static_cast<f32>(resolution) * std::sqrt(areas.uv / areas.world);
}

f32 MedianDensity(const Mesh& mesh, const UvIslands& islands, u32 set, u32 resolution,
                  bool lockedOnly) {
    std::vector<f32> values;
    values.reserve(islands.count);
    for (u32 island = 0; island < islands.count; ++island) {
        if (lockedOnly && IslandIsFree(mesh, islands, island, set)) {
            continue;
        }
        const f32 density = TexelDensity(mesh, islands, island, set, resolution);
        if (density > 0.0f) {
            values.push_back(density);
        }
    }
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void PlaceInStrip(Mesh& mesh, const UvIslands& islands, u32 island, u32 set, f32 density,
                  u32 resolution) {
    const f32 current = TexelDensity(mesh, islands, island, set, resolution);
    const f32 scale = (current > 0.0f && density > 0.0f) ? density / current : 1.0f;
    // Scaled about its own centre first, so the size is right before the place
    // is worked out.
    TransformIsland(mesh, islands, island, set, scale, Vector2f{0.0f, 0.0f});

    // The strip's own occupancy: the highest any island already in it reaches.
    // Stacking upward is deterministic and leaves a column a modeller can read
    // top to bottom in the order things were made.
    constexpr f32 kStripLow = 1.0f;
    constexpr f32 kGap = 0.02f;
    f32 top = 0.0f;
    for (u32 other = 0; other < islands.count; ++other) {
        if (other == island) {
            continue;
        }
        const UvBounds bounds = BoundsOf(mesh, islands, other, set);
        if (bounds.empty || bounds.high.x < kStripLow) {
            continue;
        }
        top = std::max(top, bounds.high.y);
    }
    const UvBounds mine = BoundsOf(mesh, islands, island, set);
    if (mine.empty) {
        return;
    }
    TransformIsland(mesh, islands, island, set, 1.0f,
                    Vector2f{kStripLow + kGap - mine.low.x, top + kGap - mine.low.y});
}

namespace {

/// One island as the packer sees it: a mask of the texels it covers, already
/// dilated by the padding, and the column profile the free rows are tested
/// against. A moving island needs only the profile, so it has no mask.
struct Footprint {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> mask;
    /// Per column: the lowest and highest row it covers, or `kInvalidId` for a
    /// column the island does not reach at all.
    std::vector<u32> low;
    std::vector<u32> high;
    /// Where the mask's corner sits in UV, so a placement can be turned back
    /// into a move.
    Vector2f origin{0.0f, 0.0f};
    u32 covered = 0;

    bool empty() const {
        return covered == 0;
    }
};

/// Fills the texels the triangle covers and the ring round them: a
/// conservative cover, so no island is packed closer than it looks, and a seam
/// never lands half in and half out.
void rasterTriangle(Footprint& out, const Vector2f& a, const Vector2f& b, const Vector2f& c) {
    detail::RasterTriangle(out.width, out.height, a, b, c, true,
                           [&](u32 x, u32 y) { out.mask[y * out.width + x] = 1; });
}

/// Grows @p out by @p padding texels in every direction, separably.
void dilate(Footprint& out, u32 padding) {
    if (padding == 0 || out.mask.empty()) {
        return;
    }
    std::vector<u8> pass(out.mask.size(), 0);
    for (u32 y = 0; y < out.height; ++y) {
        for (u32 x = 0; x < out.width; ++x) {
            if (out.mask[y * out.width + x] == 0) {
                continue;
            }
            const u32 from = x > padding ? x - padding : 0;
            const u32 to = std::min(out.width - 1, x + padding);
            for (u32 k = from; k <= to; ++k) {
                pass[y * out.width + k] = 1;
            }
        }
    }
    std::fill(out.mask.begin(), out.mask.end(), static_cast<u8>(0));
    for (u32 y = 0; y < out.height; ++y) {
        for (u32 x = 0; x < out.width; ++x) {
            if (pass[y * out.width + x] == 0) {
                continue;
            }
            const u32 from = y > padding ? y - padding : 0;
            const u32 to = std::min(out.height - 1, y + padding);
            for (u32 k = from; k <= to; ++k) {
                out.mask[k * out.width + x] = 1;
            }
        }
    }
}

void profile(Footprint& out) {
    out.low.assign(out.width, kInvalidId);
    out.high.assign(out.width, kInvalidId);
    out.covered = 0;
    for (u32 y = 0; y < out.height; ++y) {
        for (u32 x = 0; x < out.width; ++x) {
            if (out.mask[y * out.width + x] == 0) {
                continue;
            }
            ++out.covered;
            if (out.low[x] == kInvalidId) {
                out.low[x] = y;
            }
            out.high[x] = y;
        }
    }
}

/// The profile alone, as `profile` would read it off the dilated mask: a
/// column's padded span reaches from its neighbours' lowest to their highest.
void profileOnly(Footprint& out, u32 padding) {
    const std::vector<u32> low = out.low;
    const std::vector<u32> high = out.high;
    out.covered = 0;
    for (u32 x = 0; x < out.width; ++x) {
        const u32 from = x > padding ? x - padding : 0;
        const u32 to = std::min(out.width - 1, x + padding);
        u32 lowest = kInvalidId;
        u32 highest = 0;
        for (u32 k = from; k <= to; ++k) {
            if (low[k] != kInvalidId) {
                lowest = std::min(lowest, low[k]);
                highest = std::max(highest, high[k]);
            }
        }
        if (lowest == kInvalidId) {
            out.low[x] = out.high[x] = kInvalidId;
            continue;
        }
        out.low[x] = lowest > padding ? lowest - padding : 0;
        out.high[x] = std::min(out.height - 1, highest + padding);
        out.covered += out.high[x] - out.low[x] + 1;
    }
}

/// A group of islands as the packer rasterises it: its UV triangles, three
/// corners each, and the centre it turns about. Read once, since nothing moves
/// until the layout is written.
struct Shape {
    std::vector<Vector2f> corners;
    Vector2f centre{0.0f, 0.0f};
};

Shape shapeOf(const Mesh& mesh, const UvIslands& islands, std::span<const u32> group, u32 set) {
    Shape out;
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
    if (uvs.empty()) {
        return out;
    }
    UvBounds whole;
    for (const u32 island : group) {
        const UvBounds bounds = BoundsOf(mesh, islands, island, set);
        if (bounds.empty) {
            continue;
        }
        if (whole.empty) {
            whole = bounds;
            continue;
        }
        whole.low.x = std::min(whole.low.x, bounds.low.x);
        whole.low.y = std::min(whole.low.y, bounds.low.y);
        whole.high.x = std::max(whole.high.x, bounds.high.x);
        whole.high.y = std::max(whole.high.y, bounds.high.y);
    }
    if (whole.empty) {
        return out;
    }
    out.centre = Vector2f{(whole.low.x + whole.high.x) * 0.5f, (whole.low.y + whole.high.y) * 0.5f};
    for (const u32 island : group) {
        for (const u32 face : islands.facesOf(island)) {
            for (const detail::Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
                for (u32 k = 0; k < 3; ++k) {
                    out.corners.push_back(uvs[tri.corner[k].index()]);
                }
            }
        }
    }
    return out;
}

/// The footprint of a group of islands taken together (a stack is one thing to
/// the packer), at @p texels per UV unit, turned by @p radians about the
/// group's own centre.
Footprint footprintOf(const Shape& shape, f32 texels, u32 padding, f32 radians, bool mask = true) {
    Footprint out;
    if (shape.corners.empty()) {
        return out;
    }
    const Vector2f centre = shape.centre;
    const f32 sine = std::sin(radians);
    const f32 cosine = std::cos(radians);
    const auto turn = [&](const Vector2f& value) {
        const f32 dx = value.x - centre.x;
        const f32 dy = value.y - centre.y;
        return Vector2f{centre.x + dx * cosine - dy * sine, centre.y + dx * sine + dy * cosine};
    };

    // The turned bounds, which is what the mask has to hold.
    UvBounds turned;
    for (const Vector2f& corner : shape.corners) {
        const Vector2f value = turn(corner);
        if (turned.empty) {
            turned.low = turned.high = value;
            turned.empty = false;
            continue;
        }
        turned.low.x = std::min(turned.low.x, value.x);
        turned.low.y = std::min(turned.low.y, value.y);
        turned.high.x = std::max(turned.high.x, value.x);
        turned.high.y = std::max(turned.high.y, value.y);
    }
    const u32 margin = padding + 2;
    out.origin = turned.low;
    out.width = static_cast<u32>(std::ceil(turned.width() * texels)) + 2 * margin;
    out.height = static_cast<u32>(std::ceil(turned.height() * texels)) + 2 * margin;
    out.width = std::max<u32>(out.width, 1);
    out.height = std::max<u32>(out.height, 1);
    // A footprint bigger than any tile is not a footprint; the caller scales.
    if (static_cast<u64>(out.width) * out.height > 64ull * 1024 * 1024) {
        out.width = out.height = 0;
        return out;
    }
    if (mask) {
        out.mask.assign(static_cast<std::size_t>(out.width) * out.height, 0);
    } else {
        out.low.assign(out.width, kInvalidId);
        out.high.assign(out.width, 0);
    }
    const auto place = [&](const Vector2f& value) {
        const Vector2f t = turn(value);
        return Vector2f{(t.x - turned.low.x) * texels + static_cast<f32>(margin),
                        (t.y - turned.low.y) * texels + static_cast<f32>(margin)};
    };
    for (std::size_t i = 0; i + 2 < shape.corners.size(); i += 3) {
        const Vector2f a = place(shape.corners[i]);
        const Vector2f b = place(shape.corners[i + 1]);
        const Vector2f c = place(shape.corners[i + 2]);
        if (mask) {
            rasterTriangle(out, a, b, c);
            continue;
        }
        detail::RasterTriangle(out.width, out.height, a, b, c, true, [&](u32 x, u32 y) {
            out.low[x] = std::min(out.low[x], y);
            out.high[x] = std::max(out.high[x], y);
        });
    }
    if (!mask) {
        profileOnly(out, padding);
        return out;
    }
    dilate(out, padding);
    profile(out);
    return out;
}

/// The principal axis of an island group's UV points, as an angle: turning by
/// its negative lays the island along x, which is the first thing to try
/// because a long thin island packs best lying down.
f32 principalAngle(const Mesh& mesh, const UvIslands& islands, std::span<const u32> group,
                   u32 set) {
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
    if (uvs.empty()) {
        return 0.0f;
    }
    f64 sumX = 0.0;
    f64 sumY = 0.0;
    u32 count = 0;
    const Topology& topology = std::as_const(mesh).topology();
    for (const u32 island : group) {
        for (const u32 face : islands.facesOf(island)) {
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                sumX += uvs[h.index()].x;
                sumY += uvs[h.index()].y;
                ++count;
            }
        }
    }
    if (count == 0) {
        return 0.0f;
    }
    const f64 meanX = sumX / count;
    const f64 meanY = sumY / count;
    f64 xx = 0.0;
    f64 yy = 0.0;
    f64 xy = 0.0;
    for (const u32 island : group) {
        for (const u32 face : islands.facesOf(island)) {
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                const f64 dx = uvs[h.index()].x - meanX;
                const f64 dy = uvs[h.index()].y - meanY;
                xx += dx * dx;
                yy += dy * dy;
                xy += dx * dy;
            }
        }
    }
    return static_cast<f32>(-0.5 * std::atan2(2.0 * xy, xx - yy));
}

} // namespace

namespace {

/// Scales, turns and moves a whole group about one centre. A stack moves
/// rigidly or it stops being a stack, so the group's centre is the group's and
/// never each island's own.
void transformGroup(Mesh& mesh, const UvIslands& islands, std::span<const u32> group, u32 set,
                    const Vector2f& centre, f32 scale, f32 radians, const Vector2f& delta) {
    const f32 sine = std::sin(radians);
    const f32 cosine = std::cos(radians);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);
    for (const u32 island : group) {
        for (const u32 corner : cornersOfIsland(mesh, islands, island)) {
            if (corner >= uvs.size()) {
                continue;
            }
            const f32 dx = (uvs[corner].x - centre.x) * scale;
            const f32 dy = (uvs[corner].y - centre.y) * scale;
            uvs[corner] = Vector2f{centre.x + dx * cosine - dy * sine + delta.x,
                                   centre.y + dx * sine + dy * cosine + delta.y};
        }
    }
}

} // namespace

void TurnIsland(Mesh& mesh, const UvIslands& islands, u32 island, u32 set, f32 radians,
                const Vector2f& delta) {
    const UvBounds bounds = BoundsOf(mesh, islands, island, set);
    if (bounds.empty) {
        return;
    }
    const Vector2f centre{(bounds.low.x + bounds.high.x) * 0.5f,
                          (bounds.low.y + bounds.high.y) * 0.5f};
    const f32 sine = std::sin(radians);
    const f32 cosine = std::cos(radians);
    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);
    const Topology& topology = std::as_const(mesh).topology();
    std::vector<u32> corners;
    for (const u32 face : islands.facesOf(island)) {
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            corners.push_back(h.index());
        }
    }
    std::sort(corners.begin(), corners.end());
    corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
    for (const u32 corner : corners) {
        if (corner >= uvs.size()) {
            continue;
        }
        const f32 dx = uvs[corner].x - centre.x;
        const f32 dy = uvs[corner].y - centre.y;
        uvs[corner] = Vector2f{centre.x + dx * cosine - dy * sine + delta.x,
                               centre.y + dx * sine + dy * cosine + delta.y};
    }
}

void MatchDensity(Mesh& mesh, const UvIslands& islands, std::span<const u32> islandList, u32 set,
                  f32 density, u32 resolution) {
    if (density <= 0.0f) {
        return;
    }
    for (const u32 island : islandList) {
        const f32 current = TexelDensity(mesh, islands, island, set, resolution);
        if (current <= 0.0f) {
            continue;
        }
        TransformIsland(mesh, islands, island, set, density / current, Vector2f{0.0f, 0.0f});
    }
}

std::vector<std::vector<u32>> FindStacks(const Mesh& mesh, const UvIslands& islands, u32 set) {
    std::vector<std::vector<u32>> out;
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
    if (uvs.empty() || islands.count == 0) {
        return out;
    }
    // Two islands are stacked when their UV points are the same points. The
    // sorted list of them is the identity, rounded to the bit depth a file
    // would have written anyway.
    struct Key {
        std::vector<Vector2f> points;
        u32 island = 0;
    };
    std::vector<Key> keys;
    keys.reserve(islands.count);
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 island = 0; island < islands.count; ++island) {
        Key key;
        key.island = island;
        for (const u32 face : islands.facesOf(island)) {
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                key.points.push_back(uvs[h.index()]);
            }
        }
        std::sort(key.points.begin(), key.points.end(), [](const Vector2f& a, const Vector2f& b) {
            return a.x != b.x ? a.x < b.x : a.y < b.y;
        });
        key.points.erase(std::unique(key.points.begin(), key.points.end(),
                                     [](const Vector2f& a, const Vector2f& b) {
                                         return a.x == b.x && a.y == b.y;
                                     }),
                         key.points.end());
        keys.push_back(std::move(key));
    }
    // Within half a texel of a 64k map. Two islands a file laid on one another
    // are the same points, and a point that has been through a scale and a turn
    // is the same point it was -- to a float, not to the bit, which is why this
    // is a tolerance and not a comparison.
    constexpr f32 kSame = 1.0f / 131072.0f;
    // Each point of one matched to its own point of the other: not index by
    // index, since two points a hair apart in u sort one way in an island and
    // the other way in its twin, and a stack Pack kept would read as an overlap.
    const auto samepoints = [](const std::vector<Vector2f>& a, const std::vector<Vector2f>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        std::vector<u8> used(b.size(), 0);
        for (const Vector2f& p : a) {
            // Sorted by u, so only those within reach of p's u can match.
            auto it = std::lower_bound(b.begin(), b.end(), p.x - kSame,
                                       [](const Vector2f& q, f32 u) { return q.x < u; });
            bool found = false;
            for (; it != b.end() && it->x <= p.x + kSame; ++it) {
                const std::size_t k = static_cast<std::size_t>(it - b.begin());
                if (used[k] == 0 && std::abs(it->y - p.y) <= kSame) {
                    used[k] = 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    };
    std::vector<u8> taken(islands.count, 0);
    for (u32 i = 0; i < keys.size(); ++i) {
        if (taken[i] != 0 || keys[i].points.empty()) {
            continue;
        }
        std::vector<u32> group{keys[i].island};
        for (u32 j = i + 1; j < keys.size(); ++j) {
            if (taken[j] == 0 && samepoints(keys[i].points, keys[j].points)) {
                taken[j] = 1;
                group.push_back(keys[j].island);
            }
        }
        if (group.size() > 1) {
            out.push_back(std::move(group));
        }
    }
    return out;
}

PackResult Pack(Mesh& mesh, const UvIslands& islands, u32 set, const PackOptions& options) {
    const PackInput input{&mesh, &islands, set, options.movable, options.groupOf};
    PackResult out = PackMeshes(std::span<const PackInput>(&input, 1), options);
    if (!out.unplacedByInput.empty()) {
        out.unplaced = out.unplacedByInput.front();
    }
    return out;
}

PackResult PackMeshes(std::span<const PackInput> inputs, const PackOptions& options) {
    PackResult out;
    out.unplacedByInput.resize(inputs.size());
    if (inputs.empty() || options.resolution == 0) {
        return out;
    }
    const u32 resolution = options.resolution;

    // --- the groups: a stack is one thing to the packer, and so is a caller's
    // group. Neither crosses a mesh --------------------------------------------
    struct Group {
        u32 input = 0;
        std::vector<u32> islands;
    };
    std::vector<Group> groups;
    for (u32 i = 0; i < inputs.size(); ++i) {
        const PackInput& in = inputs[i];
        if (!in.mesh || !in.islands || !in.mesh->hasConnectivity() || in.islands->count == 0) {
            continue;
        }
        const Mesh& mesh = *in.mesh;
        const UvIslands& islands = *in.islands;
        std::vector<u32> root(islands.count);
        for (u32 island = 0; island < islands.count; ++island) {
            root[island] = island;
        }
        const auto find = [&](u32 island) {
            while (root[island] != island) {
                root[island] = root[root[island]];
                island = root[island];
            }
            return island;
        };
        const auto unite = [&](u32 a, u32 b) {
            a = find(a);
            b = find(b);
            if (a != b) {
                root[std::max(a, b)] = std::min(a, b);
            }
        };
        if (options.keepStacks) {
            for (const std::vector<u32>& stack : FindStacks(mesh, islands, in.set)) {
                for (const u32 island : stack) {
                    unite(stack.front(), island);
                }
            }
        }
        std::unordered_map<u32, u32> firstOf;
        for (u32 island = 0; island < islands.count && island < in.groupOf.size(); ++island) {
            const u32 group = in.groupOf[island];
            if (group == kInvalidId) {
                continue;
            }
            const auto [at, inserted] = firstOf.emplace(group, island);
            if (!inserted) {
                unite(at->second, island);
            }
        }
        std::vector<u32> groupOf(islands.count, kInvalidId);
        for (u32 island = 0; island < islands.count; ++island) {
            const u32 top = find(island);
            if (groupOf[top] == kInvalidId) {
                groupOf[top] = static_cast<u32>(groups.size());
                groups.push_back(Group{i, {}});
            }
            groupOf[island] = groupOf[top];
            groups[groupOf[island]].islands.push_back(island);
        }
    }
    const auto meshOf = [&](const Group& g) -> Mesh& { return *inputs[g.input].mesh; };
    const auto islandsOf = [&](const Group& g) -> const UvIslands& { return *inputs[g.input].islands; };
    const auto setOf = [&](const Group& g) { return inputs[g.input].set; };

    // --- moving and fixed ----------------------------------------------------
    const auto moves = [&](const Group& g, u32 island) {
        const PackInput& in = inputs[g.input];
        if (options.leavePinned && IslandIsPinned(*in.mesh, *in.islands, island, in.set)) {
            return false;
        }
        if (in.movable.empty()) {
            return IslandIsFree(*in.mesh, *in.islands, island, in.set);
        }
        return island < in.movable.size() && in.movable[island] != 0;
    };
    std::vector<u8> groupFree(groups.size(), 1);
    for (u32 g = 0; g < groups.size(); ++g) {
        for (const u32 island : groups[g].islands) {
            // A stack with one fixed member is fixed whole: moving half of a
            // stack is what breaks it.
            if (!moves(groups[g], island)) {
                groupFree[g] = 0;
                break;
            }
        }
    }

    // --- the obstacles, from every mesh on the one map, and the tiling
    // refusal ------------------------------------------------------------------
    std::vector<u8> occupied(static_cast<std::size_t>(resolution) * resolution, 0);
    bool anyLocked = false;
    for (u32 g = 0; g < groups.size(); ++g) {
        if (groupFree[g] != 0) {
            continue;
        }
        anyLocked = true;
        const UvBounds bounds = BoundsOf(meshOf(groups[g]), islandsOf(groups[g]), groups[g].islands.front(),
                                         setOf(groups[g]));
        if (options.refuseTiling && !bounds.empty &&
            (bounds.low.x < -0.001f || bounds.low.y < -0.001f || bounds.high.x > 1.001f ||
             bounds.high.y > 1.001f)) {
            // Deliberately outside the tile: the layout tiles, and packing it
            // in would undo what someone meant.
            out.refusedTiling = true;
        }
        const Footprint mark =
            footprintOf(shapeOf(meshOf(groups[g]), islandsOf(groups[g]), groups[g].islands, setOf(groups[g])),
                        static_cast<f32>(resolution), 0, 0.0f);
        if (mark.empty()) {
            continue;
        }
        const i32 baseX = static_cast<i32>(std::floor(mark.origin.x * resolution)) -
                          static_cast<i32>(2);
        const i32 baseY = static_cast<i32>(std::floor(mark.origin.y * resolution)) -
                          static_cast<i32>(2);
        for (u32 y = 0; y < mark.height; ++y) {
            for (u32 x = 0; x < mark.width; ++x) {
                if (mark.mask[y * mark.width + x] == 0) {
                    continue;
                }
                const i32 tx = baseX + static_cast<i32>(x);
                const i32 ty = baseY + static_cast<i32>(y);
                if (tx < 0 || ty < 0 || tx >= static_cast<i32>(resolution) ||
                    ty >= static_cast<i32>(resolution)) {
                    continue;
                }
                occupied[static_cast<std::size_t>(ty) * resolution + static_cast<u32>(tx)] = 1;
            }
        }
    }
    if (out.refusedTiling && anyLocked) {
        return out;
    }
    out.refusedTiling = false;

    // --- the order: largest first, by area then by mesh and first face -------
    std::vector<u32> order;
    for (u32 g = 0; g < groups.size(); ++g) {
        if (groupFree[g] != 0) {
            order.push_back(g);
        }
    }
    std::vector<f32> areaOf(groups.size(), 0.0f);
    // The largest island of each: what its print covers at the least, since a
    // stack's islands lie on one another.
    std::vector<f32> floorOf(groups.size(), 0.0f);
    std::vector<u32> firstFaceOf(groups.size(), kInvalidId);
    for (u32 g = 0; g < groups.size(); ++g) {
        const Mesh& mesh = meshOf(groups[g]);
        const UvIslands& islands = islandsOf(groups[g]);
        const std::span<const Vector3f> positions =
            mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
        const std::span<const Vector2f> uvs =
            mesh.attributes.get<const Vector2f>(names::uv(setOf(groups[g])), Domain::Halfedge);
        for (const u32 island : groups[g].islands) {
            const f32 area = detail::AreasOf(mesh, islands, island, positions, uvs).uv;
            areaOf[g] += area;
            floorOf[g] = std::max(floorOf[g], area);
            const std::span<const u32> faces = islands.facesOf(island);
            if (!faces.empty()) {
                firstFaceOf[g] = std::min(firstFaceOf[g], faces.front());
            }
        }
    }
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        if (areaOf[a] != areaOf[b]) {
            return areaOf[a] > areaOf[b];
        }
        if (groups[a].input != groups[b].input) {
            return groups[a].input < groups[b].input;
        }
        return firstFaceOf[a] < firstFaceOf[b];
    });
    if (order.empty()) {
        return out;
    }

    // --- per column, the rows the obstacles cover, as runs -------------------
    // Not a skyline: a fixed island at the top of the tile would close every
    // column above the free space below it, and a shipped layout often has one.
    using Runs = std::vector<std::vector<std::pair<u32, u32>>>;
    const auto runsOf = [&]() {
        Runs runs(resolution);
        for (u32 x = 0; x < resolution; ++x) {
            for (u32 y = 0; y < resolution;) {
                if (occupied[static_cast<std::size_t>(y) * resolution + x] == 0) {
                    ++y;
                    continue;
                }
                u32 end = y;
                while (end + 1 < resolution &&
                       occupied[static_cast<std::size_t>(end + 1) * resolution + x] != 0) {
                    ++end;
                }
                runs[x].emplace_back(y, end);
                y = end + 1;
            }
        }
        return runs;
    };
    // The lowest row a print can sit at with its left column at @p x, each of
    // its columns' spans clear of that column's runs; `kInvalidId` for none,
    // or for none below @p limit, which is all the caller could still use.
    // Every push is one any clear row has to make too, so the order columns
    // are tried in cannot change the answer: @p blocker, the tile column that
    // pushed last, goes first, since the next x usually meets it again.
    const auto lowestAt = [&](const Footprint& print, u32 x, const Runs& runs, u32 limit, u32& blocker) {
        // Moves `y` above whatever run is in column @p c's way.
        const auto push = [&](u32 c, u32& y) {
            if (print.low[c] == kInvalidId) {
                return false;
            }
            const u32 from = y + print.low[c];
            const u32 to = y + print.high[c];
            // A column's runs are sorted and apart: the first that ends at or
            // past `from` is the only one that can be in the way.
            const std::vector<std::pair<u32, u32>>& column = runs[x + c];
            const auto run = std::lower_bound(column.begin(), column.end(), from,
                                              [](const std::pair<u32, u32>& r, u32 row) { return r.second < row; });
            if (run == column.end() || run->first > to) {
                return false;
            }
            y = run->second + 1 - print.low[c];
            return true;
        };
        u32 y = 0;
        for (bool moved = true; moved;) {
            if (y >= limit) {
                return kInvalidId;
            }
            moved = blocker >= x && blocker < x + print.width && push(blocker - x, y);
            for (u32 c = 0; c < print.width && !moved; ++c) {
                if (push(c, y)) {
                    moved = true;
                    blocker = x + c;
                }
            }
            if (y + print.height > resolution) {
                return kInvalidId;
            }
        }
        return y;
    };

    // Angles to try: the island lying along its own long axis first, then the
    // four quarter turns of that. A turn is free and a bad fit is not.
    const auto anglesFor = [&](const Group& group) {
        std::vector<f32> angles;
        if (!options.rotate) {
            angles.push_back(0.0f);
            return angles;
        }
        constexpr f32 kHalfPi = 1.57079632679f;
        const f32 principal = principalAngle(meshOf(group), islandsOf(group), group.islands, setOf(group));
        angles.push_back(principal);
        for (u32 k = 1; k < 4; ++k) {
            angles.push_back(principal + kHalfPi * static_cast<f32>(k));
        }
        angles.push_back(0.0f);
        return angles;
    };
    std::vector<std::vector<f32>> angles(groups.size());
    std::vector<Shape> shapes(groups.size());
    for (const u32 g : order) {
        angles[g] = anglesFor(groups[g]);
        shapes[g] = shapeOf(meshOf(groups[g]), islandsOf(groups[g]), groups[g].islands, setOf(groups[g]));
    }

    // One attempt at the whole layout, at `scale` over the moving islands.
    struct Placement {
        u32 group = 0;
        f32 angle = 0.0f;
        Vector2f centre{0.0f, 0.0f};
        Vector2f delta{0.0f, 0.0f};
    };
    // With @p skipped, a group with no room is noted and the rest go on; without
    // it, the attempt fails.
    // No layout fits whose islands alone cover more than the free texels.
    const f64 freeTexels = static_cast<f64>(std::count(occupied.begin(), occupied.end(), static_cast<u8>(0)));
    const auto attempt = [&](f32 scale, std::vector<Placement>& placements,
                             std::vector<u32>* skipped) {
        placements.clear();
        Runs runs = runsOf();
        const f64 texels = static_cast<f64>(resolution) * scale;
        f64 free = freeTexels;
        f64 needed = 0.0;
        for (const u32 g : order) {
            needed += static_cast<f64>(floorOf[g]) * texels * texels;
        }
        for (const u32 g : order) {
            // What is left cannot fit in what is free: fail now, not at the end.
            if (!skipped && needed > free) {
                return false;
            }
            needed -= static_cast<f64>(floorOf[g]) * texels * texels;
            bool placed = false;
            f32 bestAngle = 0.0f;
            u32 bestX = 0;
            u32 bestY = 0;
            u32 bestTop = kInvalidId;
            Vector2f bestOrigin{0.0f, 0.0f};
            Footprint chosen;
            bool anyPrint = false;
            const Vector2f centre = shapes[g].centre;
            for (const f32 angle : angles[g]) {
                Footprint print =
                    footprintOf(shapes[g], static_cast<f32>(resolution) * scale, options.padding, angle, false);
                if (print.empty()) {
                    continue;
                }
                anyPrint = true;
                if (print.width > resolution || print.height > resolution) {
                    continue;
                }
                u32 height = 0;
                for (u32 c = 0; c < print.width; ++c) {
                    if (print.high[c] != kInvalidId) {
                        height = std::max(height, print.high[c]);
                    }
                }
                bool better = false;
                u32 blocker = kInvalidId;
                for (u32 x = 0; x + print.width <= resolution; ++x) {
                    // The lowest the island can sit at this column, clear of
                    // everything in its way; only a lower top than the best
                    // so far is worth the search.
                    const u32 limit = bestTop == kInvalidId ? resolution : (bestTop > height ? bestTop - height : 0);
                    const u32 y = lowestAt(print, x, runs, limit, blocker);
                    if (y == kInvalidId) {
                        continue;
                    }
                    bestTop = y + height;
                    bestAngle = angle;
                    bestX = x;
                    bestY = y;
                    bestOrigin = print.origin;
                    placed = better = true;
                }
                if (better) {
                    chosen = std::move(print);
                }
            }
            if (!anyPrint) {
                // No area at all (a map not made yet): nothing to place, and
                // nothing to stop the rest being placed. It stays where it is.
                continue;
            }
            if (!placed) {
                if (!skipped) {
                    return false;
                }
                skipped->push_back(g);
                continue;
            }
            // Where the mask landed, turned back into a move in UV.
            const f32 margin = static_cast<f32>(options.padding + 2);
            // The island is scaled and turned about its own centre, so the move
            // that lands its corner on the chosen texel has to undo what
            // scaling about the centre did to that corner.
            const Vector2f delta{
                (static_cast<f32>(bestX) + margin) / static_cast<f32>(resolution) -
                    bestOrigin.x * scale - centre.x * (1.0f - scale),
                (static_cast<f32>(bestY) + margin) / static_cast<f32>(resolution) -
                    bestOrigin.y * scale - centre.y * (1.0f - scale)};
            placements.push_back(Placement{g, bestAngle, centre, delta});
            free -= static_cast<f64>(chosen.covered);
            for (u32 c = 0; c < chosen.width; ++c) {
                if (chosen.high[c] == kInvalidId) {
                    continue;
                }
                std::vector<std::pair<u32, u32>>& column = runs[bestX + c];
                column.emplace_back(bestY + chosen.low[c], bestY + chosen.high[c]);
                std::sort(column.begin(), column.end());
                std::vector<std::pair<u32, u32>> merged;
                for (const auto& run : column) {
                    if (!merged.empty() && run.first <= merged.back().second + 1) {
                        merged.back().second = std::max(merged.back().second, run.second);
                    } else {
                        merged.push_back(run);
                    }
                }
                column = std::move(merged);
            }
        }
        return true;
    };

    // --- fit, scaling the moving islands down together if they will not ------
    std::vector<Placement> placements;
    f32 scale = 1.0f;
    f32 ceiling = 1.0f;
    bool fitted = !options.allowScale;
    if (!options.allowScale) {
        std::vector<u32> skipped;
        attempt(scale, placements, &skipped);
        for (const u32 g : skipped) {
            std::vector<u32>& unplaced = out.unplacedByInput[groups[g].input];
            unplaced.insert(unplaced.end(), groups[g].islands.begin(), groups[g].islands.end());
        }
        for (std::vector<u32>& unplaced : out.unplacedByInput) {
            std::sort(unplaced.begin(), unplaced.end());
        }
    } else {
        // The search starts below the scale the islands' area alone rules
        // out, not at a first attempt sure to fail.
        f64 needed = 0.0;
        for (const u32 g : order) {
            needed += static_cast<f64>(floorOf[g]) * resolution * resolution;
        }
        ceiling = needed > freeTexels ? static_cast<f32>(std::sqrt(freeTexels / needed)) : 1.0f;
        fitted = ceiling >= 1.0f && attempt(scale, placements, nullptr);
    }
    if (!fitted) {
        f32 low = 0.0f;
        f32 high = ceiling;
        std::vector<Placement> fits;
        for (u32 step = 0; step < 8; ++step) {
            const f32 middle = (low + high) * 0.5f;
            if (attempt(middle, placements, nullptr)) {
                low = middle;
                fits = placements;
            } else {
                high = middle;
            }
        }
        if (low <= 0.0f) {
            return out;
        }
        placements = std::move(fits);
        scale = low;
    }
    out.scaled = scale;

    // --- write it ------------------------------------------------------------
    for (const Placement& placement : placements) {
        const Group& group = groups[placement.group];
        transformGroup(meshOf(group), islandsOf(group), group.islands, setOf(group), placement.centre, scale,
                       placement.angle, placement.delta);
        ++out.placed;
    }

    // --- what it covers ------------------------------------------------------
    f32 covered = 0.0f;
    for (const PackInput& in : inputs) {
        if (!in.mesh || !in.islands) {
            continue;
        }
        const std::span<const Vector3f> positions =
            in.mesh->attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
        const std::span<const Vector2f> uvs =
            in.mesh->attributes.get<const Vector2f>(names::uv(in.set), Domain::Halfedge);
        for (u32 island = 0; island < in.islands->count; ++island) {
            covered += detail::AreasOf(*in.mesh, *in.islands, island, positions, uvs).uv;
        }
    }
    out.coverage = std::clamp(covered, 0.0f, 1.0f);
    return out;
}

f32 OrientIsland(Mesh& mesh, const UvIslands& islands, u32 island, u32 set) {
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
    if (uvs.empty() || !mesh.hasConnectivity()) {
        return 0.0f;
    }
    std::vector<Vector2f> points;
    for (const u32 corner : cornersOfIsland(mesh, islands, island)) {
        if (corner < uvs.size()) {
            points.push_back(uvs[corner]);
        }
    }
    const auto less = [](const Vector2f& a, const Vector2f& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; };
    std::sort(points.begin(), points.end(), less);
    points.erase(std::unique(points.begin(), points.end(),
                             [](const Vector2f& a, const Vector2f& b) { return a.x == b.x && a.y == b.y; }),
                 points.end());
    if (points.size() < 3) {
        return 0.0f;
    }
    // The convex hull, Andrew's monotone chain: the smallest box lies along one
    // of its edges.
    const auto turn = [](const Vector2f& o, const Vector2f& a, const Vector2f& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<Vector2f> hull(points.size() * 2);
    std::size_t k = 0;
    for (const Vector2f& p : points) {
        while (k >= 2 && turn(hull[k - 2], hull[k - 1], p) <= 0.0f) {
            --k;
        }
        hull[k++] = p;
    }
    for (std::size_t i = points.size() - 1, lower = k + 1; i-- > 0;) {
        while (k >= lower && turn(hull[k - 2], hull[k - 1], points[i]) <= 0.0f) {
            --k;
        }
        hull[k++] = points[i];
    }
    hull.resize(k > 1 ? k - 1 : k);
    // The box @p radians turns the hull into, as width and height.
    const auto boxOf = [&](f32 radians) {
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        f32 lowX = 1e30f, lowY = 1e30f, highX = -1e30f, highY = -1e30f;
        for (const Vector2f& p : hull) {
            const f32 x = p.x * c - p.y * s;
            const f32 y = p.x * s + p.y * c;
            lowX = std::min(lowX, x);
            highX = std::max(highX, x);
            lowY = std::min(lowY, y);
            highY = std::max(highY, y);
        }
        return Vector2f{highX - lowX, highY - lowY};
    };
    constexpr f32 kQuarter = 1.57079632679f;
    // Every edge's turn, folded to within an eighth of a turn of none: a
    // quarter turn more is the same box.
    f32 best = 0.0f;
    f32 bestArea = boxOf(0.0f).x * boxOf(0.0f).y;
    for (std::size_t i = 0; i < hull.size(); ++i) {
        const Vector2f& a = hull[i];
        const Vector2f& b = hull[(i + 1) % hull.size()];
        f32 t = -std::atan2(b.y - a.y, b.x - a.x);
        t -= kQuarter * std::round(t / kQuarter);
        const Vector2f box = boxOf(t);
        // Strictly smaller, by more than rounding: an island already square to
        // the axes stays as it is.
        if (box.x * box.y < bestArea * (1.0f - 1e-5f)) {
            bestArea = box.x * box.y;
            best = t;
        }
    }
    // Lying along u, as the packer tries first.
    const Vector2f box = boxOf(best);
    if (box.y > box.x * (1.0f + 1e-5f)) {
        best += best > 0.0f ? -kQuarter : kQuarter;
    }
    if (std::abs(best) < 1e-6f) {
        return 0.0f;
    }
    TurnIsland(mesh, islands, island, set, best, Vector2f{0.0f, 0.0f});
    return best;
}

bool IslandIsPinned(const Mesh& mesh, const UvIslands& islands, u32 island, u32 set) {
    const std::span<const u8> pins =
        mesh.attributes.get<const u8>(names::uvPin(set), Domain::Halfedge);
    const std::span<const u32> faces = islands.facesOf(island);
    if (pins.empty() || faces.empty() || !mesh.hasConnectivity()) {
        return false;
    }
    const Topology& topology = std::as_const(mesh).topology();
    for (const u32 face : faces) {
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            if (h.index() >= pins.size() || pins[h.index()] == 0) {
                return false;
            }
        }
    }
    return true;
}

std::vector<u32> PlaceInFreeSpace(Mesh& mesh, const UvIslands& islands, u32 set,
                                  std::span<const u8> movable, std::span<const u32> groupOf,
                                  u32 resolution, u32 padding) {
    PackOptions options;
    options.resolution = resolution;
    options.padding = padding;
    options.movable = movable;
    options.groupOf = groupOf;
    // At the size they were made: a new island is born at the model's density,
    // and shrinking it to fit would be a density nobody asked for.
    options.allowScale = false;
    options.refuseTiling = false;
    // New islands that happen to lie on one another -- a solve lays every
    // unmapped piece at the same place -- are not a stack a file meant.
    options.keepStacks = false;
    return Pack(mesh, islands, set, options).unplaced;
}

StackResult Stack(Mesh& mesh, const UvIslands& islands, u32 primary, u32 other, u32 set,
                  std::span<const u32> pointMirror) {
    StackResult out;
    if (!mesh.hasConnectivity() || primary >= islands.count || other >= islands.count ||
        primary == other) {
        out.refusedFaces = 1;
        return out;
    }
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const u32> primaryFaces = islands.facesOf(primary);
    const std::span<const u32> otherFaces = islands.facesOf(other);
    out.primaryFaces = static_cast<u32>(primaryFaces.size());
    out.otherFaces = static_cast<u32>(otherFaces.size());
    if (primaryFaces.size() != otherFaces.size() || otherFaces.empty()) {
        out.refusedFaces = out.otherFaces;
        return out;
    }

    // Every corner of the primary, by the vertex it sits at: what a pairing has
    // to look up. A vertex can hold several, one per face round it.
    std::unordered_multimap<u32, HalfedgeId> primaryCornerAt;
    for (const u32 face : primaryFaces) {
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            primaryCornerAt.emplace(topology.from(h).value(), h);
        }
    }

    // --- the mirror ----------------------------------------------------------
    // Face for face: each face of the other finds the primary face its
    // vertices' twins go round, in one direction for the whole island (a
    // mirror reverses it), and its corners take that face's. A near mirror
    // that pairs corners of different faces, or twists one, would fold the
    // copy. A face with no twin face -- its quad split along the other
    // diagonal -- takes each corner from the primary faces round its twins,
    // when those agree on one UV and the copy turns nothing over; failing
    // that, or with fewer than half the faces paired whole, the walk pairs.
    std::vector<std::pair<u32, u32>> pairing; // other corner -> primary corner
    if (!pointMirror.empty()) {
        const auto twinOf = [&](HalfedgeId h) {
            const u32 vertex = topology.from(h).value();
            return vertex < pointMirror.size() ? pointMirror[vertex] : kInvalidId;
        };
        std::vector<std::pair<u32, u32>> attempt;
        std::unordered_set<u32> taken;
        i32 direction = 0; // +1 the same way round, -1 reversed, 0 not yet known
        bool whole = true;
        std::vector<u32> unmatched;
        std::vector<HalfedgeId> mine;
        std::vector<HalfedgeId> theirs;
        for (const u32 face : otherFaces) {
            mine.clear();
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                mine.push_back(h);
            }
            const std::size_t n = mine.size();
            bool matched = false;
            for (auto [at, end] = primaryCornerAt.equal_range(twinOf(mine[0])); at != end && !matched; ++at) {
                const FaceId candidate = topology.face(at->second);
                if (taken.contains(candidate.value())) {
                    continue;
                }
                theirs.clear();
                for (const HalfedgeId h : topology.fh(candidate)) {
                    theirs.push_back(h);
                }
                if (theirs.size() != n) {
                    continue;
                }
                const std::size_t start = static_cast<std::size_t>(
                    std::find(theirs.begin(), theirs.end(), at->second) - theirs.begin());
                for (const i32 way : {1, -1}) {
                    if (matched || (direction != 0 && way != direction)) {
                        continue;
                    }
                    const auto partner = [&](std::size_t i) {
                        return theirs[way > 0 ? (start + i) % n : (start + n - i % n) % n];
                    };
                    bool round = true;
                    for (std::size_t i = 0; i < n && round; ++i) {
                        round = topology.from(partner(i)).value() == twinOf(mine[i]);
                    }
                    if (round) {
                        matched = true;
                        direction = way;
                        taken.insert(candidate.value());
                        for (std::size_t i = 0; i < n; ++i) {
                            attempt.emplace_back(mine[i].index(), partner(i).index());
                        }
                    }
                }
            }
            if (!matched) {
                unmatched.push_back(face);
            }
        }
        const std::span<const Vector2f> uvs =
            std::as_const(mesh).attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
        whole = direction != 0 && unmatched.size() * 2 <= otherFaces.size() && (unmatched.empty() || !uvs.empty());
        if (whole && !unmatched.empty()) {
            // The way round the copied faces lie in the map, which a face
            // copied corner by corner must share.
            const auto fanSigns = [&](std::span<const std::pair<u32, u32>> ring, i32& positive, i32& negative) {
                for (std::size_t k = 1; k + 1 < ring.size(); ++k) {
                    const Vector2f a = uvs[ring[0].second];
                    const Vector2f b = uvs[ring[k].second];
                    const Vector2f c = uvs[ring[k + 1].second];
                    const f32 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                    positive += area > 0.0f ? 1 : 0;
                    negative += area < 0.0f ? 1 : 0;
                }
            };
            i32 positive = 0;
            i32 negative = 0;
            for (std::size_t begin = 0; begin < attempt.size();) {
                const std::size_t n = topology.valence(topology.face(HalfedgeId(attempt[begin].first)));
                fanSigns(std::span<const std::pair<u32, u32>>(attempt).subspan(begin, n), positive, negative);
                begin += n;
            }
            const bool upward = positive >= negative;
            std::vector<u32> twins;
            std::vector<std::pair<u32, u32>> ring;
            for (const u32 face : unmatched) {
                twins.clear();
                ring.clear();
                for (const HalfedgeId h : topology.fh(FaceId(face))) {
                    twins.push_back(twinOf(h));
                }
                std::sort(twins.begin(), twins.end());
                for (const HalfedgeId h : topology.fh(FaceId(face))) {
                    // The primary corners at this twin in faces no whole pair
                    // took and that hold another of the face's twins.
                    u32 from = kInvalidId;
                    bool agree = true;
                    for (auto [at, end] = primaryCornerAt.equal_range(twinOf(h)); at != end && agree; ++at) {
                        const FaceId candidate = topology.face(at->second);
                        if (taken.contains(candidate.value())) {
                            continue;
                        }
                        u32 shared = 0;
                        for (const HalfedgeId g : topology.fh(candidate)) {
                            shared += std::binary_search(twins.begin(), twins.end(), topology.from(g).value()) ? 1 : 0;
                        }
                        if (shared < 2) {
                            continue;
                        }
                        if (from == kInvalidId) {
                            from = at->second.value();
                        } else {
                            agree = std::memcmp(&uvs[from], &uvs[at->second.index()], sizeof(Vector2f)) == 0;
                        }
                    }
                    if (from == kInvalidId || !agree) {
                        whole = false;
                        break;
                    }
                    ring.emplace_back(h.value(), from);
                }
                i32 up = 0;
                i32 down = 0;
                if (whole) {
                    fanSigns(ring, up, down);
                    whole = (upward ? down : up) == 0;
                }
                if (!whole) {
                    break;
                }
                attempt.insert(attempt.end(), ring.begin(), ring.end());
            }
        }
        if (whole) {
            pairing = std::move(attempt);
            out.byMirror = true;
        }
    }

    // --- the walk ------------------------------------------------------------
    if (pairing.empty()) {
        // Seeded at each island's longest boundary edge, walked in both
        // directions: one of the two is the way the two surfaces agree.
        const auto longest = [&](u32 island) {
            const std::span<const Vector3f> positions =
                mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
            HalfedgeId best;
            f32 bestLength = -1.0f;
            for (const HalfedgeId h : islands.boundaryOf(island)) {
                const f32 length = (positions[topology.from(topology.next(h)).index()] -
                                    positions[topology.from(h).index()])
                                       .length();
                if (length > bestLength) {
                    bestLength = length;
                    best = h;
                }
            }
            return best;
        };
        const HalfedgeId seedPrimary = longest(primary);
        const HalfedgeId seedOther = longest(other);
        if (!seedPrimary.valid() || !seedOther.valid()) {
            out.refusedFaces = out.otherFaces;
            return out;
        }
        for (u32 direction = 0; direction < 2 && pairing.empty(); ++direction) {
            std::unordered_map<u32, u32> cornerPair; // other corner -> primary corner
            std::vector<std::pair<HalfedgeId, HalfedgeId>> queue;
            const HalfedgeId start = direction == 0 ? seedPrimary : topology.next(seedPrimary);
            queue.emplace_back(seedOther, start);
            std::unordered_map<u32, u32> facePair;
            bool ok = true;
            while (!queue.empty() && ok) {
                const auto [a, b] = queue.back();
                queue.pop_back();
                const FaceId faceA = topology.face(a);
                const FaceId faceB = topology.face(b);
                if (!faceA.valid() || !faceB.valid()) {
                    ok = false;
                    break;
                }
                const auto seen = facePair.find(faceA.value());
                if (seen != facePair.end()) {
                    if (seen->second != faceB.value()) {
                        ok = false;
                    }
                    continue;
                }
                if (topology.valence(faceA) != topology.valence(faceB)) {
                    ok = false;
                    break;
                }
                facePair.emplace(faceA.value(), faceB.value());
                HalfedgeId walkA = a;
                HalfedgeId walkB = b;
                do {
                    cornerPair[walkA.index()] = walkB.index();
                    const HalfedgeId acrossA = Topology::opposite(walkA);
                    const HalfedgeId acrossB = Topology::opposite(walkB);
                    const FaceId nextA = topology.face(acrossA);
                    const FaceId nextB = topology.face(acrossB);
                    const bool insideA =
                        nextA.valid() && islands.islandOf(nextA.value()) == other;
                    const bool insideB =
                        nextB.valid() && islands.islandOf(nextB.value()) == primary;
                    if (insideA != insideB) {
                        ok = false;
                        break;
                    }
                    if (insideA) {
                        queue.emplace_back(topology.next(acrossA), topology.next(acrossB));
                    }
                    walkA = topology.next(walkA);
                    walkB = topology.next(walkB);
                } while (walkA != a && ok);
            }
            if (ok && facePair.size() == otherFaces.size()) {
                pairing.reserve(cornerPair.size());
                for (const auto& entry : cornerPair) {
                    pairing.emplace_back(entry.first, entry.second);
                }
            }
        }
    }

    if (pairing.empty()) {
        out.refusedFaces = out.otherFaces;
        return out;
    }
    // Every corner of the other island must have found one: a stack that is
    // right for most of a shape is a stack that is wrong.
    u32 corners = 0;
    for (const u32 face : otherFaces) {
        corners += topology.valence(FaceId(face));
    }
    if (pairing.size() < corners) {
        out.refusedFaces = corners - static_cast<u32>(pairing.size());
        return out;
    }

    const std::span<Vector2f> uvs =
        mesh.attributes.getOrCreate<Vector2f>(names::uv(set), Domain::Halfedge, AttrType::F32x2);
    for (const auto& pair : pairing) {
        if (pair.first < uvs.size() && pair.second < uvs.size()) {
            uvs[pair.first] = uvs[pair.second];
        }
    }
    return out;
}

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
