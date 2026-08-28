// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file txtr.h
 * @brief Main header for the Overwatch TXTR texture library.
 *
 * Include this single header to access parsing, type definitions, and the
 * payload-GUID helpers for the Overwatch `004` / `04D` texture pair.
 *
 * @example Basic Usage
 * @code
 * #include <whiteout/textures/txtr/txtr.h>
 *
 * txtr::Parser parser;
 *
 * // Ask the header which payloads it needs, fetch them from the storage.
 * std::vector<u64> guids = txtr::Parser::payloadGuids(headerBytes, textureGuid);
 * std::vector<std::vector<u8>> files;
 * for (u64 guid : guids)
 *     files.push_back(storage.read(guid));
 *
 * std::vector<std::span<const u8>> payloads(files.begin(), files.end());
 *
 * txtr::TxtrInfo info;
 * auto texture = parser.parse(headerBytes, payloads, &info);
 * @endcode
 */

#include <whiteout/textures/txtr/parser.h>
#include <whiteout/textures/txtr/types.h>

namespace whiteout {
namespace txtr = textures::txtr;
}
