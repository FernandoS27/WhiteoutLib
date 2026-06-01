// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/tiff/writer.h>

#include <cstring>
#include <memory>
#include <vector>

#include "tiff_internal.h"

#include "../../common/byte_order.h"
#include "../io_helpers.h"
#include "../issue_sink.h"

namespace whiteout::textures::tiff {

namespace {

// We emit RGBA8 only — caller coerces via copyAsFormat (same shortcut as BMP).
// Output layout (single IFD, single uncompressed strip):
//   header (8 bytes) — II + 42 + IFD offset
//   pixel data       — width * height * 4 bytes, RGBA chunky
//   IFD              — entry count (u16) + N * 12-byte entries + next-IFD (u32)
//   inline-overflow  — values too large for the 4-byte inline slot

constexpr u16 kEntryCount = 9;

/// Build a 12-byte IFD entry record into @p out.
/// @param value4 the 4 bytes to place in the value-or-offset slot
/// (the caller is responsible for writing any out-of-line payload).
void pushEntry(std::vector<u8>& out, u16 tag, FieldType type, u32 count,
               const std::array<u8, 4>& value4) {
    ::whiteout::common::pushLE16(out, tag);
    ::whiteout::common::pushLE16(out, static_cast<u16>(type));
    ::whiteout::common::pushLE32(out, count);
    out.insert(out.end(), value4.begin(), value4.end());
}

std::array<u8, 4> inlineU16(u16 v) {
    std::array<u8, 4> a{};
    ::whiteout::common::writeLE16(a.data(), v);
    return a;
}

std::array<u8, 4> inlineU32(u32 v) {
    std::array<u8, 4> a{};
    ::whiteout::common::writeLE32(a.data(), v);
    return a;
}

} // namespace

class Writer::Impl : public IssueSink {
public:
    std::vector<u8> write(const Texture& texture);
};

std::vector<u8> Writer::Impl::write(const Texture& texture) {
    issues.clear();

    if (texture.width() == 0 || texture.height() == 0) {
        fail("Cannot save an empty texture");
        return {};
    }

    // Force RGBA8 (mirrors BMP's simplification).
    Texture rgba = texture.copyAsFormat(PixelFormat::RGBA8);
    const u32 W = rgba.width();
    const u32 H = rgba.height();
    const u64 pixelSize = static_cast<u64>(W) * H * 4;

    // Layout:
    //   [0..8)               header
    //   [8..8+pixelSize)     pixel data (one strip)
    //   [pixelOff..)         IFD + inline-overflow payloads
    const u32 pixelOffset = 8;
    const u32 ifdOffset = static_cast<u32>(pixelOffset + pixelSize);

    // BitsPerSample is 4 u16 values = 8 bytes, won't fit inline (4-byte slot)
    // — store at offset *after* the IFD.
    const u32 ifdSize = 2 + kEntryCount * 12 + 4;
    const u32 bpsOffset = ifdOffset + ifdSize;
    const u32 totalSize = bpsOffset + 8; // BPS = 4 * u16

    std::vector<u8> out(totalSize, 0);

    // Header.
    out[0] = 'I'; out[1] = 'I';
    ::whiteout::common::writeLE16(out.data() + 2, 42);
    ::whiteout::common::writeLE32(out.data() + 4, ifdOffset);

    // Pixel data.
    std::memcpy(out.data() + pixelOffset, rgba.dataPtr(),
                static_cast<size_t>(pixelSize));

    // IFD: entry count, then 9 entries in ascending tag order, then next-IFD=0.
    std::vector<u8> ifd;
    ::whiteout::common::pushLE16(ifd, kEntryCount);

    pushEntry(ifd, Tag::ImageWidth, FieldType::Long, 1, inlineU32(W));
    pushEntry(ifd, Tag::ImageLength, FieldType::Long, 1, inlineU32(H));
    pushEntry(ifd, Tag::BitsPerSample, FieldType::Short, 4, inlineU32(bpsOffset));
    pushEntry(ifd, Tag::Compression, FieldType::Short, 1, inlineU16(Compression::None));
    pushEntry(ifd, Tag::PhotometricInterpretation, FieldType::Short, 1,
              inlineU16(Photometric::Rgb));
    pushEntry(ifd, Tag::StripOffsets, FieldType::Long, 1, inlineU32(pixelOffset));
    pushEntry(ifd, Tag::SamplesPerPixel, FieldType::Short, 1, inlineU16(4));
    pushEntry(ifd, Tag::RowsPerStrip, FieldType::Long, 1, inlineU32(H));
    pushEntry(ifd, Tag::StripByteCounts, FieldType::Long, 1,
              inlineU32(static_cast<u32>(pixelSize)));

    ::whiteout::common::pushLE32(ifd, 0); // next-IFD = none

    std::memcpy(out.data() + ifdOffset, ifd.data(), ifd.size());

    // BitsPerSample payload: 4 little-endian u16 each = 8.
    for (u32 i = 0; i < 4; ++i)
        ::whiteout::common::writeLE16(out.data() + bpsOffset + i * 2, 8);

    return out;
}

Writer::Writer() : pImpl(std::make_unique<Impl>()) {}
Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const Texture& texture) {
    auto data = pImpl->write(texture);
    if (data.empty()) {
        return;
    }
    if (!write_file_bytes(filePath, data, *pImpl)) {
        return;
    }
}

std::vector<u8> Writer::write(const Texture& texture) {
    return pImpl->write(texture);
}

bool Writer::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Writer::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::tiff
