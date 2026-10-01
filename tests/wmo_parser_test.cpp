// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The WMO reader against hand-built files: each case writes the chunks 12.1's walkers accept
/// (`CMapObj_ReadRootChunks` 0x141A7EC80, `CMapObjGroup_ReadChunks` 0x141A826D0) and checks the
/// parse keeps what the client keeps, including the quirks — last-chunk-wins counts, the MOPY
/// widening, MOCV's slot rule and a group walk that ignores MOGP's own size.

#include <catch2/catch_all.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <whiteout/interfaces.h>
#include <whiteout/models/wow/wmo/wmo.h>

using namespace whiteout;
using namespace whiteout::models::wow::wmo;

namespace {

template <typename T>
void append(std::vector<u8>& out, const T& value) {
    const auto* p = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
std::vector<u8> bytesOf(const std::vector<T>& values) {
    std::vector<u8> out;
    for (const T& v : values)
        append(out, v);
    return out;
}

std::vector<u8> chunk(u32 tag, const std::vector<u8>& payload) {
    std::vector<u8> out;
    append(out, tag);
    append(out, static_cast<u32>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void add(std::vector<u8>& file, const std::vector<u8>& piece) {
    file.insert(file.end(), piece.begin(), piece.end());
}

std::vector<u8> version(u32 v = 17) {
    std::vector<u8> p;
    append(p, v);
    return chunk(makeTag("MVER"), p);
}

std::vector<u8> header(u32 groupCount, u16 flags = 0, u8 lodCount = 0) {
    Header h;
    h.groupCount = groupCount;
    h.flags = flags;
    h.lodCount = lodCount;
    h.ambientColor = Color{10, 20, 30, 255};
    std::vector<u8> p;
    append(p, h);
    return chunk(makeTag("MOHD"), p);
}

// A group file: MVER, then MOGP holding the header and @p subchunks.
std::vector<u8> groupFile(const GroupHeader& h, const std::vector<u8>& subchunks,
                          u32 mogpSize = 0xFFFFFFFF) {
    std::vector<u8> file = version();
    std::vector<u8> payload;
    append(payload, h);
    payload.insert(payload.end(), subchunks.begin(), subchunks.end());
    append(file, makeTag("MOGP"));
    append(file, mogpSize == 0xFFFFFFFF ? static_cast<u32>(payload.size()) : mogpSize);
    add(file, payload);
    return file;
}

/// Group files by FileDataID.
class MapFs final : public interfaces::CascFileSystem {
public:
    std::map<u32, std::vector<u8>> files;

    std::vector<u8> readFile(u32 fileId) const override {
        auto it = files.find(fileId);
        return it == files.end() ? std::vector<u8>{} : it->second;
    }
    std::optional<u32> reserveFileId(const std::string&) override {
        return std::nullopt;
    }
    bool writeFile(u32, const std::vector<u8>&) override {
        return false;
    }
    bool fileExists(u32 fileId) const override {
        return files.count(fileId) != 0;
    }
};

class PathFs final : public interfaces::VirtualPathFileSystem {
public:
    std::map<std::string, std::vector<u8>> files;

    std::vector<u8> readFile(const std::string& path) const override {
        auto it = files.find(path);
        return it == files.end() ? std::vector<u8>{} : it->second;
    }
    bool writeFile(const std::string&, const std::vector<u8>&) override {
        return false;
    }
    bool fileExists(const std::string& path) const override {
        return files.count(path) != 0;
    }
    std::vector<interfaces::DirectoryEntry> listDirectory(const std::string&) const override {
        return {};
    }
};

} // namespace

TEST_CASE("a chunk id is the file's four bytes read reversed", "[wmo]") {
    // "MVER" is stored R E V M.
    const std::vector<u8> onDisk = {'R', 'E', 'V', 'M'};
    u32 value = 0;
    std::memcpy(&value, onDisk.data(), 4);
    CHECK(value == makeTag("MVER"));
}

TEST_CASE("a root file reads every table it carries", "[wmo]") {
    std::vector<u8> file = version();
    add(file, header(2, 0x0001 | 0x0010, 3));

    Material m;
    m.flags = 0x10;
    m.shader = 6;
    m.blendMode = 1;
    m.texture0 = 1001;
    m.texture1 = 1002;
    m.sidnColor = Color{1, 2, 3, 4};
    m.textureExtra[5] = 1009;
    add(file, chunk(makeTag("MOMT"), bytesOf(std::vector<Material>{m, Material{}})));

    add(file, chunk(makeTag("MOGN"), {'a', 0, 'h', 'a', 'l', 'l', 0}));
    GroupInfo g0;
    g0.flags = 0x8;
    g0.nameOffset = 2;
    GroupInfo g1;
    g1.flags = 0x2000;
    g1.nameOffset = -1;
    add(file, chunk(makeTag("MOGI"), bytesOf(std::vector<GroupInfo>{g0, g1})));

    DoodadSet set;
    std::memcpy(set.name.data(), "Set_$DefaultGlobal", 18);
    set.count = 1;
    add(file, chunk(makeTag("MODS"), bytesOf(std::vector<DoodadSet>{set})));
    add(file, chunk(makeTag("MODI"), bytesOf(std::vector<u32>{555})));
    DoodadDef def;
    def.nameAndFlags = 0x04000000u;
    def.scale = 2.0f;
    add(file, chunk(makeTag("MODD"), bytesOf(std::vector<DoodadDef>{def})));
    add(file, chunk(makeTag("GFID"), bytesOf(std::vector<u32>{7001, 7002})));
    add(file, chunk(makeTag("MOSB"), {0, 0, 0, 0}));

    Parser parser;
    const auto root = parser.parseRoot(file);
    REQUIRE(root);
    CHECK(root->version == 17);
    CHECK(root->header.groupCount == 2);
    CHECK(root->header.lodCount == 3);
    CHECK(hasFlag(root->header.flags, RootFlag::NoTransitionAttenuation));
    CHECK(root->header.ambientColor == Color{10, 20, 30, 255});
    REQUIRE(root->materials.size() == 2);
    CHECK(root->materials[0].shader == 6);
    CHECK(root->materials[0].texture(0) == 1001);
    CHECK(root->materials[0].texture(1) == 1002);
    CHECK(root->materials[0].texture(8) == 1009);
    CHECK(root->materials[0].sidnColor == Color{1, 2, 3, 4});
    CHECK(root->groupCount() == 2);
    CHECK(root->groupName(0) == "hall");
    CHECK(root->groupName(1).empty());
    REQUIRE(root->doodadSets.size() == 1);
    CHECK(root->doodadSets[0].nameView() == "Set_$DefaultGlobal");
    REQUIRE(root->doodadDefs.size() == 1);
    CHECK(root->doodadDefs[0].flags() == 0x04);
    CHECK(root->doodadDefs[0].scale == 2.0f);
    CHECK(root->groupFileId(1, 0) == 7002);
    REQUIRE(root->skyboxName);
    CHECK(root->skyboxName->empty());
    CHECK_FALSE(root->textureNames);
    CHECK_FALSE(parser.hasIssues());
}

TEST_CASE("the root load fails where 12.1's does", "[wmo]") {
    Parser parser;
    SECTION("a version other than 17") {
        std::vector<u8> file = version(16);
        add(file, header(0));
        CHECK_FALSE(parser.parseRoot(file));
    }
    SECTION("no MOHD") {
        std::vector<u8> file = version();
        add(file, chunk(makeTag("MOGN"), {0}));
        CHECK_FALSE(parser.parseRoot(file));
    }
    SECTION("a chunk that runs past the end") {
        std::vector<u8> file = version();
        add(file, header(0));
        append(file, makeTag("MOMT"));
        append(file, u32{64});
        CHECK_FALSE(parser.parseRoot(file));
    }
    SECTION("trailing bytes the walk cannot land on") {
        std::vector<u8> file = version();
        add(file, header(0));
        file.push_back(0);
        CHECK_FALSE(parser.parseRoot(file));
    }
    SECTION("MVER is optional") {
        std::vector<u8> file = header(0);
        const auto root = parser.parseRoot(file);
        REQUIRE(root);
        CHECK(root->version == 0);
    }
}

TEST_CASE("MOGI and MGI2 share one group count, the later chunk's", "[wmo]") {
    std::vector<u8> file = version();
    add(file, header(3));
    add(file, chunk(makeTag("MOGI"), bytesOf(std::vector<GroupInfo>(3))));
    add(file, chunk(makeTag("MGI2"), bytesOf(std::vector<GroupInfo2>(2))));
    Parser parser;
    auto root = parser.parseRoot(file);
    REQUIRE(root);
    CHECK(root->groupCount() == 2);

    std::vector<u8> reversed = version();
    add(reversed, header(3));
    add(reversed, chunk(makeTag("MGI2"), bytesOf(std::vector<GroupInfo2>(2))));
    add(reversed, chunk(makeTag("MOGI"), bytesOf(std::vector<GroupInfo>(3))));
    root = parser.parseRoot(reversed);
    REQUIRE(root);
    CHECK(root->groupCount() == 3);
}

TEST_CASE("MOTX makes texture slots name offsets", "[wmo]") {
    std::vector<u8> file = version();
    add(file, header(0));
    add(file, chunk(makeTag("MOTX"), {'a', '.', 'b', 'l', 'p', 0, 0, 0}));
    Parser parser;
    const auto root = parser.parseRoot(file);
    REQUIRE(root);
    REQUIRE(root->textureNames);
    CHECK(root->textureName(0) == "a.blp");
    CHECK(root->textureName(5).empty());
    CHECK(root->textureName(99).empty());
}

TEST_CASE("a group file reads its geometry and the client's quirks", "[wmo]") {
    GroupHeader h;
    h.flags = 0x4; // the first MOCV is colour set 0
    h.transBatchCount = 1;
    h.fogIds = {1, 2, 3, 4};

    std::vector<u8> sub;
    add(sub, chunk(makeTag("MOPY"), {0x20, 0, 0x08, 0xFF}));
    add(sub, chunk(makeTag("MOVI"), bytesOf(std::vector<u16>{0, 1, 2, 2, 1, 3})));
    const std::vector<Vector3f> pos = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    add(sub, chunk(makeTag("MOVT"), bytesOf(pos)));
    add(sub, chunk(makeTag("MONR"), bytesOf(std::vector<Vector3f>(4, {0, 0, 1}))));
    add(sub, chunk(makeTag("MOTV"), bytesOf(std::vector<Vector2f>(4, {0.5f, 0.5f}))));
    add(sub, chunk(makeTag("MOTV"), bytesOf(std::vector<Vector2f>(4, {1.0f, 1.0f}))));
    Batch b;
    b.startIndex = 0;
    b.indexCount = 6;
    b.minIndex = 0;
    b.maxIndex = 3;
    b.flags = 0x2;
    b.materialLarge = 300;
    b.materialSmall = 9;
    add(sub, chunk(makeTag("MOBA"), bytesOf(std::vector<Batch>{b})));
    add(sub, chunk(makeTag("MOCV"), bytesOf(std::vector<Color>(4, Color{1, 1, 1, 1}))));
    add(sub, chunk(makeTag("MOCV"), bytesOf(std::vector<Color>(4, Color{2, 2, 2, 2}))));
    add(sub, chunk(makeTag("MORI"), {1, 2, 3, 4}));

    // MOGP's size is ignored: the walk runs to the end of the file.
    const std::vector<u8> file = groupFile(h, sub, 68);
    Parser parser;
    const auto group = parser.parseGroup(file);
    REQUIRE(group);
    CHECK(group->version == 17);
    CHECK(group->header.transBatchCount == 1);
    REQUIRE(group->polys.size() == 2);
    CHECK(group->polysFromMopy);
    CHECK(group->polys[0].flags == 0x20);
    CHECK(group->polys[0].material == 0);
    CHECK(group->polys[1].material == 0xFFFF);
    CHECK(group->indices == std::vector<u32>{0, 1, 2, 2, 1, 3});
    CHECK_FALSE(group->wideIndices);
    CHECK(group->positions.size() == 4);
    REQUIRE(group->uvSets.size() == 2);
    CHECK(group->uvSets[1][0].x == 1.0f);
    REQUIRE(group->batches.size() == 1);
    CHECK(group->batches[0].material() == 300);
    CHECK(group->batches[0].lastVertex() == 3);
    REQUIRE(group->colorSet0());
    CHECK((*group->colorSet0())[0] == Color{1, 1, 1, 1});
    REQUIRE(group->colorSet1());
    CHECK((*group->colorSet1())[0] == Color{2, 2, 2, 2});
    CHECK_FALSE(parser.hasIssues());
}

TEST_CASE("without flag 4 every MOCV is colour set 1, the last one winning", "[wmo]") {
    GroupHeader h;
    std::vector<u8> sub;
    add(sub, chunk(makeTag("MOCV"), bytesOf(std::vector<Color>(2, Color{1, 1, 1, 1}))));
    add(sub, chunk(makeTag("MOCV"), bytesOf(std::vector<Color>(2, Color{2, 2, 2, 2}))));
    Parser parser;
    const auto group = parser.parseGroup(groupFile(h, sub));
    REQUIRE(group);
    CHECK_FALSE(group->colorSet0());
    REQUIRE(group->colorSet1());
    CHECK((*group->colorSet1())[0] == Color{2, 2, 2, 2});
}

TEST_CASE("MOBA flag 4 widens the vertex range with the leading bytes", "[wmo]") {
    Batch b;
    b.head[0] = 1;
    b.head[1] = 2;
    b.minIndex = 5;
    b.maxIndex = 6;
    b.materialSmall = 7;
    CHECK(b.firstVertex() == 5);
    CHECK(b.material() == 7);
    b.flags = 0x4;
    CHECK(b.firstVertex() == 0x10005);
    CHECK(b.lastVertex() == 0x20006);
}

TEST_CASE("MPY2, MOVX and MOGX", "[wmo]") {
    GroupHeader h;
    std::vector<u8> sub;
    std::vector<u8> mogx;
    append(mogx, u32{12});
    mogx.push_back(0x2A);
    mogx.resize(256);
    add(sub, chunk(makeTag("MOGX"), mogx));
    add(sub, chunk(makeTag("MPY2"), bytesOf(std::vector<Poly>{{0x100, 700}})));
    add(sub, chunk(makeTag("MOVX"), bytesOf(std::vector<u32>{70000, 1, 2})));
    add(sub, chunk(makeTag("MOQG"), bytesOf(std::vector<u32>{3})));
    Parser parser;
    const auto group = parser.parseGroup(groupFile(h, sub));
    REQUIRE(group);
    REQUIRE(group->queryFaceStart);
    CHECK(*group->queryFaceStart == 12);
    CHECK(group->detailDoodadKey == 0x2A);
    REQUIRE(group->polys.size() == 1);
    CHECK_FALSE(group->polysFromMopy);
    CHECK(group->polys[0].material == 700);
    CHECK(group->wideIndices);
    CHECK(group->indices[0] == 70000);
    CHECK(group->groundTypes == std::vector<u32>{3});
}

TEST_CASE("MOTA is indexed by the batches read before it", "[wmo]") {
    GroupHeader h;
    std::vector<u8> sub;
    add(sub, chunk(makeTag("MOBA"), bytesOf(std::vector<Batch>(2))));
    std::vector<u8> mota = bytesOf(std::vector<u16>{0, 0xFFFF});
    const std::vector<u8> t = bytesOf(std::vector<Vector4f>{{1, 0, 0, 1}, {0, 1, 0, -1}});
    mota.insert(mota.end(), t.begin(), t.end());
    add(sub, chunk(makeTag("MOTA"), mota));
    Parser parser;
    const auto group = parser.parseGroup(groupFile(h, sub));
    REQUIRE(group);
    REQUIRE(group->tangents);
    CHECK(group->tangents->firstTangent == std::vector<u16>{0, 0xFFFF});
    REQUIRE(group->tangents->tangents.size() == 2);
    CHECK(group->tangents->tangents[1].w == -1.0f);
}

TEST_CASE("MLIQ reads its unaligned grid", "[wmo]") {
    GroupHeader h;
    std::vector<u8> mliq;
    append(mliq, i32{2});
    append(mliq, i32{2});
    append(mliq, i32{1});
    append(mliq, i32{1});
    append(mliq, Vector3f{10, 20, 30});
    append(mliq, u16{3});
    for (int i = 0; i < 4; ++i) {
        LiquidVertex v;
        v.data = {static_cast<u8>(i), 0, 0, 0};
        v.height = 30.0f + static_cast<f32>(i);
        append(mliq, v);
    }
    mliq.push_back(0x0F);
    std::vector<u8> sub;
    add(sub, chunk(makeTag("MLIQ"), mliq));
    Parser parser;
    const auto group = parser.parseGroup(groupFile(h, sub));
    REQUIRE(group);
    REQUIRE(group->liquid);
    CHECK(group->liquid->material == 3);
    CHECK(group->liquid->corner.y == 20.0f);
    REQUIRE(group->liquid->vertices.size() == 4);
    CHECK(group->liquid->vertices[3].height == 33.0f);
    CHECK(group->liquid->tiles == std::vector<u8>{0x0F});
}

TEST_CASE("a model reads its groups by GFID, or by name from a path", "[wmo]") {
    std::vector<u8> root = version();
    add(root, header(2));
    add(root, chunk(makeTag("MOGI"), bytesOf(std::vector<GroupInfo>(2))));
    add(root, chunk(makeTag("GFID"), bytesOf(std::vector<u32>{501, 502})));

    GroupHeader a;
    a.uniqueId = 1;
    GroupHeader b;
    b.uniqueId = 2;

    MapFs ids;
    ids.files[501] = groupFile(a, {});
    ids.files[502] = groupFile(b, {});
    Parser parser;
    auto model = parser.parse(ids, root);
    REQUIRE(model);
    REQUIRE(model->groups.size() == 2);
    REQUIRE(model->groups[1]);
    CHECK(model->groups[1]->header.uniqueId == 2);

    PathFs paths;
    paths.files["world/wmo/x/house.wmo"] = root;
    paths.files["world/wmo/x/house_000.wmo"] = groupFile(a, {});
    model = parser.parse(paths, "world/wmo/x/house.wmo");
    REQUIRE(model);
    CHECK(model->groups[0]);
    CHECK_FALSE(model->groups[1]);
    CHECK(parser.hasIssues());
}

TEST_CASE("the listfile's group names", "[wmo]") {
    CHECK(groupFilePath("world/wmo/a/b.wmo", 3, 0) == "world/wmo/a/b_003.wmo");
    CHECK(groupFilePath("world/wmo/a/B.WMO", 12, 2) == "world/wmo/a/B_012_lod2.wmo");
}

TEST_CASE("root and group files are told apart by the chunk after MVER", "[wmo]") {
    std::vector<u8> root = version();
    add(root, header(0));
    CHECK(detectFileKind(root) == FileKind::Root);
    CHECK(detectFileKind(groupFile(GroupHeader{}, {})) == FileKind::Group);
    CHECK(detectFileKind(std::vector<u8>{'M', 'D', '2', '1'}) == FileKind::Unknown);
}
