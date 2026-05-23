// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file issue_sink.h
/// @brief Shared lenient error reporting for parser/writer Impl classes.
///
/// Internal header — not part of the public include path.

#pragma once

#include <string>
#include <vector>

namespace whiteout::textures {

/// Base class that collects parser/writer issues without ever throwing.
///
/// `fail()` appends the message to `issues` and returns false so that
/// call-site `if (!fail(...)) return ...;` idioms still compose.
struct IssueSink {
    std::vector<std::string> issues;

    bool fail(const std::string& msg) {
        issues.push_back(msg);
        return false;
    }
};

} // namespace whiteout::textures
