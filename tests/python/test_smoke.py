# SPDX-License-Identifier: BSD-3-Clause
"""Smoke tests for the native Python bindings.

Mirrors tests/wasm/smoke.test.js — same coverage on the same C++ surface
through pybind11 instead of Embind. Field, method, and constant names are
PEP 8 snake_case / UPPER_SNAKE_CASE on the Python side; the JavaScript
build keeps camelCase (matching JS conventions).

Run from the repo root:

    python -m pytest tests/python -q

Pre-requisite: build the extension with `scripts\\build-python.ps1`.
"""

from __future__ import annotations

import sys
import zlib
from pathlib import Path

# bindings/python/whiteout.<plat>.pyd is staged next to this directory's
# parent — make it importable without an install step.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'bindings' / 'python'))

import whiteout as w


def _make_png(width: int, height: int, rgb_pixels: bytes) -> bytes:
    """Build a valid 8-bit RGB PNG from raw pixel bytes (no hand-rolled CRCs)."""

    def _crc32(buf: bytes) -> int:
        return zlib.crc32(buf) & 0xFFFFFFFF

    def _chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return len(data).to_bytes(4, 'big') + body + _crc32(body).to_bytes(4, 'big')

    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = (width.to_bytes(4, 'big') + height.to_bytes(4, 'big')
            + bytes([8, 2, 0, 0, 0]))  # 8-bit, RGB, no interlace
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgb_pixels[y * stride:(y + 1) * stride])
    idat = zlib.compress(bytes(raw))
    return sig + _chunk(b'IHDR', ihdr) + _chunk(b'IDAT', idat) + _chunk(b'IEND', b'')


# ── Texture surface ────────────────────────────────────────────────────────

def test_png_roundtrip():
    parser = w.PngParser()
    tex = parser.parse(_make_png(1, 1, bytes([255, 0, 0])))
    assert tex.width() == 1
    assert tex.height() == 1

    writer = w.PngWriter()
    encoded = writer.write(tex)
    assert encoded[:8] == b'\x89PNG\r\n\x1a\n'

    re_tex = parser.parse(encoded)
    assert re_tex.width() == 1


# ── MDX (full type tree round-trip) ────────────────────────────────────────

def test_mdx_constants_exposed():
    assert w.mdx.NO_PARENT == 0xFFFFFFFF
    assert w.mdx.MULTIPLE_GEOSETS == 0xFFFFFFFF
    assert w.mdx.NO_GLOBAL_SEQUENCE == 0xFFFFFFFF


def test_mdx_full_type_tree_roundtrip():
    model = w.mdx.Model()
    model.version = 800
    model.model_name = "PyTest"
    model.blend_time = 150

    # Extent: bound as plain class; we modify by attribute.
    model.model_extent.bounds_radius = 42.0
    model.model_extent.minimum.x = -1.0

    # Append a sequence — opaque vector via bind_vector works in-place.
    seq = w.mdx.Sequence()
    seq.name = "Stand"
    seq.interval_end = 33
    model.sequences.append(seq)

    # Append a Bone with a Node.
    bone = w.mdx.Bone()
    bone.node.name = "Bone_Root"
    bone.node.parent_id = w.mdx.NO_PARENT
    bone.geoset_id = w.mdx.MULTIPLE_GEOSETS
    model.bones.append(bone)

    # Round-trip through writer.
    encoded = w.mdx.Writer().write(model)
    assert len(encoded) > 100

    parsed = w.mdx.Parser().parse(encoded)
    assert parsed.model_name == "PyTest"
    assert parsed.blend_time == 150
    assert parsed.model_extent.bounds_radius == 42.0
    assert len(parsed.sequences) == 1
    assert parsed.sequences[0].name == "Stand"
    assert parsed.sequences[0].interval_end == 33
    assert len(parsed.bones) == 1
    assert parsed.bones[0].node.name == "Bone_Root"


def test_mdx_track_quaternion():
    """vector<Quaternion> registration matters; would crash before fix #1
    in the earlier review."""
    tr = w.mdx.TrackQuaternion()
    q = w.Quaternion()
    q.x, q.y, q.z, q.w = 0.0, 0.0, 0.0, 1.0
    tr.keys.append(q)
    assert len(tr.keys) == 1
    assert tr.keys[0].w == 1.0


def test_mdx_enum_aliases_share_value():
    assert w.mdx.NodeFlag.UNSHADED.value == 0x8000
    assert w.mdx.NodeFlag.EMITTER_USES_MDL.value == 0x8000
    # Note: pybind11 deduplicates same-value enum entries on the Python side;
    # both names map to the same C++ enumerator.


# ── M2 / M3 (type tree exposed) ────────────────────────────────────────────

def test_m2_model_construction():
    m = w.m2.Model()
    m.model_name = "M2Test"
    assert m.model_name == "M2Test"
    assert len(m.bones) == 0

    bone = w.m2.Bone()
    m.bones.append(bone)
    assert len(m.bones) == 1

    # Extent value-object: minimum/maximum are Vector3f, sphere_radius is f32.
    m.bounding.sphere_radius = 5.0
    assert m.bounding.sphere_radius == 5.0


def test_m3_model_construction():
    m = w.m3.Model()
    m.name = "M3Test"
    m.skin_bone_count = 7
    assert m.name == "M3Test"
    assert m.skin_bone_count == 7

    bone = w.m3.Bone()
    m.bones.append(bone)
    assert len(m.bones) == 1


def test_m3_blendmode_enum_rich():
    # M3's BlendMode has many values; spot-check that they're UPPER_SNAKE.
    assert hasattr(w.m3, 'BlendMode')
    members = [name for name in dir(w.m3.BlendMode) if not name.startswith('_')]
    assert len(members) >= 5
    # Sanity: a couple of well-known names should be UPPER_SNAKE_CASE.
    snake_like = [n for n in members if n.isupper() or '_' in n]
    assert len(snake_like) >= 3, f"expected snake-case enum names, saw {members[:8]}"


# ── Texture parsers/writers from the codegen ──────────────────────────────

def test_blp_parse_mode_enum_snake():
    # Generated codegen names: BlpParseMode.STRICT / .LENIENT (UPPER_SNAKE).
    assert hasattr(w, 'BlpParseMode')
    assert w.BlpParseMode.STRICT.value != w.BlpParseMode.LENIENT.value
