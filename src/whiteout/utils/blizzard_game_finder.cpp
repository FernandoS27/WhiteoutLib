// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/utils/blizzard_game_finder.h>

#include "../common/unicode_path.h"

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <pwd.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace whiteout::utils {
namespace {

// ===========================================================================
// Shared helpers
// ===========================================================================

/// Normalize path to forward slashes and strip trailing separator.
[[maybe_unused]] std::string normalizePath(std::string p) {
    for (auto& c : p)
        if (c == '\\') c = '/';
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p;
}

/// Check that a path actually exists on disk.
[[maybe_unused]] bool directoryExists(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(common::utf8_to_path(path), ec);
}

struct GameResult {
    BlizzardGame game;
    std::string  name;
    std::string  path;
};

using Results = std::vector<GameResult>;

/// Insert a result, checking for duplicate paths.
[[maybe_unused]] void addResult(Results& results, std::unordered_set<std::string>& seen,
                      BlizzardGame game, std::string name, std::string path) {
    path = normalizePath(std::move(path));
    if (path.empty() || !directoryExists(path))
        return;

    // Lowercase key for dedup
    std::string key = path;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (seen.insert(key).second)
        results.push_back({game, std::move(name), std::move(path)});
}

/// Crude JSON string value extractor — finds "key" : "value" pairs.
[[maybe_unused]] std::string extractJsonValue(const std::string& text, size_t startPos,
                                    const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = text.find(searchKey, startPos);
    if (pos == std::string::npos)
        return {};

    pos = text.find(':', pos + searchKey.size());
    if (pos == std::string::npos)
        return {};

    pos = text.find('"', pos + 1);
    if (pos == std::string::npos)
        return {};
    ++pos;

    size_t end = text.find('"', pos);
    if (end == std::string::npos)
        return {};

    // Unescape forward-slash and backslash sequences
    std::string val = text.substr(pos, end - pos);
    std::string unescaped;
    for (size_t i = 0; i < val.size(); ++i) {
        if (val[i] == '\\' && i + 1 < val.size()) {
            char next = val[i + 1];
            if (next == '\\' || next == '/' || next == '"') {
                unescaped += next;
                ++i;
                continue;
            }
        }
        unescaped += val[i];
    }
    return unescaped;
}

// ---------------------------------------------------------------------------
// Canonical name ↔ enum mapping.
// ---------------------------------------------------------------------------

struct GameNameEntry {
    const char*  name;
    BlizzardGame game;
};

static constexpr GameNameEntry kGameNames[] = {
    {"World of Warcraft",             BlizzardGame::WorldOfWarcraft},
    {"World of Warcraft Classic",     BlizzardGame::WorldOfWarcraftClassic},
    {"World of Warcraft Classic Era", BlizzardGame::WorldOfWarcraftClassicEra},
    {"Warcraft II: Battle.net Edition", BlizzardGame::WarcraftIIBattleNetEdition},
    {"Warcraft II Remastered",        BlizzardGame::WarcraftIIRemastered},
    {"Warcraft III",                  BlizzardGame::WarcraftIII},
    {"Warcraft III Reforged",         BlizzardGame::WarcraftIIIReforged},
    {"StarCraft",                     BlizzardGame::StarCraft},
    {"StarCraft Remastered",          BlizzardGame::StarCraftRemastered},
    {"StarCraft II",                  BlizzardGame::StarCraftII},
    {"Diablo",                        BlizzardGame::Diablo},
    {"Diablo II",                     BlizzardGame::DiabloII},
    {"Diablo II Resurrected",         BlizzardGame::DiabloIIResurrected},
    {"Diablo III",                    BlizzardGame::DiabloIII},
    {"Diablo IV",                     BlizzardGame::DiabloIV},
    {"Diablo Immortal",               BlizzardGame::DiabloImmortal},
    {"Heroes of the Storm",           BlizzardGame::HeroesOfTheStorm},
    {"Overwatch 2",                   BlizzardGame::Overwatch2},
    {"Hearthstone",                   BlizzardGame::Hearthstone},
    {"Blizzard Arcade Collection",    BlizzardGame::BlizzardArcadeCollection},
    {"Battle.net",                    BlizzardGame::BattleNet},
};

// ---------------------------------------------------------------------------
// Known Battle.net product UIDs and their display names.
// ---------------------------------------------------------------------------

struct BnetProduct {
    const char*  uid;
    const char*  gameName;
    BlizzardGame game;
};

static constexpr BnetProduct kBnetProducts[] = {
    // Warcraft
    {"wow",             "World of Warcraft",             BlizzardGame::WorldOfWarcraft},
    {"wow_classic",     "World of Warcraft Classic",     BlizzardGame::WorldOfWarcraftClassic},
    {"wow_classic_era", "World of Warcraft Classic Era", BlizzardGame::WorldOfWarcraftClassicEra},
    {"w2",              "Warcraft II Remastered",        BlizzardGame::WarcraftIIRemastered},
    {"w3",              "Warcraft III Reforged",         BlizzardGame::WarcraftIIIReforged},

    // StarCraft
    {"s1",              "StarCraft Remastered",          BlizzardGame::StarCraftRemastered},
    {"s2",              "StarCraft II",                  BlizzardGame::StarCraftII},

    // Diablo
    {"d3",              "Diablo III",                    BlizzardGame::DiabloIII},
    {"osi",             "Diablo II Resurrected",         BlizzardGame::DiabloIIResurrected},
    {"fenris",          "Diablo IV",                     BlizzardGame::DiabloIV},
    {"anbs",            "Diablo Immortal",               BlizzardGame::DiabloImmortal},

    // Other Blizzard
    {"hero",            "Heroes of the Storm",           BlizzardGame::HeroesOfTheStorm},
    {"pro",             "Overwatch 2",                   BlizzardGame::Overwatch2},
    {"hs_beta",         "Hearthstone",                   BlizzardGame::Hearthstone},
    {"rtro",            "Blizzard Arcade Collection",    BlizzardGame::BlizzardArcadeCollection},
};

// ---------------------------------------------------------------------------
// Known Steam AppIDs for Blizzard-published titles.
// ---------------------------------------------------------------------------

struct SteamApp {
    const char*  appId;
    const char*  gameName;
    BlizzardGame game;
};

static constexpr SteamApp kSteamApps[] = {
    {"2357570",  "Overwatch 2",           BlizzardGame::Overwatch2},
    {"1515950",  "Diablo IV",             BlizzardGame::DiabloIV},
    {"2344520",  "Diablo II Resurrected", BlizzardGame::DiabloIIResurrected},
    {"1620",     "Warcraft III",          BlizzardGame::WarcraftIII},
    {"3042860",  "Warcraft II Remastered",BlizzardGame::WarcraftIIRemastered},
};

// ---------------------------------------------------------------------------
// Shared: Battle.net config JSON scanner
// ---------------------------------------------------------------------------

/// Scan a Battle.net config file for per-product install paths.
[[maybe_unused]] void scanBattleNetConfigFile(const fs::path& configPath,
                                    Results& results, std::unordered_set<std::string>& seen) {
    std::error_code ec;
    if (!fs::exists(configPath, ec))
        return;

    std::ifstream file(configPath);
    if (!file)
        return;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    for (auto& product : kBnetProducts) {
        std::string uidStr = std::string("\"") + product.uid + "\"";
        size_t uidPos = content.find(uidStr);
        if (uidPos == std::string::npos)
            continue;

        std::string installPath = extractJsonValue(content, uidPos, "InstallPath");
        if (installPath.empty())
            installPath = extractJsonValue(content, uidPos, "Path");
        if (!installPath.empty())
            addResult(results, seen, product.game, product.gameName, std::move(installPath));
    }
}

// ---------------------------------------------------------------------------
// Shared: product.db binary scanner
// ---------------------------------------------------------------------------

/// Scan a Battle.net product.db for embedded install paths.
/// Called from the Windows and macOS code paths; not referenced on Linux,
/// which is fine — anonymous-namespace functions don't trip -Wunused-function.
[[maybe_unused]] void scanProductDb(const fs::path& prodDb,
                   Results& results, std::unordered_set<std::string>& seen) {
    std::error_code ec;
    if (!fs::exists(prodDb, ec))
        return;

    std::ifstream dbFile(prodDb, std::ios::binary);
    if (!dbFile)
        return;

    std::string dbContent((std::istreambuf_iterator<char>(dbFile)),
                           std::istreambuf_iterator<char>());
    dbFile.close();

    for (auto& product : kBnetProducts) {
        size_t pos = 0;
        while ((pos = dbContent.find(product.uid, pos)) != std::string::npos) {
            size_t searchStart = pos + std::strlen(product.uid);
            size_t searchEnd = (std::min)(searchStart + size_t{512}, dbContent.size());

            for (size_t i = searchStart; i + 3 < searchEnd; ++i) {
                char c = dbContent[i];
#ifdef _WIN32
                // Windows: drive letter pattern  X:\  or X:/
                if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) &&
                    dbContent[i + 1] == ':' &&
                    (dbContent[i + 2] == '\\' || dbContent[i + 2] == '/')) {
#else
                // macOS: absolute path starting with /
                if (c == '/' && dbContent[i + 1] >= 0x20 && dbContent[i + 1] != '"') {
#endif
                    size_t pathStart = i;
                    size_t pathEnd = i;
                    while (pathEnd < searchEnd && dbContent[pathEnd] >= 0x20 &&
                           dbContent[pathEnd] != '"')
                        ++pathEnd;
                    std::string foundPath = dbContent.substr(pathStart, pathEnd - pathStart);
                    addResult(results, seen, product.game, product.gameName, std::move(foundPath));
                    break;
                }
            }
            pos = searchStart;
        }
    }
}

// ---------------------------------------------------------------------------
// Shared: Steam VDF parser + scanner
// ---------------------------------------------------------------------------

/// Parse a VDF file to extract library folder paths.
[[maybe_unused]] std::vector<std::string> parseSteamLibraryFolders(const fs::path& vdfPath) {
    std::vector<std::string> folders;
    std::error_code ec;
    if (!fs::exists(vdfPath, ec))
        return folders;

    std::ifstream file(vdfPath);
    if (!file)
        return folders;

    std::string line;
    while (std::getline(file, line)) {
        size_t pathKey = line.find("\"path\"");
        if (pathKey == std::string::npos)
            continue;

        size_t valStart = line.find('"', pathKey + 6);
        if (valStart == std::string::npos) continue;
        ++valStart;
        size_t valEnd = line.find('"', valStart);
        if (valEnd == std::string::npos) continue;

        std::string folderPath = line.substr(valStart, valEnd - valStart);
        // Unescape double backslashes
        std::string unescaped;
        for (size_t i = 0; i < folderPath.size(); ++i) {
            if (folderPath[i] == '\\' && i + 1 < folderPath.size() && folderPath[i + 1] == '\\') {
                unescaped += '\\';
                ++i;
            } else {
                unescaped += folderPath[i];
            }
        }
        folders.push_back(std::move(unescaped));
    }

    return folders;
}

/// Scan Steam library folders for installed Blizzard games.
[[maybe_unused]] void scanSteamLibraries(const std::string& steamRoot,
                               Results& results, std::unordered_set<std::string>& seen) {
    if (steamRoot.empty())
        return;

    fs::path vdfPath = common::utf8_to_path(steamRoot) / "steamapps" / "libraryfolders.vdf";
    std::vector<std::string> libraries = parseSteamLibraryFolders(vdfPath);
    libraries.insert(libraries.begin(), steamRoot);

    for (auto& lib : libraries) {
        fs::path steamAppsDir = common::utf8_to_path(lib) / "steamapps";

        for (auto& app : kSteamApps) {
            fs::path manifest = steamAppsDir / (std::string("appmanifest_") + app.appId + ".acf");
            std::error_code ec;
            if (!fs::exists(manifest, ec))
                continue;

            std::ifstream mf(manifest);
            if (!mf) continue;

            std::string installDir;
            std::string mfLine;
            while (std::getline(mf, mfLine)) {
                size_t key = mfLine.find("\"installdir\"");
                if (key == std::string::npos)
                    continue;
                size_t vs = mfLine.find('"', key + 12);
                if (vs == std::string::npos) continue;
                ++vs;
                size_t ve = mfLine.find('"', vs);
                if (ve == std::string::npos) continue;
                installDir = mfLine.substr(vs, ve - vs);
                break;
            }

            if (installDir.empty())
                continue;

            fs::path gamePath = steamAppsDir / "common" / installDir;
            if (fs::is_directory(gamePath, ec))
                addResult(results, seen, app.game, app.gameName, gamePath.string());
        }
    }
}

// ===========================================================================
// Windows-specific scanners
// ===========================================================================

#ifdef _WIN32

[[maybe_unused]] static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

[[maybe_unused]] std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

[[maybe_unused]] std::string readRegString(HKEY root, const wchar_t* subkey, const wchar_t* valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_32KEY, &hKey) != ERROR_SUCCESS)
        return {};

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_SZ) {
        RegCloseKey(hKey);
        return {};
    }

    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(hKey, valueName, nullptr, nullptr,
                         reinterpret_cast<BYTE*>(buf.data()), &size) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return {};
    }
    RegCloseKey(hKey);

    while (!buf.empty() && buf.back() == L'\0')
        buf.pop_back();

    return toUtf8(buf);
}

[[maybe_unused]] std::vector<std::wstring> enumSubKeys(HKEY root, const wchar_t* subkey) {
    std::vector<std::wstring> result;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_32KEY, &hKey) != ERROR_SUCCESS)
        return result;

    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD nameLen = 256;
        if (RegEnumKeyExW(hKey, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        result.emplace_back(name, nameLen);
    }
    RegCloseKey(hKey);
    return result;
}

// ---------------------------------------------------------------------------
// Windows: Registry scan
// ---------------------------------------------------------------------------

struct RegEntry {
    const wchar_t* subkey;
    const wchar_t* valueName;
    const char*    gameName;
    BlizzardGame   game;
};

static constexpr RegEntry kRegistryEntries[] = {
    {L"SOFTWARE\\Blizzard Entertainment\\World of Warcraft", L"InstallPath",
     "World of Warcraft", BlizzardGame::WorldOfWarcraft},
    {L"SOFTWARE\\Blizzard Entertainment\\Warcraft II BNE", L"InstallPath",
     "Warcraft II: Battle.net Edition", BlizzardGame::WarcraftIIBattleNetEdition},
    {L"SOFTWARE\\Blizzard Entertainment\\Warcraft II", L"InstallPath",
     "Warcraft II Remastered", BlizzardGame::WarcraftIIRemastered},
    {L"SOFTWARE\\Blizzard Entertainment\\Warcraft III", L"InstallPath",
     "Warcraft III", BlizzardGame::WarcraftIII},
    {L"SOFTWARE\\Blizzard Entertainment\\Warcraft III", L"GamePath",
     "Warcraft III", BlizzardGame::WarcraftIII},
    {L"SOFTWARE\\Blizzard Entertainment\\Warcraft III Reforged", L"InstallPath",
     "Warcraft III Reforged", BlizzardGame::WarcraftIIIReforged},
    {L"SOFTWARE\\Blizzard Entertainment\\StarCraft", L"InstallPath",
     "StarCraft", BlizzardGame::StarCraft},
    {L"SOFTWARE\\Blizzard Entertainment\\StarCraft II", L"InstallPath",
     "StarCraft II", BlizzardGame::StarCraftII},
    {L"SOFTWARE\\Blizzard Entertainment\\Heroes of the Storm", L"InstallPath",
     "Heroes of the Storm", BlizzardGame::HeroesOfTheStorm},
    {L"SOFTWARE\\Blizzard Entertainment\\Diablo", L"InstallPath",
     "Diablo", BlizzardGame::Diablo},
    {L"SOFTWARE\\Blizzard Entertainment\\Diablo II", L"InstallPath",
     "Diablo II", BlizzardGame::DiabloII},
    {L"SOFTWARE\\Blizzard Entertainment\\Diablo II Resurrected", L"InstallPath",
     "Diablo II Resurrected", BlizzardGame::DiabloIIResurrected},
    {L"SOFTWARE\\Blizzard Entertainment\\Diablo III", L"InstallPath",
     "Diablo III", BlizzardGame::DiabloIII},
    {L"SOFTWARE\\Blizzard Entertainment\\Diablo IV", L"InstallPath",
     "Diablo IV", BlizzardGame::DiabloIV},
    {L"SOFTWARE\\Blizzard Entertainment\\Diablo Immortal", L"InstallPath",
     "Diablo Immortal", BlizzardGame::DiabloImmortal},
    {L"SOFTWARE\\Blizzard Entertainment\\Overwatch", L"InstallPath",
     "Overwatch 2", BlizzardGame::Overwatch2},
    {L"SOFTWARE\\Blizzard Entertainment\\Hearthstone", L"InstallPath",
     "Hearthstone", BlizzardGame::Hearthstone},
    {L"SOFTWARE\\Blizzard Entertainment\\Blizzard Arcade Collection", L"InstallPath",
     "Blizzard Arcade Collection", BlizzardGame::BlizzardArcadeCollection},
};

[[maybe_unused]] void scanRegistry(Results& results, std::unordered_set<std::string>& seen) {
    for (auto& entry : kRegistryEntries) {
        std::string path = readRegString(HKEY_LOCAL_MACHINE, entry.subkey, entry.valueName);
        if (!path.empty())
            addResult(results, seen, entry.game, entry.gameName, std::move(path));
    }

    static constexpr const wchar_t* kUninstallKeys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    };

    for (auto* uninstallKey : kUninstallKeys) {
        for (auto& sub : enumSubKeys(HKEY_LOCAL_MACHINE, uninstallKey)) {
            std::wstring fullKey = std::wstring(uninstallKey) + L"\\" + sub;

            std::string publisher =
                readRegString(HKEY_LOCAL_MACHINE, fullKey.c_str(), L"Publisher");
            bool isBlizzard = publisher.find("Blizzard") != std::string::npos;
            if (!isBlizzard)
                continue;

            std::string name =
                readRegString(HKEY_LOCAL_MACHINE, fullKey.c_str(), L"DisplayName");
            std::string loc =
                readRegString(HKEY_LOCAL_MACHINE, fullKey.c_str(), L"InstallLocation");
            if (!name.empty() && !loc.empty()) {
                BlizzardGame g = blizzardGameFromName(name);
                addResult(results, seen, g, std::move(name), std::move(loc));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Windows: Battle.net config + product.db
// ---------------------------------------------------------------------------

[[maybe_unused]] void scanBattleNetConfig(Results& results, std::unordered_set<std::string>& seen) {
    wchar_t* appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
        fs::path configPath = fs::path(appDataPath) / "Battle.net" / "Battle.net.config";
        CoTaskMemFree(appDataPath);
        scanBattleNetConfigFile(configPath, results, seen);
    }

    wchar_t* progDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &progDataPath))) {
        fs::path agentDir = fs::path(progDataPath) / "Battle.net" / "Agent";
        CoTaskMemFree(progDataPath);
        scanProductDb(agentDir / "product.db", results, seen);
    }
}

// ---------------------------------------------------------------------------
// Windows: Steam
// ---------------------------------------------------------------------------

[[maybe_unused]] void scanSteam(Results& results, std::unordered_set<std::string>& seen) {
    std::string steamRoot =
        readRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    if (steamRoot.empty())
        steamRoot =
            readRegString(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", L"SteamPath");

    scanSteamLibraries(steamRoot, results, seen);
}

#endif // _WIN32

// ===========================================================================
// macOS-specific scanners
// ===========================================================================

#if defined(__APPLE__) || defined(__linux__)

/// Get the current user's home directory.
[[maybe_unused]] std::string getHomeDir() {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0')
        return home;

    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
        return pw->pw_dir;

    return {};
}

#endif // __APPLE__ || __linux__

#ifdef __APPLE__

// ---------------------------------------------------------------------------
// macOS: /Applications scan for known Blizzard game directories / .app bundles
// ---------------------------------------------------------------------------

struct AppEntry {
    const char*  dirName;
    const char*  gameName;
    BlizzardGame game;
};

static constexpr AppEntry kMacAppEntries[] = {
    {"World of Warcraft",           "World of Warcraft",          BlizzardGame::WorldOfWarcraft},
    {"Warcraft II",                 "Warcraft II Remastered",     BlizzardGame::WarcraftIIRemastered},
    {"Warcraft III",                "Warcraft III",               BlizzardGame::WarcraftIII},
    {"Warcraft III Reforged",       "Warcraft III Reforged",      BlizzardGame::WarcraftIIIReforged},
    {"StarCraft",                   "StarCraft",                  BlizzardGame::StarCraft},
    {"StarCraft Remastered",        "StarCraft Remastered",       BlizzardGame::StarCraftRemastered},
    {"StarCraft II",                "StarCraft II",               BlizzardGame::StarCraftII},
    {"Diablo",                      "Diablo",                     BlizzardGame::Diablo},
    {"Diablo II",                   "Diablo II",                  BlizzardGame::DiabloII},
    {"Diablo II Resurrected",       "Diablo II Resurrected",      BlizzardGame::DiabloIIResurrected},
    {"Diablo III",                  "Diablo III",                 BlizzardGame::DiabloIII},
    {"Diablo IV",                   "Diablo IV",                  BlizzardGame::DiabloIV},
    {"Heroes of the Storm",         "Heroes of the Storm",        BlizzardGame::HeroesOfTheStorm},
    {"Overwatch",                   "Overwatch 2",                BlizzardGame::Overwatch2},
    {"Hearthstone",                 "Hearthstone",                BlizzardGame::Hearthstone},
    {"Battle.net",                  "Battle.net",                 BlizzardGame::BattleNet},
    {"Blizzard Arcade Collection",  "Blizzard Arcade Collection", BlizzardGame::BlizzardArcadeCollection},
};

[[maybe_unused]] void scanApplications(Results& results, std::unordered_set<std::string>& seen) {
    for (auto& app : kMacAppEntries) {
        // Check /Applications/<name>/ (some games use plain directories)
        fs::path appDir = fs::path("/Applications") / app.dirName;
        if (directoryExists(appDir.string())) {
            addResult(results, seen, app.game, app.gameName, appDir.string());
            continue;
        }

        // Check /Applications/<name>.app (macOS app bundles)
        fs::path appBundle = fs::path("/Applications") / (std::string(app.dirName) + ".app");
        if (directoryExists(appBundle.string()))
            addResult(results, seen, app.game, app.gameName, appBundle.string());
    }
}

// ---------------------------------------------------------------------------
// macOS: Battle.net config + product.db
// ---------------------------------------------------------------------------

[[maybe_unused]] void scanBattleNetConfig(Results& results, std::unordered_set<std::string>& seen) {
    std::string home = getHomeDir();
    if (home.empty())
        return;

    // Battle.net.config lives at ~/Library/Application Support/Battle.net/
    fs::path configPath = fs::path(home) / "Library" / "Application Support" / "Battle.net" /
                          "Battle.net.config";
    scanBattleNetConfigFile(configPath, results, seen);

    // product.db may be under /Users/Shared/Battle.net/Agent/ or under
    // ~/Library/Application Support/Battle.net/Agent/
    scanProductDb(fs::path("/Users/Shared/Battle.net/Agent/product.db"), results, seen);
    scanProductDb(fs::path(home) / "Library" / "Application Support" / "Battle.net" / "Agent" /
                      "product.db",
                  results, seen);
}

// ---------------------------------------------------------------------------
// macOS: Steam
// ---------------------------------------------------------------------------

[[maybe_unused]] void scanSteam(Results& results, std::unordered_set<std::string>& seen) {
    std::string home = getHomeDir();
    if (home.empty())
        return;

    std::string steamRoot =
        (fs::path(home) / "Library" / "Application Support" / "Steam").string();
    scanSteamLibraries(steamRoot, results, seen);
}

#endif // __APPLE__

// ===========================================================================
// Linux-specific scanners
// ===========================================================================

#ifdef __linux__

/// Translate a Windows path (C:\...) to a real path inside a Wine/Proton prefix.
[[maybe_unused]] std::string translateWinePath(const std::string& winePath, const fs::path& driveC) {
    if (winePath.size() < 3)
        return {};

    char d = winePath[0];
    if (!((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z')))
        return {};
    if (winePath[1] != ':' || (winePath[2] != '\\' && winePath[2] != '/'))
        return {};
    // Only map C: drive — other drive letters are uncommon in Proton prefixes
    if (d != 'C' && d != 'c')
        return {};

    std::string rest = winePath.substr(3);
    for (auto& c : rest)
        if (c == '\\') c = '/';

    return (driveC / rest).string();
}

/// Scan a single Wine/Proton prefix for Battle.net config and product.db.
[[maybe_unused]] void scanWinePrefix(const fs::path& pfxRoot,
                           Results& results, std::unordered_set<std::string>& seen) {
    fs::path driveC = pfxRoot / "drive_c";
    std::error_code ec;
    if (!fs::is_directory(driveC, ec))
        return;

    // -- Battle.net config (under users/*/AppData/Roaming) --
    fs::path usersDir = driveC / "users";
    if (fs::is_directory(usersDir, ec)) {
        for (auto& userEntry : fs::directory_iterator(usersDir, ec)) {
            if (!userEntry.is_directory(ec))
                continue;
            fs::path configPath = userEntry.path() / "AppData" / "Roaming" /
                                  "Battle.net" / "Battle.net.config";
            if (!fs::exists(configPath, ec))
                continue;

            std::ifstream file(configPath);
            if (!file)
                continue;
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            file.close();

            for (auto& product : kBnetProducts) {
                std::string uidStr = std::string("\"" ) + product.uid + "\"";
                size_t uidPos = content.find(uidStr);
                if (uidPos == std::string::npos)
                    continue;
                std::string installPath = extractJsonValue(content, uidPos, "InstallPath");
                if (installPath.empty())
                    installPath = extractJsonValue(content, uidPos, "Path");
                if (!installPath.empty()) {
                    std::string real = translateWinePath(installPath, driveC);
                    if (!real.empty())
                        addResult(results, seen, product.game, product.gameName, std::move(real));
                }
            }
        }
    }

    // -- product.db (Windows paths inside a binary blob, translated to prefix) --
    fs::path productDb = driveC / "ProgramData" / "Battle.net" / "Agent" / "product.db";
    if (!fs::exists(productDb, ec))
        return;

    std::ifstream dbFile(productDb, std::ios::binary);
    if (!dbFile)
        return;
    std::string dbContent((std::istreambuf_iterator<char>(dbFile)),
                          std::istreambuf_iterator<char>());
    dbFile.close();

    for (auto& product : kBnetProducts) {
        size_t pos = 0;
        while ((pos = dbContent.find(product.uid, pos)) != std::string::npos) {
            size_t searchStart = pos + std::strlen(product.uid);
            size_t searchEnd = (std::min)(searchStart + size_t{512}, dbContent.size());

            for (size_t i = searchStart; i + 3 < searchEnd; ++i) {
                char c = dbContent[i];
                // Look for Windows drive letter pattern: X:\ or X:/
                if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) &&
                    dbContent[i + 1] == ':' &&
                    (dbContent[i + 2] == '\\' || dbContent[i + 2] == '/')) {
                    size_t pathStart = i;
                    size_t pathEnd = i;
                    while (pathEnd < searchEnd && dbContent[pathEnd] >= 0x20 &&
                           dbContent[pathEnd] != '"')
                        ++pathEnd;
                    std::string foundPath = dbContent.substr(pathStart, pathEnd - pathStart);
                    std::string real = translateWinePath(foundPath, driveC);
                    if (!real.empty())
                        addResult(results, seen, product.game, product.gameName, std::move(real));
                    break;
                }
            }
            pos = searchStart;
        }
    }
}

// ---------------------------------------------------------------------------
// Linux: Discover Steam root directories (native, Snap, Flatpak)
// ---------------------------------------------------------------------------

[[maybe_unused]] std::vector<std::string> findSteamRoots() {
    std::vector<std::string> roots;
    std::string home = getHomeDir();
    if (home.empty())
        return roots;

    const std::string candidates[] = {
        home + "/.steam/steam",
        home + "/.local/share/Steam",
        home + "/snap/steam/common/.local/share/Steam",
        home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
    };

    std::unordered_set<std::string> dedup;
    for (auto& c : candidates) {
        std::error_code ec;
        auto canon = fs::canonical(common::utf8_to_path(c), ec);
        if (!ec && fs::is_directory(canon, ec)) {
            std::string s = canon.string();
            if (dedup.insert(s).second)
                roots.push_back(std::move(s));
        }
    }
    return roots;
}

// ---------------------------------------------------------------------------
// Linux: Battle.net under Wine/Proton
// ---------------------------------------------------------------------------

[[maybe_unused]] void scanBattleNetConfig(Results& results, std::unordered_set<std::string>& seen) {
    std::string home = getHomeDir();
    if (home.empty())
        return;

    // Standalone Wine prefix
    scanWinePrefix(common::utf8_to_path(home) / ".wine", results, seen);

    // Proton prefixes inside each Steam library
    for (auto& root : findSteamRoots()) {
        fs::path compatDir = common::utf8_to_path(root) / "steamapps" / "compatdata";
        std::error_code ec;
        if (!fs::is_directory(compatDir, ec))
            continue;

        for (auto& entry : fs::directory_iterator(compatDir, ec)) {
            if (!entry.is_directory(ec))
                continue;
            scanWinePrefix(entry.path() / "pfx", results, seen);
        }
    }
}

// ---------------------------------------------------------------------------
// Linux: Steam
// ---------------------------------------------------------------------------

[[maybe_unused]] void scanSteam(Results& results, std::unordered_set<std::string>& seen) {
    for (auto& root : findSteamRoots())
        scanSteamLibraries(root, results, seen);
}

#endif // __linux__

} // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

std::vector<BlizzardGameInfo> findBlizzardGames() {
    Results results;
    std::unordered_set<std::string> seen;

#ifdef _WIN32
    scanRegistry(results, seen);
#endif
#ifdef __APPLE__
    scanApplications(results, seen);
#endif
    scanBattleNetConfig(results, seen);
    scanSteam(results, seen);

    std::vector<BlizzardGameInfo> out;
    out.reserve(results.size());
    for (auto& r : results)
        out.push_back({r.game, std::move(r.name), std::move(r.path)});
    return out;
}

BlizzardGame blizzardGameFromName(const std::string& name) {
    for (auto& entry : kGameNames)
        if (name == entry.name)
            return entry.game;
    return BlizzardGame::Unknown;
}

const char* blizzardGameToName(BlizzardGame game) {
    for (auto& entry : kGameNames)
        if (entry.game == game)
            return entry.name;
    return "";
}

} // namespace whiteout::utils

#else // !_WIN32 && !__APPLE__ && !__linux__

namespace whiteout::utils {

std::vector<BlizzardGameInfo> findBlizzardGames() {
    return {};
}

BlizzardGame blizzardGameFromName(const std::string&) {
    return BlizzardGame::Unknown;
}

const char* blizzardGameToName(BlizzardGame) {
    return "";
}

} // namespace whiteout::utils

#endif
