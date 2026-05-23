// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/m3/parser.h>
#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "../../common/unicode_path.h"
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
    std::vector<std::string> issues;

    Model parseFromReader(BinaryReader& reader);
    void reportIssue(const std::string& message);
};

Model Parser::Impl::parseFromReader(BinaryReader& reader) {
    issues.clear();
    Model model;
    BinaryParseVisitor visitor(reader);
    visitor.read(model);

    // Collect any issues from the visitor.
    for (const auto& issue : visitor.getIssues()) {
        reportIssue(issue);
    }
    return model;
}

void Parser::Impl::reportIssue(const std::string& message) {
    issues.push_back(message);
}

// ============================================================================
// Parser Public Interface (using PImpl)
// ============================================================================

Parser::Parser() : pImpl(std::make_unique<Impl>()) {}

Parser::~Parser() = default;

Model Parser::parse(const std::string& filePath) {
    auto file = common::open_ifstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        pImpl->issues.push_back("Failed to open M3 file: " + filePath);
        return {};
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
