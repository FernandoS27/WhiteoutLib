
#include <whiteout/models/m2/sequence_loader.h>

#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "sequence_loader_internal.h"
#include "track_walk.h"
#include "wow_file_system.h"

namespace whiteout {
namespace m2 {

namespace {

/// Reads one sequence's slice of every track out of an already-loaded `.anim`.
///
/// A track that has no reference for this sequence is left alone: either its
/// keys came from the model itself, or the sequence has no keys in that track
/// at all. Both are the same instruction — do nothing.
struct KeyFiller {
    common::BinaryReader& reader;
    u32 size = 0;
    u32 sequence = 0;

    template <class T>
    void fill(std::vector<std::vector<T>>& keys, const std::vector<KeySpanRef>& refs) {
        if (sequence >= refs.size() || sequence >= keys.size())
            return;
        const KeySpanRef& ref = refs[sequence];
        if (ref.count == 0 || ref.offset >= size)
            return;
        reader.setPosition(ref.offset);
        keys[sequence] = reader.read<std::vector<T>>(ref.count);
    }

    void operator()(AnimationTrackBase& track) {
        fill(track.timestamps, track.timestampRefs);
    }

    template <class T>
    void operator()(AnimationTrack<T>& track) {
        fill(track.timestamps, track.timestampRefs);
        fill(track.values, track.valueRefs);
    }
};

/// Frees the per-sequence references themselves, once nothing can ask for a
/// deferred load any more. Only an eager parse gets to do this — a lazy model
/// needs them for as long as it lives.
struct RefDropper {
    void operator()(AnimationTrackBase& track) {
        track.timestampRefs.clear();
        track.timestampRefs.shrink_to_fit();
    }

    template <class T>
    void operator()(AnimationTrack<T>& track) {
        (*this)(static_cast<AnimationTrackBase&>(track));
        track.valueRefs.clear();
        track.valueRefs.shrink_to_fit();
    }
};

/// The mirror of KeyFiller: frees what it read, and only that.
struct KeyDropper {
    u32 sequence = 0;

    template <class T>
    void drop(std::vector<std::vector<T>>& keys, const std::vector<KeySpanRef>& refs) {
        if (sequence >= refs.size() || sequence >= keys.size())
            return;
        if (refs[sequence].count == 0)
            return;
        keys[sequence].clear();
        keys[sequence].shrink_to_fit();
    }

    void operator()(AnimationTrackBase& track) {
        drop(track.timestamps, track.timestampRefs);
    }

    template <class T>
    void operator()(AnimationTrack<T>& track) {
        drop(track.timestamps, track.timestampRefs);
        drop(track.values, track.valueRefs);
    }
};

/// The sequence whose `.anim` file holds @p index's keys.
///
/// An alias sequence stores no keys of its own; CM2Shared::LoadLowPrioritySequence
/// walks `aliasNext` until it reaches one that does and reads that file, then
/// resolves the *requested* sequence's offsets against it. The chain is a ring,
/// so the walk is bounded by the sequence count.
u32 ResolveAliasSource(const Model& model, u32 index) {
    u32 source = index;
    for (std::size_t guard = 0; guard < model.sequences.size(); ++guard) {
        if (!hasFlag(model.sequences[source].flags, SequenceFlag::IsAlias))
            break;
        const u32 next = model.sequences[source].aliasNext;
        if (next >= model.sequences.size() || next == source)
            break;
        source = next;
    }
    return source;
}

} // namespace

SequenceLoader::SequenceLoader(std::shared_ptr<WoWFileSystem> fs, std::size_t sequenceCount)
    : fs_(std::move(fs)), state_(sequenceCount, State::Pending) {
    const auto& inFile = fs_->sequenceInFile();
    for (std::size_t i = 0; i < state_.size() && i < inFile.size(); ++i) {
        if (inFile[i] != 0)
            state_[i] = State::Resident;
    }
}

SequenceLoader::~SequenceLoader() = default;

void dropSequenceKeyRefs(Model& model) {
    RefDropper dropper;
    forEachTrack(model, dropper);
}

bool hasLazyAnimations(const Model& model) {
    return static_cast<bool>(model.sequenceLoader);
}

bool sequenceKeysPending(const Model& model, u32 sequenceIndex) {
    if (!model.sequenceLoader)
        return false;
    const auto& state = model.sequenceLoader->state();
    return sequenceIndex < state.size() && state[sequenceIndex] == SequenceLoader::State::Pending;
}

bool loadSequence(Model& model, u32 sequenceIndex) {
    if (!model.sequenceLoader)
        return true; // eager parse: the keys are already in the model
    auto& loader = *model.sequenceLoader;
    auto& state = loader.state();
    if (sequenceIndex >= state.size() || sequenceIndex >= model.sequences.size())
        return false;
    if (state[sequenceIndex] != SequenceLoader::State::Pending)
        return state[sequenceIndex] == SequenceLoader::State::Resident;

    const Sequence& source = model.sequences[ResolveAliasSource(model, sequenceIndex)];
    WoWFileSystem::AnimBuffer anim;
    if (!loader.fs().readAnim(anim, source.id, source.variationIndex,
                              hasFlag(model.globalFlags.value, GlobalFlag::UpgradedFormat))) {
        // The sibling is not there. Recording that stops a per-frame caller
        // from asking the content provider for it again on every frame; the
        // sampler already treats a keyless sequence as the animref default.
        state[sequenceIndex] = SequenceLoader::State::Missing;
        return false;
    }

    common::span_streambuf sbuf(anim.data);
    common::BinaryReader reader(sbuf);
    KeyFiller filler{reader, static_cast<u32>(anim.data.size()), sequenceIndex};
    forEachTrack(model, filler);

    // The keys are copied into the model, so the file itself is no longer
    // referenced by anything — unlike the client, which fixes its offsets up to
    // point into the buffer and has to keep it. Only worth dropping when the
    // loads are spread over time: an eager parse loads every sequence back to
    // back, and several of them can share one file.
    if (loader.fs().lazyAnimations())
        loader.fs().evictAnimBuffer(source.id, source.variationIndex);
    state[sequenceIndex] = SequenceLoader::State::Resident;
    return true;
}

void unloadSequence(Model& model, u32 sequenceIndex) {
    if (!model.sequenceLoader)
        return;
    auto& state = model.sequenceLoader->state();
    if (sequenceIndex >= state.size() || state[sequenceIndex] != SequenceLoader::State::Resident)
        return;
    const auto& inFile = model.sequenceLoader->fs().sequenceInFile();
    if (sequenceIndex < inFile.size() && inFile[sequenceIndex] != 0)
        return; // read from the `.m2` at parse time; there is nothing to re-read it from

    KeyDropper dropper{sequenceIndex};
    forEachTrack(model, dropper);
    state[sequenceIndex] = SequenceLoader::State::Pending;
}

void unloadAllSequences(Model& model) {
    if (!model.sequenceLoader)
        return;
    for (u32 i = 0; i < static_cast<u32>(model.sequenceLoader->state().size()); ++i)
        unloadSequence(model, i);
}

} // namespace m2
} // namespace whiteout
