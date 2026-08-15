// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file types.h
 * @brief Container versions, header descriptors and field layout structures
 *        shared by every DBC / DB2 version.
 *
 * The client database formats went through eleven container revisions between
 * Vanilla and current retail. They all describe the same thing — a table of
 * fixed-layout records plus a string block — so this module normalises every
 * revision onto the descriptors below and keeps the differences confined to
 * the readers.
 */

#if !defined(WHITEOUT_HAS_DATABASE)
#error                                                                                             \
    "<whiteout/database/types.h> requires database support. Configure with -DWHITEOUT_ENABLE_DATABASE=ON and link against the whiteout_database target."
#endif

#include <whiteout/common_types.h>

#include <string>
#include <vector>

namespace whiteout::database {

// ============================================================================
// Versions
// ============================================================================

/// Container revision, identified by the file's four-byte magic.
enum class Version {
    Unknown,
    WDBC, ///< 'WDBC' — Vanilla … Mists of Pandaria (.dbc)
    WDB2, ///< 'WDB2' — Cataclysm 4.0.1 … Legion 7.0.1.20740
    WDB3, ///< 'WDB3' — Legion 7.0.1.20740 … 7.0.1.20810
    WDB4, ///< 'WDB4' — Legion 7.0.1.20810 … 7.0.3.21414
    WDB5, ///< 'WDB5' — Legion 7.0.3.21479 … 7.2.0.23436
    WDB6, ///< 'WDB6' — Legion 7.2.0.23436 … 7.3.5.25600
    WDC1, ///< 'WDC1' — Legion 7.3.5.25600 … BfA 8.0.1.26231
    WDC2, ///< 'WDC2' / '1SLC' — BfA 8.0.1.26231 … 8.1.0.28048
    WDC3, ///< 'WDC3' — BfA 8.1.0.28048 … Dragonflight 10.1.0.48480
    WDC4, ///< 'WDC4' — Dragonflight 10.1.0.48480 … 10.2.5.52393
    WDC5, ///< 'WDC5' — Dragonflight 10.2.5.52432 and later
};

/// Human-readable four-character name of a version ("WDC3", …).
const char* versionName(Version version);

// ============================================================================
// Header flags (WDB4 and later)
// ============================================================================

namespace TableFlags {
constexpr u32 OffsetMap = 0x01;        ///< Variable-length records with inline strings.
constexpr u32 RelationshipData = 0x02; ///< Secondary keys; also reorders WDC4+ sections.
constexpr u32 NonInlineIds = 0x04;     ///< Record ids live in a separate id list.
constexpr u32 Bitpacked = 0x10;        ///< Record data contains bitpacked fields.
} // namespace TableFlags

// ============================================================================
// Locales
// ============================================================================

/// Locale index as used by the header's `locale` field and by the 16-slot
/// localized string columns of pre-Cataclysm DBCs (see Schema::locStringSlots).
enum class Locale : u32 {
    enUS = 0, ///< Also enGB.
    koKR = 1,
    frFR = 2,
    deDE = 3,
    zhCN = 4,
    zhTW = 5,
    esES = 6,
    esMX = 7,
    ruRU = 8, ///< Added in The Burning Crusade.
    jaJP = 9,
    ptPT = 10, ///< Also ptBR. Added in Wrath of the Lich King.
    itIT = 11,
};

/// Locale tag ("enUS", …) for a locale index, or "unknown" when out of range.
const char* localeName(u32 locale);

// ============================================================================
// Field layout
// ============================================================================

/// How a field's value is physically stored, mirroring the client's
/// `field_compression` enum (WDC1 and later).
enum class FieldStorage : u32 {
    /// Plain 1-, 2-, 3-, 4- or 8-byte little-endian integer in the record.
    None = 0,
    /// Bitpacked integer at `offsetBits`, `sizeBits` wide.
    Bitpacked = 1,
    /// Absent from the record: `defaultValue` unless the row's id appears in
    /// the common-data block.
    CommonData = 2,
    /// Bitpacked index into the pallet block.
    BitpackedIndexed = 3,
    /// Bitpacked index into the pallet block, addressing a whole array at once.
    BitpackedIndexedArray = 4,
    /// Same as Bitpacked, but sign-extended.
    BitpackedSigned = 5,
};

/// Physical description of one field (column) of the table.
///
/// Only the layout is described — the DB2 formats do not record whether a
/// field holds an integer, a float or a string reference. Supply a Schema to
/// get typed access.
struct FieldInfo {
    FieldStorage storage = FieldStorage::None;
    u32 offsetBits = 0;   ///< Bit offset of the field within the record.
    u32 sizeBits = 0;     ///< Total width in bits, summed over all array elements.
    u32 arrayCount = 1;   ///< Number of elements; 1 for scalars.
    u32 elementSize = 4;  ///< Bytes per element (FieldStorage::None only).
    u32 defaultValue = 0; ///< FieldStorage::CommonData only.
    bool signExtend = false;
    /// Byte offset of this field's slice of the pallet or common-data block.
    u32 additionalDataOffset = 0;
    u32 additionalDataSize = 0;
};

// ============================================================================
// Sections
// ============================================================================

/// One section of a WDC2+ file. Earlier versions report a single section
/// covering the whole file.
struct SectionInfo {
    u64 tactKeyHash = 0; ///< Non-zero when the section's records are encrypted.
    u32 fileOffset = 0;
    u32 recordCount = 0;
    u32 stringTableSize = 0;
    u32 copyCount = 0;
    u32 offsetMapCount = 0;
    /// True when tactKeyHash is set and the section could not be decrypted,
    /// i.e. its record data is unusable. Its rows are still listed, flagged.
    bool encrypted = false;
    /// Record ids known to be encrypted (WDC4+ `encrypted_status` block).
    std::vector<u32> encryptedIds;
};

// ============================================================================
// Table header
// ============================================================================

/// Normalised header. Fields a given version does not carry stay zero.
struct TableInfo {
    Version version = Version::Unknown;
    u32 magic = 0;

    u32 recordCount = 0;     ///< Records across all sections, before copy-table expansion.
    u32 fieldCount = 0;      ///< Fields described by the field structure block.
    u32 recordSize = 0;      ///< Bytes per record (fixed-layout tables).
    u32 stringTableSize = 0; ///< Bytes of string data across all sections.

    u32 tableHash = 0;  ///< SStrHash of the table name.
    u32 layoutHash = 0; ///< Changes whenever the column layout changes (WDB5+).
    u32 build = 0;      ///< Client build (WDBC has none; WDB5+ reuse the slot for layoutHash).
    u32 timestamp = 0;  ///< time(0) at write, WDB2 … WDB4 only.

    u32 minId = 0;
    u32 maxId = 0;
    u32 locale = 0; ///< Locale index; see Locale.
    u32 flags = 0;  ///< See TableFlags.
    u32 idIndex = 0;

    u32 totalFieldCount = 0;     ///< Includes common-data-only columns (WDB6+).
    u32 bitpackedDataOffset = 0; ///< Where bitpacked data starts in a record.
    u32 lookupColumnCount = 0;
    u32 sectionCount = 1;

    u32 versionNumber = 0;    ///< WDC5 only.
    std::string schemaString; ///< WDC5 only, e.g. "WowStatic_Patch_10_2_5".

    bool hasOffsetMap = false;
    bool hasRelationshipData = false;
    bool hasNonInlineIds = false;
    bool isBitpacked = false;
};

} // namespace whiteout::database
