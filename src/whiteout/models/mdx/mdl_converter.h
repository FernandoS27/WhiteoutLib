// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/models/mdx/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace whiteout {
namespace mdx {

// Convert MDL text source into a Model. Issues are appended to the provided vector.
Model convertMdlToModel(std::string_view source, std::vector<std::string>& issues);

} // namespace mdx
} // namespace whiteout
