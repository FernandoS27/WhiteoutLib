// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file progress_sink.h
/// @brief Item-level progress sink handed down to the loaders.
///
/// Internal header — kept free of storage internals so the table/CDN layers can
/// report without depending on them.
#pragma once

#include <whiteout/common_types.h>

#include <functional>
#include <string_view>

namespace whiteout::storages::casc {

/**
 * @brief Reports one completed item of a long loop.
 *
 * Loaders take this as a nullable pointer: it is null whenever the caller
 * supplied no progress callback, so an uninstrumented open pays one predictable
 * null check per item and nothing else. Call it at item granularity — one
 * `.idx` file, one archive index, one sub-manifest — never per parsed entry.
 *
 * Safe to call from worker threads and never blocks; the reporter behind it
 * drops a sample rather than make a worker wait.
 *
 * @return false once cancellation has been requested, so loops can stop early.
 */
using ProgressSink = std::function<bool(u64 done, u64 total, std::string_view object)>;

} // namespace whiteout::storages::casc
