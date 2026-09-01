// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P3c — the WoW material core: the join, and the shader table.
///
/// Two things here are worth more than a sweep:
///
/// 1. **The join round-trips.** Import resolves four combo tables into one
///    resolved batch; export rebuilds the tables. Doing that on hand-built
///    tables, where the expected indices are visible, is the only way to see
///    that the *right* table fed the right field — a sweep would pass just as
///    happily with `textureCoordCombos` and `textureTransformCombos` swapped.
/// 2. **A combo row past the table does not fall back to row 0.** Row 0 is
///    `Opaque_Mod2xNA_Alpha`, an opaque two-texture environment combiner, and
///    silently choosing it is the recorded black-box artifact. The refusal is
///    the behaviour, so it is asserted.
///
/// The corpus leg is hidden behind the `.` tag: 56 known corpus files drive the
/// M2 parser into multi-GB zero-filled resizes, so a sweep needs a process
/// commit cap set outside the test.

#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m2/parser.h>
#include <whiteout/utils/os_file_system.h>

#include "test_helpers.h"
#include "wem_corpus_files.h"
#include "whiteout/models/wem/materials/m2_core.h"
#include "whiteout/models/wem/materials/m2_shader_table.h"

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

/// A model whose four combo tables hold *different* values at the same offset,
/// so a converter that read the wrong one produces the wrong number rather than
/// the same number twice.
m2::Model makeModel() {
    m2::Model model;
    model.textures.push_back(m2::Texture{0, 0, "body.blp"});
    model.textures.push_back(m2::Texture{0, 0, "cloak.blp"});
    model.textures.push_back(m2::Texture{11, 0, "skin.blp"}); // a replaceable slot

    m2::Material record;
    record.flags = static_cast<u16>(native::M2MaterialFlag::TwoSided);
    record.blendingMode = static_cast<u16>(native::M2BlendingMode::Alpha);
    model.materials.push_back(record);

    // Offsets 0 and 1 are the batch's two units; the leading padding entry makes
    // "read from index 0" visibly wrong.
    model.textureCombos = {99, 0, 1};
    model.textureCoordCombos = {99, 0, 1};
    model.textureTransformCombos = {99, 7, 8};
    model.textureWeightCombos = {99, 3, 4};
    return model;
}

m2::Batch makeBatch(u16 shaderId, u16 textureCount = 2) {
    m2::Batch batch;
    batch.shaderId = shaderId;
    batch.textureCount = textureCount;
    batch.materialIndex = 0;
    batch.priorityPlane = 2;
    batch.materialLayer = 1;
    batch.colorIndex = 5;
    batch.flags = 0x40;
    batch.textureComboIndex = 1;
    batch.textureCoordComboIndex = 1;
    batch.textureTransformComboIndex = 1;
    batch.textureWeightComboIndex = 1;
    return batch;
}

m2_core::Context makeContext() {
    m2_core::Context context;
    context.sourceVersion = 274;
    context.textureIndexMap = {10, 11, 12};
    return context;
}

} // namespace

TEST_CASE("wem an m2 batch resolves its four combo tables", "[wem][materials][m2]") {
    const m2::Model model = makeModel();
    Diagnostics diagnostics;
    const Material imported = m2_core::ImportBatch(model, makeBatch(0), makeContext(), diagnostics);

    const auto& block = std::get<native::M2Material>(imported.Native());
    REQUIRE(block.units.size() == 2);
    // Each field came from its own table, and each table held a different value.
    CHECK(block.units[0].texture == 0);
    CHECK(block.units[0].uvSet == 0);
    CHECK(block.units[0].transform == 7);
    CHECK(block.units[0].weight == 3);
    CHECK(block.units[1].texture == 1);
    CHECK(block.units[1].transform == 8);
    CHECK(block.units[1].weight == 4);

    CHECK(block.sourceVersion == 274);
    CHECK(block.batchFlags == 0x40);
    CHECK(block.colorIndex == 5);
    CHECK(block.materialLayer == 1);
    CHECK(imported.Common().blend == BlendMode::AlphaBlend);
    CHECK(imported.Common().cull == CullMode::None);
}

TEST_CASE("wem a replaceable texture type rides the resolved unit", "[wem][materials][m2]") {
    m2::Model model = makeModel();
    model.textureCombos = {99, 2, 1}; // unit 0 -> the replaceable texture
    Diagnostics diagnostics;
    const Material imported = m2_core::ImportBatch(model, makeBatch(0), makeContext(), diagnostics);

    const auto& block = std::get<native::M2Material>(imported.Native());
    REQUIRE(block.units.size() == 2);
    CHECK(block.units[0].textureType == 11);
    CHECK(block.units[1].textureType == 0); // a file path
}

TEST_CASE("wem shaderId names the combiner chain", "[wem][materials][m2]") {
    const m2::Model model = makeModel();
    Diagnostics diagnostics;

    // Explicit combo row 21 is `Combiners_Mod_Mod`.
    const Material imported =
        m2_core::ImportBatch(model, makeBatch(0x8000 | 21), makeContext(), diagnostics);
    REQUIRE(imported.Common().kind() == MaterialKind::Combiners);
    const CombinersBody* body = imported.Common().combiners();
    REQUIRE(body != nullptr);
    REQUIRE(body->stages.size() == 2);
    CHECK(body->stages[0].rgb == CombinerOp::Mod);
    CHECK(body->stages[1].rgb == CombinerOp::Mod);
    CHECK(diagnostics.countOf(DiagCode::UnknownShaderCombo) == 0);
}

TEST_CASE("wem the bit-field path answers when there is no explicit combo",
          "[wem][materials][m2]") {
    const m2::Model model = makeModel();
    Diagnostics diagnostics;

    // No 0x8000: `shaderId & 7` indexes the two-texture table, and 0x70 picks
    // the environment variant of it.
    bool outOfTable = false;
    CHECK(m2_core::PixelShaderFor(2, 4, outOfTable) ==
          m2_core::M2PixelShader::Combiners_Opaque_Mod2x);
    CHECK_FALSE(outOfTable);
    CHECK(m2_core::PixelShaderFor(2, 0x74, outOfTable) ==
          m2_core::M2PixelShader::Combiners_Mod_Mod2x);
    CHECK(m2_core::PixelShaderFor(1, 0, outOfTable) == m2_core::M2PixelShader::Combiners_Opaque);
    CHECK(m2_core::PixelShaderFor(1, 0x70, outOfTable) == m2_core::M2PixelShader::Combiners_Mod);

    const Material imported = m2_core::ImportBatch(model, makeBatch(4), makeContext(), diagnostics);
    const CombinersBody* body = imported.Common().combiners();
    REQUIRE(body != nullptr);
    REQUIRE(body->stages.size() == 2);
    CHECK(body->stages[0].rgb == CombinerOp::Opaque);
    CHECK(body->stages[1].rgb == CombinerOp::Mod2x);
}

TEST_CASE("wem an out-of-table combo diagnoses and never falls back to row 0",
          "[wem][materials][m2]") {
    const m2::Model model = makeModel();
    Diagnostics diagnostics;

    bool outOfTable = false;
    const m2_core::M2PixelShader shader = m2_core::PixelShaderFor(2, 0x8000 | 200, outOfTable);
    CHECK(outOfTable);
    // Row 0 is `Combiners_Opaque_Mod2xNA_Alpha`. Landing there silently is the
    // recorded artifact; the bit-field path's answer is what comes back instead.
    CHECK(shader != m2_core::M2PixelShader::Combiners_Opaque_Mod2xNA_Alpha);
    // 0x80C8: bits 0x70 are set, so the environment table answers, and `& 7` is
    // 0 -> `Combiners_Mod_Opaque`.
    CHECK(shader == m2_core::M2PixelShader::Combiners_Mod_Opaque);

    m2_core::ImportBatch(model, makeBatch(0x8000 | 200), makeContext(), diagnostics);
    CHECK(diagnostics.countOf(DiagCode::UnknownShaderCombo) == 1);
}

TEST_CASE("wem a chain that cannot say everything says so", "[wem][materials][m2]") {
    const m2::Model model = makeModel();
    Diagnostics diagnostics;
    // Explicit row 1 is `Combiners_Opaque_AddAlpha`: `CombinerOp` has `Add` but
    // no way to say the add is scaled by the second unit's alpha.
    m2_core::ImportBatch(model, makeBatch(0x8000 | 1), makeContext(), diagnostics);
    CHECK(diagnostics.countOf(DiagCode::LossyKindConversion) >= 1);
}

TEST_CASE("wem a combo index past its table is an error, not entry 0", "[wem][materials][m2]") {
    m2::Model model = makeModel();
    model.textureCombos = {99}; // too short for the batch's two units
    Diagnostics diagnostics;
    m2_core::ImportBatch(model, makeBatch(0), makeContext(), diagnostics);
    CHECK(diagnostics.countOf(DiagCode::IndexOutOfRange) == 2);
}

TEST_CASE("wem an m2 batch survives import-export-import", "[wem][materials][m2]") {
    const m2::Model model = makeModel();
    const m2_core::Context context = makeContext();
    Diagnostics diagnostics;

    const Material first =
        m2_core::ImportBatch(model, makeBatch(0x8000 | 21), context, diagnostics);

    m2::Model rebuilt;
    rebuilt.textures = model.textures;
    const m2::Batch exported = m2_core::ExportMaterial(first, context, rebuilt, diagnostics);
    const Material second = m2_core::ImportBatch(rebuilt, exported, context, diagnostics);

    const auto& a = std::get<native::M2Material>(first.Native());
    const auto& b = std::get<native::M2Material>(second.Native());
    CHECK(a.shaderId == b.shaderId);
    CHECK(a.renderFlags == b.renderFlags);
    CHECK(a.blendingMode == b.blendingMode);
    CHECK(a.batchFlags == b.batchFlags);
    CHECK(a.colorIndex == b.colorIndex);
    CHECK(a.materialLayer == b.materialLayer);
    CHECK(a.priorityPlane == b.priorityPlane);
    REQUIRE(a.units.size() == b.units.size());
    for (std::size_t i = 0; i < a.units.size(); ++i) {
        CHECK(a.units[i].texture == b.units[i].texture);
        CHECK(a.units[i].uvSet == b.units[i].uvSet);
        // The one the round trip is really for: `textureTransformIndex` is live
        // data, unlike `ribbonColorIndex` beside it in the file, and a converter
        // that treated it as dead would lose it here.
        CHECK(a.units[i].transform == b.units[i].transform);
        CHECK(a.units[i].weight == b.units[i].weight);
    }

    // The rebuilt tables are the export's own, packed from index 0 rather than
    // reusing the source's padding.
    CHECK(rebuilt.textureCombos.size() == 2);
    CHECK(rebuilt.textureTransformCombos == std::vector<u16>{7, 8});
}

TEST_CASE("wem exporting an edited m2 material cannot recover a shaderId", "[wem][materials][m2]") {
    const m2::Model model = makeModel();
    const m2_core::Context context = makeContext();
    Diagnostics diagnostics;
    Material material = m2_core::ImportBatch(model, makeBatch(0x8000 | 21), context, diagnostics);

    // Editing the common material makes the block stale, which forces the
    // fallback path — and the fallback cannot invert a many-to-one table.
    material.MutableCommon();
    REQUIRE(material.sync() == NativeSync::CommonEdited);

    Diagnostics exportDiagnostics;
    m2::Model rebuilt;
    const m2::Batch exported =
        m2_core::ExportMaterial(material, context, rebuilt, exportDiagnostics);
    CHECK(exported.shaderId == 0);
    CHECK(exportDiagnostics.countOf(DiagCode::UnknownShaderCombo) == 1);
}

TEST_CASE("wem the m2 ingredient tables declare no manual fields", "[wem][materials][m2]") {
    CHECK(native::kM2ManualFieldCount == 0);
}

// ============================================================================
// Corpus — hidden by default, see the file comment
// ============================================================================

TEST_CASE("wem m2 corpus batches resolve their joins", "[wem][corpus][materials][.m2slow]") {
    const std::vector<fs::path> files = test::gather("WEM_M2_CORPUS_DIR", ".m2", {"WoW", "WowM2"});
    if (files.empty()) {
        SKIP("M2 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 40);

    std::size_t models = 0;
    std::size_t batches = 0;
    std::size_t combiners = 0;
    std::size_t outOfTable = 0;

    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        utils::OsFileSystem vfs(test::pathText(files[i].parent_path()));
        m2::Parser parser;
        const m2::Model model = parser.parse(vfs, test::pathText(files[i]));
        if (model.skinProfiles.empty()) {
            continue;
        }
        ++models;

        m2_core::Context context;
        for (u32 t = 0; t < static_cast<u32>(model.textures.size()); ++t) {
            context.textureIndexMap.push_back(t);
        }

        Diagnostics diagnostics;
        for (const m2::Batch& batch : model.skinProfiles[0].batches) {
            ++batches;
            const Material material = m2_core::ImportBatch(model, batch, context, diagnostics);
            if (material.Common().kind() == MaterialKind::Combiners) {
                ++combiners;
            }
        }
        outOfTable += diagnostics.countOf(DiagCode::UnknownShaderCombo);
    }

    if (batches == 0) {
        SKIP("no M2 batches parsed");
    }

    std::cout << "\n=== M2 batch import ===\n"
              << "  models        : " << models << " of " << limit << " scanned\n"
              << "  batches       : " << batches << "\n"
              << "  Combiners     : " << combiners << "\n"
              << "  unknown combo : " << outOfTable << "\n";

    // §7.2.6: every WoW material is `Combiners`. Nothing in the M2 path produces
    // any other kind, so a shortfall means a batch fell out of the chain build.
    CHECK(combiners == batches);
}
