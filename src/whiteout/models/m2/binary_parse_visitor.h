
#pragma once

#include <whiteout/models/m2/types.h>
#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "internal_structures.h"
#include "legacy.h"
#include "traits.h"
// Full type, not the forward declaration below: readAnimationVector is a
// template in this header and reads the shared per-sequence key-location flags.
#include "wow_file_system.h"

#include <algorithm>
#include <type_traits>

namespace whiteout {
namespace m2 {

class WoWFileSystem;

class BinaryParseVisitor {
public:
    explicit BinaryParseVisitor(common::BinaryReader& reader, WoWFileSystem* wfs = nullptr,
                                i32 maxSize_ = -1);

    /// Extra arguments are forwarded to the matching visit() overload — the
    /// owning BaseFile for chunks that size themselves from the model header,
    /// or a record layout for the `.phys` chunks that were respelled between
    /// file versions.
    template <typename T, typename... Args>
    void read(T& header, Args&&... args) {
        start();
        visit(header, std::forward<Args>(args)...);
    }

    void setVersion(u32 v) {
        version = v;
    }

    u32 getVersion() const {
        return version;
    }

protected:
    void start();

    void visit(GlobalFlags& flags);
    void visit(GlobalSequence& seq);
    void visit(Sequence& seq);
    void visit(Vertex& vertex);
    void visit(Bone& bone);
    void visit(Texture& texture);
    void visit(Material& material);
    void visit(TextureWeight& weight);
    void visit(TextureTransform& transform);
    void visit(ColorAnimation& color);
    void visit(Light& light);
    void visit(CameraSpline& spline);
    void visit(Camera& camera);
    void visit(Attachment& attachment);
    void visit(RibbonEmitter& emitter);
    void visit(ParticleEmitter& emitter);
    void visit(Event& event);
    void visit(Model& header);
    void visit(MD20Header& header);

    void visit(LodProfile& chunk);
    void visit(ParticleEmitterExtension& chunk);
    void visit(TXACChunk& chunk, BaseFile& file);

    void visit(PFIDChunk& chunk, BaseFile& file);
    void visit(SFIDChunk& chunk, BaseFile& file);
    void visit(AFIDEntry& entry);
    void visit(AFIDChunk& chunk);
    void visit(BFIDChunk& chunk);

    void visit(EXPTEntry& entry);
    void visit(EXPTChunk& chunk);
    void visit(EXP2Chunk& chunk);
    void visit(PABCChunk& chunk);
    void visit(PADCChunk& chunk);
    void visit(PSBCChunk& chunk);
    void visit(PEDCChunk& chunk);
    void visit(SKIDChunk& chunk);
    void visit(TXIDChunk& chunk);

    void visit(M2RPIDEntry& entry);
    void visit(M2RPIDChunk& chunk);
    void visit(GPIDEntry& entry);
    void visit(GPIDChunk& chunk);
    void visit(ParticleGeosetData& entry);
    void visit(PGD1Chunk& chunk);
    void visit(WaterfallData& data);
    void visit(WFV3Chunk& chunk);
    void visit(EdgeFadeData& entry);
    void visit(EDGFChunk& chunk);
    void visit(DistanceFadeData& entry);
    void visit(NERFChunk& chunk);
    void visit(DetailedLightData& entry);
    void visit(DETLChunk& chunk);
    void visit(DebugOcclusionData& entry);
    void visit(DBOCChunk& chunk);
    void visit(AFRAChunk& chunk);
    void visit(PCOLChunk& chunk);
    void visit(DPIVChunk& chunk);
    void visit(TexturedLightData& entry);
    void visit(TEXLChunk& chunk);

    void visit(PhysicsFrame& frame);
    void visit(PhysicsBody& body, PhysBodyLayout layout);
    void visit(PhysicsShape& shape, PhysShapeLayout layout);
    void visit(BoxShape& shape);
    void visit(CapsuleShape& shape);
    void visit(SphereShape& shape);
    void visit(std::vector<PolytopeShape>& shapes);
    void visit(PhysicsJoint& joint);
    void visit(WeldJoint& joint, PhysWeldLayout layout);
    void visit(SphericalJoint& joint);
    void visit(ShoulderJoint& joint, PhysShoulderLayout layout);
    void visit(PrismaticJoint& joint, PhysMotorLayout layout);
    void visit(RevoluteJoint& joint, PhysMotorLayout layout);
    void visit(DistanceJoint& joint);
    void visit(PhysicsTuning& tuning);

    void visit(AFM2Chunk& chunk);
    void visit(AFSAChunk& chunk);
    void visit(AFSBChunk& chunk);
    void visit(AnimFile& file);

    void visit(SKL1Chunk& chunk);
    void visit(SKA1Chunk& chunk);
    void visit(SKB1Chunk& chunk);
    void visit(SKS1Chunk& chunk);
    void visit(SKPDChunk& chunk);

    void visit(SkinSection& section);
    void visit(Batch& batch);
    void visit(ShadowBatch& batch);
    void visit(SkinProfile& profile);
    void visit(SkinFile& file);

    template <typename T>
    void visit(std::vector<T>& array);

    template <typename T>
    void parse_chunked_vector(std::vector<T>& array);

    void visit(std::string& str);

    template <typename T>
    void visit(AnimationTrack<T>& track);
    void visit(AnimationTrackBase& track);

    template <typename T>
    void visit(ParticleAnimationTrack<T>& track);

    // An animation key array — one sub-array per sequence — never goes through
    // the generic `visit(std::vector<T>&)`, because a sequence whose keys live
    // in a `.anim` sibling has to be read out of that file's buffer, and
    // because a lazy parse leaves those sub-arrays for loadSequence() to fill.
    // Only two members are key arrays, and both call this by name:
    // AnimationTrackBase::timestamps and AnimationTrack<T>::values.
    template <typename T>
    void readAnimationVector(std::vector<std::vector<T>>& keys, std::vector<KeySpanRef>& refs,
                             u16 globalSequenceId);

    // ≤TBC (<264) track reading: one flat key array on a global timeline plus
    // per-sequence interpolation ranges, split here into the per-sequence
    // sub-arrays every later version stores directly.
    template <typename T>
    std::vector<T> readFlatSpan();
    void readLegacyTrackHead(AnimationTrackBase& track);
    template <typename FileT, typename T, typename Convert>
    void readLegacyTrack(AnimationTrack<T>& track, Convert convert);
    void visitLegacyBoneRotation(AnimationTrack<CompatQuaternion>& track);
    void readLegacyParticleColorBlock(ParticleEmitter& emitter);

    u32 version = 0;
    GlobalFlag globalFlags = GlobalFlag::None;
    common::BinaryReader& reader;
    WoWFileSystem* wfs = nullptr;
    u32 baseOffset = 0;
    i32 maxSize = -1;

    /// Per sequence, in order: its [start, end] window on the ≤TBC global
    /// timeline. Filled by visit(Sequence&), consumed by the legacy track
    /// splitting. Empty for ≥264 models.
    std::vector<LegacyWindow> legacyWindows;
};

template <typename T>
void BinaryParseVisitor::visit(std::vector<T>& array) {
    const auto count = reader.read<u32>();
    const auto offset = reader.read<u32>();
    if (count == 0) {
        array.clear();
        return;
    }

    const auto currentPos = reader.getPosition();
    reader.setPosition(offset + baseOffset);

    if constexpr (bulk_copyable_v<T>) {
        array = reader.read<std::vector<T>>(count);
    } else {
        array.clear();
        array.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            T element;
            this->visit(element);
            array.push_back(std::move(element));
        }
    }

    reader.setPosition(currentPos);
}

template <typename T>
void BinaryParseVisitor::parse_chunked_vector(std::vector<T>& array) {
    if (maxSize <= 0) {
        array.clear();
        return;
    }
    const size_t totalSize = maxSize / sizeof(T);
    array = reader.read<std::vector<T>>(totalSize);
}

template <typename T>
void BinaryParseVisitor::readAnimationVector(std::vector<std::vector<T>>& keys,
                                             std::vector<KeySpanRef>& refs, u16 globalSequenceId) {
    KeySpanRef outer = reader.read<KeySpanRef>();
    if (outer.count == 0) {
        keys.clear();
        refs.clear();
        return;
    }

    // A global-sequence track is not split per sequence at all — it runs on its
    // own clock — and the client resolves every one of its sub-arrays against
    // the model itself (M2Init<T>(M2Track<T>&) takes the M2SequenceKeys branch
    // whole when globalSequenceId != 0xFFFF). Reading one out of some
    // sequence's `.anim` because the index happened to line up is how a
    // continuously scrolling texture turns to noise.
    const bool global = globalSequenceId != 0xFFFF;

    const auto currentPos = reader.getPosition();
    reader.setPosition(outer.offset + baseOffset);
    keys.resize(outer.count);
    refs.clear();
    for (size_t i = 0; i < outer.count; ++i) {
        const auto ref = reader.read<KeySpanRef>();
        const bool inFile =
            global || !wfs || i >= wfs->sequenceInFile().size() || wfs->sequenceInFile()[i] != 0;

        // Keys the model carries itself, which is everything the parse can
        // read: `ref` for any other sequence is an offset into a `.anim` file,
        // and following it into the model here reads whatever happens to sit at
        // that address — NaNs, in practice. m2::loadSequence fills those in,
        // from the file they actually belong to.
        if (inFile) {
            const auto savePos = reader.getPosition();
            reader.setPosition(ref.offset + baseOffset);
            keys[i] = reader.read<std::vector<T>>(ref.count);
            reader.setPosition(savePos);
            continue;
        }
        if (refs.empty())
            refs.resize(outer.count);
        refs[i] = ref;
        keys[i].clear();
    }
    reader.setPosition(currentPos);
}

template <typename T>
void BinaryParseVisitor::visit(AnimationTrack<T>& track) {
    if (version != 0 && version < M2_VERSION_WOTLK) {
        readLegacyTrack<T>(track, [](const T& v) { return v; });
        return;
    }
    track.interpolationType = static_cast<InterpolationType>(reader.read<u16>());
    track.globalSequenceId = reader.read<u16>();

    readAnimationVector(track.timestamps, track.timestampRefs, track.globalSequenceId);
    readAnimationVector(track.values, track.valueRefs, track.globalSequenceId);
}

template <typename T>
std::vector<T> BinaryParseVisitor::readFlatSpan() {
    const auto count = reader.read<u32>();
    const auto offset = reader.read<u32>();
    std::vector<T> out;
    if (count == 0) {
        return out;
    }
    const auto currentPos = reader.getPosition();
    reader.setPosition(offset + baseOffset);
    out = reader.read<std::vector<T>>(count);
    reader.setPosition(currentPos);
    return out;
}

template <typename FileT, typename T, typename Convert>
void BinaryParseVisitor::readLegacyTrack(AnimationTrack<T>& track, Convert convert) {
    track.interpolationType = static_cast<InterpolationType>(reader.read<u16>());
    track.globalSequenceId = reader.read<u16>();
    // Interpolation ranges: index pairs per sequence, redundant with the
    // sequence windows the split below already uses.
    (void)reader.read<KeySpanRef>();
    const std::vector<u32> times = readFlatSpan<u32>();
    const std::vector<FileT> values = readFlatSpan<FileT>();

    track.timestamps.clear();
    track.values.clear();
    track.timestampRefs.clear();
    track.valueRefs.clear();

    const size_t n = std::min(times.size(), values.size());
    if (n == 0) {
        return;
    }

    const bool global = track.globalSequenceId != 0xFFFF;
    if (global || legacyWindows.empty()) {
        // A global-sequence track runs on its own clock: its keys are already
        // relative to zero and become the single sub-array later versions
        // store. Same for a track read outside any sequence context.
        track.timestamps.emplace_back(times.begin(), times.begin() + n);
        auto& out = track.values.emplace_back();
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            out.push_back(convert(values[i]));
        }
        return;
    }

    track.timestamps.resize(legacyWindows.size());
    track.values.resize(legacyWindows.size());
    for (size_t s = 0; s < legacyWindows.size(); ++s) {
        const auto& w = legacyWindows[s];
        const auto first = std::lower_bound(times.begin(), times.begin() + n, w.start);
        const auto last = std::upper_bound(first, times.begin() + n, w.end);
        auto& ts = track.timestamps[s];
        auto& vs = track.values[s];
        ts.reserve(static_cast<size_t>(last - first));
        vs.reserve(static_cast<size_t>(last - first));
        for (auto it = first; it != last; ++it) {
            ts.push_back(*it - w.start);
            vs.push_back(convert(values[static_cast<size_t>(it - times.begin())]));
        }
    }
}

template <typename T>
void BinaryParseVisitor::visit(ParticleAnimationTrack<T>& track) {
    visit(track.timestamps);
    visit(track.values);
}

} // namespace m2
} // namespace whiteout
