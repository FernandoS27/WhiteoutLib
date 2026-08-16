
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

    /// @brief Drop a cached `.anim` file. A lazy load copies the keys it wants
    ///        out of the buffer, so holding it after that is pure footprint.
    void evictAnimBuffer(u16 animId, u16 subAnimId);

    /// @brief Defer `.anim` reads to loadSequence() instead of doing them while
    ///        the sequence list is being parsed. See Parser::setLazyAnimations.
    bool lazyAnimations() const {
        return m_lazyAnimations;
    }
    void setLazyAnimations(bool enable) {
        m_lazyAnimations = enable;
    }

    /// @brief Per sequence, in sequence order: does the model carry its keys?
    ///
    /// `flags & 0x130` — CM2Model::LoadSequence's test for "nothing to stream
    /// for this one". M2Init's own test is the narrower `flags & 0x20`; the
    /// extra bits are LoadSequence's "already loaded / in flight" state, which
    /// no file on disk carries.
    ///
    /// Shared here rather than kept on BinaryParseVisitor because one visitor
    /// does not see a whole model: a chunked model declares its sequences in
    /// SKS1 and the bone tracks that index them in SKB1, and ChunkParser builds
    /// a fresh visitor per chunk. The file system is the one object threaded
    /// through all of them.
    std::vector<u8>& sequenceInFile() {
        return m_sequenceInFile;
    }
    const std::vector<u8>& sequenceInFile() const {
        return m_sequenceInFile;
    }

    /// @brief Drop the sequence state, for a model whose sequences are about to
    ///        be read a second time (a skeleton chunk supersedes the `.m2`'s
    ///        own list, and a parent-skeleton reference re-reads both).
    void clearSequenceInFile() {
        m_sequenceInFile.clear();
    }

    /// @brief One `.anim` file's key data, held only while keys are being read
    ///        out of it.
    struct AnimBuffer {
        /// Decompressed AFM2 payload, when the `.anim` is itself chunked. Owns
        /// what `data` points at in that case.
        std::vector<u8> owned;
        std::span<const u8> data;
    };

    /// @brief Read the `.anim` sibling named by @p animId / @p subAnimId,
    ///        unwrapping an AFM2 chunk when @p chunked says the model writes
    ///        them that way. False when the file is not there.
    bool readAnim(AnimBuffer& out, u16 animId, u16 subAnimId, bool chunked);

    std::span<const u8> getSkeleton();

    /// @brief The `.phys` sibling a pre-Legion model names by path when its
    ///        LoadPhysicsData flag is set. Chunked models carry the same data
    ///        inline (PFDC), so CASC-by-id mode has nothing to resolve here.
    std::span<const u8> getPhysics();

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

    void writePhysicsFile(std::vector<u8> data);

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
    std::vector<u8> m_physCache;
    bool m_physLoaded = false;
    bool m_skelLoaded = false;
    bool m_isParentSkeleton = false;
    bool m_lazyAnimations = false;

    std::vector<u8> m_sequenceInFile;

    std::vector<u32> m_registeredSkins;
    std::vector<u32> m_registeredLodSkins;
    std::vector<AFIDEntry> m_registeredAnims;
    u32 m_registeredSkelId = 0;
    u32 m_nextPathIndex = 0;

    static u32 animKey(u16 animId, u16 subAnimId);

    std::string buildSkinPath(u32 skinId, bool isLod) const;

    std::string buildAnimPath(u16 animId, u16 subAnimId) const;

    std::string buildSkelPath() const;

    std::string buildPhysPath() const;

    u32 allocateHandle(const std::string& pathHint);
};

} // namespace m2
} // namespace whiteout
