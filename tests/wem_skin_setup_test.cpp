// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The setup the document keeps (EDIT_MODE_SKIN_DESIGN.md §13.4, gate S1's
/// setup arm).
///
/// WEM holds how a model is worked on as well as the model, so a model reopened
/// next week is set up as it was left. What that costs is asserted here: the
/// round trip, what an older file reads as, what a removal and a reorder do to
/// it, and that no file carries any of it -- with the one exception of a classic
/// pin, which changes the groups and nothing else.

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/skinning/quantize.h>
#include <whiteout/models/wem/validate.h>

#include "test_helpers.h"
#include "whiteout/common/binary_reader.h"
#include "whiteout/common/binary_writer.h"
#include "whiteout/common/streams.h"
#include "whiteout/models/wem/binary_read_visitor.h"
#include "whiteout/models/wem/binary_write_visitor.h"
#include "whiteout/models/wem/chunk_tags.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

std::vector<u8> writeDocument(const Document& document) {
    std::vector<u8> buffer;
    buffer.reserve(64 * 1024);
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    common::BinaryWriter writer(out);
    BinaryWriteVisitor visitor(writer);
    visitor.write(document, kCurrentVersion);
    buffer.shrink_to_fit();
    return buffer;
}

Document readDocument(const std::vector<u8>& bytes, std::vector<std::string>& issues) {
    common::span_streambuf streambuf(std::span<const u8>(bytes.data(), bytes.size()));
    std::istream in(&streambuf);
    common::BinaryReader reader(in);
    Document document;
    BinaryReadVisitor visitor(reader);
    visitor.read(document, kCurrentVersion);
    issues = visitor.issues();
    return document;
}

/// A quad in two triangles per mesh, each vertex bound to `bone`, with one
/// material so `toMdx` writes it.
Document makeDocument(u32 meshes = 1) {
    Document document;
    document.declare(ProfileId::Wc3Classic);
    document.defaultProfile = ProfileId::Wc3Classic;
    document.textures.push_back(TextureRef{});
    Model model;
    model.name = "test";
    model.addSlot("slot");
    for (u32 m = 0; m < meshes; ++m) {
        geom::MeshBuilder builder;
        MeshSection section;
        section.name = "part" + std::to_string(m);
        builder.addSection(std::move(section));
        const f32 offset = static_cast<f32>(m) * 10.0f;
        const geom::VertexId a = builder.addVertex(Vector3f{offset, 0, 0});
        const geom::VertexId b = builder.addVertex(Vector3f{offset + 1, 0, 0});
        const geom::VertexId c = builder.addVertex(Vector3f{offset, 1, 0});
        const geom::VertexId d = builder.addVertex(Vector3f{offset + 1, 1, 0});
        for (const geom::VertexId id : {a, b, c, d}) {
            builder.addInfluence(id, m % 2, 1.0f);
        }
        for (const auto& triangle : {std::array<geom::VertexId, 3>{a, b, c},
                                     std::array<geom::VertexId, 3>{b, d, c}}) {
            const geom::FaceId face = builder.addTriangle(triangle[0], triangle[1], triangle[2], 0);
            for (u32 corner = 0; corner < 3; ++corner) {
                builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
                builder.setCornerAttr(face, corner, geom::names::uv(0), Vector2f{0, 0});
            }
        }
        model.meshes.push_back(builder.build().mesh);
    }
    for (u32 n = 0; n < 4; ++n) {
        Node node;
        node.name = "bone_" + std::to_string(n);
        node.kind = NodeKind::Bone;
        node.parent = n == 0 ? kInvalidNode : 0;
        node.pivot = Vector3f{static_cast<f32>(n), 0, 0};
        node.local = Transform::identity();
        node.local.translation = node.pivot;
        node.resetPayloadForKind();
        model.nodes.nodes.push_back(std::move(node));
    }
    ProfileMaterialSet set;
    set.profile = ProfileId::Wc3Classic;
    set.looks.looks.push_back(Look{});
    set.resizeBindings(model.materialSlots.size());
    set.slotBindings[0].byLook[0] = 0;
    set.materials.push_back(Material{});
    model.profileSets.push_back(std::move(set));
    document.models.push_back(std::move(model));
    return document;
}

NodeSkinSetup makeSetup(u32 mirror, u32 poses) {
    NodeSkinSetup setup;
    setup.locked = true;
    Envelope envelope;
    envelope.innerStart = 0.5f;
    envelope.innerEnd = 0.25f;
    envelope.outerStart = 2.0f;
    envelope.outerEnd = 1.5f;
    envelope.falloff = EnvelopeFalloff::Gaussian;
    setup.envelope = envelope;
    Joint joint;
    joint.motion = JointMotion::Slide;
    joint.axis = JointAxis::Along;
    joint.parentSpace = true;
    joint.low = -3.0f;
    joint.high = 7.5f;
    setup.joint = joint;
    setup.mirror = mirror;
    for (u32 p = 0; p < poses; ++p) {
        PoseDelta delta;
        delta.rotation = Quaternion{0.1f * static_cast<f32>(p + 1), 0, 0, 0.9f};
        delta.translation = Vector3f{static_cast<f32>(p), 1, 2};
        setup.poseDeltas.push_back(delta);
    }
    return setup;
}

/// A mesh's `classicBones` layer, created and filled.
void pin(Mesh& mesh, u32 vertex, u16 count) {
    std::span<u16> pins = mesh.attributes.getOrCreate<u16>(geom::names::kClassicBones,
                                                           geom::Domain::Vertex, geom::AttrType::U16);
    if (vertex < pins.size()) {
        pins[vertex] = count;
    }
}

} // namespace

TEST_CASE("S1 setup: a wem round trip keeps every field", "[wem][skin][setup]") {
    Document original = makeDocument();
    Model& model = original.models.front();
    TestPose pose;
    pose.name = "arm up";
    pose.clip = kInvalidIndex;
    pose.timeMs = 250.0f;
    model.testPoses.push_back(pose);
    TestPose second;
    second.name = "walk";
    second.clip = kInvalidIndex;
    second.timeMs = 1000.0f;
    model.testPoses.push_back(second);
    model.nodes.nodes[1].skin = makeSetup(2, 2);
    model.nodes.nodes[2].skin.locked = true;
    pin(model.meshes[0], 1, 3);
    std::span<u8> locks = model.meshes[0].attributes.getOrCreate<u8>(
        geom::names::kSkinLocked, geom::Domain::Vertex, geom::AttrType::Bool);
    locks[2] = 1;
    std::span<u8> set = model.meshes[0].attributes.getOrCreate<u8>(
        geom::selectionLayer("Turret"), geom::Domain::Vertex, geom::AttrType::Bool);
    set[3] = 1;

    std::vector<std::string> issues;
    const Document reread = readDocument(writeDocument(original), issues);
    CHECK(issues.empty());
    REQUIRE(reread.models.size() == 1u);
    const Model& back = reread.models.front();
    CHECK(back.testPoses == model.testPoses);
    CHECK(back.nodes.nodes[1].skin == model.nodes.nodes[1].skin);
    CHECK(back.nodes.nodes[2].skin.locked);
    CHECK(back.nodes.nodes[0].skin.empty());
    const std::span<const u16> pins =
        back.meshes[0].attributes.get<u16>(geom::names::kClassicBones, geom::Domain::Vertex);
    REQUIRE(pins.size() == 4u);
    CHECK(pins[1] == 3);
    const std::span<const u8> readLocks =
        back.meshes[0].attributes.get<u8>(geom::names::kSkinLocked, geom::Domain::Vertex);
    REQUIRE(readLocks.size() == 4u);
    CHECK(readLocks[2] == 1);
    const std::span<const u8> readSet = back.meshes[0].attributes.get<u8>(
        geom::selectionLayer("Turret"), geom::Domain::Vertex);
    REQUIRE(readSet.size() == 4u);
    CHECK(readSet[3] == 1);

    // And the reserved names force their type, whoever creates them (§5.4).
    CHECK(geom::LookupReserved(geom::names::kSkinLocked).type == geom::AttrType::Bool);
    CHECK(geom::LookupReserved(geom::names::kClassicBones).type == geom::AttrType::U16);
    CHECK(geom::LookupReserved(geom::selectionLayer("Turret")).domain == geom::Domain::Vertex);
    CHECK(geom::selectionSetOf(geom::selectionLayer("Turret")) == "Turret");
    CHECK(geom::selectionSetOf("uv0").empty());
}

TEST_CASE("S1 setup: an older node chunk reads with an empty setup", "[wem][skin][setup]") {
    // A v6 field read off the end of a v5 record would take every node after it
    // with it, so the gate is what keeps an older file readable. Forged the way
    // `wem_v3_roundtrip_test` forges one: the setup's bytes are cut out of each
    // record and the chunk is stamped v5, so this is the file a build without
    // the field would have written.
    Document original = makeDocument();
    Model& model = original.models.front();
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        // Recognisable, so the setup can be found in the bytes.
        model.nodes.nodes[n].skin.mirror = 0x5EA70000u + n;
    }

    std::vector<u8> bytes = writeDocument(original);
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::vector<IndexEntry> entries(header.indexCount);
    std::memcpy(entries.data(), bytes.data() + header.indexOffset,
                entries.size() * sizeof(IndexEntry));

    // An empty setup is 19 bytes: `locked`, the two optional flags, `mirror`,
    // and the null `Reference` an empty vector writes. Asserted rather than
    // assumed -- if the encoding moves, this says so instead of cutting the
    // wrong bytes.
    constexpr u32 kSetupBytes = 1 + 1 + 1 + 4 + 12;
    bool cut = false;
    for (u32 i = 0; i < header.indexCount; ++i) {
        IndexEntry& entry = entries[i];
        if (entry.tag != ChunkTagTraits<Node>::value) {
            continue;
        }
        u32 end = header.indexOffset > entry.offset ? header.indexOffset
                                                    : static_cast<u32>(bytes.size());
        for (const IndexEntry& other : entries) {
            if (other.offset > entry.offset && other.offset < end) {
                end = other.offset;
            }
        }
        std::vector<u32> starts;
        for (u32 at = entry.offset; at + 4 <= end; ++at) {
            u32 word = 0;
            std::memcpy(&word, bytes.data() + at, 4);
            if ((word & 0xFFFF0000u) != 0x5EA70000u || (word & 0xFFFFu) >= model.nodes.size()) {
                continue;
            }
            REQUIRE(at >= entry.offset + 3);
            // `locked` false and two empty optionals sit in front of it.
            CHECK(bytes[at - 3] == 0);
            CHECK(bytes[at - 2] == 0);
            CHECK(bytes[at - 1] == 0);
            for (u32 k = 0; k < 12; ++k) {
                CHECK(bytes[at + 4 + k] == 0); // the null reference
            }
            starts.push_back(at - 3);
        }
        REQUIRE(starts.size() == model.nodes.size());

        // Cut them back to front, so the offsets found above stay valid, and
        // refill the chunk's tail so nothing outside it moves.
        for (auto start = starts.rbegin(); start != starts.rend(); ++start) {
            std::memmove(bytes.data() + *start, bytes.data() + *start + kSetupBytes,
                         end - *start - kSetupBytes);
        }
        std::memset(bytes.data() + end - kSetupBytes * starts.size(), 0xAA,
                    kSetupBytes * starts.size());
        entry.version = 5;
        std::memcpy(bytes.data() + header.indexOffset + i * sizeof(IndexEntry), &entry,
                    sizeof(IndexEntry));
        cut = true;
    }
    REQUIRE(cut);

    std::vector<std::string> issues;
    const Document reread = readDocument(bytes, issues);
    REQUIRE(reread.models.size() == 1u);
    const NodeTree& nodes = reread.models.front().nodes;
    REQUIRE(nodes.size() == original.models.front().nodes.size());
    for (u32 n = 0; n < nodes.size(); ++n) {
        CAPTURE(n);
        // Every node still reads -- the records did not slide -- and none of
        // them carries a setup.
        CHECK(nodes.nodes[n].name == original.models.front().nodes.nodes[n].name);
        CHECK(nodes.nodes[n].parent == original.models.front().nodes.nodes[n].parent);
        CHECK(nodes.nodes[n].skin.empty());
    }
}

TEST_CASE("S1 setup: a removal takes the setup and clears an override",
          "[wem][skin][setup]") {
    Document document = makeDocument();
    Model& model = document.models.front();
    model.testPoses.push_back(TestPose{"pose", kInvalidIndex, 0.0f});
    model.nodes.nodes[1].skin = makeSetup(/*mirror=*/2, /*poses=*/1);
    model.nodes.nodes[3].skin.mirror = 2;

    Diagnostics out;
    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(model.meshes.data(), model.meshes.size());
    referencers.channels = &model.animChannels;
    RemoveNode(model.nodes, 2, RemovePolicy::ReparentChildren, SkinPolicy::ReassignToParent,
               /*preserveWorld=*/true, referencers);
    CompactNodes(model.nodes, referencers, out);

    // Node 2 is gone, so the two overrides that named it name nothing.
    REQUIRE(model.nodes.size() == 3u);
    for (const Node& node : model.nodes.nodes) {
        CHECK(node.skin.mirror == kInvalidNode);
    }
    // And the removed node's own setup went with it, while node 1 kept its.
    CHECK(model.nodes.nodes[1].skin.locked);
    CHECK(model.nodes.nodes[1].skin.joint.has_value());
    CHECK(model.nodes.nodes[1].skin.poseDeltas.size() == 1u);
    CHECK(out.countOf(DiagCode::DanglingNodeReference) >= 1u);
}

TEST_CASE("S1 setup: a reorder moves an override with its node", "[wem][skin][setup]") {
    Document document = makeDocument();
    Model& model = document.models.front();
    model.nodes.nodes[1].skin.mirror = 3;
    // A permutation that renumbers exactly the rows a removal does.
    const std::vector<u32> remap{3, 2, 1, 0};
    std::vector<Node> reordered(model.nodes.size());
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        reordered[remap[n]] = model.nodes.nodes[n];
    }
    model.nodes.nodes = std::move(reordered);
    for (Node& node : model.nodes.nodes) {
        if (node.parent != kInvalidNode) {
            node.parent = remap[node.parent];
        }
    }
    Diagnostics out;
    RemapNodeReferencers(model.nodes, remap, NodeReferencers{}, out);
    CHECK(model.nodes.nodes[remap[1]].skin.mirror == remap[3]);
}

TEST_CASE("S1 setup: the pivot conversion remaps what names a node", "[wem][skin][setup]") {
    // `ToPivotRelative` inserts a shear helper immediately before the bone it
    // stretches, which renumbers every node after it. It kept its own copy of
    // the referencer table, and that copy already missed the emitter links; the
    // mirror override rides the walk the table itself uses.
    Document document = makeDocument();
    Model& model = document.models.front();
    model.nodes.rig = RigConvention::ExplicitBind;
    PoseSchema schema;
    schema.name = "iref";
    schema.space = PoseSpace::Model;
    schema.inverse = true;
    schema.storage = PoseStorage::Matrix;
    model.nodes.poseSchema.push_back(schema);
    model.nodes.authoritativePose = 0;
    // A non-uniform scale under a rotated bind frame is shear a pivot node
    // cannot hold, so the conversion splits it and inserts a helper.
    const Quaternion tilt = Quaternion{0.0f, 0.2588f, 0.0f, 0.9659f}.normalized();
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        Node& bone = model.nodes.nodes[n];
        bone.local.scale = n == 1 ? Vector3f{2.0f, 1.0f, 0.5f} : Vector3f{1, 1, 1};
        Transform bind;
        bind.translation = bone.pivot;
        bind.rotation = n == 1 ? tilt : Quaternion{0, 0, 0, 1};
        bind.scale = Vector3f{1, 1, 1};
        bone.poseMatrices.push_back(Matrix44f::inverse(ToMatrix(bind)));
        bone.poses.push_back(FromMatrix(bone.poseMatrices[0]));
    }
    model.nodes.nodes[3].skin.mirror = 1;
    const std::string mirrorName = model.nodes.nodes[1].name;
    const std::string ownerName = model.nodes.nodes[3].name;

    const SkeletonRetargetResult result = RetargetSkeleton(document, ProfileId::Wc3Classic);
    REQUIRE(result.ok);
    CHECK(result.nodesInserted > 0u);

    const NodeTree& nodes = document.models.front().nodes;
    u32 owner = kInvalidNode;
    u32 target = kInvalidNode;
    for (u32 n = 0; n < nodes.size(); ++n) {
        owner = nodes.nodes[n].name == ownerName ? n : owner;
        target = nodes.nodes[n].name == mirrorName ? n : target;
    }
    REQUIRE(owner != kInvalidNode);
    REQUIRE(target != kInvalidNode);
    CHECK(nodes.nodes[owner].skin.mirror == target);
}

TEST_CASE("S1 setup: a merge keeps locks, pins and sets on the right vertices",
          "[wem][skin][setup]") {
    Document document = makeDocument(2);
    Model& model = document.models.front();
    pin(model.meshes[0], 1, 2);
    std::span<u8> locks = model.meshes[0].attributes.getOrCreate<u8>(
        geom::names::kSkinLocked, geom::Domain::Vertex, geom::AttrType::Bool);
    locks[0] = 1;
    std::span<u8> set = model.meshes[1].attributes.getOrCreate<u8>(
        geom::selectionLayer("Arm"), geom::Domain::Vertex, geom::AttrType::Bool);
    set[2] = 1;
    const Vector3f pinned = model.meshes[0].attributes.get<Vector3f>(
        geom::names::kPosition, geom::Domain::Vertex)[1];
    const Vector3f selected = model.meshes[1].attributes.get<Vector3f>(
        geom::names::kPosition, geom::Domain::Vertex)[2];

    const MeshMergeResult result = MergeMeshesInto(model, std::vector<u32>{0, 1}, 0);
    REQUIRE(result.ok);
    const Mesh& merged = model.meshes[result.merged];
    const std::span<const Vector3f> positions =
        merged.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const std::span<const u16> pins =
        merged.attributes.get<u16>(geom::names::kClassicBones, geom::Domain::Vertex);
    const std::span<const u8> mergedLocks =
        merged.attributes.get<u8>(geom::names::kSkinLocked, geom::Domain::Vertex);
    const std::span<const u8> mergedSet =
        merged.attributes.get<u8>(geom::selectionLayer("Arm"), geom::Domain::Vertex);
    REQUIRE(pins.size() == positions.size());
    REQUIRE(mergedLocks.size() == positions.size());
    // The layer the other input lacked was zero-filled rather than dropped.
    REQUIRE(mergedSet.size() == positions.size());

    u32 pinnedAt = kInvalidIndex;
    u32 selectedAt = kInvalidIndex;
    for (u32 v = 0; v < positions.size(); ++v) {
        pinnedAt = positions[v] == pinned ? v : pinnedAt;
        selectedAt = positions[v] == selected ? v : selectedAt;
    }
    REQUIRE(pinnedAt != kInvalidIndex);
    REQUIRE(selectedAt != kInvalidIndex);
    CHECK(pins[pinnedAt] == 2);
    CHECK(mergedSet[selectedAt] == 1);
    u32 lockedCount = 0;
    for (const u8 value : mergedLocks) {
        lockedCount += value != 0 ? 1 : 0;
    }
    CHECK(lockedCount == 1u);
}

TEST_CASE("S1 setup: no file carries the setup, and a pin changes only the groups",
          "[wem][skin][setup]") {
    const MdxConverter converter;
    const Document plain = makeDocument();
    Document dressed = makeDocument();
    Model& model = dressed.models.front();
    model.testPoses.push_back(TestPose{"pose", kInvalidIndex, 0.0f});
    model.nodes.nodes[1].skin = makeSetup(2, 1);
    std::span<u8> locks = model.meshes[0].attributes.getOrCreate<u8>(
        geom::names::kSkinLocked, geom::Domain::Vertex, geom::AttrType::Bool);
    locks[0] = 1;
    std::span<u8> set = model.meshes[0].attributes.getOrCreate<u8>(
        geom::selectionLayer("Arm"), geom::Domain::Vertex, geom::AttrType::Bool);
    set[1] = 1;

    const Result<std::vector<u8>> without = converter.exportToBytes(plain, ProfileId::Wc3Classic);
    const Result<std::vector<u8>> with = converter.exportToBytes(dressed, ProfileId::Wc3Classic);
    REQUIRE(without.ok());
    REQUIRE(with.ok());
    CHECK(*without.value == *with.value);

    // A pin is the one thing that reaches a file, and only through the groups.
    Document pinned = makeDocument();
    Model& pinnedModel = pinned.models.front();
    for (u32 v = 0; v < 4; ++v) {
        // Two bones on every vertex, so the automatic snap would pick both.
        pinnedModel.meshes[0].skin.assignVertex(
            v, std::vector<geom::Influence>{{0, 0.5f}, {1, 0.5f}});
    }
    Document unpinned = pinned;
    pin(pinnedModel.meshes[0], 2, 1);

    const Result<mdx::Model> before = converter.toMdx(unpinned, ProfileId::Wc3Classic, 800);
    const Result<mdx::Model> after = converter.toMdx(pinned, ProfileId::Wc3Classic, 800);
    REQUIRE(before.ok());
    REQUIRE(after.ok());
    REQUIRE(before->geosets.size() == 1u);
    const mdx::Geoset& was = before->geosets[0];
    const mdx::Geoset& now = after->geosets[0];
    CHECK(was.matrixGroups == std::vector<u32>{2});
    // The pinned vertex keeps its one heaviest bone, so a second group appears.
    CHECK(now.matrixGroups.size() == 2u);
    CHECK(now.vertexGroups != was.vertexGroups);
    // And nothing else in the geoset moved.
    CHECK(now.vertexPositions == was.vertexPositions);
    CHECK(now.vertexNormals == was.vertexNormals);
    CHECK(now.faces == was.faces);
    CHECK(now.textureCoordinateSets == was.textureCoordinateSets);
    CHECK(now.skinData == was.skinData);

    // The Reforged file ignores pins altogether (§12.6).
    Document reforged = pinned;
    reforged.declare(ProfileId::Wc3Reforged);
    ProfileMaterialSet hd;
    hd.profile = ProfileId::Wc3Reforged;
    hd.looks.looks.push_back(Look{});
    hd.resizeBindings(reforged.models.front().materialSlots.size());
    hd.slotBindings[0].byLook[0] = 0;
    hd.materials.push_back(Material{});
    reforged.models.front().profileSets.push_back(std::move(hd));
    Document reforgedUnpinned = reforged;
    reforgedUnpinned.models.front().meshes[0].attributes.remove(geom::names::kClassicBones,
                                                                geom::Domain::Vertex);
    const Result<mdx::Model> hdPinned = converter.toMdx(reforged, ProfileId::Wc3Reforged, 1000);
    const Result<mdx::Model> hdPlain =
        converter.toMdx(reforgedUnpinned, ProfileId::Wc3Reforged, 1000);
    REQUIRE(hdPinned.ok());
    REQUIRE(hdPlain.ok());
    CHECK(hdPinned->geosets[0].skinData == hdPlain->geosets[0].skinData);
}

TEST_CASE("S1 setup: the quantizer keeps a pin and never merges one away",
          "[wem][skin][setup]") {
    // A pinned vertex keeps its pinned count, whatever the snap would have said.
    std::vector<std::vector<geom::Influence>> vertices;
    vertices.push_back({{0, 0.5f}, {1, 0.5f}});
    vertices.push_back({{0, 0.5f}, {1, 0.5f}});
    const std::vector<u16> pins{1, 0};
    const skinning::ClassicSkin skin = skinning::QuantizeClassic(vertices, pins);
    REQUIRE(skin.groups.size() == 2u);
    CHECK(skin.groups[skin.groupOf[0]] == std::vector<u32>{0});
    CHECK(skin.groups[skin.groupOf[1]] == std::vector<u32>{0, 1});

    // A pinned group is never the victim when the 256 limit bites: build 300
    // groups, pin the vertices of one of them, and it survives.
    std::vector<std::vector<geom::Influence>> many;
    std::vector<u16> manyPins;
    for (u32 g = 0; g < 300; ++g) {
        many.push_back({{g, 0.6f}, {g + 1, 0.4f}});
        manyPins.push_back(0);
    }
    // One group used by a single vertex -- the natural victim -- but pinned.
    many.push_back({{900, 0.6f}, {901, 0.4f}});
    manyPins.push_back(2);
    const skinning::ClassicSkin merged = skinning::QuantizeClassic(many, manyPins);
    CHECK(merged.groups.size() == 256u);
    CHECK(merged.mergedGroups > 0u);
    const std::vector<u32>& kept = merged.groups[merged.groupOf.back()];
    CHECK(kept == std::vector<u32>{900, 901});

    // And pins that alone need more than 256 groups refuse the geoset.
    std::vector<std::vector<geom::Influence>> tooMany;
    std::vector<u16> allPinned;
    for (u32 g = 0; g < 257; ++g) {
        tooMany.push_back({{g * 2, 0.6f}, {g * 2 + 1, 0.4f}});
        allPinned.push_back(2);
    }
    const skinning::ClassicSkin refused = skinning::QuantizeClassic(tooMany, allPinned);
    CHECK(refused.pinsOverflow);
}

TEST_CASE("S1 setup: Validate checks what the setup says", "[wem][skin][setup]") {
    Document document = makeDocument();
    Model& model = document.models.front();
    model.testPoses.push_back(TestPose{"pose", kInvalidIndex, 0.0f});
    model.nodes.nodes[1].skin.poseDeltas.assign(3, PoseDelta{});
    Envelope envelope;
    envelope.innerStart = 5.0f;
    envelope.outerStart = 1.0f;
    model.nodes.nodes[2].skin.envelope = envelope;
    model.testPoses.push_back(TestPose{"bad clip", 9, 0.0f});
    pin(model.meshes[0], 0, 9);
    model.nodes.nodes[3].skin.mirror = 99;

    const Diagnostics report = Validate(document, ValidateLevel::Structural);
    CHECK(report.countOf(DiagCode::SkinSetupInvalid) == 4u);
    // A mirror override naming nothing is the node table's own row.
    CHECK(report.countOf(DiagCode::DanglingNodeReference) >= 1u);

    // A setup that agrees with the document says nothing at all.
    Document clean = makeDocument();
    clean.models.front().testPoses.push_back(TestPose{"pose", kInvalidIndex, 0.0f});
    clean.models.front().nodes.nodes[1].skin.poseDeltas.assign(1, PoseDelta{});
    clean.models.front().nodes.nodes[1].skin.mirror = 2;
    CHECK(Validate(clean, ValidateLevel::Structural).countOf(DiagCode::SkinSetupInvalid) == 0u);
}

TEST_CASE("S1 setup: a test pose names a clip of its own model", "[wem][skin][setup]") {
    // The clips are the document's, shared by every model: an index in range
    // can still be another model's animation.
    Document document = makeDocument();
    Clip own;
    own.name = "Stand";
    own.model = 0;
    Clip other;
    other.name = "Walk";
    other.model = 1;
    document.clips.push_back(own);
    document.clips.push_back(other);
    Model& model = document.models.front();
    model.testPoses.push_back(TestPose{"stand", 0, 0.0f});
    CHECK(Validate(document, ValidateLevel::Structural).countOf(DiagCode::SkinSetupInvalid) == 0u);
    model.testPoses.push_back(TestPose{"walk", 1, 0.0f});
    CHECK(Validate(document, ValidateLevel::Structural).countOf(DiagCode::SkinSetupInvalid) == 1u);
}
