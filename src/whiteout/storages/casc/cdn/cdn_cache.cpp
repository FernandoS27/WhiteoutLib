// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "cdn_cache.h"

#include "../../../common/unicode_path.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace whiteout::storages::casc {

CdnCache::CdnCache(const std::string& cacheDir) : m_cacheDir(cacheDir) {}

std::string CdnCache::resourcePath(const std::string& pathType,
                                    const std::string& keyHex) const {
    if (keyHex.size() < 4) return {};
    return m_cacheDir + "/" + pathType + "/" + keyHex.substr(0, 2) + "/" +
           keyHex.substr(2, 2) + "/" + keyHex;
}

std::string CdnCache::rangePath(const std::string& archiveKeyHex,
                                 u64 offset, u32 size) const {
    if (archiveKeyHex.size() < 4) return {};
    return m_cacheDir + "/archives/" + archiveKeyHex.substr(0, 2) + "/" +
           archiveKeyHex.substr(2, 2) + "/" + archiveKeyHex + "/" +
           std::to_string(offset) + "_" + std::to_string(size) + ".blte";
}

void CdnCache::ensureDir(const std::string& path) {
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
}

std::optional<std::vector<u8>> CdnCache::readFileBytes(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;

    auto f = whiteout::common::open_ifstream(path, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;

    auto sz = f.tellg();
    if (sz <= 0) return std::vector<u8>{};

    std::vector<u8> buf(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (!f) return std::nullopt;

    return buf;
}

void CdnCache::writeFileBytes(const std::string& path, std::span<const u8> data) {
    ensureDir(path);

    // Write to temp file, then rename for atomicity.
    std::string tmpPath = path + ".tmp";
    {
        auto f = whiteout::common::open_ofstream(tmpPath, std::ios::binary);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        if (!f) {
            std::error_code ec;
            fs::remove(tmpPath, ec);
            return;
        }
    }

    std::error_code ec;
    fs::rename(tmpPath, path, ec);
    if (ec) {
        // Rename failed (e.g. cross-device) — try copy + remove.
        fs::copy_file(tmpPath, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmpPath, ec);
    }
}

// ── Public API ─────────────────────────────────────────────────────

bool CdnCache::has(const std::string& pathType, const std::string& keyHex) const {
    auto p = resourcePath(pathType, keyHex);
    if (p.empty()) return false;
    std::error_code ec;
    return fs::exists(p, ec);
}

std::optional<std::vector<u8>> CdnCache::read(const std::string& pathType,
                                               const std::string& keyHex) const {
    auto p = resourcePath(pathType, keyHex);
    if (p.empty()) return std::nullopt;
    return readFileBytes(p);
}

void CdnCache::write(const std::string& pathType, const std::string& keyHex,
                      std::span<const u8> data) {
    auto p = resourcePath(pathType, keyHex);
    if (p.empty()) return;
    writeFileBytes(p, data);
}

bool CdnCache::hasRange(const std::string& archiveKeyHex,
                         u64 offset, u32 size) const {
    auto p = rangePath(archiveKeyHex, offset, size);
    if (p.empty()) return false;
    std::error_code ec;
    return fs::exists(p, ec);
}

std::optional<std::vector<u8>> CdnCache::readRange(const std::string& archiveKeyHex,
                                                     u64 offset, u32 size) const {
    auto p = rangePath(archiveKeyHex, offset, size);
    if (p.empty()) return std::nullopt;
    return readFileBytes(p);
}

void CdnCache::writeRange(const std::string& archiveKeyHex,
                           u64 offset, u32 size,
                           std::span<const u8> data) {
    auto p = rangePath(archiveKeyHex, offset, size);
    if (p.empty()) return;
    writeFileBytes(p, data);
}

} // namespace whiteout::storages::casc
