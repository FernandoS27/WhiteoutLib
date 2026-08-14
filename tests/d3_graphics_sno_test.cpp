// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Audit of the Diablo III rendering / graphics SNO groups.
///
/// Walks every graphics group for which a corpus exists and checks that files
/// parse, that the root type actually yields fields, and that no field comes
/// back as an `__external__` marker (D3 has no external payload file -- see
/// d3_payload_test.cpp).
///
/// Groups whose root type was quarantined by the exporter (flagged `suspect`,
/// therefore emitted without fields) parse to an EMPTY object.  That is a known
/// gap, not a parse failure, so it is reported loudly rather than asserted on.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/sno_reader.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<whiteout::u8> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    std::vector<whiteout::u8> buf(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

namespace {
struct Acc {
    size_t files = 0, parsed = 0, failed = 0;
    size_t rootFields = 0;       // max root entry count seen
    size_t arrays = 0, nonEmpty = 0;
    size_t externals = 0;
    size_t emptyRoots = 0;       // files whose root had zero entries
    std::string typeName;
};
} // namespace

static void walk(const whiteout::sno::SnoValue& v, Acc& a) {
    using namespace whiteout::sno;
    if (v.isObject()) {
        for (const auto& [k, sub] : v.asObject()) {
            if (k == "__external__") ++a.externals;
            walk(sub, a);
        }
        return;
    }
    if (v.isArray()) {
        const SnoArray& arr = v.asArray();
        ++a.arrays;
        if (arr.size() > 0) ++a.nonEmpty;
        if (arr.isArray() || arr.isObject())
            for (const auto& e : arr.asValueData()) walk(e, a);
    }
}

TEST_CASE("D3 graphics SNO groups", "[d3][graphics][corpus]") {
    using namespace whiteout;
    using namespace whiteout::sno;

    fs::path base;
    for (auto c : {"Corpus/D3", "../Corpus/D3", "../../Corpus/D3"}) {
        if (fs::is_directory(c)) { base = c; break; }
    }
    if (base.empty()) SKIP("D3 corpus not found");

    struct Spec { const char* dir; const char* ext; SnoGroup group; const char* label; };
    const Spec kSpecs[] = {
        {"Actor",       ".acr", SnoGroup::Actor,       "Actor"},
        {"Anim",        ".ani", SnoGroup::Animation,   "Anim"},
        {"AnimSet",     ".ans", SnoGroup::AnimSet,     "AnimSet"},
        {"AnimTree",    ".ant", SnoGroup::AnimTree,    "AnimTree"},
        {"Appearances", ".app", SnoGroup::Appearance,  "Appearance"},
        {"Cloth",       ".clt", SnoGroup::Cloth,       "Cloth"},
        {"EffectGroup", ".efg", SnoGroup::EffectGroup, "EffectGroup"},
        {"Material",    ".mat", SnoGroup::Material,    "Material"},
        {"Particle",    ".prt", SnoGroup::Particle,    "Particle"},
        {"PhysMesh",    ".phm", SnoGroup::PhysMesh,    "PhysMesh"},
        {"Physics",     ".phy", SnoGroup::Physics,     "Physics"},
        {"ShaderMap",   ".shm", SnoGroup::ShaderMap,   "ShaderMap"},
        {"Shaders",     ".shd", SnoGroup::Shader,      "Shaders"},
    };
    constexpr size_t kMax = 300;

    SnoReader reader;
    std::vector<std::pair<std::string, Acc>> results;
    size_t totalFailed = 0, totalExternal = 0;

    for (const auto& s : kSpecs) {
        const fs::path dir = base / s.dir;
        if (!fs::is_directory(dir)) continue;
        Acc a;
        for (const auto& e : fs::directory_iterator(dir)) {
            if (a.files >= kMax) break;
            if (!e.is_regular_file() || e.path().extension() != s.ext) continue;
            ++a.files;
            auto data = readFile(e.path());
            if (data.size() < 16) { ++a.failed; continue; }
            auto file = reader.parse(data, s.group);
            if (!file) { ++a.failed; continue; }
            ++a.parsed;
            if (a.typeName.empty()) a.typeName = file->typeName;
            size_t n = 0;
            if (file->root.isObject())
                for (auto it = file->root.asObject().begin(); it != file->root.asObject().end(); ++it) ++n;
            if (n == 0) ++a.emptyRoots;
            a.rootFields = std::max(a.rootFields, n);
            walk(file->root, a);
        }
        totalFailed += a.failed;
        totalExternal += a.externals;
        results.emplace_back(s.label, a);
    }

    if (results.empty()) SKIP("no graphics corpora found");

    std::cout << "\n=== D3 graphics SNO audit ===\n";
    std::cout << std::left << std::setw(14) << "group" << std::setw(8) << "files"
              << std::setw(8) << "parsed" << std::setw(8) << "failed" << std::setw(11) << "rootFields"
              << std::setw(10) << "emptyRts" << std::setw(9) << "arrays" << std::setw(10) << "nonEmpty"
              << std::setw(6) << "ext" << "rootType\n";
    for (const auto& [name, a] : results) {
        std::cout << std::left << std::setw(14) << name << std::setw(8) << a.files
                  << std::setw(8) << a.parsed << std::setw(8) << a.failed
                  << std::setw(11) << a.rootFields << std::setw(10) << a.emptyRoots
                  << std::setw(9) << a.arrays << std::setw(10) << a.nonEmpty
                  << std::setw(6) << a.externals << a.typeName << "\n";
    }

    std::cout << "\nGroups whose root yields NO fields (exporter-quarantined type):\n";
    bool anyEmpty = false;
    for (const auto& [name, a] : results) {
        if (a.parsed > 0 && a.rootFields == 0) {
            std::cout << "   " << name << "  (" << a.typeName << ")\n";
            anyEmpty = true;
        }
    }
    if (!anyEmpty) std::cout << "   (none)\n";

    // Hard invariants: everything must parse, and D3 must never emit an
    // external-payload marker.
    CHECK(totalFailed == 0);
    CHECK(totalExternal == 0);
}
