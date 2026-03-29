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

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

namespace whiteout::casc {
class Storage;
}

namespace whiteout::utils {

/// CascFileSystem implementation backed by a whiteout::casc::Storage.
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
///   auto data = fs.readFile(12345);
class CascFileSystem : public interfaces::CascFileSystem {
public:
    explicit CascFileSystem(const casc::Storage& storage);
    ~CascFileSystem() override;

    // Non-copyable / non-movable (holds a reference)
    CascFileSystem(const CascFileSystem&) = delete;
    CascFileSystem& operator=(const CascFileSystem&) = delete;
    CascFileSystem(CascFileSystem&&) = delete;
    CascFileSystem& operator=(CascFileSystem&&) = delete;

    /// Read a file by its numeric file data ID. Returns an empty vector if not found.
    std::vector<u8> readFile(u32 fileId) const override;

    std::optional<u32> reserveFileId(const std::string& path) override;

    bool writeFile(u32 fileId, const std::vector<u8>& data) override;

    bool fileExists(u32 fileId) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
