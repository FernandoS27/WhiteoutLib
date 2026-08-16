
#include <array>

#include "../../../common/binary_reader.h"
#include "../binary_parse_visitor.h"
#include "../chunk_parser.h"
#include "../wow_file_system.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

BinaryParseVisitor::BinaryParseVisitor(common::BinaryReader& reader, WoWFileSystem* wfs,
                                       i32 maxSize_)
    : reader(reader), wfs(wfs), maxSize(maxSize_) {}

void BinaryParseVisitor::start() {
    baseOffset = reader.getPosition();
}

void BinaryParseVisitor::visit(GlobalFlags& flags) {
    flags.value = static_cast<GlobalFlag>(reader.read<u32>());
}

void BinaryParseVisitor::visit(GlobalSequence& seq) {
    seq.timestamp = reader.read<u32>();
}

void BinaryParseVisitor::visit(Sequence& seq) {
    const bool legacy = version != 0 && version < M2_VERSION_WOTLK;

    seq.id = reader.read<u16>();
    seq.variationIndex = reader.read<u16>();
    if (legacy) {
        // ≤TBC: a [start, end] window on the shared global timeline instead of
        // a duration. The window is what the legacy track split slices by.
        u32 const start = reader.read<u32>();
        u32 const end = reader.read<u32>();
        seq.duration = end >= start ? end - start : 0;
        legacyWindows.push_back(LegacyWindow{start, end});
    } else {
        seq.duration = reader.read<u32>();
    }
    seq.movespeed = reader.read<f32>();
    seq.flags = reader.read<SequenceFlag>();

    // One entry per sequence, in order — readAnimationVector indexes it by
    // sequence, so this has to push exactly once. 0x130 is
    // CM2Model::LoadSequence's own test for "this sequence's keys are in the
    // `.m2`"; anything else is streamed from a `.anim` sibling — a concept
    // WotLK introduced, so a ≤TBC model carries everything itself no matter
    // what its flags claim.
    const bool inFile = legacy || (static_cast<u32>(seq.flags) & 0x130u) != 0;
    wfs->sequenceInFile().push_back(inFile ? u8{1} : u8{0});
    seq.frequency = reader.read<i16>();
    seq.padding = reader.read<u16>();
    seq.replayMin = reader.read<u32>();
    seq.replayMax = reader.read<u32>();
    if (version != 0 && version < M2_VERSION_MOP) {
        // One u32 blend time before the WoD-era in/out split; the split gave
        // both sides the old value.
        u32 const blendTime = reader.read<u32>();
        u16 const clamped = static_cast<u16>(std::min<u32>(blendTime, 0xFFFFu));
        seq.blendTimeIn = clamped;
        seq.blendTimeOut = clamped;
    } else {
        seq.blendTimeIn = reader.read<u16>();
        seq.blendTimeOut = reader.read<u16>();
    }

    seq.bounding.minimum = reader.read<Vector3f>();
    seq.bounding.maximum = reader.read<Vector3f>();
    seq.bounding.sphereRadius = reader.read<f32>();

    seq.variationNext = reader.read<i16>();
    seq.aliasNext = reader.read<u16>();
}

void BinaryParseVisitor::visit(Vertex& vertex) {
    vertex.position = reader.read<Vector3f>();
    vertex.boneWeights = reader.readArray<u8, 4>();
    vertex.boneIndices = reader.readArray<u8, 4>();
    vertex.normal = reader.read<Vector3f>();
    vertex.texCoords = reader.readArray<Vector2f, 2>();
}

void BinaryParseVisitor::visit(Bone& bone) {
    bone.keyBoneId = reader.read<i32>();
    bone.flags = reader.read<u32>();
    bone.parentBoneId = reader.read<i16>();
    bone.submeshId = reader.read<u16>();
    bone.boneNameCRC = reader.read<u32>();

    visit(bone.translation);
    visitLegacyBoneRotation(bone.rotation);
    visit(bone.scale);

    bone.pivot = reader.read<Vector3f>();
}

void BinaryParseVisitor::visitLegacyBoneRotation(AnimationTrack<CompatQuaternion>& track) {
    // Bone rotations were float quaternions until BC introduced M2CompQuat;
    // compress vanilla keys with the client's own mapping so they land on the
    // same raw values a BC-era re-export would have stored.
    if (version != 0 && version < M2_VERSION_BC) {
        readLegacyTrack<Vector4f>(track, [](const Vector4f& q) { return compressQuat(q); });
        return;
    }
    visit(track);
}

void BinaryParseVisitor::visit(Texture& texture) {
    texture.type = reader.read<u32>();
    texture.flags = reader.read<u32>();
    visit(texture.filename);
}

void BinaryParseVisitor::visit(Material& material) {
    material.flags = reader.read<u16>();
    material.blendingMode = reader.read<u16>();
}

void BinaryParseVisitor::visit(TextureWeight& weight) {
    visit(weight.weight);
}

void BinaryParseVisitor::visit(TextureTransform& transform) {
    visit(transform.translation);
    visit(transform.rotation);
    visit(transform.scaling);
}

void BinaryParseVisitor::visit(ColorAnimation& color) {
    visit(color.color);
    visit(color.alpha);
}

void BinaryParseVisitor::visit(Light& light) {
    light.type = reader.read<u16>();
    light.boneId = reader.read<i16>();
    light.position = reader.read<Vector3f>();

    visit(light.ambientColor);
    visit(light.ambientIntensity);
    visit(light.diffuseColor);
    visit(light.diffuseIntensity);
    visit(light.attenuationStart);
    visit(light.attenuationEnd);
    visit(light.visibility);
}

void BinaryParseVisitor::visit(CameraSpline& spline) {
    spline.value = reader.read<Vector3f>();
    spline.inTangent = reader.read<Vector3f>();
    spline.outTangent = reader.read<Vector3f>();
}

void BinaryParseVisitor::visit(Camera& camera) {
    camera.type = reader.read<u32>();
    if (version < M2_VERSION_CATA) {
        camera.fieldOfView = reader.read<f32>();
    }
    camera.farClip = reader.read<f32>();
    camera.nearClip = reader.read<f32>();

    visit(camera.positions);

    camera.positionBase = reader.read<Vector3f>();

    visit(camera.targetPositions);

    camera.targetPositionBase = reader.read<Vector3f>();

    visit(camera.roll);
    if (version >= M2_VERSION_CATA) {
        visit(camera.fieldOfViewTrack);
    }
}

void BinaryParseVisitor::visit(Attachment& attachment) {
    attachment.id = reader.read<u32>();
    attachment.boneId = reader.read<u16>();
    attachment.unknown = reader.read<u16>();
    attachment.position = reader.read<Vector3f>();
    visit(attachment.animate);
}

void BinaryParseVisitor::visit(RibbonEmitter& emitter) {
    emitter.ribbonId = reader.read<u32>();
    emitter.boneId = reader.read<u32>();
    emitter.position = reader.read<Vector3f>();

    visit(emitter.textureIndices);
    visit(emitter.materialIndices);

    visit(emitter.colorTrack);
    visit(emitter.alphaTrack);
    visit(emitter.heightAbove);
    visit(emitter.heightBelow);

    emitter.edgesPerSecond = reader.read<f32>();
    emitter.edgeLifetime = reader.read<f32>();
    emitter.gravity = reader.read<f32>();
    emitter.textureRows = reader.read<u16>();
    emitter.textureCols = reader.read<u16>();

    visit(emitter.texSlot);
    visit(emitter.visibility);

    if (version == 0 || version >= M2_VERSION_WOTLK) {
        emitter.priorityPlane = reader.read<i16>();
        emitter.ribbonColorIndex = reader.read<i8>();
        emitter.textureTransformIndex = reader.read<i8>();
    } else {
        emitter.priorityPlane = 0;
        emitter.ribbonColorIndex = -1;
        emitter.textureTransformIndex = -1;
    }
}

void BinaryParseVisitor::visit(ParticleEmitter& emitter) {
    const bool legacy = version != 0 && version < M2_VERSION_WOTLK;

    emitter.particleId = reader.read<u32>();
    emitter.flags = reader.read<ParticleFlag>();
    emitter.position = reader.read<Vector3f>();
    emitter.boneId = reader.read<u16>();
    emitter.textureId = reader.read<u16>();

    visit(emitter.particleModelFilename);
    visit(emitter.childEmittersModelFilename);

    if (version != 0 && version < M2_VERSION_BC) {
        // Vanilla: both as u16, no ParticleColor.dbc index yet.
        emitter.blendingType = static_cast<ParticleBlending>(reader.read<u16>());
        emitter.emitterType = static_cast<ParticleEmitterType>(reader.read<u16>());
        emitter.particleColorIndex = 0;
    } else {
        emitter.blendingType = static_cast<ParticleBlending>(reader.read<u8>());
        emitter.emitterType = static_cast<ParticleEmitterType>(reader.read<u8>());
        emitter.particleColorIndex = reader.read<u16>();
    }

    if (version != 0 && version < M2_VERSION_CATA) {
        emitter.particleType = reader.read<u8>();
        emitter.headOrTail = reader.read<u8>();
        emitter.multiTexScale = {};
    } else {
        emitter.multiTexScale[0] = reader.read<fixed8_5>();
        emitter.multiTexScale[1] = reader.read<fixed8_5>();
    }
    emitter.textureTilerotation = reader.read<i16>();
    emitter.rows = reader.read<u16>();
    emitter.columns = reader.read<u16>();

    visit(emitter.emissionSpeed);
    visit(emitter.speedVariation);
    visit(emitter.verticalRange);
    visit(emitter.horizontalRange);
    visit(emitter.gravity);
    visit(emitter.lifespan);

    if (!legacy) {
        emitter.lifespanVariation = reader.read<f32>();
    }

    visit(emitter.emissionRate);

    if (!legacy) {
        emitter.emissionRateVariation = reader.read<f32>();
    }

    visit(emitter.emissionAreaWidth);
    visit(emitter.emissionAreaLength);
    visit(emitter.zSource);

    if (legacy) {
        readLegacyParticleColorBlock(emitter);
    } else {
        visit(emitter.colorTrack);
        visit(emitter.alphaTrack);
        visit(emitter.scaleTrack);
        emitter.scaleVary = reader.read<Vector2f>();
        visit(emitter.headUVScroll);
        visit(emitter.tailUVScroll);
    }

    emitter.tailLength = reader.read<f32>();
    emitter.twinkleSpeed = reader.read<f32>();
    emitter.twinklePercent = reader.read<f32>();
    emitter.twinkleScale.x = reader.read<f32>();
    emitter.twinkleScale.y = reader.read<f32>();
    emitter.inheritVelocityScale = reader.read<f32>();
    emitter.drag = reader.read<f32>();
    if (legacy) {
        // One rotation value pre-WotLK; the closest modern meaning is the
        // per-second spin speed.
        emitter.baseSpin = 0.0f;
        emitter.baseSpinVariation = 0.0f;
        emitter.spinSpeed = reader.read<f32>();
        emitter.spinSpeedVariation = 0.0f;
    } else {
        emitter.baseSpin = reader.read<f32>();
        emitter.baseSpinVariation = reader.read<f32>();
        emitter.spinSpeed = reader.read<f32>();
        emitter.spinSpeedVariation = reader.read<f32>();
    }

    emitter.tumble.minimum = reader.read<Vector3f>();
    emitter.tumble.maximum = reader.read<Vector3f>();
    emitter.windVector = reader.read<Vector3f>();
    emitter.windTime = reader.read<f32>();

    emitter.followSpeed1 = reader.read<f32>();
    emitter.followScale1 = reader.read<f32>();
    emitter.followSpeed2 = reader.read<f32>();
    emitter.followScale2 = reader.read<f32>();

    visit(emitter.splinePoints);
    visit(emitter.enabledIn);

    const bool extendedParticle =
        (version > 271) || hasFlag(globalFlags, GlobalFlag::NewParticleRecord);
    if (extendedParticle) {
        emitter.multiTexScrollMid[0][0] = reader.read<fixed16_9>();
        emitter.multiTexScrollMid[0][1] = reader.read<fixed16_9>();
        emitter.multiTexScrollMid[1][0] = reader.read<fixed16_9>();
        emitter.multiTexScrollMid[1][1] = reader.read<fixed16_9>();
        emitter.multiTexScrollRange[0][0] = reader.read<fixed16_9>();
        emitter.multiTexScrollRange[0][1] = reader.read<fixed16_9>();
        emitter.multiTexScrollRange[1][0] = reader.read<fixed16_9>();
        emitter.multiTexScrollRange[1][1] = reader.read<fixed16_9>();
    }
}

void BinaryParseVisitor::readLegacyParticleColorBlock(ParticleEmitter& emitter) {
    // Pre-WotLK particles animate color/alpha/scale over three fixed points of
    // the particle lifetime: begin, a mid point, end. Rebuilt here as the
    // three-key tracks WotLK stores.
    f32 mid = reader.read<f32>();
    if (mid < 0.0f)
        mid = 0.0f;
    if (mid > 1.0f)
        mid = 1.0f;
    std::array<ColorBGRA, 3> colors;
    for (auto& c : colors)
        c = reader.read<ColorBGRA>();
    std::array<f32, 4> scales{};
    for (auto& s : scales)
        s = reader.read<f32>();
    std::array<u16, 2> headBegin{};
    for (auto& v : headBegin)
        v = reader.read<u16>();
    (void)reader.read<u16>();
    std::array<u16, 2> headEnd{};
    for (auto& v : headEnd)
        v = reader.read<u16>();
    (void)reader.read<u16>();
    std::array<i16, 4> tiles{};
    for (auto& v : tiles)
        v = reader.read<i16>();

    const std::array<unorm16, 3> lifeTimes = {unorm16::from_raw(0),
                                              unorm16::from_raw(static_cast<u16>(mid * 32767.0f)),
                                              unorm16::from_raw(32767)};

    emitter.colorTrack.timestamps.assign(lifeTimes.begin(), lifeTimes.end());
    emitter.colorTrack.values.clear();
    emitter.alphaTrack.timestamps.assign(lifeTimes.begin(), lifeTimes.end());
    emitter.alphaTrack.values.clear();
    emitter.scaleTrack.timestamps.assign(lifeTimes.begin(), lifeTimes.end());
    emitter.scaleTrack.values.clear();
    for (int i = 0; i < 3; ++i) {
        // WotLK color keys keep byte-range channels as floats; alpha becomes
        // the 15-bit fixed the FBlock stores.
        emitter.colorTrack.values.push_back(Vector3f{static_cast<f32>(colors[i].r),
                                                     static_cast<f32>(colors[i].g),
                                                     static_cast<f32>(colors[i].b)});
        emitter.alphaTrack.values.push_back(
            unorm16::from_raw(static_cast<u16>((colors[i].a * 32767 + 127) / 255)));
        emitter.scaleTrack.values.push_back(Vector2f{scales[i], scales[i]});
    }
    emitter.scaleVary = Vector2f(0.0f, 0.0f);

    // Head/tail flipbook cells: two fixed cells each becomes a two-key track.
    emitter.headUVScroll.timestamps = {unorm16::from_raw(0), unorm16::from_raw(32767)};
    emitter.headUVScroll.values = {unorm16::from_raw(headBegin[0]), unorm16::from_raw(headEnd[0])};
    emitter.tailUVScroll.timestamps = {unorm16::from_raw(0), unorm16::from_raw(32767)};
    emitter.tailUVScroll.values = {unorm16::from_raw(static_cast<u16>(tiles[0])),
                                   unorm16::from_raw(static_cast<u16>(tiles[1]))};
}

void BinaryParseVisitor::visit(Event& event) {
    event.identifier = reader.read<u32>();
    event.data = reader.read<u32>();
    event.boneId = reader.read<u32>();
    event.position = reader.read<Vector3f>();
    visit(event.enabled);
}

void BinaryParseVisitor::visit(MD20Header& header) {
    header.magic = reader.read<u32>();
    header.version = reader.read<u32>();
    version = header.version;

    visit(header.model);
}

void BinaryParseVisitor::visit(Model& header) {
    const bool legacy = version != 0 && version < M2_VERSION_WOTLK;
    legacyWindows.clear();

    visit(header.modelName);
    visit(header.globalFlags);
    globalFlags = header.globalFlags.value;

    visit(header.globalLoops);
    visit(header.sequences);
    visit(header.sequenceIdxHashById);

    if (legacy) {
        visit(header.playableAnimationLookup);
    }

    visit(header.bones);
    visit(header.keyBoneIds);

    visit(header.vertices);

    if (version >= M2_VERSION_WOTLK) {
        header.numSkinProfiles = reader.read<u32>();
        header.skinProfiles.reserve(header.numSkinProfiles);
        header.lodProfiles.reserve(header.numSkinProfiles);
    } else {
        visit(header.skinProfiles);
    }

    visit(header.colors);
    visit(header.textures);
    visit(header.textureWeights);
    if (legacy) {
        visit(header.textureFlipbooks);
    }
    visit(header.textureTransforms);
    visit(header.textureIndicesById);
    visit(header.materials);
    visit(header.boneCombos);
    visit(header.textureCombos);
    visit(header.textureCoordCombos);
    visit(header.textureWeightCombos);
    visit(header.textureTransformCombos);
    header.bounding.minimum = reader.read<Vector3f>();
    header.bounding.maximum = reader.read<Vector3f>();
    header.bounding.sphereRadius = reader.read<f32>();

    header.collision.minimum = reader.read<Vector3f>();
    header.collision.maximum = reader.read<Vector3f>();
    header.collision.sphereRadius = reader.read<f32>();

    visit(header.collisionTriangleIndices);
    visit(header.collisionVertices);
    visit(header.collisionFaceNormals);

    visit(header.attachments);
    visit(header.attachmentIndicesById);
    visit(header.events);
    visit(header.lights);
    visit(header.cameras);
    visit(header.cameraIndicesById);
    visit(header.ribbonEmitters);
    visit(header.particleEmitters);

    if (hasFlag(header.globalFlags.value, GlobalFlag::UseTextureCombinerCombos)) {
        visit(header.textureCombinerCombos);
    }
}

void BinaryParseVisitor::visit(std::string& str) {
    const auto count = reader.read<u32>();
    const auto offset = reader.read<u32>();
    if (count == 0) {
        str.clear();
        return;
    }
    const auto currentPos = reader.getPosition();
    reader.setPosition(offset + baseOffset);
    str = reader.readString(count);
    reader.setPosition(currentPos);
}

void BinaryParseVisitor::visit(AnimationTrackBase& track) {
    if (version != 0 && version < M2_VERSION_WOTLK) {
        readLegacyTrackHead(track);
        return;
    }
    track.interpolationType = static_cast<InterpolationType>(reader.read<u16>());
    track.globalSequenceId = reader.read<u16>();
    readAnimationVector(track.timestamps, track.timestampRefs, track.globalSequenceId);
}

void BinaryParseVisitor::readLegacyTrackHead(AnimationTrackBase& track) {
    // The value-less track an Event carries: same ≤TBC layout as a full
    // track, minus the values array.
    track.interpolationType = static_cast<InterpolationType>(reader.read<u16>());
    track.globalSequenceId = reader.read<u16>();
    (void)reader.read<KeySpanRef>();
    const std::vector<u32> times = readFlatSpan<u32>();

    track.timestamps.clear();
    track.timestampRefs.clear();
    if (times.empty()) {
        return;
    }

    if (track.globalSequenceId != 0xFFFF || legacyWindows.empty()) {
        track.timestamps.push_back(times);
        return;
    }

    track.timestamps.resize(legacyWindows.size());
    for (size_t s = 0; s < legacyWindows.size(); ++s) {
        const auto& w = legacyWindows[s];
        const auto first = std::lower_bound(times.begin(), times.end(), w.start);
        const auto last = std::upper_bound(first, times.end(), w.end);
        auto& ts = track.timestamps[s];
        ts.reserve(static_cast<size_t>(last - first));
        for (auto it = first; it != last; ++it) {
            ts.push_back(*it - w.start);
        }
    }
}

} // namespace m2
} // namespace whiteout
