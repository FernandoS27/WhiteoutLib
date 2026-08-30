// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// readBatch container-dedupe check, written against Diablo IV.
///
/// D4 root entries are slices of shared combined-meta containers, so one batch
/// can name the same multi-megabyte blob hundreds of times. readBatch decodes
/// once per distinct blob and fans the result out; this verifies every request
/// still gets the bytes readFile would have returned, and reports the timings
/// that make the dedupe visible.
///
/// Usage:
///   casc_d4_batch_probe [casc_path] [--sample N] [--chunk N] [--threads N]
///                       [--batch-first] [--no-verify]

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/blizzard_game_finder.h>
#include <whiteout/utils/simple_thread_pool.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;
using Clock = std::chrono::steady_clock;

namespace {

struct Sample {
    std::string path;
    i32 fileDataId = kInvalidId;
};

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string keyHex(const std::array<u8, 16>& k) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    for (u8 b : k) {
        s.push_back(kHex[b >> 4]);
        s.push_back(kHex[b & 0xF]);
    }
    return s;
}

std::string label(const Sample& e) {
    return e.path.empty() ? std::to_string(e.fileDataId) : e.path;
}

std::optional<std::vector<u8>> readOne(const Storage& s, const Sample& e) {
    return e.path.empty() ? s.readFile(e.fileDataId) : s.readFile(e.path);
}

std::vector<BatchReadRequest> makeRequests(const std::vector<Sample>& sample) {
    std::vector<BatchReadRequest> reqs;
    reqs.reserve(sample.size());
    for (auto& s : sample) {
        BatchReadRequest r;
        if (!s.path.empty())
            r.path = s.path;
        else
            r.fileDataId = s.fileDataId;
        reqs.push_back(std::move(r));
    }
    return reqs;
}

struct Timing {
    double ms = 0.0;
    u64 bytes = 0;
    u32 ok = 0;
};

Timing runSerial(const Storage& s, const std::vector<Sample>& sample) {
    Timing t;
    auto t0 = Clock::now();
    for (auto& e : sample) {
        if (auto data = readOne(s, e)) {
            t.bytes += data->size();
            ++t.ok;
        }
    }
    t.ms = msSince(t0);
    return t;
}

Timing runBatch(const Storage& s, const std::vector<BatchReadRequest>& reqs, size_t chunk) {
    Timing t;
    auto t0 = Clock::now();
    for (size_t off = 0; off < reqs.size(); off += chunk) {
        size_t const n = std::min(chunk, reqs.size() - off);
        auto out = s.readBatch(std::span<const BatchReadRequest>(reqs.data() + off, n));
        for (auto& r : out) {
            if (r.success) {
                t.bytes += r.data.size();
                ++t.ok;
            }
        }
    }
    t.ms = msSince(t0);
    return t;
}

void report(const char* text, const Timing& t) {
    double const mbs = t.bytes / (1024.0 * 1024.0);
    std::cout << "  " << std::left << std::setw(30) << text << std::right << std::fixed
              << std::setprecision(1) << std::setw(10) << t.ms << " ms " << std::setw(6) << t.ok
              << " ok " << std::setw(8) << mbs << " MB " << std::setw(8) << (mbs / (t.ms / 1000.0))
              << " MB/s\n";
}

/// Every request in a batch must get exactly what a single read returns —
/// including the ones that shared a container with an earlier request, which
/// are the ones the dedupe fans out rather than decodes.
int verify(const Storage& s, const std::vector<Sample>& sample,
           const std::vector<BatchReadRequest>& reqs) {
    auto batch = s.readBatch(reqs);
    if (batch.size() != sample.size()) {
        std::cout << "  MISMATCH: readBatch returned " << batch.size() << " results for "
                  << sample.size() << " requests\n";
        return 1;
    }

    size_t mismatches = 0, batchFailures = 0, batchOnly = 0, compared = 0;
    for (size_t i = 0; i < sample.size(); ++i) {
        auto single = readOne(s, sample[i]);
        if (!single && !batch[i].success)
            continue;
        if (!batch[i].success) {
            ++batchFailures;
            continue;
        }
        if (!single) {
            ++batchOnly;
            continue;
        }
        ++compared;
        if (*single != batch[i].data) {
            if (mismatches < 5) {
                std::cout << "  MISMATCH " << label(sample[i]) << ": readFile " << single->size()
                          << " B vs readBatch " << batch[i].data.size() << " B\n";
            }
            ++mismatches;
        }
    }

    std::cout << "  compared " << compared << ", mismatches " << mismatches << ", batch-only "
              << batchOnly << ", batch failures " << batchFailures << "\n";
    return (mismatches == 0 && batchFailures == 0) ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string path;
    size_t sampleSize = 2000;
    size_t chunk = 32;
    unsigned threads = 0;
    bool batchFirst = false;
    bool doVerify = true;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--sample" && i + 1 < argc)
            sampleSize = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--chunk" && i + 1 < argc)
            chunk = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--threads" && i + 1 < argc)
            threads = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--batch-first")
            batchFirst = true;
        else if (a == "--no-verify")
            doVerify = false;
        else if (!a.empty() && a[0] != '-')
            path = a;
    }

    if (path.empty()) {
        for (auto& g : utils::findBlizzardGames()) {
            if (g.name.find("Diablo IV") != std::string::npos) {
                path = g.path;
                break;
            }
        }
    }
    if (path.empty()) {
        std::cerr << "No Diablo IV install found; pass a path.\n";
        return 1;
    }

    if (threads == 0)
        threads = std::max(2u, std::thread::hardware_concurrency());
    auto pool = std::make_unique<utils::SimpleThreadPool>(threads);

    std::cout << "Storage: " << path << "\nThreads: " << threads << "\n";

    auto t0 = Clock::now();
    auto storage = Storage::open(path, pool.get());
    if (!storage) {
        std::cerr << "open failed: " << Storage::lastError() << "\n";
        return 1;
    }
    std::cout << "Open:    " << std::fixed << std::setprecision(1) << msSince(t0) << " ms\n";

    std::vector<Sample> all;
    storage->enumerate([&](const EnumerateEntry& e) {
        all.push_back({std::string(e.path), e.fileDataId});
        return true;
    });

    std::vector<Sample> sample;
    size_t const stride = std::max<size_t>(1, all.size() / sampleSize);
    for (size_t i = 0; i < all.size() && sample.size() < sampleSize; i += stride)
        sample.push_back(all[i]);
    std::cout << "Entries: " << all.size() << ", sample " << sample.size() << " (stride " << stride
              << ")\n";

    // A container sub-entry carries its container's EKey, so a repeated EKey
    // means a shared container — the redundancy readBatch collapses.
    std::unordered_map<std::string, u32> byEKey;
    u32 noInfo = 0;
    for (auto& e : sample) {
        auto info = e.path.empty() ? storage->fileInfo(e.fileDataId) : storage->fileInfo(e.path);
        if (!info) {
            ++noInfo;
            continue;
        }
        ++byEKey[keyHex(info->eKey)];
    }
    std::map<u32, u32> histogram;
    u32 maxShare = 0;
    for (auto& [k, n] : byEKey) {
        ++histogram[n];
        maxShare = std::max(maxShare, n);
    }
    std::cout << "\nDistinct EKeys in sample: " << byEKey.size() << " for " << sample.size()
              << " requests (" << noInfo << " without info)\n";
    std::cout << "Redundancy factor: " << std::setprecision(2)
              << (byEKey.empty() ? 0.0 : double(sample.size() - noInfo) / double(byEKey.size()))
              << "x, max files sharing one blob: " << maxShare << "\n";
    std::cout << "Files-per-blob histogram (count of blobs shared by N files):\n";
    for (auto& [n, blobs] : histogram)
        std::cout << "    " << std::setw(6) << n << " file(s): " << blobs << " blob(s)\n";

    auto reqs = makeRequests(sample);

    int rc = 0;
    if (doVerify) {
        std::cout << "\nVerification (readBatch vs readFile, byte for byte):\n";
        rc = verify(*storage, sample, reqs);
    }

    std::cout << "\nTimings (each step inherits whatever the previous one cached):\n";
    if (batchFirst) {
        report("readBatch (whole, COLD)", runBatch(*storage, reqs, reqs.size()));
        report("readBatch (whole, 2nd)", runBatch(*storage, reqs, reqs.size()));
    }
    report("serial readFile", runSerial(*storage, sample));
    report("serial readFile (warm)", runSerial(*storage, sample));
    report("readBatch (whole)", runBatch(*storage, reqs, reqs.size()));
    report("readBatch (whole, again)", runBatch(*storage, reqs, reqs.size()));
    std::string const chunkLabel = "readBatch (chunk " + std::to_string(chunk) + ")";
    report(chunkLabel.c_str(), runBatch(*storage, reqs, chunk));
    report("readBatch (chunk 1)", runBatch(*storage, reqs, 1));

    return rc;
}
