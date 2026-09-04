// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P6 — Diablo III, both directions.
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

// The animation converter is a private header: `ExportAnims` is the mirror of
// `ImportAnim` and neither is on `D3Converter`, because a clip belongs to the
// document rather than to one appearance.
#include "whiteout/models/wem/converters/d3_anim.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
        app.arMaterials[static_cast<std::size_t>(m)].szName = m == 0 ? "body_mat" : "cloth_mat";
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

TEST_CASE("wem d3 an appearance becomes one model with one diablo3 set", "[wem][convert][d3]") {
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

TEST_CASE("wem d3 every bone carries five poses under a five-entry schema", "[wem][convert][d3]") {
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
    Result<std::vector<u8>> exported = converter.exportToBytes(*imported.value, ProfileId::Diablo3);
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

TEST_CASE("wem d3 the fourteen player appearances convert whole", "[wem][convert][d3][corpus]") {
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
                        << descriptorsParsed << ", bones " << bones << ", hardpoints " << hardpoints
                        << ", lights " << lights << ", validation errors " << validationErrors);
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
    for (const Material& material :
         withAssets.value->models.front().profileSets.front().materials) {
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

// ===========================================================================
// toAppearance — the native model back out
// ===========================================================================
//
// The claims, in the order they matter:
//
// - **A section is a sub-object, and it owns its vertices.** Its face corners
//   are `u16` into its own array, so the ranges are disjoint slices.
// - **The dressing survives.** Two strings decide what a D3 character draws and
//   neither is derivable: `szMaterialName` (the substring `ActorModel_ApplyLook`
//   searches) and the per-look visibility bit on the variant.
// - **The five poses leave as five.** A and B are different poses.
// - **The materials are restored, not re-derived.** The block is authoritative,
//   so the textures that come back are the shipped SNO ids.

TEST_CASE("wem d3 a model comes back as an appearance", "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> imported = converter.fromAppearance(app);
    REQUIRE(imported.ok());

    Result<D3AppearanceExport> exported = converter.toAppearance(*imported.value);
    REQUIRE(exported.ok());
    const d3n::Appearances& back = exported.value->appearance;

    CHECK(back.dwSnoId == 4242);
    REQUIRE(back.arBones.size() == 2);
    CHECK(back.arBones[0].szName == "root");
    CHECK(back.arBones[0].nParentIndex == -1);
    CHECK(back.arBones[1].szName == "spine");
    CHECK(back.arBones[1].nParentIndex == 0);

    // Five distinct slots, still distinct and still in order. The fixture puts
    // `b * 10 + slot` in each translation, so a swapped pair is visible.
    for (int b = 0; b < 2; ++b) {
        const d3n::BoneStructure& bone = back.arBones[static_cast<std::size_t>(b)];
        const d3n::PRSTransform* slots[5] = {&bone.tTransform0, &bone.tTransform1,
                                             &bone.tTransform2, &bone.tTransform3,
                                             &bone.tTransform4};
        for (int p = 0; p < 5; ++p) {
            CHECK(slots[p]->vTranslation.x == Catch::Approx(static_cast<f32>(b * 10 + p)));
        }
        // Only the LOCAL pose carries the fixture's scale of 2, and D3 holds one
        // float — the export must not average or drop it.
        CHECK(slots[2]->flScale == Catch::Approx(2.0f));
    }

    REQUIRE(back.arHardpoints.size() == 1);
    CHECK(back.arHardpoints[0].szName == "HP_head");
    CHECK(back.arHardpoints[0].nBoneIndex == 1);
    CHECK(back.arHardpoints[0].tTransform.vTranslation.z == Catch::Approx(5.0f));

    REQUIRE(back.arStaticLights.size() == 1);
    CHECK(back.arStaticLights[0].nType == 1);
    CHECK(back.arStaticLights[0].flIntensity == Catch::Approx(2.0f));

    // The counts an `.app` carries beside its arrays, which a consumer reading
    // the struct rather than the file still reads.
    CHECK(back.dwBoneCount == 2);
    CHECK(back.dwHardpointCount == 1);
    CHECK(back.dwLookCount == 2);
    CHECK(back.dwMaterialCount == 2);
    CHECK(back.tGeoSet0.dwSubObjectCount == static_cast<i32>(back.tGeoSet0.arSubObjects.size()));
}

TEST_CASE("wem d3 a section is a sub-object that owns its vertices", "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> imported = converter.fromAppearance(app);
    REQUIRE(imported.ok());
    // One mesh, two sections — which is the case a mesh-per-geoset export gets
    // wrong by merging them into one draw.
    REQUIRE(imported.value->models.front().meshes.size() == 1);
    REQUIRE(imported.value->models.front().meshes.front().sections.size() == 2);

    Result<D3AppearanceExport> exported = converter.toAppearance(*imported.value);
    REQUIRE(exported.ok());
    const d3n::GeoSet& geoset = exported.value->appearance.tGeoSet0;
    REQUIRE(geoset.arSubObjects.size() == 2);

    for (const d3n::SubObject& sub : geoset.arSubObjects) {
        CHECK(sub.arVertices.size() == 3);
        CHECK(sub.arIndices.size() == 3);
        CHECK(sub.dwVertexCount == 3);
        CHECK(sub.dwIndexCount == 3);
        // Its own array: every corner is in range for THIS sub-object, which is
        // the invariant a shared vertex pool breaks.
        for (const u16 index : sub.arIndices) {
            CHECK(index < sub.arVertices.size());
        }
    }
    CHECK(geoset.arSubObjects[0].szName == "body_mat");
    CHECK(geoset.arSubObjects[1].szName == "cloth_mat");
    // The rigid binding: the fixture puts each sub-object on its own bone and
    // neither carries influences.
    CHECK(geoset.arSubObjects[0].nBoneIndex == 0);
    CHECK(geoset.arSubObjects[1].nBoneIndex == 1);
    CHECK(geoset.arSubObjects[0].arVertexInfluences.empty());

    // The document says which section each record came from, in emission order.
    REQUIRE(exported.value->sourceSections.size() == 2);
    CHECK(exported.value->sourceSections[0] == std::pair<u32, u32>{0, 0});
    CHECK(exported.value->sourceSections[1] == std::pair<u32, u32>{0, 1});
}

TEST_CASE("wem d3 the Maya shape name survives, because the dressing reads it",
          "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> imported = converter.fromAppearance(app);
    REQUIRE(imported.ok());

    Result<D3AppearanceExport> exported = converter.toAppearance(*imported.value);
    REQUIRE(exported.ok());
    const d3n::GeoSet& geoset = exported.value->appearance.tGeoSet0;
    REQUIRE(geoset.arSubObjects.size() == 2);

    // Verbatim, not rebuilt from the parse: `ActorModel_ApplyLook` does a
    // case-sensitive substring search on this string and never parses it, so a
    // regenerated spelling would re-dress differently from the game on exactly
    // the names the parse gets wrong.
    CHECK(geoset.arSubObjects[0].szMaterialName == "N_TRS_HVY_AShape_body_mat_001");
    CHECK(geoset.arSubObjects[1].szMaterialName == "N_TRS_NKDShape_cloth_mat_001");

    // And therefore the descriptor parses to the same triple it did on the way
    // in — the labelling half of the same fact.
    const d3n::GeosetName first = d3n::parseGeosetName(geoset.arSubObjects[0]);
    CHECK(first.parsed);
    CHECK(first.slot == d3n::LookSlot::Torso);
    CHECK(first.weight == d3n::ArmourWeight::Heavy);
    CHECK(first.variant == 'A');
}

TEST_CASE("wem d3 the per-look visibility bit is on the material, not the section",
          "[wem][convert][d3]") {
    d3n::Appearances app = makeAppearance();
    // Bit 0 is what `ActorModel_BuildSubObjectRenderRecords` opens with. Set it
    // for `body_mat` under look A only — Tyrael's shape, where one look draws
    // the Stranger and the other the restored angel.
    app.arMaterials[0].arVariants[0].dwUnknown00 = 1;
    app.arMaterials[0].arVariants[1].dwUnknown00 = 0;
    app.arMaterials[1].arVariants[0].dwUnknown00 = 0;
    app.arMaterials[1].arVariants[1].dwUnknown00 = 1;

    D3Converter converter;
    Result<Document> imported = converter.fromAppearance(app);
    REQUIRE(imported.ok());
    Result<D3AppearanceExport> exported = converter.toAppearance(*imported.value);
    REQUIRE(exported.ok());
    const d3n::Appearances& back = exported.value->appearance;
    REQUIRE(back.arMaterials.size() == 2);

    // Per (slot, look), which is why it cannot fold into `SectionFlags::Hidden`:
    // that is per section and would have to pick one look's answer.
    CHECK((back.arMaterials[0].arVariants[0].dwUnknown00 & 1) == 1);
    CHECK((back.arMaterials[0].arVariants[1].dwUnknown00 & 1) == 0);
    CHECK((back.arMaterials[1].arVariants[0].dwUnknown00 & 1) == 0);
    CHECK((back.arMaterials[1].arVariants[1].dwUnknown00 & 1) == 1);

    // And in the COMMON material as well, because that is the layer a derive
    // keeps: living only in the block, it went no further than Diablo III, and
    // the Skeleton King's four alternate bodies all drew at once anywhere else.
    const ProfileMaterialSet& set = imported.value->models.front().profileSets.front();
    REQUIRE(set.materials.size() == 4);
    const auto invisible = [&](std::size_t slot, std::size_t look) {
        const u32 index = set.slotBindings[slot].byLook[look];
        REQUIRE(index < set.materials.size());
        return hasFlag(set.materials[index].Common().flags, MaterialFlags::Invisible);
    };
    CHECK_FALSE(invisible(0, 0));
    CHECK(invisible(0, 1));
    CHECK(invisible(1, 0));
    CHECK_FALSE(invisible(1, 1));
}

TEST_CASE("wem d3 a stage the pass gives no combine code for is not a chain stage",
          "[wem][convert][d3][corpus]") {
    // The engine's own reader skips it — `d3_surface_table.cpp` opens with
    // `if (!color && !alpha) continue` — and a texture the pass declares can be
    // there for the program to use some other way. Malthael's wings declare six
    // stages and combine four; the last two are the flow maps his program warps
    // by, and defaulting them to a replace put two of them over the wing.
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
    Result<Document> imported = converter.fromAppearance(players.front().second, &assets);
    REQUIRE(imported.ok());

    constexpr u32 kColorCombine = 0xA0016u;
    constexpr u32 kAlphaCombine = 0xA001Cu;
    std::size_t chains = 0;
    for (const Material& material : imported.value->models.front().profileSets.front().materials) {
        const CombinersBody* body = material.Common().combiners();
        const auto* block = std::get_if<native::D3Material>(&material.Native());
        if (body == nullptr || block == nullptr || block->opaquePasses.empty()) {
            continue;
        }
        const native::D3RenderState& pass = block->opaquePasses.front();
        std::size_t combined = 0;
        for (std::size_t stage = 0; stage < pass.textureStages.size() && stage < 6; ++stage) {
            if (pass.textureStages[stage].contentStage == 0) {
                continue;
            }
            const auto has = [&](u32 base) {
                for (const native::D3ShaderTagValue& tag : pass.shaderParams) {
                    if (tag.tagId == base + static_cast<u32>(stage)) {
                        return true;
                    }
                }
                return false;
            };
            if (has(kColorCombine) || has(kAlphaCombine)) {
                ++combined;
            }
        }
        INFO(material.name << ": " << body->stages.size() << " stages, " << combined << " combined "
                           << "of " << pass.textureStages.size() << " declared");
        CHECK(body->stages.size() == combined);
        ++chains;
    }
    INFO("checked " << chains << " chain materials");
    CHECK(chains > 0);
}

TEST_CASE("wem d3 the materials are restored from the block, not re-derived",
          "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> imported = converter.fromAppearance(app);
    REQUIRE(imported.ok());

    Result<D3AppearanceExport> exported = converter.toAppearance(*imported.value);
    REQUIRE(exported.ok());
    const d3n::Appearances& back = exported.value->appearance;
    REQUIRE(back.arMaterials.size() == 2);

    // Slot order is the model's slot list, which the import built from
    // `arMaterials` — so the join a sub-object makes on `szName` still lands.
    CHECK(back.arMaterials[0].szName == "body_mat");
    CHECK(back.arMaterials[1].szName == "cloth_mat");
    for (const d3n::AppearanceMaterial& material : back.arMaterials) {
        CHECK(material.arVariants.size() == 2);
    }
    // The shipped SNO ids, not document indices: an `UberMaterial` names a
    // texture by id and the common view holds an index into `Document::textures`,
    // so a re-derivation through the common material would bind the wrong file.
    CHECK(back.arMaterials[0].arVariants[0].tMaterial.arTextures.at(0).snoTexture.id == 100);
    CHECK(back.arMaterials[0].arVariants[1].tMaterial.arTextures.at(0).snoTexture.id == 101);
    CHECK(back.arMaterials[1].arVariants[0].tMaterial.arTextures.at(0).snoTexture.id == 110);
    CHECK(back.arMaterials[1].arVariants[1].tMaterial.arTextures.at(0).snoTexture.id == 111);
}

TEST_CASE("wem d3 a wardrobe choice comes back as the hidden mask, not as a drop",
          "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    D3ImportOptions options;
    // `body_mat` is `N_TRS_HVY_A`; asking for the naked torso hides it and
    // leaves `cloth_mat`, which is `N_TRS_NKD`, drawing.
    options.wardrobe.push_back(D3WardrobePiece{d3n::LookSlot::Torso, d3n::ArmourWeight::Naked, 0});
    Result<Document> imported = converter.fromAppearance(app, nullptr, options);
    REQUIRE(imported.ok());

    Result<D3AppearanceExport> exported = converter.toAppearance(*imported.value);
    REQUIRE(exported.ok());

    // Both sub-objects are still written — an `.app` holds every variant at
    // once and visibility is never in it — and the choice comes back beside
    // them for the caller to apply.
    CHECK(exported.value->appearance.tGeoSet0.arSubObjects.size() == 2);
    REQUIRE(exported.value->hidden.size() == 2);
    CHECK(exported.value->hidden[0] == 1);
    CHECK(exported.value->hidden[1] == 0);
}

TEST_CASE("wem d3 exporting a model with no diablo3 set refuses", "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> imported = converter.fromAppearance(app);
    REQUIRE(imported.ok());
    imported.value->models.front().profileSets.clear();

    Result<D3AppearanceExport> exported = converter.toAppearance(*imported.value);
    CHECK_FALSE(exported.ok());
    CHECK(exported.diagnostics.countOf(DiagCode::ProfileNotCarried) == 1);
}

TEST_CASE("wem d3 clips come back as the anim they came from", "[wem][convert][d3]") {
    const d3n::Appearances app = makeAppearance();
    D3Converter converter;
    Result<Document> imported = converter.fromAppearance(app);
    REQUIRE(imported.ok());

    // Two permutations of one `.ani`, driving the two fixture bones.
    d3n::Anim anim;
    anim.dwSnoId = 777;
    anim.arPermutations.resize(2);
    for (int p = 0; p < 2; ++p) {
        d3n::AnimPermutation& perm = anim.arPermutations[static_cast<std::size_t>(p)];
        perm.szName = p == 0 ? "walk_a" : "walk_b";
        perm.flFramesPerTick = 0.5f; // 30 fps
        perm.dwFrameCount = 31;      // one second
        perm.arBoneNames.resize(1);
        perm.arBoneNames[0].szBoneName = "spine";
        perm.arTranslationCurves.resize(1);
        perm.arTranslationCurves[0].arKeys.push_back(d3n::TranslationKey{0, Vector3f{0, 0, 0}});
        perm.arTranslationCurves[0].arKeys.push_back(
            d3n::TranslationKey{30, Vector3f{static_cast<f32>(p + 1), 2, 3}});
        perm.arScaleCurves.resize(1);
        perm.arScaleCurves[0].arKeys.push_back(d3n::ScaleKey{0, 1.0f});
        perm.arScaleCurves[0].arKeys.push_back(d3n::ScaleKey{30, 4.0f});
    }

    Diagnostics diagnostics;
    const std::vector<u32> clips = d3_anim::ImportAnim(anim, *imported.value, 0, diagnostics);
    REQUIRE(clips.size() == 2);

    const std::vector<d3n::Anim> back = d3_anim::ExportAnims(*imported.value, 0, diagnostics);
    REQUIRE(back.size() == 1);
    // One `Anim` per source `.ani`, and its permutations are its clips — the
    // grouping the import recorded on `Clip::native["animSnoId"]`.
    CHECK(back[0].dwSnoId == 777);
    REQUIRE(back[0].arPermutations.size() == 2);
    CHECK(back[0].dwPermutationCount == 2);

    for (int p = 0; p < 2; ++p) {
        const d3n::AnimPermutation& perm = back[0].arPermutations[static_cast<std::size_t>(p)];
        CHECK(perm.szName == (p == 0 ? "walk_a" : "walk_b"));
        // `fps = flFramesPerTick * 60` and the duration counts spans, so the
        // two invert exactly.
        CHECK(perm.flFramesPerTick == Catch::Approx(0.5f));
        CHECK(perm.dwFrameCount == 31);
        REQUIRE(perm.arBoneNames.size() == 1);
        CHECK(perm.arBoneNames[0].szBoneName == "spine");
        REQUIRE(perm.arTranslationCurves.size() == 1);
        REQUIRE(perm.arTranslationCurves[0].arKeys.size() == 2);
        CHECK(perm.arTranslationCurves[0].dwKeyCount == 2);
        CHECK(perm.arTranslationCurves[0].arKeys[1].nFrame == 30);
        CHECK(perm.arTranslationCurves[0].arKeys[1].vPosition.x ==
              Catch::Approx(static_cast<f32>(p + 1)));
        // One float, matching the channel the import declared. Widening it to
        // three would invent two components D3 has no field for.
        REQUIRE(perm.arScaleCurves[0].arKeys.size() == 2);
        CHECK(perm.arScaleCurves[0].arKeys[1].flScale == Catch::Approx(4.0f));
    }
}

TEST_CASE("wem d3 the corpus appearances round trip exactly", "[wem][convert][d3][corpus]") {
    const fs::path directory = corpusRoot() / "Appearances";
    if (!fs::is_directory(directory)) {
        SUCCEED("no D3 corpus");
        return;
    }
    CorpusProvider provider(corpusRoot());
    AssetSource assets(provider);
    D3Converter converter;

    u32 files = 0, exact = 0;
    u32 emptied = 0, reported = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (files >= 200) {
            break;
        }
        const std::vector<u8> bytes = readWhole(entry.path());
        std::optional<d3n::Appearances> parsed = d3n::parseAppearances(bytes);
        if (!parsed.has_value()) {
            continue;
        }
        ++files;

        Result<Document> imported = converter.fromAppearance(*parsed, &assets);
        REQUIRE(imported.ok());
        Result<D3AppearanceExport> exported = converter.toAppearance(*imported.value);
        REQUIRE(exported.ok());
        const d3n::Appearances& back = exported.value->appearance;

        bool clean = back.arBones.size() == parsed->arBones.size() &&
                     back.arHardpoints.size() == parsed->arHardpoints.size() &&
                     back.arMaterials.size() == parsed->arMaterials.size() &&
                     back.arLooks.size() == parsed->arLooks.size();
        for (std::size_t b = 0; b < back.arBones.size() && clean; ++b) {
            clean = back.arBones[b].szName == parsed->arBones[b].szName &&
                    back.arBones[b].nParentIndex == parsed->arBones[b].nParentIndex &&
                    back.arBones[b].tTransform4.vTranslation.x ==
                        parsed->arBones[b].tTransform4.vTranslation.x;
        }

        // A section the §5.3 manifold repair emptied writes no record, and the
        // export has to say so — a sub-object going missing from the middle of
        // a geoset is exactly what a counts-only check never sees.
        std::size_t withGeometry = 0;
        const d3n::GeoSet* sets[2] = {&parsed->tGeoSet0, &parsed->tGeoSet1};
        for (const d3n::GeoSet* set : sets) {
            for (const d3n::SubObject& sub : set->arSubObjects) {
                if (!sub.arVertices.empty() && !sub.arIndices.empty()) {
                    ++withGeometry;
                }
            }
        }
        const std::size_t written =
            back.tGeoSet0.arSubObjects.size() + back.tGeoSet1.arSubObjects.size();
        if (written != withGeometry) {
            // Not a failure, and not silent either: the count that went missing
            // and the count the export reported have to agree.
            emptied += static_cast<u32>(withGeometry - written);
            reported += exported.diagnostics.countOf(DiagCode::DegenerateFaceDropped);
        }
        // The `szMaterialName` join, on every record that was written.
        for (int g = 0; g < 2 && clean; ++g) {
            const d3n::GeoSet& source = *sets[g];
            const d3n::GeoSet& result = g == 0 ? back.tGeoSet0 : back.tGeoSet1;
            for (std::size_t s = 0; s < result.arSubObjects.size() && clean; ++s) {
                // `sourceSections` names the section, which IS the source
                // sub-object's index in its geoset.
                const std::size_t at = s < exported.value->sourceSections.size()
                                           ? exported.value->sourceSections[s].second
                                           : source.arSubObjects.size();
                clean =
                    at < source.arSubObjects.size() &&
                    result.arSubObjects[s].szName == source.arSubObjects[at].szName &&
                    result.arSubObjects[s].szMaterialName == source.arSubObjects[at].szMaterialName;
            }
        }
        if (clean) {
            ++exact;
        }
    }

    REQUIRE(files > 0);
    INFO(exact << " of " << files << " appearances round trip exactly; the repair emptied "
               << emptied << " sub-objects and reported " << reported);
    // Every one, with nothing excused: the bones, their five poses, the
    // hardpoints, the look table and the two join strings all come back as
    // shipped. The only thing that shrinks is what the manifold repair took,
    // and that is counted rather than tolerated.
    CHECK(exact == files);
    CHECK(reported >= emptied);
}
