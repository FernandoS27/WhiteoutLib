// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/nodes/node.h>

namespace whiteout {
namespace models {
namespace wem {

Transform Compose(const Transform& parent, const Transform& child) {
    Transform out;
    // Scale, then rotate, then translate — the order every TRS pipeline in this
    // tree uses, and the one D3's Skeleton_ComposeWorldPose performs.
    const Vector3f scaled{child.translation.x * parent.scale.x,
                          child.translation.y * parent.scale.y,
                          child.translation.z * parent.scale.z};
    out.translation = parent.translation + parent.rotation.rotate_vector(scaled);
    out.rotation = parent.rotation * child.rotation;
    out.scale = Vector3f{parent.scale.x * child.scale.x, parent.scale.y * child.scale.y,
                         parent.scale.z * child.scale.z};
    return out;
}

Transform Inverse(const Transform& transform) {
    Transform out;
    out.scale = Vector3f{transform.scale.x != 0 ? 1.0f / transform.scale.x : 0.0f,
                         transform.scale.y != 0 ? 1.0f / transform.scale.y : 0.0f,
                         transform.scale.z != 0 ? 1.0f / transform.scale.z : 0.0f};
    out.rotation = transform.rotation.inverse();
    const Vector3f rotated = out.rotation.rotate_vector(transform.translation);
    out.translation =
        Vector3f{-rotated.x * out.scale.x, -rotated.y * out.scale.y, -rotated.z * out.scale.z};
    return out;
}

Vector3f TransformPoint(const Transform& transform, const Vector3f& point) {
    const Vector3f scaled{point.x * transform.scale.x, point.y * transform.scale.y,
                          point.z * transform.scale.z};
    return transform.translation + transform.rotation.rotate_vector(scaled);
}

const char* ToString(NodeKind kind) {
    switch (kind) {
    case NodeKind::Helper:
        return "helper";
    case NodeKind::Bone:
        return "bone";
    case NodeKind::Attachment:
        return "attachment";
    case NodeKind::Light:
        return "light";
    case NodeKind::Camera:
        return "camera";
    case NodeKind::ParticleEmitter:
        return "particle_emitter";
    case NodeKind::RibbonEmitter:
        return "ribbon_emitter";
    case NodeKind::Event:
        return "event";
    case NodeKind::CollisionShape:
        return "collision_shape";
    case NodeKind::Count:
        break;
    }
    return "invalid";
}

void Node::resetPayloadForKind() {
    switch (kind) {
    case NodeKind::Helper:
        payload = HelperPayload{};
        break;
    case NodeKind::Bone:
        payload = BonePayload{};
        break;
    case NodeKind::Attachment:
        payload = AttachmentPayload{};
        break;
    case NodeKind::Light:
        payload = LightPayload{};
        break;
    case NodeKind::Camera:
        payload = CameraPayload{};
        break;
    case NodeKind::ParticleEmitter:
        payload = ParticlePayload{};
        break;
    case NodeKind::RibbonEmitter:
        payload = RibbonPayload{};
        break;
    case NodeKind::Event:
        payload = EventPayload{};
        break;
    case NodeKind::CollisionShape:
        payload = CollisionPayload{};
        break;
    case NodeKind::Count:
        break;
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
