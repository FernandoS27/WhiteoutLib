// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bcn.cpp
/// @brief Unified BCn encode/decode dispatcher implementation.

#include "bcn.h"

#include "bcn/bc1.h"
#include "bcn/bc2.h"
#include "bcn/bc3.h"
#include "bcn/bc4.h"
#include "bcn/bc5.h"
#include "bcn/bc6h.h"
#include "bcn/bc7.h"

namespace whiteout::textures::bcn {

// ============================================================================
// encode
// ============================================================================

std::optional<Texture> encode(const Texture& src, PixelFormat target, std::string* out_error) {
    if (!isCompressed(target)) {
        if (out_error)
            *out_error = "bcn::encode: target is not a BCn format";
        return std::nullopt;
    }

    if (!can_encode(src.format(), target)) {
        if (out_error)
            *out_error = "bcn::encode: source format is not compatible with target";
        return std::nullopt;
    }

    switch (target) {
    case PixelFormat::BC1:
        return bc1::encodeTexture(src, /*alpha=*/false, out_error);
    case PixelFormat::BC2:
        return bc2::encodeTexture(src, out_error);
    case PixelFormat::BC3:
        return bc3::encodeTexture(src, out_error);
    case PixelFormat::BC4:
        return bc4::encodeTexture(src, bc4::Channel::R, out_error);
    case PixelFormat::BC5:
        return bc5::encodeTexture(src, bc5::ChannelPair::RG, out_error);
    case PixelFormat::BC6H:
        return bc6h::encodeTexture(src, out_error);
    case PixelFormat::BC7:
        return bc7::encodeTexture(src, bc7::Quality::High, out_error);
    default:
        if (out_error)
            *out_error = "bcn::encode: unsupported target format";
        return std::nullopt;
    }
}

// ============================================================================
// decode
// ============================================================================

std::optional<Texture> decode(const Texture& src, std::string* out_error) {
    if (!isCompressed(src.format())) {
        if (out_error)
            *out_error = "bcn::decode: source is not a BCn format";
        return std::nullopt;
    }

    switch (src.format()) {
    case PixelFormat::BC1:
        return bc1::decodeTexture(src, out_error);
    case PixelFormat::BC2:
        return bc2::decodeTexture(src, out_error);
    case PixelFormat::BC3:
        return bc3::decodeTexture(src, out_error);
    case PixelFormat::BC4:
        return bc4::decodeTexture(src, out_error);
    case PixelFormat::BC5:
        return bc5::decodeTexture(src, out_error);
    case PixelFormat::BC6H:
        return bc6h::decodeTexture(src, out_error);
    case PixelFormat::BC7:
        return bc7::decodeTexture(src, out_error);
    default:
        if (out_error)
            *out_error = "bcn::decode: unsupported source format";
        return std::nullopt;
    }
}

} // namespace whiteout::textures::bcn
