// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "online_index.h"
#include "../../common/hex.h"
#include "../storage/key_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace whiteout::storages::casc {

// pimpl so OnlineIndexTable stays moveable despite once_flags + mutex.
struct OnlineIndexTable::LazyState {
    CdnFetcher* fetcher = nullptr;
    const std::vector<std::array<u8, 16>>* archiveEKeys = nullptr;
    interfaces::WorkerPool* pool = nullptr;
    std::vector<std::once_flag> archiveFlags;
    mutable std::shared_mutex mutex;
};

OnlineIndexTable::OnlineIndexTable() = default;
OnlineIndexTable::~OnlineIndexTable() = default;
OnlineIndexTable::OnlineIndexTable(OnlineIndexTable&&) noexcept = default;
OnlineIndexTable& OnlineIndexTable::operator=(OnlineIndexTable&&) noexcept = default;

// ============================================================================
// Hash
// ============================================================================

u64 OnlineIndexTable::eKeyHash(std::span<const u8> eKey) {
    // Same approach as IndexTable: hash the first 9 bytes.
    u64 h = 0;
    size_t n = std::min(eKey.size(), size_t(kEKeyTruncSize));
    std::memcpy(&h, eKey.data(), std::min(n, size_t(8)));
    if (n > 8) {
        u64 extra = eKey[8];
        h ^= (extra << 56);
    }
    return h;
}

// ============================================================================
// CDN .index footer-based parser
// ============================================================================

namespace {

#pragma pack(push, 1)
struct CdnIndexFooter {
    // Note: hashBytes of toc_hash precede this struct, but we back-compute.
    u8 formatRevision;    // Expected: 1
    u8 flags0;
    u8 flags1;
    u8 blockSizeKBytes;   // Typically 4 (→ 4096 bytes per block)
    u8 offsetBytes;       // 4 for archive, 0 for loose, 6 for archive-group
    u8 sizeBytes;         // Typically 4
    u8 keyBytes;          // Typically 9 or 16
    u8 hashBytes;         // Typically 8
    u32 numElements;      // Entry count (little-endian)
    // Followed by footerHash[hashBytes], but we treat it separately.
};
#pragma pack(pop)

[[maybe_unused]] static constexpr size_t kMinFooterSize = sizeof(CdnIndexFooter); // 12 bytes minimum

} // anonymous namespace

OnlineIndexTable OnlineIndexTable::parse(std::span<const u8> data, u32 archiveIndex) {
    OnlineIndexTable table;

    if (data.size() < 28) return table; // Too small for any valid index

    // The footer is at the end of the file. The last bytes are:
    //   [footer(12 bytes)][footerHash(hashBytes)]
    // But we don't know hashBytes yet. Try reading the footer fields
    // at a fixed offset from the end.
    //
    // Standard layout: last (12 + hashBytes) bytes = footer + footerHash.
    // We need to use a heuristic: try hashBytes = 8 (most common).
    constexpr u8 kDefaultHashBytes = 8;
    size_t footerEnd = data.size() - kDefaultHashBytes; // Position after footer, before footerHash
    if (footerEnd < sizeof(CdnIndexFooter)) return table;

    size_t footerStart = footerEnd - sizeof(CdnIndexFooter);

    CdnIndexFooter footer;
    std::memcpy(&footer, data.data() + footerStart, sizeof(CdnIndexFooter));

    if (footer.formatRevision != 1) return table;
    if (footer.blockSizeKBytes == 0) return table;
    if (footer.keyBytes == 0) return table;

    u32 blockSize = u32(footer.blockSizeKBytes) * 1024;
    u8 keyBytes = footer.keyBytes;
    u8 offsetBytes = footer.offsetBytes;
    u8 sizeBytes = footer.sizeBytes;
    u32 numElements = footer.numElements;
    u8 hashBytes = footer.hashBytes;

    // Re-validate footer position with actual hashBytes.
    if (hashBytes != kDefaultHashBytes) {
        footerEnd = data.size() - hashBytes;
        if (footerEnd < sizeof(CdnIndexFooter)) return table;
        footerStart = footerEnd - sizeof(CdnIndexFooter);
        std::memcpy(&footer, data.data() + footerStart, sizeof(CdnIndexFooter));
    }

    size_t entrySize = keyBytes + offsetBytes + sizeBytes;
    if (entrySize == 0) return table;

    // Before the footer is the table-of-contents (TOC).
    // TOC: one entry per block = (lastKey[keyBytes] + blockHash[hashBytes]) per block.
    u32 entriesPerBlock = blockSize / u32(entrySize);
    if (entriesPerBlock == 0) return table;
    u32 numBlocks = (numElements + entriesPerBlock - 1) / entriesPerBlock;

    size_t tocEntrySize = keyBytes + hashBytes;
    size_t tocSize = numBlocks * tocEntrySize;

    // Validate: tocHash(hashBytes) sits before the TOC.
    size_t tocEnd = footerStart - hashBytes; // after tocHash
    if (tocEnd < tocSize) return table;
    [[maybe_unused]] size_t tocStart = tocEnd - tocSize;

    // Data blocks start at beginning of file.
    size_t dataStart = 0;

    // Parse entries from data blocks.
    u32 entriesParsed = 0;
    table.m_entries.reserve(numElements);

    for (u32 block = 0; block < numBlocks && entriesParsed < numElements; ++block) {
        size_t blockOffset = dataStart + u64(block) * blockSize;
        u32 entriesInBlock = std::min(entriesPerBlock, numElements - entriesParsed);

        for (u32 e = 0; e < entriesInBlock; ++e) {
            size_t pos = blockOffset + u64(e) * entrySize;
            if (pos + entrySize > data.size()) break;

            Entry entry;
            entry.archiveIndex = archiveIndex;

            // Read EKey (keyBytes, typically 9).
            std::array<u8, 16> eKey{};
            size_t copyBytes = std::min<size_t>(keyBytes, 16);
            std::memcpy(eKey.data(), data.data() + pos, copyBytes);
            pos += keyBytes;

            // CDN index entry layout after eKey: [encodedSize][archiveOffset]
            // (size comes first, offset second — per CascLib/TACT spec).

            // Read encoded size (sizeBytes, big-endian).
            u32 size = 0;
            if (sizeBytes == 4) {
                size = (u32(data[pos]) << 24) | (u32(data[pos + 1]) << 16) |
                       (u32(data[pos + 2]) << 8) | u32(data[pos + 3]);
            }
            pos += sizeBytes;
            entry.encodedSize = size;

            // Read offset (offsetBytes, big-endian). May be 0, 4, or 6.
            u64 offset = 0;
            if (offsetBytes == 4) {
                // 4-byte big-endian offset.
                offset = (u64(data[pos]) << 24) | (u64(data[pos + 1]) << 16) |
                         (u64(data[pos + 2]) << 8) | u64(data[pos + 3]);
            } else if (offsetBytes == 6) {
                // 6-byte big-endian: first 2 = archive index, last 4 = offset.
                u32 archIdx = (u32(data[pos]) << 8) | u32(data[pos + 1]);
                entry.archiveIndex = archIdx;
                offset = (u64(data[pos + 2]) << 24) | (u64(data[pos + 3]) << 16) |
                         (u64(data[pos + 4]) << 8) | u64(data[pos + 5]);
            }
            // offsetBytes == 0: loose file, offset stays 0.
            pos += offsetBytes;
            entry.archiveOffset = offset;

            // Check for zero-key (padding entry at end of block).
            bool isZero = true;
            for (size_t i = 0; i < copyBytes; ++i) {
                if (eKey[i] != 0) { isZero = false; break; }
            }
            if (isZero) continue;

            u64 hash = eKeyHash(std::span<const u8>(eKey.data(), copyBytes));
            table.m_entries[hash] = entry;

            ++entriesParsed;
        }
    }

    return table;
}

// ============================================================================
// Lookup
// ============================================================================

const OnlineIndexTable::Entry* OnlineIndexTable::find(std::span<const u8> eKeyPrefix) const {
    u64 hash = eKeyHash(eKeyPrefix);

    if (!m_lazy) {
        auto it = m_entries.find(hash);
        return (it != m_entries.end()) ? &it->second : nullptr;
    }

    {
        std::shared_lock<std::shared_mutex> lk(m_lazy->mutex);
        auto it = m_entries.find(hash);
        if (it != m_entries.end()) return &it->second;
    }

    // Fault in archives one at a time; terminates on hit or when all loaded.
    const size_t N = m_lazy->archiveEKeys ? m_lazy->archiveEKeys->size() : 0;
    for (size_t i = 0; i < N; ++i) {
        loadArchive(u32(i));
        std::shared_lock<std::shared_mutex> lk(m_lazy->mutex);
        auto it = m_entries.find(hash);
        if (it != m_entries.end()) return &it->second;
    }
    return nullptr;
}

void OnlineIndexTable::merge(const OnlineIndexTable& other) {
    for (auto& [hash, entry] : other.m_entries) {
        m_entries.insert_or_assign(hash, entry);
    }
}

size_t OnlineIndexTable::entryCount() const {
    if (m_lazy) {
        std::shared_lock<std::shared_mutex> lk(m_lazy->mutex);
        return m_entries.size();
    }
    return m_entries.size();
}

// ============================================================================
// Lazy mode
// ============================================================================

OnlineIndexTable OnlineIndexTable::makeLazy(
    CdnFetcher* fetcher,
    const std::vector<std::array<u8, 16>>* archiveEKeys,
    interfaces::WorkerPool* pool) {

    OnlineIndexTable table;
    table.m_lazy = std::make_unique<LazyState>();
    table.m_lazy->fetcher = fetcher;
    table.m_lazy->archiveEKeys = archiveEKeys;
    table.m_lazy->pool = pool;
    if (archiveEKeys)
        table.m_lazy->archiveFlags = std::vector<std::once_flag>(archiveEKeys->size());
    return table;
}

void OnlineIndexTable::loadArchive(u32 archiveIndex) const {
    if (!m_lazy || !m_lazy->fetcher || !m_lazy->archiveEKeys) return;
    if (archiveIndex >= m_lazy->archiveEKeys->size()) return;

    std::call_once(m_lazy->archiveFlags[archiveIndex], [&]() {
        auto keyHex = storages::common::hexEncode16(
            (*m_lazy->archiveEKeys)[archiveIndex]);
        auto indexKeyHex = keyHex + ".index";
        auto data = m_lazy->fetcher->fetch("data", indexKeyHex);
        if (!data || data->empty()) return;

        auto parsed = OnlineIndexTable::parse(*data, archiveIndex);

        std::unique_lock<std::shared_mutex> lk(m_lazy->mutex);
        for (auto& [hash, entry] : parsed.m_entries) {
            m_entries.insert_or_assign(hash, entry);
        }
    });
}

void OnlineIndexTable::ensureAllLoaded() const {
    if (!m_lazy || !m_lazy->archiveEKeys) return;
    const size_t N = m_lazy->archiveEKeys->size();
    for (size_t i = 0; i < N; ++i)
        loadArchive(u32(i));
}

// ============================================================================
// Batch loading from CDN
// ============================================================================

OnlineIndexTable OnlineIndexTable::loadAll(
    CdnFetcher& fetcher,
    const std::vector<std::array<u8, 16>>& archiveEKeys,
    interfaces::WorkerPool* pool) {

    OnlineIndexTable combined;
    if (archiveEKeys.empty()) return combined;

    const size_t N = archiveEKeys.size();

    // Fetch all archive index files. Each archive EKey has a corresponding
    // .index file at <archiveEKey>.index on CDN.
    // We store parsed per-archive tables and merge at the end.

    struct IndexResult {
        OnlineIndexTable table;
        bool ok = false;
    };
    std::vector<IndexResult> results(N);

    auto loadSerial = [&]() {
        for (size_t i = 0; i < N; ++i) {
            auto keyHex = storages::common::hexEncode16(archiveEKeys[i]);
            auto indexKeyHex = keyHex + ".index";
            auto data = fetcher.fetch("data", indexKeyHex);
            if (data && !data->empty()) {
                results[i].table = OnlineIndexTable::parse(*data, u32(i));
                results[i].ok = true;
            }
        }
    };

    if (pool && N >= 4) {
        // Parallel fetch using an atomic completion counter.
        // We heap-allocate the synchronization state so callback lambdas
        // never reference stack variables that might be destroyed.
        struct WaitState {
            std::atomic<size_t> completed{0};
            std::mutex mtx;
            std::condition_variable cv;
        };
        auto state = std::make_shared<WaitState>();

        for (size_t i = 0; i < N; ++i) {
            auto keyHex = storages::common::hexEncode16(archiveEKeys[i]);
            auto indexKeyHex = keyHex + ".index";

            fetcher.fetchAsync("data", indexKeyHex,
                [&results, i, archIdx = u32(i), state, N](std::optional<std::vector<u8>> data) {
                    if (data && !data->empty()) {
                        results[i].table = OnlineIndexTable::parse(*data, archIdx);
                        results[i].ok = true;
                    }
                    // fetch_add returns old value; +1 == total completed.
                    if (state->completed.fetch_add(1, std::memory_order_acq_rel) + 1 == N) {
                        std::lock_guard<std::mutex> lk(state->mtx);
                        state->cv.notify_one();
                    }
                });
        }

        std::unique_lock<std::mutex> lk(state->mtx);
        state->cv.wait_for(lk, std::chrono::seconds(120), [&] {
            return state->completed.load(std::memory_order_acquire) >= N;
        });
    } else {
        loadSerial();
    }

    // Merge all per-archive tables.
    [[maybe_unused]] size_t okCount = 0;
    for (auto& r : results) {
        if (r.ok) {
            combined.merge(r.table);
            ++okCount;
        }
    }

    return combined;
}

OnlineIndexTable OnlineIndexTable::loadLoose(CdnFetcher& fetcher,
                                              const std::string& fileIndexKey) {
    auto data = fetcher.fetch("data", fileIndexKey);
    if (!data || data->empty()) return {};
    return parse(*data, 0); // archiveIndex=0 for loose (not meaningful).
}

} // namespace whiteout::storages::casc
