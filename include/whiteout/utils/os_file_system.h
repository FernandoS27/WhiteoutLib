// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <whiteout/interfaces.h>
#include <whiteout/common_types.h>

namespace whiteout::utils {

/// VirtualFileSystem implementation backed by the OS filesystem.
///
/// All paths passed to readFile() / fileExists() are resolved relative to
/// the root directory supplied at construction time.
///
/// Example:
///   utils::OsFileSystem fs("C:/Games/Warcraft III/Data");
///   auto data = fs.readFile("units/human/arthas/arthas.mdx");
class OsFileSystem : public interfaces::VirtualFileSystem
{
public:
    /// Construct with a root directory. The path is stored as-is;
    /// relative paths are resolved from the process working directory.
    explicit OsFileSystem(std::string rootPath);
    ~OsFileSystem() override;

    // Non-copyable
    OsFileSystem(const OsFileSystem&) = delete;
    OsFileSystem& operator=(const OsFileSystem&) = delete;

    // Movable
    OsFileSystem(OsFileSystem&&) noexcept;
    OsFileSystem& operator=(OsFileSystem&&) noexcept;

    /// Returns false — OS filesystems are path-based only.
    bool supportsFileIds() const override;

    /// Read a file at `rootPath / path`. Returns an empty vector if not found.
    std::vector<u8> readFile(const std::string& path) const override;

    /// Not supported for OS filesystems — always returns an empty vector.
    std::vector<u8> readFile(u32 fileId) const override;

    bool fileExists(const std::string& path) const override;

    /// Not supported for OS filesystems — always returns false.
    bool fileExists(u32 fileId) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
