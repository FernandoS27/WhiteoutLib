// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <optional>
#include <vector>
#include "base.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// ANIM File Chunks and Tags (Legion 24500+)
// ============================================================================

constexpr u32 AFM2_TAG = makeTag("AFM2");
constexpr u32 AFSA_TAG = makeTag("AFSA");
constexpr u32 AFSB_TAG = makeTag("AFSB");

// ============================================================================
// ANIM File Structure
// ============================================================================

// Pre-Legion: .anim files are raw data blobs containing timestamps and values
// for animation blocks. They are loaded when (Sequence.flags & 0x130) == 0
// Filename format: "%s%04d-%02d.anim" % (model_name, anim_id, sub_anim_id)

// Legion 24500+: Optionally chunked format
// Files are chunked if:
// - M2 header's 0x200000 flag is set (new mid-expansion format)
// - The M2 has a .skel file

// ============================================================================
// AFM2 - Animation Data (Legion 24500+)
// ============================================================================

// Raw animation data chunk - same content as old anim file but with chunk header
// For new format M2s, this contains animation data for Events only
// For old format converted files, this is bit-identical to the old .anim file
struct AFM2Chunk {
    std::vector<u8> animationData; // Raw animation block data
};

// ============================================================================
// AFSA - Attachment Skeleton Data (Legion 24500+)
// ============================================================================

// Skeleton data for attachments
// Contains animation data for attachment points
struct AFSAChunk {
    std::vector<u8> attachmentData; // Raw attachment animation data
};

// ============================================================================
// AFSB - Bone Skeleton Data (Legion 24500+)
// ============================================================================

// Skeleton data for bones
// Contains animation data for bone transformations
struct AFSBChunk {
    std::vector<u8> boneData; // Raw bone animation data
};

// ============================================================================
// Complete ANIM File Structure
// ============================================================================

struct AnimProfile {

    // For chunked files (Legion 24500+):
    std::optional<AFM2Chunk> afm2_chunk = std::nullopt; // Animation data
    std::optional<AFSAChunk> afsa_chunk = std::nullopt; // Attachment data
    std::optional<AFSBChunk> afsb_chunk = std::nullopt; // Bone data

    bool isChunked = false; // Whether this uses chunked format
};

} // namespace m2
} // namespace whiteout
