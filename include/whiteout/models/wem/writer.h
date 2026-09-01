// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief Writing a `Document` (WEM v3, design §12).
 *
 * §11.4's other half needs no parameter: the chunks this build did not
 * understand ride on `Document::unknownChunks`, which the parser fills, so a
 * read-edit-write round trip preserves them by doing nothing. Clearing that
 * vector is how a caller drops them — an explicit act, not an omission.
 */

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "diagnostics.h"
#include "document.h"

namespace whiteout {
namespace models {
namespace wem {

/// @bind methods, js_name=WemWriter
class Writer {
public:
    Writer();
    ~Writer();
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;

    bool write(const std::string& filePath, const Document& document);
    std::vector<u8> write(const Document& document);

    const Diagnostics& diagnostics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wem
} // namespace models
} // namespace whiteout
