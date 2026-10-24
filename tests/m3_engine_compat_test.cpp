// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Engine support divergence: detection on synthetic models, and the
// Heroes -> StarCraft II conversion over the HotS corpus. See
// M3_FILE_FORMAT_SPECIFICATION.md §18.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <whiteout/models/m3/engine_compat.h>
#include <whiteout/models/m3/parser.h>
#include <whiteout/models/m3/writer.h>

#include "test_helpers.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::m3;

namespace {

Model modelAt(i32 version) {
    Model model;
    model.setVersion(version);
    return model;
}

MaterialMap mapTo(MaterialType type, u32 index) {
    MaterialMap map{};
    map.materialType = type;
    map.materialIndex = index;
    return map;
}

std::vector<fs::path> hotsCorpus() {
    std::vector<fs::path> files;
    if (const char* env = std::getenv("M3_CORPUS_DIR"); env && fs::is_directory(env)) {
        for (const auto& entry : fs::recursive_directory_iterator(env))
            if (entry.is_regular_file() && entry.path().extension() == ".m3")
                files.push_back(entry.path());
    } else if (std::string base = test::findCorpusBase("Corpus"); !base.empty()) {
        fs::path dir = fs::path(base) / "HotSM3";
        if (fs::is_directory(dir))
            for (const auto& entry : fs::recursive_directory_iterator(dir))
                if (entry.is_regular_file() && entry.path().extension() == ".m3")
                    files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// The library's own `makeTag`, which lives in a private header.
constexpr u32 tagOf(const char (&str)[5]) {
    return static_cast<u32>(static_cast<unsigned char>(str[0])) << 24 |
           static_cast<u32>(static_cast<unsigned char>(str[1])) << 16 |
           static_cast<u32>(static_cast<unsigned char>(str[2])) << 8 |
           static_cast<u32>(static_cast<unsigned char>(str[3]));
}

/// Every version the index table of @p bytes states, keyed by chunk tag.
///
/// Read out of the file rather than off the model on purpose: the version a
/// chunk is written at is the one thing a `Model` cannot be asked about, and
/// it is the only field either client checks before parsing anything.
std::map<std::string, std::vector<u32>> chunkVersions(const std::vector<u8>& bytes) {
    u32 offset = 0;
    u32 count = 0;
    std::memcpy(&offset, bytes.data() + 4, sizeof(offset));
    std::memcpy(&count, bytes.data() + 8, sizeof(count));

    std::map<std::string, std::vector<u32>> out;
    for (u32 i = 0; i < count; ++i) {
        const u8* entry = bytes.data() + offset + i * 16;
        const std::string tag(reinterpret_cast<const char*>(entry), 4);
        u32 version = 0;
        std::memcpy(&version, entry + 12, sizeof(version));
        out[std::string(tag.rbegin(), tag.rend())].push_back(version);
    }
    return out;
}

/// Every `CHAR` chunk's payload, exactly as the index table delimits it.
std::vector<std::string> charChunks(const std::vector<u8>& bytes) {
    u32 offset = 0;
    u32 count = 0;
    std::memcpy(&offset, bytes.data() + 4, sizeof(offset));
    std::memcpy(&count, bytes.data() + 8, sizeof(count));

    std::vector<std::string> out;
    for (u32 i = 0; i < count; ++i) {
        const u8* entry = bytes.data() + offset + i * 16;
        if (std::string(reinterpret_cast<const char*>(entry), 4) != "RAHC") {
            continue;
        }
        u32 at = 0;
        u32 length = 0;
        std::memcpy(&at, entry + 4, sizeof(at));
        std::memcpy(&length, entry + 8, sizeof(length));
        out.emplace_back(reinterpret_cast<const char*>(bytes.data() + at), length);
    }
    return out;
}

} // namespace

TEST_CASE("M3 a name ends inside the chunk that holds it", "[m3][compat][string]") {
    // Both clients take a name by calling strlen on the chunk pointer, so the
    // terminator is payload and the reference counts it. Every one of the
    // 246,686 CHAR chunks in the shipped corpus ends in a NUL but one. This
    // export wrote none at all, and the alignment fill after a string is 0xAA,
    // so every name in every converted model ran off its chunk.
    SECTION("a name we invented is terminated") {
        Model model;
        model.name = "footman";
        Sequence sequence{};
        sequence.name = "Stand";
        model.sequences.push_back(sequence);
        Bone bone{};
        bone.name = "Bone_Root";
        model.bones.push_back(bone);

        const std::vector<std::string> chunks = charChunks(Writer().write(model));
        REQUIRE(chunks.size() == 3);
        for (const std::string& chunk : chunks) {
            INFO(chunk);
            REQUIRE_FALSE(chunk.empty());
            CHECK(chunk.back() == '\0');
            // Terminated once, and the reference counts exactly that.
            CHECK(std::strlen(chunk.c_str()) + 1 == chunk.size());
        }
    }

    SECTION("a name we read keeps the bytes it came with") {
        const std::string base = test::findCorpusBase("Corpus");
        if (base.empty()) {
            SKIP("no corpus");
        }
        const fs::path path = fs::path(base) / "Sc2M3" / "Marine.m3";
        if (!fs::is_regular_file(path)) {
            SKIP("no Marine.m3");
        }

        Parser parser;
        const Model model = parser.parse(path.string());
        const std::vector<std::string> chunks = charChunks(Writer().write(model));
        REQUIRE(chunks.size() > 100);
        for (const std::string& chunk : chunks) {
            INFO(chunk);
            CHECK(chunk.back() == '\0');
            // The reader keeps the terminator, so the writer must not add a
            // second one: a round trip states the same length it read.
            CHECK(std::strlen(chunk.c_str()) + 1 == chunk.size());
        }
    }
}

TEST_CASE("M3 engine support detection", "[m3][compat]") {
    SECTION("a v29 model with no divergent chunk loads on both") {
        const EngineSupport support = checkEngineSupport(modelAt(29));
        CHECK(support.starcraft2);
        CHECK(support.heroesOfTheStorm);
        CHECK(support.heroesOnlyReasons.empty());
        CHECK(support.starcraft2OnlyReasons.empty());
        CHECK_FALSE(isHeroesOnly(modelAt(29)));
    }

    SECTION("MODL v30 alone is disqualifying, even with no MADD records") {
        Model model = modelAt(30);
        const EngineSupport support = checkEngineSupport(model);
        CHECK_FALSE(support.starcraft2);
        CHECK(support.heroesOfTheStorm);
        REQUIRE(support.heroesOnlyReasons.size() == 1);
        CHECK(support.heroesOnlyReasons[0].find("MODL is v30") != std::string::npos);
        CHECK(isHeroesOnly(model));
    }

    SECTION("MADD records and DataDriven maps are reported separately") {
        Model model = modelAt(30);
        model.dataDrivenMaterials.emplace_back();
        model.materialMaps.push_back(mapTo(MaterialType::DataDriven, 0));
        const EngineSupport support = checkEngineSupport(model);
        CHECK_FALSE(support.starcraft2);
        CHECK(support.heroesOnlyReasons.size() == 3);
    }

    SECTION("REF_ v3 is Heroes-only") {
        Model model = modelAt(29);
        ReflectionMaterial reflection{};
        reflection.setVersion(3);
        model.reflectionMaterials.push_back(reflection);
        const EngineSupport support = checkEngineSupport(model);
        CHECK_FALSE(support.starcraft2);
        CHECK(support.heroesOfTheStorm);
    }

    SECTION("MAT_ v20 is StarCraft II only — divergence runs both ways") {
        Model model = modelAt(29);
        StandardMaterial material{};
        material.setVersion(20);
        model.standardMaterials.push_back(material);
        const EngineSupport support = checkEngineSupport(model);
        CHECK(support.starcraft2);
        CHECK_FALSE(support.heroesOfTheStorm);
        REQUIRE(support.starcraft2OnlyReasons.size() == 1);
        CHECK(support.starcraft2OnlyReasons[0].find("MAT_") != std::string::npos);
        CHECK_FALSE(isHeroesOnly(model));
    }
}

TEST_CASE("M3 the version a chunk is written at turns on the client", "[m3][compat][version]") {
    // Both descriptor tables were read out of the shipped clients
    // (`sub_102C658F0` / `sub_102771E00`) and every value here is also the
    // highest that tag reaches anywhere in the 56,139-file corpus.
    SECTION("the three tags the two clients disagree about") {
        CHECK(CurrentChunkVersion(tagOf("MODL"), 30, Engine::StarCraft2) == 29u);
        CHECK(CurrentChunkVersion(tagOf("MODL"), 30, Engine::HeroesOfTheStorm) == 30u);
        CHECK(CurrentChunkVersion(tagOf("MAT_"), 20, Engine::StarCraft2) == 20u);
        CHECK(CurrentChunkVersion(tagOf("MAT_"), 20, Engine::HeroesOfTheStorm) == 19u);
        CHECK(CurrentChunkVersion(tagOf("REF_"), 3, Engine::StarCraft2) == 2u);
        CHECK(CurrentChunkVersion(tagOf("REF_"), 3, Engine::HeroesOfTheStorm) == 3u);
    }

    SECTION("Both is the lower of the two, which is what a file loadable on either states") {
        CHECK(CurrentChunkVersion(tagOf("MODL"), 30, Engine::Both) == 29u);
        CHECK(CurrentChunkVersion(tagOf("MAT_"), 20, Engine::Both) == 19u);
        CHECK(CurrentChunkVersion(tagOf("REF_"), 3, Engine::Both) == 2u);
    }

    SECTION("every other tag is what this library can spell, whoever is asking") {
        for (const Engine engine : {Engine::StarCraft2, Engine::HeroesOfTheStorm, Engine::Both}) {
            CHECK(CurrentChunkVersion(tagOf("LAYR"), 26, engine) == 26u);
            CHECK(CurrentChunkVersion(tagOf("PAR_"), 24, engine) == 24u);
            CHECK(CurrentChunkVersion(tagOf("STC_"), 4, engine) == 4u);
        }
    }

    SECTION("a library that cannot spell the newest layout is not pushed to it") {
        CHECK(CurrentChunkVersion(tagOf("MAT_"), 15, Engine::StarCraft2) == 15u);
    }
}

TEST_CASE("M3 an invented chunk is written at a version a client reads", "[m3][compat][version]") {
    // Nothing below states a version — this is what a conversion hands the
    // writer. Every one of these used to go out as 0xFFFFFFFF, and both
    // clients abort the whole load on a version above their descriptor
    // table's, so no model this library converted was loadable in either game.
    Model model;
    model.sequences.emplace_back();
    model.bones.emplace_back();
    StandardMaterial material{};
    material.diffuseLayer.emplace();
    model.standardMaterials.push_back(material);
    model.materialMaps.push_back(mapTo(MaterialType::Standard, 0));

    const std::vector<u8> bytes = Writer().write(model);
    const std::map<std::string, std::vector<u32>> versions = chunkVersions(bytes);
    REQUIRE_FALSE(versions.empty());

    for (const auto& [tag, stated] : versions) {
        INFO(tag);
        CHECK(std::find(stated.begin(), stated.end(), 0xFFFFFFFFu) == stated.end());
    }

    // 11 in all 56,139 shipped models of both games, MD33 included.
    REQUIRE(versions.count("MD34") == 1);
    CHECK(versions.at("MD34").front() == ROOT_CHUNK_VERSION);
    // Nobody named an engine, so both have to be able to read it.
    CHECK(versions.at("MODL").front() == 29u);
    CHECK(versions.at("MAT_").front() == 19u);
    // The 107 descriptors that do not diverge are unaffected either way.
    CHECK(versions.at("LAYR").front() == 26u);
    CHECK(versions.at("SEQS").front() == 2u);
    CHECK(versions.at("BONE").front() == 1u);

    CHECK(checkEngineSupport(model).starcraft2);
}

TEST_CASE("M3 a parsed chunk keeps the version it was read with", "[m3][compat][version]") {
    // The other half of the rule: a version we did not invent is not ours to
    // raise. Marine.m3 is MODL v23 with MAT_ v15 and LAYR v22 — five versions
    // behind what StarCraft II reads today, and it goes back out that way.
    const std::string base = test::findCorpusBase("Corpus");
    if (base.empty()) {
        SKIP("no corpus");
    }
    const fs::path path = fs::path(base) / "Sc2M3" / "Marine.m3";
    if (!fs::is_regular_file(path)) {
        SKIP("no Marine.m3");
    }

    Parser parser;
    const Model model = parser.parse(path.string());
    const std::map<std::string, std::vector<u32>> versions =
        chunkVersions(Writer().write(model));

    CHECK(versions.at("MODL").front() == 23u);
    CHECK(versions.at("MAT_").front() == 15u);
    CHECK(versions.at("LAYR").front() == 22u);
    CHECK(versions.at("REGN").front() == 3u);
    // …except the root entry, which the writer states rather than reads.
    CHECK(versions.at("MD34").front() == ROOT_CHUNK_VERSION);
}

TEST_CASE("M3 a beta region keeps its face range", "[m3][compat][version]") {
    // REGN v2 (28 bytes, every MD33 beta model) has the face range as u32s
    // after its u16 vertex range; reading 20 left every region empty. Its
    // faces index the whole vertex buffer, and a model holds them per region.
    Model model;
    MeshDivision& division = model.divisions.emplace_back();
    for (u16 i = 0; i < 30; ++i)
        division.faces.push_back(i % 6);
    for (u32 r = 0; r < 2; ++r) {
        Region& region = division.regions.emplace_back();
        region.setVersion(2);
        region.firstVertex = 100 * r;
        region.vertexCount = 100;
        region.firstIndex = 12 * r;
        region.indexCount = 12 + 6 * r;
        region.firstBoneLookup = 14 * r;
        region.boneLookupCount = 14;
        region.rootBone = 3;
    }

    const std::vector<u8> bytes = Writer().write(model);
    Parser parser;
    const Model back = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(back.divisions.size() == 1);
    REQUIRE(back.divisions[0].regions.size() == 2);
    const Region& second = back.divisions[0].regions[1];
    CHECK(second.getVersion() == 2);
    CHECK(second.firstVertex == 100);
    CHECK(second.firstIndex == 12);
    CHECK(second.indexCount == 18);
    CHECK(second.firstBoneLookup == 14);
    CHECK(second.rootBone == 3);
    CHECK(back.divisions[0].faces == division.faces);

    // On disk the second region's corners are 100 on.
    u32 offset = 0;
    u32 count = 0;
    std::memcpy(&offset, bytes.data() + 4, sizeof(offset));
    std::memcpy(&count, bytes.data() + 8, sizeof(count));
    bool found = false;
    for (u32 i = 0; i < count; ++i) {
        const u8* entry = bytes.data() + offset + i * 16;
        u32 at = 0;
        u32 entries = 0;
        std::memcpy(&at, entry + 4, sizeof(at));
        std::memcpy(&entries, entry + 8, sizeof(entries));
        if (std::string(reinterpret_cast<const char*>(entry), 4) != "_61U" || entries != 30)
            continue;
        u16 corner = 0;
        std::memcpy(&corner, bytes.data() + at + 12 * sizeof(u16), sizeof(corner));
        CHECK(corner == 100);
        found = true;
    }
    CHECK(found);

    const std::string base = test::findCorpusBase("Corpus");
    const fs::path path = fs::path(base) / "Sc2BetaM3" / "InfestedMarinePortrait.m3";
    if (base.empty() || !fs::is_regular_file(path)) {
        SKIP("no Sc2BetaM3/InfestedMarinePortrait.m3");
    }
    Parser shipped;
    const Model portrait = shipped.parse(path.string());
    REQUIRE(portrait.divisions.size() == 1);
    const MeshDivision& shippedDivision = portrait.divisions[0];
    REQUIRE(shippedDivision.regions.size() == 7);
    u32 indices = 0;
    for (const Region& region : shippedDivision.regions) {
        indices += region.indexCount;
        const auto first = shippedDivision.faces.begin() + region.firstIndex;
        CHECK(*std::max_element(first, first + region.indexCount) + 1u == region.vertexCount);
    }
    CHECK(indices == shippedDivision.faces.size());
}

TEST_CASE("M3 toStarCraft2 on synthetic models", "[m3][compat]") {
    SECTION("an already-compatible model is returned unchanged") {
        const EngineConversion result = toStarCraft2(modelAt(29));
        REQUIRE(result.converted);
        CHECK(result.lossy.empty());
        CHECK(result.model.getVersion() == 29);
    }

    SECTION("MODL v30 with no materials drops to v29") {
        const EngineConversion result = toStarCraft2(modelAt(30));
        REQUIRE(result.converted);
        CHECK(result.model.getVersion() == SC2_MAX_MODEL_VERSION);
        CHECK(checkEngineSupport(result.model).starcraft2);
    }

    SECTION("REF_ v3 drops to v2 and loses its data-driven link") {
        Model model = modelAt(30);
        ReflectionMaterial reflection{};
        reflection.name = "reflect";
        reflection.unknown2 = 7;
        reflection.setVersion(3);
        model.reflectionMaterials.push_back(reflection);

        const EngineConversion result = toStarCraft2(model);
        REQUIRE(result.converted);
        REQUIRE(result.model.reflectionMaterials.size() == 1);
        CHECK(result.model.reflectionMaterials[0].getVersion() ==
              SC2_MAX_REFLECTION_MATERIAL_VERSION);
        CHECK(result.model.reflectionMaterials[0].unknown2 == 0xFFFFFFFFu);
        CHECK(result.lossy.size() == 1);
        CHECK(checkEngineSupport(result.model).starcraft2);
    }

    SECTION("a dangling DataDriven material map is refused, not mis-repointed") {
        Model model = modelAt(30);
        model.materialMaps.push_back(mapTo(MaterialType::DataDriven, 4));
        const EngineConversion result = toStarCraft2(model);
        CHECK_FALSE(result.converted);
        CHECK(result.blocker.find("out of range") == std::string::npos);
        CHECK_FALSE(result.blocker.empty());
    }
}

TEST_CASE("M3 Heroes -> StarCraft II conversion over the corpus", "[m3][corpus][compat]") {
    const std::vector<fs::path> files = hotsCorpus();
    if (files.empty())
        SKIP("HotS M3 corpus not found");

    size_t parsed = 0, heroesOnly = 0, converted = 0, blocked = 0, rewritten = 0;
    size_t strictConverted = 0, approximateOnly = 0;
    std::map<std::string, size_t> blockerKinds;
    std::vector<std::string> failures;
    auto fail = [&failures](std::string what) {
        if (failures.size() < 25)
            failures.push_back(std::move(what));
    };

    Writer writer;
    for (const fs::path& path : files) {
        Model model;
        try {
            Parser parser;
            model = parser.parse(path.string());
        } catch (const std::exception&) {
            continue;
        }
        parsed++;

        const EngineSupport before = checkEngineSupport(model);
        if (before.starcraft2)
            continue;
        heroesOnly++;

        const EngineConversion result = toStarCraft2(model);

        StarCraft2ConversionOptions strictOptions;
        strictOptions.approximate = false;
        const EngineConversion strict = toStarCraft2(model, strictOptions);
        if (strict.converted) {
            strictConverted++;
            if (!result.converted)
                fail(path.filename().string() + ": strict converted but best-effort refused");
        } else if (result.converted) {
            approximateOnly++;
        }

        if (!result.converted) {
            blocked++;
            if (result.blocker.empty())
                fail(path.filename().string() + ": refused with an empty blocker");
            blockerKinds[result.blocker.substr(0, result.blocker.find(':'))]++;
            continue;
        }
        converted++;

        const EngineSupport after = checkEngineSupport(result.model);
        if (!after.starcraft2) {
            fail(path.filename().string() + ": converted but still Heroes-only (" +
                 (after.heroesOnlyReasons.empty() ? "no reason" : after.heroesOnlyReasons[0]) +
                 ')');
            continue;
        }
        if (!result.model.dataDrivenMaterials.empty())
            fail(path.filename().string() + ": MADD records survived the conversion");
        for (const MaterialMap& map : result.model.materialMaps) {
            if (map.materialType == MaterialType::DataDriven) {
                fail(path.filename().string() + ": a material map still names DataDriven");
                break;
            }
        }
        // Every map must still land inside the array it names.
        for (const MaterialMap& map : result.model.materialMaps) {
            if (map.materialType == MaterialType::Standard &&
                map.materialIndex >= result.model.standardMaterials.size()) {
                fail(path.filename().string() + ": material map index " +
                     std::to_string(map.materialIndex) + " is out of range after conversion");
                break;
            }
        }

        // The converted model must survive a write -> re-parse round trip.
        try {
            const std::vector<u8> bytes = writer.write(result.model);
            Parser reparse;
            const Model back = reparse.parse(std::span<const u8>(bytes));
            if (back.getVersion() != SC2_MAX_MODEL_VERSION)
                fail(path.filename().string() + ": round trip produced MODL v" +
                     std::to_string(back.getVersion()));
            else if (!checkEngineSupport(back).starcraft2)
                fail(path.filename().string() + ": round trip reintroduced a Heroes-only chunk");
            else
                rewritten++;
        } catch (const std::exception& e) {
            fail(path.filename().string() + ": round trip threw: " + e.what());
        }
    }

    std::cout << "=== M3 Heroes -> StarCraft II ===\n"
              << "Parsed: " << parsed << ", Heroes-only: " << heroesOnly
              << ", converted: " << converted << " ("
              << (heroesOnly ? converted * 100 / heroesOnly : 0) << "%), blocked: " << blocked
              << ", round-tripped: " << rewritten << "\n";
    std::cout << "Strict-mode converted: " << strictConverted
              << ", approximate-only: " << approximateOnly << "\n";
    for (const auto& [reason, count] : blockerKinds)
        std::cout << "  blocked: " << reason << " x" << count << "\n";

    for (const std::string& f : failures)
        UNSCOPED_INFO(f);
    CHECK(failures.empty());
    REQUIRE(heroesOnly > 0);
    CHECK(converted > 0);
    CHECK(rewritten == converted);
    CHECK(strictConverted <= converted);
    // The approximate fallback has to actually reach models strict mode cannot.
    CHECK(approximateOnly > 0);
}

// ---- CheckEditorRules (EDIT_MODE_SC2_PROFILE_PLAN.md S0.7) ------------------

namespace {

/// A model that keeps every rule: one bone, one division with its bounds, one
/// sequence whose container lists its state, its events under a shipped id,
/// and one material with every slot.
Model ruleAbidingModel() {
    Model model = modelAt(29);
    Bone bone;
    bone.name = std::string("Root", 5);
    bone.parentIndex = 0xFFFF;
    model.bones.push_back(bone);

    MeshDivision division;
    MeshSection bounds;
    bounds.bounds.animId = 0x001F9BD2u;
    division.msec.push_back(bounds);
    Region region;
    region.boneLookupCount = 1;
    division.regions.push_back(region);
    model.divisions.push_back(division);
    model.boneLookup = {0};

    Sequence sequence;
    sequence.name = "Stand";
    model.sequences.push_back(sequence);
    AnimationGroup group;
    group.name = "Stand";
    group.subtrackIndices = {0};
    model.animationGroups.push_back(group);
    SubTrackContainer stc;
    stc.name = "Stand_full";
    AnimBlock<Event> events{};
    events.timestamps = {1000};
    events.keys.push_back(Event{});
    events.keys[0].name = "Evt_SeqEnd";
    stc.sdev.push_back(events);
    stc.animIds = {0x063F6D59u};
    stc.animRefs = {0};
    model.subTrackCollections.push_back(stc);
    AnimationState state;
    state.animIds = stc.animIds;
    model.animationStates.push_back(state);

    StandardMaterial material;
    material.name = "Body";
    for (std::optional<TextureLayer>* layer :
         {&material.diffuseLayer, &material.decalLayer, &material.specularLayer, &material.glossLayer,
          &material.emissiveLayer1, &material.emissiveLayer2, &material.environmentLayer,
          &material.environmentMaskLayer, &material.alphaLayer1, &material.alphaLayer2, &material.normalLayer,
          &material.heightLayer, &material.lightMapLayer, &material.ambientOcclusionLayer,
          &material.normalBlend1MaskLayer, &material.normalBlend2MaskLayer, &material.normalBlend1Layer,
          &material.normalBlend2Layer}) {
        *layer = TextureLayer{};
    }
    model.standardMaterials.push_back(material);
    model.materialMaps.push_back(mapTo(MaterialType::Standard, 0));
    return model;
}

bool Breaks(const Model& model, const std::string& rule, Engine engine = Engine::StarCraft2) {
    for (const EditorRuleFailure& failure : CheckEditorRules(model, engine)) {
        if (failure.rule == rule) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("M3 the editor rules hold on a model that keeps them", "[m3][compat][rules]") {
    for (const EditorRuleFailure& failure : CheckEditorRules(ruleAbidingModel(), Engine::StarCraft2)) {
        FAIL_CHECK(failure.rule + ": " + failure.message);
    }
}

TEST_CASE("M3 each editor rule names what breaks it", "[m3][compat][rules]") {
    SECTION("version: MADD in a StarCraft II file") {
        Model model = ruleAbidingModel();
        model.dataDrivenMaterials.push_back(DataDrivenMaterial{});
        model.materialMaps.push_back(mapTo(MaterialType::DataDriven, 0));
        CHECK(Breaks(model, "version"));
        CHECK_FALSE(Breaks(model, "version", Engine::HeroesOfTheStorm));
    }
    SECTION("string: a NUL inside a name") {
        Model model = ruleAbidingModel();
        model.bones[0].name = std::string("Root\0_Vis", 9);
        CHECK(Breaks(model, "string"));
    }
    SECTION("msec: a division without bounds, or bounds under another id") {
        Model model = ruleAbidingModel();
        model.divisions[0].msec[0].bounds.animId = 7;
        CHECK(Breaks(model, "msec"));
        model.divisions[0].msec.clear();
        CHECK(Breaks(model, "msec"));
    }
    SECTION("stc-sorted: a lookup out of order") {
        Model model = ruleAbidingModel();
        model.subTrackCollections[0].animIds = {9, 0x063F6D59u, 3};
        model.subTrackCollections[0].animRefs = {(2u << 16), 0, (2u << 16) | 1};
        model.animationStates[0].animIds = model.subTrackCollections[0].animIds;
        CHECK(Breaks(model, "stc-sorted"));
    }
    SECTION("sts: a state listing other ids, or a second index that does not copy it") {
        Model model = ruleAbidingModel();
        model.animationStates[0].animIds = {1};
        CHECK(Breaks(model, "sts"));
        model = ruleAbidingModel();
        model.subTrackCollections[0].animationStateIndexCopy = 3;
        CHECK(Breaks(model, "sts"));
    }
    SECTION("events: a stream under an id neither game ships") {
        Model model = ruleAbidingModel();
        model.subTrackCollections[0].animIds = {0};
        model.animationStates[0].animIds = {0};
        CHECK(Breaks(model, "events"));
    }
    SECTION("region-bones: 64 bones in one region") {
        Model model = ruleAbidingModel();
        model.divisions[0].regions[0].boneLookupCount = 64;
        CHECK(Breaks(model, "region-bones"));
    }
    SECTION("normal-blend: the flag without its factors") {
        Model model = ruleAbidingModel();
        model.standardMaterials[0].flags = model.standardMaterials[0].flags | MaterialFlag::NormalBlend;
        CHECK(Breaks(model, "normal-blend"));
        model.standardMaterials[0].normalBlendFactors.resize(4);
        CHECK_FALSE(Breaks(model, "normal-blend"));
    }
    SECTION("addons: an attachment point without its addon") {
        Model model = ruleAbidingModel();
        model.attachmentPoints.push_back(AttachmentPoint{});
        CHECK(Breaks(model, "addons"));
        model.attachmentPointAddons = {0xFFFF};
        CHECK_FALSE(Breaks(model, "addons"));
    }
    SECTION("shared-id: two bones driven by one stream") {
        Model model = ruleAbidingModel();
        Bone second = model.bones[0];
        second.parentIndex = 0;
        model.bones.push_back(second);
        for (Bone& bone : model.bones) {
            bone.position.animId = 0x1234ABCDu;
        }
        AnimBlock<Vector3f> keys{};
        keys.timestamps = {0};
        keys.keys = {Vector3f{0, 0, 0}};
        SubTrackContainer& stc = model.subTrackCollections[0];
        stc.sd3v.push_back(keys);
        stc.animIds = {0x063F6D59u, 0x1234ABCDu};
        stc.animRefs = {0, (2u << 16)};
        model.animationStates[0].animIds = stc.animIds;
        CHECK(Breaks(model, "shared-id"));
        model.bones[1].position.animId = 0;
        CHECK_FALSE(Breaks(model, "shared-id"));
    }
}
