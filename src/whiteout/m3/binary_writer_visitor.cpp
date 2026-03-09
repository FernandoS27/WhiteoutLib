// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../common/binary_writer.h"
#include "binary_writer_visitor.h"
#include "chunk_traits.h"

#include <algorithm>
#include <numeric>
#include <type_traits>

namespace whiteout {
namespace m3 {

using common::BinaryWriter;

// clang-format off
#include "binary_writer_visitor/common.inl"
#include "binary_writer_visitor/anim.inl"
#include "binary_writer_visitor/base.inl"
#include "binary_writer_visitor/effect.inl"
#include "binary_writer_visitor/material.inl"
#include "binary_writer_visitor/mesh.inl"
#include "binary_writer_visitor/physics.inl"
#include "binary_writer_visitor/scene.inl"
// clang-format on

} // namespace m3
} // namespace whiteout
