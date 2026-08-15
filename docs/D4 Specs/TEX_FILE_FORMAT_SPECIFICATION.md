# TEX File Format Specification — Diablo IV

**Format**: Diablo IV Texture Definition (`.tex`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**SNO Format Hash**: `0xF9CD83E6` (4190995430) — pre-3.0 builds; `0xF9CD83E7` (4190995431) — D4 3.0.x (build 71886, "Fenris 3.0.2") and later. Both resolve to the same `TextureDefinition` schema (type hash `3631735738`); the serialized struct layout is identical.
**SNO Type Name**: `TextureDefinition`
**Corpus**: 16,321 files analyzed (pre-3.0); 20,000-file random sample from D4 3.0.2 confirms identical layout after the hash bump.

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
    - [Cubemap Mip Minimum Allocation](#cubemap-mip-minimum-allocation)
    - [Spherical Harmonics (ptGCoeffs)](#spherical-harmonics-ptgcoeffs)
  - [10. Volume Textures](#10-volume-textures)
  - [11. Import Flags](#11-import-flags)
    - [Known Bit Positions](#known-bit-positions)
    - [Common Flag Combinations](#common-flag-combinations)
  - [12. Pixel Data Storage](#12-pixel-data-storage)
    - [External Payload Model](#external-payload-model)
    - [No Block Shuffling](#no-block-shuffling)
  - [13. Corpus Statistics](#13-corpus-statistics)
    - [Pixel Format Distribution](#pixel-format-distribution)
    - [Dimension Distribution](#dimension-distribution)
    - [Resource Type Distribution](#resource-type-distribution)
    - [Mip Level Range Distribution](#mip-level-range-distribution)
    - [serTex Entry Count Distribution](#sertex-entry-count-distribution)
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

- 16 pixel formats including BC1–BC5 and BC7 block-compressed formats, uncompressed R8 and RGBA, and HDR floating-point
- Full mip chains with configurable level ranges
- Two-tier texture streaming (high-res + low-res payload separation)
- Sprite atlas packing with trim-rect UV coordinates (up to 383 frames observed)
- Cubemap textures with per-face mip chains and spherical harmonic coefficients
- 3D volume textures with configurable slice counts
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
| **Volume textures** | Not supported | Dedicated `dwDepth`, `dwVolumeXSlices`, `dwVolumeYSlices` |
| **Pixel formats** | D3D9-era (DXT1–DXT5, ATI2, A8R8G8B8) | DXGI-era (BC1–BC5, BC7, RGBA16F) |
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

The `TextureDefinition` SNO type contains the following fields (18 total, verified against 16,321 files):

| Field | Type | Description |
|-------|------|-------------|
| `sUIStylePreset` | string (nullable) | UI style preset identifier. Always `null` in corpus. |
| `eTexFormat` | u32 | Pixel format identifier. See [Pixel Formats](#5-pixel-formats). |
| `dwVolumeXSlices` | u32 | Volume texture X slice count. `1` for standard/cubemap textures, `32` for volume textures. |
| `dwVolumeYSlices` | u32 | Volume texture Y slice count. `1` for standard/cubemap textures, `2` for volume textures. |
| `dwWidth` | u16 | Texture width in pixels. Range: 3–11,440. |
| `dwHeight` | u16 | Texture height in pixels. Range: 2–14,272. |
| `dwDepth` | u32 | Texture depth (for 3D textures). `1` for 2D/cubemap textures, `64` for volume textures. |
| `dwFaceCount` | u8 | Number of cubemap faces. `1` for 2D/volume textures, `6` for cubemaps. |
| `dwMipMapLevelMin` | u8 | Minimum (lowest-quality) stored mip level index. Range: 0–10. |
| `dwMipMapLevelMax` | u8 | Maximum (highest-quality) stored mip level index. Range: 0–10. |
| `dwImportFlags` | u32 | Import/processing flags bitfield. See [Import Flags](#11-import-flags). |
| `eTextureResourceType` | u32 | Resource type: `0` = standard 2D, `1` = cubemap probe, `2` = volume texture. |
| `rgbavalAvgColor` | float[4] | Average RGBA color of the texture. Always `(0, 0, 0, 0)` in corpus. |
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

D4 uses 16 pixel format identifiers. Format IDs 0, 9, 10, 12, and 23 are inherited from the D3 numbering scheme; IDs 25 and 41–50 are new to D4. Format 43 is a D3-inherited BC5/ATI2 format ID that appears in a small number of carry-over assets.

| `eTexFormat` | DXGI Equivalent | Bytes per Block/Pixel | Block Size | Count | Description |
|:---:|---|:---:|:---:|:---:|---|
| **0** | `R8G8B8A8_UNORM` | 4 B/px | 1×1 | 12 | Uncompressed RGBA. Lookup tables, UI. |
| **9** | `BC1_UNORM` | 8 B/blk | 4×4 | 336 | Block-compressed RGB, 1-bit alpha. General use. |
| **10** | `BC1_UNORM` (alt) | 8 B/blk | 4×4 | 14 | BC1 variant. Legacy normal maps. |
| **12** | `BC3_UNORM` | 16 B/blk | 4×4 | 200 | BC with interpolated alpha. Color+alpha, dye opacity. |
| **23** | `R8_UNORM` | 1 B/px | 1×1 | 1 | Single-channel 8-bit. Alpha masks. D3 A8 equivalent. |
| **25** | `R16G16B16A16_FLOAT` | 8 B/px | 1×1 | 75 | HDR floating-point. Cubemap light probes, HDR data. |
| **26** | `R32G32B32A32_FLOAT` | 16 B/px | 1×1 | rare | Full-precision HDR. Seen on cinematic "infection map" textures. |
| **41** | `BC4_UNORM` | 8 B/blk | 4×4 | 7,878 | Single-channel. AO, roughness, metalness, masks. |
| **42** | `BC5_UNORM` | 16 B/blk | 4×4 | 2,609 | Two-channel. Normal maps (RG = XY normal). |
| **43** | `BC5_UNORM` (D3 alt) | 16 B/blk | 4×4 | 2 | D3-inherited BC5/ATI2 format ID. Normal maps. |
| **44** | `BC5_SNORM` | 16 B/blk | 4×4 | 11 | Two-channel signed. Body markings, displacement. |
| **45** | `R8G8B8A8_UNORM_SRGB` | 4 B/px | 1×1 | 2 | Uncompressed RGBA with sRGB gamma. UI textures. |
| **46** | `BC1_UNORM` | 8 B/blk | 4×4 | 3,149 | Color (linear), emissive, masks, translucency. |
| **47** | `BC1_UNORM_SRGB` | 8 B/blk | 4×4 | 751 | Color (sRGB gamma). Exclusively diffuse/albedo. |
| **48** | `BC2_UNORM` | 16 B/blk | 4×4 | 1 | Block-compressed with explicit 4-bit alpha. Rare. |
| **49** | `BC3_UNORM` (alt) | 16 B/blk | 4×4 | 1,164 | BC3/DXT5-family RGBA. UI atlases, packed color+alpha. |
| **50** | `BC3_UNORM_SRGB` (alt) | 16 B/blk | 4×4 | 116 | sRGB BC3/DXT5-family color+alpha. |

**Observations:**

- **BC4** (format 41) is the most common format at 48.3%, reflecting D4's PBR material pipeline where single-channel masks (AO, roughness, metalness) dominate.
- Formats 41–50 follow a pattern where **even IDs** tend to be UNORM (linear) variants and **odd IDs** tend to be SRGB or signed variants.
- Formats 9, 10, 46, and 47 all produce BC1 data (8 bytes/block). They differ in semantic use and gamma handling:
  - **9**: General-purpose BC1 (legacy, UI backgrounds)
  - **10**: Legacy normal map encoding (inherited from D3)
  - **46**: Linear-space color, emissive, and mask data (D4-native)
  - **47**: sRGB color/albedo data (D4-native)
- Formats 42 and 43 both produce BC5 data. Format 43 is a D3-era ATI2 format ID that appears in 2 carry-over assets.
- Formats 49 and 50 were **confirmed as BC3** (not BC7) through corpus payload size validation. Earlier analyses incorrectly identified them as BC7.
- Gaps in the numbering (11, 13–22, 24, 26–40) correspond to D3-era format IDs not carried forward to D4.

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

**Verification:** Tested against all serTex entries across 16,321 files. 16,292 of 16,315 2D texture entries match exactly. The 23 mismatches are all cubemap non-final-face entries where a minimum allocation of 512 bytes applies (see [Cubemap Mip Minimum Allocation](#cubemap-mip-minimum-allocation)).

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
- `0x08` → Two-tier streaming (13,914 files)
- `0x00` → Single-tier (684 contiguous + 1,704 single-entry + 19 cubemap files)

**Streaming mode by `dwMipMapLevelMin`:**

| `dwMipMapLevelMin` | Two-Tier | Single-Tier | Total |
|:---:|:---:|:---:|:---:|
| 0 | 3,253 | 1,588 | 4,841 |
| 1 | 5,354 | 333 | 5,687 |
| 2 | 2,575 | 145 | 2,720 |
| 3 | 1,261 | 106 | 1,367 |
| 4 | 480 | 54 | 534 |
| 5 | 114 | 24 | 138 |
| 6+ | 877 | 157 | 1,034 |

---

## 8. Frame / Sprite Atlas System

### Single-Frame Files

The majority of textures (15,896 of 16,321) contain exactly one frame with identity UV coordinates:

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

Atlas textures in the corpus contain 2–383 frames (425 multi-frame files). They predominantly use format 49 (BC3-family RGBA) and often have `dwMipMapLevelMax = 0` (no mipmaps), as atlas textures are typically used for UI elements.

### UV Coordinate System

UV origin is **top-left**: `(0, 0)` = top-left corner, `(1, 1)` = bottom-right corner. This matches Direct3D convention.

---

## 9. Cubemap Textures

### Cubemap Properties

Cubemap textures are identified by `dwFaceCount = 6` and `eTextureResourceType = 1`. In the analyzed corpus, 19 cubemap files were found with the following formats:

| Format | Count | Description |
|:---:|:---:|---|
| 46 (BC1 linear) | 10 | Cookie/light projection cubemaps |
| 9 (BC1) | 4 | Cookie/light projection cubemaps |
| 25 (RGBA16F) | 3 | HDR lighting probes |
| 49 (BC3 alt) | 1 | Light cookie cubemap |
| 43 (BC5 alt) | 1 | Reflection probe cubemap |

All cubemaps have 128×128 or 256×256 face resolution and `eTextureResourceType = 1`. All 19 cubemaps have exactly 1 `ptGCoeffs` entry containing spherical harmonic coefficients.

### SerTex Layout for Cubemaps

For cubemaps, `serTex` entries are stored in **face-major order** with a **fixed stride of 11 per face** (total of 66 entries for all cubemaps in the corpus):

```
stride = 11   (always, regardless of mipMax)

Face 0: serTex[0 .. 10]
Face 1: serTex[11 .. 21]
Face 2: serTex[22 .. 32]
Face 3: serTex[33 .. 43]
Face 4: serTex[44 .. 54]
Face 5: serTex[55 .. 65]
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
  [7]    1×1       512 bytes  (minimum allocation — see below)
  [8]   (padding)     0
  [9]   (padding)     0
  [10]  (padding)     0
Face 1:
  [11] 128×128  131072 bytes
  ...
```

All faces share a single contiguous payload. Offsets are cumulative across all faces.

### Cubemap Mip Minimum Allocation

For cubemap textures, non-final faces (faces 0–4) apply a **minimum allocation of 512 bytes** per mip level entry. This ensures proper alignment between face data blocks in the payload. The final face (face 5) uses the actual computed size with no minimum.

This rule affects only the smallest mip levels where the 256-byte-aligned size would be less than 512 bytes. All 23 mip size mismatches in the corpus are explained by this rule.

Example for a BC1 128×128 cubemap (4×4 mip):
- Computed size: `ceil(4/4) × 8 = 8` → aligned to `256` → 256 bytes
- Faces 0–4: padded to 512 bytes
- Face 5: 256 bytes (actual computed size)

### Spherical Harmonics (ptGCoeffs)

Cubemap light probes (`eTextureResourceType = 1`) include precomputed spherical harmonic coefficients in `ptGCoeffs`. Each entry contains a 3×4 matrix of floats representing L0 + L1 SH bands (order-1 spherical harmonics) for RGB channels:

```
coeff[0][0..3] = R channel (L0, L1_-1, L1_0, L1_1)
coeff[1][0..3] = G channel
coeff[2][0..3] = B channel
```

---

## 10. Volume Textures

Five files in the corpus use `eTextureResourceType = 2`, indicating 3D volume textures:

| Property | Value |
|----------|-------|
| `dwDepth` | 64 |
| `dwVolumeXSlices` | 32 |
| `dwVolumeYSlices` | 2 |
| `dwFaceCount` | 1 |
| `eTextureResourceType` | 2 |

Volume textures encode 3D data as 2D texture slices. The `dwVolumeXSlices × dwVolumeYSlices` product gives the number of slices tiled within each 2D texture mip level. For the observed values (`32 × 2 = 64`), this matches `dwDepth`, confirming that each depth slice occupies one tile position in the atlas layout.

---

## 11. Import Flags

The `dwImportFlags` field is a bitfield controlling texture import and processing behavior. 19 active bit positions were observed across the 16,321-file corpus:

### Known Bit Positions

| Bit | Mask | Corpus Count | Probable Meaning |
|:---:|:---:|:---:|---|
| 0 | `0x00000001` | 16,108 | **Has content** — set for nearly all textures |
| 1 | `0x00000002` | 2 | (Rare, unknown) |
| 3 | `0x00000008` | 1 | (Rare, unknown) |
| 4 | `0x00000010` | 1,745 | **No mipmaps / Special processing** |
| 5 | `0x00000020` | 1,360 | **UI texture** |
| 8 | `0x00000100` | 5 | (Rare, unknown) |
| 9 | `0x00000200` | 5,389 | **sRGB color space** |
| 10 | `0x00000400` | 19 | **Cubemap-related** |
| 11 | `0x00000800` | 399 | **Environment/probe** |
| 14 | `0x00004000` | 9 | (Rare, unknown) |
| 15 | `0x00008000` | 10 | (Rare, unknown) |
| 16 | `0x00010000` | 6 | (Rare, unknown) |
| 17 | `0x00020000` | 2,098 | **Has normal map** |
| 18 | `0x00040000` | 4,887 | **Has packed channels** (ORM/multi-channel) |
| 19 | `0x00080000` | 44 | (Rare, unknown) |
| 23 | `0x00800000` | 182 | **Cubemap data** |
| 25 | `0x02000000` | 19 | (Rare, probe-related) |
| 26 | `0x04000000` | 559 | **Streaming priority / large texture** |
| 28 | `0x10000000` | 1 | (Rare, unknown) |

### Common Flag Combinations

| Value | Count | Typical Use |
|:---:|:---:|---|
| `0x00000001` | 5,787 | Standard texture, no special processing |
| `0x00000201` | 2,199 | sRGB color texture |
| `0x00040201` | 1,956 | sRGB color with packed channels |
| `0x00040001` | 1,267 | Packed channel texture (ORM) |
| `0x00000021` | 755 | UI texture |
| `0x00060001` | 709 | Normal + packed channel |
| `0x00020001` | 684 | Normal map source |
| `0x00000031` | 576 | UI texture with special processing |
| `0x00060201` | 545 | sRGB + normal + packed |
| `0x00000011` | 390 | Special processing |
| `0x00000211` | 219 | sRGB + special processing |
| `0x04000801` | 200 | Environment with streaming priority |
| `0x04800001` | 172 | Cubemap with streaming priority |

> **Note:** Bit meanings are inferred from correlation with texture naming conventions and format usage. They may represent import pipeline hints rather than runtime flags.

---

## 12. Pixel Data Storage

### External Payload Model

Unlike D3 TEX files where pixel data immediately follows the header, **all D4 TEX pixel data resides in external CASC payload files**. The `.tex` file itself contains only the SNO structure.

The relationship between `serTex` entries and CASC payloads:

1. The game engine uses the texture's SNO ID and CASC encoding keys to locate the payload file(s).
2. `serTex[i].dwOffset` gives the byte offset within the payload file.
3. `serTex[i].dwSizeAndFlags` gives the exact byte count of that mip level's data.
4. For two-tier streaming, the high-res and low-res mips are in different CASC payload files.

**Payload validation (16,321-file corpus):**
- High-res payloads: 14,413 of 14,436 match expected sizes (4 mismatches — likely truncated extractions)
- Low-res payloads: 11,996 of 11,999 match expected sizes (3 mismatches — likely truncated extractions)

### No Block Shuffling

D3 TEX files apply a "block shuffling" transform to BC-compressed data, separating block components into planar streams with a 16-byte prefix per mip. **D4 TEX files do NOT use block shuffling.** Pixel data is stored in standard DirectX block-compressed layout:

- BC blocks are stored in **raster scan order** (left-to-right, top-to-bottom).
- No per-mip prefix bytes.
- Row pitch is aligned to 256 bytes (see [Row Pitch Alignment](#row-pitch-alignment)).
- RGBA16F, R8G8B8A8, and R8 data is stored as standard pixel arrays with aligned pitch.

---

## 13. Corpus Statistics

Analysis of 16,321 D4 texture files extracted from CASC. Payload validation performed against 14,436 high-res and 11,999 low-res payload files.

### Pixel Format Distribution

| Format ID | DXGI Format | Count | Percentage |
|:---:|---|:---:|:---:|
| 41 | BC4_UNORM | 7,878 | 48.3% |
| 46 | BC1_UNORM | 3,149 | 19.3% |
| 42 | BC5_UNORM | 2,609 | 16.0% |
| 49 | BC3_UNORM (alt) | 1,164 | 7.1% |
| 47 | BC1_UNORM_SRGB | 751 | 4.6% |
| 9 | BC1_UNORM | 336 | 2.1% |
| 12 | BC3_UNORM | 200 | 1.2% |
| 50 | BC3_UNORM_SRGB (alt) | 116 | 0.7% |
| 25 | R16G16B16A16_FLOAT | 75 | 0.5% |
| 10 | BC1_UNORM (alt) | 14 | 0.1% |
| 0 | R8G8B8A8_UNORM | 12 | 0.1% |
| 44 | BC5_SNORM | 11 | 0.1% |
| 43 | BC5_UNORM (D3 alt) | 2 | < 0.1% |
| 45 | R8G8B8A8_UNORM_SRGB | 2 | < 0.1% |
| 23 | R8_UNORM | 1 | < 0.1% |
| 48 | BC2_UNORM | 1 | < 0.1% |

BC4 (single-channel masks, AO, roughness) is the most common format at 48.3%, reflecting D4's PBR material pipeline.

### Dimension Distribution

Top 20 texture dimensions observed:

| Dimensions | Count | Notes |
|:---:|:---:|---|
| 1024×1024 | 4,366 | Most common, standard material textures |
| 512×512 | 3,172 | Second most common |
| 2048×2048 | 2,121 | High-detail material textures |
| 256×256 | 1,818 | Small detail textures, UI elements |
| 4×4 | 1,414 | Minimal placeholder textures |
| 128×128 | 924 | Small detail textures, cubemap faces |
| 1024×512 | 376 | Rectangular material textures |
| 512×256 | 198 | Character detail maps |
| 64×64 | 194 | Icons, tiny masks |
| 4096×4096 | 169 | Ultra-high-detail surfaces |
| 5120×2160 | 154 | Widescreen UI/cinematic textures |
| 256×128 | 121 | Small rectangular textures |
| 2048×1024 | 118 | Large rectangular textures |
| 512×1024 | 110 | Tall textures |
| 256×512 | 97 | Tall textures |
| 1600×600 | 81 | Panoramic UI textures |
| 808×2616 | 73 | Tall atlas textures |
| 128×256 | 46 | Small tall textures |
| 1024×2048 | 46 | Large tall textures |
| 2560×1080 | 35 | Ultrawide UI textures |

Width range: 3–11,440. Height range: 2–14,272.

### Resource Type Distribution

| `eTextureResourceType` | Count | Description |
|:---:|:---:|---|
| 0 | 16,297 | Standard 2D texture |
| 1 | 19 | Cubemap probe |
| 2 | 5 | Volume texture (3D) |

### Mip Level Range Distribution

| `dwMipMapLevelMin` | Count | Meaning |
|:---:|:---:|---|
| 0 | 4,841 | Full mip chain (or no mips if max=0) |
| 1 | 5,687 | 1 mip dropped from bottom |
| 2 | 2,720 | 2 mips dropped |
| 3 | 1,367 | 3 mips dropped |
| 4 | 534 | 4 mips dropped |
| 5 | 138 | 5 mips dropped |
| 6–10 | 1,034 | Heavily truncated chains |

| `dwMipMapLevelMax` | Count | Meaning |
|:---:|:---:|---|
| 0 | 675 | Single stored mip level |
| 1 | 579 | 2 stored levels |
| 5 | 85 | 6 stored levels |
| 6 | 803 | 7 stored levels |
| 7 | 1,480 | 8 stored levels |
| 8 | 3,413 | 9 stored levels |
| 9 | 7,305 | 10 stored levels (most common) |
| 10 | 1,920 | 11 stored levels |

### serTex Entry Count Distribution

| Entries | Count | Description |
|:---:|:---:|---|
| 1 | 1,704 | Single mip level |
| 2 | 581 | Two mip levels |
| 3–5 | 300 | Small mip chains |
| 6–7 | 3,232 | Medium mip chains |
| 8 | 3,672 | Common mip chain length |
| 9 | 4,527 | Most common entry count |
| 10 | 2,118 | Large mip chains |
| 11 | 168 | Maximum 2D entry count |
| 66 | 19 | Cubemaps (6 faces × 11 stride) |

### Streaming Split Statistics

| Pattern | Count | Description |
|---|:---:|---|
| Two-tier (separate) | 13,914 | High-res mip in separate CASC payload |
| Single-tier (contiguous) | 684 | All mips in one payload |
| Single entry | 1,704 | Only one mip level stored |
| SNO byte 0x17 = `0x08` | 13,914 | Correlates perfectly with two-tier mode |
| SNO byte 0x17 = `0x00` | 2,407 | Correlates with single-tier + single-entry |

### Frame Count Distribution

| Frames | Count | Description |
|:---:|:---:|---|
| 1 | 15,896 | Standard single-image texture |
| 2 | 144 | Small atlas |
| 3–5 | 117 | Small atlas |
| 6–12 | 116 | Medium atlas |
| 13–48 | 30 | Large atlas |
| 49–128 | 13 | Very large atlas |
| 129–383 | 5 | Extreme atlas (max observed: 383 frames) |

### Import Flags Distribution

| Flag bits set | Count |
|:---:|:---:|
| bit 0 only | 5,787 |
| bit 0 + bit 9 | 2,199 |
| bit 0 + bit 9 + bit 18 | 1,956 |
| bit 0 + bit 18 | 1,267 |
| bit 0 + bit 5 | 755 |
| *(see full table in [Import Flags](#11-import-flags))* | |

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
        case  0: case 45: return {1, 4};   // R8G8B8A8 / R8G8B8A8_SRGB
        case 23: return {1, 1};            // R8
        case  9: case 10: case 46: case 47: return {4, 8};   // BC1
        case 12: case 42: case 43: case 44: case 49: case 50: return {4, 16}; // BC3/5
        case 25: return {1, 8};   // RGBA16F
        case 41: return {4, 8};   // BC4
        case 48: return {4, 16};  // BC2
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
| 2 | `eTexFormat` | u32 | — | Pixel format ID (0–50, 16 known values) |
| 3 | `dwVolumeXSlices` | u32 | 1 | Volume X slices (32 for volume textures) |
| 4 | `dwVolumeYSlices` | u32 | 1 | Volume Y slices (2 for volume textures) |
| 5 | `dwWidth` | u16 | — | Texture width (3–11,440) |
| 6 | `dwHeight` | u16 | — | Texture height (2–14,272) |
| 7 | `dwDepth` | u32 | 1 | 1 for 2D/cubemap, 64 for volume textures |
| 8 | `dwFaceCount` | u8 | 1 | 1 = 2D/volume, 6 = cubemap |
| 9 | `dwMipMapLevelMin` | u8 | 0–10 | Lowest stored mip level |
| 10 | `dwMipMapLevelMax` | u8 | 0–10 | Highest stored mip level |
| 11 | `dwImportFlags` | u32 | — | Bitfield (19 active bits) |
| 12 | `eTextureResourceType` | u32 | 0 | 0 = standard, 1 = cubemap probe, 2 = volume |
| 13 | `rgbavalAvgColor` | f32×4 | (0,0,0,0) | Always zero in corpus |
| 14 | `pHotspot` | i32×2 | (0, 0) | Always (0,0) in corpus |
| 15 | `serTex` | array | — | Mip level descriptors |
| 16 | `ptFrame` | array | — | Frame/atlas entries |
| 17 | `ptGCoeffs` | array | [] | SH coefficients (cubemap probes only) |
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
