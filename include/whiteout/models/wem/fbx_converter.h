// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file fbx_converter.h
 * @brief Autodesk FBX ⇄ WEM (FBX_OBJ_DESIGN).
 *
 * The DCC-native scene format: polygons with per-corner layers, a node tree
 * under Autodesk's transform stack, skin clusters, and animation stacks of
 * Euler curves. Like glTF it brings no profile of its own: import produces a
 * `Generic` document and export takes any carried profile.
 *
 * **Import** honours the file's axis declaration and units, evaluates the full
 * transform stack, and resamples animation through it: FBX interpolates Euler
 * angles per component, which no WEM curve reproduces between keys, so the
 * motion crosses and the authoring does not (§8).
 *
 * **Export** declares one of the `AxisPreset`s and writes the SDK's own record
 * conventions, measured from files it wrote (fbx.h). Animation is baked through
 * the evaluator the editor plays, at a frame rate, and reduced.
 */

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../fbx/fbx.h"
#include "../fbx/scene.h"
#include "converter_base.h"
#include "interchange.h"

namespace whiteout {
namespace models {
namespace wem {

struct FbxReadOptions {
    /// Overrides the file's declared axes — for a file whose declaration lies.
    std::optional<AxisPreset> axes;
    /// Restate lengths in centimetres (one `Generic` unit) from the file's
    /// `UnitScaleFactor`; off keeps the file's own numbers.
    bool normaliseUnits = true;
    /// Samples per second for the animation resample; 0 takes the file's.
    f32 sampleRate = 0.0f;
};

struct FbxWriteOptions {
    AxisPreset axes = AxisPreset::YUp;
    /// 7400 reads everywhere; 7500 has 64-bit offsets.
    u32 version = 7400;
    /// The animation bake's samples per second.
    f32 frameRate = 30.0f;
    /// Write only this clip (one file per clip); every clip otherwise.
    std::optional<u32> onlyClip;
    /// Cut polygons into the triangles the model draws.
    bool triangulate = false;
    /// The `UnitScaleFactor` declared — centimetres per file unit — for numbers
    /// the caller has already restated in that unit (FBX_OBJ_DESIGN §16).
    f64 unitScaleFactor = 1.0;
    /// Parent each attached child model under its attach point.
    bool bakeChildModels = true;
    /// The `Creator` the header names.
    std::string creator = "WhiteoutFlakes";
};

/// One file an FBX embeds (a `Video`'s `Content`), for the host to write out:
/// the library touches no filesystem.
struct FbxMedia {
    /// The `Document::textures` entry it is the pixels of.
    u32 texture = kInvalidIndex;
    std::string bytes;
};

/// Where a texture says its image is, both ways the file spells it, for the
/// host's search (FBX_OBJ_DESIGN §6): `RelativeFilename` against the file's
/// folder, then `FileName`. Forward slashes; either may be empty.
struct FbxTextureSource {
    std::string relative;
    std::string absolute;
};

struct FbxImport {
    Document document;
    std::vector<FbxMedia> media;
    /// Indexed like `Document::textures`.
    std::vector<FbxTextureSource> sources;
    /// The `UnitScaleFactor` the file declares, centimetres per unit, whether or
    /// not `FbxReadOptions::normaliseUnits` applied it.
    f64 unitScaleFactor = 1.0;
};

struct FbxExport {
    fbx::File file;
    /// What the host writes beside the file (or embeds), under the names the
    /// file uses.
    std::vector<InterchangeImage> images;
};

/// Puts @p bytes into every `Video` of @p file that names @p name, as its
/// `Content` — the "embed media" option, which Max, Maya and Blender read.
/// False when no video names it.
bool EmbedFbxMedia(fbx::File& file, std::string_view name, std::span<const u8> bytes);

class FbxConverter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    /// Binary or ASCII, by magic. Embedded media is dropped here; the typed
    /// entry point returns it.
    Result<Document> importFromBytes(std::span<const u8> data) const override;
    /// A binary file at @p version (7400 when 0), textures by name.
    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    Result<FbxImport> fromFbx(const fbx::Scene& scene, const FbxReadOptions& options = {}) const;
    Result<FbxExport> toFbx(const Document& document, ProfileId profile,
                            const FbxWriteOptions& options = {}) const;
};

} // namespace wem
} // namespace models
} // namespace whiteout
