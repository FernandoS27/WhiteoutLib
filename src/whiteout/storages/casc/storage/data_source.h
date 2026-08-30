// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file data_source.h
/// @brief Abstract internal interface for fetching BLTE blobs by EKey.
///
/// Implemented by LocalDataSource (memory-mapped archives) and
/// OnlineDataSource (CDN fetcher). Enables shared resolution logic
/// between Storage and OnlineStorage.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace whiteout::storages::casc {

/// Location of a BLTE blob inside an archive.
/// Normalizes the difference between local IndexEntry and OnlineIndexTable::Entry.
struct IndexLocation {
    u32 archiveIndex = 0;
    u64 offset = 0;
    u32 encodedSize = 0;
    bool directBLTE = false; ///< Local only: if true, no 30-byte per-entry header.
};

/// Abstract data source for fetching BLTE-encoded blobs.
class DataSource {
public:
    virtual ~DataSource() = default;

    /// Fetch BLTE-encoded data for the given EKey (loose file).
    /// Returns empty vector on failure.
    virtual std::vector<u8> fetchBlte(const std::array<u8, 16>& eKey) = 0;

    /// Fetch BLTE-encoded data from a specific archive location.
    virtual std::vector<u8> fetchBlte(u32 archiveIndex, u64 offset, u32 encodedSize) = 0;

    /// Fetch BLTE-encoded data using a resolved index location.
    /// Default implementation delegates to the 3-arg fetchBlte.
    virtual std::vector<u8> fetchBlte(const IndexLocation& loc) {
        return fetchBlte(loc.archiveIndex, loc.offset, loc.encodedSize);
    }

    /// Locate the blob for @p eKey. Takes the whole key rather than the
    /// truncated prefix an `.idx` lookup needs: a static-build-config install
    /// keeps the archive coordinates in the bytes past that prefix.
    /// Returns std::nullopt if the key is not indexed.
    virtual std::optional<IndexLocation> findInIndex(std::span<const u8, 16> eKey) const = 0;

    /// Warm the cache line a later findInIndex of @p eKey will touch. A sweep
    /// over millions of root entries is DRAM-latency bound, so issuing this a
    /// few entries ahead hides most of the miss. No-op by default.
    virtual void prefetchIndex(std::span<const u8, 16> eKey) const {
        (void)eKey;
    }
};

} // namespace whiteout::storages::casc
