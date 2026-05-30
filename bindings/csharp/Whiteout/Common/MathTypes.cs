// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Whiteout's math types are zero-cost aliases of System.Numerics, so
// values flow directly between Whiteout APIs and any other library that
// already speaks SIMD-friendly System.Numerics types.
//
// Layout assertions (Phase 1 deliverable per the bindings plan):
//   whiteout::Vector2f   = (float x, y)         12 bytes? no — 8 bytes
//   whiteout::Vector3f   = (float x, y, z)      12 bytes
//   whiteout::Vector4f   = (float x, y, z, w)   16 bytes
//   whiteout::Quaternion = (float x, y, z, w)   16 bytes
//   System.Numerics.Vector2/3/4 + Quaternion follow the same field
//   order and packing. The smoke tests round-trip a struct through the
//   C ABI to lock in the parity.

global using Vector2f = System.Numerics.Vector2;
global using Vector3f = System.Numerics.Vector3;
global using Vector4f = System.Numerics.Vector4;
global using Quaternion = System.Numerics.Quaternion;

namespace Whiteout.Common;

// Placeholder namespace declaration so the file parses cleanly; the
// `global using` directives above operate at the project level and don't
// need to live inside a namespace.
internal static class MathTypesMarker { }
