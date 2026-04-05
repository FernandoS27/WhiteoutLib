// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/utils/casc_file_system.h>
#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>
#include "../storages/common/string_utils.h"

#include <algorithm>
#include <unordered_set>

namespace whiteout::utils {

using storages::casc::Storage;
using storages::casc::StorageWritable;
using storages::casc::EnumerateEntry;

// ============================================================================
// CascFileSystem (numeric file-data-ID based)
// ============================================================================

struct CascFileSystem::Impl {
    explicit Impl(Storage& storage)
        : storage(storage)
        , writable(storage.isWritable() ? static_cast<StorageWritable*>(&storage) : nullptr) {}

    Storage& storage;
    StorageWritable* writable;
};

CascFileSystem::CascFileSystem(storages::casc::Storage& storage)
    : m_impl(std::make_unique<Impl>(storage)) {}

CascFileSystem::~CascFileSystem() = default;
CascFileSystem::CascFileSystem(CascFileSystem&&) noexcept = default;
CascFileSystem& CascFileSystem::operator=(CascFileSystem&&) noexcept = default;

std::vector<u8> CascFileSystem::readFile(u32 fileId) const {
    auto data = m_impl->storage.readFile(static_cast<i32>(fileId));
    return data.value_or(std::vector<u8>{});
}

std::optional<u32> CascFileSystem::reserveFileId(const std::string& path) {
    if (!m_impl->writable) return std::nullopt;
    auto id = m_impl->writable->reserveFileId(path);
    if (!id) {
        auto filedesc = m_impl->storage.fileInfo(path);
        if (filedesc) {
            return static_cast<u32>(filedesc->fileDataId);
        }
    }
    return id;
}

bool CascFileSystem::writeFile(u32 fileId, const std::vector<u8>& data) {
    if (!m_impl->writable) return false;
    return m_impl->writable->writeFile(static_cast<i32>(fileId), data);
}

bool CascFileSystem::fileExists(u32 fileId) const {
    return m_impl->storage.fileExists(static_cast<i32>(fileId));
}

// ============================================================================
// CascPathFileSystem (path-based)
// ============================================================================

struct CascPathFileSystem::Impl {
    explicit Impl(Storage& storage)
        : storage(storage)
        , writable(storage.isWritable() ? static_cast<StorageWritable*>(&storage) : nullptr) {}

    Storage& storage;
    StorageWritable* writable;
};

CascPathFileSystem::CascPathFileSystem(storages::casc::Storage& storage)
    : m_impl(std::make_unique<Impl>(storage)) {}

CascPathFileSystem::~CascPathFileSystem() = default;
CascPathFileSystem::CascPathFileSystem(CascPathFileSystem&&) noexcept = default;
CascPathFileSystem& CascPathFileSystem::operator=(CascPathFileSystem&&) noexcept = default;

std::vector<u8> CascPathFileSystem::readFile(const std::string& path) const {
    auto data = m_impl->storage.readFile(path);
    return data.value_or(std::vector<u8>{});
}

bool CascPathFileSystem::writeFile(const std::string& path, const std::vector<u8>& data) {
    if (!m_impl->writable) return false;
    return m_impl->writable->writeFile(path, data);
}

bool CascPathFileSystem::fileExists(const std::string& path) const {
    return m_impl->storage.fileExists(path);
}

std::vector<interfaces::DirectoryEntry> CascPathFileSystem::listDirectory(
    const std::string& dirPath) const {
    std::vector<interfaces::DirectoryEntry> result;
    std::unordered_set<std::string> seen;

    // Normalize input prefix.
    std::string prefix = dirPath;
    if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\')
        prefix.push_back('\\');

    // Convert to lowercase for comparison.
    std::string prefixLower = storages::common::toLower(prefix);

    m_impl->storage.enumerate([&](const EnumerateEntry& fe) {
        // Lowercase the entry path.
        std::string pathLower = storages::common::toLower(std::string(fe.path));

        if (pathLower.size() <= prefixLower.size()) return true;
        if (pathLower.substr(0, prefixLower.size()) != prefixLower) return true;

        // Extract the immediate child name.
        auto rest = std::string(fe.path.substr(prefix.size()));
        auto sep = rest.find_first_of("/\\");
        std::string childName;
        bool isDir = false;
        if (sep != std::string::npos) {
            childName = rest.substr(0, sep);
            isDir = true;
        } else {
            childName = rest;
        }

        if (!childName.empty() && seen.insert(childName).second) {
            result.push_back({childName, isDir});
        }
        return true;
    });

    return result;
}

} // namespace whiteout::utils
