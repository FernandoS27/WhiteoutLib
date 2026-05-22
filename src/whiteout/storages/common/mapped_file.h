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
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::storages::common {

/// Advisory access-pattern hint for memory-mapped files.
enum class AccessHint : u8 {
    Normal,     ///< Default OS behavior.
    Sequential, ///< Hint for sequential access (madvise MADV_SEQUENTIAL /
                ///< FILE_FLAG_SEQUENTIAL_SCAN).
    Random,     ///< Hint for random access (madvise MADV_RANDOM / FILE_FLAG_RANDOM_ACCESS).
};

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
    /// @param path  UTF-8 encoded file path.
    /// @param hint  Advisory access-pattern hint.
    /// @param error Optional output for a human-readable error description.
    /// @return The mapped file, or nullopt on failure (file not found,
    ///         zero-length, permission denied, address-space exhaustion, etc.).
    static std::optional<MappedFile> open(const std::string& path,
                                          AccessHint hint = AccessHint::Normal,
                                          std::string* error = nullptr);

    /// Return the path this file was mapped from. Empty if default-constructed.
    const std::string& path() const noexcept {
        return m_path;
    }

    /// Apply advisory hint to an already-opened mapping.
    void advise(AccessHint hint) const noexcept;

    /// View of the mapped region. Empty if invalid.
    std::span<const u8> data() const noexcept;

    /// Raw pointer to the mapped data. nullptr if invalid.
    const u8* ptr() const noexcept {
        return m_data;
    }

    /// Size of the mapped region in bytes. 0 if invalid.
    size_t size() const noexcept {
        return m_size;
    }

    /// True if a valid mapping is held.
    explicit operator bool() const noexcept {
        return m_data != nullptr;
    }

private:
    void release() noexcept;

    std::string m_path;
    const u8* m_data = nullptr;
    size_t m_size = 0;
    // No file or mapping handles are retained. On Windows the file
    // handle and section handle are closed immediately after
    // MapViewOfFile so we hold no share-mode contention against
    // third-party tools (e.g. WC3 World Editor) that may open the same
    // archive — the OS keeps the file alive until UnmapViewOfFile. On
    // POSIX, ::close() runs right after mmap() for the same reason.
};

/// True if @p errorMsg (as produced by MappedFile::open / readFileFully)
/// describes a Windows sharing violation or POSIX equivalent — i.e. another
/// process holds the file with a share mode that denies our read access.
inline bool isSharingViolation(const std::string& errorMsg) noexcept {
#ifdef _WIN32
    // ERROR_SHARING_VIOLATION = 32, ERROR_LOCK_VIOLATION = 33.
    return errorMsg.find("[Win32 error 32]") != std::string::npos ||
           errorMsg.find("[Win32 error 33]") != std::string::npos;
#else
    // Best effort on POSIX (advisory locks may surface as EAGAIN/EACCES).
    return errorMsg.find("[errno 11]") != std::string::npos ||
           errorMsg.find("[errno 13]") != std::string::npos;
#endif
}

/// Read the entire contents of a file into a heap buffer (RAM-only).
///
/// Opens the file with maximally permissive read sharing
/// (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE on Windows),
/// reads its full contents, and closes the OS handle before returning.
/// No file handles or memory mappings are retained — the returned vector
/// is the only reference to the data.
///
/// Use this for small configuration files that are parsed once and the
/// raw bytes discarded. For large archives accessed throughout the
/// process lifetime, prefer MappedFile::open().
///
/// @param path  UTF-8 encoded file path.
/// @param error Optional output for a human-readable error description.
/// @return File contents, or nullopt on failure (file not found, empty,
///         permission denied, read error).
std::optional<std::vector<u8>> readFileFully(const std::string& path, std::string* error = nullptr);

} // namespace whiteout::storages::common
