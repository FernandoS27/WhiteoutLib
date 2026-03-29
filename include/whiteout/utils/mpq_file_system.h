// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#if !defined(WHITEOUT_HAS_MPQ)
#error                                                                                             \
    "<whiteout/utils/mpq_file_system.h> requires MPQ support. Configure with -DWHITEOUT_ENABLE_MPQ=ON and link against " \
    "the whiteout_mpq target."
#endif

#include <memory>
#include <string>
#include <vector>

#include <whiteout/interfaces.h>
#include <whiteout/common_types.h>

namespace whiteout::storages::mpq {
    class Storage;
}

namespace whiteout::utils {

/// VirtualPathFileSystem implementation backed by an MPQ archive.
///
/// The Storage must outlive this object — MpqFileSystem holds a non-owning
/// reference to it.
///
/// Path separators: both '/' and '\\' are accepted and treated identically.
/// Filename comparison is case-insensitive, matching MPQ archive semantics.
///
/// Requires the `whiteout_mpq` CMake target.
///
/// Example:
///   auto storage = mpq::Storage::open("War3.mpq");
///   utils::MpqFileSystem fs(*storage);
///   auto data = fs.readFile("units\\orc\\grunt\\grunt.mdx");
class MpqFileSystem : public interfaces::VirtualPathFileSystem
{
public:
    /// Construct from an existing MPQ storage. The storage is not owned;
    /// it must remain valid for the lifetime of this object.
    explicit MpqFileSystem(storages::mpq::Storage& storage);
    ~MpqFileSystem() override;

    // Non-copyable (holds a reference)
    MpqFileSystem(const MpqFileSystem&) = delete;
    MpqFileSystem& operator=(const MpqFileSystem&) = delete;

    // Movable
    MpqFileSystem(MpqFileSystem&&) noexcept;
    MpqFileSystem& operator=(MpqFileSystem&&) noexcept;

    /// Read a file from the archive. Returns an empty vector if not found.
    std::vector<u8> readFile(const std::string& path) const override;

    /// Write a file into the archive overlay. Changes are not persisted to disk
    /// until storage.save() is called on the underlying Storage.
    bool writeFile(const std::string& path, const std::vector<u8>& data) override;

    /// Check if a file exists in the archive (including the write overlay).
    bool fileExists(const std::string& path) const override;

    /// List all direct children of the given virtual directory.
    ///
    /// The directory path may use either '/' or '\\' as separator.
    /// Pass an empty string to list root-level entries.
    /// Returns one entry per unique child name; isDirectory is true when the
    /// child is a path component that precedes further files.
    std::vector<interfaces::DirectoryEntry> listDirectory(const std::string& path) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::utils
