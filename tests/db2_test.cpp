// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Hand-built fixtures for every DBC / DB2 container revision.
///
/// The corpus test covers WDC5 against real game data; these fixtures pin the
/// versions no corpus is available for, plus the structural corner cases
/// (copy tables, common data, pallet data, sparse records, sections).

#include <catch2/catch_all.hpp>

#include <whiteout/database/parser.h>
#include <whiteout/database/schema.h>
#include <whiteout/database/table.h>

#include <cstring>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::database;

namespace {

/// Little-endian byte builder.
struct Blob {
    std::vector<u8> bytes;

    size_t size() const {
        return bytes.size();
    }

    Blob& tag(const char* fourcc) {
        bytes.insert(bytes.end(), fourcc, fourcc + 4);
        return *this;
    }
    Blob& u8v(u8 v) {
        bytes.push_back(v);
        return *this;
    }
    Blob& u16v(u16 v) {
        bytes.push_back(static_cast<u8>(v & 0xFF));
        bytes.push_back(static_cast<u8>(v >> 8));
        return *this;
    }
    Blob& i16v(i16 v) {
        return u16v(static_cast<u16>(v));
    }
    Blob& u32v(u32 v) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
        }
        return *this;
    }
    Blob& u64v(u64 v) {
        for (int i = 0; i < 8; ++i) {
            bytes.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
        }
        return *this;
    }
    Blob& f32v(f32 v) {
        u32 raw = 0;
        std::memcpy(&raw, &v, 4);
        return u32v(raw);
    }
    /// NUL-terminated string.
    Blob& cstr(const char* s) {
        const size_t len = std::strlen(s);
        bytes.insert(bytes.end(), s, s + len);
        bytes.push_back(0);
        return *this;
    }
    Blob& pad(size_t count, u8 value = 0) {
        bytes.insert(bytes.end(), count, value);
        return *this;
    }
    Blob& append(const Blob& other) {
        bytes.insert(bytes.end(), other.bytes.begin(), other.bytes.end());
        return *this;
    }
    void patch32(size_t offset, u32 value) {
        for (int i = 0; i < 4; ++i) {
            bytes[offset + static_cast<size_t>(i)] = static_cast<u8>((value >> (8 * i)) & 0xFF);
        }
    }
};

/// A 12-byte string block shared by several fixtures:
/// offset 1 = "Alpha", offset 7 = "Beta".
Blob stringBlock() {
    Blob b;
    b.u8v(0).cstr("Alpha").cstr("Beta");
    return b;
}

// ============================================================================
// WDBC
// ============================================================================

std::vector<u8> buildWdbc() {
    const Blob strings = stringBlock();
    Blob f;
    f.tag("WDBC")
        .u32v(2)  // record_count
        .u32v(3)  // field_count
        .u32v(12) // record_size
        .u32v(static_cast<u32>(strings.size()));
    f.u32v(1).u32v(42).u32v(1); // id 1, value 42, name "Alpha"
    f.u32v(2).u32v(7).u32v(7);  // id 2, value 7,  name "Beta"
    f.append(strings);
    return f.bytes;
}

// ============================================================================
// WDB2
// ============================================================================

std::vector<u8> buildWdb2() {
    const Blob strings = stringBlock();
    Blob f;
    f.tag("WDB2")
        .u32v(2)  // record_count
        .u32v(3)  // field_count
        .u32v(12) // record_size
        .u32v(static_cast<u32>(strings.size()))
        .u32v(0xAABBCCDD) // table_hash
        .u32v(30000)      // build
        .u32v(12345)      // timestamp_last_written
        .u32v(10)         // min_id
        .u32v(11)         // max_id
        .u32v(0)          // locale
        .u32v(0);         // copy_table_size
    // max_id != 0 → index block: one int + one short per id in [min_id, max_id].
    f.u32v(0).u32v(1);
    f.u16v(6).u16v(5);
    f.u32v(10).u32v(42).u32v(1);
    f.u32v(11).u32v(7).u32v(7);
    f.append(strings);
    return f.bytes;
}

// ============================================================================
// WDB5 — field structure block, inline ids, copy table
// ============================================================================

std::vector<u8> buildWdb5() {
    const Blob strings = stringBlock();
    Blob f;
    f.tag("WDB5")
        .u32v(2)  // record_count
        .u32v(3)  // field_count
        .u32v(16) // record_size
        .u32v(static_cast<u32>(strings.size()))
        .u32v(0xAABBCCDD) // table_hash
        .u32v(0x11223344) // layout_hash
        .u32v(1)          // min_id
        .u32v(2)          // max_id
        .u32v(0)          // locale
        .u32v(8)          // copy_table_size — one entry
        .u16v(0)          // flags
        .u16v(0);         // id_index
    // field_structure: u32 @0, u16[2] @4, u32[2] @8
    f.i16v(0).u16v(0);
    f.i16v(16).u16v(4);
    f.i16v(0).u16v(8);
    f.u32v(1).u16v(70).u16v(71).u32v(11).u32v(22);
    f.u32v(2).u16v(80).u16v(81).u32v(33).u32v(44);
    f.append(strings);
    f.u32v(100).u32v(1); // copy row 100 from row 1
    return f.bytes;
}

// ============================================================================
// WDB6 — common data table
// ============================================================================

std::vector<u8> buildWdb6(bool paddedValues) {
    const Blob strings = stringBlock();

    Blob common;
    common.u32v(4); // num_columns_in_table
    for (int i = 0; i < 3; ++i) {
        common.u32v(0).u8v(4); // count 0, type int
    }
    common.u32v(1).u8v(4).u32v(2); // column 3: one entry, id 2
    if (paddedValues) {
        common.u32v(1234);
    } else {
        common.u16v(1234); // pre-7.3.0: value width follows the type
    }
    // Column 3 is declared as 'short' in the unpadded variant so the width
    // detection has something to detect.
    if (!paddedValues) {
        common.bytes[common.bytes.size() - 7] = 1; // type = short
    }

    Blob f;
    f.tag("WDB6")
        .u32v(2)
        .u32v(3)
        .u32v(16)
        .u32v(static_cast<u32>(strings.size()))
        .u32v(0xAABBCCDD)
        .u32v(0x11223344)
        .u32v(1)
        .u32v(2)
        .u32v(0)
        .u32v(0) // copy_table_size
        .u16v(0) // flags
        .u16v(0) // id_index
        .u32v(4) // total_field_count — one more than field_count
        .u32v(static_cast<u32>(common.size()));
    f.i16v(0).u16v(0);
    f.i16v(16).u16v(4);
    f.i16v(0).u16v(8);
    f.u32v(1).u16v(70).u16v(71).u32v(11).u32v(22);
    f.u32v(2).u16v(80).u16v(81).u32v(33).u32v(44);
    f.append(strings);
    f.append(common);
    return f.bytes;
}

// ============================================================================
// WDC1 — field storage info: bitpacked, pallet, common data + relationship map
// ============================================================================

std::vector<u8> buildWdc1() {
    Blob strings;
    strings.u8v(0).cstr("Alpha").u8v(0); // 8 bytes; "Alpha" at offset 1

    Blob records;
    // byte 4 packs field1 (4 bits) and field2's pallet index (2 bits @ bit 36).
    records.u32v(1).u8v(static_cast<u8>(5 | (1 << 4))).pad(3);
    records.u32v(1).u8v(static_cast<u8>(9 | (0 << 4))).pad(3);

    Blob fieldInfo;
    // f0: plain 32-bit string reference
    fieldInfo.u16v(0).u16v(32).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    // f1: bitpacked, 4 bits at bit 32
    fieldInfo.u16v(32).u16v(4).u32v(0).u32v(1).u32v(0).u32v(0).u32v(0);
    // f2: bitpacked index into pallet_data, 2 bits at bit 36
    fieldInfo.u16v(36).u16v(2).u32v(8).u32v(3).u32v(0).u32v(0).u32v(0);
    // f3: common data, default 77
    fieldInfo.u16v(0).u16v(0).u32v(8).u32v(2).u32v(77).u32v(0).u32v(0);

    Blob f;
    f.tag("WDC1")
        .u32v(2)                                  // record_count
        .u32v(4)                                  // field_count
        .u32v(8)                                  // record_size
        .u32v(static_cast<u32>(strings.size()))   // string_table_size
        .u32v(0xAABBCCDD)                         // table_hash
        .u32v(0x11223344)                         // layout_hash
        .u32v(10)                                 // min_id
        .u32v(20)                                 // max_id
        .u32v(0)                                  // locale
        .u32v(0)                                  // copy_table_size
        .u16v(0x04)                               // flags — non-inline ids
        .u16v(0)                                  // id_index
        .u32v(4)                                  // total_field_count
        .u32v(4)                                  // bitpacked_data_offset
        .u32v(0)                                  // lookup_column_count
        .u32v(0)                                  // offset_map_offset
        .u32v(8)                                  // id_list_size
        .u32v(static_cast<u32>(fieldInfo.size())) // field_storage_info_size
        .u32v(8)                                  // common_data_size
        .u32v(8)                                  // pallet_data_size
        .u32v(20);                                // relationship_data_size
    // field_structure: bitpacked and common fields report a size of 32.
    f.i16v(0).u16v(0);
    f.i16v(32).u16v(4);
    f.i16v(32).u16v(4);
    f.i16v(32).u16v(4);
    f.append(records);
    f.append(strings);
    f.u32v(10).u32v(20); // id_list
    f.append(fieldInfo);
    f.u32v(1000).u32v(2000);       // pallet_data
    f.u32v(20).u32v(555);          // common_data — id 20 overrides the default
    f.u32v(1).u32v(100).u32v(100); // relationship_map header
    f.u32v(100).u32v(0);           // foreign key 100 → record 0
    return f.bytes;
}

// ============================================================================
// WDC2 — sections, and string references relative to the field
// ============================================================================

std::vector<u8> buildWdc2() {
    const Blob strings = stringBlock();

    Blob fieldInfo;
    fieldInfo.u16v(0).u16v(32).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);

    Blob f;
    f.tag("WDC2")
        .u32v(2)                                  // record_count
        .u32v(1)                                  // field_count
        .u32v(4)                                  // record_size
        .u32v(static_cast<u32>(strings.size()))   // string_table_size
        .u32v(0xAABBCCDD)                         // table_hash
        .u32v(0x11223344)                         // layout_hash
        .u32v(10)                                 // min_id
        .u32v(11)                                 // max_id
        .u32v(0)                                  // locale
        .u16v(0x04)                               // flags — non-inline ids
        .u16v(0)                                  // id_index
        .u32v(1)                                  // total_field_count
        .u32v(0)                                  // bitpacked_data_offset
        .u32v(0)                                  // lookup_column_count
        .u32v(static_cast<u32>(fieldInfo.size())) // field_storage_info_size
        .u32v(0)                                  // common_data_size
        .u32v(0)                                  // pallet_data_size
        .u32v(1);                                 // section_count

    const size_t sectionHeaderAt = f.size();
    f.u64v(0)    // tact_key_hash
        .u32v(0) // file_offset — patched below
        .u32v(2) // record_count
        .u32v(static_cast<u32>(strings.size()))
        .u32v(0)  // copy_table_size
        .u32v(0)  // offset_map_offset
        .u32v(8)  // id_list_size
        .u32v(0); // relationship_data_size

    f.i16v(0).u16v(0); // field_structure
    f.append(fieldInfo);

    const u32 sectionStart = static_cast<u32>(f.size());
    f.patch32(sectionHeaderAt + 8, sectionStart);

    // Record data is 8 bytes total, so the string block starts at virtual
    // offset 8; references are relative to the field that holds them.
    f.u32v(8 + 1 - 0); // row 0 → "Alpha"
    f.u32v(8 + 7 - 4); // row 1 → "Beta"
    f.append(strings);
    f.u32v(10).u32v(11); // id_list
    return f.bytes;
}

// ============================================================================
// WDC3 — sparse (offset map) records with inline strings
// ============================================================================

std::vector<u8> buildWdc3Sparse() {
    Blob fieldInfo;
    fieldInfo.u16v(0).u16v(32).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    fieldInfo.u16v(32).u16v(32).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);

    Blob f;
    f.tag("WDC3")
        .u32v(2)          // record_count
        .u32v(2)          // field_count
        .u32v(8)          // record_size
        .u32v(0)          // string_table_size
        .u32v(0xAABBCCDD) // table_hash
        .u32v(0x11223344) // layout_hash
        .u32v(7)          // min_id
        .u32v(8)          // max_id
        .u32v(0)          // locale
        .u16v(0x01 | 0x04)
        .u16v(0)
        .u32v(2)
        .u32v(0)
        .u32v(0)
        .u32v(static_cast<u32>(fieldInfo.size()))
        .u32v(0)
        .u32v(0)
        .u32v(1); // section_count

    const size_t sectionHeaderAt = f.size();
    f.u64v(0)     // tact_key_hash
        .u32v(0)  // file_offset — patched
        .u32v(2)  // record_count
        .u32v(0)  // string_table_size
        .u32v(0)  // offset_records_end — patched
        .u32v(0)  // id_list_size
        .u32v(0)  // relationship_data_size
        .u32v(2)  // offset_map_id_count
        .u32v(0); // copy_table_count

    f.i16v(0).u16v(0);
    f.i16v(0).u16v(4);
    f.append(fieldInfo);

    const u32 sectionStart = static_cast<u32>(f.size());
    f.patch32(sectionHeaderAt + 8, sectionStart);

    const u32 rec0 = static_cast<u32>(f.size());
    f.u32v(42).cstr("Alpha");
    const u32 rec1 = static_cast<u32>(f.size());
    f.u32v(43).cstr("Beta");
    const u32 recordsEnd = static_cast<u32>(f.size());
    f.patch32(sectionHeaderAt + 8 + 12, recordsEnd);

    f.u32v(rec0).u16v(static_cast<u16>(rec1 - rec0));
    f.u32v(rec1).u16v(static_cast<u16>(recordsEnd - rec1));
    f.u32v(7).u32v(8); // offset_map_id_list
    return f.bytes;
}

// ============================================================================
// WDC5 — versioned header, two sections, one of them encrypted
// ============================================================================

std::vector<u8> buildWdc5() {
    const Blob strings = stringBlock();

    Blob fieldInfo;
    fieldInfo.u16v(0).u16v(32).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);

    Blob f;
    f.tag("WDC5").u32v(5);
    {
        Blob schemaString;
        schemaString.cstr("WowStatic_Patch_10_2_5");
        schemaString.pad(128 - schemaString.size());
        f.append(schemaString);
    }
    f.u32v(3)                                     // record_count across sections
        .u32v(1)                                  // field_count
        .u32v(4)                                  // record_size
        .u32v(static_cast<u32>(strings.size()))   // string_table_size
        .u32v(0xAABBCCDD)                         // table_hash
        .u32v(0x11223344)                         // layout_hash
        .u32v(10)                                 // min_id
        .u32v(30)                                 // max_id
        .u32v(0)                                  // locale
        .u16v(0x04)                               // flags — non-inline ids
        .u16v(0)                                  // id_index
        .u32v(1)                                  // total_field_count
        .u32v(0)                                  // bitpacked_data_offset
        .u32v(0)                                  // lookup_column_count
        .u32v(static_cast<u32>(fieldInfo.size())) // field_storage_info_size
        .u32v(0)                                  // common_data_size
        .u32v(0)                                  // pallet_data_size
        .u32v(2);                                 // section_count

    const size_t section0At = f.size();
    f.u64v(0)
        .u32v(0)
        .u32v(2)
        .u32v(static_cast<u32>(strings.size()))
        .u32v(0)
        .u32v(8)
        .u32v(0)
        .u32v(0)
        .u32v(0);
    const size_t section1At = f.size();
    f.u64v(0xDEADBEEFCAFEULL).u32v(0).u32v(1).u32v(0).u32v(0).u32v(4).u32v(0).u32v(0).u32v(0);

    f.i16v(0).u16v(0);
    f.append(fieldInfo);

    // encrypted_status for section 1 (it carries a tact key).
    f.u32v(1).u32v(30);

    const u32 section0Start = static_cast<u32>(f.size());
    f.patch32(section0At + 8, section0Start);
    // Record data across both sections is 3 × 4 = 12 bytes, so the string
    // block starts at virtual offset 12.
    f.u32v(12 + 1 - 0); // row 0 → "Alpha"
    f.u32v(12 + 7 - 4); // row 1 → "Beta"
    f.append(strings);
    f.u32v(10).u32v(20); // id_list

    const u32 section1Start = static_cast<u32>(f.size());
    f.patch32(section1At + 8, section1Start);
    f.u32v(0);  // encrypted record — zeroed
    f.u32v(30); // id_list
    return f.bytes;
}

} // namespace

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("WDBC parses records and string references", "[database][wdbc]") {
    Parser parser;
    auto table = parser.parse(buildWdbc());
    REQUIRE(table.has_value());
    CHECK(parser.getIssues().empty());

    CHECK(table->info().version == Version::WDBC);
    CHECK(table->info().recordCount == 2);
    CHECK(table->fields().size() == 3);
    REQUIRE(table->rowCount() == 2);

    const Row first = table->row(0);
    CHECK(first.id() == 1);
    CHECK(first.getUInt(1) == 42);
    CHECK(first.getString(2) == "Alpha");

    const Row second = table->row(1);
    CHECK(second.id() == 2);
    CHECK(second.getUInt(1) == 7);
    CHECK(second.getString(2) == "Beta");

    auto byId = table->findById(2);
    REQUIRE(byId.has_value());
    CHECK(byId->index() == 1);
    CHECK(!table->findById(999).has_value());
}

TEST_CASE("Schema binding gives typed access", "[database][schema]") {
    Parser parser;
    auto table = parser.parse(buildWdbc());
    REQUIRE(table.has_value());

    Schema schema("Fixture");
    schema.addId("ID").add("Value", ColumnType::Int32).add("Name", ColumnType::String);

    std::vector<std::string> issues;
    REQUIRE(table->bind(schema, &issues));
    CHECK(issues.empty());

    const Row row = table->row(0);
    CHECK(row.value("ID").asUInt() == 1);
    CHECK(row.value("Value").asInt() == 42);
    CHECK(row.value("Name").asString() == "Alpha");
    CHECK(row.value("Missing").isNull());
}

TEST_CASE("Schema mismatches are reported and rejected", "[database][schema]") {
    Parser parser;
    auto table = parser.parse(buildWdbc());
    REQUIRE(table.has_value());

    Schema schema("Fixture");
    schema.addId("ID").add("Value", ColumnType::Int32);

    std::vector<std::string> issues;
    CHECK(!table->bind(schema, &issues));
    CHECK(!issues.empty());
    CHECK(table->schema() == nullptr);
}

TEST_CASE("Localized string columns span one slot per locale", "[database][schema]") {
    // Three records' worth of a single 17-slot localized column, plus an id.
    Blob strings;
    strings.u8v(0).cstr("Hello").cstr("Hallo");

    Blob f;
    f.tag("WDBC").u32v(1).u32v(18).u32v(72).u32v(static_cast<u32>(strings.size()));
    f.u32v(5); // id
    f.u32v(1); // enUS → "Hello"
    f.u32v(0).u32v(0);
    f.u32v(7); // deDE → "Hallo"
    for (int i = 0; i < 12; ++i) {
        f.u32v(0);
    }
    f.u32v(0xFF); // flags slot
    f.append(strings);

    Parser parser;
    auto table = parser.parse(f.bytes);
    REQUIRE(table.has_value());

    Schema schema("Localized");
    schema.addId("ID").add("Name_lang", ColumnType::LocString);

    std::vector<std::string> issues;
    REQUIRE(table->bind(schema, &issues));

    const Row row = table->row(0);
    CHECK(row.locString("Name_lang", Locale::enUS) == "Hello");
    CHECK(row.locString("Name_lang", Locale::deDE) == "Hallo");
    CHECK(row.locString("Name_lang", Locale::frFR).empty());
    CHECK(row.value("Name_lang").asString() == "Hello");
}

TEST_CASE("WDB2 skips the id index block", "[database][wdb2]") {
    Parser parser;
    auto table = parser.parse(buildWdb2());
    REQUIRE(table.has_value());

    CHECK(table->info().version == Version::WDB2);
    CHECK(table->info().build == 30000);
    CHECK(table->info().timestamp == 12345);
    REQUIRE(table->rowCount() == 2);
    CHECK(table->row(0).id() == 10);
    CHECK(table->row(0).getString(2) == "Alpha");
    CHECK(table->row(1).id() == 11);
    CHECK(table->row(1).getString(2) == "Beta");
}

TEST_CASE("WDB5 derives array counts and expands the copy table", "[database][wdb5]") {
    Parser parser;
    auto table = parser.parse(buildWdb5());
    REQUIRE(table.has_value());

    CHECK(table->info().version == Version::WDB5);
    CHECK(table->info().layoutHash == 0x11223344);

    REQUIRE(table->fields().size() == 3);
    CHECK(table->fields()[0].arrayCount == 1);
    CHECK(table->fields()[0].elementSize == 4);
    CHECK(table->fields()[1].arrayCount == 2);
    CHECK(table->fields()[1].elementSize == 2);
    CHECK(table->fields()[2].arrayCount == 2);
    CHECK(table->fields()[2].elementSize == 4);

    REQUIRE(table->rowCount() == 3);
    CHECK(table->row(0).getUInt(1, 0) == 70);
    CHECK(table->row(0).getUInt(1, 1) == 71);
    CHECK(table->row(0).getUInt(2, 1) == 22);
    CHECK(table->row(1).getUInt(2, 0) == 33);

    // The copy row carries a new id but the original's values.
    const Row copy = table->row(2);
    CHECK(copy.id() == 100);
    CHECK(copy.isCopy());
    CHECK(copy.getUInt(2, 0) == 11);
}

TEST_CASE("WDB6 applies common data defaults", "[database][wdb6]") {
    Parser parser;
    auto table = parser.parse(buildWdb6(true));
    REQUIRE(table.has_value());

    CHECK(table->info().version == Version::WDB6);
    CHECK(table->info().totalFieldCount == 4);
    REQUIRE(table->fields().size() == 4);
    CHECK(table->fields()[3].storage == FieldStorage::CommonData);

    REQUIRE(table->rowCount() == 2);
    CHECK(table->row(0).getUInt(3) == 0);    // id 1 — absent, so the default
    CHECK(table->row(1).getUInt(3) == 1234); // id 2 — present
}

TEST_CASE("WDB6 detects unpadded common data", "[database][wdb6]") {
    Parser parser;
    auto table = parser.parse(buildWdb6(false));
    REQUIRE(table.has_value());
    REQUIRE(table->rowCount() == 2);
    CHECK(table->row(1).getUInt(3) == 1234);
}

TEST_CASE("WDC1 decodes every storage type", "[database][wdc1]") {
    Parser parser;
    auto table = parser.parse(buildWdc1());
    REQUIRE(table.has_value());

    CHECK(table->info().version == Version::WDC1);
    REQUIRE(table->fields().size() == 4);
    CHECK(table->fields()[0].storage == FieldStorage::None);
    CHECK(table->fields()[1].storage == FieldStorage::Bitpacked);
    CHECK(table->fields()[2].storage == FieldStorage::BitpackedIndexed);
    CHECK(table->fields()[3].storage == FieldStorage::CommonData);

    REQUIRE(table->rowCount() == 2);
    const Row first = table->row(0);
    const Row second = table->row(1);

    CHECK(first.id() == 10); // non-inline id list
    CHECK(second.id() == 20);

    CHECK(first.getString(0) == "Alpha");
    CHECK(first.getUInt(1) == 5);
    CHECK(second.getUInt(1) == 9);

    CHECK(first.getUInt(2) == 2000); // pallet index 1
    CHECK(second.getUInt(2) == 1000);

    CHECK(first.getUInt(3) == 77);   // common data default
    CHECK(second.getUInt(3) == 555); // common data override

    REQUIRE(first.relationId().has_value());
    CHECK(*first.relationId() == 100);
    CHECK(!second.relationId().has_value());
}

TEST_CASE("WDC2 resolves field-relative string references", "[database][wdc2]") {
    Parser parser;
    auto table = parser.parse(buildWdc2());
    REQUIRE(table.has_value());

    CHECK(table->info().version == Version::WDC2);
    CHECK(table->sections().size() == 1);
    REQUIRE(table->rowCount() == 2);
    CHECK(table->row(0).id() == 10);
    CHECK(table->row(0).getString(0) == "Alpha");
    CHECK(table->row(1).getString(0) == "Beta");
}

TEST_CASE("WDC3 reads sparse records with inline strings", "[database][wdc3]") {
    Parser parser;
    auto table = parser.parse(buildWdc3Sparse());
    REQUIRE(table.has_value());

    CHECK(table->info().version == Version::WDC3);
    CHECK(table->info().hasOffsetMap);
    REQUIRE(table->rowCount() == 2);

    const Row first = table->row(0);
    CHECK(first.isSparse());
    CHECK(first.id() == 7);
    CHECK(first.raw().size() == 10);
    CHECK(first.getUInt(0) == 42);

    Schema schema("Sparse");
    schema.add("Value", ColumnType::Int32).add("Name", ColumnType::String);
    std::vector<std::string> issues;
    REQUIRE(table->bind(schema, &issues));

    CHECK(table->row(0).value("Name").asString() == "Alpha");
    CHECK(table->row(1).value("Value").asInt() == 43);
    CHECK(table->row(1).value("Name").asString() == "Beta");
}

TEST_CASE("WDC5 reads the versioned header and flags encrypted sections", "[database][wdc5]") {
    Parser parser;
    auto table = parser.parse(buildWdc5());
    REQUIRE(table.has_value());

    CHECK(table->info().version == Version::WDC5);
    CHECK(table->info().versionNumber == 5);
    CHECK(table->info().schemaString == "WowStatic_Patch_10_2_5");

    REQUIRE(table->sections().size() == 2);
    CHECK(!table->sections()[0].encrypted);
    CHECK(table->sections()[1].tactKeyHash == 0xDEADBEEFCAFEULL);
    CHECK(table->sections()[1].encrypted);
    REQUIRE(table->sections()[1].encryptedIds.size() == 1);
    CHECK(table->sections()[1].encryptedIds[0] == 30);

    REQUIRE(table->rowCount() == 3);
    CHECK(table->row(0).getString(0) == "Alpha");
    CHECK(table->row(1).getString(0) == "Beta");
    CHECK(!table->row(0).isEncrypted());

    const Row encrypted = table->row(2);
    CHECK(encrypted.id() == 30);
    CHECK(encrypted.isEncrypted());
}

TEST_CASE("Malformed input is rejected without throwing", "[database]") {
    Parser parser;

    CHECK(!parser.parse(std::vector<u8>{}).has_value());
    CHECK(parser.hasIssues());

    CHECK(!parser.parse(std::vector<u8>{'N', 'O', 'P', 'E', 0, 0, 0, 0}).has_value());

    SECTION("truncated file") {
        auto full = buildWdbc();
        full.resize(full.size() / 2);
        CHECK(!parser.parse(std::move(full)).has_value());
        CHECK(parser.hasIssues());
    }

    SECTION("record count beyond the file") {
        auto full = buildWdbc();
        Blob patch;
        patch.bytes = full;
        patch.patch32(4, 100000);
        CHECK(!parser.parse(std::move(patch.bytes)).has_value());
    }
}

TEST_CASE("detectVersion identifies each magic", "[database]") {
    CHECK(detectVersion(buildWdbc()) == Version::WDBC);
    CHECK(detectVersion(buildWdb2()) == Version::WDB2);
    CHECK(detectVersion(buildWdb5()) == Version::WDB5);
    CHECK(detectVersion(buildWdc1()) == Version::WDC1);
    CHECK(detectVersion(buildWdc5()) == Version::WDC5);
    CHECK(detectVersion(std::vector<u8>{'X'}) == Version::Unknown);
}
