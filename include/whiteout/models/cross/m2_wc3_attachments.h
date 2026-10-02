// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m2_wc3_attachments.h
 * @brief World of Warcraft's attachment points named as Warcraft III's, inside WEM.
 *
 * A `.m2` attachment is known by its id (19 is `Base`, 18 `PlayerName`), which
 * the import keeps as `m2AttachmentId` and spells `attachment_<id>`. Warcraft
 * III finds a point by the words of its name instead: it reads them up to the
 * first it does not know (`CLinkPtTokenizer`), and a lookup takes the point
 * holding the most of the words asked for, the first of them required
 * (`FindBestMatches`). So a point both games have takes Warcraft III's
 * spelling, `Origin Ref`, and every other keeps World of Warcraft's name as a
 * single word, `ShoulderRight Ref`, which no lookup finds and which can never
 * win one meant for another point.
 *
 * StarCraft II matches whole names against its own keywords, which include
 * `Ref_Shoulder Right`, so the M3 export restates the equivalents through the
 * nomenclature it applies to a Warcraft III model and the rest from their words.
 */

#include <optional>
#include <string>
#include <string_view>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

namespace whiteout {
namespace models {
namespace cross {

/// @brief World of Warcraft's name for attachment @p id, a word at a time:
///        `Shoulder Right`, `Vehicle Seat 1`; `Attachment <id>` for an id the
///        table does not name.
std::string M2AttachmentWords(u32 id);

/// @brief Warcraft III's own point for attachment @p id (`Origin Ref` for
///        `Base`), or nothing where it has none.
std::optional<std::string_view> M2Wc3AttachmentEquivalent(u32 id);

/// @brief The name `.m2` attachment @p id goes by in Warcraft III: its
///        equivalent, or its words as one (`ShoulderRight Ref`).
std::string M2Wc3AttachmentName(u32 id);

struct M2AttachmentReport {
    u32 matched = 0; ///< Points that took Warcraft III's name for them.
    u32 kept = 0;    ///< Points Warcraft III has no name for, kept under World of Warcraft's.
    wem::Diagnostics diagnostics;
};

/// Every attachment a `.m2` import made — the ones carrying an `m2AttachmentId`
/// — takes its Warcraft III name. Its `attachmentId` is the ATCH index `toMdx`
/// gives any attachment without an `.mdx` id of its own.
M2AttachmentReport CrossM2Attachments(wem::Document& document);

} // namespace cross
} // namespace models
} // namespace whiteout
