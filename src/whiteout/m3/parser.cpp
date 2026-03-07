// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/m3/parser.h>
#include "../common/binary_reader.h"
#include "../common/streams.h"
#include "binary_parse_visitor.h"

#include <fstream>
#include <stdexcept>

namespace whiteout {
namespace m3 {

using common::BinaryReader;

Parser::Parser(ParseMode mode) : parseMode(mode) {}

Model Parser::parse(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open M3 file: " + filePath);
    }
    BinaryReader reader(file);
    return parseFromReader(reader);
}

Model Parser::parse(std::span<const u8> buffer) {
    common::span_streambuf streambuf(buffer);
    std::istream in(&streambuf);
    BinaryReader reader(in);
    return parseFromReader(reader);
}

Model Parser::parseFromReader(BinaryReader& reader) {
    Model model;
    try {
        BinaryParseVisitor visitor(reader);
        visitor.read(model);

        // Collect any issues from the visitor
        for (const auto& issue : visitor.getIssues()) {
            reportIssue(issue);
        }
    } catch (const std::exception& e) {
        if (parseMode == ParseMode::Strict) {
            throw;
        }
        reportIssue(std::string("Parse error: ") + e.what());
    }
    return model;
}

void Parser::reportIssue(const std::string& message) {
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(message);
    }
    issues.push_back(message);
}

} // namespace m3
} // namespace whiteout
