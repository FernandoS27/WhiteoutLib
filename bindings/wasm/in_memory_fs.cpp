// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "in_memory_fs.h"

#include <algorithm>
#include <unordered_set>

namespace whiteout::wasm {

namespace {

// Normalise to forward slashes so callers do not have to care which separator
// the input uses. Anything past the last separator is the file name.
std::string normalize(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::string parentOf(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}

std::string basename(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

} // namespace

void InMemoryFileSystem::addFile(const std::string& path, std::vector<u8> data) {
    m_files[normalize(path)] = std::move(data);
}

void InMemoryFileSystem::removeFile(const std::string& path) {
    m_files.erase(normalize(path));
}

void InMemoryFileSystem::clear() {
    m_files.clear();
}

std::vector<u8> InMemoryFileSystem::readFile(const std::string& path) const {
    auto it = m_files.find(normalize(path));
    if (it == m_files.end()) return {};
    return it->second;
}

bool InMemoryFileSystem::writeFile(const std::string& path, const std::vector<u8>& data) {
    m_files[normalize(path)] = data;
    return true;
}

bool InMemoryFileSystem::fileExists(const std::string& path) const {
    return m_files.find(normalize(path)) != m_files.end();
}

std::vector<interfaces::DirectoryEntry>
InMemoryFileSystem::listDirectory(const std::string& path) const {
    const std::string dir = normalize(path);
    const std::string prefix = dir.empty() ? std::string{} : dir + "/";

    std::vector<interfaces::DirectoryEntry> entries;
    std::unordered_set<std::string> seenSubdirs;

    for (const auto& [filePath, _] : m_files) {
        if (!prefix.empty() && filePath.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const std::string rel = prefix.empty() ? filePath : filePath.substr(prefix.size());
        const auto slash = rel.find('/');
        if (slash == std::string::npos) {
            entries.push_back({rel, false});
        } else {
            const std::string subdir = rel.substr(0, slash);
            if (seenSubdirs.insert(subdir).second) {
                entries.push_back({subdir, true});
            }
        }
    }

    return entries;
}

} // namespace whiteout::wasm
