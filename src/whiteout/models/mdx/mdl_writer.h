// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <string>
#include <whiteout/models/mdx/types.h>
#include <whiteout/models/mdx/writer.h>

namespace whiteout {
namespace mdx {

/// Convert a Model to MDL text format.
/// @param model   The model to serialize.
/// @param format  Which MDL dialect to emit (default: engine-faithful).
std::string writeModelToMdl(const Model& model,
                            MdlFormat format = MdlFormat::WarcraftIII);

} // namespace mdx
} // namespace whiteout
