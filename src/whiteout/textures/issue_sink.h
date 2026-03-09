// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file issue_sink.h
/// @brief Shared strict-or-lenient error reporting for parser/writer Impl classes.
///
/// Internal header — not part of the public include path.

#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace whiteout::textures {

/// Base class providing strict-or-lenient error reporting.
///
/// When `strict_mode` is true, `fail()` throws a `std::runtime_error`.
/// Otherwise it appends the message to `issues` and returns false.
struct IssueSink {
    std::vector<std::string> issues;
    bool strict_mode = false;

    bool fail(const std::string& msg) {
        if (strict_mode)
            throw std::runtime_error(msg);
        issues.push_back(msg);
        return false;
    }
};

} // namespace whiteout::textures
