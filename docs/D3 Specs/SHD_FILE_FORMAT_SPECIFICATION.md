# SHD File Format Specification

**Format**: Diablo III Shader Definition (`.shd`)  
**Byte Order**: Little-endian  
**Magic**: `0xDEADBEEF`  
**Version**: 150  
**Corpus**: 2 files analyzed (limited — partial validation only)

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
│  SNO File Preamble                          (48 bytes)  │
│    0x000: magic=0xDEADBEEF, version=150                 │
├─────────────────────────────────────────────────────────┤
│  8 bytes padding                                        │
├─────────────────────────────────────────────────────────┤
│  Shader Name                               (256 bytes)  │
│    0x038: null-padded ASCII string                      │
├─────────────────────────────────────────────────────────┤
│  Program Block 0                           (~568 bytes)  │
│    Input semantics, FX name, VS/PS entries              │
├─────────────────────────────────────────────────────────┤
│  Program Block 1                           (~552 bytes)  │
│    Input semantics, FX name, VS/PS entries              │
├─────────────────────────────────────────────────────────┤
│  Semantic / Constant Data 0                (variable)   │
├─────────────────────────────────────────────────────────┤
│  Semantic / Constant Data 1                (variable)   │
└─────────────────────────────────────────────────────────┘
```

Total file size for the analyzed file: **2,276 bytes**.

---

## 3. SNO File Preamble

**Size**: 48 bytes

The `.shd` preamble uses the 48-byte variant shared with `.mat` and `.shm`, but repurposes several fields.

```cpp
struct SnoFilePreamble {                        // 48 bytes @ 0x000
    u32     magic;              // 0x000: 0xDEADBEEF
    u32     version;            // 0x004: 150 for .shd
    u8      _reserved08[8];     // 0x008: Zeros
    u32     snoId;              // 0x010: Unique shader SNO hash
    u8      _reserved14[8];     // 0x014: Zeros
    u32     _unknown1C;         // 0x01C: 64 (0x40) — possibly string region offset
    u32     dataOffset;         // 0x020: 0 — handled differently from .mat/.shm
    u32     dataSize;           // 0x024: 2 — program block COUNT (not byte size)
    u32     programBlockOffset; // 0x028: Offset to program block region (e.g. 0x138)
    u32     programBlockSize;   // 0x02C: Total program block region size (e.g. 0x0470)
};
```

> **Key difference from `.mat`/`.shm`**: In `.shd` files, `dataSize` represents a **count** (number of program blocks), not a byte size. Fields at 0x028 and 0x02C (normally reserved) are repurposed as an offset/size pair for the program block data region.

---

## 4. Shader Name Field

**Offset**: 0x038 | **Size**: 256 bytes

A null-terminated ASCII string, zero-padded to exactly 256 bytes. Contains the shader program's canonical name.

**Observed value**: `actor_add2x_mult2x_unlit_skin_glow`

This name matches the ShaderMap naming convention (see `SHM_FILE_FORMAT_SPECIFICATION.md` §7).

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
| Program block boundaries | Exact field sizes are approximate — only 2 files available |
| Input semantic structure | 12-byte entry format is tentative |
| Platform profiles | Two blocks may represent DX9/DX11 or SM3.0/SM5.0 — unconfirmed |
| Constant buffer layout | The constant data regions are not fully decoded |
| Relationship to runtime | These files appear to be build pipeline remnants, not loaded at runtime |

> **Limitation**: With only 2 files, many structural details remain tentative. For practical use, the `.mat` and `.shm` formats are far more important — they contain the data needed to reconstruct rendering parameters for 3D models.

---

*Specification derived from binary analysis of 2 `.shd` files. Limited corpus prevents full validation.*
