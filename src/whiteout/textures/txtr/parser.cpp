// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/txtr/parser.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../io_helpers.h"
#include "../issue_sink.h"

#include "txtr_internal.h"

namespace whiteout::textures::txtr {

namespace {

/// One contiguous run of mip levels and the bytes that carry it. The header's
/// inline block and every `04D` file both reduce to this.
struct PayloadView {
    u32 firstMip = 0;
    u32 mipCount = 0;
    std::span<const u8> data;
};

std::string hex8(u64 value) {
    static const char* digits = "0123456789ABCDEF";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i, value >>= 4)
        out[static_cast<size_t>(i)] = digits[value & 0xF];
    return out;
}

/// Surface geometry after the flags have been resolved.
struct Geometry {
    u32 width = 0;
    u32 height = 0;
    u32 depth = 1;
    u32 arraySize = 1;
    u32 faceCount = 1;
};

Geometry txtr_geometry(const Header& h, TxtrDimension dim) {
    Geometry g;
    g.width = h.width;
    g.height = h.height;

    switch (dim) {
    case TxtrDimension::Texture1D:
        g.height = 1;
        break;
    case TxtrDimension::Texture1DArray:
        g.height = 1;
        g.arraySize = h.surfaces;
        break;
    case TxtrDimension::Texture2D:
        break;
    case TxtrDimension::Texture2DArray:
        g.arraySize = h.surfaces;
        break;
    case TxtrDimension::Texture3D:
        g.depth = h.surfaces;
        break;
    case TxtrDimension::TextureCube:
        g.height = h.width; // The client forces a cube square.
        g.faceCount = 6;
        break;
    case TxtrDimension::TextureCubeArray:
        g.height = h.width;
        g.faceCount = 6;
        g.arraySize = h.surfaces / 6;
        break;
    }
    return g;
}

} // namespace

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> header,
                                 std::span<const std::span<const u8>> payloads, TxtrInfo* outInfo);

private:
    /// Turn one `04D` buffer into a view, or report why it cannot be used.
    std::optional<PayloadView> readPayload(std::span<const u8> buffer, u32 mipCount,
                                           const char* what);
};

std::optional<PayloadView> Parser::Impl::readPayload(std::span<const u8> buffer, u32 mipCount,
                                                     const char* what) {
    if (buffer.size() < kPayloadPrefixSize) {
        fail(std::string(what) + " is too small for its 16-byte prefix");
        return std::nullopt;
    }

    PayloadPrefix prefix{};
    std::memcpy(&prefix, buffer.data(), sizeof(prefix));

    if (prefix.mipsFromSmallest >= mipCount) {
        fail(std::string(what) + " starts beyond the mip chain (mipsFromSmallest " +
             std::to_string(prefix.mipsFromSmallest) + ", mip count " + std::to_string(mipCount) +
             ")");
        return std::nullopt;
    }

    PayloadView view;
    view.firstMip = mipCount - prefix.mipsFromSmallest - 1;
    view.mipCount = prefix.mipCount;

    if (view.mipCount == 0) {
        fail(std::string(what) + " declares no mip levels");
        return std::nullopt;
    }
    if (view.firstMip + view.mipCount > mipCount) {
        fail(std::string(what) + " covers mips " + std::to_string(view.firstMip) + ".." +
             std::to_string(view.firstMip + view.mipCount - 1) + ", past the chain end");
        return std::nullopt;
    }
    if (static_cast<u64>(prefix.dataOffset) + prefix.dataSize > buffer.size()) {
        fail(std::string(what) + " pixel data runs past the end of the file");
        return std::nullopt;
    }

    view.data = buffer.subspan(prefix.dataOffset, prefix.dataSize);
    return view;
}

std::optional<Texture> Parser::Impl::parse(std::span<const u8> headerBuffer,
                                           std::span<const std::span<const u8>> payloads,
                                           TxtrInfo* outInfo) {
    issues.clear();

    const auto header = txtr_read_header(headerBuffer);
    if (!header) {
        fail("TXTR header too small (need " + std::to_string(TXTR_HEADER_SIZE) + " bytes, got " +
             std::to_string(headerBuffer.size()) + ")");
        return std::nullopt;
    }
    const Header& h = *header;

    if (h.format > TXTR_MAX_FORMAT_CODE) {
        fail("TXTR surface format code " + std::to_string(h.format) + " is out of range");
        return std::nullopt;
    }
    if (h.mipCount == 0 || h.mipCount > TXTR_MAX_MIP_COUNT) {
        fail("TXTR mip count " + std::to_string(h.mipCount) + " is out of range");
        return std::nullopt;
    }
    if (h.width == 0 || h.height == 0) {
        fail("TXTR has zero dimensions");
        return std::nullopt;
    }
    if (h.payloadCount > kMaxPayloadCount) {
        fail("TXTR payload count " + std::to_string(h.payloadCount) + " exceeds " +
             std::to_string(kMaxPayloadCount));
        return std::nullopt;
    }
    if (h.headerMipCount > h.mipCount) {
        fail("TXTR header mip count " + std::to_string(h.headerMipCount) +
             " exceeds the chain length " + std::to_string(h.mipCount));
        return std::nullopt;
    }

    const auto dimension = txtr_dimension(h.flags);
    if (!dimension) {
        fail("TXTR flags 0x" + hex8(h.flags).substr(12) +
             " name no surface dimension (expected exactly one of 1D/2D/3D/Cube)");
        return std::nullopt;
    }

    if (hasFlag(h.flags, TxtrFlags::PlatformTiled)) {
        fail("TXTR pixel data is in a platform-specific tiled layout (flag 0x800), which this "
             "parser cannot detile");
        return std::nullopt;
    }

    const auto formatInfo = txtr_format_info(h.format);
    const auto mapping = txtr_format_mapping(h.format);
    if (!formatInfo || !mapping) {
        fail("Unsupported TXTR surface format code " + std::to_string(h.format) + " (DXGI " +
             std::to_string(dxgiFormatFor(h.format)) + ")");
        return std::nullopt;
    }

    const Geometry geom = txtr_geometry(h, *dimension);
    if (geom.arraySize == 0) {
        fail("TXTR declares " + std::to_string(h.surfaces) +
             " surfaces, too few for its array dimension");
        return std::nullopt;
    }

    // -- Gather every run of mips we were given -----------------------------

    std::vector<PayloadView> views;

    // The block runs to the end of the header: the field at 0x0C is the whole
    // texture's resident byte count, not this block's size, so it overshoots
    // once any mip lives in a file of its own. The prefix carries the extent.
    const std::span<const u8> inlineBlock = headerBuffer.subspan(kInlineDataOffset);

    if (h.payloadCount == 0) {
        // Self-contained texture: the block is raw mip data with no prefix.
        views.push_back(PayloadView{0, h.mipCount, inlineBlock});
    } else if (!inlineBlock.empty()) {
        auto view = readPayload(inlineBlock, h.mipCount, "TXTR inline block");
        if (!view)
            return std::nullopt;
        views.push_back(*view);
    }

    u32 supplied = 0;
    for (const auto& buffer : payloads) {
        if (buffer.empty())
            continue;
        auto view = readPayload(buffer, h.mipCount, "TXTR payload");
        if (!view)
            return std::nullopt;
        views.push_back(*view);
        ++supplied;
    }

    const u32 expectedExternal = h.payloadCount > 0 ? h.payloadCount - 1 : 0;
    const u32 missing = supplied < expectedExternal ? expectedExternal - supplied : 0;

    // -- Work out which mips that actually covers ---------------------------

    std::vector<bool> covered(h.mipCount, false);
    for (const auto& view : views) {
        for (u32 mip = view.firstMip; mip < view.firstMip + view.mipCount; ++mip) {
            if (covered[mip])
                fail("TXTR mip " + std::to_string(mip) + " is carried by more than one payload");
            covered[mip] = true;
        }
    }

    u32 baseMip = h.mipCount;
    for (u32 mip = 0; mip < h.mipCount; ++mip) {
        if (covered[mip]) {
            baseMip = mip;
            break;
        }
    }
    if (baseMip == h.mipCount) {
        fail("TXTR has no pixel data: no payload covered any mip level");
        return std::nullopt;
    }
    for (u32 mip = baseMip; mip < h.mipCount; ++mip) {
        if (!covered[mip]) {
            fail("TXTR mip " + std::to_string(mip) +
                 " is missing; the supplied payloads leave a gap in the chain");
            return std::nullopt;
        }
    }

    const u32 storedMips = h.mipCount - baseMip;
    const u32 baseWidth = std::max(geom.width >> baseMip, 1u);
    const u32 baseHeight = std::max(geom.height >> baseMip, 1u);
    const u32 baseDepth = std::max(geom.depth >> baseMip, 1u);

    // -- Allocate and fill --------------------------------------------------

    Texture result;
    switch (*dimension) {
    case TxtrDimension::Texture1D:
    case TxtrDimension::Texture2D:
        result = Texture::create2D(mapping->format, baseWidth, baseHeight, storedMips);
        break;
    case TxtrDimension::Texture1DArray:
    case TxtrDimension::Texture2DArray:
        result = Texture::create2DArray(mapping->format, baseWidth, baseHeight, geom.arraySize,
                                        storedMips);
        break;
    case TxtrDimension::Texture3D:
        result = Texture::create3D(mapping->format, baseWidth, baseHeight, baseDepth, storedMips);
        break;
    case TxtrDimension::TextureCube:
        result = Texture::createCube(mapping->format, baseWidth, storedMips);
        break;
    case TxtrDimension::TextureCubeArray:
        result = Texture::createCubeArray(mapping->format, baseWidth, geom.arraySize, storedMips);
        break;
    }
    result.setSrgb(mapping->isSrgb);

    for (const auto& view : views) {
        u64 cursor = 0;
        // The client walks a payload slice-major, then face, then mip, which is
        // the order the bytes sit in the file.
        for (u32 slice = 0; slice < geom.arraySize; ++slice) {
            for (u32 face = 0; face < geom.faceCount; ++face) {
                const u32 layer = slice * geom.faceCount + face;
                for (u32 mip = view.firstMip; mip < view.firstMip + view.mipCount; ++mip) {
                    const u32 level = mip - baseMip;
                    const MipInfo mi =
                        txtr_mip_info(*formatInfo, baseWidth, baseHeight, baseDepth, level);

                    auto dst = result.mipData(level, layer);
                    if (dst.size() != mi.size) {
                        fail("TXTR mip " + std::to_string(mip) + " size mismatch (container " +
                             std::to_string(mi.size) + ", texture " + std::to_string(dst.size()) +
                             ")");
                        return std::nullopt;
                    }
                    if (cursor + mi.size > view.data.size()) {
                        fail("TXTR payload covering mips " + std::to_string(view.firstMip) + ".." +
                             std::to_string(view.firstMip + view.mipCount - 1) + " is short by " +
                             std::to_string(cursor + mi.size - view.data.size()) + " bytes");
                        return std::nullopt;
                    }

                    std::memcpy(dst.data(), view.data.data() + cursor,
                                static_cast<size_t>(mi.size));
                    if (mapping->isBgra)
                        txtr_swizzle_bgra(dst.data(), dst.size() / 4, mapping->forceOpaque);
                    cursor += mi.size;
                }
            }
        }
        if (cursor != view.data.size()) {
            issues.push_back("TXTR payload covering mips " + std::to_string(view.firstMip) + ".." +
                             std::to_string(view.firstMip + view.mipCount - 1) + " has " +
                             std::to_string(view.data.size() - cursor) + " trailing bytes");
        }
    }

    if (missing > 0) {
        issues.push_back("TXTR is missing " + std::to_string(missing) + " of its " +
                         std::to_string(expectedExternal) + " payload files; decoded from mip " +
                         std::to_string(baseMip) + " down");
    }

    if (outInfo) {
        outInfo->flags = h.flags;
        outInfo->dimension = *dimension;
        outInfo->formatCode = h.format;
        outInfo->dxgiFormat = dxgiFormatFor(h.format);
        outInfo->pixelFormat = mapping->format;
        outInfo->isSrgb = mapping->isSrgb;
        outInfo->width = geom.width;
        outInfo->height = geom.height;
        outInfo->surfaces = h.surfaces;
        outInfo->depth = geom.depth;
        outInfo->arraySize = geom.arraySize;
        outInfo->faceCount = geom.faceCount;
        outInfo->mipCount = h.mipCount;
        outInfo->payloadCount = h.payloadCount;
        outInfo->headerMipCount = h.headerMipCount;
        outInfo->inlineSize = h.inlineSize;
        outInfo->streamMask = h.streamMask;
        outInfo->unknown05 = h.unknown05;
        outInfo->unknown07 = h.unknown07;
        outInfo->unknown12 = h.unknown12;
        outInfo->baseMip = baseMip;
        outInfo->missingPayloads = missing;
    }

    return result;
}

// ============================================================================
// Parser
// ============================================================================

Parser::Parser() : pImpl(std::make_unique<Impl>()) {}

Parser::~Parser() = default;

std::optional<Texture> Parser::parse(const std::string& filePath) {
    pImpl->issues.clear();
    auto buf = read_file_bytes(filePath, *pImpl);
    if (!buf)
        return std::nullopt;
    return pImpl->parse(std::span<const u8>{*buf}, {}, nullptr);
}

std::optional<Texture> Parser::parse(std::span<const u8> header) {
    return pImpl->parse(header, {}, nullptr);
}

std::optional<Texture> Parser::parse(std::span<const u8> header, TxtrInfo* outInfo) {
    return pImpl->parse(header, {}, outInfo);
}

std::optional<Texture> Parser::parse(std::span<const u8> header,
                                     std::span<const std::span<const u8>> payloads,
                                     TxtrInfo* outInfo) {
    return pImpl->parse(header, payloads, outInfo);
}

std::optional<Texture> Parser::parse(const std::string& headerPath,
                                     const std::vector<std::string>& payloadPaths,
                                     TxtrInfo* outInfo) {
    pImpl->issues.clear();

    auto headerBytes = read_file_bytes(headerPath, *pImpl);
    if (!headerBytes)
        return std::nullopt;

    std::vector<std::vector<u8>> owned;
    owned.reserve(payloadPaths.size());
    for (const auto& path : payloadPaths) {
        auto bytes = read_file_bytes(path, *pImpl);
        if (!bytes)
            return std::nullopt;
        owned.push_back(std::move(*bytes));
    }

    std::vector<std::span<const u8>> views;
    views.reserve(owned.size());
    for (const auto& buffer : owned)
        views.emplace_back(buffer);

    return pImpl->parse(std::span<const u8>{*headerBytes}, views, outInfo);
}

u32 Parser::payloadCount(std::span<const u8> header) {
    const auto h = txtr_read_header(header);
    if (!h)
        return 0;
    return h->payloadCount;
}

std::vector<u64> Parser::payloadGuids(std::span<const u8> header, u64 textureGuid) {
    std::vector<u64> guids;
    const u32 count = payloadCount(header);
    // Payload 0 is the header's own inline block and has no file of its own.
    for (u32 index = 1; index < count; ++index)
        guids.push_back(makePayloadGuid(textureGuid, index));
    return guids;
}

bool Parser::detect(std::span<const u8> buffer) const {
    return looks_like_txtr(buffer);
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::txtr
