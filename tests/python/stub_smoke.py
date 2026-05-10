# SPDX-License-Identifier: BSD-3-Clause
# Type-check smoke for the generated .pyi stubs. Imports a representative
# slice of the API and exercises the typed surface; a `mypy` run on this
# file regresses any drift between the runtime extension and the stubs.
#
# Run via:
#     mypy --explicit-package-bases tests/python/stub_smoke.py

from __future__ import annotations

import whiteout as w
from whiteout import Quaternion, Vector3f
from whiteout.mdx import (
    Bone, Geoset, InterpolationType, Layer, Model,
    NO_PARENT, MULTIPLE_GEOSETS,
)


# ── Construction + field assignment is type-checked ──────────────────────

bone: Bone = w.mdx.Bone()
bone.geoset_id = MULTIPLE_GEOSETS
bone.geoset_animation_id = NO_PARENT

layer: Layer = w.mdx.Layer()
layer.alpha = 0.5
layer.filter_mode = w.mdx.LayerFilterMode.BLEND


# ── Math types: bound at root, not under .mdx ────────────────────────────

v: Vector3f = w.Vector3f()
v.x = 1.0
v.y = 2.0
v.z = 3.0

q: Quaternion = w.Quaternion()
q.x, q.y, q.z, q.w = 0.0, 0.0, 0.0, 1.0


# ── Tracks ────────────────────────────────────────────────────────────────

tr = w.mdx.TrackQuaternion()
tr.is_used = True
tr.interpolation_type = InterpolationType.LINEAR
tr.keys.append(q)
n: int = len(tr.keys)


# ── Model containers iterate as expected ─────────────────────────────────

model: Model = w.mdx.Model()
model.bones.append(bone)
for b in model.bones:
    _ = b.geoset_id


# ── Texture surface (root-level) ─────────────────────────────────────────

tex_data: bytes = b"\x89PNG\x0d\x0a\x1a\x0a"
# Constructors don't require args — type checker validates this.
tex: w.Texture = w.Texture()
_w: int = tex.width()
