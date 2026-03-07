// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <vector>
#include "base.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// BONE File Chunks and Tags
// ============================================================================

constexpr u32 BIDA_TAG = makeTag("BIDA");
constexpr u32 BOMT_TAG = makeTag("BOMT");

// ============================================================================
// BONE Header
// ============================================================================

struct BONEHeader {
    u32 unk = 1; // ignored, shall be 1 (possibly version?)
};

// ============================================================================
// BIDA - Bone IDs
// ============================================================================

struct BIDAChunk {
    std::vector<u16>
        boneIds; // count should be equivalent to number of FacePose (808) sequences - 1
};

// ============================================================================
// BOMT - Bone Offset Matrices
// ============================================================================

struct BOMTChunk {
    std::vector<Matrix44f>
        boneOffsetMatrices; // same count as BIDA, transformation matrix for the given bone
};

} // namespace m2
} // namespace whiteout
