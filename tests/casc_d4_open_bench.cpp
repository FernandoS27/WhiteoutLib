// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Diablo IV root-open benchmark.
///
/// Almost all of a D4 open is the root step, and that step is two layers: a
/// TVFS traversal that produces a few million entries with paths, and D4Root
/// on top of it, which rewrites every SNO path from CoreTOC, synthesises the
/// combined-meta entries, and indexes the result. Timing `Storage::open`
/// cannot tell those apart.
///
/// This runs each layer separately and re-runs several of them in variant
/// form — the TVFS parse with and without its path index, the CoreTOC through
/// its reader and straight off the bytes, the D4 index as an
/// unordered_multimap and as a hash-and-chain table. Several of the variants
/// are the designs that were measured and replaced; they stay so a later
/// change can be checked against what it was meant to beat.
///
/// Usage:
///   casc_d4_open_bench <d4_path> [options]
///
/// Options:
///   --reps N        Repetitions for the repeatable stages (default 3).
///   --threads N     Worker-pool threads (default hardware_concurrency).
///   --no-e2e        Skip the end-to-end Storage::open reference run.
///   --csv <path>    Write the stage table as CSV.

#ifdef _WIN32
#define NOMINMAX
#endif

#include <whiteout/sno/core_toc.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/job_group.h>
#include <whiteout/utils/simple_thread_pool.h>

#include "../src/whiteout/storages/casc/codec/blte.h"
#include "../src/whiteout/storages/casc/codec/crypto.h"
#include "../src/whiteout/storages/casc/roots/common/root_build_utils.h"
#include "../src/whiteout/storages/casc/roots/d4_root.h"
#include "../src/whiteout/storages/casc/roots/tvfs_root.h"
#include "../src/whiteout/storages/casc/storage/local_data_source.h"
#include "../src/whiteout/storages/casc/tables/config.h"
#include "../src/whiteout/storages/casc/tables/encoding.h"
#include "../src/whiteout/storages/casc/tables/index.h"
#include "../src/whiteout/storages/common/mapped_file.h"
#include "../src/whiteout/storages/common/string_utils.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// clang-format off
#include <psapi.h>
// clang-format on
#pragma comment(lib, "psapi.lib")
#endif

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double peakRssMb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return double(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
#endif
    return 0.0;
}

struct Row {
    std::string name;
    double ms = 0;
    u64 bytes = 0;
    u64 items = 0;
    std::string note;
};

class Table {
public:
    void add(std::string name, double ms, u64 bytes = 0, u64 items = 0, std::string note = {}) {
        m_rows.push_back({std::move(name), ms, bytes, items, std::move(note)});
    }

    void print(const std::string& title) const {
        std::cout << "\n" << title << "\n" << std::string(110, '-') << "\n";
        std::cout << std::left << std::setw(46) << "stage" << std::right << std::setw(11) << "ms"
                  << std::setw(10) << "MB" << std::setw(14) << "items" << "  " << std::left
                  << "note" << "\n";
        std::cout << std::string(110, '-') << "\n";
        for (auto& r : m_rows) {
            std::cout << std::left << std::setw(46) << r.name << std::right << std::setw(11)
                      << std::fixed << std::setprecision(1) << r.ms << std::setw(10)
                      << std::setprecision(1) << (double(r.bytes) / (1024.0 * 1024.0))
                      << std::setw(14) << r.items << "  " << std::left << r.note << "\n";
        }
        std::cout << std::string(110, '-') << "\n";
    }

    void writeCsv(const std::string& path) const {
        std::ofstream f(path);
        f << "stage,ms,bytes,items,note\n";
        for (auto& r : m_rows)
            f << '"' << r.name << "\"," << r.ms << ',' << r.bytes << ',' << r.items << ",\""
              << r.note << "\"\n";
    }

private:
    std::vector<Row> m_rows;
};

/// Best of @p reps runs of @p fn, so a stray scheduling hiccup does not become
/// the headline number.
template <typename Fn>
double bestOf(int reps, Fn&& fn) {
    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        auto t = Clock::now();
        fn();
        best = std::min(best, msSince(t));
    }
    return best;
}

struct Paths {
    std::string base, data;
};

Paths resolvePaths(const std::string& in) {
    Paths p;
    p.base = in;
    while (!p.base.empty() && (p.base.back() == '/' || p.base.back() == '\\'))
        p.base.pop_back();
    if (fs::exists(p.base + "/Data"))
        p.data = p.base + "/Data";
    else if (fs::exists(p.base + "/data"))
        p.data = p.base + "/data";
    else
        p.data = p.base;
    return p;
}

std::string configPath(const std::string& dataPath, const std::array<u8, 16>& key) {
    static const char hex[] = "0123456789abcdef";
    std::string h;
    for (u8 b : key) {
        h += hex[b >> 4];
        h += hex[b & 0xF];
    }
    return dataPath + "/config/" + h.substr(0, 2) + "/" + h.substr(2, 2) + "/" + h;
}

// ---------------------------------------------------------------------------
// D4 path classification, duplicated from d4_root.cpp
//
// The parser keeps these in an anonymous namespace. What follows is enough to
// re-run the enrichment pass under variants; it is not a second implementation
// of the root.
// ---------------------------------------------------------------------------

constexpr std::string_view kSubFolders[] = {"child", "meta", "payload", "paylow", "paymed"};

bool tryParseInt(std::string_view sv, i32& out) {
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc{} && ptr == sv.data() + sv.size();
}

struct ParsedSnoPath {
    std::string_view folder;
    std::string_view subfolder;
    i32 snoId = 0;
    i32 subId = -1;
    bool valid = false;
};

ParsedSnoPath classifySnoPath(std::string_view path) {
    ParsedSnoPath r{};
    auto colon = path.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return r;
    r.folder = path.substr(0, colon);
    auto rest = path.substr(colon + 1);
    auto bs = rest.find('\\');
    if (bs == std::string_view::npos || bs == 0)
        return r;
    auto sub = rest.substr(0, bs);
    auto stem = rest.substr(bs + 1);
    if (stem.empty())
        return r;
    bool known = false;
    for (auto sf : kSubFolders)
        if (sub == sf) {
            known = true;
            break;
        }
    if (!known)
        return r;
    auto dash = stem.find('-');
    if (dash == std::string_view::npos) {
        if (!tryParseInt(stem, r.snoId))
            return r;
    } else {
        if (!tryParseInt(stem.substr(0, dash), r.snoId))
            return r;
        if (!tryParseInt(stem.substr(dash + 1), r.subId))
            return r;
    }
    r.subfolder = sub;
    r.valid = true;
    return r;
}

std::string buildEnrichedPath(std::string_view folder, std::string_view subfolder,
                              const sno::TocEntry& toc, i32 subId) {
    const char* ext = sno::snoGroupExtension(toc.group);
    const char* dir = sno::snoGroupDir(toc.group);
    std::string p;
    p.reserve(folder.size() + subfolder.size() + toc.name.size() + 24);
    p.append(folder);
    p.push_back(':');
    p.append(subfolder);
    p.push_back('\\');
    p.append(dir ? dir : "Unknown");
    p.push_back('\\');
    p.append(toc.name);
    if (subId >= 0) {
        p.push_back('-');
        char buf[16];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), subId);
        p.append(buf, size_t(ptr - buf));
    }
    if (ext && ext[0]) {
        p.push_back('.');
        p.append(ext);
    }
    return p;
}

/// Run @p fn over 0..@p count on the pool, chunked the way the roots do.
template <typename Fn>
void runChunked(size_t count, interfaces::WorkerPool& pool, Fn&& fn) {
    size_t const threads = std::max<size_t>(pool.threadCount(), 1);
    size_t const chunk = std::max<size_t>((count + threads - 1) / threads, 1);
    size_t const chunks = (count + chunk - 1) / chunk;
    utils::JobGroup group;
    group.add(chunks);
    for (size_t c = 0; c < chunks; ++c) {
        interfaces::WorkerTask task;
        task.fn = [&, c]() {
            size_t const begin = c * chunk;
            size_t const end = std::min(begin + chunk, count);
            for (size_t i = begin; i < end; ++i)
                fn(i);
            group.done();
        };
        pool.submit(task);
    }
    group.wait();
}

/// The D4 "new" CoreTOC entry loop, read straight out of the mapped bytes.
///
/// `CoreToc::parse` walks the same layout through a BinaryReader over an
/// istream — two seeks and a byte-at-a-time name read per entry. This is the
/// same work with memcpy and a NUL scan, so the gap between the two is the
/// headroom in the reader rather than in the format.
struct FlatToc {
    std::vector<i32> snoIds;
    std::vector<u32> nameOffsets; ///< Byte offset of the name inside the blob.
    std::vector<i32> groups;

    bool parse(std::span<const u8> data) {
        auto rd32 = [&](size_t off) {
            u32 v = 0;
            std::memcpy(&v, data.data() + off, 4);
            return v;
        };
        if (data.size() < 8 || rd32(0) != 0xBCDE6611u)
            return false;
        u32 const groupCount = rd32(4);
        if (groupCount == 0 || groupCount > 1024)
            return false;
        size_t const headerSize = 8 + size_t(groupCount) * 4 * 4;
        if (data.size() < headerSize + 4)
            return false;

        std::vector<u32> counts(groupCount), offsets(groupCount);
        for (u32 c = 0; c < groupCount; ++c)
            counts[c] = rd32(8 + size_t(c) * 4);
        for (u32 c = 0; c < groupCount; ++c)
            offsets[c] = rd32(8 + size_t(groupCount + c) * 4);

        size_t total = 0;
        for (u32 c = 0; c < groupCount; ++c)
            total += counts[c];
        snoIds.resize(total);
        nameOffsets.resize(total);
        groups.resize(total);

        size_t const dataStart = headerSize + 4;
        size_t at = 0;
        for (u32 c = 0; c < groupCount; ++c) {
            if (counts[c] == 0)
                continue;
            size_t const groupStart = dataStart + offsets[c];
            size_t const tableSize = size_t(counts[c]) * 12;
            if (groupStart + tableSize > data.size())
                return false;
            size_t const nameBase = groupStart + tableSize;
            for (u32 i = 0; i < counts[c]; ++i) {
                size_t const rec = groupStart + size_t(i) * 12;
                groups[at] = i32(rd32(rec));
                snoIds[at] = i32(rd32(rec + 4));
                nameOffsets[at] = u32(nameBase + rd32(rec + 8));
                ++at;
            }
        }
        return true;
    }
};

u64 fnv1a(std::string_view s) {
    u64 h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string d4Path, csvPath;
    int reps = 3;
    int threads = int(std::thread::hardware_concurrency());
    bool doE2E = true;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--reps" && i + 1 < argc)
            reps = std::atoi(argv[++i]);
        else if (a == "--threads" && i + 1 < argc)
            threads = std::atoi(argv[++i]);
        else if (a == "--csv" && i + 1 < argc)
            csvPath = argv[++i];
        else if (a == "--no-e2e")
            doE2E = false;
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0] << " <d4_path> [--reps N] [--threads N] [--no-e2e]"
                      << " [--csv <path>]\n";
            return 0;
        } else if (d4Path.empty())
            d4Path = a;
    }

    if (d4Path.empty() || !fs::exists(d4Path)) {
        std::cerr << "Error: pass a valid Diablo IV install path.\n";
        return 1;
    }
    if (threads < 1)
        threads = 1;

    auto paths = resolvePaths(d4Path);
    std::cout << "Install : " << paths.base << "\nData    : " << paths.data
              << "\nThreads : " << threads << "\nReps    : " << reps << "\n";

    utils::SimpleThreadPool pool{size_t(threads)};
    Table table;

    // --- End-to-end reference, with the per-step split the open reports. ---
    if (doE2E) {
        OpenOptions o;
        o.path = d4Path;
        o.pool = &pool;
        auto stepClock = std::make_shared<Clock::time_point>(Clock::now());
        std::cout << "\nStorage::open step trace\n" << std::string(56, '-') << "\n";
        o.progressCallback = [stepClock](const ProgressInfo& info) {
            if (info.state != ProgressState::End)
                return true;
            std::cout << "  " << std::left << std::setw(24) << progressStepName(info.step)
                      << std::right << std::setw(10) << std::fixed << std::setprecision(1)
                      << msSince(*stepClock) << " ms";
            if (info.total > 0)
                std::cout << "  (" << info.current << ")";
            std::cout << "\n";
            *stepClock = Clock::now();
            return true;
        };
        auto t = Clock::now();
        auto s = Storage::open(o);
        double const ms = msSince(t);
        std::cout << std::string(56, '-') << "\n  total " << std::fixed << std::setprecision(1)
                  << ms << " ms, peak RSS " << std::setprecision(0) << peakRssMb() << " MB\n";
        if (!s) {
            std::cerr << "open failed\n";
            return 1;
        }
        table.add("Storage::open (end to end)", ms, 0, s->entryCount());

        u64 counted = 0, pathBytes = 0;
        double const walkMs = bestOf(reps, [&] {
            counted = 0;
            pathBytes = 0;
            s->enumerate([&](const EnumerateEntry& fe) {
                ++counted;
                pathBytes += fe.path.size();
                return true;
            });
        });
        table.add("W1. Storage::enumerate", walkMs, pathBytes, counted, "the walk");

        u64 listed = 0;
        double const listMs = bestOf(reps, [&] {
            auto files = s->listFiles();
            listed = files.size();
        });
        table.add("W2. Storage::listFiles", listMs, 0, listed, "same walk, keeps the strings");

        // Every path and every SNO id the root reports has to come back out of
        // the indices — a hash-and-chain index that silently loses a chain
        // would still open fast and enumerate correctly.
        // Sampled across the whole set rather than off the front, so an index
        // that loses its tail cannot pass.
        std::vector<std::string> paths;
        std::vector<i32> ids;
        u64 seen = 0;
        s->enumerate([&](const EnumerateEntry& fe) {
            if ((seen++ % 11) != 0)
                return true;
            if (!fe.path.empty())
                paths.emplace_back(fe.path);
            // -1 is RootEntry's "no id", which is not kInvalidId (0) — a D4
            // entry that is not an SNO carries it.
            if (fe.fileDataId != -1)
                ids.push_back(fe.fileDataId);
            return true;
        });

        u64 pathMisses = 0, idMisses = 0;
        auto t0 = Clock::now();
        for (auto& p : paths)
            if (!s->fileInfo(p))
                ++pathMisses;
        for (i32 id : ids)
            if (!s->fileInfo(id))
                ++idMisses;
        table.add("V1. lookup every sampled path/id", msSince(t0), 0, paths.size() + ids.size(),
                  pathMisses || idMisses
                      ? std::to_string(pathMisses) + " path + " + std::to_string(idMisses) +
                            " id MISSES"
                      : "all resolved");
        if (pathMisses || idMisses)
            std::cerr << "LOOKUP REGRESSION: " << pathMisses << " paths and " << idMisses
                      << " ids did not resolve\n";
    }

    // --- Rebuild the pieces the root needs, so the root can be timed alone. ---
    KeyRing keyRing;
    std::string const buildInfoPath = fs::exists(paths.base + "/.build.info")
                                          ? paths.base + "/.build.info"
                                          : paths.data + "/.build.info";
    auto buildInfoFile = storages::common::readFileFully(buildInfoPath);
    if (!buildInfoFile) {
        std::cerr << "cannot read " << buildInfoPath << "\n";
        return 1;
    }
    auto builds = parseBuildInfo(*buildInfoFile);
    const BuildInfo* active = nullptr;
    for (auto& b : builds)
        if (b.active) {
            active = &b;
            break;
        }
    if (!active && !builds.empty())
        active = &builds[0];
    if (!active) {
        std::cerr << "no build in .build.info\n";
        return 1;
    }

    auto t = Clock::now();
    auto indexTable = IndexTable::load(paths.data, &pool);
    table.add("setup: IndexTable::load", msSince(t), 0, indexTable.entryCount());

    auto buildConfigFile = storages::common::readFileFully(configPath(paths.data, active->buildKey));
    auto cdnConfigFile = storages::common::readFileFully(configPath(paths.data, active->cdnKey));
    if (!buildConfigFile) {
        std::cerr << "cannot read build config\n";
        return 1;
    }
    auto buildConfig = parseBuildConfig(*buildConfigFile);
    CdnConfig cdnConfig;
    if (cdnConfigFile)
        cdnConfig = parseCdnConfig(*cdnConfigFile);
    if (!cdnConfig.archiveEKeys.empty())
        indexTable.loadArchiveIndices(paths.data, cdnConfig.archiveEKeys, &pool);

    std::vector<storages::common::MappedFile> archives;
    {
        u32 maxIndex = 0;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(paths.data + "/data", ec)) {
            auto name = e.path().filename().string();
            if (name.size() >= 8 && name.compare(0, 5, "data.") == 0)
                maxIndex = std::max(maxIndex, u32(std::stoul(name.substr(5))));
        }
        archives.resize(maxIndex + 1);
        for (u32 i = 0; i <= maxIndex; ++i) {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "/data/data.%03u", i);
            std::string const n = paths.data + suffix;
            if (!fs::exists(n))
                continue;
            if (auto m =
                    storages::common::MappedFile::open(n, storages::common::AccessHint::Random))
                archives[i] = std::move(*m);
        }
    }
    LocalDataSource dataSource(&indexTable, &archives);

    t = Clock::now();
    auto encodingBlte = dataSource.fetchBlte(buildConfig.encodingEKey);
    auto encodingDecoded = blteDecode(encodingBlte, &keyRing, &pool);
    auto encodingTable = EncodingTable::parse(encodingDecoded.data, &pool);
    table.add("setup: encoding table", msSince(t), encodingDecoded.data.size(),
              encodingTable.entryCount());
    if (!encodingTable.isValid()) {
        std::cerr << "encoding table invalid\n";
        return 1;
    }

    auto resolveCKey = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        std::array<u8, 16> k{};
        std::memcpy(k.data(), cKey.data(), 16);
        auto* e = encodingTable.findByCKey(k);
        if (!e)
            return {};
        auto blte = dataSource.fetchBlte(e->eKey);
        if (blte.empty())
            return {};
        auto d = blteDecode(blte, &keyRing, nullptr);
        return d.success ? std::move(d.data) : std::vector<u8>{};
    };
    auto resolveEKey = [&](std::span<const u8, 16> eKey) -> std::vector<u8> {
        std::array<u8, 16> k{};
        std::memcpy(k.data(), eKey.data(), 16);
        auto blte = dataSource.fetchBlte(k);
        if (blte.empty())
            return {};
        auto d = blteDecode(blte, &keyRing, nullptr);
        return d.success ? std::move(d.data) : std::vector<u8>{};
    };

    if (isZeroKey(buildConfig.vfsRootCKey)) {
        std::cerr << "build config has no VFS root — not a D4-style install\n";
        return 1;
    }

    // --- 1. The VFS root blob and its sub-manifests. ---
    t = Clock::now();
    auto vfsData = resolveCKey(buildConfig.vfsRootCKey);
    table.add("1. vfs root: fetch + BLTE", msSince(t), vfsData.size());
    if (vfsData.empty()) {
        std::cerr << "cannot resolve VFS root\n";
        return 1;
    }

    std::vector<std::array<u8, 16>> vfsEKeys;
    for (auto& sub : buildConfig.vfsSubManifests) {
        if (sub.cKey == buildConfig.vfsRootCKey)
            continue;
        vfsEKeys.push_back(sub.eKey);
    }

    std::vector<std::vector<u8>> subBlobs(vfsEKeys.size());
    u64 subBytes = 0;
    double const subMs = bestOf(reps, [&] {
        runChunked(vfsEKeys.size(), pool, [&](size_t i) { subBlobs[i] = resolveEKey(vfsEKeys[i]); });
    });
    for (auto& b : subBlobs)
        subBytes += b.size();
    table.add("2. vfs sub-manifests (pooled)", subMs, subBytes, vfsEKeys.size());

    std::unordered_map<u64, size_t> subByHash;
    for (size_t i = 0; i < vfsEKeys.size(); ++i)
        subByHash[keyHash64(vfsEKeys[i].data())] = i;
    VfsResolver const vfsResolver = [&](std::span<const u8> eKey) -> std::span<const u8> {
        auto it = subByHash.find(keyHash64(eKey.data()));
        if (it == subByHash.end())
            return {};
        return subBlobs[it->second];
    };

    // --- 3. The TVFS traversal, with and without the path index it builds. ---
    size_t tvfsEntries = 0;
    u64 tvfsPathBytes = 0;
    double const tvfsIdxMs = bestOf(reps, [&] {
        auto r = TvfsRoot::parse(vfsData, vfsResolver, vfsEKeys, &pool, true);
        tvfsEntries = r ? r->entryCount() : 0;
    });
    table.add("3a. TvfsRoot::parse, buildIdx=true", tvfsIdxMs, 0, tvfsEntries,
              "an index D4Root does not read");

    double const tvfsNoIdxMs = bestOf(reps, [&] {
        auto r = TvfsRoot::parse(vfsData, vfsResolver, vfsEKeys, &pool, false);
        tvfsEntries = r ? r->entryCount() : 0;
    });
    table.add("3b. TvfsRoot::parse, buildIdx=false", tvfsNoIdxMs, 0, tvfsEntries,
              "what D4 asks for");

    auto freshTvfs = [&]() {
        return TvfsRoot::parse(vfsData, vfsResolver, vfsEKeys, &pool, false);
    };

    // --- 4. D4Root::create on top of it. ---
    EKeyReader const eKeyReader = [&](std::span<const u8, 16> eKey) { return resolveEKey(eKey); };
    size_t d4Entries = 0;
    double bestCreate = 1e30;
    for (int i = 0; i < reps; ++i) {
        auto tv = TvfsRoot::parse(vfsData, vfsResolver, vfsEKeys, &pool, true);
        auto c = Clock::now();
        auto d4 = D4Root::create(std::move(tv), eKeyReader, &pool);
        bestCreate = std::min(bestCreate, msSince(c));
        d4Entries = d4 ? d4->entryCount() : 0;
    }
    table.add("4. D4Root::create", bestCreate, 0, d4Entries, "on top of the TVFS root");

    // --- 5. The pieces inside create, re-run over the real entry set. ---
    auto tvfs = freshTvfs();
    if (!tvfs) {
        std::cerr << "TVFS parse failed\n";
        return 1;
    }
    tvfs->ensureIndexed(&pool);

    std::vector<const RootEntry*> ptrs;
    double const collectMs = bestOf(reps, [&] {
        ptrs.clear();
        ptrs.reserve(tvfs->entryCount());
        tvfs->enumerate([&](const RootEntry& e) {
            ptrs.push_back(&e);
            return true;
        });
    });
    for (auto* p : ptrs)
        tvfsPathBytes += p->path.size();
    table.add("5a. collect entry pointers", collectMs, tvfsPathBytes, ptrs.size(),
              "was the first half of copying them out");

    size_t const n = ptrs.size();
    double const resizeMs = bestOf(reps, [&] {
        std::vector<RootEntry> v;
        v.resize(n);
    });
    table.add("5b. vector<RootEntry>::resize", resizeMs, u64(n) * sizeof(RootEntry), n,
              "the second half; takeEntries() pays neither");

    auto tocResults = tvfs->findByNormalizedPath("base:coretoc.dat");
    if (tocResults.empty()) {
        std::cerr << "CoreTOC.dat not found in TVFS tree\n";
        return 1;
    }
    sno::CoreToc coreToc;
    u64 tocBytes = 0;
    double const tocMs = bestOf(reps, [&] {
        auto data = eKeyReader(tocResults[0]->eKey);
        tocBytes = data.size();
        sno::CoreToc c;
        c.parse(data);
        coreToc = std::move(c);
    });
    table.add("5c. CoreTOC fetch + parse", tocMs, tocBytes, coreToc.size());

    auto tocData = eKeyReader(tocResults[0]->eKey);
    double const tocParseMs = bestOf(reps, [&] {
        sno::CoreToc c;
        c.parse(tocData);
    });
    table.add("5c1. CoreTOC parse alone", tocParseMs, tocData.size(), coreToc.size(),
              "entries, names and id index");

    FlatToc flat;
    double const flatTocMs = bestOf(reps, [&] {
        FlatToc f;
        f.parse(tocData);
        flat = std::move(f);
    });
    table.add("5c2. CoreTOC parse, flat reader", flatTocMs, tocData.size(), flat.snoIds.size(),
              "the fields alone — the floor");

    double const tocIdxMs = bestOf(reps, [&] {
        std::unordered_map<i32, size_t> idx;
        for (size_t i = 0; i < flat.snoIds.size(); ++i)
            idx[flat.snoIds[i]] = i;
    });
    table.add("5c3. CoreTOC id index, unreserved", tocIdxMs, 0, flat.snoIds.size(),
              "the unordered_map it replaced");

    double const tocNamesMs = bestOf(reps, [&] {
        std::vector<std::string> names(flat.nameOffsets.size());
        runChunked(flat.nameOffsets.size(), pool, [&](size_t i) {
            const char* p = reinterpret_cast<const char*>(tocData.data()) + flat.nameOffsets[i];
            names[i].assign(p, std::strlen(p));
        });
    });
    table.add("5c4. CoreTOC names as std::string", tocNamesMs, 0, flat.nameOffsets.size(),
              "pooled");

    std::vector<RootEntry> enriched(n);
    size_t snoCount = 0, unknownSno = 0;
    double const enrichMs = bestOf(reps, [&] {
        runChunked(n, pool, [&](size_t i) {
            auto& dst = enriched[i];
            const auto& src = *ptrs[i];
            dst.cKey = src.cKey;
            dst.eKey = src.eKey;
            dst.fileDataId = src.fileDataId;
            dst.fileNameHash = src.fileNameHash;
            dst.localeFlags = src.localeFlags;
            dst.contentFlags = src.contentFlags;
            dst.fileSize = src.fileSize;
            if (src.path.empty())
                return;
            auto parsed = classifySnoPath(src.path);
            if (!parsed.valid) {
                dst.path = src.path;
                return;
            }
            dst.fileDataId = u32(parsed.snoId);
            const sno::TocEntry* toc = coreToc.findById(parsed.snoId);
            if (!toc || toc->name.empty()) {
                dst.path = src.path;
                return;
            }
            dst.path = buildEnrichedPath(parsed.folder, parsed.subfolder, *toc, parsed.subId);
        });
    });
    for (size_t i = 0; i < n; ++i) {
        auto p = classifySnoPath(ptrs[i]->path);
        if (p.valid) {
            ++snoCount;
            if (!coreToc.findById(p.snoId))
                ++unknownSno;
        }
    }
    u64 enrichedBytes = 0;
    for (auto& e : enriched)
        enrichedBytes += e.path.size();
    table.add("5d. enrich entries (pooled)", enrichMs, enrichedBytes, snoCount,
              "items = SNO paths rewritten");

    // --- 5e. The combined meta containers D4Root reads to synthesize entries. ---
    // Picked by the same shape D4Root looks for: a ".dat" whose stem is
    // Group-Category-Language and whose last segment is not an encryption key.
    std::vector<std::array<u8, 16>> metaKeys;
    for (auto& e : enriched) {
        if (e.path.size() < 5 || e.path.compare(e.path.size() - 4, 4, ".dat") != 0)
            continue;
        auto sep = e.path.find_last_of("\\/:");
        std::string_view stem(e.path);
        if (sep != std::string_view::npos)
            stem = stem.substr(sep + 1);
        stem = stem.substr(0, stem.size() - 4);
        auto d1 = stem.find('-');
        if (d1 == std::string_view::npos || stem.find('-', d1 + 1) == std::string_view::npos)
            continue;
        auto tail = stem.substr(stem.rfind('-') + 1);
        if (tail.size() > 2 && tail[0] == '0' && tail[1] == 'x')
            continue;
        if (e.eKey != std::array<u8, 16>{})
            metaKeys.push_back(e.eKey);
    }

    u64 metaBytes = 0;
    double const metaSerialMs = bestOf(reps, [&] {
        metaBytes = 0;
        for (auto& k : metaKeys)
            metaBytes += resolveEKey(k).size();
    });
    table.add("5e. combined meta fetch, serial", metaSerialMs, metaBytes, metaKeys.size(),
              "one at a time, as create() used to");

    double const metaPoolMs = bestOf(reps, [&] {
        std::vector<size_t> sizes(metaKeys.size());
        runChunked(metaKeys.size(), pool,
                   [&](size_t i) { sizes[i] = resolveEKey(metaKeys[i]).size(); });
    });
    table.add("5f. combined meta fetch, pooled", metaPoolMs, metaBytes, metaKeys.size(),
              "one task per container — what it does now");

    // --- 6. The index, as it is built today and as a flat alternative. ---
    std::vector<std::string> normalized;
    double const normMs = bestOf(reps, [&] { normalized = normalizeEntryPaths(enriched, &pool); });
    u64 normBytes = 0;
    for (auto& s : normalized)
        normBytes += s.capacity();
    table.add("6a. normalizeEntryPaths", normMs, normBytes, normalized.size(),
              "a second copy of every path, no longer built");

    double const mapMs = bestOf(reps, [&] {
        auto copy = normalized;
        std::unordered_multimap<std::string, size_t> byPath;
        byPath.reserve(copy.size());
        for (size_t i = 0; i < copy.size(); ++i)
            if (!copy[i].empty())
                byPath.emplace(std::move(copy[i]), i);
    });
    table.add("6b. unordered_multimap<string> fill", mapMs, 0, normalized.size(),
              "the path index it replaced");

    double const snoMapMs = bestOf(reps, [&] {
        std::unordered_multimap<u32, size_t> bySno;
        bySno.reserve(enriched.size());
        for (size_t i = 0; i < enriched.size(); ++i)
            if (enriched[i].fileDataId != kInvalidFileDataId)
                bySno.emplace(enriched[i].fileDataId, i);
    });
    table.add("6c. unordered_multimap<u32> fill", snoMapMs, 0, enriched.size(),
              "the snoId index it replaced");

    double const flatMs = bestOf(reps, [&] {
        std::vector<u64> hashes(normalized.size());
        runChunked(normalized.size(), pool,
                   [&](size_t i) { hashes[i] = normalized[i].empty() ? 0 : fnv1a(normalized[i]); });
        size_t cap = 1;
        while (cap < normalized.size() * 2)
            cap <<= 1;
        std::vector<u32> heads(cap, 0xFFFFFFFFu);
        std::vector<u32> next(normalized.size(), 0xFFFFFFFFu);
        for (size_t i = 0; i < normalized.size(); ++i) {
            if (!hashes[i])
                continue;
            size_t const slot = size_t(hashes[i]) & (cap - 1);
            next[i] = heads[slot];
            heads[slot] = u32(i);
        }
    });
    table.add("6d. hash + chain fill", flatMs, 0, normalized.size(),
              "both indices now — no per-entry allocation");

    // --- 7. What the entry set actually looks like. ---
    {
        std::unordered_set<std::string_view> distinctPaths;
        distinctPaths.reserve(normalized.size());
        size_t empties = 0;
        for (auto& s : normalized) {
            if (s.empty())
                ++empties;
            else
                distinctPaths.insert(s);
        }
        table.add("stat: distinct normalized paths", 0, 0, distinctPaths.size(),
                  std::to_string(empties) + " entries have no path");

        std::unordered_set<i32> distinctSno;
        for (auto& e : enriched)
            if (e.fileDataId != kInvalidFileDataId)
                distinctSno.insert(i32(e.fileDataId));
        table.add("stat: distinct snoIds", 0, 0, distinctSno.size(),
                  std::to_string(unknownSno) + " SNO paths missing from CoreTOC");

        table.add("stat: TVFS + D4 path bytes", 0, tvfsPathBytes + enrichedBytes, 0,
                  "the copy takeEntries() no longer makes");
    }

    table.print("Diablo IV root open");
    if (!csvPath.empty())
        table.writeCsv(csvPath);
    std::cout << "peak RSS " << std::fixed << std::setprecision(0) << peakRssMb() << " MB\n";
    return 0;
}
