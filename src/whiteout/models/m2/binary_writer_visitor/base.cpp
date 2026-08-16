
#include "../../../common/binary_writer.h"
#include "../binary_writer_visitor.h"

#include <type_traits>

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

BinaryWriterVisitor::BinaryWriterVisitor(common::BinaryWriter& writer) : writer(writer) {}

void BinaryWriterVisitor::start() {
    baseOffset = writer.getPosition();
}

void BinaryWriterVisitor::visit(const GlobalFlags& flags) {
    writer.write(static_cast<u32>(flags.value));
}

void BinaryWriterVisitor::visit(const GlobalSequence& seq) {
    writer.write(seq.timestamp);
}

void BinaryWriterVisitor::visit(const Sequence& seq) {
    writer.write(seq.id);
    writer.write(seq.variationIndex);
    if (version != 0 && version < M2_VERSION_WOTLK) {
        // ≤TBC sequences occupy a window on a shared global timeline instead
        // of carrying a duration; the windows were laid out before the
        // sequence list started writing (see visit(Model)).
        const auto& window = legacyWindows[legacyWindowIndex++];
        writer.write(window.start);
        writer.write(window.end);
    } else {
        writer.write(seq.duration);
    }
    writer.write(seq.movespeed);
    writer.write(seq.flags);
    writer.write(seq.frequency);
    writer.write(seq.padding);
    writer.write(seq.replayMin);
    writer.write(seq.replayMax);
    if (version != 0 && version < M2_VERSION_MOP) {
        // One u32 blend time before the WoD-era in/out split.
        writer.write<u32>(seq.blendTimeIn);
    } else {
        writer.write(seq.blendTimeIn);
        writer.write(seq.blendTimeOut);
    }

    writer.write(seq.bounding.minimum);
    writer.write(seq.bounding.maximum);

    writer.write(seq.bounding.sphereRadius);

    writer.write(seq.variationNext);
    writer.write(seq.aliasNext);

    // Mirror of the parser's `flags & 0x130` in-file test: anything else keys
    // its data through a `.anim` sibling — a WotLK-and-later concept; earlier
    // versions carry everything in the model.
    const bool inFile = (static_cast<u32>(seq.flags) & 0x130u) != 0;
    if (!inFile && version >= M2_VERSION_WOTLK) {
        animDataBuffers.push_back(AnimDataBuffer{seq.id, seq.variationIndex, {}});
        auto& animBuffer = animDataBuffers.back();
        animDataWriterStates.push_back({});
        auto& writerState = animDataWriterStates.back();
        writerState.streambuf = std::make_unique<common::vector_streambuf>(animBuffer.data);
        writerState.stream = std::make_unique<std::ostream>(writerState.streambuf.get());
        writerState.writer = std::make_unique<common::BinaryWriter>(*writerState.stream);
        animDataWriters.push_back(writerState.writer.get());
    } else {
        animDataWriters.push_back(&writer);
    }
}

void BinaryWriterVisitor::visit(const Vertex& vertex) {
    writer.write(vertex.position.x);
    writer.write(vertex.position.y);
    writer.write(vertex.position.z);

    for (int i = 0; i < 4; ++i) {
        writer.write(vertex.boneWeights[i]);
    }
    for (int i = 0; i < 4; ++i) {
        writer.write(vertex.boneIndices[i]);
    }

    writer.write(vertex.normal.x);
    writer.write(vertex.normal.y);
    writer.write(vertex.normal.z);

    for (int i = 0; i < 2; ++i) {
        writer.write(vertex.texCoords[i].x);
        writer.write(vertex.texCoords[i].y);
    }
}

void BinaryWriterVisitor::visit(const Bone& bone) {
    writer.write(bone.keyBoneId);
    writer.write(bone.flags);
    writer.write(bone.parentBoneId);
    writer.write(bone.submeshId);
    writer.write(bone.boneNameCRC);

    visit(bone.translation);
    writeLegacyBoneRotation(bone.rotation);
    visit(bone.scale);

    writer.write(bone.pivot.x);
    writer.write(bone.pivot.y);
    writer.write(bone.pivot.z);
}

void BinaryWriterVisitor::writeLegacyBoneRotation(const AnimationTrack<CompatQuaternion>& track) {
    if (version != 0 && version < M2_VERSION_BC) {
        writeLegacyTrack<Vector4f>(track,
                                   [](const CompatQuaternion& q) { return decompressQuat(q); });
        return;
    }
    visit(track);
}

void BinaryWriterVisitor::visit(const Texture& texture) {
    writer.write(texture.type);
    writer.write(texture.flags);
    visit(texture.filename);
}

void BinaryWriterVisitor::visit(const Material& material) {
    writer.write(material.flags);
    writer.write(material.blendingMode);
}

void BinaryWriterVisitor::visit(const TextureWeight& weight) {
    visit(weight.weight);
}

void BinaryWriterVisitor::visit(const TextureTransform& transform) {
    visit(transform.translation);
    visit(transform.rotation);
    visit(transform.scaling);
}

void BinaryWriterVisitor::visit(const ColorAnimation& color) {
    visit(color.color);
    visit(color.alpha);
}

void BinaryWriterVisitor::visit(const Light& light) {
    writer.write(light.type);
    writer.write(light.boneId);
    writer.write(light.position.x);
    writer.write(light.position.y);
    writer.write(light.position.z);

    visit(light.ambientColor);
    visit(light.ambientIntensity);
    visit(light.diffuseColor);
    visit(light.diffuseIntensity);
    visit(light.attenuationStart);
    visit(light.attenuationEnd);
    visit(light.visibility);
}

void BinaryWriterVisitor::visit(const CameraSpline& spline) {
    writer.write(spline.value.x);
    writer.write(spline.value.y);
    writer.write(spline.value.z);
    writer.write(spline.inTangent.x);
    writer.write(spline.inTangent.y);
    writer.write(spline.inTangent.z);
    writer.write(spline.outTangent.x);
    writer.write(spline.outTangent.y);
    writer.write(spline.outTangent.z);
}

void BinaryWriterVisitor::visit(const Camera& camera) {
    writer.write(camera.type);
    if (version < M2_VERSION_CATA) {
        writer.write(camera.fieldOfView);
    }
    writer.write(camera.farClip);
    writer.write(camera.nearClip);

    visit(camera.positions);

    writer.write(camera.positionBase.x);
    writer.write(camera.positionBase.y);
    writer.write(camera.positionBase.z);

    visit(camera.targetPositions);

    writer.write(camera.targetPositionBase.x);
    writer.write(camera.targetPositionBase.y);
    writer.write(camera.targetPositionBase.z);

    visit(camera.roll);
    if (version >= M2_VERSION_CATA) {
        visit(camera.fieldOfViewTrack);
    }
}

void BinaryWriterVisitor::visit(const Attachment& attachment) {
    writer.write(attachment.id);
    writer.write(attachment.boneId);
    writer.write(attachment.unknown);
    writer.write(attachment.position.x);
    writer.write(attachment.position.y);
    writer.write(attachment.position.z);
    visit(attachment.animate);
}

void BinaryWriterVisitor::visit(const RibbonEmitter& emitter) {
    writer.write(emitter.ribbonId);
    writer.write(emitter.boneId);
    writer.write(emitter.position.x);
    writer.write(emitter.position.y);
    writer.write(emitter.position.z);

    visit(emitter.textureIndices);
    visit(emitter.materialIndices);

    visit(emitter.colorTrack);
    visit(emitter.alphaTrack);
    visit(emitter.heightAbove);
    visit(emitter.heightBelow);

    writer.write(emitter.edgesPerSecond);
    writer.write(emitter.edgeLifetime);
    writer.write(emitter.gravity);
    writer.write(emitter.textureRows);
    writer.write(emitter.textureCols);

    visit(emitter.texSlot);
    visit(emitter.visibility);

    if (version == 0 || version >= M2_VERSION_WOTLK) {
        writer.write(emitter.priorityPlane);
        writer.write(emitter.ribbonColorIndex);
        writer.write(emitter.textureTransformIndex);
    }
}

void BinaryWriterVisitor::visit(const ParticleEmitter& emitter) {
    const bool legacy = version != 0 && version < M2_VERSION_WOTLK;

    writer.write(emitter.particleId);
    writer.write(emitter.flags);
    writer.write(emitter.position.x);
    writer.write(emitter.position.y);
    writer.write(emitter.position.z);
    writer.write(emitter.boneId);
    writer.write(emitter.textureId);

    visit(emitter.particleModelFilename);
    visit(emitter.childEmittersModelFilename);

    if (version != 0 && version < M2_VERSION_BC) {
        writer.write<u16>(static_cast<u16>(emitter.blendingType));
        writer.write<u16>(static_cast<u16>(emitter.emitterType));
    } else {
        writer.write(emitter.blendingType);
        writer.write(emitter.emitterType);
        writer.write(emitter.particleColorIndex);
    }

    if (version != 0 && version < M2_VERSION_CATA) {
        writer.write(emitter.particleType);
        writer.write(emitter.headOrTail);
    } else {
        writer.write(emitter.multiTexScale[0]);
        writer.write(emitter.multiTexScale[1]);
    }
    writer.write(emitter.textureTilerotation);
    writer.write(emitter.rows);
    writer.write(emitter.columns);

    visit(emitter.emissionSpeed);
    visit(emitter.speedVariation);
    visit(emitter.verticalRange);
    visit(emitter.horizontalRange);
    visit(emitter.gravity);
    visit(emitter.lifespan);

    if (!legacy) {
        writer.write(emitter.lifespanVariation);
    }

    visit(emitter.emissionRate);

    if (!legacy) {
        writer.write(emitter.emissionRateVariation);
    }

    visit(emitter.emissionAreaWidth);
    visit(emitter.emissionAreaLength);
    visit(emitter.zSource);

    if (legacy) {
        writeLegacyParticleColorBlock(emitter);
    } else {
        visit(emitter.colorTrack);
        visit(emitter.alphaTrack);
        visit(emitter.scaleTrack);
        writer.write(emitter.scaleVary);
        visit(emitter.headUVScroll);
        visit(emitter.tailUVScroll);
    }

    writer.write(emitter.tailLength);
    writer.write(emitter.twinkleSpeed);
    writer.write(emitter.twinklePercent);
    writer.write(emitter.twinkleScale.x);
    writer.write(emitter.twinkleScale.y);
    writer.write(emitter.inheritVelocityScale);
    writer.write(emitter.drag);
    if (legacy) {
        writer.write(emitter.spinSpeed);
    } else {
        writer.write(emitter.baseSpin);
        writer.write(emitter.baseSpinVariation);
        writer.write(emitter.spinSpeed);
        writer.write(emitter.spinSpeedVariation);
    }

    writer.write(emitter.tumble.minimum);
    writer.write(emitter.tumble.maximum);
    writer.write(emitter.windVector);
    writer.write(emitter.windTime);

    writer.write(emitter.followSpeed1);
    writer.write(emitter.followScale1);
    writer.write(emitter.followSpeed2);
    writer.write(emitter.followScale2);

    visit(emitter.splinePoints);
    visit(emitter.enabledIn);

    const bool extendedParticle =
        (version > 271) || hasFlag(globalFlags, GlobalFlag::NewParticleRecord);
    if (extendedParticle) {
        writer.write(emitter.multiTexScrollMid[0][0]);
        writer.write(emitter.multiTexScrollMid[0][1]);
        writer.write(emitter.multiTexScrollMid[1][0]);
        writer.write(emitter.multiTexScrollMid[1][1]);
        writer.write(emitter.multiTexScrollRange[0][0]);
        writer.write(emitter.multiTexScrollRange[0][1]);
        writer.write(emitter.multiTexScrollRange[1][0]);
        writer.write(emitter.multiTexScrollRange[1][1]);
    }
}

void BinaryWriterVisitor::writeLegacyParticleColorBlock(const ParticleEmitter& emitter) {
    // Inverse of the parser's three-key reconstruction: sample the tracks at
    // begin / mid / end of the particle lifetime.
    const auto rawAt = [](const std::vector<unorm16>& v, size_t i, u16 fallback) -> u16 {
        if (v.empty())
            return fallback;
        return v[std::min(i, v.size() - 1)].value;
    };

    f32 mid = 0.5f;
    if (emitter.colorTrack.timestamps.size() == 3) {
        mid = static_cast<f32>(emitter.colorTrack.timestamps[1].value) / 32767.0f;
    }
    writer.write(mid);

    for (size_t i = 0; i < 3; ++i) {
        Vector3f color{255.0f, 255.0f, 255.0f};
        if (!emitter.colorTrack.values.empty()) {
            color = emitter.colorTrack.values[std::min(i, emitter.colorTrack.values.size() - 1)];
        }
        u16 const alpha = rawAt(emitter.alphaTrack.values, i, 32767);
        ColorBGRA c;
        c.b = static_cast<u8>(std::clamp(color.z, 0.0f, 255.0f));
        c.g = static_cast<u8>(std::clamp(color.y, 0.0f, 255.0f));
        c.r = static_cast<u8>(std::clamp(color.x, 0.0f, 255.0f));
        c.a = static_cast<u8>((static_cast<u32>(alpha) * 255 + 16383) / 32767);
        writer.write(c);
    }

    for (size_t i = 0; i < 4; ++i) {
        f32 scale = 1.0f;
        if (!emitter.scaleTrack.values.empty()) {
            scale = emitter.scaleTrack.values[std::min(i, emitter.scaleTrack.values.size() - 1)].x;
        }
        writer.write(scale);
    }

    writer.write<u16>(rawAt(emitter.headUVScroll.values, 0, 0));
    writer.write<u16>(rawAt(emitter.headUVScroll.values, 0, 0));
    writer.write<u16>(1);
    writer.write<u16>(rawAt(emitter.headUVScroll.values, 1, 0));
    writer.write<u16>(rawAt(emitter.headUVScroll.values, 1, 0));
    writer.write<u16>(1);
    writer.write<i16>(static_cast<i16>(rawAt(emitter.tailUVScroll.values, 0, 0)));
    writer.write<i16>(static_cast<i16>(rawAt(emitter.tailUVScroll.values, 1, 0)));
    writer.write<i16>(0);
    writer.write<i16>(0);
}

void BinaryWriterVisitor::visit(const Event& event) {
    writer.write(event.identifier);
    writer.write(event.data);
    writer.write(event.boneId);
    writer.write(event.position.x);
    writer.write(event.position.y);
    writer.write(event.position.z);
    visit(event.enabled);
}

void BinaryWriterVisitor::visit(const MD20Header& md20) {
    version = md20.version;
    [[maybe_unused]] const auto& header = md20.model;

    writer.write(md20.magic);
    writer.write(md20.version);

    visit(md20.model);
}

void BinaryWriterVisitor::visit(const Model& model) {
    const bool legacy = version != 0 && version < M2_VERSION_WOTLK;
    if (legacy) {
        legacyWindows = buildLegacyTimeline(model.sequences);
        legacyWindowIndex = 0;
    }

    visit(model.modelName);
    visit(model.globalFlags);
    globalFlags = model.globalFlags.value;

    visit(model.globalLoops);
    visit(model.sequences);
    visit(model.sequenceIdxHashById);

    if (legacy) {
        visit(model.playableAnimationLookup);
    }

    visit(model.bones);
    visit(model.keyBoneIds);

    visit(model.vertices);

    if (version >= M2_VERSION_WOTLK) {
        writer.write(model.numSkinProfiles);
    } else {
        visit(model.skinProfiles);
    }

    visit(model.colors);
    visit(model.textures);
    visit(model.textureWeights);
    if (legacy) {
        visit(model.textureFlipbooks);
    }
    visit(model.textureTransforms);
    visit(model.textureIndicesById);
    visit(model.materials);
    visit(model.boneCombos);
    visit(model.textureCombos);
    visit(model.textureCoordCombos);
    visit(model.textureWeightCombos);
    visit(model.textureTransformCombos);

    writer.write(model.bounding.minimum);
    writer.write(model.bounding.maximum);
    writer.write(model.bounding.sphereRadius);

    writer.write(model.collision.minimum);
    writer.write(model.collision.maximum);
    writer.write(model.collision.sphereRadius);

    visit(model.collisionTriangleIndices);
    visit(model.collisionVertices);
    visit(model.collisionFaceNormals);

    visit(model.attachments);
    visit(model.attachmentIndicesById);
    visit(model.events);
    visit(model.lights);
    visit(model.cameras);
    visit(model.cameraIndicesById);
    visit(model.ribbonEmitters);
    visit(model.particleEmitters);
    if (hasFlag(model.globalFlags.value, GlobalFlag::UseTextureCombinerCombos)) {
        visit(model.textureCombinerCombos);
    }
}

void BinaryWriterVisitor::visit(const AnimationTrackBase& track) {
    if (version != 0 && version < M2_VERSION_WOTLK) {
        writeLegacyTrackHead(track);
        return;
    }
    writer.write(track.interpolationType);
    writer.write(track.globalSequenceId);
    writeSequenceArrays(track.timestamps, track.globalSequenceId != 0xFFFF);
}

void BinaryWriterVisitor::writeLegacyTrackHead(const AnimationTrackBase& track) {
    writer.write(track.interpolationType);
    writer.write(track.globalSequenceId);

    auto& ranges = arenaVector<LegacyRange>();
    auto& times = arenaVector<u32>();

    if (track.globalSequenceId != 0xFFFF || legacyWindows.empty()) {
        if (!track.timestamps.empty()) {
            times = track.timestamps[0];
        }
    } else {
        bool any = false;
        for (const auto& sub : track.timestamps) {
            if (!sub.empty()) {
                any = true;
                break;
            }
        }
        if (any) {
            // Events fire at their timestamps; a sequence without any emits
            // none, and its range collapses instead of getting padded keys.
            for (size_t s = 0; s < legacyWindows.size(); ++s) {
                const auto& w = legacyWindows[s];
                u32 const firstIdx = static_cast<u32>(times.size());
                if (s < track.timestamps.size()) {
                    for (u32 const t : track.timestamps[s]) {
                        times.push_back(w.start + t);
                    }
                }
                ranges.push_back(LegacyRange{firstIdx, static_cast<u32>(times.size())});
            }
        }
    }

    writeVector(ranges, &writer);
    writeVector(times, &writer);
}

void BinaryWriterVisitor::visit(const std::string& str) {
    writer.write<u32>(static_cast<u32>(str.size()));
    u32 const offsetPos = writer.getPosition();
    writer.write<u32>(0);
    if (str.empty()) {
        return;
    }

    deferredWrites.push_back([this, offsetPos, &str]() {
        u32 const currentPos = writer.getPosition();
        writer.setPosition(offsetPos);
        writer.write(currentPos - baseOffset);
        writer.setPosition(currentPos);
        writer.writeString(str);
        writer.AlignTo(16);
    });
}

} // namespace m2
} // namespace whiteout
