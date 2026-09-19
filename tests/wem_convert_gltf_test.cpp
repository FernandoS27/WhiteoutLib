// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// GLTF_DESIGN P1 gate — export geometry, nodes and the container.
///
/// The in-memory arms check the crossing's two non-negotiables on a document
/// small enough to reason about by hand: the basis permutation is **bit-exact**
/// (`gltf = (y, z, x)`, no arithmetic), and the writer is **byte-stable** across
/// runs. The `[corpus]` arm drives real `.mdx` content through the same path
/// and re-parses every export with our own parser — count assertions, not
/// pictures; the pictures come from the validator and Blender (§11 gates 1, 6).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/gltf/parser.h>
#include <whiteout/models/gltf/writer.h>
#include <whiteout/models/m2/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/utils/os_file_system.h>

#include "test_helpers.h"
#include "wem_corpus_files.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

/// One quad (two triangles) plus one classic-only triangle, a two-node chain,
/// one material slot, declared `Generic`.
Document makeFixture() {
    Document document;
    document.name = "fixture";
    document.declare(ProfileId::Generic);

    Model model;
    model.name = "fixture";
    model.materialSlots.push_back("main");

    geom::MeshBuilder builder;
    const geom::VertexId a = builder.addVertex(Vector3f{0, 0, 0});
    const geom::VertexId b = builder.addVertex(Vector3f{1, 0, 0});
    const geom::VertexId c = builder.addVertex(Vector3f{0, 2, 0});
    const geom::VertexId d = builder.addVertex(Vector3f{3, 4, 5});

    MeshSection drawn;
    drawn.name = "drawn";
    drawn.materialSlot = 0;
    builder.addSection(drawn);
    MeshSection classicOnly;
    classicOnly.name = "classicOnly";
    classicOnly.materialSlot = 0;
    classicOnly.profiles = ProfileBit(ProfileId::Wc3Classic);
    builder.addSection(classicOnly);

    const geom::FaceId f0 = builder.addTriangle(a, b, c, 0);
    const geom::FaceId f1 = builder.addTriangle(b, d, c, 0);
    const geom::FaceId f2 = builder.addTriangle(a, c, d, 1); // Not drawn by Generic.
    for (const geom::FaceId face : {f0, f1, f2}) {
        for (u32 corner = 0; corner < 3; ++corner) {
            builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
            builder.setCornerAttr(face, corner, geom::names::uv(0), Vector2f{0.25f, 0.75f});
        }
    }

    // Skin everything to the Bone node (index 1) so the export grows a skin.
    for (const geom::VertexId vertex : {a, b, c, d}) {
        builder.addInfluence(vertex, 1, 0.75f); // Not normalised on purpose.
    }

    geom::MeshBuilder::BuildOutcome outcome = builder.build();
    outcome.mesh.name = "quad";
    model.meshes.push_back(std::move(outcome.mesh));

    Node root;
    root.name = "root";
    root.local.translation = Vector3f{1, 2, 3};
    model.nodes.add(root);
    Node child;
    child.name = "child";
    child.parent = 0;
    child.kind = NodeKind::Bone;
    child.resetPayloadForKind();
    child.local.scale = Vector3f{2, 3, 4};
    model.nodes.add(child);

    TextureRef stone;
    stone.key = TexturePath{"textures/Stone Floor.blp"};
    stone.path = "textures/Stone Floor.blp";
    document.textures.push_back(stone);

    ProfileMaterialSet set;
    set.profile = ProfileId::Generic;
    set.looks = LookTable::Single();
    Material material;
    material.name = "main";
    CommonMaterial& common = material.InitCommon();
    common.blend = BlendMode::Additive; // No glTF form: BLEND + a diagnostic.
    common.cull = CullMode::None;
    common.setKind(MaterialKind::PBRDeferred);
    PbrDeferredBody& body = *common.pbr();
    body.baseColorFactor = Vector4f{0.5f, 0.25f, 1.0f, 1.0f};
    body.roughnessFactor = 0.4f;
    body.emissiveFactor = Vector3f{4, 2, 1}; // Peak 4: emissive_strength.
    TextureInput orm;
    orm.texture = 0;
    orm.uvSet = 0;
    orm.wrapU = WrapMode::Clamp;
    body.set(PbrSlot::Orm, orm);
    set.materials.push_back(std::move(material));
    set.resizeBindings(model.materialSlots.size());
    set.slotBindings[0].byLook[0] = 0;
    model.profileSets.push_back(std::move(set));

    // One clip: a linear translation on the root, a Hermite rotation on the
    // bone, and one event (which has no glTF form).
    AnimChannel translationChannel;
    translationChannel.id = 1;
    translationChannel.target.kind = TrackTarget::Kind::Node;
    translationChannel.target.node = 0;
    translationChannel.target.channel = Channel::Translation;
    translationChannel.valueType = geom::AttrType::F32x3;
    model.animChannels.add(translationChannel);
    AnimChannel rotationChannel;
    rotationChannel.id = 2;
    rotationChannel.target.kind = TrackTarget::Kind::Node;
    rotationChannel.target.node = 1;
    rotationChannel.target.channel = Channel::Rotation;
    rotationChannel.valueType = geom::AttrType::Quat;
    model.animChannels.add(rotationChannel);

    Clip clip;
    clip.name = "walk";
    clip.model = 0;
    clip.duration = 2.0f;
    SubTrackContainer container;
    SubTrack translation;
    translation.channel = 1;
    translation.interp = Interpolation::Linear;
    translation.times = {0.0f, 1.0f};
    const f32 translationKeys[6] = {0, 0, 0, 1, 0, 0};
    const u8* translationRaw = reinterpret_cast<const u8*>(translationKeys);
    translation.values.assign(translationRaw, translationRaw + sizeof(translationKeys));
    container.subTracks.push_back(std::move(translation));
    SubTrack rotation;
    rotation.channel = 2;
    rotation.interp = Interpolation::Hermite;
    rotation.times = {0.0f, 2.0f};
    // Per key {value, inTan, outTan}, quats.
    const f32 rotationKeys[24] = {0, 0, 0, 1, 0.2f, 0, 0, 0, 0.2f, 0, 0, 0,
                                  0, 0, 0, 1, 0.2f, 0, 0, 0, 0.2f, 0, 0, 0};
    const u8* rotationRaw = reinterpret_cast<const u8*>(rotationKeys);
    rotation.values.assign(rotationRaw, rotationRaw + sizeof(rotationKeys));
    container.subTracks.push_back(std::move(rotation));
    clip.containers.push_back(std::move(container));
    clip.events.push_back(ClipEvent{1.0f, 0, "footstep", 0});

    document.models.push_back(std::move(model));
    document.clips.push_back(std::move(clip));
    return document;
}

// --- glTF-side matrix helpers (column-vector semantics on Matrix44f storage) --

Matrix44f columnTranslation(const Vector3f& t) {
    Matrix44f m = Matrix44f::identity();
    m.data[0][3] = t.x;
    m.data[1][3] = t.y;
    m.data[2][3] = t.z;
    return m;
}

Matrix44f columnRotation(const Quaternion& q) {
    // `Matrix44f::rotation` composes row-vector; its transpose is the
    // column-vector matrix glTF composes with.
    return Matrix44f::rotation(q).transpose();
}

Matrix44f columnScale(const Vector3f& s) {
    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = s.x;
    m.data[1][1] = s.y;
    m.data[2][2] = s.z;
    return m;
}

/// Each glTF node's global transform, composed from the written TRS exactly
/// the way a glTF consumer composes it.
std::vector<Matrix44f> gltfWorldTransforms(const gltf::Asset& asset) {
    std::vector<u32> parent(asset.nodes.size(), gltf::kNone);
    for (u32 i = 0; i < asset.nodes.size(); ++i) {
        for (const u32 child : asset.nodes[i].children) {
            parent[child] = i;
        }
    }
    std::vector<Matrix44f> world(asset.nodes.size(), Matrix44f::identity());
    std::vector<bool> done(asset.nodes.size(), false);
    // Nodes may appear in any order; resolve each chain on demand.
    const std::function<Matrix44f(u32)> resolve = [&](u32 index) -> Matrix44f {
        if (done[index]) {
            return world[index];
        }
        const gltf::Node& node = asset.nodes[index];
        Matrix44f local = node.hasMatrix
                              ? node.matrix
                              : columnTranslation(node.translation) *
                                    columnRotation(node.rotation) * columnScale(node.scale);
        world[index] =
            parent[index] == gltf::kNone ? local : resolve(parent[index]) * local;
        done[index] = true;
        return world[index];
    };
    for (u32 i = 0; i < asset.nodes.size(); ++i) {
        resolve(i);
    }
    return world;
}

/// The §11 gate 4: for every joint of every skin, IBM × world bind == identity
/// (within tolerance scaled by the translation the matrices carry). Computed
/// from the *written bytes* — this is the transpose trap's tripwire.
u32 skinIdentityFailures(const gltf::Asset& asset, f32 tolerance) {
    const std::vector<Matrix44f> world = gltfWorldTransforms(asset);
    std::vector<f32> inverseBinds;
    u32 failures = 0;
    for (const gltf::Skin& skin : asset.skins) {
        if (skin.inverseBindMatrices == gltf::kNone ||
            !gltf::ReadAccessorF32(asset, skin.inverseBindMatrices, inverseBinds) ||
            inverseBinds.size() != skin.joints.size() * 16) {
            ++failures;
            continue;
        }
        for (std::size_t j = 0; j < skin.joints.size(); ++j) {
            Matrix44f ibm;
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    ibm.data[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] =
                        inverseBinds[j * 16 + static_cast<std::size_t>(col * 4 + row)];
                }
            }
            const Matrix44f joint = world[skin.joints[j]] * ibm;
            f32 magnitude = 1.0f;
            for (int c = 0; c < 3; ++c) {
                magnitude = std::max(
                    magnitude,
                    std::fabs(world[skin.joints[j]].data[static_cast<std::size_t>(c)][3]));
            }
            const f32 limit = tolerance * magnitude;
            bool bad = false;
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    const f32 expected = r == c ? 1.0f : 0.0f;
                    bad = bad ||
                          std::fabs(joint.data[static_cast<std::size_t>(r)]
                                              [static_cast<std::size_t>(c)] -
                                    expected) > limit;
                }
            }
            failures += bad ? 1u : 0u;
        }
    }
    return failures;
}

} // namespace

TEST_CASE("gltf export permutes the basis bit-exactly", "[wem][gltf]") {
    const Document document = makeFixture();
    const GltfConverter converter;
    Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
    REQUIRE(exported.ok());

    // Round through the container so the assertions read written bytes.
    const std::vector<u8> glb = gltf::Writer::ToGlb(*exported);
    gltf::ParseOutcome reparsed = gltf::Parser::FromBytes(glb);
    REQUIRE(reparsed.ok());
    const gltf::Asset& asset = *reparsed.asset;

    // The model's synthetic root, two tree nodes, and the mesh holder.
    REQUIRE(asset.nodes.size() == 4);
    CHECK(asset.nodes[0].name == "fixture");
    REQUIRE(asset.nodes[0].children.size() == 1);
    CHECK(asset.nodes[1].name == "root");
    CHECK(asset.nodes[1].translation.x == 2.0f); // (1,2,3) -> (y,z,x) = (2,3,1)
    CHECK(asset.nodes[1].translation.y == 3.0f);
    CHECK(asset.nodes[1].translation.z == 1.0f);
    REQUIRE(asset.nodes[1].children.size() == 1);
    CHECK(asset.nodes[1].children[0] == 2);
    CHECK(asset.nodes[2].scale.x == 3.0f); // (2,3,4) -> (3,4,2)
    CHECK(asset.nodes[2].scale.y == 4.0f);
    CHECK(asset.nodes[2].scale.z == 2.0f);
    CHECK(asset.nodes[3].mesh == 0);

    REQUIRE(asset.meshes.size() == 1);
    // The classic-only section is not drawn by Generic: one primitive.
    REQUIRE(asset.meshes[0].primitives.size() == 1);
    const gltf::Primitive& primitive = asset.meshes[0].primitives[0];

    std::vector<f32> positions;
    REQUIRE(gltf::ReadAccessorF32(asset, primitive.attribute("POSITION"), positions));
    REQUIRE(positions.size() == 4 * 3);
    // Source vertex 3 was (3,4,5) -> (4,5,3), bit-exact.
    CHECK(positions[9] == 4.0f);
    CHECK(positions[10] == 5.0f);
    CHECK(positions[11] == 3.0f);

    // POSITION carries the mandatory bounds.
    const gltf::Accessor& position = asset.accessors[primitive.attribute("POSITION")];
    REQUIRE(position.min.size() == 3);
    CHECK(position.max[0] == 4.0);
    CHECK(position.max[1] == 5.0);
    CHECK(position.max[2] == 3.0);

    std::vector<f32> normals;
    REQUIRE(gltf::ReadAccessorF32(asset, primitive.attribute("NORMAL"), normals));
    REQUIRE(normals.size() == 4 * 3);
    // (0,0,1) -> (0,1,0): glTF's up is Blizzard's +Z.
    CHECK(normals[0] == 0.0f);
    CHECK(normals[1] == 1.0f);
    CHECK(normals[2] == 0.0f);

    std::vector<u32> indices;
    REQUIRE(gltf::ReadAccessorU32(asset, primitive.indices, indices));
    CHECK(indices.size() == 6);

    std::vector<f32> uvs;
    REQUIRE(gltf::ReadAccessorF32(asset, primitive.attribute("TEXCOORD_0"), uvs));
    REQUIRE(uvs.size() == 4 * 2);
    CHECK(uvs[0] == 0.25f);
    CHECK(uvs[1] == 0.75f);
}

TEST_CASE("gltf export crosses the material", "[wem][gltf]") {
    const Document document = makeFixture();
    const GltfConverter converter;
    Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
    REQUIRE(exported.ok());
    // Additive has no glTF form; the diagnostic says so.
    CHECK(exported.diagnostics.countOf(DiagCode::LossyBlendMode) == 1);

    const std::vector<u8> glb = gltf::Writer::ToGlb(*exported);
    gltf::ParseOutcome reparsed = gltf::Parser::FromBytes(glb);
    REQUIRE(reparsed.ok());
    const gltf::Asset& asset = *reparsed.asset;

    REQUIRE(asset.materials.size() == 1);
    const gltf::Material& material = asset.materials[0];
    CHECK(material.name == "main");
    CHECK(material.alphaMode == gltf::AlphaMode::Blend);
    CHECK(material.doubleSided);
    CHECK(material.pbr.baseColorFactor.x == 0.5f);
    CHECK(material.pbr.roughnessFactor == 0.4f);
    // Peak-4 emission: factor normalises, the strength extension carries 4.
    CHECK(material.emissiveStrength == 4.0f);
    CHECK(material.emissiveFactor.x == 1.0f);
    CHECK(material.emissiveFactor.y == 0.5f);
    // The ORM slot is both the metallic-roughness texture and the occlusion.
    REQUIRE(material.pbr.metallicRoughnessTexture.present());
    CHECK(material.occlusionTexture.index == material.pbr.metallicRoughnessTexture.index);

    REQUIRE(asset.images.size() == 1);
    CHECK(asset.images[0].uri == "Stone_Floor.png"); // Sanitised suggestion.
    CHECK(asset.images[0].name == "wem:texture:0");  // The driver's join key.
    REQUIRE(asset.samplers.size() == 1);
    CHECK(asset.samplers[0].wrapS == gltf::WrapMode::ClampToEdge);
    CHECK(asset.samplers[0].wrapT == gltf::WrapMode::Repeat);

    bool strengthUsed = false;
    for (const std::string& extension : asset.extensionsUsed) {
        strengthUsed = strengthUsed || extension == "KHR_materials_emissive_strength";
    }
    CHECK(strengthUsed);

    const gltf::Primitive& primitive = asset.meshes[0].primitives[0];
    CHECK(primitive.material == 0);
}

TEST_CASE("gltf export writes a skin whose binds invert", "[wem][gltf]") {
    const Document document = makeFixture();
    const GltfConverter converter;
    Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
    REQUIRE(exported.ok());

    const std::vector<u8> glb = gltf::Writer::ToGlb(*exported);
    gltf::ParseOutcome reparsed = gltf::Parser::FromBytes(glb);
    REQUIRE(reparsed.ok());
    const gltf::Asset& asset = *reparsed.asset;

    REQUIRE(asset.skins.size() == 1);
    CHECK(asset.skins[0].joints.size() == 2); // Every model node is a joint.
    const gltf::Node& holder = asset.nodes[3];
    CHECK(holder.skin == 0);

    const gltf::Primitive& primitive = asset.meshes[0].primitives[0];
    std::vector<u32> joints;
    REQUIRE(gltf::ReadAccessorU32(asset, primitive.attribute("JOINTS_0"), joints));
    REQUIRE(joints.size() == 4 * 4);
    CHECK(joints[0] == 1); // Bound to the Bone node, joint index 1.
    std::vector<f32> weights;
    REQUIRE(gltf::ReadAccessorF32(asset, primitive.attribute("WEIGHTS_0"), weights));
    REQUIRE(weights.size() == 4 * 4);
    // 0.75 shipped un-normalised; glTF requires the sum restated to 1.
    CHECK(weights[0] == 1.0f);
    CHECK(weights[1] == 0.0f);

    // Gate 4, from the written bytes: IBM × world bind == identity.
    CHECK(skinIdentityFailures(asset, 1e-4f) == 0);
}

TEST_CASE("gltf export crosses the animation clock-for-clock", "[wem][gltf]") {
    const Document document = makeFixture();
    const GltfConverter converter;
    Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
    REQUIRE(exported.ok());
    // The event is declared lost.
    CHECK(exported.diagnostics.countOf(DiagCode::AnimTrackDropped) == 1);

    const std::vector<u8> glb = gltf::Writer::ToGlb(*exported);
    gltf::ParseOutcome reparsed = gltf::Parser::FromBytes(glb);
    REQUIRE(reparsed.ok());
    const gltf::Asset& asset = *reparsed.asset;

    REQUIRE(asset.animations.size() == 1);
    const gltf::Animation& animation = asset.animations[0];
    CHECK(animation.name == "walk");
    REQUIRE(animation.channels.size() == 2);
    REQUIRE(animation.samplers.size() == 2);

    // Channel 0: translation on the root (glTF node 1).
    const gltf::AnimationChannel& translation = animation.channels[0];
    CHECK(translation.targetNode == 1);
    CHECK(translation.targetPath == gltf::AnimPath::Translation);
    const gltf::AnimationSampler& translationSampler = animation.samplers[translation.sampler];
    CHECK(translationSampler.interpolation == gltf::AnimInterpolation::Linear);
    std::vector<f32> times;
    REQUIRE(gltf::ReadAccessorF32(asset, translationSampler.input, times));
    REQUIRE(times.size() == 2);
    CHECK(times[1] == 1.0f); // Seconds, no retiming.
    // The spec requires min/max on animation inputs.
    CHECK(asset.accessors[translationSampler.input].min.size() == 1);
    CHECK(asset.accessors[translationSampler.input].max[0] == 1.0);
    std::vector<f32> values;
    REQUIRE(gltf::ReadAccessorF32(asset, translationSampler.output, values));
    REQUIRE(values.size() == 6);
    // Pivot-relative: bind (1,2,3) + key offset, then the permutation.
    CHECK(values[0] == 2.0f); // (1,2,3)+(0,0,0) -> (2,3,1)
    CHECK(values[1] == 3.0f);
    CHECK(values[2] == 1.0f);
    CHECK(values[3] == 2.0f); // (1,2,3)+(1,0,0) = (2,2,3) -> (2,3,2)
    CHECK(values[4] == 3.0f);
    CHECK(values[5] == 2.0f);

    // Channel 1: Hermite rotation -> CUBICSPLINE with reordered, per-second
    // tangents.
    const gltf::AnimationChannel& rotation = animation.channels[1];
    CHECK(rotation.targetNode == 2);
    CHECK(rotation.targetPath == gltf::AnimPath::Rotation);
    const gltf::AnimationSampler& rotationSampler = animation.samplers[rotation.sampler];
    CHECK(rotationSampler.interpolation == gltf::AnimInterpolation::CubicSpline);
    REQUIRE(gltf::ReadAccessorF32(asset, rotationSampler.output, values));
    REQUIRE(values.size() == 2 * 3 * 4); // Two keys x {in, value, out} x quat.
    // Key 0 value slot: identity, w untouched by the permutation.
    CHECK(values[4 + 3] == 1.0f);
    // Key 0 out-tangent: (0.2,0,0,0) permutes to (0,0,0.2,0), and the 2-second
    // span divides it — per-span WEM tangents are per-second in glTF (R1).
    CHECK(values[8 + 2] == 0.1f);
    CHECK(values[8 + 0] == 0.0f);
    // Key 0 in-tangent is unused and written as zero.
    CHECK(values[0] == 0.0f);
    CHECK(values[2] == 0.0f);
}

TEST_CASE("gltf export is byte-stable and refuses honestly", "[wem][gltf]") {
    const Document document = makeFixture();
    const GltfConverter converter;

    Result<std::vector<u8>> first = converter.exportToBytes(document, ProfileId::Generic);
    Result<std::vector<u8>> second = converter.exportToBytes(document, ProfileId::Generic);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(*first == *second);

    // A profile the document does not carry is refused, not substituted.
    Result<std::vector<u8>> refused = converter.exportToBytes(document, ProfileId::Sc2);
    CHECK_FALSE(refused.ok());
    CHECK(refused.diagnostics.countOf(DiagCode::ProfileNotCarried) == 1);
}

TEST_CASE("gltf round-trips through its own importer", "[wem][gltf]") {
    const Document document = makeFixture();
    const GltfConverter converter;
    Result<std::vector<u8>> glb = converter.exportToBytes(document, ProfileId::Generic);
    REQUIRE(glb.ok());

    Result<Document> back = converter.importFromBytes(*glb);
    REQUIRE(back.ok());
    const Document& imported = *back;

    // Landed on Generic, the neutral profile import was designed for (§2).
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles[0] == ProfileId::Generic);
    REQUIRE(imported.models.size() == 1);
    const Model& model = imported.models[0];

    // The four exported nodes come back by name; the export's synthetic model
    // root is the import's root.
    REQUIRE(model.nodes.size() == 4);
    CHECK(model.nodes.nodes[0].name == "fixture");
    CHECK(model.nodes.nodes[1].name == "root");
    CHECK(model.nodes.nodes[2].name == "child");
    CHECK(model.nodes.nodes[1].parent == 0);
    CHECK(model.nodes.nodes[2].parent == 1);
    // The permutation inverts bit-exactly.
    CHECK(model.nodes.nodes[1].local.translation.x == 1.0f);
    CHECK(model.nodes.nodes[1].local.translation.y == 2.0f);
    CHECK(model.nodes.nodes[1].local.translation.z == 3.0f);
    CHECK(model.nodes.nodes[2].local.scale.x == 2.0f);
    CHECK(model.nodes.nodes[2].local.scale.y == 3.0f);
    CHECK(model.nodes.nodes[2].local.scale.z == 4.0f);
    // Joints and their ancestors became bones; the rig is explicit-bind with
    // the shipped inverse binds as matrices.
    CHECK(model.nodes.rig == RigConvention::ExplicitBind);
    CHECK(model.nodes.nodes[2].kind == NodeKind::Bone);
    REQUIRE(model.nodes.poseSchema.size() == 1);
    CHECK(model.nodes.poseSchema[0].storage == PoseStorage::Matrix);

    // Geometry: the two Generic-drawn triangles, positions bit-exact.
    REQUIRE(model.meshes.size() == 1);
    const Mesh& mesh = model.meshes[0];
    CHECK(mesh.faceCount() == 2);
    CHECK(mesh.vertexCount() == 4);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(positions.size() == 4);
    CHECK(positions[3].x == 3.0f);
    CHECK(positions[3].y == 4.0f);
    CHECK(positions[3].z == 5.0f);

    // Skin: normalised on the way out, absolute on the way back in.
    REQUIRE_FALSE(mesh.skin.empty());
    const std::span<const geom::Influence> influences = mesh.skin.forVertex(0);
    REQUIRE(influences.size() == 1);
    CHECK(influences[0].bone == 2);
    CHECK(influences[0].weight == 1.0f);

    // Material: the declared losses (§9) and nothing more.
    const ProfileMaterialSet* set = model.setFor(ProfileId::Generic);
    REQUIRE(set != nullptr);
    REQUIRE(set->materials.size() == 1);
    const CommonMaterial& common = set->materials[0].Common();
    CHECK(common.blend == BlendMode::AlphaBlend); // Additive left as BLEND.
    CHECK(common.cull == CullMode::None);
    const PbrDeferredBody* body = common.pbr();
    REQUIRE(body != nullptr);
    CHECK(body->baseColorFactor.x == 0.5f);
    CHECK(body->roughnessFactor == 0.4f);
    // Emissive strength times factor restores the HDR value exactly.
    CHECK(body->emissiveFactor.x == 4.0f);
    CHECK(body->emissiveFactor.y == 2.0f);
    const TextureInput* orm = body->find(PbrSlot::Orm);
    REQUIRE(orm != nullptr);
    CHECK(orm->wrapU == WrapMode::Clamp);
    REQUIRE(imported.textures.size() == 1);
    CHECK(imported.textures[0].path == "Stone_Floor.png");

    // Animation: seconds, counts and curve data intact; tangent conversion
    // inverts exactly (the span multiply is a power-of-two here).
    REQUIRE(imported.clips.size() == 1);
    const Clip& clip = imported.clips[0];
    CHECK(clip.name == "walk");
    CHECK(clip.duration == 2.0f);
    REQUIRE(clip.containers.size() == 1);
    REQUIRE(clip.containers[0].subTracks.size() == 2);
    const SubTrack& translation = clip.containers[0].subTracks[0];
    CHECK(translation.interp == Interpolation::Linear);
    REQUIRE(translation.times.size() == 2);
    const f32* translationValues = reinterpret_cast<const f32*>(translation.values.data());
    // Absolute now (the import rig is explicit-bind): bind + offset.
    CHECK(translationValues[0] == 1.0f);
    CHECK(translationValues[3] == 2.0f);
    CHECK(translationValues[4] == 2.0f);
    CHECK(translationValues[5] == 3.0f);
    const SubTrack& rotation = clip.containers[0].subTracks[1];
    CHECK(rotation.interp == Interpolation::Hermite);
    REQUIRE(rotation.times.size() == 2);
    const f32* rotationValues = reinterpret_cast<const f32*>(rotation.values.data());
    CHECK(rotationValues[3] == 1.0f);  // Key 0 value w.
    CHECK(rotationValues[8] == 0.2f);  // Key 0 outTan x, back in per-span units.

    // And the re-export of the import round-trips the container byte count.
    Result<std::vector<u8>> again = converter.exportToBytes(imported, ProfileId::Generic);
    REQUIRE(again.ok());
}

TEST_CASE("gltf import reads a hand-written .gltf text body", "[wem][gltf]") {
    // A single triangle with a data: URI buffer — positions (0,0,0) (1,0,0)
    // (0,1,0) as F32, indices 0 1 2 as U16.
    const char* body = R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "tri", "mesh": 0, "translation": [10, 20, 30]}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
        "materials": [{"name": "flat", "pbrMetallicRoughness": {
            "baseColorFactor": [1, 0, 0, 1], "metallicFactor": 0}}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
             "min": [0, 0, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}],
        "bufferViews": [
            {"buffer": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
        "buffers": [{"byteLength": 44, "uri":
            "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAA="}]
    })";
    const GltfConverter converter;
    Result<Document> imported = converter.importFromBytes(
        std::span<const u8>(reinterpret_cast<const u8*>(body), std::strlen(body)));
    REQUIRE(imported.ok());
    REQUIRE(imported->models.size() == 1);
    const Model& model = imported->models[0];
    REQUIRE(model.meshes.size() == 1);
    CHECK(model.meshes[0].faceCount() == 1);
    // Static geometry bakes through its node's world (translation 10,20,30 in
    // glTF space = 30,10,20 in Blizzard's) and binds rigid to the node.
    const std::span<const Vector3f> positions =
        model.meshes[0].attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(positions.size() == 3);
    CHECK(positions[0].x == 30.0f);
    CHECK(positions[0].y == 10.0f);
    CHECK(positions[0].z == 20.0f);
    REQUIRE(model.meshes[0].sections.size() == 1);
    REQUIRE(model.meshes[0].sections[0].rigidNode.has_value());
    CHECK(*model.meshes[0].sections[0].rigidNode == 0);
    const ProfileMaterialSet* set = model.setFor(ProfileId::Generic);
    REQUIRE(set != nullptr);
    REQUIRE(set->materials.size() == 1);
    CHECK(set->materials[0].name == "flat");
    CHECK(set->materials[0].Common().pbr()->baseColorFactor.x == 1.0f);
    CHECK(set->materials[0].Common().pbr()->baseColorFactor.y == 0.0f);
    CHECK(set->materials[0].Common().pbr()->metallicFactor == 0.0f);
}

namespace {

/// A one-node, one-channel document for the animation-window tests: no
/// geometry (the clip export does not need any), one translation channel on
/// the node, one clip of @p duration seconds.
Document makeClipFixture(f32 duration) {
    Document document;
    document.declare(ProfileId::Generic);
    Model model;
    model.name = "clips";
    Node node;
    node.name = "node";
    model.nodes.add(node);
    AnimChannel channel;
    channel.id = 1;
    channel.target.kind = TrackTarget::Kind::Node;
    channel.target.node = 0;
    channel.target.channel = Channel::Translation;
    channel.valueType = geom::AttrType::F32x3;
    model.animChannels.add(channel);
    document.models.push_back(std::move(model));

    Clip clip;
    clip.name = "clip";
    clip.model = 0;
    clip.duration = duration;
    clip.containers.push_back(SubTrackContainer{});
    document.clips.push_back(std::move(clip));
    return document;
}

SubTrack makeLinearTrack(u32 channel, std::initializer_list<f32> times,
                         std::initializer_list<Vector3f> values) {
    SubTrack track;
    track.channel = channel;
    track.interp = Interpolation::Linear;
    track.times.assign(times);
    for (const Vector3f& value : values) {
        const u8* raw = reinterpret_cast<const u8*>(&value.data[0]);
        track.values.insert(track.values.end(), raw, raw + sizeof(f32) * 3);
    }
    return track;
}

/// The animation's one sampler, decoded: (input times, output floats).
std::pair<std::vector<f32>, std::vector<f32>> soleSampler(const gltf::Asset& asset) {
    REQUIRE(asset.animations.size() == 1);
    REQUIRE(asset.animations[0].samplers.size() == 1);
    const gltf::AnimationSampler& sampler = asset.animations[0].samplers[0];
    std::vector<f32> times;
    std::vector<f32> values;
    REQUIRE(gltf::ReadAccessorF32(asset, sampler.input, times));
    REQUIRE(gltf::ReadAccessorF32(asset, sampler.output, values));
    return {times, values};
}

} // namespace

TEST_CASE("the generic slice evaluates out-of-window keys away", "[wem][gltf]") {
    // A linear segment spanning the whole 1s window: keys at -1s and +3s.
    // The cut evaluates the segment at the boundaries — value (1,0,0) at 0
    // (u = 0.25) and (2,0,0) at 1 (u = 0.5) — and drops the outside keys.
    Document document = makeClipFixture(1.0f);
    document.clips[0].containers[0].subTracks.push_back(makeLinearTrack(
        1, {-1.0f, 3.0f}, {Vector3f{0, 0, 0}, Vector3f{4, 0, 0}}));

    const GltfConverter converter;
    Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
    REQUIRE(exported.ok());
    const auto [times, values] = soleSampler(*exported);
    REQUIRE(times.size() == 2);
    CHECK(times[0] == 0.0f);
    CHECK(times[1] == 1.0f);
    REQUIRE(values.size() == 6);
    // Blizzard (1,0,0) permutes to glTF (0,0,1).
    CHECK(values[2] == 1.0f);
    CHECK(values[5] == 2.0f);
}

TEST_CASE("an mdx-windowed clip slices the engine's way", "[wem][gltf]") {
    const GltfConverter converter;

    SECTION("bracket keys drop and the boundaries wrap last to first") {
        // In-window keys at 0.2s (1,0,0) and 0.8s (3,0,0), brackets outside.
        // FindBracket wraps: segment last->first over (0.2-0.8)+1.0 = 0.4s,
        // cut by the end at u = (1.0-0.8)/0.4 = 0.5 -> (2,0,0). Before the
        // first key the engine measures it from the first key: u = -0.5 at
        // the start -> (4,0,0), and -0.0025 a millisecond short of the first
        // key -> (3.005,0,0). The bracket values must not appear anywhere.
        Document document = makeClipFixture(1.0f);
        document.clips[0].native.set("intervalStart", static_cast<i64>(1000));
        document.clips[0].containers[0].subTracks.push_back(makeLinearTrack(
            1, {-0.5f, 0.2f, 0.8f, 1.4f},
            {Vector3f{9, 9, 9}, Vector3f{1, 0, 0}, Vector3f{3, 0, 0}, Vector3f{7, 7, 7}}));

        Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
        REQUIRE(exported.ok());
        const auto [times, values] = soleSampler(*exported);
        REQUIRE(times.size() == 5);
        CHECK(times[0] == 0.0f);
        CHECK(times[1] == Catch::Approx(0.199f));
        CHECK(times[2] == 0.2f);
        CHECK(times[3] == 0.8f);
        CHECK(times[4] == 1.0f);
        REQUIRE(values.size() == 15);
        CHECK(values[2] == Catch::Approx(4.0f));   // The segment extrapolated at the start,
        CHECK(values[5] == Catch::Approx(3.005f)); // a millisecond short of the first key,
        CHECK(values[8] == 1.0f);                  // the in-window keys,
        CHECK(values[11] == 3.0f);
        CHECK(values[14] == 2.0f);                 // and the wrap cut at the end.
        for (const f32 value : values) {
            CHECK(value != 9.0f);
            CHECK(value != 7.0f);
        }
    }

    SECTION("a track with no in-window key is the node at rest — no channel") {
        Document document = makeClipFixture(1.0f);
        document.clips[0].native.set("intervalStart", static_cast<i64>(1000));
        document.clips[0].containers[0].subTracks.push_back(makeLinearTrack(
            1, {-2.0f, 5.0f}, {Vector3f{9, 9, 9}, Vector3f{7, 7, 7}}));

        Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
        REQUIRE(exported.ok());
        CHECK(exported->animations.empty());
    }

    SECTION("a rotation curve exports its key values under LINEAR, not squad") {
        // The stored Hermite \"tangents\" of an mdx rotation are squad control
        // quaternions; fed to CUBICSPLINE they bend every mid-segment pose.
        Document document = makeClipFixture(1.0f);
        document.clips[0].native.set("intervalStart", static_cast<i64>(1000));
        Model& model = document.models[0];
        AnimChannel rotation;
        rotation.id = 2;
        rotation.target.kind = TrackTarget::Kind::Node;
        rotation.target.node = 0;
        rotation.target.channel = Channel::Rotation;
        rotation.valueType = geom::AttrType::Quat;
        model.animChannels.add(rotation);
        SubTrack track;
        track.channel = 2;
        track.interp = Interpolation::Hermite;
        track.times = {0.0f, 1.0f};
        const f32 keys[24] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1,
                              0.7071f, 0, 0, 0.7071f, 0.7071f, 0, 0, 0.7071f,
                              0.7071f, 0, 0, 0.7071f};
        const u8* raw = reinterpret_cast<const u8*>(keys);
        track.values.assign(raw, raw + sizeof(keys));
        document.clips[0].containers[0].subTracks.push_back(std::move(track));

        Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
        REQUIRE(exported.ok());
        REQUIRE(exported->animations.size() == 1);
        const gltf::Animation& animation = exported->animations[0];
        REQUIRE(animation.samplers.size() == 1);
        CHECK(animation.samplers[0].interpolation == gltf::AnimInterpolation::Linear);
        std::vector<f32> values;
        REQUIRE(gltf::ReadAccessorF32(*exported, animation.samplers[0].output, values));
        REQUIRE(values.size() == 8); // Two plain keys, no tangent triples.
    }
}

TEST_CASE("the base colour goes to the first exportable textured stage", "[wem][gltf]") {
    // Stage 0 is a team-colour plate — a replaceable slot with no file behind
    // it — and stage 1 is the diffuse texture. The texture must win the slot;
    // claiming by position exported every classic body flat white.
    Document document = makeFixture();
    TextureRef teamColour;
    teamColour.replaceableId = 1;
    document.textures.push_back(teamColour); // Texture 1, after the fixture's.

    Model& model = document.models[0];
    ProfileMaterialSet* set = model.setFor(ProfileId::Generic);
    REQUIRE(set != nullptr);
    Material& material = set->materials[0];
    CommonMaterial& common = material.InitCommon();
    common.setKind(MaterialKind::Combiners);
    CombinersBody& body = *common.combiners();
    CombinerStage plate;
    plate.input.texture = 1; // The replaceable.
    body.stages.push_back(plate);
    CombinerStage diffuse;
    diffuse.input.texture = 0; // The fixture's real path.
    body.stages.push_back(diffuse);

    const GltfConverter converter;
    Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
    REQUIRE(exported.ok());
    REQUIRE(exported->materials.size() >= 1);
    const gltf::Material& out = exported->materials[0];
    REQUIRE(out.pbr.baseColorTexture.index != gltf::kNone);
    const gltf::Texture& texture = exported->textures[out.pbr.baseColorTexture.index];
    CHECK(exported->images[texture.source].name == "wem:texture:0");
    // And the mesh still has its primitive — a textured material is not
    // game-composited.
    REQUIRE(exported->meshes.size() == 1);
    CHECK(exported->meshes[0].primitives.size() >= 1);
}

TEST_CASE("a composite emits only through an emissive layer", "[wem][gltf]") {
    // The M3 import states `hdrEmissiveMultiplier` as the composite's emissive
    // factor, and ships it at 1 on materials with no emissive layer. glTF reads
    // a factor with no texture as constant emission, which exported every crate
    // on SM_ArmorySpectreCrate glowing flat white.
    Document document = makeFixture();
    Model& model = document.models[0];
    ProfileMaterialSet* set = model.setFor(ProfileId::Generic);
    REQUIRE(set != nullptr);
    CommonMaterial& common = set->materials[0].InitCommon();
    common.setKind(MaterialKind::Composite);
    CompositeBody& body = *common.composite();
    body.emissiveFactor = Vector4f{1.0f, 1.0f, 1.0f, 1.0f};
    CompositeLayer diffuse;
    diffuse.input.texture = 0;
    body.layers.push_back(diffuse);

    const GltfConverter converter;
    {
        Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
        REQUIRE(exported.ok());
        REQUIRE(exported->materials.size() == 1);
        const gltf::Material& out = exported->materials[0];
        CHECK_FALSE(out.emissiveTexture.present());
        CHECK(out.emissiveFactor.x == 0.0f);
        CHECK(out.emissiveFactor.y == 0.0f);
        CHECK(out.emissiveFactor.z == 0.0f);
    }

    // With a layer to scale, the factor is that layer's gain and crosses.
    CompositeLayer glow;
    glow.input.texture = 0;
    glow.target = SurfaceChannel::Emissive;
    body.layers.push_back(glow);
    Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
    REQUIRE(exported.ok());
    REQUIRE(exported->materials.size() == 1);
    const gltf::Material& out = exported->materials[0];
    CHECK(out.emissiveTexture.present());
    CHECK(out.emissiveFactor.x == 1.0f);
}

TEST_CASE("a specular exponent crosses as a perceptual roughness", "[wem][gltf]") {
    // glTF squares its roughness into the GGX alpha. Handing it the alpha itself
    // made StarCraft II's exponent 20 a lacquer at 0.30; the lobe match is 0.55.
    Document document = makeFixture();
    ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Generic);
    REQUIRE(set != nullptr);
    CommonMaterial& common = set->materials[0].InitCommon();
    common.setKind(MaterialKind::Composite);
    CompositeLayer diffuse;
    diffuse.input.texture = 0;
    common.composite()->layers.push_back(diffuse);

    const GltfConverter converter;
    const auto roughnessAt = [&](f32 exponent) {
        common.composite()->specularExponent = exponent;
        Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
        REQUIRE(exported.ok());
        REQUIRE(exported->materials.size() == 1);
        return exported->materials[0].pbr.roughnessFactor;
    };
    CHECK(roughnessAt(20.0f) == Catch::Approx(0.549f).margin(0.002f));
    CHECK(roughnessAt(80.0f) == Catch::Approx(0.395f).margin(0.002f));
    CHECK(roughnessAt(0.0f) == 1.0f);
}

TEST_CASE("a replaceable-only material's sections are skipped", "[wem][gltf]") {
    Document document = makeFixture();
    TextureRef teamGlow;
    teamGlow.replaceableId = 2;
    document.textures.push_back(teamGlow);

    Model& model = document.models[0];
    ProfileMaterialSet* set = model.setFor(ProfileId::Generic);
    REQUIRE(set != nullptr);
    Material& material = set->materials[0];
    CommonMaterial& common = material.InitCommon();
    common.setKind(MaterialKind::Combiners);
    CombinersBody& body = *common.combiners();
    CombinerStage glow;
    glow.input.texture = 1;
    body.stages.push_back(glow);

    const GltfConverter converter;
    Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
    REQUIRE(exported.ok());
    // Every drawn section wore that material; nothing survives, and the
    // diagnostics say why.
    CHECK(exported->meshes.empty());
    bool said = false;
    for (const Diagnostic& diagnostic : exported.diagnostics.all()) {
        said = said || diagnostic.message.find("game-composited") != std::string::npos;
    }
    CHECK(said);
}

TEST_CASE("a section alpha-keyed invisible in the default look is skipped", "[wem][gltf]") {
    Document document = makeFixture();
    Model& model = document.models[0];
    // An alpha channel on the drawn section (mesh 0, section 0), keyed to zero
    // for the whole first clip.
    AnimChannel alpha;
    alpha.id = 7;
    alpha.target.kind = TrackTarget::Kind::Section;
    alpha.target.mesh = 0;
    alpha.target.sub = 0;
    alpha.target.channel = Channel::Alpha;
    alpha.valueType = geom::AttrType::F32;
    model.animChannels.add(alpha);
    SubTrack track;
    track.channel = 7;
    track.interp = Interpolation::Step;
    track.times = {0.0f};
    const f32 zero = 0.0f;
    const u8* raw = reinterpret_cast<const u8*>(&zero);
    track.values.assign(raw, raw + sizeof(zero));
    document.clips[0].containers[0].subTracks.push_back(std::move(track));

    const GltfConverter converter;
    Result<gltf::Asset> exported = converter.toGltf(document, ProfileId::Generic);
    REQUIRE(exported.ok());
    CHECK(exported->meshes.empty());
}

TEST_CASE("gltf import sweeps the Khronos sample assets", "[wem][gltf][corpus]") {
    const char* dir = std::getenv("GLTF_SAMPLE_ASSETS_DIR");
    if (dir == nullptr || !fs::is_directory(dir)) {
        SKIP("GLTF_SAMPLE_ASSETS_DIR not set; the Sample-Assets sweep needs a local checkout");
    }
    std::vector<fs::path> files;
    for (const char* extension : {".glb", ".gltf"}) {
        for (const fs::path& file : test::gather("GLTF_SAMPLE_ASSETS_DIR", extension, {})) {
            files.push_back(file);
        }
    }
    const GltfConverter converter;
    u32 imported = 0;
    u32 withGeometry = 0;
    for (const fs::path& file : files) {
        test::trace(file);
        const auto bytes = test::readCorpusFile(file);
        if (bytes.empty()) {
            continue;
        }
        Result<Document> result =
            converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (!result.ok()) {
            continue; // External .bin URIs are unresolved here; declared.
        }
        ++imported;
        for (const Model& model : result->models) {
            withGeometry += model.meshes.empty() ? 0u : 1u;
        }
    }
    UNSCOPED_INFO("imported " << imported << "/" << files.size() << ", " << withGeometry
                              << " with geometry");
    CHECK(imported > 0);
}

namespace {

/// One corpus export sweep: import each file, export to `.glb`, re-parse with
/// our own parser and hold the counts. With `WEM_GLTF_EXPORT_DIR` set the
/// `.glb`s are also written to disk — the §11 validator gate runs the Khronos
/// validator over that directory.
template <class ImportFn>
void sweepGltfExports(const std::vector<fs::path>& files, std::size_t limit,
                      const char* format, bool checkSkins, ImportFn&& importFile) {
    const GltfConverter converter;
    const char* exportDir = std::getenv("WEM_GLTF_EXPORT_DIR");

    u32 exported = 0;
    u32 meshes = 0;
    u32 materials = 0;
    u32 skins = 0;
    u32 emptyExports = 0;
    u32 reparseFailures = 0;
    u32 countMismatches = 0;
    u32 skinFailures = 0;
    u32 importFailures = 0;
    std::vector<std::string> failing;

    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        Result<Document> imported = importFile(files[i]);
        if (!imported.ok() || imported->models.empty()) {
            continue;
        }
        const ProfileId profile = imported->profiles.empty() ? ProfileId::Generic
                                                             : imported->profiles.front();
        Result<gltf::Asset> asset = converter.toGltf(*imported, profile);
        if (!asset.ok()) {
            if (failing.size() < 10) {
                failing.push_back(test::pathText(files[i].filename()) + " -> " +
                                  asset.diagnostics.formatHistogram());
            }
            continue;
        }
        ++exported;

        const std::vector<u8> glb = gltf::Writer::ToGlb(*asset);
        if (exportDir != nullptr) {
            // No `.string()` on the stem: WC3 corpus names are full of
            // characters the active code page cannot map, and that call throws.
            fs::path target = fs::path(exportDir) / files[i].stem();
            target += ".glb";
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(glb.data()),
                      static_cast<std::streamsize>(glb.size()));
        }
        gltf::ParseOutcome reparsed = gltf::Parser::FromBytes(glb);
        if (!reparsed.ok()) {
            ++reparseFailures;
            if (failing.size() < 10) {
                failing.push_back(test::pathText(files[i].filename()) + " -> " + reparsed.error);
            }
            continue;
        }
        if (reparsed.asset->meshes.empty()) {
            ++emptyExports;
        }
        meshes += static_cast<u32>(reparsed.asset->meshes.size());
        materials += static_cast<u32>(reparsed.asset->materials.size());
        skins += static_cast<u32>(reparsed.asset->skins.size());

        // Gate 3: the written bytes import back — same mesh count, same clip
        // count, a valid document.
        Result<Document> roundTrip =
            converter.importFromBytes(std::span<const u8>(glb.data(), glb.size()));
        if (!roundTrip.ok() || roundTrip->models.empty() ||
            roundTrip->models[0].meshes.size() != reparsed.asset->meshes.size() ||
            roundTrip->clips.size() != reparsed.asset->animations.size()) {
            ++importFailures;
            if (failing.size() < 10) {
                failing.push_back(test::pathText(files[i].filename()) + " -> import-back");
            }
        }

        // Gate 4 for the pivot-relative formats, where the identity holds by
        // construction. An explicit-bind rig (M3's IREF) legitimately
        // disagrees with its rest chain on shipped content, so those sweeps
        // pass `checkSkins = false` and the fixture covers the transpose.
        if (checkSkins) {
            const u32 bad = skinIdentityFailures(*reparsed.asset, 1e-3f);
            if (bad != 0) {
                ++skinFailures;
                if (failing.size() < 10) {
                    failing.push_back(test::pathText(files[i].filename()) + " -> " +
                                      std::to_string(bad) + " joint(s) fail bind identity");
                }
            }
        }

        // The written counts match the in-memory asset — the container lost
        // nothing on the way through.
        if (reparsed.asset->nodes.size() != asset->nodes.size() ||
            reparsed.asset->meshes.size() != asset->meshes.size() ||
            reparsed.asset->materials.size() != asset->materials.size() ||
            reparsed.asset->accessors.size() != asset->accessors.size()) {
            ++countMismatches;
            if (failing.size() < 10) {
                failing.push_back(test::pathText(files[i].filename()) + " -> reparse counts");
            }
        }
    }

    for (const std::string& line : failing) {
        UNSCOPED_INFO(line);
    }
    std::cout << "[gltf<-" << format << "] exported " << exported << " file(s), " << meshes
              << " meshes, " << materials << " materials, " << skins << " skins, "
              << emptyExports << " empty\n";
    CHECK(exported > 0);
    CHECK(meshes > 0);
    CHECK(materials > 0);
    CHECK(reparseFailures == 0);
    CHECK(countMismatches == 0);
    CHECK(skinFailures == 0);
    CHECK(importFailures == 0);
}

} // namespace

TEST_CASE("gltf export corpus sweep: mdx", "[wem][gltf][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const MdxConverter mdx;
    sweepGltfExports(files, test::sweepLimit(files.size(), 300), "mdx", true,
                     [&](const fs::path& file) {
                         const auto bytes = test::readCorpusFile(file);
                         return mdx.importFromBytes(
                             std::span<const u8>(bytes.data(), bytes.size()));
                     });
}

TEST_CASE("gltf export corpus sweep: m3", "[wem][gltf][corpus]") {
    const auto files =
        test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    if (files.empty()) {
        SKIP("M3 corpus not found");
    }
    const M3Converter m3;
    sweepGltfExports(files, test::sweepLimit(files.size(), 300), "m3", false,
                     [&](const fs::path& file) {
                         const auto bytes = test::readCorpusFile(file);
                         return m3.importFromBytes(
                             std::span<const u8>(bytes.data(), bytes.size()));
                     });
}

// Hidden like the other m2 corpus legs: 56 known corpus files drive the M2
// parser into multi-GB resizes, so a full run needs a commit cap set outside.
TEST_CASE("gltf export corpus sweep: m2", "[wem][gltf][corpus][.m2slow]") {
    const auto files = test::gather("WEM_M2_CORPUS_DIR", ".m2", {"WoW", "WowM2"});
    if (files.empty()) {
        SKIP("M2 corpus not found");
    }
    const M2Converter m2Converter;
    sweepGltfExports(files, test::sweepLimit(files.size(), 100), "m2", true,
                     [&](const fs::path& file) {
                         utils::OsFileSystem vfs(test::pathText(file.parent_path()));
                         whiteout::m2::Parser parser;
                         const whiteout::m2::Model model = parser.parse(vfs, test::pathText(file));
                         if (model.skinProfiles.empty()) {
                             return Result<Document>{};
                         }
                         return m2Converter.fromM2(model);
                     });
}
