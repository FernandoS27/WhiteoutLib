// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Interactive CASC archive explorer: open a CASC storage, browse its contents,
/// read or extract files, add/delete files, and save modifications.

#include <whiteout/storages/casc/online_storage.h>
#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_http_handler.h>
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
#include <variant>
#include <vector>

namespace casc = whiteout::storages::casc;

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

/// Format a 16-byte key as a hex string.
static std::string hexStr(const std::array<whiteout::u8, 16>& key) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (auto b : key) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0xF]);
    }
    return s;
}

/// Format content flags in a human-readable form.
static std::string formatContentFlags(whiteout::u32 flags) {
    std::string result;
    if (flags & casc::ContentFlags::Encrypted)     result += "encrypted ";
    if (flags & casc::ContentFlags::Bundle)         result += "bundle ";
    if (flags & casc::ContentFlags::NoCompression)  result += "no-compression ";
    if (flags & casc::ContentFlags::LowViolence)    result += "low-violence ";
    if (flags & casc::ContentFlags::DoNotLoad)       result += "do-not-load ";
    if (result.empty()) result = "none";
    else result.pop_back(); // trailing space
    return result;
}

/// Format locale flags in a human-readable form.
static std::string formatLocaleFlags(whiteout::u32 flags) {
    if (flags == casc::LocaleMasks::All || flags == 0)
        return "all";

    std::string result;
    if (flags & casc::LocaleMasks::enUS) result += "enUS ";
    if (flags & casc::LocaleMasks::koKR) result += "koKR ";
    if (flags & casc::LocaleMasks::frFR) result += "frFR ";
    if (flags & casc::LocaleMasks::deDE) result += "deDE ";
    if (flags & casc::LocaleMasks::zhCN) result += "zhCN ";
    if (flags & casc::LocaleMasks::esES) result += "esES ";
    if (flags & casc::LocaleMasks::zhTW) result += "zhTW ";
    if (flags & casc::LocaleMasks::enGB) result += "enGB ";
    if (flags & casc::LocaleMasks::esMX) result += "esMX ";
    if (flags & casc::LocaleMasks::ruRU) result += "ruRU ";
    if (flags & casc::LocaleMasks::ptBR) result += "ptBR ";
    if (flags & casc::LocaleMasks::itIT) result += "itIT ";
    if (flags & casc::LocaleMasks::ptPT) result += "ptPT ";
    if (result.empty()) {
        std::ostringstream oss;
        oss << "0x" << std::hex << flags;
        return oss.str();
    }
    result.pop_back(); // trailing space
    return result;
}

// ============================================================================
// Commands
// ============================================================================

template<typename S>
static void cmdInfo(S& storage) {
    auto prod = storage.product();
    auto count = storage.totalFileCount();

    std::cout << "\n  Storage Info:\n";
    if (prod) {
        std::cout << "    Product:      " << prod->name << "\n"
                  << "    Version:      " << prod->version << "\n";
        if (!prod->buildId.empty())
            std::cout << "    Build ID:     " << prod->buildId << "\n";
    }
    if (count)
        std::cout << "    Total files:  " << *count << "\n";
}

template<typename S>
static void cmdList(S& storage, const std::string& pattern) {
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
        auto sz = storage.fileSize(name);
        std::cout << "  " << name;
        if (sz)
            std::cout << "  (" << formatSize(*sz) << ")";
        std::cout << "\n";
        ++shown;
        if (shown >= 200) {
            std::cout << "  ... and " << (matched.size() - shown) << " more.\n";
            break;
        }
    }
    std::cout << "\n  " << matched.size() << " file(s) matched.\n";
}

template<typename S>
static void cmdFileInfo(S& storage) {
    std::cout << "  Filename: ";
    std::string name;
    if (!readLine(name) || name.empty()) return;

    auto info = storage.fileInfo(name);
    if (!info) {
        std::cout << "  File not found: " << name << "\n";
        return;
    }

    std::cout << "\n  File: " << (info->path.empty() ? name : info->path) << "\n"
              << "    Size:           " << formatSize(info->fileSize) << "\n"
              << "    CKey:           " << hexStr(info->cKey) << "\n"
              << "    EKey:           " << hexStr(info->eKey) << "\n"
              << "    Content flags:  " << formatContentFlags(info->contentFlags) << "\n"
              << "    Locale flags:   " << formatLocaleFlags(info->localeFlags) << "\n";

    if (info->fileDataId != casc::kInvalidId)
        std::cout << "    FileDataId:     " << info->fileDataId << "\n";
}

template<typename S>
static void cmdExtract(S& storage) {
    std::cout << "  Filename (or * for all, or wildcard pattern): ";
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

template<typename S>
static void cmdRead(S& storage) {
    std::cout << "  Filename: ";
    std::string name;
    if (!readLine(name) || name.empty()) return;

    auto data = storage.readFile(name);
    if (!data) {
        std::cout << "  File not found or read failed: " << name << "\n";
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

template<typename S>
static void cmdReadById(S& storage) {
    std::cout << "  FileDataId: ";
    std::string idStr;
    if (!readLine(idStr) || idStr.empty()) return;

    whiteout::i32 fileId;
    try {
        fileId = std::stoi(idStr);
    } catch (...) {
        std::cout << "  Invalid FileDataId.\n";
        return;
    }

    auto data = storage.readFile(fileId);
    if (!data) {
        std::cout << "  File not found for FileDataId " << fileId << ".\n";
        return;
    }

    std::cout << "  Read " << formatSize(data->size()) << " (FileDataId " << fileId << ").\n";

    // Show a hex dump of the first 256 bytes.
    size_t dumpLen = std::min<size_t>(data->size(), 256);
    std::cout << "  First " << dumpLen << " bytes:\n\n";

    for (size_t i = 0; i < dumpLen; i += 16) {
        std::cout << "    " << std::hex << std::setw(4) << std::setfill('0') << i << "  ";

        for (size_t j = 0; j < 16; ++j) {
            if (i + j < dumpLen)
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned>((*data)[i + j]) << " ";
            else
                std::cout << "   ";
            if (j == 7) std::cout << " ";
        }

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

static void cmdAdd(casc::Storage& storage) {
    std::cout << "  Path to file on disk: ";
    std::string diskPath;
    if (!readLine(diskPath) || diskPath.empty()) return;

    if (!std::filesystem::is_regular_file(diskPath)) {
        std::cout << "  Not a valid file: " << diskPath << "\n";
        return;
    }

    std::cout << "  Archive path (e.g. data\\global\\test.txt): ";
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
    std::cout << "  Compress? [Y/n]: ";
    std::string compStr;
    readLine(compStr);
    casc::WriteOptions opts;
    if (!compStr.empty() && (compStr[0] == 'n' || compStr[0] == 'N'))
        opts.compress = false;

    if (!storage.writeFile(archiveName, fileData, opts)) {
        std::cout << "  Failed to write file to overlay.\n";
        return;
    }

    std::cout << "  Added \"" << archiveName << "\" (" << formatSize(fileData.size())
              << ") to overlay. Use 'save' to persist.\n";
}

static void cmdDelete(casc::Storage& storage) {
    std::cout << "  Filename to delete: ";
    std::string name;
    if (!readLine(name) || name.empty()) return;

    if (storage.deleteFile(name))
        std::cout << "  Marked \"" << name << "\" for deletion. Use 'save' to persist.\n";
    else
        std::cout << "  File not found: " << name << "\n";
}

static void cmdSave(casc::Storage& storage) {
    std::cout << "  Save to [Enter = original location, or specify path]: ";
    std::string savePath;
    readLine(savePath);

    bool ok;
    if (savePath.empty())
        ok = storage.save();
    else
        ok = storage.save(savePath);

    if (ok)
        std::cout << "  Storage saved successfully.\n";
    else
        std::cout << "  Save failed.\n";
}

template<typename S>
static void cmdStats(S& storage) {
    auto entries = storage.listEntries();

    std::map<std::string, size_t> extCounts;
    uint64_t totalSize = 0;

    for (const auto& entry : entries) {
        std::string ext;
        if (!entry.path.empty()) {
            auto dot = entry.path.rfind('.');
            if (dot != std::string::npos)
                ext = entry.path.substr(dot);
            else
                ext = "(no ext)";
        } else {
            ext = "(no path)";
        }
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        ++extCounts[ext];
        totalSize += entry.fileSize;
    }

    // Sort by count descending.
    std::vector<std::pair<std::string, size_t>> sorted(extCounts.begin(), extCounts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return b.second < a.second; });

    std::cout << "\n  File count by extension:\n\n";
    for (const auto& [ext, cnt] : sorted)
        std::cout << "    " << std::setw(14) << ext << ": " << cnt << "\n";

    std::cout << "\n    Total entries:  " << entries.size()
              << "\n    Total size:     " << formatSize(totalSize) << "\n";
}

template<typename S>
static void cmdKeys(S& storage) {
    std::cout << "  Load encryption keys from file (path): ";
    std::string keyPath;
    if (!readLine(keyPath) || keyPath.empty()) return;

    if (storage.importKeysFromFile(keyPath))
        std::cout << "  Keys imported successfully.\n";
    else
        std::cout << "  Failed to import keys from: " << keyPath << "\n";
}

template<typename S>
static void cmdEntries(S& storage) {
    std::cout << "  Show first N entries [default 20]: ";
    std::string nStr;
    readLine(nStr);
    size_t maxShow = 20;
    if (!nStr.empty()) {
        try { maxShow = std::stoul(nStr); } catch (...) {}
    }

    size_t n = 0;
    storage.enumerate([&](const casc::EnumerateEntry& fe) {
        std::cout << "  [" << n << "] ";
        if (!fe.path.empty())
            std::cout << fe.path;
        else if (fe.fileDataId != casc::kInvalidId)
            std::cout << "(id:" << fe.fileDataId << ")";
        else
            std::cout << "(CKey:" << hexStr(fe.cKey) << ")";

        std::cout << "  size=" << formatSize(fe.fileSize)
                  << "  locale=" << formatLocaleFlags(fe.localeFlags)
                  << "\n";
        ++n;
        return n < maxShow;
    });

    auto total = storage.totalFileCount();
    if (total && *total > maxShow)
        std::cout << "  ... and " << (*total - maxShow) << " more entries.\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "=== CASC Archive Explorer ===\n\n";

    const size_t numThreads = std::max<size_t>(1, std::thread::hardware_concurrency());
    whiteout::utils::SimpleThreadPool pool(numThreads);
    std::cout << "Thread pool: " << numThreads << " workers.\n\n";

    // Determine storage mode.
    enum class Mode { Local, Online };
    Mode mode = Mode::Local;
    bool modeFromArgs = false;
    std::string localPath;
    std::string product, region = "us";

    if (argc >= 2) {
        std::string arg1 = argv[1];
        if (arg1 == "online" || arg1 == "Online" || arg1 == "ONLINE") {
            mode = Mode::Online;
            modeFromArgs = true;
            if (argc >= 3) product = argv[2];
            if (argc >= 4) region = argv[3];
        } else {
            mode = Mode::Local;
            localPath = arg1;
            modeFromArgs = true;
        }
    }

    if (!modeFromArgs) {
        std::cout << "Storage mode:\n"
                  << "  1. Local   (open from disk)\n"
                  << "  2. Online  (connect to Blizzard CDN)\n"
                  << "> ";
        std::string choice;
        if (!readLine(choice) || choice.empty()) {
            std::cout << "Bye.\n";
            return 0;
        }
        if (choice == "2" || choice == "online")
            mode = Mode::Online;
        else
            mode = Mode::Local;
    }

    // Keep the HTTP handler alive for the lifetime of the program.
    std::unique_ptr<whiteout::utils::SimpleHttpHandler> httpHandler;

    using StorageVariant = std::variant<casc::Storage, casc::OnlineStorage>;
    std::optional<StorageVariant> storage;

    if (mode == Mode::Local) {
        // --- Local storage ---
        if (localPath.empty()) {
            std::cout << "Enter path to CASC data directory (or 'new' to create empty storage): ";
            if (!readLine(localPath) || localPath.empty()) {
                std::cout << "Bye.\n";
                return 0;
            }
        }

        if (localPath == "new" || localPath == "NEW") {
            std::cout << "Creating new empty storage.\n";
            storage.emplace(casc::Storage::create({}, &pool));
        } else {
            if (!std::filesystem::exists(localPath)) {
                std::cerr << "Path not found: " << localPath << "\n";
                return 1;
            }
            localPath = std::filesystem::absolute(localPath).string();
            std::cout << "Opening: " << localPath << " ...\n";

            std::string openError;
            auto local = casc::Storage::open(localPath, &openError, &pool);
            if (!openError.empty())
                std::cerr << "  Error: " << openError << "\n";

            if (!local || !*local) {
                std::cerr << "Failed to open CASC storage.\n";
                std::cerr << "  lastError = " << casc::Storage::lastError() << "\n";
                return 1;
            }
            storage.emplace(std::move(*local));
        }
    } else {
        // --- Online storage ---
        if (product.empty()) {
            std::cout << "\n  Available products:\n\n"
                      << "    World of Warcraft:\n"
                      << "      wow              WoW (retail)\n"
                      << "      wowt             WoW (PTR)\n"
                      << "      wow_beta         WoW (beta)\n"
                      << "      wow_classic      WoW Classic\n"
                      << "      wow_classic_era  WoW Classic Era\n"
                      << "\n"
                      << "    Diablo:\n"
                      << "      d3               Diablo III\n"
                      << "      d3t              Diablo III (test)\n"
                      << "      d3cn             Diablo III (China)\n"
                      << "      fenris           Diablo IV\n"
                      << "      fenrist          Diablo IV (test)\n"
                      << "      anbs             Diablo Immortal (PC)\n"
                      << "\n"
                      << "    Overwatch:\n"
                      << "      pro              Overwatch (retail)\n"
                      << "      proc             Overwatch (China)\n"
                      << "      prot             Overwatch (test)\n"
                      << "\n"
                      << "    StarCraft:\n"
                      << "      s1               StarCraft Remastered\n"
                      << "      s1a              StarCraft: Anthology\n"
                      << "      s2               StarCraft II\n"
                      << "      s2t              StarCraft II (test)\n"
                      << "\n"
                      << "    Other:\n"
                      << "      hero             Heroes of the Storm\n"
                      << "      herot            Heroes of the Storm (test)\n"
                      << "      hsb              Hearthstone\n"
                      << "      w3               Warcraft III: Reforged\n"
                      << "      w3t              Warcraft III: Reforged (test)\n"
                      << "      rtro             Blizzard Arcade Collection\n"
                      << "      osi              Diablo II: Resurrected\n"
                      << "      d2r              Diablo II: Resurrected (alt)\n"
                      << "\n"
                      << "    Agents / Misc:\n"
                      << "      agent            Battle.net Agent\n"
                      << "      bna              Battle.net App\n"
                      << "      catalogs         Catalogs\n"
                      << "\n"
                      << "  Product code: ";
            if (!readLine(product) || product.empty()) {
                std::cout << "Bye.\n";
                return 0;
            }
        }

        if (region == "us" && !modeFromArgs) {
            std::cout << "Region [us]: ";
            std::string r;
            readLine(r);
            if (!r.empty()) region = r;
        }

        std::string cacheDir;
        std::cout << "Cache directory (Enter to skip): ";
        readLine(cacheDir);

        httpHandler = std::make_unique<whiteout::utils::SimpleHttpHandler>(numThreads);

        casc::OnlineOpenOptions opts;
        opts.product = product;
        opts.region = region;
        opts.http = httpHandler.get();
        opts.pool = &pool;
        if (!cacheDir.empty())
            opts.cacheDir = cacheDir;

        std::cout << "Connecting to " << region << " CDN for '" << product << "' ...\n";

        auto online = casc::OnlineStorage::open(opts);
        if (!online || !*online) {
            std::cerr << "Failed to open online CASC storage.\n";
            std::cerr << "  lastError = " << casc::OnlineStorage::lastError() << "\n";
            return 1;
        }
        storage.emplace(std::move(*online));
    }

    // Verify the storage is valid.
    bool valid = std::visit([](const auto& s) -> bool { return static_cast<bool>(s); }, *storage);
    if (!valid) {
        std::cerr << "Failed to open CASC storage.\n";
        return 1;
    }

    std::cout << "Storage ready.\n";
    std::visit([](auto& s) { cmdInfo(s); }, *storage);

    const bool isOnline = (mode == Mode::Online);

    // --- Interactive loop ---
    while (true) {
        std::cout << "\n--- Commands ---\n"
                  << "  1.  list [pattern]   List files (wildcard: *, ?)\n"
                  << "  2.  info             Show storage info\n"
                  << "  3.  file             Show file details (by path)\n"
                  << "  4.  read             Read & hex-dump a file (by path)\n"
                  << "  5.  readid           Read & hex-dump a file (by FileDataId)\n"
                  << "  6.  extract          Extract files to disk\n"
                  << "  7.  add              Add a file from disk" << (isOnline ? " (local only)" : "") << "\n"
                  << "  8.  delete           Delete a file" << (isOnline ? " (local only)" : "") << "\n"
                  << "  9.  save             Save storage" << (isOnline ? " (local only)" : "") << "\n"
                  << "  10. stats            File extension statistics\n"
                  << "  11. entries          Browse raw entries\n"
                  << "  12. keys             Import encryption keys\n"
                  << "  0.  quit\n"
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
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
        } else {
            cmd = input;
        }

        // Normalize command to lowercase.
        for (auto& c : cmd)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (cmd == "0" || cmd == "quit" || cmd == "q" || cmd == "exit")
            break;

        std::visit([&](auto& s) {
            using S = std::decay_t<decltype(s)>;

            if (cmd == "1" || cmd == "list" || cmd == "ls")
                cmdList(s, arg);
            else if (cmd == "2" || cmd == "info")
                cmdInfo(s);
            else if (cmd == "3" || cmd == "file")
                cmdFileInfo(s);
            else if (cmd == "4" || cmd == "read" || cmd == "hex")
                cmdRead(s);
            else if (cmd == "5" || cmd == "readid")
                cmdReadById(s);
            else if (cmd == "6" || cmd == "extract")
                cmdExtract(s);
            else if (cmd == "7" || cmd == "add") {
                if constexpr (std::is_same_v<S, casc::Storage>)
                    cmdAdd(s);
                else
                    std::cout << "  Not available in online mode.\n";
            }
            else if (cmd == "8" || cmd == "delete" || cmd == "del" || cmd == "rm") {
                if constexpr (std::is_same_v<S, casc::Storage>)
                    cmdDelete(s);
                else
                    std::cout << "  Not available in online mode.\n";
            }
            else if (cmd == "9" || cmd == "save") {
                if constexpr (std::is_same_v<S, casc::Storage>)
                    cmdSave(s);
                else
                    std::cout << "  Not available in online mode.\n";
            }
            else if (cmd == "10" || cmd == "stats")
                cmdStats(s);
            else if (cmd == "11" || cmd == "entries")
                cmdEntries(s);
            else if (cmd == "12" || cmd == "keys")
                cmdKeys(s);
            else
                std::cout << "  Unknown command. Try 'list', 'read', 'extract', etc.\n";
        }, *storage);
    }

    std::cout << "Bye.\n";
    return 0;
}
