// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Overwatch root-open benchmark.
///
/// Opening an Overwatch install spends nearly all of its time in one step, so
/// timing `Storage::open` says only that the root is slow. This takes the root
/// apart: fetching and BLTE-decoding the CMFs, decrypting them, turning hash
/// records into RootEntry, and building the GUID index.
///
/// Each stage is also re-run in variant form, so the price of a specific design
/// choice — a path string per asset, a stable sort over twenty-four million
/// pairs — is visible on its own rather than folded into a total.
///
/// Usage:
///   casc_ow_open_bench <ow_path> [options]
///
/// Options:
///   --reps N        Repetitions for the repeatable stages (default 3).
///   --threads N     Worker-pool threads (default hardware_concurrency).
///   --no-e2e        Skip the end-to-end Storage::open reference run.
///   --csv <path>    Write the stage table as CSV.

#ifdef _WIN32
#define NOMINMAX
#endif

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_thread_pool.h>

#include "../src/whiteout/storages/casc/codec/blte.h"
#include "../src/whiteout/storages/casc/codec/crypto.h"
#include "../src/whiteout/storages/casc/roots/ow/ow_asset_types.h"
#include "../src/whiteout/storages/casc/roots/ow/ow_manifest_crypto.h"
#include "../src/whiteout/storages/casc/roots/ow_root.h"
#include "../src/whiteout/storages/casc/storage/local_data_source.h"
#include "../src/whiteout/storages/casc/tables/config.h"
#include "../src/whiteout/storages/casc/tables/encoding.h"
#include "../src/whiteout/storages/casc/tables/index.h"
#include "../src/whiteout/storages/common/byte_order.h"
#include "../src/whiteout/storages/common/mapped_file.h"
#include "../src/whiteout/storages/common/string_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <execution>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
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
        std::cout << "\n" << title << "\n" << std::string(104, '-') << "\n";
        std::cout << std::left << std::setw(44) << "stage" << std::right << std::setw(11) << "ms"
                  << std::setw(10) << "MB" << std::setw(14) << "items" << "  " << std::left
                  << "note" << "\n";
        std::cout << std::string(104, '-') << "\n";
        for (auto& r : m_rows) {
            std::cout << std::left << std::setw(44) << r.name << std::right << std::setw(11)
                      << std::fixed << std::setprecision(1) << r.ms << std::setw(10)
                      << std::setprecision(1) << (double(r.bytes) / (1024.0 * 1024.0))
                      << std::setw(14) << r.items << "  " << std::left << r.note << "\n";
        }
        std::cout << std::string(104, '-') << "\n";
    }

    void writeCsv(const std::string& path) const {
        std::ofstream f(path);
        f << "stage,ms,bytes,items,note\n";
        for (auto& r : m_rows)
            f << '"' << r.name << "\"," << r.ms << ',' << r.bytes << ',' << r.items << ",\"" << r.note
              << "\"\n";
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
    else if (fs::exists(p.base + "/data/casc"))
        p.data = p.base + "/data/casc";
    else
        p.data = p.base;
    if (fs::exists(p.data + "/casc/data"))
        p.data = p.data + "/casc";
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
// CMF layout, duplicated from ow_root.cpp
//
// The parser keeps these in an anonymous namespace, and exporting them just to
// benchmark them would widen the interface for one caller. The three constants
// and the header read below are the whole of what is copied.
// ---------------------------------------------------------------------------

constexpr size_t kCmfEntrySize = 20;
constexpr size_t kHashDataOldSize = 28;
constexpr size_t kHashDataSize = 29;
constexpr u32 kBuildHashData135 = 57230;

size_t cmfHeaderSize(u32 build) {
    if (build > 68309)
        return 48;
    if (build > 47161)
        return 40;
    return 36;
}

struct CmfInfo {
    u32 build = 0;
    i32 dataCount = 0;
    i32 entryCount = 0;
    u32 magic = 0;
    bool encrypted = false;
    size_t headerSize = 0;
};

bool readCmfInfo(std::span<const u8> data, CmfInfo& out) {
    using storages::common::readLE32;
    if (data.size() < 36)
        return false;
    out.build = readLE32(data.data());
    if (out.build < 20000 || out.build >= 12923648)
        return false;
    out.headerSize = cmfHeaderSize(out.build);
    if (data.size() < out.headerSize)
        return false;
    const u8* tail = data.data() + out.headerSize;
    out.dataCount = i32(readLE32(tail - 16));
    out.entryCount = i32(readLE32(tail - 8));
    out.magic = readLE32(tail - 4);
    out.encrypted = ((out.magic >> 8) == 0x636D66);
    return true;
}

/// The path Overwatch assets get today: folder prefix, GUID in hex, type
/// extension. Reproduced here so the variants below differ only in how they
/// build it, not in what they build.
std::string guidHex(u64 guid) {
    static const char hex[] = "0123456789abcdef";
    std::string s(16, '0');
    for (size_t i = 16; i-- > 0;) {
        s[i] = hex[guid & 0xF];
        guid >>= 4;
    }
    return s;
}

std::string assetExtension(u64 guid) {
    static const char hex[] = "0123456789abcdef";
    auto const typeId = ow::assetTypeId(guid);
    if (typeId == 0) {
        auto const field = ow::assetTypeField(guid);
        return {'.', 'x', hex[(field >> 8) & 0xF], hex[(field >> 4) & 0xF], hex[field & 0xF]};
    }
    auto const name = ow::assetTypeName(typeId);
    if (!name.empty())
        return "." + std::string(name);
    return {'.', hex[(typeId >> 8) & 0xF], hex[(typeId >> 4) & 0xF], hex[typeId & 0xF]};
}

} // namespace

int main(int argc, char* argv[]) {
    std::string owPath, csvPath;
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
            std::cout << "Usage: " << argv[0] << " <ow_path> [--reps N] [--threads N] [--no-e2e]"
                      << " [--csv <path>]\n";
            return 0;
        } else if (owPath.empty())
            owPath = a;
    }

    if (owPath.empty() || !fs::exists(owPath)) {
        std::cerr << "Error: pass a valid Overwatch install path.\n";
        return 1;
    }
    if (threads < 1)
        threads = 1;

    auto paths = resolvePaths(owPath);
    std::cout << "Install : " << paths.base << "\nData    : " << paths.data
              << "\nThreads : " << threads << "\nReps    : " << reps << "\n";

    utils::SimpleThreadPool pool{size_t(threads)};
    Table table;

    // --- End-to-end reference, with the per-step split the open reports. ---
    if (doE2E) {
        OpenOptions o;
        o.path = owPath;
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

    // --- 1. The root text itself. ---
    t = Clock::now();
    auto rootData = resolveCKey(buildConfig.rootCKey);
    table.add("1. root file: fetch + BLTE", msSince(t), rootData.size());
    if (rootData.empty() || rootData[0] != '#') {
        std::cerr << "root is not an Overwatch text root\n";
        return 1;
    }

    // --- 2. Text root parse on its own (no resolver = manifest rows only). ---
    std::vector<OwRootFileEntry> manifest;
    double const textMs = bestOf(reps, [&] {
        auto r = OwRoot::parse(rootData, nullptr, &pool);
        manifest = r->manifestEntries();
    });
    table.add("2. text root parse (rows only)", textMs, rootData.size(), manifest.size());

    // --- 3. Fetch + BLTE-decode every CMF, serial then pooled. ---
    std::vector<size_t> cmfRows;
    for (size_t i = 0; i < manifest.size(); ++i) {
        auto name = manifest[i].fileName;
        for (auto& c : name)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".cmf") == 0)
            cmfRows.push_back(i);
    }

    std::vector<std::vector<u8>> cmfBlobs(cmfRows.size());
    u64 cmfBytes = 0;
    t = Clock::now();
    for (size_t i = 0; i < cmfRows.size(); ++i) {
        cmfBlobs[i] = resolveCKey(manifest[cmfRows[i]].md5);
        cmfBytes += cmfBlobs[i].size();
    }
    double const serialFetchMs = msSince(t);
    table.add("3a. CMF fetch + BLTE (serial)", serialFetchMs, cmfBytes, cmfRows.size(),
              "as OwRoot does it today");
    {
        std::vector<size_t> sizes;
        for (auto& b : cmfBlobs)
            sizes.push_back(b.size());
        std::sort(sizes.rbegin(), sizes.rend());
        std::cout << "\n  decoded CMF sizes (MB), largest first: ";
        for (size_t i = 0; i < std::min<size_t>(10, sizes.size()); ++i)
            std::cout << (sizes[i] / (1024.0 * 1024.0)) << " ";
        u64 top = 0;
        for (size_t i = 0; i < std::min<size_t>(5, sizes.size()); ++i)
            top += sizes[i];
        std::cout << "\n  top 5 hold " << (100.0 * double(top) / double(cmfBytes)) << "% of "
                  << (cmfBytes / (1024.0 * 1024.0)) << " MB\n";
    }

    {
        std::vector<std::vector<u8>> scratch(cmfRows.size());
        std::atomic<size_t> next{0};
        t = Clock::now();
        std::vector<std::thread> workers;
        for (int w = 0; w < threads; ++w) {
            workers.emplace_back([&] {
                for (size_t i = next++; i < cmfRows.size(); i = next++)
                    scratch[i] = resolveCKey(manifest[cmfRows[i]].md5);
            });
        }
        for (auto& w : workers)
            w.join();
        table.add("3b. CMF fetch + BLTE (pooled)", msSince(t), cmfBytes, cmfRows.size(),
                  std::to_string(threads) + " threads");
    }

    // --- 4. Header parse + decryption over the fetched blobs. ---
    std::vector<CmfInfo> infos(cmfRows.size());
    size_t encryptedCount = 0;
    u64 encryptedBytes = 0, assetCount = 0;
    for (size_t i = 0; i < cmfRows.size(); ++i) {
        if (!readCmfInfo(cmfBlobs[i], infos[i]))
            continue;
        assetCount += u64(std::max(infos[i].dataCount, 0));
        if (infos[i].encrypted) {
            ++encryptedCount;
            encryptedBytes += cmfBlobs[i].size() - infos[i].headerSize;
        }
    }

    {
        auto copies = cmfBlobs;
        double const decMs = bestOf(reps, [&] {
            for (size_t i = 0; i < copies.size(); ++i) {
                if (!infos[i].encrypted || copies[i].size() <= infos[i].headerSize)
                    continue;
                ow::CmfCryptoHeader const h{infos[i].build, infos[i].dataCount, infos[i].entryCount,
                                            ow::cmfNonEncryptedMagic(infos[i].magic)};
                auto const& n = manifest[cmfRows[i]].fileName;
                auto const slash = n.find_last_of("\\/");
                ow::decryptCmfBody(std::span<u8>(copies[i]).subspan(infos[i].headerSize), h,
                                   slash == std::string::npos ? std::string_view(n)
                                                              : std::string_view(n).substr(slash + 1));
            }
        });
        table.add("4. CMF decrypt (AES-256-CBC)", decMs, encryptedBytes, encryptedCount,
                  "serial, in place");
    }

    {
        auto copies = cmfBlobs;
        std::atomic<size_t> next{0};
        double const decMs = bestOf(reps, [&] {
            next = 0;
            std::vector<std::thread> workers;
            for (int w = 0; w < threads; ++w) {
                workers.emplace_back([&] {
                    for (size_t i = next++; i < copies.size(); i = next++) {
                        if (!infos[i].encrypted || copies[i].size() <= infos[i].headerSize)
                            continue;
                        ow::CmfCryptoHeader const h{infos[i].build, infos[i].dataCount,
                                                    infos[i].entryCount,
                                                    ow::cmfNonEncryptedMagic(infos[i].magic)};
                        auto const& n = manifest[cmfRows[i]].fileName;
                        auto const slash = n.find_last_of("\\/");
                        ow::decryptCmfBody(
                            std::span<u8>(copies[i]).subspan(infos[i].headerSize), h,
                            slash == std::string::npos ? std::string_view(n)
                                                       : std::string_view(n).substr(slash + 1));
                    }
                });
            }
            for (auto& w : workers)
                w.join();
        });
        table.add("4b. CMF decrypt (pooled)", decMs, encryptedBytes, encryptedCount,
                  std::to_string(threads) + " threads, same AES");
    }

    // Decrypt for real so the record walk below sees plaintext.
    for (size_t i = 0; i < cmfBlobs.size(); ++i) {
        if (!infos[i].encrypted || cmfBlobs[i].size() <= infos[i].headerSize)
            continue;
        ow::CmfCryptoHeader const h{infos[i].build, infos[i].dataCount, infos[i].entryCount,
                                    ow::cmfNonEncryptedMagic(infos[i].magic)};
        auto const& n = manifest[cmfRows[i]].fileName;
        auto const slash = n.find_last_of("\\/");
        if (ow::decryptCmfBody(std::span<u8>(cmfBlobs[i]).subspan(infos[i].headerSize), h,
                               slash == std::string::npos
                                   ? std::string_view(n)
                                   : std::string_view(n).substr(slash + 1)))
            infos[i].encrypted = false;
    }

    // --- 5. The record walk, in three shapes. ---
    // The GUID and CKey of every asset, pulled once so each variant below walks
    // exactly the same bytes.
    struct Rec {
        u64 guid;
        u32 size;
        const u8* cKey;
    };
    std::vector<Rec> records;
    records.reserve(size_t(assetCount));
    for (size_t i = 0; i < cmfBlobs.size(); ++i) {
        if (infos[i].encrypted || infos[i].dataCount <= 0)
            continue;
        size_t const recSize =
            (infos[i].build >= kBuildHashData135) ? kHashDataSize : kHashDataOldSize;
        size_t const skip = infos[i].headerSize + size_t(infos[i].entryCount) * kCmfEntrySize;
        if (skip + size_t(infos[i].dataCount) * recSize > cmfBlobs[i].size())
            continue;
        const u8* p = cmfBlobs[i].data() + skip;
        for (i32 k = 0; k < infos[i].dataCount; ++k, p += recSize) {
            records.push_back({storages::common::readLE64(p), storages::common::readLE32(p + 8),
                               p + (recSize == kHashDataSize ? 13 : 12)});
        }
    }

    std::cout << "\n  assets: " << records.size() << " across " << cmfRows.size() << " CMFs ("
              << encryptedCount << " encrypted)\n";

    // If each CMF's hash block were already GUID-ordered the whole index would
    // be a merge of sorted runs rather than a sort.
    {
        size_t sortedCmfs = 0, cmfSeen = 0, longestRun = 0, runs = 0;
        size_t at = 0;
        for (size_t i = 0; i < cmfBlobs.size(); ++i) {
            if (infos[i].encrypted || infos[i].dataCount <= 0)
                continue;
            size_t const n = size_t(infos[i].dataCount);
            if (at + n > records.size())
                break;
            ++cmfSeen;
            bool sorted = true;
            size_t run = 1;
            for (size_t k = 1; k < n; ++k) {
                if (records[at + k].guid >= records[at + k - 1].guid) {
                    ++run;
                } else {
                    sorted = false;
                    longestRun = std::max(longestRun, run);
                    ++runs;
                    run = 1;
                }
            }
            longestRun = std::max(longestRun, run);
            ++runs;
            sortedCmfs += sorted ? 1 : 0;
            at += n;
        }
        std::cout << "  GUID order: " << sortedCmfs << "/" << cmfSeen
                  << " CMFs already sorted; " << runs << " ascending runs, longest " << longestRun
                  << "\n";
    }

    {
        double const ms = bestOf(reps, [&] {
            u64 acc = 0;
            for (auto& r : records)
                acc += r.guid ^ r.size;
            if (acc == 0x1234)
                std::cout << "";
        });
        table.add("5a. record walk only (no entries)", ms, 0, records.size(), "lower bound");

        // The parser sizes the vector up front so the record walk can run on
        // the pool; that alone value-constructs three gigabytes, serially.
        double const ms0 = bestOf(reps, [&] {
            std::vector<RootEntry> v;
            v.resize(records.size());
        });
        table.add("5a2. vector<RootEntry>::resize alone", ms0,
                  u64(records.size()) * sizeof(RootEntry), records.size(), "serial, unavoidable?");
    }

    {
        std::string const prefix = "contentmanifestfiles\\windows-rcn\\enus\\speech\\";
        double ms = 0;
        {
            std::vector<RootEntry> out;
            ms = bestOf(reps, [&] {
                out.clear();
                out.shrink_to_fit();
                out.reserve(records.size());
                for (auto& r : records) {
                    RootEntry e;
                    std::memcpy(e.cKey.data(), r.cKey, 16);
                    e.fileNameHash = r.guid;
                    e.fileSize = r.size;
                    e.path = prefix + guidHex(r.guid) + assetExtension(r.guid);
                    out.push_back(std::move(e));
                }
            });
            table.add("5b. + RootEntry with path string", ms, u64(out.size()) * sizeof(RootEntry),
                      out.size(), "current behaviour");
        }
        {
            std::vector<RootEntry> out;
            double const ms2 = bestOf(reps, [&] {
                out.clear();
                out.shrink_to_fit();
                out.reserve(records.size());
                for (auto& r : records) {
                    RootEntry e;
                    std::memcpy(e.cKey.data(), r.cKey, 16);
                    e.fileNameHash = r.guid;
                    e.fileSize = r.size;
                    out.push_back(std::move(e));
                }
            });
            table.add("5c. + RootEntry, no path string", ms2, u64(out.size()) * sizeof(RootEntry),
                      out.size(), "path built on demand");
        }
        {
            // The same string, formatted in place: one allocation per entry
            // instead of the three the `a + b + c` expression costs.
            static const char hexDigits[] = "0123456789abcdef";
            std::vector<RootEntry> out;
            u64 pathBytes = 0;
            double const ms3 = bestOf(reps, [&] {
                out.clear();
                out.shrink_to_fit();
                out.reserve(records.size());
                pathBytes = 0;
                for (auto& r : records) {
                    RootEntry e;
                    std::memcpy(e.cKey.data(), r.cKey, 16);
                    e.fileNameHash = r.guid;
                    e.fileSize = r.size;

                    auto const typeId = ow::assetTypeId(r.guid);
                    auto const ext = typeId ? ow::assetTypeName(typeId) : std::string_view{};
                    e.path.reserve(prefix.size() + 16 + (ext.empty() ? 5 : ext.size() + 1));
                    e.path.assign(prefix);
                    for (int s = 60; s >= 0; s -= 4)
                        e.path.push_back(hexDigits[(r.guid >> s) & 0xF]);
                    e.path.push_back('.');
                    if (!ext.empty()) {
                        e.path.append(ext);
                    } else {
                        u32 const field = typeId ? typeId : ow::assetTypeField(r.guid);
                        if (!typeId)
                            e.path.push_back('x');
                        for (int s = 8; s >= 0; s -= 4)
                            e.path.push_back(hexDigits[(field >> s) & 0xF]);
                    }
                    pathBytes += e.path.capacity();
                    out.push_back(std::move(e));
                }
            });
            table.add("5d. + RootEntry, path in one alloc", ms3,
                      u64(out.size()) * sizeof(RootEntry) + pathBytes, out.size(),
                      "same strings, no temporaries");
        }
    }

    // --- 6. GUID index build. ---
    {
        std::vector<std::pair<u64, u32>> byGuid;
        double const ms = bestOf(reps, [&] {
            byGuid.clear();
            byGuid.shrink_to_fit();
            byGuid.reserve(records.size());
            for (size_t i = 0; i < records.size(); ++i)
                byGuid.emplace_back(records[i].guid, u32(i));
            std::stable_sort(byGuid.begin(), byGuid.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
        });
        table.add("6a. GUID index: fill + stable_sort", ms, u64(byGuid.size()) * 12,
                  byGuid.size(), "current behaviour");

        double const ms2 = bestOf(reps, [&] {
            byGuid.clear();
            byGuid.shrink_to_fit();
            byGuid.reserve(records.size());
            for (size_t i = 0; i < records.size(); ++i)
                byGuid.emplace_back(records[i].guid, u32(i));
            std::sort(std::execution::par_unseq, byGuid.begin(), byGuid.end(),
                      [](const auto& a, const auto& b) {
                          return a.first != b.first ? a.first < b.first : a.second < b.second;
                      });
        });
        table.add("6b. GUID index: fill + parallel sort", ms2, u64(byGuid.size()) * 12,
                  byGuid.size(), "ties broken by index");

        // Every CMF is already GUID-ordered, so the index is a merge of runs.
        // A merge also keeps manifest order on ties, which is what the stable
        // sort was there for.
        std::vector<size_t> runBounds{0};
        {
            size_t at = 0;
            for (size_t i = 0; i < cmfBlobs.size(); ++i) {
                if (infos[i].encrypted || infos[i].dataCount <= 0)
                    continue;
                at += size_t(infos[i].dataCount);
                if (at > records.size())
                    break;
                runBounds.push_back(at);
            }
        }

        using Pair = std::pair<u64, u32>;
        auto const less = [](const Pair& a, const Pair& b) { return a.first < b.first; };
        std::vector<Pair> scratch;
        auto mergeRuns = [&](int workers) {
            byGuid.clear();
            byGuid.shrink_to_fit();
            byGuid.reserve(records.size());
            for (size_t i = 0; i < records.size(); ++i)
                byGuid.emplace_back(records[i].guid, u32(i));
            scratch.assign(byGuid.size(), Pair{});

            auto bounds = runBounds;
            Pair* src = byGuid.data();
            Pair* dst = scratch.data();
            while (bounds.size() > 2) {
                size_t const merges = (bounds.size() - 1) / 2;
                std::atomic<size_t> next{0};
                std::vector<std::thread> pool2;
                for (int w = 0; w < std::max(1, workers); ++w) {
                    pool2.emplace_back([&] {
                        for (size_t m = next++; m < merges; m = next++) {
                            size_t const a = bounds[2 * m], b = bounds[2 * m + 1],
                                         c = bounds[2 * m + 2];
                            std::merge(src + a, src + b, src + b, src + c, dst + a, less);
                        }
                    });
                }
                for (auto& w : pool2)
                    w.join();
                if ((bounds.size() - 1) % 2 != 0) {
                    size_t const a = bounds[bounds.size() - 2];
                    std::copy(src + a, src + bounds.back(), dst + a);
                }
                std::swap(src, dst);
                std::vector<size_t> nb;
                for (size_t i = 0; i < bounds.size(); i += 2)
                    nb.push_back(bounds[i]);
                if (nb.back() != bounds.back())
                    nb.push_back(bounds.back());
                bounds = std::move(nb);
            }
            if (src != byGuid.data())
                byGuid = scratch;
        };

        double const ms3 = bestOf(reps, [&] { mergeRuns(1); });
        table.add("6c. GUID index: fill + merge runs", ms3, u64(byGuid.size()) * 12, byGuid.size(),
                  "serial merge tree");
        double const ms4 = bestOf(reps, [&] { mergeRuns(threads); });
        table.add("6d. GUID index: fill + merge (pooled)", ms4, u64(byGuid.size()) * 12,
                  byGuid.size(), std::to_string(threads) + " threads");

        std::vector<Pair> reference;
        reference.reserve(records.size());
        for (size_t i = 0; i < records.size(); ++i)
            reference.emplace_back(records[i].guid, u32(i));
        std::stable_sort(reference.begin(), reference.end(), less);
        table.add("6e. merge == stable_sort", 0, 0, reference == byGuid ? 1 : 0,
                  reference == byGuid ? "identical" : "DIFFERENT");
    }

    // --- 7. The whole root, fed from memory so no I/O is counted. ---
    {
        std::unordered_map<u64, size_t> byKey;
        auto freshBlobs = std::vector<std::vector<u8>>(cmfRows.size());
        for (size_t i = 0; i < cmfRows.size(); ++i) {
            freshBlobs[i] = resolveCKey(manifest[cmfRows[i]].md5);
            u64 h = 0;
            std::memcpy(&h, manifest[cmfRows[i]].md5.data(), 8);
            byKey[h] = i;
        }
        CKeyResolver const memResolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
            u64 h = 0;
            std::memcpy(&h, cKey.data(), 8);
            auto it = byKey.find(h);
            return it == byKey.end() ? std::vector<u8>{} : freshBlobs[it->second];
        };

        size_t entries = 0;
        double const ms = bestOf(reps, [&] {
            auto r = OwRoot::parse(rootData, memResolver, &pool);
            entries = r ? r->entryCount() : 0;
        });
        table.add("7. OwRoot::parse (CMFs from memory)", ms, 0, entries,
                  "decrypt + records + paths + index");
    }

    // --- 8. What the open does after the root: one encoding lookup per entry,
    // which fills in fileSize and marks the encoding row as referenced. ---
    {
        std::vector<u64> sizes(records.size());
        double const ms = bestOf(reps, [&] {
            for (size_t i = 0; i < records.size(); ++i) {
                std::array<u8, 16> k{};
                std::memcpy(k.data(), records[i].cKey, 16);
                const auto* enc = encodingTable.findByCKey(k, 9);
                sizes[i] = enc ? enc->fileSize : 0;
            }
        });
        table.add("8a. encoding lookup per entry", ms, 0, records.size(), "the 'Ready' step");

        std::atomic<size_t> next{0};
        double const ms2 = bestOf(reps, [&] {
            next = 0;
            std::vector<std::thread> workers;
            size_t const chunk = 64 * 1024;
            for (int w = 0; w < threads; ++w) {
                workers.emplace_back([&] {
                    for (size_t base = next.fetch_add(chunk); base < records.size();
                         base = next.fetch_add(chunk)) {
                        size_t const end = std::min(base + chunk, records.size());
                        for (size_t i = base; i < end; ++i) {
                            std::array<u8, 16> k{};
                            std::memcpy(k.data(), records[i].cKey, 16);
                            const auto* enc = encodingTable.findByCKey(k, 9);
                            sizes[i] = enc ? enc->fileSize : 0;
                        }
                    }
                });
            }
            for (auto& w : workers)
                w.join();
        });
        table.add("8b. encoding lookup per entry (pooled)", ms2, 0, records.size(),
                  std::to_string(threads) + " threads");

        size_t resolved = 0, sameAsCmf = 0;
        for (size_t i = 0; i < records.size(); ++i) {
            if (sizes[i] == 0)
                continue;
            ++resolved;
            if (sizes[i] == records[i].size)
                ++sameAsCmf;
        }
        table.add("8c. entries the encoding table knows", 0, 0, resolved,
                  std::to_string(records.size() - resolved) + " not installed");
        table.add("8d.   of those, size matched the CMF", 0, 0, sameAsCmf,
                  std::to_string(resolved - sameAsCmf) + " differ");
    }

    // --- 9. How much of the entry set is actually distinct. The root keeps one
    // entry per manifest that carries an asset, and Overwatch ships the same
    // asset in a great many locale and region manifests. ---
    {
        auto t9 = Clock::now();
        std::vector<u64> guids;
        guids.reserve(records.size());
        for (auto& r : records)
            guids.push_back(r.guid);
        std::sort(std::execution::par_unseq, guids.begin(), guids.end());
        size_t const distinctGuids = size_t(std::unique(guids.begin(), guids.end()) - guids.begin());
        table.add("9a. distinct GUIDs", msSince(t9), 0, distinctGuids,
                  std::to_string(records.size()) + " entries total");

        t9 = Clock::now();
        std::vector<u64> ckeys;
        ckeys.reserve(records.size());
        for (auto& r : records) {
            u64 h = 0;
            std::memcpy(&h, r.cKey, 8);
            ckeys.push_back(h);
        }
        std::sort(std::execution::par_unseq, ckeys.begin(), ckeys.end());
        size_t const distinctKeys = size_t(std::unique(ckeys.begin(), ckeys.end()) - ckeys.begin());
        table.add("9b. distinct content keys", msSince(t9), 0, distinctKeys,
                  "encoding holds " + std::to_string(encodingTable.entryCount()));
    }

    table.print("Overwatch root open, stage by stage");
    std::cout << "peak RSS " << std::fixed << std::setprecision(0) << peakRssMb() << " MB\n";
    if (!csvPath.empty())
        table.writeCsv(csvPath);
    return 0;
}
