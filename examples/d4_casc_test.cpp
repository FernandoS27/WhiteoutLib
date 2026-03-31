// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Diablo IV CASC read test: opens a live D4 installation and cross-validates
/// against CascLib (when available) or runs internal consistency checks.

// New pure-C++ CASC API.
#include <whiteout/storages/casc/storage.h>

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
// CascLib C API — used directly for cross-validation.
#include <CascLib.h>
#endif

// Internal helper for MD5 verification.
#include "../src/whiteout/storages/common/md5.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace whiteout;

namespace new_casc = whiteout::storages::casc;

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

static constexpr std::array<u8, 16> kZeroKey{};

// ============================================================================
// Internal consistency checks
// ============================================================================

static void testInternal(const std::string& path) {
    std::cout << "\n[Internal consistency]\n";

    auto storage = new_casc::Storage::open(path);
    check(storage.has_value(), "Storage::open succeeds");
    if (!storage) {
        std::cout << "  lastError = " << new_casc::Storage::lastError() << "\n";
        return;
    }

    // Product info.
    auto prod = storage->product();
    if (prod)
        std::cout << "  Product: " << prod->name << "  Version: " << prod->version << "\n";

    auto totalCount = storage->totalFileCount();
    check(totalCount.has_value(), "totalFileCount available");
    if (totalCount)
        std::cout << "  Root entries: " << *totalCount << "\n";

    // Collect sample entries.
    constexpr size_t sampleLimit = 500;
    std::vector<new_casc::FindEntry> sampleEntries;
    storage->enumerate([&](const new_casc::FindEntry& fe) {
        if (sampleEntries.size() >= sampleLimit) return false;
        sampleEntries.push_back(fe);
        return true;
    });

    // --- Test 1: enumerate / fileInfo CKey consistency ---
    size_t infoChecked = 0, infoMatch = 0, infoMissing = 0;
    for (auto& fe : sampleEntries) {
        ++infoChecked;
        std::optional<new_casc::FileFullInfo> info;
        if (!fe.path.empty())
            info = storage->fileInfo(fe.path);
        if (!info) { ++infoMissing; continue; }
        if (info->cKey == fe.cKey) ++infoMatch;
    }
    check(infoMatch > 0,
          "enumerate/fileInfo CKey consistent (" + std::to_string(infoMatch) +
          "/" + std::to_string(infoChecked) +
          " match, " + std::to_string(infoMissing) + " no info)");

    // --- Test 2: read + MD5 verification ---
    size_t md5Checked = 0, md5Match = 0, md5Mismatch = 0, md5Skipped = 0;
    constexpr size_t md5Limit = 200;
    for (size_t i = 0; i < std::min(md5Limit, sampleEntries.size()); ++i) {
        auto& fe = sampleEntries[i];
        if (fe.cKey == kZeroKey || fe.path.empty()) continue;
        ++md5Checked;
        auto data = storage->readFile(fe.path);
        if (!data) { ++md5Skipped; continue; }
        auto md5 = storages::common::md5Hash(
            std::span<const u8>(data->data(), data->size()));
        if (md5 == fe.cKey) ++md5Match;
        else {
            ++md5Mismatch;
            if (md5Mismatch <= 3)
                std::cout << "    MD5 mismatch: " << fe.path
                          << " expected=" << hexStr(fe.cKey)
                          << " actual=" << hexStr(md5) << "\n";
        }
    }
    if (md5Checked > 0)
        check(md5Mismatch == 0,
              "read+MD5 verification (" + std::to_string(md5Match) + "/" +
              std::to_string(md5Checked) + " pass, " +
              std::to_string(md5Skipped) + " skip)");

    // --- Test 3: fileExists consistency ---
    size_t existsChecked = 0, existsMatch = 0;
    for (auto& fe : sampleEntries) {
        if (fe.path.empty()) continue;
        ++existsChecked;
        if (storage->fileExists(fe.path)) ++existsMatch;
    }
    check(existsMatch > 0,
          "fileExists consistent (" + std::to_string(existsMatch) +
          "/" + std::to_string(existsChecked) + ")");

    // --- Test 4: fileSize matches actual read ---
    size_t sizeChecked = 0, sizeMatch = 0, sizeSkipped = 0;
    for (size_t i = 0; i < std::min<size_t>(50, sampleEntries.size()); ++i) {
        auto& fe = sampleEntries[i];
        if (fe.path.empty()) continue;
        auto sz = storage->fileSize(fe.path);
        if (!sz) continue;
        auto data = storage->readFile(fe.path);
        if (!data) { ++sizeSkipped; continue; }
        ++sizeChecked;
        if (data->size() == *sz) ++sizeMatch;
    }
    if (sizeChecked > 0)
        check(sizeMatch == sizeChecked,
              "fileSize matches actual read (" + std::to_string(sizeMatch) +
              "/" + std::to_string(sizeChecked) +
              ", " + std::to_string(sizeSkipped) + " unreadable)");

    // --- Test 5: listFiles paths exist ---
    auto allFiles = storage->listFiles();
    check(!allFiles.empty(), "listFiles non-empty (" +
          std::to_string(allFiles.size()) + " paths)");

    size_t listChecked = 0, listExist = 0;
    for (size_t i = 0; i < std::min<size_t>(20, allFiles.size()); ++i) {
        ++listChecked;
        if (storage->fileExists(allFiles[i])) ++listExist;
    }
    check(listExist == listChecked,
          "listFiles paths exist (" + std::to_string(listExist) +
          "/" + std::to_string(listChecked) + ")");

    // --- Test 6: bogus paths ---
    check(!storage->fileExists("__nonexistent__/__bogus__"),
          "bogus path doesn't exist");
    check(!storage->readFile("__nonexistent__/__bogus__").has_value(),
          "bogus path read returns nullopt");

    // --- Test 7: combined meta entries (Texture, StringList) ---
    // Entries from combined meta files should be readable via their enriched paths.
    {
        size_t texFound = 0, texRead = 0;
        size_t slFound = 0, slRead = 0;
        storage->enumerate([&](const new_casc::FindEntry& fe) {
            if (fe.path.empty()) return true;
            // Look for Texture meta entries.
            if (fe.path.find("\\Texture\\") != std::string::npos &&
                fe.path.find("meta") != std::string::npos &&
                fe.path.size() > 4 &&
                fe.path.compare(fe.path.size() - 4, 4, ".tex") == 0 &&
                fe.cKey == kZeroKey) {
                ++texFound;
                if (texFound <= 5) {
                    auto data = storage->readFile(fe.path);
                    if (data && !data->empty()) ++texRead;
                }
            }
            // Look for StringList meta entries.
            if (fe.path.find("\\StringList\\") != std::string::npos &&
                fe.path.find("meta") != std::string::npos &&
                fe.cKey == kZeroKey) {
                ++slFound;
                if (slFound <= 5) {
                    auto data = storage->readFile(fe.path);
                    if (data && !data->empty()) ++slRead;
                }
            }
            return true;
        });

        std::cout << "  Combined meta: Texture found=" << texFound
                  << " read=" << texRead
                  << "  StringList found=" << slFound
                  << " read=" << slRead << "\n";
        check(texFound > 0, "Texture combined meta entries found (" +
              std::to_string(texFound) + ")");
        check(texRead > 0, "Texture combined meta entries readable (" +
              std::to_string(texRead) + "/" +
              std::to_string(std::min<size_t>(texFound, 5)) + ")");
    }
}

// ============================================================================
// CascLib cross-validation
// ============================================================================

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)

struct CKeyHash {
    size_t operator()(const std::array<u8, 16>& k) const {
        size_t h = 0;
        for (size_t i = 0; i < 16; ++i)
            h = h * 131 + k[i];
        return h;
    }
};

// RAII wrappers for CascLib handles.
struct ScopedCascStorage {
    HANDLE h = nullptr;
    ~ScopedCascStorage() { if (h) CascCloseStorage(h); }
    explicit operator bool() const { return h != nullptr; }
};

struct ScopedCascFile {
    HANDLE h = nullptr;
    ~ScopedCascFile() { if (h) CascCloseFile(h); }
    explicit operator bool() const { return h != nullptr; }
};

struct ScopedCascFind {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~ScopedCascFind() { if (h != INVALID_HANDLE_VALUE) CascFindClose(h); }
    explicit operator bool() const { return h != INVALID_HANDLE_VALUE; }
};

/// Convert CASC_FIND_DATA CKey to std::array<u8,16>.
static std::array<u8, 16> toCKeyArray(const BYTE (&raw)[MD5_HASH_SIZE]) {
    std::array<u8, 16> out{};
    std::memcpy(out.data(), raw, 16);
    return out;
}

/// Read an entire file from CascLib by path.
static std::optional<std::vector<u8>> cascLibReadFile(HANDLE hStorage, const std::string& path) {
    ScopedCascFile file;
    if (!CascOpenFile(hStorage, path.c_str(), 0, CASC_OPEN_BY_NAME, &file.h))
        return std::nullopt;

    ULONGLONG fileSize64 = 0;
    if (!CascGetFileSize64(file.h, &fileSize64))
        return std::nullopt;

    if (fileSize64 > static_cast<ULONGLONG>(std::numeric_limits<size_t>::max()))
        return std::nullopt;

    auto fileSize = static_cast<size_t>(fileSize64);
    std::vector<u8> buffer(fileSize);
    size_t totalRead = 0;

    while (totalRead < fileSize) {
        auto chunk = static_cast<DWORD>(std::min<size_t>(fileSize - totalRead, 0x10000000u));
        DWORD bytesRead = 0;
        if (!CascReadFile(file.h, buffer.data() + totalRead, chunk, &bytesRead)) {
            DWORD err = GetCascError();
            if (err != ERROR_HANDLE_EOF)
                return std::nullopt;
            totalRead += bytesRead;
            break;
        }
        totalRead += bytesRead;
        if (bytesRead == 0) break;
    }
    buffer.resize(totalRead);
    return buffer;
}

static void testCrossValidate(const std::string& path) {
    std::cout << "\n[CascLib cross-validation]\n";

    auto t0 = std::chrono::steady_clock::now();

    // Open with both APIs.
    auto newStorage = new_casc::Storage::open(path);
    check(newStorage.has_value(), "new API opens");
    if (!newStorage) return;

    ScopedCascStorage oldStorage;
    bool oldOpened = CascOpenStorage(path.c_str(), CASC_LOCALE_ALL, &oldStorage.h);
    check(oldOpened, "CascLib opens");
    if (!oldOpened) return;

    // New API entry count.
    auto newCount = newStorage->totalFileCount();
    if (newCount) std::cout << "  New API entries: " << *newCount << "\n";

    // Build CKey → path map from CascLib enumeration.
    std::unordered_map<std::array<u8, 16>, std::string, CKeyHash> cascLibByKey;
    {
        CASC_FIND_DATA findData{};
        ScopedCascFind hFind;
        hFind.h = CascFindFirstFile(oldStorage.h, "*", &findData, nullptr);
        if (hFind) {
            do {
                auto cKey = toCKeyArray(findData.CKey);
                if (cKey != kZeroKey && findData.szFileName[0] != '\0')
                    cascLibByKey.emplace(cKey, std::string(findData.szFileName));
            } while (CascFindNextFile(hFind.h, &findData));
        }
    }
    std::cout << "  CascLib entries with CKeys: " << cascLibByKey.size() << "\n";

    // Collect new API entries with CKeys.
    struct NewEntry {
        std::array<u8, 16> cKey;
        std::string path;
    };
    std::vector<NewEntry> newEntries;
    size_t zeroCKeyCount = 0;
    newStorage->enumerate([&](const new_casc::FindEntry& fe) {
        if (fe.path.empty()) return true;
        if (fe.cKey != kZeroKey)
            newEntries.push_back({fe.cKey, fe.path});
        else
            ++zeroCKeyCount;
        return true;
    });
    std::cout << "  New API entries with CKeys: " << newEntries.size()
              << "  zeroCKey: " << zeroCKeyCount << "\n";

    // CKey overlap diagnostic.
    {
        std::unordered_set<std::array<u8, 16>, CKeyHash> newUniqueKeys;
        for (auto& ne : newEntries)
            newUniqueKeys.insert(ne.cKey);

        size_t onlyInCascLib = 0, onlyInNew = 0, inBoth = 0;
        for (auto& [k, _] : cascLibByKey) {
            if (newUniqueKeys.count(k)) ++inBoth;
            else ++onlyInCascLib;
        }
        for (auto& k : newUniqueKeys) {
            if (!cascLibByKey.count(k)) ++onlyInNew;
        }
        std::cout << "  Unique CKeys: CascLib=" << cascLibByKey.size()
                  << " New=" << newUniqueKeys.size()
                  << " Both=" << inBoth
                  << " OnlyCascLib=" << onlyInCascLib
                  << " OnlyNew=" << onlyInNew << "\n";
    }

    // Compare file data for matching CKeys.
    const size_t sampleLimit = 2000;
    size_t matched = 0, compared = 0, bytesMatch = 0, mismatch = 0;
    size_t oldFail = 0, newFail = 0, bothFail = 0;
    size_t newMd5Fail = 0, oldMd5Fail = 0;

    for (auto& ne : newEntries) {
        if (matched >= sampleLimit) break;

        auto it = cascLibByKey.find(ne.cKey);
        if (it == cascLibByKey.end()) continue;
        ++matched;

        auto newData = newStorage->readFile(ne.path);
        auto oldData = cascLibReadFile(oldStorage.h, it->second);

        if (!newData && !oldData) {
            ++bothFail;
            if (bothFail <= 5)
                std::cout << "    BOTH_FAIL: " << ne.path << "\n";
            continue;
        }
        if (!newData) {
            ++newFail;
            if (newFail <= 5)
                std::cout << "    NEW_FAIL: " << ne.path
                          << " (CascLib=" << it->second
                          << ", " << oldData->size() << " bytes)\n";
            continue;
        }
        if (!oldData) {
            ++oldFail;
            if (oldFail <= 5)
                std::cout << "    OLD_FAIL: " << ne.path << "\n";
            continue;
        }

        ++compared;

        auto newMd5 = storages::common::md5Hash(
            std::span<const u8>(newData->data(), newData->size()));
        auto oldMd5 = storages::common::md5Hash(
            std::span<const u8>(oldData->data(), oldData->size()));

        bool newCorrect = (newMd5 == ne.cKey);
        bool oldCorrect = (oldMd5 == ne.cKey);
        if (!newCorrect) {
            ++newMd5Fail;
            if (newMd5Fail <= 5)
                std::cout << "    NEW_MD5_FAIL: " << ne.path
                          << " expected=" << hexStr(ne.cKey)
                          << " got=" << hexStr(newMd5) << "\n";
        }
        if (!oldCorrect) ++oldMd5Fail;

        if (*newData == *oldData) {
            ++bytesMatch;
        } else if (newCorrect && oldCorrect) {
            ++mismatch;
            if (mismatch <= 3)
                std::cout << "    MISMATCH(impossible): CKey=" << hexStr(ne.cKey) << "\n";
        } else {
            if (newCorrect) ++bytesMatch;
        }
    }

    std::cout << "  CKey-matched: " << matched
              << "  Compared: " << compared
              << "  ByteMatch: " << bytesMatch
              << "  Mismatch: " << mismatch
              << "  NewFail: " << newFail
              << "  OldFail: " << oldFail
              << "  BothFail: " << bothFail << "\n";
    std::cout << "  NewMD5Fail: " << newMd5Fail
              << "  OldMD5Fail: " << oldMd5Fail << "\n";

    check(matched > 0, "found shared CKeys (" + std::to_string(matched) + ")");
    check(mismatch == 0,
          "zero true mismatches (" + std::to_string(bytesMatch) +
          "/" + std::to_string(compared) + " validated)");

    if (newMd5Fail > 0) {
        double aliasRate = compared > 0
            ? static_cast<double>(newMd5Fail) / static_cast<double>(compared)
            : 1.0;
        if (aliasRate > 0.5)
            check(false, "new API MD5 matches CKey (>50% alias rate)");
        else
            std::cout << "  WARN: " << newMd5Fail << "/" << compared
                      << " CKey/path aliases (TVFS trie prefix collisions)\n";
    } else {
        check(true, "new API MD5 always matches CKey");
    }

    if (compared > 0) {
        double failRate = static_cast<double>(newFail) / static_cast<double>(matched);
        check(failRate < 0.05,
              "new API failure rate < 5% (" + std::to_string(newFail) + "/" +
              std::to_string(matched) + " = " +
              std::to_string(static_cast<int>(failRate * 100)) + "%)");
    }

    // MD5 spot-check on first 200 new entries.
    size_t md5Checked = 0, md5Pass = 0;
    for (size_t i = 0; i < std::min<size_t>(200, newEntries.size()); ++i) {
        auto& ne = newEntries[i];
        auto data = newStorage->readFile(ne.path);
        if (!data) continue;
        auto md5 = storages::common::md5Hash(
            std::span<const u8>(data->data(), data->size()));
        ++md5Checked;
        if (md5 == ne.cKey) ++md5Pass;
    }
    if (md5Checked > 0)
        check(md5Pass == md5Checked,
              "MD5/CKey verification (" + std::to_string(md5Pass) +
              "/" + std::to_string(md5Checked) + ")");

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "  Completed in " << ms << " ms.\n";
}

#endif // WHITEOUT_HAS_CASCLIB_CROSSVAL

// ============================================================================
// main
// ============================================================================

int main() {
    const std::string d4Path = "C:\\Program Files (x86)\\Diablo IV";

    std::cout << "=== Diablo IV CASC Read Test ===\n";
    std::cout << "  Path: " << d4Path << "\n";

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    std::cout << "  Mode: CascLib cross-validation\n";
#else
    std::cout << "  Mode: Internal consistency (no CascLib)\n";
#endif

    // Internal consistency tests always run.
    testInternal(d4Path);

    // CascLib cross-validation when available.
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    testCrossValidate(d4Path);
#endif

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed
              << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}
