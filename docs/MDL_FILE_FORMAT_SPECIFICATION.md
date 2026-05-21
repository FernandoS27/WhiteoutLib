# MDL File Format Specification

**Format**: Blizzard MDL Model Format (Warcraft III / Warcraft III: Reforged)
**Encoding**: ASCII / UTF-8 plain text
**Dialect**: Warcraft III (engine-faithful) — see [Appendix A](#appendix-a--hiveworkshop-dialect-differences) for the HiveWorkshop variant
**Companion format**: [MDX](MDX_FILE_FORMAT_SPECIFICATION.md) (binary)

MDL is the **human-readable text representation** of the Warcraft III model
format. It carries the same model data as the binary MDX format — geometry,
materials, skeletal animation, particle effects, cameras, collision volumes —
but as a structured, brace-delimited text file that artists and tools can read
and edit directly. The Warcraft III world editor and the game engine convert
freely between MDL and MDX.

This document specifies the **Warcraft III dialect** — the text the game's own
MDL reader/writer produces and consumes. A second community dialect, used by
HiveWorkshop tooling (Retera Model Studio, Magos, MdlVis, mdx-m3-viewer), is
described in [Appendix A](#appendix-a--hiveworkshop-dialect-differences).

**Authorship attribution**: Fernando A. Sahmkow

<style>
body { line-height: 1.6; }
h1, h2, h3, h4 { line-height: 1.3; }
p, li { text-align: left; }
table { width: 100%; border-collapse: collapse; }
th, td { padding: 0.35rem 0.5rem; vertical-align: top; }
pre { padding: 0.6rem 0.8rem; overflow-x: auto; }
@media print {
    @page { margin: 18mm; }
    h2 { break-before: page; page-break-before: always; }
    h2:first-of-type { break-before: auto; page-break-before: auto; }
    h2, h3, h4 { break-after: avoid-page; page-break-after: avoid; }
    table, thead, tbody, tr, th, td, pre, blockquote, figure, img, ul, ol {
        break-inside: avoid-page; page-break-inside: avoid;
    }
    pre code { white-space: pre-wrap; overflow-wrap: anywhere; }
}
</style>

---

## Revision History

| Date | Version | Description |
|------|---------|-------------|
| 2026-05-21 | 1.0 | Initial specification of the Warcraft III MDL dialect, derived from the WhiteoutLib MDL writer/parser and validated by a byte-exact MDX→MDL→MDX round-trip over the `war3.w3mod` game-asset corpus. |

## Table of Contents

1. [Overview](#1-overview)
2. [Lexical Structure](#2-lexical-structure)
3. [Syntax](#3-syntax)
4. [Animation Tracks](#4-animation-tracks)
5. [Node Properties](#5-node-properties)
6. [Top-Level Blocks](#6-top-level-blocks)
7. [A Complete Minimal Example](#7-a-complete-minimal-example)

**Appendix**

- [A — HiveWorkshop Dialect Differences](#appendix-a--hiveworkshop-dialect-differences)

---

## 1. Overview

An MDL file is a sequence of **top-level blocks**, each introduced by a keyword.
The file has no magic number and no header — the first non-whitespace token is
the `Version` keyword. Blocks may, in principle, appear in any order, but the
Warcraft III writer always emits them in the canonical order listed in
[Section 6](#6-top-level-blocks), and readers should not depend on any other
order.

A block groups a set of **properties**, **flags**, **animation tracks**, and
nested **sub-blocks** between braces:

```mdl
Version {
	FormatVersion 800,
}

Model "Footman" {
	BlendTime 150,
	MinimumExtent { -45.0, -45.0, 0.0 },
	MaximumExtent { 45.0, 45.0, 120.0 },
	BoundsRadius 91.0,
}
```

MDL is **whitespace-insensitive** for parsing purposes — indentation and line
breaks exist only for readability. The canonical writer indents each nesting
level with a single **tab** and places one entry per line.

### MDL vs. MDX

MDL is a faithful text mirror of MDX. The two formats hold the same model and
the game converts losslessly between them, with these representational notes:

- MDX stores some quantities both as a static scalar **and** as an optional
  animation track; MDL expresses each property once (either a `static` value
  *or* an animated track block).
- Indices that MDX stores as raw `u32` (texture IDs, parent IDs, geoset IDs)
  appear in MDL either as plain integers or as named sentinels such as
  `None` and `Multiple`.
- MDL omits properties left at their default value; a reader must apply the
  documented defaults for any property a block does not mention.

---

## 2. Lexical Structure

### 2.1 Character Set & Whitespace

An MDL file is plain text. Spaces, tabs, carriage returns, and line feeds are
**whitespace** and separate tokens; runs of whitespace are equivalent to a
single space. Line breaks are not statement terminators.

### 2.2 Comments

A `//` begins a comment that runs to the end of the line. There is no
block-comment syntax.

```mdl
ObjectId 5,          // line comment — ignored by the parser
Parent 0,            // "Bone_Root"
```

### 2.3 Tokens

| Token | Description |
|-------|-------------|
| Identifier | A letter or `_` followed by letters, digits, or `_` (e.g. `FilterMode`, `Bone_Root1`). Keywords, property names, and flag names are all identifiers. |
| Number | An integer or floating-point literal (see 2.4). |
| String | A double-quoted literal (see 2.5). |
| `{` `}` | Block / vector delimiters. |
| `,` | Entry separator (and trailing separator). |
| `:` | Keyframe time/value separator. |
| `<=` | HD sub-texture slot designator (`static TextureID 0 <= 1,`). |

### 2.4 Numbers

Numbers are written in standard decimal notation and may be:

- **Integers** — optional sign then digits: `0`, `150`, `-1`, `4294967295`.
- **Floats** — optional sign, digits, `.`, fractional digits, and an optional
  exponent: `1.0`, `-0.5`, `0.95538312`, `1.5e-08`, `270.0`.
- **Non-finite** — the literals `nan`, `inf`, and `-inf` (case-insensitive).
  These occur in shipped models (e.g. an unused, uninitialised layer field) and
  must be accepted by readers.

The canonical writer prints floats using the **shortest decimal string that
round-trips back to the exact same 32-bit float**, and always keeps a decimal
point on integral values (`50` is written `50.0`). Readers must parse with
correct (nearest) rounding so that the value survives the round-trip.

> **Signed keyframe times.** Integer literals used as keyframe times and event
> times are **signed** — negative frames (e.g. `-3600`) occur in game assets as
> animation lead-in. Readers must parse them as signed 32-bit integers.

### 2.5 Strings

A string is any run of characters between double quotes: `"Textures\Armor.blp"`.

- Strings have **no escape sequences** — a backslash is a literal backslash
  (Windows-style asset paths embed them directly: `"Textures\Diffuse.blp"`).
- A string **may contain embedded line breaks**. PopcornFX emitter strings, in
  particular, contain literal `CR`/`LF` characters; the closing quote — not the
  end of line — terminates the string.

```mdl
Image "ReplaceableTextures\TeamColor\TeamColor00.blp",
```

---

## 3. Syntax

Every construct in an MDL file is one of five forms: a **block**, a
**property**, a **flag**, a **vector**, or an **animation track**.

### 3.1 Blocks

A block is a keyword, zero or more **header parameters** (strings or numbers),
an opening `{`, a body, and a closing `}`:

```
BlockName [param ...] {
	body
}
```

Header parameters identify or size the block. They are used for:

- a **name**: `Model "Footman"`, `Bone "Bone_Head"`, `Anim "Stand"`;
- a **count**: `Sequences 12`, `Vertices 348`, `PivotPoints 27`;
- both, or several counts: `Faces 1 1044`.

### 3.2 Properties

A property is a name, a value, and a trailing comma:

```mdl
BlendTime 150,
FieldOfView 0.7853982,
Image "Textures\Diffuse.blp",
FilterMode Blend,
```

The value is a number, a string, an **identifier** (an enumerated keyword such
as `Blend` or `Multiple`), or a **vector** (3.4). A property whose value is a
vector is written `Name { … },`.

### 3.3 Static Properties

Some properties may be either a fixed value or animated over time. When fixed,
they are written with the `static` keyword prefix; when animated, they appear
as an [animation track](#4-animation-tracks) of the same name:

```mdl
static Alpha 1.0,                       // fixed
```
```mdl
Alpha 3 {                               // animated — same property, track form
	Linear,
	0: 1.0,
	500: 0.0,
	1000: 1.0,
}
```

A reader encountering `static Name value` stores `value` as the property's base
value with no track; encountering `Name count { … }` stores an animation track.

### 3.4 Flags & Vectors

A **flag** is a bare identifier with a trailing comma — its mere presence sets a
boolean: `NonLooping,`, `Billboarded,`, `TwoSided,`, `DropShadow,`.

A **vector** is a brace-enclosed, comma-separated list of numbers. Vectors are
fixed-length by context: 2 for UV coordinates, 3 for positions/colours, 4 for
quaternions, 12 for bind-pose matrices:

```mdl
MinimumExtent { -45.0, -45.0, 0.0 },
Color { 1.0, 0.5, 0.25 },
{ 0.0, 0.0, 0.0, 1.0 },                 // quaternion (x, y, z, w)
```

### 3.5 Data Blocks

A data block holds an array of anonymous values — vectors or bare numbers —
each followed by a comma. The header parameter is the element count:

```mdl
Vertices 3 {
	{ 0.0, 0.0, 0.0 },
	{ 1.0, 0.0, 0.0 },
	{ 0.0, 1.0, 0.0 },
}
```
```mdl
VertexGroup {
	0,
	0,
	1,
}
```

A trailing comma after the final entry is conventional and expected.

---

## 4. Animation Tracks

An animation track stores a property's value as a series of **keyframes**. Any
property documented as *animatable* may appear in this form.

### 4.1 Track Syntax

```
PropertyName keyframeCount {
	InterpolationType,
	[GlobalSeqId index,]
	time: value,
	time: value,
	...
}
```

- **keyframeCount** — the header parameter — is the number of keyframes.
- **InterpolationType** is one of `DontInterp`, `Linear`, `Hermite`, `Bezier`
  (4.2).
- **GlobalSeqId** is present only when the track is driven by a global sequence
  ([Section 6.4](#64-globalsequences)); it gives the global-sequence index.
  When absent, the track plays on the normal sequence timeline.
- Each keyframe is `time: value,` — `time` is a signed integer in milliseconds,
  `value` matches the property's type (scalar, vector, or quaternion).

```mdl
Translation 3 {
	Linear,
	0: { 0.0, 0.0, 0.0 },
	333: { 0.0, 0.0, 40.0 },
	1000: { 0.0, 0.0, 0.0 },
}
```

### 4.2 Interpolation Types

| Keyword | Meaning |
|---------|---------|
| `DontInterp` | Stepped — the value holds until the next keyframe. |
| `Linear` | Linear interpolation between keyframes. |
| `Hermite` | Hermite spline; each keyframe carries in/out tangents. |
| `Bezier` | Bézier spline; each keyframe carries in/out tangents. |

### 4.3 Tangents (Hermite & Bezier)

For `Hermite` and `Bezier` tracks, every keyframe is followed by an indented
`InTan` and `OutTan` line, each carrying a value of the same type as the
keyframe:

```mdl
Rotation 2 {
	Hermite,
	0: { 0.0, 0.0, 0.0, 1.0 },
		InTan { 0.0, 0.0, 0.0, 1.0 },
		OutTan { 0.0, 0.0, 0.0, 1.0 },
	1000: { 0.0, 0.70710677, 0.0, 0.70710677 },
		InTan { 0.0, 0.70710677, 0.0, 0.70710677 },
		OutTan { 0.0, 0.70710677, 0.0, 0.70710677 },
}
```

### 4.4 Event Tracks

Event objects ([Section 6.19](#619-eventobject)) use a simplified track that
stores only frame times — there is no interpolation type and no values:

```mdl
EventTrack 3 {
	800,
	2133,
	4067,
}
```

---

## 5. Node Properties

Many objects — `Bone`, `Light`, `Helper`, `Attachment`, the emitters,
`EventObject`, `CollisionShape` — share a common **node** substructure that
places the object in the scene hierarchy and animates its transform. Wherever a
block below is described as *"begins with the node properties,"* it admits the
following, in this order:

| Property | Form | Description |
|----------|------|-------------|
| `ObjectId` | `ObjectId N,` | Unique node index. Always present. |
| `Parent` | `Parent N,` | Parent node's `ObjectId`. **Omitted** for a root node (a model may have several roots). |
| Behaviour flags | bare flags | Any of: `DontInheritTranslation`, `DontInheritRotation`, `DontInheritScaling`, `Billboarded`, `BillboardedLockX`, `BillboardedLockY`, `BillboardedLockZ`, `CameraAnchored`. |
| `Translation` | track (`float[3]`) | Animated local translation. |
| `Rotation` | track (`float[4]`) | Animated local rotation, quaternion `(x, y, z, w)`. |
| `Scaling` | track (`float[3]`) | Animated local scaling. |

The three transform channels are written **only when animated** — a static node
omits them entirely (its transform is identity, offset by its pivot point).

```mdl
Bone "Bone_Forearm" {
	ObjectId 4,
	Parent 3,
	Rotation 2 {
		Linear,
		0: { 0.0, 0.0, 0.0, 1.0 },
		1000: { 0.0, 0.0, 0.38268, 0.92388 },
	}
}
```

Every node also has a corresponding entry in the model's
[`PivotPoints`](#615-pivotpoints) block, indexed by `ObjectId`.

---

## 6. Top-Level Blocks

The Warcraft III writer emits blocks in the order below. All blocks are
optional except `Version` and `Model`.

### 6.1 Version

```mdl
Version {
	FormatVersion 800,
}
```

`FormatVersion` is the format version: `800` (Classic), or `900`/`1000`/`1100`/
`1200` (Reforged). It governs which version-gated properties are valid — see
the [MDX specification](MDX_FILE_FORMAT_SPECIFICATION.md#10-version-differences).

### 6.2 Model

```mdl
Model "Arthas" {
	BlendTime 150,
	MinimumExtent { -175.945, -147.584, -73.0451 },
	MaximumExtent { 172.425, 175.079, 553.363 },
	BoundsRadius 0.0,
}
```

| Property | Description |
|----------|-------------|
| *(name)* | The header parameter is the model name. |
| `BlendTime` | Animation cross-fade time in milliseconds; omitted when `0`. |
| `MinimumExtent` / `MaximumExtent` / `BoundsRadius` | Model bounding volume. |

`MinimumExtent`, `MaximumExtent`, and `BoundsRadius` together form an **extent**
— the same triple recurs in `Sequences` and `Geoset`. The model-level
`BoundsRadius` is frequently `0.0` even when the extent box is non-empty.

### 6.3 Sequences

A `Sequences` block lists the model's named animations. Its header parameter is
the sequence count; each child `Anim` block names one sequence.

```mdl
Sequences 2 {
	Anim "Stand" {
		Interval { 0, 3333 },
		MoveSpeed 0.0,
		MinimumExtent { -50.0, -50.0, 0.0 },
		MaximumExtent { 50.0, 50.0, 100.0 },
		BoundsRadius 80.0,
	}
	Anim "Walk" {
		Interval { 3334, 6667 },
		NonLooping,
		MoveSpeed 270.0,
		Rarity 0.0,
		MinimumExtent { -55.0, -55.0, 0.0 },
		MaximumExtent { 55.0, 55.0, 105.0 },
		BoundsRadius 85.0,
	}
}
```

| Property | Description |
|----------|-------------|
| `Interval { start, end }` | Sequence time range, in milliseconds, on the global timeline. |
| `NonLooping` | Flag — sequence plays once instead of looping. |
| `MoveSpeed` | Movement speed this animation is authored for; omitted when `0`. |
| `Rarity` | Random-selection weight; omitted when `0`. |
| extent | Per-sequence bounding volume. |

### 6.4 GlobalSequences

Global sequences are looping timelines independent of the model's animations;
tracks reference them by index via `GlobalSeqId`. Each `Duration` is the loop
length in milliseconds.

```mdl
GlobalSequences 2 {
	Duration 1000,
	Duration 2500,
}
```

### 6.5 Textures

Each `Bitmap` sub-block declares one texture slot.

```mdl
Textures 3 {
	Bitmap {
		Image "Textures\Footman.blp",
	}
	Bitmap {
		Image "",
		ReplaceableId 1,
	}
	Bitmap {
		Image "Textures\Footman_Detail.blp",
		WrapWidth,
		WrapHeight,
	}
}
```

| Property | Description |
|----------|-------------|
| `Image` | Texture file path. Empty when the slot is a replaceable texture. |
| `ReplaceableId` | Replaceable-texture ID (`1` = team colour, `2` = team glow, …); omitted when `0`. |
| `WrapWidth` / `WrapHeight` | Flags — wrap the texture on the U / V axis. |

### 6.6 Materials

A `Materials` block lists `Material` definitions; each material owns one or more
`Layer` sub-blocks rendered in order.

```mdl
Materials 1 {
	Material {
		PriorityPlane 0,
		Layer {
			FilterMode None,
			static TextureID 0 <= 0,
		}
		Layer {
			FilterMode Additive,
			Unshaded,
			static TextureID 2 <= 0,
			static Alpha 0.8,
		}
	}
}
```

#### Material properties

| Property | Description |
|----------|-------------|
| `PriorityPlane` | Render-order priority; omitted when `0`. |
| `ConstantColor` | Flag. |
| `TwoSided` | Flag — render both faces. |
| `Unfogged` | Flag — not affected by fog. |
| `SortPrimsNearZ` / `SortPrimsFarZ` | Flags — primitive sort order. |
| `FullResolution` | Flag. |
| `Shader` | `Shader "name",` — material-level HD shader-pipeline name. Present **only for `version` 900 and 1000** (see *Shader* below). |

#### Layer properties

A `Layer` selects a texture and the way it is blended.

| Property | Form | Description |
|----------|------|-------------|
| `FilterMode` | identifier | `None`, `Transparent`, `Blend`, `Additive`, `AddAlpha`, `Modulate`, `Modulate2x`. |
| Shading flags | bare flags | `Unshaded`, `SphereEnvMap`, `TwoSided`, `Unfogged`, `NoDepthTest`, `NoDepthSet`, `WrapWidth`, `WrapHeight`, `Unlit`. |
| `Shader` | `Shader "name",` | Per-layer HD shader name, written for `version ≥ 1100` HD layers — see *Shader* below. Omitted for SD layers. |
| `TextureID` | `static TextureID id <= slot,` | Texture binding — see below. Animatable. |
| `TVertexAnimId` | `TVertexAnimId N,` | Index into `TextureAnims`; omitted when none. |
| `CoordId` | `CoordId N,` | UV-set index in the geoset; omitted when `0`. |
| `Alpha` | `static Alpha a,` | Layer opacity. Animatable; omitted when `1.0`. |
| `EmissiveGain` | `static EmissiveGain g,` | Reforged PBR emissive gain. Animatable; **default `1.0`**. |
| `FresnelColor` | `static FresnelColor { r,g,b },` | Reforged PBR fresnel colour. Animatable; default `{ 1,1,1 }`. |
| `FresnelOpacity` / `FresnelTeamColor` | `static …` | Reforged PBR fresnel scalars. Animatable. |

**Texture binding.** In the Warcraft III dialect a texture is bound with the
slot-designator form:

```mdl
static TextureID 0 <= 0,
```

The number after `TextureID` is the index into the `Textures` block; the number
after `<=` is the layer's texture **slot**. A classic SD layer uses slot `0`
only. A Reforged HD layer (`version ≥ 1100`) carries one binding per slot —
diffuse `0`, normal `1`, ORM `2`, emissive `3`, team-colour `4`, reflections `5`:

```mdl
Layer {
	FilterMode None,
	Shader "Shader_HD_DefaultUnit",
	static TextureID 0 <= 0,
	static TextureID 1 <= 1,
	static TextureID 2 <= 2,
	static TextureID 3 <= 3,
	static TextureID 4 <= 4,
	static TextureID 5 <= 5,
	static Alpha 1.0,
}
```

An animated (flipbook) texture binding uses the track form, with no slot
designator:

```mdl
TextureID 4 {
	DontInterp,
	0: 2,
	200: 3,
	400: 4,
	600: 5,
}
```

**Shader.** Where the HD shader name is written depends on the model version:

- **`version` 900 and 1000** — the shader name is written **once at the
  material level**, as a `Shader "name",` property directly inside the
  `Material` block (before its `Layer` sub-blocks). This placement is identical
  in both the Warcraft III and the HiveWorkshop dialects.
- **`version ≥ 1100`** — the material-level field no longer exists; instead
  each HD `Layer` carries its own `Shader "name",` directive. SD layers omit it.

```mdl
Material {                              // version 900 / 1000
	Shader "Shader_HD_DefaultUnit",
	Layer {
		FilterMode None,
		...
	}
}
```

Common shader names: `"Shader_HD_DefaultUnit"`, `"Shader_HD_Crystal"`,
`"Shader_SD_FixedFunction"`. The HiveWorkshop dialect marks `version ≥ 1100` HD
layers differently — see [Appendix A.1](#a1-hd-layer-shader-version--1100).

### 6.7 TextureAnims

UV-animation definitions, referenced by a layer's `TVertexAnimId`. Each
`TVertexAnim` holds up to three transform tracks.

```mdl
TextureAnims 1 {
	TVertexAnim {
		Translation 2 {
			Linear,
			0: { 0.0, 0.0, 0.0 },
			1000: { 1.0, 0.0, 0.0 },
		}
	}
}
```

`Translation` and `Scaling` are `float[3]` tracks; `Rotation` is a `float[4]`
quaternion track.

### 6.8 Geoset

A `Geoset` is one renderable mesh. There is one `Geoset` block per mesh — they
are **not** wrapped in a counted parent block.

```mdl
Geoset {
	Vertices 4 {
		{ -16.0, -16.0, 0.0 },
		{  16.0, -16.0, 0.0 },
		{  16.0,  16.0, 0.0 },
		{ -16.0,  16.0, 0.0 },
	}
	Normals 4 {
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 0.0, 1.0 },
	}
	TVertices 4 {
		{ 0.0, 0.0 },
		{ 1.0, 0.0 },
		{ 1.0, 1.0 },
		{ 0.0, 1.0 },
	}
	VertexGroup {
		0,
		0,
		0,
		0,
	}
	Faces 1 6 {
		Triangles {
			{ 0, 1, 2, 0, 2, 3 },
		}
	}
	Groups 1 1 {
		Matrices { 0 },
	}
	MaterialID 0,
	SelectionGroup 0,
	MinimumExtent { -16.0, -16.0, 0.0 },
	MaximumExtent { 16.0, 16.0, 0.0 },
	BoundsRadius 22.63,
}
```

| Element | Description |
|---------|-------------|
| `Vertices N { … }` | `N` vertex positions (`float[3]`). |
| `Normals N { … }` | `N` vertex normals (`float[3]`). |
| `TVertices N { … }` | A UV set — `N` texture coordinates (`float[2]`). The block repeats once per UV set. |
| `VertexGroup { … }` | One matrix-group index per vertex (`u8` each). |
| `Tangents N { … }` | Reforged — `N` tangents (`float[4]`, `xyz` + handedness). |
| `SkinWeights N { … }` | Reforged HD skinning — `N` rows of 8 integers per vertex (4 bone indices + 4 weights, `0`–`255`). Carried **alongside** the legacy `VertexGroup`/`Groups` data, not instead of it. **In the Warcraft III dialect each row is written as bare, comma-separated integers — no per-vertex braces** (see [Appendix A.5](#a5-skinweights-row-form)). |
| `Faces G T { Triangles { { … } } }` | `G` triangle groups, `T` total indices; the `Triangles` sub-block lists the index buffer. |
| `Groups G M { Matrices { … }, … }` | `G` matrix groups containing `M` total matrix indices; each `Matrices { … }` lists one group's bone-matrix indices. |
| `MaterialID` | Index into the `Materials` block. |
| `SelectionGroup` | Selection-group index. |
| `Unselectable` | Flag — geoset cannot be selected. |
| `LevelOfDetail` | Reforged LOD index. |
| extent | Geoset bounding volume. |
| `Anim { extent }` | Per-sequence geoset bounding volumes — one `Anim` sub-block per sequence. |

### 6.9 GeosetAnim

Animates a geoset's visibility and vertex colour.

```mdl
GeosetAnim {
	static Alpha 1.0,
	DropShadow,
	GeosetId 0,
	static Color { 1.0, 1.0, 1.0 },
}
```

| Property | Description |
|----------|-------------|
| `Alpha` | Geoset opacity. Animatable. |
| `DropShadow` | Flag. |
| `GeosetId` | Index of the affected geoset. |
| `Color` | Vertex colour `{ b, g, r }`. Animatable; written only when used. |

### 6.10 Bone

A `Bone` is a skinning node. It begins with the [node properties](#5-node-properties)
and adds two links:

```mdl
Bone "Bone_Root" {
	ObjectId 0,
	GeosetId Multiple,
	GeosetAnimId None,
}
Bone "Bone_Spine" {
	ObjectId 1,
	Parent 0,
	GeosetId 0,
	GeosetAnimId 1,
}
```

| Property | Description |
|----------|-------------|
| `GeosetId` | Index of the geoset this bone belongs to, or `Multiple` if it spans several. |
| `GeosetAnimId` | Index of the associated `GeosetAnim`, or `None`. |

### 6.11 Light

Begins with the node properties; declares a light source.

```mdl
Light "LightOmni" {
	ObjectId 12,
	Parent 0,
	Omnidirectional,
	static AttenuationStart 50.0,
	static AttenuationEnd 200.0,
	static Color { 1.0, 0.9, 0.7 },
	static Intensity 1.0,
	static AmbColor { 1.0, 1.0, 1.0 },
	static AmbIntensity 0.0,
}
```

The light type is a bare flag: `Omnidirectional`, `Directional`, or `Ambient`.
`AttenuationStart`, `AttenuationEnd`, `Color`, `Intensity`, `AmbColor`,
`AmbIntensity`, and `Visibility` are all animatable. `Color` and `AmbColor`
default to `{ 1, 1, 1 }`.

### 6.12 Helper

A helper is a pure transform node — it begins with, and contains only, the
[node properties](#5-node-properties).

```mdl
Helper "Overhead Ref" {
	ObjectId 8,
	Parent 0,
}
```

### 6.13 Attachment

An attachment point for equipment, effects, or spawned models.

```mdl
Attachment "Origin Ref " {
	ObjectId 40,
	AttachmentID 0,
}
```

| Property | Description |
|----------|-------------|
| `AttachmentID` | Attachment slot index. |
| `Path` | Attached model path; omitted when empty. |
| `Visibility` | Animatable visibility track. |

### 6.14 PivotPoints

One pivot point — the node's local origin — per node, in `ObjectId` order.

```mdl
PivotPoints 3 {
	{ 0.0, 0.0, 0.0 },
	{ 0.0, 0.0, 60.0 },
	{ 0.0, 0.0, 110.0 },
}
```

### 6.15 ParticleEmitter

A classic emitter that spawns model instances.

```mdl
ParticleEmitter "ModelEmitter" {
	ObjectId 20,
	Parent 0,
	EmitterUsesMdl,
	static EmissionRate 8.0,
	static Gravity 0.0,
	static Longitude 90.0,
	static Latitude 0.0,
	Path "Objects\Spawnmodel\spawn.mdl",
	static LifeSpan 1.0,
	static InitVelocity 0.0,
}
```

`EmitterUsesMdl` / `EmitterUsesTga` are flags. `EmissionRate`, `Gravity`,
`Longitude`, `Latitude`, `LifeSpan`, `InitVelocity`, and `Visibility` are
animatable.

### 6.16 ParticleEmitter2

The quad (billboard) particle emitter.

```mdl
ParticleEmitter2 "Smoke" {
	ObjectId 21,
	Parent 0,
	SortPrimsFarZ,
	Unshaded,
	static Speed 100.0,
	static Variation 0.5,
	static Latitude 45.0,
	static Gravity 9.8,
	LifeSpan 2.0,
	static EmissionRate 50.0,
	static Length 16.0,
	static Width 16.0,
	Additive,
	Rows 1,
	Columns 1,
	Head,
	TailLength 0.0,
	Time 0.5,
	SegmentColor {
		Color { 1.0, 1.0, 1.0 },
		Color { 1.0, 0.5, 0.0 },
		Color { 0.0, 0.0, 0.0 },
	}
	Alpha { 255, 255, 0 },
	ParticleScaling { 1.0, 2.0, 0.0 },
	LifeSpanUVAnim { 0, 0, 1 },
	DecayUVAnim { 0, 0, 1 },
	TailUVAnim { 0, 0, 1 },
	TailDecayUVAnim { 0, 0, 1 },
	TextureID 0,
}
```

| Element | Description |
|---------|-------------|
| Behaviour flags | `SortPrimsFarZ`, `LineEmitter`, `Unfogged`, `ModelSpace`, `Unshaded`, `XYQuad`. |
| `Speed` … `Width` | Emission parameters; `Speed`, `Variation`, `Latitude`, `Gravity`, `EmissionRate`, `Length`, `Width` are animatable. |
| Filter mode | A bare flag: `Blend`, `Additive`, `Modulate`, `Modulate2x`, `AlphaKey`. |
| `Rows` / `Columns` | Texture-atlas grid. |
| Head/tail | A bare flag: `Head`, `Tail`, `Both`. |
| `SegmentColor` | A sub-block of exactly three `Color { r, g, b }` entries (start / mid / end). |
| `Alpha { a, a, a }` | Three 0–255 segment alphas. |
| `ParticleScaling { s, s, s }` | Three segment scale factors. |
| `LifeSpanUVAnim` / `DecayUVAnim` / `TailUVAnim` / `TailDecayUVAnim` | UV-animation intervals `{ start, end, repeat }`. |
| `TextureID` | Index into `Textures`. |
| `Squirt`, `PriorityPlane`, `ReplaceableId` | Optional integers, written only when non-zero. |

### 6.17 RibbonEmitter

A trailing-ribbon emitter.

```mdl
RibbonEmitter "Trail" {
	ObjectId 22,
	Parent 6,
	static HeightAbove 4.0,
	static HeightBelow 4.0,
	static Alpha 1.0,
	static Color { 1.0, 1.0, 1.0 },
	LifeSpan 0.5,
	TextureSlot 0,
	EmissionRate 30,
	Rows 1,
	Columns 1,
	MaterialID 1,
}
```

`HeightAbove`, `HeightBelow`, `Alpha`, `Color`, `TextureSlot`, and `Visibility`
are animatable. `Gravity` is written only when non-zero.

### 6.18 ParticleEmitterPopcorn

A Reforged PopcornFX emitter (the MDL keyword for the MDX `CORN` chunk).

```mdl
ParticleEmitterPopcorn "PopcornFire" {
	ObjectId 23,
	Parent 0,
	static LifeSpan 1.0,
	static EmissionRate 1.0,
	static Speed 1.0,
	static Color { 1.0, 1.0, 1.0 },
	static Alpha 1.0,
	Path "SharedFX\Hero_Glow\Hero_Glow.pkfx",
	AnimVisibilityGuide "Always=on, Death=off",
}
```

| Property | Description |
|----------|-------------|
| Flags | `SortPrimsFarZ`, `Unshaded`, `Unfogged`, `PopcornScaling`. |
| `LifeSpan`, `EmissionRate`, `Speed`, `Alpha` | Animatable scalars; **default `1.0`**. |
| `Color` | Animatable RGB; default `{ 1, 1, 1 }`. |
| `ReplaceableId` | Written only when non-zero. |
| `Path` | The `.pkfx` PopcornFX effect path. |
| `AnimVisibilityGuide` | Comma-separated `Sequence=on/off` rules; may contain embedded line breaks. |

### 6.19 EventObject

Fires sound, footprint, or splat effects at specific frames. Begins with the
node properties, then an `EventTrack` ([Section 4.4](#44-event-tracks)):

```mdl
EventObject "SNDxFootmanFootstep" {
	ObjectId 25,
	Parent 2,
	EventTrack 4 {
		800,
		2133,
		4067,
		5400,
	}
}
```

### 6.20 Camera

```mdl
Camera "Portrait" {
	Position { 0.0, -180.0, 90.0 },
	FieldOfView 0.7853982,
	FarClip 1000.0,
	NearClip 0.1,
	Target {
		Position { 0.0, 0.0, 70.0 },
	}
}
```

| Element | Description |
|---------|-------------|
| `Position` | Camera eye position. `Translation` adds an animation track. |
| `FieldOfView` | Vertical FOV in radians. |
| `FarClip` / `NearClip` | Clip-plane distances. |
| `Rotation` | Optional scalar (`f32`) roll-angle track. |
| `Target { Position … }` | Look-at target; `Translation` inside it adds a track. |

### 6.21 CollisionShape

A collision volume. Begins with the node properties, then a shape-type flag and
its geometry.

```mdl
CollisionShape "Collision" {
	ObjectId 26,
	Parent 0,
	Sphere,
	Vertices 1 {
		{ 0.0, 0.0, 60.0 },
	}
	BoundsRadius 50.0,
}
```

The shape type is a bare flag — `Box`, `Plane`, `Sphere`, or `Cylinder`. `Box`
and `Plane` carry two `Vertices`; `Sphere` carries one; `Cylinder` carries two.
`BoundsRadius` is present only for `Sphere` and `Cylinder`.

### 6.22 FaceFX

A Reforged facial-animation binding.

```mdl
FaceFX "Face" {
	Path "war3mapImported\face.facefx",
}
```

### 6.23 BindPose

Reforged bind-pose matrices — one 3×4 (12-float) affine matrix per node.

```mdl
BindPose {
	Matrices 2 {
		{ 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0 },
		{ 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 60.0 },
	}
}
```

---

## 7. A Complete Minimal Example

A valid, complete MDL file for a single textured quad:

```mdl
Version {
	FormatVersion 800,
}
Model "Quad" {
	MinimumExtent { -16.0, -16.0, 0.0 },
	MaximumExtent { 16.0, 16.0, 0.0 },
	BoundsRadius 22.63,
}
Sequences 1 {
	Anim "Stand" {
		Interval { 0, 1000 },
		MinimumExtent { -16.0, -16.0, 0.0 },
		MaximumExtent { 16.0, 16.0, 0.0 },
		BoundsRadius 22.63,
	}
}
Textures 1 {
	Bitmap {
		Image "Textures\Quad.blp",
	}
}
Materials 1 {
	Material {
		Layer {
			FilterMode None,
			static TextureID 0 <= 0,
		}
	}
}
Geoset {
	Vertices 4 {
		{ -16.0, -16.0, 0.0 },
		{  16.0, -16.0, 0.0 },
		{  16.0,  16.0, 0.0 },
		{ -16.0,  16.0, 0.0 },
	}
	Normals 4 {
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 0.0, 1.0 },
	}
	TVertices 4 {
		{ 0.0, 1.0 },
		{ 1.0, 1.0 },
		{ 1.0, 0.0 },
		{ 0.0, 0.0 },
	}
	VertexGroup {
		0,
		0,
		0,
		0,
	}
	Faces 1 6 {
		Triangles {
			{ 0, 1, 2, 0, 2, 3 },
		}
	}
	Groups 1 1 {
		Matrices { 0 },
	}
	MaterialID 0,
	SelectionGroup 0,
	MinimumExtent { -16.0, -16.0, 0.0 },
	MaximumExtent { 16.0, 16.0, 0.0 },
	BoundsRadius 22.63,
}
Bone "Bone_Root" {
	ObjectId 0,
	GeosetId 0,
	GeosetAnimId None,
}
PivotPoints 1 {
	{ 0.0, 0.0, 0.0 },
}
```

---

## Appendix A — HiveWorkshop Dialect Differences

The community tool ecosystem (Retera Model Studio, Magos' War3 Model Editor,
MdlVis, mdx-m3-viewer) reads and writes a slightly different MDL dialect. It is
identical to the Warcraft III dialect described above **except** in the areas
below. A reader that wants to accept both dialects should treat the constructs
in this appendix as alternative spellings of the same data.

### A.1 HD Layer Shader (version ≥ 1100)

For `version` 900 and 1000 the two dialects record the HD shader **identically**
— a single material-level `Shader "name",` property inside the `Material` block
(see [Section 6.6](#66-materials)). This is *not* a dialect difference.

The dialects diverge only for `version ≥ 1100`, where the material-level field
no longer exists and the shader is recorded **per layer**:

- the **Warcraft III dialect** writes a per-layer `Shader "name",` directive
  (`"Shader_HD_DefaultUnit"`, …);
- the **HiveWorkshop dialect** writes a per-layer numeric `ShaderTypeId N,`
  instead (`0` = SD, `1` = HD) — and writes no per-layer `Shader` name.

```mdl
Layer {                                 // version >= 1100, HiveWorkshop
	FilterMode None,
	ShaderTypeId 1,
	...
}
```

### A.2 HD Texture Slots — Named Properties

The Warcraft III dialect binds every HD texture slot with the
`static TextureID id <= slot,` designator form. The HiveWorkshop dialect uses a
**distinct property name per slot** and never writes the `<=` designator:

| Slot | Warcraft III dialect | HiveWorkshop dialect |
|-----:|----------------------|----------------------|
| 0 Diffuse | `static TextureID 0 <= 0,` | `static TextureID 0,` |
| 1 Normal | `static TextureID 1 <= 1,` | `static NormalTextureID 1,` |
| 2 ORM | `static TextureID 2 <= 2,` | `static ORMTextureID 2,` |
| 3 Emissive | `static TextureID 3 <= 3,` | `static EmissiveTextureID 3,` |
| 4 Team Colour | `static TextureID 4 <= 4,` | `static TeamColorTextureID 4,` |
| 5 Reflections | `static TextureID 5 <= 5,` | `static ReflectionsTextureID 5,` |

Each named-slot property is animatable in the usual track form
(`NormalTextureID 4 { … }`).

```mdl
Layer {
	FilterMode None,
	ShaderTypeId 1,
	static TextureID 0,
	static NormalTextureID 1,
	static ORMTextureID 2,
	static EmissiveTextureID 3,
	static TeamColorTextureID 4,
	static ReflectionsTextureID 5,
	static Alpha 1.0,
}
```

### A.3 Material Flag Names

- The HiveWorkshop dialect writes the far-Z primitive sort flag as
  `SortPrimitives,` rather than `SortPrimsFarZ,`.
- It **omits** the engine-only material flag `SortPrimsNearZ,`, which its tools
  do not recognise. The `Unfogged` material flag is written by **both**
  dialects.

### A.4 Layer Flag Names

The HiveWorkshop dialect omits the engine-only layer shading flags `WrapWidth`,
`WrapHeight`, and `Unlit`, which its tools do not recognise. All other layer
flags (`Unshaded`, `SphereEnvMap`, `TwoSided`, `Unfogged`, `NoDepthTest`,
`NoDepthSet`) are written identically in both dialects.

### A.5 SkinWeights Row Form

Both dialects write a Reforged HD geoset's `SkinWeights` block with the same
header (`SkinWeights N { … }`) and the same payload — 8 integers per vertex (4
bone indices + 4 weights). They differ only in how each vertex's row is
delimited:

- the **Warcraft III dialect** writes each row as **bare, comma-separated
  integers** with no enclosing braces — the engine's MDL reader requires this;
  a leading `{` makes its parse loop stop early and reject the model;
- the **HiveWorkshop dialect** wraps each vertex row in `{ … }`, the form its
  community tooling expects.

```mdl
SkinWeights 3 {                         // Warcraft III dialect
	8, 7, 0, 0, 191, 64, 0, 0,
	8, 0, 0, 0, 255, 0, 0, 0,
	76, 4, 0, 0, 252, 3, 0, 0,
}
```
```mdl
SkinWeights 3 {                         // HiveWorkshop dialect
	{ 8, 7, 0, 0, 191, 64, 0, 0 },
	{ 8, 0, 0, 0, 255, 0, 0, 0 },
	{ 76, 4, 0, 0, 252, 3, 0, 0 },
}
```

### A.6 Geoset Selection and LOD Properties

The HiveWorkshop dialect adds two `Geoset`-level properties the Warcraft III
dialect never writes:

- `SelectionFlags N,` — raw selection flags, written when the geoset's selection
  state is not simply the `Unselectable` flag. The Warcraft III dialect records
  only the `Unselectable` flag and has no raw-flags spelling.
- `LevelOfDetailName "name",` — a string label for the LOD. Both dialects write
  the numeric `LevelOfDetail N,` index; only the HiveWorkshop dialect adds the
  accompanying name.

### A.7 Summary

| Aspect | Warcraft III dialect | HiveWorkshop dialect |
|--------|----------------------|----------------------|
| HD layer shader marker (`version ≥ 1100`) | per-layer `Shader "name",` | per-layer `ShaderTypeId N,` |
| HD texture slots | `static TextureID id <= slot,` | named per-slot properties (`NormalTextureID`, …) |
| HD `SkinWeights` rows | bare comma-separated integers | each row wrapped in `{ … }` |
| Far-Z sort flag | `SortPrimsFarZ` | `SortPrimitives` |
| `SortPrimsNearZ` material flag | written | omitted |
| `WrapWidth` / `WrapHeight` / `Unlit` layer flags | written | omitted |
| Geoset `SelectionFlags` raw-flags property | omitted | written |
| Geoset `LevelOfDetailName` property | omitted | written |

The material-level `Shader "name",` of `version` 900/1000 models is **the same
in both dialects** and is therefore not listed above. Everything else — block
structure, the `static`/track property model, vectors, animation tracks, node
properties, number and string lexing, and the full set of top-level blocks — is
identical between the two dialects.
