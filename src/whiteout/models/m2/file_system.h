
#pragma once

#include <filesystem>
#include <map>
#include <optional>

#include "internal_structures.h"

namespace whiteout {
namespace m2 {

struct AnimVariants {

    std::map<int, std::filesystem::path> variants;
};

struct GroupedFiles {
    std::filesystem::path m2;
    std::optional<std::filesystem::path> skel;

    std::map<int, AnimVariants> anims;

    std::map<int, std::filesystem::path> baseSkins;

    std::map<int, std::filesystem::path> lodSkins;

    std::map<int, std::filesystem::path> bones;
};

std::optional<GroupedFiles> collectBundle(const std::filesystem::path& m2Path);

GroupedFiles fromFileSystem(const FileSystem& fsys, std::filesystem::path whereTo);

}
}
