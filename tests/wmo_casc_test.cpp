// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The WMO reader against the files 12.1 ships, read from the local install's CASC by
/// FileDataID, groups through GFID as the client finds them. A handful of fixed models runs by
/// default; `[.census]` walks every root the listfile names. Both check what a renderer relies
/// on: every group loads, every stream is one entry per vertex, every index is in range, and no
/// chunk goes unrecognised.
///
/// Skips when no World of Warcraft install is found. `WMO_WOW_DIR` names one explicitly;
/// `WMO_LISTFILE` names the listfile the census enumerates with.

#include <catch2/catch_all.hpp>

#include <whiteout/models/wow/wmo/wmo.h>
#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/blizzard_game_finder.h>
#include <whiteout/utils/casc_file_system.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::models::wow::wmo;
namespace casc = whiteout::storages::casc;

namespace {

std::string wowInstall() {
    if (const char* env = std::getenv("WMO_WOW_DIR"); env && std::filesystem::is_directory(env))
        return env;
    for (const auto& game : utils::findBlizzardGames()) {
        if (game.game == utils::BlizzardGame::WorldOfWarcraft)
            return game.path;
    }
    return {};
}

std::string listfilePath() {
    if (const char* env = std::getenv("WMO_LISTFILE"); env && std::filesystem::exists(env))
        return env;
    for (auto candidate : {"Corpus/community-listfile.csv", "../Corpus/community-listfile.csv",
                           "C:/Projects/WhiteoutLib/Corpus/community-listfile.csv"}) {
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return {};
}

std::string tactKeys() {
    for (auto candidate : {"Corpus/tactkeys.txt", "../Corpus/tactkeys.txt",
                           "C:/Projects/WhiteoutLib/Corpus/tactkeys.txt"}) {
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return {};
}

struct Install {
    std::vector<u8> listfile; // the storage borrows it
    std::optional<casc::Storage> storage;
};

Install openInstall(bool withListfile) {
    Install out;
    const std::string root = wowInstall();
    if (root.empty())
        return out;
    if (withListfile) {
        const std::string path = listfilePath();
        if (path.empty())
            return out;
        std::ifstream in(path, std::ios::binary);
        out.listfile.assign(std::istreambuf_iterator<char>(in), {});
    }
    casc::OpenOptions options;
    options.path = root;
    options.product = "wow";
    options.localeMask = casc::LocaleMasks::enUS;
    options.listfile = out.listfile;
    out.storage = casc::Storage::open(options);
    if (out.storage) {
        if (const std::string keys = tactKeys(); !keys.empty())
            out.storage->importKeysFromFile(keys);
    }
    return out;
}

/// What one model breaks, by kind; empty for a clean one.
std::map<std::string, u32> checkModel(const Model& model) {
    std::map<std::string, u32> faults;
    const Root& root = model.root;
    for (u32 g = 0; g < model.groups.size(); ++g) {
        if (!model.groups[g]) {
            ++faults["group missing"];
            continue;
        }
        const Group& group = *model.groups[g];
        const std::size_t n = group.positions.size();
        if (group.normals.size() != n)
            ++faults["normals != vertices"];
        for (const auto& uv : group.uvSets)
            if (uv.size() != n)
                ++faults["uv set != vertices"];
        for (const auto& c : group.vertexColors)
            if (c.size() != n)
                ++faults["MOCV != vertices"];
        if (group.vertexColors2 && group.vertexColors2->size() != n)
            ++faults["MOC2 != vertices"];
        if (group.uvSets.size() > 4)
            ++faults["more than four MOTV"];
        for (u32 index : group.indices)
            if (index >= n) {
                ++faults["index out of range"];
                break;
            }
        if (!group.polys.empty() && group.polys.size() * 3 != group.indices.size())
            ++faults["polys != indices / 3"];
        for (const Batch& b : group.batches) {
            if (static_cast<u64>(b.startIndex) + b.indexCount > group.indices.size())
                ++faults["batch past the indices"];
            else {
                // MOBA's own vertex range, which the window path trusts.
                for (u32 i = b.startIndex; i < b.startIndex + b.indexCount; ++i) {
                    const u32 v = group.indices[i];
                    if (v < b.firstVertex() || v > b.lastVertex()) {
                        ++faults["batch vertex outside min/max"];
                        break;
                    }
                }
            }
            if (b.material() >= root.materials.size())
                ++faults["batch material out of range"];
        }
        if (group.liquid &&
            group.liquid->vertices.size() != static_cast<std::size_t>(group.liquid->xVertices) *
                                                 static_cast<std::size_t>(group.liquid->yVertices))
            ++faults["liquid grid"];
    }
    return faults;
}

bool unknownChunk(const std::vector<std::string>& issues) {
    return std::any_of(issues.begin(), issues.end(), [](const std::string& s) {
        return s.find("unknown chunk") != std::string::npos;
    });
}

} // namespace

TEST_CASE("shipped WMOs read the way 12.1 reads them", "[wmo][casc]") {
    Install install = openInstall(false);
    if (!install.storage)
        SKIP("no World of Warcraft install");
    utils::CascFileSystem fs(*install.storage);

    // A vanilla building, a city, a group with liquid, a Legion-era LOD model, a Shadowlands
    // castle whose last LOD needs 32-bit indices, and a Dragonflight raid with MNLD/MAVG/MPY2.
    const std::vector<std::pair<u32, const char*>> models = {
        {106848, "goldshireblacksmith"},   {107243, "stormwind"},
        {304162, "gilneas_cathedral"},     {2391709, "8sw_magetower01"},
        {2737380, "9vm_vampire_castle01"}, {4631239, "10du_aberrusraid"},
    };
    for (const auto& [fileId, name] : models) {
        INFO(name);
        const std::vector<u8> bytes = fs.readFile(fileId);
        REQUIRE_FALSE(bytes.empty());
        CHECK(detectFileKind(bytes) == FileKind::Root);
        Parser parser;
        const auto model = parser.parse(fs, bytes);
        REQUIRE(model);
        CHECK(model->root.version == kVersion);
        CHECK_FALSE(model->root.groups.empty());
        CHECK_FALSE(unknownChunk(parser.getIssues()));
        const auto faults = checkModel(*model);
        for (const auto& [fault, count] : faults)
            UNSCOPED_INFO(fault << " x" << count);
        CHECK(faults.empty());

        // Every LOD level the root declares loads the groups it names.
        const u32 levels = maxLodLevel(model->root);
        Model withLods = *model;
        for (u32 lod = 1; lod <= levels; ++lod) {
            CHECK(parser.loadLod(withLods, lod, fs));
            u32 loaded = 0;
            for (const auto& g : withLods.lodGroups[lod - 1])
                loaded += g ? 1 : 0;
            CHECK(loaded > 0);
        }
    }
}

TEST_CASE("every shipped WMO root and group parses clean", "[wmo][casc][.census]") {
    Install install = openInstall(true);
    if (!install.storage)
        SKIP("no World of Warcraft install or listfile");
    utils::CascFileSystem fs(*install.storage);

    std::vector<i32> roots;
    install.storage->enumerate([&](const casc::EnumerateEntry& e) {
        const std::string_view path = e.path;
        if (path.size() > 4 && e.fileDataId > 0) {
            std::string ext(path.substr(path.size() - 4));
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wmo")
                roots.push_back(e.fileDataId);
        }
        return true;
    });
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

    u32 rootCount = 0, groupCount = 0, lodGroupCount = 0, failed = 0, unreadable = 0;
    std::map<std::string, u32> faults;
    std::map<std::string, u32> issues;
    std::vector<i32> failedIds;
    for (const i32 id : roots) {
        const std::vector<u8> bytes = fs.readFile(static_cast<u32>(id));
        if (bytes.empty()) {
            ++unreadable;
            continue;
        }
        if (detectFileKind(bytes) != FileKind::Root)
            continue;
        ++rootCount;
        Parser parser;
        auto model = parser.parse(fs, bytes);
        if (!model) {
            ++failed;
            failedIds.push_back(id);
            continue;
        }
        for (const std::string& issue : parser.getIssues()) {
            // Keep the kind of issue, not its numbers.
            std::string kind = issue.substr(0, issue.find_first_of("0123456789"));
            ++issues[kind];
        }
        for (const auto& [fault, count] : checkModel(*model))
            faults[fault] += count;
        for (const auto& g : model->groups)
            groupCount += g ? 1 : 0;
        for (u32 lod = 1; lod <= maxLodLevel(model->root); ++lod)
            parser.loadLod(*model, lod, fs);
        for (const auto& level : model->lodGroups)
            for (const auto& g : level)
                lodGroupCount += g ? 1 : 0;
    }
    std::cout << "roots " << rootCount << ", groups " << groupCount << ", LOD groups "
              << lodGroupCount << ", failed " << failed << ", unreadable " << unreadable << "\n";
    for (const auto& [kind, count] : issues)
        std::cout << "  issue: " << kind << " x" << count << "\n";
    for (const auto& [fault, count] : faults)
        std::cout << "  fault: " << fault << " x" << count << "\n";
    for (i32 id : failedIds)
        std::cout << "  failed root " << id << "\n";
    CHECK(rootCount > 10000);
    CHECK(failed == 0);
    CHECK(faults.empty());
    CHECK(issues.empty());
}
