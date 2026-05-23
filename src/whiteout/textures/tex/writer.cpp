// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/tex/writer.h>

#include "../issue_sink.h"

#include "../io_helpers.h"
#include "../utils/srgb_linearize.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "tex_internal.h"

namespace whiteout::textures::tex {

class Writer::Impl : public IssueSink {
public:
    std::vector<u8> write(const Texture& texture, const SaveOptions& opts);
};

std::vector<u8> Writer::Impl::write(const Texture& texture_in, const SaveOptions& opts) {
    issues.clear();

    // TEX has no sRGB flag — linearize if the source is sRGB.
    Texture linearized;
    const Texture& texture =
        texture_in.isSrgb() ? (linearized = linearizeSrgbCopy(texture_in)) : texture_in;

    if (texture.width() == 0 || texture.height() == 0) {
        fail("Cannot save an empty texture");
        return {};
    }

    if (texture.type() == TextureType::Texture3D) {
        fail("TEX format does not support 3D textures");
        return {};
    }

    auto tex_format_opt = pixel_format_to_tex_format(texture.format());
    if (!tex_format_opt) {
        fail("Pixel format not representable in TEX (only RGBA8, BC1, BC2, BC3, "
             "BC4, BC5 are supported)");
        return {};
    }
    const u32 tex_format = *tex_format_opt;

    const bool is_cubemap = (texture.type() == TextureType::TextureCube);
    const u32 face_count = is_cubemap ? 6u : 1u;
    const u32 mipCount = texture.mipCount();
    const bool is_shuffled = is_shuffled_bc_format(tex_format);

    u32 frame_count = static_cast<u32>(opts.frames.size());
    if (frame_count == 0) {
        frame_count = 1;
    }

    const u32 hash_array_data_size = frame_count * 4;

    const u32 frame_table_offset = align_up(OFF_HASH_ARRAY + hash_array_data_size, 8);
    const u32 frame_table_size = frame_count * FRAME_DESC_SIZE;
    const u32 pixel_data_offset = frame_table_offset + frame_table_size;

    struct MipInfo {
        u32 file_offset;
        u32 per_face_size;
    };
    std::vector<MipInfo> mip_infos(mipCount);

    u32 current_offset = pixel_data_offset;
    for (u32 mip = 0; mip < mipCount; ++mip) {
        const u32 mip_width = std::max(texture.width() >> mip, 1u);
        const u32 mip_height = std::max(texture.height() >> mip, 1u);
        const u32 face_size =
            static_cast<u32>(tex_compute_mip_size(tex_format, mip_width, mip_height));
        const u32 face_disk_size = face_size + (is_shuffled ? BC_MIP_PREFIX_SIZE : 0);

        mip_infos[mip].file_offset = current_offset;
        mip_infos[mip].per_face_size = face_size;
        current_offset += face_disk_size * face_count;
    }

    const u32 total_file_size = current_offset;

    std::vector<u8> output(total_file_size, 0);

    {
        SnoPreamble preamble{};
        preamble.magic = TEX_MAGIC;
        preamble.version = TEX_VERSION;
        std::memset(preamble._reserved08.data(), 0, preamble._reserved08.size());
        preamble.snoId = opts.snoId;
        std::memset(preamble._reserved14.data(), 0, preamble._reserved14.size());
        std::memcpy(output.data() + OFF_PREAMBLE, &preamble, sizeof(SnoPreamble));
    }

    {
        TextureDescriptor texture_desc{};
        texture_desc.pixelFormat = tex_format;
        texture_desc.width = texture.width();
        texture_desc.height = texture.height();
        texture_desc.depth = face_count;
        texture_desc.flags = static_cast<u32>(opts.flags);
        texture_desc.extraMipCount = (mipCount > 0) ? (mipCount - 1) : 0;
        std::memcpy(output.data() + OFF_TEXTURE_DESC, &texture_desc, sizeof(TextureDescriptor));
    }

    {
        std::array<MipEntry, MIP_TABLE_ENTRIES> entries{};
        for (u32 i = 0; i < mipCount && i < MIP_TABLE_ENTRIES; ++i) {
            entries[i].fileOffset = mip_infos[i].file_offset;
            entries[i].dataSize = mip_infos[i].per_face_size;
        }
        std::memcpy(output.data() + OFF_MIP_TABLE, entries.data(), MIP_TABLE_SIZE);
    }

    {
        AtlasMetadata atlas{};
        atlas.frameCount = frame_count;
        atlas.frameTableOffset = frame_table_offset;
        atlas.frameTableSize = frame_table_size;
        std::memset(atlas._reserved224.data(), 0, atlas._reserved224.size());
        atlas.field238 = 0;
        atlas.samplerHint1 = opts.samplerHint1;
        atlas.samplerHint2 = opts.samplerHint2;
        std::memset(atlas._reserved244.data(), 0, atlas._reserved244.size());
        std::memcpy(output.data() + OFF_ATLAS_META, &atlas, sizeof(AtlasMetadata));
    }

    {
        FrameHashArrayHeader hash_header{};
        hash_header.hashArrayOffset = OFF_HASH_ARRAY;
        hash_header.hashArraySize = hash_array_data_size;
        std::memcpy(output.data() + OFF_HASH_HEADER, &hash_header, sizeof(FrameHashArrayHeader));
    }

    for (u32 i = 0; i < frame_count; ++i) {
        const u32 val = i;
        std::memcpy(output.data() + OFF_HASH_ARRAY + i * 4, &val, 4);
    }

    for (u32 i = 0; i < frame_count; ++i) {
        FrameDescriptorDisk frame_disk{};
        std::memset(&frame_disk, 0, sizeof(frame_disk));
        if (i < static_cast<u32>(opts.frames.size())) {
            frame_disk.uMin = opts.frames[i].uMin;
            frame_disk.vMin = opts.frames[i].vMin;
            frame_disk.uMax = opts.frames[i].uMax;
            frame_disk.vMax = opts.frames[i].vMax;
            const size_t name_len = std::min(opts.frames[i].name.size(), size_t{63});
            std::memcpy(frame_disk.name.data(), opts.frames[i].name.c_str(), name_len);
        } else {
            // Default single-frame: full UV rect
            frame_disk.uMin = 0.0f;
            frame_disk.vMin = 0.0f;
            frame_disk.uMax = 1.0f;
            frame_disk.vMax = 1.0f;
        }
        std::memcpy(output.data() + frame_table_offset + i * FRAME_DESC_SIZE, &frame_disk,
                    sizeof(FrameDescriptorDisk));
    }

    const bool needs_swizzle = (tex_format == TEX_FMT_A8R8G8B8);

    for (u32 mip = 0; mip < mipCount; ++mip) {
        const u32 face_data_size = mip_infos[mip].per_face_size;
        const u32 face_disk_size = face_data_size + (is_shuffled ? BC_MIP_PREFIX_SIZE : 0);
        const u32 mip_width = std::max(texture.width() >> mip, 1u);
        const u32 mip_height = std::max(texture.height() >> mip, 1u);

        for (u32 face = 0; face < face_count; ++face) {
            auto source = texture.mipData(mip, face);
            u8* destination = output.data() + mip_infos[mip].file_offset + face_disk_size * face;
            encode_mip_face(tex_format, source, destination, mip_width, mip_height, face_data_size,
                            needs_swizzle, is_shuffled);
        }
    }

    return output;
}

Writer::Writer() : pImpl(std::make_unique<Impl>()) {}

Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const Texture& texture) {
    write(filePath, texture, SaveOptions{});
}

std::vector<u8> Writer::write(const Texture& texture) {
    return write(texture, SaveOptions{});
}

void Writer::write(const std::string& filePath, const Texture& texture, const SaveOptions& opts) {
    auto data = pImpl->write(texture, opts);
    if (data.empty()) {
        return; // issues already logged in Lenient mode, or threw in Strict mode
    }
    if (!write_file_bytes(filePath, data, *pImpl)) {
        return;
    }
}

std::vector<u8> Writer::write(const Texture& texture, const SaveOptions& opts) {
    return pImpl->write(texture, opts);
}

bool Writer::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Writer::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::tex
