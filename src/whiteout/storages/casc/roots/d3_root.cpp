// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "d3_root.h"
#include "root.h"
#include "../../common/byte_order.h"
#include "../../common/string_utils.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>

namespace whiteout::storages::casc {

using storages::common::readLE32;
using storages::common::toLower;

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

// Subset of known D3 asset types. Index values match the CascLib table.
static constexpr AssetTypeInfo kAssetTypes[] = {
    {0x00, "unk", "Unknown"},
    {0x01, "acr", "Actor"},
    {0x02, "adv", "Adventure"},
    {0x03, "ais", "AiBehavior"},
    {0x04, "ait", "AiState"},
    {0x05, "amb", "AmbientSound"},
    {0x06, "ani", "Anim"},
    {0x07, "an2", "Anim2D"},
    {0x08, "ans", "AnimSet"},
    {0x09, "app", "Appearance"},
    {0x0B, "clt", "Cloth"},
    {0x0C, "con", "Conversation"},
    {0x0E, "efg", "EffectGroup"},
    {0x10, "enc", "Encounter"},
    {0x12, "exp", "Explosion"},
    {0x13, "fnt", "Font"},
    {0x14, "gam", "GameBalance"},
    {0x15, "glo", "Globals"},
    {0x16, "lvl", "LevelArea"},
    {0x17, "lit", "Light"},
    {0x18, "mrk", "MarkerSet"},
    {0x1A, "mon", "Monster"},
    {0x1B, "obs", "Observer"},
    {0x1C, "phy", "Particle"},
    {0x1D, "phm", "Physics"},
    {0x1E, "pow", "Power"},
    {0x20, "qst", "Quest"},
    {0x21, "rop", "Rope"},
    {0x22, "scn", "Scene"},
    {0x23, "scg", "SceneGroup"},
    {0x25, "shk", "ShaderMap"},
    {0x26, "shd", "Shaders"},
    {0x27, "shm", "Shakes"},
    {0x28, "skl", "SkillKit"},
    {0x29, "snd", "Sound"},
    {0x2A, "snb", "SoundBank"},
    {0x2B, "stl", "StringList"},
    {0x2C, "srf", "Surface"},
    {0x2D, "tex", "Textures"},
    {0x2E, "trl", "Trail"},
    {0x2F, "ui", "UI"},
    {0x30, "wth", "Weather"},
    {0x31, "wrl", "Worlds"},
    {0x32, "rec", "Recipe"},
    {0x34, "cnd", "Condition"},
    {0x36, "act", "Act"},
    {0x37, "mat", "Material"},
    {0x39, "qsr", "QuestRange"},
    {0x3A, "lor", "Lore"},
    {0x3B, "rev", "Reverb"},
    {0x3C, "mus", "Music"},
    {0x3D, "tut", "Tutorial"},
    {0x3F, "bos", "BossEncounter"},
    {0x41, "ach", "Achievement"},
    {0x42, "aco", "Accolade"},
};

static const char* getAssetExtension(u32 assetIndex) {
    for (auto& t : kAssetTypes) {
        if (t.index == assetIndex) return t.extension;
    }
    return "unk";
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

    u32 signature = readLE32(data.data() + offset);

    if (signature == RootSignature::kD3Dir) {
        offset += 4;

        // Asset entries.
        if (offset + 4 > data.size()) return false;
        u32 assetCount = readLE32(data.data() + offset);
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
        u32 assetIdxCount = readLE32(data.data() + offset);
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
    u32 namedCount = readLE32(data.data() + offset);
    offset += 4;

    for (u32 i = 0; i < namedCount; ++i) {
        if (offset + 16 > data.size()) return false;
        NamedEntry ne;
        std::memcpy(ne.cKey.data(), data.data() + offset, 16);
        offset += 16;

        // Null-terminated string.
        size_t strStart = offset;
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

    // Phase 1: Parse the root directory for named entries (including sub-dir CKeys).
    std::vector<AssetEntry> assetEntries;
    std::vector<AssetIdxEntry> assetIdxEntries;
    std::vector<NamedEntry> namedEntries;

    size_t offset = 0;
    if (!parseDirectory(data, offset, assetEntries, assetIdxEntries, namedEntries))
        return nullptr;

    // Asset entries from root directory → flat entries with generated paths.
    for (auto& ae : assetEntries) {
        RootEntry re;
        re.cKey = ae.cKey;
        re.path = std::string(getAssetExtension(ae.fileIndex >> 16)) + "/" +
                  std::to_string(ae.fileIndex & 0xFFFF);
        root->m_entries.push_back(std::move(re));
    }

    for (auto& aie : assetIdxEntries) {
        RootEntry re;
        re.cKey = aie.cKey;
        re.path = std::string(getAssetExtension(aie.fileIndex >> 16)) + "/" +
                  std::to_string(aie.fileIndex & 0xFFFF) + "." + std::to_string(aie.subIndex);
        root->m_entries.push_back(std::move(re));
    }

    // Named entries from root → they may be sub-directory CKeys or regular files.
    std::vector<NamedEntry> subdirEntries;
    for (auto& ne : namedEntries) {
        // Heuristic: D3 sub-directories are named entries whose name doesn't contain
        // a file extension. We resolve them if a resolver is provided.
        // For now, add all named entries as root entries.
        RootEntry re;
        re.cKey = ne.cKey;
        re.path = ne.name;
        root->m_entries.push_back(std::move(re));

        // Collect entries that might be sub-directories.
        if (resolver)
            subdirEntries.push_back(ne);
    }

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
                std::string prefix = subNe.name.empty() ? "" : (subNe.name + "/");

                for (auto& ae : subAssets) {
                    RootEntry re;
                    re.cKey = ae.cKey;
                    re.path = prefix + getAssetExtension(ae.fileIndex >> 16) + "/" +
                              std::to_string(ae.fileIndex & 0xFFFF);
                    root->m_entries.push_back(std::move(re));
                }

                for (auto& aie : subAssetIdx) {
                    RootEntry re;
                    re.cKey = aie.cKey;
                    re.path = prefix + getAssetExtension(aie.fileIndex >> 16) + "/" +
                              std::to_string(aie.fileIndex & 0xFFFF) + "." +
                              std::to_string(aie.subIndex);
                    root->m_entries.push_back(std::move(re));
                }

                for (auto& sn : subNamed) {
                    RootEntry re;
                    re.cKey = sn.cKey;
                    re.path = prefix + sn.name;
                    root->m_entries.push_back(std::move(re));
                }
            }
        }
    }

    if (root->m_entries.empty()) return nullptr;

    root->buildIndices(pool);
    return root;
}

std::vector<RootEntry> D3Root::findByPath(const std::string& path) const {
    auto key = toLower(path);
    std::vector<RootEntry> results;
    auto range = m_byPath.equal_range(key);
    for (auto it = range.first; it != range.second; ++it)
        results.push_back(m_entries[it->second]);
    return results;
}

std::vector<RootEntry> D3Root::findByFileDataId(u32 /*fileDataId*/) const {
    // D3 does not use FileDataIds.
    return {};
}

std::vector<RootEntry> D3Root::findByCKey(std::span<const u8, 16> cKey) const {
    std::vector<RootEntry> results;
    for (auto& e : m_entries) {
        if (std::memcmp(e.cKey.data(), cKey.data(), 16) == 0)
            results.push_back(e);
    }
    return results;
}

void D3Root::enumerate(std::function<bool(const RootEntry&)> callback) const {
    for (auto& e : m_entries) {
        if (!callback(e)) break;
    }
}

size_t D3Root::entryCount() const {
    return m_entries.size();
}

void D3Root::buildIndices(interfaces::WorkerPool* pool) {
    size_t n = m_entries.size();

    // Pre-compute lowercase keys in parallel.
    std::vector<std::string> lowerPaths(n);
    if (pool && n > 1000) {
        size_t numThreads = std::max<size_t>(pool->threadCount(), 1);
        size_t chunkSize = (n + numThreads - 1) / numThreads;
        size_t chunks = (n + chunkSize - 1) / chunkSize;

        utils::JobGroup jobGroup;
        jobGroup.add(chunks);
        for (size_t c = 0; c < chunks; ++c) {
            interfaces::WorkerTask task;
            task.fn = [&, c]() {
                size_t start = c * chunkSize;
                size_t end = std::min(start + chunkSize, n);
                for (size_t i = start; i < end; ++i) {
                    if (!m_entries[i].path.empty())
                        lowerPaths[i] = toLower(m_entries[i].path);
                }
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    } else {
        for (size_t i = 0; i < n; ++i) {
            if (!m_entries[i].path.empty())
                lowerPaths[i] = toLower(m_entries[i].path);
        }
    }

    m_byPath.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!lowerPaths[i].empty())
            m_byPath.emplace(std::move(lowerPaths[i]), i);
    }
}

} // namespace whiteout::storages::casc
