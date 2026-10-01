// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file interchange_mesh.h
 * @brief Geometry plumbing the polygon-format converters (OBJ, FBX) share.
 *
 * Both formats carry polygons with per-corner attributes, so export walks the
 * WEM faces and their halfedges directly instead of the render view's
 * triangles; and both import through `MeshBuilder`, whose repair can drop a
 * degenerate face — Face-domain data then has to be carried by the survivor map.
 */

#include <optional>
#include <span>
#include <vector>

#include "whiteout/models/wem/diagnostics.h"
#include "whiteout/models/wem/document.h"
#include "whiteout/models/wem/geometry/repair.h"
#include "whiteout/models/wem/interchange.h"

namespace whiteout {
namespace models {
namespace wem {

/// The input face each face of a `MeshBuilder::build` result came from: the
/// output order is the input order minus `log.droppedFaces`.
std::vector<u32> SurvivingInputFaces(u32 inputFaceCount, const geom::RepairLog& log);

/// Every live face of a mesh, in order, with its corners as halfedge ids (the
/// index of every Halfedge-domain layer) and vertex ids.
class PolygonWalk {
public:
    explicit PolygonWalk(const Mesh& mesh);

    bool ok() const {
        return ok_;
    }
    u32 faceCount() const {
        return static_cast<u32>(slots_.size());
    }
    /// The face's slot — what `faceSections()` and Face layers index.
    u32 slot(u32 face) const {
        return slots_[face];
    }
    std::span<const u32> halfedges(u32 face) const {
        return {halfedges_.data() + first_[face], first_[face + 1] - first_[face]};
    }
    std::span<const u32> vertices(u32 face) const {
        return {vertices_.data() + first_[face], first_[face + 1] - first_[face]};
    }
    /// The face's corner whose vertex is @p vertex, or `kInvalidIndex`.
    u32 cornerOf(u32 face, u32 vertex) const;
    /// Face index by slot, `kInvalidIndex` for a slot with no live face.
    u32 faceOfSlot(u32 slot) const {
        return slot < faceOfSlot_.size() ? faceOfSlot_[slot] : kInvalidIndex;
    }

private:
    bool ok_ = false;
    std::vector<u32> slots_;
    std::vector<u32> first_;
    std::vector<u32> halfedges_;
    std::vector<u32> vertices_;
    std::vector<u32> faceOfSlot_;
};

/// The skinning matrices that pose model @p model at @p pose, empty for the
/// bind pose (no request, or a clip that animates another model).
std::vector<Matrix44f> SkinningAt(const Document& document, u32 model,
                                  const std::optional<PoseAt>& pose, Diagnostics& diagnostics);

/// Node @p node's model-space frame (row-vector) at @p pose, or at bind.
Matrix44f NodeFrame(const Document& document, u32 model, u32 node,
                    const std::optional<PoseAt>& pose);

/// Which models an attachment in another model claims (each at most once).
std::vector<bool> ClaimedChildModels(const Document& document);

/// `p * M` and `n * M3` for a row-vector placement matrix.
Vector3f PlacePoint(const Matrix44f& m, const Vector3f& p);
Vector3f PlaceDirection(const Matrix44f& m, const Vector3f& d);

} // namespace wem
} // namespace models
} // namespace whiteout
