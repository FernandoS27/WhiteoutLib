// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc2.h
/// @brief BC2 (DXT3) encode and decode.
///
/// BC2 stores explicit 4-bit alpha (8 bytes) followed by a BC1 colour
/// block (8 bytes), giving 16 bytes per 4x4 block.
///
/// No external dependencies -- pure C++20.

#pragma once

#include "bcn_common.h"

namespace whiteout::textures {
namespace bc2 {

// ---- Decode ----------------------------------------------------------------

/// Decode a BC2 Texture into a new Texture with RGBA8 pixel format.
///
/// @param src       Source texture (must be BC2).
/// @param out_error Optional string to receive an error message on failure.
/// @return The decoded texture, or std::nullopt on error.
std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

// ---- Encode ----------------------------------------------------------------

/// Compress an RGBA8 Texture into a new Texture with BC2 pixel format.
///
/// @param src       Source texture (must be RGBA8).
/// @param out_error Optional string to receive an error message on failure.
/// @param pool  Optional WorkerPool for parallel tile processing.
/// @return The compressed texture, or std::nullopt on error.
std::optional<Texture> encodeTexture(const Texture& src, std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

} // namespace bc2
} // namespace whiteout::textures
