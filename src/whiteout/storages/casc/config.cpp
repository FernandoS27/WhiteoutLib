// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "config.h"
#include "../common/hex.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <sstream>
#include <string_view>

namespace whiteout::storages::casc {

// ============================================================================
// Helpers
// ============================================================================

static std::string_view trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
        sv.remove_suffix(1);
    return sv;
}

static std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        auto nl = text.find('\n');
        if (nl == std::string_view::npos) {
            if (!text.empty()) lines.push_back(text);
            break;
        }
        lines.push_back(text.substr(0, nl));
        text.remove_prefix(nl + 1);
    }
    return lines;
}

static std::vector<std::string_view> split(std::string_view sv, char delim) {
    std::vector<std::string_view> parts;
    while (!sv.empty()) {
        auto pos = sv.find(delim);
        if (pos == std::string_view::npos) {
            parts.push_back(sv);
            break;
        }
        parts.push_back(sv.substr(0, pos));
        sv.remove_prefix(pos + 1);
    }
    return parts;
}

static std::array<u8, 16> parseHex16(std::string_view hex) {
    hex = trim(hex);
    return storages::common::hexDecode16(hex);
}

// ============================================================================
// .build.info
// ============================================================================

std::vector<BuildInfo> parseBuildInfo(std::span<const u8> data) {
    if (data.empty()) return {};

    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    auto lines = splitLines(text);
    if (lines.size() < 2) return {};

    // Parse header: "ColumnName!TYPE:SIZE|ColumnName!TYPE:SIZE|..."
    auto headerCols = split(lines[0], '|');

    struct ColInfo {
        std::string_view name;
        std::string_view type; // STRING, DEC, HEX
    };
    std::vector<ColInfo> columns;
    for (auto& col : headerCols) {
        ColInfo ci;
        auto bang = col.find('!');
        if (bang != std::string_view::npos) {
            ci.name = col.substr(0, bang);
            auto typeStr = col.substr(bang + 1);
            auto colon = typeStr.find(':');
            ci.type = (colon != std::string_view::npos) ? typeStr.substr(0, colon) : typeStr;
        } else {
            ci.name = col;
            ci.type = "STRING";
        }
        columns.push_back(ci);
    }

    // Find column indices for the fields we care about
    auto findCol = [&](std::string_view name) -> int {
        for (size_t i = 0; i < columns.size(); ++i)
            if (columns[i].name == name) return int(i);
        return -1;
    };

    int iBranch = findCol("Branch");
    int iActive = findCol("Active");
    int iBuildKey = findCol("Build Key");
    int iCdnKey = findCol("CDN Key");
    int iCdnPath = findCol("CDN Path");
    int iVersion = findCol("Version");
    int iProduct = findCol("Product");

    std::vector<BuildInfo> results;
    for (size_t row = 1; row < lines.size(); ++row) {
        auto line = trim(lines[row]);
        if (line.empty()) continue;

        auto fields = split(line, '|');
        BuildInfo info;

        auto getField = [&](int idx) -> std::string_view {
            if (idx >= 0 && size_t(idx) < fields.size()) return trim(fields[idx]);
            return {};
        };

        if (iBranch >= 0) info.branch = std::string(getField(iBranch));
        if (iActive >= 0) info.active = (getField(iActive) == "1");
        if (iBuildKey >= 0) info.buildKey = parseHex16(getField(iBuildKey));
        if (iCdnKey >= 0) info.cdnKey = parseHex16(getField(iCdnKey));
        if (iCdnPath >= 0) info.cdnPath = std::string(getField(iCdnPath));
        if (iVersion >= 0) info.version = std::string(getField(iVersion));
        if (iProduct >= 0) info.product = std::string(getField(iProduct));

        results.push_back(std::move(info));
    }

    return results;
}

// ============================================================================
// Build Config + CDN Config (shared key=value parser)
// ============================================================================

using KVMap = std::vector<std::pair<std::string, std::string>>;

static KVMap parseKeyValueFile(std::span<const u8> data) {
    KVMap kvs;
    if (data.empty()) return kvs;

    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    auto lines = splitLines(text);

    for (auto& line : lines) {
        auto l = trim(line);
        if (l.empty() || l[0] == '#') continue;

        auto eq = l.find(" = ");
        if (eq == std::string_view::npos) continue;

        auto key = trim(l.substr(0, eq));
        auto val = trim(l.substr(eq + 3));
        kvs.emplace_back(std::string(key), std::string(val));
    }
    return kvs;
}

static std::string kvGet(const KVMap& kvs, const std::string& key) {
    for (auto& [k, v] : kvs)
        if (k == key) return v;
    return {};
}

/// For multi-value fields like "encoding = CKey EKey", get first/second token.
static std::string firstToken(const std::string& s) {
    auto pos = s.find(' ');
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

static std::string secondToken(const std::string& s) {
    auto pos = s.find(' ');
    if (pos == std::string::npos) return {};
    std::string_view rest = std::string_view(s).substr(pos + 1);
    rest = trim(rest);
    auto pos2 = rest.find(' ');
    return std::string(rest.substr(0, pos2));
}

BuildConfig parseBuildConfig(std::span<const u8> data) {
    BuildConfig cfg;
    if (data.empty()) return cfg;

    auto kvs = parseKeyValueFile(data);

    auto root = kvGet(kvs, "root");
    if (!root.empty()) cfg.rootCKey = parseHex16(root);

    auto encoding = kvGet(kvs, "encoding");
    if (!encoding.empty()) {
        cfg.encodingCKey = parseHex16(firstToken(encoding));
        auto second = secondToken(encoding);
        if (!second.empty()) cfg.encodingEKey = parseHex16(second);
    }

    auto encodingSize = kvGet(kvs, "encoding-size");
    if (!encodingSize.empty()) {
        auto tok = firstToken(encodingSize);
        std::from_chars(tok.data(), tok.data() + tok.size(), cfg.encodingSize);
    }

    auto download = kvGet(kvs, "download");
    if (!download.empty()) cfg.downloadCKey = parseHex16(firstToken(download));

    auto install = kvGet(kvs, "install");
    if (!install.empty()) cfg.installCKey = parseHex16(firstToken(install));

    cfg.buildName = kvGet(kvs, "build-name");
    cfg.buildProduct = kvGet(kvs, "build-product");
    cfg.buildUid = kvGet(kvs, "build-uid");

    auto vfsRoot = kvGet(kvs, "vfs-root");
    if (!vfsRoot.empty()) {
        cfg.vfsRootCKey = parseHex16(firstToken(vfsRoot));
        auto second = secondToken(vfsRoot);
        if (!second.empty()) cfg.vfsRootEKey = parseHex16(second);
    }

    // Collect CKey+EKey pairs of vfs-1 .. vfs-N sub-manifests.
    for (auto& [k, v] : kvs) {
        if (k.size() >= 5 && k.substr(0, 4) == "vfs-" && k != "vfs-root" &&
            k.find("-size") == std::string::npos) {
            auto ck = firstToken(v);
            auto ek = secondToken(v);
            if (!ck.empty() && !ek.empty())
                cfg.vfsSubManifests.push_back({parseHex16(ck), parseHex16(ek)});
        }
    }

    return cfg;
}

CdnConfig parseCdnConfig(std::span<const u8> data) {
    CdnConfig cdn;
    if (data.empty()) return cdn;

    auto kvs = parseKeyValueFile(data);

    auto archives = kvGet(kvs, "archives");
    if (!archives.empty()) {
        // Space-separated hex keys
        std::string_view sv(archives);
        while (!sv.empty()) {
            sv = trim(sv);
            if (sv.empty()) break;
            auto sp = sv.find(' ');
            auto token = (sp != std::string_view::npos) ? sv.substr(0, sp) : sv;
            cdn.archiveEKeys.push_back(parseHex16(token));
            if (sp == std::string_view::npos) break;
            sv.remove_prefix(sp + 1);
        }
    }

    auto indexSizes = kvGet(kvs, "archives-index-size");
    if (!indexSizes.empty()) {
        std::string_view sv(indexSizes);
        while (!sv.empty()) {
            sv = trim(sv);
            if (sv.empty()) break;
            auto sp = sv.find(' ');
            auto token = (sp != std::string_view::npos) ? sv.substr(0, sp) : sv;
            u32 val = 0;
            std::from_chars(token.data(), token.data() + token.size(), val);
            cdn.archiveIndexSizes.push_back(val);
            if (sp == std::string_view::npos) break;
            sv.remove_prefix(sp + 1);
        }
    }

    cdn.fileIndex = kvGet(kvs, "file-index");
    auto fis = kvGet(kvs, "file-index-size");
    if (!fis.empty())
        std::from_chars(fis.data(), fis.data() + fis.size(), cdn.fileIndexSize);

    return cdn;
}

// ============================================================================
// Shmem
// ============================================================================

ShmemInfo parseShmem(std::span<const u8> data) {
    ShmemInfo info;
    if (data.size() < 8) return info;

    // Shmem is a small binary struct — first u32 is version, second u32 is archive count.
    std::memcpy(&info.version, data.data(), 4);
    std::memcpy(&info.archiveCount, data.data() + 4, 4);
    return info;
}

} // namespace whiteout::storages::casc
