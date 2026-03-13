// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#if !defined(WHITEOUT_HAS_CASC)
#error                                                                                             \
    "<whiteout/casc/storage.h> requires CASC support. Configure with -DWHITEOUT_ENABLE_CASC=ON and link against the " \
    "whiteout_casc target."
#endif

/**
 * @file storage.h
 * @brief CASC storage wrapper API
 *
 * This header exposes a C++ RAII wrapper around CascLib while keeping
 * CascLib types and headers out of the public API surface.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

namespace whiteout {
namespace casc {

// ============================================================================
// CASC Constants
// ============================================================================

/// Common CASC locale masks for open/read operations.
struct LocaleMasks {
    static constexpr u32 All = 0xFFFFFFFFu;
    static constexpr u32 AllWoW = 0x0001F3F6u;
    static constexpr u32 None = 0x00000000u;
    static constexpr u32 Unknown1 = 0x00000001u;
    static constexpr u32 EnUS = 0x00000002u;
    static constexpr u32 KoKR = 0x00000004u;
    static constexpr u32 Reserved = 0x00000008u;
    static constexpr u32 FrFR = 0x00000010u;
    static constexpr u32 DeDE = 0x00000020u;
    static constexpr u32 ZhCN = 0x00000040u;
    static constexpr u32 EsES = 0x00000080u;
    static constexpr u32 ZhTW = 0x00000100u;
    static constexpr u32 EnGB = 0x00000200u;
    static constexpr u32 EnCN = 0x00000400u;
    static constexpr u32 EnTW = 0x00000800u;
    static constexpr u32 EsMX = 0x00001000u;
    static constexpr u32 RuRU = 0x00002000u;
    static constexpr u32 PtBR = 0x00004000u;
    static constexpr u32 ItIT = 0x00008000u;
    static constexpr u32 PtPT = 0x00010000u;
};

/// Additional flags accepted by file-open/read calls.
struct FileOpenFlags {
    static constexpr u32 None = 0x00000000u;
    static constexpr u32 StrictDataCheck = 0x00000010u;
    static constexpr u32 OvercomeEncrypted = 0x00000020u;
    static constexpr u32 OpenCKeyOnce = 0x00000040u;
};

/// Storage feature flags returned by Storage::features().
struct StorageFeatureFlags {
    static constexpr u32 FileNames = 0x00000001u;
    static constexpr u32 RootCKey = 0x00000002u;
    static constexpr u32 Tags = 0x00000004u;
    static constexpr u32 FileNameHashes = 0x00000008u;
    static constexpr u32 FileNameHashesOptional = 0x00000010u;
    static constexpr u32 FileDataIds = 0x00000020u;
    static constexpr u32 LocaleFlags = 0x00000040u;
    static constexpr u32 ContentFlags = 0x00000080u;
    static constexpr u32 DataArchives = 0x00000100u;
    static constexpr u32 DataFiles = 0x00000200u;
    static constexpr u32 Online = 0x00000400u;
    static constexpr u32 ForceDownload = 0x00001000u;
};

/// WoW content flags (when supported by storage).
struct ContentFlags {
    static constexpr u32 Install = 0x00000004u;
    static constexpr u32 LoadOnWindows = 0x00000008u;
    static constexpr u32 LoadOnMac = 0x00000010u;
    static constexpr u32 X86_32 = 0x00000020u;
    static constexpr u32 X86_64 = 0x00000040u;
    static constexpr u32 LowViolence = 0x00000080u;
    static constexpr u32 DontLoad = 0x00000100u;
    static constexpr u32 UpdatePlugin = 0x00000800u;
    static constexpr u32 Arm64 = 0x00008000u;
    static constexpr u32 Encrypted = 0x08000000u;
    static constexpr u32 NoNameHash = 0x10000000u;
    static constexpr u32 UncommonResolution = 0x20000000u;
    static constexpr u32 Bundle = 0x40000000u;
    static constexpr u32 NoCompression = 0x80000000u;
};

/// Sentinel used by several CASC fields when the value is not available.
constexpr u32 kInvalidId = 0xFFFFFFFFu;

enum class FileNameType : u32 { Full = 0, DataId = 1, CKey = 2, EKey = 3, Unknown = 0xFFFFFFFFu };

// ============================================================================
// CASC Metadata Types
// ============================================================================

struct StorageTag {
    std::string name;
    u32 value = 0;
};

struct StorageProduct {
    std::string codeName;
    u32 buildNumber = 0;
};

struct FileSpanInfo {
    std::array<u8, 16> cKey{};
    std::array<u8, 16> eKey{};
    u64 startOffset = 0;
    u64 endOffset = 0;
    u32 archiveIndex = 0;
    u32 archiveOffset = 0;
    u32 headerSize = 0;
    u32 frameCount = 0;
};

struct FileFullInfo {
    std::array<u8, 16> cKey{};
    std::array<u8, 16> eKey{};
    std::string dataFileName;
    u64 storageOffset = 0;
    u64 segmentOffset = 0;
    u64 tagBitMask = 0;
    u64 fileNameHash = 0;
    u64 contentSize = 0;
    u64 encodedSize = 0;
    u32 segmentIndex = 0;
    u32 spanCount = 0;
    u32 fileDataId = kInvalidId;
    u32 localeFlags = kInvalidId;
    u32 contentFlags = kInvalidId;
};

struct FindEntry {
    std::string name;
    std::string plainName;
    std::array<u8, 16> cKey{};
    std::array<u8, 16> eKey{};
    u64 tagBitMask = 0;
    u64 fileSize = 0;
    u32 fileDataId = kInvalidId;
    u32 localeFlags = kInvalidId;
    u32 contentFlags = kInvalidId;
    u32 spanCount = 0;
    bool fileAvailable = false;
    FileNameType nameType = FileNameType::Unknown;
};

// ============================================================================
// Open Options
// ============================================================================

struct OpenStorageOptions {
    /// Path to a local storage directory (for local or online cache mode).
    std::string path;

    /// Product code (e.g. "wow") for multi-product / online storages.
    std::string codeName;

    /// Optional region selector (for online mode).
    std::string region;

    /// Optional build key (hex MD5 string).
    std::string buildKey;

    /// Optional custom CDN URL.
    std::string cdnHostUrl;

    /// Locale mask used when opening storage.
    u32 localeMask = LocaleMasks::None;

    /// Additional feature flags (StorageFeatureFlags).
    u32 flags = 0;

    /// If true, calls CascOpenStorageEx in online mode.
    bool online = false;
};

// ============================================================================
// Storage Wrapper
// ============================================================================

/// RAII wrapper around a CascLib storage handle.
///
/// Provides file reading / enumeration for Blizzard CASC archives.
/// CascLib headers are never exposed here — the public API uses only standard
/// C++ types. Link against the `whiteout_casc` CMake target to use this.
///
/// Usage:
///   Storage storage;
///   if (!storage.open("C:/Games/WoW/Data")) { /* handle error */ }
///   auto data = storage.readFile("creature/cat/cat.m3");
class Storage {
public:
    Storage() = default;

    ~Storage();

    // Non-copyable
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // Movable
    Storage(Storage&& other) noexcept;
    Storage& operator=(Storage&& other) noexcept;

    /// Open a CASC storage at the given path.
    /// @param path  Game install directory or extracted data directory.
    /// @returns true on success, false on failure.
    bool open(const std::string& path);

    /// Open a CASC storage at the given path with locale mask.
    bool open(const std::string& path, u32 localeMask);

    /// Open an online CASC storage.
    /// @param codeName  Product code name (e.g. "wow", "hero") or a
    ///                  path:product string such as "C:/cache*wow".
    /// @returns true on success, false on failure.
    bool openOnline(const std::string& codeName);

    /// Open an online CASC storage with locale mask.
    bool openOnline(const std::string& codeName, u32 localeMask);

    /// Extended storage open that maps to CascOpenStorageEx.
    bool openEx(const OpenStorageOptions& options);

    /// Close the storage. Called automatically by the destructor.
    void close();

    /// Read an entire file from the archive into memory.
    /// @param cascPath  The CASC-internal path (e.g. "creature/cat/cat.m3").
    /// @returns The file contents, or std::nullopt if the file does not exist.
    std::optional<std::vector<u8>> readFile(const std::string& cascPath) const;

    /// Read an entire file with locale/open flags.
    std::optional<std::vector<u8>> readFile(const std::string& cascPath, u32 localeFlags,
                                            u32 openFlags) const;

    /// Read an entire file from the archive by its file data ID.
    /// @param fileId  The numeric file data ID.
    /// @returns The file contents, or std::nullopt if the file does not exist.
    std::optional<std::vector<u8>> readFile(i32 fileId) const;

    /// Read an entire file by data ID with locale/open flags.
    std::optional<std::vector<u8>> readFile(i32 fileId, u32 localeFlags, u32 openFlags) const;

    /// Read a byte range from a CASC path.
    std::optional<std::vector<u8>> readFileRange(const std::string& cascPath, u64 offset,
                                                 size_t byteCount,
                                                 u32 localeFlags = LocaleMasks::None,
                                                 u32 openFlags = FileOpenFlags::None) const;

    /// Read a byte range from a CASC data ID.
    std::optional<std::vector<u8>> readFileRange(i32 fileId, u64 offset, size_t byteCount,
                                                 u32 localeFlags = LocaleMasks::None,
                                                 u32 openFlags = FileOpenFlags::None) const;

    /// Query file size by path.
    std::optional<u64> fileSize(const std::string& cascPath, u32 localeFlags = LocaleMasks::None,
                                u32 openFlags = FileOpenFlags::None) const;

    /// Query file size by data ID.
    std::optional<u64> fileSize(i32 fileId, u32 localeFlags = LocaleMasks::None,
                                u32 openFlags = FileOpenFlags::None) const;

    /// Check whether a file exists in the archive.
    bool fileExists(const std::string& cascPath) const;

    /// Check whether a file exists in the archive with locale/open flags.
    bool fileExists(const std::string& cascPath, u32 localeFlags, u32 openFlags) const;

    /// Check whether a file exists in the archive by its file data ID.
    bool fileExists(i32 fileId) const;

    /// Check whether a file exists by data ID with locale/open flags.
    bool fileExists(i32 fileId, u32 localeFlags, u32 openFlags) const;

    /// Query full metadata for a file by path.
    std::optional<FileFullInfo> fileInfo(const std::string& cascPath,
                                         u32 localeFlags = LocaleMasks::None,
                                         u32 openFlags = FileOpenFlags::None) const;

    /// Query full metadata for a file by data ID.
    std::optional<FileFullInfo> fileInfo(i32 fileId, u32 localeFlags = LocaleMasks::None,
                                         u32 openFlags = FileOpenFlags::None) const;

    /// Query span metadata for a file by path.
    std::vector<FileSpanInfo> fileSpans(const std::string& cascPath,
                                        u32 localeFlags = LocaleMasks::None,
                                        u32 openFlags = FileOpenFlags::None) const;

    /// Query span metadata for a file by data ID.
    std::vector<FileSpanInfo> fileSpans(i32 fileId, u32 localeFlags = LocaleMasks::None,
                                        u32 openFlags = FileOpenFlags::None) const;

    /// Enumerate files in the archive.
    /// @param listFilePath  Optional path to a listfile; pass "" to use the
    ///                      built-in CascLib enumeration.
    /// @param callback      Receives each file name. Return true to continue,
    ///                      false to stop early.
    void enumerate(const std::string& listFilePath,
                   std::function<bool(const std::string& name)> callback) const;

    /// Enumerate files matching a wildcard pattern.
    /// @param mask           Wildcard pattern (e.g. "*.m3", "creature/*").
    /// @param listFilePath   Optional path to a listfile; pass "" to use the
    ///                       built-in CascLib enumeration.
    /// @param callback       Receives each matching file name. Return true to
    ///                       continue, false to stop early.
    void enumerate(const std::string& mask, const std::string& listFilePath,
                   std::function<bool(const std::string& name)> callback) const;

    /// Enumerate files with full entry metadata.
    void enumerateEntries(const std::string& listFilePath,
                          std::function<bool(const FindEntry& entry)> callback) const;

    /// Enumerate files by wildcard with full entry metadata.
    void enumerateEntries(const std::string& mask, const std::string& listFilePath,
                          std::function<bool(const FindEntry& entry)> callback) const;

    /// Convenience helper returning all matching names.
    std::vector<std::string> listFiles(const std::string& mask = "*",
                                       const std::string& listFilePath = "") const;

    /// Convenience helper returning all matching entries.
    std::vector<FindEntry> listEntries(const std::string& mask = "*",
                                       const std::string& listFilePath = "") const;

    /// Storage information helpers.
    std::optional<u32> localFileCount() const;
    std::optional<u32> totalFileCount() const;
    std::optional<u32> features() const;
    std::optional<StorageProduct> product() const;
    std::optional<std::string> pathProduct() const;
    std::vector<StorageTag> tags() const;

    /// Encryption key helpers.
    bool addEncryptionKey(u64 keyName, const std::array<u8, 16>& key);
    bool addEncryptionKey(u64 keyName, const std::string& keyHex);
    bool importKeysFromString(const std::string& keyList);
    bool importKeysFromFile(const std::string& keyFilePath);
    std::optional<std::array<u8, 16>> findEncryptionKey(u64 keyName) const;
    std::optional<u64> notFoundEncryptionKey() const;

    /// CDN helpers (do not require an opened Storage).
    static std::string defaultCdnHost();
    static std::optional<std::vector<u8>> downloadFromCdn(const std::string& cdnHostUrl,
                                                          const std::string& product,
                                                          const std::string& fileName);

    /// Returns the last CascLib error code (thread-local).
    /// Useful for diagnosing why open / readFile / fileExists failed.
    static u32 lastError() noexcept;

    /// Returns true if the storage handle is valid (non-null).
    explicit operator bool() const noexcept {
        return m_handle != nullptr;
    }

private:
    void* m_handle = nullptr;
};

} // namespace casc
} // namespace whiteout
