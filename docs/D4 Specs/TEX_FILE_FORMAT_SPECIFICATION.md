# TEX File Format Specification — Diablo IV

**Format**: Diablo IV Texture Definition (`.tex`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**SNO Format Hash**: `0xF9CD83E6` (4190995430)
**SNO Type Name**: `TextureDefinition`
**Corpus**: 2,438 files analyzed

---

## Table of Contents

- [TEX File Format Specification — Diablo IV](#tex-file-format-specification--diablo-iv)
  - [Table of Contents](#table-of-contents)
  - [1. Overview](#1-overview)
    - [Key Differences from D3](#key-differences-from-d3)
  - [2. File Layout](#2-file-layout)
  - [3. D4 SNO Preamble](#3-d4-sno-preamble)
  - [4. TextureDefinition Structure](#4-texturedefinition-structure)
    - [SerializedTextureMipLevel (serTex)](#serializedtexturemiplevel-sertex)
    - [TextureFrame (ptFrame)](#textureframe-ptframe)
    - [SHCoefficients (ptGCoeffs)](#shcoefficients-ptgcoeffs)
  - [5. Pixel Formats](#5-pixel-formats)
    - [Data Size Calculation](#data-size-calculation)
    - [Row Pitch Alignment](#row-pitch-alignment)
  - [6. Mip Chain Layout](#6-mip-chain-layout)
    - [Mip Level Indexing](#mip-level-indexing)
    - [Mip Count Determination](#mip-count-determination)
    - [Entry-to-Mip Mapping](#entry-to-mip-mapping)
  - [7. Texture Streaming Model](#7-texture-streaming-model)
    - [Two-Tier Payload Architecture](#two-tier-payload-architecture)
    - [Single-Tier Payload](#single-tier-payload)
    - [Detecting the Streaming Mode](#detecting-the-streaming-mode)
  - [8. Frame / Sprite Atlas System](#8-frame--sprite-atlas-system)
    - [Single-Frame Files](#single-frame-files)
    - [Multi-Frame Atlas Files](#multi-frame-atlas-files)
    - [UV Coordinate System](#uv-coordinate-system)
  - [9. Cubemap Textures](#9-cubemap-textures)
    - [Cubemap Properties](#cubemap-properties)
    - [SerTex Layout for Cubemaps](#sertex-layout-for-cubemaps)
    - [Spherical Harmonics (ptGCoeffs)](#spherical-harmonics-ptgcoeffs)
  - [10. Import Flags](#10-import-flags)
    - [Known Bit Positions](#known-bit-positions)
    - [Common Flag Combinations](#common-flag-combinations)
  - [11. Pixel Data Storage](#11-pixel-data-storage)
    - [External Payload Model](#external-payload-model)
    - [No Block Shuffling](#no-block-shuffling)
  - [12. Corpus Statistics](#12-corpus-statistics)
    - [Pixel Format Distribution](#pixel-format-distribution)
    - [Dimension Distribution](#dimension-distribution)
    - [Mip Level Range Distribution](#mip-level-range-distribution)
    - [Streaming Split Statistics](#streaming-split-statistics)
    - [Frame Count Distribution](#frame-count-distribution)
    - [Import Flags Distribution](#import-flags-distribution)
  - [Appendix A — Reading a D4 TEX File (Pseudocode)](#appendix-a--reading-a-d4-tex-file-pseudocode)
  - [Appendix B — Complete Field Reference](#appendix-b--complete-field-reference)
    - [TextureDefinition — All Fields](#texturedefinition--all-fields)
    - [SerializedTextureMipLevel — Fields](#serializedtexturemiplevel--fields)
    - [TextureFrame — Fields](#textureframe--fields)

---

## 1. Overview

Diablo IV `.tex` files are **D4 SNO (Structured Nested Object)** files that describe textures using the `TextureDefinition` type. Unlike Diablo III, where the `.tex` file contains both metadata and pixel data in a monolithic format with hardcoded field offsets, D4 `.tex` files are proper SNO structures containing **only metadata**. All pixel data is stored externally in CASC payload files.

The D4 TEX format supports:

- 12 pixel formats including BC1–BC7 block-compressed formats, uncompressed RGBA, and HDR floating-point
- Full mip chains with configurable level ranges
- Two-tier texture streaming (high-res + low-res payload separation)
- Sprite atlas packing with trim-rect UV coordinates
- Cubemap textures with per-face mip chains
- Spherical harmonic coefficients for light probes
- 256-byte row pitch alignment for GPU-optimal layout

### Key Differences from D3

| Feature | Diablo III | Diablo IV |
|---------|-----------|-----------|
| **File type** | Monolithic binary with hardcoded offsets | D4 SNO structure (`TextureDefinition`) |
| **Pixel data** | Inline after header | External CASC payload |
| **Mip table** | Fixed 480-byte table (60 entries) | Variable-length `serTex` array |
| **Mip range** | `extraMipCount` field | `dwMipMapLevelMin` / `dwMipMapLevelMax` |
| **Streaming** | Not applicable (all data inline) | Two-tier payload split |
| **Row alignment** | None (tightly packed) | 256-byte row pitch |
| **Block shuffling** | Yes (BC blocks separated into planar streams) | No shuffling (standard BC layout) |
| **Cubemap field** | `depth` reused for face count | Dedicated `dwFaceCount` field |
| **Pixel formats** | D3D9-era (DXT1–DXT5, ATI2, A8R8G8B8) | DXGI-era (BC1–BC7, RGBA16F) |
| **Frame UVs** | Separate UV + trim arrays | Unified `ptFrame` struct with all UVs |
| **HDR support** | None | R16G16B16A16_FLOAT (format 25) |

---

## 2. File Layout

A D4 TEX file contains only the SNO header and the serialized `TextureDefinition` structure. Pixel data is stored entirely in external CASC payload files.

```
┌─────────────────────────────────────────────┐
│ D4 SNO Preamble (16 bytes)                  │
├─────────────────────────────────────────────┤
│ TextureDefinition structure (SNO-serialized)│
│   ├── Scalar fields (format, dimensions...) │
│   ├── serTex[] — mip level descriptors      │
│   ├── ptFrame[] — frame/atlas entries        │
│   └── ptGCoeffs[] — SH coefficients         │
├─────────────────────────────────────────────┤
│ (No inline pixel data)                      │
└─────────────────────────────────────────────┘

Total file size: 180–752 bytes (metadata only)
```

Typical file size breakdown:

| Component | Size |
|-----------|------|
| SNO Preamble | 16 bytes |
| Structure fields | 80–100 bytes |
| serTex array (variable) | 8 × entry_count bytes |
| ptFrame array (variable) | 36 × frame_count bytes |
| SNO alignment/padding | Variable |
| **Total** | **~180–752 bytes** |

---

## 3. D4 SNO Preamble

All D4 SNO files share a 16-byte preamble:

| Offset | Size | Field | Value |
|--------|------|-------|-------|
| 0x00 | 4 | `magic` | `0xDEADBEEF` — SNO file signature |
| 0x04 | 4 | `formatHash` | `0xF9CD83E6` — identifies this as `TextureDefinition` |
| 0x08 | 4 | `reserved_0` | Always `0x00000000` |
| 0x0C | 4 | `reserved_1` | Always `0x00000000` |

The structure payload begins at byte offset 16. Fields within the structure are serialized according to D4 SNO alignment rules and are parsed by the SNO type registry, not by hardcoded offsets.

> **Note:** Unlike D3 TEX files which use a fixed binary layout with known offsets for each field, D4 TEX files must be parsed using the SNO type system. The exact byte offsets of fields within the structure may vary with engine updates.

---

## 4. TextureDefinition Structure

The `TextureDefinition` SNO type contains the following fields:

| Field | Type | Description |
|-------|------|-------------|
| `sUIStylePreset` | string (nullable) | UI style preset identifier. Always `null` in observed corpus. |
| `eTexFormat` | u32 | Pixel format identifier. See [Pixel Formats](#5-pixel-formats). |
| `dwVolumeXSlices` | u32 | Volume texture X slice count. `1` for standard textures. |
| `dwVolumeYSlices` | u32 | Volume texture Y slice count. `1` for standard textures. |
| `dwWidth` | u16 | Texture width in pixels. Range: 1–8192. |
| `dwHeight` | u16 | Texture height in pixels. Range: 1–8192. |
| `dwDepth` | u32 | Texture depth (for 3D textures). Always `1` in observed corpus. |
| `dwFaceCount` | u8 | Number of cubemap faces. `1` for 2D textures, `6` for cubemaps. |
| `dwMipMapLevelMin` | u8 | Minimum (lowest-quality) stored mip level index. |
| `dwMipMapLevelMax` | u8 | Maximum (highest-quality) stored mip level index. |
| `dwImportFlags` | u32 | Import/processing flags bitfield. See [Import Flags](#10-import-flags). |
| `eTextureResourceType` | u32 | Resource type: `0` = standard texture, `1` = cubemap probe. |
| `rgbavalAvgColor` | float[4] | Average RGBA color of the texture (linear, 0.0–1.0). |
| `pHotspot` | i32[2] | Hotspot/anchor point (X, Y). Always `(0, 0)` in corpus. |
| `serTex` | array | Array of `SerializedTextureMipLevel` entries. See below. |
| `ptFrame` | array | Array of `TextureFrame` entries. See below. |
| `ptGCoeffs` | array | Spherical harmonic coefficients (cubemap probes only). |
| `ptPostprocessed` | u32 | Post-processing flag. Always `0` in corpus. |

### SerializedTextureMipLevel (serTex)

Each entry in the `serTex` array describes one mip level's pixel data location:

| Field | Type | Description |
|-------|------|-------------|
| `dwOffset` | u32 | Byte offset of this mip level within its payload stream. |
| `dwSizeAndFlags` | u32 | Size in bytes of this mip level's pixel data. |

The array is ordered from **highest quality** (largest mip, index 0) to **lowest quality** (smallest mip, last index). See [Mip Chain Layout](#6-mip-chain-layout) for the full indexing scheme.

### TextureFrame (ptFrame)

Each frame describes a rectangular sub-region of the texture, used for sprite atlas packing:

| Field | Type | Description |
|-------|------|-------------|
| `hImageHandle` | u32 | Hash identifier for this frame (GBID). `0` for single-frame textures. |
| `flU0` | f32 | Left edge UV coordinate (0.0–1.0). |
| `flV0` | f32 | Top edge UV coordinate (0.0–1.0). |
| `flU1` | f32 | Right edge UV coordinate (0.0–1.0). |
| `flV1` | f32 | Bottom edge UV coordinate (0.0–1.0). |
| `flTrimU0` | f32 | Left trim UV (accounts for transparent border removal). |
| `flTrimV0` | f32 | Top trim UV. |
| `flTrimU1` | f32 | Right trim UV. |
| `flTrimV1` | f32 | Bottom trim UV. |

### SHCoefficients (ptGCoeffs)

Present only on cubemap light probe textures (`eTextureResourceType = 1`). Contains spherical harmonic coefficients for ambient lighting reconstruction:

| Field | Type | Description |
|-------|------|-------------|
| `coeff` | f32[3][4] | 3 color channels × 4 SH coefficients (L0 + L1 band). |

---

## 5. Pixel Formats

D4 uses 12 pixel format identifiers. Format IDs 0, 9, 10, and 12 are inherited from the D3 numbering scheme; IDs 25 and 41–50 are new to D4.

| `eTexFormat` | DXGI Equivalent | Bytes per Block/Pixel | Block Size | Description |
|:---:|---|:---:|:---:|---|
| **0** | `R8G8B8A8_UNORM` | 4 B/px | 1×1 | Uncompressed RGBA. Lookup tables. |
| **9** | `BC1_UNORM` | 8 B/blk | 4×4 | Block-compressed RGB, 1-bit alpha. General use. |
| **10** | `BC1_UNORM` (alt) | 8 B/blk | 4×4 | BC1 variant. Used for legacy normal maps. |
| **12** | `BC3_UNORM` | 16 B/blk | 4×4 | BC with interpolated alpha. Color+alpha, dye opacity. |
| **25** | `R16G16B16A16_FLOAT` | 8 B/px | 1×1 | HDR floating-point. Cubemap light probes. |
| **41** | `BC4_UNORM` | 8 B/blk | 4×4 | Single-channel. AO, roughness, metalness, masks. |
| **42** | `BC5_UNORM` | 16 B/blk | 4×4 | Two-channel. Normal maps (RG = XY normal). |
| **44** | `BC5_SNORM` | 16 B/blk | 4×4 | Two-channel signed. Body markings, displacement. |
| **46** | `BC1_UNORM` | 8 B/blk | 4×4 | Color (linear), emissive, masks, translucency. |
| **47** | `BC1_UNORM_SRGB` | 8 B/blk | 4×4 | Color (sRGB gamma). Exclusively diffuse/albedo. |
| **49** | `BC7_UNORM` | 16 B/blk | 4×4 | High-quality RGBA. UI, color+height, fine detail. |
| **50** | `BC7_UNORM_SRGB` | 16 B/blk | 4×4 | High-quality color (sRGB). Diffuse/albedo, gradients. |

**Observations:**

- Formats 41–50 follow a pattern where **even IDs** tend to be UNORM (linear) variants and **odd IDs** tend to be SRGB or signed variants.
- Formats 9, 10, 46, and 47 all produce BC1 data (8 bytes/block). They differ in semantic use and gamma handling:
  - **9**: General-purpose BC1 (UI backgrounds, large compositions)
  - **10**: Legacy normal map encoding (inherited from D3)
  - **46**: Linear-space color, emissive, and mask data
  - **47**: sRGB color/albedo data
- Gaps in the numbering (11, 13–24, 26–40, 43, 45, 48) suggest additional formats may be defined but were not observed in the analyzed corpus.

### Data Size Calculation

For **block-compressed** formats (BC1–BC7):
```
blocks_x = ceil(width / 4)
blocks_y = ceil(height / 4)
raw_size = blocks_x × blocks_y × bytes_per_block
```

For **uncompressed** formats (R8G8B8A8, RGBA16F):
```
raw_size = width × height × bytes_per_pixel
```

### Row Pitch Alignment

All pixel data uses **256-byte row pitch alignment**. For block-compressed formats, the alignment applies to block rows:

```
row_pitch_bytes = blocks_x × bytes_per_block      (for BC formats)
row_pitch_bytes = width × bytes_per_pixel          (for uncompressed)

aligned_pitch = ceil(row_pitch_bytes / 256) × 256

mip_size = aligned_pitch × blocks_y               (for BC formats)
mip_size = aligned_pitch × height                  (for uncompressed)
```

This alignment is **always active** but only produces visible padding for small textures where the natural row pitch is less than 256 bytes. For textures ≥ 64 pixels wide in BC formats, the natural row pitch already exceeds 256 bytes and no padding occurs.

**Verification:** Tested against all 17,485 mip level entries across 2,438 files with **zero mismatches**.

---

## 6. Mip Chain Layout

### Mip Level Indexing

D4 uses a mip level indexing scheme where:

- **Level 0** is the **smallest** mip (e.g., 1×1 or 4×4 for BC formats)
- **Higher levels** represent **larger** mips
- `dwMipMapLevelMax` corresponds to the **full-resolution** texture
- `dwMipMapLevelMin` corresponds to the **smallest stored** mip level

This is the **reverse** of the conventional mip numbering (where level 0 = full resolution).

### Mip Count Determination

```
stored_mip_count = dwMipMapLevelMax - dwMipMapLevelMin + 1
```

For cubemaps:
```
total_serTex_entries = stored_mip_count_per_face × dwFaceCount
                     (with padding — see Cubemap section)
```

For 2D textures:
```
len(serTex) == stored_mip_count
```

### Entry-to-Mip Mapping

`serTex` entries are ordered from **highest quality** (full resolution) to **lowest quality** (smallest mip):

```
serTex[0]  → mip level dwMipMapLevelMax      (full resolution)
serTex[1]  → mip level dwMipMapLevelMax - 1   (half resolution)
serTex[2]  → mip level dwMipMapLevelMax - 2   (quarter resolution)
...
serTex[N]  → mip level dwMipMapLevelMin       (smallest stored)
```

The dimensions of the mip level at `serTex[i]`:
```
mip_width  = max(1, dwWidth  >> i)
mip_height = max(1, dwHeight >> i)
```

> **Note:** `dwMipMapLevelMax` does not strictly equal `log2(max(width, height))` in all cases. For some textures (particularly small or atlas textures), `dwMipMapLevelMax = 0` indicating a single stored level. For others, `dwMipMapLevelMax` may exceed the theoretical maximum, potentially reflecting the engine's virtual resolution tier.

---

## 7. Texture Streaming Model

D4 uses a two-tier texture streaming system. The `serTex` entries describe mip levels that may reside in one or two separate CASC payload files.

### Two-Tier Payload Architecture

When streaming is enabled, the mip chain is split into two payload files:

```
┌────────────────────────────────┐
│ High-Res Payload (Tier 0)      │  ← Streamed on demand
│   serTex[0]: full-res mip      │
│   (offset=0, size=N)           │
└────────────────────────────────┘

┌────────────────────────────────┐
│ Low-Res Payload (Tier 1)       │  ← Always loaded
│   serTex[1]: half-res mip      │  ← offset=0 (new stream!)
│   serTex[2]: quarter-res mip   │  ← contiguous
│   serTex[3]: 1/8-res mip       │
│   ...                          │
│   serTex[N]: smallest mip      │
└────────────────────────────────┘
```

The split is identifiable by the **offset reset**: `serTex[0].dwOffset == 0` AND `serTex[1].dwOffset == 0`. Both entries start at offset 0 within their respective payload files.

- **Tier 0 (high-res):** Contains only `serTex[0]` — the full-resolution mip. Loaded on demand when the texture needs to be displayed at maximum quality.
- **Tier 1 (low-res):** Contains `serTex[1]` through `serTex[N]` — all remaining mips. Always loaded as part of the base game data. Offsets within this tier are contiguous.

### Single-Tier Payload

When streaming is not used, all mip levels reside in a single contiguous payload:

```
┌────────────────────────────────┐
│ Single Payload                 │
│   serTex[0]: full-res mip      │  ← offset=0
│   serTex[1]: half-res mip      │  ← offset = serTex[0].size
│   serTex[2]: quarter-res mip   │  ← contiguous
│   ...                          │
└────────────────────────────────┘
```

All entries have contiguous offsets: `serTex[i+1].dwOffset == serTex[i].dwOffset + serTex[i].dwSizeAndFlags`.

### Detecting the Streaming Mode

The streaming mode can be determined by examining the first two `serTex` entries:

```
if len(serTex) < 2:
    mode = SINGLE_TIER   # Only one mip level, no split possible
elif serTex[0].dwOffset == 0 and serTex[1].dwOffset == 0:
    mode = TWO_TIER       # Offset reset indicates separate payloads
else:
    mode = SINGLE_TIER    # Contiguous offsets, single payload
```

An additional indicator exists in the SNO binary: a flag byte at file offset `0x17` correlates perfectly with the streaming mode:
- `0x08` → Two-tier streaming (2,039 files)
- `0x00` → Single-tier (106 contiguous + 288 single-entry files)

**Streaming mode by `dwMipMapLevelMin`:**

| `dwMipMapLevelMin` | Two-Tier | Single-Tier | Total |
|:---:|:---:|:---:|:---:|
| 0 | 548 | 202 | 750 |
| 1 | 781 | 5 | 786 |
| 2 | 413 | 3 | 416 |
| 3 | 191 | 3 | 194 |
| 4 | 80 | 0 | 80 |
| 5 | 16 | 1 | 17 |
| 6+ | 10 | 165 | 175 |

---

## 8. Frame / Sprite Atlas System

### Single-Frame Files

The majority of textures (2,372 of 2,438) contain exactly one frame with identity UV coordinates:

```json
{
    "hImageHandle": 0,
    "flU0": 0, "flV0": 0, "flU1": 1, "flV1": 1,
    "flTrimU0": 0, "flTrimV0": 0, "flTrimU1": 1, "flTrimV1": 1
}
```

The `hImageHandle` of `0` indicates the frame has no individual addressable hash.

### Multi-Frame Atlas Files

Atlas textures pack multiple sprites into a single texture. Each frame's UV rectangle specifies its sub-region:

```json
{
    "hImageHandle": 3477332278,
    "flU0": 0.0025, "flV0": 0.00431034,
    "flU1": 0.3775, "flV1": 0.974138,
    "flTrimU0": 0.02, "flTrimV0": 0.0646552,
    "flTrimU1": 0.37, "flTrimV1": 0.905172
}
```

Key properties:
- `hImageHandle` is a non-zero GBID hash used to look up individual sprites within the atlas.
- `flU0/flV0/flU1/flV1` define the bounding rectangle in atlas UV space.
- `flTrimU0/flTrimV0/flTrimU1/flTrimV1` define the visible (non-transparent) content region, accounting for transparent border trimming during atlas packing.

Atlas textures in the corpus contain 2–48 frames. They predominantly use format 49 (BC7_UNORM) and have `dwMipMapLevelMax = 0` (no mipmaps), as atlas textures are typically used for UI elements.

### UV Coordinate System

UV origin is **top-left**: `(0, 0)` = top-left corner, `(1, 1)` = bottom-right corner. This matches Direct3D convention.

---

## 9. Cubemap Textures

### Cubemap Properties

Cubemap textures are identified by `dwFaceCount = 6`. In the analyzed corpus, 5 cubemap files were found, all with:
- `eTexFormat = 25` (RGBA16F, 4 files) or `46` (BC1, 1 file)
- `eTextureResourceType = 1` (probe texture) for RGBA16F probes
- 128×128 or 256×256 face resolution

### SerTex Layout for Cubemaps

For cubemaps, `serTex` entries are stored in **face-major order** with a **fixed stride per face**:

```
stride = dwMipMapLevelMax + 1 + padding_entries

Face 0: serTex[0 .. stride-1]
Face 1: serTex[stride .. 2×stride-1]
...
Face 5: serTex[5×stride .. 6×stride-1]
```

Within each face's block:
- The first `(dwMipMapLevelMax - dwMipMapLevelMin + 1)` entries contain valid mip data.
- Remaining entries are zero-filled padding (`dwOffset = 0, dwSizeAndFlags = 0`).

Example for a 128×128 RGBA16F cubemap (`dwMipMapLevelMin = 0`, `dwMipMapLevelMax = 7`):

```
Face 0:
  [0]  128×128  131072 bytes
  [1]   64×64    32768 bytes
  [2]   32×32     8192 bytes
  [3]   16×16     4096 bytes  (with 256-byte row alignment)
  [4]    8×8      2048 bytes
  [5]    4×4      1024 bytes
  [6]    2×2       512 bytes
  [7]    1×1       512 bytes  (minimum allocation size)
  [8]   (padding)     0
  [9]   (padding)     0
  [10]  (padding)     0
Face 1:
  [11] 128×128  131072 bytes
  ...
```

All faces share a single contiguous payload. Offsets are cumulative across all faces.

### Spherical Harmonics (ptGCoeffs)

Cubemap light probes (`eTextureResourceType = 1`) include precomputed spherical harmonic coefficients in `ptGCoeffs`. Each entry contains a 3×4 matrix of floats representing L0 + L1 SH bands (order-1 spherical harmonics) for RGB channels:

```
coeff[0][0..3] = R channel (L0, L1_-1, L1_0, L1_1)
coeff[1][0..3] = G channel
coeff[2][0..3] = B channel
```

---

## 10. Import Flags

The `dwImportFlags` field is a bitfield controlling texture import and processing behavior. 14 active bit positions were observed:

### Known Bit Positions

| Bit | Mask | Corpus Count | Probable Meaning |
|:---:|:---:|:---:|---|
| 0 | `0x00000001` | 2,420 | **Has content** — set for nearly all textures |
| 4 | `0x00000010` | 207 | **No mipmaps / Special processing** |
| 5 | `0x00000020` | 151 | **UI texture** |
| 9 | `0x00000200` | 490 | **sRGB color space** |
| 10 | `0x00000400` | 3 | (Rare, unknown) |
| 11 | `0x00000800` | 44 | **Environment/probe** |
| 14 | `0x00004000` | 2 | (Rare, unknown) |
| 15 | `0x00008000` | 2 | (Rare, unknown) |
| 17 | `0x00020000` | 213 | **Has normal map** |
| 18 | `0x00040000` | 557 | **Has packed channels** (ORM/multi-channel) |
| 19 | `0x00080000` | 4 | (Rare, unknown) |
| 23 | `0x00800000` | 33 | **Cubemap data** |
| 25 | `0x02000000` | 5 | (Rare, probe-related) |
| 26 | `0x04000000` | 79 | **Streaming priority / large texture** |

### Common Flag Combinations

| Value | Count | Typical Use |
|:---:|:---:|---|
| `0x00000001` | 833 | Standard texture, no special processing |
| `0x00000201` | 302 | sRGB color texture |
| `0x00040201` | 290 | sRGB color with packed channels |
| `0x00040001` | 191 | Packed channel texture (ORM) |
| `0x00060001` | 123 | Normal + packed channel |
| `0x00000021` | 122 | UI texture |
| `0x00020001` | 112 | Normal map source |
| `0x00000031` | 83 | UI texture with special processing |
| `0x00000011` | 80 | Special processing |
| `0x00060201` | 72 | sRGB + normal + packed |
| `0x04800001` | 33 | Cubemap with streaming priority |

> **Note:** Bit meanings are inferred from correlation with texture naming conventions and format usage. They may represent import pipeline hints rather than runtime flags.

---

## 11. Pixel Data Storage

### External Payload Model

Unlike D3 TEX files where pixel data immediately follows the header, **all D4 TEX pixel data resides in external CASC payload files**. The `.tex` file itself contains only the SNO structure.

The relationship between `serTex` entries and CASC payloads:

1. The game engine uses the texture's SNO ID and CASC encoding keys to locate the payload file(s).
2. `serTex[i].dwOffset` gives the byte offset within the payload file.
3. `serTex[i].dwSizeAndFlags` gives the exact byte count of that mip level's data.
4. For two-tier streaming, the high-res and low-res mips are in different CASC payload files.

### No Block Shuffling

D3 TEX files apply a "block shuffling" transform to BC-compressed data, separating block components into planar streams with a 16-byte prefix per mip. **D4 TEX files do NOT use block shuffling.** Pixel data is stored in standard DirectX block-compressed layout:

- BC blocks are stored in **raster scan order** (left-to-right, top-to-bottom).
- No per-mip prefix bytes.
- Row pitch is aligned to 256 bytes (see [Row Pitch Alignment](#row-pitch-alignment)).
- RGBA16F and R8G8B8A8 data is stored as standard pixel arrays with aligned pitch.

---

## 12. Corpus Statistics

Analysis of 2,438 D4 texture files extracted from CASC.

### Pixel Format Distribution

| Format ID | DXGI Format | Count | Percentage |
|:---:|---|:---:|:---:|
| 41 | BC4_UNORM | 1,135 | 46.6% |
| 46 | BC1_UNORM | 529 | 21.7% |
| 42 | BC5_UNORM | 362 | 14.8% |
| 49 | BC7_UNORM | 182 | 7.5% |
| 47 | BC1_UNORM_SRGB | 111 | 4.6% |
| 9 | BC1_UNORM | 51 | 2.1% |
| 12 | BC3_UNORM | 27 | 1.1% |
| 50 | BC7_UNORM_SRGB | 20 | 0.8% |
| 25 | R16G16B16A16_FLOAT | 8 | 0.3% |
| 44 | BC5_SNORM | 7 | 0.3% |
| 10 | BC1_UNORM (alt) | 5 | 0.2% |
| 0 | R8G8B8A8_UNORM | 1 | < 0.1% |

BC4 (single-channel masks, AO, roughness) is the most common format, reflecting D4's PBR material pipeline.

### Dimension Distribution

Common texture dimensions observed:

| Dimensions | Count | Notes |
|:---:|:---:|---|
| 2048×2048 | 605 | Most common, standard material textures |
| 1024×1024 | 533 | Second most common |
| 512×256 | 106 | Common for character detail maps |
| 4096×4096 | 82 | High-detail surfaces |
| 128×128 | 181 | Small detail textures, cubemap faces |
| 256×256 | 151 | UI elements, small textures |
| 64×64 | 96 | Icons, tiny masks |
| Various NPOT | ~150 | UI atlas packing (400×232, 248×912, etc.) |

Maximum observed: 8192×8192. Minimum observed: 2×4.

### Mip Level Range Distribution

| `dwMipMapLevelMin` | Count | Meaning |
|:---:|:---:|---|
| 0 | 755 | Full mip chain (or no mips if max=0) |
| 1 | 786 | 1 mip dropped from bottom |
| 2 | 416 | 2 mips dropped |
| 3 | 194 | 3 mips dropped |
| 4–10 | 287 | Heavily truncated chains |

### Streaming Split Statistics

| Pattern | Count | Description |
|---|:---:|---|
| Two-tier (separate) | 2,039 | High-res mip in separate CASC payload |
| Single-tier (contiguous) | 106 | All mips in one payload |
| Single entry | 288 | Only one mip level stored |

### Frame Count Distribution

| Frames | Count | Description |
|:---:|:---:|---|
| 1 | 2,372 | Standard single-image texture |
| 2–5 | 38 | Small atlas |
| 6–12 | 21 | Medium atlas |
| 13–48 | 7 | Large atlas |

### Import Flags Distribution

| Flag bits set | Count |
|:---:|:---:|
| bit 0 only | 833 |
| bit 0 + bit 9 | 302 |
| bit 0 + bit 9 + bit 18 | 290 |
| bit 0 + bit 18 | 191 |
| bit 0 + bit 17 + bit 18 | 123 |
| *(see full table in [Import Flags](#10-import-flags))* | |

---

## Appendix A — Reading a D4 TEX File (Pseudocode)

```cpp
struct SerializedTextureMipLevel {
    uint32_t dwOffset;
    uint32_t dwSizeAndFlags;
};

struct TextureFrame {
    uint32_t hImageHandle;
    float    flU0, flV0, flU1, flV1;
    float    flTrimU0, flTrimV0, flTrimU1, flTrimV1;
};

struct TextureDefinition {
    // Parsed from D4 SNO structure — field order/offsets determined by type registry
    uint32_t eTexFormat;
    uint32_t dwVolumeXSlices;
    uint32_t dwVolumeYSlices;
    uint16_t dwWidth;
    uint16_t dwHeight;
    uint32_t dwDepth;
    uint8_t  dwFaceCount;
    uint8_t  dwMipMapLevelMin;
    uint8_t  dwMipMapLevelMax;
    uint32_t dwImportFlags;
    uint32_t eTextureResourceType;
    float    rgbavalAvgColor[4];
    int32_t  pHotspot[2];
    std::vector<SerializedTextureMipLevel> serTex;
    std::vector<TextureFrame> ptFrame;
    // ... ptGCoeffs, ptPostprocessed
};

// Parse the D4 SNO file
auto texDef = SnoReader::parse<TextureDefinition>(fileData);

// Determine streaming mode
bool isTwoTier = texDef.serTex.size() >= 2
              && texDef.serTex[0].dwOffset == 0
              && texDef.serTex[1].dwOffset == 0;

// Load pixel data from CASC
std::vector<uint8_t> hiResPayload, loResPayload;
if (isTwoTier) {
    hiResPayload = casc.readFile(hiResEncodingKey);   // serTex[0]
    loResPayload = casc.readFile(loResEncodingKey);   // serTex[1..N]
} else {
    loResPayload = casc.readFile(singleEncodingKey);  // all mips
}

// Compute aligned mip sizes
auto getBlockInfo = [](uint32_t fmt) -> std::pair<int, int> {
    switch (fmt) {
        case  0: return {1, 4};   // R8G8B8A8
        case  9: case 10: case 46: case 47: return {4, 8};   // BC1
        case 12: case 42: case 44: case 49: case 50: return {4, 16}; // BC3/5/7
        case 25: return {1, 8};   // RGBA16F
        case 41: return {4, 8};   // BC4
        default: return {1, 1};
    }
};

auto [blockDim, bytesPerBlock] = getBlockInfo(texDef.eTexFormat);

for (size_t i = 0; i < texDef.serTex.size(); ++i) {
    uint32_t mipW = std::max(1u, texDef.dwWidth  >> i);
    uint32_t mipH = std::max(1u, texDef.dwHeight >> i);

    uint32_t blocksX = (blockDim > 1)
        ? (mipW + blockDim - 1) / blockDim
        : mipW;
    uint32_t rowsY = (blockDim > 1)
        ? (mipH + blockDim - 1) / blockDim
        : mipH;

    uint32_t rowPitch = blocksX * bytesPerBlock;
    uint32_t alignedPitch = ((rowPitch + 255) / 256) * 256;
    uint32_t expectedSize = alignedPitch * rowsY;

    // Read from appropriate payload
    auto& payload = (isTwoTier && i == 0) ? hiResPayload : loResPayload;
    uint32_t offset = texDef.serTex[i].dwOffset;
    uint32_t size   = texDef.serTex[i].dwSizeAndFlags;

    // payload[offset .. offset + size] contains the mip data
    // If alignedPitch > rowPitch, strip padding per row before use
}
```

---

## Appendix B — Complete Field Reference

### TextureDefinition — All Fields

| # | Field | Type | Default/Typical | Notes |
|---|-------|------|:---:|---|
| 1 | `sUIStylePreset` | string? | `null` | Always null in corpus |
| 2 | `eTexFormat` | u32 | — | Pixel format ID (0–50) |
| 3 | `dwVolumeXSlices` | u32 | 1 | Volume X slices (`0` for 1 file) |
| 4 | `dwVolumeYSlices` | u32 | 1 | Volume Y slices |
| 5 | `dwWidth` | u16 | — | Texture width (1–8192) |
| 6 | `dwHeight` | u16 | — | Texture height (1–8192) |
| 7 | `dwDepth` | u32 | 1 | Always 1 in corpus |
| 8 | `dwFaceCount` | u8 | 1 | 1 = 2D, 6 = cubemap |
| 9 | `dwMipMapLevelMin` | u8 | 0–10 | Lowest stored mip level |
| 10 | `dwMipMapLevelMax` | u8 | 0–10 | Highest stored mip level |
| 11 | `dwImportFlags` | u32 | — | Bitfield (14 active bits) |
| 12 | `eTextureResourceType` | u32 | 0 | 0 = standard, 1 = probe |
| 13 | `rgbavalAvgColor` | f32×4 | — | Average linear RGBA |
| 14 | `pHotspot` | i32×2 | (0, 0) | Anchor point |
| 15 | `serTex` | array | — | Mip level descriptors |
| 16 | `ptFrame` | array | — | Frame/atlas entries |
| 17 | `ptGCoeffs` | array | [] | SH coefficients (probes) |
| 18 | `ptPostprocessed` | u32 | 0 | Always 0 in corpus |

### SerializedTextureMipLevel — Fields

| Field | Type | Description |
|-------|------|-------------|
| `dwOffset` | u32 | Byte offset within payload stream |
| `dwSizeAndFlags` | u32 | Mip level data size in bytes |

### TextureFrame — Fields

| Field | Type | Description |
|-------|------|-------------|
| `hImageHandle` | u32 | Frame GBID hash (0 = unnamed) |
| `flU0` | f32 | Atlas left UV |
| `flV0` | f32 | Atlas top UV |
| `flU1` | f32 | Atlas right UV |
| `flV1` | f32 | Atlas bottom UV |
| `flTrimU0` | f32 | Trimmed content left UV |
| `flTrimV0` | f32 | Trimmed content top UV |
| `flTrimU1` | f32 | Trimmed content right UV |
| `flTrimV1` | f32 | Trimmed content bottom UV |
