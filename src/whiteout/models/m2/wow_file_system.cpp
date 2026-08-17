
#include "wow_file_system.h"

#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "chunk_parser.h"

#include <cctype>
#include <string_view>

namespace whiteout {
namespace m2 {

namespace {

namespace fs = std::filesystem;

std::string extractBaseStem(const std::string& m2Path) {
    fs::path const p(m2Path);
    return (p.parent_path() / p.stem()).string();
}

std::string zeroPad(int value, int width) {
    std::string s = std::to_string(value);
    if (static_cast<int>(s.size()) < width) {
        s.insert(0, width - s.size(), '0');
    }
    return s;
}

// Sibling names come from listfiles, extractors, and the client itself
// ("_LOD00.skin" vs a listfile's "_lod00.skin"); the game's own lookups are
// case-insensitive, so ours are too.
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool iendsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

} // namespace

WoWFileSystem::WoWFileSystem(interfaces::VirtualPathFileSystem& pathFs, const std::string& m2Path)
    : m_mode(WoWFileSystemMode::Read), m_pathFs(&pathFs), m_baseStem(extractBaseStem(m2Path)),
      m_m2Storage(pathFs.readFile(m2Path)), m_m2Data(m_m2Storage) {}

WoWFileSystem::WoWFileSystem(interfaces::CascFileSystem& cascFs, std::span<const u8> m2Data)
    : m_mode(WoWFileSystemMode::Read), m_cascFs(&cascFs), m_m2Data(m2Data) {}

WoWFileSystem::WoWFileSystem(interfaces::VirtualPathFileSystem& pathFs, const std::string& m2Path,
                             WoWFileSystemMode mode)
    : m_mode(mode), m_pathFs(&pathFs), m_baseStem(extractBaseStem(m2Path)) {
    if (mode == WoWFileSystemMode::Read) {
        m_m2Storage = pathFs.readFile(m2Path);
        m_m2Data = m_m2Storage;
    }
}

WoWFileSystem::WoWFileSystem(interfaces::CascFileSystem& cascFs, WoWFileSystemMode mode)
    : m_mode(mode), m_cascFs(&cascFs) {
    assert(mode == WoWFileSystemMode::Create);
}

std::span<const u8> WoWFileSystem::getM2Base() const {
    return m_m2Data;
}

void WoWFileSystem::setSkinChunk(const SFIDChunk& chunk) {
    m_sfid = chunk;
}
void WoWFileSystem::setAnimChunk(const AFIDChunk& chunk) {
    m_afid = chunk;
}
void WoWFileSystem::setSkeletonChunk(const SKIDChunk& chunk) {
    m_skid = chunk;
}

void WoWFileSystem::setPhysicsChunk(const PFIDChunk& chunk) {
    m_pfid = chunk;
}
void WoWFileSystem::setBoneChunk(const BFIDChunk& chunk) {
    m_bfid = chunk;
}
void WoWFileSystem::setParentSkeletonChunk(const SKPDChunk& chunk) {
    SKIDChunk parentSkid;
    parentSkid.skeletonFileDataId = chunk.parentSkeletonFileId;
    m_skid = parentSkid;
    m_skelLoaded = false;
    m_isParentSkeleton = true;
}

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
        std::string const path = buildSkinPath(skinId, isLod);
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
    u32 const key = animKey(animId, subAnimId);
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
        // No fileExists() pre-check, unlike getSkin: an empty read already
        // means "not there", and a file system that answers fileExists by
        // reading the file — which the renderer's content-provider wrapper does,
        // having no cheaper probe — would otherwise read every `.anim` twice.
        data = m_pathFs->readFile(buildAnimPath(animId, subAnimId));
    }

    auto& buf = m_animCache[key];
    buf = std::move(data);
    return buf;
}

void WoWFileSystem::evictAnimBuffer(u16 animId, u16 subAnimId) {
    m_animCache.erase(animKey(animId, subAnimId));
}

bool WoWFileSystem::readAnim(AnimBuffer& out, u16 animId, u16 subAnimId, bool chunked) {
    auto animData = getAnimBuffer(animId, subAnimId);
    if (animData.empty())
        return false;

    if (!chunked) {
        out.data = animData;
        return true;
    }

    ChunkParser chunkParser;
    common::span_streambuf sbuf(animData);
    common::BinaryReader animReader(sbuf);
    AnimFile animFile;
    animFile.animId = animId;
    animFile.variant = subAnimId;
    chunkParser.parseChunkedAnim(animReader, animFile, this, true);
    if (!animFile.profile.afm2_chunk)
        return false;
    out.owned = std::move(animFile.profile.afm2_chunk->animationData);
    out.data = out.owned;
    return true;
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
        // Parent skeletons are referenced by file data ID, which requires CASC.
        // In path-based mode we can only load the skeleton that shares the same
        // base stem as the .m2 file; cross-directory parent skeletons cannot be resolved.
        if (m_isParentSkeleton) {
            return {};
        }
        std::string const path = buildSkelPath();
        if (!m_pathFs->fileExists(path)) {
            return {};
        }
        m_skelCache = m_pathFs->readFile(path);
    }
    return m_skelCache;
}

std::span<const u8> WoWFileSystem::getPhysics() {
    if (m_physLoaded) {
        return m_physCache;
    }
    m_physLoaded = true;

    if (m_cascFs) {
        if (m_pfid.physFileDataId == 0) {
            return {};
        }
        m_physCache = m_cascFs->readFile(m_pfid.physFileDataId);
    } else {
        std::string const path = buildPhysPath();
        if (m_pathFs->fileExists(path)) {
            m_physCache = m_pathFs->readFile(path);
        }
    }
    return m_physCache;
}

u32 WoWFileSystem::animKey(u16 animId, u16 subAnimId) {
    return (static_cast<u32>(animId) << 16) | subAnimId;
}

std::string WoWFileSystem::buildSkinPath(u32 skinId, bool isLod) const {
    std::string const suffix = isLod ? "_lod" + zeroPad(skinId, 2) : zeroPad(skinId, 2);
    return m_baseStem + suffix + ".skin";
}

std::string WoWFileSystem::buildAnimPath(u16 animId, u16 subAnimId) const {
    return m_baseStem + zeroPad(animId, 4) + "-" + zeroPad(subAnimId, 2) + ".anim";
}

std::string WoWFileSystem::buildSkelPath() const {
    return m_baseStem + ".skel";
}

std::string WoWFileSystem::buildPhysPath() const {
    return m_baseStem + ".phys";
}

std::string WoWFileSystem::buildBonePath(u32 index) const {
    return m_baseStem + "_" + zeroPad(static_cast<int>(index), 2) + ".bone";
}

// Path mode has no BFID to count, so the sibling run is probed instead. The
// files are numbered from zero without gaps in the WoW corpus, so the first
// miss ends the run.
void WoWFileSystem::scanBones() {
    if (m_boneScanned) {
        return;
    }
    m_boneScanned = true;

    if (!m_bfid.boneFileDataIds.empty()) {
        m_boneCount = static_cast<u32>(m_bfid.boneFileDataIds.size());
        return;
    }
    if (!m_pathFs || m_baseStem.empty()) {
        return;
    }
    while (m_pathFs->fileExists(buildBonePath(m_boneCount))) {
        ++m_boneCount;
    }
}

u32 WoWFileSystem::boneCount() {
    scanBones();
    return m_boneCount;
}

std::span<const u8> WoWFileSystem::getBone(u32 index) {
    scanBones();
    if (index >= m_boneCount) {
        return {};
    }
    if (auto it = m_boneCache.find(index); it != m_boneCache.end()) {
        return it->second;
    }

    std::vector<u8> data;
    if (m_cascFs && index < m_bfid.boneFileDataIds.size()) {
        const u32 fileId = m_bfid.boneFileDataIds[index];
        if (fileId != 0) {
            data = m_cascFs->readFile(fileId);
        }
    } else if (m_pathFs) {
        std::string const path = buildBonePath(index);
        if (m_pathFs->fileExists(path)) {
            data = m_pathFs->readFile(path);
        }
    }
    return m_boneCache.emplace(index, std::move(data)).first->second;
}

void WoWFileSystem::writeBoneFile(u32 index, std::vector<u8> data) {
    m_boneCache[index] = std::move(data);
}

void WoWFileSystem::exploratorySearch() {
    if (!m_pathFs || m_baseStem.empty()) {
        return;
    }

    fs::path const basePath(m_baseStem);
    fs::path const dir = basePath.parent_path();
    std::string const stem = basePath.filename().string();

    auto entries = m_pathFs->listDirectory(dir.string());

    for (const auto& entry : entries) {
        if (entry.isDirectory)
            continue;

        if (entry.name.size() <= stem.size())
            continue;
        if (!iequals(std::string_view(entry.name).substr(0, stem.size()), stem))
            continue;

        std::string suffix = entry.name.substr(stem.size());
        std::string const fullPath = (dir / entry.name).string();

        if (iequals(suffix, ".skel")) {
            if (!m_skelLoaded) {
                m_skelCache = m_pathFs->readFile(fullPath);
                m_skelLoaded = true;
            }
            continue;
        }

        if (iequals(suffix, ".phys")) {
            if (!m_physLoaded) {
                m_physCache = m_pathFs->readFile(fullPath);
                m_physLoaded = true;
            }
            continue;
        }

        if (iendsWith(suffix, ".skin")) {
            std::string mid = suffix.substr(0, suffix.size() - 5);
            if (mid.size() == 2 && std::isdigit(static_cast<unsigned char>(mid[0])) &&
                std::isdigit(static_cast<unsigned char>(mid[1]))) {
                u32 const skinId = static_cast<u32>(std::stoi(mid));
                if (!m_skinCache.contains(skinId)) {
                    m_skinCache[skinId] = m_pathFs->readFile(fullPath);
                }
            } else if (mid.size() == 6 && iequals(mid.substr(0, 4), "_lod") &&
                       std::isdigit(static_cast<unsigned char>(mid[4])) &&
                       std::isdigit(static_cast<unsigned char>(mid[5]))) {
                u32 const lodId = static_cast<u32>(std::stoi(mid.substr(4)));
                if (!m_lodSkinCache.contains(lodId)) {
                    m_lodSkinCache[lodId] = m_pathFs->readFile(fullPath);
                }
            }
            continue;
        }

        // `.anim` siblings are deliberately not pulled in here. Nothing reads
        // one during the parse any more: m2::loadSequence asks for the single
        // file a sequence needs, by name, when that sequence is loaded — which
        // for a lazy parse is not until something plays it.
    }
}

u32 WoWFileSystem::allocateHandle(const std::string& pathHint) {
    if (m_cascFs) {
        auto id = m_cascFs->reserveFileId(pathHint);
        return id.value_or(0);
    }
    return m_nextPathIndex++;
}

u32 WoWFileSystem::newSkinFileEntry() {
    assert(m_mode == WoWFileSystemMode::Create);
    u32 const index = static_cast<u32>(m_registeredSkins.size());
    u32 const handle = allocateHandle(buildSkinPath(index, false));
    m_registeredSkins.push_back(handle);
    return handle;
}

u32 WoWFileSystem::newLodSkinFileEntry() {
    assert(m_mode == WoWFileSystemMode::Create);
    u32 const index = static_cast<u32>(m_registeredLodSkins.size());
    u32 const handle = allocateHandle(buildSkinPath(index, true));
    m_registeredLodSkins.push_back(handle);
    return handle;
}

u32 WoWFileSystem::newSkeletonFileEntry() {
    assert(m_mode == WoWFileSystemMode::Create);
    u32 const handle = allocateHandle(buildSkelPath());
    m_registeredSkelId = handle;
    return handle;
}

u32 WoWFileSystem::newAnimFileEntry(u16 animId, u16 subAnimId) {
    assert(m_mode == WoWFileSystemMode::Create);
    u32 const handle = allocateHandle(buildAnimPath(animId, subAnimId));
    m_registeredAnims.push_back(AFIDEntry{animId, subAnimId, handle});
    return handle;
}

SFIDChunk WoWFileSystem::buildSFIDChunk() const {
    assert(m_mode == WoWFileSystemMode::Create);
    SFIDChunk chunk;
    chunk.skinFileDataIds = m_registeredSkins;
    chunk.lodSkinFileDataIds = m_registeredLodSkins;
    return chunk;
}

AFIDChunk WoWFileSystem::buildAFIDChunk() const {
    assert(m_mode == WoWFileSystemMode::Create);
    AFIDChunk chunk;
    chunk.animFileIds = m_registeredAnims;
    return chunk;
}

SKIDChunk WoWFileSystem::buildSKIDChunk() const {
    assert(m_mode == WoWFileSystemMode::Create);
    SKIDChunk chunk;
    chunk.skeletonFileDataId = m_registeredSkelId;
    return chunk;
}

void WoWFileSystem::setM2Base(std::vector<u8> data) {
    assert(m_mode == WoWFileSystemMode::Create);
    m_m2Storage = std::move(data);
    m_m2Data = m_m2Storage;
}

void WoWFileSystem::writeSkinFile(u32 handle, std::vector<u8> data) {
    assert(m_mode == WoWFileSystemMode::Create);

    for (u32 i = 0; i < static_cast<u32>(m_registeredSkins.size()); ++i) {
        if (m_registeredSkins[i] == handle) {
            m_skinCache[handle] = std::move(data);
            return;
        }
    }
    for (u32 i = 0; i < static_cast<u32>(m_registeredLodSkins.size()); ++i) {
        if (m_registeredLodSkins[i] == handle) {
            m_lodSkinCache[handle] = std::move(data);
            return;
        }
    }
    assert(false && "writeSkinFile: handle not found in registered skins or LOD skins");
}

void WoWFileSystem::writeAnimFile(u32 handle, std::vector<u8> data) {
    assert(m_mode == WoWFileSystemMode::Create);
    m_animCache[handle] = std::move(data);
}

void WoWFileSystem::writeSkeletonFile([[maybe_unused]] u32 handle, std::vector<u8> data) {
    assert(m_mode == WoWFileSystemMode::Create);
    assert(handle == m_registeredSkelId && "writeSkeletonFile: handle mismatch");
    m_skelCache = std::move(data);
    m_skelLoaded = true;
}

void WoWFileSystem::writePhysicsFile(std::vector<u8> data, u32 cascFileId) {
    assert(m_mode == WoWFileSystemMode::Create);
    m_physCache = std::move(data);
    m_physFileId = cascFileId;
    m_physLoaded = true;
}

void WoWFileSystem::flush() {
    assert(m_mode == WoWFileSystemMode::Create);

    if (m_pathFs) {

        if (!m_m2Storage.empty()) {
            m_pathFs->writeFile(m_baseStem + ".m2", m_m2Storage);
        }

        for (u32 i = 0; i < static_cast<u32>(m_registeredSkins.size()); ++i) {
            u32 const handle = m_registeredSkins[i];
            auto it = m_skinCache.find(handle);
            if (it != m_skinCache.end()) {
                m_pathFs->writeFile(buildSkinPath(i, false), it->second);
            }
        }

        for (u32 i = 0; i < static_cast<u32>(m_registeredLodSkins.size()); ++i) {
            u32 const handle = m_registeredLodSkins[i];
            auto it = m_lodSkinCache.find(handle);
            if (it != m_lodSkinCache.end()) {
                m_pathFs->writeFile(buildSkinPath(i, true), it->second);
            }
        }

        for (const auto& entry : m_registeredAnims) {
            auto it = m_animCache.find(entry.fileDataId);
            if (it != m_animCache.end()) {
                m_pathFs->writeFile(buildAnimPath(entry.animId, entry.subAnimId), it->second);
            }
        }

        if (m_skelLoaded && !m_skelCache.empty()) {
            m_pathFs->writeFile(buildSkelPath(), m_skelCache);
        }

        if (m_physLoaded && !m_physCache.empty()) {
            m_pathFs->writeFile(buildPhysPath(), m_physCache);
        }

        for (const auto& [index, data] : m_boneCache) {
            if (!data.empty()) {
                m_pathFs->writeFile(buildBonePath(index), data);
            }
        }
    } else if (m_cascFs) {

        for (u32 const handle : m_registeredSkins) {
            auto it = m_skinCache.find(handle);
            if (it != m_skinCache.end() && handle != 0) {
                m_cascFs->writeFile(handle, it->second);
            }
        }

        for (u32 const handle : m_registeredLodSkins) {
            auto it = m_lodSkinCache.find(handle);
            if (it != m_lodSkinCache.end() && handle != 0) {
                m_cascFs->writeFile(handle, it->second);
            }
        }

        for (const auto& entry : m_registeredAnims) {
            auto it = m_animCache.find(entry.fileDataId);
            if (it != m_animCache.end() && entry.fileDataId != 0) {
                m_cascFs->writeFile(entry.fileDataId, it->second);
            }
        }

        if (m_skelLoaded && !m_skelCache.empty() && m_registeredSkelId != 0) {
            m_cascFs->writeFile(m_registeredSkelId, m_skelCache);
        }

        if (m_physLoaded && !m_physCache.empty() && m_physFileId != 0) {
            m_cascFs->writeFile(m_physFileId, m_physCache);
        }

        // BFID is the only thing naming these in CASC mode, so a model that
        // never carried one cannot write its `.bone` files back.
        for (const auto& [index, data] : m_boneCache) {
            if (data.empty() || index >= m_bfid.boneFileDataIds.size()) {
                continue;
            }
            if (const u32 fileId = m_bfid.boneFileDataIds[index]; fileId != 0) {
                m_cascFs->writeFile(fileId, data);
            }
        }
    }
}

} // namespace m2
} // namespace whiteout
