# SHD File Format Specification

**Format**: Diablo III Shader Definition (`.shd`)  
**Byte Order**: Little-endian  
**Magic**: `0xDEADBEEF`  
**Version**: 150  
**SNO Group**: 37 (`Shaders`)
**Corpus**: **1 usable file** — `Corpus/D3/Shaders` holds two entries but one is a 2-byte
`.shd.tags` sidecar, not an SNO asset

See [README.md](README.md) for the build these offsets come from, the generator pipeline and
the conventions used below.

---

> ## Correction pass — 2026-08-16
>
> The byte-level reading in this document is **correct** and reproduces against the sample.
> What was wrong is the framing: there is no "48-byte preamble with repurposed fields". Like
> every SNO asset, a `.shd` is a **16-byte file header followed by the struct image**, and the
> binary registers that struct. Reframed:
>
> | Old description | File offset | Actually | struct offset |
> | --- | --- | --- | --- |
> | `_unknown1C` = 64 | 0x01C | `dwShaderFlags` | +0x0C |
> | `dataOffset` = 0 | 0x020 | `dwUnknown10` | +0x10 |
> | `dataSize` = 2 ("count, not size") | 0x024 | `dwRenderPassCount` | +0x14 |
> | `programBlockOffset` | 0x028 | `arRenderPasses` — `SerializeData.offset` | +0x18 |
> | `programBlockSize` | 0x02C | `arRenderPasses` — `SerializeData.byteSize` | +0x1C |
> | shader name | 0x038 | `szName`, inline `char[256]` | +0x28 |
>
> Nothing was "repurposed": that is the ordinary
> `{count, SerializeData}` variable-array idiom used by every other D3 group, and the reason
> `dataSize` looked like a count is that it *is* the count field, sitting one slot before the
> descriptor.
>
> **The struct is 296 bytes**, and the sample confirms it three ways: `szName` runs 0x28…0x128
> (40 + 256 = 296); `arRenderPasses.offset` is exactly **296**, so the payload begins the byte
> after the struct; and 16 + 296 = 312 = 0x138, the "block 0 starts at 0x138" this document
> already recorded.
>
> **Version skew is large here.** The shipped data is v150; the binary's compiled struct is
> revision **187 at just 56 bytes**, where the inline `char[256]` name has become an 8-byte
> reference and two fields carry the post-v0 flag `0x700000`. Any reader keyed on the header
> version must use the 296-byte v150 layout below.
>
> **This format is gated by a single file.** Every count in this document is n=1. Treat it
> accordingly.

---

## Table of Contents

1.  [Overview](#1-overview)
2.  [File Layout](#2-file-layout)
3.  [SNO File Preamble](#3-sno-file-preamble)
4.  [Shader Name Field](#4-shader-name-field)
5.  [Program Blocks](#5-program-blocks)
6.  [Input Semantic Data](#6-input-semantic-data)
7.  [Compiled Shader Relationship](#7-compiled-shader-relationship)
8.  [Known Files](#8-known-files)
9.  [Cross-References](#9-cross-references)
10. [Known Unknowns](#10-known-unknowns)

---

## 1. Overview

Shader Definition files (`.shd`) describe the **source-level definition** of a shader program — its name, input semantics, FX source file, and vertex/pixel shader entry points. Each `.shd` file contains multiple **program blocks** (typically 2), one per rendering technique or platform variant.

In the D3 material pipeline, `.shd` files are metadata definitions; they are **not** the compiled GPU bytecode. The actual compiled shaders live in separate files (`.pscod`, `.vscod`, `.ps.glsl`, `.vs.glsl`).

### Pipeline Role

```
Material (.mat)
    └──→ ShaderMap (.shm)
              │ configId → shaderSno
              ▼
         Compiled Shaders (.pscod, .vscod, .ps.glsl, .vs.glsl)
              ▲
              │ compiled from
    Shader Definition (.shd)    ◀── THIS FILE
         │  describes source metadata:
         │  - FX file reference
         │  - VS/PS entry points
         │  - Input semantic declarations
         ▼
    FX Source (Legacy.fx)       — not shipped, referenced by name
```

> **Note**: Only 2 `.shd` files exist in the shipped game data. The compiled shader variants (thousands of `.pscod`/`.vscod`/`.glsl` files) are the primary shader representation at runtime. The `.shd` files appear to be remnants of the build pipeline's shader compilation metadata.

---

## 2. File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO file header                            (16 bytes)  │
│    0x000: magic=0xDEADBEEF, u16 version=150             │
├─────────────────────────────────────────────────────────┤
│  Shaders struct                            (296 bytes)  │
│    file 0x010 = struct +0x00                            │
│    ends at file 0x138                                   │
├─────────────────────────────────────────────────────────┤
│  arRenderPasses payload      (dwRenderPassCount × 568)  │
│    file 0x138 … 0x5A8                                   │
├─────────────────────────────────────────────────────────┤
│  Semantic / constant data                  (828 bytes)  │
│    file 0x5A8 … EOF                                     │
└─────────────────────────────────────────────────────────┘
```

Total file size for the analyzed file: **2,276 bytes** = 16 + 296 + (2 × 568) + 828.

---

## 3. The Shaders Struct (296 bytes)

Offsets are **struct-relative**; file position = 16 + offset.

```cpp
struct Shaders {                                // 296 bytes (0x128), group 37
    u32     dwSnoId;            // +0x00: shader SNO id (34901 in the sample)
    u32     _pad04, _pad08;     // +0x04, +0x08: 0
    u32     dwShaderFlags;      // +0x0C: 64 (0x40) in the sample
    u32     dwUnknown10;        // +0x10: 0 in the sample
    i32     dwRenderPassCount;  // +0x14: 2 in the sample
    u32     renderPassOffset;   // +0x18: SerializeData.offset — 296, i.e. the struct end
    u32     renderPassSize;     // +0x1C: SerializeData.byteSize — 1,136 = 2 × 568
    u64     _runtimePtr20;      // +0x20: 0 on disk
    char    szName[256];        // +0x28: inline NUL-terminated ASCII, zero-padded
};                                              // 0x28 + 256 = 296 exactly
```

This is the ordinary D3 variable-array idiom — a count field followed by a `SerializeData`
`{offset, byteSize}` pair and a runtime pointer — not a repurposing of reserved preamble
words. `renderPassSize / dwRenderPassCount` gives the **568-byte** render-pass element.

---

## 4. Shader Name Field

**Struct offset**: +0x28 (file 0x038) | **Size**: 256 bytes, inline

A NUL-terminated ASCII string zero-padded to exactly 256 bytes, running to the struct's end.
Every byte after the terminator is zero in the sample.

**Observed value**: `actor_add2x_mult2x_unlit_skin_glow`

This name matches the ShaderMap naming convention (see `SHM_FILE_FORMAT_SPECIFICATION.md` §7).

The binary's **revision 187** replaces this inline array with an 8-byte reference, which is why
the registered struct is 56 bytes against v150's 296 — the same inline-string-to-hash migration
seen in `Anim`'s `BoneName` (64 → 4) and `HardpointLink` (68 → 12).

---

## 5. Program Blocks

Each `.shd` file contains multiple program blocks (count given by `dataSize` in the preamble). Block 0 starts at offset 0x138 (immediately after the 256-byte name field).

```cpp
struct ShaderProgramBlock {                     // ~568 bytes (approximate)
    // ─── Input Semantic Declarations ───────────────────────────────────────────
    u8      inputSemantics[80];     // 0x000: Input attribute descriptors (variable)

    // ─── Semantic Data Reference ───────────────────────────────────────────────
    u32     semanticDataOffset;     // Absolute file offset to input layout data
    u32     semanticDataSize;       // Size of input layout data in bytes

    u8      _padding[16];           // 16 bytes padding

    // ─── FX Source Filename ────────────────────────────────────────────────────
    char    fxFilename[256];        // Null-padded — "Legacy.fx"

    // ─── Entry Point Names ─────────────────────────────────────────────────────
    char    vsEntryPoint[64];       // Null-padded — "vs_legacy"
    char    psEntryPoint[64];       // Null-padded — "ps_legacy"

    // ─── Constant Data Reference ───────────────────────────────────────────────
    u32     constantDataOffset;     // Absolute file offset to constant buffer layout
    u32     constantDataSize;       // Size of constant buffer layout in bytes

    u8      _trailing[8];           // Trailing padding to block boundary
};
```

### Observed Values

| Field | Block 0 | Block 1 |
|-------|---------|---------|
| `fxFilename` | `Legacy.fx` | `Legacy.fx` |
| `vsEntryPoint` | `vs_legacy` | `vs_legacy` |
| `psEntryPoint` | `ps_legacy` | `ps_legacy` |
| `semanticDataOffset` | 0x0598 | 0x07B0 |
| `semanticDataSize` | 0x0078 (120 bytes) | 0x0124 (292 bytes) |
| `constantDataOffset` | 0x0610 | 0x07B0 |
| `constantDataSize` | 0x0124 | 0x0124 |

Both blocks reference the same FX file and entry points but have different semantic/constant data regions, suggesting they represent different compilation profiles (e.g., SM3.0 vs SM5.0, or DX9 vs DX11).

---

## 6. Input Semantic Data

The data regions referenced by program blocks contain structured arrays of input attribute and constant buffer descriptors.

```cpp
struct InputLayoutEntry {                       // 12 bytes (tentative)
    u16     semanticId;         // 0x00: Shader semantic identifier
    u16     unknown1;           // 0x02: Often 0x000A
    u32     defaultValue;       // 0x04: Default value (integer or float bits)
    u32     padding;            // 0x08: Usually 0
};
```

### Semantic ID Table

| ID | Hex | Likely Semantic |
|----|-----|-----------------|
| 0x01 | 0x01 | `POSITION` |
| 0x02 | 0x02 | `BLENDWEIGHT` |
| 0x07 | 0x07 | `TEXCOORD0` |
| 0x0E | 0x0E | `NORMAL` |
| 0x0F | 0x0F | `TANGENT` |
| 0x10 | 0x10 | `BINORMAL` |
| 0x11 | 0x11 | `BLENDINDICES` |
| 0x12 | 0x12 | `COLOR0` |
| 0x13 | 0x13 | `COLOR1` |
| 0x16 | 0x16 | `TEXCOORD1` |
| 0x17 | 0x17 | `TEXCOORD2` |
| 0x18 | 0x18 | `TEXCOORD3` |
| 0x19 | 0x19 | `TEXCOORD4` |
| 0x1A | 0x1A | `TEXCOORD5` |
| 0x1C | 0x1C | `TEXCOORD6` |
| 0x1D | 0x1D | `TEXCOORD7` |
| 0x1E | 0x1E | `TEXCOORD8` |
| 0x1F | 0x1F | `TEXCOORD9` |
| 0x20 | 0x20 | `PSIZE` |
| 0x29 | 0x29 | `SV_POSITION` |
| 0x33 | 0x33 | `SV_TARGET0` |
| 0x34 | 0x34 | `SV_TARGET1` |
| 0x35 | 0x35 | `SV_TARGET2` |
| 0x36 | 0x36 | `SV_TARGET3` |
| 0x3C | 0x3C | `FOG` |

---

## 7. Compiled Shader Relationship

### Compiled Shader File Types

| Extension | API | Directory | Description |
|-----------|-----|-----------|-------------|
| `.pscod` | DirectX | `CompiledShader/` | Compiled pixel shader bytecode |
| `.vscod` | DirectX | `CompiledShader/` | Compiled vertex shader bytecode |
| `.ps.glsl` | OpenGL | `OpenGLShaders/` | ARB assembly pixel shader (Cg 3.1.0010) |
| `.vs.glsl` | OpenGL | `OpenGLShaders/` | ARB assembly vertex shader (Cg 3.1.0010) |

### OpenGL Shader Header Example

```
!!ARBfp1.0
# cgc version 3.1.0010, build date Feb 22 2012
# profile arbfp1
# 42 instructions, 8 R-regs
```

These shaders declare texture sampler usage that maps to the material's texture slot assignments:

```glsl
PARAM c[1] = { program.local[0] };              // Shader constant 0
TEX R0, fragment.texcoord[0], texture[0], 2D;    // Slot 1 → sampler 0
TEX R1, fragment.texcoord[1], texture[1], 2D;    // Slot 3 → sampler 1
```

---

## 8. Known Files

Only 2 `.shd` files exist in the shipped Diablo III game data:

| Filename | Size | SNO ID |
|----------|------|--------|
| `actor_add2x_mult2x_unlit_skin_glow.shd` | 2,276 bytes | 0x00008855 |
| *(second file)* | similar | — |

---

## 9. Cross-References

```
Material (.mat)
    └──→ ShaderMap (.shm)
              └──→ Compiled Shader (.pscod/.vscod)
                        ▲
              Shader Definition (.shd)  ◀── THIS FILE
```

| Related Format | Extension | Relationship |
|----------------|-----------|--------------|
| Material | `.mat` | References shaders via ShaderMap |
| ShaderMap | `.shm` | Maps configId → compiled shader SNO |
| Compiled Shader | `.pscod`/`.vscod` | GPU bytecode compiled from `.shd` definition |

---

## 10. Known Unknowns

| Item | Notes |
|------|-------|
| `dwShaderFlags` (+0x0C) | 64 (0x40) in the sample; bit meanings unknown, n=1 |
| `dwUnknown10` (+0x10) | 0 in the sample; registered `DT_INT`, no default |
| Render-pass element | The **568-byte** size is now exact (`renderPassSize / dwRenderPassCount`), but the internal field layout is tentative — the binary registers `RenderPass` as a type, and it is one of the 33 that `gen_d3_native.py` prunes for having no native entry point |
| Input semantic structure | 12-byte entry format is tentative |
| Platform profiles | Two passes may represent DX9/DX11 or SM3.0/SM5.0 — unconfirmed |
| Constant buffer layout | The 828-byte trailing region is not fully decoded |
| Relationship to runtime | These files appear to be build-pipeline remnants, not loaded at runtime |

> **Limitation**: the corpus is **one file**. Every count in this document is n=1, so nothing
> here is gated in the sense [README.md](README.md) §3 uses — the structural claims that *are*
> firm (296-byte struct, 568-byte element, inline 256-byte name) are firm because they are
> forced by the registration and by internal consistency, not because a corpus confirmed them.
> For practical use the `.mat` and `.shm` formats matter far more: they carry the data needed
> to reconstruct rendering parameters for 3D models, and both have real corpora.

---

*Layout from the Switch 2.6.2 type registration, cross-checked against the single available
`.shd`. See [README.md](README.md) for the derivation basis.*
