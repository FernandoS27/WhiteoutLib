// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>
#include <whiteout/interfaces.h>
#include "../../common/string_utils.h"

#include <algorithm>
#include <unordered_set>

namespace whiteout::storages::casc {

// ============================================================================
// CascFileSystemImpl (numeric file-data-ID based)
// ============================================================================

class CascFileSystemImpl final : public interfaces::CascFileSystem {
public:
    explicit CascFileSystemImpl(Storage& storage)
        : m_storage(storage)
        , m_writable(storage.isWritable() ? static_cast<StorageWritable*>(&storage) : nullptr) {}

    std::vector<u8> readFile(u32 fileId) const override {
        auto data = m_storage.readFile(static_cast<i32>(fileId));
        return data.value_or(std::vector<u8>{});
    }

    std::optional<u32> reserveFileId(const std::string& path) override {
        if (!m_writable) return std::nullopt;
        auto id = m_writable->reserveFileId(path);
        if (!id) {
            auto filedesc = m_storage.fileInfo(path);
            if (filedesc) {
                return static_cast<u32>(filedesc->fileDataId);
            }
        }
        return id;
    }

    bool writeFile(u32 fileId, const std::vector<u8>& data) override {
        if (!m_writable) return false;
        return m_writable->writeFile(static_cast<i32>(fileId), data);
    }

    bool fileExists(u32 fileId) const override {
        return m_storage.fileExists(static_cast<i32>(fileId));
    }

private:
    Storage& m_storage;
    StorageWritable* m_writable;
};

// ============================================================================
// CascPathFileSystemImpl (path-based)
// ============================================================================

class CascPathFileSystemImpl final : public interfaces::VirtualPathFileSystem {
public:
    explicit CascPathFileSystemImpl(Storage& storage)
        : m_storage(storage)
        , m_writable(storage.isWritable() ? static_cast<StorageWritable*>(&storage) : nullptr) {}

    std::vector<u8> readFile(const std::string& path) const override {
        auto data = m_storage.readFile(path);
        return data.value_or(std::vector<u8>{});
    }

    bool writeFile(const std::string& path, const std::vector<u8>& data) override {
        if (!m_writable) return false;
        return m_writable->writeFile(path, data);
    }

    bool fileExists(const std::string& path) const override {
        return m_storage.fileExists(path);
    }

    std::vector<interfaces::DirectoryEntry> listDirectory(const std::string& dirPath) const override {
        std::vector<interfaces::DirectoryEntry> result;
        std::unordered_set<std::string> seen;

        // Normalize input prefix.
        std::string prefix = dirPath;
        if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\')
            prefix.push_back('\\');

        // Convert to lowercase for comparison.
        std::string prefixLower = storages::common::toLower(prefix);

        m_storage.enumerate([&](const EnumerateEntry& fe) {
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

private:
    Storage& m_storage;
    StorageWritable* m_writable;
};

} // namespace whiteout::storages::casc
