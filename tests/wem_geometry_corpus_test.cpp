// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P1 gate — the corpus sweep.
///
/// Two things this proves that a synthetic mesh cannot:
///
/// 1. Every shipped mesh, once through `MeshBuilder`, satisfies C1–C9 — so the
///    §5.10 contract is a property of real content and not of test fixtures.
/// 2. **How expensive the §5.3 repair actually is.** The design calls this "the
///    one number this design most wants": if the answer is 0.1% of vertices the
///    trade is free; if it is 12% on WoW character models that is worth knowing
///    before P5 depends on repair. The sweep publishes it per format.
///
/// Skips rather than fails when the corpus is absent, so CI without it stays
/// green on the unit tests. `WEM_CORPUS_LIMIT` caps the file count; the per-format
/// `*_CORPUS_DIR` variables override the directory.
///
/// The `.m2` leg is deliberately capped low by default: 56 known corpus files
/// drive the M2 parser into multi-GB zero-filled resizes, so a full sweep needs
/// a process commit cap set outside the test.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/utils/os_file_system.h>

#include "test_helpers.h"
#include "wem_corpus_files.h"
#include "wem_geometry_ingest.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

struct SweepStats {
    u32 files = 0;
    u32 skippedKnownBad = 0;
    u32 meshes = 0;
    u64 sourceVertices = 0;
    u64 sourceFaces = 0;
    u64 verticesAdded = 0;
    u64 facesDropped = 0;
    u32 filesNeedingRepair = 0;
    u32 meshesWithErrors = 0;
    std::vector<std::string> failing;

    void report(const char* format) const {
        const double vertexCost = sourceVertices == 0 ? 0.0
                                                      : 100.0 * static_cast<double>(verticesAdded) /
                                                            static_cast<double>(sourceVertices);
        const double faceCost = sourceFaces == 0 ? 0.0
                                                 : 100.0 * static_cast<double>(facesDropped) /
                                                       static_cast<double>(sourceFaces);
        std::cout << "=== WEM P1 geometry sweep: " << format << " ===\n"
                  << "  files                 " << files << " (" << filesNeedingRepair
                  << " needed repair)\n"
                  << "  meshes                " << meshes << "\n"
                  << "  source vertices       " << sourceVertices << "\n"
                  << "  source faces          " << sourceFaces << "\n"
                  << "  vertices added        " << verticesAdded << "  (" << vertexCost << "%)\n"
                  << "  faces dropped         " << facesDropped << "  (" << faceCost << "%)\n"
                  << "  meshes failing C1-C9  " << meshesWithErrors << "\n"
                  << "  skipped (known bad)   " << skippedKnownBad << std::endl;
        for (std::size_t i = 0; i < failing.size() && i < 20; ++i) {
            std::cout << "    " << failing[i] << std::endl;
        }
    }
};

/// Runs the C1-C9 checks on one ingested mesh and folds it into the stats.
void accumulate(SweepStats& stats, const std::string& label, test::IngestedMesh& ingested,
                bool& fileNeededRepair) {
    ++stats.meshes;
    stats.sourceVertices += ingested.sourcePositions.size();
    stats.sourceFaces += ingested.sourceIndices.size() / 3;
    stats.verticesAdded += ingested.repairStats.verticesAdded;
    stats.facesDropped += ingested.repairStats.facesDropped;
    if (ingested.repairStats.verticesAdded != 0 || ingested.repairStats.facesDropped != 0) {
        fileNeededRepair = true;
    }

    Diagnostics diagnostics;
    geom::CheckStructural(ingested.mesh, 0, diagnostics);
    geom::CheckManifold(ingested.mesh, 0, diagnostics);
    if (diagnostics.hasErrors()) {
        ++stats.meshesWithErrors;
        if (stats.failing.size() < 20) {
            stats.failing.push_back(label + " -> " + diagnostics.formatHistogram());
        }
    }
}

} // namespace

TEST_CASE("WEM P1 geometry sweep: mdx", "[wem][corpus][geometry]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 400);

    SweepStats stats;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            ++stats.skippedKnownBad;
            continue;
        }
        test::trace(files[i]);
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        ++stats.files;
        bool needed = false;
        auto meshes = test::IngestMdx(std::span<const u8>(bytes.data(), bytes.size()));
        for (std::size_t m = 0; m < meshes.size(); ++m) {
            accumulate(stats, test::pathText(files[i].filename()) + "#" + std::to_string(m),
                       meshes[m], needed);
        }
        if (needed) {
            ++stats.filesNeedingRepair;
        }
    }

    stats.report("mdx");
    CHECK(stats.meshes > 0);
    // The gate: every shipped mesh, through MeshBuilder, satisfies C1-C9.
    CHECK(stats.meshesWithErrors == 0);
}

TEST_CASE("WEM P1 geometry sweep: m3", "[wem][corpus][geometry]") {
    const auto files =
        test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    if (files.empty()) {
        SKIP("M3 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 400);

    SweepStats stats;
    for (std::size_t i = 0; i < limit; ++i) {
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        m3::Parser parser;
        const m3::Model model = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        ++stats.files;
        bool needed = false;
        auto meshes = test::IngestM3(model);
        for (std::size_t m = 0; m < meshes.size(); ++m) {
            accumulate(stats, test::pathText(files[i].filename()) + "#" + std::to_string(m),
                       meshes[m], needed);
        }
        if (needed) {
            ++stats.filesNeedingRepair;
        }
    }

    stats.report("m3");
    CHECK(stats.meshes > 0);
    CHECK(stats.meshesWithErrors == 0);
}

TEST_CASE("WEM P1 geometry sweep: m2", "[wem][corpus][geometry][.m2slow]") {
    // Hidden by default (the `.` tag): 56 known corpus files drive the M2 parser
    // into multi-GB zero-filled resizes, so this leg needs a process commit cap
    // set outside the test. Run it explicitly with the tag.
    const auto files = test::gather("WEM_M2_CORPUS_DIR", ".m2", {"WoW", "WowM2"});
    if (files.empty()) {
        SKIP("M2 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 100);

    SweepStats stats;
    for (std::size_t i = 0; i < limit; ++i) {
        // An `.m2` is a bundle — base file plus `.skin` siblings — so it is
        // parsed through a filesystem rather than from a byte span.
        test::trace(files[i]);
        utils::OsFileSystem vfs(test::pathText(files[i].parent_path()));
        m2::Parser parser;
        const m2::Model model = parser.parse(vfs, test::pathText(files[i]));
        if (model.skinProfiles.empty()) {
            if (std::getenv("WEM_SWEEP_TRACE") != nullptr) {
                std::cerr << "  no skin profiles (vertices=" << model.vertices.size() << ")"
                          << std::endl;
            }
            continue;
        }
        ++stats.files;
        bool needed = false;
        auto meshes = test::IngestM2(model);
        for (std::size_t m = 0; m < meshes.size(); ++m) {
            accumulate(stats, test::pathText(files[i].filename()) + "#" + std::to_string(m),
                       meshes[m], needed);
        }
        if (needed) {
            ++stats.filesNeedingRepair;
        }
    }

    stats.report("m2");
    CHECK(stats.meshesWithErrors == 0);
}
