// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MDL Parser — comprehensive test suite
// Tests both synthetic MDL snippets and real corpus files (v800 + v1000).

#include <catch2/catch_all.hpp>

#include "whiteout/models/mdx/mdl_parser.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace whiteout;
using namespace whiteout::mdx;

// ============================================================================
// Helpers
// ============================================================================

static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Shorthand: get child as property
static const MdlProperty& prop(const MdlNode& node, std::size_t idx) {
    return std::get<MdlProperty>(node.children.at(idx));
}

// Shorthand: get child as track
static const MdlAnimTrack& track(const MdlNode& node, std::size_t idx) {
    return std::get<MdlAnimTrack>(node.children.at(idx));
}

// Shorthand: get child as block
static const MdlNode& block(const MdlNode& node, std::size_t idx) {
    return std::get<MdlNode>(node.children.at(idx));
}

// ============================================================================
// Basic block parsing
// ============================================================================

TEST_CASE("empty_block", "[mdl_parser]") {
    auto doc = MdlParser::parse("Version {\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    REQUIRE(doc.roots.size() == 1);
    CHECK(doc.roots[0].name == "Version");
    CHECK(doc.roots[0].children.empty());
}

TEST_CASE("block_with_string_param", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Model "TestModel" {
})");
    REQUIRE_FALSE(doc.hasErrors());
    REQUIRE(doc.roots.size() == 1);
    CHECK(doc.roots[0].name == "Model");
    REQUIRE(doc.roots[0].headerParams.size() == 1);
    CHECK(doc.roots[0].headerParams[0].isString());
    CHECK(doc.roots[0].headerParams[0].asString() == "TestModel");
}

TEST_CASE("block_with_numeric_param", "[mdl_parser]") {
    auto doc = MdlParser::parse("Sequences 3 {\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    REQUIRE(doc.roots[0].headerParams.size() == 1);
    CHECK(doc.roots[0].headerParams[0].asNumber() == 3.0);
}

TEST_CASE("block_with_multi_numeric_params", "[mdl_parser]") {
    auto doc = MdlParser::parse("Faces 1 10572 {\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    auto& node = doc.roots[0];
    CHECK(node.name == "Faces");
    REQUIRE(node.headerParams.size() == 2);
    CHECK(node.headerParams[0].asNumber() == 1.0);
    CHECK(node.headerParams[1].asNumber() == 10572.0);
}

TEST_CASE("multiple_top_level_blocks", "[mdl_parser]") {
    auto doc = MdlParser::parse("Version {\n}\nModel \"X\" {\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    REQUIRE(doc.roots.size() == 2);
    CHECK(doc.roots[0].name == "Version");
    CHECK(doc.roots[1].name == "Model");
}

// ============================================================================
// Property parsing
// ============================================================================

TEST_CASE("simple_numeric_property", "[mdl_parser]") {
    auto doc = MdlParser::parse("Version {\n    FormatVersion 1000,\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    auto& p = prop(doc.roots[0], 0);
    CHECK(p.name == "FormatVersion");
    REQUIRE(p.values.size() == 1);
    CHECK(p.values[0].asNumber() == 1000.0);
    CHECK_FALSE(p.isStatic);
}

TEST_CASE("string_property", "[mdl_parser]") {
    auto doc = MdlParser::parse("Bitmap {\n    Image \"foo.blp\",\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    auto& p = prop(doc.roots[0], 0);
    CHECK(p.name == "Image");
    CHECK(p.values[0].asString() == "foo.blp");
}

TEST_CASE("vector_property", "[mdl_parser]") {
    auto doc = MdlParser::parse("Geoset {\n    MinimumExtent { -1.5, -2.0, -3.5 },\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    auto& p = prop(doc.roots[0], 0);
    CHECK(p.name == "MinimumExtent");
    REQUIRE(p.values.size() == 1);
    REQUIRE(p.values[0].isArray());
    auto& arr = p.values[0].asArray();
    REQUIRE(arr.size() == 3);
    CHECK(arr[0].asNumber() == Catch::Approx(-1.5));
    CHECK(arr[1].asNumber() == Catch::Approx(-2.0));
    CHECK(arr[2].asNumber() == Catch::Approx(-3.5));
}

TEST_CASE("static_property", "[mdl_parser]") {
    auto doc = MdlParser::parse("Layer {\n    static TextureID 5,\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    auto& p = prop(doc.roots[0], 0);
    CHECK(p.name == "TextureID");
    CHECK(p.isStatic);
    CHECK(p.values[0].asNumber() == 5.0);
}

TEST_CASE("bare_flag_property", "[mdl_parser]") {
    auto doc = MdlParser::parse("Material {\n    TwoSided,\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    auto& p = prop(doc.roots[0], 0);
    CHECK(p.name == "TwoSided");
    CHECK(p.values.empty());
}

TEST_CASE("identifier_value_property", "[mdl_parser]") {
    auto doc = MdlParser::parse("Layer {\n    FilterMode Transparent,\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    auto& p = prop(doc.roots[0], 0);
    CHECK(p.name == "FilterMode");
    REQUIRE(p.values.size() == 1);
    CHECK(p.values[0].asString() == "Transparent");
}

TEST_CASE("multi_value_property", "[mdl_parser]") {
    auto doc = MdlParser::parse("Geoset {\n    Interval { 0, 100 },\n}\n");
    REQUIRE_FALSE(doc.hasErrors());
    auto& p = prop(doc.roots[0], 0);
    CHECK(p.name == "Interval");
    REQUIRE(p.values.size() == 1);
    REQUIRE(p.values[0].isArray());
    CHECK(p.values[0].asArray().size() == 2);
}

TEST_CASE("static_vector_property", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Bone "test" {
    static Translation { 0, 0, 0 },
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& p = prop(doc.roots[0], 0);
    CHECK(p.name == "Translation");
    CHECK(p.isStatic);
    REQUIRE(p.values.size() == 1);
    CHECK(p.values[0].isArray());
    CHECK(p.values[0].asArray().size() == 3);
}

// ============================================================================
// Animation track parsing
// ============================================================================

TEST_CASE("linear_translation_track", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Bone "test" {
    Translation 2 {
        Linear,
        0: { 0, 0, 0 },
        1000: { 1, 2, 3 },
    }
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& t = track(doc.roots[0], 0);
    CHECK(t.name == "Translation");
    CHECK(t.count == 2);
    CHECK(t.interpolation == "Linear");
    REQUIRE(t.keyframes.size() == 2);
    CHECK(t.keyframes[0].time == 0);
    CHECK(t.keyframes[0].value.isArray());
    CHECK(t.keyframes[0].value.asArray().size() == 3);
    CHECK_FALSE(t.keyframes[0].hasTangents);
    CHECK(t.keyframes[1].time == 1000);
}

TEST_CASE("hermite_track_with_tangents", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Bone "test" {
    Rotation 2 {
        Hermite,
        0: { 0, 0, 0, 1 },
            InTan { 0, 0, 0, 1 },
            OutTan { 0, 0, 0, 1 },
        333: { 0, 0.5, 0, 0.866 },
            InTan { 0, 0.5, 0, 0.866 },
            OutTan { 0, 0.5, 0, 0.866 },
    }
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& t = track(doc.roots[0], 0);
    CHECK(t.interpolation == "Hermite");
    REQUIRE(t.keyframes.size() == 2);

    CHECK(t.keyframes[0].hasTangents);
    CHECK(t.keyframes[0].inTan.isArray());
    CHECK(t.keyframes[0].outTan.isArray());

    CHECK(t.keyframes[1].hasTangents);
    CHECK(t.keyframes[1].time == 333);
}

TEST_CASE("bezier_track_with_tangents", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Helper "grip" {
    Translation 1 {
        Bezier,
        100: { 1, 2, 3 },
            InTan { 0.5, 1, 1.5 },
            OutTan { 1.5, 3, 4.5 },
    }
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& t = track(doc.roots[0], 0);
    CHECK(t.interpolation == "Bezier");
    CHECK(t.keyframes[0].hasTangents);
}

TEST_CASE("dontinterp_track", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Bone "test" {
    Visibility 2 {
        DontInterp,
        0: 1,
        500: 0,
    }
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& t = track(doc.roots[0], 0);
    CHECK(t.interpolation == "DontInterp");
    REQUIRE(t.keyframes.size() == 2);
    CHECK(t.keyframes[0].value.asNumber() == 1.0);
    CHECK(t.keyframes[1].value.asNumber() == 0.0);
}

TEST_CASE("event_track", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(EventObject "SNDxFOOT" {
    EventTrack 3 {
        100,
        500,
        900,
    }
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& t = track(doc.roots[0], 0);
    CHECK(t.name == "EventTrack");
    CHECK(t.count == 3);
    CHECK(t.interpolation.empty());
    REQUIRE(t.keyframes.size() == 3);
    CHECK(t.keyframes[0].time == 100);
    CHECK(t.keyframes[1].time == 500);
    CHECK(t.keyframes[2].time == 900);
}

// ============================================================================
// Nested blocks
// ============================================================================

TEST_CASE("nested_blocks", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Material {
    Layer {
        FilterMode None,
        static TextureID 0,
    }
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& mat = doc.roots[0];
    CHECK(mat.name == "Material");
    REQUIRE(mat.children.size() == 1);
    auto& layer = block(mat, 0);
    CHECK(layer.name == "Layer");
    REQUIRE(layer.children.size() == 2);
    CHECK(prop(layer, 0).name == "FilterMode");
    CHECK(prop(layer, 1).name == "TextureID");
    CHECK(prop(layer, 1).isStatic);
}

TEST_CASE("anim_subblock_with_string_param", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Sequences 2 {
    Anim "Stand" {
        Interval { 0, 1000 },
        NonLooping,
    }
    Anim "Walk" {
        Interval { 1001, 2000 },
        Rarity 0.5,
    }
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& seq = doc.roots[0];
    REQUIRE(seq.children.size() == 2);
    auto& stand = block(seq, 0);
    CHECK(stand.name == "Anim");
    CHECK(stand.headerParams[0].asString() == "Stand");
    CHECK(prop(stand, 1).name == "NonLooping");
    auto& walk = block(seq, 1);
    CHECK(prop(walk, 1).name == "Rarity");
}

// ============================================================================
// Data blocks with anonymous values
// ============================================================================

TEST_CASE("data_block_with_vectors", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Vertices 3 {
    { 1.0, 2.0, 3.0 },
    { 4.0, 5.0, 6.0 },
    { 7.0, 8.0, 9.0 },
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& v = doc.roots[0];
    CHECK(v.name == "Vertices");
    // Each anonymous vector is a property with "" name
    REQUIRE(v.children.size() == 3);
    auto& first = prop(v, 0);
    CHECK(first.name.empty());
    REQUIRE(first.values.size() == 1);
    CHECK(first.values[0].isArray());
    CHECK(first.values[0].asArray().size() == 3);
}

TEST_CASE("data_block_with_bare_numbers", "[mdl_parser]") {
    // v800-style VertexGroup
    auto doc = MdlParser::parse(R"(VertexGroup {
    0,
    1,
    2,
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& vg = doc.roots[0];
    REQUIRE(vg.children.size() == 3);
    auto& first = prop(vg, 0);
    CHECK(first.values[0].asNumber() == 0.0);
}

// ============================================================================
// PivotPoints
// ============================================================================

TEST_CASE("pivot_points_block", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(PivotPoints 2 {
    { 0, 0, 0 },
    { 1.5, 2.5, 3.5 },
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& pp = doc.roots[0];
    CHECK(pp.name == "PivotPoints");
    REQUIRE(pp.children.size() == 2);
}

// ============================================================================
// Mixed block children
// ============================================================================

TEST_CASE("bone_with_mixed_children", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Bone "bone_root" {
    ObjectId 0,
    Parent -1,
    GeosetId Multiple,
    GeosetAnimId None,
    Translation 1 {
        Linear,
        0: { 0, 0, 0 },
    }
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& bone = doc.roots[0];
    CHECK(bone.name == "Bone");

    // Properties
    CHECK(prop(bone, 0).name == "ObjectId");
    CHECK(prop(bone, 1).name == "Parent");
    // GeosetId with identifier value "Multiple"
    CHECK(prop(bone, 2).name == "GeosetId");
    CHECK(prop(bone, 2).values[0].asString() == "Multiple");
    CHECK(prop(bone, 3).name == "GeosetAnimId");
    CHECK(prop(bone, 3).values[0].asString() == "None");

    // Animation track
    auto& t = track(bone, 4);
    CHECK(t.name == "Translation");
}

// ============================================================================
// Comments
// ============================================================================

TEST_CASE("inline_comments_ignored", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Bone "test" {
    ObjectId 0,
    Parent 5, // "parent_bone"
})");
    REQUIRE_FALSE(doc.hasErrors());
    CHECK(prop(doc.roots[0], 1).name == "Parent");
    CHECK(prop(doc.roots[0], 1).values[0].asNumber() == 5.0);
}

// ============================================================================
// Negative numbers
// ============================================================================

TEST_CASE("negative_numbers", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Geoset {
    MinimumExtent { -100.5, -200.3, -300.1 },
    Parent -1,
})");
    REQUIRE_FALSE(doc.hasErrors());
    CHECK(prop(doc.roots[0], 1).values[0].asNumber() == Catch::Approx(-1.0));
}

// ============================================================================
// Trailing commas after sub-blocks (regression tests)
// ============================================================================

TEST_CASE("matrices_subblock_with_trailing_comma", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(Groups 3 3 {
    Matrices { 81 },
    Matrices { 82 },
    Matrices { 83 },
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& groups = doc.roots[0];
    REQUIRE(groups.children.size() == 3);
    // Matrices { 81 } is parsed as a property with vector value (first token inside { is a number)
    auto& m = prop(groups, 0);
    CHECK(m.name == "Matrices");
    REQUIRE(m.values.size() == 1);
    CHECK(m.values[0].isArray());
    CHECK(m.values[0].asArray()[0].asNumber() == 81.0);
}

TEST_CASE("segment_color_nested_blocks_trailing_commas", "[mdl_parser]") {
    auto doc = MdlParser::parse(R"(ParticleEmitter2 "smoke" {
    SegmentColor {
        Color { 1, 0.5, 0 },
        Color { 0.8, 0.3, 0 },
        Color { 0.5, 0.1, 0 },
    },
    Alpha {255, 200, 0},
    ParticleScaling {12, 6, 0.001},
    TextureID 5,
})");
    REQUIRE_FALSE(doc.hasErrors());
    auto& pe = doc.roots[0];

    // SegmentColor is a sub-block; Color entries are properties with vector values
    // because Color { 0, 0, 0.784 } has a number as the first token inside the brace
    auto& sc = block(pe, 0);
    CHECK(sc.name == "SegmentColor");
    REQUIRE(sc.children.size() == 3);
    auto& c0 = prop(sc, 0);
    CHECK(c0.name == "Color");
    REQUIRE(c0.values.size() == 1);
    CHECK(c0.values[0].isArray());
    CHECK(c0.values[0].asArray().size() == 3);

    // Alpha, ParticleScaling are properties with vector values
    auto& alpha = prop(pe, 1);
    CHECK(alpha.name == "Alpha");
    CHECK(alpha.values[0].isArray());
    CHECK(alpha.values[0].asArray().size() == 3);

    auto& ps = prop(pe, 2);
    CHECK(ps.name == "ParticleScaling");

    CHECK(prop(pe, 3).name == "TextureID");
}

// ============================================================================
// Error recovery
// ============================================================================

TEST_CASE("missing_closing_brace_reports_error", "[mdl_parser]") {
    auto doc = MdlParser::parse("Version {\n    FormatVersion 1000,\n");
    CHECK(doc.hasErrors());
    // Should still produce partial results
    CHECK(doc.roots.size() >= 1);
}

TEST_CASE("empty_input_produces_empty_document", "[mdl_parser]") {
    auto doc = MdlParser::parse("");
    CHECK_FALSE(doc.hasErrors());
    CHECK(doc.roots.empty());
}

TEST_CASE("whitespace_only_input", "[mdl_parser]") {
    auto doc = MdlParser::parse("   \n\n  \t  ");
    CHECK_FALSE(doc.hasErrors());
    CHECK(doc.roots.empty());
}

// ============================================================================
// Corpus integration tests
// ============================================================================

TEST_CASE("v1000_corpus_zovaal", "[mdl_parser][corpus]") {
    auto source = readFile("Corpus/MDL/Zovaal/Zovaal the Banished One.mdl");
    if (source.empty()) {
        WARN("Corpus file not found — skipping (Corpus/MDL/Zovaal/Zovaal the Banished One.mdl)");
        return;
    }

    auto doc = MdlParser::parse(source);

    // Should parse without errors
    if (doc.hasErrors()) {
        for (auto& e : doc.errors) {
            WARN("Parse error at line " << e.line << ":" << e.column << " — " << e.message);
        }
    }
    CHECK_FALSE(doc.hasErrors());

    // Should have many top-level blocks (at minimum: Version, Model, Sequences, Textures, ...)
    CHECK(doc.roots.size() > 5);

    // First block should be Version
    CHECK(doc.roots[0].name == "Version");

    // Find the Model block
    bool foundModel = false;
    for (auto& root : doc.roots) {
        if (root.name == "Model") {
            foundModel = true;
            CHECK_FALSE(root.headerParams.empty());
            CHECK(root.headerParams[0].isString());
            break;
        }
    }
    CHECK(foundModel);
}

TEST_CASE("v800_corpus_saurus_warrior", "[mdl_parser][corpus]") {
    auto source = readFile("Corpus/MDL/saurus_warrior_unit_run.mdl");
    if (source.empty()) {
        WARN("Corpus file not found — skipping (Corpus/MDL/saurus_warrior_unit_run.mdl)");
        return;
    }

    auto doc = MdlParser::parse(source);

    if (doc.hasErrors()) {
        for (auto& e : doc.errors) {
            WARN("Parse error at line " << e.line << ":" << e.column << " — " << e.message);
        }
    }
    CHECK_FALSE(doc.hasErrors());

    CHECK(doc.roots.size() > 5);
    CHECK(doc.roots[0].name == "Version");

    // v800 should have FormatVersion 800
    bool found800 = false;
    for (auto& child : doc.roots[0].children) {
        if (auto* p = std::get_if<MdlProperty>(&child)) {
            if (p->name == "FormatVersion" && !p->values.empty() &&
                p->values[0].isNumber() && p->values[0].asNumber() == 800.0) {
                found800 = true;
            }
        }
    }
    CHECK(found800);

    // Should have Bones with Hermite/Bezier tracks
    bool foundHermiteOrBezier = false;
    for (auto& root : doc.roots) {
        if (root.name == "Bone") {
            for (auto& child : root.children) {
                if (auto* t = std::get_if<MdlAnimTrack>(&child)) {
                    if (t->interpolation == "Hermite" || t->interpolation == "Bezier") {
                        foundHermiteOrBezier = true;
                        // Verify tangents on first keyframe
                        if (!t->keyframes.empty()) {
                            CHECK(t->keyframes[0].hasTangents);
                        }
                    }
                }
            }
        }
        if (foundHermiteOrBezier) break;
    }
    CHECK(foundHermiteOrBezier);
}
