// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file casc_read_bench.cpp
/// @brief Open-then-read benchmark: Storage::open with a listfile, followed by
///        reading a few randomly chosen .m2 files by path.
///
/// Deliberately restricted to the public API so the same source builds against
/// an unoptimised tree for A/B comparison.
///
/// Usage: casc_read_bench <casc_path> --listfile <path> [options]

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_thread_pool.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static std::vector<u8> readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    auto const size = f.tellg();
    f.seekg(0);
    std::vector<u8> data(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

/// Collect every `.m2` path from a `fdid;path` listfile.
static std::vector<std::string> collectM2Paths(const std::vector<u8>& listfile) {
    std::vector<std::string> paths;
    const char* p = reinterpret_cast<const char*>(listfile.data());
    const char* end = p + listfile.size();

    while (p < end) {
        const char* lineEnd = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
        if (!lineEnd)
            lineEnd = end;

        const char* sep = static_cast<const char*>(std::memchr(p, ';', size_t(lineEnd - p)));
        if (sep) {
            const char* pathStart = sep + 1;
            const char* pathEnd = lineEnd;
            if (pathEnd > pathStart && pathEnd[-1] == '\r')
                --pathEnd;
            size_t const len = size_t(pathEnd - pathStart);
            if (len > 3 && std::equal(pathEnd - 3, pathEnd, ".m2"))
                paths.emplace_back(pathStart, len);
        }
        p = (lineEnd == end) ? end : lineEnd + 1;
    }
    return paths;
}

struct RepResult {
    double openMs = 0;
    double readMs = 0;
    size_t filesRead = 0;
    size_t bytesRead = 0;
};

static double median(std::vector<double> v) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

int main(int argc, char** argv) {
    std::string cascPath;
    std::string listfilePath;
    int reps = 5;
    int filesPerRep = 3;
    unsigned seed = 1;
    int threads = int(std::thread::hardware_concurrency());
    bool noPool = false;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--listfile" && i + 1 < argc)
            listfilePath = argv[++i];
        else if (a == "--reps" && i + 1 < argc)
            reps = std::atoi(argv[++i]);
        else if (a == "--files" && i + 1 < argc)
            filesPerRep = std::atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc)
            seed = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--threads" && i + 1 < argc)
            threads = std::atoi(argv[++i]);
        else if (a == "--nopool")
            noPool = true;
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0] << " <casc_path> --listfile <path> [options]\n"
                      << "  --reps N      repetitions (default 5)\n"
                      << "  --files N     .m2 files read per repetition (default 3)\n"
                      << "  --seed S      RNG seed; same seed picks the same files (default 1)\n"
                      << "  --threads N   worker-pool threads\n"
                      << "  --nopool      open without a worker pool\n";
            return 0;
        } else if (cascPath.empty())
            cascPath = a;
    }

    if (cascPath.empty()) {
        std::cerr << "usage: " << argv[0] << " <casc_path> [--listfile <path>]\n";
        return 2;
    }

    // Without a listfile the candidates come from enumerate() instead, which is
    // what non-WoW products (D4, D3, OW) need — they have no community listfile
    // and no .m2 files.
    std::vector<u8> listfile;
    std::vector<std::string> m2Paths;
    if (!listfilePath.empty()) {
        listfile = readWholeFile(listfilePath);
        if (listfile.empty()) {
            std::cerr << "failed to read listfile: " << listfilePath << "\n";
            return 1;
        }
        m2Paths = collectM2Paths(listfile);
        if (m2Paths.empty()) {
            std::cerr << "no .m2 paths in listfile\n";
            return 1;
        }
    }

    std::cout << "CASC storage : " << cascPath << "\n"
              << "Listfile     : "
              << (listfilePath.empty()
                      ? std::string("none — candidates drawn from enumerate()")
                      : listfilePath + "  (" + std::to_string(m2Paths.size()) + " .m2 paths)")
              << "\n"
              << "Pool         : " << (noPool ? "none" : std::to_string(threads) + " threads")
              << "\n"
              << "Reps         : " << reps << ", " << filesPerRep << " files each, seed " << seed
              << "\n\n";

    utils::SimpleThreadPool pool{size_t(threads)};
    std::cout << std::fixed;

    std::vector<RepResult> results;
    for (int rep = 0; rep < reps; ++rep) {
        // Same seed sequence in every build, so both sides read the same files.
        std::mt19937 rng(seed + unsigned(rep));
        std::vector<std::string> candidates;
        if (!m2Paths.empty()) {
            std::uniform_int_distribution<size_t> pick(0, m2Paths.size() - 1);
            candidates.reserve(64);
            for (int i = 0; i < 64; ++i)
                candidates.push_back(m2Paths[pick(rng)]);
        }

        RepResult r;

        OpenOptions opts;
        opts.path = cascPath;
        opts.pool = noPool ? nullptr : &pool;
        if (!listfile.empty())
            opts.listfile = std::span<const u8>(listfile);
        std::string err;
        opts.errorOut = &err;

        auto t0 = Clock::now();
        auto storage = Storage::open(opts);
        r.openMs = msSince(t0);
        if (!storage) {
            std::cerr << "open failed: " << err << "\n";
            return 1;
        }

        if (candidates.empty()) {
            // Untimed: enumeration is not part of the workload being compared,
            // it only supplies the candidate paths a listfile would have.
            std::vector<std::string> pathPool;
            storage->enumerate([&](const EnumerateEntry& e) {
                if (!e.path.empty())
                    pathPool.emplace_back(e.path);
                return pathPool.size() < 65536;
            });
            if (!pathPool.empty()) {
                std::uniform_int_distribution<size_t> pick(0, pathPool.size() - 1);
                for (int i = 0; i < 64; ++i)
                    candidates.push_back(pathPool[pick(rng)]);
            }
        }

        // Candidates are drawn blind, so walk them until enough resolve. A miss
        // costs the same lookup in both builds.
        auto t1 = Clock::now();
        for (auto& path : candidates) {
            if (r.filesRead >= size_t(filesPerRep))
                break;
            auto data = storage->readFile(path);
            if (data && !data->empty()) {
                ++r.filesRead;
                r.bytesRead += data->size();
            }
        }
        r.readMs = msSince(t1);

        std::cout << "rep " << (rep + 1) << "/" << reps << "  open " << std::setw(8)
                  << std::setprecision(1) << r.openMs << " ms   read " << std::setw(7) << r.readMs
                  << " ms   (" << r.filesRead << " files, " << std::setprecision(2)
                  << double(r.bytesRead) / 1024.0 << " KB)   total " << std::setprecision(1)
                  << std::setw(8) << (r.openMs + r.readMs) << " ms\n";
        results.push_back(r);
    }

    std::vector<double> opens, reads, totals;
    for (auto& r : results) {
        opens.push_back(r.openMs);
        reads.push_back(r.readMs);
        totals.push_back(r.openMs + r.readMs);
    }

    auto minOf = [](const std::vector<double>& v) {
        return *std::min_element(v.begin(), v.end());
    };

    std::cout << "\n" << std::string(64, '-') << "\n"
              << std::setprecision(1) << "open   min " << std::setw(8) << minOf(opens) << "   med "
              << std::setw(8) << median(opens) << "\n"
              << "read   min " << std::setw(8) << minOf(reads) << "   med " << std::setw(8)
              << median(reads) << "\n"
              << "TOTAL  min " << std::setw(8) << minOf(totals) << "   med " << std::setw(8)
              << median(totals) << "\n";
    return 0;
}
