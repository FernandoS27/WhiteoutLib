
#pragma once

#include <whiteout/models/m2/types.h>
#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "internal_structures.h"
#include "legacy.h"
#include "traits.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <ostream>
#include <type_traits>

namespace whiteout {
namespace m2 {

class BinaryWriterVisitor {
public:
    explicit BinaryWriterVisitor(common::BinaryWriter& writer);

    template <typename T>
    void write(const T& header) {
        start();
        deferredWrites.clear();
        deferredWrites.push_back([this, &header]() { visit(header); });
        while (!deferredWrites.empty()) {
            auto writeFunc = std::move(deferredWrites.front());
            deferredWrites.pop_front();
            writeFunc();
        }
    }

    struct AnimDataBuffer {
        u32 anim_id;
        u32 sub_anim_id;
        std::vector<u8> data;
    };

    std::deque<AnimDataBuffer>& getAnimDataBuffers() {
        return animDataBuffers;
    }
    const std::deque<AnimDataBuffer>& getAnimDataBuffers() const {
        return animDataBuffers;
    }

    void setVersion(u32 v) {
        version = v;
    }

protected:
    void start();

    void visit(const GlobalFlags& flags);
    void visit(const GlobalSequence& seq);
    void visit(const Sequence& seq);
    void visit(const Vertex& vertex);
    void visit(const Bone& bone);
    void visit(const Texture& texture);
    void visit(const Material& material);
    void visit(const TextureWeight& weight);
    void visit(const TextureTransform& transform);
    void visit(const ColorAnimation& color);
    void visit(const Light& light);
    void visit(const CameraSpline& spline);
    void visit(const Camera& camera);
    void visit(const Attachment& attachment);
    void visit(const RibbonEmitter& emitter);
    void visit(const ParticleEmitter& emitter);
    void visit(const Event& event);
    void visit(const Model& model);
    void visit(const MD20Header& header);

    void visit(const TXACChunk& chunk);
    void visit(const PFIDChunk& chunk);
    void visit(const SFIDChunk& chunk);
    void visit(const AFIDEntry& entry);
    void visit(const AFIDChunk& chunk);
    void visit(const BFIDChunk& chunk);
    void visit(const EXPTEntry& entry);
    void visit(const EXPTChunk& chunk);
    void visit(const EXP2Chunk& chunk);
    void visit(const PABCChunk& chunk);
    void visit(const PADCChunk& chunk);
    void visit(const PSBCChunk& chunk);
    void visit(const PEDCChunk& chunk);
    void visit(const SKIDChunk& chunk);
    void visit(const TXIDChunk& chunk);
    void visit(const LodProfile& chunk);
    void visit(const M2RPIDEntry& entry);
    void visit(const M2RPIDChunk& chunk);
    void visit(const GPIDEntry& entry);
    void visit(const GPIDChunk& chunk);
    void visit(const ParticleGeosetData& entry);
    void visit(const PGD1Chunk& chunk);
    void visit(const WaterfallData& data);
    void visit(const WFV3Chunk& chunk);
    void visit(const PFDCChunk& chunk);
    void visit(const EdgeFadeData& entry);
    void visit(const EDGFChunk& chunk);
    void visit(const DistanceFadeData& entry);
    void visit(const NERFChunk& chunk);
    void visit(const DetailedLightData& entry);
    void visit(const DETLChunk& chunk);
    void visit(const DebugOcclusionData& entry);
    void visit(const DBOCChunk& chunk);
    void visit(const AFRAChunk& chunk);
    void visit(const PCOLChunk& chunk);
    void visit(const DPIVChunk& chunk);
    void visit(const TexturedLightData& entry);
    void visit(const TEXLChunk& chunk);
    void visit(const ParticleEmitterExtension& chunk);

    void visit(const PHYSHeader& header);
    void visit(const PHYTEntry& entry);
    void visit(const BODYEntry& entry);
    void visit(const BDY2Entry& entry);
    void visit(const BDY3Entry& entry);
    void visit(const BDY4Entry& entry);
    void visit(const SHAPEntry& entry);
    void visit(const SHP2Entry& entry);
    void visit(const BOXSEntry& entry);
    void visit(const CAPSEntry& entry);
    void visit(const SPHSEntry& entry);
    void visit(const PLYTNode& node);
    void visit(const PLYTData& data, const PLYTHeader& header);
    void visit(const PLYTEntry& entry);
    void visit(const JOINEntry& entry);
    void visit(const Matrix3x4& matrix);
    void visit(const WELJEntry& entry);
    void visit(const WLJ2Entry& entry);
    void visit(const WLJ3Entry& entry);
    void visit(const SPHJEntry& entry);
    void visit(const SHOJEntry& entry);
    void visit(const SHJ2Entry& entry);
    void visit(const PRSJEntry& entry);
    void visit(const PRS2Entry& entry);
    void visit(const REVJEntry& entry);
    void visit(const REV2Entry& entry);
    void visit(const DSTJEntry& entry);
    void visit(const PHYVEntry& entry);

    void visit(const BONEHeader& header);
    void visit(const Matrix44f& matrix);
    void visit(const BIDAChunk& chunk);
    void visit(const BOMTChunk& chunk);

    void visit(const AFM2Chunk& chunk);
    void visit(const AFSAChunk& chunk);
    void visit(const AFSBChunk& chunk);

    void visit(const SKL1Chunk& chunk);
    void visit(const SKA1Chunk& chunk);
    void visit(const SKB1Chunk& chunk);
    void visit(const SKS1Chunk& chunk);
    void visit(const SKPDChunk& chunk);

    void visit(const SkinSection& section);
    void visit(const Batch& batch);
    void visit(const ShadowBatch& batch);
    void visit(const SkinProfile& profile);
    void visit(const SkinFile& file);

    void visit(const AnimationTrackBase& track);

    template <typename T>
    void visit(const AnimationTrack<T>& track);

    template <typename T>
    void visit(const ParticleAnimationTrack<T>& track);

    template <typename T>
    void writeVector(const std::vector<T>& array, common::BinaryWriter* dataWriter);

    /// @brief Write a per-sequence key array (the M2Array-of-M2Array half of a
    ///        track): outer count/offset here, inner headers deferred to the
    ///        data region, each sequence's keys routed to its `.anim` buffer
    ///        unless the model keeps them (or @p inFileOnly forces it, which is
    ///        what a global-sequence track needs — its sub-arrays all live in
    ///        the model, whatever the sequence flags say).
    template <typename U>
    void writeSequenceArrays(const std::vector<std::vector<U>>& values, bool inFileOnly);

    // ≤TBC (<264) track writing: merge the per-sequence keys back onto the
    // global timeline laid out by visit(Model), and emit the interpolation
    // ranges that index it.
    template <typename FileT, typename T, typename Convert>
    void writeLegacyTrack(const AnimationTrack<T>& track, Convert convert);
    void writeLegacyTrackHead(const AnimationTrackBase& track);
    void writeLegacyBoneRotation(const AnimationTrack<CompatQuaternion>& track);
    void writeLegacyParticleColorBlock(const ParticleEmitter& emitter);

    /// Backing storage for the merged arrays a legacy track write defers; the
    /// deferred lambdas hold references, so the vectors must outlive them.
    template <typename V>
    std::vector<V>& arenaVector() {
        auto holder = std::make_shared<std::vector<V>>();
        legacyArena.push_back(holder);
        return *holder;
    }

    template <typename T>
    void visit(const std::vector<T>& array);

    void visit(const std::string& str);

    common::BinaryWriter& writer;
    uint32_t version = 0;
    GlobalFlag globalFlags = GlobalFlag::None;
    std::deque<std::function<void()>> deferredWrites;
    u32 baseOffset = 0;
    struct AnimDataWriterState {
        std::unique_ptr<common::vector_streambuf> streambuf;
        std::unique_ptr<std::ostream> stream;
        std::unique_ptr<common::BinaryWriter> writer;
    };

    std::deque<AnimDataBuffer> animDataBuffers;
    std::deque<AnimDataWriterState> animDataWriterStates;
    std::vector<common::BinaryWriter*> animDataWriters;

    /// Per sequence, in order: its window on the ≤TBC global timeline; laid
    /// out by visit(Model) before the sequence list writes, consumed by
    /// visit(Sequence) and the legacy track merge. Empty for ≥264 output.
    std::vector<LegacyWindow> legacyWindows;
    size_t legacyWindowIndex = 0;
    std::deque<std::shared_ptr<void>> legacyArena;
};

template <typename T>
void BinaryWriterVisitor::visit(const AnimationTrack<T>& track) {
    if (version != 0 && version < M2_VERSION_WOTLK) {
        writeLegacyTrack<T>(track, [](const T& v) { return v; });
        return;
    }
    writer.write(track.interpolationType);
    writer.write(track.globalSequenceId);

    const bool global = track.globalSequenceId != 0xFFFF;
    writeSequenceArrays(track.timestamps, global);
    writeSequenceArrays(track.values, global);
}

template <typename FileT, typename T, typename Convert>
void BinaryWriterVisitor::writeLegacyTrack(const AnimationTrack<T>& track, Convert convert) {
    writer.write(track.interpolationType);
    writer.write(track.globalSequenceId);

    auto& ranges = arenaVector<LegacyRange>();
    auto& times = arenaVector<u32>();
    auto& values = arenaVector<FileT>();

    const size_t nSub = std::min(track.timestamps.size(), track.values.size());
    const T* fallback = nullptr;
    for (size_t s = 0; s < nSub && !fallback; ++s) {
        if (!track.values[s].empty()) {
            fallback = &track.values[s][0];
        }
    }

    if (track.globalSequenceId != 0xFFFF || legacyWindows.empty()) {
        // Global-sequence tracks keep their single span verbatim; its times
        // are already relative to the global loop's own clock.
        if (nSub != 0) {
            const auto& ts = track.timestamps[0];
            const auto& vs = track.values[0];
            const size_t n = std::min(ts.size(), vs.size());
            for (size_t k = 0; k < n; ++k) {
                times.push_back(ts[k]);
                values.push_back(convert(vs[k]));
            }
        }
    } else if (fallback != nullptr) {
        for (size_t s = 0; s < legacyWindows.size(); ++s) {
            const auto& w = legacyWindows[s];
            const auto* ts = s < nSub ? &track.timestamps[s] : nullptr;
            const auto* vs = s < nSub ? &track.values[s] : nullptr;
            const size_t n = (ts && vs) ? std::min(ts->size(), vs->size()) : 0;
            u32 const firstIdx = static_cast<u32>(times.size());
            if (n > 1) {
                for (size_t k = 0; k < n; ++k) {
                    times.push_back(w.start + (*ts)[k]);
                    values.push_back(convert((*vs)[k]));
                }
                ranges.push_back(LegacyRange{firstIdx, firstIdx + static_cast<u32>(n) - 1});
            } else {
                // Old clients want each sequence's window covered; a constant
                // or keyless sequence becomes a boundary pair holding its one
                // value (or the track's first value anywhere).
                const T& v = n == 1 ? (*vs)[0] : *fallback;
                times.push_back(w.start);
                values.push_back(convert(v));
                times.push_back(w.end);
                values.push_back(convert(v));
                ranges.push_back(LegacyRange{firstIdx, firstIdx + 1});
            }
        }
        ranges.push_back(LegacyRange{0, 0});
    }

    writeVector(ranges, &writer);
    writeVector(times, &writer);
    writeVector(values, &writer);
}

template <typename U>
void BinaryWriterVisitor::writeSequenceArrays(const std::vector<std::vector<U>>& values,
                                              bool inFileOnly) {
    writer.template write<u32>(static_cast<u32>(values.size()));
    u32 const offsetPos = writer.getPosition();
    writer.template write<u32>(0);
    if (values.empty()) {
        return;
    }

    deferredWrites.push_back([this, offsetPos, &values, inFileOnly]() {
        u32 const currentPos = writer.getPosition();
        writer.setPosition(offsetPos);
        writer.write(currentPos - baseOffset);
        writer.setPosition(currentPos);

        for (size_t i = 0; i < values.size(); ++i) {
            common::BinaryWriter* dataWriter =
                (!inFileOnly && i < animDataWriters.size()) ? animDataWriters[i] : &writer;
            writeVector(values[i], dataWriter);
        }

        writer.AlignTo(16);
    });
}

template <typename T>
void BinaryWriterVisitor::visit(const ParticleAnimationTrack<T>& track) {
    visit(track.timestamps);
    visit(track.values);
}

template <typename T>
void BinaryWriterVisitor::writeVector(const std::vector<T>& array,
                                      common::BinaryWriter* dataWriter) {
    // The count/offset header always lives in the file being visited; only the
    // element data may be routed elsewhere (a `.anim` sibling's buffer). An
    // externally-stored span records its offset within that sibling, which the
    // sibling reads from its own origin — no baseOffset involved.
    writer.template write<u32>(static_cast<u32>(array.size()));
    u32 const offsetPos = writer.getPosition();
    writer.template write<u32>(0);
    if (array.empty()) {
        return;
    }

    deferredWrites.push_back([this, dataWriter, offsetPos, &array]() {
        const bool external = dataWriter != &writer;
        u32 const dataPos = dataWriter->getPosition();
        u32 const currentPos = writer.getPosition();
        writer.setPosition(offsetPos);
        writer.write(external ? dataPos : dataPos - baseOffset);
        writer.setPosition(currentPos);

        if constexpr (bulk_copyable_v<T>) {
            dataWriter->write(array);
        } else {
            assert(dataWriter == &writer && "per-field elements cannot be routed to .anim data");
            for (const T& item : array) {
                visit(item);
            }
        }

        dataWriter->AlignTo(16);
    });
}

template <typename T>
void BinaryWriterVisitor::visit(const std::vector<T>& array) {
    writeVector(array, &writer);
}

} // namespace m2
} // namespace whiteout
