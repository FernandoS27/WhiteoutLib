#include "whiteout/models/wow/wmo/cull.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace whiteout {
namespace models {
namespace wow {
namespace wmo {

namespace {

constexpr f32 kBoxTolerance = 0.019444443f; // 0x3C9F49F4
constexpr f32 kPortalBand = 0.01f;          // 0x3C23D70A
constexpr f32 kMinRect = 0.001f;
constexpr f32 kMinClipW = 1.0e-6f;
constexpr u32 kMaxDepth = 12;
constexpr u32 kMaxPortalVertices = 16;
constexpr u16 kNoGroup = 0xFFFF;

constexpr u32 kGroupExterior = 0x8;
constexpr u32 kGroupInterior = 0x2000;
constexpr u32 kGroupAlwaysDraw = 0x10000;
constexpr u32 kGroupAntiportal = 0x4000000;
constexpr u32 kGroupWindow = 0x20000000;
constexpr u32 kGroup2NoPortals = 0x100;
constexpr u32 kGroup2SplitParent = 0x40;
constexpr u32 kGroup2SplitChild = 0x80;

constexpr u32 kPortalCulled = 0x10;
constexpr u32 kPortalEyeOn = 0x20;
constexpr u32 kPortalClosed = 0x200;

f32 Dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector3f Lerp(const Vector3f& a, const Vector3f& b, f32 t) {
    // `(b - a)·t + a`, the client's order.
    return {(b.x - a.x) * t + a.x, (b.y - a.y) * t + a.y, (b.z - a.z) * t + a.z};
}

f32 Side(const Plane& p, const Vector3f& v) {
    return p.normal.x * v.x + p.normal.y * v.y + p.normal.z * v.z + p.distance;
}

// `plane(p0, p1, p2)`: n = (p1 − p0) × (p2 − p0), unit unless it is tiny.
Plane PlaneOf(const Vector3f& p0, const Vector3f& p1, const Vector3f& p2) {
    const Vector3f a = Sub(p1, p0), b = Sub(p2, p0);
    Vector3f n{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    const f32 len2 = Dot(n, n);
    if (len2 > 2.384186e-7f) {
        const f32 inv = 1.0f / std::sqrt(len2);
        n = {n.x * inv, n.y * inv, n.z * inv};
    }
    return {n, -Dot(n, p0)};
}

// The p-vertex: the corner furthest along @p n (the sign bit picks the minimum).
Vector3f FarCorner(const Box& b, const Vector3f& n) {
    return {std::signbit(n.x) ? b.minimum.x : b.maximum.x, std::signbit(n.y) ? b.minimum.y : b.maximum.y,
            std::signbit(n.z) ? b.minimum.z : b.maximum.z};
}

Vector3f NearCorner(const Box& b, const Vector3f& n) {
    return {std::signbit(n.x) ? b.maximum.x : b.minimum.x, std::signbit(n.y) ? b.maximum.y : b.minimum.y,
            std::signbit(n.z) ? b.maximum.z : b.minimum.z};
}

void Union(CullRect& r, const CullRect& o) {
    r.minX = std::min(r.minX, o.minX);
    r.minY = std::min(r.minY, o.minY);
    r.maxX = std::max(r.maxX, o.maxX);
    r.maxY = std::max(r.maxY, o.maxY);
}

CullRect To01(const CullRect& ndc) {
    return {(ndc.minX + 1.0f) * 0.5f, (ndc.minY + 1.0f) * 0.5f, (ndc.maxX + 1.0f) * 0.5f, (ndc.maxY + 1.0f) * 0.5f};
}

CullRect ToNdc(const CullRect& r01) {
    return {r01.minX * 2.0f - 1.0f, r01.minY * 2.0f - 1.0f, r01.maxX * 2.0f - 1.0f, r01.maxY * 2.0f - 1.0f};
}

const CullRect kFullNdc{-1.0f, -1.0f, 1.0f, 1.0f};

// `CFrustum_ClipPolygon5Planes` 0x141B8DA60: Sutherland–Hodgman against the
// top, bottom, left, right and far planes; the near one is left out.
void ClipPolygon(const CullFrustum& f, std::vector<Vector3f>& poly) {
    std::vector<Vector3f> out;
    for (u32 p = 0; p < 5 && poly.size() >= 3; ++p) {
        out.clear();
        const Plane& plane = f.planes[p];
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const Vector3f& a = poly[i];
            const Vector3f& b = poly[(i + 1) % poly.size()];
            const f32 da = Side(plane, a), db = Side(plane, b);
            if ((da < 0.0f) != (db < 0.0f)) {
                const f32 t = da / (da - db);
                out.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
            }
            if (db >= 0.0f)
                out.push_back(b);
        }
        poly.swap(out);
    }
}

// The NDC bounds of @p poly as projected, or the whole screen when a vertex
// is at or behind the eye's plane (`w < 1e-6`).
CullRect ProjectBounds(const std::vector<Vector3f>& poly, const Matrix4& toClip) {
    CullRect r;
    const auto& m = toClip.m;
    for (const Vector3f& v : poly) {
        const f32 x = v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0] + m[3][0];
        const f32 y = v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1] + m[3][1];
        const f32 w = v.x * m[0][3] + v.y * m[1][3] + v.z * m[2][3] + m[3][3];
        if (w < kMinClipW)
            return kFullNdc;
        r.minX = std::min(r.minX, x / w);
        r.maxX = std::max(r.maxX, x / w);
        r.minY = std::min(r.minY, y / w);
        r.maxY = std::max(r.maxY, y / w);
    }
    return r;
}

// `PointInPolygonMajorAxis` 0x143687500: the crossings test in the plane
// that drops @p n's largest axis (ties go to the later axis).
bool PointInPortal(const Vector3f& p, std::span<const Vector3f> poly, const Vector3f& n) {
    const f32 ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    const u32 drop = (ax > ay && ax > az) ? 0u : (ay > az ? 1u : 2u);
    const auto uv = [drop](const Vector3f& v) -> std::array<f32, 2> {
        if (drop == 0)
            return {v.y, v.z};
        if (drop == 1)
            return {v.x, v.z};
        return {v.x, v.y};
    };
    const auto q = uv(p);
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const auto a = uv(poly[i]), b = uv(poly[j]);
        if ((a[1] > q[1]) != (b[1] > q[1]) && q[0] < (b[0] - a[0]) * (q[1] - a[1]) / (b[1] - a[1]) + a[0])
            inside = !inside;
    }
    return inside;
}

f32 MopeFloat(u32 bits) {
    f32 f;
    static_assert(sizeof(f) == sizeof(bits));
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

struct Cull {
    const CullPlacement& placement;
    const Model& model;
    const CullView& view;
    u32 frame;
    Vector3f eyeLocal;
    CullFrustum full;   // the whole view
    CullFrustum active; // ctx+920: whichever frustum the pass works in
    bool interior = false;
    CullRect exterior01;
    std::vector<VisibleGroup> visible;
    std::vector<i32> visibleIndex; // per group, into `visible`

    const Group* Object(u32 g) const {
        if (g >= placement.currentLod.size())
            return nullptr;
        const u32 lod = placement.currentLod[g];
        if (lod == 0)
            return g < model.groups.size() && model.groups[g] ? &*model.groups[g] : nullptr;
        if (lod - 1 >= model.lodGroups.size() || g >= model.lodGroups[lod - 1].size())
            return nullptr;
        const auto& obj = model.lodGroups[lod - 1][g];
        return obj ? &*obj : nullptr;
    }

    u32 GroupFlags(u32 g) const {
        return g < model.root.groups.size() ? model.root.groups[g].flags : 0u;
    }

    u32 GroupFlags2(u32 g) const {
        return g < model.root.groups2.size() ? model.root.groups2[g].flags2 : 0u;
    }

    Box GroupBox(u32 g) const {
        return g < placement.groupBoxes.size() ? placement.groupBoxes[g] : Box{};
    }

    // `MapObjRender_AddVisGroupInst`: once a frame per group; a later visit
    // ORs its mask and grows its rect.
    void Add(u32 g, const CullRect& rect01, u32 mask, bool doodads) {
        if (!Object(g))
            return; // a placeholder draws nothing
        if (visibleIndex[g] >= 0) {
            VisibleGroup& e = visible[static_cast<std::size_t>(visibleIndex[g])];
            e.mask |= mask;
            Union(e.rect, rect01);
            return;
        }
        visibleIndex[g] = static_cast<i32>(visible.size());
        visible.push_back({g, rect01, mask, doodads});
        // An interior "window" group shows the outside.
        const u32 mogp = Object(g)->header.flags;
        if ((mogp & kGroupInterior) && (mogp & kGroupWindow))
            Union(exterior01, rect01);
    }

    std::vector<Vector3f> PortalPolygon(const Portal& p, const Vector3f& offset) const {
        std::vector<Vector3f> poly;
        const u32 n = std::min<u32>(p.vertexCount, kMaxPortalVertices);
        for (u32 i = 0; i < n; ++i) {
            const u32 v = p.startVertex + i;
            if (v >= model.root.portalVertices.size())
                break;
            const Vector3f& l = model.root.portalVertices[v];
            poly.push_back(transformPoint({l.x + offset.x, l.y + offset.y, l.z + offset.z}, placement.localToWorld));
        }
        return poly;
    }

    // `CWorldViewCull_TransformPortal` 0x141B83F20.
    void Transform(PortalRecord& rec, u32 portal, const PortalExtra* mope, u32 target) const {
        rec.flags &= 0xFu;
        rec.rect = CullRect{};
        const Portal& p = model.root.portals[portal];
        const f32 s = Side(p.plane, eyeLocal);
        bool near;
        if (mope)
            near = s >= 0.0f ? s < MopeFloat(mope->unknown[0]) + kPortalBand
                             : s > MopeFloat(mope->unknown[1]) - kPortalBand;
        else
            near = std::fabs(s) < kPortalBand;
        if (near) {
            const u32 first = p.startVertex;
            const u32 count = std::min<u32>(p.vertexCount, static_cast<u32>(model.root.portalVertices.size()) - first);
            if (first < model.root.portalVertices.size() &&
                PointInPortal(eyeLocal, std::span(model.root.portalVertices).subspan(first, count), p.plane.normal))
                rec.flags |= kPortalEyeOn;
        }
        if (rec.flags & kPortalEyeOn) {
            rec.rect = kFullNdc;
        } else {
            std::vector<Vector3f> poly = PortalPolygon(p, {0.0f, 0.0f, 0.0f});
            ClipPolygon(active, poly);
            if (poly.size() < 3) {
                rec.flags |= kPortalCulled;
                rec.rect = CullRect{};
            } else {
                rec.rect = ProjectBounds(poly, view.worldToClip);
            }
        }
        // A placeholder or missing target closes the portal; the distance fade
        // that could also close it never does at 12.1's defaults.
        if (!Object(target))
            rec.flags |= kPortalClosed;
    }

    // `CWorldViewCull_CreateExteriorPortalView` 0x141B848C0: the outside seen
    // through a portal, its clipped polygon's whole screen bounds.
    void ExteriorView(const Portal& p, const PortalRef& ref, const PortalExtra* mope) {
        f32 push = kPortalBand;
        if (mope)
            push += std::fabs(MopeFloat(ref.side <= 0 ? mope->unknown[1] : mope->unknown[0]));
        if (ref.side > 0)
            push = -push;
        std::vector<Vector3f> poly =
            PortalPolygon(p, {p.plane.normal.x * push, p.plane.normal.y * push, p.plane.normal.z * push});
        ClipPolygon(full, poly);
        if (poly.size() > 2)
            Union(exterior01, To01(ProjectBounds(poly, view.worldToClip)));
    }

    // `CWorldViewCull_CullPortals` 0x141B853D0.
    void Portals(u32 g, u32 from, const CullRect& rect, u32 depth, bool fullFade, u32 mask) {
        if (depth > kMaxDepth)
            return;
        const u32 gf = GroupFlags(g);
        if ((gf & kGroupAlwaysDraw) || (GroupFlags2(g) & kGroup2NoPortals))
            return;
        const CullRect inst = To01(rect);
        const Group* obj = Object(g);
        if (gf & kGroupInterior) {
            if (frustumTouches(active, GroupBox(g)))
                Add(g, inst, mask, true);
            if (obj && (obj->header.flags2 & kGroup2SplitParent)) {
                for (i32 c = obj->header.splitParentOrFirstChild; c >= 0 && c != kNoGroup;) {
                    const Group* child = Object(static_cast<u32>(c));
                    if (!child)
                        break;
                    if (frustumTouches(active, GroupBox(static_cast<u32>(c))))
                        Add(static_cast<u32>(c), inst, mask, true);
                    if (!(child->header.flags2 & kGroup2SplitChild))
                        break;
                    c = child->header.nextSplitChild;
                }
            }
        } else {
            Add(g, inst, mask, true);
        }
        (void)fullFade;
        if (!obj)
            return;
        const Root& root = model.root;
        const u32 end = std::min<u32>(obj->header.portalStart + obj->header.portalCount,
                                      static_cast<u32>(root.portalRefs.size()));
        for (u32 r = obj->header.portalStart; r < end; ++r) {
            const PortalRef& ref = root.portalRefs[r];
            if (ref.groupIndex == kNoGroup)
                continue;
            const u32 p = ref.portalIndex;
            if (p >= root.portals.size())
                continue;
            if ((p >> 5) < 8 && (placement.disabledPortals[p >> 5] & (1u << (p & 31))))
                continue;
            if (ref.groupIndex == from)
                continue;
            const u32 tf = GroupFlags(ref.groupIndex);
            const PortalExtra* mope =
                (ref.flags & 0x20) && p < root.portalExtras.size() ? &root.portalExtras[p] : nullptr;
            PortalRecord& rec = (*placement.portals)[p];
            if (rec.frame != frame) {
                rec.frame = frame;
                Transform(rec, p, mope, ref.groupIndex);
            }
            const Portal& portal = root.portals[p];
            f32 s = Side(portal.plane, eyeLocal);
            if (ref.side < 0)
                s = -s;
            if (mope)
                s += std::fabs(MopeFloat(ref.side <= 0 ? mope->unknown[0] : mope->unknown[1]));
            if (s < 0.0f)
                continue; // the eye is not on this group's side
            if ((rec.flags & (kPortalCulled | kPortalEyeOn)) == kPortalCulled)
                continue;
            const CullRect& pr = rec.rect;
            if (!(rect.maxX >= pr.minX && rect.minX <= pr.maxX && rect.maxY >= pr.minY && rect.minY <= pr.maxY))
                continue;
            const CullRect n{std::max(pr.minX, rect.minX), std::max(pr.minY, rect.minY),
                             std::min(pr.maxX, rect.maxX), std::min(pr.maxY, rect.maxY)};
            if (n.maxX - n.minX < kMinRect || n.maxY - n.minY < kMinRect)
                continue;
            if (interior) {
                if (tf & 0x50148u) {
                    ExteriorView(portal, ref, mope);
                    if (tf & (kGroupAlwaysDraw | kGroupExterior))
                        continue; // never walked into from inside
                }
            } else if (tf & (kGroupAlwaysDraw | kGroupExterior)) {
                continue; // never walked back out
            }
            if (!(rec.flags & kPortalClosed))
                Portals(ref.groupIndex, g, n, depth + 1, false, 63u);
        }
    }

    // `CWorldViewCull_CullMapObjDefExterior` 0x141B86640.
    void Exterior(u32 defMask) {
        interior = false;
        const CullRect ndc = ToNdc(exterior01);
        if (placement.proxyGroup) {
            Add(*placement.proxyGroup, exterior01, defMask, false);
            return;
        }
        for (u32 g = 0; g < model.root.groups.size(); ++g) {
            const u32 gf = GroupFlags(g);
            if ((gf & kGroupAntiportal) || !(gf & (kGroupWindow | kGroupAlwaysDraw | kGroupExterior)))
                continue;
            if (!Object(g))
                continue;
            u32 mask = 0;
            if (defMask) {
                if (!frustumTouches(active, GroupBox(g), mask))
                    continue;
                mask = ~mask & 0x3Fu;
            }
            if (gf & kGroupAlwaysDraw)
                Add(g, exterior01, mask, false);
            else
                Portals(g, kNoGroup, ndc, 0, false, mask);
        }
    }
};

} // namespace

Vector3f transformPoint(const Vector3f& p, const Matrix4& t) {
    const auto& m = t.m;
    return {p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + m[3][0],
            p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + m[3][1],
            p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2] + m[3][2]};
}

CullFrustum frustumFromCorners(const FrustumCorners& c) {
    CullFrustum f;
    f.planes[0] = PlaneOf(c[1], c[5], c[6]); // top
    f.planes[1] = PlaneOf(c[0], c[7], c[4]); // bottom
    f.planes[2] = PlaneOf(c[0], c[4], c[5]); // left
    f.planes[3] = PlaneOf(c[3], c[6], c[7]); // right
    f.planes[4] = PlaneOf(c[5], c[4], c[6]); // far
    const Vector3f n{-f.planes[4].normal.x, -f.planes[4].normal.y, -f.planes[4].normal.z};
    f.planes[5] = {n, -Dot(n, c[2])}; // near
    return f;
}

FrustumCorners subCorners(const FrustumCorners& c, const CullRect& r) {
    FrustumCorners out{};
    for (u32 q = 0; q < 8; q += 4) {
        const auto at = [&](f32 u, f32 v) {
            const Vector3f lo = Lerp(c[q + 0], c[q + 3], u);
            const Vector3f hi = Lerp(c[q + 1], c[q + 2], u);
            return Lerp(lo, hi, v);
        };
        out[q + 0] = at(r.minX, r.minY);
        out[q + 1] = at(r.minX, r.maxY);
        out[q + 2] = at(r.maxX, r.maxY);
        out[q + 3] = at(r.maxX, r.minY);
    }
    return out;
}

bool frustumTouches(const CullFrustum& f, const Box& b) {
    for (const Plane& p : f.planes)
        if (Side(p, FarCorner(b, p.normal)) < -kBoxTolerance)
            return false;
    return true;
}

bool frustumTouches(const CullFrustum& f, const Box& b, u32& insideMask) {
    u32 inside = 0;
    for (u32 i = 0; i < 6; ++i) {
        const Plane& p = f.planes[i];
        if (Side(p, FarCorner(b, p.normal)) < -kBoxTolerance)
            return false;
        if (Side(p, NearCorner(b, p.normal)) > kBoxTolerance)
            inside |= 1u << i;
    }
    insideMask = inside;
    return true;
}

bool frustumTouchesMasked(const CullFrustum& f, const Box& b, u32 mask) {
    for (u32 i = 0; i < 6; ++i) {
        if (!(mask & (1u << i)))
            continue;
        const Plane& p = f.planes[i];
        if (Side(p, FarCorner(b, p.normal)) < -kBoxTolerance)
            return false;
    }
    return true;
}

Box transformBox(const Box& box, const Matrix4& t) {
    const auto& m = t.m;
    Box out;
    out.minimum = out.maximum = {m[3][0], m[3][1], m[3][2]};
    const f32 lo[3] = {box.minimum.x, box.minimum.y, box.minimum.z};
    const f32 hi[3] = {box.maximum.x, box.maximum.y, box.maximum.z};
    f32* omin[3] = {&out.minimum.x, &out.minimum.y, &out.minimum.z};
    f32* omax[3] = {&out.maximum.x, &out.maximum.y, &out.maximum.z};
    for (u32 j = 0; j < 3; ++j) {
        for (u32 i = 0; i < 3; ++i) {
            const f32 a = m[i][j] * lo[i], b = m[i][j] * hi[i];
            *omin[j] += std::min(a, b);
            *omax[j] += std::max(a, b);
        }
    }
    return out;
}

CullResult cullGroups(const CullPlacement& placement, const CullView& view, const CameraLocation* inside,
                      u32 frame, const CullRect& exterior) {
    if (!placement.model || !placement.portals)
        return {};
    const Model& model = *placement.model;
    if (placement.portals->size() != model.root.portals.size())
        placement.portals->assign(model.root.portals.size(), PortalRecord{});
    Cull c{placement, model, view, frame, transformPoint(view.eye, placement.worldToLocal), {}, {}, false, {}, {}, {}};
    c.full = frustumFromCorners(view.corners);
    c.visibleIndex.assign(model.root.groups.size(), -1);

    if (inside && inside->inside) {
        // CWorldViewCull_InteriorCull: each camera group a root, the whole
        // screen and the whole frustum, a split child walking from its parent.
        c.interior = true;
        c.active = c.full;
        for (const u16 g : inside->groups) {
            if (g == kNoGroup || g >= model.root.groups.size())
                continue;
            u32 start = g;
            if (const Group* obj = c.Object(g); obj && (obj->header.flags2 & kGroup2SplitChild)) {
                const i32 parent = obj->header.splitParentOrFirstChild;
                if (parent >= 0 && c.Object(static_cast<u32>(parent)))
                    start = static_cast<u32>(parent);
            }
            c.Portals(start, kNoGroup, kFullNdc, 0, true, 63u);
        }
        if (c.exterior01.empty())
            return {std::move(c.visible), c.exterior01}; // nothing outdoors at all
    } else {
        c.exterior01 = exterior;
        if (c.exterior01.empty())
            return {};
    }

    // The exterior pass, for this WMO too, in the frustum narrowed to what the
    // interior pass saw of the outside.
    c.active = frustumFromCorners(subCorners(view.corners, c.exterior01));
    Box defBox = transformBox(model.root.header.bounds, placement.localToWorld);
    for (const Box& b : placement.groupBoxes) {
        defBox.minimum = {std::min(defBox.minimum.x, b.minimum.x), std::min(defBox.minimum.y, b.minimum.y),
                          std::min(defBox.minimum.z, b.minimum.z)};
        defBox.maximum = {std::max(defBox.maximum.x, b.maximum.x), std::max(defBox.maximum.y, b.maximum.y),
                          std::max(defBox.maximum.z, b.maximum.z)};
    }
    u32 defInside = 0;
    if (frustumTouches(c.active, defBox, defInside))
        c.Exterior(~defInside & 0x3Fu);
    return {std::move(c.visible), c.exterior01};
}

u16 doodadFadeTarget(const Box& box, f32 size, const Vector3f& eye, const DoodadFadeRule& rule) {
    const Vector3f centre{(box.minimum.x + box.maximum.x) * 0.5f, (box.minimum.y + box.maximum.y) * 0.5f,
                          (box.minimum.z + box.maximum.z) * 0.5f};
    const Vector3f d = Sub(centre, eye);
    const f32 dist = std::sqrt(Dot(d, d));
    bool out;
    if (dist - size > rule.far) {
        out = true;
    } else {
        const f32 t = std::max(dist - rule.near, 0.0f) * rule.k;
        out = t * t > size * size * 9.0f;
    }
    return out ? 0 : 0xFF00;
}

u16 stepDoodadFade(u16 current, u16 target, u32 elapsedMs) {
    const u32 step = std::min<u32>(static_cast<u32>(static_cast<f32>(elapsedMs << 8)), 0xFFFFu);
    if (target > current)
        return static_cast<u16>(std::min<u32>(current + step, target));
    return static_cast<u16>(std::max<i32>(static_cast<i32>(current) - static_cast<i32>(step), target));
}

} // namespace wmo
} // namespace wow
} // namespace models
} // namespace whiteout
