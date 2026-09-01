// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// Shared corpus plumbing for the WEM sweeps.
///
/// Every one of them needs the same four things, and each of the four is a trap
/// somebody already fell into:
///
/// - **`pathText`** — `fs::path::string()` narrows through the active code page
///   and *throws* on a name it cannot map. The WC3 corpus is full of non-ASCII
///   directory names, so every path-to-text conversion goes through here.
/// - **`gather`** — collects and **sorts**, so a sweep with a limit visits the
///   same files on every machine rather than whatever the directory iterator
///   happened to hand back first.
/// - **`sweepLimit`** — the corpus is tens of gigabytes; the default caps are
///   what makes these tests runnable at all, and `WEM_CORPUS_LIMIT` raises them.
/// - **`isKnownBad`** — files the *existing* parsers cannot handle. Naming them
///   is how a sweep measures what it can and says plainly what it could not.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include "test_helpers.h"

namespace whiteout::test {

namespace fs = std::filesystem;

/// Corpus files the existing format parsers cannot handle, with the symptom.
/// Pre-existing defects a sweep surfaced, not regressions of anything in WEM.
struct KnownBadFile {
    const char* filename;
    const char* symptom;
};

inline constexpr KnownBadFile kKnownBad[] = {
    {"Aris.mdx", "mdx::Parser does not terminate (7.8 MB v800 file)"},
    {"Aris.fixed.mdx", "mdx::Parser does not terminate (same content, 'fixed' copy)"},
    // Found by raising WEM_CORPUS_LIMIT past the old default of 400: the parser
    // allocates without bound and the sweep dies on std::bad_alloc, taking the
    // whole run with it rather than failing one file. Same shape as the Aris
    // pair — a 7.8 MB v800 file and its 'fixed' copy — and the parse-only P1
    // geometry sweep dies on it identically, so it is the parser and not
    // anything above it.
    {"hero_RyugeKisaki_A.fixed.mdx", "mdx::Parser allocates without bound (7.8 MB v800 file)"},
    {"hero_RyugeKisaki_A.mdx", "mdx::Parser allocates without bound (same content)"},
};

inline bool isKnownBad(const fs::path& path) {
    for (const KnownBadFile& entry : kKnownBad) {
        if (path.filename() == entry.filename) {
            return true;
        }
    }
    return false;
}

/// UTF-8 text for a path. See the file comment — `string()` throws here.
inline std::string pathText(const fs::path& path) {
    // `generic_u8string`, not `u8string`: the M2 parser resolves `.skin`
    // siblings through a virtual filesystem that expects forward slashes, and a
    // native-separator path silently resolves to no skin profiles at all.
    const std::u8string utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

/// With `WEM_SWEEP_TRACE` set, name each file before touching it, unbuffered —
/// so a parser that dies on one file says which.
inline void trace(const fs::path& path) {
    static const bool enabled = std::getenv("WEM_SWEEP_TRACE") != nullptr;
    if (enabled) {
        std::cerr << pathText(path) << std::endl;
    }
}

inline std::vector<u8> readCorpusFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
}

inline std::size_t sweepLimit(std::size_t available, std::size_t fallback) {
    std::size_t limit = std::min(available, fallback);
    if (const char* env = std::getenv("WEM_CORPUS_LIMIT"); env != nullptr && *env != '\0') {
        limit = std::min<std::size_t>(available, std::strtoull(env, nullptr, 10));
    }
    return limit;
}

inline std::vector<fs::path> gather(const char* envVar, const char* extension,
                                    std::initializer_list<const char*> subdirs) {
    std::vector<fs::path> files;
    const auto collect = [&](const fs::path& root) {
        if (!fs::is_directory(root)) {
            return;
        }
        std::error_code error;
        for (fs::recursive_directory_iterator it(
                 root, fs::directory_options::skip_permission_denied, error);
             it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (error) {
                break;
            }
            if (it->is_regular_file(error)) {
                std::string ext = pathText(it->path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == extension) {
                    files.push_back(it->path());
                }
            }
        }
    };

    if (const char* env = std::getenv(envVar); env != nullptr && fs::is_directory(env)) {
        collect(env);
    } else {
        const std::string base = findCorpusBase("Corpus");
        if (!base.empty()) {
            for (const char* sub : subdirs) {
                collect(fs::path(base) / sub);
            }
        }
    }
    // Absolute, because `findCorpusBase` can return a relative root and the M2
    // parser resolves `.skin` siblings against the filesystem root it was given —
    // a relative pair parses to an empty model rather than failing loudly.
    std::error_code absError;
    for (fs::path& file : files) {
        fs::path resolved = fs::absolute(file, absError);
        if (!absError) {
            file = resolved.lexically_normal();
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace whiteout::test
