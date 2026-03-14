// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/tex/parser.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "../io_helpers.h"
#include "../issue_sink.h"

#include "tex_internal.h"

namespace whiteout::textures::tex {

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> buffer, TexInfo* outInfo);

private:
    std::span<const u8> buffer_;
};

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer, TexInfo* outInfo) {
    issues.clear();
    buffer_ = buffer;

    if (buffer_.size() < MIN_HEADER_SIZE) {
        fail("TEX file too small for header");
        return std::nullopt;
    }

    SnoPreamble preamble{};
    std::memcpy(&preamble, buffer_.data() + OFF_PREAMBLE, sizeof(SnoPreamble));

    if (preamble.magic != TEX_MAGIC) {
        fail("Invalid TEX magic (expected 0xDEADBEEF, got 0x" + [&] {
            std::array<char, 16> buf;
            std::snprintf(buf.data(), buf.size(), "%08X", preamble.magic);
            return std::string(buf.data());
        }() + ")");
        return std::nullopt;
    }

    if (preamble.version != TEX_VERSION) {
        fail("Unsupported TEX version: " + std::to_string(preamble.version) + " (expected " +
             std::to_string(TEX_VERSION) + ")");
        return std::nullopt;
    }

    TextureDescriptor texture_desc{};
    std::memcpy(&texture_desc, buffer_.data() + OFF_TEXTURE_DESC, sizeof(TextureDescriptor));

    if (texture_desc.width == 0 || texture_desc.height == 0) {
        fail("TEX texture has zero dimensions");
        return std::nullopt;
    }

    if (texture_desc.depth != 1 && texture_desc.depth != 6) {
        fail("Unsupported TEX depth: " + std::to_string(texture_desc.depth) + " (expected 1 or 6)");
        return std::nullopt;
    }

    const bool is_cubemap = (texture_desc.depth == 6);

    auto mapping = tex_format_to_pixel_format(texture_desc.pixelFormat);
    if (!mapping) {
        fail("Unsupported TEX pixel format ID: " + std::to_string(texture_desc.pixelFormat));
        return std::nullopt;
    }

    const PixelFormat output_fmt = mapping->format;
    const bool needs_conversion = mapping->needs_conversion;

    if (buffer_.size() < OFF_MIP_TABLE + MIP_TABLE_SIZE) {
        fail("TEX file too small for mip table");
        return std::nullopt;
    }

    std::array<MipEntry, MIP_TABLE_ENTRIES> mip_table{};
    std::memcpy(mip_table.data(), buffer_.data() + OFF_MIP_TABLE, MIP_TABLE_SIZE);

    u32 mipCount = 0;
    for (u32 i = 0; i < MIP_TABLE_ENTRIES; ++i) {
        if (mip_table[i].dataSize == 0) {
            break;
        }
        ++mipCount;
    }

    if (mipCount == 0) {
        fail("TEX file has no mip levels");
        return std::nullopt;
    }

    AtlasMetadata atlas{};
    if (buffer_.size() >= OFF_ATLAS_META + ATLAS_META_SIZE) {
        std::memcpy(&atlas, buffer_.data() + OFF_ATLAS_META, sizeof(AtlasMetadata));
    }

    std::vector<TexFrame> frames;
    if (atlas.frameCount > 0 && atlas.frameTableOffset > 0 && atlas.frameTableSize > 0) {
        const u64 frame_end = static_cast<u64>(atlas.frameTableOffset) +
                              static_cast<u64>(atlas.frameCount) * FRAME_DESC_SIZE;
        if (frame_end <= buffer_.size()) {
            frames.reserve(atlas.frameCount);
            for (u32 i = 0; i < atlas.frameCount; ++i) {
                FrameDescriptorDisk frame_disk{};
                std::memcpy(&frame_disk,
                            buffer_.data() + atlas.frameTableOffset + i * FRAME_DESC_SIZE,
                            sizeof(FrameDescriptorDisk));
                TexFrame frame;
                frame.uMin = frame_disk.uMin;
                frame.vMin = frame_disk.vMin;
                frame.uMax = frame_disk.uMax;
                frame.vMax = frame_disk.vMax;

                frame_disk.name[63] = '\0';
                frame.name = frame_disk.name.data();
                frames.push_back(std::move(frame));
            }
        }
    }

    Texture result;
    if (is_cubemap) {
        if (texture_desc.width != texture_desc.height) {
            fail("Cubemap faces must be square, got " + std::to_string(texture_desc.width) + "x" +
                 std::to_string(texture_desc.height));
            return std::nullopt;
        }
        result = Texture::createCube(output_fmt, texture_desc.width, mipCount);
    } else {
        result = Texture::create2D(output_fmt, texture_desc.width, texture_desc.height, mipCount);
    }

    const bool is_shuffled = is_shuffled_bc_format(texture_desc.pixelFormat);
    const u32 face_count = is_cubemap ? 6u : 1u;

    for (u32 mip = 0; mip < mipCount; ++mip) {
        const u32 mip_width = std::max(texture_desc.width >> mip, 1u);
        const u32 mip_height = std::max(texture_desc.height >> mip, 1u);
        const u32 disk_prefix = is_shuffled ? BC_MIP_PREFIX_SIZE : 0u;
        const u64 face_disk_size = static_cast<u64>(mip_table[mip].dataSize) + disk_prefix;

        if (mip_table[mip].fileOffset + face_disk_size * face_count > buffer_.size()) {
            fail("TEX mip " + std::to_string(mip) + " data out of bounds");
            return std::nullopt;
        }

        for (u32 face = 0; face < face_count; ++face) {
            auto destination = result.mipData(mip, face);

            if (needs_conversion &&
                destination.size() != static_cast<u64>(mip_width) * mip_height * 4) {
                fail("Internal mip size mismatch");
                return std::nullopt;
            }

            const u8* face_data =
                buffer_.data() + mip_table[mip].fileOffset + face_disk_size * face;
            decode_mip_face(texture_desc.pixelFormat, face_data, destination, mip_width, mip_height,
                            mip_table[mip].dataSize, needs_conversion, is_shuffled);
        }
    }

    if (outInfo) {
        outInfo->snoId = preamble.snoId;
        outInfo->flags = static_cast<TexFlags>(texture_desc.flags);
        outInfo->samplerHint1 = atlas.samplerHint1;
        outInfo->samplerHint2 = atlas.samplerHint2;
        outInfo->frames = std::move(frames);
    }

    return result;
}

Parser::Parser(ParseMode parseMode) : pImpl(std::make_unique<Impl>()) {
    pImpl->strict_mode = (parseMode == ParseMode::Strict);
}

Parser::~Parser() = default;

std::optional<Texture> Parser::parse(const std::string& filePath) {
    pImpl->issues.clear();
    auto buf = read_file_bytes(filePath, *pImpl);
    if (!buf) {
        return std::nullopt;
    }
    return pImpl->parse(std::span<const u8>{*buf}, nullptr);
}

std::optional<Texture> Parser::parse(std::span<const u8> buffer) {
    return pImpl->parse(buffer, nullptr);
}

std::optional<Texture> Parser::parse(std::span<const u8> buffer, TexInfo* outInfo) {
    return pImpl->parse(buffer, outInfo);
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::tex
