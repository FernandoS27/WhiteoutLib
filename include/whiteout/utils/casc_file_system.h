// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#if !defined(WHITEOUT_HAS_CASC)
#error                                                                                             \
    "<whiteout/utils/casc_file_system.h> requires CASC support. Configure with -DWHITEOUT_ENABLE_CASC=ON and link against the " \
    "whiteout_casc target."
#endif

#include <memory>
#include <string>
#include <vector>

#include <whiteout/interfaces.h>
#include <whiteout/common_types.h>

namespace whiteout::casc { 
    class Storage; 
}

namespace whiteout::utils {

/// VirtualFileSystem implementation backed by a whiteout::casc::Storage.
///
/// The Storage must outlive this object — CascFileSystem holds a non-owning
/// reference to it.
///
/// Requires the `whiteout_casc` CMake target.
///
/// Example:
///   casc::Storage storage;
///   storage.open("C:/Games/Diablo IV");
///   utils::CascFileSystem fs(storage);
///   auto data = fs.readFile("base/meta/actor/assassin.acr");
class CascFileSystem : public interfaces::VirtualFileSystem
{
public:
    explicit CascFileSystem(const casc::Storage& storage);
    ~CascFileSystem() override;

    // Non-copyable / non-movable (holds a reference)
    CascFileSystem(const CascFileSystem&) = delete;
    CascFileSystem& operator=(const CascFileSystem&) = delete;
    CascFileSystem(CascFileSystem&&) = delete;
    CascFileSystem& operator=(CascFileSystem&&) = delete;

    bool supportsFileIds() const override;

    /// Read a file by its CASC path. Returns an empty vector if not found.
    std::vector<u8> readFile(const std::string& path) const override;

    /// Read a file by its numeric file data ID. Returns an empty vector if not found.
    std::vector<u8> readFile(u32 fileId) const override;

    bool fileExists(const std::string& path) const override;

    bool fileExists(u32 fileId) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
