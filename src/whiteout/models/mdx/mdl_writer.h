// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <string>
#include <whiteout/models/mdx/types.h>

namespace whiteout {
namespace mdx {

/// Convert a Model to MDL text format.
std::string writeModelToMdl(const Model& model);

} // namespace mdx
} // namespace whiteout
