// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Gate on the StringList parser (the display-name half of the D3 wardrobe).
///
/// The snapshot under `Corpus/D3/StringList` is the five item-facing `.stl`
/// files of a 2.8.0.99920 client -- the complete population this layer
/// consumes -- so the entry counts below are exact, not samples. The record
/// is the 40-byte 2469 entry (the Switch reflection's 24-byte one is the odd
/// one out, exactly as with GameBalance's Item); see the snapshot README.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/d3/native/character.h>
#include <whiteout/sno/d3/native/d3_native.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace nat = whiteout::sno::d3::native;
using whiteout::u32;
using whiteout::u8;

namespace {

std::vector<u8> readWhole(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    std::vector<u8> b(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(sz));
    return b;
}

fs::path stringListDir() {
    for (auto c : {"Corpus/D3/StringList", "../Corpus/D3/StringList",
                   "../../Corpus/D3/StringList"})
        if (fs::is_directory(c)) return c;
    return {};
}

/// Every snapshot file with its SNO id and exact entry count, measured at
/// snapshot time.
struct StlFixture {
    const char* file;
    whiteout::i32 snoId;
    size_t entries;
};
constexpr StlFixture kStls[] = {
    {"Items.stl", 52008, 6600},         {"ItemSets.stl", 123197, 129},
    {"ItemSlots.stl", 77407, 24},       {"ItemTypeNames.stl", 52009, 236},
    {"ItemQuality.stl", 77010, 17},
};

std::string_view valueOf(const nat::StringList& stl, std::string_view key) {
    for (const auto& e : stl.arEntries)
        if (nat::stlText(e.szKey) == key) return nat::stlText(e.szValue);
    return {};
}

} // namespace

TEST_CASE("d3_stringlist_snapshot_parses", "[d3][stringlist]") {
    const fs::path dir = stringListDir();
    REQUIRE(!dir.empty());

    for (const auto& fx : kStls) {
        CAPTURE(fx.file);
        const auto bytes = readWhole(dir / fx.file);
        REQUIRE(!bytes.empty());
        const auto stl = nat::parseStringList(bytes);
        REQUIRE(stl.has_value());
        CHECK(stl->dwSnoId == fx.snoId);
        REQUIRE(stl->arEntries.size() == fx.entries);

        for (const auto& e : stl->arEntries) {
            const auto key = nat::stlText(e.szKey);
            // Every span is NUL-terminated text; stlText must strip exactly
            // that terminator (a key that keeps its NUL fails every equality
            // against the spelling the cross-referencing record carries).
            REQUIRE(!key.empty());
            CHECK(key.find('\0') == std::string_view::npos);
            CHECK(nat::stlText(e.szValue).find('\0') == std::string_view::npos);
            // The entry's own integrity word: the case-SENSITIVE Str_Hash33
            // of the key. Pins both the field decode and the hash identity.
            CHECK(static_cast<u32>(e.dwKeyHash) == nat::lookNameHash33(key));
        }
    }
}

TEST_CASE("d3_stringlist_display_names", "[d3][stringlist]") {
    const fs::path dir = stringListDir();
    REQUIRE(!dir.empty());

    const auto items = nat::parseStringList(readWhole(dir / "Items.stl"));
    REQUIRE(items.has_value());
    CHECK(valueOf(*items, "Unique_Chest_001") == "Goldskin");
    CHECK(valueOf(*items, "Unique_Sword_1H_017") == "Monster Hunter");

    const auto sets = nat::parseStringList(readWhole(dir / "ItemSets.stl"));
    REQUIRE(sets.has_value());
    CHECK(valueOf(*sets, "Earthquake_Set_x1") == "Might of the Earth");
    CHECK(valueOf(*sets, "Thorns_Set_x1") == "Thorns of the Invoker");

    // The type-name table is the complete client-side ItemType name space:
    // its keys hash (gbidHash) onto the `gbidItemType` values GameBalance
    // records carry, including the class-suffixed armour types that encode
    // the game's per-class item restriction.
    const auto types = nat::parseStringList(readWhole(dir / "ItemTypeNames.stl"));
    REQUIRE(types.has_value());
    CHECK(valueOf(*types, "ChestArmor_Wizard") == "Chest Armor");
    bool sawSpiritStone = false;
    for (const auto& e : types->arEntries)
        sawSpiritStone |= nat::stlText(e.szKey) == "SpiritStone_Monk";
    CHECK(sawSpiritStone);
}
