// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Dump the structure and the first rows of a DBC or DB2 file.
///
///   db2_dump_example <file.db2> [row count]

#include <whiteout/database/parser.h>
#include <whiteout/database/table.h>

#include <iomanip>
#include <iostream>
#include <string>

using namespace whiteout;
using namespace whiteout::database;

namespace {

const char* storageName(FieldStorage storage) {
    switch (storage) {
    case FieldStorage::None:
        return "inline";
    case FieldStorage::Bitpacked:
        return "bitpacked";
    case FieldStorage::CommonData:
        return "common";
    case FieldStorage::BitpackedIndexed:
        return "pallet";
    case FieldStorage::BitpackedIndexedArray:
        return "pallet[]";
    case FieldStorage::BitpackedSigned:
        return "bitpacked/s";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: db2_dump_example <file.db2> [row count]\n";
        return 1;
    }
    const size_t rowsToPrint = argc > 2 ? static_cast<size_t>(std::stoul(argv[2])) : 10;

    Parser parser;
    auto table = parser.parse(std::string(argv[1]));
    for (const auto& issue : parser.getIssues()) {
        std::cerr << "note: " << issue << '\n';
    }
    if (!table) {
        std::cerr << "failed to parse " << argv[1] << '\n';
        return 1;
    }

    const TableInfo& info = table->info();
    std::cout << versionName(info.version) << "  table hash 0x" << std::hex << info.tableHash
              << "  layout hash 0x" << info.layoutHash << std::dec << '\n';
    if (!info.schemaString.empty()) {
        std::cout << "build: " << info.schemaString << '\n';
    }
    std::cout << "rows: " << table->rowCount() << "  fields: " << table->fields().size()
              << "  sections: " << table->sections().size()
              << "  locale: " << localeName(info.locale) << (info.hasOffsetMap ? "  [sparse]" : "")
              << '\n';

    for (size_t i = 0; i < table->sections().size(); ++i) {
        const SectionInfo& section = table->sections()[i];
        if (section.tactKeyHash != 0) {
            std::cout << "  section " << i << ": " << section.recordCount << " records, TACT key 0x"
                      << std::hex << section.tactKeyHash << std::dec
                      << (section.encrypted ? " (locked)" : " (decrypted)") << '\n';
        }
    }

    for (size_t f = 0; f < table->fields().size(); ++f) {
        const FieldInfo& field = table->fields()[f];
        std::cout << "  field " << std::setw(3) << f << "  " << std::setw(11)
                  << storageName(field.storage) << "  bits " << std::setw(5) << field.offsetBits
                  << " +" << std::setw(3) << field.sizeBits;
        if (field.arrayCount > 1) {
            std::cout << "  [" << field.arrayCount << "]";
        }
        std::cout << '\n';
    }

    for (size_t r = 0; r < table->rowCount() && r < rowsToPrint; ++r) {
        const Row row = table->row(r);
        std::cout << std::setw(8) << row.id() << (row.isCopy() ? " c" : "  ");
        if (row.isEncrypted()) {
            std::cout << " <encrypted>";
        } else if (row.isSparse()) {
            std::cout << " <" << row.raw().size() << " bytes>";
        } else {
            for (u32 f = 0; f < table->fields().size(); ++f) {
                for (u32 a = 0; a < table->fields()[f].arrayCount; ++a) {
                    std::cout << ' ' << row.getUInt(f, a);
                }
            }
        }
        std::cout << '\n';
    }
    return 0;
}
