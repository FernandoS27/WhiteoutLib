// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file obj_converter.h
 * @brief Wavefront OBJ + MTL ⇄ WEM (FBX_OBJ_DESIGN).
 *
 * OBJ is a static polygon format, and its per-corner index triples are WEM's
 * halfedge-corner model, so geometry crosses with no splitting and no welding:
 * a quad stays a quad both ways. Like glTF it brings no profile of its own —
 * import produces a `Generic` document and export takes any carried profile.
 * There is no skeleton to write, so a skinned model goes out at its bind pose,
 * or posed at a clip time when asked.
 *
 * The material library is a second file: the typed entry points take it parsed,
 * and the byte-level ones work without it.
 */

#include <optional>
#include <string>
#include <vector>

#include "../obj/obj.h"
#include "converter_base.h"
#include "interchange.h"

namespace whiteout {
namespace models {
namespace wem {

struct ObjReadOptions {
    AxisPreset axes = AxisPreset::YUp;
    /// Prefixed to every relative texture path in the library — the folder the
    /// `.mtl` sits in, relative to the `.obj`.
    std::string textureDirectory;
};

struct ObjWriteOptions {
    AxisPreset axes = AxisPreset::YUp;
    /// Cut polygons into the triangles the model draws (its stored diagonals).
    bool triangulate = false;
    /// Write `Pr`/`Pm` and their maps beside the Phong statements.
    bool pbrExtension = true;
    bool vertexColors = true;
    /// Bake the geometry at a clip time; the bind pose otherwise.
    std::optional<PoseAt> pose;
    /// Bake each attached child model in at its attach point.
    bool bakeChildModels = true;
    /// The `mtllib` the `.obj` names; empty writes no library reference.
    std::string materialLibrary;
};

struct ObjExport {
    obj::Asset asset;
    obj::MaterialLibrary materials;
    /// What the host writes next to the files, under the names the library uses.
    std::vector<InterchangeImage> images;
};

class ObjConverter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    /// The geometry alone: the `.mtl` is a second file bytes cannot supply.
    Result<Document> importFromBytes(std::span<const u8> data) const override;
    /// The `.obj` text alone, materials unwritten.
    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    Result<Document> fromObj(const obj::Asset& source, const obj::MaterialLibrary* materials,
                             const ObjReadOptions& options = {}) const;
    Result<ObjExport> toObj(const Document& document, ProfileId profile,
                            const ObjWriteOptions& options = {}) const;
};

} // namespace wem
} // namespace models
} // namespace whiteout
