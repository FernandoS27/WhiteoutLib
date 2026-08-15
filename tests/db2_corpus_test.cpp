// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Corpus test: parse every DB2 shipped in Corpus/WoW/dbfilesclient and decode
/// every field of every row. Between them these tables cover all six storage
/// types, multi-section files, encrypted sections, sparse records and
/// relationship maps, so a clean pass exercises the whole reader.
///
/// Skipped when the corpus is not present.

#include <catch2/catch_all.hpp>

#include <whiteout/database/parser.h>
#include <whiteout/database/table.h>

#include "test_helpers.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace whiteout;
using namespace whiteout::database;

namespace {

std::string findDbCorpus() {
    const std::string base = test::findCorpusBase();
    if (base.empty()) {
        return {};
    }
    const std::filesystem::path dir = std::filesystem::path(base) / "WoW" / "dbfilesclient";
    return std::filesystem::exists(dir) ? dir.string() : std::string{};
}

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
}

struct Totals {
    size_t files = 0;
    size_t rows = 0;
    size_t values = 0;
    size_t strings = 0;
    size_t encryptedRows = 0;
    size_t sparseRows = 0;
    size_t copyRows = 0;
};

/// Decode every field of every row. Returns the number of values read.
size_t decodeAll(const Table& table, Totals& totals) {
    size_t values = 0;
    const auto& fields = table.fields();
    const size_t rowCount = table.rowCount();

    for (size_t r = 0; r < rowCount; ++r) {
        const Row row = table.row(r);
        REQUIRE(row.valid());
        totals.rows++;
        if (row.isEncrypted()) {
            totals.encryptedRows++;
        }
        if (row.isSparse()) {
            totals.sparseRows++;
        }
        if (row.isCopy()) {
            totals.copyRows++;
        }
        // Sparse records are variable-length and have no fixed field layout
        // without a schema, so only their raw bytes are checked.
        if (row.isSparse()) {
            CHECK(!row.raw().empty());
            continue;
        }
        for (u32 f = 0; f < fields.size(); ++f) {
            for (u32 a = 0; a < fields[f].arrayCount; ++a) {
                values += 1;
                const u64 raw = row.getUInt(f, a);
                (void)raw;
                if (fields[f].storage == FieldStorage::None && fields[f].elementSize == 4) {
                    // Any 4-byte field could be a string reference; resolving
                    // it must stay in bounds whether or not it actually is one.
                    const std::string_view text = row.getString(f, a);
                    totals.strings += text.empty() ? 0 : 1;
                }
            }
        }
    }
    return values;
}

} // namespace

TEST_CASE("DB2 corpus parses and fully decodes", "[database][corpus]") {
    const std::string corpus = findDbCorpus();
    if (corpus.empty()) {
        SKIP("Corpus/WoW/dbfilesclient not found");
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(corpus)) {
        if (entry.is_regular_file() && entry.path().extension() == ".db2") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    REQUIRE(!files.empty());

    Totals totals;
    std::vector<std::string> failures;
    const auto start = std::chrono::steady_clock::now();

    for (const auto& path : files) {
        std::vector<u8> bytes = readFile(path);
        if (bytes.empty()) {
            failures.push_back(path.filename().string() + ": unreadable");
            continue;
        }

        Parser parser;
        auto table = parser.parse(std::move(bytes));
        if (!table) {
            std::string message = path.filename().string() + ": parse failed";
            for (const auto& issue : parser.getIssues()) {
                message += "\n    " + issue;
            }
            failures.push_back(message);
            continue;
        }
        totals.files++;

        // Rows account for every stored record plus every copy-table entry.
        // An encrypted section of a sparse table is the one exception: its
        // offset map is encrypted along with the records, so there is nothing
        // to locate its rows by.
        size_t expected = 0;
        for (const SectionInfo& section : table->sections()) {
            if (table->info().hasOffsetMap) {
                expected += section.encrypted ? 0 : section.offsetMapCount;
            } else {
                expected += section.recordCount;
            }
            expected += section.copyCount;
        }
        if (expected != table->rowCount()) {
            failures.push_back(path.filename().string() + ": " + std::to_string(table->rowCount()) +
                               " rows, expected " + std::to_string(expected));
        }

        // Ids are the table's primary key, so every readable row must have a
        // distinct one. This is what catches an id read from the wrong field.
        // Encrypted sections are excluded: their id lists are encrypted too,
        // so they all decode to zero.
        const auto& ids = table->ids();
        CHECK(ids.size() == table->rowCount());
        std::unordered_set<u32> unique;
        size_t readable = 0;
        bool anyId = false;
        for (size_t r = 0; r < table->rowCount(); ++r) {
            const Row row = table->row(r);
            if (row.isEncrypted()) {
                continue;
            }
            ++readable;
            anyId = anyId || row.id() != 0;
            unique.insert(row.id());
        }
        // A handful of tables (BroadcastTextDuration, CharStartKit) ship an
        // all-zero id list because their real key is the relationship map.
        if (anyId && unique.size() != readable) {
            failures.push_back(path.filename().string() + ": duplicate ids (" +
                               std::to_string(readable - unique.size()) + ")");
        }

        totals.values += decodeAll(*table, totals);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    std::cout << "DB2 corpus: " << totals.files << '/' << files.size() << " files, " << totals.rows
              << " rows (" << totals.copyRows << " copies, " << totals.sparseRows << " sparse, "
              << totals.encryptedRows << " encrypted), " << totals.values << " values, "
              << totals.strings << " strings, " << elapsed.count() << " ms\n";

    for (const auto& failure : failures) {
        UNSCOPED_INFO(failure);
    }
    CHECK(failures.empty());
    CHECK(totals.files == files.size());
}

TEST_CASE("DB2 corpus decodes known values", "[database][corpus]") {
    const std::string corpus = findDbCorpus();
    if (corpus.empty()) {
        SKIP("Corpus/WoW/dbfilesclient not found");
    }

    const std::filesystem::path spellName = std::filesystem::path(corpus) / "spellname.db2";
    if (!std::filesystem::exists(spellName)) {
        SKIP("spellname.db2 not in the corpus");
    }

    Parser parser;
    auto table = parser.parse(readFile(spellName));
    REQUIRE(table.has_value());

    // SpellName is a single string column addressed by a non-inline id list,
    // spread over many sections — the sharpest test of WDC2+ string offsets.
    CHECK(table->info().version == Version::WDC5);
    CHECK(table->sections().size() > 1);

    auto fireball = table->findById(133);
    REQUIRE(fireball.has_value());
    CHECK(fireball->getString(0) == "Fireball");

    auto frostbolt = table->findById(116);
    REQUIRE(frostbolt.has_value());
    CHECK(frostbolt->getString(0) == "Frostbolt");

    Schema schema("SpellName");
    schema.addId("ID").add("Name_lang", ColumnType::String);
    std::vector<std::string> issues;
    REQUIRE(table->bind(schema, &issues));
    CHECK(fireball->value("Name_lang").asString() == "Fireball");
}
