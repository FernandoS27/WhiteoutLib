// SPDX-License-Identifier: BSD-3-Clause
// Codec-isolated PackBits unit tests for the TIFF parser.

#include <catch2/catch_all.hpp>

#include <span>
#include <vector>

#include "../src/whiteout/textures/tiff/packbits.h"

using namespace whiteout;
using namespace whiteout::textures::tiff;

// ============================================================================
// Phase 2.1 — TIFF 6.0 Appendix C examples
// ============================================================================
//
// Worked example from the TIFF 6.0 spec, Appendix C (PackBits):
// Decompressed: AA AA AA 80 00 2A AA AA AA AA 80 00 2A 22 AA AA AA AA AA AA AA AA AA AA AA AA AA AA AA
// One canonical compression of that sequence (matching the spec):
//   FE AA          run of 3 (= 257 - 0xFE = 3) of byte 0xAA
//   02 80 00 2A    literal of 3 bytes: 80 00 2A
//   FD AA          run of 4 of 0xAA
//   03 80 00 2A 22 literal of 4 bytes: 80 00 2A 22
//   F7 AA          run of 10 of 0xAA
// Each of the cases below isolates ONE of the three PackBits opcodes
// (literal, run, no-op) so a regression in any one is unambiguously named.

TEST_CASE("packbits_test::literal_run_of_three", "[tiff][packbits][phase2]") {
    // header=0x02 -> copy next 3 bytes literally
    const u8 src[] = {0x02, 0x80, 0x00, 0x2A};
    auto out = packBitsDecompress(src);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == 0x80);
    REQUIRE(out[1] == 0x00);
    REQUIRE(out[2] == 0x2A);
}

TEST_CASE("packbits_test::run_of_three", "[tiff][packbits][phase2]") {
    // header=0xFE (-2 in two's complement) -> repeat next byte 3 times
    const u8 src[] = {0xFE, 0xAA};
    auto out = packBitsDecompress(src);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == 0xAA);
    REQUIRE(out[1] == 0xAA);
    REQUIRE(out[2] == 0xAA);
}

TEST_CASE("packbits_test::noop_header_is_skipped", "[tiff][packbits][phase2]") {
    // header=0x80 -> no-op, then literal 0x02 of byte 0x42
    const u8 src[] = {0x80, 0x00, 0x42};
    auto out = packBitsDecompress(src);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0] == 0x42);
}

TEST_CASE("packbits_test::full_appendix_c_round_trip",
          "[tiff][packbits][phase2]") {
    // Concatenation of the five Appendix-C opcodes.
    const u8 src[] = {
        0xFE, 0xAA,                          // run 3 of 0xAA
        0x02, 0x80, 0x00, 0x2A,              // literal 3
        0xFD, 0xAA,                          // run 4 of 0xAA
        0x03, 0x80, 0x00, 0x2A, 0x22,        // literal 4
        0xF7, 0xAA,                          // run 10 of 0xAA
    };
    const u8 expected[] = {
        0xAA, 0xAA, 0xAA,                    // run 3
        0x80, 0x00, 0x2A,                    // literal 3
        0xAA, 0xAA, 0xAA, 0xAA,              // run 4
        0x80, 0x00, 0x2A, 0x22,              // literal 4
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA,        // run 10
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    };
    auto out = packBitsDecompress(src);
    REQUIRE(out.size() == sizeof(expected));
    for (size_t i = 0; i < out.size(); ++i) {
        INFO("byte index " << i);
        REQUIRE(out[i] == expected[i]);
    }
}

TEST_CASE("packbits_test::truncated_literal_returns_empty",
          "[tiff][packbits][phase2]") {
    // header says 4-byte literal but only 2 bytes follow.
    const u8 src[] = {0x03, 0x01, 0x02};
    auto out = packBitsDecompress(src);
    REQUIRE(out.empty());
}

TEST_CASE("packbits_test::truncated_run_returns_empty",
          "[tiff][packbits][phase2]") {
    // header says run-of-N but no byte follows.
    const u8 src[] = {0xFE};
    auto out = packBitsDecompress(src);
    REQUIRE(out.empty());
}
