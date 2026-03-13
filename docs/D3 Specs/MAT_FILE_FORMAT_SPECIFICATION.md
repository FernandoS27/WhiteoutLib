# MAT File Format Specification

**Format**: Diablo III Material Definition (`.mat`)  
**Byte Order**: Little-endian  
**Magic**: `0xDEADBEEF`  
**Version**: 25  
**Corpus**: 3,843 files analyzed

---

## Table of Contents

1. [Overview](#1-overview)
2. [Material Pipeline Architecture](#2-material-pipeline-architecture)
3. [SNO File Preamble](#3-sno-file-preamble)
4. [Material Header](#4-material-header)
5. [Shader Parameter Overrides](#5-shader-parameter-overrides)
6. [Texture Entries](#6-texture-entries)
7. [APP Embedded Materials](#7-app-embedded-materials)
8. [Texture Slot Assignments](#8-texture-slot-assignments)
9. [Common Shader Parameter IDs](#9-common-shader-parameter-ids)
10. [Corpus Statistics](#10-corpus-statistics)
11. [Known Unknowns](#11-known-unknowns)
12. [Appendix A — Reading a .mat File (C++)](#appendix-a--reading-a-mat-file-c)
13. [Appendix B — All Structures Summary](#appendix-b--all-structures-summary)

---

## 1. Overview

Material files (`.mat`) define surface rendering properties for 3D objects in Diablo III. Each material specifies:

- A **ShaderMap** reference (determining which shader program to use)
- **Color constants** (diffuse, specular, emissive, ambient)
- **Shader parameter overrides** (rendering flags and constants)
- **Texture slot assignments** (texture references with UV transforms)

Materials are standalone SNO assets that can be referenced by multiple models. The same material data structure also appears embedded within `.app` model files (see [§7](#7-app-embedded-materials)).

### File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO File Preamble                          (48 bytes)  │
│    magic, version, snoId, dataOffset, dataSize          │
├─────────────────────────────────────────────────────────┤
│  Material Header                            (88 bytes)  │
│    shaderMapSno, colors[4], emissive[4],                │
│    ambient[4], specularPower, flags,                    │
│    texDataOffset, texDataSize                           │
├─────────────────────────────────────────────────────────┤
│  16-byte zero padding                                   │
├─────────────────────────────────────────────────────────┤
│  Shader Parameter Overrides   (4 + N × 12 bytes)       │
│    count + N × (type, paramId, value)                   │
├─────────────────────────────────────────────────────────┤
│  16-byte zero padding                                   │
├─────────────────────────────────────────────────────────┤
│  Texture Entries              (M × 160 bytes)           │
│    M texture slots with SNO refs + UV transforms        │
└─────────────────────────────────────────────────────────┘
```

---

## 2. Material Pipeline Architecture

Diablo III's material system follows a layered pipeline:

```
Material (.mat)
    │
    ├──→ ShaderMap (.shm)         — Maps render configurations to compiled shaders
    │        │
    │        └──→ Shader (.shd)   — Shader definition with entry points, FX references
    │                  │
    │                  └──→ Compiled Shaders (.pscod/.vscod) — DX9/11 bytecode
    │                  └──→ OpenGL Shaders (.ps.glsl/.vs.glsl) — ARB assembly
    │
    └──→ Textures (.tex)          — Referenced by SNO ID in texture slot entries
```

### Rendering Flow

1. The **Material** specifies a **ShaderMap** via SNO reference
2. The **ShaderMap** contains entries mapping **render configuration IDs** (platform/pass/quality combinations) to **compiled shader SNO references**
3. At runtime, the engine selects the appropriate shader variant from the ShaderMap based on the current rendering context (DX level, shadow pass, etc.)
4. The material's **texture entries** are bound to shader texture samplers by slot index
5. **Shader parameter overrides** provide per-material constant values to the shader

---

## 3. SNO File Preamble

All Diablo III standalone asset files share a common 48-byte preamble. This structure is shared by `.mat`, `.shm`, `.shd`, and other SNO file types.

```cpp
struct SnoFilePreamble {                        // 48 bytes
    u32     magic;              // 0x00: 0xDEADBEEF — file signature
    u32     version;            // 0x04: Format version (25 for .mat, 26 for .shm, 150 for .shd)
    u8      _reserved08[8];     // 0x08: Always zero
    u32     snoId;              // 0x10: SNO hash — unique asset ID in D3 asset database
    u8      _reserved14[12];    // 0x14: Always zero
    u32     dataOffset;         // 0x20: Offset to first data section (+16 for actual data)
    u32     dataSize;           // 0x24: Size in bytes of first data section
    u8      _reserved28[8];     // 0x28: Always zero
};
```

> **+16 Convention**: All offset fields in D3 SNO files point to a 16-byte zero-padding region that precedes the actual data. Add 16 to any stored offset to reach the data entries. This convention appears throughout the `.mat`, `.shm`, `.shd`, and `.app` formats.

---

## 4. Material Header

**Offset**: 48–135 (immediately after the SNO preamble)  
**Size**: 88 bytes

```cpp
struct MaterialHeader {                         // 88 bytes (offset 0x30–0x87)
    // ─── Shader Reference ──────────────────────────────────────────────────────
    u32     shaderMapSno;       // 0x30: SNO reference to a ShaderMap (.shm) asset
                                //       Determines which shader pipeline this material uses

    // ─── Color Constants (0x34–0x53) ───────────────────────────────────────────
    f32     diffuseR;           // 0x34: Diffuse color red   (typically 1.0)
    f32     diffuseG;           // 0x38: Diffuse color green
    f32     diffuseB;           // 0x3C: Diffuse color blue
    f32     diffuseA;           // 0x40: Diffuse color alpha
    f32     specularR;          // 0x44: Specular color red  (typically 1.0)
    f32     specularG;          // 0x48: Specular color green
    f32     specularB;          // 0x4C: Specular color blue
    f32     specularA;          // 0x50: Specular color alpha

    // ─── Emissive Color (0x54–0x63) ────────────────────────────────────────────
    f32     emissiveR;          // 0x54: Emissive color red   (23% of files non-zero)
    f32     emissiveG;          // 0x58: Emissive color green
    f32     emissiveB;          // 0x5C: Emissive color blue
    f32     emissiveA;          // 0x60: Emissive color alpha (rarely set)

    // ─── Ambient Color (0x64–0x73) ─────────────────────────────────────────────
    f32     ambientR;           // 0x64: Ambient color red    (10% of files, small values)
    f32     ambientG;           // 0x68: Ambient color green
    f32     ambientB;           // 0x6C: Ambient color blue
    f32     ambientA;           // 0x70: Ambient color alpha  (rarely set)

    // ─── Specular & Flags (0x74–0x7B) ──────────────────────────────────────────
    f32     specularPower;      // 0x74: Specular shininess exponent (4% of files)
                                //       Common values: 10.0, 40.0, 60.0
    i32     materialFlags;      // 0x78: Material flags / render priority
                                //       Values: -1 (0xFFFFFFFF), -2, 1, 2, etc.
                                //       24% of files set this; -1 is most common

    // ─── Texture Data Reference (0x7C–0x87) ────────────────────────────────────
    u32     texDataOffset;      // 0x7C: Offset to texture entry array (+16 convention)
    u32     texDataSize;        // 0x80: Total byte size of texture entries
    u8      _reserved84[4];     // 0x84: Always zero
};
```

### Color Constant Patterns

| Pattern | Files | Description |
|---------|-------|-------------|
| All 8 floats = 1.0 | 3,514 (91.4%) | Default white materials |
| Diffuse RGBA = 1,1,1,0 | 38 | Transparent diffuse |
| All zeros | 25 | Fully transparent/black |
| Non-default emissive | 889 | Self-illuminating materials |
| Non-default ambient | 372 | Custom ambient response |

---

## 5. Shader Parameter Overrides

**Location**: At `SnoFilePreamble.dataOffset + 16`  
**Size**: `SnoFilePreamble.dataSize` bytes (= 4 + N × 12, where N = number of parameters)

The shader parameter section allows materials to override shader constant values:

```cpp
struct ShaderParamSection {
    u32     count;              // Number of parameter override entries

    struct ParamEntry {         // 12 bytes per entry
        u32     paramType;      // Parameter value type (see table below)
        u32     paramId;        // Shader parameter identifier (see §9)
        u32     value;          // Parameter value (interpretation depends on paramType)
    } entries[];
};
```

### Parameter Value Types

| Type | Interpretation | Example Values |
|------|---------------|----------------|
| 0 | Integer / boolean | 0, 1, 7 |
| 1 | Float (IEEE 754) | 1.0, 0.75, 0.5, 8.0, 256.0 |
| 2 | SNO reference (to texture or other asset) | 0x0000904D, 0x00009045 |

### Data Size Calculation

The `dataSize` field in the preamble always equals `4 + count × 12`:

| dataSize | Entry Count | Occurrence |
|----------|-------------|------------|
| 4 | 0 | 2,027 files (52.7%) |
| 16 | 1 | 1,051 files |
| 28 | 2 | 230 files |
| 40 | 3 | 116 files |
| 52 | 4 | 59 files |
| 64+ | 5+ | 360 files |

---

## 6. Texture Entries

**Location**: At `MaterialHeader.texDataOffset + 16`  
**Size**: `MaterialHeader.texDataSize` bytes  
**Entry size**: 160 bytes (fixed)  
**Entry count**: `texDataSize / 160`

Each texture entry binds a texture asset to a numbered shader sampler slot with UV transform and animation parameters.

```cpp
struct TextureEntry {                           // 160 bytes
    // ─── Slot & Reference (0x00–0x0F) ──────────────────────────────────────────
    u32     slotIndex;          // 0x00: Shader texture sampler slot number (see §8)
    u32     _reserved04;        // 0x04: Always 0
    u32     textureSno;         // 0x08: SNO reference to the texture asset (.tex)
    u32     textureFlags;       // 0x0C: 0=default, 1=has UV transform, 2=has UV scroll

    // ─── UV Transform Matrix (0x10–0x4F) ───────────────────────────────────────
    f32     uvMatrix[16];       // 0x10: 4×4 UV coordinate transform matrix (64 bytes)
                                //       Row-major layout:
                                //       [0]=scaleU  [1]=0  [2]=0  [3]=0
                                //       [4]=0  [5]=scaleV  [6]=0  [7]=0
                                //       [8]=0  [9]=0  [10]=scaleW  [11]=0
                                //       [12]=0 [13]=0 [14]=0 [15]=1.0
                                //
                                //       For simple tiling: [0]=tilingU, [5]=tilingV
                                //       All other elements usually 0.
                                //       Elements [10] and [15] non-zero in ~1,018 entries
                                //       (3D textures or multi-layer projections)

    // ─── UV Animation Parameters (0x50–0x8F) ──────────────────────────────────
    u8      _animReserved50[4]; // 0x50: Rarely non-zero (24 entries)
    f32     uvScrollSpeedU;     // 0x54: UV scroll speed along U axis (1,124 entries)
    f32     _animParam58;       // 0x58: Unknown animation parameter (47 entries)
    f32     _animParam5C;       // 0x5C: Unknown animation parameter (25 entries)
    f32     uvScrollSpeedV;     // 0x60: UV scroll speed along V axis (1,393 entries)
    f32     _animParam64;       // 0x64: Unknown (43 entries)
    f32     _animParam68;       // 0x68: Unknown (22 entries)
    f32     _animParam6C;       // 0x6C: Unknown (113 entries)
    u8      _animReserved70[8]; // 0x70: Rarely non-zero
    f32     _animParam78;       // 0x78: Unknown (24 entries)
    u8      _animReserved7C[4]; // 0x7C: Rarely non-zero (2 entries)
    u8      _animReserved80[8]; // 0x80: Rarely non-zero

    // ─── Texture Type & Trailing Data (0x88–0x9F) ─────────────────────────────
    u8      _reserved88[4];     // 0x88: Rarely non-zero (1 entry)
    u8      _reserved8C[4];     // 0x8C: Zero
    u32     _reserved90;        // 0x90: Rarely non-zero (4 entries)
    u32     _reserved94;        // 0x94: Rarely non-zero (4 entries)
    u32     textureType;        // 0x98: Texture format/sampler type (NOT a float)
                                //       See table below. 0 for most entries
    u32     _reserved9C;        // 0x9C: Always 0
};
```

### textureType Values

| Value | Occurrences | Likely Meaning |
|-------|-------------|----------------|
| 0 | 12,855 | Default (inherit from shader) |
| 3 | 3,135 | DXT5 compressed / has alpha |
| 4 | 726 | RGBA8 uncompressed |
| 1 | 54 | DXT1 compressed / no alpha |
| 2 | 59 | DXT3 compressed |
| 5 | 40 | BC5 / 3Dc normal map |
| 7 | 26 | Unknown |
| 8 | 23 | Unknown |
| 12 | 47 | Cubemap / environment |
| 11 | 3 | Unknown |

### textureFlags Values

| Value | Occurrences | Description |
|-------|-------------|-------------|
| 0 | 1,498 | No UV transform (identity) |
| 1 | 11,226 | Has UV transform matrix |
| 2 | 4,248 | Has UV scroll animation |

---

## 7. APP Embedded Materials

When materials are embedded within `.app` model files, the data layout is similar but wrapped differently. Each `MaterialEntry` (144 bytes) in the APP file contains a `shaderDataOffset` and `shaderDataSize` pointing to the material's data block.

### APP Material Data Block Layout

```cpp
struct AppMaterialDataBlock {                   // Variable size
    // ─── APP-Specific Header (0x00–0x17) ───────────────────────────────────────
    u32     renderGroup;        // 0x00: Render group / object type
    i32     _unknown04;         // 0x04: Usually -1 (0xFFFFFFFF)
    u32     paramSectionOffset; // 0x08: Absolute file offset to shader params (+16 convention)
    u32     paramSectionSize;   // 0x0C: Size of shader param data (4 + N × 12)
    u8      _reserved10[8];     // 0x10: Always zero

    // ─── Shared with .mat header ───────────────────────────────────────────────
    u32     shaderMapSno;       // 0x18: SNO reference to ShaderMap (same as .mat [0x30])
    f32     diffuseR;           // 0x1C: Same color layout as .mat header
    f32     diffuseG;           // 0x20
    f32     diffuseB;           // 0x24
    f32     diffuseA;           // 0x28
    f32     specularR;          // 0x2C
    f32     specularG;          // 0x30
    f32     specularB;          // 0x34
    f32     specularA;          // 0x38
    f32     emissiveRGBA[4];    // 0x3C: Emissive
    f32     ambientRGBA[4];     // 0x4C: Ambient
    f32     specularPower;      // 0x5C: Specular exponent
    i32     materialFlags;      // 0x60: Material flags

    // ─── Texture Data Reference ────────────────────────────────────────────────
    u32     texDataOffset;      // 0x64: Absolute file offset to texture entries (+16)
    u32     texDataSize;        // 0x68: Total texture data size (M × 160)
    u8      _reserved6C[12];    // 0x6C: Zero padding

    // ─── Extended Reserved Area ────────────────────────────────────────────────
    i32     _unknown80;         // 0x80: Sometimes -1 (0xFFFFFFFF)
    u8      _reserved84[124];   // 0x84: Zero padding (total block ~248 bytes)
};
```

> **Key difference from .mat files**: APP material data blocks use **absolute file offsets** for `paramSectionOffset` and `texDataOffset`, whereas standalone `.mat` files use the SNO preamble's `dataOffset` field. Both follow the +16 zero-padding convention.

---

## 8. Texture Slot Assignments

Texture slots map to specific shader sampler registers. The slot numbering depends on the shader program being used (referenced via ShaderMap). The following slot assignments are derived from the 3,843-file corpus analysis:

### Common Texture Slots

| Slot | Uses | Purpose |
|------|------|---------|
| 1 | 3,839 | **Diffuse map** (primary color texture, almost universal) |
| 2 | 777 | **Second diffuse / blend layer** (multi-layer materials) |
| 3 | 1,598 | **Normal map** (bump mapping) |
| 4 | 1,239 | **Specular / gloss map** |
| 5 | 979 | **Emissive / glow map** |
| 6 | 93 | **Environment / reflection map** |
| 8 | 2 | Unknown (very rare) |

### Floor/Terrain Layer Slots (5-layer floor shaders)

| Slot | Uses | Purpose |
|------|------|---------|
| 11 | 535 | Layer 1 diffuse |
| 12 | 488 | Layer 1 normal |
| 13 | 438 | Layer 2 diffuse |
| 14 | 408 | Layer 2 normal |
| 15 | 438 | Layer 3 diffuse |
| 16 | 82 | Layer 3 normal |
| 17 | 413 | Layer 4 diffuse |
| 18 | 398 | Blend / weight map |
| 19 | 690 | Blend / mask map |

### Lightmap / Decal Slots

| Slot | Uses | Purpose |
|------|------|---------|
| 25–38 | ~213 each | Lightmap channels (only in `_5_layer_lightmap_` shaders) |
| 40–41 | 1–3 | Extended lightmap (very rare) |

### Side Projection Slots (for `_sideproj` shaders)

| Slot | Uses | Purpose |
|------|------|---------|
| 47 | 380 | Side projection layer 1 |
| 48 | 356 | Side projection layer 2 |
| 49 | 364 | Side projection layer 3 |
| 50 | 359 | Side projection layer 4 |
| 55 | 39 | Side projection blend map |
| 58 | 58 | Side projection mask |

---

## 9. Common Shader Parameter IDs

Shader parameters use a hierarchical ID scheme: `0x0003XXYY` where `XX` is the parameter group and `YY` is the parameter index within that group.

### Group 0x01 — Material Properties

| ID | Type | Uses | Typical Values | Likely Purpose |
|----|------|------|----------------|----------------|
| 0x00030100 | float | 17 | 0.9 | Overall opacity / alpha cutoff |
| 0x00030102 | float | 1,514 | 1.0 | Specular intensity / diffuse multiplier |
| 0x00030103 | int | 418 | 0 or 1 | Boolean: enable normal mapping |
| 0x00030104 | int | 456 | 0 or 1 | Boolean: enable specular mapping |
| 0x00030105 | float | 17 | varies | Unknown material property |
| 0x00030106 | float | 61 | varies | Unknown material property |
| 0x00030107 | float | 169 | varies | Unknown material property |

### Group 0x02 — Lighting Properties

| ID | Type | Uses | Likely Purpose |
|----|------|------|----------------|
| 0x00030200 | varies | 224 | Light response factor |
| 0x00030201 | varies | 93 | Unknown lighting param |
| 0x00030202 | varies | 101 | Unknown lighting param |
| 0x00030203 | varies | 96 | Unknown lighting param |
| 0x00030204 | varies | 85 | Unknown lighting param |

### Group 0x03 — Unknown

| ID | Type | Uses | Likely Purpose |
|----|------|------|----------------|
| 0x00030300 | varies | 12 | Unknown |
| 0x00030301 | varies | 12 | Unknown |
| 0x00030302 | varies | 9 | Unknown |

### Group 0x04 — Texture/Layer Properties

| ID | Type | Uses | Typical Values | Likely Purpose |
|----|------|------|----------------|----------------|
| 0x00030400 | SNO ref | 355 | 0x0000904D etc. | Layer 1 texture override |
| 0x00030401 | SNO ref | 354 | varies | Layer 2 texture override |
| 0x00030402 | SNO ref | 352 | varies | Layer 3 texture override |
| 0x00030403 | SNO ref | 352 | varies | Layer 4 texture override |
| 0x00030404 | SNO ref | 352 | varies | Layer 5 texture override |

---

## 10. Corpus Statistics

Analysis of 3,843 `.mat` files from the Diablo III game data:

### Shader Usage (Top 20)

| Shader (ShaderMap) | Count | % |
|--------------------|-------|---|
| scene_opaque | 651 | 16.9% |
| scene_opaque_gloss | 392 | 10.2% |
| scene_foliage_transparent | 280 | 7.3% |
| scene_opaque_floor | 242 | 6.3% |
| scene_transparent_ground | 217 | 5.6% |
| scene_opaque_gloss_floor | 207 | 5.4% |
| scene_foliage_transparent_floor | 180 | 4.7% |
| scene_vertalpha_ground | 157 | 4.1% |
| scene_opaque_5_layer_floor_sideproj | 156 | 4.1% |
| scene_transparent | 124 | 3.2% |
| scene_opaque_5_layer_floor | 90 | 2.3% |
| scene_vertalpha | 74 | 1.9% |
| scene_opaque_gloss_5_layer_lightmap_floor_sideproj | 47 | 1.2% |
| scene_skybox_opaque | 46 | 1.2% |
| scene_gloss_5_layer_floor | 44 | 1.1% |
| scene_gloss_transparent_ground | 43 | 1.1% |
| scene_skybox_transparent | 40 | 1.0% |
| scene_transparent_battlenet | 39 | 1.0% |
| scene_vertalpha_5_layer_ground | 34 | 0.9% |
| scene_foliage_transparent_spec | 34 | 0.9% |

### Shader Naming Convention

ShaderMap names follow the pattern: `{category}_{blendMode}_{features}`:

| Category | Description |
|----------|-------------|
| `scene_` | Static scene geometry (walls, floors, terrain) |
| `actor_` / `actor2_` | Animated characters and monsters |
| `particle_` | Particle effects |
| `prop_` | Interactive/destructible objects |
| `cloth_` | Cloth simulation meshes |
| `tree_` | Foliage with special wind animation |
| `rope_` | Rope physics rendering |
| `trail_` | Weapon trail effects |

| Blend Mode | Description |
|------------|-------------|
| `opaque` | Standard opaque rendering |
| `transparent` | Alpha blended |
| `additive` | Additive blending (glow, fire) |
| `alphatest` | Alpha testing (hard cutout) |
| `vertalpha` | Vertex alpha blending |

| Feature Suffix | Description |
|----------------|-------------|
| `_gloss` | Specular/gloss mapping |
| `_bump` | Normal/bump mapping |
| `_glow` / `_bloom` | Self-illumination / bloom pass |
| `_skin` | Subsurface scattering for characters |
| `_5_layer_floor` | 5-layer terrain blending |
| `_sideproj` | Additional side-projected textures |
| `_lightmap` | Pre-baked lightmap support |
| `_herotint` | Hero customization color tinting |
| `_unlit` | Ignores scene lighting |
| `_edgeAlpha` | Edge-based transparency |
| `_weather_deform` | Weather-driven vertex deformation |

### Texture Entry Distribution

| Entry Count | Files | Description |
|-------------|-------|-------------|
| 1 | 1,087 | Simple single-texture materials |
| 2 | 794 | Diffuse + normal |
| 3 | 418 | Diffuse + normal + specular |
| 4 | 681 | Diffuse + normal + specular + emissive |
| 5–10 | 321 | Multi-feature materials |
| 11–16 | 427 | 5-layer floor materials |
| 17+ | 115 | Complex multi-layer + side projection |

---

## 11. Known Unknowns

| Field(s)                    | Offset(s)     | Notes                                          |
|-----------------------------|---------------|-------------------------------------------------|
| `_reserved08[8]`            | 0x08–0x0F     | Standard SNO preamble, always zero              |
| `_reserved14[12]`           | 0x14–0x1F     | Standard SNO preamble, always zero              |
| `_reserved28[8]`            | 0x28–0x2F     | Standard SNO preamble, always zero              |
| `_reserved84[4]`            | 0x84–0x87     | MaterialHeader trailing, always zero            |
| UV anim params 0x58–0x80    | TextureEntry  | Multiple unknown animation parameters           |
| `textureType` semantics     | TextureEntry  | Values 7, 8, 11 purpose unknown                 |
| Shader param groups 0x02–0x03 | §9          | Parameter IDs in groups 2 and 3 not fully mapped |

---

## Appendix A — Reading a .mat File (C++)

```cpp
FILE* f = fopen(filepath, "rb");

// Read SNO preamble
SnoFilePreamble preamble;
fread(&preamble, sizeof(SnoFilePreamble), 1, f);
assert(preamble.magic == 0xDEADBEEF);
assert(preamble.version == 25);

// Read material header
MaterialHeader matHeader;
fread(&matHeader, sizeof(MaterialHeader), 1, f);

printf("ShaderMap SNO: 0x%06X\n", matHeader.shaderMapSno);
printf("Diffuse: (%.2f, %.2f, %.2f, %.2f)\n",
       matHeader.diffuseR, matHeader.diffuseG, matHeader.diffuseB, matHeader.diffuseA);

// Read shader parameter overrides
fseek(f, preamble.dataOffset + 16, SEEK_SET);
u32 paramCount;
fread(&paramCount, sizeof(u32), 1, f);

for (u32 i = 0; i < paramCount; i++) {
    u32 paramType, paramId, paramValue;
    fread(&paramType, sizeof(u32), 1, f);
    fread(&paramId, sizeof(u32), 1, f);
    fread(&paramValue, sizeof(u32), 1, f);

    if (paramType == 1) {
        float fval = *(float*)&paramValue;
        printf("  Param 0x%08X = %.4f (float)\n", paramId, fval);
    } else if (paramType == 2) {
        printf("  Param 0x%08X = 0x%08X (SNO ref)\n", paramId, paramValue);
    } else {
        printf("  Param 0x%08X = %u (int)\n", paramId, paramValue);
    }
}

// Read texture entries
u32 texEntryCount = matHeader.texDataSize / 160;
fseek(f, matHeader.texDataOffset + 16, SEEK_SET);

for (u32 i = 0; i < texEntryCount; i++) {
    TextureEntry tex;
    fread(&tex, sizeof(TextureEntry), 1, f);

    printf("  Texture slot %u: SNO=0x%06X, flags=%u, tiling=(%.1f, %.1f)\n",
           tex.slotIndex, tex.textureSno, tex.textureFlags,
           tex.uvMatrix[0], tex.uvMatrix[5]);
}

fclose(f);
```

---

## Appendix B — All Structures Summary

```cpp
// §3 – SNO File Preamble (shared across D3 asset types)
struct SnoFilePreamble {                        // 48 bytes
    u32 magic; u32 version; u8 _res08[8];
    u32 snoId; u8 _res14[12];
    u32 dataOffset; u32 dataSize; u8 _res28[8];
};

// §4 – Material Header
struct MaterialHeader {                         // 88 bytes
    u32 shaderMapSno;
    f32 diffuseR, diffuseG, diffuseB, diffuseA;
    f32 specularR, specularG, specularB, specularA;
    f32 emissiveR, emissiveG, emissiveB, emissiveA;
    f32 ambientR, ambientG, ambientB, ambientA;
    f32 specularPower; i32 materialFlags;
    u32 texDataOffset; u32 texDataSize; u8 _res[4];
};

// §5 – Shader Parameter Override
struct ShaderParamEntry {                       // 12 bytes
    u32 paramType;      // 0=int, 1=float, 2=SNO ref
    u32 paramId;        // 0x0003XXYY identifier
    u32 value;          // Type-dependent value
};

// §6 – Texture Entry
struct TextureEntry {                           // 160 bytes
    u32 slotIndex; u32 _res04; u32 textureSno; u32 textureFlags;
    f32 uvMatrix[16];           // 4×4 UV transform
    f32 uvAnimParams[16];       // UV scroll/animation (first 64 bytes of 80)
    u32 _resTail[4];            // Trailing data incl. textureType at offset +152
};
```
