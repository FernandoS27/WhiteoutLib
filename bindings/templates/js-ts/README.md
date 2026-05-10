# whiteout-wasm

WebAssembly build of [WhiteoutLib](https://github.com/...) — pure-C++ parsers
and writers for Blizzard model and texture formats.

**Scope:** models (MDX, M2, M3, WEM) and textures (BLP, DDS, PNG, JPEG, BMP,
TGA). Storage backends (CASC, MPQ) and CDN/networking are out of scope; this
build expects callers to feed raw byte buffers.

## Install

```bash
npm install whiteout-wasm
```

## API shape

The facade groups everything by format. Per-format namespaces hold the
generated types AND the parse/write convenience methods. Shared types
live at the root.

```js
import { Whiteout } from "whiteout-wasm";

const whiteout = await Whiteout();

// Per-format namespaces — types and helpers together
whiteout.mdx.Bone, whiteout.mdx.Sequence, whiteout.mdx.Layer       // classes
whiteout.mdx.NodeFlag, whiteout.mdx.SequenceFlag                   // enums
whiteout.mdx.NoParent, whiteout.mdx.MultipleGeosets                // constants
whiteout.mdx.parse(bytes), whiteout.mdx.write(model)               // helpers
whiteout.mdx.parseMdl(bytes), whiteout.mdx.writeMdl(model)         // MDL text format
whiteout.m2.Bone, whiteout.m2.parse(files, mainPath)
whiteout.m3.Bone, whiteout.m3.parse(bytes), whiteout.m3.write(model)

// Texture format facades
whiteout.blp.parse(bytes), whiteout.blp.write(tex)
whiteout.dds, whiteout.png, whiteout.jpeg, whiteout.bmp, whiteout.tga

// Shared types at root
whiteout.Texture, whiteout.PixelFormat, whiteout.TextureType
whiteout.Vector2f, whiteout.Vector3f, whiteout.Vector4f, whiteout.Quaternion
whiteout.InMemoryFileSystem    // for M2 sibling files

// Helpers
whiteout.flags(...enumValues)  // OR-combine flag enums
whiteout.module                // raw Embind module — escape hatch
```

## Usage

```js
import { Whiteout } from "whiteout-wasm";
import { readFileSync } from "node:fs";

const whiteout = await Whiteout();

// Texture round-trip: BLP -> PNG
const blpBytes = readFileSync("texture.blp");
const tex = whiteout.blp.parse(blpBytes);
console.log(`${tex.width()}x${tex.height()} ${whiteout.PixelFormat[tex.format()]}`);
const pngBytes = whiteout.png.write(tex);
tex.delete();

// MDX round-trip
const mdxBytes = readFileSync("model.mdx");
const model = whiteout.mdx.parse(mdxBytes);
console.log(`${model.modelName} v${model.version}: ${model.geosets.size()} geosets`);
const reEncoded = whiteout.mdx.write(model);
model.delete();

// Constructing a model from scratch
const bone = new whiteout.mdx.Bone();
bone.node.parentId = whiteout.mdx.NoParent;
bone.geosetId = whiteout.mdx.MultipleGeosets;

// M2 (multi-file): provide sibling .skin / .skel / .anim files
const m2Model = whiteout.m2.parse({
    "models/character.m2":     readFileSync("character.m2"),
    "models/character00.skin": readFileSync("character00.skin"),
}, "models/character.m2");
m2Model.delete();
```

## Memory model

Embind objects (Texture, mdx.Model, mdx.Bone, etc.) live in WASM linear
memory and must be released with `.delete()`. Forgetting to do so leaks
memory.

`Texture.data()` and `mdx.Geoset.vertexGroupsView()` return `Uint8Array`
views backed by the WASM heap. Copy (`new Uint8Array(view)`) before
calling any other WASM function — heap growth invalidates the view.

## Zero-copy `.view()` for primitive & math-struct vectors

Vectors of primitive types (`u8`/`u16`/`u32`/`f32`/...) and of the
standard math types (`Vector2f`, `Vector3f`, `Vector4f`, `Quaternion`,
`ColorBGRA`) expose a `.view()` method returning a JS TypedArray
aliased to the WASM heap — **no copy**, mutations write through.

```js
// Primitive vector → 1D TypedArray of length N
const keys = track.keys;                  // EmbindBufferVector<number, Float32Array>
const view = keys.view();                 // Float32Array(N), aliased to heap
view[1] = 99.0;                           // writes through to C++
keys.delete();

// Math-struct vector → flat TypedArray of length N * components
const pivots = model.pivotPoints;         // EmbindBufferVector<Vector3f, Float32Array>
const v = pivots.view();                  // Float32Array(N*3) laid out [x0,y0,z0,x1,...]
const x1 = v[1*3 + 0], y1 = v[1*3 + 1], z1 = v[1*3 + 2];
pivots.delete();
```

Element → TypedArray mapping:

| C++ element                             | JS TypedArray   |
|-----------------------------------------|-----------------|
| `u8`, `ColorBGRA`                       | `Uint8Array`    |
| `u16`                                   | `Uint16Array`   |
| `u32`                                   | `Uint32Array`   |
| `i8`/`i16`/`i32`                        | `Int8/16/32Array` |
| `f32`, `Vector2f`/`3f`/`4f`, `Quaternion` | `Float32Array` |
| `f64`                                   | `Float64Array`  |

**Caveat — same as `Texture.data()`:** any `push_back`/`resize`/heap-
growing WASM call may invalidate the view. Re-call `.view()` after
mutation, or `new Float32Array(view)` to copy out.

## Math types pass as plain JS objects

`Vector2f`, `Vector3f`, `Vector4f`, `Quaternion`, and `mdx.Extent` are
bound as Embind `value_object` — read and write them as plain
`{x, y, z, ...}` literals; no `.delete()` required.

```js
model.modelExtent = {
    boundsRadius: 42,
    minimum: { x: -1, y: -2, z: -3 },
    maximum: { x:  4, y:  5, z:  6 },
};
const pivot = model.pivotPoints.get(0);   // {x, y, z} — no .delete()
console.log(pivot.x, pivot.y, pivot.z);
```

The facade exposes `vec2`/`vec3`/`vec4`/`quat` as terse factories:

```js
model.modelExtent = {
    boundsRadius: 42,
    minimum: whiteout.vec3(-1, -2, -3),
    maximum: whiteout.vec3( 4,  5,  6),
};
const q = whiteout.quat(0, 0, 0, 1);   // identity rotation
```

## Iterating vector fields

Every vector field supports `for (const x of vec)`:

```js
for (const bone of model.bones) {
    console.log(bone.node.name);
}
```

The iterator yields **copies** (Embind semantics). Mutating the loop
variable does NOT write back — use `vec.set(i, value)` or the
read-modify-write pattern below.

## Combining flag enums

Embind enums wrap their numeric value behind `.value`; bitwise operators on
the wrappers yield `NaN`. Use the `flags()` helper:

```js
node.flags = whiteout.flags(
    whiteout.mdx.NodeFlag.Bone,
    whiteout.mdx.NodeFlag.Billboarded,
);
```

## Sentinel constants

The `0xFFFFFFFF` "no parent" / "multiple geosets" / "no global sequence"
sentinels are exposed by name on each format namespace:

```js
whiteout.mdx.NoParent           // === 0xFFFFFFFF
whiteout.mdx.MultipleGeosets    // === 0xFFFFFFFF
whiteout.mdx.NoGlobalSequence   // === 0xFFFFFFFF
```

## Mutating model fields (read-modify-write)

Embind class properties — `std::vector<T>`, `std::string`, nested class
structs — return COPIES on read and assign by value on write. To mutate a
vector field in place, use the read-modify-write pattern:

```js
// Wrong — mutates a copy that's then discarded:
model.sequences.push_back(seq);          // BUG: model.sequences unchanged

// Right — read, mutate, write back:
const seqs = model.sequences;
seqs.push_back(seq);
model.sequences = seqs;
seqs.delete();
```

A handy helper:

```js
function withField(obj, name, mutate) {
    const v = obj[name];
    try { mutate(v); obj[name] = v; } finally { v.delete(); }
}

withField(model, "sequences", (seqs) => seqs.push_back(seq));
```

This applies recursively to nested vectors (e.g. `material.layers`).

## Animation tracks

`mdx.Track*` types require `keyCount` to be set explicitly when building
from JS — the writer reads `keyCount` to know how many entries to
serialize, not `timestamps.size()`. For non-smooth interpolation
`keyCount === timestamps.length`; for Hermite/Bezier the `keys` vector
holds `keyCount * 3` entries (value, inTangent, outTangent).

## What's not included

- CASC / MPQ archive reading
- HTTP / CDN fetching
- Multithreading (BC7 encode is single-threaded; slow but correct)
- Disk-based BLP0 sibling-mip files

## License

BSD-3-Clause
