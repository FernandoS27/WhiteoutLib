// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wow/wmo/runtime.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

// Several rules here (the liquid mesh, the detail doodads) match the client's
// float arithmetic bit for bit, so no multiply-add may be fused.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace whiteout {
namespace models {
namespace wow {
namespace wmo {

namespace {

// s_wmoShaderMetaData (0x1444440C0): texture count, UV sets, colour sets.
constexpr std::array<ShaderInfo, kShaderCount> kShaders = {{
    {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {2, 1, 1}, {1, 1, 1}, {2, 1, 1}, {2, 2, 2},
    {3, 2, 2}, {2, 1, 2}, {2, 2, 2}, {1, 1, 1}, {3, 2, 2}, {3, 2, 2}, {2, 2, 2},
    {1, 1, 1}, {2, 2, 2}, {1, 1, 1}, {3, 2, 2}, {3, 3, 2}, {2, 2, 2}, {3, 3, 2},
    {3, 1, 1}, {6, 3, 2}, {9, 5, 2}, {3, 1, 1}, {1, 1, 1},
}};

// cvtss2si under the default rounding mode: nearest, ties to even.
i32 RoundToInt(f32 v) {
    return static_cast<i32>(std::nearbyint(v));
}

u8 ToByte(i32 v) {
    return static_cast<u8>(v);
}

// The two in-plane axes for a plane whose normal's largest component is the
// index (0x1443AE580): x → (y, z), y → (z, x), z → (x, y).
constexpr std::array<std::array<u32, 2>, 3> kPlaneAxes = {{{1, 2}, {2, 0}, {0, 1}}};

f32 Component(const Vector3f& v, u32 axis) {
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

// C3Vector::MajorAxis (0x14367A040).
u32 MajorAxis(const Vector3f& n) {
    const f32 x = std::fabs(n.x), y = std::fabs(n.y), z = std::fabs(n.z);
    if (x <= y)
        return (y <= z) ? 2u : 1u;
    return (x <= z) ? 2u : 0u;
}

// Point-in-polygon on the plane of the major axis (0x143687500): a crossing
// test over edges (i-1, i) starting from the last vertex.
bool InsidePolygon(const Vector3f& p, std::span<const Vector3f> poly, u32 axis) {
    if (poly.empty())
        return false;
    const u32 u = kPlaneAxes[axis][0];
    const u32 w = kPlaneAxes[axis][1];
    const f32 py = Component(p, w);
    std::size_t prev = poly.size() - 1;
    bool prevAbove = py <= Component(poly[prev], w);
    bool inside = false;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const f32 cur = Component(poly[i], w);
        const bool curAbove = cur >= py;
        if (prevAbove != curAbove) {
            const f32 lhs = (Component(poly[prev], u) - Component(poly[i], u)) * (cur - py);
            const f32 rhs =
                (Component(poly[i], u) - Component(p, u)) * (Component(poly[prev], w) - cur);
            if ((lhs >= rhs) == curAbove)
                inside = !inside;
        }
        prev = i;
        prevAbove = curAbove;
    }
    return inside;
}

// NTempest::DistanceFromPolygonEdge (0x14368B0A0): the nearest distance from
// @p p to any edge segment.
f32 DistanceFromPolygonEdge(const Vector3f& p, std::span<const Vector3f> poly) {
    f32 best = std::numeric_limits<f32>::max();
    if (poly.empty())
        return best;
    std::size_t prev = poly.size() - 1;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Vector3f& a = poly[prev];
        const Vector3f& b = poly[i];
        f32 dx = p.x - a.x, dy = p.y - a.y, dz = p.z - a.z;
        const f32 ex = b.x - a.x, ey = b.y - a.y, ez = b.z - a.z;
        const f32 t = ex * dx + ey * dy + ez * dz;
        if (t > 0.0f) {
            const f32 len2 = ex * ex + ey * ey + ez * ez;
            if (t <= len2) {
                const f32 s = t / len2;
                dx -= s * ex;
                dy -= s * ey;
                dz -= s * ez;
            } else {
                dx -= ex;
                dy -= ey;
                dz -= ez;
            }
        }
        best = std::fmin(std::sqrt(dx * dx + dy * dy + dz * dz), best);
        prev = i;
    }
    return best;
}

// The plane's distance in the client's evaluation order.
f32 PlaneDistance(const Plane& plane, const Vector3f& v) {
    return (plane.normal.y * v.y + plane.normal.x * v.x) + (plane.normal.z * v.z + plane.distance);
}

// NTempest::Intersect(ray, plane) (0x143686F40) with an unnormalised
// direction: where the ray from @p origin along @p dir meets the plane, or
// @p origin itself when they are parallel and it lies on the plane.
Vector3f IntersectRayPlane(const Vector3f& origin, const Vector3f& dir, const Plane& plane) {
    constexpr f32 kEpsilon = 0.01f;
    const f32 denom = plane.normal.x * dir.x + plane.normal.y * dir.y + plane.normal.z * dir.z;
    const f32 dist = (plane.normal.x * origin.x + plane.normal.y * origin.y) +
                     plane.normal.z * origin.z + plane.distance;
    if (std::fabs(denom) < 0.0001f)
        return origin;
    const f32 t = std::fabs(dist) >= kEpsilon ? -(dist / denom) : 0.0f;
    return {t * dir.x + origin.x, t * dir.y + origin.y, t * dir.z + origin.z};
}

} // namespace

ShaderInfo shaderInfo(u32 shader) {
    return shader < kShaderCount ? kShaders[shader] : kShaders[0];
}

bool hasTexture(const Root& root, const Material& material, u32 slot) {
    const u32 value = material.texture(slot);
    if (!root.textureNames)
        return value != 0;
    return value < root.textureNames->size() && (*root.textureNames)[value] != '\0';
}

u32 effectiveShader(const Root& root, const Material& material) {
    const u32 needed = shaderInfo(material.shader).textureCount;
    for (u32 slot = 0; slot < needed; ++slot) {
        if (!hasTexture(root, material, slot))
            return (material.shader == 21 || material.shader == 23) ? material.shader : 4u;
    }
    return material.shader;
}

bool groupDraws(const Root& root, const Group& group) {
    if ((group.header.flags & 0x0C000000u) != 0 || hasFlag(group.header.flags2, GroupFlag2::AttachmentMesh))
        return false;
    return Root::nameAt(root.groupNames, group.header.nameOffset) != "antiportal";
}

bool batchDraws(const Root& root, const Batch& batch) {
    const u32 material = batch.material();
    return material < root.materials.size() && !isSpecialShader(root.materials[material].shader);
}

u32 maxLodLevel(const Root& root) {
    if (!hasFlag(root.header.flags, RootFlag::Lod))
        return 0;
    const u32 count = std::min<u32>(root.header.lodCount, 5);
    u32 maxLod = count ? count - 1 : 2;
    if (maxLod != 0 && hasFlag(root.header.flags, RootFlag::ExtraLodLevel))
        --maxLod;
    return maxLod;
}

bool groupHasLod(const Root& root, u32 group, u32 lod) {
    if (lod == 0)
        return true;
    if (group >= root.groups.size() || !hasFlag(root.groups[group].flags, GroupFlag::Lod))
        return false;
    if (group < root.groups2.size())
        return root.groups2[group].lodIndex >= lod;
    return lod <= maxLodLevel(root);
}

LodContext lodContext(const Root& root) {
    LodContext c;
    const u32 flags = root.header.flags;
    const bool lod = (flags & 0x10u) != 0;
    c.maxLod = maxLodLevel(root);
    // 0x200 survives the load-time strip only where there was no level to take.
    const bool kept200 = lod && (flags & 0x200u) && c.maxLod == 0;
    if (lod && (flags & 0x20u))
        c.special20 = static_cast<u8>(kept200 ? c.maxLod - 1 : c.maxLod);
    if (kept200)
        c.special200 = c.maxLod;
    if (flags & 0x220u) {
        for (u32 g = 0; g < root.groups.size(); ++g) {
            if (root.groups[g].flags & 0x8u) {
                c.firstExterior = g;
                break;
            }
        }
    }
    c.lodIndex.assign(root.groups.size(), 0);
    for (u32 g = 0; g < root.groups.size(); ++g) {
        const u32 gf = root.groups[g].flags;
        const u32 flags2 = g < root.groups2.size() ? root.groups2[g].flags2 : 0u;
        if ((gf & 0x4000000u) || (flags2 & 0x100u))
            continue;
        if (!lod || !(gf & 0x400u)) {
            c.lodIndex[g] = 1;
            continue;
        }
        if (g < root.groups2.size()) {
            c.lodIndex[g] = root.groups2[g].lodIndex;
            continue;
        }
        u32 last = 0;
        for (u32 l = 1; l <= c.maxLod && l < c.special20; ++l)
            last = l;
        if (g == c.firstExterior && c.special20 != 0xFFu)
            last = c.special20;
        c.lodIndex[g] = last;
    }
    bool interior2000 = false;
    for (const GroupInfo& gi : root.groups)
        interior2000 = interior2000 || (gi.flags & 0x2000u);
    const i64 extraMaterials = static_cast<i64>(root.materials.size()) -
                               static_cast<i64>(root.groups.size()) * static_cast<i64>(c.maxLod);
    c.smallPath = root.groups.size() <= 1 && root.doodadDefs.size() <= 10 && !interior2000 &&
                  c.firstExterior == 0xFFFF && std::max<i64>(0, extraMaterials) <= 3 &&
                  c.maxLod != 0;
    return c;
}

std::array<f32, 5> lodThresholds(const Root& root, const LodContext& context, f32 horizonDistance,
                                 u8 placementLodScale, f32 wmoLodDist, f32 wmoLodDistScale) {
    constexpr f32 kStep = 66.666664f;
    f32 s = static_cast<f32>(placementLodScale) * 0.0078125f;
    if (wmoLodDist * s < 200.0f)
        s = 200.0f / wmoLodDist;
    const f32 B = (wmoLodDistScale * wmoLodDist) * 1.0f;
    std::array<f32, 5> t{0.0f, B * s, (s * 1.75f) * B, (s * 3.0f) * B, (s * 4.0f) * B};
    const u32 flags = root.header.flags;
    if ((flags & 0x20u) && !(flags & 0x100u) && context.special20 < t.size())
        t[context.special20] = horizonDistance;
    if (context.special200 < t.size())
        t[context.special200] = horizonDistance;
    for (std::size_t i = 1; i < t.size(); ++i)
        t[i] = std::max(t[i - 1] + kStep, t[i]);
    return t;
}

u32 distanceLod(const Box& box, const Vector3f& camera, const std::array<f32, 5>& thresholds,
                u32 cap, u32 maxLodClamp) {
    const auto axis = [](f32 p, f32 lo, f32 hi) {
        const f32 c = std::min(std::max(p, lo), hi);
        return p - c;
    };
    const f32 dx = axis(camera.x, box.minimum.x, box.maximum.x);
    const f32 dy = axis(camera.y, box.minimum.y, box.maximum.y);
    const f32 dz = axis(camera.z, box.minimum.z, box.maximum.z);
    const f32 d2 = dx * dx + dy * dy + dz * dz;
    const f32 d = d2 > 1.0f ? std::sqrt(d2) : d2;
    u32 l = std::min<u32>(cap, static_cast<u32>(thresholds.size()) - 1);
    while (l != 0 && d < thresholds[l])
        --l;
    return std::min(l, maxLodClamp);
}

LodTargets lodTargets(const Model& model, const LodContext& context, const Vector3f& camera,
                      bool cameraInside, f32 horizonDistance, u8 placementLodScale,
                      u32 maxLodClamp) {
    const Root& root = model.root;
    LodTargets out;
    out.target.assign(root.groups.size(), 0);
    if (context.maxLod == 0)
        return out;
    if (context.smallPath) {
        // Picked per draw from the root's box, with its own step-raised table
        // and no horizon level (`CMapObjDefGroup_UpdateLod` 0x141B3E460).
        const bool lodOn =
            root.header.lodCount != 1 && !root.groups2.empty() && root.groups2[0].lodIndex != 0;
        if (!lodOn)
            return out;
        f32 s = static_cast<f32>(placementLodScale) * 0.0078125f;
        constexpr f32 dist = 300.0f;
        if (dist * s < 200.0f)
            s = 200.0f / dist;
        const f32 B = dist;
        std::array<f32, 5> t{};
        t[1] = std::max(B * s, 66.666664f);
        t[2] = std::max((s * 1.75f) * B, t[1] + 66.666664f);
        t[3] = std::max((s * 3.0f) * B, t[2] + 66.666664f);
        t[4] = std::max((s * 4.0f) * B, t[3] + 66.666664f);
        const u32 l =
            std::min(distanceLod(root.header.bounds, camera, t, context.maxLod), maxLodClamp);
        std::fill(out.target.begin(), out.target.end(), std::min(l, context.maxLod));
        out.sdfd = static_cast<u16>(static_cast<i32>(t[1]));
        return out;
    }
    const std::array<f32, 5> t = lodThresholds(root, context, horizonDistance, placementLodScale);
    out.sdfd = static_cast<u16>(static_cast<i32>(t[1]));
    u32 cap = std::min(context.maxLod, std::min(context.special20, context.special200) - 1u);
    if (cameraInside)
        cap = 0;
    else
        out.wmoLod = distanceLod(root.header.bounds, camera, t, context.maxLod, maxLodClamp);
    const bool perGroup = !(out.wmoLod > cap);
    for (u32 g = 0; g < root.groups.size(); ++g) {
        const u32 gf = root.groups[g].flags;
        const u32 flags2 = g < root.groups2.size() ? root.groups2[g].flags2 : 0u;
        if ((gf & 0x4000000u) || (flags2 & 0x100u))
            continue;
        const u32 li = context.lodIndex[g];
        out.target[g] =
            perGroup ? std::min(distanceLod(root.groups[g].bounds, camera, t, cap, maxLodClamp), li)
                     : std::min(out.wmoLod, li + 1);
    }
    return out;
}

bool lodHasObject(const Root& root, const LodContext& context, u32 group, u32 lod) {
    if (lod == 0)
        return true;
    if (group >= root.groups.size())
        return false;
    if (group == context.firstExterior && lod == context.special20)
        return true;
    return (root.groups[group].flags & 0x400u) && lod <= context.lodIndex[group];
}

Vector2f emissiveFade(u16 sdfd) {
    if (sdfd == 0)
        return {0.0f, 1.0f};
    const f32 d = static_cast<f32>(sdfd);
    const f32 s = 1.0f / (0.99f * d - 0.85f * d);
    return {-s, s * (0.99f * d)};
}

u32 transitionVertexCount(const Group& group) {
    const u32 trans = group.header.transBatchCount;
    if (trans == 0)
        return 0;
    if (hasFlag(group.header.flags, GroupFlag::Lod))
        return static_cast<u32>(group.positions.size());
    u32 highest = 0;
    for (u32 i = 0; i < trans && i < group.batches.size(); ++i)
        highest = std::max(highest, group.batches[i].lastVertex());
    return highest + 1;
}

void attenuateTransitionVertices(const Root& root, const Group& group, std::vector<Color>& colors) {
    if (hasFlag(root.header.flags, RootFlag::NoTransitionAttenuation) ||
        group.header.transBatchCount == 0)
        return;
    const u32 count =
        std::min<u32>(transitionVertexCount(group),
                      static_cast<u32>(std::min(colors.size(), group.positions.size())));
    for (u32 v = 0; v < count; ++v) {
        const Vector3f& vertex = group.positions[v];
        f32 accumulated = 0.0f;
        bool blocked = false;
        for (u32 r = 0; r < group.header.portalCount; ++r) {
            const u32 refIndex = group.header.portalStart + r;
            if (refIndex >= root.portalRefs.size())
                break;
            const PortalRef& ref = root.portalRefs[refIndex];
            if (ref.portalIndex >= root.portals.size())
                continue;
            const Portal& portal = root.portals[ref.portalIndex];
            const std::size_t first = portal.startVertex;
            const std::size_t last =
                std::min<std::size_t>(first + portal.vertexCount, root.portalVertices.size());
            const std::span<const Vector3f> poly =
                first < last
                    ? std::span<const Vector3f>(root.portalVertices).subspan(first, last - first)
                    : std::span<const Vector3f>();

            // Project the vertex onto the portal's plane along the normal.
            Vector3f onPlane = vertex;
            const f32 d = PlaneDistance(portal.plane, vertex);
            if (static_cast<f64>(d) > 0.001)
                onPlane = IntersectRayPlane(
                    vertex,
                    {-portal.plane.normal.x, -portal.plane.normal.y, -portal.plane.normal.z},
                    portal.plane);
            else if (static_cast<f64>(d) < -0.001)
                onPlane = IntersectRayPlane(vertex, portal.plane.normal, portal.plane);

            f32 distance;
            if (InsidePolygon(onPlane, poly, MajorAxis(portal.plane.normal))) {
                distance = PlaneDistance(portal.plane, vertex);
                if (ref.side != 1)
                    distance = -distance;
            } else {
                distance = DistanceFromPolygonEdge(vertex, poly);
            }

            const u32 otherFlags =
                ref.groupIndex < root.groups.size() ? root.groups[ref.groupIndex].flags : 0;
            if ((otherFlags & 0x48) != 0) {
                const f32 fade = 1.0f - std::fmax(distance, 0.0f) * 0.15000001f;
                if (static_cast<f64>(fade) > 0.001)
                    accumulated += fade;
            } else if (distance > -1.0f && distance < 1.0f) {
                blocked = true;
                break;
            }
        }
        const f32 opacity =
            blocked || static_cast<f64>(accumulated) <= 0.001 ? 0.0f : std::fmin(accumulated, 1.0f);
        Color& c = colors[v];
        c.r = ToByte(RoundToInt(static_cast<f32>(c.r) * (1.0f - opacity)));
        c.g = ToByte(RoundToInt(static_cast<f32>(c.g) * (1.0f - opacity)));
        c.b = ToByte(RoundToInt(static_cast<f32>(c.b) * (1.0f - opacity)));
        c.a = ToByte(RoundToInt(opacity * 255.0f));
    }
}

void fixVertexColorAlpha(const Root& root, const Group& group, std::vector<Color>& colors) {
    const u32 count = static_cast<u32>(colors.size());
    const u32 trans = std::min(transitionVertexCount(group), count);
    const u8 exteriorAlpha = hasFlag(group.header.flags, GroupFlag::Exterior) ? 0xFF : 0x00;
    if (hasFlag(root.header.flags, RootFlag::VertexColorAlphaOnly)) {
        for (u32 v = trans; v < count; ++v)
            colors[v].a = exteriorAlpha;
        return;
    }
    Color ambient;
    if (!hasFlag(root.header.flags, RootFlag::NoAmbientInVertexColor))
        ambient = root.header.ambientColor;

    for (u32 v = 0; v < trans; ++v) {
        Color& c = colors[v];
        const f32 keep = 1.0f - static_cast<f32>(c.a) * 0.0039215689f;
        const auto fix = [&](u8 channel, u8 amb) {
            const i32 lifted = std::max(static_cast<i32>(channel) - static_cast<i32>(amb), 0);
            return static_cast<u8>(ToByte(RoundToInt(static_cast<f32>(lifted) * keep)) >> 1);
        };
        c.r = fix(c.r, ambient.r);
        c.g = fix(c.g, ambient.g);
        c.b = fix(c.b, ambient.b);
    }
    for (u32 v = trans; v < count; ++v) {
        Color& c = colors[v];
        const u32 a = c.a;
        const auto fix = [&](u8 channel, u8 amb) {
            const i32 lit = static_cast<i32>(channel) + static_cast<i32>((channel * a) >> 6) -
                            static_cast<i32>(amb);
            return static_cast<u8>(std::min(std::max(lit, 0) >> 1, 255));
        };
        c.r = fix(c.r, ambient.r);
        c.g = fix(c.g, ambient.g);
        c.b = fix(c.b, ambient.b);
        c.a = exteriorAlpha;
    }
}

std::vector<Color> loadedColorSet0(const Root& root, const Group& group) {
    const auto* set0 = group.colorSet0();
    if (!set0)
        return {};
    std::vector<Color> colors = *set0;
    if (group.header.transBatchCount != 0)
        attenuateTransitionVertices(root, group, colors);
    fixVertexColorAlpha(root, group, colors);
    return colors;
}

VertexColors vertexColors(const Root& root, const Group& group) {
    const std::size_t n = group.positions.size();
    const auto rgba = [](const Color& c) {
        return static_cast<u32>(c.r) | (static_cast<u32>(c.g) << 8) |
               (static_cast<u32>(c.b) << 16) | (static_cast<u32>(c.a) << 24);
    };
    VertexColors out;
    out.color0.assign(n, 0xFF000000u);
    out.color1.assign(n, 0xFF000000u);
    out.color2.assign(n, 0u);

    if (group.colorSet0()) {
        const std::vector<Color> colors = loadedColorSet0(root, group);
        const bool forceOpaque = (group.header.flags & 0x2040) == 0x2040;
        for (std::size_t v = 0; v < n && v < colors.size(); ++v) {
            out.color0[v] = rgba(colors[v]);
            if (forceOpaque)
                out.color0[v] |= 0xFF000000u;
        }
    }
    if (const auto* set1 = group.colorSet1()) {
        for (std::size_t v = 0; v < n && v < set1->size(); ++v)
            out.color1[v] = rgba((*set1)[v]);
    }
    if (group.vertexColors2) {
        for (std::size_t v = 0; v < n && v < group.vertexColors2->size(); ++v)
            out.color2[v] = rgba((*group.vertexColors2)[v]);
    }
    return out;
}

std::array<Color, 3> ambientColors(const Root& root, std::span<const u16> activeDoodadSets) {
    const auto fromVolume = [](const AmbientVolume& v) -> std::array<Color, 3> {
        if (v.flags & 1)
            return {v.color1, v.color2, v.color3};
        return {v.color1, v.color1, v.color1};
    };
    if (!root.globalAmbients.empty()) {
        for (const AmbientVolume& v : root.globalAmbients) {
            if (v.doodadSetId != 0 && std::find(activeDoodadSets.begin(), activeDoodadSets.end(),
                                                v.doodadSetId) != activeDoodadSets.end())
                return fromVolume(v);
        }
        return fromVolume(root.globalAmbients.front());
    }
    if (!root.ambientVolumes.empty())
        return fromVolume(root.ambientVolumes.front());
    const Color c = root.header.ambientColor;
    return {c, c, c};
}

namespace {

// sub_141AAEF50: @p from toward @p to by @p t/255 per channel, alpha kept.
// The difference is taken through a u16, so a negative one wraps back.
Color LerpBytes(Color from, u8 t, Color to) {
    if (t == 0)
        return from;
    if (t == 255)
        return {to.b, to.g, to.r, from.a};
    const auto lerp = [t](u8 a, u8 b) {
        return static_cast<u8>(a + (static_cast<u16>(t * (static_cast<i32>(b) - a)) >> 8));
    };
    return {lerp(from.b, to.b), lerp(from.g, to.g), lerp(from.r, to.r), from.a};
}

// A sum of weights and colours, one channel at a time, as the client's registers hold them.
struct AmbientSum {
    f32 c[3][3] = {}; // [colour][r, g, b]
    f32 weight = 0.0f;
};

u8 UnitToByte(f32 v) {
    const f32 c = v >= 0.0f ? std::min(v, 1.0f) : 0.0f;
    return static_cast<u8>(static_cast<i32>(c * 255.0f));
}

bool SetActive(u16 set, std::span<const u16> activeSets) {
    return set == 0 || std::find(activeSets.begin(), activeSets.end(), set) != activeSets.end();
}

} // namespace

std::array<Color, 3> cameraAmbientColors(const Model& model, u32 cameraGroup,
                                         const Vector3f& camera, std::span<const u16> activeSets,
                                         f32 portalDistance) {
    const Root& root = model.root;
    const std::array<Color, 3> base = ambientColors(root, activeSets);
    if (root.globalAmbients.empty() && root.ambientVolumes.empty())
        return base;

    std::array<Color, 3> vol = base;
    if (cameraGroup < model.groups.size() && model.groups[cameraGroup]) {
        const Group& group = *model.groups[cameraGroup];
        constexpr f32 k = 0.0039215689f;
        AmbientSum s;
        for (u16 r : group.ambientVolumeRefs) {
            if (r >= root.ambientVolumes.size())
                continue;
            const AmbientVolume& e = root.ambientVolumes[r];
            if (!SetActive(e.doodadSetId, activeSets))
                continue;
            const f32 dz = e.position.z - camera.z;
            const f32 dy = e.position.y - camera.y;
            const f32 dx = e.position.x - camera.x;
            const f32 d = std::sqrt((dy * dy + dx * dx) + dz * dz);
            if (!(d < e.end))
                continue;
            const f32 dc = d >= 0.0f ? std::min(d, e.end) : 0.0f;
            const f32 w = dc >= e.start ? 1.0f - (dc - e.start) / (e.end - e.start) : 1.0f;
            // Colour 1's red is (byte·w)·k; its green and blue w·(byte·k).
            s.c[0][0] += (static_cast<f32>(e.color1.r) * w) * k;
            s.c[0][1] += w * (static_cast<f32>(e.color1.g) * k);
            s.c[0][2] += w * (static_cast<f32>(e.color1.b) * k);
            if (e.flags & 1) {
                const Color* cs[2] = {&e.color2, &e.color3};
                for (u32 i = 0; i < 2; ++i) {
                    s.c[i + 1][0] += (static_cast<f32>(cs[i]->r) * w) * k;
                    s.c[i + 1][1] += (static_cast<f32>(cs[i]->g) * w) * k;
                    s.c[i + 1][2] += (static_cast<f32>(cs[i]->b) * w) * k;
                }
            } else {
                for (u32 i = 1; i < 3; ++i) {
                    s.c[i][0] += (static_cast<f32>(e.color1.r) * w) * k;
                    s.c[i][1] += (static_cast<f32>(e.color1.g) * k) * w;
                    s.c[i][2] += (static_cast<f32>(e.color1.b) * k) * w;
                }
            }
            s.weight += w;
        }
        for (u16 r : group.ambientBoxRefs) {
            if (r >= root.ambientBoxes.size())
                continue;
            const AmbientBox& e = root.ambientBoxes[r];
            if (!SetActive(e.doodadSetId, activeSets))
                continue;
            f32 m = std::numeric_limits<f32>::max();
            bool inside = true;
            for (const Plane& p : e.planes) {
                const f32 d = (camera.x * p.normal.x + camera.y * p.normal.y) +
                              (camera.z * p.normal.z + p.distance);
                if (d < 0.0f) {
                    inside = false;
                    break;
                }
                m = std::min(d, m);
            }
            if (!inside || !(m > 0.0f))
                continue;
            const f32 w = m <= e.end ? 1.0f - (e.end - std::min(m, e.end)) / e.end : 1.0f;
            s.c[0][0] += (static_cast<f32>(e.color1.r) * w) * k;
            s.c[0][1] += w * (static_cast<f32>(e.color1.g) * k);
            s.c[0][2] += w * (static_cast<f32>(e.color1.b) * k);
            if (e.flags & 1) {
                const Color* cs[2] = {&e.color2, &e.color3};
                for (u32 i = 0; i < 2; ++i) {
                    s.c[i + 1][0] += (static_cast<f32>(cs[i]->r) * k) * w;
                    s.c[i + 1][1] += w * (static_cast<f32>(cs[i]->g) * k);
                    s.c[i + 1][2] += w * (static_cast<f32>(cs[i]->b) * k);
                }
            } else {
                for (u32 i = 1; i < 3; ++i) {
                    s.c[i][0] += (static_cast<f32>(e.color1.r) * w) * k;
                    s.c[i][1] += w * (static_cast<f32>(e.color1.g) * k);
                    s.c[i][2] += w * (static_cast<f32>(e.color1.b) * k);
                }
            }
            s.weight += w;
        }
        // The placement's triple fills what the volumes leave of a whole weight.
        f32 total = s.weight;
        if (s.weight < 1.0f) {
            const f32 rest = 1.0f - s.weight;
            for (u32 i = 0; i < 3; ++i) {
                s.c[i][0] += (static_cast<f32>(base[i].r) * rest) * k;
                s.c[i][1] += (static_cast<f32>(base[i].g) * rest) * k;
                s.c[i][2] += (static_cast<f32>(base[i].b) * rest) * k;
            }
            total = 1.0f;
        }
        const f32 inv = 1.0f / total;
        for (u32 i = 0; i < 3; ++i)
            vol[i] = {UnitToByte(inv * s.c[i][2]), UnitToByte(inv * s.c[i][1]),
                      UnitToByte(inv * s.c[i][0]), 0xFF};
    }

    // Back to the placement's triple over the last 10 yd before an exterior portal.
    const f32 t = (10.0f - portalDistance) * 0.1f;
    const u8 tb =
        static_cast<u8>(static_cast<i32>((t >= 0.0f ? std::min(t, 1.0f) : 0.0f) * 255.0f));
    for (u32 i = 0; i < 3; ++i)
        vol[i] = LerpBytes(vol[i], tb, base[i]);
    return vol;
}

namespace {

// The fog ids a camera group names: its MFVR, else its four header bytes.
std::vector<u16> GroupFogIds(const Group& group) {
    if (!group.fogRefs.empty())
        return group.fogRefs;
    return {group.header.fogIds[0], group.header.fogIds[1], group.header.fogIds[2],
            group.header.fogIds[3]};
}

// An MFED doodad set that is off hides its fog.
bool FogSetActive(const Root& root, u16 id, std::span<const u16> activeSets) {
    if (root.fogExtras.empty() || id >= root.fogExtras.size())
        return true;
    return SetActive(root.fogExtras[id].doodadSetId, activeSets);
}

f32 CameraDistance(const Vector3f& a, const Vector3f& camera) {
    const f32 dx = a.x - camera.x, dy = a.y - camera.y, dz = a.z - camera.z;
    return std::sqrt((dx * dx + dy * dy) + dz * dz);
}

// A sphere fog's weight at @p d: 1 inside the start radius, to 0 at the end.
f32 SphereWeight(const Fog& f, f32 d) {
    const f32 dc = d >= 0.0f ? std::min(f.radiusEnd, d) : 0.0f;
    return dc >= f.radiusStart ? 1.0f - (dc - f.radiusStart) / (f.radiusEnd - f.radiusStart) : 1.0f;
}

// CMapObj_BlendFogsSorted: farthest first, each fog lerped over the running result.
void BlendFogsSorted(const Root& root, const Group& group, const Vector3f& camera,
                     std::span<const u16> activeSets, Fog& out) {
    std::vector<std::pair<f32, u16>> taken;
    for (u16 id : GroupFogIds(group)) {
        if (id == 0 || id >= root.fogs.size() || !FogSetActive(root, id, activeSets))
            continue;
        const Fog& f = root.fogs[id];
        const f32 d = CameraDistance(f.position, camera);
        if (d < f.radiusEnd && !(f.flags & 1))
            taken.emplace_back(d, id);
    }
    std::stable_sort(taken.begin(), taken.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < taken.size(); ++i) {
        const Fog& f = root.fogs[taken[i].second];
        const f32 w = SphereWeight(f, taken[i].first);
        const u8 wb = static_cast<u8>(static_cast<i32>(w * 255.0f));
        out.fog.end = (f.fog.end - out.fog.end) * w + out.fog.end;
        out.fog.startScalar = (f.fog.startScalar - out.fog.startScalar) * w + out.fog.startScalar;
        out.fog.color = LerpBytes(out.fog.color, wb, f.fog.color);
        out.underwater.end = (f.underwater.end - out.underwater.end) * w + out.underwater.end;
        out.underwater.startScalar = (f.underwater.startScalar - out.underwater.startScalar) * w +
                                     out.underwater.startScalar;
        out.underwater.color = LerpBytes(out.underwater.color, wb, f.underwater.color);
        if (i + 1 == taken.size())
            out.flags = f.flags; // the nearest one's
    }
}

// CMapObj_BlendFogsWeighted: the spheres and MFOB boxes averaged by weight,
// MFOG[0] filling what they leave of a whole one.
void BlendFogsWeighted(const Root& root, const Group& group, const Vector3f& camera,
                       std::span<const u16> activeSets, Fog& out) {
    constexpr f32 k = 0.0039215689f;
    f32 color[3] = {}, uwColor[3] = {};
    f32 end = 0.0f, startScalar = 0.0f, uwEnd = 0.0f, uwStartScalar = 0.0f, weight = 0.0f;
    const auto add = [&](f32 w, const FogParams& fog, const FogParams& uw) {
        color[0] += (static_cast<f32>(fog.color.r) * w) * k;
        color[1] += (static_cast<f32>(fog.color.g) * w) * k;
        color[2] += (static_cast<f32>(fog.color.b) * w) * k;
        uwColor[0] += (static_cast<f32>(uw.color.r) * w) * k;
        uwColor[1] += (static_cast<f32>(uw.color.g) * w) * k;
        uwColor[2] += (static_cast<f32>(uw.color.b) * w) * k;
        startScalar += w * fog.startScalar;
        end += w * fog.end;
        uwStartScalar += w * uw.startScalar;
        uwEnd += w * uw.end;
        weight += w;
    };

    // The nearest volume decides the flags. A box compares its depth inside
    // against the spheres' distances, and one the camera is outside of takes
    // -FLT_MAX, which nothing after it beats: the client's own rule.
    f32 nearest = std::numeric_limits<f32>::max();
    u16 nearestId = 0;
    bool nearestIsSphere = true;
    for (u16 id : GroupFogIds(group)) {
        if (id == 0 || id >= root.fogs.size() || !FogSetActive(root, id, activeSets))
            continue;
        const Fog& f = root.fogs[id];
        const f32 d = CameraDistance(f.position, camera);
        if (d < f.radiusEnd && !(f.flags & 1))
            add(SphereWeight(f, d), f.fog, f.underwater);
        if (d < nearest) {
            nearest = d;
            nearestId = id;
            nearestIsSphere = true;
        }
    }
    for (u16 id : group.fogBoxRefs) {
        if (id >= root.fogBoxes.size())
            continue;
        const FogBox& b = root.fogBoxes[id];
        if (!SetActive(b.doodadSetId, activeSets))
            continue;
        f32 m = std::numeric_limits<f32>::max();
        for (const Plane& p : b.planes) {
            const f32 d = (camera.y * p.normal.y + camera.x * p.normal.x) +
                          (camera.z * p.normal.z + p.distance);
            if (d < 0.0f) {
                m = -std::numeric_limits<f32>::max();
                break;
            }
            m = std::min(d, m);
        }
        if (m > 0.0f) {
            const f32 w =
                m <= b.fadeDistance
                    ? 1.0f - (b.fadeDistance - std::min(b.fadeDistance, m)) / b.fadeDistance
                    : 1.0f;
            add(w, b.fog, b.underwater);
        }
        if (m < nearest) {
            nearest = m;
            nearestId = id;
            nearestIsSphere = false;
        }
    }
    if (nearestId != 0)
        out.flags = nearestIsSphere ? root.fogs[nearestId].flags : root.fogBoxes[nearestId].flags;

    if (weight < 1.0f) {
        const Fog& base = root.fogs.front();
        const f32 rest = 1.0f - weight;
        color[0] += (static_cast<f32>(base.fog.color.r) * k) * rest;
        color[1] += (static_cast<f32>(base.fog.color.g) * k) * rest;
        color[2] += (static_cast<f32>(base.fog.color.b) * k) * rest;
        startScalar += base.fog.startScalar * rest;
        uwColor[0] += (static_cast<f32>(base.underwater.color.r) * k) * rest;
        uwColor[1] += (static_cast<f32>(base.underwater.color.g) * k) * rest;
        uwColor[2] += (static_cast<f32>(base.underwater.color.b) * k) * rest;
        end += base.fog.end * rest;
        uwStartScalar += base.underwater.startScalar * rest;
        uwEnd += base.underwater.end * rest;
        weight = 1.0f;
    }
    const f32 inv = 1.0f / weight;
    out.fog.color = {UnitToByte(inv * color[2]), UnitToByte(inv * color[1]),
                     UnitToByte(inv * color[0]), 0xFF};
    out.underwater.color = {UnitToByte(uwColor[2] * inv), UnitToByte(uwColor[1] * inv),
                            UnitToByte(uwColor[0] * inv), 0xFF};
    out.fog.startScalar = startScalar * inv;
    out.underwater.startScalar = uwStartScalar * inv;
    out.fog.end = end * inv;
    out.underwater.end = uwEnd * inv;
}

} // namespace

std::optional<Fog> cameraFog(const Model& model, u32 cameraGroup, const Vector3f& camera,
                             std::span<const u16> activeSets) {
    const Root& root = model.root;
    if (root.fogs.empty())
        return std::nullopt;
    Fog out = root.fogs.front();
    const bool weighted = (out.flags & 0x1000u) != 0;
    // A lone MFOG[0] fogs nothing unless it asks for the weighted blend; that
    // blend is off under 0x10000.
    if (root.fogs.size() + root.fogBoxes.size() == 1 && !weighted)
        return std::nullopt;
    if (weighted && (out.flags & 0x10000u))
        return std::nullopt;
    const Group* group = cameraGroup < model.groups.size() && model.groups[cameraGroup]
                             ? &*model.groups[cameraGroup]
                             : nullptr;
    if (group) {
        if (weighted)
            BlendFogsWeighted(root, *group, camera, activeSets, out);
        else
            BlendFogsSorted(root, *group, camera, activeSets, out);
    }
    return out;
}

namespace {

constexpr u16 kNoGroup = 0xFFFF;

Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
f32 Dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f32 Length(const Vector3f& v) {
    return std::sqrt(Dot(v, v));
}
f32 Axis(const Vector3f& v, u32 a) {
    return a == 0 ? v.x : (a == 1 ? v.y : v.z);
}

// CAaBox_IntersectSegment 0x143682FD0: the slab test over the segment's span.
bool BoxHitsSegment(const Box& b, const Vector3f& p0, const Vector3f& p1) {
    f32 lo = 0.0f, hi = 1.0f;
    for (u32 a = 0; a < 3; ++a) {
        const f32 o = Axis(p0, a), d = Axis(p1, a) - o;
        const f32 mn = Axis(b.minimum, a), mx = Axis(b.maximum, a);
        if (std::fabs(d) < 1.0e-12f) {
            if (o < mn || o > mx)
                return false;
            continue;
        }
        f32 t0 = (mn - o) / d, t1 = (mx - o) / d;
        if (t0 > t1)
            std::swap(t0, t1);
        lo = std::max(lo, t0);
        hi = std::min(hi, t1);
        if (lo > hi)
            return false;
    }
    return true;
}

std::span<const Vector3f> PortalPolygon(const Root& root, const Portal& portal) {
    const std::size_t first = portal.startVertex;
    const std::size_t last = first + portal.vertexCount;
    if (first >= last || last > root.portalVertices.size())
        return {};
    return std::span<const Vector3f>(root.portalVertices).subspan(first, last - first);
}

bool PointInPortal(const Vector3f& p, const Root& root, const Portal& portal) {
    return InsidePolygon(p, PortalPolygon(root, portal), MajorAxis(portal.plane.normal));
}

// C3Vector_DistanceToPolygon 0x14368AD50: to the plane where the point projects
// inside the portal, else to its nearest edge.
f32 DistanceToPortal(const Vector3f& p, const Root& root, const Portal& portal) {
    const auto poly = PortalPolygon(root, portal);
    const Vector3f& n = portal.plane.normal;
    const f32 d = PlaneDistance(portal.plane, p);
    Vector3f onPlane = p;
    if (static_cast<f64>(d) > 0.001)
        onPlane = IntersectRayPlane(p, {-n.x, -n.y, -n.z}, portal.plane);
    else if (static_cast<f64>(d) < -0.001)
        onPlane = IntersectRayPlane(p, n, portal.plane);
    if (InsidePolygon(onPlane, poly, MajorAxis(n)))
        return std::fabs(d);
    return DistanceFromPolygonEdge(p, poly);
}

// RayTriangleIntersect 0x143685840: Möller–Trumbore, both faces, 0.002 of slack.
bool RayHitsTriangle(const Vector3f& o, const Vector3f& d, const Vector3f& a, const Vector3f& b,
                     const Vector3f& c, f32& dist) {
    const Vector3f e1 = Sub(b, a), e2 = Sub(c, a);
    const Vector3f p = Cross(d, e2);
    const f32 det = Dot(e1, p);
    if (std::fabs(det) < 1.0e-6f)
        return false;
    const f32 inv = 1.0f / det;
    const Vector3f s = Sub(o, a);
    const f32 u = Dot(s, p) * inv;
    if (u < -0.002f)
        return false;
    const Vector3f q = Cross(s, e1);
    const f32 v = Dot(d, q) * inv;
    if (v < -0.002f || u + v > 1.002f)
        return false;
    dist = Dot(e2, q) * inv;
    return true;
}

// CAaBsp_QuerySegmentLeaves 0x141B19850: the leaves the segment passes through.
void BspLeaves(const Group& g, i32 node, Box box, const Vector3f& a, const Vector3f& b,
               std::vector<u32>& leaves) {
    constexpr f32 eps = 0.01f;
    if (node < 0 || static_cast<size_t>(node) >= g.bspNodes.size() || leaves.size() >= 0x8000)
        return;
    const BspNode& n = g.bspNodes[static_cast<size_t>(node)];
    if (n.flags & 4) {
        leaves.push_back(static_cast<u32>(node));
        return;
    }
    const u32 ax = n.flags & 3u;
    if (ax > 2)
        return;
    const f32 amin = Axis(a, ax) - Axis(box.minimum, ax),
              bmin = Axis(b, ax) - Axis(box.minimum, ax);
    const f32 amax = Axis(a, ax) - Axis(box.maximum, ax),
              bmax = Axis(b, ax) - Axis(box.maximum, ax);
    if (!((amin >= -eps || bmin >= -eps) && (amax <= eps || bmax <= eps)))
        return;
    Box pos = box, neg = box;
    (ax == 0 ? pos.minimum.x : ax == 1 ? pos.minimum.y : pos.minimum.z) = n.planeDistance;
    (ax == 0 ? neg.maximum.x : ax == 1 ? neg.maximum.y : neg.maximum.z) = n.planeDistance;
    const f32 da = Axis(a, ax) - n.planeDistance, db = Axis(b, ax) - n.planeDistance;
    if (std::fabs(da) <= eps || std::fabs(db) <= eps) {
        BspLeaves(g, n.positiveChild, pos, a, b, leaves);
        BspLeaves(g, n.negativeChild, neg, a, b, leaves);
    } else if (da > eps && db > eps) {
        BspLeaves(g, n.positiveChild, pos, a, b, leaves);
    } else if (da < -eps && db < -eps) {
        BspLeaves(g, n.negativeChild, neg, a, b, leaves);
    } else {
        const f32 k = da / (da - db);
        const Vector3f m{a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k};
        if (da > 0.0f) {
            BspLeaves(g, n.positiveChild, pos, a, m, leaves);
            BspLeaves(g, n.negativeChild, neg, m, b, leaves);
        } else {
            BspLeaves(g, n.negativeChild, neg, a, m, leaves);
            BspLeaves(g, n.positiveChild, pos, m, b, leaves);
        }
    }
}

bool FaceTriangle(const Group& g, u32 face, Vector3f (&tri)[3]) {
    if (3u * face + 2u >= g.indices.size())
        return false;
    for (u32 k = 0; k < 3; ++k) {
        const u32 vi = g.indices[3u * face + k];
        if (vi >= g.positions.size())
            return false;
        tri[k] = g.positions[vi];
    }
    return true;
}

// CMapObjGroup_IntersectSegment 0x141B11560 as the viewer query calls it: the
// nearest face, any face, through the BSP or, under flags2 0x100, all of them.
bool NearestFace(const Group& g, const Vector3f& p0, const Vector3f& dir, f32& maxDist) {
    bool hit = false;
    const auto test = [&](u32 face) {
        Vector3f tri[3];
        f32 d;
        if (FaceTriangle(g, face, tri) && RayHitsTriangle(p0, dir, tri[0], tri[1], tri[2], d) &&
            d >= 0.0f && d <= maxDist) {
            maxDist = d;
            hit = true;
        }
    };
    if (g.header.flags2 & 0x100u) {
        for (u32 f = 0; 3u * f + 2u < g.indices.size(); ++f)
            test(f);
        return hit;
    }
    if (g.bspNodes.empty())
        return false;
    std::vector<u32> leaves;
    const Vector3f end{p0.x + dir.x * maxDist, p0.y + dir.y * maxDist, p0.z + dir.z * maxDist};
    BspLeaves(g, 0, g.header.bounds, p0, end, leaves);
    std::vector<bool> seen;
    for (u32 leaf : leaves) {
        const BspNode& n = g.bspNodes[leaf];
        for (u32 i = 0; i < n.faceCount; ++i) {
            const size_t r = static_cast<size_t>(n.firstFace) + i;
            if (r >= g.bspFaces.size())
                break;
            const u16 f = g.bspFaces[r];
            if (f >= seen.size())
                seen.resize(f + 1u, false);
            if (seen[f])
                continue;
            seen[f] = true;
            test(f);
        }
    }
    return hit;
}

u32 MogiFlags(const Root& root, u32 g) {
    return g < root.groups.size() ? root.groups[g].flags : 0u;
}

// CMapObj_DistFromClosestExtPortal's walk: portals into @p mask groups measured,
// the rest walked through, three portals deep.
void WalkExteriorPortals(const Model& model, u32 depth, const Group& g, const Group* prev,
                         const Vector3f& p, f32& best) {
    if (depth > 3)
        return;
    const Root& root = model.root;
    for (u32 i = 0; i < g.header.portalCount; ++i) {
        const size_t r = static_cast<size_t>(g.header.portalStart) + i;
        if (r >= root.portalRefs.size())
            break;
        const PortalRef& ref = root.portalRefs[r];
        if (ref.flags & 1u)
            continue;
        if (ref.groupIndex >= model.groups.size() || !model.groups[ref.groupIndex])
            continue;
        const Group& t = *model.groups[ref.groupIndex];
        if (&t == prev)
            continue;
        if (MogiFlags(root, ref.groupIndex) & 0x48u) {
            if (ref.portalIndex < root.portals.size()) {
                const f32 d = DistanceToPortal(p, root, root.portals[ref.portalIndex]);
                if (d < 25.0f && d < best)
                    best = d;
            }
        } else {
            WalkExteriorPortals(model, depth + 1, t, &g, p, best);
        }
    }
}

} // namespace

CameraLocation locateCamera(const Model& model, const Vector3f& eye, const Vector3f& below,
                            f32 worldScale, f32 tLimit) {
    const Root& root = model.root;
    CameraLocation out;
    out.t = tLimit;
    const Vector3f seg = Sub(below, eye);
    const f32 len = Length(seg);
    if (!(len > 0.0f))
        return out;
    const Vector3f dir{seg.x / len, seg.y / len, seg.z / len};
    const f32 invLenWorld = 1.0f / (len * worldScale);

    // CWorldMap_LocateViewer_CollectGroups: every group whose box the drop touches.
    std::vector<std::pair<f32, u32>> candidates;
    for (u32 g = 0; g < root.groups.size() && g < model.groups.size(); ++g) {
        if ((root.groups[g].flags & 0x410080u) || !model.groups[g])
            continue;
        const Box& b = root.groups[g].bounds;
        const bool inside = eye.x >= b.minimum.x - 0.001f && eye.x <= b.maximum.x + 0.001f &&
                            eye.y >= b.minimum.y - 0.001f && eye.y <= b.maximum.y + 0.001f &&
                            eye.z >= b.minimum.z - 0.001f && eye.z <= b.maximum.z + 0.001f;
        if (inside) {
            candidates.emplace_back(0.0f, g);
        } else if (BoxHitsSegment(b, eye, below)) {
            const Vector3f q{std::clamp(eye.x, b.minimum.x, b.maximum.x),
                             std::clamp(eye.y, b.minimum.y, b.maximum.y),
                             std::clamp(eye.z, b.minimum.z, b.maximum.z)};
            candidates.emplace_back(Length(Sub(eye, q)) * worldScale, g);
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    u16 g0 = kNoGroup, g1 = kNoGroup;
    bool exterior = false, found = false;
    for (const auto& [dist, g] : candidates) {
        if (dist * invLenWorld > out.t)
            break;
        const Group& group = *model.groups[g];
        f32 maxDist = len * out.t;
        if (NearestFace(group, eye, dir, maxDist)) {
            out.t = std::min(maxDist / len, out.t);
            found = true;
            g0 = static_cast<u16>(g);
            g1 = kNoGroup;
            exterior = (group.header.flags & 0x8u) != 0;
            if (!exterior) {
                // Within a third of a yard of a portal into another interior group: both count.
                f32 best = 0.3333f;
                u16 near = kNoGroup;
                for (u32 i = 0; i < group.header.portalCount; ++i) {
                    const size_t r = static_cast<size_t>(group.header.portalStart) + i;
                    if (r >= root.portalRefs.size())
                        break;
                    const PortalRef& ref = root.portalRefs[r];
                    if ((ref.flags & 1u) || ref.portalIndex >= root.portals.size())
                        continue;
                    const f32 d = DistanceToPortal(eye, root, root.portals[ref.portalIndex]);
                    if (d < best) {
                        best = d;
                        near = ref.groupIndex;
                    }
                }
                if (near != kNoGroup && !(MogiFlags(root, near) & 0x8u))
                    g1 = near;
            }
        }
        // CMapObj_VectorIntersectPortals: a portal no more than 1 yd past the face wins.
        f32 maxD = len * 1.05f;
        bool portalHit = false;
        u16 side[2] = {kNoGroup, kNoGroup};
        for (u32 i = 0; i < group.header.portalCount; ++i) {
            const size_t r = static_cast<size_t>(group.header.portalStart) + i;
            if (r >= root.portalRefs.size())
                break;
            const PortalRef& ref = root.portalRefs[r];
            if ((ref.flags & 1u) || ref.portalIndex >= root.portals.size() ||
                ref.groupIndex >= model.groups.size() || !model.groups[ref.groupIndex])
                continue;
            const Portal& portal = root.portals[ref.portalIndex];
            const f32 s = Dot(portal.plane.normal, eye) + portal.plane.distance;
            const f32 den = Dot(portal.plane.normal, dir);
            f32 d;
            if (std::fabs(den) < 1.0e-4f) {
                if (!(std::fabs(s) < 0.1f))
                    continue;
                d = 0.0f;
            } else {
                d = std::fabs(s) < 0.1f ? 0.0f : -s / den;
            }
            if (d < 0.0f || d > maxD)
                continue;
            const Vector3f q{eye.x + dir.x * d, eye.y + dir.y * d, eye.z + dir.z * d};
            if (!PointInPortal(q, root, portal))
                continue;
            maxD = d;
            portalHit = true;
            const u16 self = static_cast<u16>(g), other = ref.groupIndex;
            const bool eyeFirst = s < 0.0f ? ref.side <= 0 : ref.side > 0;
            side[0] = eyeFirst ? self : other;
            side[1] = eyeFirst ? other : self;
        }
        if (portalHit) {
            const f32 tp = maxD / len;
            if (tp - out.t < 0.0001f) {
                out.t = tp;
                found = true;
                g0 = side[0];
                exterior = (MogiFlags(root, side[0]) & 0x8u) != 0;
                g1 = (MogiFlags(root, side[1]) & 0x8u) ? kNoGroup : side[1];
            }
        }
    }
    // The nearest surface below belongs to an exterior group: not inside.
    out.hit = found;
    if (!found || exterior)
        return out;
    out.inside = true;
    out.groups = {g0, g1 == g0 ? kNoGroup : g1};
    return out;
}

f32 exteriorPortalDistance(const Model& model, const CameraLocation& location,
                           const Vector3f& eye) {
    if (!location.inside)
        return 0.0f;
    bool interior = false;
    f32 dPort = std::numeric_limits<f32>::max();
    for (u16 gi : location.groups) {
        if (gi == kNoGroup || gi >= model.groups.size() || !model.groups[gi])
            continue;
        const Group* g = &*model.groups[gi];
        if (g->header.flags & 0x48u)
            continue;
        interior = true;
        if (g->header.flags2 & 0x80u) {
            const i16 parent = g->header.splitParentOrFirstChild;
            g = parent >= 0 && static_cast<size_t>(parent) < model.groups.size() &&
                        model.groups[static_cast<size_t>(parent)]
                    ? &*model.groups[static_cast<size_t>(parent)]
                    : nullptr;
        }
        f32 d = std::numeric_limits<f32>::max();
        if (g)
            WalkExteriorPortals(model, 0, *g, nullptr, eye, d);
        dPort = std::min(dPort, d);
    }
    return interior ? dPort : 0.0f;
}

namespace {

// One component of the MOUV scroll: the phase of @p timeMs in a period of
// `1000 / speed` ms, the period truncated to an integer as the client does.
f32 ScrollPhase(f32 speed, u32 timeMs) {
    if (speed == 0.0f)
        return 0.0f;
    const i32 period = static_cast<i32>(1000.0f / speed);
    if (period == 0)
        return 0.0f;
    if (period > 0)
        return static_cast<f32>(static_cast<i32>(timeMs % static_cast<u32>(period))) /
               static_cast<f32>(period);
    const u32 back = static_cast<u32>(-period);
    return 1.0f - static_cast<f32>(static_cast<i32>(timeMs % back)) / static_cast<f32>(back);
}

} // namespace

UvScroll uvScroll(const Root& root, u32 material, u32 timeMs) {
    if (material >= root.uvAnimations.size() || material >= root.materials.size())
        return {};
    return uvScroll(root.uvAnimations[material], timeMs);
}

UvScroll uvScroll(const MaterialUvAnimation& anim, u32 timeMs) {
    UvScroll out;
    out.layer0 = {ScrollPhase(anim.speed0.x, timeMs), ScrollPhase(anim.speed0.y, timeMs)};
    out.layer1 = {ScrollPhase(anim.speed1.x, timeMs), ScrollPhase(anim.speed1.y, timeMs)};
    out.animated = !(out.layer0.x == 0.0f && out.layer0.y == 0.0f && out.layer1.x == 0.0f &&
                     out.layer1.y == 0.0f);
    return out;
}

u32 groupLiquidType(const Root& root, const Group& group) {
    u32 t;
    if (hasFlag(root.header.flags, RootFlag::LiquidTypeFromDb))
        t = group.header.groupLiquid;
    else
        t = group.header.groupLiquid == 15 ? 0 : group.header.groupLiquid + 1;
    const bool ocean = hasFlag(group.header.flags, GroupFlag::Ocean);
    const auto basic = [&](u32 b) -> u32 {
        switch (b & 3) {
        case 0:
            return ocean ? 14 : 13;
        case 1:
            return 14;
        case 2:
            return 19;
        default:
            return 20;
        }
    };
    if (t >= 1 && t <= 20)
        return basic(t - 1);
    if (t == 0 && group.liquid) {
        // Legacy files name the type per tile: the first tile that is not
        // "no liquid" (low nibble 0xF) decides.
        for (u8 tile : group.liquid->tiles) {
            if ((tile & 0x0F) != 0x0F)
                return basic(tile & 3);
        }
    }
    return t;
}

bool hasLiquid(const Group& group) {
    return (group.header.flags & 0x1000u) && !(group.header.flags2 & 0x80u);
}

bool liquidExterior(const Group& group, u32 typeFlags) {
    return (group.header.flags & 0x48u) || (typeFlags & 0x200u);
}

u32 liquidDrawType(u32 type, bool exterior) {
    return !exterior && type >= 1 && type <= 20 && ((type - 1) & 3) == 0 ? 17 : type;
}

std::array<f32, 4> liquidDepthCurve(i32 lvf, u32 int0, const std::array<f32, 4>& coefficients) {
    if (lvf != 0 && lvf != 2 && lvf != 3 && lvf != 4)
        return {0.0f, 1.0f, 0.0f, 1.0f};
    if (int0 >= 2)
        return coefficients;
    if (int0 == 1)
        return {0.0f, 1.0f, 0.0f, 0.0f};
    return {0.0f, 6.0714f, 0.0f, 0.0f};
}

namespace {

// 12.1's liquid constants, bit for bit: two are not the nearest floats.
const f32 kLiquidTile = std::bit_cast<f32>(0x40855555u);    // 4.1666665
const f32 kWorldUvScale = std::bit_cast<f32>(0x3D75C290u);  // 0.06
const f32 kSharedUvScale = std::bit_cast<f32>(0x3E75C28Fu); // 0.24
const f32 kInv255 = std::bit_cast<f32>(0x3B808081u);
const f32 kInv65025 = std::bit_cast<f32>(0x37810183u);

// The depth weight: the grid's sum and a shared vertex's add the same terms in
// different orders.
f32 LiquidDepth(u8 byte, const std::array<f32, 4>& c, bool shared) {
    const f32 d = static_cast<f32>(byte);
    const f32 a = d * kInv255;
    const f32 b = (d * d) * kInv65025;
    const f32 sum = shared ? ((b * a) * c[3] + b * c[2]) + (a * c[1] + c[0])
                           : ((a * c[1] + c[0]) + b * c[2]) + (b * a) * c[3];
    return std::min(sum, 1.0f);
}

Vector2f LiquidUv(const std::array<u8, 4>& attr, const Vector3f& p, const Vector3f& corner,
                  bool hasUV, bool worldUV, bool shared) {
    if (hasUV) {
        i16 st[2];
        std::memcpy(st, attr.data(), sizeof(st));
        return {static_cast<f32>(st[0]) / 256.0f, static_cast<f32>(st[1]) / 256.0f};
    }
    if (worldUV)
        return {p.x * kWorldUvScale, p.y * kWorldUvScale};
    if (shared)
        return {(p.x - corner.x) * kSharedUvScale, (p.y - corner.y) * kSharedUvScale};
    return {};
}

// `sub_141B6E060`'s polygon: vertices that remember how a cut made them, and
// edges that name their ends.
struct ClipVertex {
    Vector3f p{0.0f, 0.0f, 0.0f};
    std::optional<std::array<u8, 4>> attr; ///< none while derived and unresolved
    i32 parentA = -1;
    i32 parentB = -1;
    f32 t = 0.0f;
    i32 edgeIn = -1;
    i32 edgeOut = -1;
};
struct ClipEdge {
    i32 v0 = 0;
    i32 v1 = 0;
    bool removed = false;
};
struct ClipPoly {
    std::vector<ClipVertex> v;
    std::vector<ClipEdge> e;
};

// Keeps `side·(n·p + d) ≥ 0`. Only a cut with exactly two crossings changes
// the polygon's shape; edges wholly outside go either way.
void ClipLiquidPolygon(ClipPoly& poly, const Plane& plane, i32 side) {
    const f32 s = static_cast<f32>(side);
    const Vector3f& n = plane.normal;
    std::vector<f32> dist(poly.v.size());
    for (std::size_t k = 0; k < poly.v.size(); ++k) {
        const Vector3f& q = poly.v[k].p;
        dist[k] = ((q.y * n.y + q.x * n.x) + (q.z * n.z + plane.distance)) * s;
    }
    const std::size_t firstNew = poly.v.size();
    const i32 newEdge = static_cast<i32>(poly.e.size());
    i32 crossings = 0, exitEdge = -1, entryEdge = -1, exitVertex = -1, entryVertex = -1;
    for (std::size_t k = 0; k < static_cast<std::size_t>(newEdge); ++k) {
        ClipEdge& e = poly.e[k];
        if (e.removed)
            continue;
        const f32 da = dist[static_cast<std::size_t>(e.v0)],
                  db = dist[static_cast<std::size_t>(e.v1)];
        const bool aIn = da >= 0.0f;
        const bool bIn = !(db < 0.0f);
        if (aIn && bIn)
            continue;
        if (!aIn && !bIn) {
            e.removed = true;
            continue;
        }
        const f32 sum = std::abs(db) + std::abs(da);
        if (sum <= std::numeric_limits<f32>::epsilon())
            continue;
        if (++crossings > 2)
            break;
        const f32 t = std::abs(da) / sum;
        const Vector3f& A = poly.v[static_cast<std::size_t>(e.v0)].p;
        const Vector3f& B = poly.v[static_cast<std::size_t>(e.v1)].p;
        ClipVertex c;
        c.p = {(B.x - A.x) * t + A.x, (B.y - A.y) * t + A.y, (B.z - A.z) * t + A.z};
        c.parentA = e.v0;
        c.parentB = e.v1;
        c.t = t;
        const i32 index = static_cast<i32>(poly.v.size());
        if (aIn) {
            c.edgeIn = static_cast<i32>(k);
            c.edgeOut = newEdge;
            exitEdge = static_cast<i32>(k);
            exitVertex = index;
        } else {
            c.edgeOut = static_cast<i32>(k);
            c.edgeIn = newEdge;
            entryEdge = static_cast<i32>(k);
            entryVertex = index;
        }
        poly.v.push_back(c);
    }
    if (crossings != 2) {
        poly.v.resize(firstNew);
        return;
    }
    poly.e[static_cast<std::size_t>(exitEdge)].v1 = exitVertex;
    poly.e[static_cast<std::size_t>(entryEdge)].v0 = entryVertex;
    poly.e.push_back({exitVertex, entryVertex, false});
}

// A cut vertex's attributes, lerped from its parents' and truncated: a UV mesh
// lerps two u16 halves, the rest byte 0 alone.
const std::array<u8, 4>& ResolveLiquidAttr(ClipPoly& poly, i32 k, bool hasUV) {
    ClipVertex& v = poly.v[static_cast<std::size_t>(k)];
    if (v.attr)
        return *v.attr;
    const std::array<u8, 4> b = ResolveLiquidAttr(poly, v.parentB, hasUV);
    const std::array<u8, 4> a = ResolveLiquidAttr(poly, v.parentA, hasUV);
    std::array<u8, 4> out{};
    const f32 t = poly.v[static_cast<std::size_t>(k)].t;
    if (hasUV) {
        for (std::size_t h = 0; h < 2; ++h) {
            u16 ua, ub;
            std::memcpy(&ua, a.data() + 2 * h, 2);
            std::memcpy(&ub, b.data() + 2 * h, 2);
            const u16 r = static_cast<u16>(static_cast<i32>(
                (static_cast<f32>(ub) - static_cast<f32>(ua)) * t + static_cast<f32>(ua)));
            std::memcpy(out.data() + 2 * h, &r, 2);
        }
    } else {
        out[0] = static_cast<u8>(static_cast<i32>(
            (static_cast<f32>(b[0]) - static_cast<f32>(a[0])) * t + static_cast<f32>(a[0])));
    }
    poly.v[static_cast<std::size_t>(k)].attr = out;
    return *poly.v[static_cast<std::size_t>(k)].attr;
}

} // namespace

LiquidMesh buildLiquidMesh(const Model& model, u32 groupIndex, bool hasUV, bool worldUV,
                           const std::array<f32, 4>& depthCurve) {
    LiquidMesh out;
    if (groupIndex >= model.groups.size() || !model.groups[groupIndex] ||
        !model.groups[groupIndex]->liquid)
        return out;
    const Root& root = model.root;
    const Group& group = *model.groups[groupIndex];
    const Liquid& liquid = *group.liquid;
    if (liquid.material >= root.materials.size() || liquid.xVertices <= 0 || liquid.yVertices <= 0)
        return out;
    const u32 xv = static_cast<u32>(liquid.xVertices), yv = static_cast<u32>(liquid.yVertices);
    if (liquid.vertices.size() < static_cast<std::size_t>(xv) * yv)
        return out;

    out.vertices.reserve(static_cast<std::size_t>(xv) * yv);
    f32 y = liquid.corner.y;
    for (u32 j = 0; j < yv; ++j, y += kLiquidTile) {
        f32 x = liquid.corner.x;
        for (u32 i = 0; i < xv; ++i, x += kLiquidTile) {
            const LiquidVertex& v = liquid.vertices[static_cast<std::size_t>(j) * xv + i];
            LiquidMeshVertex m;
            m.position = {x, y, v.height};
            m.uv = (hasUV || worldUV)
                       ? LiquidUv(v.data, m.position, liquid.corner, hasUV, worldUV, false)
                       : Vector2f{static_cast<f32>(i), static_cast<f32>(j)};
            m.depth = LiquidDepth(v.data[0], depthCurve, false);
            out.vertices.push_back(m);
        }
    }

    const u32 xt = static_cast<u32>(std::max(liquid.xTiles, 0)),
              yt = static_cast<u32>(std::max(liquid.yTiles, 0));
    const auto tileAt = [&](u32 i, u32 j) -> u8 {
        const std::size_t k = static_cast<std::size_t>(j) * xt + i;
        return k < liquid.tiles.size() ? liquid.tiles[k] : u8{0x0F};
    };
    for (u32 j = 0; j < yt && j + 1 < yv; ++j) {
        for (u32 i = 0; i < xt && i + 1 < xv; ++i) {
            const u8 b = tileAt(i, j);
            if ((b & 0x0Fu) == 0x0Fu || (b & 0x80u))
                continue;
            // The strip's two triangles for this tile.
            const u32 a = j * xv + i, up = (j + 1) * xv + i;
            out.indices.insert(out.indices.end(), {up, a, up + 1, a, up + 1, a + 1});
        }
    }

    // Shared tiles, each clipped by the portals into neighbours whose liquid
    // rectangle overlaps it (with none, the rectangle is 0).
    for (u32 j = 0; j < yt && j + 1 < yv; ++j) {
        for (u32 i = 0; i < xt && i + 1 < xv; ++i) {
            const u8 b = tileAt(i, j);
            if ((b & 0x0Fu) == 0x0Fu || !(b & 0x80u))
                continue;
            const f32 x0 = static_cast<f32>(i) * kLiquidTile + liquid.corner.x;
            const f32 x1 = static_cast<f32>(i + 1) * kLiquidTile + liquid.corner.x;
            const f32 y0 = static_cast<f32>(j) * kLiquidTile + liquid.corner.y;
            const f32 y1 = static_cast<f32>(j + 1) * kLiquidTile + liquid.corner.y;
            const u32 corners[4] = {j * xv + i, (j + 1) * xv + i, (j + 1) * xv + i + 1,
                                    j * xv + i + 1};
            const Vector2f xy[4] = {{x0, y0}, {x0, y1}, {x1, y1}, {x1, y0}};
            ClipPoly poly;
            for (i32 k = 0; k < 4; ++k) {
                const LiquidVertex& lv = liquid.vertices[corners[k]];
                ClipVertex c;
                c.p = {xy[k].x, xy[k].y, lv.height};
                c.attr = lv.data;
                c.edgeIn = (k + 3) % 4;
                c.edgeOut = k;
                poly.v.push_back(c);
                poly.e.push_back({k, (k + 1) % 4, false});
            }
            const u32 refEnd =
                static_cast<u32>(group.header.portalStart) + group.header.portalCount;
            for (u32 r = group.header.portalStart; r < refEnd && r < root.portalRefs.size(); ++r) {
                const PortalRef& ref = root.portalRefs[r];
                if (ref.groupIndex >= model.groups.size() || !model.groups[ref.groupIndex] ||
                    ref.portalIndex >= root.portals.size())
                    continue;
                const Group& n = *model.groups[ref.groupIndex];
                Vector3f nc{0.0f, 0.0f, 0.0f};
                f32 nw = 0.0f, nh = 0.0f;
                if (n.liquid) {
                    nc = n.liquid->corner;
                    nw = static_cast<f32>(n.liquid->xTiles) * kLiquidTile;
                    nh = static_cast<f32>(n.liquid->yTiles) * kLiquidTile;
                }
                if (x1 > nc.x && y1 > nc.y && x0 < nw + nc.x && y0 < nh + nc.y)
                    ClipLiquidPolygon(poly, root.portals[ref.portalIndex].plane, ref.side);
            }

            // The strip: zigzag from the first live edge.
            i32 first = -1;
            for (std::size_t k = 0; k < poly.e.size() && first < 0; ++k) {
                if (!poly.e[k].removed)
                    first = static_cast<i32>(k);
            }
            if (first < 0)
                continue;
            std::vector<i32> order;
            i32 a = poly.e[static_cast<std::size_t>(first)].v0,
                bv = poly.e[static_cast<std::size_t>(first)].v1;
            for (u32 k = 0; k < 64; ++k) {
                order.push_back((k & 1u) ? bv : a);
                if (bv == a)
                    break;
                if ((k & 1u) == 0)
                    a = poly.e[static_cast<std::size_t>(poly.v[static_cast<std::size_t>(a)].edgeIn)]
                            .v0;
                else
                    bv = poly.e[static_cast<std::size_t>(
                                    poly.v[static_cast<std::size_t>(bv)].edgeOut)]
                             .v1;
            }
            const u32 base = static_cast<u32>(out.vertices.size());
            for (const i32 k : order) {
                const std::array<u8, 4> attr = ResolveLiquidAttr(poly, k, hasUV);
                LiquidMeshVertex m;
                m.position = poly.v[static_cast<std::size_t>(k)].p;
                m.uv = LiquidUv(attr, m.position, liquid.corner, hasUV, worldUV, true);
                m.depth = LiquidDepth(attr[0], depthCurve, true);
                out.vertices.push_back(m);
            }
            for (u32 k = 0; k + 2 < order.size(); ++k)
                out.indices.insert(out.indices.end(), {base + k, base + k + 1, base + k + 2});
        }
    }
    return out;
}

std::vector<u8> exteriorAmbientGroups(const Model& model) {
    const Root& root = model.root;
    std::vector<u8> out(model.groups.size(), 0);
    for (u32 g = 0; g < model.groups.size(); ++g) {
        if (!model.groups[g])
            continue;
        const Group& group = *model.groups[g];
        if (group.batches.empty() || !group.colorSet0())
            continue;
        if ((group.header.flags & 0x48) == 0 ||
            hasFlag(group.header.flags2, GroupFlag2::SplitChild))
            continue;
        out[g] = 1;
        for (u32 r = 0; r < group.header.portalCount; ++r) {
            const u32 ref = group.header.portalStart + r;
            if (ref >= root.portalRefs.size())
                break;
            const u32 other = root.portalRefs[ref].groupIndex;
            if (other < root.groups.size() && other < out.size() &&
                hasFlag(root.groups[other].flags, GroupFlag::Interior))
                out[other] = 1;
        }
    }
    return out;
}

u16 doodadSetOf(const Root& root, u32 doodad) {
    u16 set = 0xFFFF;
    for (u32 i = 0; i < root.doodadSets.size(); ++i) {
        const DoodadSet& s = root.doodadSets[i];
        if (doodad >= s.startIndex && doodad - s.startIndex < s.count)
            set = static_cast<u16>(i & 0xFF); // the client's map holds a byte
    }
    return set;
}

std::vector<u32> activeDoodads(const Root& root, const Group& group,
                               std::span<const u16> activeSets) {
    std::vector<u32> out;
    for (u16 ref : group.doodadRefs) {
        if (ref >= root.doodadDefs.size())
            continue;
        const u16 set = doodadSetOf(root, ref);
        if (set == 0 || std::find(activeSets.begin(), activeSets.end(), set) != activeSets.end())
            out.push_back(ref);
    }
    return out;
}

DoodadModel doodadModel(const Root& root, const DoodadDef& def) {
    DoodadModel out;
    if (!root.doodadNames.empty()) {
        out.path = Root::nameAt(root.doodadNames, def.nameIndex());
        return out;
    }
    if (!root.doodadFileIds.empty()) {
        const u32 index = def.nameIndex() < root.doodadFileIds.size() ? def.nameIndex() : 0;
        out.fileId = root.doodadFileIds[index];
    }
    return out;
}

namespace {

template <std::size_t N, typename T>
T ReadAt(const RawRecord<N>& r, std::size_t offset) {
    T v{};
    std::memcpy(&v, r.bytes.data() + offset, sizeof(T));
    return v;
}

template <std::size_t N>
Vector3f Vec3At(const RawRecord<N>& r, std::size_t offset) {
    return {ReadAt<N, f32>(r, offset), ReadAt<N, f32>(r, offset + 4),
            ReadAt<N, f32>(r, offset + 8)};
}

template <std::size_t N>
Color ColorAt(const RawRecord<N>& r, std::size_t offset) {
    return {r.bytes[offset], r.bytes[offset + 1], r.bytes[offset + 2], r.bytes[offset + 3]};
}

// MOLP's first 32 bytes, which the other three records share.
template <std::size_t N>
GatheredLight PointRecord(const RawRecord<N>& r, GatheredLight::Source source) {
    GatheredLight l;
    l.source = source;
    l.id = ReadAt<N, u32>(r, 0);
    l.colorA = l.colorB = ColorAt(r, 4);
    l.position = Vec3At(r, 8);
    l.attenuationStart = l.blendNear = ReadAt<N, f32>(r, 20);
    l.attenuationEnd = l.blendFar = ReadAt<N, f32>(r, 24);
    l.intensity = ReadAt<N, f32>(r, 28);
    return l;
}

template <std::size_t N>
void SpotFields(const RawRecord<N>& r, GatheredLight& l) {
    l.spot = true;
    l.rotation = Vec3At(r, 32);
    l.falloff = ReadAt<N, f32>(r, 44);
    l.innerAngle = ReadAt<N, f32>(r, 48);
    l.outerAngle = ReadAt<N, f32>(r, 52);
}

template <std::size_t N>
void FlickerAt(const RawRecord<N>& r, std::size_t offset, GatheredLight& l) {
    l.flickerAmount = ReadAt<N, f32>(r, offset);
    l.flickerSpeed = ReadAt<N, f32>(r, offset + 4);
    l.flickerMode = ReadAt<N, i16>(r, offset + 8);
}

// A set table's runs: entry 0 always, entry i while set i is on.
template <typename Record, typename Fn>
void ForActiveRuns(const std::vector<LightSetRange>& sets, const std::vector<Record>& records,
                   std::span<const u16> activeSets, Fn&& fn) {
    for (u32 i = 0; i < sets.size(); ++i) {
        if (!SetActive(static_cast<u16>(i), activeSets))
            continue;
        const u64 end = static_cast<u64>(sets[i].offset) + sets[i].count;
        for (u64 k = sets[i].offset; k < end && k < records.size(); ++k)
            fn(records[static_cast<std::size_t>(k)]);
    }
}

} // namespace

std::vector<GatheredLight> gatherLights(const Model& model, u32 group,
                                        std::span<const u16> activeSets, bool rtShadows) {
    std::vector<GatheredLight> out;
    if (group >= model.groups.size() || !model.groups[group])
        return out;
    const Group& g = *model.groups[group];
    using S = GatheredLight::Source;
    ForActiveRuns(g.pointLightSets, g.pointLights, activeSets,
                  [&](const PointLight& r) { out.push_back(PointRecord(r, S::Molp)); });
    ForActiveRuns(g.spotLightSets, g.spotLights, activeSets, [&](const SpotLight& r) {
        GatheredLight l = PointRecord(r, S::Mols);
        SpotFields(r, l);
        out.push_back(l);
    });
    ForActiveRuns(g.pointAnimSets, g.pointLightAnims, activeSets, [&](const PointLightAnim& r) {
        GatheredLight l = PointRecord(r, S::Mop2);
        l.rotation = Vec3At(r, 32);
        FlickerAt(r, 44, l);
        l.cookieFileId = ReadAt<96, u32>(r, 72);
        out.push_back(l);
    });
    ForActiveRuns(g.spotAnimSets, g.spotLightAnims, activeSets, [&](const SpotLightAnim& r) {
        GatheredLight l = PointRecord(r, S::Mos2);
        SpotFields(r, l);
        FlickerAt(r, 56, l);
        l.cookieFileId = ReadAt<108, u32>(r, 84);
        out.push_back(l);
    });

    const std::vector<NewLight>& lights = model.root.newLights;
    for (const u16 ref : g.newLightRefs) {
        if (lights.empty())
            break;
        const NewLight& n = lights[ref < lights.size() ? ref : 0];
        const u32 flags = static_cast<u32>(n.flags);
        if (!SetActive(static_cast<u16>(n.doodadSet), activeSets) || ((flags & 4u) && !rtShadows) ||
            n.type < 0 || n.type > 1)
            continue;
        GatheredLight l;
        l.source = S::Mnld;
        l.id = static_cast<u32>(n.lightIndex);
        l.spot = n.type == 1;
        l.position = n.position;
        l.rotation = n.rotation;
        l.flags = flags & 0xFu;
        l.colorA = n.innerColor;
        l.attenuationStart = n.attenuationStart;
        l.attenuationEnd = n.attenuationEnd;
        l.intensity = n.intensity;
        const bool blend = (flags & 1u) != 0;
        l.colorB = blend ? n.outerColor : n.innerColor;
        l.blendNear = blend ? n.blendStart : n.attenuationStart;
        l.blendFar = blend ? n.blendEnd : n.attenuationEnd;
        l.flickerAmount = n.flickerIntensity;
        l.flickerSpeed = n.flickerSpeed;
        l.flickerMode = static_cast<i16>(n.flickerMode);
        l.cookieFileId = n.cookieFileId;
        l.falloff = n.falloff;
        l.innerAngle = n.innerAngle;
        l.outerAngle = n.outerAngle;
        out.push_back(l);
    }
    return out;
}

Vector3f lightDirection(const Vector3f& rotation) {
    const f32 sx = std::sin(rotation.x), cx = std::cos(rotation.x);
    const f32 sy = std::sin(rotation.y), cy = std::cos(rotation.y);
    const f32 sz = std::sin(rotation.z), cz = std::cos(rotation.z);
    return {cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy};
}

std::vector<Batch> shadowBatches(const Root& root, const Group& group) {
    const bool keepFile = ((root.header.flags & 0x10u) && (group.header.flags & 0x400u)) ||
                          (group.header.flags2 & 0x20u);
    if (keepFile)
        return group.shadowBatches;
    std::vector<Batch> out;
    i64 key = -1;
    for (u32 i = 0; i < group.batches.size(); ++i) {
        const Batch& b = group.batches[i];
        const u32 mat = b.material();
        if (mat >= root.materials.size())
            break;
        const Material& m = root.materials[mat];
        if ((m.flags & 0x100u) || m.blendMode > 1)
            break;
        const i64 k = static_cast<i64>(m.blendMode | ((m.flags & 4u) << 8)) |
                      (m.blendMode == 1 ? static_cast<i64>(i) << 16 : 0);
        const u32 first = b.firstVertex(), last = b.lastVertex();
        if (k == key && static_cast<u32>(out.back().indexCount) + b.indexCount <= 0xFFFFu) {
            Batch& r = out.back();
            r.indexCount = static_cast<u16>(r.indexCount + b.indexCount);
            const u32 lo = std::min(r.firstVertex(), first), hi = std::max(r.lastVertex(), last);
            r.head[0] = static_cast<u8>(lo >> 16);
            r.head[1] = static_cast<u8>(hi >> 16);
            r.minIndex = static_cast<u16>(lo);
            r.maxIndex = static_cast<u16>(hi);
            continue;
        }
        Batch r;
        r.startIndex = b.startIndex;
        r.indexCount = b.indexCount;
        r.head[0] = static_cast<u8>(first >> 16);
        r.head[1] = static_cast<u8>(last >> 16);
        r.minIndex = static_cast<u16>(first);
        r.maxIndex = static_cast<u16>(last);
        r.materialLarge = static_cast<u16>(mat);
        r.flags = m.blendMode == 1 ? 0x06 : 0x86;
        out.push_back(r);
        key = k;
    }
    return out;
}

std::vector<PlacedDoodad> placedDoodads(const Model& model, std::span<const u16> activeSets) {
    const Root& root = model.root;
    std::vector<PlacedDoodad> out;
    std::vector<i32> slot(root.doodadDefs.size(), -1);
    for (u32 g = 0; g < model.groups.size(); ++g) {
        if (!model.groups[g])
            continue;
        const bool interior = g < root.groups.size() && (root.groups[g].flags & 0x48u) == 0;
        for (u32 d : activeDoodads(root, *model.groups[g], activeSets)) {
            if (slot[d] < 0) {
                slot[d] = static_cast<i32>(out.size());
                out.push_back({d, doodadSetOf(root, d), g, interior});
            } else if (!interior) {
                // One exterior group makes it exterior for good.
                out[static_cast<size_t>(slot[d])].interior = false;
            }
        }
    }
    return out;
}

namespace {

// sub_14367A040: the index of the largest magnitude, the later one on a tie.
u32 LargestIndex(const f32 v[3]) {
    const f32 a = std::fabs(v[0]), b = std::fabs(v[1]), c = std::fabs(v[2]);
    if (a <= b)
        return b <= c ? 2u : 1u;
    return a <= c ? 2u : 0u;
}

constexpr f32 kByteToUnit = 0.0039215689f;

struct Hsv {
    f32 h = -1.0f;
    f32 s = 0.0f;
    f32 v = 0.0f;
};

// Color_RGBToHSV (0x143689120). A hue of -1 is "none".
Hsv RgbToHsv(const f32 rgb[3]) {
    Hsv o;
    for (u32 i = 0; i < 3; ++i) {
        if (rgb[i] < 0.0f || rgb[i] > 1.0f)
            return o;
    }
    const u32 hi = LargestIndex(rgb);
    const f32 lo = std::min(rgb[0], std::min(rgb[1], rgb[2]));
    o.v = rgb[hi];
    if (o.v == 0.0f)
        return o;
    o.s = (rgb[hi] - lo) / rgb[hi];
    if (o.s == 0.0f)
        return o;
    const f32 d = rgb[hi] - lo;
    f32 h;
    if (hi == 0)
        h = (rgb[1] - rgb[2]) / d;
    else if (hi == 1)
        h = (rgb[2] - rgb[0]) / d + 2.0f;
    else
        h = (rgb[0] - rgb[1]) / d + 4.0f;
    h *= 60.0f;
    o.h = h < 0.0f ? h + 360.0f : h;
    return o;
}

// Color_HSVToRGB (0x143688F60).
void HsvToRgb(const Hsv& c, f32 rgb[3]) {
    const auto fill = [&](f32 x) { rgb[0] = rgb[1] = rgb[2] = x; };
    const bool none = c.h == -1.0f && c.s == 0.0f;
    if (c.h < 0.0f || c.h > 360.0f)
        return fill(none ? c.v : 0.0f);
    if (c.s < 0.0f || c.s > 1.0f)
        return fill(0.0f);
    if (c.v < 0.0f || c.v > 1.0f)
        return fill(none ? c.v : 0.0f);
    if (c.s == 0.0f)
        return fill(c.v);
    const f32 x = c.h * 0.016666668f;
    const i32 i = std::min(static_cast<i32>(x), 5);
    const f32 v = c.v;
    const f32 p = (1.0f - c.s) * v;
    const f32 f = x - static_cast<f32>(i);
    const f32 q = (1.0f - f * c.s) * v;
    const f32 t = (1.0f - (1.0f - f) * c.s) * v;
    const f32 rows[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    for (u32 k = 0; k < 3; ++k)
        rgb[k] = rows[i][k];
}

u8 MaxChannel(Color c) {
    return std::max(c.b, std::max(c.r, c.g));
}

// The light's direction at the box's centre from @p from, when the two are apart.
void AimFrom(const Vector3f& from, const Vector3f& lo, const Vector3f& hi, Vector3f& dir) {
    const Vector3f v{(lo.x + hi.x) * 0.5f - from.x, (lo.y + hi.y) * 0.5f - from.y,
                     (lo.z + hi.z) * 0.5f - from.z};
    const f32 len2 = (v.y * v.y + v.x * v.x) + v.z * v.z;
    if (len2 > 0.000099999997f) {
        const f32 inv = 1.0f / std::sqrt(len2);
        dir = {inv * v.x, inv * v.y, inv * v.z};
    }
}

// The doodad record's runtime flags from MODD's (`CMapObj_CreateDoodadDef`):
// +72 and +76.
constexpr u32 kWmoLit = 0x10u, kFromMolt = 0x1000000u, kFromColor = 0x2000000u;
constexpr u32 kAddColor = 0x1u, kMdal = 0x2u, kBlackDirect = 0x100u, kNoColorAdd = 0x200u;

} // namespace

u8 colorByte(f32 value) {
    const f32 y = value + 512.0f;
    u32 bits;
    std::memcpy(&bits, &y, sizeof(bits));
    return static_cast<u8>(bits >> 14);
}

Color brightenDirect(Color c) {
    const u8 hi = MaxChannel(c);
    if (hi >= 112)
        return c;
    f32 rgb[3] = {static_cast<f32>(c.r) * kByteToUnit, static_cast<f32>(c.g) * kByteToUnit,
                  static_cast<f32>(c.b) * kByteToUnit};
    Hsv hsv = RgbToHsv(rgb);
    hsv.v = hsv.v * (112.0f / static_cast<f32>(hi == 0 ? 1 : hi));
    HsvToRgb(hsv, rgb);
    Color o;
    o.r = static_cast<u8>(static_cast<i32>(rgb[0] * 255.0f));
    o.g = static_cast<u8>(static_cast<i32>(rgb[1] * 255.0f));
    o.b = static_cast<u8>(static_cast<i32>(rgb[2] * 255.0f));
    o.a = 0xFF;
    return o;
}

Color clampAmbient(Color c) {
    const u8 hi = MaxChannel(c);
    if (hi <= 0x60)
        return c;
    const i32 k = static_cast<i32>(24480.0f / static_cast<f32>(hi));
    const auto scale = [k](u8 ch) { return static_cast<u8>(static_cast<u16>(k * ch + 255) >> 8); };
    c.r = scale(c.r);
    c.g = scale(c.g);
    c.b = scale(c.b);
    return c;
}

Color addColorSaturate(Color out, const Vector3f& add, Color ambient) {
    f32 v[3] = {static_cast<f32>(ambient.r) * kByteToUnit + add.x,
                static_cast<f32>(ambient.g) * kByteToUnit + add.y,
                static_cast<f32>(ambient.b) * kByteToUnit + add.z};
    const f32 hi = v[LargestIndex(v)];
    if (hi > 1.0f) {
        const f32 inv = 1.0f / hi;
        for (f32& x : v)
            x = x * inv;
    }
    out.r = colorByte(std::min(v[0], 1.0f) * 255.0f);
    out.g = colorByte(std::min(v[1], 1.0f) * 255.0f);
    out.b = colorByte(std::min(v[2], 1.0f) * 255.0f);
    return out;
}

DoodadLighting doodadLighting(const Root& root, u32 index, const Group& group,
                              const Vector3f& boundsMin, const Vector3f& boundsMax,
                              const std::array<Color, 3>& ambient) {
    DoodadLighting out;
    if (root.doodadDefs.empty())
        return out;
    const u32 d = index < root.doodadDefs.size() ? index : 0;
    const DoodadDef& def = root.doodadDefs[d];
    const u8 m = def.flags();
    u32 f72 = ((m & 0x02) ? kWmoLit : 0u) | ((m & 0x04) ? kFromMolt : 0u) |
              ((m & 0x08) ? kFromColor : 0u);
    u32 f76 = ((m & 0x10) ? kAddColor : 0u) | ((m & 0x40) ? kBlackDirect : 0u) |
              ((m & 0x80) ? kNoColorAdd : 0u);
    // +152: MODD's colour, which a MOLT light overwrites below.
    Color color = def.color;
    // +304: MDDI's, or zero without the chunk.
    const auto& mults = root.doodadColorMultipliers;
    const f32 mddi = mults.empty() ? 0.0f : mults[index < mults.size() ? index : 0];
    const auto molt = [&](u8 a) -> const Light* {
        return root.lights.empty() ? nullptr : &root.lights[a < root.lights.size() ? a : 0];
    };

    // CMapObj_ComputeDoodadLighting.
    if (f76 & kBlackDirect)
        f72 |= kFromMolt;
    if (f72 & kFromColor) {
        const Color c{color.b, color.g, color.r, 0};
        out.sky = out.horizon = out.ground = c;
    }
    if (f72 & kFromMolt) {
        const u8 a = color.a;
        if (f76 & kBlackDirect) {
            color = Color{0, 0, 0, a};
            out.direct = color;
        } else if (a != 0xFF) {
            if (const Light* l = molt(a)) {
                AimFrom(l->position, boundsMin, boundsMax, out.direction);
                f32 c[3] = {static_cast<f32>(l->color.r) * kByteToUnit * l->intensity,
                            static_cast<f32>(l->color.g) * kByteToUnit * l->intensity,
                            static_cast<f32>(l->color.b) * kByteToUnit * l->intensity};
                const f32 hi = c[LargestIndex(c)];
                if (hi > 1.0f) {
                    for (f32& x : c)
                        x = std::min((1.0f / hi) * x, 1.0f);
                }
                color.r = colorByte(c[0] * 255.0f);
                color.g = colorByte(c[1] * 255.0f);
                color.b = colorByte(c[2] * 255.0f);
                out.direct = (f72 & kWmoLit) ? color : brightenDirect(color);
            }
        }
    }
    if (f72 & kWmoLit) {
        const Light* l = color.a == 0xFF ? nullptr : molt(color.a);
        const Box& b = group.header.bounds;
        const Vector3f from =
            l ? l->position
              : Vector3f{(b.minimum.x + b.maximum.x) * 0.5f, (b.minimum.y + b.maximum.y) * 0.5f,
                         (b.minimum.z + b.maximum.z) * 0.5f};
        if (!(f72 & kFromColor) && group.ambientOverride) {
            f76 |= kMdal;
            out.sky = out.horizon = out.ground = *group.ambientOverride;
        }
        AimFrom(from, boundsMin, boundsMax, out.direction);
    }

    // CMapObjDef_DoodadAdjustLighting.
    if ((f72 & (kFromMolt | kFromColor)) == (kFromMolt | kFromColor))
        return out;
    const f32 mult = std::max(mddi, 1.0f);
    const Vector3f add = (f76 & kNoColorAdd)
                             ? Vector3f{0.0f, 0.0f, 0.0f}
                             : Vector3f{static_cast<f32>(color.r) * kByteToUnit * mult,
                                        static_cast<f32>(color.g) * kByteToUnit * mult,
                                        static_cast<f32>(color.b) * kByteToUnit * mult};
    Color direct = color;
    std::array<Color, 3> amb{};
    if (f72 & kWmoLit) {
        if (!(f76 & kMdal)) {
            amb = ambient;
            if ((f76 & kAddColor) && !(f72 & kFromMolt))
                direct = addColorSaturate(direct, add, ambient[0]);
        }
    } else {
        if (f76 & kAddColor) {
            for (u32 i = 0; i < 3; ++i)
                amb[i] = addColorSaturate(Color{}, add, ambient[i]);
            direct = amb[0];
        } else {
            amb = {color, color, color};
        }
        direct = brightenDirect(direct);
        for (Color& c : amb)
            c = clampAmbient(c);
    }
    if (!(f72 & kFromMolt))
        out.direct = direct;
    if (!(f72 & kFromColor) && !(f76 & kMdal)) {
        out.sky = amb[0];
        out.horizon = amb[1];
        out.ground = amb[2];
    }
    return out;
}

// ---- Detail doodads (MDDL) ------------------------------------------------

namespace {

// CRandom's table (0x143838A90).
constexpr std::array<u32, 61> kRandomTable = {
    0x9927148E, 0x08C7AAFD, 0x1F3EE6D5, 0xDA55BBF6, 0x6A4AA075, 0xFF97BDE8, 0x9FBC9BDE, 0x46A18A81,
    0x63E30B6E, 0x5D6C7A76, 0xCA69D388, 0x25B947C3, 0x3FA2AB83, 0xBA7C41A6, 0x0195ACE5, 0xC109CF7E,
    0x717062D9, 0x0205DB8D, 0x54EF8724, 0x3037D4C6, 0x7BCB1BD0, 0xECD8E4B8, 0xDCADCE49, 0xC494A913,
    0x0DAE398F, 0x0EDD5218, 0x85F5FA78, 0x6DAFD258, 0x3B53B2A4, 0xBE50A551, 0x11F42DFC, 0xF1169848,
    0x663DDF86, 0x2F2E445E, 0x176B0736, 0xB64C298B, 0xE75F89E2, 0xE121A7CD, 0xED65C94D, 0x239CEEFE,
    0x04B77D33, 0x402A9A9E, 0xF35B10B3, 0x921C7782, 0x571E4E20, 0x8C067222, 0xFB732C67, 0xBF0AC259,
    0x0CF95C79, 0x68121A28, 0x42193474, 0xF884C0B1, 0x9D15F038, 0x6F3AF260, 0x91EB90B4, 0x61357F1D,
    0x5603325A, 0x932BC5A3, 0x434B0F80, 0x3CE0A8F7, 0x2664D196,
};

// cvttss2si / cvttsd2si: truncation, and 0x80000000 where the value has no int.
i32 TruncateToInt(f64 v) {
    if (!(v >= -2147483648.0 && v < 2147483648.0))
        return std::numeric_limits<i32>::min();
    return static_cast<i32>(v);
}

i64 TruncateToInt64(f64 v) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        return std::numeric_limits<i64>::min();
    return static_cast<i64>(v);
}

// An instance byte: the low byte of the truncated value.
u8 ByteOf(f32 v) {
    return static_cast<u8>(static_cast<u32>(TruncateToInt(v)));
}

f32 Rand01(u32 r) {
    return std::bit_cast<f32>(0x3F800000u | (r & 0x7FFFFFu)) - 1.0f;
}

// `CMapObjDetailDoodadLoc_Hash` (0x14368E0E0): xorshift of (u, v).
u32 LocHash(const DetailDoodadLoc& loc) {
    u32 x = static_cast<u32>(loc.u) | (static_cast<u32>(loc.v) << 16);
    x ^= x << 13;
    x ^= x >> 17;
    return x ^ (x << 5);
}

bool LocLess(const DetailDoodadLoc& a, const DetailDoodadLoc& b) {
    if (a.id != b.id)
        return a.id < b.id;
    return LocHash(a) < LocHash(b);
}

// 0x141B16A30: the middle of three, by index.
i64 Median3(const std::vector<DetailDoodadLoc>& v, i64 a, i64 b, i64 c) {
    if (LocLess(v[a], v[b])) {
        if (LocLess(v[b], v[c]))
            return b;
        return LocLess(v[a], v[c]) ? c : a;
    }
    if (LocLess(v[a], v[c]))
        return a;
    return LocLess(v[b], v[c]) ? c : b;
}

// 0x141B16B20, then 0x141B16D00: build the heap, then pop it.
void HeapSortLocs(std::vector<DetailDoodadLoc>& v, i64 first, i64 last) {
    const auto at = [&](i64 i) -> DetailDoodadLoc& { return v[first + i]; };
    const auto sift = [&](i64 hole, i64 top, i64 n, const DetailDoodadLoc& value) {
        i64 child = 2 * hole + 2;
        while (child < n) {
            if (LocLess(at(child), at(child - 1)))
                --child;
            at(hole) = at(child);
            hole = child;
            child = 2 * child + 2;
        }
        if (child == n) {
            at(hole) = at(n - 1);
            hole = n - 1;
        }
        while (hole > top) {
            const i64 parent = (hole - 1) >> 1;
            if (!LocLess(at(parent), value))
                break;
            at(hole) = at(parent);
            hole = parent;
        }
        at(hole) = value;
    };
    const i64 n = last - first;
    if (n >= 2) {
        for (i64 start = (n >> 1) - 1;; --start) {
            sift(start, start, n, DetailDoodadLoc(at(start)));
            if (start == 0)
                break;
        }
    }
    for (i64 m = n - 1; m >= 1; --m) {
        const DetailDoodadLoc value = at(m);
        at(m) = at(0);
        sift(0, 0, m, value);
    }
}

// `Sort_DetailDoodadLocs` (0x141B16080): a ninther pivot at the back, a
// three-way partition whose equal runs move beside the pivot one short on each
// side, the left part recursed and the right looped, insertion under 33 and a
// heap sort once the halving budget runs out. Ties keep this order exactly.
void SortLocs(std::vector<DetailDoodadLoc>& v, i64 first, i64 last, i64 ideal) {
    i64 cur = first;
    while (last - cur >= 33 && ideal > 0) {
        const i64 count = last - cur;
        const i64 mid = cur + count / 2;
        const i64 step = count / 8;
        const i64 p = Median3(v, Median3(v, cur, cur + step, cur + 2 * step),
                              Median3(v, mid - step, mid, mid + step),
                              Median3(v, last - 1 - 2 * step, last - 1 - step, last - 1));
        std::swap(v[p], v[last - 1]);
        const i64 pivot = last - 1;
        i64 i = cur;
        i64 j = last - 1;
        i64 leftEqual = cur;
        i64 rightEqual = last - 1;
        for (;;) {
            while (LocLess(v[i], v[pivot]))
                ++i;
            do {
                --j;
            } while (LocLess(v[pivot], v[j]) && j != cur);
            if (i >= j)
                break;
            std::swap(v[i], v[j]);
            if (!LocLess(v[i], v[pivot]) && !LocLess(v[pivot], v[i]))
                std::swap(v[leftEqual++], v[i]);
            if (!LocLess(v[pivot], v[j]) && !LocLess(v[j], v[pivot]))
                std::swap(v[j], v[--rightEqual]);
            ++i;
        }
        std::swap(v[i], v[pivot]);
        i64 leftEnd = i - 1;
        for (i64 k = cur; k + 1 < leftEqual; ++k, --leftEnd)
            std::swap(v[k], v[leftEnd]);
        i64 rightBegin = i + 1;
        for (i64 k = last - 2; k > rightEqual; --k, ++rightBegin)
            std::swap(v[rightBegin], v[k]);
        ideal >>= 1;
        SortLocs(v, cur, leftEnd + 1, ideal);
        cur = rightBegin;
    }
    if (last - cur < 33) {
        for (i64 k = cur + 1; k < last; ++k) {
            const DetailDoodadLoc value = v[k];
            i64 hole = k;
            while (hole != cur && LocLess(value, v[hole - 1])) {
                v[hole] = v[hole - 1];
                --hole;
            }
            v[hole] = value;
        }
        return;
    }
    HeapSortLocs(v, cur, last);
}

struct Quaternion {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f;
};

// 0x14367F210.
Quaternion AxisAngle(const std::array<f32, 3>& axis, f32 angle) {
    const f32 half = angle * 0.5f;
    const f32 s = std::sin(half);
    return {s * axis[0], s * axis[1], s * axis[2], std::cos(half)};
}

} // namespace

CRandom::CRandom(u32 seed)
    : acc_(seed), index_(((seed % 47u) * 4u) << 24 | ((seed % 53u) * 4u) << 16 |
                         ((seed % 59u) * 4u) << 8 | (seed % 61u) * 4u) {}

u32 CRandom::next() {
    const u32 b3 = index_ >> 24;
    const u32 b2 = (index_ >> 16) & 0xFFu;
    const u32 b1 = (index_ >> 8) & 0xFFu;
    const u32 b0 = index_ & 0xFFu;
    const u32 o3 = b3 >= 4u ? b3 - 4u : b3 + 184u;
    const u32 o2 = b2 >= 12u ? b2 - 12u : b2 + 200u;
    const u32 o1 = b1 >= 24u ? b1 - 24u : b1 + 212u;
    const u32 o0 = b0 >= 28u ? b0 - 28u : b0 + 216u;
    index_ = o3 << 24 | o2 << 16 | o1 << 8 | o0;
    acc_ += kRandomTable[o0 / 4u] ^ std::rotr(kRandomTable[o1 / 4u], 29) ^
            std::rotr(kRandomTable[o2 / 4u], 30) ^ std::rotr(kRandomTable[o3 / 4u], 31);
    return acc_;
}

std::optional<DetailDoodadData> parseDetailDoodads(const Root& root) {
    const std::vector<u8>& d = root.detailDoodads;
    std::size_t at = 0;
    const auto have = [&](std::size_t n) { return at + n <= d.size(); };
    const auto read16 = [&](std::size_t o) { return static_cast<u32>(d[o] | d[o + 1] << 8); };
    const auto read32 = [&](std::size_t o) {
        u32 v = 0;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    };
    if (!have(2))
        return std::nullopt;
    u32 version = 0;
    if (read16(0) == 0xFFDDu) {
        if (!have(4))
            return std::nullopt;
        version = read16(2);
        if (version > 1)
            return std::nullopt;
        at = 4;
    }
    DetailDoodadData out;
    if (!have(4))
        return std::nullopt;
    out.density = std::bit_cast<f32>(read32(at));
    at += 4;
    if (std::isnan(out.density))
        return std::nullopt;
    if (version != 0) {
        if (!have(1))
            return std::nullopt;
        out.flags = d[at++];
    }
    if (!have(2))
        return std::nullopt;
    const u32 layerCount = read16(at);
    at += 2;
    if (layerCount == 0)
        return std::nullopt;
    out.layers.resize(layerCount);
    for (DetailDoodadLayer& layer : out.layers) {
        if (!have(2))
            return std::nullopt;
        layer.threshold = d[at];
        const u32 n = d[at + 1];
        at += 2;
        for (u32 e = 0; e < n; ++e) {
            if (!have(5))
                return std::nullopt;
            layer.entries.push_back({read32(at), d[at + 4]});
            layer.totalWeight += d[at + 4];
            at += 5;
        }
    }
    const u32 groupCount = root.groupCount();
    out.blocks.assign(groupCount, -1);
    while (at < d.size()) {
        if (!have(6))
            return std::nullopt;
        const u32 g = read16(at);
        if (g >= groupCount)
            return std::nullopt;
        out.blocks[g] = static_cast<i64>(at + 6);
        at += 6 + static_cast<std::size_t>(read32(at + 2));
    }
    return out;
}

std::vector<DetailDoodadLoc> detailDoodadLocs(const Root& root, const DetailDoodadData& data,
                                              const Group& group, u32 groupIndex) {
    std::vector<DetailDoodadLoc> out;
    // 12.1 reads the u16 MOVI alone, which a MOVX group does not have.
    if (groupIndex >= data.blocks.size() || data.blocks[groupIndex] < 0 || group.wideIndices)
        return out;
    const u32 key = (group.header.flags2 & 0x200u) ? group.detailDoodadKey : (groupIndex & 0xFFFFu);
    const auto vertex = [&](u64 i) -> Vector3f {
        if (i >= group.indices.size() || group.indices[i] >= group.positions.size())
            return {};
        return group.positions[group.indices[i]];
    };

    // 1. Triangle areas (`CMapObjGroup_InitDetailDoodadData` 0x141A80640).
    struct Range {
        u32 start = 0;
        u32 count = 0;
    };
    struct Slots {
        u32 firstTriangle = 0;
        std::vector<f32> areas;
        std::vector<Range> ranges;
    };
    std::vector<Slots> batches(group.batches.size());
    for (std::size_t b = 0; b < batches.size(); ++b) {
        const Batch& batch = group.batches[b];
        Slots& s = batches[b];
        s.firstTriangle = batch.startIndex / 3u;
        s.areas.resize(batch.indexCount / 3u);
        for (u32 t = 0; t < s.areas.size(); ++t) {
            const u64 first = static_cast<u64>(batch.startIndex) + 3u * t;
            const Vector3f a = vertex(first), b1 = vertex(first + 1), c = vertex(first + 2);
            const f32 e1z = b1.z - a.z, e1y = b1.y - a.y, e1x = b1.x - a.x;
            const f32 e2y = c.y - a.y, e2z = c.z - a.z, e2x = c.x - a.x;
            const f32 cx = e1y * e2z - e1z * e2y;
            const f32 cy = e1z * e2x - e1x * e2z;
            const f32 cz = e1x * e2y - e1y * e2x;
            s.areas[t] = std::sqrt((cx * cx + cy * cy) + cz * cz) * 0.5f;
        }
    }

    // 2. Slot counts (`CMapObjDetailDoodadData_DistributeCounts` 0x14368D990):
    //    the area's expected count, its fraction kept by a draw.
    constexpr f32 kSlotsPerArea = std::bit_cast<f32>(0x40B0F27Cu); // 5.5296
    const f32 density2 = data.density * data.density;
    u32 total = 0;
    for (u32 b = 0; b < batches.size(); ++b) {
        Slots& s = batches[b];
        CRandom rng((key << 16) | b);
        s.ranges.resize(s.areas.size());
        for (u32 t = 0; t < s.areas.size(); ++t) {
            const f64 e = static_cast<f64>((s.areas[t] * kSlotsPerArea) * density2);
            const u32 whole = static_cast<u32>(TruncateToInt64(e));
            const u32 r = rng.next();
            const u32 count = (e - static_cast<f64>(whole)) * 4294967295.0 > static_cast<f64>(r)
                                  ? whole + 1u
                                  : whole;
            s.ranges[t] = {total, count};
            total += count;
        }
    }

    // 3. The layers (`CMapObjDetailDoodadData_PlaceLayers` 0x14368C590): each
    //    names batches, whole or by listed triangles and slots.
    struct Triangle {
        u32 absolute;
        CRandom rng;
        u32 last;
    };
    const auto begin = [&](const Slots& s, u32 t) {
        const u32 bits = std::bit_cast<u32>(s.areas[t]) & 0xFFFFu;
        u32 seed = static_cast<u8>(s.ranges[t].count) + (((key << 16) + bits) << 8);
        if (seed == 0)
            seed = 1;
        return Triangle{s.firstTriangle + t, CRandom(seed), s.ranges[t].start - 1u};
    };
    // `DetailDoodadTriRng_GetLoc` (0x14368DCA0): two draws per skipped slot.
    const auto place = [&](const DetailDoodadLayer& layer, Triangle& tri, u32 id) {
        const u32 next = tri.last + 1u;
        if (next < id) {
            for (u32 k = id - next; k != 0; --k) {
                tri.rng.next();
                tri.rng.next();
            }
        }
        u16 u = static_cast<u16>(tri.rng.next());
        u16 v = static_cast<u16>(tri.rng.next());
        tri.last = id;
        if (static_cast<u32>(u) + v > 0xFFFFu) {
            u = static_cast<u16>(~u);
            v = static_cast<u16>(~v);
        }
        CRandom pick(static_cast<u8>(tri.absolute) + (static_cast<u32>(static_cast<u8>(u)) << 8) +
                     (static_cast<u32>(static_cast<u8>(v)) << 16) + (key << 24));
        const u32 r = pick.next();
        // A zero total divides by zero in 12.1.
        if (layer.threshold < (r >> 20) % 24u || layer.totalWeight == 0)
            return;
        const u32 w = r % layer.totalWeight;
        u32 low = 0;
        for (const DetailDoodadLayer::Entry& e : layer.entries) {
            if (w >= low && w < low + e.weight) {
                if (e.id != 0)
                    out.push_back({tri.absolute, u, v, e.id});
                return;
            }
            low += e.weight;
        }
    };
    const std::vector<u8>& bytes = root.detailDoodads;
    std::size_t at = static_cast<std::size_t>(data.blocks[groupIndex]);
    // Past the end reads as terminators, which ends every loop.
    const auto read8 = [&]() -> u32 { return at < bytes.size() ? bytes[at++] : 0xFFu; };
    const auto read16 = [&]() -> u32 {
        if (at + 2 > bytes.size()) {
            at = bytes.size();
            return 0xFFFFu;
        }
        const u32 v = bytes[at] | bytes[at + 1] << 8;
        at += 2;
        return v;
    };
    // A bad batch, triangle or slot stops the whole stream.
    [&] {
        for (u32 layerIndex = read16(); layerIndex != 0xFFFFu; layerIndex = read16()) {
            if (layerIndex >= data.layers.size())
                return;
            const DetailDoodadLayer& layer = data.layers[layerIndex];
            for (u32 word = read16(); word != 0xFFFFu; word = read16()) {
                const u32 b = word & 0x7FFFu;
                if (b >= batches.size())
                    return;
                const Slots& s = batches[b];
                if (word & 0x8000u) {
                    for (u32 t = 0; t < s.ranges.size(); ++t) {
                        Triangle tri = begin(s, t);
                        for (u32 j = 0; j < s.ranges[t].count; ++j)
                            place(layer, tri, s.ranges[t].start + j);
                    }
                    continue;
                }
                u32 triangleBase = 0;
                for (u32 c = read8(); c != 0xFFu; c = read8()) {
                    if (c == 0x7Fu) {
                        triangleBase += 127;
                        continue;
                    }
                    const u32 t = triangleBase + (c & 0x7Fu);
                    if (t >= s.ranges.size())
                        return;
                    Triangle tri = begin(s, t);
                    const Range& range = s.ranges[t];
                    // Bit 7: the listed slots; else all but them.
                    const bool listed = (c & 0x80u) != 0;
                    u32 slotBase = 0;
                    u32 next = 0;
                    for (u32 k = read8(); k != 0xFFu; k = read8()) {
                        if (k == 0xFEu) {
                            slotBase += 254;
                            continue;
                        }
                        const u32 i = slotBase + k;
                        if (i >= range.count)
                            return;
                        if (listed) {
                            place(layer, tri, range.start + i);
                        } else {
                            for (u32 j = next; j < i; ++j)
                                place(layer, tri, range.start + j);
                        }
                        next = i + 1;
                    }
                    if (!listed) {
                        for (u32 j = next; j < range.count; ++j)
                            place(layer, tri, range.start + j);
                    }
                }
            }
        }
    }();

    // 4. Their order.
    SortLocs(out, 0, static_cast<i64>(out.size()), static_cast<i64>(out.size()));
    return out;
}

f32 capDetailDoodads(std::vector<DetailDoodadLoc>& locs, f32 density, std::optional<f32> maxScale) {
    if (!maxScale || !(*maxScale < density))
        return density;
    const f64 ratio = static_cast<f32>(*maxScale / density);
    const f64 keep = static_cast<f64>(static_cast<i32>(locs.size())) * ratio * ratio;
    const u64 n = static_cast<u32>(TruncateToInt(keep));
    if (n < locs.size())
        locs.resize(static_cast<std::size_t>(n));
    return *maxScale;
}

std::optional<DetailDoodadInstance> detailDoodadInstance(const Group& group, u32 groupIndex,
                                                         std::span<const Color> colorSet0,
                                                         const DetailDoodadLoc& loc,
                                                         const DetailDoodadModel& model) {
    if (loc.triangle + 2u * (loc.triangle + 1u) >= group.indices.size())
        return std::nullopt;
    const u32 i0 = group.indices[3u * loc.triangle];
    const u32 i1 = group.indices[3u * loc.triangle + 1u];
    const u32 i2 = group.indices[3u * loc.triangle + 2u];
    if (i0 >= group.positions.size() || i1 >= group.positions.size() ||
        i2 >= group.positions.size())
        return std::nullopt;
    const Vector3f& p0 = group.positions[i0];
    const Vector3f& p1 = group.positions[i1];
    const Vector3f& p2 = group.positions[i2];

    constexpr f32 kTwoPi = std::bit_cast<f32>(0x40C90FDBu);
    constexpr f32 kPi = std::bit_cast<f32>(0x40490FDBu);
    constexpr f32 kDegreesToRadians = std::bit_cast<f32>(0x3C8EFA35u);
    constexpr f32 kInvU16 = std::bit_cast<f32>(0x37800080u);
    constexpr f32 kInvU8 = std::bit_cast<f32>(0x3B808081u);
    constexpr f32 kAngleToByte = std::bit_cast<f32>(0x4222568Au); // 255/2π
    constexpr f32 kSignToByte = std::bit_cast<f32>(0x437F0001u);
    const f32 yawBase = (kTwoPi - model.yawMax * kDegreesToRadians) + kPi;
    const f32 yawRange = ((kTwoPi - model.yawMin * kDegreesToRadians) + kPi) - yawBase;
    const f32 scaleRange = model.maxScale - model.minScale;

    // Seeded by the group INDEX, where the generation used the key.
    CRandom rng((groupIndex << 24) + static_cast<u8>(loc.triangle) +
                (static_cast<u32>(static_cast<u8>(loc.u)) << 8) +
                (static_cast<u32>(static_cast<u8>(loc.v)) << 16));
    const f32 scale = Rand01(rng.next()) * scaleRange + model.minScale;
    const u32 signBits = rng.next();
    const f32 mantissa = std::bit_cast<f32>(0x3F800000u | (signBits & 0x7FFFFFu));
    const f32 sign = static_cast<i32>(signBits) >= 0 ? mantissa - 2.0f : 2.0f - mantissa;
    const f32 yaw = Rand01(rng.next()) * yawRange + yawBase;

    const f32 wv = static_cast<f32>(loc.v) * kInvU16;
    const f32 wu = static_cast<f32>(loc.u) * kInvU16;
    const f32 wr =
        static_cast<f32>(0xFFFF - static_cast<i32>(loc.v) - static_cast<i32>(loc.u)) * kInvU16;

    DetailDoodadInstance out;
    out.position = {(wv * p1.x + wu * p0.x) + wr * p2.x, (wv * p1.y + wu * p0.y) + wr * p2.y,
                    (wv * p1.z + wu * p0.z) + wr * p2.z};

    const f32 ax = p1.x - p0.x, az = p1.z - p0.z, ay = p1.y - p0.y;
    const f32 bz = p2.z - p0.z, bx = p2.x - p0.x, by = p2.y - p0.y;
    f32 nx = ay * bz - az * by;
    f32 ny = az * bx - ax * bz;
    f32 nz = ax * by - ay * bx;
    const f32 inv = 1.0f / std::sqrt((nx * nx + ny * ny) + nz * nz);
    nx *= inv;
    ny *= inv;
    nz *= inv;
    out.normal = {ByteOf((nx + 1.0f) * 127.5f), ByteOf((ny + 1.0f) * 127.5f),
                  ByteOf((nz + 1.0f) * 127.5f)};
    const f32 amount = model.amount >= 0.0f ? std::min(model.amount, 1.0f) : 0.0f;
    out.amount = ByteOf(amount * 255.0f);
    out.fixed = {0x7F, 0x7F, 0x7F, 0xFF};

    out.color = {0x00, 0x00, 0x00, 0xFF};
    if (!colorSet0.empty()) {
        const auto unpack = [&](u32 i) -> std::array<f32, 4> {
            if (i >= colorSet0.size())
                return {};
            const Color& c = colorSet0[i];
            return {static_cast<f32>(c.r) * kInvU8, static_cast<f32>(c.g) * kInvU8,
                    static_cast<f32>(c.b) * kInvU8, static_cast<f32>(c.a) * kInvU8};
        };
        const std::array<f32, 4> c0 = unpack(i0), c1 = unpack(i1), c2 = unpack(i2);
        for (u32 k = 0; k < 4; ++k)
            out.color[k] = ByteOf(((c1[k] * wv + c0[k] * wu) + c2[k] * wr) * 255.0f);
    }

    f32 angle = yaw;
    out.axis = {0x7F, 0x7F, 0xFF};
    if (model.alignToNormal) {
        // Tilt onto the normal about (-n.y, n.x, 0), after the yaw about +Z.
        const f32 tx = -ny;
        const f32 tilt2 = tx * tx + nx * nx;
        std::array<f32, 3> tiltAxis{0.0f, 0.0f, 1.0f};
        f32 tiltAngle = 0.0f;
        if (tilt2 >= std::bit_cast<f32>(0x34800000u)) {
            const f32 s = 1.0f / std::sqrt(tilt2);
            tiltAxis = {s * tx, s * nx, 0.0f};
            tiltAngle = std::acos(nz >= -1.0f ? std::min(nz, 1.0f) : -1.0f);
        }
        const Quaternion a = AxisAngle(tiltAxis, tiltAngle);
        const Quaternion b = AxisAngle({0.0f, 0.0f, 1.0f}, yaw);
        const Quaternion q = {((b.x * a.w + a.x * b.w) + b.z * a.y) - a.z * b.y,
                              ((a.w * b.y + b.w * a.y) + b.x * a.z) - a.x * b.z,
                              ((a.w * b.z + a.z * b.w) + a.x * b.y) - b.x * a.y,
                              ((a.w * b.w - a.x * b.x) - b.y * a.y) - b.z * a.z};
        // Back to axis and angle (0x14367FC30); w unclamped, as there.
        std::array<f32, 3> axis{1.0f, 0.0f, 0.0f};
        angle = 0.0f;
        const f32 len2 = (q.y * q.y + q.x * q.x) + q.z * q.z;
        if (len2 > 0.0f) {
            const f32 c = std::acos(q.w);
            angle = c + c;
            const f32 s = 1.0f / std::sqrt(len2);
            axis = {s * q.x, s * q.y, s * q.z};
        }
        out.axis = {ByteOf((axis[0] + 1.0f) * 127.5f), ByteOf((axis[1] + 1.0f) * 127.5f),
                    ByteOf((axis[2] + 1.0f) * 127.5f)};
    }
    out.angle = ByteOf(angle * kAngleToByte);
    out.scale = ByteOf(scale * 127.5f);
    out.sign = ByteOf(sign * kSignToByte);
    return out;
}

} // namespace wmo
} // namespace wow
} // namespace models
} // namespace whiteout
