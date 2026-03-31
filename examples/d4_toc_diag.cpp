// Quick diagnostic: search D4 TVFS for CoreTOC entries.
#include <whiteout/storages/casc/storage.h>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <algorithm>

using namespace whiteout;
using namespace whiteout::storages::casc;

static std::string hexStr(const std::array<u8, 16>& key) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (auto b : key) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0xF]);
    }
    return s;
}

int main() {
    auto storage = Storage::open("C:\\Program Files (x86)\\Diablo IV");
    if (!storage) {
        std::cerr << "Failed to open storage, err=" << Storage::lastError() << "\n";
        return 1;
    }

    std::cout << "Searching for CoreTOC / base: entries...\n\n";

    // Count entries by prefix.
    size_t total = 0, baseColon = 0, baseMeta = 0, tocRelated = 0;
    storage->enumerate([&](const FindEntry& fe) {
        ++total;
        std::string lower = fe.path;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower.starts_with("base:")) {
            ++baseColon;
            if (baseColon <= 20) {
                std::cout << "  base: \"" << fe.path << "\"  size=" << fe.fileSize
                          << "  CKey=" << hexStr(fe.cKey) << "\n";
            }
            if (lower.starts_with("base:meta")) ++baseMeta;
        }
        if (lower.find("coretoc") != std::string::npos) {
            std::cout << "  TOC!  \"" << fe.path << "\"  size=" << fe.fileSize
                      << "  CKey=" << hexStr(fe.cKey) << "\n";
            ++tocRelated;
        }
        return true;
    });
    std::cout << "\nTotal: " << total << ", base: " << baseColon
              << ", base:meta: " << baseMeta << ", TOC: " << tocRelated << "\n";

    // Try reading CoreTOC with various paths.
    std::cout << "\nTrying to read CoreTOC.dat...\n";
    for (auto& p : {"Base/CoreTOC.dat", "base/coretoc.dat",
                     "base:CoreTOC.dat", "base:coretoc.dat",
                     "Base\\CoreTOC.dat", "base:meta/CoreTOC.dat",
                     "base:Base/CoreTOC.dat"}) {
        auto data = storage->readFile(p);
        if (data && !data->empty()) {
            std::cout << "  OK: \"" << p << "\" -> " << data->size() << " bytes";
            if (data->size() >= 4) {
                u32 magic = 0;
                std::memcpy(&magic, data->data(), 4);
                std::cout << "  magic=0x" << std::hex << magic << std::dec;
            }
            std::cout << "\n";
        } else {
            std::cout << "  FAIL: \"" << p << "\" err=" << Storage::lastError() << "\n";
        }
    }

    // Try reading the "base:" container itself — check if archivedata is on disk.
    std::cout << "\nTrying to read the base: container entry directly...\n";
    auto baseData = storage->readFile("base:");
    if (baseData && !baseData->empty()) {
        std::cout << "  OK: base: container is " << baseData->size() << " bytes\n";
        // Check if it starts with TVFS header.
        if (baseData->size() >= 4) {
            u32 magic = 0;
            std::memcpy(&magic, baseData->data(), 4);
            std::cout << "  First 4 bytes: 0x" << std::hex << magic << std::dec << "\n";
        }
    } else {
        std::cout << "  FAIL: base: container not readable, err=" << Storage::lastError() << "\n";
        std::cout << "  (This means the 53MB VFS sub-manifest is not in local archives)\n";
    }

    // Try reading "Base" (the non-colon entry).
    auto baseNoColon = storage->readFile("Base");
    if (baseNoColon && !baseNoColon->empty()) {
        std::cout << "  Base (no colon) readable: " << baseNoColon->size() << " bytes\n";
    }

    // Check the fenris data directory for local data archives.
    std::cout << "\nLocal archive check...\n";
    size_t dataArchives = 0;
    for (auto& dirEntry : std::filesystem::directory_iterator("C:\\Program Files (x86)\\Diablo IV\\Data\\data")) {
        if (dirEntry.is_regular_file()) {
            auto ext = dirEntry.path().extension().string();
            if (ext.empty()) {
                ++dataArchives;
                if (dataArchives <= 5)
                    std::cout << "  " << dirEntry.path().filename().string()
                              << "  " << dirEntry.file_size() << " bytes\n";
            }
        }
    }
    std::cout << "  Total data archives: " << dataArchives << "\n";

    return 0;
}
