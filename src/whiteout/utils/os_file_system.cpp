// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/utils/os_file_system.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace whiteout::utils {

struct OsFileSystem::Impl
{
    fs::path root;

    explicit Impl(std::string rootPath) : root(std::move(rootPath)) {}

    fs::path resolve(const std::string& path) const
    {
        return root / path;
    }
};

OsFileSystem::OsFileSystem(std::string rootPath)
    : m_impl(std::make_unique<Impl>(std::move(rootPath)))
{}

OsFileSystem::~OsFileSystem() = default;

OsFileSystem::OsFileSystem(OsFileSystem&&) noexcept = default;
OsFileSystem& OsFileSystem::operator=(OsFileSystem&&) noexcept = default;

std::vector<u8> OsFileSystem::readFile(const std::string& path) const
{
    const fs::path fullPath = m_impl->resolve(path);

    std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return {};

    const auto size = file.tellg();
    if (size <= 0)
        return {};

    file.seekg(0);
    std::vector<u8> buffer(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    if (!file)
        return {};

    return buffer;
}

bool OsFileSystem::fileExists(const std::string& path) const
{
    return fs::is_regular_file(m_impl->resolve(path));
}

std::vector<interfaces::DirectoryEntry> OsFileSystem::listDirectory(const std::string& path) const
{
    std::vector<interfaces::DirectoryEntry> entries;
    const fs::path fullPath = m_impl->resolve(path);

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fullPath, ec)) {
        if (ec) break;
        entries.push_back({
            entry.path().filename().string(),
            entry.is_directory()
        });
    }
    return entries;
}

} // namespace whiteout::utils
