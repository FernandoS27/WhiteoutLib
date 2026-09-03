// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/nodes/node.h>

#include <cmath>

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

Matrix44f ToMatrix(const Transform& transform) {
    // `Matrix44f::rotation` is the column-vector matrix of the same rotation, so
    // the row-vector 3x3 is its transpose; scaling first means row i is scaled
    // by s_i.
    const Matrix44f rotation = Matrix44f::rotation(transform.rotation);
    const f32 scale[3] = {transform.scale.x, transform.scale.y, transform.scale.z};
    Matrix44f out = Matrix44f::identity();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.data[r][c] = scale[r] * rotation.data[c][r];
        }
    }
    out.data[3][0] = transform.translation.x;
    out.data[3][1] = transform.translation.y;
    out.data[3][2] = transform.translation.z;
    return out;
}

Transform FromMatrix(const Matrix44f& matrix) {
    Transform out;
    out.translation = Vector3f{matrix.data[3][0], matrix.data[3][1], matrix.data[3][2]};

    // Row i of the 3x3 is s_i times a unit row of the rotation, so its length is
    // the scale and dividing it out leaves the rotation.
    f32 scale[3];
    for (int r = 0; r < 3; ++r) {
        scale[r] = std::sqrt(matrix.data[r][0] * matrix.data[r][0] +
                             matrix.data[r][1] * matrix.data[r][1] +
                             matrix.data[r][2] * matrix.data[r][2]);
    }
    const f32 det =
        matrix.data[0][0] *
            (matrix.data[1][1] * matrix.data[2][2] - matrix.data[1][2] * matrix.data[2][1]) -
        matrix.data[0][1] *
            (matrix.data[1][0] * matrix.data[2][2] - matrix.data[1][2] * matrix.data[2][0]) +
        matrix.data[0][2] *
            (matrix.data[1][0] * matrix.data[2][1] - matrix.data[1][1] * matrix.data[2][0]);
    // A mirrored bone: the sign has to land on the scale, because the remainder
    // must stay a rotation for the quaternion to exist at all.
    if (det < 0.0f) {
        scale[0] = -scale[0];
    }
    out.scale = Vector3f{scale[0], scale[1], scale[2]};

    // Transposed back into the column-vector form `extract_rotation` reads, with
    // the scale already divided out so it has nothing left to remove.
    Matrix44f pure = Matrix44f::identity();
    for (int r = 0; r < 3; ++r) {
        const f32 inv = scale[r] != 0.0f ? 1.0f / scale[r] : 0.0f;
        for (int c = 0; c < 3; ++c) {
            pure.data[c][r] = matrix.data[r][c] * inv;
        }
    }
    out.rotation = pure.extract_rotation();
    return out;
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
