# SHM File Format Specification

**Format**: Diablo III ShaderMap (`.shm`)  
**Byte Order**: Little-endian  
**Magic**: `0xDEADBEEF`  
**Version**: 26  
**Corpus**: 1,192 files analyzed
**SNO Group**: 36 (`ShaderMap`)
**Registered revision**: 31 — the shipped data is v26, so the binary's compiled struct describes a *newer* layout (see below / README §4)

See [README.md](README.md) for the build these offsets come from, the generator pipeline
and the conventions used below.

---


## Table of Contents

1.  [Overview](#1-overview)
2.  [File Layout](#2-file-layout)
3.  [ShaderMap Struct](#3-shadermap-struct)
4.  [Tag Map Payload](#4-tag-map-payload)
5.  [Tag IDs](#5-tag-ids)
7.  [Naming Conventions](#7-naming-conventions)
8.  [Corpus Statistics](#8-corpus-statistics)
9.  [Cross-References](#9-cross-references)
10. [Known Unknowns](#10-known-unknowns)

---

## 1. Overview

ShaderMap files (`.shm`) are lookup tables that map **render configuration IDs** to **compiled shader SNO references**. Each material (`.mat`) references one ShaderMap, which tells the engine which shader variant to use for a given rendering context (platform, quality level, pass type).

### Pipeline Role

```
Material (.mat)
    │  shaderMapSno
    ▼
ShaderMap (.shm)    ◀── THIS FILE
    │  dwTagId → snoShader
    ▼
Compiled Shaders (.pscod, .vscod, .ps.glsl, .vs.glsl)
```

Each ShaderMap contains a variable number of entries (1–10; 720 of the 1,192 corpus files hold exactly one), where each entry maps a `dwTagId` to a compiled shader SNO hash. The engine selects the appropriate entry based on the current render pass, quality settings, and platform.

---

## 2. File Layout

> **Corrected 2026-08-15.** The layout below replaces a 48-byte "preamble" with
> the data at file `0x040`. There is no 48-byte preamble and no 16-byte pad: a
> `.shm` is the standard SNO shape — a **16-byte file header** followed by the
> **32-byte ShaderMap struct** — and the payload starts at file **`0x030`**.
> The word at file `0x020` is the array's `SerializeData.byteOffset`, and like
> every stored offset in a D3 SNO file it is **struct-relative**: it holds 32,
> not 48, and the file position is `16 + 32`. Reading it as file-absolute and
> then adding another 16 is what produced `0x040`.
>
> Three checks settle it, unanimous over the corpus:
>
> | check | result |
> | --- | --- |
> | the `SerializeData.byteOffset` at file `0x020` equals 32 | 1,192 / 1,192 |
> | `dataSize == 4 + 12 * count` | 1,192 / 1,192 |
> | file length `== 16 + 32 + dataSize` | 1,192 / 1,192 |
>
> The binary agrees: `ShaderMapHandler_ctor` registers the group with struct
> size **32**, so there is no version skew for this group at all — the "corpus
> v0 = 48" that circulated came from the stale pre-reflection type table, whose
> 48-byte shape was a misreading of the first tag-map entry (its four "SNO"
> slots at 32/36/40/44 are the payload's count word, the first entry's
> `nValueType`, its `dwTagId` and its value). That is why every file appeared to
> hold "2" at +36 — it is an entry type — and why "snoMinDefaultBaseShader" at
> +32 never held anything but 1..10: it is the entry count.
>
> Consequently the smallest file is **64 bytes** (one entry), not 84, and no
> corpus file holds more than **10** entries.

```
┌─────────────────────────────────────────────────────────┐
│  SNO file header                            (16 bytes)  │
│    0x000: magic = 0xDEADBEEF, u16 version = 26          │
│    0x010: (struct begins here)                          │
├─────────────────────────────────────────────────────────┤
│  ShaderMap struct                           (32 bytes)  │
│    0x010: dwSnoId                                       │
│    0x01C: dwShaderMapFlags   (0 in 1192/1192)           │
│    0x020: SerializeData {byteOffset = 32, byteSize}     │
│    0x028: tag-map pointer    (0 on disk)                │
├─────────────────────────────────────────────────────────┤
│  Tag map payload                           (variable)   │
│    0x030: u32 count                                     │
│    0x034: ShaderMapEntry[count]  (12 bytes each)        │
└─────────────────────────────────────────────────────────┘
```

File sizes run 64 bytes (1 entry) to 172 (10). Entry-count distribution:
1 × 720, 2 × 331, 3 × 69, 4 × 45, 5 × 7, 6 × 13, 7 × 5, 8 × 1, 10 × 1.

---

## 3. ShaderMap Struct

```cpp
struct ShaderMap {                              // 32 bytes, at file 0x010
    i32     dwSnoId;            // 0x00: Unique SNO asset identifier
    u8      _reserved04[8];     // 0x04: Zero
    i32     dwShaderMapFlags;   // 0x0C: 0 in every corpus file
    i32     dataOffset;         // 0x10: SerializeData.byteOffset -- 32, struct-relative
    i32     dataSize;           // 0x14: SerializeData.byteSize == 4 + 12*count
    u64     _runtimePtr;        // 0x18: Nulled on disk
};
```

The field at 0x10/0x14 is registered by `TypeDesc_RegisterField_TagMap`, so the
payload is a standard D3 **tag map**, the same container `.mat` and `.ans` use.

---

## 4. Tag Map Payload

At file offset `0x030`:

```cpp
struct ShaderMapTagMap {
    u32              count;     // number of entries
    ShaderMapEntry   entries[]; // count × 12 bytes
};

struct ShaderMapEntry {                         // 12 bytes
    i32     nValueType;         // 0x00: 2 = SNO reference, in all 1,935 corpus entries
    u32     dwTagId;            // 0x04: which render configuration (see §5)
    u32     snoShader;          // 0x08: Shaders (group 37) SNO id
};
```

`nValueType == 2` in **1,935 / 1,935** entries, so the value word is always an
asset reference rather than an int or float. It points into the **Shaders**
group: the single `.shd` in this corpus,
`actor_add2x_mult2x_unlit_skin_glow.shd` (snoId `0x8855`), is referenced by a
`.shm`. 1,422 distinct shader ids are referenced in total.

---

## 5. Tag IDs

> **Corrected 2026-08-15.** The `0x0003XXYY` "config ID encoding" table that
> stood here — base pass / shadow pass / ambient occlusion / quality variants
> decoded out of the nibbles — was invented. The ids are not a bitfield; they
> are entries in a registered table, and that table survives in the shipped
> binary **with its names**.

The engine keeps them in a 64-byte-stride descriptor table in `.data` at
`0x7101096940` (named `kShaderMapTagDescriptors` in the IDB). Each record is
`{u32 tagId, u32 valueTypeId, TypeDescriptor*, u64, const char* displayName,
const char* symbolName}`. There are 24 ids; the corpus uses 20 of them and
**nothing outside them**.

| id | display name | symbol | files |
| --- | --- | --- | ---: |
| 0x30500 | Default Medium | `TAG_SHADERMAP_MED_DEFAULT` | 1192 |
| 0x30501 | Fade Medium | `TAG_SHADERMAP_MED_FADE` | 343 |
| 0x30502 | Reflection Medium | `TAG_SHADERMAP_MED_REFLECTION` | 13 |
| 0x30504 | Unskinned Medium | `TAG_SHADERMAP_MED_UNSKINNED` | 51 |
| 0x30600 | Default High | `TAG_SHADERMAP_HIGH_DEFAULT` | 57 |
| 0x30601 | Fade High | `TAG_SHADERMAP_HIGH_FADE` | 0 |
| 0x30602 | Reflection High | `TAG_SHADERMAP_HIGH_REFLECTION` | 3 |
| 0x30604 | Unskinned High | `TAG_SHADERMAP_HIGH_UNSKINNED` | 0 |
| 0x30700 | Default Min | `TAG_SHADERMAP_MIN_DEFAULT` | 50 |
| 0x30701 | Fade Min | `TAG_SHADERMAP_MIN_FADE` | 28 |
| 0x30702 | Reflection Min | `TAG_SHADERMAP_MIN_REFLECTION` | 0 |
| 0x30704 | Unskinned Min | `TAG_SHADERMAP_MIN_UNSKINNED` | 0 |
| 0x30800 | Ghost Z pass | `TAG_SHADERMAP_GHOST_ZPASS` | 5 |
| 0x30810 | Highlight | `TAG_SHADERMAP_HIGHLIGHT` | 10 |
| 0x30820 | Occluded | `TAG_SHADERMAP_OCCLUDED` | 1 |
| 0x30830 | Default Console | `TAG_SHADERMAP_CONSOLE_DEFAULT` | 29 |
| 0x30840 | Shadow Cookie | `TAG_SHADERMAP_COOKIE` | 1 |
| 0x30850 | Default High Per-Pixel | `TAG_SHADERMAP_HIGH_PP_DEFAULT` | 46 |
| 0x30860 | Fade High Per-Pixel | `TAG_SHADERMAP_HIGH_PP_FADE` | 21 |
| 0x30861 | Default Alphamask | `TAG_SHADERMAP_ALPHAMASK_DEFAULT` | 19 |
| 0x30870 | Depth Prepass | `TAG_SHADERMAP_PREPASS` | 26 |
| 0x30880 | Stencil Mask | `TAG_SHADERMAP_MASK` | 19 |
| 0x30890 | Depth Prepass (AlphaMask) | `TAG_SHADERMAP_PREPASS_ALPHAMASK` | 20 |
| 0x30891 | Highlight (AlphaMask) | `TAG_SHADERMAP_ALPHAMASK_HIGHLIGHT` | 1 |

So a ShaderMap is keyed by (quality tier × variant) plus a set of special
passes. **Default Medium is the mandatory fallback**: it is present in every one
of the 1,192 files, and 720 files consist of nothing else. The commonest shapes
are `[MED_DEFAULT]` × 720, `[MED_DEFAULT, MED_FADE]` × 238,
`[MED_DEFAULT, MED_FADE, MED_UNSKINNED]` × 29 and
`[MIN_FADE, MIN_DEFAULT, MED_DEFAULT, MED_FADE]` × 28.

A caution on the id space: it is shared with Material. A `.mat`'s
shader-parameter tag map uses 0x30100..0x30404 in the same `0x0003` block (23
ids over 3,843 files) and no id appears in both, but that is a property of how
the tables were allocated, not a rule. The sixth argument to
`TypeDesc_RegisterField_TagMap` — `0x30000` for ShaderMap, `0xA0000` for
Material, `0x50000` for Surface, `0x100000` for RenderPass — is **not** the
tag-id prefix; it is a per-usage registry selector, and tag ids are unique only
within their own table.

---

## 7. Naming Conventions

ShaderMap assets are named using a systematic pattern: `{category}_{blendMode}_{features}`.

### Category Prefixes

| Prefix | Description |
|--------|-------------|
| `scene_` | Static scene geometry (walls, floors, terrain) |
| `actor_` / `actor2_` | Animated characters and monsters |
| `particle_` | Particle effects |
| `prop_` | Interactive/destructible objects |
| `cloth_` | Cloth simulation meshes |
| `tree_` | Foliage with wind animation |
| `rope_` | Rope physics rendering |
| `trail_` | Weapon trail effects |

### Feature Suffixes

| Suffix | Description |
|--------|-------------|
| `_gloss` | Specular/gloss mapping enabled |
| `_bump` | Normal/bump mapping |
| `_glow` / `_bloom` | Self-illumination / bloom pass |
| `_skin` | Subsurface scattering for characters |
| `_5_layer_floor` | 5-layer terrain blending |
| `_sideproj` | Side-projected textures |
| `_lightmap` | Pre-baked lightmap support |
| `_unlit` | Ignores dynamic scene lighting |
| `_edgeAlpha` / `_edgeAlphaComp` | Edge-based transparency falloff |
| `_weather_deform` | Weather-driven vertex displacement |
| `_floor` | Optimized for floor/ground rendering |
| `_ground` | Ground-plane variant |

---

## 8. Corpus Statistics

### File Sizes

| Metric | Value |
|--------|-------|
| Total files | 1,192 |
| Smallest | 64 bytes (1 entry) |
| Largest | 172 bytes (10 entries) |
| Total entries | 1,935 |
| Average entries | 1.62 |
| Distinct shaders referenced | 1,422 |

*(Corrected 2026-08-15. The previous figures — 84/308 bytes, 2..20 entries,
average 12.8, 6,847 references — followed from the 48-byte-preamble reading and
are all wrong. Re-measured over the same 1,192 files.)*

### Most Common ShaderMaps (by Material Reference Count)

| ShaderMap | `.mat` References | Category |
|-----------|-------------------|----------|
| `scene_opaque` | 651 | Scene base |
| `scene_opaque_gloss` | 392 | Scene + specular |
| `scene_foliage_transparent` | 280 | Foliage |
| `scene_opaque_floor` | 242 | Floor base |
| `scene_transparent_ground` | 217 | Ground transparency |
| `scene_opaque_gloss_floor` | 207 | Floor + specular |
| `scene_foliage_transparent_floor` | 180 | Foliage on floors |
| `scene_vertalpha_ground` | 157 | Vertex alpha |
| `scene_opaque_5_layer_floor_sideproj` | 156 | Complex terrain |
| `scene_transparent` | 124 | Scene transparency |

---

## 9. Cross-References

```
Material (.mat)
    │  shaderMapSno
    ▼
ShaderMap (.shm)    ◀── THIS FILE
    │  dwTagId → snoShader
    ▼
Compiled Shaders (.pscod, .vscod)
    ▲
Shader Definition (.shd) — build pipeline metadata
```

| Related Format | Extension | Relationship |
|----------------|-----------|--------------|
| Material | `.mat` | References one ShaderMap via `shaderMapSno` |
| Shader Definition | `.shd` | Build-time metadata for shader compilation |
| Compiled Shader | `.pscod`/`.vscod` | Runtime shader bytecode referenced by `shaderSno` |
| OpenGL Shader | `.ps.glsl`/`.vs.glsl` | ARB assembly shader variants |

---

## 10. Known Unknowns

| Item | Notes |
|------|-------|
| `nValueType` values | Only `2` (SNO reference) observed, in 1,935/1,935 entries. The tag-map container also defines 0 = int and 1 = float, which no ShaderMap uses |
| Config ID full encoding | The `0x0003XXYY` pattern is identified, but exact bit meanings for pass/quality selection are inferred |
| Runtime selection logic | How the engine picks which tag to use at runtime is not established. `TAG_SHADERMAP_MED_DEFAULT` is present in 1,192/1,192 files, so it is at least the mandatory fallback |
| Shader SNO resolution | The `shaderSno` values reference compiled shader assets, but the lookup mechanism (hash table, linear scan) is unknown |

---

## Appendix A: Reading a .shm File

```cpp
FILE* f = fopen(filepath, "rb");

// 16-byte SNO file header, then the 32-byte ShaderMap struct
SnoFileHeader hdr;
fread(&hdr, sizeof(hdr), 1, f);
assert(hdr.magic == 0xDEADBEEF);
assert(hdr.version == 26);

ShaderMap sm;
fread(&sm, sizeof(sm), 1, f);

// dataOffset is struct-relative, so the file position is 16 + it (== 0x30)
fseek(f, 16 + sm.dataOffset, SEEK_SET);

// Read entry count
u32 count;
fread(&count, sizeof(u32), 1, f);

printf("ShaderMap SNO: 0x%06X, %u variants\n", sm.dwSnoId, count);

// Read entries
for (u32 i = 0; i < count; i++) {
    ShaderMapEntry entry;
    fread(&entry, sizeof(ShaderMapEntry), 1, f);

    printf("  [%2u] type=%u config=0x%08X shader=0x%06X\n",
           i, entry.nValueType, entry.dwTagId, entry.snoShader);
}

fclose(f);
```

---

## Appendix B: Complete Structure Summary

```cpp
// §2 — SNO file header (16 bytes) then §3 — ShaderMap struct (32 bytes)
struct SnoFileHeader {                          // 16 bytes
    u32     magic;              // 0xDEADBEEF
    u16     version;            // 26 for .shm
    u8      _reserved06[10];
};

struct ShaderMap {                              // 32 bytes, at file 0x010
    i32     dwSnoId;
    u8      _reserved04[8];
    i32     dwShaderMapFlags;   // 0 in 1192/1192
    i32     dataOffset;         // 32, struct-relative -> file 0x030
    i32     dataSize;           // 4 + count × 12
    u8      _reserved28[8];
};

// §4 — tag map payload (at file 0x030)
struct ShaderMapTagMap {
    u32     count;              // Number of entries

    // §5 — one entry (12 bytes each)
    struct ShaderMapEntry {
        i32 nValueType;         // 2 = SNO reference, in 1935/1935 entries
        u32 dwTagId;            // TAG_SHADERMAP_* -- a table id, not a bitfield
        u32 snoShader;          // Shaders (group 37) SNO id
    } entries[count];
};
```

---

*Specification derived from binary analysis of 1,192 ShaderMap files from Diablo III: Reaper of Souls.*
