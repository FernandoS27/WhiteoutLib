// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "wow_file_system.h"

#include <cctype>

namespace whiteout {
namespace m2 {

namespace {

namespace fs = std::filesystem;

/// Strip the .m2 extension to get the base stem (directory + stem).
/// "creature/nightelfmale/nightelfmale_hd.m2" → "creature/nightelfmale/nightelfmale_hd"
std::string extractBaseStem(const std::string& m2Path) {
    fs::path p(m2Path);
    return (p.parent_path() / p.stem()).string();
}

std::string zeroPad(int value, int width) {
    std::string s = std::to_string(value);
    if (static_cast<int>(s.size()) < width) {
        s.insert(0, width - s.size(), '0');
    }
    return s;
}

} // namespace

// ============================================================================
// Construction
// ============================================================================

WoWFileSystem::WoWFileSystem(interfaces::VirtualPathFileSystem& pathFs, const std::string& m2Path)
    : m_pathFs(&pathFs), m_baseStem(extractBaseStem(m2Path)),
      m_m2Storage(pathFs.readFile(m2Path)), m_m2Data(m_m2Storage) {}

WoWFileSystem::WoWFileSystem(interfaces::CascFileSystem& cascFs, std::span<const u8> m2Data)
    : m_cascFs(&cascFs), m_m2Data(m2Data) {}

// ============================================================================
// Base M2 data
// ============================================================================

std::span<const u8> WoWFileSystem::getM2Base() const { return m_m2Data; }

// ============================================================================
// Chunk metadata setters
// ============================================================================

void WoWFileSystem::setSkinChunk(const SFIDChunk& chunk) { m_sfid = chunk; }
void WoWFileSystem::setAnimChunk(const AFIDChunk& chunk) { m_afid = chunk; }
void WoWFileSystem::setSkeletonChunk(const SKIDChunk& chunk) { m_skid = chunk; }

// ============================================================================
// Satellite data accessors
// ============================================================================

std::span<const u8> WoWFileSystem::getSkin(u32 skinId, bool isLod) {
    auto& cache = isLod ? m_lodSkinCache : m_skinCache;

    auto it = cache.find(skinId);
    if (it != cache.end()) {
        return it->second;
    }

    std::vector<u8> data;
    if (m_cascFs) {
        const auto& ids = isLod ? m_sfid.lodSkinFileDataIds : m_sfid.skinFileDataIds;
        if (skinId >= ids.size() || ids[skinId] == 0) {
            return {};
        }
        data = m_cascFs->readFile(ids[skinId]);
    } else {
        std::string path = buildSkinPath(skinId, isLod);
        if (!m_pathFs->fileExists(path)) {
            return {};
        }
        data = m_pathFs->readFile(path);
    }

    auto& buf = cache[skinId];
    buf = std::move(data);
    return buf;
}

std::span<const u8> WoWFileSystem::getAnimBuffer(u16 animId, u16 subAnimId) {
    u32 key = animKey(animId, subAnimId);
    auto it = m_animCache.find(key);
    if (it != m_animCache.end()) {
        return it->second;
    }

    std::vector<u8> data;
    if (m_cascFs) {
        u32 fileDataId = 0;
        for (const auto& entry : m_afid.animFileIds) {
            if (entry.animId == animId && entry.subAnimId == subAnimId) {
                fileDataId = entry.fileDataId;
                break;
            }
        }
        if (fileDataId == 0) {
            return {};
        }
        data = m_cascFs->readFile(fileDataId);
    } else {
        std::string path = buildAnimPath(animId, subAnimId);
        if (!m_pathFs->fileExists(path)) {
            return {};
        }
        data = m_pathFs->readFile(path);
    }

    auto& buf = m_animCache[key];
    buf = std::move(data);
    return buf;
}

std::span<const u8> WoWFileSystem::getSkeleton() {
    if (m_skelLoaded) {
        return m_skelCache;
    }
    m_skelLoaded = true;

    if (m_cascFs) {
        if (m_skid.skeletonFileDataId == 0) {
            return {};
        }
        m_skelCache = m_cascFs->readFile(m_skid.skeletonFileDataId);
    } else {
        std::string path = buildSkelPath();
        if (!m_pathFs->fileExists(path)) {
            return {};
        }
        m_skelCache = m_pathFs->readFile(path);
    }
    return m_skelCache;
}

// ============================================================================
// Helpers
// ============================================================================

u32 WoWFileSystem::animKey(u16 animId, u16 subAnimId) {
    return (static_cast<u32>(animId) << 16) | subAnimId;
}

std::string WoWFileSystem::buildSkinPath(u32 skinId, bool isLod) const {
    std::string suffix = isLod ? "_lod" + zeroPad(skinId, 2) : zeroPad(skinId, 2);
    return m_baseStem + suffix + ".skin";
}

std::string WoWFileSystem::buildAnimPath(u16 animId, u16 subAnimId) const {
    return m_baseStem + zeroPad(animId, 4) + "-" + zeroPad(subAnimId, 2) + ".anim";
}

std::string WoWFileSystem::buildSkelPath() const {
    return m_baseStem + ".skel";
}

// ============================================================================
// Exploratory search
// ============================================================================

void WoWFileSystem::exploratorySearch() {
    if (!m_pathFs || m_baseStem.empty()) {
        return;
    }

    // Extract directory and filename stem from m_baseStem.
    // e.g. "creature/nightelfmale/nightelfmale_hd" → dir="creature/nightelfmale", stem="nightelfmale_hd"
    fs::path basePath(m_baseStem);
    fs::path dir = basePath.parent_path();
    std::string stem = basePath.filename().string();

    auto entries = m_pathFs->listDirectory(dir.string());

    for (const auto& entry : entries) {
        if (entry.isDirectory) continue;

        // Only consider files whose name starts with our stem
        if (entry.name.size() <= stem.size()) continue;
        if (entry.name.compare(0, stem.size(), stem) != 0) continue;

        std::string suffix = entry.name.substr(stem.size());
        std::string fullPath = (dir / entry.name).string();

        // .skel — e.g. "nightelfmale_hd.skel"
        if (suffix == ".skel") {
            if (!m_skelLoaded) {
                m_skelCache = m_pathFs->readFile(fullPath);
                m_skelLoaded = true;
            }
            continue;
        }

        // .skin — e.g. "nightelfmale_hd00.skin" or "nightelfmale_hd_lod01.skin"
        if (suffix.size() > 5 && suffix.substr(suffix.size() - 5) == ".skin") {
            std::string mid = suffix.substr(0, suffix.size() - 5); // e.g. "00" or "_lod01"
            if (mid.size() == 2 && std::isdigit(static_cast<unsigned char>(mid[0]))
                               && std::isdigit(static_cast<unsigned char>(mid[1]))) {
                u32 skinId = static_cast<u32>(std::stoi(mid));
                if (!m_skinCache.contains(skinId)) {
                    m_skinCache[skinId] = m_pathFs->readFile(fullPath);
                }
            } else if (mid.size() == 6 && mid.substr(0, 4) == "_lod"
                       && std::isdigit(static_cast<unsigned char>(mid[4]))
                       && std::isdigit(static_cast<unsigned char>(mid[5]))) {
                u32 lodId = static_cast<u32>(std::stoi(mid.substr(4)));
                if (!m_lodSkinCache.contains(lodId)) {
                    m_lodSkinCache[lodId] = m_pathFs->readFile(fullPath);
                }
            }
            continue;
        }

        // .anim — e.g. "nightelfmale_hd0000-00.anim"
        if (suffix.size() == 12 && suffix.substr(suffix.size() - 5) == ".anim"
            && suffix[4] == '-') {
            // suffix = "0000-00.anim"  (4 digits, dash, 2 digits, .anim)
            std::string animIdStr = suffix.substr(0, 4);
            std::string subIdStr  = suffix.substr(5, 2);
            bool allDigits = true;
            for (char c : animIdStr) allDigits &= std::isdigit(static_cast<unsigned char>(c)) != 0;
            for (char c : subIdStr)  allDigits &= std::isdigit(static_cast<unsigned char>(c)) != 0;
            if (allDigits) {
                u16 animId    = static_cast<u16>(std::stoi(animIdStr));
                u16 subAnimId = static_cast<u16>(std::stoi(subIdStr));
                u32 key = animKey(animId, subAnimId);
                if (!m_animCache.contains(key)) {
                    m_animCache[key] = m_pathFs->readFile(fullPath);
                }
            }
            continue;
        }
    }
}

} // namespace m2
} // namespace whiteout
