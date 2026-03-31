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
    : m_path(std::move(other.m_path)), m_data(other.m_data), m_size(other.m_size)
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
        m_path = std::move(other.m_path);
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

// ============================================================================
// Helpers
// ============================================================================

namespace {

#ifdef _WIN32
std::string lastErrorString() {
    DWORD err = GetLastError();
    if (err == 0)
        return "Unknown error";
    LPSTR buf = nullptr;
    DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string msg(buf, len);
    LocalFree(buf);
    // Trim trailing newline.
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    return msg;
}
#else
std::string lastErrorString() {
    return strerror(errno);
}
#endif

void setError(std::string* error, const std::string& msg) {
    if (error)
        *error = msg;
}

} // anonymous namespace

// ============================================================================
// Open
// ============================================================================

#ifdef _WIN32

std::optional<MappedFile> MappedFile::open(const std::string& path, AccessHint hint,
                                           std::string* error) {
    // Convert UTF-8 path to wide string.
    if (path.empty()) {
        setError(error, "Empty path");
        return std::nullopt;
    }

    int wideLen =
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
    if (wideLen <= 0) {
        setError(error, "Failed to convert path to wide string");
        return std::nullopt;
    }

    std::wstring widePath(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), widePath.data(),
                        wideLen);

    // Map AccessHint to CreateFile flags.
    DWORD flagsAndAttrs = FILE_ATTRIBUTE_NORMAL;
    switch (hint) {
    case AccessHint::Sequential: flagsAndAttrs |= FILE_FLAG_SEQUENTIAL_SCAN; break;
    case AccessHint::Random: flagsAndAttrs |= FILE_FLAG_RANDOM_ACCESS; break;
    default: break;
    }

    // Open file for reading.
    HANDLE hFile = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, flagsAndAttrs, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        setError(error, "CreateFileW failed: " + lastErrorString());
        return std::nullopt;
    }

    // Get file size.
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
        std::string reason =
            fileSize.QuadPart == 0 ? "File is empty" : "GetFileSizeEx failed: " + lastErrorString();
        setError(error, reason);
        CloseHandle(hFile);
        return std::nullopt;
    }

    // Create file mapping.
    HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        setError(error, "CreateFileMappingW failed: " + lastErrorString());
        CloseHandle(hFile);
        return std::nullopt;
    }

    // Map view.
    void* viewPtr = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!viewPtr) {
        setError(error, "MapViewOfFile failed: " + lastErrorString());
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return std::nullopt;
    }

    MappedFile result;
    result.m_path = path;
    result.m_data = static_cast<const u8*>(viewPtr);
    result.m_size = static_cast<size_t>(fileSize.QuadPart);
    result.m_fileHandle = hFile;
    result.m_mappingHandle = hMapping;
    return result;
}

void MappedFile::advise(AccessHint /*hint*/) const noexcept {
    // On Windows, the access hint is applied at CreateFile time.
    // Changing it after the fact requires remapping, which is not worth it.
    // This is a no-op for already-opened mappings.
}

#else // POSIX

std::optional<MappedFile> MappedFile::open(const std::string& path, AccessHint hint,
                                           std::string* error) {
    if (path.empty()) {
        setError(error, "Empty path");
        return std::nullopt;
    }

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        setError(error, "open() failed: " + lastErrorString());
        return std::nullopt;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        std::string reason =
            st.st_size == 0 ? "File is empty" : "fstat() failed: " + lastErrorString();
        setError(error, reason);
        ::close(fd);
        return std::nullopt;
    }

    auto fileSize = static_cast<size_t>(st.st_size);
    void* mapped = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd); // fd can be closed after mmap — mapping stays valid.

    if (mapped == MAP_FAILED) {
        setError(error, "mmap() failed: " + lastErrorString());
        return std::nullopt;
    }

    // Apply access hint.
    int advice = MADV_NORMAL;
    switch (hint) {
    case AccessHint::Sequential: advice = MADV_SEQUENTIAL; break;
    case AccessHint::Random: advice = MADV_RANDOM; break;
    default: break;
    }
    if (advice != MADV_NORMAL)
        madvise(mapped, fileSize, advice);

    MappedFile result;
    result.m_path = path;
    result.m_data = static_cast<const u8*>(mapped);
    result.m_size = fileSize;
    return result;
}

void MappedFile::advise(AccessHint hint) const noexcept {
    if (!m_data)
        return;
    int advice = MADV_NORMAL;
    switch (hint) {
    case AccessHint::Sequential: advice = MADV_SEQUENTIAL; break;
    case AccessHint::Random: advice = MADV_RANDOM; break;
    default: break;
    }
    madvise(const_cast<u8*>(m_data), m_size, advice);
}

#endif

} // namespace whiteout::storages::common
