// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::wasm {

/// VirtualPathFileSystem backed by an in-memory map. Used by the WASM
/// bindings so JavaScript can populate a "path -> bytes" table before
/// invoking parsers (notably M2, which reads sibling .skin / .skel /
/// .anim files alongside the base .m2).
class InMemoryFileSystem : public interfaces::VirtualPathFileSystem {
public:
    InMemoryFileSystem() = default;

    void addFile(const std::string& path, std::vector<u8> data);
    void removeFile(const std::string& path);
    void clear();
    size_t fileCount() const noexcept { return m_files.size(); }

    // VirtualPathFileSystem
    std::vector<u8> readFile(const std::string& path) const override;
    bool writeFile(const std::string& path, const std::vector<u8>& data) override;
    bool fileExists(const std::string& path) const override;
    std::vector<interfaces::DirectoryEntry> listDirectory(const std::string& path) const override;

private:
    std::unordered_map<std::string, std::vector<u8>> m_files;
};

} // namespace whiteout::wasm
