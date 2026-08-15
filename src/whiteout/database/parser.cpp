// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/database/parser.h>

#include "db_internal.h"

#include <fstream>
#include <utility>

#include "../common/unicode_path.h"

namespace whiteout::database {

namespace {

constexpr u32 fourcc(const char (&tag)[5]) {
    return static_cast<u32>(static_cast<u8>(tag[0])) |
           (static_cast<u32>(static_cast<u8>(tag[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(tag[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(tag[3])) << 24);
}

struct MagicEntry {
    u32 magic;
    Version version;
};

constexpr MagicEntry MAGICS[] = {
    {fourcc("WDBC"), Version::WDBC},
    {fourcc("WDB2"), Version::WDB2},
    {fourcc("WDB3"), Version::WDB3},
    {fourcc("WDB4"), Version::WDB4},
    {fourcc("WDB5"), Version::WDB5},
    {fourcc("WDB6"), Version::WDB6},
    {fourcc("WDC1"), Version::WDC1},
    {fourcc("WDC2"), Version::WDC2},
    // Classic ships WDC2 under a different tag.
    {fourcc("1SLC"), Version::WDC2},
    {fourcc("WDC3"), Version::WDC3},
    {fourcc("WDC4"), Version::WDC4},
    {fourcc("WDC5"), Version::WDC5},
};

} // namespace

const char* versionName(Version version) {
    switch (version) {
    case Version::WDBC:
        return "WDBC";
    case Version::WDB2:
        return "WDB2";
    case Version::WDB3:
        return "WDB3";
    case Version::WDB4:
        return "WDB4";
    case Version::WDB5:
        return "WDB5";
    case Version::WDB6:
        return "WDB6";
    case Version::WDC1:
        return "WDC1";
    case Version::WDC2:
        return "WDC2";
    case Version::WDC3:
        return "WDC3";
    case Version::WDC4:
        return "WDC4";
    case Version::WDC5:
        return "WDC5";
    case Version::Unknown:
        break;
    }
    return "unknown";
}

const char* localeName(u32 locale) {
    switch (static_cast<Locale>(locale)) {
    case Locale::enUS:
        return "enUS";
    case Locale::koKR:
        return "koKR";
    case Locale::frFR:
        return "frFR";
    case Locale::deDE:
        return "deDE";
    case Locale::zhCN:
        return "zhCN";
    case Locale::zhTW:
        return "zhTW";
    case Locale::esES:
        return "esES";
    case Locale::esMX:
        return "esMX";
    case Locale::ruRU:
        return "ruRU";
    case Locale::jaJP:
        return "jaJP";
    case Locale::ptPT:
        return "ptPT";
    case Locale::itIT:
        return "itIT";
    }
    return "unknown";
}

Version detectVersion(std::span<const u8> buffer) {
    if (buffer.size() < 4) {
        return Version::Unknown;
    }
    u32 magic = 0;
    std::memcpy(&magic, buffer.data(), 4);
    for (const auto& entry : MAGICS) {
        if (entry.magic == magic) {
            return entry.version;
        }
    }
    return Version::Unknown;
}

Parser::Parser() = default;
Parser::~Parser() = default;

std::optional<Table> Parser::parse(std::span<const u8> buffer) {
    return parse(std::vector<u8>(buffer.begin(), buffer.end()));
}

std::optional<Table> Parser::parse(std::vector<u8>&& buffer) {
    issues_.clear();

    IssueSink sink{&issues_};

    auto data = std::make_unique<TableData>();
    data->buffer = std::move(buffer);

    const Version version = detectVersion(data->buffer);
    if (version == Version::Unknown) {
        sink.fail(data->buffer.size() < 4 ? "file is too small to hold a magic"
                                          : "unrecognised magic — not a DBC or DB2 file");
        return std::nullopt;
    }

    data->info.version = version;
    std::memcpy(&data->info.magic, data->buffer.data(), 4);

    ByteCursor cursor(data->buffer.data(), data->buffer.size());
    cursor.skip(4);

    bool ok = false;
    switch (version) {
    case Version::WDBC:
        ok = readWdbc(*data, cursor, sink);
        break;
    case Version::WDB2:
    case Version::WDB3:
    case Version::WDB4:
    case Version::WDB5:
    case Version::WDB6:
        ok = readWdb(*data, cursor, sink);
        break;
    case Version::WDC1:
    case Version::WDC2:
    case Version::WDC3:
    case Version::WDC4:
    case Version::WDC5:
        ok = readWdc(*data, cursor, sink);
        break;
    case Version::Unknown:
        break;
    }

    if (!ok) {
        return std::nullopt;
    }

    data->finalize();

    Table table;
    table.data_ = std::move(data);
    return table;
}

std::optional<Table> Parser::parse(const std::string& filePath) {
    issues_.clear();

    std::ifstream file = common::open_ifstream(filePath, std::ios::binary | std::ios::ate);
    if (!file) {
        issues_.push_back("cannot open '" + filePath + "'");
        return std::nullopt;
    }

    const std::streamoff size = file.tellg();
    if (size <= 0) {
        issues_.push_back("'" + filePath + "' is empty");
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);

    std::vector<u8> buffer(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    if (!file) {
        issues_.push_back("failed to read '" + filePath + "'");
        return std::nullopt;
    }

    return parse(std::move(buffer));
}

bool Parser::hasIssues() const {
    return !issues_.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return issues_;
}

} // namespace whiteout::database
