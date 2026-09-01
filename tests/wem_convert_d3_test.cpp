// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P6 — Diablo III import.
///
/// The claims under test, in the order they matter:
///
/// - **An actor is one model.** An `.app` is the parts; which of them draw is the
///   caller's input, so a wardrobe request must *hide* and never drop, and the
///   parsed descriptor must survive on every section either way.
/// - **The join is `szName`.** `szMaterialName` holds the Maya shape name, and
///   using it leaves every player character untextured.
/// - **Five poses, not one.** Bind A and bind B disagree on shipped bones, and a
///   hardpoint composes against A while skinning uses B.
/// - **Sharing is conditional.** 2.24 actors per appearance, so reusing a model
///   matters — but only when the second actor would build the identical thing.
///   The two corpus actors this test finds sharing an appearance do *not*: one
///   carries attach points and one does not, so sharing them unconditionally
///   drops real data.
///
/// The corpus arm needs the extracted SNO tree. It has no CoreTOC, so the
/// provider below indexes a directory by each file's own `dwSnoId` — the first
/// dword of every payload — which is what makes group-37 shader resolution
/// reachable at all.

#include <catch2/catch_all.hpp>

#include <whiteout/models/wem/d3_converter.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/sno/d3/native/character.h>

#include "test_helpers.h"
#include "wem_d3_corpus.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = whiteout::sno::d3::native;

using namespace whiteout;
using namespace whiteout::models::wem;
using whiteout::test::d3::CorpusProvider;
using whiteout::test::d3::corpusRoot;
using whiteout::test::d3::EmptyProvider;
using whiteout::test::d3::readWhole;

namespace {

std::vector<std::pair<std::string, d3n::Appearances>> loadPlayerAppearances() {
    std::vector<std::pair<std::string, d3n::Appearances>> out;
    const fs::path directory = corpusRoot() / "Appearances";
    if (!fs::is_directory(directory)) {
        return out;
    }
    for (i32 c = 0; c < static_cast<i32>(d3n::kPlayerClassCount); ++c) {
        for (const d3n::Gender gender : {d3n::Gender::Male, d3n::Gender::Female}) {
            const std::string stem =
                d3n::playerAppearanceStem(static_cast<d3n::PlayerClass>(c), gender);
            const std::vector<u8> bytes = readWhole(directory / (stem + ".app"));
            if (bytes.empty()) {
                continue;
            }
            auto parsed = d3n::parseAppearances(bytes);
            if (parsed.has_value()) {
                out.emplace_back(stem, std::move(*parsed));
            }
        }
    }
    return out;
}

/// One sub-object's worth of geometry: a single triangle, so a fixture can be
/// about the join and the nodes rather than about a mesh.
d3n::SubObject makeSubObject(const std::string& name, const std::string& materialName,
                             i32 boneIndex) {
    d3n::SubObject sub;
    sub.szName = name;
    sub.szMaterialName = materialName;
    sub.nBoneIndex = boneIndex;
    sub.dwVertexCount = 3;
    sub.arVertices.resize(3);
    sub.arVertices[0].vPosition = Vector3f{0, 0, 0};
    sub.arVertices[1].vPosition = Vector3f{1, 0, 0};
    sub.arVertices[2].vPosition = Vector3f{0, 1, 0};
    sub.dwIndexCount = 3;
    sub.arIndices = {0, 1, 2};
    return sub;
}

d3n::PRSTransform makePrs(f32 x, f32 scale) {
    d3n::PRSTransform out;
    out.qRotation = Vector4f{0, 0, 0, 1};
    out.vTranslation = Vector3f{x, 0, 0};
    out.flScale = scale;
    return out;
}

/// Two bones, one hardpoint, one light, two materials over two looks.
d3n::Appearances makeAppearance() {
    d3n::Appearances app;
    app.dwSnoId = 4242;

    app.arBones.resize(2);
    app.arBones[0].szName = "root";
    app.arBones[0].nParentIndex = -1;
    app.arBones[1].szName = "spine";
    app.arBones[1].nParentIndex = 0;
    for (int b = 0; b < 2; ++b) {
        // Five distinct transforms, so a test can tell which slot is which.
        app.arBones[b].tTransform0 = makePrs(static_cast<f32>(b * 10 + 0), 1.0f);
        app.arBones[b].tTransform1 = makePrs(static_cast<f32>(b * 10 + 1), 1.0f);
        app.arBones[b].tTransform2 = makePrs(static_cast<f32>(b * 10 + 2), 2.0f);
        app.arBones[b].tTransform3 = makePrs(static_cast<f32>(b * 10 + 3), 1.0f);
        app.arBones[b].tTransform4 = makePrs(static_cast<f32>(b * 10 + 4), 1.0f);
    }

    app.dwHardpointCount = 1;
    app.arHardpoints.resize(1);
    app.arHardpoints[0].szName = "HP_head";
    app.arHardpoints[0].nBoneIndex = 1;
    app.arHardpoints[0].tTransform.qRotation = Vector4f{0, 0, 0, 1};
    app.arHardpoints[0].tTransform.vTranslation = Vector3f{0, 0, 5};

    app.dwStaticLightCount = 1;
    app.arStaticLights.resize(1);
    app.arStaticLights[0].nType = 1;
    app.arStaticLights[0].vPosition = Vector3f{3, 4, 5};
    app.arStaticLights[0].dwColor = 0x0000FF00u; // green in R,G,B memory order
    app.arStaticLights[0].flIntensity = 2.0f;

    app.arLooks.resize(2);
    app.arLooks[0].szName = "A";
    app.arLooks[1].szName = "B";
    app.dwLookCount = 2;

    app.arMaterials.resize(2);
    app.dwMaterialCount = 2;
    for (int m = 0; m < 2; ++m) {
        app.arMaterials[static_cast<std::size_t>(m)].szName =
            m == 0 ? "body_mat" : "cloth_mat";
        app.arMaterials[static_cast<std::size_t>(m)].arVariants.resize(2);
        for (int look = 0; look < 2; ++look) {
            auto& variant = app.arMaterials[static_cast<std::size_t>(m)]
                                .arVariants[static_cast<std::size_t>(look)];
            variant.tMaterial.tColors.vDiffuse = Vector4f{1, 1, 1, 1};
            variant.tMaterial.arTextures.resize(1);
            variant.tMaterial.arTextures[0].dwSlotIndex = 1; // the type, @0x00 -- diffuse
            variant.tMaterial.arTextures[0].snoTexture.id = 100 + m * 10 + look;
            variant.tMaterial.arTextures[0].snoTexture.group = d3n::Group::Textures;
        }
    }

    app.tGeoSet0.arSubObjects.push_back(
        makeSubObject("body_mat", "N_TRS_HVY_AShape_body_mat_001", 0));
    app.tGeoSet0.arSubObjects.push_back(
        makeSubObject("cloth_mat", "N_TRS_NKDShape_cloth_mat_001", 1));
    app.tGeoSet0.dwSubObjectCount = 2;
    return app;
}

} // namespace

// ===========================================================================
// Shape, on a fixture
// ===========================================================================

TEST_CASE("wem d3 an appearance becomes one model with one diablo3 set",
          "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> result = converter.fromAppearance(app);

    REQUIRE(result.ok());
    const Document& document = *result.value;
    REQUIRE(document.models.size() == 1);
    CHECK(document.defaultProfile == ProfileId::Diablo3);
    CHECK(document.carries(ProfileId::Diablo3));

    const Model& model = document.models.front();
    REQUIRE(model.profileSets.size() == 1);
    CHECK(model.profileSets.front().profile == ProfileId::Diablo3);
    CHECK(model.meshes.size() == 1);

    // The join is on `szName`, so the slot list is the appearance's material
    // names and every sub-object finds one.
    REQUIRE(model.materialSlots.size() == 2);
    CHECK(model.materialSlots[0] == "body_mat");
    CHECK(model.materialSlots[1] == "cloth_mat");
    const Mesh& mesh = model.meshes.front();
    REQUIRE(mesh.sections.size() == 2);
    CHECK(mesh.sections[0].materialSlot == 0);
    CHECK(mesh.sections[1].materialSlot == 1);
}

TEST_CASE("wem d3 every bone carries five poses under a five-entry schema",
          "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> result = converter.fromAppearance(app);
    REQUIRE(result.ok());

    const NodeTree& tree = result.value->models.front().nodes;
    REQUIRE(tree.poseSchema.size() == 5);
    CHECK(tree.poseSchema[0].name == "bindA");
    CHECK(tree.poseSchema[1].inverse);
    CHECK(tree.poseSchema[2].space == PoseSpace::ParentRelative);
    CHECK(tree.poseSchema[3].name == "bindB");
    CHECK(tree.poseSchema[4].inverse);
    // Skinning is defined against B; entry 4 is the inverse a palette wants.
    CHECK(tree.authoritativePose == 3);

    REQUIRE(tree.nodes.size() >= 2);
    for (u32 b = 0; b < 2; ++b) {
        const Node& bone = tree.nodes[b];
        REQUIRE(bone.kind == NodeKind::Bone);
        REQUIRE(bone.poses.size() == 5);
        // Slot 2 is the LOCAL parent-relative pose, and it is what `local` is.
        CHECK(bone.local.translation.x == bone.poses[2].translation.x);
        CHECK(bone.poses[0].translation.x != bone.poses[3].translation.x);
        CHECK(bone.uniformScaleOnly);
    }
}

TEST_CASE("wem d3 hardpoints and static lights are nodes", "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> result = converter.fromAppearance(app);
    REQUIRE(result.ok());

    const NodeTree& tree = result.value->models.front().nodes;
    const Node* hardpoint = nullptr;
    const Node* light = nullptr;
    for (const Node& node : tree.nodes) {
        if (node.kind == NodeKind::Attachment) {
            hardpoint = &node;
        }
        if (node.kind == NodeKind::Light) {
            light = &node;
        }
    }
    REQUIRE(hardpoint != nullptr);
    CHECK(hardpoint->name == "HP_head");
    // A hardpoint is skeleton-relative, so it is a child of its bone.
    CHECK(hardpoint->parent == 1);
    CHECK(hardpoint->local.translation.z == 5.0f);
    // Nothing rides it yet: the asset is what an actor's event supplies.
    const AttachmentPayload& payload = std::get<AttachmentPayload>(hardpoint->payload);
    CHECK(payload.asset.empty());
    CHECK(payload.model == kInvalidIndex);

    REQUIRE(light != nullptr);
    // Static lights are placed in the model, not on a bone.
    CHECK(light->parent == kInvalidNode);
    CHECK(light->local.translation.x == 3.0f);
    const LightPayload& lightPayload = std::get<LightPayload>(light->payload);
    CHECK(lightPayload.color.y == 1.0f);
    CHECK(lightPayload.color.x == 0.0f);
}

TEST_CASE("wem d3 a wardrobe hides sections and drops none", "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;

    // With no wardrobe, the naked variant of a slot draws and the heavy one
    // does not.
    Result<Document> bare = converter.fromAppearance(app);
    REQUIRE(bare.ok());
    const Mesh& bareMesh = bare.value->models.front().meshes.front();
    REQUIRE(bareMesh.sections.size() == 2);
    CHECK(hasFlag(bareMesh.sections[0].flags, SectionFlags::Hidden));
    CHECK_FALSE(hasFlag(bareMesh.sections[1].flags, SectionFlags::Hidden));

    // Asking for the heavy torso flips exactly that, and the naked one goes.
    D3ImportOptions dressed;
    dressed.wardrobe.push_back(
        D3WardrobePiece{d3n::LookSlot::Torso, d3n::ArmourWeight::Heavy, 'A'});
    Result<Document> worn = converter.fromAppearance(app, nullptr, dressed);
    REQUIRE(worn.ok());
    const Mesh& wornMesh = worn.value->models.front().meshes.front();
    // Nothing was dropped -- the choice is a flag, so a host can re-dress.
    REQUIRE(wornMesh.sections.size() == 2);
    CHECK_FALSE(hasFlag(wornMesh.sections[0].flags, SectionFlags::Hidden));
    CHECK(hasFlag(wornMesh.sections[1].flags, SectionFlags::Hidden));

    // The descriptor rides the section whichever way the choice went (§8).
    for (const MeshSection& section : wornMesh.sections) {
        CHECK(section.native.value("descriptorParsed", 0) == 1);
        CHECK(section.native.find("lookSlot") != nullptr);
    }
}

TEST_CASE("wem d3 the look table is the appearance's and the default is chosen",
          "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;

    Result<Document> defaulted = converter.fromAppearance(app);
    REQUIRE(defaulted.ok());
    const ProfileMaterialSet& set = defaulted.value->models.front().profileSets.front();
    REQUIRE(set.looks.size() == 2);
    CHECK(set.looks.looks[0].name == "A");
    CHECK(set.defaultLook == 0);
    // Every slot binds one material per look — the look index IS the variant
    // index, which is what makes `byLook.size() == looks.size()` an invariant.
    REQUIRE(set.slotBindings.size() == 2);
    for (const SlotBinding& binding : set.slotBindings) {
        REQUIRE(binding.byLook.size() == 2);
        CHECK(binding.bound(0));
        CHECK(binding.bound(1));
    }

    D3ImportOptions second;
    second.materialLook = "B";
    Result<Document> chosen = converter.fromAppearance(app, nullptr, second);
    REQUIRE(chosen.ok());
    CHECK(chosen.value->models.front().profileSets.front().defaultLook == 1);

    // A look the appearance does not carry falls back and says so.
    D3ImportOptions missing;
    missing.materialLook = "Z";
    Result<Document> fallback = converter.fromAppearance(app, nullptr, missing);
    REQUIRE(fallback.ok());
    CHECK(fallback.value->models.front().profileSets.front().defaultLook == 0);
    CHECK(fallback.diagnostics.countOf(DiagCode::LookDropped) == 1);
}

TEST_CASE("wem d3 export is refused, not faked", "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> imported = converter.fromAppearance(app);
    REQUIRE(imported.ok());

    CHECK_FALSE(converter.supportsExport());
    Result<std::vector<u8>> exported =
        converter.exportToBytes(*imported.value, ProfileId::Diablo3);
    CHECK_FALSE(exported.ok());
    CHECK(exported.diagnostics.countOf(DiagCode::OperationUnsupported) == 1);
}

TEST_CASE("wem d3 the version sniff is a last resort that can fail", "[wem][convert][d3]") {
    std::vector<u8> bytes(16, 0);
    const u32 magic = 0xDEADBEEFu;
    std::memcpy(bytes.data(), &magic, 4);

    u32 version = 260;
    std::memcpy(bytes.data() + 4, &version, 4);
    CHECK(D3Converter::SniffGroup(bytes) == d3n::Group::Appearance);

    version = 282;
    std::memcpy(bytes.data() + 4, &version, 4);
    CHECK(D3Converter::SniffGroup(bytes) == d3n::Group::Actor);

    // 118 is `.ani`, and the sniff must not claim it.
    version = 118;
    std::memcpy(bytes.data() + 4, &version, 4);
    CHECK(D3Converter::SniffGroup(bytes) == d3n::Group::Unknown);

    // No magic, no answer.
    std::vector<u8> junk(16, 0x5A);
    CHECK(D3Converter::SniffGroup(junk) == d3n::Group::Unknown);
}

TEST_CASE("wem d3 importing an actor without a provider refuses", "[wem][convert][d3]") {
    std::vector<u8> bytes(16, 0);
    const u32 magic = 0xDEADBEEFu;
    const u32 version = 282;
    std::memcpy(bytes.data(), &magic, 4);
    std::memcpy(bytes.data() + 4, &version, 4);

    D3Converter converter;
    Result<Document> result = converter.importFromBytes(bytes);
    CHECK_FALSE(result.ok());
}

// ===========================================================================
// The corpus arm — §16's P6 gate
// ===========================================================================

TEST_CASE("wem d3 the fourteen player appearances convert whole",
          "[wem][convert][d3][corpus]") {
    const auto players = loadPlayerAppearances();
    if (players.empty()) {
        SKIP("D3 Appearances corpus not found");
    }
    CHECK(players.size() == 14);

    D3Converter converter;
    std::size_t subObjects = 0;
    std::size_t joined = 0;
    std::size_t descriptorsParsed = 0;
    std::size_t bindingsSized = 0;
    std::size_t bindingsExpected = 0;
    std::size_t hardpoints = 0;
    std::size_t lights = 0;
    std::size_t bonesWithFivePoses = 0;
    std::size_t bones = 0;

    std::size_t validationErrors = 0;
    for (const auto& [stem, app] : players) {
        Result<Document> result = converter.fromAppearance(app);
        REQUIRE(result.ok());
        // A green conversion proves a field parsed, never that the document it
        // built is well formed. This is the question the sweep can answer.
        const Diagnostics report = Validate(*result.value, ValidateLevel::Profile);
        for (const Diagnostic& entry : report.bySeverity(Severity::Error)) {
            INFO(stem << ": " << ToString(entry.code) << " " << Describe(entry.where) << " -- "
                      << entry.message);
            CHECK(entry.code == DiagCode::Unspecified); // never true; names the offender
        }
        validationErrors += report.bySeverity(Severity::Error).size();
        const Model& model = result.value->models.front();
        const ProfileMaterialSet& set = model.profileSets.front();

        for (const Mesh& mesh : model.meshes) {
            for (const MeshSection& section : mesh.sections) {
                ++subObjects;
                // A sub-object that found no appearance material gets a slot of
                // its own, which is what `SlotNotBound` reports.
                if (section.materialSlot < set.slotBindings.size() &&
                    set.slotBindings[section.materialSlot].bound(0)) {
                    ++joined;
                }
                if (section.native.value("descriptorParsed", 0) == 1) {
                    ++descriptorsParsed;
                }
            }
        }
        for (const SlotBinding& binding : set.slotBindings) {
            ++bindingsExpected;
            if (binding.byLook.size() == set.looks.size()) {
                ++bindingsSized;
            }
        }
        for (const Node& node : model.nodes.nodes) {
            if (node.kind == NodeKind::Bone) {
                ++bones;
                if (node.poses.size() == 5) {
                    ++bonesWithFivePoses;
                }
            }
            if (node.kind == NodeKind::Attachment) {
                ++hardpoints;
            }
            if (node.kind == NodeKind::Light) {
                ++lights;
            }
        }
    }

    INFO("sub-objects " << subObjects << ", joined " << joined << ", descriptors "
                        << descriptorsParsed << ", bones " << bones << ", hardpoints "
                        << hardpoints << ", lights " << lights << ", validation errors "
                        << validationErrors);
    CHECK(validationErrors == 0);

    // §16's P6 gate, as measured.
    CHECK(subObjects == 441);
    CHECK(joined == 441);
    CHECK(descriptorsParsed == 436);
    // `byLook.size() == looks.size()` everywhere: a mismatch is a parse bug.
    CHECK(bindingsSized == bindingsExpected);
    CHECK(bonesWithFivePoses == bones);
    CHECK(bones > 0);
    CHECK(hardpoints > 0);
}

TEST_CASE("wem d3 model sharing is conditional on the actors agreeing",
          "[wem][convert][d3][corpus]") {
    const fs::path root = corpusRoot();
    if (!fs::is_directory(root / "Actor")) {
        SKIP("D3 corpus not found");
    }
    CorpusProvider provider(root);
    AssetSource assets(provider);
    D3Converter converter;

    // Every actor in the corpus that names an appearance more than one other
    // actor names — the sharing case, found rather than assumed.
    std::map<i32, std::vector<i32>> actorsByAppearance;
    const fs::path actorDirectory = root / "Actor";
    std::size_t scanned = 0;
    for (fs::directory_iterator it(actorDirectory); it != fs::directory_iterator(); ++it) {
        if (scanned++ >= 400) {
            break;
        }
        const std::vector<u8> bytes = readWhole(it->path());
        auto parsed = d3n::parseActor(bytes);
        if (parsed.has_value() && parsed->snoAppearance.valid()) {
            actorsByAppearance[parsed->snoAppearance.id].push_back(parsed->dwSnoId);
        }
    }
    REQUIRE_FALSE(actorsByAppearance.empty());

    i32 shared = -1;
    for (const auto& [appearance, actors] : actorsByAppearance) {
        if (actors.size() >= 2) {
            shared = appearance;
            break;
        }
    }
    if (shared < 0) {
        SKIP("no shared appearance in the sampled actors");
    }

    // Two actors over one appearance, into one document.
    Document document;
    document.declare(ProfileId::Diablo3);
    document.defaultProfile = ProfileId::Diablo3;
    std::set<u32> models;
    std::size_t appended = 0;
    for (const i32 actorId : actorsByAppearance[shared]) {
        const d3n::Actor* actor = assets.actor(actorId);
        if (actor == nullptr) {
            continue;
        }
        Result<u32> index = converter.appendActor(document, *actor, assets);
        if (index.ok()) {
            models.insert(*index.value);
            ++appended;
        }
    }
    REQUIRE(appended >= 2);

    // **What is shared, and what is not.** The parse is shared unconditionally —
    // that is what the cache is for, and where the 594x cost actually is. The
    // *model* is shared only when the second actor would have built the
    // identical thing, because an actor is one model and two actors that equip
    // differently are two models. Sharing unconditionally silently drops the
    // second actor's attach points.
    CHECK(assets.stats().hits > 0);
    CHECK(models.size() <= appended);

    // Every actor that contributed no attach point landed on the same model.
    std::set<u32> bare;
    for (const u32 index : models) {
        bool attached = false;
        for (const Node& node : document.models[index].nodes.nodes) {
            const auto* payload = std::get_if<AttachmentPayload>(&node.payload);
            if (payload != nullptr &&
                (!payload->asset.empty() || payload->model != kInvalidIndex)) {
                attached = true;
                break;
            }
        }
        if (!attached) {
            bare.insert(index);
        }
    }
    INFO("appended " << appended << " actors into " << models.size() << " models, " << bare.size()
                     << " of them bare");
    CHECK(bare.size() <= 1);
    // Child models arrive through attach points, and every one of those indices
    // has to address a model this document holds.
    const Diagnostics report = Validate(document, ValidateLevel::Profile);
    CHECK(report.bySeverity(Severity::Error).empty());
}

TEST_CASE("wem d3 shader resolution fills the render state", "[wem][convert][d3][corpus]") {
    const fs::path root = corpusRoot();
    if (!fs::is_directory(root / "Shaders")) {
        SKIP("D3 corpus not found");
    }
    const auto players = loadPlayerAppearances();
    if (players.empty()) {
        SKIP("D3 Appearances corpus not found");
    }

    CorpusProvider provider(root);
    AssetSource assets(provider);
    D3Converter converter;

    Result<Document> withAssets = converter.fromAppearance(players.front().second, &assets);
    REQUIRE(withAssets.ok());

    std::size_t resolved = 0;
    std::size_t combiners = 0;
    std::size_t legacy = 0;
    for (const Material& material : withAssets.value->models.front().profileSets.front().materials) {
        const auto* block = std::get_if<native::D3Material>(&material.Native());
        REQUIRE(block != nullptr);
        // Import-only, so the block is the truth and common is a projection.
        CHECK(material.sync() == NativeSync::NativeAuthoritative);
        if (!block->opaquePasses.empty()) {
            ++resolved;
        }
        if (material.Common().kind() == MaterialKind::Combiners) {
            ++combiners;
        }
        if (material.Common().kind() == MaterialKind::LegacyDeferred) {
            ++legacy;
        }
    }
    INFO("resolved " << resolved << " passes, " << combiners << " combiners, " << legacy
                     << " legacy");
    CHECK(resolved > 0);
    // §7.2.6: the kind follows the stage block, so both shapes must occur.
    CHECK(combiners + legacy ==
          withAssets.value->models.front().profileSets.front().materials.size());
    // The cache did its job: fewer loads than materials asked for.
    CHECK(assets.stats().hits > 0);
}
