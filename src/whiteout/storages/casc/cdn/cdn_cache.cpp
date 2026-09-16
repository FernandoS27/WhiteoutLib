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

std::string CdnCache::resourcePath(const std::string& pathType, const std::string& keyHex) const {
    if (keyHex.size() < 4)
        return {};
    return m_cacheDir + "/" + pathType + "/" + keyHex.substr(0, 2) + "/" + keyHex.substr(2, 2) +
           "/" + keyHex;
}

std::string CdnCache::rangePath(const std::string& archiveKeyHex, u64 offset, u32 size) const {
    if (archiveKeyHex.size() < 4)
        return {};
    return m_cacheDir + "/archives/" + archiveKeyHex.substr(0, 2) + "/" +
           archiveKeyHex.substr(2, 2) + "/" + archiveKeyHex + "/" + std::to_string(offset) + "_" +
           std::to_string(size) + ".blte";
}

// Cache paths are UTF-8. The cache usually sits under a user profile, and a
// profile name need not fit the ANSI code page std::filesystem would read a
// plain std::string in.
using whiteout::common::utf8_to_path;

void CdnCache::ensureDir(const std::string& path) {
    auto parent = utf8_to_path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
}

std::optional<std::vector<u8>> CdnCache::readFileBytes(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(utf8_to_path(path), ec))
        return std::nullopt;

    auto f = whiteout::common::open_ifstream(path, std::ios::binary | std::ios::ate);
    if (!f)
        return std::nullopt;

    auto sz = f.tellg();
    if (sz <= 0)
        return std::vector<u8>{};

    std::vector<u8> buf(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (!f)
        return std::nullopt;

    return buf;
}

void CdnCache::writeFileBytes(const std::string& path, std::span<const u8> data) {
    ensureDir(path);

    // Write to temp file, then rename for atomicity.
    std::string const tmpPath = path + ".tmp";
    {
        auto f = whiteout::common::open_ofstream(tmpPath, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "[whiteout cdn_cache] open failed: %s\n", tmpPath.c_str());
            return;
        }
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        if (!f) {
            std::fprintf(stderr, "[whiteout cdn_cache] write failed: %s\n", tmpPath.c_str());
            std::error_code ec;
            fs::remove(utf8_to_path(tmpPath), ec);
            return;
        }
    }

    fs::path const from = utf8_to_path(tmpPath);
    fs::path const to = utf8_to_path(path);
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec)
        return;

    // Rename failed (e.g. cross-device) — try copy + remove.
    std::error_code copyEc;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, copyEc);
    fs::remove(from, ec);
    if (copyEc) {
        std::fprintf(stderr, "[whiteout cdn_cache] commit failed: %s (%s)\n", path.c_str(),
                     copyEc.message().c_str());
    }
}

// ── Public API ─────────────────────────────────────────────────────

bool CdnCache::has(const std::string& pathType, const std::string& keyHex) const {
    auto p = resourcePath(pathType, keyHex);
    if (p.empty())
        return false;
    std::error_code ec;
    return fs::exists(utf8_to_path(p), ec);
}

std::optional<std::vector<u8>> CdnCache::read(const std::string& pathType,
                                              const std::string& keyHex) const {
    auto p = resourcePath(pathType, keyHex);
    if (p.empty())
        return std::nullopt;
    return readFileBytes(p);
}

void CdnCache::write(const std::string& pathType, const std::string& keyHex,
                     std::span<const u8> data) {
    auto p = resourcePath(pathType, keyHex);
    if (p.empty())
        return;
    writeFileBytes(p, data);
}

bool CdnCache::hasRange(const std::string& archiveKeyHex, u64 offset, u32 size) const {
    auto p = rangePath(archiveKeyHex, offset, size);
    if (p.empty())
        return false;
    std::error_code ec;
    return fs::exists(utf8_to_path(p), ec);
}

std::optional<std::vector<u8>> CdnCache::readRange(const std::string& archiveKeyHex, u64 offset,
                                                   u32 size) const {
    auto p = rangePath(archiveKeyHex, offset, size);
    if (p.empty())
        return std::nullopt;
    return readFileBytes(p);
}

void CdnCache::writeRange(const std::string& archiveKeyHex, u64 offset, u32 size,
                          std::span<const u8> data) {
    auto p = rangePath(archiveKeyHex, offset, size);
    if (p.empty())
        return;
    writeFileBytes(p, data);
}

} // namespace whiteout::storages::casc
