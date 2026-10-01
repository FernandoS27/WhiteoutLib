// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// FBX_OBJ_DESIGN F2-F7 — `FbxConverter` both ways.
///
/// The in-memory arms hold what only this crossing promises on a document small
/// enough to reason about: polygons stay polygons, positions come back
/// bit-exact through the declared basis, names, binds and weights survive, and
/// a clip's motion survives the bake and the resample within a tolerance (key
/// identity is not promised: the export bakes). The `[corpus]` arm imports
/// every file under WEM_FBX_CORPUS_DIR and, with WEM_FBX_DUMP_DIR set, writes
/// our own evaluation in the 3ds Max oracle's format for the referee.

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/fbx/scene.h>
#include <whiteout/models/wem/anim/pose.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/fbx_converter.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/skinning/deform.h>

#include "test_helpers.h"
#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;
namespace fs = std::filesystem;

namespace {

/// A two-bone chain skinning one quad, a red Phong material, and one clip that
/// turns the tip bone a quarter round and slides the root.
Document makeRig() {
    Document document;
    document.name = "rig";
    document.declare(ProfileId::Generic);
    Model model;
    model.name = "rig";
    model.materialSlots.push_back("paint");
    model.nodes.rig = RigConvention::ExplicitBind;

    Node root;
    root.name = "root";
    root.kind = NodeKind::Bone;
    root.resetPayloadForKind();
    root.local.translation = Vector3f{0, 0, 1};
    model.nodes.add(root);
    Node tip;
    tip.name = "tip";
    tip.parent = 0;
    tip.kind = NodeKind::Bone;
    tip.resetPayloadForKind();
    tip.local.translation = Vector3f{2, 0, 0};
    model.nodes.add(tip);
    Node ref;
    ref.name = "Hand Ref";
    ref.parent = 1;
    ref.kind = NodeKind::Attachment;
    ref.resetPayloadForKind();
    ref.local.translation = Vector3f{0.5f, 0, 0};
    model.nodes.add(ref);

    geom::MeshBuilder builder;
    const geom::VertexId a = builder.addVertex(Vector3f{0, -1, 0});
    const geom::VertexId b = builder.addVertex(Vector3f{3, -1, 0});
    const geom::VertexId c = builder.addVertex(Vector3f{3, 1, 0});
    const geom::VertexId d = builder.addVertex(Vector3f{0, 1, 0.25f});
    MeshSection section;
    section.name = "skin";
    section.materialSlot = 0;
    builder.addSection(section);
    const geom::VertexId corners[4] = {a, b, c, d};
    const geom::FaceId face = builder.addFace(corners, 0);
    const Vector2f uvs[4] = {{0.0f, 0.25f}, {1.0f, 0.25f}, {1.0f, 0.875f}, {0.0f, 0.875f}};
    for (u32 k = 0; k < 4; ++k) {
        builder.setCornerAttr(face, k, geom::names::kNormal, Vector3f{0, 0, 1});
        builder.setCornerAttr(face, k, geom::names::uv(0), uvs[k]);
    }
    builder.addInfluence(a, 0, 1.0f);
    builder.addInfluence(d, 0, 1.0f);
    builder.addInfluence(b, 1, 0.75f);
    builder.addInfluence(b, 0, 0.25f);
    builder.addInfluence(c, 1, 1.0f);
    geom::MeshBuilder::BuildOutcome outcome = builder.build();
    outcome.mesh.name = "body";
    model.meshes.push_back(std::move(outcome.mesh));

    ProfileMaterialSet set;
    set.profile = ProfileId::Generic;
    set.looks = LookTable::Single();
    Material material;
    material.name = "paint";
    CommonMaterial& common = material.InitCommon();
    common.setKind(MaterialKind::LegacyDeferred);
    common.legacy()->diffuseFactor = Vector4f{1, 0, 0, 1};
    common.legacy()->specularExponent = 32.0f;
    set.materials.push_back(std::move(material));
    set.resizeBindings(1);
    set.slotBindings[0].byLook[0] = 0;
    model.profileSets.push_back(std::move(set));

    // Bind = rest.
    PoseSchema bind;
    bind.name = "bind";
    bind.space = PoseSpace::Model;
    bind.inverse = true;
    bind.storage = PoseStorage::Matrix;
    model.nodes.poseSchema.push_back(bind);
    model.nodes.authoritativePose = 0;
    model.nodes.conformPoses();
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        const Matrix44f inverse = Matrix44f::inverse(ToMatrix(model.nodes.worldBind(i)));
        model.nodes.nodes[i].poseMatrices.assign(1, inverse);
        model.nodes.nodes[i].poses[0] = FromMatrix(inverse);
    }

    AnimChannel slide;
    slide.id = 1;
    slide.target.kind = TrackTarget::Kind::Node;
    slide.target.node = 0;
    slide.target.channel = Channel::Translation;
    slide.valueType = geom::AttrType::F32x3;
    model.animChannels.add(slide);
    AnimChannel turn;
    turn.id = 2;
    turn.target.kind = TrackTarget::Kind::Node;
    turn.target.node = 1;
    turn.target.channel = Channel::Rotation;
    turn.valueType = geom::AttrType::Quat;
    model.animChannels.add(turn);

    Clip clip;
    clip.name = "Stand";
    clip.model = 0;
    clip.duration = 1.0f;
    SubTrackContainer container;
    SubTrack translation;
    translation.channel = 1;
    translation.interp = Interpolation::Linear;
    translation.times = {0.0f, 1.0f};
    const f32 t[6] = {0, 0, 1, 0, 2, 1};
    translation.values.resize(sizeof(t));
    std::memcpy(translation.values.data(), t, sizeof(t));
    container.subTracks.push_back(translation);
    SubTrack rotation;
    rotation.channel = 2;
    rotation.interp = Interpolation::Slerp;
    rotation.times = {0.0f, 1.0f};
    const f32 half = std::sqrt(0.5f);
    const f32 q[8] = {0, 0, 0, 1, 0, 0, half, half};
    rotation.values.resize(sizeof(q));
    std::memcpy(rotation.values.data(), q, sizeof(q));
    container.subTracks.push_back(rotation);
    clip.containers.push_back(container);
    document.clips.push_back(clip);
    document.models.push_back(std::move(model));
    return document;
}

Result<FbxImport> RoundTrip(const Document& source, const FbxWriteOptions& options,
                            std::vector<u8>* bytesOut = nullptr) {
    const FbxConverter converter;
    Result<FbxExport> exported = converter.toFbx(source, ProfileId::Generic, options);
    REQUIRE(exported.ok());
    std::vector<u8> bytes = fbx::WriteBinary(exported->file);
    fbx::ReadOutcome read = fbx::Read(bytes);
    REQUIRE(read.ok());
    fbx::SceneOutcome scene = fbx::Scene::Build(std::move(*read.file));
    REQUIRE(scene.ok());
    if (bytesOut != nullptr) {
        *bytesOut = std::move(bytes);
    }
    return converter.fromFbx(*scene.scene);
}

u32 NodeNamed(const Model& model, const std::string& name) {
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes.nodes[i].name == name) {
            return i;
        }
    }
    return kInvalidNode;
}

Vector3f Origin(const Matrix44f& m) {
    return {m.data[3][0], m.data[3][1], m.data[3][2]};
}

std::vector<u8> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// WEM_FBX_DUMP_FRAMES: the comma-separated frames the dumps sample.
std::vector<f32> DumpFrames() {
    std::vector<f32> frames;
    const char* text = std::getenv("WEM_FBX_DUMP_FRAMES");
    if (text == nullptr) {
        return frames;
    }
    const std::string list = text;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t comma = list.find(',', start);
        const std::string item =
            list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty()) {
            frames.push_back(std::strtof(item.c_str(), nullptr));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return frames;
}

/// Our evaluation of @p document's first clip (WEM_FBX_DUMP_CLIP picks
/// another by index, or `last`: 3ds Max plays a file's last take) in the 3ds
/// Max oracle's record format (scripts/oracle/max_dump.ms): node worlds and
/// skinned vertices.
void DumpEvaluation(const fs::path& out, const Document& document, f64 fps,
                    const std::vector<f32>& frames) {
    const Model& model = document.models[0];
    const char* clipText = std::getenv("WEM_FBX_DUMP_CLIP");
    u32 clip = 0;
    if (clipText != nullptr) {
        clip = std::string_view(clipText) == "last" && !document.clips.empty()
                   ? static_cast<u32>(document.clips.size() - 1)
                   : static_cast<u32>(std::strtoul(clipText, nullptr, 10));
    }
    std::ofstream dump(out);
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        dump << "node|" << model.nodes.nodes[i].name << "|" << ToString(model.nodes.nodes[i].kind)
             << "|0|0|\n";
    }
    for (const f32 frame : frames) {
        const f32 seconds = static_cast<f32>(frame / fps);
        std::vector<Matrix44f> worlds(model.nodes.size(), Matrix44f::identity());
        std::vector<Matrix44f> skin;
        if (clip < document.clips.size()) {
            const ClipPose pose(document, 0, clip);
            for (u32 i = 0; i < model.nodes.size(); ++i) {
                worlds[i] = pose.frame(i, seconds);
            }
            pose.skinningAt(seconds, skin);
        } else {
            for (u32 i = 0; i < model.nodes.size(); ++i) {
                worlds[i] = ToMatrix(model.nodes.worldBind(i));
            }
        }
        for (u32 i = 0; i < model.nodes.size(); ++i) {
            const Matrix44f& m = worlds[i];
            dump << "pose|" << frame << "|" << model.nodes.nodes[i].name;
            for (std::size_t r = 0; r < 4; ++r) {
                for (std::size_t c = 0; c < 3; ++c) {
                    dump << "|" << m.data[r][c];
                }
            }
            dump << "\n";
        }
        for (const Mesh& mesh : model.meshes) {
            for (const Vector3f& p : skinning::DeformMesh(mesh, skin)) {
                dump << "vert|" << frame << "|" << mesh.name << "|" << p.x << "|" << p.y << "|" << p.z
                     << "\n";
            }
        }
    }
}

std::string Label(const fs::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
}

std::string LowerExtension(const fs::path& path) {
    std::string ext = path.extension().generic_string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

} // namespace

TEST_CASE("fbx export and import keep polygons, names and positions exactly", "[fbx][wem]") {
    const Document source = makeRig();
    for (const AxisPreset axes : {AxisPreset::YUp, AxisPreset::ZUpMax, AxisPreset::Native}) {
        FbxWriteOptions options;
        options.axes = axes;
        Result<FbxImport> back = RoundTrip(source, options);
        REQUIRE(back.ok());
        const Document& document = back->document;
        CHECK(document.profiles == std::vector<ProfileId>{ProfileId::Generic});
        REQUIRE(document.models.size() == 1);
        const Model& model = document.models[0];
        // The synthetic root and the mesh holder are stripped again.
        CHECK(model.nodes.size() == 3);
        CHECK(NodeNamed(model, "root") != kInvalidNode);
        const u32 ref = NodeNamed(model, "Hand Ref");
        REQUIRE(ref != kInvalidNode);
        CHECK(model.nodes.nodes[ref].kind == NodeKind::Attachment);
        REQUIRE(model.meshes.size() == 1);
        const Mesh& mesh = model.meshes[0];
        CHECK(mesh.faceCount() == 1);
        CHECK(mesh.faceSet().faceValence[0] == 4);
        const auto original = source.models[0].meshes[0].attributes.get<Vector3f>(
            geom::names::kPosition, geom::Domain::Vertex);
        const auto returned =
            mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        REQUIRE(returned.size() == original.size());
        for (const Vector3f& p : original) {
            bool found = false;
            for (const Vector3f& r : returned) {
                found = found || std::memcmp(&p, &r, sizeof(Vector3f)) == 0;
            }
            INFO("preset " << static_cast<int>(axes) << " wanted " << p.x << "," << p.y << "," << p.z);
            CHECK(found);
        }
        // The UV that is not symmetric under the flip.
        bool sawUv = false;
        for (const Vector2f& uv :
             mesh.attributes.get<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge)) {
            sawUv = sawUv || (uv.x == 1.0f && uv.y == 0.875f);
        }
        CHECK(sawUv);
        const Material* material = Resolve(model, 0, ProfileId::Generic, 0);
        REQUIRE(material != nullptr);
        REQUIRE(material->Common().legacy() != nullptr);
        CHECK(material->Common().legacy()->diffuseFactor.x == 1.0f);
        CHECK(material->Common().legacy()->specularExponent == 32.0f);
    }
}

TEST_CASE("fbx export and import keep the skin and its binds", "[fbx][wem]") {
    const Document source = makeRig();
    Result<FbxImport> back = RoundTrip(source, FbxWriteOptions{});
    REQUIRE(back.ok());
    const Model& model = back->document.models[0];
    const Mesh& mesh = model.meshes[0];
    REQUIRE(!mesh.skin.empty());
    const u32 root = NodeNamed(model, "root");
    const u32 tip = NodeNamed(model, "tip");
    REQUIRE(root != kInvalidNode);
    REQUIRE(tip != kInvalidNode);
    // Every bone's bind inverts its rest world here (bind = rest in the fixture).
    for (const u32 bone : {root, tip}) {
        const Matrix44f product = model.nodes.inverseBindMatrix(bone) * ToMatrix(model.nodes.worldBind(bone));
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t c = 0; c < 4; ++c) {
                CHECK(product.data[r][c] == Catch::Approx(r == c ? 1.0f : 0.0f).margin(1e-5));
            }
        }
    }
    // The shared vertex keeps both weights.
    bool sawShared = false;
    for (u32 v = 0; v < mesh.skin.vertexCount(); ++v) {
        const auto influences = mesh.skin.forVertex(v);
        if (influences.size() == 2) {
            f32 tipWeight = 0.0f;
            for (const geom::Influence& influence : influences) {
                if (influence.bone == tip) {
                    tipWeight = influence.weight;
                }
            }
            CHECK(tipWeight == Catch::Approx(0.75f));
            sawShared = true;
        }
    }
    CHECK(sawShared);
}

TEST_CASE("fbx animation survives the bake and the resample", "[fbx][wem]") {
    const Document source = makeRig();
    Result<FbxImport> back = RoundTrip(source, FbxWriteOptions{});
    REQUIRE(back.ok());
    const Document& document = back->document;
    REQUIRE(document.clips.size() == 1);
    CHECK(document.clips[0].name == "Stand");
    CHECK(document.clips[0].duration == Catch::Approx(1.0f));
    const ClipPose before(source, 0, 0);
    const ClipPose after(document, 0, 0);
    const Model& model = document.models[0];
    for (const f32 t : {0.0f, 0.25f, 0.5f, 0.9f, 1.0f}) {
        for (const char* name : {"root", "tip", "Hand Ref"}) {
            const u32 a = NodeNamed(source.models[0], name);
            const u32 b = NodeNamed(model, name);
            REQUIRE(b != kInvalidNode);
            const Vector3f pa = Origin(before.frame(a, t));
            const Vector3f pb = Origin(after.frame(b, t));
            INFO(name << " at " << t);
            CHECK(pb.x == Catch::Approx(pa.x).margin(2e-3));
            CHECK(pb.y == Catch::Approx(pa.y).margin(2e-3));
            CHECK(pb.z == Catch::Approx(pa.z).margin(2e-3));
        }
    }
}

TEST_CASE("fbx export is deterministic and refuses honestly", "[fbx][wem]") {
    const Document source = makeRig();
    std::vector<u8> first;
    std::vector<u8> second;
    (void)RoundTrip(source, FbxWriteOptions{}, &first);
    (void)RoundTrip(source, FbxWriteOptions{}, &second);
    CHECK(first == second);
    const FbxConverter converter;
    CHECK_FALSE(converter.toFbx(source, ProfileId::Wow).ok());
    FbxWriteOptions old;
    old.version = 6100;
    CHECK_FALSE(converter.toFbx(source, ProfileId::Generic, old).ok());
    const std::string junk = "Kaydara FBX Binary  \0\x1a\0";
    CHECK_FALSE(converter
                    .importFromBytes(std::span<const u8>(reinterpret_cast<const u8*>(junk.data()),
                                                         junk.size()))
                    .ok());
}

TEST_CASE("fbx export declares the unit it is given and import restates it", "[fbx][wem]") {
    for (const LengthUnit unit : kLengthUnits) {
        CHECK(LengthUnitOf(CentimetresPer(unit)) == unit);
        CHECK(LengthUnitFromSymbol(LengthUnitSymbol(unit)) == unit);
    }
    CHECK(LengthUnitOf(2.54001) == LengthUnit::Inch);
    CHECK_FALSE(LengthUnitOf(10.0).has_value());
    CHECK_FALSE(LengthUnitFromSymbol("yd").has_value());

    // The numbers are the caller's to restate; declared as metres, each reads
    // back as a hundred centimetres.
    const Document source = makeRig();
    FbxWriteOptions metres;
    metres.unitScaleFactor = CentimetresPer(LengthUnit::Metre);
    Result<FbxImport> back = RoundTrip(source, metres);
    REQUIRE(back.ok());
    CHECK(back->unitScaleFactor == 100.0);
    const auto original = source.models[0].meshes[0].attributes.get<Vector3f>(
        geom::names::kPosition, geom::Domain::Vertex);
    const auto returned = back->document.models[0].meshes[0].attributes.get<Vector3f>(
        geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(returned.size() == original.size());
    for (const Vector3f& p : original) {
        bool found = false;
        for (const Vector3f& r : returned) {
            found = found || (std::fabs(r.x - p.x * 100.0f) < 1e-3f && std::fabs(r.y - p.y * 100.0f) < 1e-3f &&
                              std::fabs(r.z - p.z * 100.0f) < 1e-3f);
        }
        INFO("wanted " << p.x * 100.0f << "," << p.y * 100.0f << "," << p.z * 100.0f);
        CHECK(found);
    }
    CHECK(RoundTrip(source, FbxWriteOptions{})->unitScaleFactor == 1.0);
}

// 3ds Max 2027's PBR materials, restated from files it wrote: the Physical
// material's `3dsMax|Parameters|` compound and the glTF material's
// `3dsMax|main|`, each beside a Phong restatement that must not win.
TEST_CASE("fbx import reads 3ds Max's PBR materials", "[fbx][wem]") {
    const std::string text = R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXVersion: 7400
}
Objects:  {
	Geometry: 10, "Geometry::tri", "Mesh" {
		Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
		PolygonVertexIndex: *3 { a: 0,1,-3 }
	}
	Model: 20, "Model::PhysicalBox", "Mesh" {
	}
	Geometry: 11, "Geometry::tri2", "Mesh" {
		Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
		PolygonVertexIndex: *3 { a: 0,1,-3 }
	}
	Model: 21, "Model::GltfBox", "Mesh" {
	}
	Material: 30, "Material::PhysicalMat", "" {
		ShadingModel: "unknown"
		Properties70:  {
			P: "DiffuseColor", "ColorRGB", "Color", "",0.78,0.39,0.19
			P: "ShininessExponent", "double", "Number", "",90.5
			P: "3dsMax|ORIGINAL_MTL", "KString", "", "", "PHYSICAL_MTL"
			P: "3dsMax|Parameters|base_weight", "Float", "", "A",1
			P: "3dsMax|Parameters|base_color", "ColorAndAlpha", "", "A",0.5,0.25,0.125,1
			P: "3dsMax|Parameters|roughness", "Float", "", "A",0.35
			P: "3dsMax|Parameters|roughness_inv", "Bool", "", "A",0
			P: "3dsMax|Parameters|metalness", "Float", "", "A",0.7
			P: "3dsMax|Parameters|transparency", "Float", "", "A",0
			P: "3dsMax|Parameters|emission", "Float", "", "A",0.5
			P: "3dsMax|Parameters|emit_color", "ColorAndAlpha", "", "A",0.2,0.4,0.6,1
		}
	}
	Material: 31, "Material::GltfMat", "" {
		ShadingModel: "unknown"
		Properties70:  {
			P: "3dsMax|main|baseColor", "ColorAndAlpha", "", "A",0.2,0.6,1,1
			P: "3dsMax|main|alphaMode", "Integer", "", "A",2
			P: "3dsMax|main|alphaCutoff", "Float", "", "A",0.25
			P: "3dsMax|main|metalness", "Float", "", "A",0.2
			P: "3dsMax|main|roughness", "Float", "", "A",0.6
			P: "3dsMax|main|DoubleSided", "Bool", "", "A",1
		}
	}
	Texture: 40, "Texture::base", "" {
		RelativeFilename: "base.png"
	}
	Texture: 41, "Texture::rough", "" {
		RelativeFilename: "rough.png"
	}
	Texture: 42, "Texture::Normal_Bump", "" {
		FileName: "C:\maps\normal.png"
		RelativeFilename: "maps\normal.png"
	}
}
Connections:  {
	C: "OO",20,0
	C: "OO",10,20
	C: "OO",30,20
	C: "OO",21,0
	C: "OO",11,21
	C: "OO",31,21
	C: "OP",40,30, "DiffuseColor"
	C: "OP",41,30, "ShininessExponent"
	C: "OP",40,30, "3dsMax|Parameters|base_color_map"
	C: "OP",41,30, "3dsMax|Parameters|roughness_map"
	C: "OP",42,30, "3dsMax|Parameters|bump_map"
	C: "OP",42,30, "3dsMax|Parameters|normalCamera"
	C: "OP",41,31, "3dsMax|main|metalnessMap"
}
)";
    const FbxConverter converter;
    fbx::ReadOutcome read = fbx::Read(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    REQUIRE(read.ok());
    fbx::SceneOutcome scene = fbx::Scene::Build(std::move(*read.file));
    REQUIRE(scene.ok());
    Result<FbxImport> imported = converter.fromFbx(*scene.scene);
    REQUIRE(imported.ok());
    const Document& document = imported->document;
    const ProfileMaterialSet& set = document.models[0].profileSets[0];
    REQUIRE(set.materials.size() == 2);

    const CommonMaterial& physical = set.materials[0].Common();
    REQUIRE(physical.kind() == MaterialKind::PBRDeferred);
    const PbrDeferredBody& p = *physical.pbr();
    CHECK(p.baseColorFactor.x == Catch::Approx(0.5f));
    CHECK(p.roughnessFactor == Catch::Approx(0.35f));
    CHECK(p.metallicFactor == Catch::Approx(0.7f));
    CHECK(p.emissiveFactor.z == Catch::Approx(0.3f));
    REQUIRE(p.find(PbrSlot::BaseColor) != nullptr);
    REQUIRE(p.find(PbrSlot::Roughness) != nullptr);
    REQUIRE(p.find(PbrSlot::Normal) != nullptr);
    CHECK(p.slots.size() == 3); // the Phong restatement added nothing
    CHECK(document.textures[p.find(PbrSlot::Roughness)->texture].path == "rough.png");
    // Both spellings reach the host's search.
    const u32 normal = p.find(PbrSlot::Normal)->texture;
    REQUIRE(normal < imported->sources.size());
    CHECK(imported->sources[normal].relative == "maps/normal.png");
    CHECK(imported->sources[normal].absolute == "C:/maps/normal.png");

    const CommonMaterial& gltf = set.materials[1].Common();
    REQUIRE(gltf.kind() == MaterialKind::PBRDeferred);
    CHECK(gltf.blend == BlendMode::AlphaKey);
    CHECK(gltf.alphaTestThreshold == Catch::Approx(0.25f));
    CHECK(gltf.cull == CullMode::None);
    CHECK(gltf.pbr()->roughnessFactor == Catch::Approx(0.6f));
    CHECK(gltf.pbr()->find(PbrSlot::Metallic) != nullptr);
}

TEST_CASE("fbx corpus files import", "[fbx][wem][corpus]") {
    const char* root = std::getenv("WEM_FBX_CORPUS_DIR");
    if (root == nullptr || !fs::is_directory(root)) {
        SKIP("set WEM_FBX_CORPUS_DIR to a folder of .fbx files");
    }
    const char* dumpDir = std::getenv("WEM_FBX_DUMP_DIR");
    const std::vector<f32> frames = DumpFrames();
    const FbxConverter converter;
    u32 files = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || LowerExtension(entry.path()) != ".fbx") {
            continue;
        }
        INFO(Label(entry.path()));
        const std::vector<u8> bytes = ReadAll(entry.path());
        fbx::ReadOutcome read = fbx::Read(bytes);
        REQUIRE(read.ok());
        fbx::SceneOutcome scene = fbx::Scene::Build(std::move(*read.file));
        REQUIRE(scene.ok());
        const f64 fps = scene.scene->globals().frameRate();
        Result<FbxImport> imported = converter.fromFbx(*scene.scene);
        REQUIRE(imported.ok());
        const Document& document = imported->document;
        REQUIRE(document.models.size() == 1);
        const Model& model = document.models[0];
        CHECK((model.nodes.size() > 0 || !model.meshes.empty()));
        ++files;
        std::cout << "[fbx import] " << entry.path().filename().generic_string() << ": "
                  << model.nodes.size() << " node(s), " << model.meshes.size() << " mesh(es), "
                  << document.clips.size() << " clip(s), " << document.textures.size()
                  << " texture(s), " << imported->media.size() << " embedded\n";
        if (dumpDir != nullptr) {
            fs::path out = fs::path(dumpDir) / entry.path().filename();
            out.replace_extension(".ours.txt");
            DumpEvaluation(out, document, fps, frames);
        }
    }
    std::cout << "[fbx import] " << files << " file(s)\n";
    CHECK(files > 0);
}

// FBX_OBJ_DESIGN gate 6: the MDX corpus exports with no refusal, the same
// bytes twice, and reads back with every mesh and clip it went out with.
TEST_CASE("fbx export corpus sweep: mdx", "[fbx][wem][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const MdxConverter mdx;
    const FbxConverter converter;
    const std::size_t limit = test::sweepLimit(files.size(), 300);
    u32 exported = 0;
    std::vector<std::string> failing;
    for (std::size_t i = 0; i < files.size() && i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        Result<Document> document = mdx.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (!document.ok()) {
            continue; // the MDX side's own sweep answers for this
        }
        const ProfileId profile = document->defaultProfile;
        Result<std::vector<u8>> first = converter.exportToBytes(*document, profile);
        Result<std::vector<u8>> second = converter.exportToBytes(*document, profile);
        const std::string name = test::pathText(files[i].filename());
        if (!first.ok() || !second.ok()) {
            failing.push_back(name + " -> refused");
            continue;
        }
        if (*first != *second) {
            failing.push_back(name + " -> bytes differ between two exports");
            continue;
        }
        Result<Document> back = converter.importFromBytes(*first);
        if (!back.ok()) {
            failing.push_back(name + " -> did not read back");
            continue;
        }
        ++exported;
    }
    for (const std::string& f : failing) {
        UNSCOPED_INFO(f);
    }
    std::cout << "[fbx<-mdx] exported " << exported << " file(s), " << failing.size() << " failing\n";
    CHECK(exported > 0);
    CHECK(failing.empty());
}

// The export half of the referee: each model WEM_FBX_EXPORT_IN lists (`;`
// separated .mdx / .m3) is written as FBX into WEM_FBX_DUMP_DIR beside our own
// evaluation of it, for the oracle to import and the referee to compare.
TEST_CASE("fbx export for the oracle", "[fbx][wem][.oracle]") {
    const char* list = std::getenv("WEM_FBX_EXPORT_IN");
    const char* dumpDir = std::getenv("WEM_FBX_DUMP_DIR");
    if (list == nullptr || dumpDir == nullptr) {
        SKIP("set WEM_FBX_EXPORT_IN and WEM_FBX_DUMP_DIR");
    }
    const std::vector<f32> frames = DumpFrames();
    const FbxConverter converter;
    const FbxWriteOptions options;
    const std::string text = list;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t semi = text.find(';', start);
        const std::string item = text.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        start = semi == std::string::npos ? text.size() : semi + 1;
        const fs::path source(std::u8string(item.begin(), item.end()));
        INFO(Label(source));
        const std::vector<u8> bytes = ReadAll(source);
        const std::span<const u8> view(bytes.data(), bytes.size());
        Result<Document> document = LowerExtension(source) == ".m3" ? M3Converter{}.importFromBytes(view)
                                                                     : MdxConverter{}.importFromBytes(view);
        REQUIRE(document.ok());
        Result<FbxExport> exported = converter.toFbx(*document, document->defaultProfile, options);
        REQUIRE(exported.ok());
        const std::vector<u8> fbxBytes = fbx::WriteBinary(exported->file);
        fs::path out = fs::path(dumpDir) / source.filename();
        out.replace_extension(".fbx");
        std::ofstream(out, std::ios::binary)
            .write(reinterpret_cast<const char*>(fbxBytes.data()), static_cast<std::streamsize>(fbxBytes.size()));
        out.replace_extension(".ours.txt");
        DumpEvaluation(out, *document, options.frameRate, frames);
        std::cout << "[fbx oracle] " << Label(out) << ": " << document->clips.size() << " clip(s)\n";
    }
}
