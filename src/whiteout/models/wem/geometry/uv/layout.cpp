// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/uv/layout.h>

#include "common.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
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
/// dilated by the padding, and the column profile the skyline is tested
/// against.
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

/// Fills the texels whose centre is inside the triangle, and the ring round
/// them: a conservative cover, so no island is packed closer than it looks.
void rasterTriangle(Footprint& out, const Vector2f& a, const Vector2f& b, const Vector2f& c) {
    const f32 lowX = std::min(a.x, std::min(b.x, c.x));
    const f32 highX = std::max(a.x, std::max(b.x, c.x));
    const f32 lowY = std::min(a.y, std::min(b.y, c.y));
    const f32 highY = std::max(a.y, std::max(b.y, c.y));
    const i32 x0 = std::max(0, static_cast<i32>(std::floor(lowX)) - 1);
    const i32 x1 = std::min(static_cast<i32>(out.width) - 1, static_cast<i32>(std::ceil(highX)));
    const i32 y0 = std::max(0, static_cast<i32>(std::floor(lowY)) - 1);
    const i32 y1 = std::min(static_cast<i32>(out.height) - 1, static_cast<i32>(std::ceil(highY)));
    const f32 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.0f) {
        return;
    }
    const f32 inverse = 1.0f / area;
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const f32 px = static_cast<f32>(x) + 0.5f;
            const f32 py = static_cast<f32>(y) + 0.5f;
            const f32 w0 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inverse;
            const f32 w1 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * inverse;
            const f32 w2 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * inverse;
            // A texel the triangle only clips still counts: the ring is what
            // keeps a seam from landing half in and half out.
            const bool inside = (w0 >= -0.5f && w1 >= -0.5f && w2 >= -0.5f);
            if (inside) {
                out.mask[static_cast<u32>(y) * out.width + static_cast<u32>(x)] = 1;
            }
        }
    }
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

/// The footprint of a group of islands taken together (a stack is one thing to
/// the packer), at @p texels per UV unit, turned by @p radians about the
/// group's own centre.
Footprint footprintOf(const Mesh& mesh, const UvIslands& islands, std::span<const u32> group,
                      u32 set, f32 texels, u32 padding, f32 radians, Vector2f& centre) {
    Footprint out;
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
    centre = Vector2f{(whole.low.x + whole.high.x) * 0.5f, (whole.low.y + whole.high.y) * 0.5f};
    const f32 sine = std::sin(radians);
    const f32 cosine = std::cos(radians);
    const auto turn = [&](const Vector2f& value) {
        const f32 dx = value.x - centre.x;
        const f32 dy = value.y - centre.y;
        return Vector2f{centre.x + dx * cosine - dy * sine, centre.y + dx * sine + dy * cosine};
    };

    // The turned bounds, which is what the mask has to hold.
    UvBounds turned;
    for (const u32 island : group) {
        for (const u32 face : islands.facesOf(island)) {
            for (const detail::Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
                for (u32 k = 0; k < 3; ++k) {
                    const Vector2f value = turn(uvs[tri.corner[k].index()]);
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
            }
        }
    }
    if (turned.empty) {
        return out;
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
    out.mask.assign(static_cast<std::size_t>(out.width) * out.height, 0);
    const auto place = [&](const Vector2f& value) {
        const Vector2f t = turn(value);
        return Vector2f{(t.x - turned.low.x) * texels + static_cast<f32>(margin),
                        (t.y - turned.low.y) * texels + static_cast<f32>(margin)};
    };
    for (const u32 island : group) {
        for (const u32 face : islands.facesOf(island)) {
            for (const detail::Tri& tri : detail::TrianglesOf(mesh, FaceId(face))) {
                rasterTriangle(out, place(uvs[tri.corner[0].index()]),
                               place(uvs[tri.corner[1].index()]),
                               place(uvs[tri.corner[2].index()]));
            }
        }
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
    const auto samepoints = [](const std::vector<Vector2f>& a, const std::vector<Vector2f>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::abs(a[i].x - b[i].x) > kSame || std::abs(a[i].y - b[i].y) > kSame) {
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
    PackResult out;
    if (!mesh.hasConnectivity() || islands.count == 0 || options.resolution == 0) {
        return out;
    }
    const u32 resolution = options.resolution;

    // --- the groups: a stack is one thing to the packer ----------------------
    std::vector<u32> groupOf(islands.count, kInvalidId);
    std::vector<std::vector<u32>> groups;
    const std::vector<std::vector<u32>> stacks =
        options.keepStacks ? FindStacks(mesh, islands, set) : std::vector<std::vector<u32>>{};
    for (const std::vector<u32>& stack : stacks) {
        const u32 index = static_cast<u32>(groups.size());
        for (const u32 island : stack) {
            groupOf[island] = index;
        }
        groups.push_back(stack);
    }
    for (u32 island = 0; island < islands.count; ++island) {
        if (groupOf[island] == kInvalidId) {
            groupOf[island] = static_cast<u32>(groups.size());
            groups.push_back({island});
        }
    }

    // --- free and locked -----------------------------------------------------
    std::vector<u8> groupFree(groups.size(), 1);
    for (u32 g = 0; g < groups.size(); ++g) {
        for (const u32 island : groups[g]) {
            // A stack with one locked member is locked whole: moving half of a
            // stack is what breaks it.
            if (!IslandIsFree(mesh, islands, island, set)) {
                groupFree[g] = 0;
                break;
            }
        }
    }

    // --- the obstacles, and the tiling refusal -------------------------------
    std::vector<u8> occupied(static_cast<std::size_t>(resolution) * resolution, 0);
    bool anyLocked = false;
    for (u32 g = 0; g < groups.size(); ++g) {
        if (groupFree[g] != 0) {
            continue;
        }
        anyLocked = true;
        Vector2f centre{0.0f, 0.0f};
        const UvBounds bounds = BoundsOf(mesh, islands, groups[g].front(), set);
        if (!bounds.empty && (bounds.low.x < -0.001f || bounds.low.y < -0.001f ||
                              bounds.high.x > 1.001f || bounds.high.y > 1.001f)) {
            // Deliberately outside the tile: the layout tiles, and packing it
            // in would undo what someone meant.
            out.refusedTiling = true;
        }
        const Footprint mark = footprintOf(mesh, islands, groups[g], set,
                                           static_cast<f32>(resolution), 0, 0.0f, centre);
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

    // --- the order: largest first, by area then by first face ----------------
    std::vector<u32> order;
    for (u32 g = 0; g < groups.size(); ++g) {
        if (groupFree[g] != 0) {
            order.push_back(g);
        }
    }
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    const std::span<const Vector2f> uvs =
        mesh.attributes.get<const Vector2f>(names::uv(set), Domain::Halfedge);
    std::vector<f32> areaOf(groups.size(), 0.0f);
    std::vector<u32> firstFaceOf(groups.size(), kInvalidId);
    for (u32 g = 0; g < groups.size(); ++g) {
        for (const u32 island : groups[g]) {
            areaOf[g] += detail::AreasOf(mesh, islands, island, positions, uvs).uv;
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
        return firstFaceOf[a] < firstFaceOf[b];
    });
    if (order.empty()) {
        return out;
    }

    // --- the skyline: the first free row of every column ---------------------
    const auto skylineOf = [&]() {
        std::vector<u32> first(resolution, 0);
        for (u32 x = 0; x < resolution; ++x) {
            for (u32 y = resolution; y-- > 0;) {
                if (occupied[static_cast<std::size_t>(y) * resolution + x] != 0) {
                    first[x] = y + 1;
                    break;
                }
            }
        }
        return first;
    };

    // Angles to try: the island lying along its own long axis first, then the
    // four quarter turns of that. A turn is free and a bad fit is not.
    const auto anglesFor = [&](std::span<const u32> group) {
        std::vector<f32> angles;
        if (!options.rotate) {
            angles.push_back(0.0f);
            return angles;
        }
        constexpr f32 kHalfPi = 1.57079632679f;
        const f32 principal = principalAngle(mesh, islands, group, set);
        angles.push_back(principal);
        for (u32 k = 1; k < 4; ++k) {
            angles.push_back(principal + kHalfPi * static_cast<f32>(k));
        }
        angles.push_back(0.0f);
        return angles;
    };

    // One attempt at the whole layout, at `scale` over the free islands.
    struct Placement {
        u32 group = 0;
        f32 angle = 0.0f;
        Vector2f centre{0.0f, 0.0f};
        Vector2f delta{0.0f, 0.0f};
    };
    const auto attempt = [&](f32 scale, std::vector<Placement>& placements) {
        placements.clear();
        std::vector<u32> skyline = skylineOf();
        for (const u32 g : order) {
            bool placed = false;
            f32 bestAngle = 0.0f;
            u32 bestX = 0;
            u32 bestY = 0;
            u32 bestTop = kInvalidId;
            Vector2f bestOrigin{0.0f, 0.0f};
            for (const f32 angle : anglesFor(groups[g])) {
                Vector2f centre{0.0f, 0.0f};
                const Footprint print =
                    footprintOf(mesh, islands, groups[g], set,
                                static_cast<f32>(resolution) * scale, options.padding, angle,
                                centre);
                if (print.empty() || print.width > resolution || print.height > resolution) {
                    continue;
                }
                for (u32 x = 0; x + print.width <= resolution; ++x) {
                    // The lowest the island can sit at this column, given what
                    // is already below it.
                    u32 y = 0;
                    for (u32 c = 0; c < print.width; ++c) {
                        if (print.low[c] == kInvalidId) {
                            continue;
                        }
                        const u32 need = skyline[x + c] > print.low[c]
                                             ? skyline[x + c] - print.low[c]
                                             : 0;
                        y = std::max(y, need);
                    }
                    if (y + print.height > resolution) {
                        continue;
                    }
                    u32 top = 0;
                    for (u32 c = 0; c < print.width; ++c) {
                        if (print.high[c] != kInvalidId) {
                            top = std::max(top, y + print.high[c]);
                        }
                    }
                    if (top < bestTop) {
                        bestTop = top;
                        bestAngle = angle;
                        bestX = x;
                        bestY = y;
                        bestOrigin = print.origin;
                        placed = true;
                    }
                }
            }
            if (!placed) {
                return false;
            }
            // Where the mask landed, turned back into a move in UV.
            Vector2f centre{0.0f, 0.0f};
            const Footprint chosen =
                footprintOf(mesh, islands, groups[g], set,
                            static_cast<f32>(resolution) * scale, options.padding, bestAngle,
                            centre);
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
            for (u32 c = 0; c < chosen.width; ++c) {
                if (chosen.high[c] != kInvalidId) {
                    skyline[bestX + c] = bestY + chosen.high[c] + 1;
                }
            }
        }
        return true;
    };

    // --- fit, scaling the free islands down together if they will not --------
    std::vector<Placement> placements;
    f32 scale = 1.0f;
    if (!attempt(scale, placements)) {
        f32 low = 0.0f;
        f32 high = 1.0f;
        for (u32 step = 0; step < 8; ++step) {
            const f32 middle = (low + high) * 0.5f;
            if (attempt(middle, placements)) {
                low = middle;
            } else {
                high = middle;
            }
        }
        if (low <= 0.0f || !attempt(low, placements)) {
            return out;
        }
        scale = low;
    }
    out.scaled = scale;

    // --- write it ------------------------------------------------------------
    for (const Placement& placement : placements) {
        transformGroup(mesh, islands, groups[placement.group], set, placement.centre, scale,
                       placement.angle, placement.delta);
        ++out.placed;
    }

    // --- what it covers ------------------------------------------------------
    f32 covered = 0.0f;
    for (u32 island = 0; island < islands.count; ++island) {
        covered += detail::AreasOf(mesh, islands, island, positions, uvs).uv;
    }
    out.coverage = std::clamp(covered, 0.0f, 1.0f);
    return out;
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

    // Where every corner of the primary is, by the vertex it sits at: what a
    // pairing has to look up.
    std::unordered_map<u32, HalfedgeId> primaryCornerAt;
    for (const u32 face : primaryFaces) {
        for (const HalfedgeId h : topology.fh(FaceId(face))) {
            primaryCornerAt.emplace(topology.from(h).value(), h);
        }
    }

    // --- the mirror ----------------------------------------------------------
    std::vector<std::pair<u32, u32>> pairing; // other corner -> primary corner
    if (!pointMirror.empty()) {
        u32 hits = 0;
        u32 total = 0;
        std::vector<std::pair<u32, u32>> attempt;
        for (const u32 face : otherFaces) {
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                ++total;
                const u32 vertex = topology.from(h).value();
                const u32 twin = vertex < pointMirror.size() ? pointMirror[vertex] : kInvalidId;
                if (twin == kInvalidId) {
                    continue;
                }
                const auto found = primaryCornerAt.find(twin);
                if (found == primaryCornerAt.end()) {
                    continue;
                }
                attempt.emplace_back(h.index(), found->second.index());
                ++hits;
            }
        }
        // Nine in ten is a mirrored pair; anything less is two shapes that
        // happen to be near each other.
        if (total > 0 && hits * 10 >= total * 9) {
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
