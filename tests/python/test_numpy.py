# SPDX-License-Identifier: BSD-3-Clause
"""Buffer-protocol / numpy zero-copy tests.

Verifies that primitive vectors and math-struct vectors (Vector2f,
Vector3f, Vector4f, Quaternion, ColorBGRA) expose `__buffer__` so
`np.asarray(vec)` returns a zero-copy ndarray that shares memory with
the underlying C++ std::vector.

Pre-requisite: build the extension with `scripts\\build-python.ps1`.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'bindings' / 'python'))

import numpy as np
import pytest
import whiteout as w


def _v3(x: float, y: float, z: float) -> 'w.Vector3f':
    v = w.Vector3f()
    v.x, v.y, v.z = x, y, z
    return v


def _v4(x: float, y: float, z: float, ww: float) -> 'w.Vector4f':
    v = w.Vector4f()
    v.x, v.y, v.z, v.w = x, y, z, ww
    return v


def _quat(x: float, y: float, z: float, ww: float) -> 'w.Quaternion':
    q = w.Quaternion()
    q.x, q.y, q.z, q.w = x, y, z, ww
    return q


# ── Primitive vectors ─────────────────────────────────────────────────────

def test_vector_u8_numpy_view():
    geo = w.mdx.Geoset()
    geo.vertex_groups.append(7)
    geo.vertex_groups.append(11)
    geo.vertex_groups.append(255)

    arr = np.asarray(geo.vertex_groups)
    assert arr.dtype == np.uint8
    assert arr.shape == (3,)
    np.testing.assert_array_equal(arr, [7, 11, 255])


def test_vector_u32_numpy_view():
    geo = w.mdx.Geoset()
    geo.face_type_groups.append(4)   # 4 = triangle list
    geo.face_type_groups.append(4)

    arr = np.asarray(geo.face_type_groups)
    assert arr.dtype in (np.uint32, np.uintc)
    assert arr.shape == (2,)
    np.testing.assert_array_equal(arr, [4, 4])


def test_vector_f32_writable_zero_copy():
    """Mutating the numpy view writes through to the C++ vector."""
    tr = w.mdx.TrackF32()
    tr.keys.append(1.0)
    tr.keys.append(2.0)
    tr.keys.append(3.0)

    arr = np.asarray(tr.keys)
    assert arr.dtype == np.float32
    assert arr.flags.writeable
    arr[1] = 99.0   # mutate the view

    # The mutation must be visible through standard Python indexing.
    assert tr.keys[1] == 99.0


# ── Math-struct vectors (Vector3f / Vector4f / Quaternion) ────────────────

def test_vector_vector3f_shape_and_dtype():
    model = w.mdx.Model()
    model.pivot_points.append(_v3(1, 2, 3))
    model.pivot_points.append(_v3(4, 5, 6))
    model.pivot_points.append(_v3(7, 8, 9))

    arr = np.asarray(model.pivot_points)
    assert arr.dtype == np.float32
    assert arr.shape == (3, 3)
    np.testing.assert_array_equal(arr, [[1, 2, 3], [4, 5, 6], [7, 8, 9]])


def test_vector_vector3f_zero_copy_writeback():
    model = w.mdx.Model()
    model.pivot_points.append(_v3(0, 0, 0))

    arr = np.asarray(model.pivot_points)
    arr[0] = [10, 20, 30]

    p = model.pivot_points[0]
    assert p.x == 10 and p.y == 20 and p.z == 30


def test_vector_vector4f_shape():
    geo = w.mdx.Geoset()
    geo.tangents.append(_v4(1, 0, 0, 1))
    geo.tangents.append(_v4(0, 1, 0, -1))

    arr = np.asarray(geo.tangents)
    assert arr.dtype == np.float32
    assert arr.shape == (2, 4)


def test_vector_quaternion_shape():
    tr = w.mdx.TrackQuaternion()
    tr.keys.append(_quat(0, 0, 0, 1))
    tr.keys.append(_quat(1, 0, 0, 0))

    arr = np.asarray(tr.keys)
    assert arr.dtype == np.float32
    assert arr.shape == (2, 4)
    np.testing.assert_array_almost_equal(arr[0], [0, 0, 0, 1])


# ── Bulk import: numpy -> C++ vector (round-trip) ─────────────────────────

def test_bulk_numpy_to_vector():
    """Common pattern: build a numpy array, copy into the C++ vector."""
    coords = np.array([[1, 2, 3], [4, 5, 6], [7, 8, 9]], dtype=np.float32)

    model = w.mdx.Model()
    for row in coords:
        model.pivot_points.append(_v3(float(row[0]), float(row[1]), float(row[2])))

    out = np.asarray(model.pivot_points)
    np.testing.assert_array_equal(out, coords)


# ── Caveat: vector resize invalidates the view ────────────────────────────

def test_resize_invalidates_view():
    """Once you grab a numpy view, growing the vector reallocates the
    underlying buffer and the view points at freed memory. This is the
    standard numpy aliasing gotcha; the test documents the expected user
    contract: re-acquire the view after any push_back."""
    tr = w.mdx.TrackF32()
    tr.keys.append(1.0)
    arr = np.asarray(tr.keys)
    assert len(arr) == 1

    # Old view is now stale — but a fresh np.asarray() is fine.
    tr.keys.append(2.0)
    arr2 = np.asarray(tr.keys)
    assert len(arr2) == 2
