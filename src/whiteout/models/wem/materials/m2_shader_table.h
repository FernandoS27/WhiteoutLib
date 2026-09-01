// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2_shader_table.h
 * @brief `shaderId` -> a combiner chain (design §7.2.2).
 *
 * ### Why this is authored and not generated
 *
 * §15.2 calls the M2 ingredient tables "generated", and the render-flag
 * vocabulary and the batch record are — they exist in the m2 headers. **This one
 * cannot be.** `s_modelShaderEffect` is a table in the client's `.exe`, not in
 * any file format, so no header the generator can read contains it. It is a
 * transcription, and a transcription is authored data with a provenance comment,
 * which is what this file is.
 *
 * Transcribed from retail 12.1.0.69404 @ 0x14443E9F0 (36 rows of {u32 pixel, u32
 * vertex}) and diffed against 6.0.1's @ 0x101c74150. The pixel column — the only
 * column WEM needs, since WEM wants ops and not shader ids — is identical across
 * both clients for all 30 shared rows.
 *
 * ### The row-0 fallback is a defect, and it is reproduced here as a refusal
 *
 * A `shaderId` past the transcribed rows must produce a **diagnostic and a
 * best-effort chain**, never row 0. Row 0 is `Opaque_Mod2xNA_Alpha` — an opaque
 * two-texture environment combiner, the worst available guess — and silently
 * choosing it is the recorded black-box artifact. `ChainFor` returns the
 * bit-field path's answer instead and says it did.
 *
 * ### What a chain cannot say
 *
 * `CombinerOp` has six values and the shipped shader names have modifiers the six
 * cannot express: `AddAlpha` scales the add by the unit's alpha, `Wgt` weights it,
 * `Dual_Crossfade` interpolates three units, `Depth` fades on depth. Each row
 * therefore carries `unexpressed` — the text of what the chain leaves out — and a
 * converter turns it into one diagnostic rather than pretending the chain is
 * exact.
 */

#include <whiteout/models/wem/materials/common.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m2_core {

/// `s_modelPixelShaders`. The order is load-bearing: it is the index space the
/// explicit-combo table selects into.
enum class M2PixelShader : u8 {
    Combiners_Opaque = 0,
    Combiners_Mod,
    Combiners_Opaque_Mod,
    Combiners_Opaque_Mod2x,
    Combiners_Opaque_Mod2xNA,
    Combiners_Opaque_Opaque,
    Combiners_Mod_Mod,
    Combiners_Mod_Mod2x,
    Combiners_Mod_Add,
    Combiners_Mod_Mod2xNA,
    Combiners_Mod_AddNA,
    Combiners_Mod_Opaque,
    Combiners_Opaque_Mod2xNA_Alpha,
    Combiners_Opaque_AddAlpha,
    Combiners_Opaque_AddAlpha_Alpha,
    Combiners_Opaque_Mod2xNA_Alpha_Add,
    Combiners_Mod_AddAlpha,
    Combiners_Mod_AddAlpha_Alpha,
    Combiners_Opaque_Alpha_Alpha,
    Combiners_Opaque_Mod2xNA_Alpha_3s,
    Combiners_Opaque_AddAlpha_Wgt,
    Combiners_Mod_Add_Alpha,
    Combiners_Opaque_ModNA_Alpha,
    Combiners_Mod_AddAlpha_Wgt,
    Combiners_Opaque_Mod_Add_Wgt,
    Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha,
    Combiners_Mod_Dual_Crossfade,
    Combiners_Opaque_Mod2xNA_Alpha_Alpha,
    Combiners_Mod_Masked_Dual_Crossfade,
    Combiners_Opaque_Alpha,
    Guild,
    Guild_NoBorder,
    Guild_Opaque,
    Combiners_Mod_Depth,
    Illum,
    Combiners_Unnamed_35,
    Combiners_Mod_Mod_Depth,
    Count,
};

const char* ToString(M2PixelShader shader);

/// 36 rows in 12.1.0.69404; 6.0.1 stopped at 30, and the corpus ships 33/34/35.
inline constexpr u32 kNumShaderEffects = 36;

/// `shaderId & 0x8000` — the batch names a pre-baked combo rather than letting
/// the bit fields compute one.
constexpr bool IsExplicitCombo(u16 shaderId) {
    return (shaderId & 0x8000u) != 0;
}

/// The chain one pixel shader stands for, plus what it leaves unsaid.
struct Chain {
    u8 stageCount = 1;
    CombinerOp rgb[4] = {CombinerOp::Opaque, CombinerOp::Opaque, CombinerOp::Opaque,
                         CombinerOp::Opaque};
    CombinerOp alpha[4] = {CombinerOp::Opaque, CombinerOp::Opaque, CombinerOp::Opaque,
                           CombinerOp::Opaque};
    /// Null when the chain is exact; otherwise what `CombinerOp` cannot express.
    const char* unexpressed = nullptr;
};

/// The pixel shader @p shaderId and @p textureCount select. `outOfTable` is set
/// when an explicit combo named a row past the table — the caller must
/// diagnose, and the answer is the bit-field path's, never row 0.
M2PixelShader PixelShaderFor(u32 textureCount, u16 shaderId, bool& outOfTable);

/// The chain for a pixel shader.
Chain ChainOf(M2PixelShader shader);

} // namespace m2_core
} // namespace wem
} // namespace models
} // namespace whiteout
