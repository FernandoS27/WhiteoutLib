// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Interactive MPQ archive explorer: open an .mpq file, browse its contents,
/// read or extract files, add/delete files, and save modifications.

#include <whiteout/storages/mpq/storage.h>
#include <whiteout/utils/simple_thread_pool.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace mpq = whiteout::storages::mpq;

// ============================================================================
// Helpers
// ============================================================================

/// Read a trimmed line from stdin. Returns false on EOF.
static bool readLine(std::string& out) {
    out.clear();
    if (!std::getline(std::cin, out))
        return false;
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return true;
}

/// Try to parse a 1-based index from a string. Returns 0 on failure.
static size_t parseIndex(const std::string& s) {
    try {
        return std::stoul(s);
    } catch (...) {
        return 0;
    }
}

/// Format a byte count as a human-readable string.
static std::string formatSize(uint64_t bytes) {
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + " KB";
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << mb << " MB";
    return oss.str();
}

/// Case-insensitive wildcard match (* and ? only).
static bool wildcardMatch(const std::string& pattern, const std::string& text) {
    size_t pi = 0, ti = 0;
    size_t starP = std::string::npos, starT = 0;

    while (ti < text.size()) {
        if (pi < pattern.size() &&
            (std::tolower(static_cast<unsigned char>(pattern[pi])) ==
                 std::tolower(static_cast<unsigned char>(text[ti])) ||
             pattern[pi] == '?')) {
            ++pi;
            ++ti;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            starP = pi++;
            starT = ti;
        } else if (starP != std::string::npos) {
            pi = starP + 1;
            ti = ++starT;
        } else {
            return false;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*')
        ++pi;

    return pi == pattern.size();
}

/// Print file flags in a human-readable form.
static std::string formatFlags(mpq::FileFlags flags) {
    std::string result;
    if (mpq::hasFlag(flags, mpq::FileFlags::Compressed)) result += "compressed ";
    if (mpq::hasFlag(flags, mpq::FileFlags::Encrypted))  result += "encrypted ";
    if (mpq::hasFlag(flags, mpq::FileFlags::SingleUnit)) result += "single-unit ";
    if (result.empty()) result = "none";
    else result.pop_back(); // trailing space
    return result;
}

// ============================================================================
// Commands
// ============================================================================

static void cmdInfo(mpq::Storage& storage) {
    auto info = storage.archiveInfo();
    std::cout << "\n  Archive Info:\n"
              << "    Format version:    V" << (info.formatVersion + 1) << "\n"
              << "    Hash table size:   " << info.hashTableEntries << "\n"
              << "    Block table size:  " << info.blockTableEntries << "\n"
              << "    Sector size:       " << info.sectorSize << " bytes\n"
              << "    Archive size:      " << formatSize(info.archiveSize) << "\n";
}

static void cmdList(mpq::Storage& storage, const std::string& pattern) {
    auto files = storage.listFiles();

    std::vector<std::string> matched;
    for (const auto& name : files) {
        if (pattern.empty() || pattern == "*" || wildcardMatch(pattern, name))
            matched.push_back(name);
    }

    std::sort(matched.begin(), matched.end());

    if (matched.empty()) {
        std::cout << "  No files match \"" << pattern << "\".\n";
        return;
    }

    std::cout << "\n";
    size_t shown = 0;
    for (const auto& name : matched) {
        auto fi = storage.fileInfo(name);
        std::cout << "  " << name;
        if (fi) {
            std::cout << "  (" << formatSize(fi->uncompressedSize) << ")";
        }
        std::cout << "\n";
        ++shown;
        if (shown >= 200) {
            std::cout << "  ... and " << (matched.size() - shown) << " more.\n";
            break;
        }
    }
    std::cout << "\n  " << matched.size() << " file(s) matched.\n";
}

static void cmdFileInfo(mpq::Storage& storage) {
    std::cout << "  Filename: ";
    std::string name;
    if (!readLine(name) || name.empty()) return;

    auto fi = storage.fileInfo(name);
    if (!fi) {
        std::cout << "  File not found: " << name << "\n";
        return;
    }

    std::cout << "\n  File: " << fi->name << "\n"
              << "    Compressed size:   " << formatSize(fi->compressedSize) << "\n"
              << "    Uncompressed size: " << formatSize(fi->uncompressedSize) << "\n"
              << "    Flags:             " << formatFlags(fi->flags) << "\n"
              << "    Locale:            0x" << std::hex << std::setw(4) << std::setfill('0')
              << fi->locale << std::dec << "\n";

    if (fi->compressedSize > 0 && fi->uncompressedSize > 0) {
        double ratio = 100.0 * (1.0 - static_cast<double>(fi->compressedSize) /
                                           static_cast<double>(fi->uncompressedSize));
        std::cout << "    Compression ratio: " << std::fixed << std::setprecision(1)
                  << ratio << "%\n";
    }
}

static void cmdExtract(mpq::Storage& storage) {
    std::cout << "  Filename (or * for all): ";
    std::string pattern;
    if (!readLine(pattern) || pattern.empty()) return;

    std::cout << "  Output directory: ";
    std::string outDir;
    if (!readLine(outDir) || outDir.empty()) {
        std::cout << "  No output directory specified.\n";
        return;
    }

    auto files = storage.listFiles();
    std::vector<std::string> toExtract;
    for (const auto& name : files) {
        if (pattern == "*" || wildcardMatch(pattern, name))
            toExtract.push_back(name);
    }

    if (toExtract.empty()) {
        std::cout << "  No files match \"" << pattern << "\".\n";
        return;
    }

    std::cout << "  Extracting " << toExtract.size() << " file(s)...\n";

    std::filesystem::create_directories(outDir);
    size_t extracted = 0, failed = 0;
    uint64_t totalBytes = 0;

    for (const auto& name : toExtract) {
        auto data = storage.readFile(name);
        if (!data) {
            std::cerr << "    FAILED: " << name << "\n";
            ++failed;
            continue;
        }

        // Build output path, converting backslashes to the OS separator.
        std::string safeName = name;
        for (char& c : safeName) {
            if (c == '\\') c = '/';
        }

        auto dest = std::filesystem::path(outDir) / safeName;
        std::filesystem::create_directories(dest.parent_path());

        std::ofstream out(dest, std::ios::binary);
        if (!out) {
            std::cerr << "    FAILED to write: " << dest.string() << "\n";
            ++failed;
            continue;
        }

        out.write(reinterpret_cast<const char*>(data->data()),
                  static_cast<std::streamsize>(data->size()));
        totalBytes += data->size();
        ++extracted;
    }

    std::cout << "  Done! Extracted " << extracted << " file(s), "
              << formatSize(totalBytes) << " total";
    if (failed > 0)
        std::cout << " (" << failed << " failed)";
    std::cout << ".\n  Output: " << std::filesystem::absolute(outDir).string() << "\n";
}

static void cmdRead(mpq::Storage& storage) {
    std::cout << "  Filename: ";
    std::string name;
    if (!readLine(name) || name.empty()) return;

    std::string readErr;
    auto data = storage.readFile(name, &readErr);
    if (!data) {
        std::cout << "  File not found or read failed: " << name << "\n";
        if (!readErr.empty()) std::cout << "  Reason: " << readErr << "\n";
        return;
    }
    if (data->empty() && !readErr.empty()) {
        std::cout << "  Read returned empty: " << readErr << "\n";
        return;
    }

    std::cout << "  Read " << formatSize(data->size()) << ".\n";

    // Show a hex dump of the first 256 bytes.
    size_t dumpLen = std::min<size_t>(data->size(), 256);
    std::cout << "  First " << dumpLen << " bytes:\n\n";

    for (size_t i = 0; i < dumpLen; i += 16) {
        std::cout << "    " << std::hex << std::setw(4) << std::setfill('0') << i << "  ";

        // Hex bytes.
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < dumpLen)
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned>((*data)[i + j]) << " ";
            else
                std::cout << "   ";
            if (j == 7) std::cout << " ";
        }

        // ASCII.
        std::cout << " |";
        for (size_t j = 0; j < 16 && (i + j) < dumpLen; ++j) {
            char c = static_cast<char>((*data)[i + j]);
            std::cout << (std::isprint(static_cast<unsigned char>(c)) ? c : '.');
        }
        std::cout << "|\n";
    }
    std::cout << std::dec;

    if (data->size() > dumpLen)
        std::cout << "    ... (" << (data->size() - dumpLen) << " more bytes)\n";
}

static void cmdAdd(mpq::Storage& storage) {
    std::cout << "  Path to file on disk: ";
    std::string diskPath;
    if (!readLine(diskPath) || diskPath.empty()) return;

    if (!std::filesystem::is_regular_file(diskPath)) {
        std::cout << "  Not a valid file: " << diskPath << "\n";
        return;
    }

    std::cout << "  Archive name (e.g. scripts\\common.j): ";
    std::string archiveName;
    if (!readLine(archiveName) || archiveName.empty()) return;

    // Read the file from disk.
    std::ifstream in(diskPath, std::ios::binary | std::ios::ate);
    if (!in) {
        std::cout << "  Failed to open: " << diskPath << "\n";
        return;
    }
    auto fileSize = in.tellg();
    in.seekg(0);
    std::vector<whiteout::u8> fileData(static_cast<size_t>(fileSize));
    in.read(reinterpret_cast<char*>(fileData.data()), fileSize);

    // Ask for compression.
    std::cout << "  Compression [1=None, 2=Zlib (default), 3=PKware]: ";
    std::string compStr;
    readLine(compStr);
    mpq::WriteOptions opts;
    if (compStr == "1") opts.compression = mpq::Compression::None;
    else if (compStr == "3") opts.compression = mpq::Compression::PKware;
    else opts.compression = mpq::Compression::Zlib;

    if (!storage.writeFile(archiveName, std::span<const whiteout::u8>(fileData), opts)) {
        std::cout << "  Failed to write (hash table full?).\n";
        return;
    }

    std::cout << "  Added \"" << archiveName << "\" (" << formatSize(fileData.size())
              << ") to overlay. Use 'save' to persist.\n";
}

static void cmdDelete(mpq::Storage& storage) {
    std::cout << "  Filename to delete: ";
    std::string name;
    if (!readLine(name) || name.empty()) return;

    if (storage.deleteFile(name))
        std::cout << "  Marked \"" << name << "\" for deletion. Use 'save' to persist.\n";
    else
        std::cout << "  File not found: " << name << "\n";
}

static void cmdSave(mpq::Storage& storage, const std::string& originalPath) {
    std::cout << "  Save to [Enter = original path, or specify new path]: ";
    std::string savePath;
    readLine(savePath);

    bool ok;
    if (savePath.empty())
        ok = storage.save();
    else
        ok = storage.save(savePath);

    if (ok)
        std::cout << "  Archive saved successfully.\n";
    else
        std::cout << "  Save failed.\n";
}

static void cmdStats(mpq::Storage& storage) {
    auto files = storage.listFiles();

    std::map<std::string, size_t> extCounts;
    uint64_t totalUncompressed = 0, totalCompressed = 0;

    for (const auto& name : files) {
        // Extension.
        std::string ext;
        auto dot = name.rfind('.');
        if (dot != std::string::npos)
            ext = name.substr(dot);
        else
            ext = "(no ext)";
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        ++extCounts[ext];

        // Sizes.
        auto fi = storage.fileInfo(name);
        if (fi) {
            totalUncompressed += fi->uncompressedSize;
            totalCompressed += fi->compressedSize;
        }
    }

    // Sort by count descending.
    std::vector<std::pair<std::string, size_t>> sorted(extCounts.begin(), extCounts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return b.second < a.second; });

    std::cout << "\n  File count by extension:\n\n";
    for (const auto& [ext, cnt] : sorted)
        std::cout << "    " << std::setw(12) << ext << ": " << cnt << "\n";

    std::cout << "\n    Total files:      " << files.size()
              << "\n    Total size:       " << formatSize(totalUncompressed)
              << "\n    Compressed size:  " << formatSize(totalCompressed);
    if (totalUncompressed > 0) {
        double ratio = 100.0 * (1.0 - static_cast<double>(totalCompressed) /
                                           static_cast<double>(totalUncompressed));
        std::cout << "  (" << std::fixed << std::setprecision(1) << ratio << "% saved)";
    }
    std::cout << "\n";
}

// ============================================================================
// Diagnostics — pre-flight checks when Storage::open() fails
// ============================================================================

/// MPQ archive signature "MPQ\x1A" as a little-endian u32.
static constexpr uint32_t kMpqMagic = 0x1A51504DU;

/// Header search alignment used by the library.
static constexpr size_t kHeaderSearchAlignment = 0x200;

static void diagnoseOpenFailure(const std::string& path) {
    std::error_code ec;
    auto fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        std::cerr << "  Reason: cannot stat file (" << ec.message() << ").\n";
        return;
    }

    std::cout << "  File size: " << formatSize(fileSize) << " (" << fileSize << " bytes)\n";

    if (fileSize == 0) {
        std::cerr << "  Reason: file is empty (0 bytes).\n";
        return;
    }

    if (fileSize < 32) {
        std::cerr << "  Reason: file is too small for an MPQ header (need >= 32 bytes).\n";
        return;
    }

    // Read the first chunk to look for the MPQ signature.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "  Reason: cannot open file for reading (permission denied?).\n";
        return;
    }

    // Read up to 2 MB to search for the header.
    size_t readSize = std::min<size_t>(static_cast<size_t>(fileSize), 2 * 1024 * 1024);
    std::vector<uint8_t> buf(readSize);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(readSize));
    size_t bytesRead = static_cast<size_t>(in.gcount());

    if (bytesRead < 4) {
        std::cerr << "  Reason: could not read file contents.\n";
        return;
    }

    // Show the first 4 bytes for identification.
    std::cout << "  First 4 bytes: "
              << std::hex << std::setfill('0')
              << std::setw(2) << static_cast<unsigned>(buf[0]) << " "
              << std::setw(2) << static_cast<unsigned>(buf[1]) << " "
              << std::setw(2) << static_cast<unsigned>(buf[2]) << " "
              << std::setw(2) << static_cast<unsigned>(buf[3])
              << std::dec << " ('"
              << (std::isprint(buf[0]) ? static_cast<char>(buf[0]) : '.')
              << (std::isprint(buf[1]) ? static_cast<char>(buf[1]) : '.')
              << (std::isprint(buf[2]) ? static_cast<char>(buf[2]) : '.')
              << (std::isprint(buf[3]) ? static_cast<char>(buf[3]) : '.')
              << "')\n";

    // Search for MPQ\x1A at 0x200-aligned offsets (same algorithm as the library).
    bool foundMagic = false;
    size_t magicOffset = 0;
    for (size_t off = 0; off + 4 <= bytesRead; off += kHeaderSearchAlignment) {
        uint32_t sig = 0;
        std::memcpy(&sig, buf.data() + off, 4);
        if (sig == kMpqMagic) {
            foundMagic = true;
            magicOffset = off;
            break;
        }
    }

    if (!foundMagic) {
        // Also check offset 0 in case it's not aligned.
        uint32_t sig0 = 0;
        std::memcpy(&sig0, buf.data(), 4);
        if (sig0 == kMpqMagic) {
            foundMagic = true;
            magicOffset = 0;
        }
    }

    if (!foundMagic) {
        std::cerr << "  Reason: no MPQ signature ('MPQ\\x1A') found in the first "
                  << formatSize(bytesRead) << ".\n"
                  << "  This file may not be an MPQ archive, or the header offset exceeds\n"
                  << "  the search range. Check if the file is corrupted or a different format.\n";
        return;
    }

    std::cout << "  MPQ signature found at offset 0x" << std::hex << magicOffset << std::dec << ".\n";

    // We found the magic but open() still failed — check header fields.
    if (magicOffset + 32 > bytesRead) {
        std::cerr << "  Reason: MPQ header is truncated (signature at offset "
                  << magicOffset << " but file too short for full header).\n";
        return;
    }

    // Parse minimal header fields for diagnostics.
    auto readU32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, buf.data() + magicOffset + off, 4);
        return v;
    };
    auto readU16 = [&](size_t off) -> uint16_t {
        uint16_t v = 0;
        std::memcpy(&v, buf.data() + magicOffset + off, 2);
        return v;
    };

    uint32_t headerSize     = readU32(4);
    uint32_t archiveSize    = readU32(8);
    uint16_t formatVersion  = readU16(12);
    uint16_t sectorShift    = readU16(14);
    uint32_t hashTableOff   = readU32(16);
    uint32_t blockTableOff  = readU32(20);
    uint32_t hashTableCount = readU32(24);
    uint32_t blockTableCount= readU32(28);

    std::cout << "  Header details:\n"
              << "    Header size:       " << headerSize << " bytes\n"
              << "    Archive size:      " << archiveSize << " (" << formatSize(archiveSize) << ")\n"
              << "    Format version:    V" << (formatVersion + 1) << "\n"
              << "    Sector size shift: " << sectorShift
              << " (sector = " << (512u << sectorShift) << " bytes)\n"
              << "    Hash table:        offset 0x" << std::hex << hashTableOff
              << ", " << std::dec << hashTableCount << " entries\n"
              << "    Block table:       offset 0x" << std::hex << blockTableOff
              << ", " << std::dec << blockTableCount << " entries\n";

    // Check if tables exceed file bounds.
    uint64_t htEnd = static_cast<uint64_t>(magicOffset) + hashTableOff +
                     static_cast<uint64_t>(hashTableCount) * 16;
    uint64_t btEnd = static_cast<uint64_t>(magicOffset) + blockTableOff +
                     static_cast<uint64_t>(blockTableCount) * 16;

    if (htEnd > fileSize) {
        std::cerr << "  Reason: hash table extends past end of file (need "
                  << htEnd << " bytes, file is " << fileSize << ").\n";
    } else if (btEnd > fileSize) {
        std::cerr << "  Reason: block table extends past end of file (need "
                  << btEnd << " bytes, file is " << fileSize << ").\n";
    } else if (headerSize < 32) {
        std::cerr << "  Reason: header size (" << headerSize << ") is less than minimum (32).\n";
    } else {
        std::cerr << "  Reason: header and tables appear in-bounds but parsing failed.\n"
                  << "  Possible causes: corrupted table data, unsupported format version,\n"
                  << "  or memory-mapping failure. The file may require a format extension\n"
                  << "  not yet supported.\n";
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "=== MPQ Archive Explorer ===\n\n";
    const size_t numThreads = std::max<size_t>(1, std::thread::hardware_concurrency());
    whiteout::utils::SimpleThreadPool pool(numThreads);

    // Get archive path from argv or prompt.
    std::string archivePath;
    if (argc >= 2) {
        archivePath = argv[1];
    } else {
        std::cout << "Enter path to .mpq file (or 'new' to create empty archive): ";
        if (!readLine(archivePath) || archivePath.empty()) {
            std::cout << "Bye.\n";
            return 0;
        }
    }

    // Open or create.
    std::optional<mpq::Storage> storage;

    if (archivePath == "new" || archivePath == "NEW") {
        std::cout << "Creating new empty archive.\n";
        storage.emplace(mpq::Storage::create({}, &pool));
    } else {
        if (!std::filesystem::is_regular_file(archivePath)) {
            std::cerr << "File not found: " << archivePath << "\n";
            return 1;
        }
        archivePath = std::filesystem::absolute(archivePath).string();
        std::cout << "Opening: " << archivePath << " ...\n";
        std::string openError;
        storage = mpq::Storage::open(archivePath, &openError, &pool);
        if (!openError.empty())
            std::cerr << "  Library error: " << openError << "\n";
    }

    if (!storage || !*storage) {
        std::cerr << "Failed to open archive.\n";
        if (archivePath != "new" && archivePath != "NEW")
            diagnoseOpenFailure(archivePath);
        return 1;
    }

    std::cout << "Archive ready.\n";
    cmdInfo(*storage);

    auto files = storage->listFiles();
    std::cout << "  Files found:         " << files.size() << "\n";

    // --- Interactive loop ---
    while (true) {
        std::cout << "\n--- Commands ---\n"
                  << "  1. list [pattern]    List files (wildcard: *, ?)\n"
                  << "  2. info              Show archive info\n"
                  << "  3. file <name>       Show file details\n"
                  << "  4. read <name>       Read & hex-dump a file\n"
                  << "  5. extract           Extract files to disk\n"
                  << "  6. add               Add a file from disk\n"
                  << "  7. delete            Delete a file\n"
                  << "  8. save              Save archive\n"
                  << "  9. stats             File extension statistics\n"
                  << "  0. quit\n"
                  << "> ";

        std::string input;
        if (!readLine(input)) break;
        if (input.empty()) continue;

        // Parse command and optional argument.
        std::string cmd, arg;
        auto spacePos = input.find(' ');
        if (spacePos != std::string::npos) {
            cmd = input.substr(0, spacePos);
            arg = input.substr(spacePos + 1);
            // Trim leading spaces from arg.
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
        } else {
            cmd = input;
        }

        // Normalize command to lowercase.
        for (auto& c : cmd)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (cmd == "0" || cmd == "quit" || cmd == "q" || cmd == "exit")
            break;
        else if (cmd == "1" || cmd == "list" || cmd == "ls")
            cmdList(*storage, arg);
        else if (cmd == "2" || cmd == "info")
            cmdInfo(*storage);
        else if (cmd == "3" || cmd == "file")
            cmdFileInfo(*storage);
        else if (cmd == "4" || cmd == "read" || cmd == "hex")
            cmdRead(*storage);
        else if (cmd == "5" || cmd == "extract")
            cmdExtract(*storage);
        else if (cmd == "6" || cmd == "add")
            cmdAdd(*storage);
        else if (cmd == "7" || cmd == "delete" || cmd == "del" || cmd == "rm")
            cmdDelete(*storage);
        else if (cmd == "8" || cmd == "save")
            cmdSave(*storage, archivePath);
        else if (cmd == "9" || cmd == "stats")
            cmdStats(*storage);
        else
            std::cout << "  Unknown command. Try 'list', 'read', 'extract', etc.\n";
    }

    std::cout << "Bye.\n";
    return 0;
}
