
#pragma once

#include <cassert>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>
#include "structures/chunks.h"
#include "structures/skeleton.h"

namespace whiteout {
namespace m2 {

enum class WoWFileSystemMode {
    Read,
    Create,
};

class WoWFileSystem {
public:
    WoWFileSystem(interfaces::VirtualPathFileSystem& pathFs, const std::string& m2Path);

    WoWFileSystem(interfaces::CascFileSystem& cascFs, std::span<const u8> m2Data);

    WoWFileSystem(interfaces::VirtualPathFileSystem& pathFs, const std::string& m2Path,
                  WoWFileSystemMode mode);

    WoWFileSystem(interfaces::CascFileSystem& cascFs, WoWFileSystemMode mode);

    std::span<const u8> getM2Base() const;

    WoWFileSystemMode mode() const {
        return m_mode;
    }

    void setSkinChunk(const SFIDChunk& chunk);
    void setAnimChunk(const AFIDChunk& chunk);
    void setSkeletonChunk(const SKIDChunk& chunk);
    void setParentSkeletonChunk(const SKPDChunk& chunk);

    std::span<const u8> getSkin(u32 skinId, bool isLod);

    std::span<const u8> getAnimBuffer(u16 animId, u16 subAnimId);

    std::span<const u8> getSkeleton();

    void exploratorySearch();

    u32 newSkinFileEntry();

    u32 newLodSkinFileEntry();

    u32 newAnimFileEntry(u16 animId, u16 subAnimId);

    u32 newSkeletonFileEntry();

    SFIDChunk buildSFIDChunk() const;

    AFIDChunk buildAFIDChunk() const;

    SKIDChunk buildSKIDChunk() const;

    void setM2Base(std::vector<u8> data);

    void writeSkinFile(u32 handle, std::vector<u8> data);

    void writeAnimFile(u32 handle, std::vector<u8> data);

    void writeSkeletonFile(u32 handle, std::vector<u8> data);

    void flush();

private:
    WoWFileSystemMode m_mode = WoWFileSystemMode::Read;

    interfaces::VirtualPathFileSystem* m_pathFs = nullptr;
    interfaces::CascFileSystem* m_cascFs = nullptr;

    std::string m_baseStem;

    std::vector<u8> m_m2Storage;
    std::span<const u8> m_m2Data;

    SFIDChunk m_sfid;
    AFIDChunk m_afid;
    SKIDChunk m_skid;

    std::map<u32, std::vector<u8>> m_skinCache;
    std::map<u32, std::vector<u8>> m_lodSkinCache;
    std::map<u32, std::vector<u8>> m_animCache;
    std::vector<u8> m_skelCache;
    bool m_skelLoaded = false;
    bool m_isParentSkeleton = false;

    std::vector<u32> m_registeredSkins;
    std::vector<u32> m_registeredLodSkins;
    std::vector<AFIDEntry> m_registeredAnims;
    u32 m_registeredSkelId = 0;
    u32 m_nextPathIndex = 0;

    static u32 animKey(u16 animId, u16 subAnimId);

    std::string buildSkinPath(u32 skinId, bool isLod) const;

    std::string buildAnimPath(u16 animId, u16 subAnimId) const;

    std::string buildSkelPath() const;

    u32 allocateHandle(const std::string& pathHint);
};

} // namespace m2
} // namespace whiteout
