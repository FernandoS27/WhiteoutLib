// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7.cpp
/// @brief BC7 block-compression codec: RGBA8 ↔ BC7 decode and encode.
///
/// Fully decodes and encodes all 8 BC7 modes (0–7) according to the
/// BC7/BPTC specification.  No external dependencies – pure C++20.
///
/// Implementation is split into .inl parts under bc7/ for readability:
///   decode.inl         – mode table, block decoder, decode_texture
///   encode_common.inl  – colour helpers, PCA, endpoint fitting
///   encode_mode[0-7].inl – individual mode encoders
///   encode_block.inl   – partition search, encode_block dispatch, encode_texture

#include "../common/binary_reader.h"
#include "binary_parse_visitor.h"
#include "chunk_traits.h"

#include <array>
#include <cassert>
#include <type_traits>

namespace whiteout {
namespace m3 {

using common::BinaryReader;

// clang-format off
#include "binary_parse_visitor/common.inl"
#include "binary_parse_visitor/anim.inl"
#include "binary_parse_visitor/base.inl"
#include "binary_parse_visitor/effect.inl"
#include "binary_parse_visitor/material.inl"
#include "binary_parse_visitor/mesh.inl"
#include "binary_parse_visitor/physics.inl"
#include "binary_parse_visitor/scene.inl"
// clang-format on

} // namespace m3
} // namespace whiteout