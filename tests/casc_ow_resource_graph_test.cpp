// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_ow_resource_graph_test: TRG header and block layout.
///
/// The skin block is a chain of variable-length records, so getting a skin
/// wrong desynchronises everything after it. These build graphs by hand and
/// check both that a good one reads back exactly and that the ways a bad one
/// goes wrong are all caught.

#include "../src/whiteout/storages/casc/roots/ow/ow_resource_graph.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;
namespace ow = whiteout::storages::casc::ow;

namespace {

constexpr u32 kPlainMagic = 0x0D747267;

void put32(std::vector<u8>& v, size_t off, u32 value) {
    v[off] = u8(value);
    v[off + 1] = u8(value >> 8);
    v[off + 2] = u8(value >> 16);
    v[off + 3] = u8(value >> 24);
}

void put64(std::vector<u8>& v, size_t off, u64 value) {
    put32(v, off, u32(value));
    put32(v, off + 4, u32(value >> 32));
}

struct Skin {
    u64 guid;
    std::vector<std::pair<u64, u64>> entries;
};

/// A plaintext graph with the given packages and skins, and empty graph and
/// type-bundle-index blocks.
std::vector<u8> buildTrg(const std::vector<u64>& packages, const std::vector<Skin>& skins) {
    size_t skinBytes = 0;
    for (auto const& s : skins)
        skinBytes += 40 + s.entries.size() * 32;

    std::vector<u8> out(ow::kTrgHeaderSize + packages.size() * 30 + skinBytes, 0);
    put32(out, 0x04, 152736);
    put32(out, 0x18, u32(packages.size()));
    put32(out, 0x1C, u32(packages.size() * 30));
    put32(out, 0x20, u32(skins.size()));
    put32(out, 0x24, u32(skinBytes));
    put32(out, 0x2C, 0);
    put32(out, 0x3C, 0);
    put32(out, 0x40, 0);
    put32(out, 0x50, kPlainMagic);

    size_t off = ow::kTrgHeaderSize;
    for (u64 guid : packages) {
        put64(out, off, guid);
        put32(out, off + 0x10, 0x40ABCDEF);
        put32(out, off + 0x14, 7);
        off += 30;
    }
    for (auto const& s : skins) {
        put64(out, off, 40);
        put64(out, off + 8, s.guid);
        put32(out, off + 0x18, 0x40001234);
        out[off + 0x24] = u8(s.entries.size());
        out[off + 0x25] = u8(s.entries.size() >> 8);
        size_t eo = off + 40;
        for (auto const& [source, target] : s.entries) {
            put64(out, eo, source);
            put64(out, eo + 8, target);
            eo += 32;
        }
        off += 40 + s.entries.size() * 32;
    }
    return out;
}

} // namespace

TEST_CASE("TRG header reads the block table", "[casc][ow_trg]") {
    auto data = buildTrg({0x0250000000001111ULL}, {});

    ow::TrgHeader header;
    REQUIRE(ow::parseTrgHeader(data, header));
    CHECK(header.buildVersion == 152736);
    CHECK(header.packageCount == 1);
    CHECK(header.packageBytes == 30);
    CHECK(header.skinCount == 0);
    CHECK_FALSE(header.encrypted);
    CHECK_FALSE(header.patch);
}

// The stored magic is byte-reversed while the body is encrypted, which is the
// only signal that it is.
TEST_CASE("TRG header spots an encrypted body", "[casc][ow_trg]") {
    auto data = buildTrg({}, {});
    put32(data, 0x50, 0x6772740D);

    ow::TrgHeader header;
    REQUIRE(ow::parseTrgHeader(data, header));
    CHECK(header.encrypted);
}

TEST_CASE("TRG header rejects a foreign magic", "[casc][ow_trg]") {
    auto data = buildTrg({}, {});
    put32(data, 0x50, 0x12345678);

    ow::TrgHeader header;
    CHECK_FALSE(ow::parseTrgHeader(data, header));

    std::vector<u8> truncated(ow::kTrgHeaderSize - 1, 0);
    CHECK_FALSE(ow::parseTrgHeader(truncated, header));
}

TEST_CASE("TRG parses packages and skins", "[casc][ow_trg]") {
    std::vector<Skin> skins = {
        {0x0968000000000ABCULL, {{0x0300000000002860ULL, 0x03000000000047FFULL}}},
        {0x0A50000000001234ULL,
         {{0x04D0000000003750ULL, 0x04D0000000005A1CULL},
          {0x0C0000000000135BULL, 0x0C00000000007DE0ULL},
          {0x0300000000002862ULL, 0x0300000000004E05ULL}}},
        {0x0A50000000005678ULL, {}},
    };
    auto data = buildTrg({0x0250000000001111ULL, 0x0250000000002222ULL}, skins);

    auto graph = ow::parseResourceGraph(data, "win_spwin_rcn_eext.trg");
    REQUIRE(graph.has_value());

    REQUIRE(graph->packages.size() == 2);
    CHECK(graph->packages[0].guid == 0x0250000000001111ULL);
    CHECK(graph->packages[1].guid == 0x0250000000002222ULL);
    CHECK(graph->packages[0].graphRef == 0x40ABCDEF);

    REQUIRE(graph->skins.size() == 3);
    CHECK(graph->skins[0].guid == 0x0968000000000ABCULL);
    CHECK(graph->skins[0].graphRef == 0x40001234);
    REQUIRE(graph->skins[0].entries.size() == 1);
    CHECK(graph->skins[0].entries[0].source == 0x0300000000002860ULL);
    CHECK(graph->skins[0].entries[0].target == 0x03000000000047FFULL);

    // A skin with three entries has to leave the walk on the next record, not
    // three entries short of it.
    REQUIRE(graph->skins[1].entries.size() == 3);
    CHECK(graph->skins[1].entries[2].target == 0x0300000000004E05ULL);

    // An empty skin is a real thing and must not end the block early.
    CHECK(graph->skins[2].entries.empty());
    CHECK(graph->skins[2].guid == 0x0A50000000005678ULL);
}

TEST_CASE("TRG rejects a skin block that does not close", "[casc][ow_trg]") {
    std::vector<Skin> skins = {
        {0x0968000000000ABCULL, {{0x0300000000002860ULL, 0x03000000000047FFULL}}},
        {0x0A50000000001234ULL, {{0x04D0000000003750ULL, 0x04D0000000005A1CULL}}},
    };

    SECTION("an entry count that overruns the block") {
        auto data = buildTrg({}, skins);
        data[ow::kTrgHeaderSize + 0x24] = 0xFF;
        CHECK_FALSE(ow::parseResourceGraph(data, "x.trg").has_value());
    }

    SECTION("an entry count that leaves the block short") {
        auto data = buildTrg({}, skins);
        data[ow::kTrgHeaderSize + 0x24] = 0;
        CHECK_FALSE(ow::parseResourceGraph(data, "x.trg").has_value());
    }

    SECTION("a skin count the block cannot supply") {
        auto data = buildTrg({}, skins);
        put32(data, 0x20, 5);
        CHECK_FALSE(ow::parseResourceGraph(data, "x.trg").has_value());
    }
}

TEST_CASE("TRG rejects a header whose blocks do not fit", "[casc][ow_trg]") {
    SECTION("a package count that disagrees with the block size") {
        auto data = buildTrg({0x0250000000001111ULL}, {});
        put32(data, 0x18, 2);
        CHECK_FALSE(ow::parseResourceGraph(data, "x.trg").has_value());
    }

    SECTION("a block table larger than the file") {
        auto data = buildTrg({}, {});
        put32(data, 0x40, 1u << 20);
        CHECK_FALSE(ow::parseResourceGraph(data, "x.trg").has_value());
    }

    SECTION("a block table that leaves more than a padding block spare") {
        auto data = buildTrg({0x0250000000001111ULL}, {});
        data.resize(data.size() + 16);
        CHECK_FALSE(ow::parseResourceGraph(data, "x.trg").has_value());
    }
}
