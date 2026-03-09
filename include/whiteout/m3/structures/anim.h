// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file anim.h
 * @brief Animation system structures — sequences, tracks, groups, and states
 *
 * Defines the M3 animation hierarchy: Sequence (SEQS) holds frame ranges and
 * playback metadata; SubTrackContainer (STC_) binds animation IDs to 13 typed
 * AnimBlock arrays; AnimationGroup (STG_) groups sub-tracks; AnimationState
 * (STS_) provides top-level state management. Event (EVNT) and
 * BoneAnimationSet (BSET) round out the animation chunk types.
 *
 * @see M3_FILE_FORMAT_SPECIFICATION.md §7 Animation System
 */

#include "base.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Animation System
// ============================================================================

/**
 * @brief EVNT — Animation event (v0–v2, 104–108 bytes)
 *
 * Named event triggered at a specific bone with an optional type code and
 * parameter string. Used for sound cues, spawn effects, etc.
 */
struct Event {
    static constexpr u32 tag = TAG_EVNT;  ///< FourCC tag
    static constexpr u32 max_version = 2; ///< Latest known EVNT version
    std::string name;                     ///< Event name (Ref<CHAR>)
    u32 unknown;                          ///< Unknown field
    u16 boneIndex;                        ///< Index into BONE array
    u16 padding;                          ///< Alignment padding
    Matrix44f transform;                  ///< 4×4 transform matrix
    u32 eventType;                        ///< Engine-specific event type code
    std::string optionString;             ///< Optional parameter string (Ref<CHAR>)
    u32 rttChannelIndex;                  ///< RTT channel index
    u32 extraParameter;                   ///< Extra parameter (v2+)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief SEQS — Animation sequence (v0–v2, up to 92 bytes)
 *
 * Defines a named animation clip with frame range, playback speed,
 * looping flags, blend time, and bounding volume.
 */
struct Sequence {
    static constexpr u32 tag = TAG_SEQS;     ///< FourCC tag
    static constexpr u32 max_version = 2;    ///< Latest known SEQS version
    i32 id;                                  ///< Unique sequence identifier
    i32 index;                               ///< Sequence index
    std::string name;                        ///< Sequence name (Ref<CHAR>)
    u32 startFrame;                          ///< First frame (inclusive)
    u32 endFrame;                            ///< Last frame (inclusive)
    f32 moveSpeed;                           ///< Movement speed multiplier
    SequenceFlag flags = SequenceFlag::None; ///< Playback flags (loop, global, etc.)
    u32 frequency;                           ///< Selection frequency / priority weight
    u32 replayStart;                         ///< Replay region start frame
    u32 replayEnd;                           ///< Replay region end frame
    u32 blendTime;                           ///< Blend-in time (ms)
    Extent bounds;                           ///< Animated bounding volume
    std::vector<u8> animationSets;           ///< Animation set indices (U8__)

    struct {
        u32 unknown; ///< v2: 4 bytes; v1: 8 bytes
    } deprecated;    ///< v1 only — removed in v2
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief STC_ — Sub-track container (v0–v4, 204 bytes)
 *
 * Binds animation IDs to concrete keyframe data stored in 13 typed AnimBlock
 * arrays (slots 0–12). Each slot handles a different value type: events,
 * vectors, quaternions, colors, scalars, flags, and bounding extents.
 */
struct SubTrackContainer {
    static constexpr u32 tag = TAG_STC;   ///< FourCC tag
    static constexpr u32 max_version = 4; ///< Latest known STC_ version
    std::string name;                     ///< Container name (Ref<CHAR>)
    u16 runsConcurrent;                   ///< Non-zero if runs concurrently
    u16 animPriority;                     ///< Animation priority level
    u16 animationStateIndex;              ///< Parent STS_ index
    u16 padding;                          ///< Alignment padding
    std::vector<u32> animIds;             ///< Animation IDs (U32_)
    std::vector<u32> animRefs;            ///< Animation reference indices (U32_)
    u32 unknown;                          ///< Unknown field
    // 13 animation data block arrays (each contains AnimBlocks with typed keys)
    std::vector<AnimBlock<Event>> sdev;      ///< Slot 0: SDEV (Event keys)
    std::vector<AnimBlock<Vector2f>> sd2v;   ///< Slot 1: SD2V (Vector2f keys)
    std::vector<AnimBlock<Vector3f>> sd3v;   ///< Slot 2: SD3V (Vector3f keys)
    std::vector<AnimBlock<Quaternion>> sd4q; ///< Slot 3: SD4Q (Quaternion keys)
    std::vector<AnimBlock<ColorBGRA>> sdcc;  ///< Slot 4: SDCC (ColorBGRA keys)
    std::vector<AnimBlock<f32>> sdr3;        ///< Slot 5: SDR3 (f32 keys)
    std::vector<AnimBlock<u8>> sdu8;         ///< Slot 6: SDU8 (u8 keys)
    std::vector<AnimBlock<i16>> sds6;        ///< Slot 7: SDS6 (i16 keys)
    std::vector<AnimBlock<u16>> sdu6;        ///< Slot 8: SDU6 (u16 keys)
    std::vector<AnimBlock<i32>> sds3;        ///< Slot 9: SDS3 (i32 keys)
    std::vector<AnimBlock<u32>> sdu3;        ///< Slot 10: SDU3 (u32 keys)
    std::vector<AnimBlock<Flag>> sdfg;       ///< Slot 11: SDFG (Flag keys)
    std::vector<AnimBlock<Extent>> sdmb;     ///< Slot 12: SDMB (Extent keys)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief STG_ — Animation group (v0, 24 bytes)
 *
 * Groups sub-track containers by name for organizational purposes.
 */
struct AnimationGroup {
    static constexpr u32 tag = TAG_STG;   ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known STG_ version
    std::string name;                     ///< Group name (Ref<CHAR>)
    std::vector<u32> subtrackIndices;     ///< Indices into STC_ array (U32_)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief STS_ — Animation state (v0, 28 bytes)
 *
 * Top-level animation state containing a set of animation IDs and
 * 16 bytes of unknown state data.
 */
struct AnimationState {
    static constexpr u32 tag = TAG_STS;   ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known STS_ version
    std::vector<u32> animIds;             ///< Animation IDs (U32_)
    std::array<u8, 16> unknown;           ///< Unknown state data (16 bytes)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief BSET — Bone animation set (v0, 32 bytes)
 *
 * Maps a bone to specific animation sequences with fallback support.
 * In practice, always null in observed corpus data.
 */
struct BoneAnimationSet {
    static constexpr u32 tag = TAG_BSET;  ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known BSET version
    Flag flags;                           ///< Flags
    u16 animationSequenceIndex;           ///< Primary sequence index
    u16 fallbackSequenceIndex;            ///< Fallback sequence index
    std::string name;                     ///< Set name (Ref<CHAR>)
    std::vector<u16> splitItems;          ///< Split item indices (U16_)
    M3_DEFINE_VERSION_ACCESSORS()
};

} // namespace m3
} // namespace whiteout
