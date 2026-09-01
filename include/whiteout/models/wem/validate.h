// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file validate.h
 * @brief Document validation at three levels (WEM v3, design §5.7, §6.5).
 *
 * Three levels, because the answers differ by intent:
 *
 * - **Structural** — always, cheap. A violation means the document is corrupt.
 * - **Manifold** — the §5.10 interop contract. After a clean import this set is
 *   empty *by construction*; it is here to catch a bad edit, and to answer
 *   "could a strict half-edge library take this mesh right now" in one call.
 * - **Profile** — §5.7's third group plus §6.3's coverage rule.
 *
 * Levels are cumulative: `Profile` runs `Manifold` runs `Structural`.
 *
 * The profile is enforced at exactly three sites (§6.5) and this is the first;
 * the other two are export and native-block construction. In particular the
 * *geometry* layer never consults a profile — limits are checked here, not
 * enforced during editing, because enforcing mid-edit is what turns "temporarily
 * 5 influences while I redistribute weights" into a spurious failure.
 *
 * Rules register into one table per level, in `validate.cpp`. Each later phase
 * appends rules to those tables; the tables are the single place that knows what
 * "valid" means, which is also why §10.6's node-referencer table lives beside
 * them.
 */

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "diagnostics.h"

namespace whiteout {
namespace models {
namespace wem {

struct Document; ///< Defined in document.h (P3).

enum class ValidateLevel : u8 {
    Structural = 0, ///< Connectivity and index ranges.
    Manifold = 1,   ///< Structural, plus the §5.10 contract.
    Profile = 2,    ///< Manifold, plus per-profile limits and coverage.
};

const char* ToString(ValidateLevel level);

/// A single check. Rules never mutate; they only report.
using ValidationRule = void (*)(const Document& document, Diagnostics& out);

/**
 * @brief Run every rule at or below @p level and return what they found.
 *
 * A returned report with no errors means the document satisfies the level. Info
 * and warning entries are normal output, not failures.
 */
Diagnostics Validate(const Document& document, ValidateLevel level);

/// The rules that would run at @p level exactly (not the levels below it).
/// Exposed so a test can assert a phase actually registered its rules.
std::span<const ValidationRule> ValidationRulesFor(ValidateLevel level);

} // namespace wem
} // namespace models
} // namespace whiteout
