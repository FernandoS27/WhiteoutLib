// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The 12.1 client's load- and draw-time readings of a WMO (runtime.h), each case worked by hand
/// from the function it cites: FixColorVertexAlpha's integer halving, AttenTransVerts' portal
/// falloff, the ambient pick, the MOUV period and the liquid-type mapping.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstring>
#include <limits>
#include <vector>
#include <whiteout/models/wow/wmo/wmo.h>

using namespace whiteout;
using namespace whiteout::models::wow::wmo;

namespace {

Group quadGroup(u32 flags, u16 transBatches, u32 transMaxIndex) {
    Group g;
    g.header.flags = flags;
    g.header.transBatchCount = transBatches;
    g.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    Batch trans;
    trans.maxIndex = static_cast<u16>(transMaxIndex);
    g.batches = {trans, Batch{}};
    return g;
}

} // namespace

TEST_CASE("the shader table and the missing-texture fallback", "[wmo][runtime]") {
    CHECK(shaderInfo(23).textureCount == 9);
    CHECK(shaderInfo(22).textureCount == 6);
    CHECK(shaderInfo(24).textureCount == 3);
    CHECK(shaderInfo(99).textureCount == 1);

    Root root;
    Material m;
    m.shader = 6; // two textures
    m.texture0 = 10;
    CHECK(effectiveShader(root, m) == 4);
    m.texture1 = 11;
    CHECK(effectiveShader(root, m) == 6);
    m.shader = 23; // exempt: keeps its shader with slots missing
    CHECK(effectiveShader(root, m) == 23);

    root.textureNames = std::vector<char>{'a', 0, 0};
    m.shader = 0;
    m.texture0 = 1; // the empty string after "a"
    CHECK_FALSE(hasTexture(root, m, 0));
    CHECK(effectiveShader(root, m) == 4);
    m.texture0 = 0;
    CHECK(hasTexture(root, m, 0));
}

TEST_CASE("LOD levels and which groups have files at them", "[wmo][runtime]") {
    Root root;
    CHECK(maxLodLevel(root) == 0);
    root.header.flags = static_cast<u16>(RootFlag::Lod);
    root.header.lodCount = 3;
    CHECK(maxLodLevel(root) == 2);
    root.header.lodCount = 0;
    CHECK(maxLodLevel(root) == 2);
    root.header.lodCount = 9;
    CHECK(maxLodLevel(root) == 4);
    root.header.lodCount = 3;
    root.header.flags |= static_cast<u16>(RootFlag::ExtraLodLevel);
    CHECK(maxLodLevel(root) == 1);

    root.groups.resize(2);
    root.groups[0].flags = 0x400;
    root.groups2.resize(2);
    root.groups2[0].lodIndex = 1;
    CHECK(groupHasLod(root, 0, 0));
    CHECK(groupHasLod(root, 0, 1));
    CHECK_FALSE(groupHasLod(root, 0, 2));
    CHECK_FALSE(groupHasLod(root, 1, 1));
}

TEST_CASE("transition vertices run to the highest one a transition batch uses", "[wmo][runtime]") {
    CHECK(transitionVertexCount(quadGroup(0, 0, 0)) == 0);
    CHECK(transitionVertexCount(quadGroup(0, 1, 1)) == 2);
    // GroupFlag::Lod makes every vertex one.
    CHECK(transitionVertexCount(quadGroup(0x400, 1, 1)) == 4);
}

TEST_CASE("FixColorVertexAlpha, worked through", "[wmo][runtime]") {
    Root root;
    root.header.ambientColor = Color{10, 20, 30, 255}; // b, g, r
    const Group group = quadGroup(0x8, 1, 1);

    std::vector<Color> colors = {
        {100, 100, 100, 51}, // trans: (c − amb) · (1 − a/255), rounded, halved
        {5, 200, 40, 0},     // trans, b below the ambient clamps to 0
        {100, 50, 200, 64},  // int/ext: (c + (c·a >> 6) − amb) >> 1
        {250, 250, 250, 255},
    };
    fixVertexColorAlpha(root, group, colors);
    CHECK(colors[0] == Color{36, 32, 28, 51});
    CHECK(colors[1] == Color{0, 90, 5, 0});
    CHECK(colors[2] == Color{95, 40, 185, 255});
    CHECK(colors[3] == Color{255, 255, 255, 255});

    SECTION("an interior group's alpha goes to 0") {
        std::vector<Color> c = {{0, 0, 0, 0}, {0, 0, 0, 0}, {8, 8, 8, 7}, {0, 0, 0, 0}};
        fixVertexColorAlpha(root, quadGroup(0, 1, 1), c);
        CHECK(c[2].a == 0);
    }
    SECTION("MOHD 0x2 takes no ambient out") {
        root.header.flags = static_cast<u16>(RootFlag::NoAmbientInVertexColor);
        std::vector<Color> c = {{0, 0, 0, 0}, {0, 0, 0, 0}, {100, 50, 200, 64}, {0, 0, 0, 0}};
        fixVertexColorAlpha(root, group, c);
        CHECK(c[2] == Color{100, 50, 200, 255});
    }
    SECTION("MOHD 0x8 rewrites alpha alone") {
        root.header.flags = static_cast<u16>(RootFlag::VertexColorAlphaOnly);
        std::vector<Color> c = {{9, 9, 9, 9}, {9, 9, 9, 9}, {9, 9, 9, 9}, {9, 9, 9, 9}};
        fixVertexColorAlpha(root, group, c);
        CHECK(c[0] == Color{9, 9, 9, 9});
        CHECK(c[2] == Color{9, 9, 9, 255});
    }
}

TEST_CASE("AttenTransVerts fades toward an exterior group through a portal", "[wmo][runtime]") {
    Root root;
    // A 2x2 portal in the plane x = 0, facing +x, leading to group 1.
    root.portalVertices = {{0, -1, -1}, {0, 1, -1}, {0, 1, 1}, {0, -1, 1}};
    Portal portal;
    portal.startVertex = 0;
    portal.vertexCount = 4;
    portal.plane = Plane{{1, 0, 0}, 0};
    root.portals = {portal};
    PortalRef ref;
    ref.portalIndex = 0;
    ref.groupIndex = 1;
    ref.side = 1;
    root.portalRefs = {ref};
    root.groups.resize(2);
    root.groups[1].flags = 0x8; // exterior

    Group group;
    group.header.transBatchCount = 1;
    group.header.portalStart = 0;
    group.header.portalCount = 1;
    group.positions = {{2, 0, 0}, {-0.5f, 0, 0}, {0.5f, 5, 0}};
    Batch trans;
    trans.maxIndex = 2;
    group.batches = {trans};

    std::vector<Color> colors(3, Color{100, 100, 100, 0});
    attenuateTransitionVertices(root, group, colors);
    // In front, 2 away: 1 − 2·0.15 = 0.7.
    CHECK(colors[0] == Color{30, 30, 30, 178});
    // Behind it (clamped to 0 away): fully faded.
    CHECK(colors[1] == Color{0, 0, 0, 255});
    // Off the polygon: the distance to its nearest edge, √(0.25 + 16).
    CHECK(colors[2].a == 101);
    CHECK(colors[2].r == 60);

    SECTION("an interior neighbour within a yard blocks the fade") {
        root.groups[1].flags = 0;
        std::vector<Color> c(3, Color{100, 100, 100, 7});
        group.positions = {{0.5f, 0, 0}, {-0.5f, 0, 0}, {5, 0, 0}};
        attenuateTransitionVertices(root, group, c);
        CHECK(c[0] == Color{100, 100, 100, 0});
        CHECK(c[1] == Color{100, 100, 100, 0});
        CHECK(c[2] == Color{100, 100, 100, 0});
    }
    SECTION("MOHD 0x1 turns it off") {
        root.header.flags = static_cast<u16>(RootFlag::NoTransitionAttenuation);
        std::vector<Color> c(3, Color{100, 100, 100, 7});
        attenuateTransitionVertices(root, group, c);
        CHECK(c[0] == Color{100, 100, 100, 7});
    }
}

TEST_CASE("the vertex colours the client uploads", "[wmo][runtime]") {
    Root root;
    Group group;
    group.header.flags = 0x2000 | 0x40 | 0x4; // interior, exterior-lit, MOCV
    group.positions.resize(2);
    group.vertexColors = {{{1, 2, 3, 4}, {5, 6, 7, 8}}};
    group.vertexColors2 = std::vector<Color>{{9, 10, 11, 12}, {0, 0, 0, 0}};
    const VertexColors c = vertexColors(root, group);
    // RGBA byte order, halved by the fix-up, alpha forced by flags 0x2040.
    CHECK(c.color0[0] == (1u | 1u << 8 | 0u << 16 | 0xFFu << 24));
    CHECK(c.color1[0] == 0xFF000000u);
    CHECK(c.color2[0] == (11u | 10u << 8 | 9u << 16 | 12u << 24));

    Group bare;
    bare.positions.resize(1);
    const VertexColors d = vertexColors(root, bare);
    CHECK(d.color0[0] == 0xFF000000u);
    CHECK(d.color2[0] == 0u);
}

TEST_CASE("the ambient the root gives a placement", "[wmo][runtime]") {
    Root root;
    root.header.ambientColor = Color{1, 1, 1, 1};
    CHECK(ambientColors(root, {})[1] == Color{1, 1, 1, 1});

    AmbientVolume first;
    first.flags = 1;
    first.color1 = {10, 0, 0, 0};
    first.color2 = {20, 0, 0, 0};
    first.color3 = {30, 0, 0, 0};
    AmbientVolume forSet;
    forSet.doodadSetId = 3;
    forSet.color1 = {40, 0, 0, 0};
    root.globalAmbients = {first, forSet};

    const std::vector<u16> none;
    const auto a = ambientColors(root, none);
    CHECK(a[0].b == 10);
    CHECK(a[1].b == 20);
    CHECK(a[2].b == 30);
    const std::vector<u16> three = {3};
    const auto b = ambientColors(root, three);
    CHECK(b[0].b == 40);
    CHECK(b[2].b == 40);
}

TEST_CASE("MOUV scrolls by whole-millisecond periods", "[wmo][runtime]") {
    Root root;
    root.materials.resize(1);
    root.uvAnimations = {MaterialUvAnimation{{0.5f, -0.25f}, {0.0f, 0.0f}}};
    const UvScroll s = uvScroll(root, 0, 1500);
    CHECK(s.animated);
    CHECK(s.layer0.x == Catch::Approx(0.75f));
    CHECK(s.layer0.y == Catch::Approx(0.625f));
    CHECK(s.layer1.x == 0.0f);

    root.uvAnimations[0] = MaterialUvAnimation{};
    CHECK_FALSE(uvScroll(root, 0, 1500).animated);
    CHECK_FALSE(uvScroll(root, 5, 1500).animated);
}

TEST_CASE("a group's liquid type", "[wmo][runtime]") {
    Root root;
    Group group;
    group.header.groupLiquid = 0; // legacy: + 1, then the basic type
    CHECK(groupLiquidType(root, group) == 13);
    group.header.flags = 0x80000;
    CHECK(groupLiquidType(root, group) == 14);
    group.header.flags = 0;
    group.header.groupLiquid = 2;
    CHECK(groupLiquidType(root, group) == 19);

    // 15 means "per tile": the first tile that holds liquid decides.
    group.header.groupLiquid = 15;
    Liquid liquid;
    liquid.tiles = {0x0F, 0x03};
    group.liquid = liquid;
    CHECK(groupLiquidType(root, group) == 20);

    root.header.flags = static_cast<u16>(RootFlag::LiquidTypeFromDb);
    group.header.groupLiquid = 100;
    CHECK(groupLiquidType(root, group) == 100);
    group.header.groupLiquid = 1;
    CHECK(groupLiquidType(root, group) == 13);
}

TEST_CASE("doodad sets and the doodads a group draws", "[wmo][runtime]") {
    Root root;
    DoodadSet global;
    global.startIndex = 0;
    global.count = 2;
    DoodadSet extra;
    extra.startIndex = 2;
    extra.count = 1;
    root.doodadSets = {global, extra};
    root.doodadDefs.resize(4);
    root.doodadFileIds = {100, 200, 300};
    root.doodadDefs[2].nameAndFlags = 2;
    root.doodadDefs[3].nameAndFlags = 9; // out of range reads MODI[0]

    CHECK(doodadSetOf(root, 1) == 0);
    CHECK(doodadSetOf(root, 2) == 1);
    CHECK(doodadSetOf(root, 3) == 0xFFFF);
    CHECK(doodadModel(root, root.doodadDefs[2]).fileId == 300);
    CHECK(doodadModel(root, root.doodadDefs[3]).fileId == 100);

    Group group;
    group.doodadRefs = {0, 2, 3};
    CHECK(activeDoodads(root, group, {}) == std::vector<u32>{0});
    const std::vector<u16> sets = {1};
    CHECK(activeDoodads(root, group, sets) == std::vector<u32>{0, 2});
}

TEST_CASE("a placement's doodads: one per MODD entry, exterior if any group is", "[wmo][runtime]") {
    Model model;
    Root& root = model.root;
    DoodadSet global;
    global.count = 2;
    DoodadSet extra;
    extra.startIndex = 2;
    extra.count = 2;
    root.doodadSets = {global, extra};
    root.doodadDefs.resize(4);
    root.groups.resize(2);
    root.groups[1].flags = 0x8; // exterior
    Group inside;
    inside.doodadRefs = {3, 0, 2};
    Group outside;
    outside.doodadRefs = {0, 1};
    model.groups = {inside, outside};

    const std::vector<u16> sets = {1};
    const auto placed = placedDoodads(model, sets);
    REQUIRE(placed.size() == 4);
    CHECK(
        (placed[0].index == 3 && placed[0].set == 1 && placed[0].group == 0 && placed[0].interior));
    CHECK((placed[1].index == 0 && placed[1].set == 0 && placed[1].group == 0 &&
           !placed[1].interior));
    CHECK((placed[2].index == 2 && placed[2].interior));
    CHECK((placed[3].index == 1 && placed[3].group == 1 && !placed[3].interior));

    const auto base = placedDoodads(model, {});
    REQUIRE(base.size() == 2);
    CHECK(base[0].index == 0);
    CHECK(base[1].index == 1);
}

TEST_CASE("the doodad colour helpers, byte for byte", "[wmo][runtime]") {
    // x + 512 keeps 14 fraction bits: truncation, except within 2^-15 of the next.
    CHECK(colorByte(127.5f) == 127);
    CHECK(colorByte(100.99999f) == 101);
    CHECK(colorByte(255.0f) == 255);

    // k = int(24480 / max); channel = (k·c + 255) >> 8.
    CHECK(clampAmbient(Color{40, 80, 200, 9}) == Color{20, 39, 96, 9});
    CHECK(clampAmbient(Color{50, 150, 100, 1}) == Color{32, 96, 64, 1});
    CHECK(clampAmbient(Color{10, 96, 20, 0}) == Color{10, 96, 20, 0});

    // HSV value to 112/255, truncated back, alpha 255; 112 and over untouched.
    CHECK(brightenDirect(Color{10, 20, 30, 0x80}) == Color{37, 74, 112, 255});
    CHECK(brightenDirect(Color{50, 50, 50, 1}) == Color{112, 112, 112, 255});
    CHECK(brightenDirect(Color{7, 111, 60, 0}) == Color{7, 112, 60, 255});
    CHECK(brightenDirect(Color{0, 0, 0, 7}) == Color{0, 0, 0, 255});
    CHECK(brightenDirect(Color{0, 0, 112, 3}) == Color{0, 0, 112, 3});

    // Over 1, divided by the largest; the output's alpha stays.
    CHECK(addColorSaturate(Color{0, 0, 0, 9}, {0.5f, 0.0f, 0.0f}, Color{0, 128, 255, 0}) ==
          Color{0, 85, 255, 9});
    CHECK(addColorSaturate(Color{}, {0.2f, 0.0f, 0.0f}, Color{0, 0, 102, 0}) ==
          Color{0, 0, 153, 0});
}

TEST_CASE("an interior doodad's lighting per MODD flag", "[wmo][runtime]") {
    Root root;
    root.doodadDefs.resize(1);
    DoodadDef& def = root.doodadDefs[0];
    Group group;
    const std::array<Color, 3> ambient = {Color{0, 0, 102, 0}, Color{0, 102, 0, 0},
                                          Color{102, 0, 0, 0}};
    const Vector3f lo{-1.0f, -1.0f, -1.0f}, hi{1.0f, 1.0f, 1.0f};
    const auto lit = [&] { return doodadLighting(root, 0, group, lo, hi, ambient); };
    const auto flags = [&](u8 f) { def.nameAndFlags = static_cast<u32>(f) << 24; };

    SECTION("no flags: the colour, clamped for the ambients and brightened for the light") {
        def.color = {40, 80, 200, 255};
        const DoodadLighting l = lit();
        CHECK(l.sky == Color{20, 39, 96, 255});
        CHECK(l.ground == l.sky);
        CHECK(l.direct == Color{40, 80, 200, 255});
        CHECK(l.direction.z == -0.9f);
    }
    SECTION("0x10: the colour times max(MDDI, 1) added to each ambient") {
        flags(0x10);
        def.color = {0, 0, 51, 0};
        DoodadLighting l = lit();
        CHECK(l.direct == Color{0, 0, 153, 0}); // the sky's sum, before the clamp
        CHECK(l.sky == Color{0, 0, 96, 0});
        CHECK(l.horizon == Color{0, 96, 48, 0});
        CHECK(l.ground == Color{96, 0, 48, 0});
        root.doodadColorMultipliers = {2.0f};
        l = lit();
        CHECK(l.direct == Color{0, 0, 204, 0});
        CHECK(l.sky == Color{0, 0, 96, 0});
        root.doodadColorMultipliers.clear();
    }
    SECTION("0x08: the colour is all three ambients, as it stands") {
        flags(0x08);
        def.color = {10, 20, 30, 0x80};
        const DoodadLighting l = lit();
        CHECK(l.sky == Color{10, 20, 30, 0});
        CHECK(l.horizon == l.sky);
        CHECK(l.direct == Color{37, 74, 112, 255});
    }
    SECTION("0x40: a black light, and the black colour reaches the ambients too") {
        flags(0x40);
        def.color = {9, 9, 9, 5};
        const DoodadLighting l = lit();
        CHECK(l.direct == Color{0, 0, 0, 5});
        CHECK(l.sky == Color{0, 0, 0, 5});
    }
    SECTION("0x02 | 0x04: MOLT's light at its colour times intensity, MDAL's ambients") {
        flags(0x06);
        def.color = {0, 0, 0, 1};
        root.lights.resize(2);
        root.lights[1].color = {0, 255, 255, 0};
        root.lights[1].intensity = 2.0f;
        root.lights[1].position = {0.0f, 0.0f, 10.0f};
        group.ambientOverride = Color{1, 2, 3, 4};
        DoodadLighting l = lit();
        CHECK(l.direct == Color{0, 255, 255, 1}); // WMO-lit: not brightened
        CHECK(l.direction.x == 0.0f);
        CHECK(l.direction.z == -1.0f);
        CHECK(l.sky == Color{1, 2, 3, 4});
        CHECK(l.ground == Color{1, 2, 3, 4});
        group.ambientOverride.reset();
        l = lit();
        CHECK(l.sky == ambient[0]);
        CHECK(l.horizon == ambient[1]);
        CHECK(l.ground == ambient[2]);
    }
    SECTION("0x02 with no MOLT light: aimed from the group's centre") {
        flags(0x02);
        def.color = {0, 0, 0, 0xFF};
        group.header.bounds = {{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}};
        const DoodadLighting l =
            doodadLighting(root, 0, group, {0.0f, 0.0f, 4.0f}, {2.0f, 2.0f, 6.0f}, ambient);
        CHECK(l.direction.z == 1.0f);
        CHECK(l.direct == Color{0, 0, 0, 0xFF});
        CHECK(l.sky == ambient[0]);
    }
}

TEST_CASE("the camera's ambient: volumes weighted at the camera, faded at the portal",
          "[wmo][runtime]") {
    Model model;
    Root& root = model.root;
    AmbientVolume global; // black: what the volumes are topped up with
    global.color1 = {0, 0, 0, 0};
    root.globalAmbients = {global};
    AmbientVolume sphere;
    sphere.position = {0.0f, 0.0f, 0.0f};
    sphere.start = 1.0f;
    sphere.end = 3.0f;
    sphere.color1 = {0, 0, 255, 0};
    root.ambientVolumes = {sphere};
    Group group;
    group.ambientVolumeRefs = {0};
    model.groups = {group};
    const Vector3f camera{2.0f, 0.0f, 0.0f}; // half way through the falloff

    // Half red, half the black triple: (255·0.5)/255, truncated.
    auto a = cameraAmbientColors(model, 0, camera, {}, 10.0f);
    CHECK(a[0] == Color{0, 0, 127, 255});
    CHECK(a[2] == Color{0, 0, 127, 255});
    // 5 yd from the portal: half way back, through the client's byte lerp.
    a = cameraAmbientColors(model, 0, camera, {}, 5.0f);
    CHECK(a[0].r == 63);
    // At the portal, or with no interior camera group: the placement's triple.
    CHECK(cameraAmbientColors(model, 0, camera, {}, 0.0f)[0] == Color{0, 0, 0, 255});
    // Out of reach, or in another set: only the placement's triple.
    CHECK(cameraAmbientColors(model, 0, {5.0f, 0.0f, 0.0f}, {}, 10.0f)[0] == Color{0, 0, 0, 255});
    root.ambientVolumes[0].doodadSetId = 2;
    CHECK(cameraAmbientColors(model, 0, camera, {}, 10.0f)[0] == Color{0, 0, 0, 255});
    const std::vector<u16> sets = {2};
    CHECK(cameraAmbientColors(model, 0, camera, sets, 10.0f)[0].r == 127);
}

TEST_CASE("the camera's ambient in an MBVD box weights by the nearest plane", "[wmo][runtime]") {
    Model model;
    Root& root = model.root;
    root.ambientVolumes.resize(1); // a root with MAVD, so volumes count; black
    AmbientBox box;
    // |x|, |y|, |z| <= 4, each plane facing in: n·p + d >= 0.
    const Vector3f normals[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (u32 i = 0; i < 6; ++i)
        box.planes[i] = {normals[i], 4.0f};
    box.end = 2.0f;
    box.color1 = {0, 200, 0, 0};
    root.ambientBoxes = {box};
    Group group;
    group.ambientBoxRefs = {0};
    model.groups = {group};

    // 1 yd inside the +x face with a 2 yd ramp: weight 0.5.
    CHECK(cameraAmbientColors(model, 0, {3.0f, 0.0f, 0.0f}, {}, 10.0f)[0].g == 100);
    CHECK(cameraAmbientColors(model, 0, {0.0f, 0.0f, 0.0f}, {}, 10.0f)[0].g == 200);
    CHECK(cameraAmbientColors(model, 0, {5.0f, 0.0f, 0.0f}, {}, 10.0f)[0].g == 0);
    // Without MAVG or MAVD a box alone is ignored.
    root.ambientVolumes.clear();
    root.header.ambientColor = {7, 8, 9, 10};
    CHECK(cameraAmbientColors(model, 0, {0.0f, 0.0f, 0.0f}, {}, 10.0f)[0] == Color{7, 8, 9, 10});
}

TEST_CASE("the camera's WMO fog: when there is one, and the sorted blend", "[wmo][runtime]") {
    Model model;
    Root& root = model.root;
    Fog base;
    base.fog = {100.0f, 0.5f, {0, 0, 0, 0}};
    root.fogs = {base};
    Group group;
    model.groups = {group};
    const Vector3f camera{15.0f, 0.0f, 0.0f};

    // A lone default fog does nothing; one flagged 0x1000 blends, unless 0x10000.
    CHECK_FALSE(cameraFog(model, 0, camera, {}).has_value());
    root.fogs[0].flags = 0x1000;
    CHECK(cameraFog(model, 0, camera, {}).has_value());
    root.fogs[0].flags = 0x11000;
    CHECK_FALSE(cameraFog(model, 0, camera, {}).has_value());
    root.fogs[0].flags = 0;

    Fog near;
    near.flags = 0x20;
    near.radiusStart = 10.0f;
    near.radiusEnd = 20.0f;
    near.fog = {50.0f, 0.25f, {255, 255, 255, 0}};
    root.fogs.push_back(near);
    model.groups[0]->header.fogIds = {1, 0, 0, 0};

    // Half way through the sphere's falloff: half way from MFOG[0].
    const auto fog = cameraFog(model, 0, camera, {});
    REQUIRE(fog.has_value());
    CHECK(fog->fog.end == Catch::Approx(75.0f));
    CHECK(fog->fog.startScalar == Catch::Approx(0.375f));
    CHECK(fog->fog.color == Color{126, 126, 126, 0}); // the byte lerp at 127/255
    CHECK(fog->flags == 0x20);                        // the nearest fog's
    // MFVR replaces the header ids.
    model.groups[0]->fogRefs = {0};
    CHECK(cameraFog(model, 0, camera, {})->fog.end == Catch::Approx(100.0f));
}

TEST_CASE("the camera's WMO fog: the weighted blend with MFOB boxes", "[wmo][runtime]") {
    Model model;
    Root& root = model.root;
    Fog base;
    base.flags = 0x1000;
    base.fog = {100.0f, 0.5f, {0, 0, 0, 0}};
    root.fogs = {base};
    FogBox box;
    const Vector3f normals[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (u32 i = 0; i < 6; ++i)
        box.planes[i] = {normals[i], 4.0f};
    box.fadeDistance = 2.0f;
    box.fog = {40.0f, 0.1f, {255, 255, 255, 0}};
    box.flags = 0x7;
    root.fogBoxes = {box, box};
    Group group;
    group.fogBoxRefs = {1};
    model.groups = {group};

    // 1 yd inside a 2 yd fade: half the box, half MFOG[0].
    auto fog = cameraFog(model, 0, {3.0f, 0.0f, 0.0f}, {});
    REQUIRE(fog.has_value());
    CHECK(fog->fog.end == Catch::Approx(70.0f));
    CHECK(fog->fog.color == Color{127, 127, 127, 255});
    CHECK(fog->flags == 0x7);
    // Outside the box: MFOG[0] alone, yet the box still names the flags.
    fog = cameraFog(model, 0, {9.0f, 0.0f, 0.0f}, {});
    CHECK(fog->fog.end == Catch::Approx(100.0f));
    CHECK(fog->fog.color == Color{0, 0, 0, 255});
    CHECK(fog->flags == 0x7);
}

namespace {

// Two rooms side by side, joined by a portal in the plane x = 10: group 0 over
// x in [-10, 10], group 1 over [10, 30], each a floor at z = 0 in one BSP leaf.
Model TwoRooms(u32 secondFlags) {
    Model model;
    Root& root = model.root;
    root.groups.resize(2);
    root.groups[0].bounds = {{-10.0f, -10.0f, 0.0f}, {10.0f, 10.0f, 10.0f}};
    root.groups[1].bounds = {{10.0f, -10.0f, 0.0f}, {30.0f, 10.0f, 10.0f}};
    root.groups[1].flags = secondFlags;
    root.portalVertices = {
        {10.0f, -10.0f, 0.0f}, {10.0f, 10.0f, 0.0f}, {10.0f, 10.0f, 10.0f}, {10.0f, -10.0f, 10.0f}};
    Portal portal;
    portal.vertexCount = 4;
    portal.plane = {{1.0f, 0.0f, 0.0f}, -10.0f};
    root.portals = {portal};
    PortalRef toSecond;
    toSecond.groupIndex = 1;
    toSecond.side = -1; // group 0 is on the negative side
    PortalRef toFirst;
    toFirst.groupIndex = 0;
    toFirst.side = 1;
    root.portalRefs = {toSecond, toFirst};

    const auto room = [](f32 x0, f32 x1, u32 flags, u16 portalStart) {
        Group g;
        g.header.flags = flags;
        g.header.bounds = {{x0, -10.0f, 0.0f}, {x1, 10.0f, 10.0f}};
        g.header.portalStart = portalStart;
        g.header.portalCount = 1;
        g.positions = {
            {x0, -10.0f, 0.0f}, {x1, -10.0f, 0.0f}, {x1, 10.0f, 0.0f}, {x0, 10.0f, 0.0f}};
        g.indices = {0, 1, 2, 0, 2, 3};
        BspNode leaf;
        leaf.flags = 4;
        leaf.faceCount = 2;
        g.bspNodes = {leaf};
        g.bspFaces = {0, 1};
        return g;
    };
    model.groups = {room(-10.0f, 10.0f, 0, 0), room(10.0f, 30.0f, secondFlags, 1)};
    return model;
}

Vector3f Below(const Vector3f& eye) {
    return {eye.x, eye.y, eye.z - 10000.0f};
}

} // namespace

TEST_CASE("the camera stands in the group whose surface is nearest below it", "[wmo][runtime]") {
    const Model model = TwoRooms(0x8); // the second room is exterior
    const Vector3f eye{0.0f, 0.0f, 5.0f};
    CameraLocation at = locateCamera(model, eye, Below(eye));
    REQUIRE(at.inside);
    CHECK(at.groups[0] == 0);
    CHECK(at.groups[1] == 0xFFFF);
    CHECK(at.t == Catch::Approx(5.0f / 10000.0f));
    // 10 yd from the portal into the exterior room.
    CHECK(exteriorPortalDistance(model, at, eye) == Catch::Approx(10.0f));

    // Over the exterior room: not inside, and no portal distance.
    const Vector3f out{20.0f, 0.0f, 5.0f};
    at = locateCamera(model, out, Below(out));
    CHECK(at.hit);
    CHECK_FALSE(at.inside);
    CHECK(exteriorPortalDistance(model, at, out) == 0.0f);

    // Nothing below at all.
    const Vector3f off{0.0f, 50.0f, 5.0f};
    CHECK_FALSE(locateCamera(model, off, Below(off)).hit);
}

TEST_CASE("a third of a yard from a portal into another interior group, both count",
          "[wmo][runtime]") {
    const Vector3f eye{9.8f, 0.0f, 5.0f};
    // Into an exterior room the second group is not taken.
    CameraLocation at = locateCamera(TwoRooms(0x8), eye, Below(eye));
    CHECK(at.groups[1] == 0xFFFF);
    // Into an interior one it is, and neither leads outside: no portal in reach.
    const Model inner = TwoRooms(0);
    at = locateCamera(inner, eye, Below(eye));
    CHECK(at.groups[0] == 0);
    CHECK(at.groups[1] == 1);
    CHECK(exteriorPortalDistance(inner, at, eye) == std::numeric_limits<f32>::max());
}

TEST_CASE("a portal flagged 0x1 is invisible to the camera queries", "[wmo][runtime]") {
    Model model = TwoRooms(0x8);
    model.root.portalRefs[0].flags = 1;
    const Vector3f eye{0.0f, 0.0f, 5.0f};
    const CameraLocation at = locateCamera(model, eye, Below(eye));
    REQUIRE(at.inside);
    CHECK(exteriorPortalDistance(model, at, eye) == std::numeric_limits<f32>::max());
}

namespace {

Batch Moba(u32 material, u32 start, u16 count, u16 minIndex, u16 maxIndex) {
    Batch b;
    b.materialSmall = static_cast<u8>(material);
    b.startIndex = start;
    b.indexCount = count;
    b.minIndex = minIndex;
    b.maxIndex = maxIndex;
    return b;
}

Material Blend(u32 blendMode, u32 flags = 0) {
    Material m;
    m.blendMode = blendMode;
    m.flags = flags;
    return m;
}

} // namespace

TEST_CASE("the shadow list: opaque neighbours join, alpha-key batches stand alone",
          "[wmo][runtime]") {
    Root root;
    root.materials = {Blend(0), Blend(0), Blend(1), Blend(0, 4), Blend(2)};
    Group group;
    group.batches = {Moba(0, 0, 6, 0, 3),    Moba(1, 6, 6, 4, 7),    Moba(2, 12, 3, 8, 10),
                     Moba(2, 15, 3, 11, 13), Moba(3, 18, 3, 14, 16), Moba(4, 21, 3, 17, 19),
                     Moba(0, 24, 3, 20, 22)};
    const std::vector<Batch> list = shadowBatches(root, group);
    // Blend 2 ends the list: the opaque batch after it casts nothing.
    REQUIRE(list.size() == 4);
    CHECK(list[0].startIndex == 0);
    CHECK(list[0].indexCount == 12);
    CHECK(list[0].firstVertex() == 0);
    CHECK(list[0].lastVertex() == 7);
    CHECK(list[0].flags == 0x86);
    CHECK(list[0].material() == 0);
    // Each alpha-key batch keys on its own index, so two with one material stay two.
    CHECK(list[1].startIndex == 12);
    CHECK(list[1].flags == 0x06);
    CHECK(list[2].startIndex == 15);
    CHECK(list[2].material() == 2);
    // F_UNCULLED is part of the key.
    CHECK(list[3].startIndex == 18);
    CHECK(list[3].flags == 0x86);
}

TEST_CASE("the shadow list stops at the first material flagged 0x100", "[wmo][runtime]") {
    Root root;
    root.materials = {Blend(0), Blend(0, 0x100)};
    Group group;
    group.batches = {Moba(1, 0, 3, 0, 2), Moba(0, 3, 3, 3, 5)};
    CHECK(shadowBatches(root, group).empty());
}

TEST_CASE("an LOD root's 0x400 group, or flags2 0x20, keeps the file's MOBS", "[wmo][runtime]") {
    Root root;
    root.materials = {Blend(0)};
    Group group;
    group.batches = {Moba(0, 0, 3, 0, 2)};
    group.shadowBatches = {Moba(0, 0, 3, 0, 2), Moba(0, 3, 3, 3, 5)};
    CHECK(shadowBatches(root, group).size() == 1);
    group.header.flags = 0x400;
    CHECK(shadowBatches(root, group).size() == 1);
    root.header.flags = 0x10;
    CHECK(shadowBatches(root, group).size() == 2);
    root.header.flags = 0;
    group.header.flags = 0;
    group.header.flags2 = 0x20;
    CHECK(shadowBatches(root, group).size() == 2);
}

namespace {

template <std::size_t N, typename T>
void Put(RawRecord<N>& r, std::size_t offset, T v) {
    std::memcpy(r.bytes.data() + offset, &v, sizeof(T));
}

PointLight Molp(u32 id, f32 x) {
    PointLight r;
    Put(r, 0, id);
    r.bytes[4] = 10; // B
    r.bytes[5] = 20; // G
    r.bytes[6] = 30; // R
    Put(r, 8, x);
    Put(r, 20, 1.5f); // attenStart
    Put(r, 24, 6.0f); // attenEnd
    Put(r, 28, 2.0f); // intensity
    return r;
}

} // namespace

TEST_CASE("a group's lights: set tables, MNLR and MNLD's own gates", "[wmo][runtime][lights]") {
    Model model;
    model.groups.resize(1);
    model.groups[0] = Group{};
    Group& g = *model.groups[0];
    g.pointLights = {Molp(7, 1.0f), Molp(8, 2.0f), Molp(9, 3.0f)};
    // Set 0's run is always lit; set 1's only while it is on.
    g.pointLightSets = {{0, 1}, {1, 2}};

    auto lights = gatherLights(model, 0, {});
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].source == GatheredLight::Source::Molp);
    CHECK(lights[0].id == 7);
    CHECK(lights[0].colorA.r == 30);
    CHECK(lights[0].colorB.b == 10);
    CHECK(lights[0].attenuationStart == 1.5f);
    CHECK(lights[0].attenuationEnd == 6.0f);
    CHECK(lights[0].blendFar == 6.0f);
    CHECK(lights[0].intensity == 2.0f);
    CHECK_FALSE(lights[0].spot);
    const std::array<u16, 1> set1{1};
    CHECK(gatherLights(model, 0, set1).size() == 3);

    NewLight spot;
    spot.type = 1;
    spot.lightIndex = 40;
    spot.flags = 1;
    spot.innerColor = {1, 2, 3, 255};
    spot.outerColor = {4, 5, 6, 255};
    spot.blendStart = 2.0f;
    spot.blendEnd = 5.0f;
    spot.outerAngle = 1.0f;
    NewLight setLight = spot;
    setLight.lightIndex = 41;
    setLight.doodadSet = 2;
    NewLight rtOnly = spot;
    rtOnly.flags = 4;
    NewLight area = spot;
    area.type = 2;
    model.root.newLights = {spot, setLight, rtOnly, area};
    // 9 is out of range: MNLD[0] again.
    g.newLightRefs = {0, 1, 2, 3, 9};
    lights = gatherLights(model, 0, {});
    REQUIRE(lights.size() == 3);
    const GatheredLight& n = lights[1];
    CHECK(n.source == GatheredLight::Source::Mnld);
    CHECK(n.id == 40);
    CHECK(n.spot);
    CHECK(n.colorB.b == 4);
    CHECK(n.blendNear == 2.0f);
    CHECK(n.blendFar == 5.0f);
    CHECK(lights[2].id == 40);
    const std::array<u16, 1> set2{2};
    CHECK(gatherLights(model, 0, set2).size() == 4);
    CHECK(gatherLights(model, 0, set2, true).size() == 5);
}

TEST_CASE("a light's axis is local +Z through Rz Ry Rx", "[wmo][runtime][lights]") {
    const Vector3f up = lightDirection({0.0f, 0.0f, 0.0f});
    CHECK(up.z == 1.0f);
    const Vector3f d = lightDirection({1.5707964f, 0.0f, 0.0f});
    CHECK(d.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(d.y == Catch::Approx(-1.0f));
    CHECK(d.z == Catch::Approx(0.0f).margin(1e-6));
}

TEST_CASE("a liquid: who makes one, its interior type, its depth curve", "[wmo][runtime][liquid]") {
    Group group;
    group.header.flags = 0x1000;
    CHECK(hasLiquid(group));
    group.header.flags2 = 0x80; // a split child never does
    CHECK_FALSE(hasLiquid(group));

    group.header.flags = 0x1000;
    CHECK_FALSE(liquidExterior(group, 0));
    CHECK(liquidExterior(group, 0x200)); // WMO Ocean
    group.header.flags = 0x1008;
    CHECK(liquidExterior(group, 0));
    // Basic water indoors is WMO Water - Interior; ocean, magma and slime stay.
    CHECK(liquidDrawType(13, false) == 17);
    CHECK(liquidDrawType(1, false) == 17);
    CHECK(liquidDrawType(13, true) == 13);
    CHECK(liquidDrawType(14, false) == 14);
    CHECK(liquidDrawType(19, false) == 19);

    const std::array<f32, 4> coefficients{0.5f, 0.25f, 0.125f, 0.0625f};
    CHECK(liquidDepthCurve(1, 1, coefficients) == std::array<f32, 4>{0.0f, 1.0f, 0.0f, 1.0f});
    CHECK(liquidDepthCurve(0, 2, coefficients) == coefficients);
    CHECK(liquidDepthCurve(0, 1, coefficients) == std::array<f32, 4>{0.0f, 1.0f, 0.0f, 0.0f});
    CHECK(liquidDepthCurve(0, 0, coefficients) == std::array<f32, 4>{0.0f, 6.0714f, 0.0f, 0.0f});
}

namespace {

Model LiquidModel(const Liquid& liquid) {
    Model m;
    m.root.materials.resize(1);
    m.groups.resize(1);
    m.groups[0] = Group{};
    m.groups[0]->liquid = liquid;
    return m;
}

} // namespace

TEST_CASE("a liquid's mesh: the grid, its UVs, and two triangles a tile",
          "[wmo][runtime][liquid]") {
    Liquid l;
    l.xVertices = 3;
    l.yVertices = 2;
    l.xTiles = 2;
    l.yTiles = 1;
    l.corner = {10.0f, 20.0f, 0.0f};
    l.vertices.resize(6);
    for (u32 k = 0; k < 6; ++k) {
        l.vertices[k].height = 5.0f + k;
        l.vertices[k].data = {static_cast<u8>(51 * k), 0, 0, 0};
    }
    l.tiles = {0x00, 0x0F};
    const LiquidMesh m = buildLiquidMesh(LiquidModel(l), 0, false, false, {0.0f, 1.0f, 0.0f, 0.0f});
    REQUIRE(m.vertices.size() == 6);
    CHECK(m.vertices[4].position.x == 10.0f + 4.1666665f);
    CHECK(m.vertices[4].position.y == 20.0f + 4.1666665f);
    CHECK(m.vertices[4].position.z == 9.0f);
    CHECK(m.vertices[4].uv.x == 1.0f);
    CHECK(m.vertices[4].uv.y == 1.0f);
    CHECK(m.vertices[2].depth == Catch::Approx(0.4f));
    // Only the first tile holds liquid.
    REQUIRE(m.indices == std::vector<u32>{3, 0, 4, 0, 4, 1});
    // No material record, no mesh.
    Model bare = LiquidModel(l);
    bare.root.materials.clear();
    CHECK(buildLiquidMesh(bare, 0, false, false, {}).vertices.empty());

    // World UVs, and magma's own.
    CHECK(buildLiquidMesh(LiquidModel(l), 0, false, true, {}).vertices[1].uv.x ==
          Catch::Approx((10.0f + 4.1666665f) * 0.06f));
    const i16 st[2] = {512, -256};
    std::memcpy(l.vertices[0].data.data(), st, sizeof(st));
    const LiquidMesh magma =
        buildLiquidMesh(LiquidModel(l), 0, true, false, {0.0f, 1.0f, 0.0f, 0.0f});
    CHECK(magma.vertices[0].uv.x == 2.0f);
    CHECK(magma.vertices[0].uv.y == -1.0f);
    // Its depth byte is the low byte of s.
    CHECK(magma.vertices[0].depth == 0.0f);
    // The curve's cap.
    CHECK(buildLiquidMesh(LiquidModel(l), 0, false, false, {0.0f, 6.0714f, 0.0f, 0.0f})
              .vertices[5]
              .depth == 1.0f);
}

TEST_CASE("a shared liquid tile is clipped at the portal into its neighbour",
          "[wmo][runtime][liquid]") {
    constexpr f32 C = 4.1666665f;
    Liquid l;
    l.xVertices = 2;
    l.yVertices = 2;
    l.xTiles = 1;
    l.yTiles = 1;
    l.vertices.resize(4);
    const u8 depth[4] = {0, 50, 100, 200}; // grid order: (0,0), (1,0), (0,1), (1,1)
    for (u32 k = 0; k < 4; ++k)
        l.vertices[k].data = {depth[k], 0, 0, 0};
    l.tiles = {0x80};
    Model m = LiquidModel(l);
    m.groups.resize(2);
    m.groups[1] = Group{};
    m.groups[1]->liquid = l;
    m.groups[0]->header.portalStart = 0;
    m.groups[0]->header.portalCount = 1;
    Portal portal;
    portal.plane = {{1.0f, 0.0f, 0.0f}, -C * 0.5f};
    m.root.portals = {portal};
    m.root.portalRefs = {{0, 1, 1, 0}};

    const LiquidMesh mesh = buildLiquidMesh(m, 0, false, false, {0.0f, 1.0f, 0.0f, 0.0f});
    // The grid, then the right half as a strip: (C/2, C), (C, C), (C/2, 0), (C, 0).
    REQUIRE(mesh.vertices.size() == 8);
    CHECK(mesh.indices == std::vector<u32>{4, 5, 6, 5, 6, 7});
    CHECK(mesh.vertices[4].position.x == Catch::Approx(C * 0.5f));
    CHECK(mesh.vertices[4].position.y == Catch::Approx(C));
    CHECK(mesh.vertices[5].position.x == Catch::Approx(C));
    CHECK(mesh.vertices[6].position.y == 0.0f);
    // Cut attributes lerp and truncate: (100 + 200)/2 and (50 + 0)/2.
    CHECK(mesh.vertices[4].depth == Catch::Approx(150.0f / 255.0f));
    CHECK(mesh.vertices[6].depth == Catch::Approx(25.0f / 255.0f));
    // A shared tile's UVs are its offset from the corner · 0.24.
    CHECK(mesh.vertices[5].uv.x == Catch::Approx(C * 0.24f));

    // A neighbour without liquid has an empty rectangle: no cut, the whole tile.
    m.groups[1]->liquid.reset();
    const LiquidMesh whole = buildLiquidMesh(m, 0, false, false, {0.0f, 1.0f, 0.0f, 0.0f});
    REQUIRE(whole.vertices.size() == 8);
    CHECK(whole.vertices[4].position.x == 0.0f);
    CHECK(whole.vertices[5].position.y == Catch::Approx(C));
}

namespace {

Root LodRoot(u16 flags) {
    Root root;
    root.header.flags = flags;
    root.header.lodCount = 4;
    root.header.bounds = {{0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 20.0f}};
    GroupInfo lodGroup, plain, anti;
    lodGroup.flags = 0x400 | 0x8;
    lodGroup.bounds = {{0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}};
    plain.bounds = lodGroup.bounds;
    anti.flags = 0x4000000;
    root.groups = {lodGroup, plain, anti};
    GroupInfo2 two;
    two.lodIndex = 2;
    root.groups2 = {two, GroupInfo2{}, GroupInfo2{}};
    root.materials.resize(8);
    return root;
}

} // namespace

TEST_CASE("the LOD context: levels, the far proxy, and who has objects", "[wmo][runtime][lod]") {
    const LodContext c = lodContext(LodRoot(0x10));
    CHECK(c.maxLod == 3);
    CHECK(c.special20 == 0xFF);
    CHECK(c.firstExterior == 0xFFFF);
    CHECK(c.lodIndex == std::vector<u32>{2, 1, 0});
    CHECK_FALSE(c.smallPath);
    // A non-LOD group's level 1 is a placeholder.
    const Root root = LodRoot(0x10);
    CHECK(lodHasObject(root, c, 0, 2));
    CHECK_FALSE(lodHasObject(root, c, 0, 3));
    CHECK_FALSE(lodHasObject(root, c, 1, 1));
    CHECK(lodHasObject(root, c, 1, 0));

    const LodContext far = lodContext(LodRoot(0x30));
    CHECK(far.special20 == 3);
    CHECK(far.firstExterior == 0);
    // No LOD flag: no levels at all.
    CHECK(lodContext(LodRoot(0)).maxLod == 0);
}

TEST_CASE("the LOD thresholds: 300 yd steps, a raised small scale, the horizon level",
          "[wmo][runtime][lod]") {
    const Root root = LodRoot(0x10);
    const LodContext c = lodContext(root);
    const auto t = lodThresholds(root, c, 800.0f);
    CHECK(t[0] == 0.0f);
    CHECK(t[1] == 300.0f);
    CHECK(t[2] == 525.0f);
    CHECK(t[3] == 900.0f);
    CHECK(t[4] == 1200.0f);
    // A half-scale placement is raised until 300·s reaches 200.
    CHECK(lodThresholds(root, c, 800.0f, 64)[1] == Catch::Approx(200.0f));
    // MOHD 0x20: the far proxy's level starts at the horizon.
    const Root farRoot = LodRoot(0x30);
    const auto f = lodThresholds(farRoot, lodContext(farRoot), 800.0f);
    CHECK(f[3] == 800.0f);
    CHECK(f[4] == 1200.0f);
}

TEST_CASE("a group's LOD by its box's distance, capped by what it has", "[wmo][runtime][lod]") {
    Model model;
    model.root = LodRoot(0x10);
    const LodContext c = lodContext(model.root);
    const std::array<f32, 5> t{0.0f, 300.0f, 525.0f, 900.0f, 1200.0f};
    const Box box{{0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}};
    CHECK(distanceLod(box, {410.0f, 5.0f, 5.0f}, t, 3) == 1);
    CHECK(distanceLod(box, {1010.0f, 5.0f, 5.0f}, t, 3) == 3);
    CHECK(distanceLod(box, {1010.0f, 5.0f, 5.0f}, t, 3, 2) == 2);
    CHECK(distanceLod(box, {1010.0f, 5.0f, 5.0f}, t, 0) == 0);
    CHECK(distanceLod(box, {5.0f, 5.0f, 5.0f}, t, 3) == 0);

    const LodTargets far = lodTargets(model, c, {1010.0f, 5.0f, 5.0f}, false, 800.0f);
    // The LOD group to its last level, the plain one to its placeholder, the
    // antiportal pinned.
    CHECK(far.target == std::vector<u32>{2, 1, 0});
    CHECK(far.sdfd == 300);
    // From inside, every group at LOD 0.
    CHECK(lodTargets(model, c, {1010.0f, 5.0f, 5.0f}, true, 800.0f).target ==
          std::vector<u32>{0, 0, 0});
}

TEST_CASE("the emissive fade from the group fade distance", "[wmo][runtime][lod]") {
    const Vector2f f = emissiveFade(300);
    const f32 s = 1.0f / (0.99f * 300.0f - 0.85f * 300.0f);
    CHECK(f.x == -s);
    CHECK(f.y == s * (0.99f * 300.0f));
    CHECK(emissiveFade(0).x == 0.0f);
    CHECK(emissiveFade(0).y == 1.0f);
}

TEST_CASE("CRandom's first draw from seed 0, from its table", "[wmo][runtime][mddl]") {
    // idx 0 steps to (184, 200, 212, 216): T[54] ^ ror(T[53], 29) ^ ror(T[50], 30) ^ ror(T[46],
    // 31).
    CRandom rng(0);
    CHECK(rng.next() == 0x16BE8AA9u);
    CRandom a(1234), b(1234);
    for (int i = 0; i < 100; ++i)
        CHECK(a.next() == b.next());
}

namespace {

// A version-1 MDDL: density 2, flags 1, one layer (threshold 23: every draw
// stays) of id 5 weight 3 and an empty slot weight 1, and group 0's block:
// layer 0, batch 0 whole, end of batches, end of layers.
std::vector<u8> DetailChunk() {
    std::vector<u8> d = {0xDD, 0xFF, 0x01, 0x00};
    const f32 density = 2.0f;
    d.insert(d.end(), reinterpret_cast<const u8*>(&density),
             reinterpret_cast<const u8*>(&density) + 4);
    d.push_back(0x01);
    d.insert(d.end(), {0x01, 0x00, 23, 2, 5, 0, 0, 0, 3, 0, 0, 0, 0, 1});
    const std::vector<u8> block = {0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF};
    d.insert(d.end(), {0x00, 0x00, static_cast<u8>(block.size()), 0x00, 0x00, 0x00});
    d.insert(d.end(), block.begin(), block.end());
    return d;
}

Group DetailGroup() {
    Group g;
    g.positions = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 10.0f, 0.0f}};
    g.indices = {0, 1, 2};
    Batch b;
    b.startIndex = 0;
    b.indexCount = 3;
    g.batches = {b};
    return g;
}

} // namespace

TEST_CASE("MDDL parses as 12.1 reads it", "[wmo][runtime][mddl]") {
    Root root;
    root.groups.resize(2);
    root.detailDoodads = DetailChunk();
    const auto data = parseDetailDoodads(root);
    REQUIRE(data);
    CHECK(data->density == 2.0f);
    CHECK(data->flags == 1);
    REQUIRE(data->layers.size() == 1);
    CHECK(data->layers[0].threshold == 23);
    CHECK(data->layers[0].totalWeight == 4);
    CHECK(data->blocks == std::vector<i64>{static_cast<i64>(DetailChunk().size() - 8), -1});

    // A block naming a group past the root's count throws the chunk away, as
    // does an empty layer list.
    Root one;
    one.groups.resize(0);
    one.detailDoodads = DetailChunk();
    CHECK_FALSE(parseDetailDoodads(one));
    Root none;
    none.groups.resize(1);
    none.detailDoodads = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00};
    CHECK_FALSE(parseDetailDoodads(none));
}

TEST_CASE("a whole batch's detail doodads land on its triangle, sorted", "[wmo][runtime][mddl]") {
    Root root;
    root.groups.resize(2);
    root.detailDoodads = DetailChunk();
    const auto data = parseDetailDoodads(root);
    REQUIRE(data);
    const Group group = DetailGroup();
    const auto locs = detailDoodadLocs(root, *data, group, 0);
    // Area 50 at density 2: 50·5.5296·4 = 1105.92 slots, 1105 or 1106 kept
    // before the layer's empty quarter.
    CHECK(locs.size() > 700);
    CHECK(locs.size() < 1000);
    for (const DetailDoodadLoc& l : locs) {
        CHECK(l.triangle == 0);
        CHECK(l.id == 5);
        CHECK(static_cast<u32>(l.u) + l.v <= 0xFFFFu);
    }
    CHECK(detailDoodadLocs(root, *data, group, 1).empty());
    Group wide = group;
    wide.wideIndices = true;
    CHECK(detailDoodadLocs(root, *data, wide, 0).empty());
}

TEST_CASE("WmoMaxScale keeps the leading (maxScale/density)^2", "[wmo][runtime][mddl]") {
    std::vector<DetailDoodadLoc> locs(100);
    CHECK(capDetailDoodads(locs, 2.0f, 1.0f) == 1.0f);
    CHECK(locs.size() == 25);
    CHECK(capDetailDoodads(locs, 2.0f, 3.0f) == 2.0f);
    CHECK(locs.size() == 25);
    CHECK(capDetailDoodads(locs, 2.0f, std::nullopt) == 2.0f);
}

TEST_CASE("a detail doodad instance: the fixed bytes, the colourless default",
          "[wmo][runtime][mddl]") {
    const Group group = DetailGroup();
    const DetailDoodadLoc loc{0, 0x4000, 0x2000, 5};
    const DetailDoodadModel model{false, 0.5f, 1.0f, 1.0f, 0.0f, 0.0f};
    const auto inst = detailDoodadInstance(group, 0, {}, loc, model);
    REQUIRE(inst);
    // Flat and facing +Z: (0, 0, 1) packs to 127, 127, 255.
    CHECK(inst->normal == std::array<u8, 3>{127, 127, 255});
    CHECK(inst->amount == 127);
    CHECK(inst->fixed == std::array<u8, 4>{0x7F, 0x7F, 0x7F, 0xFF});
    CHECK(inst->color == std::array<u8, 4>{0x00, 0x00, 0x00, 0xFF});
    CHECK(inst->axis == std::array<u8, 3>{0x7F, 0x7F, 0xFF});
    CHECK(inst->scale == 127);
    // u weighs P0, v weighs P1 (x), the rest P2 (y).
    CHECK(inst->position.x == Catch::Approx(10.0f * 0x2000 / 65535.0f));
    CHECK(inst->position.y == Catch::Approx(10.0f * (0xFFFF - 0x6000) / 65535.0f));
    CHECK_FALSE(detailDoodadInstance(group, 0, {}, {1, 0, 0, 5}, model));
}
