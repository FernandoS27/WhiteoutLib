// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file storage_backend_impl.cpp
/// @brief Template method bodies + explicit instantiations for StorageBackendImpl.

#include "constants.h"
#include "storage_backend_impl.h"
#include "storage_impl.h"

#include <whiteout/utils/job_group.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>

namespace whiteout::storages::casc {

// ============================================================================
// Container slicing helper (shared by resolve methods)
// ============================================================================

namespace {

/// Slice a container sub-entry from decoded data, prepending header bytes.
inline std::optional<std::vector<u8>> sliceContainer(const std::vector<u8>& data,
                                                     const RootEntry& entry) {
    auto off = static_cast<size_t>(entry.containerOffset);
    auto sz = static_cast<size_t>(entry.containerSize);
    if (off + sz > data.size())
        return std::nullopt;

    size_t const hdrSz = entry.headerSize;
    std::vector<u8> result(hdrSz + sz);
    if (hdrSz > 0)
        std::memcpy(result.data(), entry.headerPrefix.data(), hdrSz);
    std::memcpy(result.data() + hdrSz, data.data() + off, sz);
    return result;
}

} // anonymous namespace

// ============================================================================
// resolveCKey
// ============================================================================

template <typename DT, typename CT>
std::vector<u8> StorageBackendImpl<DT, CT>::resolveCKey(std::span<const u8, 16> cKey,
                                                        interfaces::WorkerPool* poolToUse) const {

    std::array<u8, 16> cKey16{};
    std::memcpy(cKey16.data(), cKey.data(), 16);

    // Cache lookup (no-op for NoCachePolicy — optimised away).
    if (auto cached = m_cache.get(cKey16))
        return std::move(*cached);

    auto encEntry = m_encoding.findByCKey(cKey, kEKeyTruncSize);
    if (!encEntry)
        return {};

    // Index first, then loose-file fallback.
    auto loc = m_data.findInIndex(encEntry->eKey);
    std::vector<u8> owned;
    std::span<const u8> blteData;
    if (loc) {
        if constexpr (DT::supportsZeroCopy())
            blteData = m_data.viewBlte(*loc);
        if (blteData.empty()) {
            owned = m_data.fetchBlte(*loc);
            blteData = owned;
        }
    } else {
        owned = m_data.fetchBlte(encEntry->eKey);
        blteData = owned;
    }
    if (blteData.empty())
        return {};

    auto decoded = blteDecode(blteData, &m_keyRing, poolToUse);
    if (!decoded.success)
        return {};

    // Cache store (no-op for NoCachePolicy).
    m_cache.put(cKey16, decoded.data);
    return std::move(decoded.data);
}

// ============================================================================
// resolveEKey
// ============================================================================

template <typename DT, typename CT>
std::vector<u8> StorageBackendImpl<DT, CT>::resolveEKey(std::span<const u8, 16> eKey,
                                                        interfaces::WorkerPool* poolToUse) const {

    std::array<u8, 16> eKey16{};
    std::memcpy(eKey16.data(), eKey.data(), 16);

    if (auto cached = m_cache.get(eKey16))
        return std::move(*cached);

    auto loc = m_data.findInIndex(eKey);
    std::vector<u8> owned;
    std::span<const u8> blteData;
    if (loc) {
        if constexpr (DT::supportsZeroCopy())
            blteData = m_data.viewBlte(*loc);
        if (blteData.empty()) {
            owned = m_data.fetchBlte(*loc);
            blteData = owned;
        }
    } else {
        owned = m_data.fetchBlte(eKey16);
        blteData = owned;
    }
    if (blteData.empty())
        return {};

    auto decoded = blteDecode(blteData, &m_keyRing, poolToUse);
    if (!decoded.success)
        return {};

    m_cache.put(eKey16, decoded.data);
    return std::move(decoded.data);
}

// ============================================================================
// resolveRootEntry
// ============================================================================

template <typename DT, typename CT>
std::optional<std::vector<u8>> StorageBackendImpl<DT, CT>::resolveRootEntry(
    const std::vector<const RootEntry*>& entries, u32 localeFlags) const {

    if (entries.empty())
        return std::nullopt;

    const RootEntry* best = selectBestEntry(entries, localeFlags);
    if (!best)
        return std::nullopt;

    // Container sub-entry: try cache first (no-op for NoCachePolicy).
    if (best->containerOffset != 0) {
        std::array<u8, 16> const cacheKey = !isZeroKey(best->eKey) ? best->eKey : best->cKey;
        if (auto cached = m_cache.view(cacheKey)) {
            auto off = static_cast<size_t>(best->containerOffset);
            auto sz = static_cast<size_t>(best->containerSize);
            if (off + sz <= cached->size()) {
                size_t const hdrSz = best->headerSize;
                std::vector<u8> result(hdrSz + sz);
                if (hdrSz > 0)
                    std::memcpy(result.data(), best->headerPrefix.data(), hdrSz);
                std::memcpy(result.data() + hdrSz, cached->data().data() + off, sz);
                return result;
            }
        }
    }

    if constexpr (DT::isLocal()) {
        // Local path: direct index lookup + mmap read + BLTE decode.
        // Prefer the entry's own EKey — TVFS roots carry it straight from the
        // manifest, so the CKey -> encoding-table lookup is a needless step.
        std::optional<IndexLocation> loc;
        std::array<u8, 16> eKey{};
        if (!isZeroKey(best->eKey)) {
            eKey = best->eKey;
            loc = m_data.findInIndex(eKey);
        }
        if (!loc && !isZeroKey(best->cKey)) {
            if (auto encEntry = m_encoding.findByCKey(best->cKey, kEKeyTruncSize)) {
                eKey = encEntry->eKey;
                loc = m_data.findInIndex(eKey);
            }
        }

        std::vector<u8> blteData;
        if (loc) {
            auto span = m_data.viewBlte(*loc);
            blteData.assign(span.begin(), span.end());
        }
        if (blteData.empty()) {
            // No index entry, or an archive slot that produced nothing: the
            // file is stored loose rather than inside a `.data` archive.
            // resolveCKey has always fallen back this way; not doing it here is
            // why the same file could be read by content key and not by name.
            if (isZeroKey(eKey))
                return std::nullopt;
            blteData = m_data.fetchBlte(eKey);
            if (blteData.empty())
                return std::nullopt;
        }

        auto decoded = blteDecode(blteData, &m_keyRing, m_pool);
        if (!decoded.success)
            return std::nullopt;

        if (best->containerOffset != 0) {
            std::array<u8, 16> const cacheKey = !isZeroKey(best->eKey) ? best->eKey : best->cKey;
            m_cache.put(cacheKey, decoded.data);
            return sliceContainer(decoded.data, *best);
        }
        return std::move(decoded.data);
    } else {
        // Online path: resolveCKey/resolveEKey already BLTE-decodes.
        // Prefer the entry's own EKey when present (TVFS roots carry it
        // straight from the manifest) — skips a CKey -> encoding-table lookup,
        // which under lazy encoding is a CDN range-fetch on the cold path.
        if (!isZeroKey(best->eKey)) {
            auto data = resolveEKey(best->eKey);
            if (!data.empty()) {
                if (best->containerOffset != 0) {
                    std::array<u8, 16> const cacheKey =
                        !isZeroKey(best->eKey) ? best->eKey : best->cKey;
                    m_cache.put(cacheKey, data);
                    return sliceContainer(data, *best);
                }
                return data;
            }
        }
        if (!isZeroKey(best->cKey)) {
            auto data = resolveCKey(best->cKey);
            if (!data.empty()) {
                if (best->containerOffset != 0) {
                    std::array<u8, 16> const cacheKey =
                        !isZeroKey(best->eKey) ? best->eKey : best->cKey;
                    m_cache.put(cacheKey, data);
                    return sliceContainer(data, *best);
                }
                return data;
            }
        }
        return std::nullopt;
    }
}

// ============================================================================
// resolveBatch — Phase 0.5 (cache) + Phase 1 (fetch) delegated to traits
// ============================================================================

template <typename DT, typename CT>
void StorageBackendImpl<DT, CT>::resolveBatch(std::span<ResolveWork> work,
                                              std::span<ResolvedBlob> blobs,
                                              interfaces::WorkerPool* pool) const {

    // Phase 0.5: Container cache lookup.
    if constexpr (CT::hasCache()) {
        for (size_t i = 0; i < work.size(); ++i) {
            const RootEntry* best = selectBestEntry(work[i].rootEntries, work[i].localeFlags);
            if (!best || best->containerOffset == 0)
                continue;

            std::array<u8, 16> const cacheKey = !isZeroKey(best->eKey) ? best->eKey : best->cKey;
            auto cached = m_cache.view(cacheKey);
            if (!cached)
                continue;

            auto off = static_cast<size_t>(best->containerOffset);
            auto sz = static_cast<size_t>(best->containerSize);
            if (off + sz > cached->size())
                continue;

            size_t const hdrSz = best->headerSize;
            std::vector<u8> sliced(hdrSz + sz);
            if (hdrSz > 0)
                std::memcpy(sliced.data(), best->headerPrefix.data(), hdrSz);
            std::memcpy(sliced.data() + hdrSz, cached->data().data() + off, sz);

            // Signal cached result (storage_core will handle setting it on BatchReadResult).
            blobs[i].blteData = std::move(sliced);
            blobs[i].resolved = true;
            work[i].cachedResult = true;
        }
    }

    // Phase 1: Data fetch (delegated to DataTraits).
    m_data.resolveBatchPhase1(m_encoding, work, blobs, pool);
}

// ============================================================================
// LocalDataTraits::resolveBatchPhase1 — parallel mmap reads
// ============================================================================

void LocalDataTraits::resolveBatchPhase1(const EncodingTable& encoding, std::span<ResolveWork> work,
                                         std::span<ResolvedBlob> blobs,
                                         interfaces::WorkerPool* pool) const {

    auto resolveOne = [&](size_t idx) {
        auto& w = work[idx];
        auto& blob = blobs[idx];
        if (w.cachedResult)
            return;

        const RootEntry* best = selectBestEntry(w.rootEntries, w.localeFlags);
        if (!best) {
            blob.error = "no matching root entry";
            return;
        }

        std::optional<IndexLocation> loc;
        if (!isZeroKey(best->eKey)) {
            loc = dataSource->findInIndex(best->eKey);
        }
        if (!loc && !isZeroKey(best->cKey)) {
            auto encEntry = encoding.findByCKey(best->cKey, kEKeyTruncSize);
            if (encEntry)
                loc = dataSource->findInIndex(encEntry->eKey);
        }
        if (!loc) {
            blob.error = "file not found in index";
            return;
        }

        blob.blteSpan = dataSource->viewBlte(*loc);
        if (blob.blteSpan.empty()) {
            blob.error = "failed to read raw BLTE data from archive";
            return;
        }
        blob.resolved = true;
        blob.useSpan = true;
    };

    constexpr size_t kMinBatchParallel = 4;
    if (pool && work.size() >= kMinBatchParallel && pool->threadCount() > 0) {
        utils::JobGroup resolveGroup;
        resolveGroup.add(work.size());
        for (size_t idx = 0; idx < work.size(); ++idx) {
            interfaces::WorkerTask task;
            task.fn = [&, idx]() {
                resolveOne(idx);
                resolveGroup.done();
            };
            pool->submit(task);
        }
        resolveGroup.wait();
    } else {
        for (size_t idx = 0; idx < work.size(); ++idx)
            resolveOne(idx);
    }
}

// ============================================================================
// OnlineDataTraits::resolveBatchPhase1 — async HTTP fetch
// ============================================================================

void OnlineDataTraits::resolveBatchPhase1(const EncodingTable& encoding,
                                          std::span<ResolveWork> work,
                                          std::span<ResolvedBlob> blobs,
                                          interfaces::WorkerPool* /*pool*/) const {

    // Pass 1: Resolve each to fetch parameters.
    struct FetchParams {
        bool needsFetch = false;
        bool useArchive = false;
        u32 archiveIndex = 0;
        u64 archiveOffset = 0;
        u32 encodedSize = 0;
        std::array<u8, 16> eKey{};
    };
    std::vector<FetchParams> fetchParams(work.size());

    for (size_t idx = 0; idx < work.size(); ++idx) {
        auto& w = work[idx];
        auto& blob = blobs[idx];
        auto& fp = fetchParams[idx];
        if (w.cachedResult)
            continue;

        const RootEntry* best = selectBestEntry(w.rootEntries, w.localeFlags);
        if (!best) {
            blob.error = "no matching root entry";
            continue;
        }

        const OnlineIndexTable::Entry* idxEntry = nullptr;
        std::array<u8, 16> eKey{};

        // Prefer the entry's own EKey — TVFS roots carry it from the manifest,
        // avoiding a CKey -> encoding-table lookup per file.
        if (!isZeroKey(best->eKey)) {
            eKey = best->eKey;
            idxEntry = onlineIndex->find(eKeyTrunc(best->eKey));
        }
        if (!idxEntry && !isZeroKey(best->cKey)) {
            auto encEntry = encoding.findByCKey(best->cKey, kEKeyTruncSize);
            if (encEntry) {
                eKey = encEntry->eKey;
                idxEntry = onlineIndex->find(eKeyTrunc(encEntry->eKey));
            }
        }

        if (idxEntry) {
            fp.needsFetch = true;
            fp.useArchive = true;
            fp.archiveIndex = idxEntry->archiveIndex;
            fp.archiveOffset = idxEntry->archiveOffset;
            fp.encodedSize = idxEntry->encodedSize;
        } else if (!isZeroKey(eKey)) {
            fp.needsFetch = true;
            fp.useArchive = false;
            fp.eKey = eKey;
        } else {
            blob.error = "failed to resolve to CDN fetch params";
        }
    }

    // Pass 2: Dispatch async HTTP fetches.
    struct WaitState {
        std::atomic<size_t> completed{0};
        size_t total = 0;
        std::mutex mtx;
        std::condition_variable cv;
    };
    auto state = std::make_shared<WaitState>();
    for (auto& fp : fetchParams)
        if (fp.needsFetch)
            ++state->total;

    if (state->total > 0) {
        auto makeCallback = [&blobs, state](size_t idx) {
            return [&blobs, state, idx](std::optional<std::vector<u8>> data) {
                if (data && !data->empty()) {
                    blobs[idx].blteData = std::move(*data);
                    blobs[idx].resolved = true;
                } else {
                    blobs[idx].error = "failed to fetch BLTE data from CDN";
                }
                if (state->completed.fetch_add(1, std::memory_order_acq_rel) + 1 >= state->total) {
                    std::lock_guard<std::mutex> const lk(state->mtx);
                    state->cv.notify_one();
                }
            };
        };

        for (size_t idx = 0; idx < work.size(); ++idx) {
            auto& fp = fetchParams[idx];
            if (!fp.needsFetch)
                continue;
            if (fp.useArchive) {
                dataSource->fetchBlteAsync(fp.archiveIndex, fp.archiveOffset, fp.encodedSize,
                                           makeCallback(idx));
            } else {
                dataSource->fetchBlteAsync(fp.eKey, makeCallback(idx));
            }
        }

        std::unique_lock<std::mutex> lk(state->mtx);
        state->cv.wait_for(lk, std::chrono::seconds(120), [&] {
            return state->completed.load(std::memory_order_acquire) >= state->total;
        });
    }
}

// ============================================================================
// LocalDataTraits::prefetchVfs — JobGroup parallel resolve
// ============================================================================

std::unordered_map<u64, std::vector<u8>> LocalDataTraits::prefetchVfs(
    const Storage::Impl& impl, const std::vector<std::array<u8, 16>>& vfsEKeys,
    const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey,
    const ProgressSink* sink) const {
    return prefetchVfsLocal(impl, vfsEKeys, vfsEKeyToCKey, sink);
}

// ============================================================================
// OnlineDataTraits::prefetchVfs — async HTTP
// ============================================================================

std::unordered_map<u64, std::vector<u8>> OnlineDataTraits::prefetchVfs(
    const Storage::Impl& impl, const std::vector<std::array<u8, 16>>& vfsEKeys,
    const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey,
    const ProgressSink* sink) const {
    return prefetchVfsOnline(impl, vfsEKeys, vfsEKeyToCKey, sink);
}

// ============================================================================
// Explicit template instantiations — 4 variants
// ============================================================================

template class StorageBackendImpl<LocalDataTraits, NoCachePolicy>;
template class StorageBackendImpl<LocalDataTraits, MemCacheEnabled>;
template class StorageBackendImpl<OnlineDataTraits, NoCachePolicy>;
template class StorageBackendImpl<OnlineDataTraits, MemCacheEnabled>;

} // namespace whiteout::storages::casc
