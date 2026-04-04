// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file storage_writable.cpp
/// @brief StorageWritable: write overlay + save-to-disk operations.

#include "storage_impl.h"
#include "storage_backend_impl.h"
#include "constants.h"
#include "../../common/md5.h"

#include <whiteout/storages/casc/storage_writable.h>

#include <filesystem>

namespace whiteout::storages::casc {

// ============================================================================
// StorageWritable construction
// ============================================================================

StorageWritable::StorageWritable() = default;
StorageWritable::~StorageWritable() = default;
StorageWritable::StorageWritable(StorageWritable&&) noexcept = default;
StorageWritable& StorageWritable::operator=(StorageWritable&&) noexcept = default;

// ============================================================================
// StorageWritable::open (local, writable)
// ============================================================================

std::optional<StorageWritable> StorageWritable::open(const OpenOptions& opts) {
    // Open as a normal local storage first.
    auto base = Storage::open(opts);
    if (!base) return std::nullopt;

    // Enable write overlay.
    base->m_impl->writeOverlay = std::make_unique<WriteOverlay>();
    if (base->m_impl->root)
        base->m_impl->writeOverlay->requestedRootFormat = base->m_impl->root->format();

    StorageWritable writable;
    writable.m_impl = std::move(base->m_impl);
    return writable;
}

std::optional<StorageWritable> StorageWritable::open(const std::string& basePath) {
    OpenOptions opts;
    opts.path = basePath;
    return open(opts);
}

std::optional<StorageWritable> StorageWritable::open(const std::string& basePath,
                                                     std::string* /*errorString*/,
                                                     interfaces::WorkerPool* pool) {
    OpenOptions opts;
    opts.path = basePath;
    opts.pool = pool;
    return open(opts);
}

// ============================================================================
// StorageWritable::create (new empty)
// ============================================================================

StorageWritable StorageWritable::create(CreateOptions opts,
                                        interfaces::WorkerPool* pool) {
    auto implPtr = std::make_unique<Impl>();
    auto& impl = *implPtr;
    impl.pool = pool;
    impl.buildConfig.buildProduct = opts.product;
    impl.buildConfig.buildName = opts.version;

    // Create local state for eventual save().
    impl.localState = std::make_unique<LocalState>();

    // Create write overlay so writeFile/deleteFile/save work.
    impl.writeOverlay = std::make_unique<WriteOverlay>();
    impl.writeOverlay->requestedRootFormat = opts.rootFormat;

    impl.isValid = true;
    StorageWritable writable;
    writable.m_impl = std::move(implPtr);
    return writable;
}

// ============================================================================
// Write operations
// ============================================================================

bool StorageWritable::writeFile(const std::string& path,
                                const std::vector<u8>& data,
                                WriteOptions opts) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);
    if (!m_impl->writeOverlay) { s_lastError = kNotValid; return false; }

    auto normalized = storages::common::normalizeCascPath(path);
    OverlayKey key{normalized, std::nullopt};

    m_impl->writeOverlay->pendingDeletes.erase(key);
    m_impl->writeOverlay->pendingWrites[key] = OverlayEntry{data, opts};
    return true;
}

bool StorageWritable::writeFile(i32 fileId, const std::vector<u8>& data,
                                WriteOptions opts, FileIdHint hint) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);
    if (!m_impl->writeOverlay) { s_lastError = kNotValid; return false; }

    OverlayKey key{"", static_cast<u32>(fileId), hint};
    m_impl->writeOverlay->pendingDeletes.erase(key);
    m_impl->writeOverlay->pendingWrites[key] = OverlayEntry{data, opts};
    return true;
}

bool StorageWritable::deleteFile(const std::string& path) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);
    if (!m_impl->writeOverlay) { s_lastError = kNotValid; return false; }

    auto normalized = storages::common::normalizeCascPath(path);
    OverlayKey key{normalized, std::nullopt};

    m_impl->writeOverlay->pendingWrites.erase(key);

    bool existsInSource = m_impl->root && m_impl->root->hasPath(normalized);
    if (!existsInSource) {
        s_lastError = kFileNotFound;
        return false;
    }

    m_impl->writeOverlay->pendingDeletes.insert(key);
    return true;
}

bool StorageWritable::deleteFile(i32 fileId, FileIdHint hint) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);
    if (!m_impl->writeOverlay) { s_lastError = kNotValid; return false; }

    OverlayKey key{"", static_cast<u32>(fileId), hint};
    m_impl->writeOverlay->pendingWrites.erase(key);

    bool existsInSource = m_impl->root &&
                          m_impl->root->hasFileDataId(static_cast<u32>(fileId), hint);
    if (!existsInSource) {
        s_lastError = kFileNotFound;
        return false;
    }

    m_impl->writeOverlay->pendingDeletes.insert(key);
    return true;
}

// ============================================================================
// reserveFileId
// ============================================================================

/// Initialise the overlay's nextFileDataId lazily from the current root.
static void ensureNextFileDataId(WriteOverlay& overlay, const RootManifest* root) {
    if (overlay.nextFileDataId != 0)
        return;

    u32 maxId = 0;
    if (root) {
        root->enumerate([&](const RootEntry& re) -> bool {
            if (re.fileDataId != kInvalidFileDataId && re.fileDataId > maxId)
                maxId = re.fileDataId;
            return true;
        });
    }
    for (auto& [id, _] : overlay.reservedFileIds) {
        if (id > maxId) maxId = id;
    }
    overlay.nextFileDataId = maxId + 1;
}

/// Initialise the overlay's CoreTOC lazily, reading from the existing archive.
static void ensureCoreToc(WriteOverlay& overlay, const RootManifest* root,
                          Storage::Impl* impl) {
    if (overlay.coreToc)
        return;

    overlay.coreToc = std::make_unique<sno::CoreToc>();

    // Try to read CoreTOC.dat from the existing archive.
    if (root && impl) {
        // D4: "base:coretoc.dat"
        auto d4Results = root->findByNormalizedPath("base:coretoc.dat");
        if (!d4Results.empty()) {
            OverlayKey key{"base:coretoc.dat", std::nullopt};
            if (auto data = impl->readFileResolved(key, d4Results, 0))
                overlay.coreToc->parse(*data);
            return;
        }

        // D3: normalised as "base/coretoc.dat"
        auto d3Results = root->findByNormalizedPath("base/coretoc.dat");
        if (!d3Results.empty()) {
            OverlayKey key{"base/coretoc.dat", std::nullopt};
            if (auto data = impl->readFileResolved(key, d3Results, 0))
                overlay.coreToc->parse(*data);
        }
    }
}

std::optional<u32> StorageWritable::reserveFileId(const std::string& name) {
    if (!m_impl) { s_lastError = kNotValid; return std::nullopt; }
    std::unique_lock lock(m_impl->mutex);
    if (!m_impl->writeOverlay) { s_lastError = kNotValid; return std::nullopt; }

    auto& overlay = *m_impl->writeOverlay;

    // Determine the root format.
    RootFormat rootFmt = overlay.requestedRootFormat;
    if (m_impl->root)
        rootFmt = m_impl->root->format();

    // Check for duplicate name in existing reservations.
    for (auto& [_, res] : overlay.reservedFileIds) {
        if (res.name == name)
            return std::nullopt;
    }

    ensureNextFileDataId(overlay, m_impl->root.get());

    if (rootFmt == RootFormat::Wow || rootFmt == RootFormat::WowTvfs) {
        // ── WoW: name is a full CASC path ──────────────────────────
        auto normalized = storages::common::normalizeCascPath(name);

        // Check existence in root.
        if (m_impl->root && m_impl->root->hasPath(normalized))
            return std::nullopt;

        const u32 id = overlay.nextFileDataId++;
        FileIdReservation res;
        res.name = name;
        res.enrichedPath = name; // Keep the original path.
        overlay.reservedFileIds[id] = std::move(res);
        return id;
    }

    // ── D3 / D4 / TVFS: name is "asset_name.ext" ──────────────────

    // Parse extension.
    auto dotPos = name.rfind('.');
    if (dotPos == std::string::npos || dotPos == 0)
        return std::nullopt;

    std::string snoName = name.substr(0, dotPos);
    std::string ext = name.substr(dotPos + 1);

    auto group = sno::snoGroupFromExtension(ext.c_str());
    if (group == sno::SnoGroup::None)
        return std::nullopt;

    // Ensure CoreTOC is loaded.
    ensureCoreToc(overlay, m_impl->root.get(), m_impl.get());

    // Check if name already exists in the CoreTOC.
    if (overlay.coreToc->findByName(group, snoName))
        return std::nullopt;

    // Allocate snoId.
    i32 maxSno = overlay.coreToc->maxSnoId();
    if (static_cast<u32>(maxSno) >= overlay.nextFileDataId)
        overlay.nextFileDataId = static_cast<u32>(maxSno) + 1;

    const u32 id = overlay.nextFileDataId++;
    const i32 snoId = static_cast<i32>(id);

    // Add to CoreTOC.
    sno::TocEntry tocEntry;
    tocEntry.group = group;
    tocEntry.snoId = snoId;
    tocEntry.name = snoName;
    overlay.coreToc->addEntry(tocEntry);

    // Build the enriched path depending on format.
    FileIdReservation res;
    res.name = name;
    res.snoGroup = group;
    res.snoName = snoName;
    res.snoId = snoId;

    if (rootFmt == RootFormat::Diablo3) {
        const char* dirName = sno::snoGroupDirD3(group);
        res.enrichedPath = std::string("Base\\") + (dirName ? dirName : "Unknown")
                           + "\\" + snoName + "." + ext;
    } else {
        // D4 / TVFS — use the D4 enriched path format.
        const char* dirName = sno::snoGroupDir(group);
        res.enrichedPath = std::string("base:child\\") + (dirName ? dirName : "Unknown")
                           + "\\" + snoName + "." + ext;
    }

    overlay.reservedFileIds[id] = std::move(res);
    return id;
}

// ============================================================================
// Save
// ============================================================================

bool StorageWritable::save() {
    if (!m_impl || !m_impl->isValid) { s_lastError = kNotValid; return false; }
    if (!m_impl->localState) { s_lastError = kNotValid; return false; }
    return save(m_impl->localState->basePath);
}

bool StorageWritable::save(const std::string& outputPath) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);

    if (!m_impl->writeOverlay) { s_lastError = kNotValid; return false; }

    namespace fs = std::filesystem;

    // Build merged file list.
    std::vector<WriteEntry> entries;

    struct PendingRead {
        size_t entryIndex;
        u32 archiveIndex;
        u32 archiveOffset;
        u32 encodedSize;
        bool directBLTE;
    };
    std::vector<PendingRead> pendingReads;

    if (m_impl->root) {
        m_impl->root->enumerate([&](const RootEntry& re) -> bool {
            OverlayKey pathKey{storages::common::normalizeCascPath(re.path), std::nullopt};
            if (m_impl->writeOverlay->pendingDeletes.count(pathKey))
                return true;

            if (re.fileDataId != kInvalidFileDataId) {
                OverlayKey idKey{"", re.fileDataId};
                if (m_impl->writeOverlay->pendingDeletes.count(idKey))
                    return true;
            }

            if (!re.path.empty() && m_impl->writeOverlay->pendingWrites.count(pathKey))
                return true;

            if (re.fileDataId != kInvalidFileDataId) {
                OverlayKey idKey{"", re.fileDataId};
                if (m_impl->writeOverlay->pendingWrites.count(idKey))
                    return true;
            }

            WriteEntry we;
            we.path = re.path;
            we.cKey = re.cKey;
            we.fileDataId = re.fileDataId;
            we.localeFlags = re.localeFlags;
            we.contentFlags = re.contentFlags;

            auto encEntry = m_impl->resolveEncoding(re);
            if (encEntry) {
                we.eKey = encEntry->eKey;
                we.fileSize = encEntry->fileSize;

                if (m_impl->localState) {
                    auto idxEntry = m_impl->localState->indexTable.find(
                        eKeyTrunc(encEntry->eKey));
                    if (idxEntry) {
                        pendingReads.push_back({entries.size(),
                            idxEntry->archiveIndex, idxEntry->archiveOffset,
                            idxEntry->encodedSize, idxEntry->directBLTE});
                    }
                }
            }

            entries.push_back(std::move(we));
            return true;
        });

        // Phase 2: Read pre-encoded BLTE blobs in parallel.
        if (m_impl->localState) {
            auto& localDS = *m_impl->localState->dataSource;
            if (m_impl->pool && pendingReads.size() > 1) {
                utils::JobGroup jobGroup;
                jobGroup.add(pendingReads.size());
                for (auto& pr : pendingReads) {
                    interfaces::WorkerTask task;
                    task.fn = [&, idx = pr.entryIndex, ai = pr.archiveIndex,
                               ao = pr.archiveOffset, es = pr.encodedSize,
                               direct = pr.directBLTE]() {
                        auto span = localDS.readRawBlteDirect(ai, ao, es);
                        if (!direct) {
                            // Use header-aware read.
                            IndexEntry ie{};
                            ie.archiveIndex = ai;
                            ie.archiveOffset = ao;
                            ie.encodedSize = es;
                            ie.directBLTE = direct;
                            span = localDS.readBlteFromIndex(ie);
                        }
                        entries[idx].encodedBlob.assign(span.begin(), span.end());
                        entries[idx].hasPreEncoded = true;
                        jobGroup.done();
                    };
                    m_impl->pool->submit(task);
                }
                jobGroup.wait();
            } else {
                for (auto& pr : pendingReads) {
                    auto span = pr.directBLTE
                        ? localDS.readRawBlteDirect(pr.archiveIndex, pr.archiveOffset, pr.encodedSize)
                        : localDS.readBlteFromIndex(IndexEntry{
                              .archiveIndex = pr.archiveIndex,
                              .archiveOffset = pr.archiveOffset,
                              .encodedSize = pr.encodedSize,
                              .directBLTE = pr.directBLTE,
                          });
                    entries[pr.entryIndex].encodedBlob.assign(span.begin(), span.end());
                    entries[pr.entryIndex].hasPreEncoded = true;
                }
            }
        }
    }

    // 2. Add overlay entries.
    for (auto& [key, overlay] : m_impl->writeOverlay->pendingWrites) {
        WriteEntry we;
        we.path = key.path;
        if (key.fileDataId) {
            we.fileDataId = *key.fileDataId;
            // Enrich path from reservation if the overlay key has no path.
            if (we.path.empty()) {
                auto resIt = m_impl->writeOverlay->reservedFileIds.find(*key.fileDataId);
                if (resIt != m_impl->writeOverlay->reservedFileIds.end()) {
                    auto& res = resIt->second;
                    // For D4/TVFS with a hint, build a sub-folder specific path.
                    if (key.hint != FileIdHint::None &&
                        res.snoGroup != sno::SnoGroup::None) {
                        const char* subfolder = "child";
                        switch (key.hint) {
                            case FileIdHint::Meta:    subfolder = "meta";    break;
                            case FileIdHint::Payload: subfolder = "payload"; break;
                            case FileIdHint::Paylow:  subfolder = "paylow";  break;
                            case FileIdHint::Paymed:  subfolder = "paymed";  break;
                            default: break;
                        }
                        const char* dirName = sno::snoGroupDir(res.snoGroup);
                        const char* ext = sno::snoGroupExtension(res.snoGroup);
                        we.path = std::string("base:") + subfolder + "\\"
                                  + (dirName ? dirName : "Unknown") + "\\"
                                  + res.snoName + "." + (ext ? ext : "dat");
                    } else {
                        we.path = res.enrichedPath;
                    }
                }
            }
        }
        we.rawData = overlay.rawData;
        we.localeFlags = overlay.writeOptions.localeFlags;
        we.contentFlags = overlay.writeOptions.contentFlags;
        we.compress = overlay.writeOptions.compress;
        we.hasPreEncoded = false;
        entries.push_back(std::move(we));
    }

    // 3. Determine root format.
    RootFormat rootFmt = m_impl->writeOverlay->requestedRootFormat;
    if (m_impl->root)
        rootFmt = m_impl->root->format();

    // 3a. Serialize CoreTOC for D3/D4 formats and add as a file entry.
    if (m_impl->writeOverlay->coreToc &&
        !m_impl->writeOverlay->reservedFileIds.empty()) {

        std::vector<u8> tocData;
        std::string tocPath;
        if (rootFmt == RootFormat::Diablo3) {
            tocData = m_impl->writeOverlay->coreToc->serializeD3Legacy();
            tocPath = "Base\\CoreTOC.dat";
        } else {
            tocData = m_impl->writeOverlay->coreToc->serializeD4New();
            tocPath = "base:coretoc.dat";
        }

        if (!tocData.empty()) {
            // Remove any existing CoreTOC entry to replace it with ours.
            auto normalizedToc = storages::common::normalizeCascPath(tocPath);
            std::erase_if(entries, [&](const WriteEntry& e) {
                return storages::common::normalizeCascPath(e.path) == normalizedToc;
            });

            WriteEntry tocEntry;
            tocEntry.path = tocPath;
            tocEntry.rawData = std::move(tocData);
            tocEntry.compress = true;
            tocEntry.hasPreEncoded = false;
            entries.push_back(std::move(tocEntry));
        }
    }

    // Auto-assign fileDataIds for formats that use them.
    if (rootFmt == RootFormat::Wow || rootFmt == RootFormat::Diablo3) {
        u32 maxId = 0;
        for (auto& e : entries) {
            if (e.fileDataId != kInvalidFileDataId && e.fileDataId > maxId)
                maxId = e.fileDataId;
        }
        for (auto& e : entries) {
            if (e.fileDataId == kInvalidFileDataId) {
                e.fileDataId = ++maxId;
            }
        }
    }

    // 4. Writer options.
    WriterOptions writerOpts;
    writerOpts.product = m_impl->buildConfig.buildProduct.empty()
                             ? "custom" : m_impl->buildConfig.buildProduct;
    writerOpts.version = m_impl->buildConfig.buildName.empty()
                             ? "1.0.0" : m_impl->buildConfig.buildName;
    writerOpts.rootFormat = rootFmt;

    // 5. Write to temp dir, then rename.
    std::string tempDir = outputPath + ".tmp_save";
    std::error_code ec;
    fs::remove_all(tempDir, ec);

    bool ok = writeStorage(tempDir, entries, writerOpts, m_impl->pool);
    if (!ok) {
        fs::remove_all(tempDir, ec);
        s_lastError = kSaveFailed;
        return false;
    }

    // 6. Atomic replace.
    if (m_impl->localState) {
        m_impl->localState->dataArchives.clear();
    }
    m_impl->root.reset();
    m_impl->isValid = false;

    std::string basePath = m_impl->localState ? m_impl->localState->basePath : "";

    if (outputPath != basePath) {
        fs::rename(tempDir, outputPath, ec);
        if (ec) {
            fs::remove_all(tempDir, ec);
            s_lastError = kSaveFailed;
            return false;
        }
    } else {
        std::string oldDir = outputPath + ".old_save";
        fs::remove_all(oldDir, ec);
        fs::rename(outputPath, oldDir, ec);
        if (ec) {
            fs::remove_all(tempDir, ec);
            s_lastError = kSaveFailed;
            return false;
        }
        fs::rename(tempDir, outputPath, ec);
        if (ec) {
            fs::rename(oldDir, outputPath, ec);
            s_lastError = kSaveFailed;
            return false;
        }
        fs::remove_all(oldDir, ec);
    }

    // 7. Clear overlay and reload.
    m_impl->writeOverlay->pendingWrites.clear();
    m_impl->writeOverlay->pendingDeletes.clear();
    m_impl->writeOverlay->reservedFileIds.clear();
    m_impl->writeOverlay->coreToc.reset();
    m_impl->writeOverlay->nextFileDataId = 0;

    auto reopened = Storage::open(outputPath, m_impl->pool);
    if (reopened) {
        m_impl->buildConfig = std::move(reopened->m_impl->buildConfig);
        m_impl->cdnConfig = std::move(reopened->m_impl->cdnConfig);
        m_impl->encodingTable = std::move(reopened->m_impl->encodingTable);
        m_impl->root = std::move(reopened->m_impl->root);
        m_impl->m_encodingReferenced = std::move(reopened->m_impl->m_encodingReferenced);
        if (reopened->m_impl->localState) {
            m_impl->localState = std::move(reopened->m_impl->localState);
            m_impl->localState->dataSource = std::make_unique<LocalDataSource>(
                &m_impl->localState->indexTable,
                &m_impl->localState->dataArchives);
            m_impl->dataSource = m_impl->localState->dataSource.get();

            // Reconstruct backend for the reloaded local state.
            m_impl->backend = std::make_unique<StorageBackendImpl<LocalDataTraits, NoCachePolicy>>(
                LocalDataTraits{&m_impl->localState->indexTable, m_impl->localState->dataSource.get()},
                NoCachePolicy{},
                m_impl->encodingTable, m_impl->keyRing, m_impl->pool);
        }
        m_impl->isValid = true;
    }

    return true;
}

} // namespace whiteout::storages::casc
