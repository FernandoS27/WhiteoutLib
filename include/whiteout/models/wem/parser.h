// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief Reading a `Document` (WEM v3, design §12).
 *
 * `Diagnostics` rather than a bool, because a document that read with a
 * preserved chunk this build did not understand is neither a success nor a
 * failure, and the caller has to be able to tell the difference.
 *
 * There is no compatibility path. WEM v2 was never shipped to anyone, and the
 * only v2 files that exist were produced by this repository's own example
 * programs; carrying a second on-disk generation for them would have cost more
 * than regenerating them.
 */

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "diagnostics.h"
#include "document.h"

namespace whiteout {
namespace models {
namespace wem {

/// Which formats this reader is willing to parse the native block of.
///
/// §11.3's wasm-build-without-D3 property, as a runtime option rather than a
/// build one: a skipped block's chunk is preserved whole and its reference is
/// re-emitted, so a document read this way can be edited and written back with
/// the skipped sets intact. Empty means parse everything.
struct ReadOptions {
    std::vector<NativeKind> skipNativeKinds;
};

/// @bind methods, js_name=WemParser
class Parser {
public:
    Parser();
    ~Parser();
    Parser(Parser&&) noexcept;
    Parser& operator=(Parser&&) noexcept;

    std::optional<Document> parse(const std::string& filePath, const ReadOptions& options = {});
    std::optional<Document> parse(std::span<const u8> bytes, const ReadOptions& options = {});

    /// Chunks the read preserved rather than parsed — unknown tags, and the
    /// native blocks `ReadOptions` asked to skip. Hand these back to `Writer` or
    /// they are dropped on the next write.
    const std::vector<UnknownChunk>& unknownChunks() const;

    const Diagnostics& diagnostics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// True when @p bytes is a WEM file this build can read. Cheap: it looks at the
/// magic and the version and nothing else.
bool IsWemFile(std::span<const u8> bytes, u32* versionOut = nullptr);

} // namespace wem
} // namespace models
} // namespace whiteout
