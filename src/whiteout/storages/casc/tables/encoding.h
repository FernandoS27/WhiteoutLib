// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file encoding.h
/// @brief CASC ENCODING manifest parser — maps CKey → EKey.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "flat_hash_map.h"

namespace whiteout::storages::casc {

class CdnFetcher; // forward decl — defined in cdn/cdn_fetcher.h
class KeyRing;    // forward decl — defined in codec/crypto.h

struct EncodingEntry {
    std::array<u8, 16> cKey{};
    std::array<u8, 16> eKey{};  ///< Primary EKey (first encoding key).
    u64 fileSize = 0;           ///< Uncompressed file size.
    std::string eSpec;          ///< BLTE encoding specification (e.g. "z", "n", "b:{65536*=z}").
};

class EncodingTable {
public:
    EncodingTable();
    ~EncodingTable();
    EncodingTable(EncodingTable&&) noexcept;
    EncodingTable& operator=(EncodingTable&&) noexcept;
    EncodingTable(const EncodingTable&) = delete;
    EncodingTable& operator=(const EncodingTable&) = delete;

    /// Eager parse — full table in memory, lookups are pure hash hits.
    static EncodingTable parse(std::span<const u8> data,
                               interfaces::WorkerPool* pool = nullptr);

    /// Lazy parse — only header + CKey page TOC up front; pages decoded
    /// on findByCKey miss. findByEKey forces ensureFullyParsed.
    static EncodingTable openLazy(std::span<const u8> data,
                                  interfaces::WorkerPool* pool = nullptr);

    /// Lazy + range-fetched over CDN. fetcher must outlive the table.
    static EncodingTable openLazyOnline(CdnFetcher* fetcher,
                                        const std::string& archiveKeyHex,
                                        u64 archiveOffset,
                                        u32 encodedSize,
                                        const KeyRing* keys = nullptr,
                                        interfaces::WorkerPool* pool = nullptr);

    class LazyEncodingBlob; // opaque; impls live in encoding.cpp

    /// Force-parse any pending lazy pages and build the EKey index.
    void ensureFullyParsed() const;

    /// True if header parsed (eager or lazy); distinct from entryCount()==0,
    /// which lazy tables legitimately return until the first page faults in.
    bool isValid() const;

    const EncodingEntry* findByCKey(std::span<const u8, 16> cKey, size_t matchBytes = 0) const;
    const EncodingEntry* findByEKey(std::span<const u8, 16> eKey, size_t matchBytes = 0) const;

    void insert(const EncodingEntry& entry);
    std::vector<u8> serialize() const;
    size_t entryCount() const;

    /// Forces ensureFullyParsed() in lazy mode.
    const std::vector<EncodingEntry>& entries() const;

private:
    mutable std::vector<EncodingEntry> m_entries;
    mutable FlatHashMap<size_t> m_cKeyIndex;
    mutable FlatHashMap<size_t> m_eKeyIndex;

    struct LazyState;
    mutable std::unique_ptr<LazyState> m_lazy;

    void parseCKeyPage(u32 pageIdx) const;
    static bool initLazyFromBlob(LazyState& state,
                                 std::unique_ptr<LazyEncodingBlob> blob);
};

} // namespace whiteout::storages::casc
