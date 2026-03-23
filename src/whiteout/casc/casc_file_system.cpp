// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/utils/casc_file_system.h"
#include "whiteout/casc/storage.h"

namespace whiteout::utils {

struct CascFileSystem::Impl
{
    const casc::Storage& storage;

    explicit Impl(const casc::Storage& s) noexcept : storage(s) {}
};

CascFileSystem::CascFileSystem(const casc::Storage& storage)
    : m_impl(std::make_unique<Impl>(storage))
{}

CascFileSystem::~CascFileSystem() = default;

std::vector<u8> CascFileSystem::readFile(u32 fileId) const
{
    auto result = m_impl->storage.readFile(static_cast<i32>(fileId));
    return result ? std::move(*result) : std::vector<u8>{};
}

std::optional<u32> CascFileSystem::reserveFileId(const std::string& path)
{
    // CascLib does not support reserving file IDs, so we return std::nullopt to indicate failure.
    return std::nullopt;
}

bool CascFileSystem::writeFile(u32 fileId, const std::vector<u8>& data)
{
    // CascLib does not support writing files, so we return false to indicate failure.
    return false;
}

bool CascFileSystem::fileExists(u32 fileId) const
{
    return m_impl->storage.fileExists(static_cast<i32>(fileId));
}

} // namespace whiteout::utils
