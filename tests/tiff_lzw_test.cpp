// SPDX-License-Identifier: BSD-3-Clause
// Codec-isolated LZW unit tests for the TIFF parser.

#include <catch2/catch_all.hpp>

#include <array>
#include <fstream>
#include <span>
#include <vector>

#include "../src/whiteout/textures/tiff/lzw.h"

using namespace whiteout;
using namespace whiteout::textures::tiff;

// ============================================================================
// Phase 2.4 — basic decode against a hand-crafted stream
// ============================================================================

TEST_CASE("lzw_test::decodes_minimal_stream",
          "[tiff][lzw][phase2]") {
    const u8 src[] = {0x80, 0x10, 0x48, 0x50, 0x10};
    auto out = lzwDecompress(src);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == 'A');
    REQUIRE(out[1] == 'B');
}

// ============================================================================
// Phase 2.5 — TIFF early-change quirk (named regression)
// ============================================================================
namespace {

// Minimal MSB-first LZW encoder for the AAA... regression case only.
class TestLzwEncoder {
public:
    std::vector<u8> encode_all_A(size_t out_string_count) {
        codeSize = 9;
        nextCode = 258;
        emitCode(256);
        emitCode(static_cast<u32>('A'));
        prevLen = 1;
        for (size_t i = 0; i < out_string_count; ++i) {
            u32 code = nextCode;
            emitCode(code);
            ++nextCode;
            ++prevLen;
            if (nextCode == ((1u << codeSize) - 1) && codeSize < 12)
                ++codeSize;
        }
        emitCode(257);
        flush();
        return out;
    }

private:
    std::vector<u8> out;
    u32 bitBuf = 0;
    int bitsInBuf = 0;
    int codeSize = 9;
    u32 nextCode = 258;
    int prevLen = 0;

    void emitCode(u32 code) {
        bitBuf = (bitBuf << codeSize) | (code & ((1u << codeSize) - 1));
        bitsInBuf += codeSize;
        while (bitsInBuf >= 8) {
            bitsInBuf -= 8;
            out.push_back(static_cast<u8>((bitBuf >> bitsInBuf) & 0xFF));
        }
    }
    void flush() {
        if (bitsInBuf > 0) {
            out.push_back(static_cast<u8>((bitBuf << (8 - bitsInBuf)) & 0xFF));
            bitsInBuf = 0;
        }
    }
};

} // namespace

TEST_CASE("lzw_test::handles_tiff_early_change_quirk",
          "[tiff][lzw][phase2]") {
    const size_t TABLE_GROWS = 300;
    TestLzwEncoder enc;
    auto stream = enc.encode_all_A(TABLE_GROWS);
    const size_t N = TABLE_GROWS + 1;
    const size_t expected_size = N * (N + 1) / 2;

    auto out = lzwDecompress(stream, expected_size);
    INFO("expected_size=" << expected_size << " got_size=" << out.size());
    REQUIRE(out.size() == expected_size);
    bool all_A = std::all_of(out.begin(), out.end(),
                             [](u8 c) { return c == 'A'; });
    REQUIRE(all_A);
}

// ============================================================================
// Wild-corpus regression — first strip of Koltira_BodyArmor__Emissive.tif.
// 158 bytes encoding 8192 bytes of largely-uniform pixel data (after the
// predictor pre-pass on the encoder side, most deltas are zero, so the LZW
// codebook grows fast). This test is named so a regression in handling
// either codebook saturation or the 12-bit ceiling shows up unambiguously.
// ============================================================================
TEST_CASE("lzw_test::decodes_koltira_emissive_strip0",
          "[tiff][lzw][corpus]") {
    const std::string path = "Corpus/MDL/Koltira Deathweaver (High Elf "
                             "Death Knight)/Koltira_BodyArmor__Emissive.tif";
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        SKIP("Koltira corpus not present");
    const auto sz = f.tellg();
    f.seekg(0);
    std::vector<u8> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(buf.size()));

    // Strip 0 lives at byte offset 16 of this fixture, 158 bytes long (per
    // the IFD parsed by PIL — we hard-code so the test is self-contained).
    // If the file changes, the test will fail loudly and we'll update both.
    const size_t strip0_off = 8;
    const size_t strip0_len = 158;
    REQUIRE(buf.size() > strip0_off + strip0_len);

    std::span<const u8> strip{buf.data() + strip0_off, strip0_len};
    auto out = lzwDecompress(strip, 8192);
    REQUIRE(out.size() == 8192);
}

// ============================================================================
// Wild-corpus regression — sweep every strip of Koltira_BodyArmor__Emissive,
// reading offsets / counts from the parser's own IFD walk so we know which
// strip index breaks first.
// ============================================================================
TEST_CASE("lzw_test::decodes_every_koltira_emissive_strip",
          "[tiff][lzw][corpus]") {
    const std::string path = "Corpus/MDL/Koltira Deathweaver (High Elf "
                             "Death Knight)/Koltira_BodyArmor__Emissive.tif";
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        SKIP("Koltira corpus not present");
    const auto sz = f.tellg();
    f.seekg(0);
    std::vector<u8> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(buf.size()));

    // Bootstrap: walk the TIFF header + IFD by hand to extract strip
    // offsets/counts arrays. (Single-IFD, II byte order, classic TIFF.)
    REQUIRE(buf[0] == 'I'); REQUIRE(buf[1] == 'I');
    auto rd16 = [&](size_t o) -> u16 {
        return u16(buf[o]) | (u16(buf[o + 1]) << 8);
    };
    auto rd32 = [&](size_t o) -> u32 {
        return u32(buf[o]) | (u32(buf[o + 1]) << 8) |
               (u32(buf[o + 2]) << 16) | (u32(buf[o + 3]) << 24);
    };
    const u32 ifd_off = rd32(4);
    const u16 num_entries = rd16(ifd_off);
    u32 strip_off_tag_off = 0, strip_off_tag_count = 0;
    u32 strip_cnt_tag_off = 0, strip_cnt_tag_count = 0;
    u16 strip_off_type = 0, strip_cnt_type = 0;
    for (u16 i = 0; i < num_entries; ++i) {
        const size_t e = ifd_off + 2 + i * 12;
        const u16 tag = rd16(e);
        const u16 type = rd16(e + 2);
        const u32 cnt = rd32(e + 4);
        const u32 val = rd32(e + 8);
        if (tag == 273) { // StripOffsets
            strip_off_tag_off = val;
            strip_off_tag_count = cnt;
            strip_off_type = type;
        } else if (tag == 279) { // StripByteCounts
            strip_cnt_tag_off = val;
            strip_cnt_tag_count = cnt;
            strip_cnt_type = type;
        }
    }
    REQUIRE(strip_off_tag_count == 2048);
    REQUIRE(strip_off_tag_count == strip_cnt_tag_count);

    auto readArr = [&](u32 off, u32 cnt, u16 type) -> std::vector<u32> {
        std::vector<u32> r(cnt);
        const u32 elemBytes = (type == 3) ? 2 : 4;
        for (u32 i = 0; i < cnt; ++i) {
            const size_t e = off + i * elemBytes;
            r[i] = (type == 3) ? rd16(e) : rd32(e);
        }
        return r;
    };

    auto strip_offs = readArr(strip_off_tag_off, strip_off_tag_count, strip_off_type);
    auto strip_cnts = readArr(strip_cnt_tag_off, strip_cnt_tag_count, strip_cnt_type);

    for (size_t i = 0; i < strip_offs.size(); ++i) {
        std::span<const u8> strip{buf.data() + strip_offs[i], strip_cnts[i]};
        auto out = lzwDecompress(strip, 8192);
        if (out.size() != 8192) {
            FAIL("strip " << i << " of " << strip_offs.size()
                 << " decoded " << out.size() << " bytes (expected 8192). "
                 << "strip_len=" << strip_cnts[i]);
        }
    }
}
