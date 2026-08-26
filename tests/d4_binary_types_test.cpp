// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Pins the D4 fallback tables recovered from the shipped client against the
// d4data-derived registry.  Every claim below was cross-checked twice while
// the tables were built: once against the 135 SnoContainer_Ctor call sites in
// the executable, once against the CoreTOC of the installed 3.1.1.72836 game.

#include <catch2/catch_all.hpp>

#include "whiteout/sno/d4/binary_types.h"
#include "whiteout/sno/d4/sno_defs.h"

#include <whiteout/sno/sno_types.h>

#include <cstdlib>
#include <set>
#include <string>

using namespace whiteout;
using namespace whiteout::sno;

TEST_CASE("D4 binary types: every group resolves to a real registry type", "[sno][d4][types]") {
    const auto& reg = d4::SnoTypeRegistry::instance();

    size_t resolved = 0;
    std::set<u32> seenTypes;
    for (i32 g = 1; g <= 181; ++g) {
        u32 const th = d4::rootTypeHashForGroup(g);
        if (th == 0)
            continue;
        ++resolved;
        seenTypes.insert(th);

        auto* def = reg.findType(th);
        INFO("group " << g << " type hash " << th);
        REQUIRE(def != nullptr);
        CHECK(def->isBasic == 0);
        // Only root types carry a format hash; a group must bind a root.
        CHECK(def->dwFormatHash != 0);
    }

    CHECK(resolved == d4::kBinaryGroupCount);
    // Each group binds a distinct root type — no two groups share one.
    CHECK(seenTypes.size() == resolved);
}

TEST_CASE("D4 binary types: the groups the name guess cannot resolve", "[sno][d4][types]") {
    const auto& reg = d4::SnoTypeRegistry::instance();

    // The last-resort fallback builds "<snoGroupName()>Definition".  That is
    // right for most groups, but nine of the 135 it cannot reach: six spell
    // the type differently (case, or an outright different name) and three
    // have no snoGroupName() case at all.  Those nine are exactly what the
    // group table buys us, so pin them rather than assume.
    std::set<i32> guessFails;
    for (i32 g = 1; g <= 181; ++g) {
        u32 const th = d4::rootTypeHashForGroup(g);
        if (th == 0)
            continue;
        const char* gn = snoGroupName(static_cast<SnoGroup>(g));
        if (gn == nullptr) {
            guessFails.insert(g);
            continue;
        }
        if (reg.typeHashFromName((std::string(gn) + "Definition").c_str()) != th)
            guessFails.insert(g);
    }

    std::set<i32> const expected = {
        2,   // NpcComponentSet -> NPCComponentSetDefinition
        46,  // UI              -> UIDialogDefinition
        80,  // SubZone         -> SubzoneDefinition
        90,  // StoryBoard      -> StoryboardDefinition
        129, // AbTest          -> ABTestDefinition
        150, // Modal           -> UIModalDefinition
        173, // CrowdTemplates  -> no snoGroupName() case
        174, // CrowdPlacements -> no snoGroupName() case
        181, // Pip             -> no snoGroupName() case
    };
    CHECK(guessFails == expected);

    // ...and the table resolves every one of them.
    for (i32 g : expected) {
        u32 const th = d4::rootTypeHashForGroup(g);
        INFO("group " << g);
        REQUIRE(th != 0);
        REQUIRE(reg.findType(th) != nullptr);
    }

    // Spot-check the two that are genuinely different types, not just spelling.
    auto nameOf = [&](i32 g) {
        auto* d = reg.findType(d4::rootTypeHashForGroup(g));
        return d ? std::string(reg.typeName(*d)) : std::string();
    };
    CHECK(nameOf(46) == "UIDialogDefinition");
    CHECK(nameOf(150) == "UIModalDefinition");
}

TEST_CASE("D4 binary types: format-hash aliases agree with the registry", "[sno][d4][types]") {
    const auto& reg = d4::SnoTypeRegistry::instance();

    size_t agreed = 0, addedByBinary = 0;
    for (i32 g = 1; g <= 181; ++g) {
        u32 const th = d4::rootTypeHashForGroup(g);
        if (th == 0)
            continue;
        u32 const fh = d4::formatHashForGroup(g);
        INFO("group " << g);
        REQUIRE(fh != 0);

        // Whatever the registry already knows about this format hash must not
        // contradict the client.
        u32 const viaRegistry = reg.typeHashFromKey(fh);
        if (viaRegistry != 0) {
            CHECK(viaRegistry == th);
            ++agreed;
        } else {
            // The registry has fallen behind for this group; the alias table
            // is what keeps it resolving.
            CHECK(d4::rootTypeHashForFormatHash(fh) == th);
            ++addedByBinary;
        }
    }
    CHECK(agreed + addedByBinary == d4::kBinaryGroupCount);
    // Known state of the shipped 3.1.1.72836 build: only Global has drifted.
    CHECK(addedByBinary == 1);
}

TEST_CASE("D4 binary types: Global's format hash moved and is now covered", "[sno][d4][types]") {
    const auto& reg = d4::SnoTypeRegistry::instance();

    // The registry (d4data) still carries the older hash; the shipped client
    // and its CoreTOC both use 1018203909.
    constexpr u32 kShippedGlobalFormatHash = 1018203909u;
    constexpr u32 kStaleGlobalFormatHash = 3422516271u;

    CHECK(reg.typeHashFromKey(kShippedGlobalFormatHash) == 0);
    u32 const th = d4::rootTypeHashForFormatHash(kShippedGlobalFormatHash);
    REQUIRE(th != 0);

    auto* def = reg.findType(th);
    REQUIRE(def != nullptr);
    CHECK(std::string(reg.typeName(*def)) == "GlobalDefinition");
    CHECK(reg.typeHashFromKey(kStaleGlobalFormatHash) == th);
    CHECK(d4::rootTypeHashForGroup(21) == th); // group 21 = Global
}

TEST_CASE("D4 binary types: field name hash matches the client", "[sno][d4][types]") {
    // Retail strips field names; the client keeps only this hash at +0 of each
    // 136-byte field entry.  The four values below were read straight out of
    // TextureDefinition's field-array initialiser in 3.1.1.72836 and match the
    // registry's field names for that type in order.
    CHECK(d4::fieldNameHash("sUIStylePreset") == 0x0BDC5815u);
    CHECK(d4::fieldNameHash("eTexFormat") == 0x07A7E95Fu);
    CHECK(d4::fieldNameHash("dwVolumeXSlices") == 0x0098E8AEu);
    CHECK(d4::fieldNameHash("dwVolumeYSlices") == 0x0D93256Fu);

    // Two names one letter apart differ by the same amount the recurrence
    // predicts — 'i'-'a' carried once (x33) minus 'x'-'n'.
    CHECK(d4::fieldNameHash("dwMipMapLevelMin") - d4::fieldNameHash("dwMipMapLevelMax") == 254u);

    // Case matters, and the result always fits 28 bits.
    CHECK(d4::fieldNameHash("dwWidth") != d4::fieldNameHash("dwwidth"));
    CHECK((d4::fieldNameHash("someArbitraryFieldNameThatIsQuiteLong") >> 28) == 0u);
    CHECK(d4::fieldNameHash("") == 0u);
    CHECK(d4::fieldNameHash(nullptr) == 0u);

    // d4data spells fields it never recovered "unk_<hash>" with this same hash,
    // so a registry name in that form decodes straight back to the client's value.
    const auto& reg = d4::SnoTypeRegistry::instance();
    auto* tex = reg.findType(3631735738u); // TextureDefinition
    REQUIRE(tex != nullptr);
    size_t unknamed = 0, decoded = 0;
    for (const auto& f : reg.fields(*tex)) {
        std::string const n = reg.fieldName(f);
        if (n.rfind("unk_", 0) != 0)
            continue;
        ++unknamed;
        decoded += (std::stoul(n.substr(4), nullptr, 16) < (1u << 28));
    }
    CHECK(unknamed == decoded); // vacuously true if TextureDefinition has none
}

TEST_CASE("D4 binary types: D3 version numbers cannot collide with D4 hashes", "[sno][d4][types]") {
    // D3 and D4 use the same header slot for different things — D3 a small
    // struct version, D4 a hash.  SnoReader gates its D4-only fallbacks on
    // formatHash > 0xFFFF; that gate is only sound while no D4 format hash
    // falls that low.  Check the invariant here rather than trusting it: the
    // group table would otherwise hand a D4 root type to a D3 file, and the D4
    // reader running on D3 bytes does not fail cleanly.
    for (i32 g = 1; g <= 181; ++g) {
        u32 const fh = d4::formatHashForGroup(g);
        if (fh == 0)
            continue;
        INFO("group " << g << " format hash " << fh);
        CHECK(fh > 0xFFFFu);
    }

    // The largest version any shipped D3 group reached is in the low hundreds
    // (data/d3_group_versions.json), so nothing in that range may resolve.
    for (u32 v = 1; v <= 512; ++v)
        CHECK(d4::rootTypeHashForFormatHash(v) == 0);
}

TEST_CASE("D4 binary types: unknown keys stay unknown", "[sno][d4][types]") {
    CHECK(d4::rootTypeHashForGroup(0) == 0);
    CHECK(d4::rootTypeHashForGroup(-1) == 0);
    CHECK(d4::rootTypeHashForGroup(9999) == 0);
    CHECK(d4::formatHashForGroup(9999) == 0);
    CHECK(d4::rootTypeHashForFormatHash(0) == 0);
    CHECK(d4::rootTypeHashForFormatHash(0xFFFFFFFFu) == 0);

    // Groups with an extension but no container in the client (tools-only or
    // retired) must not be invented.
    CHECK(d4::rootTypeHashForGroup(159) == 0);
    CHECK(d4::rootTypeHashForGroup(163) == 0);
    CHECK(d4::rootTypeHashForGroup(171) == 0);
}
