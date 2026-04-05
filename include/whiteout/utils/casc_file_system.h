// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#if !defined(WHITEOUT_HAS_CASC)
#error                                                                                             \
    "<whiteout/utils/casc_file_system.h> requires CASC support. Configure with -DWHITEOUT_ENABLE_CASC=ON and link against " \
    "the whiteout_casc target."
#endif

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

namespace whiteout::storages::casc {
class Storage;
}

namespace whiteout::utils {

/// CascFileSystem implementation backed by a CASC archive (numeric file-data-ID).
///
/// The Storage must outlive this object — CascFileSystem holds a non-owning
/// reference to it.
///
/// If the underlying Storage is writable (StorageWritable), write operations
/// are forwarded to the write overlay. Otherwise writeFile() returns false
/// and reserveFileId() returns std::nullopt.
///
/// Requires the `whiteout_casc` CMake target.
///
/// Example:
///   auto storage = casc::Storage::open("C:/Games/WoW/Data");
///   utils::CascFileSystem fs(*storage);
///   auto data = fs.readFile(12345);
class CascFileSystem : public interfaces::CascFileSystem {
public:
    explicit CascFileSystem(storages::casc::Storage& storage);
    ~CascFileSystem() override;

    CascFileSystem(const CascFileSystem&) = delete;
    CascFileSystem& operator=(const CascFileSystem&) = delete;

    CascFileSystem(CascFileSystem&&) noexcept;
    CascFileSystem& operator=(CascFileSystem&&) noexcept;

    std::vector<u8> readFile(u32 fileId) const override;
    std::optional<u32> reserveFileId(const std::string& path) override;
    bool writeFile(u32 fileId, const std::vector<u8>& data) override;
    bool fileExists(u32 fileId) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// VirtualPathFileSystem implementation backed by a CASC archive (path-based).
///
/// The Storage must outlive this object — CascPathFileSystem holds a non-owning
/// reference to it.
///
/// Path separators: both '/' and '\\' are accepted and treated identically.
/// Filename comparison is case-insensitive, matching CASC archive semantics.
///
/// If the underlying Storage is writable (StorageWritable), write operations
/// are forwarded to the write overlay. Otherwise writeFile() returns false.
///
/// Requires the `whiteout_casc` CMake target.
///
/// Example:
///   auto storage = casc::Storage::open("C:/Games/WoW/Data");
///   utils::CascPathFileSystem fs(*storage);
///   auto data = fs.readFile("data\\global\\excel\\items.txt");
class CascPathFileSystem : public interfaces::VirtualPathFileSystem {
public:
    explicit CascPathFileSystem(storages::casc::Storage& storage);
    ~CascPathFileSystem() override;

    CascPathFileSystem(const CascPathFileSystem&) = delete;
    CascPathFileSystem& operator=(const CascPathFileSystem&) = delete;

    CascPathFileSystem(CascPathFileSystem&&) noexcept;
    CascPathFileSystem& operator=(CascPathFileSystem&&) noexcept;

    std::vector<u8> readFile(const std::string& path) const override;
    bool writeFile(const std::string& path, const std::vector<u8>& data) override;
    bool fileExists(const std::string& path) const override;
    std::vector<interfaces::DirectoryEntry> listDirectory(const std::string& path) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
