// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pixel_convert.cpp
/// @brief Template-generated pixel-format conversion tables.

#include "pixel_convert.h"

namespace whiteout::textures {

// ============================================================================
// N×N bulk conversion table (auto-generated from UncompressedFormats)
// ============================================================================

namespace {

using BulkFn = BulkConvertFn;

/// Build the N×N table at static-init time via template instantiation.
struct ConversionTable {
    BulkFn entries[kUncompressedCount][kUncompressedCount]{};

    ConversionTable() {
        for_each_format<UncompressedFormats>([this]<PixelFormat Src>() {
            for_each_format<UncompressedFormats>([this]<PixelFormat Dst>() {
                constexpr u32 si = static_cast<u32>(Src);
                constexpr u32 di = static_cast<u32>(Dst);
                entries[si][di] = &convert_pixels<Src, Dst>;
            });
        });
    }
};

const ConversionTable g_table;

} // anonymous namespace

BulkConvertFn get_converter(PixelFormat src, PixelFormat dst) {
    const u32 si = static_cast<u32>(src);
    const u32 di = static_cast<u32>(dst);
    if (si >= kUncompressedCount || di >= kUncompressedCount) return nullptr;
    return g_table.entries[si][di];
}

// ============================================================================
// Per-pixel float converter arrays (for custom pipelines)
// ============================================================================

namespace {

struct ToTable {
    ToRGBA32F entries[kUncompressedCount]{};

    ToTable() {
        for_each_format<UncompressedFormats>([this]<PixelFormat Fmt>() {
            entries[static_cast<u32>(Fmt)] = &read_rgba32f<Fmt>;
        });
    }
};

struct FromTable {
    FromRGBA32F entries[kUncompressedCount]{};

    FromTable() {
        for_each_format<UncompressedFormats>([this]<PixelFormat Fmt>() {
            entries[static_cast<u32>(Fmt)] = &write_rgba32f<Fmt>;
        });
    }
};

const ToTable g_to_table;
const FromTable g_from_table;

} // anonymous namespace

ToRGBA32F get_to_rgba32f(PixelFormat fmt) {
    const u32 idx = static_cast<u32>(fmt);
    if (idx >= kUncompressedCount) return nullptr;
    return g_to_table.entries[idx];
}

FromRGBA32F get_from_rgba32f(PixelFormat fmt) {
    const u32 idx = static_cast<u32>(fmt);
    if (idx >= kUncompressedCount) return nullptr;
    return g_from_table.entries[idx];
}

} // namespace whiteout::textures
