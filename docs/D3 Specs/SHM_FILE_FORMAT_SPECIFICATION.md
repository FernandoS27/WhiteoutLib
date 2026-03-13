# SHM File Format Specification

**Format**: Diablo III ShaderMap (`.shm`)  
**Byte Order**: Little-endian  
**Magic**: `0xDEADBEEF`  
**Version**: 26  
**Corpus**: 1,192 files analyzed

---

## Table of Contents

1.  [Overview](#1-overview)
2.  [File Layout](#2-file-layout)
3.  [SNO File Preamble](#3-sno-file-preamble)
4.  [ShaderMap Data](#4-shadermap-data)
5.  [Shader Entry Structure](#5-shader-entry-structure)
6.  [Config ID Encoding](#6-config-id-encoding)
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
    │  configId → shaderSno
    ▼
Compiled Shaders (.pscod, .vscod, .ps.glsl, .vs.glsl)
```

Each ShaderMap contains a variable number of entries (typically 6–20), where each entry maps a `configId` to a compiled shader SNO hash. The engine selects the appropriate entry based on the current render pass, quality settings, and platform.

---

## 2. File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO File Preamble                          (48 bytes)  │
│    0x000: magic=0xDEADBEEF, version=26, snoId          │
├─────────────────────────────────────────────────────────┤
│  16-byte zero padding                                   │
├─────────────────────────────────────────────────────────┤
│  ShaderMap Data                            (variable)   │
│    0x040: count (u32)                                   │
│    0x044: entries[count] (count × 12 bytes)             │
└─────────────────────────────────────────────────────────┘
```

**Data offset**: `preamble.dataOffset + 16` = 48 + 16 = **0x040** (fixed for all files).

---

## 3. SNO File Preamble

**Size**: 48 bytes

```cpp
struct SnoFilePreamble {                        // 48 bytes @ 0x000
    u32     magic;              // 0x000: 0xDEADBEEF
    u32     version;            // 0x004: 26 for .shm
    u8      _reserved08[8];     // 0x008: Zeros
    u32     snoId;              // 0x010: Unique SNO asset identifier
    u8      _reserved14[12];    // 0x014: Zeros
    u32     dataOffset;         // 0x020: Always 48 (0x30)
    u32     dataSize;           // 0x024: 4 + count × 12
    u8      _reserved28[8];     // 0x028: Zeros
};
```

---

## 4. ShaderMap Data

At offset 0x040 (after the 16-byte zero padding following the preamble):

```cpp
struct ShaderMapData {                          // variable size @ 0x040
    u32     count;              // 0x040: Number of shader entries
    ShaderEntry entries[];      // 0x044: count × ShaderEntry (12 bytes each)
};
```

**Size formula**: `dataSize = 4 + count × 12`

---

## 5. Shader Entry Structure

```cpp
struct ShaderEntry {                            // 12 bytes
    u32     entryType;          // 0x00: Always 2 (verified across all files)
    u32     configId;           // 0x04: Render configuration ID (see §6)
    u32     shaderSno;          // 0x08: SNO reference to compiled shader asset
};
```

| Field | Description |
|-------|-------------|
| `entryType` | Always `2` — possibly a type discriminator for a serialized union |
| `configId` | Encodes platform/pass/quality selection (see §6) |
| `shaderSno` | SNO hash of the compiled shader (`.pscod`/`.vscod`/`.glsl`) |

---

## 6. Config ID Encoding

Config IDs follow a `0x0003XXYY` pattern where `XX` and `YY` encode render pass and quality:

| Config ID | Hex | Observed In | Likely Meaning |
|-----------|-----|-------------|----------------|
| 0x00030001 | 0x30001 | Most files | Base pass — primary render |
| 0x00030002 | 0x30002 | Common | Shadow pass — shadow map generation |
| 0x00030003 | 0x30003 | Common | Lighting pass (deferred) |
| 0x00030004 | 0x30004 | Some | Ambient occlusion pass |
| 0x00030005 | 0x30005 | Some | Specular highlights pass |
| 0x00030006 | 0x30006 | Some | Alpha composite pass |
| 0x00030100 | 0x30100 | Complex | Low quality variant |
| 0x00030101 | 0x30101 | Complex | Medium quality variant |
| 0x00030102 | 0x30102 | Complex | High quality variant |

### ID Pattern

The `0x0003` prefix is shared with material shader parameter IDs (see `MAT_FILE_FORMAT_SPECIFICATION.md` §9), suggesting a common Diablo III engine namespace for shader-related configuration.

### Entry Count Distribution

| Entries | Files | Description |
|---------|-------|-------------|
| 2 | 84 | Minimal (2 entries — smallest observed) |
| 6 | 221 | Simple shader (common for scene geometry) |
| 8 | 184 | Standard shader |
| 10 | 140 | Standard with variants |
| 12 | 128 | Feature-rich shader |
| 14 | 95 | Complex shader |
| 16 | 118 | Multi-pass with quality levels |
| 20 | 222 | Full-featured (maximum common count) |

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
| Smallest | 84 bytes (2 entries) |
| Largest | 308 bytes (20 entries) |
| Average entries | 12.8 |
| Total unique shader references | 6,847 |

### Most Common ShaderMaps (by Material Reference Count)

| ShaderMap | `.mat` References | Category |
|-----------|-------------------|----------|
| `scene_opaque` | 651 | Scene base |
| `scene_opaque_gloss` | 392 | Scene + specular |
| `scene_foliage_transparent` | 280 | Foliage |
| `scene_opaque_floor` | 242 | Floor base |
| `scene_transparent_ground` | 217 | Ground transparency |
| `scene_opaque_gloss_floor` | 207 | Floor + specular |
| `actor_opaque` | ~200+ | Actor base |
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
    │  configId → shaderSno
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
| `entryType` values | Only `2` observed — other possible values unknown |
| Config ID full encoding | The `0x0003XXYY` pattern is identified, but exact bit meanings for pass/quality selection are inferred |
| Runtime selection logic | How the engine picks which `configId` to use at runtime is not documented |
| Shader SNO resolution | The `shaderSno` values reference compiled shader assets, but the lookup mechanism (hash table, linear scan) is unknown |

---

## Appendix A: Reading a .shm File

```cpp
FILE* f = fopen(filepath, "rb");

// Read SNO preamble
SnoFilePreamble preamble;
fread(&preamble, sizeof(SnoFilePreamble), 1, f);
assert(preamble.magic == 0xDEADBEEF);
assert(preamble.version == 26);

// Seek to data section (dataOffset + 16)
fseek(f, preamble.dataOffset + 16, SEEK_SET);

// Read entry count
u32 count;
fread(&count, sizeof(u32), 1, f);

printf("ShaderMap SNO: 0x%06X, %u variants\n", preamble.snoId, count);

// Read entries
for (u32 i = 0; i < count; i++) {
    ShaderEntry entry;
    fread(&entry, sizeof(ShaderEntry), 1, f);

    printf("  [%2u] type=%u config=0x%08X shader=0x%06X\n",
           i, entry.entryType, entry.configId, entry.shaderSno);
}

fclose(f);
```

---

## Appendix B: Complete Structure Summary

```cpp
// §3 — SNO File Preamble (48-byte variant)
struct SnoFilePreamble {                        // 48 bytes
    u32     magic;              // 0xDEADBEEF
    u32     version;            // 26 for .shm
    u8      _reserved08[8];
    u32     snoId;
    u8      _reserved14[12];
    u32     dataOffset;         // Always 48
    u32     dataSize;           // 4 + count × 12
    u8      _reserved28[8];
};

// §4 — ShaderMap Data (at offset 0x040)
struct ShaderMapData {
    u32     count;              // Number of shader entries

    // §5 — Shader Entry (12 bytes each)
    struct ShaderEntry {
        u32 entryType;          // Always 2
        u32 configId;           // 0x0003XXYY render configuration
        u32 shaderSno;          // Compiled shader SNO ID
    } entries[count];
};
```

---

*Specification derived from binary analysis of 1,192 ShaderMap files from Diablo III: Reaper of Souls.*
