// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P5 gate — the converter sweeps, against **recorded expected loss**.
///
/// A lossy converter's gate cannot be "no diagnostics": every one of these three
/// formats loses something real on shipped content, and the useful question is
/// whether it loses *exactly what it lost last time*. So each sweep writes its
/// diagnostics as a code histogram and diffs that against a checked-in golden
/// under `tests/data/wem/`. A new code, or a code that stopped firing, fails.
///
/// The counts are deliberately **not** compared — they scale with how many files
/// the sweep reached, and `WEM_CORPUS_LIMIT` moves that. The *set* of codes is
/// what a converter change alters and what a reviewer needs to see.
///
/// Re-record with `WEM_RECORD_LOSS=1`, and read the diff before committing it:
/// a golden that grows a code is a converter that started losing something new.
///
/// The `.m2` leg is hidden behind a tag for the same reason the P1 sweep's is:
/// 56 known corpus files drive the M2 parser into multi-GB zero-filled resizes,
/// so a full run needs a process commit cap set outside the test.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m2/parser.h>
#include <whiteout/models/m3/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>
#include <whiteout/utils/os_file_system.h>

#include "test_helpers.h"
#include "wem_corpus_files.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

struct ConvertStats {
    u32 files = 0;
    u32 skippedKnownBad = 0;
    u32 imported = 0;
    u32 models = 0;
    u32 meshes = 0;
    u32 materials = 0;
    u32 nodes = 0;
    u32 textures = 0;
    u32 profileSets = 0;
    u32 documentRoundTrips = 0;
    u32 roundTripMismatches = 0;
    u32 validationErrors = 0;
    std::vector<std::string> failing;

    /// Every diagnostic code the sweep saw, once each. The set is the golden;
    /// the counts stay in the log, where a reader can see them without the gate
    /// depending on how many files ran.
    std::set<u16> codes;
    std::vector<std::pair<u16, u32>> counts;

    /// With `WEM_SHOW_LOSS=<CodeName>`, print the first few messages behind that
    /// code. A histogram says a converter lost 963 things; only the messages say
    /// whether that is 963 real losses or one misread field.
    static void show(const Diagnostics& diagnostics, const std::string& label) {
        static const char* wanted = std::getenv("WEM_SHOW_LOSS");
        static int shown = 0;
        if (wanted == nullptr || shown >= 12) {
            return;
        }
        for (const Diagnostic& entry : diagnostics.all()) {
            if (std::string(ToString(entry.code)) == wanted && shown < 12) {
                std::cout << "  [" << label << "] " << entry.message << "\n";
                ++shown;
            }
        }
    }

    void observe(const Diagnostics& diagnostics) {
        for (const Diagnostics::CodeCount& row : diagnostics.histogram()) {
            const u16 code = static_cast<u16>(row.code);
            codes.insert(code);
            bool found = false;
            for (auto& entry : counts) {
                if (entry.first == code) {
                    entry.second += row.count;
                    found = true;
                    break;
                }
            }
            if (!found) {
                counts.emplace_back(code, row.count);
            }
        }
    }

    void report(const char* format) const {
        std::cout << "\n=== " << format << " -> Document ===\n";
        std::cout << "  files       : " << files << " (" << skippedKnownBad << " known bad)\n";
        std::cout << "  imported    : " << imported << "\n";
        std::cout << "  models      : " << models << ", meshes " << meshes << ", nodes " << nodes
                  << "\n";
        std::cout << "  materials   : " << materials << " in " << profileSets << " profile set(s)"
                  << ", textures " << textures << "\n";
        std::cout << "  wem I/O     : " << documentRoundTrips << " round trips, "
                  << roundTripMismatches << " mismatches\n";
        std::cout << "  validation  : " << validationErrors << " document(s) with errors\n";
        std::cout << "  loss codes  :\n";
        std::vector<std::pair<u16, u32>> sorted = counts;
        std::sort(sorted.begin(), sorted.end());
        for (const auto& [code, count] : sorted) {
            std::cout << "    " << ToString(static_cast<DiagCode>(code)) << " " << count << "\n";
        }
        for (const std::string& line : failing) {
            std::cout << "  ! " << line << "\n";
        }
        std::cout << "======================================\n";
    }
};

fs::path goldenPath(const char* format) {
    return fs::path(WHITEOUT_TEST_SOURCE_DIR) / "data" / "wem" /
           (std::string("expected_loss_") + format + ".txt");
}

/// A recorded list: the codes, plus the two numbers that make comparing them
/// meaningful.
struct Golden {
    std::vector<std::string> codes;
    std::size_t limit = 0;      ///< File cap the list was recorded at.
    long validationErrors = -1; ///< Documents that failed `Validate` then.
    bool present = false;
};

Golden readGolden(const fs::path& path) {
    Golden golden;
    std::ifstream stream(path);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        golden.present = true;
        if (line.rfind("limit=", 0) == 0) {
            golden.limit = std::strtoull(line.c_str() + 6, nullptr, 10);
        } else if (line.rfind("validation_errors=", 0) == 0) {
            golden.validationErrors = std::strtol(line.c_str() + 18, nullptr, 10);
        } else {
            golden.codes.push_back(line);
        }
    }
    return golden;
}

/// Compares the sweep to its recorded list.
///
/// Three rules, each earning its place:
///
/// - A **new** code always fails. The converter started losing something it did
///   not lose before, whatever the file count.
/// - A **missing** code fails only when this run read at least as many files as
///   the recording did. Fewer files legitimately miss the rarer losses; more
///   files cannot.
/// - The **validation-error count** is compared only at the recorded limit,
///   where it is the same population. A count and not a set, because the
///   regression worth catching is "this now fails on twenty models" and the code
///   would be identical either way.
void checkAgainstGolden(const char* format, const ConvertStats& stats, std::size_t limit) {
    std::vector<std::string> observed;
    for (u16 code : stats.codes) {
        observed.push_back(ToString(static_cast<DiagCode>(code)));
    }
    std::sort(observed.begin(), observed.end());

    const fs::path path = goldenPath(format);
    if (std::getenv("WEM_RECORD_LOSS") != nullptr) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::trunc);
        out << "# WEM P5 expected-loss list for the " << format << " converter.\n";
        out << "# One DiagCode per line, sorted. Counts are deliberately absent: they scale\n";
        out << "# with the file cap and the set of codes does not. `limit` is the cap this was\n";
        out << "# recorded at, so a smaller run knows it may legitimately see fewer codes.\n";
        out << "limit=" << limit << "\n";
        out << "validation_errors=" << stats.validationErrors << "\n";
        for (const std::string& code : observed) {
            out << code << "\n";
        }
        std::cout << "recorded " << observed.size() << " code(s) at limit " << limit << " to "
                  << test::pathText(path) << "\n";
        return;
    }

    const Golden golden = readGolden(path);
    if (!golden.present) {
        SKIP("no recorded expected-loss list for " + std::string(format) +
             "; run with WEM_RECORD_LOSS=1");
    }

    for (const std::string& code : observed) {
        if (std::find(golden.codes.begin(), golden.codes.end(), code) == golden.codes.end()) {
            FAIL_CHECK(std::string(format) + " lost something new: " + code);
        }
    }
    if (limit >= golden.limit) {
        for (const std::string& code : golden.codes) {
            if (std::find(observed.begin(), observed.end(), code) == observed.end()) {
                FAIL_CHECK(std::string(format) + " stopped reporting " + code +
                           " (fixed, or stopped looking?)");
            }
        }
    }
    if (limit == golden.limit && golden.validationErrors >= 0) {
        CHECK(static_cast<long>(stats.validationErrors) == golden.validationErrors);
    }
}

/// The other half of the gate: the imported document survives a real write and
/// read, and comes back structurally the same. A converter that produces a
/// document the writer cannot represent is a converter that has not landed.
void roundTrip(ConvertStats& stats, const Document& document, const std::string& label) {
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    if (bytes.empty()) {
        ++stats.roundTripMismatches;
        if (stats.failing.size() < 20) {
            stats.failing.push_back(label + " -> writer produced nothing");
        }
        return;
    }
    Parser parser;
    const std::optional<Document> reloaded =
        parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    ++stats.documentRoundTrips;
    if (!reloaded.has_value()) {
        ++stats.roundTripMismatches;
        if (stats.failing.size() < 20) {
            stats.failing.push_back(label + " -> did not read back");
        }
        return;
    }
    if (reloaded->models.size() != document.models.size() ||
        reloaded->textures.size() != document.textures.size() ||
        reloaded->profiles.size() != document.profiles.size()) {
        ++stats.roundTripMismatches;
        if (stats.failing.size() < 20) {
            stats.failing.push_back(label + " -> shape changed across the round trip");
        }
        return;
    }
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        const Model& before = document.models[m];
        const Model& after = reloaded->models[m];
        if (before.meshes.size() != after.meshes.size() ||
            before.nodes.size() != after.nodes.size() ||
            before.materialSlots.size() != after.materialSlots.size()) {
            ++stats.roundTripMismatches;
            if (stats.failing.size() < 20) {
                stats.failing.push_back(label + " -> model " + std::to_string(m) + " changed");
            }
            return;
        }
    }
}

void accumulate(ConvertStats& stats, const Document& document, const Diagnostics& diagnostics,
                const std::string& label) {
    ++stats.imported;
    stats.observe(diagnostics);
    ConvertStats::show(diagnostics, label);
    stats.models += static_cast<u32>(document.models.size());
    stats.textures += static_cast<u32>(document.textures.size());
    for (const Model& model : document.models) {
        stats.meshes += static_cast<u32>(model.meshes.size());
        stats.nodes += model.nodes.size();
        stats.profileSets += static_cast<u32>(model.profileSets.size());
        for (const ProfileMaterialSet& set : model.profileSets) {
            stats.materials += static_cast<u32>(set.materials.size());
        }
    }

    const Diagnostics report = Validate(document, ValidateLevel::Profile);
    stats.observe(report);
    ConvertStats::show(report, label);
    if (report.hasErrors()) {
        ++stats.validationErrors;
        if (stats.failing.size() < 20) {
            stats.failing.push_back(label + " -> " + report.formatHistogram());
        }
    }

    roundTrip(stats, document, label);
}

} // namespace

TEST_CASE("WEM P5 converter sweep: mdx", "[wem][corpus][convert]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 300);
    const MdxConverter converter;

    ConvertStats stats;
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
        Result<Document> result =
            converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (!result.ok()) {
            continue;
        }
        accumulate(stats, *result, result.diagnostics, test::pathText(files[i].filename()));
    }

    stats.report("mdx");
    CHECK(stats.imported > 0);
    CHECK(stats.meshes > 0);
    CHECK(stats.materials > 0);
    CHECK(stats.roundTripMismatches == 0);
    checkAgainstGolden("mdx", stats, limit);
}

TEST_CASE("WEM P5 converter sweep: m3", "[wem][corpus][convert]") {
    const auto files =
        test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    if (files.empty()) {
        SKIP("M3 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 300);
    const M3Converter converter;

    ConvertStats stats;
    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        ++stats.files;
        Result<Document> result =
            converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (!result.ok()) {
            continue;
        }
        accumulate(stats, *result, result.diagnostics, test::pathText(files[i].filename()));
    }

    stats.report("m3");
    CHECK(stats.imported > 0);
    CHECK(stats.meshes > 0);
    CHECK(stats.materials > 0);
    CHECK(stats.roundTripMismatches == 0);
    checkAgainstGolden("m3", stats, limit);
}

TEST_CASE("WEM P5 converter sweep: m2", "[wem][corpus][convert][.m2slow]") {
    const auto files = test::gather("WEM_M2_CORPUS_DIR", ".m2", {"WoW", "WowM2"});
    if (files.empty()) {
        SKIP("M2 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 100);
    const M2Converter converter;

    ConvertStats stats;
    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        utils::OsFileSystem vfs(test::pathText(files[i].parent_path()));
        m2::Parser parser;
        const m2::Model model = parser.parse(vfs, test::pathText(files[i]));
        if (model.skinProfiles.empty()) {
            continue;
        }
        ++stats.files;
        Result<Document> result = converter.fromM2(model);
        if (!result.ok()) {
            continue;
        }
        accumulate(stats, *result, result.diagnostics, test::pathText(files[i].filename()));
    }

    stats.report("m2");
    CHECK(stats.roundTripMismatches == 0);
    checkAgainstGolden("m2", stats, limit);
}
