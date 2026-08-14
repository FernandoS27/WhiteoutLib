// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Payload regression test for Diablo III SNO files.
///
/// D3 has no external payload file.  Its payload bytes live in the same buffer,
/// immediately after the struct image, and the engine reads them with the same
/// Read(stream, offset, size, dst) call it uses for the struct itself
/// (SNOGroupHandler_OnAssetLoaded_base in the 2.6.2 Switch build).
///
/// It therefore follows that a D3 parse must NEVER yield an `__external__`
/// marker.  It used to: `isExternalField()` tested the D4 flag bits
/// 0x200000/0x400000, but in D3 those bits are conditional-serialization
/// gate/polarity pairs (see TypeDesc_FieldPassesSerializationFilter,
/// sub_7100612560) and are set on 300 fields across 81 types.  76 D3
/// variable-array fields matched, so their contents were replaced by a marker.
///
/// The APP and ANI corpora cannot catch this because Appearance and Anim both
/// fall back to the legacy definitions, whose flags are 0.  The groups below are
/// reflection-backed and do carry the flags.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/sno_reader.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<whiteout::u8> readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    std::vector<whiteout::u8> buf(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

namespace {
struct Stats {
    size_t externalMarkers = 0; ///< must stay 0 for D3
    size_t arrays = 0;
    size_t nonEmptyArrays = 0;
};
} // namespace

/// Recursively walk a parsed value, counting external markers and arrays.
static void walk(const whiteout::sno::SnoValue& v, Stats& st) {
    using namespace whiteout::sno;
    if (v.isObject()) {
        for (const auto& [key, sub] : v.asObject()) {
            if (key == "__external__") ++st.externalMarkers;
            walk(sub, st);
        }
        return;
    }
    if (v.isArray()) {
        const SnoArray& arr = v.asArray();
        ++st.arrays;
        if (arr.size() > 0) ++st.nonEmptyArrays;
        // Only generic arrays can hold nested objects/arrays; typed arrays
        // (floats, colours, ...) cannot contain a marker.
        if (arr.isArray() || arr.isObject()) {
            for (const auto& e : arr.asValueData()) walk(e, st);
        }
    }
}

TEST_CASE("D3 payload: no external markers", "[d3][payload][corpus]") {
    using namespace whiteout;
    using namespace whiteout::sno;

    fs::path base;
    for (auto candidate : {"Corpus/D3", "../Corpus/D3", "../../Corpus/D3"}) {
        if (fs::is_directory(candidate)) { base = candidate; break; }
    }
    if (base.empty()) SKIP("D3 corpus not found");

    struct GroupSpec {
        const char* dir;
        const char* ext;
        SnoGroup group;
    };
    // Appearance first: it is the group whose arrays actually resolve today, so
    // it is what gives the walk real array data to chew on.  (Its own defs come
    // from the legacy fallback, so it is not itself affected by the flag bug.)
    const GroupSpec kGroups[] = {
        {"Appearances", ".app", SnoGroup::Appearance},
        {"Particle", ".prt", SnoGroup::Particle},
        {"Actor", ".acr", SnoGroup::Actor},
        {"Material", ".mat", SnoGroup::Material},
        {"EffectGroup", ".efg", SnoGroup::EffectGroup},
        {"Cloth", ".clt", SnoGroup::Cloth},
        {"Physics", ".phy", SnoGroup::Physics},
        {"ShaderMap", ".shm", SnoGroup::ShaderMap},
        {"AnimSet", ".ans", SnoGroup::AnimSet},
    };
    constexpr size_t kMaxPerGroup = 400; // corpora are large; sample is plenty

    SnoReader reader;
    Stats total;
    size_t totalFiles = 0, parsed = 0, failed = 0;
    bool sawAny = false;

    for (const auto& g : kGroups) {
        const fs::path dir = base / g.dir;
        if (!fs::is_directory(dir)) continue;
        sawAny = true;

        Stats gs;
        size_t n = 0, ok = 0, bad = 0;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (n >= kMaxPerGroup) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != g.ext) continue;
            ++n;

            auto data = readFile(entry.path());
            if (data.size() < 16) { ++bad; continue; }
            auto file = reader.parse(data, g.group);
            if (!file) { ++bad; continue; }
            ++ok;
            if (ok == 1) {
                std::cout << "    [first] type=" << file->typeName
                          << " rootIsObject=" << file->root.isObject()
                          << " rootIsArray=" << file->root.isArray();
                if (file->root.isObject()) {
                    const auto& o = file->root.asObject();
                    std::cout << " entries=" << std::distance(o.begin(), o.end());
                    size_t shown = 0;
                    for (const auto& [k, sub] : o) {
                        if (shown++ >= 4) break;
                        const char* kind = sub.isArray()    ? "arr"
                                           : sub.isObject() ? "obj"
                                           : sub.isNull()   ? "null"
                                           : sub.isInt()    ? "int"
                                           : sub.isUint()   ? "uint"
                                           : sub.isFloat()  ? "float"
                                           : sub.isString() ? "str"
                                           : sub.isBool()   ? "bool"
                                                            : "?";
                        std::cout << " | " << k << "(" << kind << ")";
                    }
                }
                std::cout << "\n";
            }
            walk(file->root, gs);
        }

        std::cout << "  " << g.dir << ": " << n << " files, " << ok << " parsed, " << bad
                  << " failed, " << gs.arrays << " arrays (" << gs.nonEmptyArrays
                  << " non-empty), " << gs.externalMarkers << " external markers\n";

        total.externalMarkers += gs.externalMarkers;
        total.arrays += gs.arrays;
        total.nonEmptyArrays += gs.nonEmptyArrays;
        totalFiles += n;
        parsed += ok;
        failed += bad;
    }

    if (!sawAny) SKIP("no D3 group corpora found");

    std::cout << "\n=== D3 payload summary ===\n"
              << "Files:            " << totalFiles << "\n"
              << "Parsed:           " << parsed << "\n"
              << "Failed:           " << failed << "\n"
              << "Arrays:           " << total.arrays << "\n"
              << "Non-empty arrays: " << total.nonEmptyArrays << "\n"
              << "External markers: " << total.externalMarkers << "  (must be 0)\n";

    // D3 has no external payload file -- a marker means a variable array was
    // silently replaced by a stub instead of being read from the same buffer.
    CHECK(total.externalMarkers == 0);
    CHECK(parsed > 0);
    // Sanity: the sampled groups really do contain array data, so a zero marker
    // count means "read correctly", not "nothing was exercised".
    CHECK(total.nonEmptyArrays > 0);
}
