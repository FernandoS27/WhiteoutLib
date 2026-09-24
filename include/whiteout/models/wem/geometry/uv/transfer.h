// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file transfer.h
 * @brief Transfer: a texture laid out on one UV set, redrawn for another
 *        (EDIT_MODE_UV_DESIGN.md §9.3).
 *
 * Every target texel an island covers is traced back through its triangle to
 * the same point on the surface in the source set, and the source image is read
 * there. What comes out is the same paint on the new layout -- which is what
 * lets a modeller redraw a painted map without repainting it.
 *
 * A normal map is not paint: its texels are directions in the frame its own
 * set's UVs make, so a texel is decoded through the source triangle's frame and
 * re-encoded through the target's. Without that, a layout that turned an
 * island a quarter would turn every bump on it the wrong way.
 */

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

#include "../mesh.h"
#include "islands.h"

#include <optional>
#include <span>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

/// How a normal map spells its direction in its channels.
enum class NormalPacking : u8 {
    None,       ///< Not packed: x, y and z in R, G and B.
    XInAlpha,   ///< x in A and y in G, z rebuilt: the DXT5nm spelling.
    TwoChannel, ///< x in R and y in G, z rebuilt: BC5's.
};

struct TransferSource {
    const textures::Texture* image = nullptr;
    /// Whether a UV past the edge repeats the image, per axis; clamped when not.
    bool wrapU = true;
    bool wrapV = true;
    /// Colour stored in sRGB: sampled after linearising, re-encoded after.
    bool srgb = false;
    /// How the image's normals are spelled. Read only when the image's kind is
    /// `TextureKind::Normal`.
    NormalPacking packing = NormalPacking::None;
};

/// One mesh to redraw, and which of its sets the image is read from and
/// written for.
struct TransferMesh {
    const Mesh* mesh = nullptr;
    u32 from = 0;
    u32 to = 1;
    /// The islands of `to`, for telling two islands apart where they share
    /// texels. Optional: without them the mesh is one owner.
    const UvIslands* toIslands = nullptr;
};

struct TransferOptions {
    /// The image written. 0 is the source's own size.
    u32 width = 0;
    u32 height = 0;
    /// Rings of texels filled outward from what the islands cover, so a mip
    /// does not bleed the empty tile into an island's edge.
    u32 padding = 4;
};

struct TransferReport {
    /// Texels an island covered.
    u32 written = 0;
    /// Texels the padding filled.
    u32 dilated = 0;
    /// Faces with no area in the target set, which draw nothing and so could
    /// not be redrawn.
    u32 unmappedFaces = 0;
    /// Texels two islands both covered with different values: a stacked
    /// target whose sources disagree, where the last island wins.
    u32 twiceDiffering = 0;
};

/// Redraws @p source for each mesh's `to` set. Nothing when there is no image
/// or it cannot be decoded; an RGBA8 image otherwise, sRGB when the source was.
std::optional<textures::Texture> TransferTexture(std::span<const TransferMesh> meshes,
                                                 const TransferSource& source,
                                                 const TransferOptions& options,
                                                 TransferReport& report);

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
