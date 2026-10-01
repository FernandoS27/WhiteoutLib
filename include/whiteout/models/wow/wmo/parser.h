// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "group.h"
#include "root.h"

namespace whiteout {
namespace interfaces {
class CascFileSystem;
class VirtualPathFileSystem;
} // namespace interfaces

namespace models {
namespace wow {
namespace wmo {

/// A root and the group files it names.
struct Model {
    Root root;
    /// One per group the root declares; empty where the file could not be read.
    std::vector<std::optional<Group>> groups;
    /// `lodGroups[l - 1][g]`: group @p g's file at LOD level @p l, for the
    /// levels loadLod() was asked for. Empty where the group has no file there.
    std::vector<std::vector<std::optional<Group>>> lodGroups;
};

/// What kind of `.wmo` a buffer holds. Both kinds open with MVER; the chunk
/// after it tells them apart.
enum class FileKind { Unknown, Root, Group };

FileKind detectFileKind(std::span<const u8> data);

class Parser {
public:
    Parser();
    ~Parser();

    /// A root file. Fails — returns nullopt — where 12.1 fails the load: an
    /// MVER other than 17, a chunk running past the end, or no MOHD.
    std::optional<Root> parseRoot(std::span<const u8> data);

    /// One group file. The client validates nothing here, so neither does
    /// this; nullopt only for a buffer too short to hold the MOGP header.
    std::optional<Group> parseGroup(std::span<const u8> data);

    /// The root in @p rootData and its groups at LOD 0, read by the
    /// FileDataIDs its GFID names — the only way 12.1 finds them.
    std::optional<Model> parse(interfaces::CascFileSystem& fs, std::span<const u8> rootData);

    /// The root at @p rootPath and its groups at LOD 0, read beside it as
    /// `<stem>_NNN.wmo` — the names the community listfile gives them, and so
    /// the layout of a loose extraction. The client has no such naming.
    std::optional<Model> parse(interfaces::VirtualPathFileSystem& fs, const std::string& rootPath);

    /// Read LOD level @p lod (1 and up) of every group that has one: MOGI
    /// flag 0x400 and an MGI2 `lodIndex` at least @p lod. False when the root
    /// declares no such level.
    bool loadLod(Model& model, u32 lod, interfaces::CascFileSystem& fs);
    bool loadLod(Model& model, u32 lod, interfaces::VirtualPathFileSystem& fs,
                 const std::string& rootPath);

    bool hasIssues() const;
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

/// `<stem>_NNN.wmo`, or `<stem>_NNN_lodL.wmo` above LOD 0, for a root at
/// @p rootPath: the listfile's names for its group files.
std::string groupFilePath(const std::string& rootPath, u32 group, u32 lod);

} // namespace wmo
} // namespace wow
} // namespace models
} // namespace whiteout
