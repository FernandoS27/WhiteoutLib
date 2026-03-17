// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pixel_convert.h
/// @brief Pixel-format conversion dispatch table and per-pixel converters.
///
/// Internal header — not part of the public include path.
///
/// Provides:
///   - BulkConvertFn / get_converter()  — N×N table for fast bulk conversion
///   - ToRGBA32F / FromRGBA32F          — per-pixel float converters for
///                                        custom pipelines (e.g. normal maps)

#pragma once

#include "format_traits.h"

namespace whiteout::textures {

// ============================================================================
// Bulk conversion table (N×N, auto-sized from UncompressedFormats)
// ============================================================================

/// Bulk-converts @p pixel_count pixels from one uncompressed format to another.
using BulkConvertFn = void (*)(const u8* src, u8* dst, u32 pixel_count);

/// Look up the optimal conversion function for a (src, dst) format pair.
/// Both formats must be uncompressed (index < kUncompressedCount).
BulkConvertFn get_converter(PixelFormat src, PixelFormat dst);

// ============================================================================
// Per-pixel float converters (for custom per-pixel pipelines)
// ============================================================================

using ToRGBA32F = void (*)(const u8* src, f32* dst);
using FromRGBA32F = void (*)(const f32* src, u8* dst);

ToRGBA32F get_to_rgba32f(PixelFormat fmt);
FromRGBA32F get_from_rgba32f(PixelFormat fmt);

} // namespace whiteout::textures
