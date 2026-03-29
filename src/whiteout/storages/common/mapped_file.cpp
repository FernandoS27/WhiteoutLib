// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file mapped_file.cpp
/// @brief Cross-platform read-only memory-mapped file implementation.

#include "mapped_file.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <utility>

namespace whiteout::storages::common {

// ============================================================================
// Lifecycle
// ============================================================================

MappedFile::MappedFile(MappedFile&& other) noexcept
    : m_data(other.m_data), m_size(other.m_size)
#ifdef _WIN32
      ,
      m_fileHandle(other.m_fileHandle), m_mappingHandle(other.m_mappingHandle)
#endif
{
    other.m_data = nullptr;
    other.m_size = 0;
#ifdef _WIN32
    other.m_fileHandle = nullptr;
    other.m_mappingHandle = nullptr;
#endif
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        release();
        m_data = other.m_data;
        m_size = other.m_size;
#ifdef _WIN32
        m_fileHandle = other.m_fileHandle;
        m_mappingHandle = other.m_mappingHandle;
        other.m_fileHandle = nullptr;
        other.m_mappingHandle = nullptr;
#endif
        other.m_data = nullptr;
        other.m_size = 0;
    }
    return *this;
}

MappedFile::~MappedFile() {
    release();
}

void MappedFile::release() noexcept {
    if (!m_data)
        return;

#ifdef _WIN32
    UnmapViewOfFile(m_data);
    if (m_mappingHandle)
        CloseHandle(m_mappingHandle);
    if (m_fileHandle)
        CloseHandle(m_fileHandle);
    m_mappingHandle = nullptr;
    m_fileHandle = nullptr;
#else
    munmap(const_cast<u8*>(m_data), m_size);
#endif
    m_data = nullptr;
    m_size = 0;
}

// ============================================================================
// Factory
// ============================================================================

std::span<const u8> MappedFile::data() const noexcept {
    if (!m_data)
        return {};
    return {m_data, m_size};
}

#ifdef _WIN32

std::optional<MappedFile> MappedFile::open(const std::string& path) {
    // Convert UTF-8 path to wide string.
    if (path.empty())
        return std::nullopt;

    int wideLen =
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
    if (wideLen <= 0)
        return std::nullopt;

    std::wstring widePath(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), widePath.data(),
                        wideLen);

    // Open file for reading.
    HANDLE hFile = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return std::nullopt;

    // Get file size.
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        return std::nullopt;
    }

    // Create file mapping.
    HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        CloseHandle(hFile);
        return std::nullopt;
    }

    // Map view.
    void* viewPtr = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!viewPtr) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return std::nullopt;
    }

    MappedFile result;
    result.m_data = static_cast<const u8*>(viewPtr);
    result.m_size = static_cast<size_t>(fileSize.QuadPart);
    result.m_fileHandle = hFile;
    result.m_mappingHandle = hMapping;
    return result;
}

#else // POSIX

std::optional<MappedFile> MappedFile::open(const std::string& path) {
    if (path.empty())
        return std::nullopt;

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return std::nullopt;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        ::close(fd);
        return std::nullopt;
    }

    auto fileSize = static_cast<size_t>(st.st_size);
    void* mapped = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd); // fd can be closed after mmap — mapping stays valid.

    if (mapped == MAP_FAILED)
        return std::nullopt;

    MappedFile result;
    result.m_data = static_cast<const u8*>(mapped);
    result.m_size = fileSize;
    return result;
}

#endif

} // namespace whiteout::storages::common
