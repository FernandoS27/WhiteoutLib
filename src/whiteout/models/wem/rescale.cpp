// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/retarget.h>

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {

namespace {

void scale(Vector3f& value, f32 factor) {
    value.x *= factor;
    value.y *= factor;
    value.z *= factor;
}

void scale(Extent& extent, f32 factor) {
    scale(extent.minimum, factor);
    scale(extent.maximum, factor);
    extent.sphereRadius *= factor;
}

void scale(Sphere& sphere, f32 factor) {
    scale(sphere.center, factor);
    sphere.radius *= factor;
}

/// The translation row of a bind matrix, in `ToMatrix`'s row-vector convention.
/// The 3x3 is a rotation-scale and is dimensionless, so it is left alone —
/// which is also why this is exact for an `IREF` carrying shear.
void scaleMatrixTranslation(Matrix44f& matrix, f32 factor) {
    matrix.data[3][0] *= factor;
    matrix.data[3][1] *= factor;
    matrix.data[3][2] *= factor;
}

/// Whether a channel's values are lengths. Everything else a channel can drive
/// — a rotation, a scale, a colour, an alpha, a UV, a texture index — is
/// dimensionless and survives a rescale untouched.
bool isLengthChannel(Channel channel) {
    switch (channel) {
    case Channel::Translation:
    case Channel::AttenuationStart:
    case Channel::AttenuationEnd:
        return true;
    default:
        return false;
    }
}

/// Scale every `f32` in @p bytes. Valid only for a float-typed channel, which
/// the caller checks: a `Translation` keyed as `F32x3` and one keyed as a single
/// `F32` are both lengths and both scale componentwise, and a Hermite or Bezier
/// key's tangents are in the same units as the value they flank, so scaling the
/// whole block is right for every interpolation `SubTrack` allows.
u32 scaleFloats(std::vector<u8>& bytes, f32 factor) {
    const std::size_t count = bytes.size() / sizeof(f32);
    for (std::size_t i = 0; i < count; ++i) {
        f32 value = 0;
        std::memcpy(&value, bytes.data() + i * sizeof(f32), sizeof(f32));
        value *= factor;
        std::memcpy(bytes.data() + i * sizeof(f32), &value, sizeof(f32));
    }
    return static_cast<u32>(count);
}

bool isFloatType(geom::AttrType type) {
    switch (type) {
    case geom::AttrType::F32:
    case geom::AttrType::F32x2:
    case geom::AttrType::F32x3:
    case geom::AttrType::F32x4:
        return true;
    default:
        return false;
    }
}

void rescaleNode(Node& node, f32 factor) {
    scale(node.pivot, factor);
    scale(node.local.translation, factor);
    for (Transform& pose : node.poses) {
        scale(pose.translation, factor);
    }
    for (Matrix44f& matrix : node.poseMatrices) {
        scaleMatrixTranslation(matrix, factor);
    }

    // Only the payload alternatives that hold a length. An attachment's asset
    // key, an event id and a particle system reference are names, and a light's
    // colour and intensity are not distances.
    if (auto* bone = std::get_if<BonePayload>(&node.payload)) {
        scale(bone->bounds, factor);
        scale(bone->sphere, factor);
    } else if (auto* light = std::get_if<LightPayload>(&node.payload)) {
        light->attenuationStart *= factor;
        light->attenuationEnd *= factor;
    } else if (auto* camera = std::get_if<CameraPayload>(&node.payload)) {
        // `fov` is an angle and stays; the two clip planes are distances from
        // the camera and move with the model.
        camera->nearClip *= factor;
        camera->farClip *= factor;
    } else if (auto* collision = std::get_if<CollisionPayload>(&node.payload)) {
        scale(collision->shape.box, factor);
        scale(collision->shape.sphere, factor);
        collision->shape.height *= factor;
    }
}

} // namespace

f32 RescaleFactorBetween(ProfileId from, ProfileId to) {
    if (static_cast<u32>(from) >= static_cast<u32>(ProfileId::Count) ||
        static_cast<u32>(to) >= static_cast<u32>(ProfileId::Count)) {
        return 1.0f;
    }
    const f32 source = Profile(from).sceneScale;
    const f32 target = Profile(to).sceneScale;
    if (!(source > 0.0f) || !(target > 0.0f)) {
        return 1.0f;
    }
    return source / target;
}

RescaleResult RescaleDocument(Document& document, f32 factor) {
    RescaleResult result;
    result.factor = factor;

    if (!std::isfinite(factor) || factor <= 0.0f) {
        result.diagnostics.error(DiagCode::GeometryRescaled,
                                 "a rescale factor must be finite and positive");
        return result;
    }
    if (factor == 1.0f) {
        // Not a no-op call to be reported as an error: a pair of profiles at the
        // same `sceneScale` asks for exactly this, and the caller should not
        // have to special-case it.
        result.ok = true;
        return result;
    }

    scale(document.bounds, factor);
    // What one WEM unit measures does not change because the numbers did: a
    // model twice as many units long has units half as large.
    if (document.unitScale > 0.0f) {
        document.unitScale /= factor;
    }

    // Which channels of which model are lengths, so a clip's sub-tracks can be
    // filtered without searching the table per key. A channel id is only
    // meaningful inside one model (§10.8), which is why the map is per model.
    std::vector<std::unordered_map<u32, bool>> lengthChannels(document.models.size());

    for (std::size_t modelIndex = 0; modelIndex < document.models.size(); ++modelIndex) {
        Model& model = document.models[modelIndex];
        scale(model.bounds, factor);

        for (Mesh& mesh : model.meshes) {
            scale(mesh.bounds, factor);
            for (MeshSection& section : mesh.sections) {
                scale(section.bounds, factor);
            }
            const std::span<Vector3f> positions =
                mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
            for (Vector3f& position : positions) {
                scale(position, factor);
            }
            result.verticesScaled += static_cast<u32>(positions.size());
        }

        for (Node& node : model.nodes.nodes) {
            rescaleNode(node, factor);
        }
        result.nodesScaled += static_cast<u32>(model.nodes.nodes.size());

        for (AnimChannel& channel : model.animChannels.channels) {
            const bool isLength = isLengthChannel(channel.target.channel);
            lengthChannels[modelIndex].emplace(channel.id, isLength);
            if (!isLength || !isFloatType(channel.valueType)) {
                continue;
            }
            // The rest value an opaque container contributes for an un-keyed
            // channel (§10.8.1) is a value of the channel like any other.
            result.keysScaled += scaleFloats(channel.initValue, factor);
        }
    }

    for (Clip& clip : document.clips) {
        scale(clip.bounds, factor);
        if (clip.model >= document.models.size()) {
            continue;
        }
        const Model& model = document.models[clip.model];
        const auto& lengths = lengthChannels[clip.model];
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                const auto found = lengths.find(track.channel);
                if (found == lengths.end() || !found->second) {
                    continue;
                }
                const AnimChannel* channel = model.animChannels.find(track.channel);
                if (channel == nullptr || !isFloatType(channel->valueType)) {
                    continue;
                }
                result.keysScaled += scaleFloats(track.values, factor);
            }
        }
    }

    result.ok = true;
    result.diagnostics.info(DiagCode::GeometryRescaled,
                            "every length restated at " + std::to_string(factor) +
                                "x: " + std::to_string(result.verticesScaled) + " vertices, " +
                                std::to_string(result.nodesScaled) + " nodes, " +
                                std::to_string(result.keysScaled) + " keys");
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
