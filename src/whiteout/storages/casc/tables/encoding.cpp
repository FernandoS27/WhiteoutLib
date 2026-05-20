// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "encoding.h"
#include "../cdn/cdn_fetcher.h"
#include "../codec/blte.h"
#include "../storage/constants.h"
#include "../storage/key_utils.h"
#include "../../common/byte_order.h"
#include "../../common/md5.h"

#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>
#include <list>
#include <mutex>
#include <numeric>
#include <shared_mutex>
#include <unordered_map>

namespace whiteout::storages::casc {

// Byte-source for the encoding blob — in-memory or range-fetched.
class EncodingTable::LazyEncodingBlob {
public:
    virtual ~LazyEncodingBlob() = default;
    virtual bool resolveLayout() = 0;
    virtual size_t totalDecodedSize() const = 0;
    virtual std::vector<u8> getRange(size_t offset, size_t length) = 0;
};

namespace {

class InMemoryEncodingBlob : public EncodingTable::LazyEncodingBlob {
public:
    explicit InMemoryEncodingBlob(std::vector<u8> data) : m_data(std::move(data)) {}

    bool resolveLayout() override { return true; }
    size_t totalDecodedSize() const override { return m_data.size(); }

    std::vector<u8> getRange(size_t offset, size_t length) override {
        if (offset >= m_data.size()) return {};
        size_t const take = std::min(length, m_data.size() - offset);
        return std::vector<u8>(m_data.begin() + offset,
                               m_data.begin() + offset + take);
    }

private:
    std::vector<u8> m_data;
};

class NetworkEncodingBlob : public EncodingTable::LazyEncodingBlob {
public:
    NetworkEncodingBlob(CdnFetcher* fetcher, std::string archiveKeyHex,
                        u64 baseOffset, u32 totalEncodedSize,
                        const KeyRing* keys)
        : m_fetcher(fetcher), m_archiveKey(std::move(archiveKeyHex)),
          m_baseOffset(baseOffset), m_encodedSize(totalEncodedSize),
          m_keys(keys) {}

    bool resolveLayout() override {
        if (m_layoutResolved) return true;

        auto hdrBytes = m_fetcher->fetchRange(m_archiveKey, m_baseOffset, 8);
        if (!hdrBytes || hdrBytes->size() < 8) return false;

        u32 const magic = storages::common::readBE32(hdrBytes->data());
        if (magic != 0x424C5445u) return false; // 'BLTE'
        u32 const headerSize = storages::common::readBE32(hdrBytes->data() + 4);

        if (headerSize == 0) {
            // Single-frame BLTE — no per-frame layout, no sub-range fetch
            // possible. Pull the whole thing once and treat it as decoded.
            auto full = m_fetcher->fetchRange(m_archiveKey, m_baseOffset, m_encodedSize);
            if (!full) return false;

            BlteFrameLayout sl;
            sl.frames.push_back({u32(m_encodedSize - 8), 0});
            sl.offsets.push_back(8);
            sl.valid = true;

            auto decoded = blteDecodeFrame(*full, sl, 0, m_keys);
            if (!decoded.success) return false;

            m_singleFrameDecoded = std::move(decoded.data);
            m_totalDecoded = m_singleFrameDecoded.size();
            m_isSingleFrame = true;
            m_layoutResolved = true;
            return true;
        }

        auto tableBytes = m_fetcher->fetchRange(m_archiveKey, m_baseOffset + 8, headerSize);
        if (!tableBytes || tableBytes->size() < headerSize) return false;

        // flags(1) + frameCount(u24 BE), then frameCount * (compSize, uncompSize, md5).
        if (tableBytes->size() < 4) return false;
        u32 const frameCount =
            (u32((*tableBytes)[1]) << 16) | (u32((*tableBytes)[2]) << 8) | u32((*tableBytes)[3]);

        constexpr size_t kEntrySize = 4 + 4 + 16;
        if (tableBytes->size() < 4 + frameCount * kEntrySize) return false;

        m_layout.frames.reserve(frameCount);
        m_layout.offsets.reserve(frameCount);
        m_cumDecoded.reserve(frameCount + 1);
        m_cumDecoded.push_back(0);

        u64 frameDataOff = 8 + headerSize;
        u64 cumUncomp = 0;
        for (u32 i = 0; i < frameCount; ++i) {
            const u8* entry = tableBytes->data() + 4 + i * kEntrySize;
            u32 const compSize = storages::common::readBE32(entry);
            u32 const uncompSize = storages::common::readBE32(entry + 4);
            m_layout.frames.push_back({compSize, uncompSize});
            m_layout.offsets.push_back(frameDataOff);
            cumUncomp += uncompSize;
            m_cumDecoded.push_back(cumUncomp);
            frameDataOff += compSize;
        }
        m_layout.valid = true;
        m_totalDecoded = cumUncomp;
        m_layoutResolved = true;
        return true;
    }

    size_t totalDecodedSize() const override { return m_totalDecoded; }

    std::vector<u8> getRange(size_t offset, size_t length) override {
        if (!resolveLayout()) return {};
        if (offset >= m_totalDecoded) return {};

        size_t const end = std::min(offset + length, m_totalDecoded);
        if (end <= offset) return {};

        if (m_isSingleFrame) {
            return std::vector<u8>(m_singleFrameDecoded.begin() + offset,
                                   m_singleFrameDecoded.begin() + end);
        }

        // m_cumDecoded[i] = bytes before frame i, so frame containing
        // `offset` is the one whose cumDecoded[i+1] > offset.
        auto upper = std::upper_bound(m_cumDecoded.begin(),
                                      m_cumDecoded.end(), uint64_t(offset));
        if (upper == m_cumDecoded.begin()) return {};
        size_t const firstFrame = size_t(std::distance(m_cumDecoded.begin(), upper) - 1);

        std::vector<u8> result;
        result.reserve(end - offset);

        for (size_t fi = firstFrame; fi < m_layout.frames.size(); ++fi) {
            u64 const frameStart = m_cumDecoded[fi];
            if (frameStart >= end) break;

            auto frameData = decodeFrameCached(u32(fi));
            if (frameData.empty()) return {};

            size_t const copyFrom = (offset > frameStart) ? size_t(offset - frameStart) : 0;
            size_t const copyTo = std::min(size_t(end - frameStart), frameData.size());
            if (copyTo > copyFrom) {
                result.insert(result.end(),
                              frameData.begin() + copyFrom,
                              frameData.begin() + copyTo);
            }
        }
        return result;
    }

private:
    static constexpr size_t kMaxCachedFrames = 32;

    std::vector<u8> decodeFrameCached(u32 frameIdx) {
        {
            std::lock_guard<std::mutex> const lk(m_cacheMutex);
            auto it = m_frameCache.find(frameIdx);
            if (it != m_frameCache.end()) {
                // Touch in LRU.
                m_lruOrder.splice(m_lruOrder.begin(), m_lruOrder,
                                  m_lruIters[frameIdx]);
                return it->second;
            }
        }

        // Range-fetch and decode this frame outside the lock.
        u64 const frameByteOffset = m_baseOffset + m_layout.offsets[frameIdx];
        u32 const compSize = m_layout.frames[frameIdx].compressedSize;
        auto raw = m_fetcher->fetchRange(m_archiveKey, frameByteOffset, compSize);
        if (!raw) return {};

        BlteFrameLayout sl;
        sl.frames.push_back(m_layout.frames[frameIdx]);
        sl.offsets.push_back(0);
        sl.valid = true;

        auto decoded = blteDecodeFrame(*raw, sl, 0, m_keys);
        if (!decoded.success) return {};

        std::lock_guard<std::mutex> const lk(m_cacheMutex);
        // Another thread may have inserted while we were decoding.
        auto it = m_frameCache.find(frameIdx);
        if (it != m_frameCache.end()) {
            m_lruOrder.splice(m_lruOrder.begin(), m_lruOrder,
                              m_lruIters[frameIdx]);
            return it->second;
        }

        m_frameCache[frameIdx] = decoded.data;
        m_lruOrder.push_front(frameIdx);
        m_lruIters[frameIdx] = m_lruOrder.begin();

        while (m_lruOrder.size() > kMaxCachedFrames) {
            u32 const victim = m_lruOrder.back();
            m_lruOrder.pop_back();
            m_lruIters.erase(victim);
            m_frameCache.erase(victim);
        }
        return decoded.data;
    }

    CdnFetcher* m_fetcher;
    std::string m_archiveKey;
    u64 m_baseOffset;
    u32 m_encodedSize;
    const KeyRing* m_keys;

    bool m_layoutResolved = false;
    bool m_isSingleFrame = false;
    BlteFrameLayout m_layout;
    std::vector<u64> m_cumDecoded;     // size = frameCount + 1
    size_t m_totalDecoded = 0;
    std::vector<u8> m_singleFrameDecoded; // populated iff m_isSingleFrame

    std::mutex m_cacheMutex;
    std::unordered_map<u32, std::vector<u8>> m_frameCache;
    std::list<u32> m_lruOrder;
    std::unordered_map<u32, std::list<u32>::iterator> m_lruIters;
};

} // namespace

// pimpl so the table stays moveable despite the once_flag array + mutex.
struct EncodingTable::LazyState {
    std::unique_ptr<LazyEncodingBlob> blob;

    u8 cKeySize = 16;
    u8 eKeySize = 16;
    size_t cKeyPageSize = 4096;
    u32 cKeyPageCount = 0;
    size_t cKeyPagesStart = 0;

    std::vector<std::array<u8, 16>> pageFirstCKey;
    std::vector<std::once_flag> pageFlags;

    mutable std::once_flag eKeyIndexFlag;
    mutable bool eKeyIndexBuilt = false;
    mutable std::shared_mutex mutex;
};

EncodingTable::EncodingTable() = default;
EncodingTable::~EncodingTable() = default;
EncodingTable::EncodingTable(EncodingTable&&) noexcept = default;
EncodingTable& EncodingTable::operator=(EncodingTable&&) noexcept = default;

using storages::common::readBE16;
using storages::common::readBE32;
using storages::common::readBE40;
using storages::common::writeBE16;
using storages::common::writeBE32;
using storages::common::writeBE40;

// ---- Constants local to Encoding codec ----

/// Encoding file magic bytes: 'E', 'N'.
static constexpr u8 kEncodingMagic0 = 'E';
static constexpr u8 kEncodingMagic1 = 'N';

/// Expected encoding file version.
static constexpr u8 kEncodingVersion = 1;

/// Minimum encoding file size (header fields).
static constexpr size_t kEncodingMinHeaderSize = 22;

/// Default page size for encoding table serialization (4 KB).
static constexpr u16 kEncodingPageSizeKB = 4;
static constexpr size_t kEncodingPageSize = size_t(kEncodingPageSizeKB) * 1024;

// ============================================================================
// Helpers
// ============================================================================

static int cmpKey16(const std::array<u8, 16>& a, const std::array<u8, 16>& b) {
    return std::memcmp(a.data(), b.data(), 16);
}

// ============================================================================
// Header parsing — shared by eager and lazy paths
// ============================================================================

namespace {

struct EncodingHeader {
    bool valid = false;
    u8 cKeySize = 16;
    u8 eKeySize = 16;
    size_t cKeyPageSize = 4096;
    u32 cKeyPageCount = 0;
    size_t cKeyPagesStart = 0; ///< Byte offset of first CKey page.
};

EncodingHeader parseEncodingHeader(std::span<const u8> data) {
    EncodingHeader hdr;
    if (data.size() < kEncodingMinHeaderSize) return hdr;
    if (data[0] != kEncodingMagic0 || data[1] != kEncodingMagic1) return hdr;
    if (data[2] != kEncodingVersion) return hdr;

    hdr.cKeySize = data[3];
    hdr.eKeySize = data[4];
    u16 const cKeyPageSizeKB = readBE16(data.data() + 5);
    hdr.cKeyPageCount = readBE32(data.data() + 9);
    u32 const eSpecBlockSize = readBE32(data.data() + 18);

    hdr.cKeyPageSize = size_t(cKeyPageSizeKB) * 1024;
    // CKey TOC entry: firstCKey(cKeySize) + pageMD5(16).
    hdr.cKeyPagesStart = kEncodingMinHeaderSize + eSpecBlockSize
                       + size_t(hdr.cKeyPageCount) * (size_t(hdr.cKeySize) + 16);
    hdr.valid = true;
    return hdr;
}

/// Parse one CKey page's entries and append them to `out`. Decodes only
/// the requested page — no global mutation.
void parseCKeyPageInto(std::span<const u8> data,
                        const EncodingHeader& hdr,
                        u32 pageIdx,
                        std::vector<EncodingEntry>& out) {
    size_t const pageStart = hdr.cKeyPagesStart + size_t(pageIdx) * hdr.cKeyPageSize;
    if (pageStart + 2 > data.size()) return;

    size_t pos = pageStart;
    size_t const pageEnd = std::min(pageStart + hdr.cKeyPageSize, data.size());

    while (pos + 1 + 5 + hdr.cKeySize + hdr.eKeySize <= pageEnd) {
        u8 const keyCount = data[pos];
        if (keyCount == 0) break;                      // zero-padded tail

        u64 const fileSize = readBE40(data.data() + pos + 1);
        size_t const cKeyOff = pos + 6;
        if (cKeyOff + hdr.cKeySize > pageEnd) break;

        EncodingEntry entry;
        std::memcpy(entry.cKey.data(), data.data() + cKeyOff, hdr.cKeySize);
        entry.fileSize = fileSize;

        size_t const eKeyOff = cKeyOff + hdr.cKeySize;
        if (eKeyOff + hdr.eKeySize > pageEnd) break;
        std::memcpy(entry.eKey.data(), data.data() + eKeyOff, hdr.eKeySize);

        out.push_back(entry);
        pos = eKeyOff + size_t(keyCount) * hdr.eKeySize;
    }
}

} // namespace

// ============================================================================
// EncodingTable::parse  — eager path
// ============================================================================

EncodingTable EncodingTable::parse(std::span<const u8> data,
                                   interfaces::WorkerPool* pool) {
    EncodingTable table;

    auto hdr = parseEncodingHeader(data);
    if (!hdr.valid) return table;

    // Pre-reserve based on estimated entry count (~38 bytes per entry).
    size_t const estimatedEntries = size_t(hdr.cKeyPageCount) * (hdr.cKeyPageSize / 38);
    table.m_entries.reserve(estimatedEntries);

    // --- Phase 1: Parse all pages (sequential — order matters for stable
    // indices used by m_encodingReferenced). ---
    for (u32 page = 0; page < hdr.cKeyPageCount; ++page)
        parseCKeyPageInto(data, hdr, page, table.m_entries);

    // --- Phase 2: Build hash indices ---
    size_t n = table.m_entries.size();
    table.m_cKeyIndex.reserve(n);
    table.m_eKeyIndex.reserve(n);

    if (pool) {
        utils::JobGroup jobGroup;
        jobGroup.add(2);
        interfaces::WorkerTask ckeyTask;
        ckeyTask.fn = [&]() {
            for (size_t i = 0; i < n; ++i)
                table.m_cKeyIndex.emplace(keyHash64(table.m_entries[i].cKey), i);
            jobGroup.done();
        };
        interfaces::WorkerTask ekeyTask;
        ekeyTask.fn = [&]() {
            for (size_t i = 0; i < n; ++i)
                table.m_eKeyIndex.emplace(keyHash64(table.m_entries[i].eKey), i);
            jobGroup.done();
        };
        pool->submit(ckeyTask);
        pool->submit(ekeyTask);
        jobGroup.wait();
    } else {
        for (size_t i = 0; i < n; ++i) {
            table.m_cKeyIndex.emplace(keyHash64(table.m_entries[i].cKey), i);
            table.m_eKeyIndex.emplace(keyHash64(table.m_entries[i].eKey), i);
        }
    }

    return table;
}

// ============================================================================
// EncodingTable::openLazy — lazy path
// ============================================================================

bool EncodingTable::initLazyFromBlob(EncodingTable::LazyState& state,
                                      std::unique_ptr<EncodingTable::LazyEncodingBlob> blob) {
    if (!blob->resolveLayout()) return false;

    auto headerBytes = blob->getRange(0, kEncodingMinHeaderSize);
    if (headerBytes.size() < kEncodingMinHeaderSize) return false;

    auto hdr = parseEncodingHeader(headerBytes);
    if (!hdr.valid) return false;

    const size_t tocStart = kEncodingMinHeaderSize + readBE32(headerBytes.data() + 18);
    const size_t tocEntrySize = size_t(hdr.cKeySize) + 16;
    const size_t tocBytes = size_t(hdr.cKeyPageCount) * tocEntrySize;

    auto tocData = blob->getRange(tocStart, tocBytes);
    if (tocData.size() < tocBytes) return false;

    state.cKeySize = hdr.cKeySize;
    state.eKeySize = hdr.eKeySize;
    state.cKeyPageSize = hdr.cKeyPageSize;
    state.cKeyPageCount = hdr.cKeyPageCount;
    state.cKeyPagesStart = hdr.cKeyPagesStart;
    state.pageFlags = std::vector<std::once_flag>(hdr.cKeyPageCount);
    state.pageFirstCKey.resize(hdr.cKeyPageCount);
    for (u32 p = 0; p < hdr.cKeyPageCount; ++p) {
        std::memcpy(state.pageFirstCKey[p].data(),
                    tocData.data() + size_t(p) * tocEntrySize,
                    std::min<size_t>(hdr.cKeySize, 16));
    }

    state.blob = std::move(blob);
    return true;
}

EncodingTable EncodingTable::openLazy(std::span<const u8> data,
                                      interfaces::WorkerPool* /*pool*/) {
    EncodingTable table;

    auto state = std::make_unique<LazyState>();
    if (!initLazyFromBlob(*state,
        std::make_unique<InMemoryEncodingBlob>(
            std::vector<u8>(data.begin(), data.end()))))
        return table;

    // Reserve so lazy push_backs never reallocate — keeps returned
    // Entry* stable across concurrent page faults.
    size_t const estimatedEntries = size_t(state->cKeyPageCount) * (state->cKeyPageSize / 38);
    table.m_entries.reserve(estimatedEntries);
    table.m_cKeyIndex.reserve(estimatedEntries);
    table.m_lazy = std::move(state);
    return table;
}

EncodingTable EncodingTable::openLazyOnline(CdnFetcher* fetcher,
                                            const std::string& archiveKeyHex,
                                            u64 archiveOffset,
                                            u32 encodedSize,
                                            const KeyRing* keys,
                                            interfaces::WorkerPool* /*pool*/) {
    EncodingTable table;
    if (!fetcher) return table;

    auto state = std::make_unique<LazyState>();
    if (!initLazyFromBlob(*state,
        std::make_unique<NetworkEncodingBlob>(
            fetcher, archiveKeyHex, archiveOffset, encodedSize, keys)))
        return table;

    size_t const estimatedEntries = size_t(state->cKeyPageCount) * (state->cKeyPageSize / 38);
    table.m_entries.reserve(estimatedEntries);
    table.m_cKeyIndex.reserve(estimatedEntries);
    table.m_lazy = std::move(state);
    return table;
}

void EncodingTable::parseCKeyPage(u32 pageIdx) const {
    if (!m_lazy) return;
    if (pageIdx >= m_lazy->cKeyPageCount) return;

    std::call_once(m_lazy->pageFlags[pageIdx], [&]() {
        // parseCKeyPageInto receives the page's bytes starting at offset 0,
        // so cKeyPagesStart=0 and pageIdx=0 in the local header.
        EncodingHeader hdr;
        hdr.cKeySize = m_lazy->cKeySize;
        hdr.eKeySize = m_lazy->eKeySize;
        hdr.cKeyPageSize = m_lazy->cKeyPageSize;
        hdr.cKeyPagesStart = 0;
        hdr.cKeyPageCount = m_lazy->cKeyPageCount;

        const size_t pageByteOffset = m_lazy->cKeyPagesStart +
                                      size_t(pageIdx) * m_lazy->cKeyPageSize;
        auto pageBytes = m_lazy->blob->getRange(pageByteOffset, m_lazy->cKeyPageSize);
        if (pageBytes.empty()) return;

        std::vector<EncodingEntry> pageEntries;
        parseCKeyPageInto(pageBytes, hdr, /*pageIdx=*/0, pageEntries);
        if (pageEntries.empty()) return;

        std::unique_lock<std::shared_mutex> const lk(m_lazy->mutex);
        for (auto& e : pageEntries) {
            size_t const idx = m_entries.size();
            m_entries.push_back(std::move(e));
            m_cKeyIndex.emplace(keyHash64(m_entries[idx].cKey), idx);
        }
    });
}

void EncodingTable::ensureFullyParsed() const {
    if (!m_lazy) return;

    const u32 N = m_lazy->cKeyPageCount;
    for (u32 p = 0; p < N; ++p)
        parseCKeyPage(p);

    // page-by-page parsing only fills m_cKeyIndex; m_eKeyIndex needs a
    // second pass once every entry is known.
    std::call_once(m_lazy->eKeyIndexFlag, [this]() {
        std::unique_lock<std::shared_mutex> const lk(m_lazy->mutex);
        m_eKeyIndex.reserve(m_entries.size());
        for (size_t i = 0; i < m_entries.size(); ++i)
            m_eKeyIndex.emplace(keyHash64(m_entries[i].eKey), i);
        m_lazy->eKeyIndexBuilt = true;
    });
}

// ============================================================================
// findByCKey
// ============================================================================

const EncodingEntry* EncodingTable::findByCKey(std::span<const u8, 16> cKey,
                                                size_t matchBytes) const {
    if (matchBytes == 0) matchBytes = 16;
    if (matchBytes > 16) matchBytes = 16;
    u64 h = keyHash64(cKey.data());

    auto checkHit = [&]() -> const EncodingEntry* {
        auto* idxPtr = m_cKeyIndex.find(h);
        if (!idxPtr) return nullptr;
        auto& e = m_entries[*idxPtr];
        return (std::memcmp(e.cKey.data(), cKey.data(), matchBytes) == 0) ? &e : nullptr;
    };

    if (!m_lazy) return checkHit();

    {
        std::shared_lock<std::shared_mutex> const lk(m_lazy->mutex);
        if (auto* hit = checkHit()) return hit;
    }

    // pageFirstCKey[i] is the first CKey on page i; target lives on
    // (upper_bound - 1).
    if (m_lazy->pageFirstCKey.empty()) return nullptr;
    std::array<u8, 16> target{};
    std::memcpy(target.data(), cKey.data(), 16);
    auto it = std::upper_bound(m_lazy->pageFirstCKey.begin(),
                               m_lazy->pageFirstCKey.end(),
                               target,
                               [](const std::array<u8, 16>& a,
                                  const std::array<u8, 16>& b) {
                                   return std::memcmp(a.data(), b.data(), 16) < 0;
                               });
    if (it == m_lazy->pageFirstCKey.begin()) return nullptr;
    u32 const pageIdx = u32(std::distance(m_lazy->pageFirstCKey.begin(), it) - 1);

    parseCKeyPage(pageIdx);

    std::shared_lock<std::shared_mutex> const lk(m_lazy->mutex);
    return checkHit();
}

// ============================================================================
// findByEKey
// ============================================================================

const EncodingEntry* EncodingTable::findByEKey(std::span<const u8, 16> eKey,
                                                size_t matchBytes) const {
    if (matchBytes == 0) matchBytes = 16;
    if (matchBytes > 16) matchBytes = 16;

    // EKey lookup isn't page-localised — the index needs every page parsed.
    if (m_lazy) ensureFullyParsed();

    auto* idxPtr = m_eKeyIndex.find(keyHash64(eKey.data()));
    if (!idxPtr)
        return nullptr;

    auto& e = m_entries[*idxPtr];
    if (std::memcmp(e.eKey.data(), eKey.data(), matchBytes) == 0)
        return &e;

    return nullptr;
}

// ============================================================================
// insert
// ============================================================================

bool EncodingTable::isValid() const {
    if (m_lazy) return m_lazy->cKeyPageCount > 0;
    return !m_entries.empty();
}

size_t EncodingTable::entryCount() const {
    // Lazy mode returns currently-parsed count; call ensureFullyParsed()
    // first if you need the total.
    if (m_lazy) {
        std::shared_lock<std::shared_mutex> const lk(m_lazy->mutex);
        return m_entries.size();
    }
    return m_entries.size();
}

const std::vector<EncodingEntry>& EncodingTable::entries() const {
    if (m_lazy) ensureFullyParsed();
    return m_entries;
}

void EncodingTable::insert(const EncodingEntry& entry) {
    size_t const idx = m_entries.size();
    m_entries.push_back(entry);

    m_cKeyIndex.emplace(keyHash64(entry.cKey), idx);
    m_eKeyIndex.emplace(keyHash64(entry.eKey), idx);
}

// ============================================================================
// serialize
// ============================================================================

std::vector<u8> EncodingTable::serialize() const {
    // Create a sorted copy for serialization (CKey pages require CKey order).
    auto sorted = m_entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const EncodingEntry& a, const EncodingEntry& b) {
                  return cmpKey16(a.cKey, b.cKey) < 0;
              });

    constexpr u16 kPageSizeKB = kEncodingPageSizeKB;
    constexpr size_t kPageSize = kEncodingPageSize;

    // --- Build ESpec string pool ---
    // Unique null-terminated strings. espec_index is a 0-based ordinal
    // counting strings (not byte offset) per wowdev.wiki convention.
    std::vector<u8> eSpecPool;
    // Map: eSpec string → ordinal index.
    std::vector<std::pair<std::string, u32>> eSpecMap;
    auto getESpecIndex = [&](const std::string& spec) -> u32 {
        for (auto& [s, idx] : eSpecMap) {
            if (s == spec) return idx;
        }
        u32 const idx = static_cast<u32>(eSpecMap.size());
        eSpecMap.push_back({spec, idx});
        eSpecPool.insert(eSpecPool.end(), spec.begin(), spec.end());
        eSpecPool.push_back(0); // null terminator
        return idx;
    };

    // Pre-populate indices for all entries.
    std::vector<u32> sortedESpecIndices;
    sortedESpecIndices.reserve(sorted.size());
    for (auto& entry : sorted) {
        sortedESpecIndices.push_back(
            entry.eSpec.empty() ? getESpecIndex("n") : getESpecIndex(entry.eSpec));
    }
    u32 const eSpecBlockSize = static_cast<u32>(eSpecPool.size());

    // --- Build CKey pages (CEKeyPageTable) ---
    struct PageData {
        std::vector<u8> data;
        std::array<u8, 16> firstKey{};
    };
    std::vector<PageData> cKeyPages;
    {
        PageData current;
        for (auto& entry : sorted) {
            // Entry: keyCount(1) + fileSize(5) + CKey(16) + EKey(16) = 38 bytes.
            constexpr size_t kEntrySize = 1 + 5 + kCKeySize + kEKeySize;

            if (current.data.size() + kEntrySize > kPageSize) {
                current.data.resize(kPageSize, 0);
                cKeyPages.push_back(std::move(current));
                current = PageData{};
            }

            if (current.data.empty())
                current.firstKey = entry.cKey;

            current.data.push_back(1); // keyCount = 1
            size_t const off = current.data.size();
            current.data.resize(off + 5);
            writeBE40(current.data.data() + off, entry.fileSize);
            current.data.insert(current.data.end(), entry.cKey.begin(), entry.cKey.end());
            current.data.insert(current.data.end(), entry.eKey.begin(), entry.eKey.end());
        }
        if (!current.data.empty()) {
            current.data.resize(kPageSize, 0);
            cKeyPages.push_back(std::move(current));
        }
    }

    // --- Build EKey pages (EKeySpecPageTable) ---
    // Entries sorted by EKey: eKey(16) + espec_index(4 BE) + file_size(5 BE) = 25 bytes.
    // Create EKey-sorted index.
    std::vector<size_t> eKeySortOrder(sorted.size());
    std::iota(eKeySortOrder.begin(), eKeySortOrder.end(), 0);
    std::sort(eKeySortOrder.begin(), eKeySortOrder.end(),
              [&](size_t a, size_t b) {
                  return cmpKey16(sorted[a].eKey, sorted[b].eKey) < 0;
              });

    std::vector<PageData> eKeyPages;
    {
        PageData current;
        for (size_t const idx : eKeySortOrder) {
            auto& entry = sorted[idx];
            u32 const specIdx = sortedESpecIndices[idx];

            // EKey page entry: eKey(16) + espec_index(4 BE) + file_size(5 BE) = 25 bytes.
            constexpr size_t kEKeyEntrySize = kEKeySize + 4 + 5;

            if (current.data.size() + kEKeyEntrySize > kPageSize) {
                current.data.resize(kPageSize, 0);
                eKeyPages.push_back(std::move(current));
                current = PageData{};
            }

            if (current.data.empty())
                current.firstKey = entry.eKey;

            current.data.insert(current.data.end(), entry.eKey.begin(), entry.eKey.end());
            size_t off = current.data.size();
            current.data.resize(off + 4);
            writeBE32(current.data.data() + off, specIdx);
            off = current.data.size();
            current.data.resize(off + 5);
            writeBE40(current.data.data() + off, entry.fileSize);
        }
        if (!current.data.empty()) {
            current.data.resize(kPageSize, 0);
            eKeyPages.push_back(std::move(current));
        }
    }

    u32 const cKeyPageCount = static_cast<u32>(cKeyPages.size());
    u32 const eKeyPageCount = static_cast<u32>(eKeyPages.size());

    // --- Calculate total size ---
    size_t const headerSize = kEncodingMinHeaderSize;
    size_t const cKeyPageTableSize = cKeyPageCount * (kCKeySize + 16); // firstCKey + MD5
    size_t const eKeyPageTableSize = eKeyPageCount * (kEKeySize + 16); // firstEKey + MD5
    size_t const cKeyPagesSize = cKeyPageCount * kPageSize;
    size_t const eKeyPagesSize = eKeyPageCount * kPageSize;

    size_t const totalSize = headerSize + eSpecBlockSize + cKeyPageTableSize + cKeyPagesSize +
                             eKeyPageTableSize + eKeyPagesSize;

    std::vector<u8> output(totalSize, 0);
    size_t pos = 0;

    // --- Header (22 bytes) ---
    output[pos++] = kEncodingMagic0;
    output[pos++] = kEncodingMagic1;
    output[pos++] = kEncodingVersion;
    output[pos++] = kCKeySize;
    output[pos++] = kEKeySize;
    writeBE16(output.data() + pos, kPageSizeKB); pos += 2;
    writeBE16(output.data() + pos, kPageSizeKB); pos += 2;
    writeBE32(output.data() + pos, cKeyPageCount); pos += 4;
    writeBE32(output.data() + pos, eKeyPageCount); pos += 4;
    output[pos++] = 0; // padding
    writeBE32(output.data() + pos, eSpecBlockSize); pos += 4;

    // --- ESpec string pool ---
    std::memcpy(output.data() + pos, eSpecPool.data(), eSpecPool.size());
    pos += eSpecBlockSize;

    // --- CKey page table: firstCKey(16) + MD5(page)(16) ---
    for (auto& page : cKeyPages) {
        std::memcpy(output.data() + pos, page.firstKey.data(), kCKeySize);
        pos += kCKeySize;
        auto hash = common::md5Hash(std::span(page.data.data(), page.data.size()));
        std::memcpy(output.data() + pos, hash.data(), 16);
        pos += 16;
    }

    // --- CKey pages ---
    for (auto& page : cKeyPages) {
        std::memcpy(output.data() + pos, page.data.data(), page.data.size());
        pos += kPageSize;
    }

    // --- EKey page table: firstEKey(16) + MD5(page)(16) ---
    for (auto& page : eKeyPages) {
        std::memcpy(output.data() + pos, page.firstKey.data(), kEKeySize);
        pos += kEKeySize;
        auto hash = common::md5Hash(std::span(page.data.data(), page.data.size()));
        std::memcpy(output.data() + pos, hash.data(), 16);
        pos += 16;
    }

    // --- EKey pages ---
    for (auto& page : eKeyPages) {
        std::memcpy(output.data() + pos, page.data.data(), page.data.size());
        pos += kPageSize;
    }

    return output;
}

} // namespace whiteout::storages::casc
