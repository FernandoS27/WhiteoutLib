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

bool CascFileSystem::fileExists(u32 fileId) const
{
    return m_impl->storage.fileExists(static_cast<i32>(fileId));
}

} // namespace whiteout::utils
