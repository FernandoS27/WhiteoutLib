// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/tiff/parser.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lzw.h"
#include "packbits.h"
#include "tiff_internal.h"

#include "../../common/deflate.h"
#include "../io_helpers.h"
#include "../issue_sink.h"

namespace whiteout::textures::tiff {

namespace {

/// Per-IFD decoded set of the tags we consume. Defaults follow TIFF 6.0
/// where the spec provides them (e.g. Compression defaults to 1=None).
struct IFD {
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> bitsPerSample{8};
    u16 compression = Compression::None;
    u16 photometric = 0xFFFF; // invalid sentinel; baseline TIFF requires this tag
    u32 samplesPerPixel = 1;
    u32 rowsPerStrip = 0xFFFFFFFFu;
    std::vector<u32> stripOffsets;
    std::vector<u32> stripByteCounts;
    u16 planarConfiguration = 1; // 1=chunky (default)
    u16 orientation = 1;         // 1=top-left (default)
    u16 predictor = 1;           // 1=no predictor (default)
    std::vector<u32> extraSamples;
    /// ColorMap: u16 values in R0..R255, G0..G255, B0..B255 order (768 entries
    /// for 8-bit palette). Empty when photometric != Palette.
    std::vector<u32> colorMap;
};

/// Walk the IFD at @p offset, populating @p ifd. Returns false on malformed
/// data (truncated buffer, bad entry counts, etc.).
bool walkIFD(std::span<const u8> file, u32 offset, const EndianReader& er,
             IFD& ifd, IssueSink& sink) {
    if (offset + 2 > file.size())
        return sink.fail("IFD offset past end of buffer");

    const u16 entryCount = er.readU16(file.data() + offset);
    const u32 entriesStart = offset + 2;
    const u64 entriesEnd = static_cast<u64>(entriesStart) + entryCount * 12;
    if (entriesEnd + 4 > file.size())
        return sink.fail("IFD entries extend past end of buffer");

    for (u16 i = 0; i < entryCount; ++i) {
        IFDEntry e{};
        const u8* base = file.data() + entriesStart + i * 12;
        e.tag = er.readU16(base);
        e.type = er.readU16(base + 2);
        e.count = er.readU32(base + 4);
        std::memcpy(e.raw.data(), base + 8, 4);

        switch (e.tag) {
        case Tag::ImageWidth:
            if (auto v = tagAsU32(e, er); v)
                ifd.width = *v;
            else
                return sink.fail("ImageWidth not a single scalar");
            break;
        case Tag::ImageLength:
            if (auto v = tagAsU32(e, er); v)
                ifd.height = *v;
            else
                return sink.fail("ImageLength not a single scalar");
            break;
        case Tag::BitsPerSample:
            if (auto v = tagAsU32Vec(e, er, file); v)
                ifd.bitsPerSample = *v;
            else
                return sink.fail("BitsPerSample malformed");
            break;
        case Tag::Compression:
            if (auto v = tagAsU32(e, er); v)
                ifd.compression = static_cast<u16>(*v);
            break;
        case Tag::PhotometricInterpretation:
            if (auto v = tagAsU32(e, er); v)
                ifd.photometric = static_cast<u16>(*v);
            break;
        case Tag::SamplesPerPixel:
            if (auto v = tagAsU32(e, er); v)
                ifd.samplesPerPixel = *v;
            break;
        case Tag::RowsPerStrip:
            if (auto v = tagAsU32(e, er); v)
                ifd.rowsPerStrip = *v;
            break;
        case Tag::StripOffsets:
            if (auto v = tagAsU32Vec(e, er, file); v)
                ifd.stripOffsets = *v;
            else
                return sink.fail("StripOffsets malformed");
            break;
        case Tag::StripByteCounts:
            if (auto v = tagAsU32Vec(e, er, file); v)
                ifd.stripByteCounts = *v;
            else
                return sink.fail("StripByteCounts malformed");
            break;
        case Tag::PlanarConfiguration:
            if (auto v = tagAsU32(e, er); v)
                ifd.planarConfiguration = static_cast<u16>(*v);
            break;
        case Tag::Orientation:
            if (auto v = tagAsU32(e, er); v)
                ifd.orientation = static_cast<u16>(*v);
            break;
        case Tag::Predictor:
            if (auto v = tagAsU32(e, er); v)
                ifd.predictor = static_cast<u16>(*v);
            break;
        case Tag::ExtraSamples:
            if (auto v = tagAsU32Vec(e, er, file); v)
                ifd.extraSamples = *v;
            break;
        case Tag::ColorMap:
            if (auto v = tagAsU32Vec(e, er, file); v)
                ifd.colorMap = *v;
            break;
        default:
            break; // ignored
        }
    }
    return true;
}

/// Validate that the IFD describes a configuration we support.
/// Also fills in TIFF 6.0 defaults that depend on other tags.
bool validateIFD(IFD& ifd, IssueSink& sink) {
    if (ifd.width == 0 || ifd.height == 0)
        return sink.fail("TIFF has zero dimensions");
    // Default: a single strip spanning the whole image.
    if (ifd.rowsPerStrip == 0xFFFFFFFFu || ifd.rowsPerStrip == 0)
        ifd.rowsPerStrip = ifd.height;
    switch (ifd.compression) {
    case Compression::None:
    case Compression::PackBits:
    case Compression::Deflate:
    case Compression::AdobeDeflate:
    case Compression::Lzw:
        break;
    default:
        return sink.fail("Unsupported TIFF compression code: " +
                         std::to_string(ifd.compression));
    }
    if (ifd.photometric != Photometric::Rgb &&
        ifd.photometric != Photometric::BlackIsZero &&
        ifd.photometric != Photometric::Palette)
        return sink.fail("Unsupported PhotometricInterpretation: " +
                         std::to_string(ifd.photometric));
    if (ifd.photometric == Photometric::Rgb &&
        (ifd.samplesPerPixel != 3 && ifd.samplesPerPixel != 4))
        return sink.fail("RGB photometric requires 3 or 4 samples per pixel");
    if (ifd.photometric == Photometric::BlackIsZero && ifd.samplesPerPixel != 1)
        return sink.fail("Gray photometric requires 1 sample per pixel");
    if (ifd.photometric == Photometric::Palette) {
        if (ifd.samplesPerPixel != 1)
            return sink.fail("Palette photometric requires SamplesPerPixel=1");
        if (ifd.colorMap.empty())
            return sink.fail("Palette photometric requires ColorMap");
        if (ifd.colorMap.size() != 3u * (1u << ifd.bitsPerSample[0]))
            return sink.fail("ColorMap size mismatch with BitsPerSample");
    }
    for (u32 b : ifd.bitsPerSample)
        if (b != 8)
            return sink.fail("Only 8-bit samples supported in Phase 1");
    if (ifd.planarConfiguration != 1)
        return sink.fail("Only chunky PlanarConfiguration supported in Phase 1");
    if (ifd.predictor != 1 && ifd.predictor != 2)
        return sink.fail("Unsupported Predictor: " + std::to_string(ifd.predictor));
    if (ifd.stripOffsets.empty() ||
        ifd.stripOffsets.size() != ifd.stripByteCounts.size())
        return sink.fail("StripOffsets/StripByteCounts mismatch");
    return true;
}

/// Expected uncompressed byte size of one strip of @p rows full-width rows.
u64 expectedStripBytes(const IFD& ifd, u32 rows) {
    // 8-bit samples assumed in Phase 1/2; one byte per sample, chunky layout.
    return static_cast<u64>(ifd.width) * rows * ifd.samplesPerPixel;
}

/// Apply TIFF predictor 2 (horizontal differencing) post-pass to a chunky
/// 8-bit strip. Each sample is the difference from the same-channel sample
/// on its immediate left; we reconstruct by running prefix-sums per row.
void applyPredictor2(std::vector<u8>& buf, u32 width, u32 rows, u32 samples) {
    for (u32 r = 0; r < rows; ++r) {
        u8* row = buf.data() + static_cast<u64>(r) * width * samples;
        for (u32 x = 1; x < width; ++x) {
            u8* px = row + x * samples;
            // Previous-pixel base pointer (well-defined: x >= 1).
            const u8* prev = px - samples;
            for (u32 c = 0; c < samples; ++c)
                px[c] = static_cast<u8>(px[c] + prev[c]);
        }
    }
}

/// Decode (and decompress if needed) every strip and concatenate into one
/// contiguous chunky-pixel buffer.
std::optional<std::vector<u8>> readStripData(std::span<const u8> file,
                                              const IFD& ifd,
                                              IssueSink& sink) {
    // Total expected uncompressed size = width * height * samples (8-bit).
    const u64 totalUncompressed =
        static_cast<u64>(ifd.width) * ifd.height * ifd.samplesPerPixel;
    std::vector<u8> out;
    out.reserve(static_cast<size_t>(totalUncompressed));

    for (size_t i = 0; i < ifd.stripOffsets.size(); ++i) {
        const u32 off = ifd.stripOffsets[i];
        const u32 cnt = ifd.stripByteCounts[i];
        if (static_cast<u64>(off) + cnt > file.size()) {
            sink.fail("Strip extends past end of buffer");
            return std::nullopt;
        }
        std::span<const u8> stripSrc{file.data() + off, cnt};

        // Per-strip expected row count (last strip may be short).
        const u32 stripStartRow = static_cast<u32>(i) * ifd.rowsPerStrip;
        const u32 thisStripRows =
            std::min(ifd.rowsPerStrip, ifd.height - stripStartRow);
        const u64 stripExpected = expectedStripBytes(ifd, thisStripRows);

        std::vector<u8> stripDecoded;
        if (ifd.compression == Compression::None) {
            stripDecoded.assign(stripSrc.begin(), stripSrc.end());
        } else if (ifd.compression == Compression::PackBits) {
            stripDecoded = packBitsDecompress(stripSrc, static_cast<size_t>(stripExpected));
            if (stripDecoded.empty()) {
                sink.fail("PackBits decompression failed");
                return std::nullopt;
            }
        } else if (ifd.compression == Compression::Lzw) {
            stripDecoded = lzwDecompress(stripSrc, static_cast<size_t>(stripExpected));
            if (stripDecoded.empty()) {
                sink.fail("LZW decompression failed");
                return std::nullopt;
            }
        } else if (ifd.compression == Compression::Deflate ||
                   ifd.compression == Compression::AdobeDeflate) {
            std::string err;
            stripDecoded = ::whiteout::zlib_decompress(
                stripSrc, &err, static_cast<size_t>(stripExpected));
            if (stripDecoded.empty()) {
                sink.fail("Deflate decompression failed: " + err);
                return std::nullopt;
            }
        } else {
            sink.fail("Unreachable: compression " +
                      std::to_string(ifd.compression) +
                      " passed validation but has no decoder");
            return std::nullopt;
        }

        if (stripDecoded.size() != stripExpected) {
            sink.fail("Decoded strip size mismatch (expected " +
                      std::to_string(stripExpected) + ", got " +
                      std::to_string(stripDecoded.size()) + ")");
            return std::nullopt;
        }

        if (ifd.predictor == 2)
            applyPredictor2(stripDecoded, ifd.width, thisStripRows,
                            ifd.samplesPerPixel);

        out.insert(out.end(), stripDecoded.begin(), stripDecoded.end());
    }
    return out;
}

/// Lift the decoded sample bytes into an RGBA8 buffer.
void liftToRGBA8(const std::vector<u8>& strip, const IFD& ifd, u8* dst) {
    const u64 px = static_cast<u64>(ifd.width) * ifd.height;
    if (ifd.photometric == Photometric::Rgb && ifd.samplesPerPixel == 3) {
        const u8* s = strip.data();
        for (u64 i = 0; i < px; ++i) {
            dst[0] = s[0];
            dst[1] = s[1];
            dst[2] = s[2];
            dst[3] = 255;
            dst += 4;
            s += 3;
        }
        return;
    }
    if (ifd.photometric == Photometric::Rgb && ifd.samplesPerPixel == 4) {
        std::memcpy(dst, strip.data(), static_cast<size_t>(px * 4));
        return;
    }
    if (ifd.photometric == Photometric::BlackIsZero) {
        const u8* s = strip.data();
        for (u64 i = 0; i < px; ++i) {
            dst[0] = s[0];
            dst[1] = s[0];
            dst[2] = s[0];
            dst[3] = 255;
            dst += 4;
            s += 1;
        }
        return;
    }
    if (ifd.photometric == Photometric::Palette) {
        // ColorMap is u16 (0..65535) in R0..R_n, G0..G_n, B0..B_n order.
        // We downshift to 8-bit by >> 8 (the TIFF-spec convention).
        const u32 entries = 1u << ifd.bitsPerSample[0];
        const u32* r = ifd.colorMap.data();
        const u32* g = r + entries;
        const u32* b = g + entries;
        const u8* s = strip.data();
        for (u64 i = 0; i < px; ++i) {
            const u8 idx = s[i];
            dst[0] = static_cast<u8>(r[idx] >> 8);
            dst[1] = static_cast<u8>(g[idx] >> 8);
            dst[2] = static_cast<u8>(b[idx] >> 8);
            dst[3] = 255;
            dst += 4;
        }
        return;
    }
}

} // namespace

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> buffer);
};

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer) {
    issues.clear();

    if (buffer.size() < kClassicHeaderSize) {
        fail("Buffer too small for a TIFF header");
        return std::nullopt;
    }

    // Header.
    const u16 raw_order = static_cast<u16>(buffer[0]) |
                          static_cast<u16>(buffer[1]) << 8;
    EndianReader er;
    if (raw_order == kByteOrderLE) {
        er.littleEndian = true;
    } else if (raw_order == kByteOrderBE) {
        er.littleEndian = false;
    } else {
        fail("Invalid TIFF byte-order marker");
        return std::nullopt;
    }

    const u16 magic = er.readU16(buffer.data() + 2);
    if (magic == kMagicBigTiff) {
        fail("BigTIFF (magic 43) is not supported");
        return std::nullopt;
    }
    if (magic != kMagicClassic) {
        fail("Invalid TIFF magic: " + std::to_string(magic));
        return std::nullopt;
    }

    const u32 ifd0Offset = er.readU32(buffer.data() + 4);

    // Walk IFD0.
    IFD ifd;
    if (!walkIFD(buffer, ifd0Offset, er, ifd, *this))
        return std::nullopt;
    if (!validateIFD(ifd, *this))
        return std::nullopt;

    // Read strip payload and lift to RGBA8.
    auto strip = readStripData(buffer, ifd, *this);
    if (!strip)
        return std::nullopt;

    Texture texture = Texture::create2D(PixelFormat::RGBA8, ifd.width,
                                        ifd.height, 1);
    liftToRGBA8(*strip, ifd, texture.dataPtr());
    return texture;
}

Parser::Parser() : pImpl(std::make_unique<Impl>()) {}
Parser::~Parser() = default;

std::optional<Texture> Parser::parse(const std::string& filePath) {
    pImpl->issues.clear();
    auto buf = read_file_bytes(filePath, *pImpl);
    if (!buf) {
        return std::nullopt;
    }
    return pImpl->parse(std::span<const u8>{*buf});
}

std::optional<Texture> Parser::parse(std::span<const u8> buffer) {
    return pImpl->parse(buffer);
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::tiff
