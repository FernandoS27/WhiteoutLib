// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// CASC loader example: demonstrates opening a CASC storage and reading files
/// using the new pure-C++ CASC implementation.

#include <whiteout/storages/casc/storage.h>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

using namespace whiteout;
using namespace whiteout::storages::casc;

static int g_passed = 0;
static int g_failed = 0;

static void check(bool condition, const std::string& name) {
    if (condition) {
        std::cout << "  PASS: " << name << "\n";
        ++g_passed;
    } else {
        std::cout << "  FAIL: " << name << "\n";
        ++g_failed;
    }
}

static std::string hexStr(const std::array<u8, 16>& key) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (auto b : key) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0xF]);
    }
    return s;
}

static std::string findCorpus() {
    for (auto& p : {"Corpus/CASC", "../Corpus/CASC", "../../Corpus/CASC",
                     "C:/Projects/WhiteoutLib/Corpus/CASC"}) {
        if (std::filesystem::exists(p))
            return p;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Test: open a storage and exercise the read API
// ---------------------------------------------------------------------------
static void testStorage(const std::string& label, const std::string& path) {
    std::cout << "\n[Test: " << label << "]\n";
    std::cout << "  Path: " << path << "\n";

    auto storage = Storage::open(path);
    check(storage.has_value(), "Storage::open succeeds");
    if (!storage) {
        std::cout << "  lastError = " << Storage::lastError() << "\n";
        return;
    }
    check(bool(*storage), "Storage is valid");

    // Product info.
    auto prod = storage->product();
    if (prod) {
        std::cout << "  Product: " << prod->name << "  Version: " << prod->version << "\n";
    }

    // Total file count.
    auto count = storage->totalFileCount();
    check(count.has_value(), "totalFileCount succeeds");
    if (count) {
        std::cout << "  Total files: " << *count << "\n";
        check(*count > 0, "has at least one file");
    }

    // Enumerate first 10 entries.
    std::cout << "  First 10 entries:\n";
    size_t n = 0;
    storage->enumerate([&](const EnumerateEntry& fe) {
        if (n < 10) {
            std::cout << "    [" << n << "] "
                      << (fe.path.empty() ? "(id:" + std::to_string(fe.fileDataId) + ")" : std::string(fe.path))
                      << "  size=" << fe.fileSize
                      << "  CKey=" << hexStr(fe.cKey) << "\n";
        }
        ++n;
        return n < 10;
    });
    check(n > 0, "enumerate returned entries");

    // listFiles.
    auto files = storage->listFiles();
    std::cout << "  listFiles: " << files.size() << " paths\n";

    // Try reading.
    bool readOk = false;
    if (!files.empty()) {
        auto data = storage->readFile(files[0]);
        check(data.has_value(), "readFile(\"" + files[0] + "\") succeeds");
        if (data) {
            check(!data->empty(), "file has content (" + std::to_string(data->size()) + " bytes)");
            readOk = true;
        }
    }

    // Try fileExists / fileSize / fileInfo.
    if (!files.empty()) {
        check(storage->fileExists(files[0]), "fileExists for known file");
        check(!storage->fileExists("__nonexistent_path__/__bogus__"),
              "fileExists false for bogus path");

        auto sz = storage->fileSize(files[0]);
        check(sz.has_value(), "fileSize returns value");
        if (sz) std::cout << "  fileSize: " << *sz << "\n";

        auto info = storage->fileInfo(files[0]);
        check(info.has_value(), "fileInfo succeeds");
        if (info) {
            std::cout << "  fileInfo CKey=" << hexStr(info->cKey)
                      << "  EKey=" << hexStr(info->eKey)
                      << "  size=" << info->fileSize << "\n";
        }
    }

    // Read a small batch and verify non-empty.
    size_t readCount = 0, readMax = std::min<size_t>(5, files.size());
    for (size_t i = 0; i < readMax; ++i) {
        auto data = storage->readFile(files[i]);
        if (data && !data->empty()) {
            ++readCount;
        } else {
            std::cout << "  batch-read fail: " << files[i] << "\n";
        }
    }
    // Allow partial success — locale sub-files in D3 may point to archives
    // absent from a partial corpus snapshot.  Require at least 2 readable files
    // (the first entry + at least one more) or all of them if readMax <= 2.
    size_t readMin = (readMax <= 2) ? readMax : 2;
    check(readCount >= readMin,
          "batch-read " + std::to_string(readCount) + "/" + std::to_string(readMax) + " files OK (min " + std::to_string(readMin) + ")");
}

int main() {
    std::cout << "=== CASC Loader Example (Phase 4) ===\n";

    auto corpus = findCorpus();
    if (corpus.empty()) {
        std::cout << "Corpus not found — skipping.\n";
        return 0;
    }

    // Diablo III corpus.
    if (std::filesystem::exists(corpus + "/Diablo III"))
        testStorage("Diablo III", corpus + "/Diablo III");

    // Warcraft III Reforged corpus.
    if (std::filesystem::exists(corpus + "/Warcraft III"))
        testStorage("Warcraft III Reforged", corpus + "/Warcraft III");

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}
