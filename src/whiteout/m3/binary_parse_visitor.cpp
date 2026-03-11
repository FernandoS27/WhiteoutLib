// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../common/binary_reader.h"
#include "binary_parse_visitor.h"
#include "chunk_traits.h"

#include <array>
#include <cassert>
#include <type_traits>

namespace whiteout {
namespace m3 {

using common::BinaryReader;

// clang-format off
#include "binary_parse_visitor/common.inl"
#include "binary_parse_visitor/anim.inl"
#include "binary_parse_visitor/base.inl"
#include "binary_parse_visitor/effect.inl"
#include "binary_parse_visitor/material.inl"
#include "binary_parse_visitor/mesh.inl"
#include "binary_parse_visitor/physics.inl"
#include "binary_parse_visitor/scene.inl"
// clang-format on

} // namespace m3
} // namespace whiteout