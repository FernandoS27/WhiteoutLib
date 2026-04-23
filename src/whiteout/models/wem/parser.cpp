// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/parser.h>

#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "../../common/unicode_path.h"

#include "binary_parse_visitor.h"

#include <fstream>
#include <stdexcept>

namespace whiteout {
namespace models {
namespace wem {

using common::BinaryReader;

// ============================================================================
// ParserImpl
// ============================================================================

class Parser::Impl {
public:
    Parser::ParseMode parseMode = ParseMode::Lenient;
    std::vector<std::string> issues;

    std::optional<Model> parse(BinaryReader& reader) {
        issues.clear();
        Model model;
        try {
            BinaryParseVisitor visitor(reader);
            visitor.read(model);

            for (const auto& issue : visitor.getIssues()) {
                reportIssue(issue);
            }
        } catch (const std::exception& e) {
            if (parseMode == ParseMode::Strict) {
                throw;
            }
            reportIssue(std::string("Parse error: ") + e.what());
            return std::nullopt;
        }

        // If the visitor reported fatal issues, return nullopt
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
        if (parseMode == ParseMode::Strict) {
            throw std::runtime_error(message);
        }
        issues.push_back(message);
    }
};

// ============================================================================
// Parser Public Interface
// ============================================================================

Parser::Parser(ParseMode parseMode) : pImpl(std::make_unique<Impl>()) {
    pImpl->parseMode = parseMode;
}

Parser::~Parser() = default;

std::optional<Model> Parser::parse(const std::string& filePath) {
    auto file = common::open_ifstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filePath);
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
