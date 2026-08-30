// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// CASC storage-open benchmark.
///
/// Profiles `Storage::open` for a local CASC install, splitting the cost into
/// the phases the open pipeline actually walks through, and re-runs the whole
/// thing with an external community listfile so the listfile cost is visible
/// separately from the rest of the open.
///
/// Usage:
///   casc_open_bench <casc_path> [options]
///
/// Options:
///   --listfile <path>   Community listfile (FileDataId;path per line).
///   --reps N            Repetitions per measurement (default 3, min/median reported).
///   --threads N         Worker-pool threads (default hardware_concurrency).
///   --csv <path>        Write results as CSV.
///   --no-phases         Skip the instrumented phase breakdown.
///   --no-matrix         Skip the end-to-end feature-flag matrix.
///   --index-experiment  Compare root index-build strategies over the real entry set.
///   --single <flags>    Run one Storage::open with the given StorageFeatureFlags and exit.
///   --nopool            With --single: open without a worker pool.
///   --trace-steps       Print each ProgressStep as the open advances.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/job_group.h>
#include <whiteout/utils/simple_thread_pool.h>

#include "../src/whiteout/storages/casc/codec/blte.h"
#include "../src/whiteout/storages/casc/codec/crypto.h"
#include "../src/whiteout/storages/casc/roots/common/listfile_parser.h"
#include "../src/whiteout/storages/casc/roots/common/wow_tvfs_path.h"
#include "../src/whiteout/storages/casc/roots/d4_root.h"
#include "../src/whiteout/storages/casc/roots/tvfs_root.h"
#include "../src/whiteout/storages/casc/roots/wow_tvfs_root.h"
#include "../src/whiteout/storages/casc/storage/local_data_source.h"
#include "../src/whiteout/storages/casc/tables/config.h"
#include "../src/whiteout/storages/casc/tables/encoding.h"
#include "../src/whiteout/storages/casc/tables/index.h"
#include "../src/whiteout/storages/common/mapped_file.h"
#include "../src/whiteout/storages/common/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Stat {
    std::string name;
    std::vector<double> samples;
    u64 bytes = 0;  ///< Bytes touched by this phase (0 = unknown).
    u64 items = 0;  ///< Items produced (entries, files, ...).

    double min() const { return samples.empty() ? 0.0 : *std::min_element(samples.begin(), samples.end()); }
    double median() const {
        if (samples.empty())
            return 0.0;
        auto s = samples;
        std::sort(s.begin(), s.end());
        return s[s.size() / 2];
    }
};

class Results {
public:
    void add(const std::string& group, const std::string& name, double ms, u64 bytes = 0,
             u64 items = 0) {
        auto& vec = m_groups[group];
        for (auto& s : vec) {
            if (s.name == name) {
                s.samples.push_back(ms);
                if (bytes) s.bytes = bytes;
                if (items) s.items = items;
                return;
            }
        }
        Stat s;
        s.name = name;
        s.samples.push_back(ms);
        s.bytes = bytes;
        s.items = items;
        vec.push_back(std::move(s));
        m_order.push_back(group);
    }

    void print(const std::string& group, const std::string& title) const {
        auto it = m_groups.find(group);
        if (it == m_groups.end())
            return;
        std::cout << "\n" << title << "\n";
        std::cout << std::string(96, '-') << "\n";
        std::cout << std::left << std::setw(46) << "phase" << std::right << std::setw(11) << "min ms"
                  << std::setw(11) << "med ms" << std::setw(12) << "MB" << std::setw(14) << "items"
                  << "\n";
        std::cout << std::string(96, '-') << "\n";
        double totMin = 0, totMed = 0;
        for (auto& s : it->second) {
            totMin += s.min();
            totMed += s.median();
            std::cout << std::left << std::setw(46) << s.name << std::right << std::fixed
                      << std::setprecision(1) << std::setw(11) << s.min() << std::setw(11)
                      << s.median() << std::setw(12);
            if (s.bytes)
                std::cout << std::setprecision(1) << (double(s.bytes) / (1024.0 * 1024.0));
            else
                std::cout << "-";
            std::cout << std::setw(14);
            if (s.items)
                std::cout << s.items;
            else
                std::cout << "-";
            std::cout << "\n";
        }
        std::cout << std::string(96, '-') << "\n";
        std::cout << std::left << std::setw(46) << "TOTAL" << std::right << std::fixed
                  << std::setprecision(1) << std::setw(11) << totMin << std::setw(11) << totMed
                  << "\n";
    }

    void writeCsv(const std::string& path) const {
        std::ofstream out(path);
        if (!out)
            return;
        out << "group,phase,min_ms,median_ms,bytes,items,samples\n";
        std::vector<std::string> groups;
        for (auto& g : m_order)
            if (std::find(groups.begin(), groups.end(), g) == groups.end())
                groups.push_back(g);
        for (auto& g : groups) {
            for (auto& s : m_groups.at(g)) {
                out << g << ",\"" << s.name << "\"," << s.min() << "," << s.median() << ","
                    << s.bytes << "," << s.items << ",";
                for (size_t i = 0; i < s.samples.size(); ++i)
                    out << (i ? " " : "") << s.samples[i];
                out << "\n";
            }
        }
    }

private:
    std::unordered_map<std::string, std::vector<Stat>> m_groups;
    std::vector<std::string> m_order;
};

// ---------------------------------------------------------------------------
// Path resolution (mirrors Storage::open's basePath/dataPath logic)
// ---------------------------------------------------------------------------

struct Paths {
    std::string base;
    std::string data;
};

Paths resolvePaths(const std::string& in) {
    Paths p;
    p.base = in;
    auto leaf = fs::path(p.base).filename().string();
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), ::tolower);
    if (leaf == "data") {
        p.data = p.base;
        p.base = fs::path(p.base).parent_path().string();
    } else if (fs::exists(p.base + "/Data")) {
        p.data = p.base + "/Data";
    } else if (fs::exists(p.base + "/data")) {
        p.data = p.base + "/data";
    } else {
        p.data = p.base;
    }
    p.base = fs::path(p.base).lexically_normal().string();
    p.data = fs::path(p.data).lexically_normal().string();
    return p;
}

std::string configPath(const std::string& dataPath, const std::array<u8, 16>& key) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string h;
    h.reserve(32);
    for (u8 b : key) {
        h += hex[b >> 4];
        h += hex[b & 0xF];
    }
    return dataPath + "/config/" + h.substr(0, 2) + "/" + h.substr(2, 2) + "/" + h;
}

u64 dirBytes(const std::string& dir, const std::string& ext) {
    u64 total = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return 0;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ext)
            total += e.file_size(ec);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Instrumented open — replicates the Storage::open pipeline step by step
// ---------------------------------------------------------------------------

struct PhaseOpts {
    interfaces::WorkerPool* pool = nullptr;
    std::span<const u8> listfile;
    bool indexExperiment = false;
    bool reportVfsShape = false;
};

void indexExperiment(std::vector<RootEntry>& entries, interfaces::WorkerPool* pool, int reps,
                     Results& res);

u64 countEntries(const RootManifest* root) {
    if (!root)
        return 0;
    u64 n = 0;
    root->enumerate([&](const RootEntry&) {
        ++n;
        return true;
    });
    return n;
}

void instrumentedOpen(const Paths& paths, const PhaseOpts& opts, Results& res,
                      const std::string& group) {
    KeyRing keyRing;

    // --- 1. .build.info ---
    auto t = Clock::now();
    std::string buildInfoPath = fs::exists(paths.base + "/.build.info")
                                    ? paths.base + "/.build.info"
                                    : paths.data + "/.build.info";
    auto buildInfoFile = storages::common::readFileFully(buildInfoPath);
    if (!buildInfoFile) {
        std::cerr << "cannot read " << buildInfoPath << "\n";
        return;
    }
    auto builds = parseBuildInfo(*buildInfoFile);
    res.add(group, "1. read+parse .build.info", msSince(t), buildInfoFile->size(), builds.size());

    const BuildInfo* activeBuild = nullptr;
    for (auto& b : builds)
        if (b.active) { activeBuild = &b; break; }
    if (!activeBuild && !builds.empty())
        activeBuild = &builds[0];
    if (!activeBuild)
        return;

    // --- 2. .idx index table ---
    t = Clock::now();
    auto indexTable = IndexTable::load(paths.data, opts.pool);
    res.add(group, "2. IndexTable::load (.idx buckets)", msSince(t),
            dirBytes(paths.data + "/data", ".idx"), indexTable.entryCount());

    // --- 3. build config + cdn config ---
    t = Clock::now();
    auto buildConfigFile = storages::common::readFileFully(configPath(paths.data, activeBuild->buildKey));
    BuildConfig buildConfig;
    if (buildConfigFile)
        buildConfig = parseBuildConfig(*buildConfigFile);
    auto cdnConfigFile = storages::common::readFileFully(configPath(paths.data, activeBuild->cdnKey));
    CdnConfig cdnConfig;
    if (cdnConfigFile)
        cdnConfig = parseCdnConfig(*cdnConfigFile);
    res.add(group, "3. read+parse build/cdn config", msSince(t),
            (buildConfigFile ? buildConfigFile->size() : 0) +
                (cdnConfigFile ? cdnConfigFile->size() : 0),
            cdnConfig.archiveEKeys.size());

    // --- 4. archive .index files ---
    t = Clock::now();
    size_t const beforeArc = indexTable.entryCount();
    if (!cdnConfig.archiveEKeys.empty())
        indexTable.loadArchiveIndices(paths.data, cdnConfig.archiveEKeys, opts.pool);
    res.add(group, "4. loadArchiveIndices (.index)", msSince(t),
            dirBytes(paths.data + "/indices", ".index"), indexTable.entryCount() - beforeArc);

    // --- 5. memory-map data.NNN archives ---
    t = Clock::now();
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
        u64 mapped = 0;
        for (u32 i = 0; i <= maxIndex; ++i) {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "/data/data.%03u", i);
            std::string const n = paths.data + suffix;
            if (!fs::exists(n))
                continue;
            if (auto m = storages::common::MappedFile::open(n, storages::common::AccessHint::Random)) {
                mapped += m->size();
                archives[i] = std::move(*m);
            }
        }
        res.add(group, "5. mmap data.NNN archives", msSince(t), mapped, archives.size());
    }

    LocalDataSource dataSource(&indexTable, &archives);

    // --- 6. encoding table: fetch / BLTE decode / parse ---
    t = Clock::now();
    std::vector<u8> encodingOwned;
    std::span<const u8> encodingBlte;
    if (auto loc = dataSource.findInIndex(buildConfig.encodingEKey))
        encodingBlte = dataSource.viewBlte(*loc);
    if (encodingBlte.empty()) {
        encodingOwned = dataSource.fetchBlte(buildConfig.encodingEKey);
        encodingBlte = encodingOwned;
    }
    res.add(group, "6a. encoding: locate + view BLTE", msSince(t), encodingBlte.size());
    if (encodingBlte.empty()) {
        static constexpr char hx[] = "0123456789abcdef";
        std::string k;
        for (u8 b : buildConfig.encodingEKey) { k += hx[b >> 4]; k += hx[b & 0xF]; }
        auto loc = dataSource.findInIndex(buildConfig.encodingEKey);
        std::cerr << "  [diag] encodingEKey=" << k << " indexHit=" << (loc ? "yes" : "no");
        if (loc)
            std::cerr << " arc=" << loc->archiveIndex << " off=" << loc->offset
                      << " size=" << loc->encodedSize << " direct=" << loc->directBLTE
                      << " archives=" << archives.size();
        std::cerr << "\n";
    }

    t = Clock::now();
    auto encodingDecoded = blteDecode(encodingBlte, &keyRing, opts.pool);
    res.add(group, "6b. encoding: BLTE decode", msSince(t), encodingDecoded.data.size());

    t = Clock::now();
    auto encodingTable = EncodingTable::parse(encodingDecoded.data, opts.pool);
    res.add(group, "6c. encoding: EncodingTable::parse", msSince(t), 0,
            encodingTable.entryCount());

    if (!encodingTable.isValid()) {
        std::cerr << "encoding table invalid\n";
        return;
    }

    auto resolveCKey = [&](const std::array<u8, 16>& cKey) -> std::vector<u8> {
        auto* e = encodingTable.findByCKey(cKey);
        if (!e)
            return {};
        auto blte = dataSource.fetchBlte(e->eKey);
        if (blte.empty())
            return {};
        auto d = blteDecode(blte, &keyRing, nullptr);
        return d.success ? std::move(d.data) : std::vector<u8>{};
    };
    auto resolveEKeyPooled = [&](const std::array<u8, 16>& eKey,
                                 interfaces::WorkerPool* framePool) -> std::vector<u8> {
        std::vector<u8> owned;
        std::span<const u8> blte;
        if (auto loc = dataSource.findInIndex(eKey))
            blte = dataSource.viewBlte(*loc);
        if (blte.empty()) {
            owned = dataSource.fetchBlte(eKey);
            blte = owned;
        }
        if (blte.empty())
            return {};
        auto d = blteDecode(blte, &keyRing, framePool);
        return d.success ? std::move(d.data) : std::vector<u8>{};
    };
    auto resolveEKey = [&](const std::array<u8, 16>& eKey) -> std::vector<u8> {
        return resolveEKeyPooled(eKey, nullptr);
    };

    bool const hasVfs =
        std::any_of(buildConfig.vfsRootCKey.begin(), buildConfig.vfsRootCKey.end(),
                    [](u8 b) { return b != 0; });
    if (!hasVfs) {
        // Non-TVFS product (classic WoW MFST root, D3, etc.).
        t = Clock::now();
        auto rootData = resolveCKey(buildConfig.rootCKey);
        res.add(group, "7. root: resolve + decode", msSince(t), rootData.size());
        return;
    }

    // --- 7. VFS sub-manifest prefetch ---
    std::vector<std::array<u8, 16>> vfsEKeys;
    std::unordered_map<u64, std::array<u8, 16>> vfsEKeyToCKey;
    for (auto& sub : buildConfig.vfsSubManifests) {
        if (sub.cKey == buildConfig.vfsRootCKey)
            continue;
        vfsEKeys.push_back(sub.eKey);
        vfsEKeyToCKey[keyHash64(sub.eKey)] = sub.cKey;
    }

    t = Clock::now();
    std::unordered_map<u64, std::vector<u8>> vfsCache;
    u64 vfsBytes = 0;
    if (opts.pool && vfsEKeys.size() > 1) {
        struct SubResult {
            u64 hash = 0;
            std::vector<u8> data;
        };
        std::vector<SubResult> results(vfsEKeys.size());
        auto one = [&](size_t i, interfaces::WorkerPool* framePool) {
            results[i].hash = keyHash64(vfsEKeys[i]);
            results[i].data = resolveEKeyPooled(vfsEKeys[i], framePool);
            if (results[i].data.empty()) {
                auto c = vfsEKeyToCKey.find(results[i].hash);
                if (c != vfsEKeyToCKey.end())
                    results[i].data = resolveCKey(c->second);
            }
        };

        constexpr u32 kFrameParallelBytes = 4u << 20;
        std::vector<size_t> large;
        std::vector<size_t> small;
        for (size_t i = 0; i < vfsEKeys.size(); ++i) {
            auto loc = dataSource.findInIndex(vfsEKeys[i]);
            if (loc && loc->encodedSize >= kFrameParallelBytes)
                large.push_back(i);
            else
                small.push_back(i);
        }
        std::sort(large.begin(), large.end(), [&](size_t a, size_t b) {
            auto la = dataSource.findInIndex(vfsEKeys[a]);
            auto lb = dataSource.findInIndex(vfsEKeys[b]);
            return (la ? la->encodedSize : 0) > (lb ? lb->encodedSize : 0);
        });

        utils::JobGroup jobGroup;
        jobGroup.add(small.size());
        for (size_t const i : small) {
            interfaces::WorkerTask task;
            task.fn = [&, i]() {
                one(i, nullptr);
                jobGroup.done();
            };
            opts.pool->submit(task);
        }
        for (size_t const i : large)
            one(i, opts.pool);
        jobGroup.wait();

        for (auto& r : results) {
            if (!r.data.empty()) {
                vfsBytes += r.data.size();
                vfsCache.emplace(r.hash, std::move(r.data));
            }
        }
    } else {
        for (auto& ek : vfsEKeys) {
            u64 const h = keyHash64(ek);
            auto d = resolveEKey(ek);
            if (d.empty()) {
                auto c = vfsEKeyToCKey.find(h);
                if (c != vfsEKeyToCKey.end())
                    d = resolveCKey(c->second);
            }
            if (!d.empty()) {
                vfsBytes += d.size();
                vfsCache.emplace(h, std::move(d));
            }
        }
    }
    res.add(group, "7. VFS sub-manifest prefetch", msSince(t), vfsBytes, vfsEKeys.size());

    if (opts.reportVfsShape) {
        std::vector<u64> sizes;
        sizes.reserve(vfsCache.size());
        for (auto& [h, d] : vfsCache)
            sizes.push_back(d.size());
        std::sort(sizes.begin(), sizes.end(), std::greater<u64>());
        u64 const total = std::accumulate(sizes.begin(), sizes.end(), u64(0));
        u64 top8 = 0;
        for (size_t i = 0; i < std::min<size_t>(8, sizes.size()); ++i)
            top8 += sizes[i];
        std::cout << "  [vfs shape] n=" << sizes.size() << "  total="
                  << (double(total) / 1048576.0) << " MB  largest="
                  << (double(sizes.empty() ? 0 : sizes[0]) / 1048576.0) << " MB  top8="
                  << (100.0 * double(top8) / double(total ? total : 1)) << "% of bytes  median="
                  << (sizes.empty() ? 0 : sizes[sizes.size() / 2] / 1024) << " KB\n";
    }

    // --- 8. vfs-root resolve ---
    t = Clock::now();
    auto vfsData = resolveCKey(buildConfig.vfsRootCKey);
    res.add(group, "8. vfs-root resolve + decode", msSince(t), vfsData.size());
    if (vfsData.empty())
        return;

    // --- 9. TVFS traversal ---
    std::mutex vfsFaultMtx;
    VfsResolver const vfsResolver = [&](std::span<const u8> eKey) -> std::span<const u8> {
        u64 const h = keyHash64(eKey.data());
        {
            std::lock_guard<std::mutex> const lk(vfsFaultMtx);
            auto it = vfsCache.find(h);
            if (it != vfsCache.end())
                return it->second;
        }
        std::array<u8, 16> k{};
        std::memcpy(k.data(), eKey.data(), std::min(eKey.size(), size_t(16)));
        auto data = resolveEKey(k);
        if (data.empty())
            return {};
        std::lock_guard<std::mutex> const lk(vfsFaultMtx);
        return vfsCache.emplace(h, std::move(data)).first->second;
    };

    t = Clock::now();
    auto tvfsRoot = TvfsRoot::parse(vfsData, vfsResolver, vfsEKeys, opts.pool, /*buildIdx=*/false);
    double const tvfsMs = msSince(t);
    res.add(group, "9. TvfsRoot::parse (traverse manifests)", tvfsMs, 0, countEntries(tvfsRoot.get()));
    if (!tvfsRoot)
        return;

    // --- 10. listfile parse (only when one was supplied) ---
    if (!opts.listfile.empty()) {
        t = Clock::now();
        auto lf = parseListfile(opts.listfile, opts.pool);
        res.add(group, "10. parseListfile (standalone timing)", msSince(t), opts.listfile.size(),
                lf.size());
    }

    if (opts.indexExperiment) {
        auto entries = tvfsRoot->takeEntries();
        ListfileIndex lf;
        if (!opts.listfile.empty())
            lf = parseListfile(opts.listfile, opts.pool);
        bool const haveLf = !lf.empty();
        for (auto& e : entries) {
            wow_tvfs_path::Info info;
            bool const decoded = wow_tvfs_path::tryDecode(e.path, info);
            if (decoded) {
                e.cKey = info.cKey;
                e.localeFlags = info.localeFlags;
                e.contentFlags = info.contentFlags;
                e.fileDataId = info.fileDataId;
            }
            if (haveLf) {
                std::string np;
                if (decoded)
                    np.assign(lf.find(info.fileDataId));
                e.path = std::move(np);
            }
        }
        indexExperiment(entries, opts.pool, 1, res);
        return;
    }

    // --- 11. Root decoration. D4 wraps the same TvfsRoot but enriches from
    //         CoreTOC instead, so time whichever one this storage actually uses.
    if (tvfsRoot && !WowTvfsRoot::looksLikeWowTvfs(*tvfsRoot)) {
        // D4 locates CoreTOC by path, so it needs TvfsRoot's own index — which
        // is why Storage::open passes buildIdx=true for everything but WoW.
        t = Clock::now();
        tvfsRoot->ensureIndexed(opts.pool);
        res.add(group, "10. TvfsRoot::ensureIndexed (path map)", msSince(t));

        EKeyReader const eKeyReader = [&](std::span<const u8, 16> eKey) -> std::vector<u8> {
            std::array<u8, 16> k{};
            std::memcpy(k.data(), eKey.data(), 16);
            return resolveEKey(k);
        };
        t = Clock::now();
        auto d4 = D4Root::create(std::move(tvfsRoot), eKeyReader, opts.pool);
        res.add(group, "11. D4Root::create (CoreTOC enrich)", msSince(t), 0,
                countEntries(d4.get()));
        return;
    }

    t = Clock::now();
    auto wow = WowTvfsRoot::create(std::move(tvfsRoot), opts.pool, opts.listfile);
    double const wowMs = msSince(t);
    res.add(group, "11. WowTvfsRoot::create (+listfile+index)", wowMs, 0, countEntries(wow.get()));

    // Deferred work: the path index is only built when a path lookup happens.
    if (wow) {
        t = Clock::now();
        wow->findByPath("interface/icons/temp.blp");
        res.add(group, "12. first findByPath (lazy path index)", msSince(t));
    }
}

// ---------------------------------------------------------------------------
// Index-build experiment
//
// WowTvfsRoot::buildIndex dominates open time. This reproduces the two indices
// it builds over the real entry set and compares alternative constructions so
// the headroom is measurable rather than guessed.
// ---------------------------------------------------------------------------

u64 hashPath(std::string_view s) {
    // FNV-1a over the already-normalized key.
    u64 h = 1469598103934665603ULL;
    for (char c : s) {
        h ^= u8(c);
        h *= 1099511628211ULL;
    }
    return h;
}

void normalizeInto(const std::string& in, std::string& out) {
    out.assign(in);
    for (auto& c : out) {
        if (c == '/')
            c = '\\';
        else if (c >= 'A' && c <= 'Z')
            c = char(c + 32);
    }
}

void indexExperiment(std::vector<RootEntry>& entries, interfaces::WorkerPool* pool, int reps,
                     Results& res) {
    const size_t n = entries.size();
    const size_t threads = std::max<size_t>(pool ? pool->threadCount() : 1, 1);

    for (int r = 0; r < reps; ++r) {
        // --- path index, variant A: current implementation ---
        {
            auto t = Clock::now();
            std::unordered_multimap<std::string, size_t> m;
            for (size_t i = 0; i < n; ++i) {
                if (!entries[i].path.empty())
                    m.emplace(storages::common::normalizeCascPath(entries[i].path), i);
            }
            res.add("index", "path A: multimap<string> (current)", msSince(t), 0, m.size());
        }

        // --- path index, variant B: + reserve ---
        {
            auto t = Clock::now();
            std::unordered_multimap<std::string, size_t> m;
            m.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                if (!entries[i].path.empty())
                    m.emplace(storages::common::normalizeCascPath(entries[i].path), i);
            }
            res.add("index", "path B: multimap<string> + reserve", msSince(t), 0, m.size());
        }

        // --- path index, variant C: hash key, no string allocation ---
        {
            auto t = Clock::now();
            std::unordered_multimap<u64, u32> m;
            m.reserve(n);
            std::string buf;
            for (size_t i = 0; i < n; ++i) {
                if (entries[i].path.empty())
                    continue;
                normalizeInto(entries[i].path, buf);
                m.emplace(hashPath(buf), u32(i));
            }
            res.add("index", "path C: multimap<u64 hash> + reserve", msSince(t), 0, m.size());
        }

        // --- path index, variant D: parallel hash + sorted flat vector ---
        {
            auto t = Clock::now();
            std::vector<std::pair<u64, u32>> flat(n);
            if (pool && n > 10000) {
                size_t const chunk = (n + threads - 1) / threads;
                size_t const chunks = (n + chunk - 1) / chunk;
                utils::JobGroup jg;
                jg.add(chunks);
                for (size_t c = 0; c < chunks; ++c) {
                    interfaces::WorkerTask task;
                    task.fn = [&, c]() {
                        std::string buf;
                        size_t const b = c * chunk, e = std::min(b + chunk, n);
                        for (size_t i = b; i < e; ++i) {
                            if (entries[i].path.empty()) {
                                flat[i] = {~0ULL, u32(i)};
                                continue;
                            }
                            normalizeInto(entries[i].path, buf);
                            flat[i] = {hashPath(buf), u32(i)};
                        }
                        jg.done();
                    };
                    pool->submit(task);
                }
                jg.wait();
            } else {
                std::string buf;
                for (size_t i = 0; i < n; ++i) {
                    if (entries[i].path.empty()) {
                        flat[i] = {~0ULL, u32(i)};
                        continue;
                    }
                    normalizeInto(entries[i].path, buf);
                    flat[i] = {hashPath(buf), u32(i)};
                }
            }
            std::sort(flat.begin(), flat.end());
            res.add("index", "path D: parallel hash + sorted vector", msSince(t), 0, flat.size());
        }

        // --- fdid index, variant A: current implementation ---
        {
            auto t = Clock::now();
            std::unordered_multimap<u32, size_t> m;
            m.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                if (entries[i].fileDataId != kInvalidFileDataId)
                    m.emplace(entries[i].fileDataId, i);
            }
            res.add("index", "fdid A: multimap<u32> (current)", msSince(t), 0, m.size());
        }

        // --- fdid index, variant B: sorted flat vector ---
        {
            auto t = Clock::now();
            std::vector<std::pair<u32, u32>> flat;
            flat.resize(n);
            for (size_t i = 0; i < n; ++i)
                flat[i] = {entries[i].fileDataId, u32(i)};
            std::sort(flat.begin(), flat.end());
            res.add("index", "fdid B: sorted flat vector", msSince(t), 0, flat.size());
        }
    }
}

// ---------------------------------------------------------------------------
// Lookup-selection dump
//
// Differential-testing aid: records what the root actually resolves for every
// FileDataId and a sample of paths, so a change to the index structures can be
// proven to preserve selection behaviour byte for byte.
// ---------------------------------------------------------------------------

std::string hex16(const std::array<u8, 16>& k) {
    static constexpr char h[] = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (u8 b : k) {
        s += h[b >> 4];
        s += h[b & 0xF];
    }
    return s;
}

void dumpSelection(const Storage& storage, const std::string& outPath) {
    std::vector<i32> fdids;
    std::vector<std::string> paths;
    fdids.reserve(4u << 20);
    storage.enumerate([&](const EnumerateEntry& e) {
        if (e.fileDataId != kInvalidId)
            fdids.push_back(e.fileDataId);
        if (!e.path.empty())
            paths.emplace_back(e.path);
        return true;
    });
    std::sort(fdids.begin(), fdids.end());
    fdids.erase(std::unique(fdids.begin(), fdids.end()), fdids.end());
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    std::ofstream out(outPath, std::ios::binary);
    out << "# fdids=" << fdids.size() << " paths=" << paths.size() << "\n";
    for (i32 id : fdids) {
        auto info = storage.fileInfo(id);
        out << "F " << id << ' ';
        if (info)
            out << hex16(info->cKey) << ' ' << info->localeFlags << ' ' << info->contentFlags << ' '
                << info->fileSize;
        else
            out << "-";
        out << '\n';
    }
    constexpr size_t kPathStride = 31;
    for (size_t i = 0; i < paths.size(); i += kPathStride) {
        auto info = storage.fileInfo(paths[i]);
        out << "P " << paths[i] << ' ';
        if (info)
            out << hex16(info->cKey) << ' ' << info->fileSize;
        else
            out << "-";
        out << '\n';
    }
    std::cout << "dumped " << fdids.size() << " fdids and " << (paths.size() / kPathStride + 1)
              << " path samples to " << outPath << "\n";
}

// ---------------------------------------------------------------------------
// Index verification
//
// Order-insensitive correctness check: enumerate() is the ground truth for what
// the root contains, so every FileDataId and path it reports must resolve, and
// must resolve to one of the entries actually carrying that key.
// ---------------------------------------------------------------------------

/// Hammer the read-only query APIs from every thread at once, with all threads
/// released by the same flag so they collide inside the lazy index build rather
/// than trickling in one at a time.
int stressConcurrentQueries(const Storage& storage, int threads, int rounds) {
    std::vector<std::pair<i32, std::string>> sample;
    storage.enumerate([&](const EnumerateEntry& e) {
        if (sample.size() >= 4096)
            return false;
        if (e.fileDataId != kInvalidId && !e.path.empty())
            sample.emplace_back(e.fileDataId, std::string(e.path));
        return true;
    });
    if (sample.empty()) {
        std::cout << "  no sample entries — skipped\n";
        return 0;
    }

    // Nothing is looked up before the fan-out, so the very first findByPath is
    // itself contended — that is the point. Every thread answers the whole
    // sample independently and the answers are compared afterwards: a stable
    // disagreement with enumerate() shows up identically in all of them, while
    // a race shows up as threads disagreeing with each other.
    std::atomic<bool> go{false};
    std::atomic<size_t> queries{0};
    std::vector<std::vector<std::string>> answers{size_t(threads), {}};

    std::vector<std::thread> ts;
    ts.reserve(size_t(threads));
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t]() {
            auto& mine = answers[size_t(t)];
            mine.reserve(sample.size() * 2);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int r = 0; r < rounds; ++r) {
                for (size_t k = 0; k < sample.size(); ++k) {
                    auto& [id, path] = sample[k];
                    auto byId = storage.fileInfo(id);
                    auto byPath = storage.fileInfo(path);
                    queries.fetch_add(2, std::memory_order_relaxed);
                    if (r == 0) {
                        mine.push_back(byId ? hex16(byId->cKey) : std::string());
                        mine.push_back(byPath ? hex16(byPath->cKey) : std::string());
                    }
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : ts)
        th.join();

    size_t divergent = 0;
    for (size_t t = 1; t < answers.size(); ++t) {
        for (size_t k = 0; k < answers[0].size() && k < answers[t].size(); ++k) {
            if (answers[t][k] != answers[0][k])
                ++divergent;
        }
    }

    std::cout << "  " << queries.load() << " concurrent queries across " << threads
              << " threads, " << divergent << " answers diverged between threads\n";
    return divergent == 0 ? 0 : 1;
}

int verifyIndices(const Storage& storage) {
    std::unordered_map<i32, std::vector<std::string>> byId;
    std::unordered_map<std::string, std::vector<std::string>> byPath;

    storage.enumerate([&](const EnumerateEntry& e) {
        if (e.fileDataId != kInvalidId)
            byId[e.fileDataId].push_back(hex16(e.cKey));
        if (!e.path.empty())
            byPath[std::string(e.path)].push_back(hex16(e.cKey));
        return true;
    });

    size_t missingId = 0, wrongId = 0, missingPath = 0, wrongPath = 0;

    for (auto& [id, cKeys] : byId) {
        auto info = storage.fileInfo(id);
        if (!info) {
            ++missingId;
            continue;
        }
        if (std::find(cKeys.begin(), cKeys.end(), hex16(info->cKey)) == cKeys.end())
            ++wrongId;
    }
    for (auto& [path, cKeys] : byPath) {
        auto info = storage.fileInfo(path);
        if (!info) {
            ++missingPath;
            continue;
        }
        if (std::find(cKeys.begin(), cKeys.end(), hex16(info->cKey)) == cKeys.end())
            ++wrongPath;
    }

    std::cout << "  FileDataIds: " << byId.size() << " checked, " << missingId << " unresolved, "
              << wrongId << " resolved outside their entry set\n"
              << "  Paths:       " << byPath.size() << " checked, " << missingPath
              << " unresolved, " << wrongPath << " resolved outside their entry set\n";

    bool const ok = missingId == 0 && wrongId == 0 && missingPath == 0 && wrongPath == 0;
    std::cout << (ok ? "  VERIFY OK\n" : "  VERIFY FAILED\n");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// End-to-end matrix
// ---------------------------------------------------------------------------

struct MatrixCase {
    std::string name;
    u32 flags;
    bool withListfile;
    bool withPool;
};

void runMatrix(const std::string& path, const std::vector<u8>& listfile, int reps,
               interfaces::WorkerPool* pool, bool traceSteps, bool countProgress, Results& res) {
    std::vector<MatrixCase> cases = {
        {"baseline (no pool, no listfile)", StorageFeatureFlags::None, false, false},
        {"pool, no listfile", StorageFeatureFlags::None, false, true},
        {"pool + listfile", StorageFeatureFlags::None, true, true},
        {"no pool + listfile", StorageFeatureFlags::None, true, false},
        {"pool, LoadOnDemand", StorageFeatureFlags::LoadOnDemand, false, true},
        {"pool, LazyIdxBuckets", StorageFeatureFlags::LazyIdxBuckets, false, true},
        {"pool, LazyArchiveIndex", StorageFeatureFlags::LazyArchiveIndex, false, true},
        {"pool, LazyEncodingFrames", StorageFeatureFlags::LazyEncodingFrames, false, true},
        {"pool, LazyIdx+LazyArchive", StorageFeatureFlags::LazyIdxBuckets |
                                          StorageFeatureFlags::LazyArchiveIndex,
         false, true},
        {"pool, LazyIdx+LazyEncoding", StorageFeatureFlags::LazyIdxBuckets |
                                           StorageFeatureFlags::LazyEncodingFrames,
         false, true},
        {"pool, LazyArchive+LazyEncoding", StorageFeatureFlags::LazyArchiveIndex |
                                               StorageFeatureFlags::LazyEncodingFrames,
         false, true},
        {"pool, all lazy flags", StorageFeatureFlags::LazyIdxBuckets |
                                     StorageFeatureFlags::LazyArchiveIndex |
                                     StorageFeatureFlags::LazyEncodingFrames,
         false, true},
        {"pool, all lazy + listfile", StorageFeatureFlags::LazyIdxBuckets |
                                          StorageFeatureFlags::LazyArchiveIndex |
                                          StorageFeatureFlags::LazyEncodingFrames,
         true, true},
    };

    for (auto& c : cases) {
        if (c.withListfile && listfile.empty())
            continue;
        for (int r = 0; r < reps; ++r) {
            std::cout << "  [" << c.name << "] rep " << (r + 1) << "/" << reps << std::flush;
            OpenOptions opts;
            opts.path = path;
            opts.flags = c.flags;
            opts.pool = c.withPool ? pool : nullptr;
            if (c.withListfile)
                opts.listfile = std::span<const u8>(listfile);
            auto stepClock = std::make_shared<Clock::time_point>(Clock::now());
            // Measures what the instrumentation itself costs: a callback that
            // does nothing but count, so the delta against a run without one is
            // the reporting overhead.
            auto eventCount = std::make_shared<std::atomic<u64>>(0);
            if (countProgress) {
                opts.progressCallback = [eventCount](const ProgressInfo&) {
                    eventCount->fetch_add(1, std::memory_order_relaxed);
                    return true;
                };
            }
            if (traceSteps) {
                opts.progressCallback = [stepClock](const ProgressInfo& info) {
                    // Step timings come from the End events; Update samples only
                    // move the counters within a step.
                    if (info.state == ProgressState::Update)
                        return true;
                    if (info.state == ProgressState::Begin)
                        return true;
                    std::cerr << "\n      " << std::left << std::setw(24)
                              << progressStepName(info.step) << std::right << std::setw(8)
                              << std::fixed << std::setprecision(1) << msSince(*stepClock)
                              << " ms";
                    if (info.total > 0)
                        std::cerr << "  (" << info.current << "/" << info.total << ")";
                    std::cerr << std::flush;
                    *stepClock = Clock::now();
                    return true;
                };
            }
            auto t = Clock::now();
            auto storage = Storage::open(opts);
            double const ms = msSince(t);
            if (!storage) {
                std::cout << "  -> FAILED\n";
                break;
            }
            std::cout << "  -> " << std::fixed << std::setprecision(1) << ms << " ms";
            if (countProgress)
                std::cout << "  (" << eventCount->load() << " progress events)";
            std::cout << "\n" << std::flush;
            res.add("matrix", c.name, ms, 0, 0);
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::string cascPath;
    std::string listfilePath;
    std::string csvPath;
    std::string dumpPath;
    int reps = 3;
    int threads = int(std::thread::hardware_concurrency());
    bool doPhases = true, doMatrix = true, doIndexExp = false, traceSteps = false;
    bool countProgress = false;
    bool doVerify = false;
    bool doStress = false;
    long long singleFlags = -1;
    bool singleNoPool = false;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--listfile" && i + 1 < argc)
            listfilePath = argv[++i];
        else if (a == "--reps" && i + 1 < argc)
            reps = std::atoi(argv[++i]);
        else if (a == "--threads" && i + 1 < argc)
            threads = std::atoi(argv[++i]);
        else if (a == "--csv" && i + 1 < argc)
            csvPath = argv[++i];
        else if (a == "--single" && i + 1 < argc)
            singleFlags = u32(std::stoul(argv[++i], nullptr, 0));
        else if (a == "--nopool")
            singleNoPool = true;
        else if (a == "--index-experiment")
            doIndexExp = true;
        else if (a == "--trace-steps")
            traceSteps = true;
        else if (a == "--progress-cost")
            countProgress = true;
        else if (a == "--dump" && i + 1 < argc)
            dumpPath = argv[++i];
        else if (a == "--verify")
            doVerify = true;
        else if (a == "--stress")
            doStress = true;
        else if (a == "--no-phases")
            doPhases = false;
        else if (a == "--no-matrix")
            doMatrix = false;
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0] << " <casc_path> [options]\n\n"
                      << "  --listfile <path>   community listfile (FileDataId;path per line)\n"
                      << "  --reps N            repetitions per measurement (default 3)\n"
                      << "  --threads N         worker-pool threads (default hardware_concurrency)\n"
                      << "  --csv <path>        write results as CSV\n"
                      << "  --no-phases         skip the instrumented phase breakdown\n"
                      << "  --no-matrix         skip the end-to-end feature-flag matrix\n"
                      << "  --index-experiment  compare root index-build strategies\n"
                      << "  --single <flags>    run one open with the given feature flags\n"
                      << "  --nopool            with --single: open without a worker pool\n"
                      << "  --trace-steps       print each ProgressStep during open\n"
                      << "  --progress-cost     attach a counting no-op progress callback\n";
            return 0;
        } else if (cascPath.empty())
            cascPath = a;
    }

    if (cascPath.empty() || !fs::exists(cascPath)) {
        std::cerr << "Error: pass a valid CASC storage path.\n";
        return 1;
    }
    if (threads < 1)
        threads = 1;

    auto paths = resolvePaths(cascPath);
    std::cout << "CASC storage : " << paths.base << "\n";
    std::cout << "Data dir     : " << paths.data << "\n";
    std::cout << "Threads      : " << threads << "\n";
    std::cout << "Reps         : " << reps << "\n";

    std::vector<u8> listfile;
    if (!listfilePath.empty()) {
        auto t = Clock::now();
        if (auto d = storages::common::readFileFully(listfilePath)) {
            listfile = std::move(*d);
            std::cout << "Listfile     : " << listfilePath << "  ("
                      << (double(listfile.size()) / (1024.0 * 1024.0)) << " MB, read in "
                      << std::fixed << std::setprecision(1) << msSince(t) << " ms)\n";
        } else {
            std::cerr << "Warning: cannot read listfile " << listfilePath << "\n";
        }
    }

    utils::SimpleThreadPool pool{size_t(threads)};
    Results res;

    if (doVerify) {
        OpenOptions o;
        o.path = cascPath;
        o.pool = &pool;
        if (!listfile.empty())
            o.listfile = std::span<const u8>(listfile);
        auto s = Storage::open(o);
        if (!s) {
            std::cerr << "open failed\n";
            return 1;
        }
        std::cout << "\n=== Index verification ===\n";
        return verifyIndices(*s);
    }

    if (doStress) {
        std::cout << "\n=== Concurrency stress ===\n";
        int failures = 0;
        for (int round = 0; round < reps; ++round) {
            OpenOptions o;
            o.path = cascPath;
            o.pool = &pool;
            if (!listfile.empty())
                o.listfile = std::span<const u8>(listfile);
            auto s = Storage::open(o);
            if (!s) {
                std::cerr << "open failed\n";
                return 1;
            }
            std::cout << "round " << (round + 1) << "/" << reps << ":\n";
            failures += stressConcurrentQueries(*s, threads, 2);
        }
        std::cout << (failures ? "  STRESS FAILED\n" : "  STRESS OK\n");
        return failures ? 1 : 0;
    }

    if (!dumpPath.empty()) {
        OpenOptions o;
        o.path = cascPath;
        // --nopool makes the dump comparable against the single-threaded open,
        // which is the path the pooled one has to agree with.
        o.pool = singleNoPool ? nullptr : &pool;
        if (!listfile.empty())
            o.listfile = std::span<const u8>(listfile);
        auto s = Storage::open(o);
        if (!s) {
            std::cerr << "open failed\n";
            return 1;
        }
        dumpSelection(*s, dumpPath);
        return 0;
    }

    if (singleFlags >= 0) {
        OpenOptions o;
        o.path = cascPath;
        o.flags = u32(singleFlags);
        o.pool = singleNoPool ? nullptr : &pool;
        if (!listfile.empty())
            o.listfile = std::span<const u8>(listfile);
        std::string err;
        o.errorOut = &err;
        auto stepClock = std::make_shared<Clock::time_point>(Clock::now());
        if (traceSteps) {
            o.progressCallback = [stepClock](const ProgressInfo& info) {
                if (info.state != ProgressState::End)
                    return true;
                std::cout << "  " << std::left << std::setw(24) << progressStepName(info.step)
                          << std::right << std::setw(9) << std::fixed << std::setprecision(1)
                          << msSince(*stepClock) << " ms";
                if (info.total > 0)
                    std::cout << "  (" << info.current << "/" << info.total << ")";
                std::cout << "\n" << std::flush;
                *stepClock = Clock::now();
                return true;
            };
        }
        auto t = Clock::now();
        auto s = Storage::open(o);
        std::cout << "single open flags=0x" << std::hex << singleFlags << std::dec
                  << " pool=" << (singleNoPool ? "no" : "yes") << " -> "
                  << (s ? "OK" : "FAILED") << " in " << std::fixed << std::setprecision(1)
                  << msSince(t) << " ms  " << err << "\n";
        return s ? 0 : 1;
    }

    if (doIndexExp) {
        std::cout << "\n=== Index-build experiment (WowTvfsRoot::buildIndex alternatives) ===\n";
        PhaseOpts o;
        o.pool = &pool;
        o.indexExperiment = true;
        instrumentedOpen(paths, o, res, "phases_exp");
        res.print("index", "Index build — encoded TVFS paths (no listfile)");
        if (!listfile.empty()) {
            Results res2;
            PhaseOpts o2;
            o2.pool = &pool;
            o2.listfile = std::span<const u8>(listfile);
            o2.indexExperiment = true;
            instrumentedOpen(paths, o2, res2, "phases_exp_lf");
            res2.print("index", "Index build — listfile paths");
        }
        return 0;
    }

    if (doPhases) {
        std::cout << "\n=== Phase breakdown (instrumented replica of Storage::open) ===\n";
        for (int r = 0; r < reps; ++r) {
            PhaseOpts o;
            o.pool = &pool;
            o.reportVfsShape = (r == 0);
            instrumentedOpen(paths, o, res, "phases_pool");
        }
        res.print("phases_pool", "Phases — worker pool, no listfile");

        for (int r = 0; r < reps; ++r) {
            PhaseOpts o;
            o.pool = &pool;
            o.listfile = std::span<const u8>(listfile);
            instrumentedOpen(paths, o, res, "phases_pool_lf");
        }
        if (!listfile.empty())
            res.print("phases_pool_lf", "Phases — worker pool + listfile");

        for (int r = 0; r < reps; ++r) {
            PhaseOpts o;
            instrumentedOpen(paths, o, res, "phases_serial");
        }
        res.print("phases_serial", "Phases — single-threaded (no pool), no listfile");
    }

    if (doMatrix) {
        std::cout << "\n=== End-to-end Storage::open matrix ===\n";
        runMatrix(cascPath, listfile, reps, &pool, traceSteps, countProgress, res);
        res.print("matrix", "Storage::open — end to end");
    }

    if (!csvPath.empty()) {
        res.writeCsv(csvPath);
        std::cout << "\nCSV written to " << csvPath << "\n";
    }
    return 0;
}
