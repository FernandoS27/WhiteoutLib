// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Interactive CASC archive explorer: open a CASC storage, browse its contents,
/// read or extract files, add/delete files, and save modifications.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>
#include <whiteout/utils/blizzard_game_finder.h>
#include <whiteout/utils/cdn_product_finder.h>
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
#include <random>
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

/// Case-insensitive check whether `text` ends with `suffix`.
static bool endsWithCI(const std::string& text, const std::string& suffix) {
    if (suffix.size() > text.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[text.size() - suffix.size() + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    }
    return true;
}

/// Extract a file to disk, creating directories as needed.
/// Returns (success, bytes_written).
static std::pair<bool, uint64_t> extractFile(
    const std::string& name, const std::vector<uint8_t>& data, const std::string& outDir)
{
    std::string safeName = name;
    for (char& c : safeName) {
        if (c == '\\') c = '/';
        // Replace characters that are invalid in Windows file paths.
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }

    try {
        auto dest = std::filesystem::path(outDir) / safeName;
        std::filesystem::create_directories(dest.parent_path());

        std::ofstream out(dest, std::ios::binary);
        if (!out) return {false, 0};

        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        return {true, data.size()};
    } catch (...) {
        return {false, 0};
    }
}

template<typename S>
static void cmdSample(S& storage) {
    std::cout << "  File extension to sample (e.g. .m2, .blp, .mdx): ";
    std::string ext;
    if (!readLine(ext) || ext.empty()) return;
    if (ext.front() != '.') ext.insert(ext.begin(), '.');

    std::cout << "  Number of files to extract: ";
    std::string countStr;
    if (!readLine(countStr) || countStr.empty()) return;
    size_t count = 0;
    try { count = std::stoul(countStr); } catch (...) {
        std::cout << "  Invalid number.\n";
        return;
    }
    if (count == 0) return;

    std::cout << "  Also extract companion files? (y/n) [n]: ";
    std::string companionStr;
    readLine(companionStr);
    bool withCompanions = (!companionStr.empty() &&
        (companionStr[0] == 'y' || companionStr[0] == 'Y'));

    std::cout << "  Output directory: ";
    std::string outDir;
    if (!readLine(outDir) || outDir.empty()) {
        std::cout << "  No output directory specified.\n";
        return;
    }

    // Collect all files matching the extension.
    auto allFiles = storage.listFiles();
    std::vector<std::string> candidates;
    for (const auto& f : allFiles) {
        if (endsWithCI(f, ext))
            candidates.push_back(f);
    }

    if (candidates.empty()) {
        std::cout << "  No files found with extension '" << ext << "'.\n";
        return;
    }

    // Random sample.
    count = std::min(count, candidates.size());
    {
        std::mt19937 rng(std::random_device{}());
        // Fisher-Yates partial shuffle: move `count` random elements to front.
        for (size_t i = 0; i < count; ++i) {
            std::uniform_int_distribution<size_t> dist(i, candidates.size() - 1);
            std::swap(candidates[i], candidates[dist(rng)]);
        }
        candidates.resize(count);
    }

    // Collect companion files if requested.
    std::vector<std::string> toExtract = candidates;

    if (withCompanions) {
        // For each sampled file, find companions:
        //   same directory + filename starts with the stem of the sampled file.
        // E.g. "dir/goblin.m2" -> stem = "goblin", dir = "dir/"
        //   matches: "dir/goblin.skel", "dir/goblin01.skin", "dir/goblin0016_002.anim"
        //   rejects: "dir/gobli01.skin" (stem doesn't match)

        // Normalize all paths to forward-slash for consistent matching.
        std::vector<std::string> allNorm;
        allNorm.reserve(allFiles.size());
        for (auto f : allFiles) {
            for (char& c : f) if (c == '\\') c = '/';
            allNorm.push_back(std::move(f));
        }

        // Sort for efficient prefix scanning.
        std::sort(allNorm.begin(), allNorm.end());

        for (const auto& primary : candidates) {
            std::string norm = primary;
            for (char& c : norm) if (c == '\\') c = '/';

            // Find directory and stem.
            auto lastSlash = norm.rfind('/');
            std::string dir = (lastSlash != std::string::npos) ? norm.substr(0, lastSlash + 1) : "";
            std::string baseName = (lastSlash != std::string::npos) ? norm.substr(lastSlash + 1) : norm;
            auto dotPos = baseName.rfind('.');
            std::string stem = (dotPos != std::string::npos) ? baseName.substr(0, dotPos) : baseName;

            if (stem.empty()) continue;

            // Build the prefix to search for: dir + stem
            std::string prefix = dir + stem;

            // Binary search to the first file >= prefix in sorted allNorm.
            auto it = std::lower_bound(allNorm.begin(), allNorm.end(), prefix,
                [](const std::string& a, const std::string& b) {
                    // Case-insensitive compare.
                    size_t len = std::min(a.size(), b.size());
                    for (size_t i = 0; i < len; ++i) {
                        char ca = std::tolower(static_cast<unsigned char>(a[i]));
                        char cb = std::tolower(static_cast<unsigned char>(b[i]));
                        if (ca != cb) return ca < cb;
                    }
                    return a.size() < b.size();
                });

            for (; it != allNorm.end(); ++it) {
                const auto& f = *it;
                // Must still be in the same directory and start with stem.
                if (f.size() < prefix.size()) break;

                // Case-insensitive prefix check.
                bool prefixMatch = true;
                for (size_t i = 0; i < prefix.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(f[i])) !=
                        std::tolower(static_cast<unsigned char>(prefix[i]))) {
                        prefixMatch = false;
                        break;
                    }
                }
                if (!prefixMatch) break;

                // After the prefix, the next char must NOT be '/' (must stay in same dir).
                // Also, if there IS a next char, it must not be a letter (to avoid
                // "goblin.m2" matching "goblinFemale.m2"). It can be a digit, dot,
                // or underscore — which covers suffixes like "01.skin", ".skel",
                // "0016_002.anim".
                if (f.size() > prefix.size()) {
                    char next = f[prefix.size()];
                    if (next == '/' || next == '\\') break; // entering subdirectory
                    if (std::isalpha(static_cast<unsigned char>(next))) continue; // different name
                }

                // Skip the primary file itself.
                if (endsWithCI(f, ext) &&
                    f.size() == norm.size()) {
                    bool same = true;
                    for (size_t i = 0; i < f.size(); ++i) {
                        if (std::tolower(static_cast<unsigned char>(f[i])) !=
                            std::tolower(static_cast<unsigned char>(norm[i]))) {
                            same = false;
                            break;
                        }
                    }
                    if (same) continue;
                }

                toExtract.push_back(f);
            }
        }

        // Deduplicate.
        std::sort(toExtract.begin(), toExtract.end());
        toExtract.erase(std::unique(toExtract.begin(), toExtract.end()), toExtract.end());
    }

    std::cout << "  Extracting " << count << " '" << ext << "' file(s)";
    if (withCompanions)
        std::cout << " + " << (toExtract.size() - count) << " companion(s)";
    std::cout << " (" << toExtract.size() << " total) ...\n" << std::flush;

    std::filesystem::create_directories(outDir);
    size_t extracted = 0, failed = 0;
    uint64_t totalBytes = 0;

    for (size_t i = 0; i < toExtract.size(); ++i) {
        const auto& name = toExtract[i];

        // Print progress every 50 files.
        if (i % 50 == 0)
            std::cout << "    [" << i << "/" << toExtract.size() << "] ..." << std::flush;

        try {
            auto data = storage.readFile(name);
            if (!data) {
                ++failed;
                continue;
            }

            auto [ok, bytes] = extractFile(name, *data, outDir);
            if (!ok) {
                std::cerr << "\n    FAILED to write: " << name << "\n";
                ++failed;
                continue;
            }
            totalBytes += bytes;
            ++extracted;
        } catch (const std::exception& e) {
            std::cerr << "\n    EXCEPTION on '" << name << "': " << e.what() << "\n";
            ++failed;
        } catch (...) {
            std::cerr << "\n    UNKNOWN EXCEPTION on '" << name << "'\n";
            ++failed;
        }
    }
    std::cout << "\n";

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

static void cmdAdd(casc::StorageWritable& storage) {
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

static void cmdDelete(casc::StorageWritable& storage) {
    std::cout << "  Filename to delete: ";
    std::string name;
    if (!readLine(name) || name.empty()) return;

    if (storage.deleteFile(name))
        std::cout << "  Marked \"" << name << "\" for deletion. Use 'save' to persist.\n";
    else
        std::cout << "  File not found: " << name << "\n";
}

static void cmdSave(casc::StorageWritable& storage) {
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

    // Pre-scan for --listfile <path> (can appear anywhere in args).
    std::string listfilePath;
    std::vector<uint8_t> listfileData;
    {
        std::vector<std::string> filteredArgs;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--listfile" && i + 1 < argc) {
                listfilePath = argv[++i];
            } else {
                filteredArgs.push_back(std::move(a));
            }
        }

        // Load listfile.
        if (!listfilePath.empty()) {
            std::ifstream lf(listfilePath, std::ios::binary | std::ios::ate);
            if (!lf) {
                std::cerr << "Failed to open listfile: " << listfilePath << "\n";
                return 1;
            }
            auto sz = lf.tellg();
            lf.seekg(0);
            listfileData.resize(static_cast<size_t>(sz));
            lf.read(reinterpret_cast<char*>(listfileData.data()), sz);
            std::cout << "Listfile loaded: " << listfilePath
                      << " (" << listfileData.size() << " bytes)\n";
        }

        // Rebuild argc/argv from filtered args for positional parsing below.
        // (We just re-use the filteredArgs vector directly.)
        argc = static_cast<int>(filteredArgs.size()) + 1;  // +1 for program name
        // Store as static to keep pointers valid.
        static std::vector<std::string> s_args;
        s_args = std::move(filteredArgs);
        static std::vector<char*> s_argv;
        s_argv.clear();
        s_argv.push_back(argv[0]);
        for (auto& a : s_args) s_argv.push_back(a.data());
        argv = s_argv.data();
    }

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

    using StorageVariant = std::variant<casc::StorageWritable, casc::Storage>;
    std::optional<StorageVariant> storage;

    if (mode == Mode::Local) {
        // --- Local storage ---
        if (localPath.empty()) {
            // Scan for installed Blizzard games.
            auto games = whiteout::utils::findBlizzardGames();
            if (!games.empty()) {
                std::cout << "\n  Installed Blizzard games:\n\n";
                for (size_t i = 0; i < games.size(); ++i) {
                    std::cout << "    " << (i + 1) << ". " << games[i].name
                              << "\n       " << games[i].path << "\n";
                }
                std::cout << "\n  Enter a number to select a game,\n"
                          << "  or type a path (or 'new' to create empty storage): ";
            } else {
                std::cout << "  No installed Blizzard games found.\n"
                          << "  Enter path to CASC data directory (or 'new' to create empty storage): ";
            }
            if (!readLine(localPath) || localPath.empty()) {
                std::cout << "Bye.\n";
                return 0;
            }
            // Check if input is a numeric selection.
            bool isNumber = !localPath.empty() &&
                std::all_of(localPath.begin(), localPath.end(),
                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
            if (isNumber) {
                size_t idx = std::stoul(localPath);
                if (idx >= 1 && idx <= games.size()) {
                    localPath = games[idx - 1].path;
                } else {
                    std::cerr << "Invalid selection.\n";
                    return 1;
                }
            }
        }

        if (localPath == "new" || localPath == "NEW") {
            std::cout << "Creating new empty storage.\n";
            storage.emplace(casc::StorageWritable::create({}, &pool));
        } else {
            if (!std::filesystem::exists(localPath)) {
                std::cerr << "Path not found: " << localPath << "\n";
                return 1;
            }
            localPath = std::filesystem::absolute(localPath).string();

            // Prompt for listfile if not provided via --listfile.
            if (listfileData.empty()) {
                std::cout << "Listfile path (Enter to skip): ";
                std::string lfPath;
                readLine(lfPath);
                if (!lfPath.empty()) {
                    std::ifstream lf(lfPath, std::ios::binary | std::ios::ate);
                    if (!lf) {
                        std::cerr << "Failed to open listfile: " << lfPath << "\n";
                        return 1;
                    }
                    auto sz = lf.tellg();
                    lf.seekg(0);
                    listfileData.resize(static_cast<size_t>(sz));
                    lf.read(reinterpret_cast<char*>(listfileData.data()), sz);
                    std::cout << "Listfile loaded: " << lfPath
                              << " (" << listfileData.size() << " bytes)\n";
                }
            }

            std::cout << "Opening: " << localPath << " ...\n";

            casc::OpenOptions openOpts;
            openOpts.path = localPath;
            openOpts.pool = &pool;
            if (!listfileData.empty())
                openOpts.listfile = listfileData;

            auto local = casc::StorageWritable::open(openOpts);

            if (!local || !*local) {
                std::cerr << "Failed to open CASC storage.\n";
                std::cerr << "  lastError = " << casc::Storage::lastError() << "\n";
                return 1;
            }
            storage.emplace(std::move(*local));
        }
    } else {
        // --- Online storage ---
        httpHandler = std::make_unique<whiteout::utils::SimpleHttpHandler>(numThreads);

        if (product.empty()) {
            if (region == "us" && !modeFromArgs) {
                std::cout << "Region [us]: ";
                std::string r;
                readLine(r);
                if (!r.empty()) region = r;
            }

            std::cout << "\nFetching product list from " << region << " CDN ...\n";
            whiteout::utils::CdnProductFinderOptions findOpts;
            findOpts.http = httpHandler.get();
            findOpts.region = region;
            auto cdnProducts = whiteout::utils::findCdnProducts(findOpts);

            if (cdnProducts.empty()) {
                std::cerr << "Failed to retrieve product list from CDN.\n"
                          << "Enter product code manually: ";
                if (!readLine(product) || product.empty()) {
                    std::cout << "Bye.\n";
                    return 0;
                }
            } else {
                std::cout << "\n  Available products (" << cdnProducts.size() << "):\n\n";
                for (size_t i = 0; i < cdnProducts.size(); ++i) {
                    std::cout << "    " << std::setw(3) << std::right << (i + 1) << ". "
                              << cdnProducts[i].product;
                    if (cdnProducts[i].seqn)
                        std::cout << "  (seqn " << cdnProducts[i].seqn << ")";
                    std::cout << "\n";
                }
                std::cout << "\n  Enter a number or product code: ";
                std::string input;
                if (!readLine(input) || input.empty()) {
                    std::cout << "Bye.\n";
                    return 0;
                }
                // Check if numeric selection.
                bool isNumber = std::all_of(input.begin(), input.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
                if (isNumber) {
                    size_t idx = std::stoul(input);
                    if (idx >= 1 && idx <= cdnProducts.size())
                        product = cdnProducts[idx - 1].product;
                    else {
                        std::cerr << "Invalid selection.\n";
                        return 1;
                    }
                } else {
                    product = input;
                }
            }
        }

        // If product was given via args but region wasn't customized, offer to change it.
        if (modeFromArgs && region == "us" && argc < 4) {
            std::cout << "Region [us]: ";
            std::string r;
            readLine(r);
            if (!r.empty()) region = r;
        }

        std::string cacheDir;
        std::cout << "Cache directory (Enter to skip): ";
        readLine(cacheDir);

        // Prompt for listfile if not provided via --listfile.
        if (listfileData.empty()) {
            std::cout << "Listfile path (Enter to skip): ";
            std::string lfPath;
            readLine(lfPath);
            if (!lfPath.empty()) {
                std::ifstream lf(lfPath, std::ios::binary | std::ios::ate);
                if (!lf) {
                    std::cerr << "Failed to open listfile: " << lfPath << "\n";
                    return 1;
                }
                auto sz = lf.tellg();
                lf.seekg(0);
                listfileData.resize(static_cast<size_t>(sz));
                lf.read(reinterpret_cast<char*>(listfileData.data()), sz);
                std::cout << "Listfile loaded: " << lfPath
                          << " (" << listfileData.size() << " bytes)\n";
            }
        }

        casc::OnlineOpenOptions opts;
        opts.product = product;
        opts.region = region;
        opts.http = httpHandler.get();
        opts.pool = &pool;
        if (!cacheDir.empty())
            opts.cacheDir = cacheDir;
        if (!listfileData.empty())
            opts.listfile = listfileData;

        std::cout << "Connecting to " << region << " CDN for '" << product << "' ...\n";

        auto online = casc::Storage::openOnline(opts);
        if (!online || !*online) {
            std::cerr << "Failed to open online CASC storage.\n";
            std::cerr << "  lastError = " << casc::Storage::lastError() << "\n";
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
                  << "  13. sample           Extract random files by extension\n"
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
                if constexpr (std::is_same_v<S, casc::StorageWritable>)
                    cmdAdd(s);
                else
                    std::cout << "  Not available in online mode.\n";
            }
            else if (cmd == "8" || cmd == "delete" || cmd == "del" || cmd == "rm") {
                if constexpr (std::is_same_v<S, casc::StorageWritable>)
                    cmdDelete(s);
                else
                    std::cout << "  Not available in online mode.\n";
            }
            else if (cmd == "9" || cmd == "save") {
                if constexpr (std::is_same_v<S, casc::StorageWritable>)
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
            else if (cmd == "13" || cmd == "sample")
                cmdSample(s);
            else
                std::cout << "  Unknown command. Try 'list', 'read', 'extract', etc.\n";
        }, *storage);
    }

    std::cout << "Bye.\n";
    return 0;
}
