// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7.cpp
/// @brief BC7 block-compression codec: RGBA8 ↔ BC7 decode and encode.
///
/// Fully decodes and encodes all 8 BC7 modes (0–7) according to the
/// BC7/BPTC specification.  No external dependencies – pure C++20.
///
/// Implementation is split into .inl parts under bc7/ for readability:
///   decode.inl         – mode table, block decoder, decodeTexture
///   encode_common.inl  – colour helpers, PCA, endpoint fitting
///   encode_mode[0-7].inl – individual mode encoders
///   encode_block.inl   – partition search, encode_block dispatch, encodeTexture

#include "bc7.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace whiteout::textures {
namespace bc7 {

// clang-format off
#include "bc7/decode.inl"
#include "bc7/encode_common.inl"
#include "bc7/encode_mode6.inl"
#include "bc7/encode_mode5.inl"
#include "bc7/encode_mode1.inl"
#include "bc7/encode_mode3.inl"
#include "bc7/encode_mode0.inl"
#include "bc7/encode_mode2.inl"
#include "bc7/encode_mode4.inl"
#include "bc7/encode_mode7.inl"
#include "bc7/encode_block.inl"
// clang-format on

} // namespace bc7
} // namespace whiteout::textures