// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/m3/parser.h>
#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "binary_parse_visitor.h"

#include <fstream>
#include <stdexcept>

namespace whiteout {
namespace m3 {

using common::BinaryReader;

// ============================================================================
// ParserImpl - Implementation class using PImpl idiom
// ============================================================================

class Parser::Impl {
public:
    ParseMode parseMode;
    std::vector<std::string> issues;

    Model parseFromReader(BinaryReader& reader);
    void reportIssue(const std::string& message);
};

Model Parser::Impl::parseFromReader(BinaryReader& reader) {
    issues.clear();
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

void Parser::Impl::reportIssue(const std::string& message) {
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(message);
    }
    issues.push_back(message);
}

// ============================================================================
// Parser Public Interface (using PImpl)
// ============================================================================

Parser::Parser(ParseMode mode) : pImpl(std::make_unique<Impl>()) {
    pImpl->parseMode = mode;
}

Parser::~Parser() = default;

Model Parser::parse(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open M3 file: " + filePath);
    }
    BinaryReader reader(file);
    return pImpl->parseFromReader(reader);
}

Model Parser::parse(std::span<const u8> buffer) {
    common::span_streambuf streambuf(buffer);
    std::istream in(&streambuf);
    BinaryReader reader(in);
    return pImpl->parseFromReader(reader);
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

} // namespace m3
} // namespace whiteout
