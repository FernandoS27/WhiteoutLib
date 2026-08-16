// Round-trip coverage for pre-Legion M2 versions: write a synthetic model at
// vanilla/BC/WotLK versions, re-parse it, and compare the animation data in
// depth. The ≤TBC path exercises the global-timeline track conversion; the
// WotLK path exercises path-named `.skin`/`.anim` siblings.

#include <catch2/catch_all.hpp>

#include <whiteout/models/m2/m2.h>
#include <whiteout/utils/os_file_system.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::m2;

namespace {

Model buildModel() {
    Model model;
    model.modelName = "LegacyTest";

    Sequence stand;
    stand.id = 0;
    stand.duration = 1000;
    stand.flags = SequenceFlag::Looping; // 0x20: keys live in the .m2
    stand.blendTimeIn = 150;
    stand.blendTimeOut = 150;
    Sequence walk;
    walk.id = 4;
    walk.duration = 2000;
    walk.flags = SequenceFlag::Looping;
    walk.movespeed = 2.5f;
    walk.blendTimeIn = 150;
    walk.blendTimeOut = 150;
    model.sequences = {stand, walk};
    model.sequenceIdxHashById = {0, 1};

    model.globalLoops.push_back(GlobalSequence{3000});

    Bone root;
    root.keyBoneId = -1;
    root.boneNameCRC = 0xDEADBEEF;
    root.translation.interpolationType = InterpolationType::Linear;
    root.translation.timestamps = {{0, 500, 1000}, {0, 2000}};
    root.translation.values = {{Vector3f{0, 0, 0}, Vector3f{1, 2, 3}, Vector3f{0, 0, 0}},
                               {Vector3f{4, 5, 6}, Vector3f{7, 8, 9}}};
    root.rotation.interpolationType = InterpolationType::Linear;
    root.rotation.timestamps = {{0, 1000}, {0}};
    root.rotation.values = {{CompatQuaternion::fromFloats(0, 0, 0, 1),
                             CompatQuaternion::fromFloats(0.7071f, 0, 0, 0.7071f)},
                            {CompatQuaternion::fromFloats(0, 0, 0, 1)}};
    root.pivot = Vector3f{0.5f, 0.5f, 0.5f};

    Bone child;
    child.parentBoneId = 0;
    // A track animated on a global loop: single span, its own clock.
    child.scale.interpolationType = InterpolationType::Linear;
    child.scale.globalSequenceId = 0;
    child.scale.timestamps = {{0, 1500, 3000}};
    child.scale.values = {{Vector3f{1, 1, 1}, Vector3f{2, 2, 2}, Vector3f{1, 1, 1}}};
    model.bones = {root, child};
    model.keyBoneIds = {0};

    Vertex v;
    v.position = Vector3f{1, 2, 3};
    v.boneWeights = {255, 0, 0, 0};
    v.normal = Vector3f{0, 0, 1};
    model.vertices = {v, v, v};

    SkinProfile profile;
    profile.vertices = {0, 1, 2};
    profile.indices = {0, 1, 2};
    profile.bones = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    SkinSection section;
    section.vertexCount = 3;
    section.indexCount = 3;
    section.boneCount = 1;
    section.boneInfluences = 1;
    section.centerPosition = Vector3f{1, 2, 3};
    section.sortCenterPosition = Vector3f{4, 5, 6};
    section.sortRadius = 7.0f;
    profile.submeshes = {section};
    Batch batch;
    batch.textureCount = 1;
    profile.batches = {batch};
    model.skinProfiles = {profile};
    model.numSkinProfiles = 1;

    Texture tex;
    tex.type = 0;
    tex.filename = "Creature\\LegacyTest\\Body.blp";
    model.textures = {tex};
    model.textureIndicesById = {0};
    model.textureCombos = {0};
    model.materials = {Material{0, 0}};

    TextureWeight weight;
    weight.weight.interpolationType = InterpolationType::Linear;
    weight.weight.timestamps = {{0, 1000}, {}};
    weight.weight.values = {{32767, 16384}, {}};
    model.textureWeights = {weight};
    model.textureWeightCombos = {0};

    TextureTransform transform;
    transform.rotation.interpolationType = InterpolationType::Linear;
    transform.rotation.timestamps = {{0, 1000}, {}};
    transform.rotation.values = {{Quaternion(0, 0, 0, 1), Quaternion(0, 0.5f, 0, 0.866f)}, {}};
    model.textureTransforms = {transform};
    model.textureTransformCombos = {0};

    ColorAnimation color;
    color.color.interpolationType = InterpolationType::Linear;
    color.color.timestamps = {{0}, {}};
    color.color.values = {{Vector3f{1, 0.5f, 0.25f}}, {}};
    model.colors = {color};

    Event event;
    event.identifier = 0x24242424;
    event.enabled.timestamps = {{250, 750}, {500}};
    model.events = {event};

    Camera camera;
    camera.type = 0;
    camera.fieldOfView = 0.9f;
    camera.farClip = 100.0f;
    camera.nearClip = 0.1f;
    camera.positionBase = Vector3f{10, 0, 2};
    model.cameras = {camera};
    model.cameraIndicesById = {0};

    RibbonEmitter ribbon;
    ribbon.boneId = 0;
    ribbon.edgesPerSecond = 20.0f;
    ribbon.edgeLifetime = 1.5f;
    ribbon.textureIndices = {0};
    ribbon.materialIndices = {0};
    ribbon.heightAbove.timestamps = {{0}, {}};
    ribbon.heightAbove.values = {{0.4f}, {}};
    model.ribbonEmitters = {ribbon};

    ParticleEmitter particle;
    particle.particleId = UINT32_MAX;
    particle.boneId = 0;
    particle.textureId = 0;
    particle.blendingType = ParticleBlending::Additive;
    particle.emitterType = ParticleEmitterType::Plane;
    particle.particleType = 0;
    particle.headOrTail = 0;
    particle.rows = 1;
    particle.columns = 1;
    particle.lifespan.timestamps = {{0}, {}};
    particle.lifespan.values = {{2.0f}, {}};
    particle.emissionRate.timestamps = {{0}, {}};
    particle.emissionRate.values = {{15.0f}, {}};
    particle.colorTrack.timestamps = {unorm16::from_raw(0), unorm16::from_raw(16384),
                                      unorm16::from_raw(32767)};
    particle.colorTrack.values = {Vector3f{255, 128, 0}, Vector3f{128, 128, 128},
                                  Vector3f{0, 64, 255}};
    particle.alphaTrack.timestamps = particle.colorTrack.timestamps;
    particle.alphaTrack.values = {unorm16::from_raw(32767), unorm16::from_raw(16384),
                                  unorm16::from_raw(0)};
    particle.scaleTrack.timestamps = particle.colorTrack.timestamps;
    particle.scaleTrack.values = {Vector2f{1, 1}, Vector2f{2, 2}, Vector2f{0.5f, 0.5f}};
    model.particleEmitters = {particle};

    model.playableAnimationLookup = {0x00010000, 0x00010004};
    model.textureFlipbooks = {7, 9};

    return model;
}

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        path = fs::temp_directory_path() / "whiteout_m2_legacy" / name;
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        if (std::getenv("M2_TEST_KEEP") != nullptr) {
            return;
        }
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

Model roundTrip(const Model& model, u32 version, const std::string& tag,
                std::vector<std::string>* parseIssues = nullptr) {
    TempDir dir("v" + std::to_string(version) + "_" + tag);

    WriteOptions opts;
    opts.format = Format::ClassicMD20;
    opts.m2Version = version;
    Writer writer(opts);
    utils::OsFileSystem outFs(dir.path.string());
    writer.write(outFs, "model.m2", model);
    INFO("writer issues: " << (writer.hasIssues() ? writer.getIssues()[0] : "none"));
    REQUIRE_FALSE(writer.hasIssues());

    utils::OsFileSystem inFs(dir.path.string());
    Parser parser;
    Model result = parser.parse(inFs, "model.m2");
    if (parseIssues) {
        *parseIssues = parser.getIssues();
    } else {
        INFO("parse issues: " << (parser.hasIssues() ? parser.getIssues()[0] : "none"));
        REQUIRE_FALSE(parser.hasIssues());
    }
    return result;
}

void checkCommon(const Model& in, const Model& out, u32 version) {
    REQUIRE(out.sequences.size() == in.sequences.size());
    for (size_t i = 0; i < in.sequences.size(); ++i) {
        CAPTURE(i);
        CHECK(out.sequences[i].id == in.sequences[i].id);
        CHECK(out.sequences[i].duration == in.sequences[i].duration);
        CHECK(out.sequences[i].blendTimeIn == in.sequences[i].blendTimeIn);
    }

    REQUIRE(out.bones.size() == in.bones.size());
    const auto& inRoot = in.bones[0];
    const auto& outRoot = out.bones[0];
    CHECK(outRoot.boneNameCRC == inRoot.boneNameCRC);
    REQUIRE(outRoot.translation.timestamps.size() == inRoot.translation.timestamps.size());
    for (size_t s = 0; s < inRoot.translation.timestamps.size(); ++s) {
        CAPTURE(s);
        CHECK(outRoot.translation.timestamps[s] == inRoot.translation.timestamps[s]);
        REQUIRE(outRoot.translation.values[s].size() == inRoot.translation.values[s].size());
        for (size_t k = 0; k < inRoot.translation.values[s].size(); ++k) {
            CHECK(outRoot.translation.values[s][k].x == inRoot.translation.values[s][k].x);
            CHECK(outRoot.translation.values[s][k].y == inRoot.translation.values[s][k].y);
            CHECK(outRoot.translation.values[s][k].z == inRoot.translation.values[s][k].z);
        }
    }

    // Global-sequence track: single span, unsliced, on its own clock.
    const auto& inScale = in.bones[1].scale;
    const auto& outScale = out.bones[1].scale;
    CHECK(outScale.globalSequenceId == 0);
    REQUIRE(outScale.timestamps.size() == 1);
    CHECK(outScale.timestamps[0] == inScale.timestamps[0]);

    // The float-quaternion texture transform rotation (all versions).
    REQUIRE(out.textureTransforms.size() == 1);
    const auto& inRot = in.textureTransforms[0].rotation;
    const auto& outRot = out.textureTransforms[0].rotation;
    REQUIRE_FALSE(outRot.values.empty());
    REQUIRE(outRot.values[0].size() == inRot.values[0].size());
    for (size_t k = 0; k < inRot.values[0].size(); ++k) {
        CHECK(outRot.values[0][k].x == inRot.values[0][k].x);
        CHECK(outRot.values[0][k].y == inRot.values[0][k].y);
        CHECK(outRot.values[0][k].z == inRot.values[0][k].z);
        CHECK(outRot.values[0][k].w == inRot.values[0][k].w);
    }

    REQUIRE(out.events.size() == 1);
    CHECK(out.events[0].enabled.timestamps == in.events[0].enabled.timestamps);

    CHECK(out.vertices.size() == in.vertices.size());
    REQUIRE(out.textures.size() == 1);
    CHECK(out.textures[0].filename == in.textures[0].filename);
    REQUIRE(out.cameras.size() == 1);
    if (version < M2_VERSION_CATA) {
        // Cata replaced the fixed FOV with a spline track.
        CHECK(out.cameras[0].fieldOfView == in.cameras[0].fieldOfView);
    }
}

} // namespace

TEST_CASE("M2 legacy versions round-trip", "[m2][legacy]") {
    const Model model = buildModel();

    for (u32 const version : {u32(M2_VERSION_VANILLA), u32(M2_VERSION_BC), u32(263)}) {
        DYNAMIC_SECTION("version " << version) {
            Model out = roundTrip(model, version, "legacy");
            checkCommon(model, out, version);

            // Embedded skin profiles: no external .skin sibling exists.
            REQUIRE(out.skinProfiles.size() == 1);
            CHECK(out.numSkinProfiles == 0);
            const auto& outSection = out.skinProfiles[0].submeshes.at(0);
            CHECK(outSection.vertexCount == 3);
            if (version >= M2_VERSION_BC) {
                CHECK(outSection.sortRadius == 7.0f);
            } else {
                // Vanilla sections carry no sort data; the parser mirrors the
                // center instead.
                CHECK(outSection.sortRadius == 0.0f);
                CHECK(outSection.sortCenterPosition.x == outSection.centerPosition.x);
            }

            // ≤TBC-only header arrays survive.
            CHECK(out.playableAnimationLookup == model.playableAnimationLookup);
            CHECK(out.textureFlipbooks == model.textureFlipbooks);

            // Bone rotation: exact for compressed storage, one-tick tolerance
            // through the vanilla float representation. A one-key sub-track
            // comes back as an equal-valued boundary pair — that is what the
            // global-timeline layout stores for a constant sequence.
            const auto& inQ = model.bones[0].rotation.values;
            const auto& outQ = out.bones[0].rotation.values;
            REQUIRE(outQ.size() == inQ.size());
            for (size_t s = 0; s < inQ.size(); ++s) {
                CAPTURE(s);
                REQUIRE(outQ[s].size() == (inQ[s].size() > 1 ? inQ[s].size() : 2));
                for (size_t k = 0; k < outQ[s].size(); ++k) {
                    const auto& expected = inQ[s][std::min(k, inQ[s].size() - 1)];
                    if (version >= M2_VERSION_BC) {
                        CHECK(outQ[s][k].data == expected.data);
                    } else {
                        for (int c = 0; c < 4; ++c) {
                            CHECK(std::abs(int(outQ[s][k].data[c]) - int(expected.data[c])) <= 1);
                        }
                    }
                }
            }

            // Ribbons: the WotLK tail fields do not exist yet.
            REQUIRE(out.ribbonEmitters.size() == 1);
            CHECK(out.ribbonEmitters[0].ribbonColorIndex == -1);
            CHECK(out.ribbonEmitters[0].heightAbove.values.at(0).at(0) == 0.4f);

            // Particles: the fixed three-point color block.
            REQUIRE(out.particleEmitters.size() == 1);
            const auto& p = out.particleEmitters[0];
            CHECK(p.blendingType == ParticleBlending::Additive);
            REQUIRE(p.colorTrack.values.size() == 3);
            CHECK(p.colorTrack.values[0].x == 255.0f);
            CHECK(p.colorTrack.values[2].z == 255.0f);
            CHECK(p.colorTrack.timestamps[1].value == 16384);
            CHECK(p.alphaTrack.values[0].value == 32767);
            CHECK(p.scaleTrack.values[1].x == 2.0f);
            CHECK(p.lifespan.values.at(0).at(0) == 2.0f);
        }
    }
}

TEST_CASE("M2 WotLK MD20 with path-named siblings round-trips", "[m2][legacy]") {
    Model model = buildModel();
    // WotLK+ header arrays only; drop the ≤TBC leftovers so equality holds.
    model.playableAnimationLookup.clear();
    model.textureFlipbooks.clear();
    // Make the second sequence external: its keys must travel through a
    // path-named `.anim` sibling and come back.
    model.sequences[1].flags = SequenceFlag::None;

    for (u32 const version : {u32(M2_VERSION_WOTLK), u32(M2_VERSION_LEGION)}) {
        DYNAMIC_SECTION("version " << version) {
            TempDir dir("v" + std::to_string(version) + "_wotlk");

            WriteOptions opts;
            opts.format = Format::ClassicMD20;
            opts.m2Version = version;
            Writer writer(opts);
            utils::OsFileSystem outFs(dir.path.string());
            writer.write(outFs, "model.m2", model);
            REQUIRE_FALSE(writer.hasIssues());

            CHECK(fs::exists(dir.path / "model00.skin"));
            CHECK(fs::exists(dir.path / "model0004-00.anim"));

            utils::OsFileSystem inFs(dir.path.string());
            Parser parser;
            Model out = parser.parse(inFs, "model.m2");
            REQUIRE_FALSE(parser.hasIssues());

            checkCommon(model, out, version);

            // External skins: back as profiles, count preserved.
            REQUIRE(out.skinProfiles.size() == 1);
            CHECK(out.skinProfiles[0].submeshes.at(0).sortRadius == 7.0f);

            // Modern particle representation kept verbatim.
            REQUIRE(out.particleEmitters.size() == 1);
            CHECK(out.particleEmitters[0].colorTrack.timestamps[1].value == 16384);
        }
    }
}
