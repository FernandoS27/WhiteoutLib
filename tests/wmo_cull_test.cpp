// 12.1's WMO view cull on a hand-built WMO: an exterior yard (group 0) with a
// doorway into a room (group 1), and a small window from the room into a back
// room (group 2). Distances are in yards.

#include <catch2/catch_all.hpp>

#include <cmath>
#include <whiteout/models/wow/wmo/cull.h>

using namespace whiteout;
using namespace whiteout::models::wow::wmo;

namespace {

Group MakeGroup(u32 flags, u16 portalStart, u16 portalCount) {
    Group g;
    g.header.flags = flags;
    g.header.portalStart = portalStart;
    g.header.portalCount = portalCount;
    return g;
}

// A portal: a square in the plane x = @p x, centre (y, z) = (5, 5), half size
// @p half; its plane faces +x.
void AddPortal(Root& root, f32 x, f32 half) {
    Portal p;
    p.startVertex = static_cast<u16>(root.portalVertices.size());
    p.vertexCount = 4;
    p.plane = {{1.0f, 0.0f, 0.0f}, -x};
    for (const auto& [y, z] : std::array<std::array<f32, 2>, 4>{
             {{5 - half, 5 - half}, {5 + half, 5 - half}, {5 + half, 5 + half}, {5 - half, 5 + half}}})
        root.portalVertices.push_back({x, y, z});
    root.portals.push_back(p);
}

Model MakeWmo() {
    Model m;
    Root& root = m.root;
    root.header.bounds = {{-10.0f, 0.0f, 0.0f}, {30.0f, 10.0f, 10.0f}};
    root.groups = {{0x8u, {{-10.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}}, -1},
                   {0x2000u, {{10.0f, 0.0f, 0.0f}, {20.0f, 10.0f, 10.0f}}, -1},
                   {0x2000u, {{20.0f, 0.0f, 0.0f}, {30.0f, 10.0f, 10.0f}}, -1}};
    root.groups2.resize(3);
    AddPortal(root, 10.0f, 3.0f); // the doorway, yard ↔ room
    AddPortal(root, 20.0f, 1.0f); // the window, room ↔ back room
    // MOPR: side +1 is the +x side of a portal's plane.
    root.portalRefs = {{0, 1, -1, 0},  // the yard sees the doorway from −x
                       {0, 0, 1, 0},   // the room, from +x
                       {1, 2, -1, 0},  // the room sees the window from −x
                       {1, 1, 1, 0}};  // the back room, from +x
    m.groups = {MakeGroup(0x8u, 0, 1), MakeGroup(0x2000u, 1, 2), MakeGroup(0x2000u, 3, 1)};
    return m;
}

// An eye at @p eye looking along @p fwd (unit, never along z), up +z, 90° by
// 90°, near 0.1, far 1000.
CullView MakeView(const Vector3f& eye, const Vector3f& fwd) {
    const Vector3f up0{0.0f, 0.0f, 1.0f};
    Vector3f right{fwd.y * up0.z - fwd.z * up0.y, fwd.z * up0.x - fwd.x * up0.z, fwd.x * up0.y - fwd.y * up0.x};
    const f32 rl = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    right = {right.x / rl, right.y / rl, right.z / rl};
    const Vector3f up{right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z,
                      right.x * fwd.y - right.y * fwd.x};
    CullView v;
    v.eye = eye;
    const f32 dists[2] = {0.1f, 1000.0f};
    const f32 ndc[4][2] = {{-1, -1}, {-1, 1}, {1, 1}, {1, -1}};
    for (u32 q = 0; q < 2; ++q)
        for (u32 i = 0; i < 4; ++i) {
            const f32 d = dists[q];
            v.corners[q * 4 + i] = {eye.x + fwd.x * d + right.x * ndc[i][0] * d + up.x * ndc[i][1] * d,
                                    eye.y + fwd.y * d + right.y * ndc[i][0] * d + up.y * ndc[i][1] * d,
                                    eye.z + fwd.z * d + right.z * ndc[i][0] * d + up.z * ndc[i][1] * d};
        }
    const auto dot = [](const Vector3f& a, const Vector3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    auto& m = v.worldToClip.m;
    m = {};
    const Vector3f cols[3] = {right, up, fwd};
    const u32 out[3] = {0, 1, 3};
    for (u32 c = 0; c < 3; ++c) {
        m[0][out[c]] = cols[c].x;
        m[1][out[c]] = cols[c].y;
        m[2][out[c]] = cols[c].z;
        m[3][out[c]] = -dot(eye, cols[c]);
    }
    return v;
}

std::vector<u32> Groups(const CullResult& r) {
    std::vector<u32> g;
    for (const VisibleGroup& v : r.groups)
        g.push_back(v.group);
    std::sort(g.begin(), g.end());
    return g;
}

struct Fixture {
    Model model = MakeWmo();
    std::vector<Box> boxes;
    std::vector<u32> lod = {0, 0, 0};
    std::vector<PortalRecord> portals;
    CullPlacement placement;

    Fixture() {
        for (const GroupInfo& g : model.root.groups)
            boxes.push_back(g.bounds);
        placement.model = &model;
        placement.groupBoxes = boxes;
        placement.currentLod = lod;
        placement.portals = &portals;
    }
};

} // namespace

TEST_CASE("cull frustum: planes from corners, box tests, the box transform", "[wmo][cull]") {
    const CullView v = MakeView({0.0f, 5.0f, 5.0f}, {1.0f, 0.0f, 0.0f});
    const CullFrustum f = frustumFromCorners(v.corners);
    // Every plane faces in: the point ahead is inside all six.
    for (const Plane& p : f.planes) {
        CHECK(std::fabs(p.normal.x * p.normal.x + p.normal.y * p.normal.y + p.normal.z * p.normal.z - 1.0f) < 1e-5f);
        CHECK(p.normal.x * 50.0f + p.normal.y * 5.0f + p.normal.z * 5.0f + p.distance > 0.0f);
    }
    CHECK(frustumTouches(f, Box{{40.0f, 0.0f, 0.0f}, {60.0f, 10.0f, 10.0f}}));
    CHECK_FALSE(frustumTouches(f, Box{{-60.0f, 0.0f, 0.0f}, {-40.0f, 10.0f, 10.0f}}));
    // Within the 7/360 yd tolerance of the near plane still counts.
    CHECK(frustumTouches(f, Box{{0.08f, 4.0f, 4.0f}, {0.09f, 6.0f, 6.0f}}));
    u32 inside = 0;
    REQUIRE(frustumTouches(f, Box{{40.0f, 4.0f, 4.0f}, {60.0f, 6.0f, 6.0f}}, inside));
    CHECK(inside == 0x3Fu);
    REQUIRE(frustumTouches(f, Box{{40.0f, -100.0f, 4.0f}, {60.0f, 6.0f, 6.0f}}, inside));
    CHECK(inside != 0x3Fu);
    // Only the masked planes are asked.
    CHECK(frustumTouchesMasked(f, Box{{-60.0f, 0.0f, 0.0f}, {-40.0f, 10.0f, 10.0f}}, 0u));

    Matrix4 rot; // a quarter turn about z, then (100, 0, 0)
    rot.m = {{{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {100, 0, 0, 1}}};
    const Box b = transformBox(Box{{0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 3.0f}}, rot);
    CHECK(b.minimum.x == 99.0f);
    CHECK(b.maximum.x == 100.0f);
    CHECK(b.minimum.y == 0.0f);
    CHECK(b.maximum.y == 2.0f);
    CHECK(b.maximum.z == 3.0f);
}

TEST_CASE("cull: from the yard the doorway shows the room, the window the back room", "[wmo][cull]") {
    Fixture fx;
    const CullView v = MakeView({-30.0f, 5.0f, 5.0f}, {1.0f, 0.0f, 0.0f});
    const CullResult r = cullGroups(fx.placement, v, nullptr, 1);
    CHECK(Groups(r) == std::vector<u32>{0, 1, 2});
    for (const VisibleGroup& g : r.groups) {
        // Reached through a portal: every plane is tested for the doodads.
        if (g.group != 0)
            CHECK(g.mask == 0x3Fu);
        CHECK(g.doodads);
    }
    // Looking away, nothing.
    const CullView away = MakeView({-30.0f, 5.0f, 5.0f}, {-1.0f, 0.0f, 0.0f});
    CHECK(cullGroups(fx.placement, away, nullptr, 2).groups.empty());
}

TEST_CASE("cull: a portal behind the eye, or into a missing file, stops the walk", "[wmo][cull]") {
    Fixture fx;
    // In the yard, facing away from the doorway: it is clipped away behind the
    // eye, so the yard alone.
    const CullView past = MakeView({0.0f, 5.0f, 5.0f}, {-1.0f, 0.0f, 0.0f});
    const CullResult r = cullGroups(fx.placement, past, nullptr, 1);
    CHECK(Groups(r) == std::vector<u32>{0});
    // A back room whose file is missing at its level closes the window.
    fx.lod[2] = 1;
    const CullView v = MakeView({-30.0f, 5.0f, 5.0f}, {1.0f, 0.0f, 0.0f});
    CHECK(Groups(cullGroups(fx.placement, v, nullptr, 2)) == std::vector<u32>{0, 1});
}

TEST_CASE("cull: from the room the yard shows through the doorway's rect only", "[wmo][cull]") {
    Fixture fx;
    CameraLocation in;
    in.inside = true;
    in.groups = {1, 0xFFFF};
    // Facing the doorway: the room, the yard through it.
    const CullView back = MakeView({15.0f, 5.0f, 5.0f}, {-1.0f, 0.0f, 0.0f});
    const CullResult a = cullGroups(fx.placement, back, &in, 1);
    CHECK(Groups(a) == std::vector<u32>{0, 1});
    CHECK_FALSE(a.exterior.empty());
    CHECK(a.exterior.maxX - a.exterior.minX < 1.0f);
    // Facing the window: the room and the back room; no exterior portal in
    // view, so nothing outdoors at all.
    const CullView front = MakeView({15.0f, 5.0f, 5.0f}, {1.0f, 0.0f, 0.0f});
    const CullResult b = cullGroups(fx.placement, front, &in, 2);
    CHECK(Groups(b) == std::vector<u32>{1, 2});
    CHECK(b.exterior.empty());
}

TEST_CASE("doodad fade: 30 yd plus 20 of its size, past the horizon, 256 a millisecond", "[wmo][cull]") {
    const DoodadFadeRule rule;
    const Box one{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}; // size 1: out past 50 yd
    CHECK(doodadFadeTarget(one, 1.0f, {49.0f, 0.0f, 0.0f}, rule) == 0xFF00);
    CHECK(doodadFadeTarget(one, 1.0f, {51.0f, 0.0f, 0.0f}, rule) == 0);
    const Box big{{-500.0f, -500.0f, -500.0f}, {500.0f, 500.0f, 500.0f}};
    CHECK(doodadFadeTarget(big, 1000.0f, {1800.0f, 0.0f, 0.0f}, rule) == 0xFF00);
    CHECK(doodadFadeTarget(big, 1000.0f, {1900.0f, 0.0f, 0.0f}, rule) == 0);
    CHECK(stepDoodadFade(0, 0xFF00, 100) == 25600);
    CHECK(stepDoodadFade(0, 0xFF00, 300) == 0xFF00);
    CHECK(stepDoodadFade(0xFF00, 0, 16) == 0xFF00 - 4096);
}
