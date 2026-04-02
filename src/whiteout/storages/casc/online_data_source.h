// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file online_data_source.h
/// @brief Online (CDN) data source — fetches BLTE blobs via CdnFetcher.
///
/// Internal header — not part of the public include path.
#pragma once

#include "data_source.h"
#include "cdn_fetcher.h"
#include "online_index.h"
#include "../common/hex.h"

#include <vector>

namespace whiteout::storages::casc {

/// Fetches BLTE blobs from CDN archives via CdnFetcher + OnlineIndexTable.
///
/// @warning All pointer parameters must outlive this object. In OnlineStorage::Impl,
/// this is ensured by declaring dataSource *after* fetcher/onlineIndex/cdnConfig
/// (members are destroyed in reverse declaration order).
class OnlineDataSource : public DataSource {
public:
    OnlineDataSource(CdnFetcher* fetcher,
                     const OnlineIndexTable* archiveIndex,
                     const OnlineIndexTable* looseIndex,
                     const std::vector<std::array<u8, 16>>* archiveEKeys)
        : m_fetcher(fetcher)
        , m_archiveIndex(archiveIndex)
        , m_looseIndex(looseIndex)
        , m_archiveEKeys(archiveEKeys) {}

    std::vector<u8> fetchBlte(const std::array<u8, 16>& eKey) override {
        // Loose-file fetch by EKey (full 16-byte hex).
        auto keyHex = storages::common::hexEncode16(eKey);
        auto data = m_fetcher->fetch("data", keyHex);
        return data.value_or(std::vector<u8>{});
    }

    std::vector<u8> fetchBlte(u32 archiveIndex, u64 offset, u32 encodedSize) override {
        if (!m_archiveEKeys || archiveIndex >= m_archiveEKeys->size())
            return {};
        auto archiveKeyHex = storages::common::hexEncode16((*m_archiveEKeys)[archiveIndex]);
        auto data = m_fetcher->fetchRange(archiveKeyHex, offset, encodedSize);
        return data.value_or(std::vector<u8>{});
    }

private:
    CdnFetcher* m_fetcher;
    const OnlineIndexTable* m_archiveIndex;
    const OnlineIndexTable* m_looseIndex;
    const std::vector<std::array<u8, 16>>* m_archiveEKeys;
};

} // namespace whiteout::storages::casc
