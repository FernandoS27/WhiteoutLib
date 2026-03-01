#pragma once

#include <map>
#include <filesystem>
#include <optional>

#include "../include/m2/structures.h"

namespace whiteout {
namespace m2 {

struct AnimVariants {
    // variant index -> file
    std::map<int, std::filesystem::path> variants;
};

struct M2GroupedFiles {
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

std::optional<M2GroupedFiles> collectM2Bundle(const std::filesystem::path& m2Path);

M2GroupedFiles fromM2FileSystem(const M2FileSystem& fsys, std::filesystem::path whereTo);

}  // namespace m2
}  // namespace whiteout
