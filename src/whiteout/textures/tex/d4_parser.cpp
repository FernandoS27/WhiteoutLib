// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file d4_parser.cpp
/// @brief Diablo IV TEX parser — reads SNO metadata + external pixel payload.

#include <whiteout/textures/tex/parser.h>

#include <algorithm>
#include <cstring>

#include <whiteout/common_types.h>
#include <whiteout/sno/sno_reader.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>
#include <whiteout/textures/texture.h>

#include "../io_helpers.h"
#include "../issue_sink.h"
#include "tex_internal.h"

namespace whiteout::textures::tex {

// ============================================================================
// Helpers — safe field extraction from SnoValue tree
// ============================================================================

/// Read a u32 from any integer-like SnoValue.
static u32 val_u32(const sno::SnoValue* v, u32 fallback = 0) {
    if (!v)
        return fallback;
    switch (v->type()) {
    case sno::SVT_INT:
        return static_cast<u32>(v->asInt());
    case sno::SVT_UINT:
        return v->asUint();
    case sno::SVT_BYTE:
        return v->asByte();
    case sno::SVT_WORD:
        return v->asWord();
    default:
        return fallback;
    }
}

[[maybe_unused]] static i32 val_i32(const sno::SnoValue* v, i32 fallback = 0) {
    if (!v)
        return fallback;
    switch (v->type()) {
    case sno::SVT_INT:
        return v->asInt();
    case sno::SVT_UINT:
        return static_cast<i32>(v->asUint());
    case sno::SVT_BYTE:
        return static_cast<i32>(v->asByte());
    case sno::SVT_WORD:
        return static_cast<i32>(v->asWord());
    default:
        return fallback;
    }
}

static f32 val_f32(const sno::SnoValue* v, f32 fallback = 0.0f) {
    if (!v)
        return fallback;
    if (v->isFloat())
        return v->asFloat();
    return fallback;
}

// ============================================================================
// serTex entry (offset + sizeAndFlags)
// ============================================================================

struct SerTexEntry {
    u32 offset;
    u32 sizeAndFlags;
};

static std::vector<SerTexEntry> extract_ser_tex(const sno::SnoValue& root) {
    std::vector<SerTexEntry> entries;
    const auto* arr = root.field("serTex");
    if (!arr || !arr->isArray())
        return entries;

    const auto& sa = arr->asArray();
    if (sa.isArray()) {
        // Generic array of SnoValue objects
        for (size_t i = 0; i < sa.size(); ++i) {
            const auto& elem = sa.asValueData()[i];
            if (!elem.isObject())
                continue;
            SerTexEntry e{};
            e.offset = val_u32(elem.field("dwOffset"));
            e.sizeAndFlags = val_u32(elem.field("dwSizeAndFlags"));
            entries.push_back(e);
        }
    }
    return entries;
}

// ============================================================================
// Frame extraction
// ============================================================================

static std::vector<D4TexFrame> extract_frames(const sno::SnoValue& root) {
    std::vector<D4TexFrame> frames;
    const auto* arr = root.field("ptFrame");
    if (!arr || !arr->isArray())
        return frames;

    const auto& sa = arr->asArray();
    if (!sa.isArray())
        return frames;

    for (size_t i = 0; i < sa.size(); ++i) {
        const auto& elem = sa.asValueData()[i];
        if (!elem.isObject())
            continue;
        D4TexFrame f{};
        f.imageHandle = val_u32(elem.field("hImageHandle"));
        f.u0 = val_f32(elem.field("flU0"));
        f.v0 = val_f32(elem.field("flV0"));
        f.u1 = val_f32(elem.field("flU1"));
        f.v1 = val_f32(elem.field("flV1"));
        f.trimU0 = val_f32(elem.field("flTrimU0"));
        f.trimV0 = val_f32(elem.field("flTrimV0"));
        f.trimU1 = val_f32(elem.field("flTrimU1"));
        f.trimV1 = val_f32(elem.field("flTrimV1"));
        frames.push_back(f);
    }
    return frames;
}

// ============================================================================
// avgColor extraction
// ============================================================================

static std::array<f32, 4> extract_avg_color(const sno::SnoValue& root) {
    std::array<f32, 4> c{0.0f, 0.0f, 0.0f, 0.0f};
    const auto* val = root.field("rgbavalAvgColor");
    if (!val || !val->isObject())
        return c;
    c[0] = val_f32(val->field("r"));
    c[1] = val_f32(val->field("g"));
    c[2] = val_f32(val->field("b"));
    c[3] = val_f32(val->field("a"));
    return c;
}

// ============================================================================
// RGBA16F → RGBA32F conversion
// ============================================================================

static void convert_rgba16f_to_rgba32f(const u8* src, u8* dst, u32 width, u32 height) {
    const u64 pixel_count = static_cast<u64>(width) * height;
    const u16* in = reinterpret_cast<const u16*>(src);
    f32* out = reinterpret_cast<f32*>(dst);

    for (u64 i = 0; i < pixel_count; ++i) {
        for (u32 ch = 0; ch < 4; ++ch) {
            f16 half;
            half.raw = in[i * 4 + ch];
            out[i * 4 + ch] = half.to_float();
        }
    }
}

// ============================================================================
// Row-aligned copy (strips 256-byte row alignment padding)
// ============================================================================

static void copy_mip_stripping_alignment(const u8* src, u8* dst, u32 d4_fmt, u32 width,
                                         u32 height) {
    auto mapping = d4_tex_format_to_pixel_format(d4_fmt);
    if (!mapping)
        return;

    u32 rows;
    u32 row_bytes;
    if (mapping->block_dim > 1) {
        const u32 bw = std::max(1u, (width + mapping->block_dim - 1) / mapping->block_dim);
        rows = std::max(1u, (height + mapping->block_dim - 1) / mapping->block_dim);
        row_bytes = bw * mapping->bytes_per_unit;
    } else {
        rows = height;
        row_bytes = width * mapping->bytes_per_unit;
    }

    const u32 aligned_pitch = align_up(row_bytes, D4_ROW_ALIGNMENT);

    if (aligned_pitch == row_bytes) {
        // No padding — straight copy
        std::memcpy(dst, src, static_cast<size_t>(row_bytes) * rows);
    } else {
        for (u32 r = 0; r < rows; ++r) {
            std::memcpy(dst + static_cast<size_t>(r) * row_bytes,
                        src + static_cast<size_t>(r) * aligned_pitch, row_bytes);
        }
    }
}

// ============================================================================
// Core D4 parse implementation
// ============================================================================

std::optional<Texture> parseD4Impl(std::span<const u8> texData, std::span<const u8> payloadData,
                                   std::span<const u8> lowResPayloadData, D4TexInfo* outInfo,
                                   IssueSink& sink) {
    const bool has_lowres = !lowResPayloadData.empty();
    // ---- Parse SNO metadata ------------------------------------------------
    sno::SnoReader reader;
    auto snoFile = reader.parse(texData, sno::SnoGroup::Texture);
    if (!snoFile) {
        sink.fail("Failed to parse D4 TEX SNO structure");
        return std::nullopt;
    }

    const auto& root = snoFile->root;
    if (!root.isObject()) {
        sink.fail("D4 TEX root is not an object");
        return std::nullopt;
    }

    // ---- Extract fields ----------------------------------------------------
    const u32 tex_fmt = val_u32(root.field("eTexFormat"));
    const u32 full_width = val_u32(root.field("dwWidth"));
    const u32 full_height = val_u32(root.field("dwHeight"));
    const u32 depth = val_u32(root.field("dwDepth"), 1);
    const u32 face_count = val_u32(root.field("dwFaceCount"), 1);
    const u32 mip_min = val_u32(root.field("dwMipMapLevelMin"));
    const u32 mip_max = val_u32(root.field("dwMipMapLevelMax"));
    const u32 import_flags = val_u32(root.field("dwImportFlags"));
    const u32 tex_res_type = val_u32(root.field("eTextureResourceType"));

    if (full_width == 0 || full_height == 0) {
        sink.fail("D4 TEX has zero dimensions");
        return std::nullopt;
    }

    // ---- Map format --------------------------------------------------------
    auto mapping = d4_tex_format_to_pixel_format(tex_fmt);
    if (!mapping) {
        sink.fail("Unsupported D4 TEX format: " + std::to_string(tex_fmt));
        return std::nullopt;
    }

    const bool is_rgba16f = (tex_fmt == D4_TEX_FMT_RGBA16F);
    const PixelFormat pixel_fmt = mapping->format; // RGBA32F for RGBA16F
    const bool is_cubemap = (face_count == 6);

    // ---- Parse serTex entries ----------------------------------------------
    auto ser_tex = extract_ser_tex(root);
    if (ser_tex.empty()) {
        sink.fail("D4 TEX has no serTex entries");
        return std::nullopt;
    }

    // ---- Detect hi-res mip count -----------------------------------------
    // For cubemaps, serTex contains (mipCount × faceCount) entries in
    // face-major order: all mips for face 0, then all mips for face 1, etc.
    const u32 entries_per_face = is_cubemap ? static_cast<u32>(ser_tex.size()) / face_count
                                            : static_cast<u32>(ser_tex.size());

    // The payload is always the hi-res payload.  In two-tier textures the
    // serTex array contains hi-res entries followed by low-res entries whose
    // offset resets to 0.  We only read entries before that reset.
    bool is_two_tier = false;
    u32 hires_mip_count = entries_per_face;
    for (u32 i = 1; i < entries_per_face; ++i) {
        if (ser_tex[i].offset == 0) {
            hires_mip_count = i;
            is_two_tier = true;
            break;
        }
    }

    if (hires_mip_count == 0) {
        sink.fail("D4 TEX has no hi-res mip levels");
        return std::nullopt;
    }

    // Low-res mip count: the remaining entries after the hi-res split.
    const u32 lowres_mip_count =
        (is_two_tier && has_lowres) ? (entries_per_face - hires_mip_count) : 0;
    const u32 total_mip_count = hires_mip_count + lowres_mip_count;

    // ---- Create texture ----------------------------------------------------
    Texture result;
    if (is_cubemap) {
        result = Texture::createCube(pixel_fmt, full_width, total_mip_count);
    } else {
        result = Texture::create2D(pixel_fmt, full_width, full_height, total_mip_count);
    }

    if (mapping->is_srgb) {
        result.setSrgb(true);
    }

    // ---- Helper: decode one mip from a payload ----------------------------
    auto decode_mip = [&](u32 mip_idx, u32 ser_idx, u32 face, std::span<const u8> payload,
                          const char* tier_name) -> bool {
        const u32 mip_width = std::max(full_width >> mip_idx, 1u);
        const u32 mip_height = std::max(full_height >> mip_idx, 1u);

        if (ser_idx >= ser_tex.size()) {
            sink.fail("D4 TEX serTex index out of range for " + std::string(tier_name) + " mip " +
                      std::to_string(mip_idx));
            return false;
        }

        const u32 payload_offset = ser_tex[ser_idx].offset;
        const u32 payload_size = ser_tex[ser_idx].sizeAndFlags;

        if (payload_size == 0)
            return true; // zero-size padding entry (cubemap tail)

        const u64 payload_end = static_cast<u64>(payload_offset) + payload_size;

        if (payload_end > payload.size()) {
            sink.fail("D4 TEX " + std::string(tier_name) + " payload data out of bounds at mip " +
                      std::to_string(mip_idx) + " (offset " + std::to_string(payload_offset) +
                      " + size " + std::to_string(payload_size) + " > " +
                      std::to_string(payload.size()) + ")");
            return false;
        }

        auto dest = result.mipData(mip_idx, face);
        const u8* src = payload.data() + payload_offset;

        if (is_rgba16f) {
            const u64 src_raw_bytes = static_cast<u64>(mip_width) * mip_height * 8;
            std::vector<u8> temp(src_raw_bytes);
            copy_mip_stripping_alignment(src, temp.data(), tex_fmt, mip_width, mip_height);
            convert_rgba16f_to_rgba32f(temp.data(), dest.data(), mip_width, mip_height);
        } else {
            copy_mip_stripping_alignment(src, dest.data(), tex_fmt, mip_width, mip_height);
        }
        return true;
    };

    // ---- Copy hi-res pixel data --------------------------------------------
    for (u32 face = 0; face < (is_cubemap ? face_count : 1u); ++face) {
        for (u32 mip = 0; mip < hires_mip_count; ++mip) {
            const u32 ser_idx = is_cubemap ? face * entries_per_face + mip : mip;
            if (!decode_mip(mip, ser_idx, face, payloadData, "hi-res"))
                return std::nullopt;
        }
    }

    // ---- Copy low-res pixel data (when provided) ---------------------------
    for (u32 face = 0; face < (is_cubemap ? face_count : 1u); ++face) {
        for (u32 lr = 0; lr < lowres_mip_count; ++lr) {
            const u32 mip_idx = hires_mip_count + lr;
            const u32 ser_idx =
                is_cubemap ? face * entries_per_face + hires_mip_count + lr : hires_mip_count + lr;
            if (!decode_mip(mip_idx, ser_idx, face, lowResPayloadData, "low-res"))
                return std::nullopt;
        }
    }

    // ---- Fill output metadata ----------------------------------------------
    if (outInfo) {
        outInfo->snoId = snoFile->snoId;
        outInfo->texFormat = tex_fmt;
        outInfo->width = full_width;
        outInfo->height = full_height;
        outInfo->depth = depth;
        outInfo->faceCount = face_count;
        outInfo->mipMapLevelMin = mip_min;
        outInfo->mipMapLevelMax = mip_max;
        outInfo->importFlags = import_flags;
        outInfo->textureResourceType = tex_res_type;
        outInfo->avgColor = extract_avg_color(root);
        outInfo->frames = extract_frames(root);
        outInfo->isTwoTier = is_two_tier;
        outInfo->hiResMipCount = hires_mip_count;
        outInfo->lowResMipCount = lowres_mip_count;
    }

    return result;
}

} // namespace whiteout::textures::tex
