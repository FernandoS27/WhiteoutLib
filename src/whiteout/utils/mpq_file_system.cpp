// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/utils/mpq_file_system.h"
#include "whiteout/storages/mpq/storage.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace whiteout::utils {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

/// Normalize a path for MPQ comparison: replace '/' with '\\', convert to
/// lowercase.  MPQ archives are case-insensitive and conventionally use '\\'.
std::string normalizePath(const std::string& path)
{
    std::string result = path;
    for (char& c : result) {
        if (c == '/')
            c = '\\';
        else
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

/// Strip a trailing '\\' from the path, if present.
std::string stripTrailingSep(std::string path)
{
    if (!path.empty() && path.back() == '\\')
        path.pop_back();
    return path;
}

} // anonymous namespace

// ============================================================================
// Impl
// ============================================================================

struct MpqFileSystem::Impl
{
    storages::mpq::Storage& storage;

    explicit Impl(storages::mpq::Storage& s) : storage(s) {}

    /// Build the canonical MPQ filename: normalize and strip trailing separator.
    std::string resolve(const std::string& path) const
    {
        return stripTrailingSep(normalizePath(path));
    }
};

// ============================================================================
// Public API
// ============================================================================

MpqFileSystem::MpqFileSystem(storages::mpq::Storage& storage)
    : m_impl(std::make_unique<Impl>(storage))
{}

MpqFileSystem::~MpqFileSystem() = default;

MpqFileSystem::MpqFileSystem(MpqFileSystem&&) noexcept = default;
MpqFileSystem& MpqFileSystem::operator=(MpqFileSystem&&) noexcept = default;

std::vector<u8> MpqFileSystem::readFile(const std::string& path) const
{
    auto result = m_impl->storage.readFile(m_impl->resolve(path));
    if (!result)
        return {};
    return std::move(*result);
}

bool MpqFileSystem::writeFile(const std::string& path, const std::vector<u8>& data)
{
    return m_impl->storage.writeFile(m_impl->resolve(path),
                                     std::span<const u8>(data));
}

bool MpqFileSystem::fileExists(const std::string& path) const
{
    return m_impl->storage.fileExists(m_impl->resolve(path));
}

std::vector<interfaces::DirectoryEntry> MpqFileSystem::listDirectory(
    const std::string& path) const
{
    // Normalize the directory prefix used for matching.
    const std::string normPrefix = stripTrailingSep(normalizePath(path));
    // The full prefix to strip: "dir\\" or "" for root.
    const std::string matchPrefix = normPrefix.empty() ? "" : normPrefix + "\\";

    std::set<std::string> seen; // normalized component names for dedup
    std::vector<interfaces::DirectoryEntry> entries;

    m_impl->storage.enumerate([&](const std::string& name) -> bool {
        const std::string normName = normalizePath(name);

        // Check the entry lives under our directory.
        if (!matchPrefix.empty()) {
            if (normName.size() <= matchPrefix.size())
                return true;
            if (normName.compare(0, matchPrefix.size(), matchPrefix) != 0)
                return true;
        }

        // Extract the next path component after the prefix.
        const std::string rest = normName.substr(matchPrefix.size());
        if (rest.empty())
            return true;

        const size_t sep = rest.find('\\');
        const std::string normComponent = (sep == std::string::npos)
                                              ? rest
                                              : rest.substr(0, sep);
        const bool isDir = (sep != std::string::npos);

        if (seen.insert(normComponent).second) {
            // Use original casing from the archive name for the entry name.
            const std::string original = name.substr(matchPrefix.size());
            const size_t origSep = original.find_first_of("/\\");
            const std::string origComponent = (origSep == std::string::npos)
                                                  ? original
                                                  : original.substr(0, origSep);
            entries.push_back({origComponent, isDir});
        }

        return true; // continue enumeration
    });

    return entries;
}

} // namespace whiteout::utils
