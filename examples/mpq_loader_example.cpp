// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Minimal MPQ loader example: open an archive and list its files.

#include <whiteout/storages/mpq/storage.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace mpq = whiteout::storages::mpq;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path-to.mpq>\n";
        return 1;
    }

    std::string path = argv[1];
    if (!std::filesystem::is_regular_file(path)) {
        std::cerr << "File not found: " << path << "\n";
        return 1;
    }

    auto storage = mpq::Storage::open(path);
    if (!storage) {
        std::cerr << "Failed to open: " << path << "\n";
        return 1;
    }

    auto info = storage->archiveInfo();
    std::cout << "Archive: " << path << "\n"
              << "  Format: V" << (info.formatVersion + 1) << "\n"
              << "  Sector size: " << info.sectorSize << "\n"
              << "  Hash table: " << info.hashTableEntries << " entries\n"
              << "  Block table: " << info.blockTableEntries << " entries\n\n";

    auto files = storage->listFiles();
    std::cout << "Files (" << files.size() << "):\n";
    for (const auto& name : files) {
        auto fi = storage->fileInfo(name);
        std::cout << "  " << name;
        if (fi)
            std::cout << "  (" << fi->uncompressedSize << " bytes)";
        std::cout << "\n";
    }

    return 0;
}
