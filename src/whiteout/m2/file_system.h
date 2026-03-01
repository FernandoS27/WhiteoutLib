// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <filesystem>
#include <map>
#include <optional>

#include <whiteout/m2/m2.h>

namespace whiteout {
namespace m2 {

struct AnimVariants {
    // variant index -> file
    std::map<int, std::filesystem::path> variants;
};

struct GroupedFiles {
    std::filesystem::path m2;
    std::optional<std::filesystem::path> skel;

    // animId -> variants
    std::map<int, AnimVariants> anims;

    // index -> skin
    std::map<int, std::filesystem::path> baseSkins;

    // lod -> skin
    std::map<int, std::filesystem::path> lodSkins;

    // index -> bone
    std::map<int, std::filesystem::path> bones;
};

std::optional<GroupedFiles> collectBundle(const std::filesystem::path& m2Path);

GroupedFiles fromFileSystem(const FileSystem& fsys, std::filesystem::path whereTo);

} // namespace m2
} // namespace whiteout
