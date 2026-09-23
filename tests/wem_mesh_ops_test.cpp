// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G18 (EDIT_MODE_MESH_PLAN.md L2): merging meshes into one mesh with one
/// section, the mesh-axis referencer table, and the converter's say on whether
/// `toMdx` could write the result.
///
/// The trap this suite exists for is C1: `MergeMeshes` copies the corners only
/// of an input with connectivity, and a mesh read back from `.wem` has none —
/// so a merge of such meshes came out with every normal and UV zero, and said
/// nothing. Every geometry claim below is checked on meshes read from `.wem`
/// as well as on meshes straight from the import.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/nodes/emitters.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

mdx::Node makeNode(const std::string& name, u32 objectId) {
    mdx::Node node;
    node.name = name;
    node.objectId = objectId;
    node.parentId = mdx::Node::NO_PARENT;
    return node;
}

/// Three quads, each its own geoset with its own normals and UVs, so a zeroed
/// corner is visible. Geoset 2's record carries a keyed alpha; bone 1 is gated
/// on geoset 2.
mdx::Model makeThreeGeosets() {
    mdx::Model model;
    model.version = 800;
    model.modelName = "three";
    mdx::Texture texture;
    texture.fileName = "textures/body.blp";
    model.textures.push_back(texture);
    mdx::Material material;
    mdx::Layer layer;
    layer.textureId = 0;
    material.layers.push_back(layer);
    model.materials.push_back(material);
    model.materials.push_back(material);

    for (u32 b = 0; b < 2; ++b) {
        mdx::Bone bone;
        bone.node = makeNode("bone_" + std::to_string(b), b);
        model.bones.push_back(bone);
    }
    model.bones[1].geosetId = 2;
    model.bones[1].geosetAnimationId = 0;
    model.pivotPoints = {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}};

    for (u32 g = 0; g < 3; ++g) {
        const f32 x = static_cast<f32>(g) * 4.0f;
        const f32 tilt = 0.1f * static_cast<f32>(g + 1);
        mdx::Geoset geoset;
        geoset.lodName = "part_" + std::to_string(g);
        geoset.vertexPositions = {Vector3f{x, 0, 0}, Vector3f{x + 1, 0, 0}, Vector3f{x + 1, 1, 0},
                                  Vector3f{x, 1, 0}};
        for (u32 v = 0; v < 4; ++v) {
            geoset.vertexNormals.push_back(Vector3f{tilt, 0, 1});
        }
        geoset.textureCoordinateSets.push_back({Vector2f{0.1f * g, 0}, Vector2f{1, 0.2f * g},
                                                Vector2f{1, 1}, Vector2f{0.3f, 1}});
        geoset.faces = {0, 1, 2, 0, 2, 3};
        geoset.vertexGroups = {0, 0, 0, 0};
        geoset.matrixGroups = {1};
        geoset.matrixIndices = {g == 2 ? 1u : 0u};
        geoset.materialId = g == 1 ? 1u : 0u;
        model.geosets.push_back(geoset);
    }

    mdx::GeosetAnimation keyed;
    keyed.geosetId = 2;
    keyed.alphaTracks.isUsed = true;
    keyed.alphaTracks.interpolationType = mdx::InterpolationType::Linear;
    keyed.alphaTracks.keyCount = 2;
    keyed.alphaTracks.timestamps = {0, 100};
    keyed.alphaTracks.keys_data = {0.0f, 1.0f};
    model.geosetAnimations.push_back(keyed);

    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 100;
    model.sequences.push_back(stand);
    return model;
}

Document imported() {
    const MdxConverter converter;
    Result<Document> document = converter.fromMdx(makeThreeGeosets());
    REQUIRE(document.ok());
    return std::move(*document.value);
}

/// Through `.wem` and back: every mesh arrives without connectivity.
Document viaWem(const Document& document) {
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    Parser parser;
    std::optional<Document> read = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(read.has_value());
    return std::move(*read);
}

struct Corner {
    Vector3f position;
    Vector3f normal;
    Vector2f uv;
};

/// Every corner, in face order, as the halfedge layers hold it.
std::vector<Corner> cornersOf(Mesh mesh) {
    REQUIRE(mesh.ensureConnectivity().ok());
    const auto& topology = mesh.topology();
    const auto positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const auto normals =
        mesh.attributes.get<const Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    const auto uvs = mesh.attributes.get<const Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
    std::vector<Corner> out;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        for (const geom::HalfedgeId h : topology.fh(geom::FaceId(f))) {
            Corner c;
            c.position = positions[topology.from(h).index()];
            c.normal = h.index() < normals.size() ? normals[h.index()] : Vector3f{0, 0, 0};
            c.uv = h.index() < uvs.size() ? uvs[h.index()] : Vector2f{0, 0};
            out.push_back(c);
        }
    }
    return out;
}

bool same(const Vector3f& a, const Vector3f& b) {
    return std::abs(a.x - b.x) < 1e-5f && std::abs(a.y - b.y) < 1e-5f && std::abs(a.z - b.z) < 1e-5f;
}
bool same(const Vector2f& a, const Vector2f& b) {
    return std::abs(a.x - b.x) < 1e-5f && std::abs(a.y - b.y) < 1e-5f;
}

u32 sectionChannel(Model& model, u32 mesh) {
    AnimChannel channel;
    channel.id = static_cast<u32>(model.animChannels.channels.size()) + 100;
    channel.target.kind = TrackTarget::Kind::Section;
    channel.target.mesh = mesh;
    channel.target.sub = 0;
    channel.target.channel = Channel::Alpha;
    channel.valueType = geom::AttrType::F32;
    model.animChannels.channels.push_back(channel);
    return channel.id;
}

const AnimChannel* channelById(const Model& model, u32 id) {
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.id == id) {
            return &channel;
        }
    }
    return nullptr;
}

/// A mesh of two quads in two sections, built directly: one skinned, one not.
Mesh twoSectionMesh() {
    geom::MeshBuilder builder;
    for (u32 s = 0; s < 2; ++s) {
        MeshSection section;
        section.name = "s" + std::to_string(s);
        builder.addSection(std::move(section));
    }
    for (u32 s = 0; s < 2; ++s) {
        const f32 x = static_cast<f32>(s) * 4.0f;
        const geom::VertexId a = builder.addVertex(Vector3f{x, 0, 0});
        const geom::VertexId b = builder.addVertex(Vector3f{x + 1, 0, 0});
        const geom::VertexId c = builder.addVertex(Vector3f{x + 1, 1, 0});
        const geom::VertexId d = builder.addVertex(Vector3f{x, 1, 0});
        for (const geom::VertexId v : {a, b, c, d}) {
            builder.addInfluence(v, 0, 1.0f);
        }
        builder.addTriangle(a, b, c, s);
        builder.addTriangle(a, c, d, s);
    }
    Mesh mesh = std::move(builder.build().mesh);
    mesh.repairLog.droppedFaces.push_back(geom::FaceRecord{{0, 1, 2}, 1, 9});
    return mesh;
}

} // namespace

// ---------------------------------------------------------------------------
// MergeSections and BakeRigidNode
// ---------------------------------------------------------------------------

TEST_CASE("wem MergeSections relabels faces and remaps the rest", "[wem][mesh][merge]") {
    Mesh mesh = twoSectionMesh();
    const u32 both[] = {0, 1};
    const std::vector<u32> remap = geom::MergeSections(mesh, both, 0);
    REQUIRE(remap.size() == 2);
    CHECK(remap[0] == 0u);
    CHECK(remap[1] == geom::kInvalidId);
    REQUIRE(mesh.sections.size() == 1);
    CHECK(mesh.sections[0].name == "s0");
    for (const u32 s : mesh.faceSections()) {
        CHECK(s == 0u);
    }
    // The repair log follows the faces it recorded.
    CHECK(mesh.repairLog.droppedFaces[0].section == 0u);
}

TEST_CASE("wem MergeSections keeps the section named and renumbers past it",
          "[wem][mesh][merge]") {
    Mesh mesh = twoSectionMesh();
    const u32 both[] = {0, 1};
    const std::vector<u32> remap = geom::MergeSections(mesh, both, 1);
    REQUIRE(mesh.sections.size() == 1);
    CHECK(mesh.sections[0].name == "s1");
    CHECK(remap[1] == 0u);
    CHECK(remap[0] == geom::kInvalidId);
}

TEST_CASE("wem MergeSections refuses sections that bind or gate differently",
          "[wem][mesh][merge]") {
    const u32 both[] = {0, 1};
    SECTION("a rigid node") {
        Mesh mesh = twoSectionMesh();
        mesh.sections[1].rigidNode = 3;
        const std::vector<u32> faces(mesh.faceSections().begin(), mesh.faceSections().end());
        CHECK(geom::MergeSections(mesh, both, 0).empty());
        CHECK(mesh.sections.size() == 2);
        CHECK(std::vector<u32>(mesh.faceSections().begin(), mesh.faceSections().end()) == faces);
    }
    SECTION("a visibility gate") {
        Mesh mesh = twoSectionMesh();
        mesh.sections[0].native.set(kSectionVisibilityNode, 4);
        CHECK(geom::MergeSections(mesh, both, 0).empty());
        CHECK(mesh.sections.size() == 2);
        // "No gate" stored and "no gate" omitted are the same gate.
        mesh.sections[0].native.set(kSectionVisibilityNode, kSectionAlwaysDrawn);
        CHECK_FALSE(geom::MergeSections(mesh, both, 0).empty());
    }
    SECTION("an index out of range") {
        Mesh mesh = twoSectionMesh();
        const u32 bad[] = {0, 5};
        CHECK(geom::MergeSections(mesh, bad, 0).empty());
        CHECK(mesh.sections.size() == 2);
    }
}

TEST_CASE("wem BakeRigidNode writes a rigid section into the skin", "[wem][mesh][merge]") {
    Mesh mesh = twoSectionMesh();
    mesh.sections[1].rigidNode = 7;
    REQUIRE(geom::BakeRigidNode(mesh, 1));
    CHECK_FALSE(mesh.sections[1].rigidNode.has_value());
    REQUIRE(mesh.skin.vertexCount() == mesh.vertexCount());
    // Section 1's four vertices now bind bone 7 at weight 1; section 0's keep 0.
    for (u32 v = 0; v < 8; ++v) {
        const auto influences = std::as_const(mesh.skin).forVertex(v);
        REQUIRE(influences.size() == 1);
        CHECK(influences[0].bone == (v < 4 ? 0u : 7u));
        CHECK(influences[0].weight == 1.0f);
    }
}

TEST_CASE("wem BakeRigidNode refuses a vertex another section shares", "[wem][mesh][merge]") {
    geom::MeshBuilder builder;
    builder.addSection(MeshSection{});
    builder.addSection(MeshSection{});
    const geom::VertexId a = builder.addVertex(Vector3f{0, 0, 0});
    const geom::VertexId b = builder.addVertex(Vector3f{1, 0, 0});
    const geom::VertexId c = builder.addVertex(Vector3f{1, 1, 0});
    const geom::VertexId d = builder.addVertex(Vector3f{0, 1, 0});
    builder.addTriangle(a, b, c, 0);
    builder.addTriangle(a, c, d, 1); // a and c in both
    Mesh mesh = std::move(builder.build().mesh);
    mesh.sections[1].rigidNode = 2;
    const geom::SkinBinding before = mesh.skin;
    CHECK_FALSE(geom::BakeRigidNode(mesh, 1));
    CHECK(mesh.sections[1].rigidNode == 2u);
    CHECK(mesh.skin.offsets == before.offsets);
    CHECK_FALSE(geom::BakeRigidNode(mesh, 0)); // not rigid
}

// ---------------------------------------------------------------------------
// MergeMeshesInto
// ---------------------------------------------------------------------------

TEST_CASE("wem MergeMeshesInto keeps every corner imported or read from .wem",
          "[wem][mesh][merge]") {
    for (const bool throughWem : {false, true}) {
        INFO((throughWem ? "read back from .wem" : "from fromMdx"));
        const Document original = imported();
        Document document = throughWem ? viaWem(original) : original;
        Model& model = document.models[0];
        if (throughWem) {
            for (const Mesh& mesh : model.meshes) {
                CHECK_FALSE(mesh.hasConnectivity()); // the case C1 is about
            }
        }
        // Expected: the primary's corners, then the absorbed ones in index order.
        std::vector<Corner> expected = cornersOf(original.models[0].meshes[1]);
        for (const u32 m : {0u, 2u}) {
            const std::vector<Corner> more = cornersOf(original.models[0].meshes[m]);
            expected.insert(expected.end(), more.begin(), more.end());
        }

        const u32 merging[] = {0, 1, 2};
        const MeshMergeResult result = MergeMeshesInto(model, merging, 1);
        REQUIRE(result.ok);
        REQUIRE(model.meshes.size() == 1);
        CHECK(result.merged == 0u);
        CHECK(result.meshRemap == std::vector<u32>{kInvalidIndex, 0, kInvalidIndex});
        const Mesh& merged = model.meshes[0];
        REQUIRE(merged.sections.size() == 1);
        CHECK(merged.sections[0].name == "part_1");
        CHECK(merged.sections[0].materialSlot == 1u);
        CHECK(merged.name == original.models[0].meshes[1].name);

        const std::vector<Corner> got = cornersOf(merged);
        REQUIRE(got.size() == expected.size());
        u32 zeroNormals = 0;
        for (std::size_t i = 0; i < got.size(); ++i) {
            CAPTURE(i);
            CHECK(same(got[i].position, expected[i].position));
            CHECK(same(got[i].normal, expected[i].normal));
            CHECK(same(got[i].uv, expected[i].uv));
            zeroNormals += same(got[i].normal, Vector3f{0, 0, 0}) ? 1u : 0u;
        }
        CHECK(zeroNormals == 0u);
        // And it writes as one geoset.
        const MdxConverter converter;
        Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic, 800);
        REQUIRE(exported.ok());
        CHECK(exported->geosets.size() == 1);
        CHECK(exported->geosets[0].faces.size() == 18);
    }
}

TEST_CASE("wem MergeMeshesInto moves bone gates and invalidates absorbed channels",
          "[wem][mesh][merge]") {
    Document document = imported();
    Model& model = document.models[0];
    // Bone 1 is gated on mesh 2 by the import; a channel on each mesh.
    u32 gated = kInvalidIndex;
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes.nodes[i].name == "bone_1") {
            gated = i;
        }
    }
    REQUIRE(gated != kInvalidIndex);
    REQUIRE(std::get<BonePayload>(model.nodes.nodes[gated].payload).gateMesh == 2u);
    const u32 kept = sectionChannel(model, 0);
    const u32 absorbed = sectionChannel(model, 2);
    const std::size_t declared = model.animChannels.channels.size();

    const u32 merging[] = {0, 2};
    const MeshMergeResult result = MergeMeshesInto(model, merging, 0);
    REQUIRE(result.ok);
    CHECK(result.meshRemap == std::vector<u32>{0, 1, kInvalidIndex});
    CHECK(result.linksMoved == 1u);
    CHECK(result.channelsInvalidated >= 1u);
    CHECK(std::get<BonePayload>(model.nodes.nodes[gated].payload).gateMesh == 0u);

    // Declarations are kept; the absorbed one names no mesh now.
    CHECK(model.animChannels.channels.size() == declared);
    REQUIRE(channelById(model, kept) != nullptr);
    CHECK(channelById(model, kept)->target.mesh == 0u);
    REQUIRE(channelById(model, absorbed) != nullptr);
    CHECK(channelById(model, absorbed)->target.mesh == kInvalidIndex);
    CHECK(result.diagnostics.countOf(DiagCode::AnimChannelInvalidated) >= 1u);
}

TEST_CASE("wem MergeMeshesInto renumbers the meshes after an absorbed one",
          "[wem][mesh][merge]") {
    Document document = imported();
    Model& model = document.models[0];
    const u32 onLast = sectionChannel(model, 2);
    const u32 merging[] = {0, 1};
    const MeshMergeResult result = MergeMeshesInto(model, merging, 0);
    REQUIRE(result.ok);
    CHECK(result.meshRemap == std::vector<u32>{0, kInvalidIndex, 1});
    CHECK(channelById(model, onLast)->target.mesh == 1u);
    CHECK(result.channelsInvalidated == 0u);
    CHECK_FALSE(Validate(document, ValidateLevel::Structural).hasErrors());
}

TEST_CASE("wem MergeMeshesInto clears a StarCraft II shape on an absorbed mesh 0",
          "[wem][mesh][merge]") {
    Document document = imported();
    Model& model = document.models[0];
    Node emitter;
    emitter.name = "emitter";
    emitter.kind = NodeKind::Sc2ParticleEmitter;
    emitter.parent = kInvalidNode;
    emitter.resetPayloadForKind();
    std::get<Sc2ParticleEmitterPayload>(emitter.payload).shapeSections = {0};
    model.nodes.add(std::move(emitter));
    const u32 index = model.nodes.size() - 1;

    const u32 merging[] = {0, 1};
    SECTION("mesh 0 is the primary: its sections are renumbered") {
        REQUIRE(MergeMeshesInto(model, merging, 0).ok);
        CHECK(std::get<Sc2ParticleEmitterPayload>(model.nodes.nodes[index].payload).shapeSections ==
              std::vector<u32>{0});
    }
    SECTION("mesh 0 is absorbed: they name nothing now") {
        const MeshMergeResult result = MergeMeshesInto(model, merging, 1);
        REQUIRE(result.ok);
        CHECK(std::get<Sc2ParticleEmitterPayload>(model.nodes.nodes[index].payload)
                  .shapeSections.empty());
    }
}

TEST_CASE("wem MergeMeshesInto refuses what one geoset cannot be", "[wem][mesh][merge]") {
    Document document = imported();
    Model& model = document.models[0];
    const Model before = model;
    SECTION("fewer than two") {
        const u32 one[] = {1, 1};
        CHECK_FALSE(MergeMeshesInto(model, one, 1).ok);
    }
    SECTION("a primary not among them") {
        const u32 two[] = {0, 1};
        CHECK_FALSE(MergeMeshesInto(model, two, 2).ok);
    }
    SECTION("an every-level mesh and a LOD 0 one") {
        model.meshes[2].lodLevel = kAllLods;
        const u32 two[] = {0, 2};
        const MeshMergeResult result = MergeMeshesInto(model, two, 0);
        CHECK_FALSE(result.ok);
        CHECK(result.diagnostics.countOf(DiagCode::OperationUnsupported) == 1u);
    }
    SECTION("a mesh of more than one section") {
        model.meshes[2].sections.push_back(model.meshes[2].sections[0]);
        const u32 two[] = {0, 2};
        CHECK_FALSE(MergeMeshesInto(model, two, 0).ok);
    }
    CHECK(model.meshes.size() == 3);
}

TEST_CASE("wem MergeMeshesInto gives an absorbed mesh the primary's tangent frame",
          "[wem][mesh][merge]") {
    // The primary carries tangents, as an HD geoset does; the absorbed mesh does
    // not, and half its faces are a mirrored UV island.
    const auto quad = [](f32 x, bool mirrored, bool tangents) {
        geom::MeshBuilder builder;
        builder.addSection(MeshSection{});
        const geom::VertexId a = builder.addVertex(Vector3f{x, 0, 0});
        const geom::VertexId b = builder.addVertex(Vector3f{x + 1, 0, 0});
        const geom::VertexId c = builder.addVertex(Vector3f{x + 1, 1, 0});
        const geom::VertexId d = builder.addVertex(Vector3f{x, 1, 0});
        const f32 s = mirrored ? -1.0f : 1.0f;
        const Vector2f uv[] = {{0, 0}, {s, 0}, {s, 1}, {0, 1}};
        const u32 uvAbc[] = {0, 1, 2};
        const u32 uvAcd[] = {0, 2, 3};
        const geom::FaceId f0 = builder.addTriangle(a, b, c, 0);
        const geom::FaceId f1 = builder.addTriangle(a, c, d, 0);
        for (u32 k = 0; k < 3; ++k) {
            builder.setCornerAttr(f0, k, geom::names::kNormal, Vector3f{0, 0, 1});
            builder.setCornerAttr(f1, k, geom::names::kNormal, Vector3f{0, 0, 1});
            builder.setCornerAttr(f0, k, geom::names::uv(0), uv[uvAbc[k]]);
            builder.setCornerAttr(f1, k, geom::names::uv(0), uv[uvAcd[k]]);
            if (tangents) {
                builder.setCornerAttr(f0, k, geom::names::kTangent, Vector4f{1, 0, 0, 1});
                builder.setCornerAttr(f1, k, geom::names::kTangent, Vector4f{1, 0, 0, 1});
            }
        }
        return std::move(builder.build().mesh);
    };
    Model model;
    model.meshes.push_back(quad(0, false, true));
    model.meshes.push_back(quad(4, true, false));
    model.meshes.push_back(quad(8, false, false));
    const u32 merging[] = {0, 1, 2};
    REQUIRE(MergeMeshesInto(model, merging, 0).ok);

    Mesh merged = model.meshes[0];
    REQUIRE(merged.ensureConnectivity().ok());
    const auto tangents =
        merged.attributes.get<const Vector4f>(geom::names::kTangent, geom::Domain::Halfedge);
    const auto positions =
        merged.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE_FALSE(tangents.empty());
    const auto& topology = merged.topology();
    u32 mirroredCorners = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        for (const geom::HalfedgeId h : topology.fh(geom::FaceId(f))) {
            const Vector4f& t = tangents[h.index()];
            const f32 x = positions[topology.from(h).index()].x;
            CAPTURE(f, x);
            CHECK(std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z) > 0.99f); // not zero
            if (x >= 3.5f && x <= 5.5f) {
                ++mirroredCorners;
                CHECK(t.w == -1.0f); // the mirrored island
                CHECK(t.x < -0.99f);
            } else {
                CHECK(t.w == 1.0f);
                CHECK(t.x > 0.99f);
            }
        }
    }
    CHECK(mirroredCorners == 6u);
}

TEST_CASE("wem MergeMeshesInto carries Edge layers", "[wem][mesh][merge]") {
    // Once the canary of a merge that zero-filled them (C31): a `sharp` edge on
    // the primary and one on an absorbed mesh both survive, on and off `.wem`.
    for (const bool throughWem : {false, true}) {
        CAPTURE(throughWem);
        Document document = imported();
        Model& model = document.models[0];
        const auto markFirstEdge = [](Mesh& mesh) {
            REQUIRE(mesh.ensureConnectivity().ok());
            const std::span<u8> sharp = mesh.attributes.getOrCreate<u8>(
                geom::names::kSharp, geom::Domain::Edge, geom::AttrType::Bool);
            REQUIRE_FALSE(sharp.empty());
            sharp[0] = 1;
            const geom::HalfedgeId h = geom::Topology::halfedge(geom::EdgeId(0), 0);
            return std::pair<Vector3f, Vector3f>{
                mesh.attributes.get<const Vector3f>(geom::names::kPosition,
                                                     geom::Domain::Vertex)[mesh.topology().from(h).index()],
                mesh.attributes.get<const Vector3f>(geom::names::kPosition,
                                                     geom::Domain::Vertex)[mesh.topology().to(h).index()]};
        };
        const auto first = markFirstEdge(model.meshes[0]);
        const auto second = markFirstEdge(model.meshes[1]);
        if (throughWem) {
            document = viaWem(document);
        }
        const u32 merging[] = {0, 1};
        REQUIRE(MergeMeshesInto(document.models[0], merging, 0).ok);
        Mesh& merged = document.models[0].meshes[0];
        REQUIRE(merged.ensureConnectivity().ok());
        const std::span<const u8> sharp =
            merged.attributes.get<const u8>(geom::names::kSharp, geom::Domain::Edge);
        const std::span<const Vector3f> positions =
            merged.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        u32 marked = 0;
        for (u32 e = 0; e < sharp.size(); ++e) {
            if (sharp[e] == 0) {
                continue;
            }
            ++marked;
            const geom::HalfedgeId h = geom::Topology::halfedge(geom::EdgeId(e), 0);
            const Vector3f a = positions[merged.topology().from(h).index()];
            const Vector3f b = positions[merged.topology().to(h).index()];
            const auto is = [&](const std::pair<Vector3f, Vector3f>& edge) {
                return (same(a, edge.first) && same(b, edge.second)) ||
                       (same(a, edge.second) && same(b, edge.first));
            };
            CHECK((is(first) || is(second)));
        }
        CHECK(marked == 2u);
    }
}

// ---------------------------------------------------------------------------
// checkGeoset against toMdx
// ---------------------------------------------------------------------------

namespace {

/// One mesh in one section, skinned to `bones` bones, as `shape` asks:
/// - "sets": 257 vertices, each binding a distinct pair of 30 bones;
/// - "palette": 257 vertices, each binding its own bone;
/// - "wide": 65,538 unshared vertices in disjoint triangles.
Document limitDocument(const std::string& shape) {
    const u32 bones = shape == "sets" ? 30u : shape == "palette" ? 257u : 1u;
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = shape;
    section.profiles = ProfileBit(ProfileId::Wc3Classic);
    builder.addSection(std::move(section));
    const u32 vertices = shape == "wide" ? 65538u : 258u; // a multiple of 3
    std::vector<std::pair<u32, u32>> pairs;
    for (u32 a = 0; a < bones && pairs.size() < vertices; ++a) {
        for (u32 b = a + 1; b < bones && pairs.size() < vertices; ++b) {
            pairs.emplace_back(a, b);
        }
    }
    for (u32 v = 0; v < vertices; ++v) {
        // Triangle t is a small right triangle of its own, so none is dropped
        // as degenerate and no two share a vertex.
        const u32 t = v / 3;
        const u32 k = v % 3;
        const geom::VertexId id = builder.addVertex(
            Vector3f{static_cast<f32>((t % 256) * 2 + (k == 1 ? 1 : 0)),
                     static_cast<f32>((t / 256) * 2 + (k == 2 ? 1 : 0)), 0.0f});
        if (shape == "sets") {
            builder.addInfluence(id, pairs[v % pairs.size()].first, 0.5f);
            builder.addInfluence(id, pairs[v % pairs.size()].second, 0.5f);
        } else if (shape == "palette") {
            builder.addInfluence(id, v % bones, 1.0f);
        } else {
            builder.addInfluence(id, 0, 1.0f);
        }
    }
    for (u32 v = 0; v + 2 < vertices; v += 3) {
        const geom::FaceId face = builder.addTriangle(geom::VertexId(v), geom::VertexId(v + 1),
                                                      geom::VertexId(v + 2), 0);
        for (u32 corner = 0; corner < 3; ++corner) {
            builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
            builder.setCornerAttr(face, corner, geom::names::uv(0), Vector2f{0, 0});
        }
    }

    Document document;
    document.declare(ProfileId::Wc3Classic);
    document.defaultProfile = ProfileId::Wc3Classic;
    document.textures.push_back(TextureRef{});
    Model model;
    model.meshes.push_back(std::move(builder.build().mesh));
    ProfileMaterialSet set;
    set.profile = ProfileId::Wc3Classic;
    set.looks.looks.push_back(Look{});
    model.addSlot("slot");
    set.resizeBindings(model.materialSlots.size());
    set.slotBindings[0].byLook[0] = 0;
    set.materials.push_back(Material{});
    model.profileSets.push_back(std::move(set));
    for (u32 b = 0; b < bones; ++b) {
        Node bone;
        bone.name = "bone_" + std::to_string(b);
        bone.kind = NodeKind::Bone;
        bone.parent = kInvalidNode;
        bone.resetPayloadForKind();
        model.nodes.nodes.push_back(std::move(bone));
    }
    document.models.push_back(std::move(model));
    return document;
}

} // namespace

TEST_CASE("wem checkGeoset says what toMdx says about the limits", "[wem][mesh][merge]") {
    const MdxConverter converter;
    for (const char* shape : {"sets", "palette", "wide"}) {
        const Document document = limitDocument(shape);
        for (const u32 version : {800u, 1000u}) {
            CAPTURE(shape, version);
            const Diagnostics checked = converter.checkGeoset(
                document, 0, document.models[0].meshes[0], ProfileId::Wc3Classic, version);
            const Result<mdx::Model> exported =
                converter.toMdx(document, ProfileId::Wc3Classic, version);
            // A `SKIN` palette the file cannot index is refused rather than
            // written with the weight dropped (EDIT_MODE_SKIN_DESIGN.md §12.1);
            // past 256 classic groups the Skin Quantizer merges, and writes.
            const bool refused = std::string(shape) == "palette" && version > 800;
            CHECK(exported.ok() == !refused);
            CHECK(checked.hasErrors() == refused);
            CHECK(checked.countOf(DiagCode::IndexWidthExceeded) ==
                  exported.diagnostics.countOf(DiagCode::IndexWidthExceeded));
            // No difference left: the export's old "sets" warning above v800
            // went with the quantizer (the Mesh plan's C8).
            CHECK(checked.countOf(DiagCode::BonePaletteLimit) ==
                  exported.diagnostics.countOf(DiagCode::BonePaletteLimit));
        }
    }
    // And each fixture trips the limit it is named for.
    CHECK(converter.checkGeoset(limitDocument("wide"), 0, limitDocument("wide").models[0].meshes[0],
                                ProfileId::Wc3Classic, 800)
              .countOf(DiagCode::IndexWidthExceeded) == 1u);
    const Document sets = limitDocument("sets");
    CHECK(converter.checkGeoset(sets, 0, sets.models[0].meshes[0], ProfileId::Wc3Classic, 800)
              .countOf(DiagCode::BonePaletteLimit) == 1u);
    CHECK(converter.checkGeoset(sets, 0, sets.models[0].meshes[0], ProfileId::Wc3Classic, 1000)
              .countOf(DiagCode::BonePaletteLimit) == 0u);
    const Document palette = limitDocument("palette");
    CHECK(converter
              .checkGeoset(palette, 0, palette.models[0].meshes[0], ProfileId::Wc3Classic, 1000)
              .countOf(DiagCode::BonePaletteLimit) == 1u);
}

TEST_CASE("wem checkGeoset counts the vertices the geosets hold", "[wem][mesh][merge]") {
    const Document document = imported();
    const MdxConverter converter;
    u32 vertices = 0;
    converter.checkGeoset(document, 0, document.models[0].meshes[0], ProfileId::Wc3Classic, 800,
                          &vertices);
    CHECK(vertices == 4u);
}
