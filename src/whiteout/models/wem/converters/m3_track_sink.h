// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3_track_sink.h
 * @brief One keyed property into an `.m3`'s sequences, and its AnimRef bound to it.
 *
 * `m3_anim::Export` was the only writer of SD blocks, and the Warcraft III window
 * rule lived inside it. The effect crossing (`cross/mdx_m3_effects`) writes the
 * same blocks for records WEM does not hold -- a particle's emission rate, a
 * ribbon's colour -- so the rule lives here once and both call it
 * (WC3_TO_SC2_COMPLETION_PLAN.md Â§2.1). Nothing here knows what a channel
 * targets: the caller says which stream, which basis and which AnimRef.
 */

#include <set>
#include <span>
#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/wem/anim/clip.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_sink {

/// The typed SD array a stream's keys go into; the value is the slot number the
/// `(slot << 16) | block` reference carries.
enum class Stream : u32 {
    None = 0,
    Sd2v = 1,
    Sd3v = 2,
    Sd4q = 3,
    Sdcc = 4,
    Sdr3 = 5,
    Sds6 = 7,
    Sdu3 = 10,
    Sdfg = 11,
};

/// Milliseconds, rounded to the nearest tick.
i32 Ticks(f32 seconds);

/// Whether @p clip is played Warcraft III's way: only the keys inside the
/// window count, and the spans outside them -- before the first key, after the
/// last -- are ONE segment from the last key back to the first. A sequence
/// window and a global sequence both are.
bool WarcraftWindow(const Clip& clip);

/// How one sub-track's values are laid out and where they go.
struct StreamSpec {
    Stream stream = Stream::None;
    /// The layout of one key's value in the sub-track: F32, F32x2, F32x3, F32x4
    /// (an RGBA colour for `Sdcc`), Quat, or U32 (`Sdu3`, and `Sds6`'s counts).
    geom::AttrType type = geom::AttrType::F32;
    /// WEM's canonical basis back into StarCraft II's, on a vector or rotation key.
    bool unrebaseVector = false;
    bool unrebaseQuaternion = false;
    /// The rest the first quaternion key is kept on the side of, in the FILE's
    /// basis; null when the property states none.
    const Quaternion* restQuaternion = nullptr;
};

/// Writes @p track into @p stc's typed block for @p spec and returns
/// `(slot << 16) | block` -- or `kInvalidIndex` when no key of it plays inside the
/// clip, in which case the property rests at its AnimRef's own value.
///
/// @p origin is the sequence's first frame in milliseconds and @p duration its
/// length in seconds; @p warcraft is `WarcraftWindow` of the clip.
u32 WriteStream(m3::SubTrackContainer& stc, const StreamSpec& spec, const SubTrack& track,
                i32 origin, f32 duration, bool warcraft);

/// Binding an AnimRef is two statements: `flags` says a track answers the
/// property, and `interpType` is the step half the loader folds into flags
/// bit 4. One AnimRef serves every sequence, so the step stands only while
/// EVERY clip's track holds -- a clip that moves clears it for good, whichever
/// order the clips are wired in.
class Wiring {
public:
    template <class T>
    void wire(m3::AnimRef<T>& ref, u32 animId, Interpolation interp) {
        ref.animId = animId;
        if (interp != Interpolation::Step) {
            moving_.insert(animId);
        }
        const bool step = interp == Interpolation::Step && moving_.count(animId) == 0;
        ref.flags = kBound;
        ref.interpType = step ? u16(0) : u16(1);
    }

    /// What a bound AnimRef states in `flags`: bit 1 that an animId in this
    /// model's own tracks answers the property, bit 2 that the tracks were this
    /// model's and not an attached `.m3a`'s. All 15,633 bound bone refs measured
    /// over 1,169 shipped self-animated models write exactly this and all 56,519
    /// unbound ones write 0 -- there is no third value.
    static constexpr u16 kBound = 0x6;

private:
    std::set<u32> moving_;
};

/// Sorts a container's id/ref table by id: StarCraft II searches it, and every
/// shipped container keeps it sorted.
void SortLookup(m3::SubTrackContainer& stc);

/// Adds one `(animId, animRef)` pair to container @p stcIndex of @p out, keeping
/// its lookup sorted and the animation state it names listing the same ids.
void AddToContainer(m3::Model& out, u32 stcIndex, u32 animId, u32 animRef);

/// The `animId` of every AnimRef an export can key: bones, lights, standard
/// material layers, particle and ribbon emitters. The pointers stay valid while
/// no record is added to @p out.
std::vector<u32*> KeyableAnimIds(m3::Model& out);

/// Gives every AnimRef a track drives an id of its own. No shipped file lets two
/// AnimRefs name one driven id (0 of 33,060), and the game answers a ref through
/// the track-table row its load binds it to (`M3_IsAnimRefAnimated`), never the
/// id -- yet a fold's mask copy, a tint carrier on several material copies, and
/// an emitter's head and tail records all read one stream. The first AnimRef
/// keeps the id; each other takes a fresh one and a copy of the keys in every
/// container that plays them.
void UnshareAnimIds(m3::Model& out);

/// One RGBA sub-track (F32x4, linear or step) out of a colour track and an alpha
/// track that each key on their own times -- resampled on the union of the two,
/// the missing half answered by its rest where a track is absent.
SubTrack MergeColorAlpha(const SubTrack* color, const SubTrack* alpha, const Vector3f& restColor,
                         f32 restAlpha);

} // namespace m3_sink
} // namespace wem
} // namespace models
} // namespace whiteout
