// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <span>
#include <string>
#include <vector>
#include "structures.h"

namespace whiteout {
namespace common {
class BinaryReader;
}

namespace m3 {

enum class ParseMode {
    Strict,  ///< Throw on any unexpected data
    Lenient, ///< Log issues and continue
};

class Parser {
public:
    explicit Parser(ParseMode mode = ParseMode::Lenient);

    /// Parse an M3 file from disk.
    Model parse(const std::string& filePath);

    /// Parse an M3 file from a memory buffer.
    Model parse(std::span<const u8> buffer);

    const std::vector<std::string>& getIssues() const {
        return issues;
    }
    void clearIssues() {
        issues.clear();
    }

private:
    ParseMode parseMode;
    std::vector<std::string> issues;

    Model parseFromReader(common::BinaryReader& reader);
    void reportIssue(const std::string& message);
};

} // namespace m3
} // namespace whiteout
