// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/sno/sno_types.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout {
namespace sno {

/// An entry from the CoreTOC — maps an SNO ID to its group and name.
struct TocEntry {
    SnoGroup group;
    i32 snoId;
    std::string name;
};

/// Detected CoreTOC format.
enum class CoreTocFormat {
    Unknown,
    D3Legacy, ///< Diablo III: no magic, 70 fixed groups, 4 header arrays
    D4Old,    ///< Diablo IV old: firstWord = snoGroupsCount
    D4New,    ///< Diablo IV new: magic 0xBCDE6611
};

/// Parser for CoreTOC.dat files from Diablo III and Diablo IV.
///
/// The CoreTOC (Core Table of Contents) is the master index of every asset in
/// the game. It maps (SnoGroup, SnoId) pairs to human-readable names.
///
/// Supports three formats:
/// - **D3 Legacy**: No magic number, 70 fixed SNO groups, 4 header arrays,
///   per-group sections with inline name strings.
/// - **D4 Old**: First word is snoGroupsCount, 3 header arrays.
/// - **D4 New**: Magic 0xBCDE6611, 4 header arrays including format hashes.
///
/// Usage:
/// @code
///   auto tocData = readFile("CoreTOC.dat");
///   CoreToc toc;
///   if (toc.parse(tocData)) {
///       for (auto& entry : toc.entriesForGroup(SnoGroup::Actor)) {
///           std::cout << entry.name << "\n";
///       }
///   }
/// @endcode
class CoreToc {
public:
    CoreToc() = default;

    /// Parse a CoreTOC.dat file from raw bytes.
    /// Auto-detects format (D3 Legacy, D4 Old, D4 New).
    /// @returns true on success, false if the data is malformed.
    bool parse(std::span<const u8> data);

    /// Returns the detected format after a successful parse.
    CoreTocFormat format() const {
        return m_format;
    }

    /// Return all entries for a given SNO group.
    /// Returns an empty span if the group has no entries.
    std::span<const TocEntry> entriesForGroup(SnoGroup group) const;

    /// Look up a single entry by its SNO ID. Returns nullptr if not found.
    const TocEntry* findById(i32 snoId) const;

    /// Total number of entries across all groups.
    size_t size() const {
        return m_all.size();
    }

    /// All entries, in parse order.
    std::span<const TocEntry> entries() const {
        return m_all;
    }

    /// Per-group format hash table (group index → format hash).
    /// Only present in the "new" CoreTOC format (magic 0xBCDE6611).
    const std::unordered_map<i32, u32>& formatHashes() const {
        return m_formatHashes;
    }

private:
    std::vector<TocEntry> m_all;

    /// group-id → (start index, count) into m_all.
    std::unordered_map<i32, std::pair<size_t, size_t>> m_groupIndex;

    /// snoId → index into m_all.
    std::unordered_map<i32, size_t> m_idIndex;

    /// group → format hash
    std::unordered_map<i32, u32> m_formatHashes;

    /// Detected format.
    CoreTocFormat m_format = CoreTocFormat::Unknown;

    /// Parse D3's legacy CoreTOC format (no magic, 70 fixed groups).
    bool parseD3Legacy(std::span<const u8> data);

    /// Parse D4's CoreTOC format (old or new).
    bool parseD4(std::span<const u8> data, bool newFormat);
};

} // namespace sno
} // namespace whiteout
