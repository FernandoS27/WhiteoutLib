// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m2_core.h"

#include "../native/m2_copy.h"
#include "m2_shader_table.h"

#include <string>

namespace whiteout {
namespace models {
namespace wem {
namespace m2_core {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

/// `Batch::flags` bit 0x40 — it changes what `BeginDraw` multiplies, so it is
/// not a hint anything downstream can infer.
constexpr u8 kBatchProjectedOrUnlitWeight = 0x40;

BlendMode blendFor(u16 blendingMode) {
    switch (static_cast<native::M2BlendingMode>(blendingMode)) {
    case native::M2BlendingMode::Opaque:
        return BlendMode::Opaque;
    case native::M2BlendingMode::AlphaKey:
        return BlendMode::AlphaKey;
    case native::M2BlendingMode::Alpha:
        return BlendMode::AlphaBlend;
    case native::M2BlendingMode::NoAlphaAdd:
        return BlendMode::AdditiveAlpha;
    case native::M2BlendingMode::Add:
        return BlendMode::Additive;
    case native::M2BlendingMode::Mod:
        return BlendMode::Modulate;
    case native::M2BlendingMode::Mod2x:
        return BlendMode::Modulate2x;
    case native::M2BlendingMode::BlendAdd:
        return BlendMode::BlendAdd;
    default:
        return BlendMode::Opaque;
    }
}

u16 blendingModeFor(BlendMode blend) {
    native::M2BlendingMode mode = native::M2BlendingMode::Opaque;
    switch (blend) {
    case BlendMode::AlphaKey:
    case BlendMode::Transparent:
        mode = native::M2BlendingMode::AlphaKey;
        break;
    case BlendMode::AlphaBlend:
        mode = native::M2BlendingMode::Alpha;
        break;
    case BlendMode::AdditiveAlpha:
        mode = native::M2BlendingMode::NoAlphaAdd;
        break;
    case BlendMode::Additive:
        mode = native::M2BlendingMode::Add;
        break;
    case BlendMode::Modulate:
        mode = native::M2BlendingMode::Mod;
        break;
    case BlendMode::Modulate2x:
        mode = native::M2BlendingMode::Mod2x;
        break;
    case BlendMode::BlendAdd:
    case BlendMode::PremultipliedAlpha:
        mode = native::M2BlendingMode::BlendAdd;
        break;
    default:
        break;
    }
    return static_cast<u16>(mode);
}

/// One combo-table read, reported rather than clamped: a unit that silently
/// samples entry 0 is indistinguishable from a unit that meant to.
///
/// An **empty** table is not that case. Several of these lookups were retired
/// after WotLK and ship absent; "no lookup here" is the format speaking, not a
/// bad index, and reporting it buried the real overruns under 28k false ones.
/// A short table the batch runs off the end of is still worth saying -- as a
/// warning, because the client skips such a unit rather than refusing the draw.
bool comboAt(const std::vector<u16>& table, u32 base, u32 unit, const char* name, u16& value,
             Diagnostics& out) {
    if (table.empty()) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(base) + unit;
    if (index >= table.size()) {
        out.warn(DiagCode::IndexOutOfRange,
                 std::string(name) + "[" + number(index) + "] of " + number(table.size()),
                 ElementRef(ElementKind::Layer, unit), ProfileId::Wow);
        return false;
    }
    value = table[index];
    return true;
}

} // namespace

// ============================================================================
// Context
// ============================================================================

u32 Context::toDocument(u32 m2TextureId) const {
    if (m2TextureId >= textureIndexMap.size()) {
        return kInvalidIndex;
    }
    return textureIndexMap[m2TextureId];
}

u32 Context::toM2(u32 documentTextureId) const {
    for (std::size_t i = 0; i < textureIndexMap.size(); ++i) {
        if (textureIndexMap[i] == documentTextureId) {
            return static_cast<u32>(i);
        }
    }
    return 0;
}

// ============================================================================
// Import
// ============================================================================

Material ImportBatch(const m2::Model& model, const m2::Batch& batch, const Context& context,
                     Diagnostics& out) {
    Material result;
    CommonMaterial& common = result.InitCommon();

    native::M2Material block;
    block.sourceVersion = context.sourceVersion;
    block.shaderId = batch.shaderId;
    block.priorityPlane = batch.priorityPlane;
    block.materialLayer = batch.materialLayer;
    block.colorIndex = batch.colorIndex;
    block.batchFlags = batch.flags;

    // --- the render record --------------------------------------------------
    if (batch.materialIndex < model.materials.size()) {
        const m2::Material& record = model.materials[batch.materialIndex];
        block.renderFlags = record.flags;
        block.blendingMode = record.blendingMode;
        common.blend = blendFor(record.blendingMode);

        const auto flags = static_cast<native::M2MaterialFlag>(record.flags);
        if (hasFlag(flags, native::M2MaterialFlag::TwoSided)) {
            common.cull = CullMode::None;
        }
        if (hasFlag(flags, native::M2MaterialFlag::Unlit)) {
            common.flags |= MaterialFlags::Unlit;
        }
        if (hasFlag(flags, native::M2MaterialFlag::Unfogged)) {
            common.flags |= MaterialFlags::Unfogged;
        }
        // The M2 bits are "enable", not "disable": a record that names neither
        // draws with depth test and write on, which is the engine's default.
        common.depth.test = !hasFlag(flags, native::M2MaterialFlag::DepthTest);
        common.depth.write = !hasFlag(flags, native::M2MaterialFlag::DepthWrite);
    } else {
        out.error(DiagCode::IndexOutOfRange,
                  "batch names render record " + number(batch.materialIndex) + " of " +
                      number(model.materials.size()),
                  ElementRef(), ProfileId::Wow);
    }
    common.priorityPlane = batch.priorityPlane;

    // --- the join -----------------------------------------------------------
    for (u32 unit = 0; unit < batch.textureCount; ++unit) {
        native::M2TextureUnit resolved;
        u16 value = 0;
        if (comboAt(model.textureCombos, batch.textureComboIndex, unit, "textureCombos", value,
                    out)) {
            resolved.texture = value;
        }
        if (comboAt(model.textureCoordCombos, batch.textureCoordComboIndex, unit,
                    "textureCoordCombos", value, out)) {
            resolved.uvSet = value;
        }
        if (comboAt(model.textureTransformCombos, batch.textureTransformComboIndex, unit,
                    "textureTransformCombos", value, out)) {
            // Live data, unlike `ribbonColorIndex` sitting next to it in the
            // file — which is exactly why it is stored per unit.
            resolved.transform = value;
        }
        if (comboAt(model.textureWeightCombos, batch.textureWeightComboIndex, unit,
                    "textureWeightCombos", value, out)) {
            resolved.weight = value;
        }
        if (resolved.texture < model.textures.size()) {
            const m2::Texture& texture = model.textures[resolved.texture];
            resolved.textureType = static_cast<u8>(texture.type);
            // In M2 the two are the same number — type 0 is a file path and
            // every other value names the replaceable slot the caller must fill
            // — so `replaceableId` is the type, or 0 when there is nothing to
            // fill. The pair exists because §7.3 names both, and because in WC3
            // they are genuinely different fields.
            resolved.replaceableId = texture.type == 0 ? 0 : static_cast<u8>(texture.type);
        }
        block.units.push_back(resolved);
    }

    // --- the chain ----------------------------------------------------------
    bool outOfTable = false;
    const M2PixelShader shader = PixelShaderFor(batch.textureCount, batch.shaderId, outOfTable);
    if (outOfTable) {
        out.warn(DiagCode::UnknownShaderCombo,
                 "shaderId 0x" + number(batch.shaderId) + " names explicit combo row " +
                     number(batch.shaderId & 0x7FFFu) + ", past the transcribed " +
                     number(kNumShaderEffects) + "; using the bit-field path, not row 0",
                 ElementRef(), ProfileId::Wow);
    }
    const Chain chain = ChainOf(shader);
    if (chain.unexpressed != nullptr) {
        out.info(DiagCode::LossyKindConversion,
                 std::string(ToString(shader)) + ": " + chain.unexpressed);
    }

    CombinersBody body;
    const u32 stages = chain.stageCount < block.units.size() ? chain.stageCount
                                                             : static_cast<u32>(block.units.size());
    for (u32 i = 0; i < stages; ++i) {
        CombinerStage stage;
        stage.input.texture = context.toDocument(block.units[i].texture);
        if (stage.input.texture == kInvalidIndex) {
            out.warn(DiagCode::TextureUnresolved,
                     "unit " + number(i) + " names texture " + number(block.units[i].texture) +
                         ", which the document has no entry for",
                     ElementRef(ElementKind::Layer, i), ProfileId::Wow);
        }
        stage.input.uvSet = block.units[i].uvSet;
        stage.rgb = chain.rgb[i];
        stage.alpha = chain.alpha[i];
        // `M2TextureUnit::weight` is an index into `textureWeights`, not a
        // value: the transparency is a track. So there is no static weight to
        // put here, and the shipped rule — only unit 0 takes it, and the
        // *product* of colour and transparency is what draws — is a statement
        // about that track. It targets the stage through §10.8's material
        // channels at P7; the index is in the native block until then.
        stage.input.weight = 1.0f;
        body.stages.push_back(std::move(stage));
    }
    if (block.units.size() > chain.stageCount) {
        out.info(DiagCode::LayerDropped,
                 std::string(ToString(shader)) + " reads " + number(chain.stageCount) +
                     " of the batch's " + number(block.units.size()) + " texture units",
                 ElementRef(), ProfileId::Wow);
    }
    common.body = std::move(body);

    if ((batch.flags & kBatchProjectedOrUnlitWeight) != 0) {
        out.info(DiagCode::LossyKindConversion,
                 "batch flag 0x40 changes what BeginDraw multiplies; the common material "
                 "cannot say so and the native block keeps it",
                 ElementRef(), ProfileId::Wow);
    }

    result.SetNativeInSync(std::move(block));
    return result;
}

// ============================================================================
// Export
// ============================================================================

namespace {

/// Appends @p units to the combo tables and returns the base index each ran to.
void appendCombos(const std::vector<native::M2TextureUnit>& units, m2::Model& model,
                  m2::Batch& batch) {
    batch.textureCount = static_cast<u16>(units.size());
    batch.textureComboIndex = static_cast<u16>(model.textureCombos.size());
    batch.textureCoordComboIndex = static_cast<u16>(model.textureCoordCombos.size());
    batch.textureTransformComboIndex = static_cast<u16>(model.textureTransformCombos.size());
    batch.textureWeightComboIndex = static_cast<u16>(model.textureWeightCombos.size());
    for (const native::M2TextureUnit& unit : units) {
        model.textureCombos.push_back(unit.texture);
        model.textureCoordCombos.push_back(unit.uvSet);
        model.textureTransformCombos.push_back(unit.transform);
        model.textureWeightCombos.push_back(unit.weight);
    }
}

u16 pushRenderRecord(u16 flags, u16 blendingMode, m2::Model& model) {
    // The record is two u16s and the same pair recurs constantly, so reuse an
    // existing row rather than growing the array once per batch.
    for (std::size_t i = 0; i < model.materials.size(); ++i) {
        if (model.materials[i].flags == flags && model.materials[i].blendingMode == blendingMode) {
            return static_cast<u16>(i);
        }
    }
    m2::Material record;
    record.flags = flags;
    record.blendingMode = blendingMode;
    model.materials.push_back(record);
    return static_cast<u16>(model.materials.size() - 1);
}

} // namespace

m2::Batch ExportMaterial(const Material& material, const Context& context, m2::Model& model,
                         Diagnostics& out) {
    m2::Batch batch;

    // §7.1: a native block that is not stale IS the answer, and for M2 that
    // matters more than elsewhere — the resolved batch is the only place the
    // join survives at all.
    if (material.hasNative() && material.sync() != NativeSync::CommonEdited &&
        material.nativeKind() == NativeKind::M2) {
        const auto& block = std::get<native::M2Material>(material.Native());
        batch.flags = block.batchFlags;
        batch.priorityPlane = static_cast<i8>(block.priorityPlane);
        batch.shaderId = block.shaderId;
        batch.colorIndex = block.colorIndex;
        batch.materialLayer = block.materialLayer;
        batch.materialIndex = pushRenderRecord(block.renderFlags, block.blendingMode, model);
        appendCombos(block.units, model, batch);
        return batch;
    }

    const CommonMaterial& common = material.Common();
    batch.priorityPlane = static_cast<i8>(common.priorityPlane);
    batch.materialLayer = 0;

    u16 flags = 0;
    if (common.cull == CullMode::None) {
        flags |= static_cast<u16>(native::M2MaterialFlag::TwoSided);
    }
    if (hasFlag(common.flags, MaterialFlags::Unlit)) {
        flags |= static_cast<u16>(native::M2MaterialFlag::Unlit);
    }
    if (hasFlag(common.flags, MaterialFlags::Unfogged)) {
        flags |= static_cast<u16>(native::M2MaterialFlag::Unfogged);
    }
    if (!common.depth.test) {
        flags |= static_cast<u16>(native::M2MaterialFlag::DepthTest);
    }
    if (!common.depth.write) {
        flags |= static_cast<u16>(native::M2MaterialFlag::DepthWrite);
    }
    batch.materialIndex = pushRenderRecord(flags, blendingModeFor(common.blend), model);

    std::vector<native::M2TextureUnit> units;
    if (const CombinersBody* body = common.combiners()) {
        for (const CombinerStage& stage : body->stages) {
            native::M2TextureUnit unit;
            unit.texture = static_cast<u16>(context.toM2(stage.input.texture));
            unit.uvSet = static_cast<u16>(stage.input.uvSet);
            units.push_back(unit);
        }
        // No `shaderId` is recoverable from a chain: the table is many-to-one,
        // several rows produce the same ops, and the modifiers a row carries are
        // exactly what the chain does not record. Leaving it 0 (the bit-field
        // path's `Combiners_Opaque` / `Combiners_Mod`) is the honest answer,
        // with a note.
        batch.shaderId = 0;
        out.warn(DiagCode::UnknownShaderCombo,
                 "no shaderId is recoverable from a combiner chain; the batch gets the "
                 "bit-field path's id and the chain's modifiers are gone",
                 ElementRef(), ProfileId::Wow);
    } else {
        out.warn(DiagCode::UnsupportedMaterialKind,
                 std::string("an M2 export needs a Combiners material, not ") +
                     ToString(common.kind()),
                 ElementRef(), ProfileId::Wow);
    }
    appendCombos(units, model, batch);
    return batch;
}

} // namespace m2_core
} // namespace wem
} // namespace models
} // namespace whiteout
