#include "m2_file_system.h"

#include <string>
#include <sstream>
#include <iomanip>

namespace m2 {

namespace {

std::string format_number(int num, int width) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(width) << num;
    return oss.str();
}

std::string format_anim_id(int anim_id, int variant) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << anim_id << "-" << std::setw(2) << variant;
    return oss.str();
}

std::optional<std::pair<int, int>> parseAnimSuffix(std::string_view s) {
    // expects "0069-01.anim"
    if (s.size() < 11) return std::nullopt;
    if (s[4] != '-' || s.substr(7) != ".anim") return std::nullopt;

    int anim = std::stoi(std::string(s.substr(0, 4)));
    int var  = std::stoi(std::string(s.substr(5, 2)));
    return {{anim, var}};
}

std::optional<int> parseBaseSkin(std::string_view s) {
    // "00.skin"
    if (!s.ends_with(".skin")) return std::nullopt;
    return std::stoi(std::string(s.substr(0, s.size() - 5)));
}

std::optional<int> parseLodSkin(std::string_view s) {
    // "_lod03.skin"
    if (!s.starts_with("_lod") || !s.ends_with(".skin")) return std::nullopt;
    return std::stoi(std::string(s.substr(4, 2)));
}

std::optional<int> parseBone(std::string_view s) {
    // "_07.bone"
    if (!s.starts_with("_") || !s.ends_with(".bone")) return std::nullopt;
    return std::stoi(std::string(s.substr(1, s.size() - 6)));
}

} // namespace

std::optional<M2GroupedFiles> collectM2Bundle(const std::filesystem::path& m2Path) {
    using namespace std::filesystem;

    M2GroupedFiles out;
    out.m2 = m2Path;
    if (!m2Path.has_extension()) {
        out.m2 += ".m2";
    }

    if (out.m2.extension() != ".m2") {
        return std::nullopt;
    }

    if (out.m2.is_relative()) {
        out.m2 = current_path() / out.m2;
    }

    const path dir = out.m2.parent_path();
    const std::string base = out.m2.stem().string();

    for (const directory_entry& e : directory_iterator(dir)) {
        if (!e.is_regular_file()) {
            continue;
        }

        const path& p = e.path();
        const std::string name = p.filename().string();

        if (!name.starts_with(base)) {
            continue;
        }

        const std::string suffix = name.substr(base.size());
        const std::string ext = p.extension().string();

        if (ext == ".skel") {
            if (name == base + ".skel") {
                out.skel = p;
            }
        }
        else if (ext == ".anim") {
            if (auto av = parseAnimSuffix(suffix)) {
                auto& [animId, variant] = *av;
                out.anims[animId].variants[variant] = p;
            }
        }
        else if (ext == ".skin") {
            if (auto lod = parseLodSkin(suffix)) {
                out.lodSkins[*lod] = p;
            } else if (auto idx = parseBaseSkin(suffix)) {
                out.baseSkins[*idx] = p;
            }
        }
        else if (ext == ".bone") {
            if (auto idx = parseBone(suffix)) {
                out.bones[*idx] = p;
            }
        }
    }

    return out;
}

M2GroupedFiles
fromM2FileSystem(const M2FileSystem& fsys,
                 std::filesystem::path whereTo)
{
    namespace fs = std::filesystem;
    std::filesystem::path dir = whereTo;
    std::filesystem::path base = whereTo.stem();
    if (base.empty()) {
        if (fsys.baseName.empty()) {
            base = "just_another_model";
        } else {
            base = std::filesystem::path(fsys.baseName).stem();
        }
    } else {
        dir = whereTo.parent_path();
    }

    if (dir.is_relative()) {
        dir = std::filesystem::current_path() / dir;
    }

    fs::create_directories(dir);

    M2GroupedFiles out;

    // --------------------------------------------------
    // Base M2
    // --------------------------------------------------
    out.m2 = dir / fs::path(base.string() + ".m2");

    // --------------------------------------------------
    // Skeleton (exactly one)
    // --------------------------------------------------
    if (fsys.skeleton.has_value()) {
        out.skel = dir / fs::path(base.string() + ".skel");
    }

    // --------------------------------------------------
    // Skins
    // --------------------------------------------------
    for (const M2SkinFile& skin : fsys.skins) {
        if (skin.isLodSkin) {
            fs::path p = dir /
                fs::path(base.string() + "_lod" +
                 format_number(skin.lodLevel, 2) + ".skin");

            out.lodSkins[skin.lodLevel] = p;
        } else {
            fs::path p = dir /
                fs::path(base.string() +
                 format_number(skin.index, 2) + ".skin");

            out.baseSkins[skin.index] = p;
        }
    }

    // --------------------------------------------------
    // Animations
    // --------------------------------------------------
    for (const M2AnimFile& anim : fsys.anims) {
        fs::path p = dir /
            fs::path(base.string() +
             format_anim_id(anim.animId, anim.variant) +
             ".anim");

        out.anims[anim.animId].variants[anim.variant] = p;
    }

    // --------------------------------------------------
    // Bones
    // --------------------------------------------------
    for (size_t i = 0; i < fsys.bones.size(); ++i) {
        fs::path p = dir /
            fs::path(base.string() + "_" +
             format_number(static_cast<int>(i), 2) +
             ".bone");

        out.bones[static_cast<int>(i)] = p;
    }

    return out;
}

}  // namespace m2
