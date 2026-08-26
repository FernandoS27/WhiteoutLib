// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// MADD (DataDrivenMaterial) validation over the HotS corpus: every property
// blob must decode, every Tex* property must index a real entry in
// texturePaths, and a parse → write → re-parse cycle must reproduce the record
// byte for byte.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <whiteout/models/m3/parser.h>
#include <whiteout/models/m3/writer.h>

#include "test_helpers.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::m3;

namespace {

struct Stats {
    size_t files = 0;
    size_t records = 0;
    size_t blobs = 0;
    size_t groups = 0;
    size_t properties = 0;
    size_t namedGroups = 0;
    size_t namedProperties = 0;
    size_t roundTrips = 0;
    std::map<u8, size_t> shaderTypes;
    std::vector<std::string> failures;

    void fail(const fs::path& p, const std::string& what) {
        if (failures.size() < 40)
            failures.push_back(p.filename().string() + ": " + what);
    }
};

bool sameRecord(const DataDrivenMaterial& a, const DataDrivenMaterial& b) {
    return a.materialName == b.materialName && a.fragmentHashes == b.fragmentHashes &&
           a.extraHashes == b.extraHashes && a.propertyBlob == b.propertyBlob &&
           a.texturePaths == b.texturePaths && a.effectNameHash == b.effectNameHash &&
           a.effectNameHash2 == b.effectNameHash2 && a.effectNameHash3 == b.effectNameHash3 &&
           a.shaderType == b.shaderType && a.alphaFresnelFlags == b.alphaFresnelFlags &&
           a.unknown136 == b.unknown136 && a.unknown144 == b.unknown144;
}

void checkRecord(const fs::path& path, const DataDrivenMaterial& madd, Stats& stats) {
    stats.records++;
    stats.shaderTypes[static_cast<u8>(madd.shaderType)]++;

    if (static_cast<u8>(madd.shaderType) > 4)
        stats.fail(path, "shaderType out of range");
    if (madd.padding128 != 0)
        stats.fail(path, "padding128 non-zero");

    if (madd.propertyBlob.empty())
        return;
    stats.blobs++;

    const DataDrivenProperties decoded = madd.decodeProperties();
    if (decoded.groups.empty()) {
        stats.fail(path, "property blob failed to decode");
        return;
    }

    // The engine reads the same list twice: fragmentHashes duplicates the blob's
    // level-1 keys, in the same order.
    if (madd.fragmentHashes.size() == decoded.groups.size()) {
        for (size_t i = 0; i < decoded.groups.size(); ++i)
            if (madd.fragmentHashes[i] != decoded.groups[i].nameHash)
                stats.fail(path, "fragmentHashes disagrees with blob keys at " + std::to_string(i));
    }

    for (const auto& group : decoded.groups) {
        stats.groups++;
        if (!group.name.empty())
            stats.namedGroups++;
        for (const auto& property : group.properties) {
            stats.properties++;
            if (!property.name.empty())
                stats.namedProperties++;

            if (property.name.rfind("Tex", 0) == 0 && property.data.size() == 8) {
                u32 index = 0;
                std::memcpy(&index, property.data.data(), sizeof(index));
                if (index >= madd.texturePaths.size())
                    stats.fail(path, property.name + " indexes texturePaths[" +
                                         std::to_string(index) + "] of " +
                                         std::to_string(madd.texturePaths.size()));
            }
        }
    }
}

} // namespace

TEST_CASE("M3 data-driven material (MADD) decode and round-trip", "[m3][corpus][madd]") {
    std::vector<fs::path> files;
    std::string corpusBase = test::findCorpusBase("Corpus");
    if (const char* env = std::getenv("M3_CORPUS_DIR"); env && fs::is_directory(env)) {
        for (const auto& entry : fs::recursive_directory_iterator(env))
            if (entry.is_regular_file() && entry.path().extension() == ".m3")
                files.push_back(entry.path());
    } else if (!corpusBase.empty()) {
        fs::path dir = fs::path(corpusBase) / "HotSM3";
        if (fs::is_directory(dir))
            for (const auto& entry : fs::recursive_directory_iterator(dir))
                if (entry.is_regular_file() && entry.path().extension() == ".m3")
                    files.push_back(entry.path());
    }
    if (files.empty())
        SKIP("HotS M3 corpus not found");
    std::sort(files.begin(), files.end());

    size_t limit = files.size();
    if (const char* env = std::getenv("M3_CORPUS_LIMIT"); env && *env)
        limit = std::min<size_t>(limit, std::strtoull(env, nullptr, 10));

    Stats stats;
    for (size_t idx = 0; idx < limit; ++idx) {
        const auto& path = files[idx];
        Model model;
        try {
            Parser parser;
            model = parser.parse(path.string());
        } catch (const std::exception&) {
            continue;
        }
        if (model.dataDrivenMaterials.empty())
            continue;
        stats.files++;

        for (const auto& madd : model.dataDrivenMaterials)
            checkRecord(path, madd, stats);

        try {
            Writer writer;
            std::vector<u8> bytes = writer.write(model);
            Parser reparser;
            Model reparsed = reparser.parse(std::span<const u8>(bytes));
            if (reparsed.dataDrivenMaterials.size() != model.dataDrivenMaterials.size()) {
                stats.fail(path, "round-trip changed the MADD record count");
                continue;
            }
            for (size_t i = 0; i < model.dataDrivenMaterials.size(); ++i)
                if (!sameRecord(model.dataDrivenMaterials[i], reparsed.dataDrivenMaterials[i]))
                    stats.fail(path, "round-trip changed MADD record " + std::to_string(i));
            stats.roundTrips++;
        } catch (const std::exception& e) {
            stats.fail(path, std::string("round-trip threw: ") + e.what());
        }
    }

    std::cout << "=== MADD validation ===\n"
              << "Files with MADD: " << stats.files << ", records: " << stats.records
              << ", blobs: " << stats.blobs << "\n"
              << "Groups: " << stats.namedGroups << "/" << stats.groups << " named, properties: "
              << stats.namedProperties << "/" << stats.properties << " named\n"
              << "Round-trips: " << stats.roundTrips << std::endl;
    for (const auto& [type, count] : stats.shaderTypes)
        std::cout << "  shaderType " << int(type) << ": " << count << std::endl;
    for (const auto& f : stats.failures)
        std::cout << "FAIL " << f << std::endl;

    REQUIRE(stats.records > 0);
    REQUIRE(stats.blobs > 0);
    CHECK(stats.failures.empty());
    // The recovered name table resolves the overwhelming majority of real keys;
    // the residue is the shader-graph vocabulary that is absent from the client.
    CHECK(stats.namedProperties * 100 >= stats.properties * 95);
}

TEST_CASE("M3 data-driven material -> standard material", "[m3][corpus][madd]") {
    std::vector<fs::path> files;
    std::string corpusBase = test::findCorpusBase("Corpus");
    if (const char* env = std::getenv("M3_CORPUS_DIR"); env && fs::is_directory(env)) {
        for (const auto& entry : fs::recursive_directory_iterator(env))
            if (entry.is_regular_file() && entry.path().extension() == ".m3")
                files.push_back(entry.path());
    } else if (!corpusBase.empty()) {
        fs::path dir = fs::path(corpusBase) / "HotSM3";
        if (fs::is_directory(dir))
            for (const auto& entry : fs::recursive_directory_iterator(dir))
                if (entry.is_regular_file() && entry.path().extension() == ".m3")
                    files.push_back(entry.path());
    }
    if (files.empty())
        SKIP("HotS M3 corpus not found");
    std::sort(files.begin(), files.end());

    size_t total = 0, converted = 0, layers = 0, textured = 0;
    std::map<std::string, size_t> blockers;
    std::map<std::string, size_t> lossyReasons;
    std::vector<std::string> failures;

    for (const auto& path : files) {
        Model model;
        try {
            Parser parser;
            model = parser.parse(path.string());
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& madd : model.dataDrivenMaterials) {
            total++;
            const StandardMaterialConversion result = madd.toStandardMaterial();
            if (!result.converted) {
                REQUIRE_FALSE(result.blocker.empty());
                blockers[result.blocker.substr(0, result.blocker.find(':'))]++;
                continue;
            }
            converted++;
            for (const auto& reason : result.lossy)
                lossyReasons[reason.substr(0, reason.find('\''))]++;

            const StandardMaterial& mat = result.material;
            if (mat.name != madd.materialName && failures.size() < 20)
                failures.push_back(path.filename().string() + ": name not carried over");

            const std::optional<TextureLayer>* slots[] = {
                &mat.diffuseLayer,     &mat.decalLayer,        &mat.specularLayer,
                &mat.glossLayer,       &mat.emissiveLayer1,    &mat.emissiveLayer2,
                &mat.environmentLayer, &mat.environmentMaskLayer, &mat.alphaLayer1,
                &mat.alphaLayer2,      &mat.normalLayer,       &mat.heightLayer,
                &mat.lightMapLayer,    &mat.ambientOcclusionLayer};
            for (const auto* slot : slots) {
                if (!slot->has_value())
                    continue;
                layers++;
                const TextureLayer& layer = **slot;
                if (!layer.texturePath.empty()) {
                    textured++;
                    // Every texture a layer names must come from this record's table.
                    const bool known = std::find(madd.texturePaths.begin(),
                                                 madd.texturePaths.end(),
                                                 layer.texturePath) != madd.texturePaths.end();
                    if (!known && failures.size() < 20)
                        failures.push_back(path.filename().string() + ": layer texture '" +
                                           layer.texturePath + "' is not in texturePaths");
                } else if (!hasFlag(layer.flags, TextureLayerFlag::Color) &&
                           failures.size() < 20) {
                    failures.push_back(path.filename().string() +
                                       ": textureless layer not marked Color");
                }
            }
        }
    }

    std::cout << "=== MADD -> StandardMaterial ===\n"
              << "Records: " << total << ", converted: " << converted << " ("
              << (total ? converted * 100 / total : 0) << "%), layers: " << layers
              << " (" << textured << " textured)" << std::endl;
    for (const auto& [reason, count] : blockers)
        std::cout << "  blocked: " << reason << " x" << count << std::endl;
    for (const auto& [reason, count] : lossyReasons)
        std::cout << "  lossy: " << reason << " x" << count << std::endl;
    for (const auto& f : failures)
        std::cout << "FAIL " << f << std::endl;

    REQUIRE(total > 0);
    CHECK(failures.empty());
    // 86% of the corpus is fixed-function; the rest is shader-graph or another
    // material type, and must be refused rather than mis-converted.
    CHECK(converted * 100 >= total * 80);
    CHECK(converted < total);
}

TEST_CASE("M3 data-driven material -> approximated standard material", "[m3][corpus][madd]") {
    std::vector<fs::path> files;
    std::string corpusBase = test::findCorpusBase("Corpus");
    if (const char* env = std::getenv("M3_CORPUS_DIR"); env && fs::is_directory(env)) {
        for (const auto& entry : fs::recursive_directory_iterator(env))
            if (entry.is_regular_file() && entry.path().extension() == ".m3")
                files.push_back(entry.path());
    } else if (!corpusBase.empty()) {
        fs::path dir = fs::path(corpusBase) / "HotSM3";
        if (fs::is_directory(dir))
            for (const auto& entry : fs::recursive_directory_iterator(dir))
                if (entry.is_regular_file() && entry.path().extension() == ".m3")
                    files.push_back(entry.path());
    }
    if (files.empty())
        SKIP("HotS M3 corpus not found");
    std::sort(files.begin(), files.end());

    size_t total = 0, exactRecords = 0, graphRecords = 0, graphApproximated = 0;
    size_t layers = 0, delegationMismatch = 0;
    std::map<std::string, size_t> blockers;
    std::map<std::string, size_t> roles;
    std::vector<std::string> failures;
    auto fail = [&failures](const std::string& what) {
        if (failures.size() < 20)
            failures.push_back(what);
    };

    for (const auto& path : files) {
        Model model;
        try {
            Parser parser;
            model = parser.parse(path.string());
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& madd : model.dataDrivenMaterials) {
            total++;
            const StandardMaterialConversion exact = madd.toStandardMaterial();
            const StandardMaterialConversion approx = madd.approximateStandardMaterial();

            const bool isGraph = exact.blocker.rfind("shader-graph material", 0) == 0;
            if (!isGraph) {
                exactRecords++;
                // A material that has an exact conversion must not be approximated:
                // the entry point forwards it untouched.
                if (approx.converted != exact.converted || approx.blocker != exact.blocker ||
                    approx.lossy != exact.lossy) {
                    delegationMismatch++;
                    fail(path.filename().string() + ": approximate() did not forward an exact record");
                }
                continue;
            }

            graphRecords++;
            if (!approx.converted) {
                REQUIRE_FALSE(approx.blocker.empty());
                blockers[approx.blocker]++;
                continue;
            }
            graphApproximated++;

            // An approximation must always announce itself, so a caller cannot
            // mistake one for a conversion.
            const bool announced =
                std::any_of(approx.lossy.begin(), approx.lossy.end(), [](const std::string& l) {
                    return l.rfind("approximated from a shader graph", 0) == 0;
                });
            if (!announced)
                fail(path.filename().string() + ": approximation is not labelled as one");

            const std::pair<const char*, const std::optional<TextureLayer>*> slots[] = {
                {"diffuse", &approx.material.diffuseLayer},
                {"normal", &approx.material.normalLayer},
                {"specular", &approx.material.specularLayer},
                {"emissive", &approx.material.emissiveLayer1},
                {"emissive2", &approx.material.emissiveLayer2},
                {"environment", &approx.material.environmentLayer}};
            size_t here = 0;
            for (const auto& [name, slot] : slots) {
                if (!slot->has_value())
                    continue;
                here++;
                layers++;
                roles[name]++;
                const std::string& tex = (*slot)->texturePath;
                if (tex.empty()) {
                    fail(path.filename().string() + ": approximated layer has no texture");
                    continue;
                }
                if (std::find(madd.texturePaths.begin(), madd.texturePaths.end(), tex) ==
                    madd.texturePaths.end())
                    fail(path.filename().string() + ": approximated texture '" + tex +
                         "' is not in this record's texturePaths");
            }
            if (here == 0)
                fail(path.filename().string() + ": converted but produced no layer");
            if (approx.material.name != madd.materialName)
                fail(path.filename().string() + ": name not carried over");
        }
    }

    std::cout << "=== MADD -> approximated StandardMaterial ===\n"
              << "Records: " << total << " (exact " << exactRecords << ", shader-graph "
              << graphRecords << ")\n"
              << "Shader-graph approximated: " << graphApproximated << " ("
              << (graphRecords ? graphApproximated * 100 / graphRecords : 0) << "% of graph records), "
              << layers << " layers" << std::endl;
    for (const auto& [reason, count] : blockers)
        std::cout << "  refused: " << reason << " x" << count << std::endl;
    for (const auto& [role, count] : roles)
        std::cout << "  role " << role << ": " << count << std::endl;
    for (const auto& f : failures)
        std::cout << "FAIL " << f << std::endl;

    REQUIRE(total > 0);
    REQUIRE(graphRecords > 0);
    CHECK(failures.empty());
    CHECK(delegationMismatch == 0);
    // Roughly half the shader-graph materials are surface materials that name
    // their textures by role; the rest are procedural FX with nothing to map.
    CHECK(graphApproximated * 100 >= graphRecords * 40);
    CHECK(graphApproximated < graphRecords);
}
