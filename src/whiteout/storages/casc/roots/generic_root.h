// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file generic_root.h
/// @brief Fallback (dummy) root handler for products without a known root format.
///
/// Some Blizzard products (Hearthstone, Diablo Immortal, Battle.net Agent,
/// Warcraft I/II remasters, etc.) ship a root file that is not a parseable
/// manifest — e.g. it's an executable binary.  CascLib calls this a "dummy"
/// root.  This handler accepts any root data but provides an empty entry set,
/// allowing the storage to open.  Files can still be accessed by CKey/EKey
/// through the encoding table.
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"

namespace whiteout::storages::casc {

class GenericRoot final : public RootManifest {
public:
    /// Create a generic (empty) root manifest.
    static std::unique_ptr<GenericRoot> create() {
        return std::make_unique<GenericRoot>();
    }

    std::vector<const RootEntry*> findByPath(const std::string&) const override { return {}; }
    std::vector<const RootEntry*> findByFileDataId(u32, FileIdHint = FileIdHint::None) const override { return {}; }

    RootFormat format() const override { return RootFormat::Unknown; }

protected:
    const std::vector<RootEntry>& entries() const override { return m_entries; }
    std::vector<RootEntry>& mutableEntries() override { return m_entries; }

private:
    std::vector<RootEntry> m_entries;
};

} // namespace whiteout::storages::casc
