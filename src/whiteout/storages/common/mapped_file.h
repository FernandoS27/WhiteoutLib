// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file mapped_file.h
/// @brief Cross-platform read-only memory-mapped file (RAII).
///
/// Internal header — not part of the public include path.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include <whiteout/common_types.h>

namespace whiteout::storages::common {

/// RAII wrapper for a read-only memory-mapped file.
///
/// Movable, non-copyable. Maps the entire file contents into the process
/// address space. The OS pages in only the accessed regions on demand.
class MappedFile {
public:
    /// Default constructor — creates an empty/invalid mapping.
    MappedFile() noexcept = default;

    /// Move constructor — transfers ownership.
    MappedFile(MappedFile&& other) noexcept;

    /// Move assignment — releases current mapping, transfers ownership.
    MappedFile& operator=(MappedFile&& other) noexcept;

    /// Non-copyable.
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    /// Destructor — unmaps and closes all OS handles.
    ~MappedFile();

    /// Open and memory-map a file for reading.
    /// @param path UTF-8 encoded file path.
    /// @return The mapped file, or nullopt on failure (file not found,
    ///         zero-length, permission denied, address-space exhaustion, etc.).
    static std::optional<MappedFile> open(const std::string& path);

    /// View of the mapped region. Empty if invalid.
    std::span<const u8> data() const noexcept;

    /// Raw pointer to the mapped data. nullptr if invalid.
    const u8* ptr() const noexcept { return m_data; }

    /// Size of the mapped region in bytes. 0 if invalid.
    size_t size() const noexcept { return m_size; }

    /// True if a valid mapping is held.
    explicit operator bool() const noexcept { return m_data != nullptr; }

private:
    void release() noexcept;

    const u8* m_data = nullptr;
    size_t m_size = 0;

#ifdef _WIN32
    void* m_fileHandle = nullptr;    // HANDLE to the file
    void* m_mappingHandle = nullptr; // HANDLE to the file mapping object
#endif
    // On POSIX, only m_data and m_size are needed (munmap).
};

} // namespace whiteout::storages::common
