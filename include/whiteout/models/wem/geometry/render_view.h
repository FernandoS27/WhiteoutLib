// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file render_view.h
 * @brief The half-edge -> GPU bridge (WEM v3, design §5.8).
 *
 * Editing wants halfedge properties; a GPU wants one vertex per unique attribute
 * tuple. This is the only place the two meet, and it is **not** part of the
 * document — a `RenderMesh` is derived, disposable, and rebuilt whenever the
 * mesh or the requested attribute set changes.
 *
 * Halfedges are grouped on `(mergeGroup, every requested attribute's bytes)`.
 * Keying on `mergeGroup` rather than `VertexId` is what makes the §5.3 manifold
 * repair invisible here: two vertices the repair split apart carry the same
 * merge group, so they regroup into one GPU vertex — exactly as the source had
 * them. That is the whole point of the design's G2:
 *
 * > **Identity property.** For a mesh imported from an all-triangle source where
 * > each source vertex became one `mergeGroup` and its halfedges kept source
 * > order, `BuildRenderMesh` with the source's attribute set reproduces the
 * > source vertex buffer and index buffer **byte-identically** — repaired or not.
 *
 * Emission order is `(mergeGroup, first-seen)`, not first-seen alone. Both are
 * deterministic; only the first reproduces the *source's* vertex order, and the
 * identity property is a statement about the source's buffers, not merely about
 * some stable buffer. Within one merge group, the tie-break is halfedge order
 * within face order — which is where "in halfedge order within face order" from
 * §5.8 lands.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/utils/vertex_buffer.h>

#include "../diagnostics.h"
#include "attributes.h"
#include "ids.h"
#include "mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// One attribute the caller wants in the GPU buffer, by layer name.
///
/// The layer is looked up on the Halfedge domain first and the Vertex domain
/// second, so `position` finds its Vertex layer and `normal`/`uv0` find their
/// Halfedge ones without the caller naming a domain. A request that resolves to
/// nothing is skipped with an informational diagnostic — a mesh with no tangents
/// is a mesh with no tangents, not an error.
struct AttrRequest {
    std::string name;
    utils::AttributeClass attrClass = utils::AttributeClass::Position;
    utils::AttributeEncoding encoding = utils::AttributeEncoding::Float32;
    /// 0 = as many as the layer's `AttrType` has.
    u32 components = 0;
    /// Padding for the attribute's slot, forwarded to `VertexBufferBuilder`.
    u32 align = 0;
};

enum class TriangulationPolicy : u8 {
    /// Every face becomes a fan from its first halfedge. Triangles pass through
    /// untouched, which the identity property depends on.
    FanFromFirstHalfedge,
    /// N-gons are emitted as-is; the index buffer is then not a triangle list.
    /// Only useful for a consumer that does its own tessellation.
    None,
};

struct RenderRange {
    u32 section = 0;
    u32 materialSlot = 0;
    u32 firstIndex = 0;
    u32 indexCount = 0;
};

struct RenderMeshDesc {
    std::vector<AttrRequest> attributes;
    u32 maxInfluences = 4;
    bool wantU16Indices = false;
    /// Emit one range per section, with the faces of a section contiguous. When
    /// false, faces keep mesh order and ranges are the runs that fall out of it.
    bool splitBySection = true;
    TriangulationPolicy triangulation = TriangulationPolicy::FanFromFirstHalfedge;

    /// Append `BlendIndices` / `BlendWeights` from `Mesh::skin`, `maxInfluences`
    /// wide. A section with a `rigidNode` binds every one of its vertices there
    /// at weight 1 without consulting the skin (§5.6).
    bool includeSkin = false;
    utils::AttributeEncoding blendIndexEncoding = utils::AttributeEncoding::UInt8;
    utils::AttributeEncoding blendWeightEncoding = utils::AttributeEncoding::Float32;

    /// A default set: position + normal + uv0, all `Float32`.
    static RenderMeshDesc Standard();
};

struct RenderMesh {
    utils::VertexBuffer vertices;
    std::vector<u32> indices;
    /// Populated only when `wantU16Indices` and the vertex count fits; otherwise
    /// empty, with an `IndexWidthExceeded` diagnostic.
    std::vector<u16> indices16;
    std::vector<RenderRange> ranges;

    /// Per GPU vertex, the WEM vertex and the halfedge its attributes came from.
    /// The halfedge is the *first* of its group in emission order, which is the
    /// one an editor should select when the user picks that GPU vertex.
    std::vector<u32> vertexToWemVertex;
    std::vector<HalfedgeId> vertexToWemHalfedge;

    Diagnostics diagnostics;

    u32 vertexCount() const {
        return static_cast<u32>(vertexToWemVertex.size());
    }
};

/**
 * @brief Builds the GPU view of @p mesh.
 *
 * Works from the mesh's cached connectivity when it has one and from a locally
 * built copy when it does not, so the bulk conversion path never has to
 * materialise half-edge arrays it will not otherwise use. Either way the
 * halfedge numbering is the same — the build is deterministic (§5.9) — so
 * Halfedge-domain layers index correctly in both cases.
 */
RenderMesh BuildRenderMesh(const Mesh& mesh, const RenderMeshDesc& desc);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
