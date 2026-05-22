// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

namespace whiteout::utils {

/// VirtualPathFileSystem implementation backed by the OS filesystem.
///
/// All paths passed to readFile() / fileExists() are resolved relative to
/// the root directory supplied at construction time.
///
/// Example:
///   utils::OsFileSystem fs("C:/Games/Warcraft III/Data");
///   auto data = fs.readFile("units/human/arthas/arthas.mdx");
///
/// @bind methods, ctors=string, move_only, extends=whiteout::interfaces::VirtualPathFileSystem,
/// java_package=whiteout.utils
class OsFileSystem : public interfaces::VirtualPathFileSystem {
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

    /// Read a file at `rootPath / path`. Returns an empty vector if not found.
    std::vector<u8> readFile(const std::string& path) const override;

    bool writeFile(const std::string& path, const std::vector<u8>& data) override;

    bool fileExists(const std::string& path) const override;

    /// @bind skip — DirectoryEntry vector not exposed via codegen yet
    std::vector<interfaces::DirectoryEntry> listDirectory(const std::string& path) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
