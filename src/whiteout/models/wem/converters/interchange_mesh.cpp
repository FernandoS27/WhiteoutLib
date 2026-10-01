// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "interchange_mesh.h"

#include <cmath>

#include "whiteout/models/wem/anim/pose.h"
#include "whiteout/models/wem/geometry/topology.h"

namespace whiteout {
namespace models {
namespace wem {

std::vector<u32> SurvivingInputFaces(u32 inputFaceCount, const geom::RepairLog& log) {
    std::vector<u32> survivors;
    survivors.reserve(inputFaceCount);
    std::size_t dropped = 0;
    for (u32 f = 0; f < inputFaceCount; ++f) {
        if (dropped < log.droppedFaces.size() && log.droppedFaces[dropped].index == f) {
            ++dropped;
            continue;
        }
        survivors.push_back(f);
    }
    return survivors;
}

PolygonWalk::PolygonWalk(const Mesh& mesh) {
    // Halfedge layers index the cached connectivity, or the deterministic local
    // build that reproduces its numbering when there is none.
    geom::Topology local;
    const geom::Topology* topology = nullptr;
    if (mesh.hasConnectivity()) {
        topology = &mesh.topology();
    } else {
        if (!local.build(mesh.faceSet()).ok()) {
            return;
        }
        topology = &local;
    }
    const u32 faceCount = topology->faceCount();
    faceOfSlot_.assign(faceCount, kInvalidIndex);
    first_.push_back(0);
    for (u32 f = 0; f < faceCount; ++f) {
        const geom::FaceId face(f);
        if (topology->isDeleted(face)) {
            continue;
        }
        faceOfSlot_[f] = static_cast<u32>(slots_.size());
        slots_.push_back(f);
        for (geom::HalfedgeId h : topology->fh(face)) {
            halfedges_.push_back(h.value());
            vertices_.push_back(topology->from(h).value());
        }
        first_.push_back(static_cast<u32>(halfedges_.size()));
    }
    ok_ = true;
}

u32 PolygonWalk::cornerOf(u32 face, u32 vertex) const {
    const std::span<const u32> corners = vertices(face);
    for (u32 k = 0; k < corners.size(); ++k) {
        if (corners[k] == vertex) {
            return k;
        }
    }
    return kInvalidIndex;
}

std::vector<Matrix44f> SkinningAt(const Document& document, u32 model,
                                  const std::optional<PoseAt>& pose, Diagnostics& diagnostics) {
    std::vector<Matrix44f> skin;
    if (!pose.has_value()) {
        return skin;
    }
    if (pose->clip >= document.clips.size()) {
        diagnostics.warn(DiagCode::ClipTargetMissing,
                         "the requested pose names clip " + std::to_string(pose->clip) +
                             ", which the document does not have; exported at bind");
        return skin;
    }
    if (document.clips[pose->clip].model != model) {
        return skin;
    }
    const ClipPose clip(document, model, pose->clip);
    if (!clip.empty()) {
        clip.skinningAt(pose->seconds, skin);
    }
    return skin;
}

Matrix44f NodeFrame(const Document& document, u32 model, u32 node,
                    const std::optional<PoseAt>& pose) {
    const Model& owner = document.models[model];
    if (pose.has_value() && pose->clip < document.clips.size() &&
        document.clips[pose->clip].model == model) {
        const ClipPose clip(document, model, pose->clip);
        if (!clip.empty()) {
            return clip.frame(node, pose->seconds);
        }
    }
    return ToMatrix(owner.nodes.worldBind(node));
}

std::vector<bool> ClaimedChildModels(const Document& document) {
    std::vector<bool> claimed(document.models.size(), false);
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        for (const Node& node : document.models[m].nodes.nodes) {
            const AttachmentPayload* attachment = std::get_if<AttachmentPayload>(&node.payload);
            if (attachment != nullptr && attachment->model != kInvalidIndex &&
                attachment->model < document.models.size() && attachment->model != m) {
                claimed[attachment->model] = true;
            }
        }
    }
    return claimed;
}

Vector3f PlacePoint(const Matrix44f& m, const Vector3f& p) {
    return {p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] + m.data[3][0],
            p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] + m.data[3][1],
            p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] + m.data[3][2]};
}

Vector3f PlaceDirection(const Matrix44f& m, const Vector3f& d) {
    Vector3f out{d.x * m.data[0][0] + d.y * m.data[1][0] + d.z * m.data[2][0],
                 d.x * m.data[0][1] + d.y * m.data[1][1] + d.z * m.data[2][1],
                 d.x * m.data[0][2] + d.y * m.data[1][2] + d.z * m.data[2][2]};
    const f32 length = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z);
    return length > 1e-12f ? out * (1.0f / length) : d;
}

} // namespace wem
} // namespace models
} // namespace whiteout
