// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/m3/structures/material.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/physics/references.h>

#include "native/m3_walk.h"

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
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

/// A move speed is a length per second and the second does not change, so it
/// rides the geometry's factor (a model restated 100x larger walks 100x faster,
/// or its feet slide), times the change of unit a game states it in. A clip
/// that states no travel acquires none.
void scaleMoveSpeed(Clip& clip, f32 factor) {
    if (const f32 speed = ClipMoveSpeed(clip); speed != 0.0f && factor != 1.0f) {
        SetClipMoveSpeed(clip, speed * factor);
    }
}

/// The translation row of a bind matrix, in `ToMatrix`'s row-vector convention.
/// The 3x3 is a rotation-scale and is dimensionless, so it is left alone —
/// which is also why this is exact for an `IREF` carrying shear.
void scaleMatrixTranslation(Matrix44f& matrix, f32 factor) {
    matrix.data[3][0] *= factor;
    matrix.data[3][1] *= factor;
    matrix.data[3][2] *= factor;
}

} // namespace

/// A Warcraft III light's falloff is the one negative power: it is a factor of
/// `exp(-damping d²) / (1 + linear d + quadratic d²)`, and it falls off over the
/// rescaled model as it did over the original only when each coefficient carries
/// the inverse of its term's length. A camera's f-stop is the same case: the
/// game blurs by `(1/focus - 1/depth) · focalLength² / fStop`.
int ChannelLengthPower(const Model& model, const AnimChannel& channel) {
    // A native stream's slot is its value type, not a field: it carries the
    // `channel` it was defaulted to, so only its record says what it is.
    if (channel.target.kind == TrackTarget::Kind::Native) {
        return NativeChannelLength(model, channel.id).power;
    }
    switch (channel.target.channel) {
    case Channel::Translation:
    case Channel::Target:
    case Channel::AttenuationStart:
    case Channel::AttenuationEnd:
    case Channel::ShadowCastingStart:
    case Channel::ShadowCastingEnd:
    case Channel::FocusDistance:
    case Channel::ClothDriverTranslation:
    case Channel::SoftLumpTranslation:
        return 1;
    case Channel::LinearFalloff:
    case Channel::FStop:
        return -1;
    case Channel::QuadraticFalloff:
    case Channel::Damping:
        return -2;
    case Channel::EmitterProperty: {
        if (channel.target.kind != TrackTarget::Kind::Node ||
            channel.target.node >= model.nodes.size()) {
            return 0;
        }
        const EmitterPropertyDesc* desc =
            FindEmitterProperty(model.nodes.nodes[channel.target.node].kind, channel.target.sub);
        return desc != nullptr && desc->length ? 1 : 0;
    }
    default:
        return 0;
    }
}

namespace {

/// @p factor raised to @p power: what a value carrying that power of length is
/// multiplied by.
f32 powerOf(f32 factor, int power) {
    return power == 1 ? factor : std::pow(factor, static_cast<f32>(power));
}

template <class T>
struct IsAnimRef : std::false_type {};
template <class T>
struct IsAnimRef<native::M3AnimRef<T>> : std::true_type {};

/// A StarCraft II length as one rescale visit meets it: its values times the
/// factor to the field's power, and an AnimRef's id remembered with that power,
/// since a native channel keys the field whose ref names it (F2 of
/// EDIT_MODE_SC2_SCENE_TOOLS_DESIGN.md).
struct LengthScaler {
    f32 factor = 1.0f;
    std::unordered_map<u32, int>* powers = nullptr;

    void operator()(f32& value, int power) const {
        value *= powerOf(factor, power);
    }
    void operator()(Vector2f& value, int power) const {
        value = value * powerOf(factor, power);
    }
    void operator()(Vector3f& value, int power) const {
        scale(value, powerOf(factor, power));
    }
    void operator()(native::M3Extent& extent, int power) const {
        const f32 by = powerOf(factor, power);
        scale(extent.min, by);
        scale(extent.max, by);
        extent.radius *= by;
    }
    /// A frame's translation row; its 3x3 is dimensionless.
    void operator()(Matrix44f& matrix, int power) const {
        scaleMatrixTranslation(matrix, powerOf(factor, power));
    }
    void operator()(std::vector<Vector3f>& points, int power) const {
        for (Vector3f& point : points) {
            (*this)(point, power);
        }
    }
    template <class T>
    void operator()(native::M3AnimRef<T>& ref, int power) const {
        (*this)(ref.initValue, power);
        (*this)(ref.nullValue, power);
        if (powers != nullptr && ref.animId != 0) {
            (*powers)[ref.animId] = power;
        }
    }
};

/// Every length a StarCraft II record kept on @p payload holds, as
/// `f(field, power)` — the mirrors `toM3` writes back (`LITE`, `CAM_`, `PROJ`,
/// `SSGS`, `ATVL`), whose typed fields `RescaleNodePayload` scales beside them.
/// A field is here only where the Art Tools exporter's unit pass
/// (`CMaxExporter::FinalizeExportData`, ×0.01 on the way out) scales it: a
/// camera's near falloffs and a projector's attenuation (a fraction) it writes
/// as they are, and a projector's offset never.
template <class PayloadT, class F>
void forEachPayloadRecordLength(PayloadT& payload, F&& f) {
    const auto get = [&payload]<class T>(std::type_identity<T>) { return std::get_if<T>(&payload); };
    const auto shape = [&f](auto& record) {
        f(record.transform, 1);
        f(record.sizeX, 1);
        f(record.sizeY, 1);
        f(record.sizeZ, 1);
        f(record.vertexPositions, 1);
    };
    if (auto* light = get(std::type_identity<LightPayload>{}); light != nullptr && light->sc2) {
        // `decay` is the far range's AnimRef (`toM3`), beside the plain float.
        f(light->sc2->decay, 1);
        f(light->sc2->attenuationEnd, 1);
        f(light->sc2->attenuationStart, 1);
    } else if (auto* camera = get(std::type_identity<CameraPayload>{}); camera != nullptr && camera->sc2) {
        f(camera->sc2->farClip, 1);
        f(camera->sc2->nearClip, 1);
        f(camera->sc2->shadowClipDistance, 1);
        f(camera->sc2->focusDistance, 1);
        f(camera->sc2->farFocusRange, 1);
        f(camera->sc2->nearFocusRange, 1);
    } else if (auto* projector = get(std::type_identity<ProjectorPayload>{})) {
        auto& record = projector->record;
        f(record.near, 1);
        f(record.far, 1);
        f(record.boxOffsetZBottom, 1);
        f(record.boxOffsetZTop, 1);
        f(record.boxOffsetXLeft, 1);
        f(record.boxOffsetXRight, 1);
        f(record.boxOffsetYFront, 1);
        f(record.boxOffsetYBack, 1);
    } else if (auto* collision = get(std::type_identity<CollisionPayload>{});
               collision != nullptr && collision->shape.sc2) {
        shape(*collision->shape.sc2);
    } else if (auto* attachment = get(std::type_identity<AttachmentPayload>{})) {
        for (auto& volume : attachment->sc2Volumes) {
            shape(volume);
        }
    }
}

/// The node's own records beside its payload's: its shadow box (`SHBX`).
template <class NodeT, class F>
void forEachNodeRecordLength(NodeT& node, F&& f) {
    forEachPayloadRecordLength(node.payload, f);
    if (node.sc2ShadowBox) {
        f(node.sc2ShadowBox->length, 1);
        f(node.sc2ShadowBox->width, 1);
        f(node.sc2ShadowBox->height, 1);
    }
}

/// A leg's ground probe (`IKJT`) and a turret's offset from its group's main
/// one (`PATU`), which only the game runs. A turret's frame is a turn alone.
template <class StageT, class F>
void forEachStageLength(StageT& stage, F&& f) {
    if (stage.legIk) {
        f(stage.legIk->probeUp, 1);
        f(stage.legIk->probeDown, 1);
        f(stage.legIk->maxSpeed, 1);
        f(stage.legIk->goalThreshold, 1);
    }
    if (stage.turret) {
        f(stage.turret->mainBoneOffset, 1);
    }
}

/// The lengths of an `.m3` material block: a standard material's parallax
/// height, the one a `MADD` restores included, and every layer's triplanar
/// offset. Its depth blend falloff, a layer's triplanar scale and the volume,
/// reflection and flare distances the exporter writes as they are.
template <class BlockT, class F>
void forEachMaterialLength(BlockT& block, F&& f) {
    // Read through the mirrors' `reflect`, which a const block cannot call;
    // a const visitor only reads.
    native::M3Material& walked = const_cast<native::M3Material&>(block);
    native::WalkM3Material(walked, [&f](native::M3TextureLayer& layer) { f(layer.triplanarOffset, 1); });
    if (auto* standard = std::get_if<native::M3Standard>(&walked.body)) {
        f(standard->parallaxHeight, 1);
    }
    if (walked.restoredStandard) {
        f(walked.restoredStandard->parallaxHeight, 1);
    }
}

/// The `MADD` properties that restate a standard material's lengths
/// (`data_driven_to_standard.cpp`), by name, with their power.
int DataDrivenLengthPower(std::string_view name) {
    constexpr std::string_view kTriplanarOffset = "TriplanarOffset";
    if (name == "HeightMapScale") {
        return 1;
    }
    if (name.size() >= kTriplanarOffset.size() &&
        name.substr(name.size() - kTriplanarOffset.size()) == kTriplanarOffset) {
        return 1;
    }
    return 0;
}

/// Scales, in place, every live property of a `MADD` blob that
/// `DataDrivenLengthPower` names: the blob is what the export writes, and its
/// layout (`DataDrivenMaterial::decodeProperties`) fixes each value's place
/// and size, so nothing moves.
void scaleDataDrivenBlob(std::vector<u8>& blob, f32 factor) {
    const auto readAt = [&blob]<class T>(u64 offset, T& out) {
        if (offset + sizeof(T) > blob.size()) {
            return false;
        }
        std::memcpy(&out, blob.data() + offset, sizeof(T));
        return true;
    };
    u32 groups = 0;
    u64 keys = 0;
    u64 offsets = 0;
    if (!readAt(0, groups) || !readAt(4, keys) || !readAt(12, offsets) || keys != 20 ||
        offsets != 20ull + 4ull * groups || offsets + 8ull * groups > blob.size()) {
        return;
    }
    for (u32 g = 0; g < groups; ++g) {
        u64 group = 0;
        u32 count = 0;
        u64 hashes = 0;
        u64 sizes = 0;
        u64 types = 0;
        u64 values = 0;
        if (!readAt(offsets + 8ull * g, group) || !readAt(group, count) || !readAt(group + 4, hashes) ||
            !readAt(group + 12, sizes) || !readAt(group + 20, types) || !readAt(group + 28, values) ||
            values + 8ull * count > blob.size() || types + count > blob.size()) {
            continue;
        }
        for (u32 i = 0; i < count; ++i) {
            u32 hash = 0;
            u16 size = 0;
            u64 at = 0;
            if (blob[static_cast<std::size_t>(types) + i] != 1 || !readAt(hashes + 4ull * i, hash) ||
                !readAt(sizes + 2ull * i, size) || !readAt(values + 8ull * i, at) || at + size > blob.size()) {
                continue;
            }
            const char* name = m3::dataDrivenName(hash);
            const int power = name != nullptr ? DataDrivenLengthPower(name) : 0;
            if (power == 0) {
                continue;
            }
            const f32 by = powerOf(factor, power);
            for (u64 v = at; v + sizeof(f32) <= at + size; v += sizeof(f32)) {
                f32 value = 0;
                std::memcpy(&value, blob.data() + v, sizeof(f32));
                value *= by;
                std::memcpy(blob.data() + v, &value, sizeof(f32));
            }
        }
    }
}

/// @p material's `.m3` block with its lengths restated, its sync state carried
/// over: both halves move together, so neither goes stale (§7.1).
void rescaleM3Material(Material& material, const LengthScaler& scaler) {
    const auto* found = std::get_if<native::M3Material>(&material.Native());
    if (found == nullptr) {
        return;
    }
    native::M3Material block = *found;
    forEachMaterialLength(block, scaler);
    if (auto* madd = std::get_if<native::M3DataDriven>(&block.body)) {
        scaleDataDrivenBlob(madd->propertyBlob, scaler.factor);
    }
    if (block.dataDrivenProperties) {
        for (native::M3DataDrivenGroup& group : block.dataDrivenProperties->groups) {
            for (native::M3DataDrivenProperty& property : group.properties) {
                const int power = DataDrivenLengthPower(property.name);
                if (power == 0) {
                    continue;
                }
                const f32 by = powerOf(scaler.factor, power);
                for (std::size_t v = 0; v + sizeof(f32) <= property.data.size(); v += sizeof(f32)) {
                    f32 value = 0;
                    std::memcpy(&value, property.data.data() + v, sizeof(f32));
                    value *= by;
                    std::memcpy(property.data.data() + v, &value, sizeof(f32));
                }
            }
        }
    }
    const NativeSync sync = material.sync();
    if (sync == NativeSync::NativeAuthoritative) {
        material.SetNativeAuthoritative(std::move(block));
    } else {
        material.SetNativeInSync(std::move(block));
        if (sync == NativeSync::CommonEdited) {
            material.MutableCommon();
        }
    }
}

/// Every length the StarCraft II records of @p model hold that are not a
/// node's: the meshes' section bounds (`MSEC`), the game's two stages, and the
/// material blocks.
void rescaleModelRecords(Model& model, const LengthScaler& scaler) {
    for (Mesh& mesh : model.meshes) {
        for (native::M3MeshSection& bounds : mesh.sc2Bounds) {
            scaler(bounds.bounds, 1);
        }
    }
    for (PoseStage& stage : model.poseStages) {
        forEachStageLength(stage, scaler);
    }
    for (ProfileMaterialSet& set : model.profileSets) {
        for (Material& material : set.materials) {
            rescaleM3Material(material, scaler);
        }
    }
}

template <class T>
void scale(Sc2Property<T>& property, f32 factor) {
    if constexpr (std::is_same_v<T, f32>) {
        property.initValue *= factor;
        property.nullValue *= factor;
    } else {
        scale(property.initValue, factor);
        scale(property.nullValue, factor);
    }
}

/// The emitter systems' distances, speeds and accelerations (§10.9) -- the same
/// fields their `EmitterPropertyDesc` rows mark `length`, plus the static ones no
/// track can key. What a field means is the system's own, so only a field whose
/// unit is unambiguous is here: angles, rates, times, colours and counts stay.
/// A StarCraft II field is here exactly when the Art Tools exporter's unit pass
/// scales it (EDIT_MODE_SC2_SCENE_TOOLS_DESIGN.md §3.3): `tailLength`,
/// `instanceDistance`, the bounds and a spline point's variations it writes as
/// they are, and a particle's kill radius it stores squared.
void rescaleEmitter(NodePayload& payload, f32 factor) {
    if (auto* p = std::get_if<Wc3ParticleEmitter1Payload>(&payload)) {
        p->gravity *= factor;
        p->speed *= factor;
    } else if (auto* p2 = std::get_if<Wc3ParticleEmitter2Payload>(&payload)) {
        p2->speed *= factor;
        p2->gravity *= factor;
        p2->width *= factor;
        p2->length *= factor;
        p2->start.scaling *= factor;
        p2->middle.scaling *= factor;
        p2->end.scaling *= factor;
    } else if (auto* m2 = std::get_if<M2ParticleEmitterPayload>(&payload)) {
        m2->speed *= factor;
        m2->gravity = {m2->gravity.x * factor, m2->gravity.y * factor, m2->gravity.z * factor};
        m2->width *= factor;
        m2->length *= factor;
        m2->zSource *= factor;
        for (Vector2f& size : m2->scales) {
            size = {size.x * factor, size.y * factor};
        }
        for (Vector3f& point : m2->splinePoints) {
            point = {point.x * factor, point.y * factor, point.z * factor};
        }
        m2->wind = {m2->wind.x * factor, m2->wind.y * factor, m2->wind.z * factor};
        // The follow line's speeds are the emitter's, a length a second.
        m2->followSpeed1 *= factor;
        m2->followSpeed2 *= factor;
    } else if (auto* r = std::get_if<Wc3RibbonEmitterPayload>(&payload)) {
        r->heightAbove *= factor;
        r->heightBelow *= factor;
        r->gravity *= factor;
    } else if (auto* sp = std::get_if<Sc2ParticleEmitterPayload>(&payload)) {
        scale(sp->initialSpeed, factor);
        scale(sp->initialSpeedRandom, factor);
        sp->killRadius *= factor * factor;
        sp->gravity *= factor;
        sp->noiseAmplitude *= factor;
        scale(sp->sizeAnimation, factor);
        scale(sp->shapeOuter, factor);
        scale(sp->shapeInner, factor);
        scale(sp->outerRadius, factor);
        scale(sp->innerRadius, factor);
        scale(sp->sizeRandomAnimation, factor);
        scale(sp->speed.amplitude, factor);
        scale(sp->size.amplitude, factor);
        for (Sc2Property<Vector3f>& point : sp->splinePoints) {
            scale(point, factor);
        }
    } else if (auto* field = std::get_if<ForceFieldPayload>(&payload)) {
        // The strength is an acceleration, a length a second squared.
        field->strength *= factor;
        field->width *= factor;
        // A cone's height is the cosine of its angle.
        if (field->volume != ForceVolume::Cone) {
            field->height *= factor;
        }
        // An unbounded field's length is not one.
        if (!field->unbounded) {
            field->length *= factor;
        }
        scaleMatrixTranslation(field->transform, factor);
    } else if (auto* warp = std::get_if<VertexWarpPayload>(&payload)) {
        // Its height the exporter writes as it is.
        warp->radius *= factor;
    } else if (auto* sr = std::get_if<Sc2RibbonEmitterPayload>(&payload)) {
        scale(sr->initialSpeed, factor);
        scale(sr->initialSpeedRandom, factor);
        sr->gravityX *= factor;
        sr->gravityY *= factor;
        sr->gravity *= factor;
        scale(sr->sizeAnimation, factor);
        sr->innerRadius *= factor;
        scale(sr->maxLength, factor);
        scale(sr->speed.amplitude, factor);
        scale(sr->size.amplitude, factor);
        sr->noiseAmplitude *= factor;
        for (Sc2RibbonSplinePoint& point : sr->splinePoints) {
            scale(point.emissionOffset, factor);
            point.emissionVector = point.emissionVector * factor;
            point.endTangent = point.endTangent * factor;
            point.endOffset = point.endOffset * factor;
            // Per length: the exporter's `0.01 / |emissionVector|`.
            point.emissionVectorNormFactor /= factor;
            point.velocityNormFactor /= factor;
        }
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

void rescalePayload(NodePayload& payload, const LengthScaler& scaler);

void rescaleNode(Node& node, const LengthScaler& scaler) {
    const f32 factor = scaler.factor;
    scale(node.pivot, factor);
    scale(node.local.translation, factor);
    for (Transform& pose : node.poses) {
        scale(pose.translation, factor);
    }
    for (Matrix44f& matrix : node.poseMatrices) {
        scaleMatrixTranslation(matrix, factor);
    }
    rescalePayload(node.payload, scaler);
    if (node.sc2ShadowBox) {
        scaler(node.sc2ShadowBox->length, 1);
        scaler(node.sc2ShadowBox->width, 1);
        scaler(node.sc2ShadowBox->height, 1);
    }
}

} // namespace

NativeLength NativeChannelLength(const Model& model, u32 animId) {
    NativeLength out;
    if (animId == 0) {
        return out;
    }
    const auto find = [&](u32 node) {
        return [&, node](const auto& field, int power) {
            using Field = std::remove_cvref_t<decltype(field)>;
            if constexpr (IsAnimRef<Field>::value) {
                if (field.animId == animId) {
                    out = {power, node};
                }
            }
        };
    };
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        forEachNodeRecordLength(model.nodes.nodes[n], find(n));
    }
    for (const Mesh& mesh : model.meshes) {
        for (const native::M3MeshSection& bounds : mesh.sc2Bounds) {
            if (bounds.bounds.animId == animId) {
                out = {1, bounds.nodeIndex};
            }
        }
    }
    for (const ProfileMaterialSet& set : model.profileSets) {
        for (const Material& material : set.materials) {
            if (const auto* block = std::get_if<native::M3Material>(&material.Native())) {
                forEachMaterialLength(*block, find(kInvalidNode));
            }
        }
    }
    return out;
}

void RescaleNodeRecords(Node& node, f32 factor) {
    const LengthScaler scaler{factor, nullptr};
    rescalePayload(node.payload, scaler);
    if (node.sc2ShadowBox) {
        scaler(node.sc2ShadowBox->length, 1);
        scaler(node.sc2ShadowBox->width, 1);
        scaler(node.sc2ShadowBox->height, 1);
    }
}

void RescaleNodePayload(NodePayload& payload, f32 factor) {
    rescalePayload(payload, LengthScaler{factor, nullptr});
}

void RescaleGameStage(PoseStage& stage, f32 factor) {
    forEachStageLength(stage, LengthScaler{factor, nullptr});
}

namespace {

void rescalePayload(NodePayload& payload, const LengthScaler& scaler) {
    const f32 factor = scaler.factor;
    forEachPayloadRecordLength(payload, scaler);
    // Only the payload alternatives that hold a length. An attachment's asset
    // key, an event id and a particle system reference are names, and a light's
    // colour and intensity are not distances.
    if (auto* bone = std::get_if<BonePayload>(&payload)) {
        scale(bone->bounds, factor);
        scale(bone->sphere, factor);
    } else if (auto* light = std::get_if<LightPayload>(&payload)) {
        light->attenuationStart *= factor;
        light->attenuationEnd *= factor;
        light->shadowCastingStart *= factor;
        light->shadowCastingEnd *= factor;
        // Per distance and per area (`ChannelLengthPower`).
        light->linearFalloff /= factor;
        light->quadraticFalloff /= factor * factor;
        light->damping /= factor * factor;
    } else if (auto* camera = std::get_if<CameraPayload>(&payload)) {
        // `fov` is an angle and stays; the two clip planes are distances from
        // the camera and move with the model, and the target is a point in it.
        camera->nearClip *= factor;
        camera->farClip *= factor;
        scale(camera->target, factor);
        camera->focusDistance *= factor;
        camera->fStop /= factor; // per distance (`ChannelLengthPower`)
    } else if (auto* collision = std::get_if<CollisionPayload>(&payload)) {
        scale(collision->shape.box, factor);
        scale(collision->shape.sphere, factor);
        collision->shape.height *= factor;
    } else {
        rescaleEmitter(payload, factor);
    }
}

/// A float stream's, and an `.m3` section bounds stream's: its keys are
/// `m3::Extent`s as the file stores them, seven floats of length.
bool isScalableType(const AnimChannel& channel) {
    return isFloatType(channel.valueType) ||
           (channel.target.kind == TrackTarget::Kind::Native && channel.valueType == geom::AttrType::Extent);
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

f32 MoveSpeedUnitsBetween(ProfileId from, ProfileId to) {
    // How many of a profile's speed units its length unit a second makes.
    const auto units = [](ProfileId profile) {
        return static_cast<u32>(profile) < static_cast<u32>(ProfileId::Count) && GameOf(profile) == Game::StarCraft
                   ? 100.0f
                   : 1.0f;
    };
    return units(to) / units(from);
}

RescaleResult RescaleDocument(Document& document, f32 factor) {
    return RescaleDocument(document, factor, factor);
}

RescaleResult RescaleDocument(Document& document, f32 factor, f32 moveSpeedFactor) {
    RescaleResult result;
    result.factor = factor;

    if (!std::isfinite(factor) || factor <= 0.0f || !std::isfinite(moveSpeedFactor) || moveSpeedFactor <= 0.0f) {
        result.diagnostics.error(DiagCode::GeometryRescaled,
                                 "a rescale factor must be finite and positive");
        return result;
    }
    if (factor == 1.0f) {
        // Not a no-op call to be reported as an error: a pair of profiles at the
        // same `sceneScale` asks for exactly this, and the caller should not
        // have to special-case it.
        for (Clip& clip : document.clips) {
            scaleMoveSpeed(clip, moveSpeedFactor);
        }
        result.ok = true;
        return result;
    }

    scale(document.bounds, factor);
    // What one WEM unit measures does not change because the numbers did: a
    // model twice as many units long has units half as large.
    if (document.unitScale > 0.0f) {
        document.unitScale /= factor;
    }

    // What each channel of each model is multiplied by -- 1 for one that is
    // not a length -- so a clip's sub-tracks can be filtered without searching
    // the table per key. A channel id is only meaningful inside one model
    // (§10.8), which is why the map is per model.
    std::vector<std::unordered_map<u32, f32>> lengthChannels(document.models.size());

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

        // The StarCraft II records name their native channels as they go.
        std::unordered_map<u32, int> nativePowers;
        const LengthScaler scaler{factor, &nativePowers};
        for (Node& node : model.nodes.nodes) {
            rescaleNode(node, scaler);
        }
        result.nodesScaled += static_cast<u32>(model.nodes.nodes.size());
        rescaleModelRecords(model, scaler);
        RescalePhysics(model.physics, factor);

        for (AnimChannel& channel : model.animChannels.channels) {
            int power = 0;
            if (channel.target.kind == TrackTarget::Kind::Native) {
                const auto named = nativePowers.find(channel.id);
                power = named != nativePowers.end() ? named->second : 0;
            } else {
                power = ChannelLengthPower(model, channel);
            }
            const f32 multiplier = powerOf(factor, power);
            lengthChannels[modelIndex].emplace(channel.id, multiplier);
            if (power == 0 || !isScalableType(channel)) {
                continue;
            }
            // The rest value an opaque container contributes for an un-keyed
            // channel (§10.8.1) is a value of the channel like any other.
            result.keysScaled += scaleFloats(channel.initValue, multiplier);
        }
    }

    for (Clip& clip : document.clips) {
        scale(clip.bounds, factor);
        scaleMoveSpeed(clip, moveSpeedFactor);
        // An `.m3` event's frame is in its bone's.
        for (ClipEvent& event : clip.events) {
            scaleMatrixTranslation(event.transform, factor);
        }
        if (clip.model >= document.models.size()) {
            continue;
        }
        const Model& model = document.models[clip.model];
        const auto& lengths = lengthChannels[clip.model];
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                const auto found = lengths.find(track.channel);
                if (found == lengths.end() || found->second == 1.0f) {
                    continue;
                }
                const AnimChannel* channel = model.animChannels.find(track.channel);
                if (channel == nullptr || !isScalableType(*channel)) {
                    continue;
                }
                result.keysScaled += scaleFloats(track.values, found->second);
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
