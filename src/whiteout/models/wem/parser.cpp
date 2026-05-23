// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/parser.h>

#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "../../common/unicode_path.h"

#include "binary_parse_visitor.h"

#include <fstream>

namespace whiteout {
namespace models {
namespace wem {

using common::BinaryReader;

// ============================================================================
// ParserImpl
// ============================================================================

class Parser::Impl {
public:
    std::vector<std::string> issues;

    std::optional<Model> parse(BinaryReader& reader) {
        issues.clear();
        Model model;
        BinaryParseVisitor visitor(reader);
        visitor.read(model);

        for (const auto& issue : visitor.getIssues()) {
            reportIssue(issue);
        }

        // If the visitor reported fatal issues, return nullopt.
        for (const auto& issue : issues) {
            if (issue.find("Invalid WEM") != std::string::npos ||
                issue.find("no model data") != std::string::npos ||
                issue.find("index out of bounds") != std::string::npos) {
                return std::nullopt;
            }
        }

        return model;
    }

private:
    void reportIssue(const std::string& message) {
        issues.push_back(message);
    }
};

// ============================================================================
// Parser Public Interface
// ============================================================================

Parser::Parser() : pImpl(std::make_unique<Impl>()) {}

Parser::~Parser() = default;

std::optional<Model> Parser::parse(const std::string& filePath) {
    auto file = common::open_ifstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        pImpl->issues.push_back("Failed to open file: " + filePath);
        return std::nullopt;
    }
    BinaryReader reader(file);
    return pImpl->parse(reader);
}

std::optional<Model> Parser::parse(std::span<const u8> buffer) {
    common::span_streambuf streambuf(buffer);
    std::istream in(&streambuf);
    BinaryReader reader(in);
    return pImpl->parse(reader);
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

} // namespace wem
} // namespace models
} // namespace whiteout
