// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// M3 version-upgrade validation: iterates the M3 corpora, parses each file,
// and checks that old-version chunks come out with the same canonical values
// the SC2 client's version-normalization pass (M3_ProcessChunks) produces.
// Files containing old-version chunks additionally go through a
// parse → write → re-parse cycle to verify the writer restores the raw
// pre-upgrade values for old-version write-back.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
    size_t parseOk = 0;
    size_t parseFail = 0;
    size_t roundTripOk = 0;
    size_t roundTripFail = 0;
    std::map<std::string, size_t> chunkVersions;
    std::vector<std::string> violations;
    std::vector<std::string> mismatches;

    void violation(const fs::path& file, const std::string& what) {
        if (violations.size() < 50)
            violations.push_back(file.filename().string() + ": " + what);
        else if (violations.size() == 50)
            violations.push_back("(more violations suppressed)");
    }
    void mismatch(const fs::path& file, const std::string& what) {
        if (mismatches.size() < 50)
            mismatches.push_back(file.filename().string() + ": " + what);
        else if (mismatches.size() == 50)
            mismatches.push_back("(more mismatches suppressed)");
    }
};

InterpolationMode expectedParticleColorSmoothing(ParticleFlag flags) {
    if (hasFlag(flags, ParticleFlag::OldColorBezier))
        return InterpolationMode::Bezier;
    if (hasFlag(flags, ParticleFlag::OldColorSmooth))
        return InterpolationMode::LinearSmooth;
    return InterpolationMode::Linear;
}

InterpolationMode expectedRibbonSizeSmoothing(RibbonFlag flags) {
    if (hasFlag(flags, RibbonFlag::BezierSmoothSize))
        return InterpolationMode::Bezier;
    if (hasFlag(flags, RibbonFlag::SmoothSize))
        return InterpolationMode::LinearSmooth;
    return InterpolationMode::Linear;
}

bool hasOldVersionChunks(const Model& model) {
    for (const auto& p : model.particleEmitters)
        if (p.getVersion() >= 0 && p.getVersion() < 24)
            return true;
    for (const auto& r : model.ribbonEmitters)
        if (r.getVersion() >= 0 && r.getVersion() < 9)
            return true;
    for (const auto& b : model.rigidBodies) {
        if (b.getVersion() >= 0 && b.getVersion() < 4)
            return true;
        for (const auto& s : b.rigidBodyShape)
            if (s.getVersion() >= 0 && s.getVersion() < 3)
                return true;
    }
    return false;
}

void checkUpgradeInvariants(const fs::path& file, const Model& model, Stats& stats) {
    for (const auto& p : model.particleEmitters) {
        const i32 v = p.getVersion();
        stats.chunkVersions["PAR_ v" + std::to_string(v)]++;
        if (v < 24 && p.worldForcesMassMultiplier != 1.0f)
            stats.violation(file, "PAR_ v" + std::to_string(v) + " worldForcesMassMultiplier != 1");
        if (v < 18 && !hasFlag(p.rotationFlags, ParticleRotationFlag::Unknown6))
            stats.violation(file, "PAR_ v" + std::to_string(v) + " rotationFlags missing Unknown6");
        if (v <= 12 && p.colorSmoothing != expectedParticleColorSmoothing(p.flags))
            stats.violation(file, "PAR_ v" + std::to_string(v) + " colorSmoothing not derived");
        if (v <= 22 && hasFlag(p.flags, ParticleFlag::ModelParticles) &&
            p.spawnRibbonOnBounceChance != 1.0f)
            stats.violation(file, "PAR_ v" + std::to_string(v) + " ModelParticles not migrated");
    }
    for (const auto& r : model.ribbonEmitters) {
        const i32 v = r.getVersion();
        stats.chunkVersions["RIB_ v" + std::to_string(v)]++;
        if (v <= 8 && r.worldForcesMassMultiplier != 1.0f)
            stats.violation(file, "RIB_ v" + std::to_string(v) + " worldForcesMassMultiplier != 1");
        if (v <= 6) {
            if (r.sizeSmoothing != expectedRibbonSizeSmoothing(r.flags))
                stats.violation(file, "RIB_ v" + std::to_string(v) + " sizeSmoothing not derived");
            if (r.colorSmoothing != InterpolationMode::Linear)
                stats.violation(file, "RIB_ v" + std::to_string(v) + " colorSmoothing != Linear");
        }
    }
    for (const auto& b : model.rigidBodies) {
        const i32 v = b.getVersion();
        stats.chunkVersions["PHRB v" + std::to_string(v)]++;
        if (v <= 2) {
            if (b.physicsType != 24 || b.simulationType != 0)
                stats.violation(file,
                                "PHRB v" + std::to_string(v) + " absent-field defaults missing");
        }
        for (const auto& s : b.rigidBodyShape) {
            const i32 sv = s.getVersion();
            stats.chunkVersions["PHSH v" + std::to_string(sv)]++;
            if (sv == 1) {
                if (s.shapeType == PhysicsShapeType::ConvexHull &&
                    s.hullVertexCount != s.hullVertexPositions.size())
                    stats.violation(file, "PHSH v1 hull vertices not migrated");
                if (s.shapeType == PhysicsShapeType::Mesh &&
                    s.meshVertexCount != s.meshVertexPositions.size())
                    stats.violation(file, "PHSH v1 mesh vertices not migrated");
            }
            if (sv == 2 && s.shapeType == PhysicsShapeType::Mesh && !s.meshFaceIndices32.empty() &&
                s.meshVertexPositions.empty())
                stats.violation(file, "PHSH v2 mesh vertices not migrated");
        }
    }
}

void compareUpgradedFields(const fs::path& file, const Model& a, const Model& b, Stats& stats) {
    if (a.particleEmitters.size() != b.particleEmitters.size() ||
        a.ribbonEmitters.size() != b.ribbonEmitters.size() ||
        a.rigidBodies.size() != b.rigidBodies.size()) {
        stats.mismatch(file, "chunk counts changed after round-trip");
        return;
    }
    for (size_t i = 0; i < a.particleEmitters.size(); ++i) {
        const auto& pa = a.particleEmitters[i];
        const auto& pb = b.particleEmitters[i];
        if (pa.instanceType != pb.instanceType)
            stats.mismatch(file, "PAR_ instanceType");
        if (pa.instanceDistance != pb.instanceDistance)
            stats.mismatch(file, "PAR_ instanceDistance");
        if (pa.rotationFlags != pb.rotationFlags)
            stats.mismatch(file, "PAR_ rotationFlags");
        if (pa.additionalFlags != pb.additionalFlags)
            stats.mismatch(file, "PAR_ additionalFlags");
        if (pa.colorSmoothing != pb.colorSmoothing || pa.sizeSmoothing != pb.sizeSmoothing ||
            pa.rotationSmoothing != pb.rotationSmoothing)
            stats.mismatch(file, "PAR_ smoothing");
        if (pa.ribbonLinkIndex != pb.ribbonLinkIndex)
            stats.mismatch(file, "PAR_ ribbonLinkIndex");
    }
    for (size_t i = 0; i < a.ribbonEmitters.size(); ++i) {
        const auto& ra = a.ribbonEmitters[i];
        const auto& rb = b.ribbonEmitters[i];
        if (ra.sizeSmoothing != rb.sizeSmoothing || ra.colorSmoothing != rb.colorSmoothing)
            stats.mismatch(file, "RIB_ smoothing");
        if (ra.additionalFlags != rb.additionalFlags)
            stats.mismatch(file, "RIB_ additionalFlags");
    }
    for (size_t i = 0; i < a.rigidBodies.size(); ++i) {
        const auto& ba = a.rigidBodies[i];
        const auto& bb = b.rigidBodies[i];
        if (ba.density != bb.density || ba.friction != bb.friction ||
            ba.gravityScale != bb.gravityScale)
            stats.mismatch(file, "PHRB material");
        if (ba.rigidBodyShape.size() != bb.rigidBodyShape.size()) {
            stats.mismatch(file, "PHSH count");
            continue;
        }
        for (size_t j = 0; j < ba.rigidBodyShape.size(); ++j) {
            const auto& sa = ba.rigidBodyShape[j];
            const auto& sb = bb.rigidBodyShape[j];
            if (sa.shapeDimensions.x != sb.shapeDimensions.x ||
                sa.shapeDimensions.y != sb.shapeDimensions.y ||
                sa.shapeDimensions.z != sb.shapeDimensions.z)
                stats.mismatch(file, "PHSH shapeDimensions");
            if (sa.meshVertexCount != sb.meshVertexCount ||
                sa.meshFaceIndex32Count != sb.meshFaceIndex32Count ||
                sa.meshTreeDepth != sb.meshTreeDepth)
                stats.mismatch(file, "PHSH mesh counts");
            if (sa.hullVertexPositions != sb.hullVertexPositions ||
                sa.meshVertexPositions != sb.meshVertexPositions ||
                sa.meshFaceIndices32 != sb.meshFaceIndices32 ||
                sa.meshBvhNodes.size() != sb.meshBvhNodes.size())
                stats.mismatch(file, "PHSH migrated geometry");
            if (sa.deprecated.v2.tailUnknown0 != sb.deprecated.v2.tailUnknown0 ||
                sa.deprecated.v2.tailUnknown1 != sb.deprecated.v2.tailUnknown1 ||
                sa.deprecated.v2.tailUnknown2 != sb.deprecated.v2.tailUnknown2)
                stats.mismatch(file, "PHSH v2 tail");
        }
    }
}

} // namespace

TEST_CASE("M3 corpus version-upgrade validation", "[m3][corpus]") {
    std::vector<fs::path> files;
    std::string corpusBase = test::findCorpusBase("Corpus");
    if (const char* env = std::getenv("M3_CORPUS_DIR"); env && fs::is_directory(env)) {
        corpusBase.clear();
        for (const auto& entry : fs::recursive_directory_iterator(env))
            if (entry.is_regular_file() && entry.path().extension() == ".m3")
                files.push_back(entry.path());
    } else if (!corpusBase.empty()) {
        for (auto sub : {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"}) {
            fs::path dir = fs::path(corpusBase) / sub;
            if (!fs::is_directory(dir))
                continue;
            for (const auto& entry : fs::recursive_directory_iterator(dir))
                if (entry.is_regular_file() && entry.path().extension() == ".m3")
                    files.push_back(entry.path());
        }
    }
    if (files.empty())
        SKIP("M3 corpus not found");
    std::sort(files.begin(), files.end());

    size_t limit = files.size();
    if (const char* env = std::getenv("M3_CORPUS_LIMIT"); env && *env)
        limit = std::min<size_t>(limit, std::strtoull(env, nullptr, 10));

    std::cout << "=== M3 Upgrade Validation ===\n"
              << "Files: " << limit << " of " << files.size() << std::endl;

    Stats stats;
    for (size_t idx = 0; idx < limit; ++idx) {
        const auto& path = files[idx];
        if (idx % 2000 == 0 && idx > 0)
            std::cout << "[" << idx << "/" << limit << "]" << std::endl;

        Model model;
        try {
            Parser parser;
            model = parser.parse(path.string());
            stats.parseOk++;
        } catch (const std::exception&) {
            stats.parseFail++;
            continue;
        }

        checkUpgradeInvariants(path, model, stats);

        if (!hasOldVersionChunks(model))
            continue;
        try {
            Writer writer;
            std::vector<u8> bytes = writer.write(model);
            Parser reparser;
            Model reparsed = reparser.parse(std::span<const u8>(bytes));
            compareUpgradedFields(path, model, reparsed, stats);
            stats.roundTripOk++;
        } catch (const std::exception& e) {
            stats.roundTripFail++;
            stats.mismatch(path, std::string("round-trip threw: ") + e.what());
        }
    }

    std::cout << "Parsed OK: " << stats.parseOk << ", failed: " << stats.parseFail << "\n"
              << "Round-trips OK: " << stats.roundTripOk << ", failed: " << stats.roundTripFail
              << "\nChunk versions seen:" << std::endl;
    for (const auto& [key, count] : stats.chunkVersions)
        std::cout << "  " << key << ": " << count << std::endl;
    for (const auto& v : stats.violations)
        std::cout << "VIOLATION " << v << std::endl;
    for (const auto& m : stats.mismatches)
        std::cout << "MISMATCH " << m << std::endl;

    REQUIRE(stats.parseOk > 0);
    CHECK(stats.violations.empty());
    CHECK(stats.mismatches.empty());
}
