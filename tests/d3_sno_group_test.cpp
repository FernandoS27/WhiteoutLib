// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Gate on the two enumerations a Diablo III field's `enumTableId` can name.
///
/// `TypeDesc_RegisterField_*_EnumTable` stores that id at `FieldDescriptor+80`
/// for every field type, and its meaning depends on the field's declared type:
/// the SNO group for a `DT_SNO` reference, the GameBalance group for a `DT_GBID`.
/// Both tables are generated from the game's own data (`g_SnoGroupExtensions`,
/// `g_GameBalanceGroupNames`), so the checks below are consistency gates on the
/// generator, not restatements of hand-written constants.
///
/// The load-bearing one is `sno_field_groups_name_a_real_group`: it is what would
/// catch the generator emitting a raw `enumTableId` it had not validated.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/sno_reader.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../src/whiteout/sno/d3/sno_defs.h"

using whiteout::sno::d3::SnoTypeRegistry;

TEST_CASE("d3_sno_groups_are_complete", "[d3][sno]") {
    const auto& reg = SnoTypeRegistry::instance();
    const auto groups = reg.snoGroups();

    // 66 handlers across the id space 1..69; 16, 30 and 50 are holes.
    REQUIRE(groups.size() == 66);

    std::set<whiteout::u32> ids;
    for (const auto& g : groups) {
        INFO("group " << g.id << " " << (g.name ? g.name : "(null)"));
        REQUIRE(g.id >= 1);
        REQUIRE(g.id <= 69);
        REQUIRE(g.name != nullptr);
        REQUIRE(std::string(g.name).size() > 0);
        // Every registered group stores files, so every one has an extension.
        REQUIRE(g.extension != nullptr);
        REQUIRE(std::string(g.extension).size() >= 3);
        REQUIRE(g.extension[0] == '.');
        REQUIRE(g.structSize > 0);
        REQUIRE(ids.insert(g.id).second); // ids are unique
    }
    for (whiteout::u32 hole : {16u, 30u, 50u})
        REQUIRE(ids.count(hole) == 0);

    // Spot-check the groups the actor render path walks (guide sections 4-6, 15).
    struct Expect { whiteout::u32 id; const char* name; const char* ext; whiteout::u32 size; };
    for (const Expect e : {Expect{1, "Actor", ".acr", 448},
                           Expect{6, "Anim", ".ani", 56},
                           Expect{8, "AnimSet", ".ans", 480},
                           Expect{9, "Appearance", ".app", 552},
                           Expect{57, "Material", ".mat", 136},
                           Expect{61, "PhysMesh", ".phm", 48},
                           Expect{67, "AnimTree", ".ant", 96}}) {
        const auto* g = reg.snoGroup(e.id);
        REQUIRE(g != nullptr);
        CHECK(std::string(g->name) == e.name);
        CHECK(std::string(g->extension) == e.ext);
        CHECK(g->structSize == e.size);
    }

    // Only Hero and Account register isSingleton=1 -- which is why `.hro` cannot
    // be the per-class definition (that is GameBalance group 7, Heros).
    std::set<whiteout::u32> singletons;
    for (const auto& g : groups)
        if (g.singleton) singletons.insert(g.id);
    CHECK(singletons == std::set<whiteout::u32>{10u, 53u});

    CHECK(reg.snoGroup(16) == nullptr);
    CHECK(reg.snoGroup(70) == nullptr);
}

TEST_CASE("d3_gamebalance_groups_match_the_runtime_tables", "[d3][sno]") {
    const auto& reg = SnoTypeRegistry::instance();
    const auto groups = reg.gameBalanceGroups();
    REQUIRE(groups.size() == 35);

    size_t named = 0, withTable = 0;
    for (const auto& g : groups) {
        INFO("gb group " << g.id << " " << (g.name ? g.name : "(unnamed)"));
        if (g.name) ++named;
        if (g.hasRuntimeTable) {
            ++withTable;
            // A group the runtime can index must have both halves of the lookup:
            // GameBalance_LookupRecord needs the stride, and the array offset
            // inside the 568-byte GameBalance root.
            CHECK(g.stride > 0);
            CHECK(g.rootOffset > 0);
            CHECK(g.rootOffset < 568);
        } else {
            CHECK(g.stride == 0);
        }
    }
    CHECK(named == 34);     // 48 is the one group with no name
    CHECK(withTable == 33); // ...and Scenery (17) the one name with no table

    // The two asymmetries, asserted so a re-extraction that smooths them over fails.
    const auto* scenery = reg.gameBalanceGroup(17);
    REQUIRE(scenery != nullptr);
    CHECK(std::string(scenery->name) == "Scenery");
    CHECK_FALSE(scenery->hasRuntimeTable);

    const auto* unnamed48 = reg.gameBalanceGroup(48);
    REQUIRE(unnamed48 != nullptr);
    CHECK(unnamed48->name == nullptr);
    CHECK(unnamed48->rootOffset == 80);
    CHECK_FALSE(unnamed48->hasRuntimeTable);

    // Items: what GameBalance_ResolveActorSno reads for kind == 2.
    const auto* items = reg.gameBalanceGroup(2);
    REQUIRE(items != nullptr);
    CHECK(std::string(items->name) == "Items");
    CHECK(items->stride == 696);

    // Heros: the player class definition, 264 bytes per row.
    const auto* heros = reg.gameBalanceGroup(7);
    REQUIRE(heros != nullptr);
    CHECK(std::string(heros->name) == "Heros");
    CHECK(heros->stride == 264);

    CHECK(reg.gameBalanceGroup(47) == nullptr); // used by 9 fields, unnamed here
    CHECK(reg.gameBalanceGroup(99) == nullptr);
}

TEST_CASE("sno_field_groups_name_a_real_group", "[d3][sno]") {
    const auto& reg = SnoTypeRegistry::instance();

    // Every field that carries a group must point at one of the two enums.  A
    // field cannot be checked against a *specific* enum here because the reader's
    // type vocabulary folds DT_GBID into an integer, so accept either -- the
    // point is that no unvalidated `enumTableId` reaches the table.
    size_t withGroup = 0;
    for (const auto& def : reg.types()) {
        for (const auto& f : reg.fields(def)) {
            if (f.group < 0) continue;
            ++withGroup;
            const auto id = static_cast<whiteout::u32>(f.group);
            INFO("type " << reg.typeName(def) << " field +" << f.offset
                         << " group " << f.group);
            CHECK((reg.snoGroup(id) != nullptr || reg.gameBalanceGroup(id) != nullptr));
        }
    }
    // Was 0 before the group wiring; guards against a silent regression to -1.
    CHECK(withGroup > 250);
}

// ---------------------------------------------------------------------------
// End-to-end: a parsed reference should now carry the group it points at.
//
// `SnoRef::group` comes from `SnoFieldDef::group`, so this is the check that the
// wiring survives all the way from the binary's reflection data to a value handed
// back by the reader -- which is what `d3_native.cpp`'s resolve() needs in order
// to load a dependency at all.  Before the group was wired every ref came back -1.
// ---------------------------------------------------------------------------

namespace {
namespace fs = std::filesystem;

std::vector<whiteout::u8> readWholeFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    std::vector<whiteout::u8> buf(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

void collectRefs(const whiteout::sno::SnoValue& v, std::map<int, size_t>& out,
                 size_t& total, int depth = 0) {
    using namespace whiteout::sno;
    if (depth > 8) return;
    if (v.isRef()) {
        ++total;
        ++out[v.asRef().group];
        return;
    }
    if (v.isObject()) {
        for (const auto& [k, sub] : v.asObject()) collectRefs(sub, out, total, depth + 1);
        return;
    }
    if (v.isArray()) {
        const SnoArray& arr = v.asArray();
        if (arr.isRef()) {
            for (const auto& r : arr.asRefData()) { ++total; ++out[r.group]; }
        } else if (arr.isArray() || arr.isObject()) {
            for (const auto& e : arr.asValueData()) collectRefs(e, out, total, depth + 1);
        }
    }
}
} // namespace

TEST_CASE("d3_parsed_refs_carry_their_group", "[d3][sno][corpus]") {
    fs::path dir;
    for (auto candidate : {"Corpus/D3/Actor", "../Corpus/D3/Actor", "../../Corpus/D3/Actor"})
        if (fs::is_directory(candidate)) { dir = candidate; break; }
    if (dir.empty()) SKIP("D3 Actor corpus not found");

    whiteout::sno::SnoReader reader;
    std::map<int, size_t> byGroup;
    size_t total = 0, files = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (files >= 200) break;
        if (!e.is_regular_file()) continue;
        auto data = readWholeFile(e.path());
        if (data.size() < 16) continue;
        auto file = reader.parse(data, static_cast<whiteout::sno::SnoGroup>(1));
        if (!file) continue;
        ++files;
        collectRefs(file->root, byGroup, total);
    }
    REQUIRE(files > 0);
    REQUIRE(total > 0);

    INFO("parsed " << files << " actors, " << total << " refs");
    for (const auto& [g, n] : byGroup)
        UNSCOPED_INFO("  group " << g << " -> " << n << " ref(s)");

    // Every group an actor's refs report must be a real one.  -1 is allowed: a
    // v282 Actor has fields the current reflection revision does not describe.
    for (const auto& [g, n] : byGroup) {
        if (g < 0) continue;
        INFO("group " << g << " on " << n << " ref(s)");
        CHECK(SnoTypeRegistry::instance().snoGroup(static_cast<whiteout::u32>(g)) != nullptr);
    }

    // The Actor -> Appearance edge is the one the whole render path hangs off
    // (guide section 15.3), so require it specifically rather than settling for
    // "some ref had a group".
    CHECK(byGroup.count(9) > 0);
    const size_t withGroup = total - byGroup[-1];
    CHECK(withGroup > 0);
}
