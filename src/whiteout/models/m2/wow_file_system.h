// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>
#include <whiteout/models/m2/structures/chunks.h>

namespace whiteout {
namespace m2 {

/// Provides lazy access to the satellite files of an M2 bundle.
///
/// Constructed with either:
///   - a VirtualPathFileSystem + the M2 path  (reads the base .m2 via the path FS), or
///   - a CascFileSystem + a pre-loaded span of the base .m2 data.
///
/// After construction the caller feeds chunk metadata (SFID, AFID, SKID) as it
/// is parsed from the MD21 wrapper.  Satellite data is then fetched on demand
/// from the appropriate file system using either paths or file-data IDs.
class WoWFileSystem {
public:
    /// Construct from a path-based FS + on-disk path.  The base .m2 is read immediately.
    WoWFileSystem(interfaces::VirtualPathFileSystem& pathFs, const std::string& m2Path);

    /// Construct from a CASC FS + already-loaded M2 data.
    WoWFileSystem(interfaces::CascFileSystem& cascFs, std::span<const u8> m2Data);

    /// Returns the raw bytes of the base .m2 file.
    std::span<const u8> getM2Base() const;

    // -- chunk metadata setters (call once each, right after parsing the chunk) --

    void setSkinChunk(const SFIDChunk& chunk);
    void setAnimChunk(const AFIDChunk& chunk);
    void setSkeletonChunk(const SKIDChunk& chunk);

    // -- satellite data accessors (lazy: fetched from VFS on first call) --

    /// @param skinId  Zero-based index into the skin or LOD-skin list.
    /// @param isLod   If true, look up in the LOD skin list; otherwise in the
    ///                main skin list.
    std::span<const u8> getSkin(u32 skinId, bool isLod);

    /// @param animId     Animation ID.
    /// @param subAnimId  Sub-animation index.
    std::span<const u8> getAnimBuffer(u16 animId, u16 subAnimId);

    /// Returns the skeleton (.skel) data, or an empty span if no SKID chunk was
    /// set or the file-data ID is zero.
    std::span<const u8> getSkeleton();

    /// List the parent directory via VirtualPathFileSystem::listDirectory() and
    /// load all satellite files (.skel, .skin, .anim) whose names match the
    /// base M2 stem.  Only meaningful when constructed with a
    /// VirtualPathFileSystem and a path was provided at construction.
    void exploratorySearch();

private:
    interfaces::VirtualPathFileSystem* m_pathFs = nullptr;
    interfaces::CascFileSystem* m_cascFs = nullptr;

    // Path components for path-based lookups (empty when constructed from span).
    // m_baseStem = directory + stem, e.g. "creature/nightelfmale/nightelfmale_hd"
    std::string m_baseStem;

    // Base .m2 data (owned when loaded via VFS path, otherwise non-owning view).
    std::vector<u8> m_m2Storage;
    std::span<const u8> m_m2Data;

    // Chunk metadata ---------------------------------------------------------
    SFIDChunk m_sfid;
    AFIDChunk m_afid;
    SKIDChunk m_skid;

    // Cached satellite buffers -----------------------------------------------
    std::map<u32, std::vector<u8>> m_skinCache;
    std::map<u32, std::vector<u8>> m_lodSkinCache;
    std::map<u32, std::vector<u8>> m_animCache; // key = (animId << 16) | subAnimId
    std::vector<u8> m_skelCache;
    bool m_skelLoaded = false;

    static u32 animKey(u16 animId, u16 subAnimId);

    /// Build "{stem}{skinId:02d}.skin" or "{stem}_lod{skinId:02d}.skin".
    std::string buildSkinPath(u32 skinId, bool isLod) const;

    /// Build "{stem}{animId:04d}-{subAnimId:02d}.anim".
    std::string buildAnimPath(u16 animId, u16 subAnimId) const;

    /// Build "{stem}.skel".
    std::string buildSkelPath() const;

};

} // namespace m2
} // namespace whiteout
