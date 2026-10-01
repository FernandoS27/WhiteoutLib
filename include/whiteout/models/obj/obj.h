// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file obj.h
 * @brief Wavefront OBJ and its MTL material library, in memory
 *        (FBX_OBJ_DESIGN §1).
 *
 * The struct keeps the file's own model: three index pools (positions, UVs,
 * normals) and polygons whose corners each name one entry of every pool. That
 * per-corner triple is exactly WEM's halfedge-corner model, so nothing is split
 * or welded on the way in or out. Coordinates and UVs are the file's own; the
 * basis and the V flip are the converter's business.
 */

#include <optional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace obj {

inline constexpr u32 kNone = 0xFFFFFFFFu;

/// One polygon corner: 0-based pool indices, `kNone` where the file gave none.
struct Corner {
    u32 position = kNone;
    u32 uv = kNone;
    u32 normal = kNone;
};

/// The `o` / `g` names in force when a face was read.
struct Group {
    std::string object;
    std::string group;
};

struct Face {
    u32 firstCorner = 0;
    u32 cornerCount = 0;
    u32 group = 0;          ///< Into `Asset::groups`.
    u32 material = kNone;   ///< Into `Asset::materials` (`usemtl`).
    u32 smoothing = 0;      ///< `s N`; 0 is `s off`.
};

struct Asset {
    std::vector<Vector3f> positions;
    /// The de-facto `v x y z r g b` extension: empty, or one per position.
    std::vector<Vector3f> colors;
    std::vector<Vector2f> uvs;
    std::vector<Vector3f> normals;
    std::vector<Corner> corners;
    std::vector<Face> faces;
    std::vector<Group> groups;
    /// `usemtl` names, in first-use order.
    std::vector<std::string> materials;
    /// `mtllib` file names, as written.
    std::vector<std::string> materialLibraries;
    /// Whether any `s` statement was read — without one, a file with no normals
    /// gets the default shading angle rather than faceting.
    bool smoothingStated = false;
    /// Lines, points and free-form geometry the format allows and a mesh cannot hold.
    u32 skippedElements = 0;
};

// ---------------------------------------------------------------------------
// MTL
// ---------------------------------------------------------------------------

/// A `map_*` statement: the file and the options a mesh exporter writes back.
struct TextureMap {
    std::string file;
    Vector2f offset{0.0f, 0.0f}; ///< `-o u v`.
    Vector2f scale{1.0f, 1.0f};  ///< `-s u v`.
    bool clamp = false;          ///< `-clamp on`.
    f32 bumpMultiplier = 1.0f;   ///< `-bm`.

    bool present() const {
        return !file.empty();
    }
};

struct Material {
    std::string name;
    std::optional<Vector3f> ambient;  ///< `Ka`
    std::optional<Vector3f> diffuse;  ///< `Kd`
    std::optional<Vector3f> specular; ///< `Ks`
    std::optional<Vector3f> emissive; ///< `Ke`
    std::optional<f32> exponent;      ///< `Ns`
    std::optional<f32> dissolve;      ///< `d`, or `1 - Tr`
    std::optional<f32> ior;           ///< `Ni`
    std::optional<i32> illum;
    std::optional<f32> roughness;     ///< `Pr` (the PBR extension)
    std::optional<f32> metallic;      ///< `Pm`

    TextureMap diffuseMap;   ///< `map_Kd`
    TextureMap specularMap;  ///< `map_Ks`
    TextureMap exponentMap;  ///< `map_Ns`
    TextureMap dissolveMap;  ///< `map_d`
    TextureMap emissiveMap;  ///< `map_Ke`
    TextureMap bumpMap;      ///< `map_Bump`, `bump`
    TextureMap normalMap;    ///< `norm`
    TextureMap roughnessMap; ///< `map_Pr`
    TextureMap metallicMap;  ///< `map_Pm`

    /// Whether the material speaks the PBR extension.
    bool isPbr() const {
        return roughness.has_value() || metallic.has_value() || roughnessMap.present() ||
               metallicMap.present();
    }
};

struct MaterialLibrary {
    std::vector<Material> materials;

    const Material* find(const std::string& name) const {
        for (const Material& material : materials) {
            if (material.name == name) {
                return &material;
            }
        }
        return nullptr;
    }
};

} // namespace obj
} // namespace models
} // namespace whiteout
