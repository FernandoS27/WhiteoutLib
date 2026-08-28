// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file d4_parser.cpp
/// @brief Diablo IV TEX parser — reads SNO metadata + external pixel payload.
///
/// The rules encoded here were read out of
/// (build 3.1.1.72836); see docs/D4 Specs/TEX_ENGINE_NOTES.md, which cites the
/// function for each one.

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

static i32 val_i32(const sno::SnoValue* v, i32 fallback = 0) {
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
// Raw header access
// ============================================================================

/// Little-endian u32 straight out of a byte span, without host-endianness
/// assumptions.  Returns 0 when the span is too short.
static u32 le32(std::span<const u8> data, size_t offset) {
    if (offset + 4 > data.size())
        return 0;
    return static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8) |
           (static_cast<u32>(data[offset + 2]) << 16) | (static_cast<u32>(data[offset + 3]) << 24);
}

/// The record header flags dword, at record offset 4 (file offset 0x14).
///
/// `SnoReader` never surfaces this: the loader overwrites the slot at runtime,
/// so the type registry does not model it.  On disk it carries the baker's
/// flags, and bits 26/27/28 are the payload-tier bits the engine reads in
/// `SnoTexture_SelectPayloadTier` (0x140877E60).
static u32 read_record_flags(std::span<const u8> texData) {
    return le32(texData, D4_REC_FLAGS_OFFSET);
}

/// Skip the 16-byte SNO header if the payload buffer still carries one.
///
/// Payload files are SNO files too — `SNOFile_OpenPayload` (0x1416CC000)
/// validates a `{0xDEADBEEF, formatHash, 0, 0}` header on them, and every
/// engine read goes through the header-stripped stream, so `serTex[].dwOffset`
/// is relative to payload byte 16.  Extractors differ on whether they keep the
/// header, so detect it rather than assuming either way.  The format-hash gate
/// is the same one `SnoReader::parse` uses (D4 hashes start at 419,536), which
/// keeps a pixel run that happens to begin with 0xDEADBEEF from being mistaken
/// for a header.
static std::span<const u8> strip_sno_header(std::span<const u8> payload, bool* stripped) {
    if (stripped)
        *stripped = false;
    if (payload.size() < D4_SNO_HEADER_SIZE)
        return payload;
    if (le32(payload, 0) != TEX_MAGIC)
        return payload;
    if (le32(payload, 4) <= 0xFFFFu)
        return payload;
    if (stripped)
        *stripped = true;
    return payload.subspan(D4_SNO_HEADER_SIZE);
}

// ============================================================================
// serTex entry (offset + sizeAndFlags)
// ============================================================================

struct SerTexEntry {
    u32 offset = 0;
    u32 sizeAndFlags = 0;

    /// Byte size of this mip level.  Bit 31 is not part of the size — every
    /// engine read masks it off (`Texture2D_UploadFromPayload` 0x140875490,
    /// `TextureCube_UploadFromPayload` 0x140876640,
    /// `SnoTexture_ComputeStreamingRange` 0x140878030).
    u32 size() const {
        return sizeAndFlags & D4_SER_TEX_SIZE_MASK;
    }
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
// avgColor / hotspot / SH coefficients / UI style preset
// ============================================================================

static std::array<f32, 4> extract_avg_color(const sno::SnoValue& root) {
    std::array<f32, 4> c{0.0f, 0.0f, 0.0f, 0.0f};
    const auto* val = root.field("rgbavalAvgColor");
    if (!val)
        return c;
    if (val->isColorF()) {
        const auto& cf = val->asColorF();
        return {cf.r, cf.g, cf.b, cf.a};
    }
    if (val->isObject()) {
        c[0] = val_f32(val->field("r"));
        c[1] = val_f32(val->field("g"));
        c[2] = val_f32(val->field("b"));
        c[3] = val_f32(val->field("a"));
    }
    return c;
}

/// `pHotspot` — DT_BCVEC2I anchor point.
static std::array<i32, 2> extract_hotspot(const sno::SnoValue& root) {
    std::array<i32, 2> h{0, 0};
    const auto* val = root.field("pHotspot");
    if (!val)
        return h;
    if (val->isIVec2()) {
        const auto& v = val->asIVec2();
        return {v.x, v.y};
    }
    if (val->isObject()) {
        h[0] = val_i32(val->field("x"));
        h[1] = val_i32(val->field("y"));
    }
    return h;
}

/// `ptGCoeffs` — spherical-harmonic coefficients on cubemap light probes.
/// Each `TGCoeffs` holds `coeff`, a DT_FIXEDARRAY[3] of DT_VECTOR4D.
static std::vector<D4TexSHCoeffs> extract_sh_coeffs(const sno::SnoValue& root) {
    std::vector<D4TexSHCoeffs> out;
    const auto* arr = root.field("ptGCoeffs");
    if (!arr || !arr->isArray())
        return out;

    const auto& sa = arr->asArray();
    if (!sa.isArray())
        return out;

    for (size_t i = 0; i < sa.size(); ++i) {
        const auto& elem = sa.asValueData()[i];
        if (!elem.isObject())
            continue;
        const auto* coeff = elem.field("coeff");
        if (!coeff || !coeff->isArray())
            continue;

        D4TexSHCoeffs sh{};
        const auto& ca = coeff->asArray();
        if (ca.isVec4()) {
            const auto& vecs = ca.asVec4Data();
            for (size_t c = 0; c < vecs.size() && c < sh.coeff.size(); ++c)
                sh.coeff[c] = {vecs[c].x, vecs[c].y, vecs[c].z, vecs[c].w};
        } else if (ca.isArray()) {
            const auto& vals = ca.asValueData();
            for (size_t c = 0; c < vals.size() && c < sh.coeff.size(); ++c) {
                if (!vals[c].isVec4())
                    continue;
                const auto& v = vals[c].asVec4();
                sh.coeff[c] = {v.x, v.y, v.z, v.w};
            }
        }
        out.push_back(sh);
    }
    return out;
}

/// `sUIStylePreset` — a DT_SNO reference into group 153 (`Preset`), not a
/// string.  Returns the referenced SNO id, or 0 when unset.
///
/// Read straight out of the record rather than through the SnoValue tree.
/// D3 serializes DT_SNO as `{i32 group, i32 id}` and `SnoReader` reads eight
/// bytes for it, but D4 stores only the id and takes the group from the field
/// def: across the D4 registry, 1,643 of the 1,822 fields that follow a DT_SNO
/// field sit exactly 4 bytes later (`TextureDefinition` itself has
/// `sUIStylePreset` at record+8 and `eTexFormat` at record+12).  Going through
/// the tree here would hand back `eTexFormat` as the preset id.
static u32 extract_ui_style_preset(std::span<const u8> texData) {
    return le32(texData, D4_SNO_HEADER_SIZE + 8);
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

static void convert_d4_mip(D4Conversion conversion, const u8* src, u8* dst, u32 width,
                           u32 height) {
    const u64 pixel_count = static_cast<u64>(width) * height;
    switch (conversion) {
    case D4Conversion::F16ToF32:
        convert_rgba16f_to_rgba32f(src, dst, width, height);
        break;
    case D4Conversion::BGRA8ToRGBA8:
        convert_a8r8g8b8_to_rgba8(src, dst, pixel_count);
        break;
    case D4Conversion::A8ToRGBA8:
        convert_a8_to_rgba8(src, dst, pixel_count);
        break;
    case D4Conversion::None:
        break;
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
    // ---- Record header flags ----------------------------------------------
    // Read before the SNO parse: the type registry does not model record+4,
    // and the loader overwrites that slot at runtime anyway.
    const u32 record_flags = read_record_flags(texData);
    const bool flag_low_payload = (record_flags & D4_REC_FLAG_HAS_LOW_PAYLOAD) != 0;
    const bool flag_med_payload = (record_flags & D4_REC_FLAG_HAS_MED_PAYLOAD) != 0;
    const bool flag_stub = (record_flags & D4_REC_FLAG_STUB) != 0;

    // ---- Payload framing ---------------------------------------------------
    bool hires_header_stripped = false;
    bool lowres_header_stripped = false;
    const std::span<const u8> hiResPayload = strip_sno_header(payloadData, &hires_header_stripped);
    const std::span<const u8> lowResPayload =
        strip_sno_header(lowResPayloadData, &lowres_header_stripped);
    const bool has_lowres = !lowResPayload.empty();

    // ---- Parse SNO metadata ------------------------------------------------
    sno::SnoReader const reader;
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
    const u32 volume_x = val_u32(root.field("dwVolumeXSlices"), 1);
    const u32 volume_y = val_u32(root.field("dwVolumeYSlices"), 1);
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

    const D4Conversion conversion = mapping->conversion;
    const PixelFormat pixel_fmt = mapping->format;
    const bool is_cubemap = (face_count == 6);

    // ---- Parse serTex entries ----------------------------------------------
    auto ser_tex = extract_ser_tex(root);
    if (ser_tex.empty()) {
        sink.fail("D4 TEX has no serTex entries");
        return std::nullopt;
    }

    // ---- How many mip levels per face --------------------------------------
    // Cubemap serTex is 6 faces x a FIXED stride of 11 slots, face-major:
    // `TextureCube_UploadFromPayload` loops `for (base = 0; base < 0x42;
    // base += 11)`.  It is not len(serTex) / dwFaceCount.
    const u32 face_stride = is_cubemap ? D4_CUBE_SERTEX_STRIDE : 0;

    // How many levels each face stores.  `Texture_ComputeMipInfo` derives this
    // from the record's mip range, which is the only workable answer for a
    // cubemap because its array is padded out to 66 fixed slots.  For a 2D
    // texture the array length is the ground truth for how many levels
    // actually carry bytes, and the corpus has it agreeing with the mip range,
    // so prefer it there and never drop a stored level.
    const u32 range_mips = d4_stored_mip_count(tex_fmt, full_width, full_height, mip_min, mip_max);

    u32 entries_per_face;
    if (is_cubemap) {
        entries_per_face = range_mips;
        if (entries_per_face == 0) {
            // Degenerate mip range: count the populated slots of face 0.
            for (u32 i = 0; i < D4_CUBE_SERTEX_STRIDE && i < ser_tex.size(); ++i) {
                if (ser_tex[i].size() == 0)
                    break;
                ++entries_per_face;
            }
        }
    } else {
        entries_per_face = static_cast<u32>(ser_tex.size());
    }
    entries_per_face = std::min(entries_per_face, D4_MAX_MIP_SLOTS);

    if (entries_per_face == 0) {
        sink.fail("D4 TEX has no stored mip levels");
        return std::nullopt;
    }

    // ---- Detect the streaming split ---------------------------------------
    // Authority for *whether* the chain is split is record flag bit 27; the
    // offset reset locates *where*.  Tier 0 normally holds only the
    // full-resolution mip, so fall back to a split after index 0.
    u32 hires_mip_count = entries_per_face;
    bool split_found = false;
    for (u32 i = 1; i < entries_per_face && i < ser_tex.size(); ++i) {
        if (ser_tex[i].offset == 0) {
            hires_mip_count = i;
            split_found = true;
            break;
        }
    }

    bool is_two_tier = split_found;
    if (record_flags != 0) {
        is_two_tier = flag_low_payload;
        if (is_two_tier && !split_found)
            hires_mip_count = std::min(1u, entries_per_face);
        else if (!is_two_tier)
            hires_mip_count = entries_per_face;
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
            // The engine reads a shared {0, 0} dummy for slots past the end of
            // the array, so a short serTex is legal and simply means "no data".
            return true;
        }

        const u32 payload_offset = ser_tex[ser_idx].offset;
        const u32 payload_size = ser_tex[ser_idx].size();

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

        if (conversion != D4Conversion::None) {
            const u64 src_raw_bytes =
                static_cast<u64>(mip_width) * mip_height * mapping->bytes_per_unit;
            std::vector<u8> temp(src_raw_bytes);
            copy_mip_stripping_alignment(src, temp.data(), tex_fmt, mip_width, mip_height);
            convert_d4_mip(conversion, temp.data(), dest.data(), mip_width, mip_height);
        } else {
            copy_mip_stripping_alignment(src, dest.data(), tex_fmt, mip_width, mip_height);
        }
        return true;
    };

    // ---- Copy hi-res pixel data --------------------------------------------
    for (u32 face = 0; face < (is_cubemap ? face_count : 1u); ++face) {
        for (u32 mip = 0; mip < hires_mip_count; ++mip) {
            const u32 ser_idx = is_cubemap ? face * face_stride + mip : mip;
            if (!decode_mip(mip, ser_idx, face, hiResPayload, "hi-res"))
                return std::nullopt;
        }
    }

    // ---- Copy low-res pixel data (when provided) ---------------------------
    for (u32 face = 0; face < (is_cubemap ? face_count : 1u); ++face) {
        for (u32 lr = 0; lr < lowres_mip_count; ++lr) {
            const u32 mip_idx = hires_mip_count + lr;
            const u32 ser_idx = is_cubemap ? face * face_stride + mip_idx : mip_idx;
            if (!decode_mip(mip_idx, ser_idx, face, lowResPayload, "low-res"))
                return std::nullopt;
        }
    }

    // ---- Fill output metadata ----------------------------------------------
    if (outInfo) {
        outInfo->snoId = snoFile->snoId;
        outInfo->recordFlags = record_flags;
        outInfo->uiStylePreset = extract_ui_style_preset(texData);
        outInfo->texFormat = tex_fmt;
        outInfo->width = full_width;
        outInfo->height = full_height;
        outInfo->depth = depth;
        outInfo->volumeXSlices = volume_x;
        outInfo->volumeYSlices = volume_y;
        outInfo->faceCount = face_count;
        outInfo->mipMapLevelMin = mip_min;
        outInfo->mipMapLevelMax = mip_max;
        outInfo->importFlags = import_flags;
        outInfo->textureResourceType = tex_res_type;
        outInfo->avgColor = extract_avg_color(root);
        outInfo->hotspot = extract_hotspot(root);
        outInfo->frames = extract_frames(root);
        outInfo->shCoeffs = extract_sh_coeffs(root);
        outInfo->hasLowPayload = flag_low_payload;
        outInfo->hasMedPayload = flag_med_payload;
        outInfo->isStubRecord = flag_stub;
        outInfo->isTwoTier = is_two_tier;
        outInfo->hiResMipCount = hires_mip_count;
        outInfo->lowResMipCount = lowres_mip_count;
        outInfo->payloadHeaderStripped = hires_header_stripped || lowres_header_stripped;
    }

    return result;
}

} // namespace whiteout::textures::tex
