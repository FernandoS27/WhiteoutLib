// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_ow_type_survey_test: Checks Overwatch's asset-type names against content.
///
/// Overwatch has no filenames — every asset is a bare 64-bit GUID whose only
/// structure is a type, and not even plainly: the client bit-reverses the top
/// 16 bits and adds one to get it, so the `0D00` an asset prints with is type
/// 0x00C. The type is what makes an install searchable.
///
/// The names now come from the client's own registry (see ow_asset_types.h).
/// This walks a real install and checks them against what the files actually
/// hold: Wwise containers announce themselves as RIFF or BKHD, Blizzard's own
/// containers carry a `6F 45 23 F1` magic followed by a reversed four-character
/// tag. A container that contradicts its name means the GUID-to-type-id
/// reversal is wrong, which is the thing worth catching.
///
/// Point WHITEOUT_OW_PATH at an install; the test skips without it.

#include "../src/whiteout/storages/casc/roots/ow/ow_asset_types.h"
#include "../src/whiteout/storages/casc/roots/ow/ow_manifest_crypto.h"
#include "../src/whiteout/storages/casc/roots/ow/ow_resource_graph.h"

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_thread_pool.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;
namespace ow = whiteout::storages::casc::ow;

namespace {

constexpr u64 kTypeMask = 0x0FFF000000000000ULL;
constexpr size_t kSamplesPerType = 6;

/// Blizzard's container magic; the four bytes after it are a tag stored
/// back-to-front ("LDOM" for MODL).
constexpr u8 kTankMagic[4] = {0x6F, 0x45, 0x23, 0xF1};

u64 typeOf(u64 guid) {
    return (guid & kTypeMask) >> 48;
}

bool trailingGuid(std::string_view path, u64& out) {
    auto const dot = path.find_last_of('.');
    if (dot != std::string_view::npos)
        path = path.substr(0, dot);
    if (path.size() < 16)
        return false;
    u64 value = 0;
    for (char c : path.substr(path.size() - 16)) {
        u64 nibble = 0;
        if (c >= '0' && c <= '9')
            nibble = u64(c - '0');
        else if (c >= 'a' && c <= 'f')
            nibble = u64(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            nibble = u64(c - 'A' + 10);
        else
            return false;
        value = (value << 4) | nibble;
    }
    out = value;
    return true;
}

bool startsWith(std::span<const u8> data, const char* tag) {
    return data.size() >= 4 && std::memcmp(data.data(), tag, 4) == 0;
}

u32 readLE32(std::span<const u8> data, size_t off) {
    if (data.size() < off + 4)
        return 0;
    return u32(data[off]) | (u32(data[off + 1]) << 8) | (u32(data[off + 2]) << 16) |
           (u32(data[off + 3]) << 24);
}

/// What the leading bytes say this file is, or "" when nothing is recognised.
std::string signatureOf(std::span<const u8> data) {
    if (data.size() < 8)
        return {};

    if (startsWith(data, "RIFF"))
        return "wem"; // Wwise audio
    if (startsWith(data, "BKHD"))
        return "bnk"; // Wwise soundbank
    if (startsWith(data, "DUTS"))
        return "stud";
    if (startsWith(data, "IVOM"))
        return "movi";
    if (startsWith(data, "OggS"))
        return "ogg";

    if (std::memcmp(data.data(), kTankMagic, 4) == 0) {
        std::string tag;
        for (size_t i = 8; i-- > 4;)
            tag += char(std::tolower(data[i]));
        return tag;
    }

    // The STU data assets all lead with a 0x24-byte header.
    if (readLE32(data, 4) == 0x24)
        return "stu";

    return {};
}

struct Bucket {
    size_t total = 0;
    std::vector<std::string> samples;
};

/// Spellings of a manifest name to try, best first.
///
/// The IV for an encrypted manifest is the SHA-1 of its name, so the exact
/// casing decides whether the first block decrypts. Enumeration hands back the
/// normalised, lowercased path and Storage does not expose the root that keeps
/// the original, so a survey has to guess. Each token gets its known casings and
/// the combinations are tried in order; parseResourceGraph rejects a wrong one
/// rather than returning a graph with a corrupt first record, so a wrong guess
/// costs a retry rather than bad data.
std::vector<std::string> rootSpellings(std::string_view normalised) {
    static const std::map<std::string, std::vector<std::string>> kTokens = {
        {"win", {"Win"}},   {"winprism", {"WinPrism"}}, {"spwin", {"SPWin"}},
        {"eext", {"EExt"}}, {"trg", {"trg"}},
    };
    // A token we have not pinned down is short — a region code — so every
    // casing of it is a handful of tries rather than a search.
    auto casingsOf = [](const std::string& token) {
        auto const it = kTokens.find(token);
        if (it != kTokens.end())
            return it->second;
        if (token.size() > 5)
            return std::vector<std::string>{token};
        std::vector<std::string> all;
        for (u32 mask = 0; mask < (1u << token.size()); ++mask) {
            std::string s = token;
            for (size_t i = 0; i < token.size(); ++i) {
                if (mask & (1u << i))
                    s[i] = char(std::toupper(s[i]));
            }
            all.push_back(std::move(s));
        }
        return all;
    };
    std::vector<std::string> out = {""};
    size_t start = 0;
    while (start <= normalised.size()) {
        auto const stop = normalised.find_first_of("_.", start);
        auto const end = stop == std::string_view::npos ? normalised.size() : stop;
        std::string const token(normalised.substr(start, end - start));

        auto const casings = casingsOf(token);
        std::vector<std::string> next;
        for (auto const& prefix : out) {
            for (auto const& casing : casings)
                next.push_back(prefix + casing);
        }
        out = std::move(next);

        if (stop == std::string_view::npos)
            break;
        for (auto& s : out)
            s += normalised[stop];
        start = stop + 1;
    }
    return out;
}

} // namespace

TEST_CASE("OW asset type table matches file contents", "[casc][overwatch][.survey]") {
    const char* installPath = std::getenv("WHITEOUT_OW_PATH");
    if (installPath == nullptr)
        SKIP("set WHITEOUT_OW_PATH to an Overwatch install");

    utils::SimpleThreadPool pool(8);

    OpenOptions opts;
    opts.path = installPath;
    opts.product = "pro";
    opts.localeMask = LocaleMasks::enUS;
    opts.pool = &pool;

    std::string error;
    opts.errorOut = &error;

    auto storage = Storage::open(opts);
    if (!storage) {
        WARN("open failed: " << error);
        SKIP("could not open the install");
    }

    if (auto prod = storage->product())
        std::cout << "Overwatch " << prod->version << "\n";

    std::map<u32, Bucket> buckets;
    size_t total = 0, manifestRows = 0, rejected = 0;

    storage->enumerate([&](const EnumerateEntry& e) {
        ++total;
        u64 guid = 0;
        if (e.path.empty() || !trailingGuid(e.path, guid)) {
            ++manifestRows;
            return true;
        }
        u32 const typeId = ow::assetTypeId(guid);
        if (typeId == 0)
            ++rejected;
        auto& bucket = buckets[typeId];
        ++bucket.total;
        if (bucket.samples.size() < kSamplesPerType)
            bucket.samples.emplace_back(e.path);
        return true;
    });

    REQUIRE(total > 0);
    size_t const assets = total - manifestRows;
    std::cout << assets << " assets, " << manifestRows << " manifest rows, " << buckets.size()
              << " type ids, " << rejected << " the client would reject\n\n";

    std::vector<std::pair<u32, size_t>> byCount;
    for (auto const& [typeId, bucket] : buckets)
        byCount.emplace_back(typeId, bucket.total);
    std::sort(byCount.begin(), byCount.end(),
              [](auto const& a, auto const& b) { return a.second > b.second; });

    size_t namedAssets = 0;
    std::cout << "// type   count      name                    content\n";

    for (auto const& [typeId, count] : byCount) {
        std::string_view const name = ow::assetTypeName(typeId);
        if (!name.empty())
            namedAssets += count;

        std::set<std::string> seen;
        for (auto const& path : buckets[typeId].samples) {
            if (auto data = storage->readFile(path))
                seen.insert(signatureOf(*data));
        }
        seen.erase("");

        std::string content;
        for (auto const& s : seen)
            content += (content.empty() ? "" : "/") + s;

        std::cout << "    0x" << std::hex << std::setw(3) << std::setfill('0') << typeId << std::dec
                  << " " << std::setw(9) << std::setfill(' ') << count << "  " << std::setw(23)
                  << std::left << (name.empty() ? "?" : std::string(name)) << std::right << " "
                  << (content.empty() ? "?" : content) << "\n";

        // The names come from the client, so a container that flatly
        // contradicts one means the GUID-to-type-id reversal is wrong, not that
        // a name needs tightening. Only the unambiguous containers are checked.
        for (auto const& sig : seen) {
            INFO("type 0x" << std::hex << typeId << std::dec << " named '" << name << "' holds a "
                           << sig << " container");
            if (sig == "wem")
                CHECK(name.find("wemfile") != std::string_view::npos);
            else if (sig == "bnk")
                CHECK(name == "soundbank");
            else if (sig == "modl")
                CHECK((name == "model" || name == "skeleton" || name == "modelvertexdata"));
            else if (sig == "frac")
                CHECK(name == "fracturable");
            else if (sig == "mssd")
                CHECK(name == "mapshadowdata");
        }
    }

    std::cout << "\n"
              << namedAssets << " of " << assets << " assets carry a name ("
              << (100.0 * double(namedAssets) / double(assets)) << "%)\n";

    // Four GUIDs in the whole install are malformed; anything more means the
    // reversal is dropping real assets on the floor.
    CHECK(rejected < 100);
}

// Diablo IV ships CoreTOC.dat, an in-archive table naming every SNO type, which
// is why its root needs no guesswork. If Overwatch has an equivalent it would
// be a rare asset — a registry is one file, not a million — and it would carry
// the names as readable strings. This looks for one.
TEST_CASE("OW type registry hunt", "[casc][overwatch][.survey]") {
    const char* installPath = std::getenv("WHITEOUT_OW_PATH");
    if (installPath == nullptr)
        SKIP("set WHITEOUT_OW_PATH to an Overwatch install");

    utils::SimpleThreadPool pool(8);
    OpenOptions opts;
    opts.path = installPath;
    opts.product = "pro";
    opts.localeMask = LocaleMasks::enUS;
    opts.pool = &pool;

    auto storage = Storage::open(opts);
    if (!storage)
        SKIP("could not open the install");

    // Anything the root already names, rather than synthesises from a GUID.
    std::cout << "named (non-GUID) entries, by extension:\n";
    size_t named = 0;
    std::map<std::string, size_t> byExtension;
    storage->enumerate([&](const EnumerateEntry& e) {
        u64 guid = 0;
        if (e.path.empty() || trailingGuid(e.path, guid))
            return true;
        ++named;
        auto const dot = e.path.find_last_of('.');
        byExtension[dot == std::string::npos ? "<none>" : std::string(e.path.substr(dot))]++;
        return true;
    });
    for (auto const& [ext, count] : byExtension)
        std::cout << "  " << std::setw(8) << count << "  " << ext << "\n";
    std::cout << "  (" << named << " total)\n\n";

    // Rare types first: a registry would not be mass-produced.
    std::map<u64, std::vector<std::string>> rare;
    std::map<u64, size_t> counts;
    storage->enumerate([&](const EnumerateEntry& e) {
        u64 guid = 0;
        if (e.path.empty() || !trailingGuid(e.path, guid))
            return true;
        auto const type = typeOf(guid);
        ++counts[type];
        if (rare[type].size() < 2)
            rare[type].emplace_back(e.path);
        return true;
    });

    std::cout << "printable strings in rare types:\n";
    for (auto const& [type, paths] : rare) {
        if (counts[type] > 500)
            continue;
        for (auto const& path : paths) {
            auto data = storage->readFile(path);
            if (!data || data->empty())
                continue;

            std::vector<std::string> found;
            std::string run;
            for (u8 b : *data) {
                if (b >= 0x20 && b < 0x7F) {
                    run += char(b);
                    continue;
                }
                if (run.size() >= 6)
                    found.push_back(run);
                run.clear();
            }
            if (run.size() >= 6)
                found.push_back(run);
            if (found.empty())
                continue;

            std::cout << "  " << std::hex << type << std::dec << " (" << counts[type] << " assets, "
                      << data->size() << " bytes): ";
            for (size_t i = 0; i < found.size() && i < 6; ++i)
                std::cout << "\"" << found[i].substr(0, 40) << "\" ";
            std::cout << (found.size() > 6 ? "..." : "") << "\n";
            break;
        }
    }
    SUCCEED("hunt complete");
}

// Type 708 is six assets of ~20 MB carrying readable names; type 790 carries
// map paths. If Overwatch has an in-archive equivalent of Diablo IV's CoreTOC
// this is where it is, so dump enough to tell what these actually index.
TEST_CASE("OW string asset dump", "[casc][overwatch][.survey]") {
    const char* installPath = std::getenv("WHITEOUT_OW_PATH");
    if (installPath == nullptr)
        SKIP("set WHITEOUT_OW_PATH to an Overwatch install");
    const char* wantType = std::getenv("WHITEOUT_OW_TYPE");
    u64 const target = wantType ? std::strtoull(wantType, nullptr, 16) : 0x708;

    utils::SimpleThreadPool pool(8);
    OpenOptions opts;
    opts.path = installPath;
    opts.product = "pro";
    opts.localeMask = LocaleMasks::enUS;
    opts.pool = &pool;

    auto storage = Storage::open(opts);
    if (!storage)
        SKIP("could not open the install");

    std::vector<std::string> paths;
    storage->enumerate([&](const EnumerateEntry& e) {
        u64 guid = 0;
        if (e.path.empty() || !trailingGuid(e.path, guid))
            return true;
        if (typeOf(guid) == target && paths.size() < 3)
            paths.emplace_back(e.path);
        return true;
    });

    for (auto const& path : paths) {
        auto data = storage->readFile(path);
        if (!data)
            continue;
        std::cout << "\n=== " << path << "  (" << data->size() << " bytes)\n";

        std::vector<std::string> found;
        std::string run;
        for (u8 b : *data) {
            if (b >= 0x20 && b < 0x7F) {
                run += char(b);
                continue;
            }
            if (run.size() >= 5)
                found.push_back(run);
            run.clear();
        }
        if (run.size() >= 5)
            found.push_back(run);

        std::cout << found.size() << " strings; first 120:\n";
        for (size_t i = 0; i < found.size() && i < 120; ++i)
            std::cout << "  " << found[i].substr(0, 90) << "\n";
    }
    SUCCEED("dump complete");
}

TEST_CASE("OW resource graphs parse", "[casc][overwatch][.survey]") {
    const char* installPath = std::getenv("WHITEOUT_OW_PATH");
    if (installPath == nullptr)
        SKIP("set WHITEOUT_OW_PATH to an Overwatch install");

    utils::SimpleThreadPool pool(8);
    OpenOptions opts;
    opts.path = installPath;
    opts.product = "pro";
    opts.localeMask = LocaleMasks::enUS;
    opts.pool = &pool;

    auto storage = Storage::open(opts);
    if (!storage)
        SKIP("could not open the install");

    std::vector<std::string> paths;
    storage->enumerate([&](const EnumerateEntry& e) {
        if (e.path.size() > 4 && e.path.substr(e.path.size() - 4) == ".trg")
            paths.emplace_back(e.path);
        return true;
    });
    REQUIRE_FALSE(paths.empty());
    std::cout << paths.size() << " resource graphs\n";
    size_t parsed = 0, unrecovered = 0;

    for (auto const& path : paths) {
        auto data = storage->readFile(path);
        REQUIRE(data);

        auto const slash = path.find_last_of("\\/");
        std::string const leaf = slash == std::string::npos ? path : path.substr(slash + 1);

        std::optional<ow::ResourceGraph> graph;
        std::string name;
        for (auto const& candidate : rootSpellings(leaf)) {
            auto attempt = *data;
            graph = ow::parseResourceGraph(attempt, candidate);
            if (graph) {
                name = candidate;
                break;
            }
        }
        if (!graph) {
            // Only the IV depends on the name, so this is the survey failing to
            // guess the root's casing, not the parser failing to read a graph.
            // Its plaintext WinPrism_ twin covers the same content.
            std::cout << "\n  " << leaf << "  [encrypted, root spelling not recovered]\n";
            ++unrecovered;
            continue;
        }

        ++parsed;
        auto const& h = graph->header;
        size_t substitutions = 0, largest = 0;
        for (auto const& skin : graph->skins) {
            substitutions += skin.entries.size();
            largest = std::max(largest, skin.entries.size());
        }

        std::cout << "\n  " << name << (h.encrypted ? "  [encrypted]" : "  [plain]") << "\n"
                  << "    build " << h.buildVersion << ", " << h.packageCount << " packages, "
                  << h.skinCount << " skins, " << substitutions << " substitutions"
                  << " (largest skin " << largest << ")\n";

        CHECK(graph->packages.size() == h.packageCount);
        CHECK(graph->skins.size() == h.skinCount);
        CHECK(substitutions > 0);

        // Every GUID a graph mentions has to be one the client would accept.
        // A wrong key survives the block walk far less often than it survives
        // a single record, but this is what actually proves it decrypted.
        size_t checkedGuids = 0, badType = 0;
        for (size_t i = 0; i < graph->packages.size(); ++i) {
            ++checkedGuids;
            if (ow::assetTypeId(graph->packages[i].guid) == 0) {
                ++badType;
                if (badType <= 3)
                    std::cout << "    bad package[" << i << "] guid=" << std::hex << std::setw(16)
                              << std::setfill('0') << graph->packages[i].guid << std::dec
                              << std::setfill(' ') << "\n";
            }
        }
        for (size_t s = 0; s < graph->skins.size(); ++s) {
            auto const& skin = graph->skins[s];
            auto report = [&](const char* what, u64 guid) {
                ++badType;
                if (badType <= 3)
                    std::cout << "    bad " << what << " in skin[" << s << "] guid=" << std::hex
                              << std::setw(16) << std::setfill('0') << guid << std::dec
                              << std::setfill(' ') << " field=0x" << std::hex
                              << ow::assetTypeField(guid) << std::dec << "\n";
            };
            ++checkedGuids;
            if (ow::assetTypeId(skin.guid) == 0)
                report("skin", skin.guid);
            for (auto const& e : skin.entries) {
                checkedGuids += 2;
                if (ow::assetTypeId(e.source) == 0)
                    report("source", e.source);
                if (ow::assetTypeId(e.target) == 0)
                    report("target", e.target);
            }
        }
        std::cout << "    " << checkedGuids << " GUIDs, " << badType << " with no type id\n";
        CHECK(badType == 0);

        // Most substitutions keep the type. The ones that do not are worth
        // seeing rather than asserting away, so report which pairs they are.
        std::map<std::string, size_t> crossType;
        size_t typeChanges = 0;
        for (auto const& skin : graph->skins) {
            for (auto const& e : skin.entries) {
                auto const from = ow::assetTypeId(e.source), to = ow::assetTypeId(e.target);
                if (from == to)
                    continue;
                ++typeChanges;
                auto nameOf = [](u32 id) {
                    auto n = ow::assetTypeName(id);
                    return n.empty() ? std::string("?") : std::string(n);
                };
                crossType[nameOf(from) + "->" + nameOf(to)]++;
            }
        }
        std::cout << "    " << typeChanges << " of " << substitutions
                  << " substitutions change asset type:";
        for (auto const& [pair, count] : crossType)
            std::cout << " " << pair << "=" << count;
        std::cout << "\n";

        // What a skin is made of, as a sanity read.
        std::map<std::string, size_t> byType;
        for (auto const& skin : graph->skins) {
            for (auto const& e : skin.entries) {
                auto name = ow::assetTypeName(ow::assetTypeId(e.source));
                byType[name.empty() ? "?" : std::string(name)]++;
            }
        }
        std::cout << "    substituted asset types:";
        for (auto const& [name, count] : byType)
            std::cout << " " << name << "=" << count;
        std::cout << "\n";
    }
    std::cout << "\n"
              << parsed << " graphs parsed, " << unrecovered
              << " encrypted graphs whose root spelling the survey could not guess\n";
    CHECK(parsed >= 4);
    SUCCEED("graphs parsed");
}

// How far real per-asset names could go, measured rather than guessed.
//
// Some assets carry the path they were authored under. Whether that is a
// foundation for naming the archive or a curiosity depends on how many do, and
// on whether the path can be told apart from the other strings beside it, so
// this samples every type and counts.
TEST_CASE("OW embedded authoring paths", "[casc][overwatch][.survey]") {
    const char* installPath = std::getenv("WHITEOUT_OW_PATH");
    if (installPath == nullptr)
        SKIP("set WHITEOUT_OW_PATH to an Overwatch install");

    utils::SimpleThreadPool pool(8);
    OpenOptions opts;
    opts.path = installPath;
    opts.product = "pro";
    opts.localeMask = LocaleMasks::enUS;
    opts.pool = &pool;

    auto storage = Storage::open(opts);
    if (!storage)
        SKIP("could not open the install");

    constexpr size_t kSample = 12;
    std::map<u32, Bucket> buckets;
    size_t assets = 0;
    storage->enumerate([&](const EnumerateEntry& e) {
        u64 guid = 0;
        if (e.path.empty() || !trailingGuid(e.path, guid))
            return true;
        ++assets;
        auto& b = buckets[ow::assetTypeId(guid)];
        ++b.total;
        if (b.samples.size() < kSample)
            b.samples.emplace_back(e.path);
        return true;
    });

    // An authoring path, not just any printable run containing a backslash.
    // Compressed audio is full of those by chance, and counting them says 6% of
    // the archive is named when almost none of it is. Every segment has to read
    // like something a person typed.
    auto looksLikeSegment = [](std::string_view s) {
        if (s.size() < 2)
            return false;
        size_t letters = 0;
        for (char c : s) {
            bool const ok = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' ||
                            c == '-' || c == '.' || c == '\'';
            if (!ok)
                return false;
            if (std::isalpha(static_cast<unsigned char>(c)))
                ++letters;
        }
        return letters * 2 >= s.size();
    };
    auto authoringPaths = [&](std::span<const u8> data) {
        std::vector<std::string> found;
        std::string run;
        auto flush = [&] {
            std::string_view v(run);
            if (v.size() > 2 && v[1] == ':' && v[2] == '\\')
                v.remove_prefix(3); // a drive letter is a real root, not a segment
            size_t segments = 0;
            bool ok = v.size() >= 8;
            size_t start = 0;
            while (ok && start <= v.size()) {
                auto const stop = v.find('\\', start);
                auto const end = stop == std::string_view::npos ? v.size() : stop;
                ok = looksLikeSegment(v.substr(start, end - start));
                ++segments;
                if (stop == std::string_view::npos)
                    break;
                start = stop + 1;
            }
            if (ok && segments >= 2)
                found.push_back(run);
            run.clear();
        };
        for (u8 c : data) {
            if (c >= 0x20 && c <= 0x7E)
                run += char(c);
            else
                flush();
        }
        flush();
        return found;
    };

    struct Row {
        size_t sampled = 0, withPath = 0, assetsInType = 0;
        std::string example;
    };
    std::map<std::string, Row> byName;
    size_t sampledAssets = 0, coveredTypes = 0, coveredAssets = 0;

    for (auto const& [typeId, bucket] : buckets) {
        auto name = ow::assetTypeName(typeId);
        Row row;
        row.assetsInType = bucket.total;
        for (auto const& path : bucket.samples) {
            auto data = storage->readFile(path);
            if (!data)
                continue;
            ++row.sampled;
            ++sampledAssets;
            auto paths = authoringPaths(*data);
            if (!paths.empty()) {
                ++row.withPath;
                if (row.example.empty())
                    row.example = paths.front().substr(0, 70);
            }
        }
        if (row.sampled == 0)
            continue;
        if (row.withPath == row.sampled) {
            ++coveredTypes;
            coveredAssets += bucket.total;
        }
        std::string const key = (name.empty() ? "?" : std::string(name)) + " 0x" + [&] {
            char buf[8];
            std::snprintf(buf, sizeof buf, "%03X", typeId);
            return std::string(buf);
        }();
        byName[key] = row;
    }

    std::cout << "sampled " << sampledAssets << " assets across " << buckets.size() << " types\n\n";
    std::cout << "types where every sample carried an authoring path:\n";
    for (auto const& [key, row] : byName) {
        if (row.withPath != row.sampled)
            continue;
        std::cout << "  " << std::setw(28) << std::left << key << std::right << std::setw(9)
                  << row.assetsInType << "  " << row.example << "\n";
    }

    std::cout << "\ntypes where only some samples carried one:\n";
    for (auto const& [key, row] : byName) {
        if (row.withPath == row.sampled || row.withPath == 0)
            continue;
        std::cout << "  " << std::setw(28) << std::left << key << std::right << std::setw(9)
                  << row.assetsInType << "  " << row.withPath << "/" << row.sampled << "  "
                  << row.example << "\n";
    }

    std::cout << "\n"
              << coveredTypes << " of " << buckets.size() << " types always carry a path, covering "
              << coveredAssets << " of " << assets << " assets ("
              << (100.0 * double(coveredAssets) / double(assets)) << "%)\n";
    SUCCEED("measured");
}
