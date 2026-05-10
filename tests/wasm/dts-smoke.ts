// SPDX-License-Identifier: BSD-3-Clause
// TypeScript usage smoke test — type-check only, no runtime.
//
// Verifies the generated `.d.ts` shape matches how callers actually use
// the library. Run via:
//
//     npx tsc -p tsconfig.dts-check.json
//
// (already wired to include this file; if any usage below stops type-
// checking, the codegen has regressed.)

import type {
    WhiteoutAPI, MdxNamespace, M2Namespace, M3Namespace,
    Vector3f, Quaternion, Texture, EnumValue,
} from "../../packages/js-ts/index";
import type * as mdx from "../../packages/js-ts/types/mdx";


// ── 1. Constructing types feels natural ────────────────────────────────

declare const whiteout: WhiteoutAPI;

const bone:     mdx.Bone     = new whiteout.mdx.Bone();
const sequence: mdx.Sequence = new whiteout.mdx.Sequence();
const layer:    mdx.Layer    = new whiteout.mdx.Layer();

// Field access is type-checked.
sequence.name          = "Stand";
sequence.intervalStart = 0;
sequence.intervalEnd   = 33;
sequence.flags         = whiteout.mdx.SequenceFlag.NonLooping;

// Math types come through as plain JS objects.
const pivot: Vector3f = { x: 0, y: 1, z: 2 };
sequence.extent = {
    boundsRadius: 5,
    minimum: { x: -1, y: -1, z: -1 },
    maximum: pivot,
};


// ── 2. Constants and sentinels are reachable ──────────────────────────

bone.node.parentId     = whiteout.mdx.NoParent;          // type: number
bone.geosetId          = whiteout.mdx.MultipleGeosets;
bone.geosetAnimationId = whiteout.mdx.MultipleGeosets;


// ── 3. Enums offer autocomplete + per-enum branding ───────────────────

layer.filterMode = whiteout.mdx.LayerFilterMode.Blend;
layer.shader     = whiteout.mdx.LayerShaderType.HD;

// Bitwise composition via the helper (raw `|` would yield NaN on enum
// wrappers).
const composed: number = whiteout.flags(
    whiteout.mdx.NodeFlag.Bone,
    whiteout.mdx.NodeFlag.Billboarded,
);


// ── 4. Vector containers expose the Embind iteration API ──────────────

bone.geosetId = 0;
const seqs = sequence as any;  // not actually a vector; just here to demo
const model = new whiteout.mdx.Model();
model.modelName = "test";
model.bones.push_back(bone);
const len: number = model.bones.size();
const first: mdx.Bone = model.bones.get(0);


// ── 5. Texture surface ────────────────────────────────────────────────

declare const blpBytes: Uint8Array;
const tex: Texture = whiteout.blp.parse(blpBytes);
const w: number = tex.width();
const h: number = tex.height();
const pngBytes: Uint8Array = whiteout.png.write(tex);


// ── 6. M2 multi-file parse ────────────────────────────────────────────

declare const m2Files: Record<string, Uint8Array>;
const m2model = whiteout.m2.parse(m2Files, "models/character.m2");
const m2Bone: any = new whiteout.m2.Bone();


// ── 7. Track instantiations are typed ─────────────────────────────────

const track = new whiteout.mdx.TrackQuaternion();
track.isUsed   = true;
track.keyCount = 2;
const q: Quaternion = { x: 0, y: 0, z: 0, w: 1 };
track.keys.push_back(q);


// All the references above MUST type-check. If `tsc` fails, the public
// surface has drifted from the codegen output.
export {};
