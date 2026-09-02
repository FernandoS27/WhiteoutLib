// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Gate on the GameBalance parser (D3-G7's WhiteoutLib half).
///
/// The snapshot under `Corpus/D3/GameBalance` is every `.gam` a 2.8.0.99920
/// client ships -- all twenty -- so unlike the other D3 corpora this one is
/// the COMPLETE population, and the counts below are exact, not samples.
///
/// What is deliberately absent: the plan's ItemType parent-chain check. The
/// 2.8 client ships no ItemTypes table at all (CoreTOC group 20 lists exactly
/// the twenty snapshot files); item types exist client-side only as the
/// `gbidItemType` hash on each item, which is why the checks below pin hashes
/// of known type NAMES instead of walking records that do not exist.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/d3/native/character.h>
#include <whiteout/sno/d3/native/d3_native.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace nat = whiteout::sno::d3::native;
using whiteout::i32;
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

fs::path gameBalanceDir() {
    for (auto c : {"Corpus/D3/GameBalance", "../Corpus/D3/GameBalance",
                   "../../Corpus/D3/GameBalance"})
        if (fs::is_directory(c)) return c;
    return {};
}

/// The Items tables and their exact record counts, measured once at snapshot
/// time. eGameBalanceType == 2 for all four; every other snapshot file holds a
/// different table type and must parse with an EMPTY arItems.
struct ItemsFixture {
    const char* file;
    size_t records;
};
constexpr ItemsFixture kItemsTables[] = {
    {"Items_Legendary.gam", 3181},
    {"Item_ArtTest.gam", 355},
    {"Items_Quests_Beta.gam", 131},
    {"Items_RandomPlaceholder.gam", 18},
    {"Items_PagesOfFate.gam", 1},
};

} // namespace

TEST_CASE("d3_gbid_hash", "[d3][gamebalance]") {
    // Pinned against Str_HashLower33 (retail Windows 0x54BF00) evaluated by
    // hand: seed 0, h*33 + tolower(c).
    CHECK(nat::gbidHash("") == 0u);
    CHECK(nat::gbidHash("a") == u32('a'));
    CHECK(nat::gbidHash("A") == u32('a'));            // case-insensitive
    CHECK(nat::gbidHash("Dagger") == 0xF059666Au);    // a shipped gbidItemType
    CHECK(nat::gbidHash("dagger") == nat::gbidHash("DAGGER"));
}

TEST_CASE("d3_gamebalance_snapshot_parses", "[d3][gamebalance]") {
    const fs::path dir = gameBalanceDir();
    if (dir.empty()) {
        WARN("Corpus/D3/GameBalance not present; skipping");
        return;
    }

    size_t files = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".gam") continue;
        ++files;
        const auto data = readWhole(e.path());
        auto gb = nat::parseGameBalance(data);
        REQUIRE(gb.has_value());
        // Every snapshot file's header id is its CASC file id; non-positive
        // would mean the header was read from the wrong offset.
        CHECK(gb->dwSnoId > 0);
        // arItems is the type-2 container. Populated iff the selector says so;
        // a record in any other table type here would mean the parser read a
        // container that file does not use.
        if (gb->eGameBalanceType == 2)
            CHECK(!gb->arItems.empty());
        else
            CHECK(gb->arItems.empty());
    }
    CHECK(files == 20);
}

TEST_CASE("d3_gamebalance_items", "[d3][gamebalance]") {
    const fs::path dir = gameBalanceDir();
    if (dir.empty()) {
        WARN("Corpus/D3/GameBalance not present; skipping");
        return;
    }

    size_t total = 0, withActor = 0;
    std::set<u32> typeGbids;
    for (const auto& fx : kItemsTables) {
        const auto data = readWhole(dir / fx.file);
        auto gb = nat::parseGameBalance(data);
        REQUIRE(gb.has_value());
        CHECK(gb->eGameBalanceType == 2);
        CHECK(gb->arItems.size() == fx.records);

        std::set<std::string> names;
        for (const auto& item : gb->arItems) {
            ++total;
            // A name is the record's identity: non-empty, ASCII, and unique
            // within its table. A zero-length or binary-garbage name means the
            // 1408-byte stride drifted.
            REQUIRE(!item.szName.empty());
            for (const char c : item.szName)
                CHECK((c >= 0x20 && c < 0x7F));
            CHECK(names.insert(item.szName).second);

            if (item.snoActor.id > 0) ++withActor;
            if (item.gbidItemType != 0 && item.gbidItemType != 0xFFFFFFFFu)
                typeGbids.insert(item.gbidItemType);
        }
    }
    CHECK(total == 3686);
    // Measured at snapshot time; a drop means snoActor moved off +264.
    CHECK(withActor == 3491);

    // The type references are hashes of type names. The client ships no
    // ItemTypes records to compare against, but the hash function plus a known
    // name reproduces the reference -- which validates gbidHash, the szName
    // read and the gbidItemType read against each other.
    CHECK(typeGbids.size() == 131);
    for (const char* n : {"Ring", "Helm", "Boots", "Bracers", "Amulet", "Sword",
                          "ChestArmor", "Legs", "Gloves", "Shoulders",
                          "FistWeapon", "Sword2H", "Shield", "CrusaderShield",
                          "Mojo", "Orb", "Quiver", "VoodooMask", "WizardHat",
                          "Dagger"})
        CHECK(typeGbids.count(nat::gbidHash(n)) == 1);
}

TEST_CASE("d3_gamebalance_item_actor_join", "[d3][gamebalance]") {
    const fs::path dir = gameBalanceDir();
    if (dir.empty()) {
        WARN("Corpus/D3/GameBalance not present; skipping");
        return;
    }
    // The corpus Actor files are name-keyed on disk and id-keyed inside; the
    // dressing pipeline joins item -> actor by the id. One verified pair
    // (CoreTOC: 195174 = Dagger_norm_unique_03) proves the join end to end.
    const auto data = readWhole(dir / "Items_Legendary.gam");
    auto gb = nat::parseGameBalance(data);
    REQUIRE(gb.has_value());

    const nat::Item* dagger = nullptr;
    for (const auto& item : gb->arItems)
        if (item.szName == "Unique_Dagger_003") dagger = &item;
    REQUIRE(dagger != nullptr);
    CHECK(dagger->snoActor.id == 195174);
    CHECK(dagger->gbidItemType == nat::gbidHash("Dagger"));

    fs::path actorDir;
    for (auto c : {"Corpus/D3/Actor", "../Corpus/D3/Actor", "../../Corpus/D3/Actor"})
        if (fs::is_directory(c)) actorDir = c;
    if (actorDir.empty()) {
        WARN("Corpus/D3/Actor not present; join checked by id only");
        return;
    }
    const auto actorBytes = readWhole(actorDir / "Dagger_norm_unique_03.acr");
    REQUIRE(!actorBytes.empty());
    auto actor = nat::parseActor(actorBytes);
    REQUIRE(actor.has_value());
    CHECK(actor->dwSnoId == dagger->snoActor.id);
}
