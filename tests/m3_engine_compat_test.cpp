// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Engine support divergence: detection on synthetic models, and the
// Heroes -> StarCraft II conversion over the HotS corpus. See
// M3_FILE_FORMAT_SPECIFICATION.md §18.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdlib>
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

} // namespace

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
