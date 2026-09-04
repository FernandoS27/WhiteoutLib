// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P5 — `M3Converter` on hand-built models.
///
/// Four claims a green parse cannot check: the profile follows the `MODL`
/// version, the SC2 -> Blizzard rebase is the exact one the design specifies
/// (and is bit-exact both ways), a region's faces are region-local, and a
/// vertex's bone index goes through its region's `boneLookup` window.

#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/converters.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

/// The `.m3` vertex layout for `UV1` and nothing else: 32 bytes, position at 0,
/// weights at 12, indices at 16, normal UNORM at 20, uv i16 at 24.
constexpr std::size_t kStride = 32;

void pushVertex(std::vector<u8>& blob, const Vector3f& position, u8 boneSlot) {
    const std::size_t base = blob.size();
    blob.resize(base + kStride, 0);
    std::memcpy(blob.data() + base, &position, sizeof(Vector3f));
    blob[base + 12] = 255;      // weight 0 = 1.0
    blob[base + 16] = boneSlot; // index 0, region-local
    blob[base + 20] = 128;      // normal ~ (0, 0, 1) in UNORM
    blob[base + 21] = 128;
    blob[base + 22] = 255;
    blob[base + 23] = 255;
}

/// Two bones, a quad, one region whose bone-lookup window names bone 1 only.
m3::Model makeModel(u32 version) {
    m3::Model model;
    model.setVersion(static_cast<i32>(version));
    model.name = "unit";
    model.bounds.min = Vector3f{-1, -1, 0};
    model.bounds.max = Vector3f{1, 1, 0};

    m3::Bone root;
    root.name = "root";
    root.parentIndex = 0xFFFF;
    // What a shipped bone actually carries. The three `Inherit` bits are set on
    // NO bone in the corpus — 0 of 50,771 across 4,000 files — so a fixture
    // that sets them is the one shape the mapping below cannot get wrong.
    root.flags = m3::BoneFlag::Real | m3::BoneFlag::Animated | m3::BoneFlag::Skinned;
    root.scale.initValue = Vector3f{1, 1, 1};
    root.rotation.initValue = Quaternion{0, 0, 0, 1};
    m3::Bone child = root;
    child.name = "child";
    child.parentIndex = 0;
    // In SC2's basis: 3 units "forward" is -Y.
    child.position.initValue = Vector3f{0, -3, 0};
    model.bones.push_back(root);
    model.bones.push_back(child);

    std::vector<u8> blob;
    const Vector3f corners[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (const Vector3f& corner : corners) {
        pushVertex(blob, corner, 0);
    }
    model.vertices.flags = m3::VertexFormatFlag::UV1;
    model.vertices.data = std::move(blob);
    model.vertices.initialize();

    // Slot 0 of this region's window is bone 1 — a converter reading the vertex
    // byte as a bone id would skin the quad to bone 0 instead.
    model.boneLookup = {1};

    m3::MeshDivision division;
    division.faces = {0, 1, 2, 0, 2, 3};
    m3::Region region;
    region.firstVertex = 0;
    region.vertexCount = 4;
    region.firstIndex = 0;
    region.indexCount = 6;
    region.firstBoneLookup = 0;
    region.boneLookupCount = 1;
    region.rootBone = 0;
    division.regions.push_back(region);

    m3::Batch batch;
    batch.regionIndex = 0;
    batch.materialIndex = 0;
    batch.boneCount = 0xFFFF;
    division.batches.push_back(batch);
    model.divisions.push_back(division);

    m3::StandardMaterial material;
    material.name = "body";
    m3::TextureLayer diffuse;
    diffuse.texturePath = "assets/textures/body.dds";
    material.diffuseLayer = diffuse;
    model.standardMaterials.push_back(material);

    m3::MaterialMap map;
    map.materialType = m3::MaterialType::Standard;
    map.materialIndex = 0;
    model.materialMaps.push_back(map);

    return model;
}

} // namespace

TEST_CASE("wem m3 bone flags stay native", "[wem][convert][m3][nodes]") {
    const M3Converter converter;
    Result<Document> imported = converter.fromM3(makeModel(25));
    REQUIRE(imported.ok());
    const NodeTree& tree = imported->models.front().nodes;
    REQUIRE(tree.nodes.size() >= 2);

    // `BONE.flags` says nothing `NodeFlags` can carry. StarCraft II reads two
    // bits out of it (both dead), never the three `Inherit` ones — and those
    // three are set on no shipped bone, so deriving `DontInherit*` from their
    // absence hands every bone of every `.m3` all three suppressions. A
    // pivot-relative target honours them, which detached every bone from its
    // parent the moment one was opened as Warcraft III.
    for (const Node& node : tree.nodes) {
        CHECK(node.flags == NodeFlags::None);
    }
    // The word itself survives, under a name only the M3 converter answers to.
    CHECK(tree.nodes[0].native.value("m3FlagBits") ==
          static_cast<i64>(static_cast<u32>(m3::BoneFlag::Real | m3::BoneFlag::Animated |
                                            m3::BoneFlag::Skinned)));
    CHECK(tree.nodes[0].native.find("flagBits") == nullptr);

    Result<m3::Model> exported = converter.toM3(*imported, ProfileId::Sc2);
    REQUIRE(exported.ok());
    REQUIRE(exported->bones.size() >= 2);
    CHECK(exported->bones[0].flags ==
          (m3::BoneFlag::Real | m3::BoneFlag::Animated | m3::BoneFlag::Skinned));
}

TEST_CASE("wem m3 picks its profile from the MODL version", "[wem][convert][m3]") {
    CHECK(M3Converter::ProfileForVersion(23) == ProfileId::Sc2);
    CHECK(M3Converter::ProfileForVersion(29) == ProfileId::Sc2);
    CHECK(M3Converter::ProfileForVersion(30) == ProfileId::Heroes);

    const M3Converter converter;
    Result<Document> sc2 = converter.fromM3(makeModel(25));
    REQUIRE(sc2.ok());
    CHECK(sc2->defaultProfile == ProfileId::Sc2);

    Result<Document> heroes = converter.fromM3(makeModel(30));
    REQUIRE(heroes.ok());
    CHECK(heroes->defaultProfile == ProfileId::Heroes);

    // A caller who knows better wins: content moves between the two games and a
    // version number is a signal, not a proof.
    Result<Document> forced = converter.fromM3(makeModel(30), ProfileId::Sc2);
    REQUIRE(forced.ok());
    CHECK(forced->defaultProfile == ProfileId::Sc2);
}

TEST_CASE("wem m3 rebases into the canonical space", "[wem][convert][m3][space]") {
    const M3Converter converter;
    Result<Document> result = converter.fromM3(makeModel(30));
    REQUIRE(result.ok());
    CHECK(result->space == CoordSpace::Blizzard);

    const NodeTree& nodes = result->models.front().nodes;
    REQUIRE(nodes.size() == 2);
    // SC2's (0, -3, 0) -- three units along its forward -- is (3, 0, 0) in
    // Blizzard's basis, where forward is +X.
    CHECK(nodes.nodes[1].local.translation.x == 3.0f);
    CHECK(nodes.nodes[1].local.translation.y == 0.0f);
}

TEST_CASE("wem m3 skinning goes through the region's bone lookup", "[wem][convert][m3][skin]") {
    const M3Converter converter;
    Result<Document> result = converter.fromM3(makeModel(30));
    REQUIRE(result.ok());
    const Mesh& mesh = result->models.front().meshes[0];

    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.faceCount() == 2);
    const auto influences = mesh.skin.forVertex(0);
    REQUIRE(influences.size() == 1);
    // Slot 0 of the window, not bone 0.
    CHECK(influences[0].bone == 1);
    CHECK(influences[0].weight == 1.0f);
}

TEST_CASE("wem m3 interns texture paths as it imports", "[wem][convert][m3][textures]") {
    const M3Converter converter;
    Result<Document> result = converter.fromM3(makeModel(30));
    REQUIRE(result.ok());

    // The document's texture table is built by the material import, because the
    // layers a material reads are the only enumeration of them there is.
    REQUIRE(result->textures.size() == 1);
    CHECK(result->textures[0].path == "assets/textures/body.dds");
    REQUIRE(result->models.front().materialSlots.size() == 1);
    CHECK(Resolve(result->models.front(), 0, ProfileId::Heroes) != nullptr);
}

TEST_CASE("wem m3 round trip returns to the source basis", "[wem][convert][m3]") {
    const M3Converter converter;
    Result<Document> imported = converter.fromM3(makeModel(30));
    REQUIRE(imported.ok());

    Result<m3::Model> exported = converter.toM3(*imported, ProfileId::Heroes, 30);
    REQUIRE(exported.ok());
    const m3::Model& out = *exported;

    REQUIRE(out.bones.size() == 2);
    // Bit-exact: the rebase is an axis permutation with sign flips, so a round
    // trip is not "close enough", it is equal.
    CHECK(out.bones[1].position.initValue.y == -3.0f);
    CHECK(out.bones[1].position.initValue.x == 0.0f);

    REQUIRE(out.divisions.size() == 1);
    CHECK(out.divisions[0].faces.size() == 6);
    REQUIRE(out.divisions[0].regions.size() == 1);
    CHECK(out.divisions[0].regions[0].vertexCount == 4);
    CHECK(out.vertices.vertexCount() == 4);

    const std::vector<Vector3f> positions = out.vertices.getPositions();
    REQUIRE(positions.size() == 4);
    CHECK(positions[0].x == 0.0f);
    CHECK(positions[0].y == 0.0f);
}

TEST_CASE("wem m3 carries IREF through the document as a matrix", "[wem][convert][m3]") {
    // IREF is not derivable and not a TRS. It is not the inverse of the composed
    // rest chain — on `Marine.m3` the two are 2.2 units apart — and across 250
    // corpus models 23 of them hold shear no translation/rotation/scale can
    // reproduce, up to 30.7 units out. So the pose is stored as the matrix it
    // is, and this is the shape of the model that proves it: a bind matrix whose
    // upper 3x3 is deliberately non-orthogonal.
    m3::Model source = makeModel(29);
    m3::InitialReference root;
    root.matrix = Matrix44f::identity();
    m3::InitialReference sheared;
    sheared.matrix = Matrix44f::identity();
    sheared.matrix.data[0][1] = 0.35f; // a shear, not a rotation
    sheared.matrix.data[1][0] = -0.10f;
    sheared.matrix.data[0][0] = 2.0f; // and a non-uniform scale under it
    sheared.matrix.data[1][1] = 0.5f;
    sheared.matrix.data[3][2] = 7.0f;
    source.initialReference.push_back(root);
    source.initialReference.push_back(sheared);

    const M3Converter converter;
    Result<Document> document = converter.fromM3(source);
    REQUIRE(document.ok());
    const NodeTree& tree = document->models.front().nodes;
    REQUIRE(tree.poseSchema.size() == 1);
    CHECK(tree.poseSchema[0].storage == PoseStorage::Matrix);
    REQUIRE(tree.nodes.size() >= 2);
    CHECK(tree.nodes[1].poseMatrices.size() == 1);

    Result<m3::Model> back = converter.toM3(*document, ProfileId::Sc2, 29);
    REQUIRE(back.ok());
    REQUIRE(back->initialReference.size() == source.initialReference.size());
    for (std::size_t b = 0; b < source.initialReference.size(); ++b) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                INFO("bone " << b << " element [" << c << "][" << r << "]");
                CHECK(std::fabs(back->initialReference[b].matrix.data[c][r] -
                                source.initialReference[b].matrix.data[c][r]) < 1e-5f);
            }
        }
    }
}

TEST_CASE("wem m3 derives IREF when the document carries none", "[wem][convert][m3]") {
    // A document from another format has no `iref` pose, and a bind pose is the
    // one thing an M3 cannot be written without. The inverse of the composed
    // rest chain is the only answer available, and taking it beats writing
    // identity.
    //
    // This used to assert the chain itself rather than its inverse, which is the
    // `.mdx -> .m3` defect in miniature: `poseMatrixOf` answers with a schema
    // entry that does not say `inverse`, so the exporter wrote the bone's
    // position where the matrix undoing it belongs and every skinned vertex left
    // twice as far from the bone as it started. `inverseBindMatrix` is the
    // question the exporter actually has.
    Document document;
    document.declare(ProfileId::Sc2);
    document.defaultProfile = ProfileId::Sc2;
    Model model;
    Node root;
    root.name = "root";
    root.kind = NodeKind::Bone;
    root.resetPayloadForKind();
    Node child = root;
    child.name = "child";
    child.parent = 0;
    child.local.translation = Vector3f{0, 4, 0};
    model.nodes.add(std::move(root));
    model.nodes.add(std::move(child));
    ProfileMaterialSet set;
    set.profile = ProfileId::Sc2;
    set.looks.looks.push_back(Look{});
    model.profileSets.push_back(std::move(set));
    document.models.push_back(std::move(model));

    const M3Converter converter;
    Result<m3::Model> back = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(back.ok());
    REQUIRE(back->initialReference.size() == 2);
    // The child sits 4 units from the root along WEM's +Y, which is `.m3`'s +X,
    // so the matrix that undoes it translates by -4 there.
    const Matrix44f& childIref = back->initialReference[1].matrix;
    const Vector3f undone = childIref.extract_translation();
    CHECK(std::fabs(undone.x + 4.0f) < 1e-4f);
    CHECK(std::fabs(undone.y) < 1e-4f);

    // And the root, which sits at the origin, binds as the identity.
    const Vector3f rootUndone = back->initialReference[0].matrix.extract_translation();
    CHECK(std::fabs(rootUndone.x) < 1e-4f);
    CHECK(std::fabs(rootUndone.y) < 1e-4f);
    CHECK(std::fabs(rootUndone.z) < 1e-4f);
}

// ---------------------------------------------------------------------------
// Two regions in one division, which is where a shared vertex buffer breaks.
// ---------------------------------------------------------------------------

namespace {

/// Three bones and two quads, each quad its own region with its own one-entry
/// bone-lookup window: the first skins to bone 1, the second to bone 2, and
/// both write slot 0 into the vertex. Nothing but the region a vertex belongs
/// to distinguishes them, so a converter that pools the two regions' vertices
/// -- or their windows -- gets the second quad's bone wrong.
m3::Model makeTwoRegionModel() {
    m3::Model model = makeModel(30);

    m3::Bone second = model.bones[1];
    second.name = "child2";
    second.position.initValue = Vector3f{0, -6, 0};
    model.bones.push_back(second);

    std::vector<u8> blob = model.vertices.data;
    // Quad B sits at x 2..3, so a vertex's bone is decidable from its position.
    const Vector3f corners[4] = {{2, 0, 0}, {3, 0, 0}, {3, 1, 0}, {2, 1, 0}};
    for (const Vector3f& corner : corners) {
        pushVertex(blob, corner, 0);
    }
    model.vertices.data = std::move(blob);
    model.vertices.initialize();

    model.boneLookup = {1, 2};

    m3::MeshDivision& division = model.divisions[0];
    // Region-local corners, which is what the format stores.
    division.faces.insert(division.faces.end(), {0, 1, 2, 0, 2, 3});
    m3::Region region = division.regions[0];
    region.index = 1;
    region.firstVertex = 4;
    region.vertexCount = 4;
    region.firstIndex = 6;
    region.indexCount = 6;
    region.firstBoneLookup = 1;
    region.boneLookupCount = 1;
    division.regions.push_back(region);

    m3::Batch batch = division.batches[0];
    batch.regionIndex = 1;
    division.batches.push_back(batch);
    return model;
}

/// Every vertex of `model`, as (position, the bone its region's window names).
std::vector<std::pair<Vector3f, u32>> skinOf(const m3::Model& model) {
    std::vector<std::pair<Vector3f, u32>> out;
    const std::vector<Vector3f> positions = model.vertices.getPositions();
    const std::vector<std::array<u8, 4>> indices = model.vertices.getBoneIndices();
    const std::vector<std::array<u8, 4>> weights = model.vertices.getBoneWeights();
    for (const m3::MeshDivision& division : model.divisions) {
        for (const m3::Region& region : division.regions) {
            for (u32 v = 0; v < region.vertexCount; ++v) {
                const std::size_t g = region.firstVertex + v;
                if (g >= positions.size()) {
                    break;
                }
                for (std::size_t k = 0; k < 4; ++k) {
                    if (weights[g][k] == 0) {
                        continue;
                    }
                    const std::size_t slot = region.firstBoneLookup + indices[g][k];
                    REQUIRE(indices[g][k] < region.boneLookupCount);
                    REQUIRE(slot < model.boneLookup.size());
                    out.emplace_back(positions[g], model.boneLookup[slot]);
                }
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("wem m3 two regions keep their own vertices and windows",
          "[wem][convert][m3][skin]") {
    const M3Converter converter;
    Result<Document> doc = converter.fromM3(makeTwoRegionModel());
    REQUIRE(doc.ok());
    Result<m3::Model> back = converter.toM3(*doc.value, ProfileId::Heroes, 30);
    REQUIRE(back.ok());

    REQUIRE(back->divisions.size() == 1);
    const m3::MeshDivision& division = back->divisions[0];
    REQUIRE(division.regions.size() == 2);

    // A vertex belongs to one region: its four bone indices are slots in that
    // region's window and mean nothing in another's. Overlapping slices would
    // make the same byte have to mean two things.
    const m3::Region& a = division.regions[0];
    const m3::Region& b = division.regions[1];
    CHECK((a.firstVertex + a.vertexCount <= b.firstVertex ||
           b.firstVertex + b.vertexCount <= a.firstVertex));
    CHECK(a.boneLookupCount > 0);
    CHECK(b.boneLookupCount > 0);

    // Both quads' bones survive, and the round trip agrees with the source
    // vertex for vertex -- position decides which quad, the window the bone.
    const std::vector<std::pair<Vector3f, u32>> before = skinOf(makeTwoRegionModel());
    const std::vector<std::pair<Vector3f, u32>> after = skinOf(*back.value);
    REQUIRE(before.size() == 8);
    REQUIRE(after.size() == before.size());
    for (const auto& [position, bone] : after) {
        CHECK(bone == (position.x < 1.5f ? 1u : 2u));
    }
}

// ---------------------------------------------------------------------------
// The vertex declaration is the model's, not the converter's.
// ---------------------------------------------------------------------------

namespace {

/// 40 bytes: the base 24, a BGRA colour, two UV pairs, and the tangent. The two
/// high bits of `flags` are ones this reader cannot name -- they are here to be
/// carried, not understood.
constexpr u32 kRichFlags = 0x01800000u | 0x0200u | 0x20000u | 0x40000u;
constexpr std::size_t kRichStride = 40;

void pushRichVertex(std::vector<u8>& blob, const Vector3f& position, const Vector2f& uv0,
                    const Vector2f& uv1, const std::array<u8, 4>& rgba,
                    const std::array<u8, 3>& tangent) {
    const std::size_t base = blob.size();
    blob.resize(base + kRichStride, 0);
    std::memcpy(blob.data() + base, &position, sizeof(Vector3f));
    blob[base + 12] = 255; // weight 0 = 1.0
    blob[base + 16] = 0;   // slot 0 of the region's window
    blob[base + 20] = 128; // normal ~ (0, 0, 1)
    blob[base + 21] = 128;
    blob[base + 22] = 255;
    blob[base + 23] = 0; // bitangent handedness -1
    blob[base + 24] = rgba[2];
    blob[base + 25] = rgba[1];
    blob[base + 26] = rgba[0];
    blob[base + 27] = rgba[3];
    const i16 coords[4] = {static_cast<i16>(uv0.x * 2048.0f), static_cast<i16>(uv0.y * 2048.0f),
                           static_cast<i16>(uv1.x * 2048.0f), static_cast<i16>(uv1.y * 2048.0f)};
    std::memcpy(blob.data() + base + 28, coords, sizeof(coords));
    blob[base + 36] = tangent[0];
    blob[base + 37] = tangent[1];
    blob[base + 38] = tangent[2];
    blob[base + 39] = 255;
}

m3::Model makeRichModel() {
    m3::Model model = makeModel(30);
    std::vector<u8> blob;
    const Vector3f corners[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (std::size_t i = 0; i < 4; ++i) {
        // Exact multiples of 1/2048, so nothing here is a rounding question.
        const f32 t = static_cast<f32>(i) / 2048.0f;
        pushRichVertex(blob, corners[i], Vector2f{t, 0.5f}, Vector2f{0.25f, t},
                       {static_cast<u8>(10 * i), 20, 30, 255},
                       {static_cast<u8>(200 + i), 60, 70});
    }
    model.vertices.flags = static_cast<m3::VertexFormatFlag>(kRichFlags);
    model.vertices.data = std::move(blob);
    model.vertices.initialize();
    return model;
}

} // namespace

TEST_CASE("wem m3 carries the vertex declaration it was given", "[wem][convert][m3]") {
    const m3::Model source = makeRichModel();
    REQUIRE(source.vertices.vertexSize() == kRichStride);
    REQUIRE(source.vertices.UVsNum() == 2);
    REQUIRE(source.vertices.hasVertexColors());

    const M3Converter converter;
    Result<Document> doc = converter.fromM3(source);
    REQUIRE(doc.ok());
    Result<m3::Model> back = converter.toM3(*doc.value, ProfileId::Heroes, 30);
    REQUIRE(back.ok());

    // The word itself, opaque bits included: only the UV and colour bits are
    // this converter's to decide.
    CHECK(static_cast<u32>(back->vertices.flags) == kRichFlags);
    CHECK(back->vertices.vertexSize() == kRichStride);
    REQUIRE(back->vertices.vertexCount() == 4);

    // Position identifies a vertex; the render view is free to reorder them.
    const std::vector<Vector3f> positions = back->vertices.getPositions();
    const std::vector<Vector2f> uv0 = back->vertices.getUVs(0);
    const std::vector<Vector2f> uv1 = back->vertices.getUVs(1);
    const std::vector<m3::ColorBGRA> colors = back->vertices.getColors();
    const std::vector<Vector4f> tangents = back->vertices.getTangents();
    REQUIRE(uv1.size() == 4);
    REQUIRE(colors.size() == 4);

    const std::vector<Vector2f> srcUv0 = source.vertices.getUVs(0);
    const std::vector<Vector2f> srcUv1 = source.vertices.getUVs(1);
    const std::vector<m3::ColorBGRA> srcColors = source.vertices.getColors();
    const std::vector<Vector4f> srcTangents = source.vertices.getTangents();
    const std::vector<Vector3f> srcPositions = source.vertices.getPositions();

    std::size_t matched = 0;
    for (std::size_t v = 0; v < positions.size(); ++v) {
        for (std::size_t s = 0; s < srcPositions.size(); ++s) {
            if (positions[v].x != srcPositions[s].x || positions[v].y != srcPositions[s].y) {
                continue;
            }
            ++matched;
            CHECK(uv0[v].x == srcUv0[s].x);
            CHECK(uv0[v].y == srcUv0[s].y);
            CHECK(uv1[v].x == srcUv1[s].x);
            CHECK(uv1[v].y == srcUv1[s].y);
            CHECK(colors[v].r == srcColors[s].r);
            CHECK(colors[v].g == srcColors[s].g);
            CHECK(colors[v].b == srcColors[s].b);
            CHECK(colors[v].a == srcColors[s].a);
            for (int c = 0; c < 3; ++c) {
                CHECK(tangents[v].data[c] == srcTangents[s].data[c]);
            }
            // The bitangent's handedness rides the normal's fourth byte.
            CHECK(tangents[v].w == srcTangents[s].w);
            break;
        }
    }
    CHECK(matched == 4);
}
