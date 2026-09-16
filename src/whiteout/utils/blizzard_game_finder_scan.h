// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file blizzard_game_finder_scan.h
/// @brief The byte-level half of game discovery: the install paths a Battle.net
///        product.db or Battle.net.config names, before any is checked on disk.
///
/// Separate from the scanners that read those files so it can be tested on
/// bytes, rather than on whatever the machine running the test has installed.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/utils/blizzard_game_finder.h>

#include <string>
#include <string_view>
#include <vector>

namespace whiteout::utils::game_finder_scan {

/// An install a Battle.net file names. `path` is UTF-8, exactly as stored.
struct Candidate {
    BlizzardGame game;
    std::string name;
    std::string path;
};

/// Install paths in a product.db. Drive-letter paths when @p windowsPaths
/// (Windows, and a Wine prefix before translation), absolute POSIX ones
/// otherwise.
std::vector<Candidate> productDbPaths(std::string_view db, bool windowsPaths);

/// Per-product `InstallPath` (else `Path`) values in a Battle.net.config.
std::vector<Candidate> battleNetConfigPaths(std::string_view config);

} // namespace whiteout::utils::game_finder_scan
