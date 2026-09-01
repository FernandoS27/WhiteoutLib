// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P0 — the `Diagnostics` container. The behaviour under test is what
/// makes a lossy converter gateable: stable order, groupability by code and by
/// profile, and a histogram that is a literal diff against a checked-in golden.

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/validate.h>

using namespace whiteout;
using namespace whiteout::models;

TEST_CASE("wem diagnostics record severity, code, place and profile", "[wem][diagnostics]") {
    wem::Diagnostics diag;
    CHECK(diag.empty());
    CHECK_FALSE(diag.hasErrors());

    diag.warn(wem::DiagCode::LossyBlendMode, "no equivalent for blend_add",
              wem::ElementRef(wem::ElementKind::Material, 3), wem::ProfileId::Wc3Classic);

    REQUIRE(diag.size() == 1);
    const auto& entry = diag.all()[0];
    CHECK(entry.severity == wem::Severity::Warning);
    CHECK(entry.code == wem::DiagCode::LossyBlendMode);
    CHECK(entry.message == "no equivalent for blend_add");
    CHECK(entry.where.kind == wem::ElementKind::Material);
    CHECK(entry.where.index == 3);
    CHECK(entry.where.sub == wem::ElementRef::kInvalidIndex);
    CHECK(entry.profile == wem::ProfileId::Wc3Classic);
    CHECK(entry.hasProfile());
    CHECK_FALSE(diag.hasErrors());
}

TEST_CASE("wem diagnostics track errors separately from warnings", "[wem][diagnostics]") {
    wem::Diagnostics diag;
    diag.info(wem::DiagCode::MaterialSlotUnused, "slot 2 unused");
    diag.warn(wem::DiagCode::LayerDropped, "layer 3 dropped");
    CHECK_FALSE(diag.hasErrors());

    diag.error(wem::DiagCode::ProfileNotCarried, "document does not carry wow");
    CHECK(diag.hasErrors());
    CHECK(diag.size() == 3);
    CHECK(diag.bySeverity(wem::Severity::Info).size() == 1);
    CHECK(diag.bySeverity(wem::Severity::Warning).size() == 1);
    CHECK(diag.bySeverity(wem::Severity::Error).size() == 1);

    diag.clear();
    CHECK(diag.empty());
    CHECK_FALSE(diag.hasErrors());
}

TEST_CASE("wem diagnostics group by code and by profile", "[wem][diagnostics]") {
    wem::Diagnostics diag;
    diag.warn(wem::DiagCode::NonManifoldEdgeSplit, "edge 10");
    diag.warn(wem::DiagCode::NonManifoldEdgeSplit, "edge 12");
    diag.warn(wem::DiagCode::DegenerateFaceDropped, "face 7");
    diag.warn(wem::DiagCode::LossyBlendMode, "material 0", {}, wem::ProfileId::Wow);
    diag.warn(wem::DiagCode::LossyBlendMode, "material 1", {}, wem::ProfileId::Sc2);

    CHECK(diag.byCode(wem::DiagCode::NonManifoldEdgeSplit).size() == 2);
    CHECK(diag.countOf(wem::DiagCode::NonManifoldEdgeSplit) == 2);
    CHECK(diag.countOf(wem::DiagCode::DegenerateFaceDropped) == 1);
    CHECK(diag.countOf(wem::DiagCode::OrphanChunk) == 0);

    CHECK(diag.byProfile(wem::ProfileId::Wow).size() == 1);
    CHECK(diag.byProfile(wem::ProfileId::Sc2).size() == 1);
    // Entries with no profile group under the sentinel, not under Generic.
    CHECK(diag.byProfile(wem::ProfileId::Count).size() == 3);
    CHECK(diag.byProfile(wem::ProfileId::Generic).empty());

    // Grouping preserves insertion order within a group.
    const auto splits = diag.byCode(wem::DiagCode::NonManifoldEdgeSplit);
    REQUIRE(splits.size() == 2);
    CHECK(splits[0].message == "edge 10");
    CHECK(splits[1].message == "edge 12");
}

TEST_CASE("wem diagnostics histogram is code-ordered, not insertion-ordered",
          "[wem][diagnostics]") {
    // The expected-loss goldens are diffed literally, so the histogram must not
    // depend on the order codes happened to be reported in.
    wem::Diagnostics a;
    a.warn(wem::DiagCode::LossyBlendMode, "x");
    a.warn(wem::DiagCode::DegenerateFaceDropped, "y");
    a.warn(wem::DiagCode::DegenerateFaceDropped, "z");

    wem::Diagnostics b;
    b.warn(wem::DiagCode::DegenerateFaceDropped, "z");
    b.warn(wem::DiagCode::LossyBlendMode, "x");
    b.warn(wem::DiagCode::DegenerateFaceDropped, "y");

    CHECK(a.formatHistogram() == b.formatHistogram());

    const auto rows = a.histogram();
    REQUIRE(rows.size() == 2);
    // DegenerateFaceDropped is in the geometry group, LossyBlendMode in materials:
    // ascending code order puts geometry first.
    CHECK(rows[0].code == wem::DiagCode::DegenerateFaceDropped);
    CHECK(rows[0].count == 2);
    CHECK(rows[1].code == wem::DiagCode::LossyBlendMode);
    CHECK(rows[1].count == 1);

    CHECK(a.formatHistogram() == "DegenerateFaceDropped 2\nLossyBlendMode 1\n");
}

TEST_CASE("wem diagnostics append preserves order and error count", "[wem][diagnostics]") {
    wem::Diagnostics first;
    first.warn(wem::DiagCode::LayerDropped, "one");

    wem::Diagnostics second;
    second.error(wem::DiagCode::IndexOutOfRange, "two");
    second.warn(wem::DiagCode::LayerDropped, "three");

    first.append(second);
    REQUIRE(first.size() == 3);
    CHECK(first.all()[0].message == "one");
    CHECK(first.all()[1].message == "two");
    CHECK(first.all()[2].message == "three");
    CHECK(first.hasErrors());
    CHECK(first.countOf(wem::DiagCode::LayerDropped) == 2);
}

TEST_CASE("wem diagnostic code names are complete and unique", "[wem][diagnostics]") {
    // The name table is indexed by the enum; a code added without a name would
    // fail the static_assert in diagnostics.cpp, and a duplicated name would make
    // two different losses indistinguishable in a golden.
    std::vector<std::string> names;
    for (std::size_t i = 0; i < static_cast<std::size_t>(wem::DiagCode::Count); ++i) {
        const char* name = wem::ToString(static_cast<wem::DiagCode>(i));
        INFO("code index " << i);
        REQUIRE(name != nullptr);
        CHECK(std::string(name) != "Invalid");
        CHECK(std::string(name).size() > 0);
        names.emplace_back(name);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());

    CHECK(std::string(wem::ToString(wem::DiagCode::Count)) == "Invalid");
}

TEST_CASE("wem element refs describe themselves", "[wem][diagnostics]") {
    CHECK(wem::Describe(wem::ElementRef()) == "");
    CHECK(wem::Describe(wem::ElementRef(wem::ElementKind::Mesh, 0)) == "mesh[0]");
    CHECK(wem::Describe(wem::ElementRef(wem::ElementKind::Material, 12, 1)) ==
          "material[12].sub[1]");
    CHECK(wem::Describe(wem::ElementRef(wem::ElementKind::Node, 4095)) == "node[4095]");
    CHECK(std::string(wem::ToString(wem::ElementKind::None)).empty());
    CHECK(std::string(wem::ToString(wem::ElementKind::Halfedge)) == "halfedge");
}

TEST_CASE("wem validate levels each carry rules", "[wem][validate]") {
    // P0 delivered the plumbing with three empty tables; each later phase appends
    // to them in validate.cpp. This test exists so that "the phase registered its
    // rules" is an assertion someone can make, not an inspection. What the rules
    // *do* is asserted next to the structures they check.
    CHECK_FALSE(wem::ValidationRulesFor(wem::ValidateLevel::Structural).empty());
    CHECK_FALSE(wem::ValidationRulesFor(wem::ValidateLevel::Manifold).empty());
    CHECK_FALSE(wem::ValidationRulesFor(wem::ValidateLevel::Profile).empty());

    CHECK(std::string(wem::ToString(wem::ValidateLevel::Structural)) == "structural");
    CHECK(std::string(wem::ToString(wem::ValidateLevel::Manifold)) == "manifold");
    CHECK(std::string(wem::ToString(wem::ValidateLevel::Profile)) == "profile");
}
