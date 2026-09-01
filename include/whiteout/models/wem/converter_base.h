// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file converter_base.h
 * @brief `FormatConverter` and the registry (WEM v3, design §14).
 *
 * A converter is named for the **format** it reads; the profiles it lists are
 * named for the games. `profiles()` is a span rather than a scalar because two
 * of the four converters serve two profiles each — that granularity is the whole
 * point of the profile axis, and a converter that returned one profile could not
 * express "this `.mdx` carries both a classic and a Reforged material set over
 * one geometry".
 *
 * Import produces a whole `Document`; export takes one **profile's** material
 * set and refuses a profile the document does not carry. There is no
 * "export whatever is in there" mode: a document with two sets has two possible
 * exports, and picking one silently is how the wrong one ships.
 */

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "diagnostics.h"
#include "document.h"
#include "profile.h"

namespace whiteout {
namespace models {
namespace wem {

/**
 * @brief A value that may not have been produced, plus why (§13).
 *
 * Diagnostics ride along whether or not the value exists — a lossy conversion
 * that succeeded and a conversion that failed are different states, and both
 * have something to say. The library is exception-free, so this is the only
 * error channel.
 */
template <class T>
struct Result {
    std::optional<T> value;
    Diagnostics diagnostics;

    bool ok() const {
        return value.has_value();
    }
    explicit operator bool() const {
        return ok();
    }

    const T& operator*() const {
        return *value;
    }
    T& operator*() {
        return *value;
    }
    const T* operator->() const {
        return &*value;
    }
    T* operator->() {
        return &*value;
    }

    /// Moves the value out. Only valid when `ok()`.
    T take() {
        return std::move(*value);
    }
};

// ============================================================================
// FormatConverter
// ============================================================================

class FormatConverter {
public:
    virtual ~FormatConverter() = default;

    /// Short lowercase format identifier — "mdx", "m2", "m3", "d3".
    virtual std::string formatId() const = 0;

    /// Human-readable format name.
    virtual std::string formatName() const = 0;

    /// The games this format serves, in the order a caller should prefer them.
    virtual std::span<const ProfileId> profiles() const = 0;

    virtual bool supportsImport() const = 0;
    virtual bool supportsExport() const = 0;

    /// Target format version used when `exportToBytes` is passed 0.
    virtual u32 defaultExportVersion() const = 0;

    /// Format bytes -> `Document`. The default refuses with
    /// `OperationUnsupported`, which is the honest answer for a format whose
    /// import needs more than one file.
    virtual Result<Document> importFromBytes(std::span<const u8> data) const;

    /// `Document` -> format bytes, for exactly one profile's material set.
    ///
    /// A profile the document does not carry is refused with `ProfileNotCarried`
    /// rather than substituted: the sets are independent (§6.3), so there is no
    /// nearest neighbour to fall back to.
    virtual Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                                  u32 version = 0) const;

protected:
    /// Shared precondition for every `exportToBytes`. Returns false and files
    /// the diagnostic when @p profile is not one this converter serves, or not
    /// one @p document carries.
    bool checkExportProfile(const Document& document, ProfileId profile, Diagnostics& out) const;

    /// Animation is **import-only in v3**: §16's P7 is four importers and no
    /// exporter. Every `toX` calls this so a document's clips are not dropped in
    /// silence — one row with the count, because the expected-loss golden diffs
    /// the code *set* and a row per clip would bury everything else in it.
    static void reportUnwrittenClips(const Document& document, Diagnostics& out);
};

// ============================================================================
// ConverterRegistry
// ============================================================================

/**
 * @brief Singleton registry keyed by `formatId()`.
 *
 * Built-in converters register themselves on first access. `findForProfile`
 * exists because callers usually know the *game* — "load this Heroes model" —
 * and the format is an implementation detail of that answer.
 */
class ConverterRegistry {
public:
    static ConverterRegistry& instance();

    /// Registers @p converter, replacing any with the same `formatId()`.
    void registerConverter(std::shared_ptr<FormatConverter> converter);

    const FormatConverter* find(const std::string& formatId) const;

    /// The first registered converter listing @p profile, or null.
    const FormatConverter* findForProfile(ProfileId profile) const;

    std::vector<const FormatConverter*> all() const;

private:
    ConverterRegistry();
    ~ConverterRegistry();

    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace wem
} // namespace models
} // namespace whiteout
