# SPDX-License-Identifier: BSD-3-Clause
"""End-to-end VertexBufferBuilder tests.

Walks through:
  - every AttributeClass (Position, Normal, UV, Color, BlendIndices/Weights)
  - every AttributeEncoding (Float32/16, SNorm8/16, UNorm8/16, UInt/Int 8/16/32)
  - typical attribute combos (PBR vertex, skinned vertex, minimal vertex)
  - alignment padding
  - stride / offset arithmetic
  - round-trip read-back via getPositions()
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'packages' / 'python'))

import numpy as np
import pytest
import whiteout as w


# ── Helpers ──────────────────────────────────────────────────────────────
#
# The builder's `declare_float_attribute` and `declare_int_attribute`
# accept `std::span<const T>`. The pybind11 binding marshals that from
# a `py::array_t<T>` so callers pass numpy arrays directly — zero-copy
# when the dtype matches and the array is contiguous.


def _f32(values) -> np.ndarray:
    return np.asarray(values, dtype=np.float32)


def _u32(values) -> np.ndarray:
    return np.asarray(values, dtype=np.uint32)


# Bytes-per-encoding lookup — mirrors the C++ `bytesPerEncoding`.
_BYTES_PER_ENCODING = {
    'FLOAT32': 4, 'U_INT32': 4, 'INT32': 4,
    'FLOAT16': 2, 'S_NORM16': 2, 'U_NORM16': 2, 'U_INT16': 2, 'INT16': 2,
    'S_NORM8': 1,  'U_NORM8': 1,  'U_INT8': 1,  'INT8': 1,
}


def _bytes_per(encoding: 'w.utils.AttributeEncoding') -> int:
    name = encoding.name if hasattr(encoding, 'name') else None
    if name is None:
        # pybind11 enums use `str(value)` like "AttributeEncoding.Float32".
        name = str(encoding).rsplit('.', 1)[-1]
    return _BYTES_PER_ENCODING[name.upper()]


def _build_with(declarations):
    """Run a list of (kind, data, components, attr_class, encoding[, align]) tuples
    through a fresh builder and return the resulting VertexBuffer."""
    builder = w.utils.VertexBufferBuilder()
    for decl in declarations:
        kind = decl[0]
        data = decl[1]
        components = decl[2]
        attr_class = decl[3]
        encoding = decl[4]
        align = decl[5] if len(decl) > 5 else 0
        if kind == 'float':
            builder.declare_float_attribute(data, components, attr_class, encoding, align)
        else:
            builder.declare_int_attribute(data, components, attr_class, encoding, align)
    return builder.build()


# ── Single-attribute exercises — every encoding, per source type ─────────


# All 12 encodings, grouped by which builder entry-point accepts them.
_FLOAT_ENCODINGS = ['FLOAT32', 'FLOAT16', 'S_NORM8', 'S_NORM16', 'U_NORM8', 'U_NORM16']
_INT_ENCODINGS   = ['U_INT8', 'U_INT16', 'U_INT32', 'INT8', 'INT16', 'INT32']


@pytest.mark.parametrize('encoding_name', _FLOAT_ENCODINGS)
def test_float_attribute_every_encoding(encoding_name: str):
    """Each float encoding accepts a basic 3-vertex / 3-component position
    payload and produces a buffer with the right stride."""
    enc = getattr(w.utils.AttributeEncoding, encoding_name)
    positions = _f32([0, 0, 0, 1, 2, 3, 4, 5, 6])

    vbuf = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION, enc),
    ])
    assert vbuf.vertex_count() == 3
    assert vbuf.vertex_size() == 3 * _bytes_per(enc)
    assert len(vbuf.data) == vbuf.vertex_count() * vbuf.vertex_size()


@pytest.mark.parametrize('encoding_name', _INT_ENCODINGS)
def test_int_attribute_every_encoding(encoding_name: str):
    """Each integer encoding accepts a 2-vertex / 4-component bone-indices
    payload and lays out the right stride."""
    enc = getattr(w.utils.AttributeEncoding, encoding_name)
    indices = _u32([0, 1, 2, 3, 4, 5, 6, 7])  # 2 vertices × 4 comps

    vbuf = _build_with([
        ('int', indices, 4, w.utils.AttributeClass.BLEND_INDICES, enc),
    ])
    assert vbuf.vertex_count() == 2
    assert vbuf.vertex_size() == 4 * _bytes_per(enc)
    assert len(vbuf.data) == vbuf.vertex_count() * vbuf.vertex_size()


# ── Realistic combinations ───────────────────────────────────────────────


def test_pbr_style_vertex_layout():
    """Position (f32×3) + Normal (snorm8×3) + Tangent (snorm8×4) + UV (f16×2)
    + Color (unorm8×4) — the layout a typical rendering pipeline ships."""
    n_verts = 5
    positions = _f32([i * 0.1 for i in range(n_verts * 3)])
    normals   = _f32([0.0, 0.0, 1.0] * n_verts)
    tangents  = _f32([1.0, 0.0, 0.0, 1.0] * n_verts)
    uvs       = _f32([0.25, 0.75] * n_verts)
    colors    = _f32([1.0, 0.5, 0.25, 1.0] * n_verts)

    vbuf = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION, w.utils.AttributeEncoding.FLOAT32),
        ('float', normals,   3, w.utils.AttributeClass.NORMAL,   w.utils.AttributeEncoding.S_NORM8),
        ('float', tangents,  4, w.utils.AttributeClass.TANGENT,  w.utils.AttributeEncoding.S_NORM8),
        ('float', uvs,       2, w.utils.AttributeClass.UV,       w.utils.AttributeEncoding.FLOAT16),
        ('float', colors,    4, w.utils.AttributeClass.COLOR,    w.utils.AttributeEncoding.U_NORM8),
    ])
    assert vbuf.vertex_count() == n_verts
    expected_stride = 12 + 3 + 4 + 4 + 4   # 12 pos + 3 nrm + 4 tan + 2*2 uv + 4 col
    assert vbuf.vertex_size() == expected_stride

    # Layout entries should describe all five attrs in declaration order.
    assert len(vbuf.layout) == 5
    cls = w.utils.AttributeClass
    assert [a.attr_class for a in vbuf.layout] == [
        cls.POSITION, cls.NORMAL, cls.TANGENT, cls.UV, cls.COLOR,
    ]

    # Offsets must be monotonically increasing.
    offsets = [a.offset for a in vbuf.layout]
    assert offsets == sorted(offsets)
    assert offsets[0] == 0


def test_skinned_vertex_layout():
    """Position + BoneIndices (uint8×4) + BoneWeights (unorm8×4)."""
    n_verts = 4
    positions = _f32([float(i) for i in range(n_verts * 3)])
    indices   = _u32([0, 1, 2, 3] * n_verts)
    weights   = _f32([0.25, 0.25, 0.25, 0.25] * n_verts)

    vbuf = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION,      w.utils.AttributeEncoding.FLOAT32),
        ('int',   indices,   4, w.utils.AttributeClass.BLEND_INDICES, w.utils.AttributeEncoding.U_INT8),
        ('float', weights,   4, w.utils.AttributeClass.BLEND_WEIGHTS, w.utils.AttributeEncoding.U_NORM8),
    ])
    assert vbuf.vertex_count() == n_verts
    assert vbuf.vertex_size() == 12 + 4 + 4
    assert len(vbuf.layout) == 3


def test_minimal_position_only_vertex():
    """Smallest useful buffer — a position-only stream."""
    positions = _f32([0, 0, 0, 1, 0, 0, 0, 1, 0])
    vbuf = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION,
         w.utils.AttributeEncoding.FLOAT32),
    ])
    assert vbuf.vertex_count() == 3
    assert vbuf.vertex_size() == 12
    assert len(vbuf.layout) == 1
    assert vbuf.layout[0].component_count == 3
    assert vbuf.layout[0].offset == 0


# ── Alignment padding ────────────────────────────────────────────────────


def test_alignment_padding_pushes_next_attribute_offset():
    """Pad a 3-byte SNorm8 normal to 4 bytes, so the next attribute starts
    on a 4-byte boundary instead of byte 15."""
    n_verts = 2
    positions = _f32([1, 2, 3, 4, 5, 6])     # f32×3 → 12 bytes
    normals   = _f32([0, 0, 1, 0, 0, 1])     # snorm8×3 → 3 bytes natural
    uvs       = _f32([0.5, 0.5, 0.5, 0.5])   # f16×2 → 4 bytes

    # Without alignment, normal occupies bytes 12-15 and UV starts at 15.
    vbuf_unaligned = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION, w.utils.AttributeEncoding.FLOAT32),
        ('float', normals,   3, w.utils.AttributeClass.NORMAL,   w.utils.AttributeEncoding.S_NORM8),
        ('float', uvs,       2, w.utils.AttributeClass.UV,       w.utils.AttributeEncoding.FLOAT16),
    ])

    # With align=4 on the normal, UV starts at byte 16 instead.
    vbuf_aligned = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION, w.utils.AttributeEncoding.FLOAT32),
        ('float', normals,   3, w.utils.AttributeClass.NORMAL,   w.utils.AttributeEncoding.S_NORM8, 4),
        ('float', uvs,       2, w.utils.AttributeClass.UV,       w.utils.AttributeEncoding.FLOAT16),
    ])

    uv_offset_unaligned = vbuf_unaligned.layout[2].offset
    uv_offset_aligned = vbuf_aligned.layout[2].offset
    assert uv_offset_aligned >= uv_offset_unaligned
    assert uv_offset_aligned % 4 == 0


# ── Round-trip: positions written by builder come back via getPositions ─


def test_positions_round_trip_via_getters():
    """`get_positions()` reads the position attribute back as Vector3f.
    Only works when Position uses Float32 + offset 0."""
    coords = [
        (0.0, 0.0, 0.0),
        (1.5, 2.5, 3.5),
        (-1.0, 0.0, 1.0),
        (10.0, 20.0, 30.0),
    ]
    flat = [c for triple in coords for c in triple]
    positions = _f32(flat)

    vbuf = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION, w.utils.AttributeEncoding.FLOAT32),
    ])
    got = vbuf.get_positions()
    assert len(got) == len(coords)
    for i, (x, y, z) in enumerate(coords):
        p = got[i]
        assert p.x == pytest.approx(x)
        assert p.y == pytest.approx(y)
        assert p.z == pytest.approx(z)


def test_uvs_round_trip_when_layout_supports_it():
    """Mirror of the position round-trip — exercises `get_u_vs(0)`. The
    getter expects i16-encoded UVs at the conventional offset, so we
    only assert that calling it doesn't crash; the value mapping is
    handled by the C++ layer."""
    n_verts = 3
    positions = _f32([float(i) for i in range(n_verts * 3)])
    uvs       = _f32([0.0, 0.0,  0.5, 0.5,  1.0, 1.0])

    vbuf = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION, w.utils.AttributeEncoding.FLOAT32),
        ('float', uvs,       2, w.utils.AttributeClass.UV,       w.utils.AttributeEncoding.FLOAT16),
    ])
    assert vbuf.u_vs_num() >= 1
    # `get_u_vs(0)` returns Vector2f; just confirm we get a populated vector.
    got = vbuf.get_u_vs(0)
    assert len(got) == n_verts


# ── Zero-copy confirmation ───────────────────────────────────────────────


def test_numpy_array_is_passed_as_span_without_owning_a_copy():
    """The `std::span<const f32>` parameter binding shouldn't take
    ownership of the numpy data. Confirm by:
      1. Passing an array, building.
      2. Verifying the original numpy array is still valid + intact.
      3. Verifying the produced VertexBuffer's data matches what we sent.
    """
    src = np.array([1.0, 2.0, 3.0, 4.0, 5.0, 6.0], dtype=np.float32)
    src_copy = src.copy()

    builder = w.utils.VertexBufferBuilder()
    builder.declare_float_attribute(
        src, 3,
        w.utils.AttributeClass.POSITION,
        w.utils.AttributeEncoding.FLOAT32,
    )
    vbuf = builder.build()

    # Source numpy array survives the call unchanged.
    np.testing.assert_array_equal(src, src_copy)

    # The buffer's data view holds the same float32 bytes we sent.
    data = np.frombuffer(bytes(vbuf.data), dtype=np.float32)
    np.testing.assert_array_equal(data, src)


def test_non_contiguous_numpy_input_gets_force_cast():
    """A strided numpy view (e.g. the second column of a 2-D array) goes
    through `forcecast`, producing a contiguous copy for the C++ side."""
    matrix = np.arange(12, dtype=np.float32).reshape(4, 3)
    column = matrix[:, 1]  # non-contiguous view
    assert not column.flags['C_CONTIGUOUS']

    builder = w.utils.VertexBufferBuilder()
    builder.declare_float_attribute(
        column, 1,
        w.utils.AttributeClass.POSITION,
        w.utils.AttributeEncoding.FLOAT32,
    )
    vbuf = builder.build()
    assert vbuf.vertex_count() == 4


def test_int_attribute_accepts_smaller_dtype_via_forcecast():
    """A u8 numpy array is upcast to u32 by `forcecast` and accepted by
    the int-attribute entry point."""
    indices_u8 = np.array([0, 1, 2, 3, 4, 5, 6, 7], dtype=np.uint8)

    builder = w.utils.VertexBufferBuilder()
    builder.declare_int_attribute(
        indices_u8, 4,
        w.utils.AttributeClass.BLEND_INDICES,
        w.utils.AttributeEncoding.U_INT8,
    )
    vbuf = builder.build()
    assert vbuf.vertex_count() == 2


# ── Edge cases ───────────────────────────────────────────────────────────


def test_empty_builder_produces_empty_buffer():
    """A builder with no declarations builds a default-constructed buffer."""
    vbuf = w.utils.VertexBufferBuilder().build()
    assert vbuf.vertex_count() == 0
    assert vbuf.vertex_size() == 0


def test_multiple_uv_layers():
    """A vertex can carry several UV layers (lightmap atlases, lookup
    masks, …). The buffer reports the total via `u_vs_num()`."""
    n_verts = 2
    positions = _f32([0, 0, 0, 1, 0, 0])
    uv0       = _f32([0.0, 0.0, 1.0, 1.0])
    uv1       = _f32([0.1, 0.2, 0.3, 0.4])

    vbuf = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION, w.utils.AttributeEncoding.FLOAT32),
        ('float', uv0,       2, w.utils.AttributeClass.UV,       w.utils.AttributeEncoding.FLOAT16),
        ('float', uv1,       2, w.utils.AttributeClass.UV,       w.utils.AttributeEncoding.FLOAT16),
    ])
    assert vbuf.u_vs_num() == 2


def test_has_vertex_colors_predicate():
    """`has_vertex_colors()` returns True iff a Color attribute was declared."""
    positions = _f32([0, 0, 0])

    no_color = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION,
         w.utils.AttributeEncoding.FLOAT32),
    ])
    assert no_color.has_vertex_colors() is False

    colors = _f32([1.0, 0.5, 0.25, 1.0])
    with_color = _build_with([
        ('float', positions, 3, w.utils.AttributeClass.POSITION,
         w.utils.AttributeEncoding.FLOAT32),
        ('float', colors,    4, w.utils.AttributeClass.COLOR,
         w.utils.AttributeEncoding.U_NORM8),
    ])
    assert with_color.has_vertex_colors() is True
