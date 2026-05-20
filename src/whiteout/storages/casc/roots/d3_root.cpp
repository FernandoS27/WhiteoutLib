// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "d3_root.h"
#include "root.h"
#include "common/root_build_utils.h"
#include "../../common/byte_order.h"
#include "../../common/string_utils.h"

#include <whiteout/interfaces.h>
#include <whiteout/sno/core_toc.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace whiteout::storages::casc {

using storages::common::readLE32;
using storages::common::normalizeCascPath;

// ---- Constants local to D3 root parser ----

/// D3 asset entry sizes.
static constexpr size_t kD3AssetEntrySize = 20;    ///< CKey(16) + fileIndex(4).
static constexpr size_t kD3AssetIdxEntrySize = 24;  ///< CKey(16) + fileIndex(4) + subIndex(4).

namespace {

/// Known D3 SNO asset types (index → extension + name).
struct AssetTypeInfo {
    u32 index;
    const char* extension;
    const char* name;
};

// D3 SNO asset types. Indices and extensions match CascLib's D3AssetTypes[].
static constexpr AssetTypeInfo kAssetTypes[] = {
    {0x01, "acr", "Actor"},
    {0x02, "adv", "Adventure"},
    {0x05, "ams", "AmbientSound"},
    {0x06, "ani", "Anim"},
    {0x07, "an2", "Anim2D"},
    {0x08, "ans", "AnimSet"},
    {0x09, "app", "Appearance"},
    {0x0B, "clt", "Cloth"},
    {0x0C, "cnv", "Conversation"},
    {0x0E, "efg", "EffectGroup"},
    {0x0F, "enc", "Encounter"},
    {0x11, "xpl", "Explosion"},
    {0x13, "fnt", "Font"},
    {0x14, "gam", "GameBalance"},
    {0x15, "glo", "Globals"},
    {0x16, "lvl", "LevelArea"},
    {0x17, "lit", "Light"},
    {0x18, "mrk", "MarkerSet"},
    {0x19, "mon", "Monster"},
    {0x1A, "obs", "Observer"},
    {0x1B, "prt", "Particle"},
    {0x1C, "phy", "Physics"},
    {0x1D, "pow", "Power"},
    {0x1F, "qst", "Quest"},
    {0x20, "rop", "Rope"},
    {0x21, "scn", "Scene"},
    {0x22, "scg", "SceneGroup"},
    {0x24, "shm", "ShaderMap"},
    {0x25, "shd", "Shaders"},
    {0x26, "shk", "Shakes"},
    {0x27, "skl", "SkillKit"},
    {0x28, "snd", "Sound"},
    {0x29, "sbk", "SoundBank"},
    {0x2A, "stl", "StringList"},
    {0x2B, "srf", "Surface"},
    {0x2C, "tex", "Textures"},
    {0x2D, "trl", "Trail"},
    {0x2E, "ui",  "UI"},
    {0x2F, "wth", "Weather"},
    {0x30, "wrl", "Worlds"},
    {0x31, "rcp", "Recipe"},
    {0x33, "cnd", "Condition"},
    {0x38, "act", "Act"},
    {0x39, "mat", "Material"},
    {0x3A, "qsr", "QuestRange"},
    {0x3B, "lor", "Lore"},
    {0x3C, "rev", "Reverb"},
    {0x3D, "phm", "PhysMesh"},
    {0x3E, "mus", "Music"},
    {0x3F, "tut", "Tutorial"},
    {0x40, "bos", "BossEncounter"},
    {0x42, "aco", "Accolade"},
};

static const char* getAssetExtension(u32 assetIndex) {
    for (auto& t : kAssetTypes) {
        if (t.index == assetIndex) return t.extension;
    }
    return nullptr;
}

static const char* getAssetDirName(u32 assetIndex) {
    for (auto& t : kAssetTypes) {
        if (t.index == assetIndex) return t.name;
    }
    return nullptr;
}

/// Build a CascLib-compatible path for an asset entry using CoreTOC.
/// Returns empty string if the CoreTOC does not contain the entry.
static std::string buildAssetPath(const std::string& prefix, u32 fileIndex,
                                  const sno::CoreToc* coreToc) {
    if (!coreToc) return {};

    auto* tocEntry = coreToc->findById(static_cast<i32>(fileIndex));
    if (!tocEntry || tocEntry->name.empty()) return {};

    auto group = static_cast<u32>(tocEntry->group);
    const char* dirName = getAssetDirName(group);
    const char* ext = getAssetExtension(group);
    if (!dirName || !ext) return {};

    return prefix + dirName + "\\" + tocEntry->name + "." + ext;
}

/// Build a CascLib-compatible path for an assetIdx entry using CoreTOC.
/// SubIndex entries get a subfolder: Dir\Name\NNNN.ext
static std::string buildAssetIdxPath(const std::string& prefix, u32 fileIndex,
                                     u32 subIndex, const sno::CoreToc* coreToc) {
    if (!coreToc) return {};

    auto* tocEntry = coreToc->findById(static_cast<i32>(fileIndex));
    if (!tocEntry || tocEntry->name.empty()) return {};

    auto group = static_cast<u32>(tocEntry->group);
    const char* dirName = getAssetDirName(group);
    const char* ext = getAssetExtension(group);
    if (!dirName || !ext) return {};

    char subBuf[16];
    std::snprintf(subBuf, sizeof(subBuf), "%04u", subIndex);
    return prefix + dirName + "\\" + tocEntry->name + "\\" + subBuf + "." + ext;
}

/// Build the directory prefix for an asset entry's generated path (fallback).
/// Known groups get their 3-letter extension; unknown groups get "unk_NN"
/// (decimal group ID) so that different unknown groups don't collide.
static std::string getGroupDir(u32 fileIndex) {
    u32 const group = fileIndex >> 16;
    auto ext = getAssetExtension(group);
    if (ext) return ext;
    return "unk_" + std::to_string(group);
}

/// Build a fallback numeric path when CoreTOC is not available.
static std::string buildFallbackAssetPath(const std::string& prefix, u32 fileIndex) {
    return prefix + getGroupDir(fileIndex) + "\\" + std::to_string(fileIndex);
}

static std::string buildFallbackAssetIdxPath(const std::string& prefix, u32 fileIndex,
                                             u32 subIndex) {
    return prefix + getGroupDir(fileIndex) + "\\" +
           std::to_string(fileIndex) + "." + std::to_string(subIndex);
}

// ============================================================================
// Directory parsing
// ============================================================================

struct AssetEntry {
    std::array<u8, 16> cKey;
    u32 fileIndex;
};

struct AssetIdxEntry {
    std::array<u8, 16> cKey;
    u32 fileIndex;
    u32 subIndex;
};

struct NamedEntry {
    std::array<u8, 16> cKey;
    std::string name;
};

/// Parse a single D3 root directory blob.
/// Returns named entries and asset/assetidx entries separately.
static bool parseDirectory(std::span<const u8> data, size_t& offset,
                           std::vector<AssetEntry>& assetEntries,
                           std::vector<AssetIdxEntry>& assetIdxEntries,
                           std::vector<NamedEntry>& namedEntries) {
    if (offset + 4 > data.size()) return false;

    u32 const signature = readLE32(data.data() + offset);

    if (signature == RootSignature::kD3Dir) {
        offset += 4;

        // Asset entries.
        if (offset + 4 > data.size()) return false;
        u32 const assetCount = readLE32(data.data() + offset);
        offset += 4;

        for (u32 i = 0; i < assetCount; ++i) {
            if (offset + kD3AssetEntrySize > data.size()) return false;
            AssetEntry ae;
            std::memcpy(ae.cKey.data(), data.data() + offset, 16);
            ae.fileIndex = readLE32(data.data() + offset + 16);
            offset += kD3AssetEntrySize;
            assetEntries.push_back(ae);
        }

        // AssetIdx entries.
        if (offset + 4 > data.size()) return false;
        u32 const assetIdxCount = readLE32(data.data() + offset);
        offset += 4;

        for (u32 i = 0; i < assetIdxCount; ++i) {
            if (offset + kD3AssetIdxEntrySize > data.size()) return false;
            AssetIdxEntry aie;
            std::memcpy(aie.cKey.data(), data.data() + offset, 16);
            aie.fileIndex = readLE32(data.data() + offset + 16);
            aie.subIndex = readLE32(data.data() + offset + 20);
            offset += kD3AssetIdxEntrySize;
            assetIdxEntries.push_back(aie);
        }
    } else if (signature == RootSignature::kD3Root) {
        // Root directory: only named entries follow (no asset/assetidx).
        offset += 4;
    } else {
        // Unknown directory format.
        return false;
    }

    // Named entries (always present after optional asset sections).
    if (offset + 4 > data.size()) return false;
    u32 const namedCount = readLE32(data.data() + offset);
    offset += 4;

    for (u32 i = 0; i < namedCount; ++i) {
        if (offset + 16 > data.size()) return false;
        NamedEntry ne;
        std::memcpy(ne.cKey.data(), data.data() + offset, 16);
        offset += 16;

        // Null-terminated string.
        size_t const strStart = offset;
        while (offset < data.size() && data[offset] != 0) ++offset;
        if (offset >= data.size()) return false;
        ne.name = std::string(reinterpret_cast<const char*>(data.data() + strStart),
                              offset - strStart);
        ++offset; // skip null terminator
        namedEntries.push_back(std::move(ne));
    }

    return true;
}

} // anonymous namespace

// ============================================================================
// D3Root public API
// ============================================================================

std::unique_ptr<D3Root> D3Root::parse(std::span<const u8> data, CKeyResolver resolver,
                                      interfaces::WorkerPool* pool) {
    if (data.size() < 4) return nullptr;

    auto root = std::make_unique<D3Root>();

    // Phase 1: Parse the root directory for named entries (sub-dir CKeys).
    std::vector<AssetEntry> assetEntries;
    std::vector<AssetIdxEntry> assetIdxEntries;
    std::vector<NamedEntry> namedEntries;

    size_t offset = 0;
    if (!parseDirectory(data, offset, assetEntries, assetIdxEntries, namedEntries))
        return nullptr;

    // Collect root-level asset entries for later path generation.
    // CascLib resolves these AFTER loading CoreTOC in Phase 2.
    struct PendingAsset { AssetEntry ae; std::string prefix; };
    struct PendingAssetIdx { AssetIdxEntry aie; std::string prefix; };
    std::vector<PendingAsset> pendingAssets;
    std::vector<PendingAssetIdx> pendingAssetIdx;

    for (auto& ae : assetEntries)
        pendingAssets.push_back({ae, {}});
    for (auto& aie : assetIdxEntries)
        pendingAssetIdx.push_back({aie, {}});

    // Named entries from root are sub-directory references (Base, enUS, Windows, …)
    // — not actual files. Collect them for sub-directory resolution only.
    std::vector<NamedEntry> subdirEntries;
    if (resolver) {
        subdirEntries = std::move(namedEntries);
    }

    // CoreTOC CKey — look for it among sub-directory named entries.
    std::array<u8, 16> coreTocCKey{};
    bool haveCoreTocCKey = false;

    // Phase 2: Resolve sub-directories.
    if (resolver && !subdirEntries.empty()) {
        // Resolve all sub-directory CKeys (parallel when pool available).
        std::vector<std::vector<u8>> subdirData(subdirEntries.size());

        if (pool && subdirEntries.size() > 1) {
            utils::JobGroup jobGroup;
            jobGroup.add(subdirEntries.size());
            for (size_t i = 0; i < subdirEntries.size(); ++i) {
                interfaces::WorkerTask task;
                task.fn = [&, i]() {
                    subdirData[i] = resolver(subdirEntries[i].cKey);
                    jobGroup.done();
                };
                pool->submit(task);
            }
            jobGroup.wait();
        } else {
            for (size_t i = 0; i < subdirEntries.size(); ++i)
                subdirData[i] = resolver(subdirEntries[i].cKey);
        }

        // Parse resolved sub-directories.
        for (size_t si = 0; si < subdirEntries.size(); ++si) {
            if (subdirData[si].empty()) continue;

            auto& subNe = subdirEntries[si];

            std::vector<AssetEntry> subAssets;
            std::vector<AssetIdxEntry> subAssetIdx;
            std::vector<NamedEntry> subNamed;
            size_t subOffset = 0;

            if (parseDirectory(subdirData[si], subOffset, subAssets, subAssetIdx, subNamed)) {
                std::string const prefix = subNe.name.empty() ? "" : (subNe.name + "\\");

                // Save asset entries for Phase 3 (after CoreTOC resolution).
                for (auto& ae : subAssets)
                    pendingAssets.push_back({ae, prefix});
                for (auto& aie : subAssetIdx)
                    pendingAssetIdx.push_back({aie, prefix});

                // Named entries are real files — add them directly.
                for (auto& sn : subNamed) {
                    // Look for CoreTOC.dat in the "Base" sub-directory.
                    if (!haveCoreTocCKey && sn.name == "CoreTOC.dat") {
                        coreTocCKey = sn.cKey;
                        haveCoreTocCKey = true;
                    }

                    RootEntry re;
                    re.cKey = sn.cKey;
                    re.path = prefix + sn.name;
                    root->m_entries.push_back(std::move(re));
                }
            }
        }
    }

    // Phase 3: Resolve CoreTOC and build CascLib-compatible paths.
    sno::CoreToc coreToc;
    bool haveCoreToc = false;
    if (haveCoreTocCKey && resolver) {
        auto tocData = resolver(coreTocCKey);
        if (!tocData.empty()) {
            haveCoreToc = coreToc.parse(tocData);
        }
    }

    const sno::CoreToc* tocPtr = haveCoreToc ? &coreToc : nullptr;

    for (auto& pa : pendingAssets) {
        RootEntry re;
        re.cKey = pa.ae.cKey;
        re.fileDataId = pa.ae.fileIndex;

        auto path = buildAssetPath(pa.prefix, pa.ae.fileIndex, tocPtr);
        re.path = path.empty()
            ? buildFallbackAssetPath(pa.prefix, pa.ae.fileIndex)
            : std::move(path);
        root->m_entries.push_back(std::move(re));
    }

    for (auto& pia : pendingAssetIdx) {
        RootEntry re;
        re.cKey = pia.aie.cKey;
        re.fileDataId = pia.aie.fileIndex;

        auto path = buildAssetIdxPath(pia.prefix, pia.aie.fileIndex,
                                      pia.aie.subIndex, tocPtr);
        re.path = path.empty()
            ? buildFallbackAssetIdxPath(pia.prefix, pia.aie.fileIndex,
                                        pia.aie.subIndex)
            : std::move(path);
        root->m_entries.push_back(std::move(re));
    }

    if (!root->m_entries.empty())
        root->buildIndices(pool);
    return root;
}

std::vector<const RootEntry*> D3Root::findByPath(const std::string& path) const {
    auto key = normalizeCascPath(path);
    return findByNormalizedPath(key);
}

std::vector<const RootEntry*> D3Root::findByNormalizedPath(const std::string& normalizedPath) const {
    return m_byPath.findAll(m_entries, normalizedPath);
}

bool D3Root::hasPath(const std::string& normalizedPath) const {
    return m_byPath.contains(normalizedPath);
}

std::vector<const RootEntry*> D3Root::findByFileDataId(u32 fileDataId, FileIdHint /*hint*/) const {
    return m_byFileDataId.findAll(m_entries, fileDataId);
}

bool D3Root::hasFileDataId(u32 fileDataId, FileIdHint /*hint*/) const {
    return m_byFileDataId.contains(fileDataId);
}

void D3Root::buildIndices(interfaces::WorkerPool* pool) {
    size_t const n = m_entries.size();

    auto lowerPaths = normalizeEntryPaths(m_entries, pool);

    m_byPath.reserve(n);
    m_byFileDataId.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!lowerPaths[i].empty())
            m_byPath.emplace(std::move(lowerPaths[i]), i);
        if (m_entries[i].fileDataId != kInvalidFileDataId)
            m_byFileDataId.emplace(m_entries[i].fileDataId, i);
    }
}

} // namespace whiteout::storages::casc
