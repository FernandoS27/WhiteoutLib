// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// APNG (Animated PNG) parse / write test suite — TDD driver for APNG support
// layered onto the existing PNG parser and writer.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/textures/png/parser.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/texture.h>

// Internal headers — used only to hand-build APNG byte streams in tests.
#include "whiteout/textures/png/deflate.h"
#include "whiteout/textures/png/png_internal.h"

using namespace whiteout;
using namespace whiteout::textures;

namespace {

// ============================================================================
// Pixel / texture helpers
// ============================================================================

/// A solid w*h RGBA8 pixel buffer.
std::vector<u8> solid(u32 w, u32 h, u8 r, u8 g, u8 b, u8 a) {
    std::vector<u8> v(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        v[i * 4 + 0] = r;
        v[i * 4 + 1] = g;
        v[i * 4 + 2] = b;
        v[i * 4 + 3] = a;
    }
    return v;
}

/// Build an RGBA8 Texture from a pixel buffer.
Texture textureFrom(const std::vector<u8>& rgba, u32 w, u32 h) {
    Texture t = Texture::create2D(PixelFormat::RGBA8, w, h, 1);
    std::memcpy(t.dataPtr(), rgba.data(), rgba.size());
    return t;
}

/// Read one RGBA8 pixel from a texture.
std::array<u8, 4> pixel(const Texture& t, u32 x, u32 y) {
    const u8* p = t.dataPtr() + (static_cast<size_t>(y) * t.width() + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

/// True if every pixel of the two equal-sized textures matches.
bool samePixels(const Texture& a, const std::vector<u8>& rgba) {
    if (a.dataSize() != rgba.size())
        return false;
    return std::memcmp(a.dataPtr(), rgba.data(), rgba.size()) == 0;
}

// ============================================================================
// Hand chunk-writer — exercises the parser before the writer exists.
// ============================================================================

void putChunk(std::vector<u8>& out, u32 type, const std::vector<u8>& data) {
    size_t const pos = out.size();
    out.resize(pos + 12 + data.size());
    u8* p = out.data() + pos;
    png::writeU32BE(p, static_cast<u32>(data.size()));
    png::writeU32BE(p + 4, type);
    if (!data.empty())
        std::memcpy(p + 8, data.data(), data.size());
    u32 const c = png::crc32(p + 4, 4 + data.size());
    png::writeU32BE(p + 8 + data.size(), c);
}

/// Filter (type 0 / None) and zlib-compress a w*h RGBA8 image into an
/// IDAT/fdAT-style data stream.
std::vector<u8> encodeImage(const std::vector<u8>& rgba, u32 w, u32 h) {
    std::vector<u8> filtered;
    filtered.reserve(static_cast<size_t>(h) * (1 + w * 4));
    for (u32 y = 0; y < h; ++y) {
        filtered.push_back(0); // filter type None
        const u8* row = rgba.data() + static_cast<size_t>(y) * w * 4;
        filtered.insert(filtered.end(), row, row + static_cast<size_t>(w) * 4);
    }
    std::string err;
    auto compressed = png::zlib_compress(std::span<const u8>(filtered), &err);
    REQUIRE(!compressed.empty());
    return compressed;
}

std::vector<u8> ihdrChunk(u32 w, u32 h) {
    std::vector<u8> d(13, 0);
    png::writeU32BE(d.data() + 0, w);
    png::writeU32BE(d.data() + 4, h);
    d[8] = 8;                          // bit depth
    d[9] = png::COLOR_TRUECOLOR_ALPHA; // color type 6
    return d;
}

std::vector<u8> actlChunk(u32 numFrames, u32 numPlays) {
    std::vector<u8> d(8, 0);
    png::writeU32BE(d.data() + 0, numFrames);
    png::writeU32BE(d.data() + 4, numPlays);
    return d;
}

std::vector<u8> fctlChunk(u32 seq, u32 w, u32 h, u32 x, u32 y, u16 delayNum, u16 delayDen,
                          u8 dispose, u8 blend) {
    std::vector<u8> d(26, 0);
    png::writeU32BE(d.data() + 0, seq);
    png::writeU32BE(d.data() + 4, w);
    png::writeU32BE(d.data() + 8, h);
    png::writeU32BE(d.data() + 12, x);
    png::writeU32BE(d.data() + 16, y);
    d[20] = static_cast<u8>(delayNum >> 8);
    d[21] = static_cast<u8>(delayNum & 0xFF);
    d[22] = static_cast<u8>(delayDen >> 8);
    d[23] = static_cast<u8>(delayDen & 0xFF);
    d[24] = dispose;
    d[25] = blend;
    return d;
}

std::vector<u8> fdatChunk(u32 seq, const std::vector<u8>& imageData) {
    std::vector<u8> d(4 + imageData.size());
    png::writeU32BE(d.data(), seq);
    std::memcpy(d.data() + 4, imageData.data(), imageData.size());
    return d;
}

/// Description of one animation frame for the builder.
struct Frame {
    std::vector<u8> rgba; // w*h*4 sub-rectangle pixels
    u32 w = 0, h = 0, x = 0, y = 0;
    u16 delayNum = 1, delayDen = 10;
    u8 dispose = png::DISPOSE_NONE;
    u8 blend = png::BLEND_SOURCE;
};

/// Build a complete APNG byte stream.
/// - firstIsFrame0 == true:  frame 0's pixels are emitted as IDAT (fcTL precedes IDAT).
/// - firstIsFrame0 == false: `staticImg` is emitted as a standalone IDAT not part of
///                           the animation; every animation frame uses fdAT.
std::vector<u8> buildApng(u32 cw, u32 ch, const std::vector<Frame>& frames, u32 numPlays,
                          bool firstIsFrame0, const std::vector<u8>& staticImg = {}) {
    std::vector<u8> out(png::PNG_SIGNATURE.begin(), png::PNG_SIGNATURE.end());
    putChunk(out, png::CHUNK_IHDR, ihdrChunk(cw, ch));
    putChunk(out, png::CHUNK_acTL, actlChunk(static_cast<u32>(frames.size()), numPlays));

    u32 seq = 0;
    if (firstIsFrame0) {
        const Frame& f0 = frames[0];
        putChunk(out, png::CHUNK_fcTL,
                 fctlChunk(seq++, f0.w, f0.h, f0.x, f0.y, f0.delayNum, f0.delayDen, f0.dispose,
                           f0.blend));
        putChunk(out, png::CHUNK_IDAT, encodeImage(f0.rgba, f0.w, f0.h));
        for (size_t i = 1; i < frames.size(); ++i) {
            const Frame& f = frames[i];
            putChunk(out, png::CHUNK_fcTL,
                     fctlChunk(seq++, f.w, f.h, f.x, f.y, f.delayNum, f.delayDen, f.dispose,
                               f.blend));
            putChunk(out, png::CHUNK_fdAT, fdatChunk(seq++, encodeImage(f.rgba, f.w, f.h)));
        }
    } else {
        putChunk(out, png::CHUNK_IDAT, encodeImage(staticImg, cw, ch));
        for (const Frame& f : frames) {
            putChunk(out, png::CHUNK_fcTL,
                     fctlChunk(seq++, f.w, f.h, f.x, f.y, f.delayNum, f.delayDen, f.dispose,
                               f.blend));
            putChunk(out, png::CHUNK_fdAT, fdatChunk(seq++, encodeImage(f.rgba, f.w, f.h)));
        }
    }
    putChunk(out, png::CHUNK_IEND, {});
    return out;
}

/// A full-canvas frame (sub-rect == whole image).
Frame fullFrame(u32 w, u32 h, u8 r, u8 g, u8 b, u8 a, u16 dn = 1, u16 dd = 10) {
    Frame f;
    f.rgba = solid(w, h, r, g, b, a);
    f.w = w;
    f.h = h;
    f.delayNum = dn;
    f.delayDen = dd;
    return f;
}

} // namespace

// ============================================================================
// 1. A plain (non-animated) PNG still parses and is reported as not animated.
// ============================================================================

TEST_CASE("static PNG still parses and is not animated", "[apng][regression]") {
    auto src = solid(16, 12, 200, 100, 50, 255);
    Texture tex = textureFrom(src, 16, 12);

    png::Writer writer;
    auto bytes = writer.write(tex);
    REQUIRE(!bytes.empty());

    png::Parser parser;
    auto parsed = parser.parse(std::span<const u8>(bytes));
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parser.isAnimated());
    CHECK(parser.frameCount() == 0);
    CHECK(samePixels(*parsed, src));
}

// ============================================================================
// 2. acTL is recognized — the parser reports the PNG as animated.
// ============================================================================

TEST_CASE("parse acTL: APNG recognized as animated", "[apng][parse]") {
    std::vector<Frame> frames = {fullFrame(8, 8, 10, 20, 30, 255)};
    auto bytes = buildApng(8, 8, frames, /*numPlays=*/0, /*firstIsFrame0=*/true);

    png::Parser parser;
    auto parsed = parser.parse(std::span<const u8>(bytes));
    REQUIRE(parsed.has_value());
    CHECK(parser.isAnimated());
    CHECK(parser.frameCount() == 1);
    CHECK(parser.loopCount() == 0);
}

// ============================================================================
// 3. fcTL metadata and the frame-delay formula (delay_den == 0 means 100).
// ============================================================================

TEST_CASE("parse fcTL: metadata and delay conversion", "[apng][parse]") {
    SECTION("delay 3/10 s -> 300 ms") {
        std::vector<Frame> frames = {fullFrame(4, 4, 1, 2, 3, 255, /*dn=*/3, /*dd=*/10)};
        auto bytes = buildApng(4, 4, frames, 0, true);
        png::Parser parser;
        REQUIRE(parser.parse(std::span<const u8>(bytes)).has_value());
        CHECK(parser.frameDelayMs(0) == 300);
    }
    SECTION("delay_den == 0 is treated as 100 -> 5/100 s == 50 ms") {
        std::vector<Frame> frames = {fullFrame(4, 4, 1, 2, 3, 255, /*dn=*/5, /*dd=*/0)};
        auto bytes = buildApng(4, 4, frames, 0, true);
        png::Parser parser;
        REQUIRE(parser.parse(std::span<const u8>(bytes)).has_value());
        CHECK(parser.frameDelayMs(0) == 50);
    }
}

// ============================================================================
// 4. A single-frame APNG whose frame 0 comes from IDAT.
// ============================================================================

TEST_CASE("single-frame APNG: frame 0 decoded from IDAT", "[apng][decode]") {
    auto px = solid(10, 6, 77, 88, 99, 255);
    Frame f;
    f.rgba = px;
    f.w = 10;
    f.h = 6;
    auto bytes = buildApng(10, 6, {f}, 0, /*firstIsFrame0=*/true);

    png::Parser parser;
    auto parsed = parser.parse(std::span<const u8>(bytes));
    REQUIRE(parsed.has_value());
    CHECK(parser.frameCount() == 1);
    CHECK(samePixels(parser.frame(0), px));
    // The default image returned by parse() equals frame 0 here.
    CHECK(samePixels(*parsed, px));
}

// ============================================================================
// 5. IDAT that is a static default image, NOT part of the animation.
// ============================================================================

TEST_CASE("APNG with IDAT not part of the animation", "[apng][decode]") {
    auto staticImg = solid(8, 8, 255, 0, 0, 255);   // colour A — static default
    auto animPx = solid(8, 8, 0, 255, 0, 255);      // colour B — the one frame
    Frame f;
    f.rgba = animPx;
    f.w = 8;
    f.h = 8;
    auto bytes = buildApng(8, 8, {f}, 0, /*firstIsFrame0=*/false, staticImg);

    png::Parser parser;
    auto parsed = parser.parse(std::span<const u8>(bytes));
    REQUIRE(parsed.has_value());
    CHECK(parser.frameCount() == 1);
    CHECK(samePixels(*parsed, staticImg));      // parse() returns the static default
    CHECK(samePixels(parser.frame(0), animPx)); // frame 0 is the fdAT frame
}

// ============================================================================
// 6. Multi-frame APNG: each fdAT frame decodes independently (NONE / SOURCE).
// ============================================================================

TEST_CASE("multi-frame APNG: independent full-canvas frames", "[apng][decode]") {
    std::vector<Frame> frames = {
        fullFrame(12, 9, 200, 0, 0, 255),
        fullFrame(12, 9, 0, 200, 0, 255),
        fullFrame(12, 9, 0, 0, 200, 255),
    };
    auto bytes = buildApng(12, 9, frames, 0, true);

    png::Parser parser;
    REQUIRE(parser.parse(std::span<const u8>(bytes)).has_value());
    REQUIRE(parser.frameCount() == 3);
    CHECK(samePixels(parser.frame(0), solid(12, 9, 200, 0, 0, 255)));
    CHECK(samePixels(parser.frame(1), solid(12, 9, 0, 200, 0, 255)));
    CHECK(samePixels(parser.frame(2), solid(12, 9, 0, 0, 200, 255)));
}

// ============================================================================
// 7. A sub-rectangle frame composited over a full-canvas base.
// ============================================================================

TEST_CASE("APNG sub-rectangle frame composites onto the canvas", "[apng][composite]") {
    // Frame 0: full 8x8 red. Frame 1: a 2x2 blue patch at offset (3,3), SOURCE.
    Frame f0 = fullFrame(8, 8, 255, 0, 0, 255);
    Frame f1;
    f1.rgba = solid(2, 2, 0, 0, 255, 255);
    f1.w = 2;
    f1.h = 2;
    f1.x = 3;
    f1.y = 3;
    f1.dispose = png::DISPOSE_NONE;
    f1.blend = png::BLEND_SOURCE;
    auto bytes = buildApng(8, 8, {f0, f1}, 0, true);

    png::Parser parser;
    REQUIRE(parser.parse(std::span<const u8>(bytes)).has_value());
    REQUIRE(parser.frameCount() == 2);

    const Texture& frame1 = parser.frame(1);
    // Inside the patch -> blue.
    CHECK(pixel(frame1, 3, 3) == std::array<u8, 4>{0, 0, 255, 255});
    CHECK(pixel(frame1, 4, 4) == std::array<u8, 4>{0, 0, 255, 255});
    // Outside the patch -> still red from frame 0 (dispose NONE kept the canvas).
    CHECK(pixel(frame1, 0, 0) == std::array<u8, 4>{255, 0, 0, 255});
    CHECK(pixel(frame1, 7, 7) == std::array<u8, 4>{255, 0, 0, 255});
}

// ============================================================================
// 8. blend_op == OVER alpha-composites against the canvas.
// ============================================================================

TEST_CASE("APNG blend_op OVER alpha-composites", "[apng][composite]") {
    // Frame 0: opaque red. Frame 1: full-canvas green at alpha 128, blend OVER.
    Frame f0 = fullFrame(4, 4, 255, 0, 0, 255);
    Frame f1 = fullFrame(4, 4, 0, 255, 0, 128);
    f1.blend = png::BLEND_OVER;
    auto bytes = buildApng(4, 4, {f0, f1}, 0, true);

    png::Parser parser;
    REQUIRE(parser.parse(std::span<const u8>(bytes)).has_value());

    // Standard 8-bit straight-alpha OVER: src=(0,255,0,128) over dst=(255,0,0,255).
    const u32 sa = 128, da = 255;
    const u32 oa = sa + da * (255 - sa) / 255; // == 255
    auto over = [&](u32 sc, u32 dc) -> u8 {
        u32 v = sc * sa + dc * da * (255 - sa) / 255;
        return static_cast<u8>((v + oa / 2) / oa);
    };
    std::array<u8, 4> expect{over(0, 255), over(255, 0), over(0, 0), static_cast<u8>(oa)};
    CHECK(pixel(parser.frame(1), 2, 2) == expect);
}

// ============================================================================
// 9. dispose_op state machine: BACKGROUND and PREVIOUS.
// ============================================================================

TEST_CASE("APNG dispose_op BACKGROUND and PREVIOUS", "[apng][composite]") {
    // Canvas 4x4. Frame 0: full red, dispose=BACKGROUND (clears whole canvas after).
    // Frame 1: full green, dispose=PREVIOUS (snapshots, then restores after).
    // Frame 2: a 1x1 blue patch at (0,0), dispose=NONE.
    Frame f0 = fullFrame(4, 4, 255, 0, 0, 255);
    f0.dispose = png::DISPOSE_BACKGROUND;
    Frame f1 = fullFrame(4, 4, 0, 255, 0, 255);
    f1.dispose = png::DISPOSE_PREVIOUS;
    Frame f2;
    f2.rgba = solid(1, 1, 0, 0, 255, 255);
    f2.w = 1;
    f2.h = 1;
    f2.x = 0;
    f2.y = 0;
    auto bytes = buildApng(4, 4, {f0, f1, f2}, 0, true);

    png::Parser parser;
    REQUIRE(parser.parse(std::span<const u8>(bytes)).has_value());
    REQUIRE(parser.frameCount() == 3);

    // Frame 0 output: full red.
    CHECK(pixel(parser.frame(0), 1, 1) == std::array<u8, 4>{255, 0, 0, 255});
    // Frame 1 output: full green (frame 0 disposed to background first).
    CHECK(pixel(parser.frame(1), 1, 1) == std::array<u8, 4>{0, 255, 0, 255});
    // Frame 2: frame 1 used dispose=PREVIOUS, so the canvas was restored to
    // the pre-frame-1 state (transparent, since frame 0 cleared it). Then the
    // blue patch is drawn at (0,0).
    CHECK(pixel(parser.frame(2), 0, 0) == std::array<u8, 4>{0, 0, 255, 255});
    CHECK(pixel(parser.frame(2), 3, 3) == std::array<u8, 4>{0, 0, 0, 0});
}

// ============================================================================
// 10. dispose_op == PREVIOUS on frame 0 is coerced to BACKGROUND.
// ============================================================================

TEST_CASE("APNG dispose PREVIOUS on frame 0 behaves as BACKGROUND", "[apng][edge]") {
    // Frame 0: a 2x2 red patch at (0,0), dispose=PREVIOUS (coerced to BACKGROUND).
    // Frame 1: a 2x2 green patch at (2,2). The frame-0 patch must NOT survive.
    // A sub-rectangle frame 0 is only valid when IDAT is a separate static
    // image (the spec requires an IDAT-backed frame 0 to be full-canvas).
    Frame f0;
    f0.rgba = solid(2, 2, 255, 0, 0, 255);
    f0.w = 2;
    f0.h = 2;
    f0.dispose = png::DISPOSE_PREVIOUS;
    Frame f1;
    f1.rgba = solid(2, 2, 0, 255, 0, 255);
    f1.w = 2;
    f1.h = 2;
    f1.x = 2;
    f1.y = 2;
    auto bytes = buildApng(4, 4, {f0, f1}, 0, /*firstIsFrame0=*/false, solid(4, 4, 0, 0, 0, 0));

    png::Parser parser;
    REQUIRE(parser.parse(std::span<const u8>(bytes)).has_value());
    // Frame 1: the (0,0) red patch was cleared (BACKGROUND), green patch present.
    CHECK(pixel(parser.frame(1), 0, 0) == std::array<u8, 4>{0, 0, 0, 0});
    CHECK(pixel(parser.frame(1), 3, 3) == std::array<u8, 4>{0, 255, 0, 255});
}

// ============================================================================
// 11. Out-of-order sequence numbers: the parser records an issue but still
//     produces a best-effort result (the library is non-throwing).
// ============================================================================

TEST_CASE("APNG out-of-order sequence number handling", "[apng][edge]") {
    // Build a valid 2-frame APNG, then corrupt the first fdAT's sequence number.
    std::vector<Frame> frames = {
        fullFrame(6, 6, 1, 1, 1, 255),
        fullFrame(6, 6, 2, 2, 2, 255),
    };
    auto bytes = buildApng(6, 6, frames, 0, true);

    // Locate the fdAT chunk and bump its sequence number, then fix the CRC.
    size_t pos = 8;
    bool patched = false;
    while (pos + 12 <= bytes.size()) {
        u32 const len = png::readU32BE(bytes.data() + pos);
        u32 const type = png::readU32BE(bytes.data() + pos + 4);
        if (type == png::CHUNK_fdAT) {
            u8* data = bytes.data() + pos + 8;
            png::writeU32BE(data, png::readU32BE(data) + 7); // wrong sequence number
            u32 const c = png::crc32(bytes.data() + pos + 4, 4 + len);
            png::writeU32BE(bytes.data() + pos + 8 + len, c);
            patched = true;
            break;
        }
        pos += 12 + len;
    }
    REQUIRE(patched);

    png::Parser parser;
    auto parsed = parser.parse(std::span<const u8>(bytes));
    CHECK(parsed.has_value());
    CHECK(parser.hasIssues());
}

// ============================================================================
// 12. writeAnimated emits a valid APNG the parser accepts.
// ============================================================================

TEST_CASE("writeAnimated emits a valid APNG", "[apng][write]") {
    std::vector<png::ApngFrame> frames = {
        {textureFrom(solid(10, 10, 255, 0, 0, 255), 10, 10), 100},
        {textureFrom(solid(10, 10, 0, 255, 0, 255), 10, 10), 100},
        {textureFrom(solid(10, 10, 0, 0, 255, 255), 10, 10), 100},
    };
    png::ApngSaveOptions opts;
    opts.loopCount = 5;

    png::Writer writer;
    auto bytes = writer.writeAnimated(frames, opts);
    REQUIRE(!bytes.empty());

    png::Parser parser;
    auto parsed = parser.parse(std::span<const u8>(bytes));
    REQUIRE(parsed.has_value());
    CHECK(parser.isAnimated());
    CHECK(parser.frameCount() == 3);
    CHECK(parser.loopCount() == 5);
}

// ============================================================================
// 13. MAIN TARGET — multi-frame pixel round-trip through writer and parser.
// ============================================================================

TEST_CASE("APNG multi-frame pixel round-trip", "[apng][roundtrip]") {
    struct Spec {
        std::vector<u8> rgba;
        u32 delayMs;
    };
    const u32 w = 20, h = 14;
    std::vector<Spec> specs = {
        {solid(w, h, 240, 16, 16, 255), 40},
        {solid(w, h, 16, 240, 16, 255), 80},
        {solid(w, h, 16, 16, 240, 128), 120},
        {solid(w, h, 90, 90, 90, 200), 250},
    };

    std::vector<png::ApngFrame> frames;
    for (const auto& s : specs)
        frames.push_back({textureFrom(s.rgba, w, h), s.delayMs});

    png::Writer writer;
    auto bytes = writer.writeAnimated(frames);
    REQUIRE(!bytes.empty());

    png::Parser parser;
    REQUIRE(parser.parse(std::span<const u8>(bytes)).has_value());
    REQUIRE(parser.frameCount() == specs.size());
    for (size_t i = 0; i < specs.size(); ++i) {
        INFO("frame " << i);
        CHECK(samePixels(parser.frame(static_cast<u32>(i)), specs[i].rgba));
        CHECK(parser.frameDelayMs(static_cast<u32>(i)) == specs[i].delayMs);
    }
}

// ============================================================================
// 14. A single-frame writeAnimated call produces a valid 1-frame APNG.
// ============================================================================

TEST_CASE("writeAnimated single frame produces a valid APNG", "[apng][edge]") {
    auto px = solid(7, 5, 33, 66, 99, 255);
    std::vector<png::ApngFrame> frames = {{textureFrom(px, 7, 5), 100}};

    png::Writer writer;
    auto bytes = writer.writeAnimated(frames);
    REQUIRE(!bytes.empty());

    png::Parser parser;
    REQUIRE(parser.parse(std::span<const u8>(bytes)).has_value());
    CHECK(parser.isAnimated());
    CHECK(parser.frameCount() == 1);
    CHECK(samePixels(parser.frame(0), px));
}

// ============================================================================
// 15. Round-trip a plain texture through the single-image writer — confirm the
//     APNG chunk-loop additions did not regress static PNG decoding.
// ============================================================================

TEST_CASE("static PNG round-trip unaffected by APNG support", "[apng][regression]") {
    // Deterministic non-uniform content exercises the filter/unfilter paths.
    const u32 w = 24, h = 18;
    std::vector<u8> src(static_cast<size_t>(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* p = src.data() + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = static_cast<u8>(x * 7 + y * 3);
            p[1] = static_cast<u8>(x * 2 + y * 11);
            p[2] = static_cast<u8>(x * 5 + y * 5);
            p[3] = 255;
        }
    }
    png::Writer writer;
    auto bytes = writer.write(textureFrom(src, w, h));
    REQUIRE(!bytes.empty());

    png::Parser parser;
    auto parsed = parser.parse(std::span<const u8>(bytes));
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parser.isAnimated());
    CHECK(parser.frameCount() == 0);
    CHECK(samePixels(*parsed, src));
}
