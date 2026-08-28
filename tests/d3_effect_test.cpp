// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Corpus test for D3 EffectGroup (.efg) files.
///
/// EffectGroup is the route the shipped particles arrive by -- 11,849 of the
/// 21,593 .prt are named by an .efg and by nothing else -- so a misread here
/// silently loses most of the game's effects rather than failing loudly.
///
/// Every claim checked below is a claim the format spec makes, so this is the
/// gate that keeps docs/D3 Specs/EFG_FILE_FORMAT_SPECIFICATION.md honest:
///
///   * the file closes exactly:      size == 16 + 120 + SerializeData.size
///   * the element stride is 480:    SerializeData.size == 480 * count
///   * the resolved array agrees with the count the struct carries
///   * eSelectMode only ever takes a value EffectGroup_Play switches on
///   * dwPlayedItemMask is runtime scratch and is zero in every shipped file
///   * every item's eMessageType is 5000 -- inside an .efg the group itself is
///     the trigger, which is what distinguishes it from the same 412-byte
///     record inside an .acr
///   * szLookLink is read only by select mode 10, and only mode-10 files set it
///   * nRepeatMin/nRepeatMax are read only by select mode 1

#include <catch2/catch_all.hpp>

#include <whiteout/sno/d3/native/d3_native.h>

// White-box, like d3_native_test: the checks that prove the parsed values came
// from the right bytes read the on-disk image directly, so this target carries
// src/ on its include path (tests/CMakeLists.txt).
#include "whiteout/sno/d3/native/layout.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace whiteout;
namespace nat = whiteout::sno::d3::native;

static std::vector<u8> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    std::vector<u8> buf(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

TEST_CASE("D3 EFG corpus", "[d3][efg][corpus]") {
    fs::path dir;
    for (auto c : {"Corpus/D3/EffectGroup", "../Corpus/D3/EffectGroup",
                   "../../Corpus/D3/EffectGroup"})
        if (fs::is_directory(c)) { dir = c; break; }
    if (dir.empty()) SKIP("D3 EffectGroup corpus not found");

    // The cases EffectGroup_Play (0x710008B230) switches on.  4 and 6 are not
    // among them, and no shipped file uses them either.
    const std::set<i32> kSelectModes = {0, 1, 2, 3, 5, 7, 8, 9, 10, 11, 12, 13,
                                        14, 15, 16, 17};

    size_t files = 0, parsed = 0, badSize = 0, badClose = 0, badStride = 0;
    size_t badCount = 0, badMode = 0, maskWords = 0, maskNonZero = 0;
    size_t items = 0, badMessageType = 0, emptyGroups = 0, duplicateIds = 0;
    size_t mode10 = 0, mode10WithLook = 0, otherFiles = 0, otherWithLook = 0;
    size_t mode1 = 0, mode1WithRepeat = 0, otherWithRepeat = 0;
    size_t powerSet = 0;
    std::set<i32> seenIds;
    std::map<i32, size_t> modeCensus, flagCensus, chanceCensus;

    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".efg") continue;
        ++files;
        auto data = readFile(e.path());
        if (data.size() < 16 + sizeof(nat::layout::EffectGroup)) { ++badSize; continue; }

        auto eg = nat::parseEffectGroup(data);
        if (!eg) continue;
        ++parsed;

        const auto& img =
            *reinterpret_cast<const nat::layout::EffectGroup*>(data.data() + 16);

        // The items are the whole file past the root, so the three sizes have
        // to agree: nothing else lives in an .efg.
        if (data.size() != 16 + sizeof(nat::layout::EffectGroup) +
                               static_cast<size_t>(img.arEffectItems_size))
            ++badClose;
        if (img.arEffectItems_size != 480 * img.dwEffectItemCount) ++badStride;
        if (static_cast<i64>(eg->arEffectItems.size()) != eg->dwEffectItemCount)
            ++badCount;
        if (eg->arEffectItems.empty()) ++emptyGroups;

        if (!seenIds.insert(eg->dwSnoId).second) ++duplicateIds;
        if (eg->snoPower.valid()) ++powerSet;
        if (!kSelectModes.count(eg->eSelectMode)) ++badMode;
        ++modeCensus[eg->eSelectMode];
        ++flagCensus[eg->dwFlags];

        for (u32 w : img.dwPlayedItemMask) {
            ++maskWords;
            if (w != 0) ++maskNonZero;
        }

        bool anyLook = false;
        for (const auto& it : eg->arEffectItems) {
            ++items;
            if (it.tEvent.eMessageType != 5000) ++badMessageType;
            ++chanceCensus[it.tEvent.tEvent.tConditions.nChance];
            if (!it.szLookLink.empty()) anyLook = true;
        }

        if (eg->eSelectMode == 10) {
            ++mode10;
            if (anyLook) ++mode10WithLook;
        } else {
            ++otherFiles;
            if (anyLook) ++otherWithLook;
        }

        const bool repeats = eg->nRepeatMin != 0 || eg->nRepeatMax != 0;
        if (eg->eSelectMode == 1) {
            ++mode1;
            if (repeats) ++mode1WithRepeat;
        } else if (repeats) {
            ++otherWithRepeat;
        }
    }

    std::cout << "\n=== D3 EFG corpus ===\n"
              << "files: " << files << "   parsed: " << parsed
              << "   too small: " << badSize << "\n"
              << "file size != 16+120+SerializeData.size: " << badClose << "  (must be 0)\n"
              << "SerializeData.size != 480*count:        " << badStride << "  (must be 0)\n"
              << "resolved array size != count:           " << badCount << "  (must be 0)\n"
              << "eSelectMode outside the switch:         " << badMode << "  (must be 0)\n"
              << "item eMessageType != 5000:              " << badMessageType << "  (must be 0)\n"
              << "dwPlayedItemMask non-zero words:        " << maskNonZero << " of "
              << maskWords << "  (must be 0)\n"
              << "duplicate dwSnoId:                      " << duplicateIds << "  (must be 0)\n"
              << "items: " << items << "   empty groups: " << emptyGroups
              << "   snoPower set: " << powerSet << "\n"
              << "szLookLink set: mode 10 " << mode10WithLook << "/" << mode10
              << "   every other mode " << otherWithLook << "/" << otherFiles << "\n"
              << "nRepeatMin/Max set: mode 1 " << mode1WithRepeat << "/" << mode1
              << "   every other mode " << otherWithRepeat << "\n";

    std::cout << "eSelectMode:";
    for (auto& [m, n] : modeCensus) std::cout << "  " << m << ":" << n;
    std::cout << "\ndwFlags:";
    for (auto& [f, n] : flagCensus) std::cout << "  " << f << ":" << n;
    std::cout << "\nnChance:";
    for (auto& [c, n] : chanceCensus) std::cout << "  " << c << ":" << n;
    std::cout << "\n";

    CHECK(files > 0);
    CHECK(parsed == files);
    CHECK(badSize == 0);
    CHECK(badClose == 0);
    CHECK(badStride == 0);
    CHECK(badCount == 0);
    CHECK(badMode == 0);
    CHECK(badMessageType == 0);
    CHECK(maskNonZero == 0);
    CHECK(duplicateIds == 0);
    CHECK(items > 0);
    // 4 and 6 are the two holes in the switch; a file using either would mean
    // the mode is being read from the wrong word.
    CHECK(modeCensus.count(4) == 0);
    CHECK(modeCensus.count(6) == 0);
    // Every mode-10 file sets the look link its own case reads, and almost no
    // other file does -- the field's meaning, stated as a corpus fact.
    CHECK(mode10WithLook == mode10);
    CHECK(otherWithLook * 100 < otherFiles);
    // Same shape for the repeat range, which only case 1 reads.
    CHECK(mode1WithRepeat == mode1);
    CHECK(otherWithRepeat * 100 < otherFiles);
}
