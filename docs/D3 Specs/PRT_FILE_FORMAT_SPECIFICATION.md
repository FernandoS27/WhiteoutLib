# PRT File Format Specification

**Format**: Diablo III Particle Emitter (`.prt`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**Version**: 180
**Corpus**: 21,593 files analyzed

---

## Table of Contents

1. [Overview](#1-overview)
2. [File Layout](#2-file-layout)
3. [SNO Preamble](#3-sno-preamble)
4. [Particle Header](#4-particle-header)
5. [AnimRef Structure](#5-animref-structure)
6. [AnimRef Regions & Gap Regions](#6-animref-regions--gap-regions)
7. [Block 12 — Material / Color Reference](#7-block-12--material--color-reference)
8. [Block 13 — Color Gradient](#8-block-13--color-gradient)
9. [Keyframe Data](#9-keyframe-data)
10. [Gap Region Details](#10-gap-region-details)
11. [Look Variant Records](#11-look-variant-records)
12. [Block-by-Block Field Catalog](#12-block-by-block-field-catalog)
13. [M3 PAR_ Cross-Reference](#13-m3-par_-cross-reference)
14. [Enumerations](#14-enumerations)
15. [Corpus Statistics](#15-corpus-statistics)
16. [Known Unknowns](#16-known-unknowns)
17. [Appendix A — Reading a PRT File (C++)](#appendix-a--reading-a-prt-file-c)
18. [Appendix B — All Structures Summary](#appendix-b--all-structures-summary)

---

## 1. Overview

PRT files define **particle emitters** for Diablo III's visual effects pipeline. Each file
encodes a single emitter with ~41 animated parameter channels (AnimRef blocks), static
configuration fields, material references, and color gradient data. The format is structurally
analogous to the **PAR_ chunk** in the M3 format used by StarCraft II and Heroes of the Storm,
sharing the same parameter set (emission rate, speed, lifespan, size curves, color over life,
physics, noise modulation) expressed through a different serialization pattern.

Particle emitters are referenced by Appearance files (`.app`) and embedded into actor effect
graphs. The pipeline is:

```
Appearance (.app)  →  Particle (.prt)  →  Material (.mat)
   model/actor          emitter def          texture/shader
```

Key characteristics:
- **Version 180** across all 21,593 files — no version variation
- **Narrow size range** (3,140–8,464 bytes) — size variation from keyframe data and look variants
- **41 AnimRef blocks** (40 in smallest, 43 in largest) organized into **4 regions**
- **+16 data access convention** — all stored offsets require adding 16 to reach actual data

---

## 2. File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO Preamble                               (32 bytes)  │
│    0x000: magic, version, snoId                         │
├─────────────────────────────────────────────────────────┤
│  Particle Header                            (72 bytes)  │
│    0x020: flags, duration, emitter type, scale          │
├─────────────────────────────────────────────────────────┤
│  AnimRef Region 0         (13 × 48 = 624 bytes)        │
│    0x068: Blocks 0–12 — Emission & lifetime params      │
├─────────────────────────────────────────────────────────┤
│  Gap 0 — Scale Constants                   (44 bytes)   │
│    0x2D8: Per-axis scale enables                        │
├─────────────────────────────────────────────────────────┤
│  AnimRef Region 1          (1 × 48 = 48 bytes)         │
│    0x304: Block 13 — Color gradient                     │
├─────────────────────────────────────────────────────────┤
│  Gap 1 — Timing & Material                (68 bytes)    │
│    0x334: Emission timing, texture SNO ref              │
├─────────────────────────────────────────────────────────┤
│  AnimRef Region 2          (3 × 48 = 144 bytes)        │
│    0x378: Blocks 14–16 — Emission area / physics        │
├─────────────────────────────────────────────────────────┤
│  Gap 2 — Rendering Config                (136 bytes)    │
│    0x408: Noise, flipbook, rendering flags              │
├─────────────────────────────────────────────────────────┤
│  AnimRef Region 3         (24 × 48 = 1152 bytes)       │
│    0x490: Blocks 17–40 — Per-particle properties        │
├─────────────────────────────────────────────────────────┤
│  Keyframe Data Pool                        (variable)   │
│    0x910: Referenced by AnimRef offset fields            │
├─────────────────────────────────────────────────────────┤
│  Look Variant Records                      (optional)   │
│    variable: Alternate emitter configurations           │
└─────────────────────────────────────────────────────────┘
```

Total fixed structure: **0x910 = 2,320 bytes**. Remaining bytes (820–6,144) hold keyframe data
and optional look variant records.

---

## 3. SNO Preamble

**Tag**: PRT | **Version**: 180 | **Size**: 32 bytes

```cpp
struct SnoPreamble {                            // 32 bytes
    u32     magic;              // 0x000: Always 0xDEADBEEF
    u32     version;            // 0x004: Always 180 for .prt
    u32     snoId;              // 0x008: Unique asset identifier
    u32     _unknown00C;        // 0x00C: Varies per file
    u32     _unknown010;        // 0x010: Often 0
    u32     _unknown014;        // 0x014: Varies
    u32     _unknown018;        // 0x018: Often 0
    u32     _unknown01C;        // 0x01C: Often 0
};
```

---

## 4. Particle Header

**Tag**: ParticleHeader | **Version**: — | **Size**: 72 bytes

Global emitter configuration at offset 0x020–0x067.

```cpp
struct ParticleHeader {                         // 72 bytes
    // ─── Behavior Flags ────────────────────────────────────────────────────────
    u32     flags;              // 0x020: Behavior bitfield (see §14.1)
                                //        Common: 5160, 1064, 5416

    // ─── Timing Parameters ─────────────────────────────────────────────────────
    u32     duration;           // 0x024: Emitter lifetime in ticks
                                //        Common: 120(113), 60(110), 180(47)
    u32     startDelay;         // 0x028: Delay before first emission
                                //        Common: 0(208), 30(172), 15(44)
    u32     loopDelay;          // 0x02C: Delay between loop iterations (0 = no delay)
    u32     _reserved030;       // 0x030: Always 0
    u32     _reserved034;       // 0x034: Almost always 0
    f32     loopScale;          // 0x038: Loop playback rate (1.0 = normal, 0.0 = non-looping)
    u32     _reserved03C;       // 0x03C: Always 0

    // ─── Emitter Configuration ─────────────────────────────────────────────────
    u32     emitterType;        // 0x040: Billboard/tail/cylinder/etc. (see §14.2)
                                //        1=billboard(91.8%), 2=tail, 3=cylinder
    f32     emitterAngle;       // 0x044: Orientation angle in radians (0 = default)
    f32     globalScale;        // 0x048: Global size multiplier (typically 1.0)
    u32     renderLayer;        // 0x04C: Render sorting layer (0 or 5)
    u32     blendMode;          // 0x050: Blend mode (0=alpha, 1=additive, 8=unknown)
    f32     midpointBias;       // 0x054: Mid-keyframe time bias (0.0–1.0)
    f32     speedMultiplier;    // 0x058: Global speed scale (typically 1.0–2.0)
    u32     _reserved05C;       // 0x05C: Always 0
    u32     _reserved060;       // 0x060: Always 0
    u32     _reserved064;       // 0x064: Always 0
};
```

The `duration` values suggest a tick rate of approximately 60 ticks/second based on common
effect timings (60 = 1 sec, 120 = 2 sec, 180 = 3 sec).

---

## 5. AnimRef Structure

**Size**: 48 bytes

Each animated parameter is encoded as a 48-byte AnimRef block — a reference to external
keyframe data plus default values and randomization ranges. This is the D3 equivalent of the
M3 `AnimationReference<T>` but with a uniform 48-byte layout.

```cpp
struct AnimRef {                                // 48 bytes
    u32     dataOffset;         // 0x00: Offset to keyframe data (+16 convention)
    u32     dataSize;           // 0x04: Size of keyframe data in bytes
    u32     interpType;         // 0x08: Interpolation type (see §14.3)
    u32     _padding0C;         // 0x0C: Usually 0
    f32     defaultValue;       // 0x10: Default parameter value (typically 1.0)
    u32     _padding14;         // 0x14: Usually 0
    u32     _padding18;         // 0x18: Usually 0
    f32     rangeMin;           // 0x1C: Randomization range minimum
    f32     rangeMax;           // 0x20: Randomization range maximum
    u32     _padding24;         // 0x24: Usually 0
    u32     _padding28;         // 0x28: Usually 0
    u32     _padding2C;         // 0x2C: Usually 0
};
```

**Data Access**: Keyframe data at `dataOffset + 16`. When `dataOffset` is 0 and `dataSize` is
12, the block contains a single default keyframe.

**Randomization**: At runtime, each spawned particle randomizes between `rangeMin` and
`rangeMax`. When both are 0.0 and 1.0 respectively, the default value passes through unscaled.

**Critical Note — Default as Scale Multiplier**: The `defaultValue` field is almost universally
**1.0** across all blocks. It functions as a **scale multiplier** rather than the actual
parameter value. The actual parameter is stored in the first keyframe (kf0). Runtime formula:
`effectiveValue = kf0_value × defaultValue × randomize(rangeMin, rangeMax)`.

---

## 6. AnimRef Regions & Gap Regions

The 41 AnimRef blocks are organized into **4 contiguous regions** separated by **3 gap regions**
of fixed-value configuration data.

```
Region 0:  0x068 – 0x2A7   13 blocks (0–12)    Emission + material
Gap 0:     0x2D8 – 0x303   44 bytes             Scale constants
Region 1:  0x304 – 0x333   1 block  (13)        Color gradient
Gap 1:     0x334 – 0x377   68 bytes             Timing & material reference
Region 2:  0x378 – 0x407   3 blocks (14–16)     Emission area / physics
Gap 2:     0x408 – 0x48F   136 bytes            Rendering flags / flipbook
Region 3:  0x490 – 0x90F   24 blocks (17–40)    Per-particle animated properties
```

**Note**: Block 12 (at 0x2A8) uses a **non-standard layout** — see §7. Block 13 (at 0x304)
stores color gradient data with a differently-interpreted header — see §8.

---

## 7. Block 12 — Material / Color Reference

**Tag**: Block12 | **Offset**: 0x2A8 | **Size**: 48 bytes

Block 12 occupies the standard 48 bytes but does **not** follow the AnimRef field layout.
Instead, it encodes the emitter's material association and a fallback color multiplier.

```cpp
struct Block12MaterialRef {                     // 48 bytes at 0x2A8
    u32     dataOffset;         // 0x2A8: Points to 12 bytes of kf data (+16)
    u32     dataSize;           // 0x2AC: Always 12
    f32     colorTimeScale;     // 0x2B0: Color animation time scale
                                //        Common: 0.01(447), 0.2(16), 0.025(10)
    u32     _zero;              // 0x2B4: Always 0
    u32     materialSnoId;      // 0x2B8: SNO hash or 0xFFFFFFFF (no material)
    f32     colorMult_R;        // 0x2BC: 1.0 when no material, 0.0 when has material
    f32     colorMult_G;        // 0x2C0: Color multiplier green
    f32     colorMult_B;        // 0x2C4: Color multiplier blue
    f32     colorMult_A;        // 0x2C8: Color multiplier alpha
    f32     colorMult2_R;       // 0x2CC: Secondary color multiplier red
    f32     colorMult2_G;       // 0x2D0: Secondary color multiplier green
    f32     colorMult2_B;       // 0x2D4: Secondary color multiplier blue
};
```

**Material binding pattern**: When `materialSnoId` = 0xFFFFFFFF, the emitter has no associated
material and uses the color multiplier fields (all 1.0 = full white). When a material SNO hash
is present, these multiplier fields are zeroed because the `.mat` file provides all color/texture
information.

---

## 8. Block 13 — Color Gradient

**Tag**: Block13 | **Offset**: 0x304 | **Size**: 48 bytes header + variable gradient data

Block 13 stores a **color-over-life gradient table**. The header occupies 48 bytes but uses a
non-standard interpretation.

```cpp
struct Block13ColorGradient {                   // 48 bytes header
    u32     dataOffset;         // 0x304: +16 convention to gradient data
    u32     dataSize;           // 0x308: N × 160-byte gradient stops
                                //        12(77), 160(122), 320(248), 480(18), 640(31)
    u32     interpType;         // 0x30C: 0 = has gradient, 1 = constant color
    u32     _padding[4];        // 0x310: Padding
    u32     sentinel;           // 0x320: 0xFFFFFFFF = gradient active, 0 = constant
    f32     timeScale;          // 0x324: Color animation time scale
                                //        0.031(422), 1.0(77) — ~1/32 sec
    u32     _remaining[3];      // 0x328: Remaining header fields
};
```

### Gradient Stop Format (160 bytes each)

```cpp
struct GradientStop {                           // 160 bytes
    u32     index;              // 0x00: Stop index
    u32     _pad04;             // 0x04: Padding
    u32     hash;               // 0x08: Stop identifier hash
    u32     count;              // 0x0C: Entry count
    f32     colorMatrix[4][4];  // 0x10: 4×4 RGBA color transform matrix (64 bytes)
                                //       Row0 = Red:   (R, 0, 0, 0)
                                //       Row1 = Green: (0, G, 0, 0)
                                //       Row2 = Blue:  (0, 0, B, 0)
                                //       Row3 = Alpha: (0, 0, 0, A)
                                //       Identity = full-brightness, unmodified color
    f32     tangentData[8];     // 0x50: Tangent data for smooth interpolation (32 bytes)
    u8      _padding[48];       // 0x70: Additional tangent data / zeros
};
```

**Size distribution** (n=500): 12 bytes (77) = constant color, 160 bytes (122) = 1 stop,
320 bytes (248) = 2 stops, 480 bytes (18) = 3 stops, 640 bytes (31) = 4 stops.

---

## 9. Keyframe Data

Keyframe data is stored in the **Keyframe Data Pool** starting at approximately offset 0x910.
Each AnimRef block's `dataOffset + 16` points into this pool.

### 9.1 Scalar Keyframes (12-byte stride)

For scalar parameters (emission rate, speed, size, opacity, etc.):

```cpp
struct ScalarKeyframe {                         // 12 bytes
    f32     value;              // 0x00: Parameter value
    f32     valueRange;         // 0x04: Randomization upper bound (often = value)
    f32     normalizedTime;     // 0x08: Position in particle lifetime (0.0–1.0)
};
```

**Example — Fade-out opacity** (block 2, 4 keyframes):

| Value | Range | Time  | Meaning                    |
|------:|------:|------:|:---------------------------|
| 1.000 | 1.000 | 0.000 | Full opacity at birth      |
| 1.000 | 1.000 | 0.746 | Hold until 75% of life     |
| 0.233 | 0.233 | 0.830 | Rapid fade                 |
| 0.000 | 0.000 | 0.951 | Near-transparent at death  |

### 9.2 Vector Keyframes (28-byte stride)

For 3D vector parameters (emission area, velocity spread, cutout size):

```cpp
struct VectorKeyframe {                         // 28 bytes
    f32     minX, minY, minZ;   // 0x00: Minimum XYZ / start values (12 bytes)
    f32     maxX, maxY, maxZ;   // 0x0C: Maximum XYZ / end values (12 bytes)
    u32     _padding;           // 0x18: Always 0
};
```

**Example — Emission area** (block 6, 28 bytes):

```
min: (0.100, -0.100,  0.100)    — XYZ min bounds
max: (0.200,  0.100,  0.200)    — XYZ max bounds
```

Blocks consistently using vec3 format: **6, 7, 11, 16, 26, 30, 33–38**.

---

## 10. Gap Region Details

### 10.1 Gap 0 — Scale Constants (44 bytes at 0x2D8–0x303)

```cpp
struct Gap0ScaleConstants {                     // 44 bytes
    f32     scaleX;             // 0x2D8: X-axis scale enable (0.0 or 1.0)
    u32     _pad0[3];           // 0x2DC: Zeros
    f32     scaleY;             // 0x2E8: Y-axis scale enable (0.0 or 1.0)
    u32     _pad1[3];           // 0x2EC: Zeros
    f32     scaleZ;             // 0x2F8: Z-axis scale enable (0.0 or 1.0)
    u32     _pad2;              // 0x2FC: Zero
    u32     _trailing;          // 0x300: Trailing pad
};
```

All three scale factors always share the same value (either all 0.0 or all 1.0). 52.7% of files
have scaling enabled (full-corpus validation).

### 10.2 Gap 1 — Timing & Material Reference (68 bytes at 0x334–0x377)

```cpp
struct Gap1TimingParams {                       // 68 bytes
    f32     emissionMidpoint;   // 0x334: Mid-time for emission curve (always 0.3)
    f32     constant338;        // 0x338: Always 1.0
    f32     emissionEndMult;    // 0x33C: End-phase emission multiplier (always 1.25)
    u32     _zero340;           // 0x340: Always 0
    f32     constant344;        // 0x344: Always 1.0
    u32     textureSnoRef;      // 0x348: Optional texture SNO hash (24% non-zero)
    u32     _zero34C;           // 0x34C: Always 0
    u32     instanceFlag;       // 0x350: Boolean — instance particles
    u32     _reserved354;       // 0x354: Almost always 0
    f32     constant358;        // 0x358: Always 1.0
    u32     _reserved35C;       // 0x35C: Almost always 0
    f32     _reserved360;       // 0x360: Usually 0
    f32     colorMidTime;       // 0x364: Mid-time for color interpolation (0.0–1.0)
    f32     tailLength;         // 0x368: Tail particle length multiplier (typ. 1.0)
    u32     _zero[3];           // 0x36C: Always 0
};
```

### 10.3 Gap 2 — Rendering Configuration (136 bytes at 0x408–0x48F)

```cpp
struct Gap2RenderConfig {                       // 136 bytes
    u8      modelPath[96];      // 0x408: Embedded model/texture path (rare, ~1%)
    f32     noiseAmplitude;     // 0x468: Noise modulation amplitude
    f32     noiseFrequency;     // 0x46C: Noise modulation frequency
    f32     noiseCohesion;      // 0x470: Noise coherence factor (typically 1.0)
    u32     flipbookType;       // 0x474: Flipbook animation mode (0, 3, or 5)
    u32     _reserved478;       // 0x478: Zero
    f32     _reserved47C;       // 0x47C: Usually 0
    f32     constant480;        // 0x480: Always 1.0
    u8      _remaining[24];     // 0x484: Zero padding
};
```

---

## 11. Look Variant Records

Files larger than ~3,500 bytes may contain **look variant records** after the keyframe data pool.
These define alternate appearances ("looks") for the particle system.

**Statistics**: 4.9% of files (244/5,000) contain look variant data.

```cpp
struct LookVariantName {                        // 68 bytes
    char    name[64];           // 0x00: Variant name (null-padded ASCII)
    u32     flags;              // 0x40: Flags / record type marker
};
```

Look variants appear in **pairs** with a **344-byte gap** between pairs (suggesting ~7 AnimRef
overrides × 48 bytes = 336 bytes per variant). All observed variant names are `"Default"`,
indicating the look system is initialized but variants are not customized in shipped assets.

---

## 12. Block-by-Block Field Catalog

### Region 0 — Emission & Lifetime Parameters (Blocks 0–11)

| Block | Offset | Format | Default | Anim% | Probable Function |
|------:|--------|--------|--------:|------:|:------------------|
| 0 | 0x068 | scalar | 1.0 | 8.2% | **Emission rate** ★★★ |
| 1 | 0x098 | scalar | 1.0 | 67.5% | **Emission speed (over-life curve)** |
| 2 | 0x0C8 | scalar | 1.0 | 42.9% | **Opacity / alpha over life** |
| 3 | 0x0F8 | scalar | 1.0 | 1.0% | **Emission angle X** |
| 4 | 0x128 | scalar | 1.0 | 1.9% | **Emission spread X** |
| 5 | 0x158 | scalar | 1.0 | 1.0% | **Emission spread Y** |
| 6 | 0x188 | vec3 | 1.0 | 1.1% | **Emission area size** |
| 7 | 0x1B8 | vec3 | 1.0 | 2.6% | **Emission area cutout** |
| 8 | 0x1E8 | scalar | 1.0 | 38.6% | **Particle size scale** |
| 9 | 0x218 | scalar | 1.0 | 3.8% | **Lifespan multiplier** (NOT absolute) |
| 10 | 0x248 | scalar | 1.0 | 0.1% | **Lifespan range** |
| 11 | 0x278 | vec3 | 1.0 | 0.6% | **Velocity / gravity offset** |

### Region 2 — Additional Parameters (Blocks 14–16)

| Block | Offset | Format | Default | Anim% | Probable Function |
|------:|--------|--------|--------:|------:|:------------------|
| 14 | 0x378 | scalar | 1.0 | 1.4% | **Emission radius** |
| 15 | 0x3A8 | scalar | 1.0 | 1.5% | **Emission cutout radius** |
| 16 | 0x3D8 | vec3 | N/A | 0.1% | **Non-float packed data** (rotation?) |

### Region 3 — Per-Particle Properties (Blocks 17–40)

| Block | Offset | Format | Default | Anim% | Probable Function |
|------:|--------|--------|--------:|------:|:------------------|
| 17 | 0x490 | scalar | 1.0 | 13.4% | Noise yaw amplitude |
| 18 | 0x4C0 | scalar | 1.0 | **81.6%** | **Primary over-life curve A** |
| 19 | 0x4F0 | scalar | 1.0 | 1.3% | Noise pitch amplitude |
| 20 | 0x520 | scalar | 1.0 | **69.9%** | **Primary over-life curve B** |
| 21 | 0x550 | scalar | 1.0 | 33.7% | Noise speed amplitude |
| 22 | 0x580 | scalar | 1.0 | 2.5% | **Z-acceleration / gravity** ★★★ |
| 23 | 0x5B0 | scalar | 1.0 | 15.5% | **Z-acceleration secondary** ★★★ |
| 24 | 0x5E0 | scalar | 1.0 | 1.4% | Noise size frequency |
| 25 | 0x610 | scalar | 1.0 | 0.1% | Noise alpha amplitude |
| 26 | 0x640 | vec3 | 1.0 | 0.6% | Noise alpha frequency |
| 27 | 0x670 | scalar | 1.0 | 2.4% | Noise rotation amplitude |
| 28 | 0x6A0 | scalar | 1.0 | 11.8% | Noise rotation frequency |
| 29 | 0x6D0 | scalar | 1.0 | 4.6% | Noise horizontal amplitude |
| 30 | 0x700 | vec3 | 1.0 | 0.1% | Noise horizontal frequency |
| 31 | 0x730 | scalar | 1.0 | 1.0% | Noise vertical amplitude |
| 32 | 0x760 | scalar | 1.0 | 5.3% | Noise vertical frequency |
| 33 | 0x790 | vec3 | 1.0 | 1.7% | Particle velocity |
| 34 | 0x7C0 | vec3 | 1.0 | 4.8% | Alpha threshold |
| 35 | 0x7F0 | vec3 | 1.0 | 2.2% | UV offset |
| 36 | 0x820 | vec3 | 1.0 | 0.8% | **Emission spread** ★★★ |
| 37 | 0x850 | vec3 | 1.0 | 5.8% | UV tiling |
| 38 | 0x880 | vec3 | 1.0 | 0.7% | Lower bound |
| 39 | 0x8B0 | scalar | 1.0 | 2.1% | Upper bound |
| 40 | 0x8E0 | scalar | **0.8** | 1.8% | **Trailing particle rate** |

★★★ = Confirmed via cross-corpus statistical validation against M3.

**Animation Rate Tiers** (full corpus, n=21,593):
- **Tier 1 (>60%)**: Block 13 (85.6%), Block 18 (81.6%), Block 20 (69.9%), Block 1 (67.5%)
- **Tier 2 (30–60%)**: Block 2 (42.9%), Block 8 (38.6%), Block 21 (33.7%)
- **Tier 3 (10–30%)**: Block 23 (15.5%), Block 17 (13.4%), Block 28 (11.8%)
- **Tier 4 (<10%)**: All other blocks — rarely or never animated

---

## 13. M3 PAR_ Cross-Reference

The D3 .prt format is a structural analog of the M3/HotS **PAR_ v24** particle emitter chunk
(1,496 bytes).

| Aspect | M3 PAR_ | D3 .prt |
|--------|---------|---------|
| Container | Chunk within .m3 file | Standalone .prt SNO file |
| Struct size | 1,496 bytes fixed | 2,320 bytes fixed + variable keyframes |
| AnimRef size | 20 bytes (float), 36 bytes (vec3) | 48 bytes (all types) |
| Default value | `initValue` = absolute parameter value | `defaultValue` ≈ 1.0 (scale multiplier) |
| Keyframe storage | Inline SEQS/STC system | External pool at end of file |
| Color model | 3 × AnimRef<color> (start/mid/end) | Color transform matrix gradient |
| Material ref | materialReferenceIndex (u32) | materialSnoId (SNO hash) |

### Confirmed Field Mappings (Statistical Evidence)

| D3 Block | M3 Field | Overlap | Evidence |
|----------|----------|---------|----------|
| 0 | emissionRate (0x194) | **1.000** | PRT kf0 mean=4.19; M3 initValue mean=6.87 |
| 22 | zAcceleration (0x238) | **1.000** | Negative kf0 = downward gravity |
| 23 | zAcceleration₂ | **1.000** | Secondary acceleration channel |
| 36 | emissionSpreadX (0x05C) | **1.000** | Angle spread in radians |
| 40 | trailingRate (0x5B7) | — | Both default ≈ 0.8, <2% usage |

### Emitter Type Mapping

| Rank | M3 Type | M3 % | D3 Type | D3 % | Function |
|------|---------|------|---------|------|----------|
| 1 | 0 | 63.4% | 1 | 91.8% | **Billboard** |
| 2 | 1 | 23.2% | 2 | 3.0% | **Tail / speed-stretch** |
| 3 | 7 | 5.1% | 3 | 3.0% | **Cylinder** |
| 4 | 9 | 4.2% | 4 | 1.0% | **Ring / disc** |
| 5 | 5 | 2.5% | 5 | 0.7% | **Directional** |

### Key Architectural Differences

1. **Default values**: M3 `initValue` = actual value. D3 `defaultValue` ≈ 1.0 = scale multiplier.
2. **Color model**: M3 uses three AnimRef<color> fields. D3 uses variable-length color gradient
   with 4×4 transform matrices.
3. **Lifespan**: M3 stores absolute seconds. D3 uses header `duration` (ticks) with block 9 as
   optional multiplier.
4. **Block 16**: M3 rotation field is standard floats. D3 encodes as non-float packed data.

---

## 14. Enumerations

### 14.1 Flags Bitfield (0x020)

| Bit | Set % | Probable Meaning |
|-----|------:|:-----------------|
| 0 | 43.0% | Screen-space alignment |
| 1 | 29.8% | Local coordinate space |
| 2 | 19.2% | Inherit parent velocity |
| 3 | **99.7%** | Particle system enabled |
| 4 | 36.4% | Texture animation enabled |
| 5 | **97.5%** | Emitter active on spawn |
| 6 | 22.6% | Gravity enabled |
| 7 | 14.2% | Collision enabled |
| 8 | 42.1% | Sort particles by depth |
| 9 | 17.8% | Flipbook random start frame |
| 10 | **85.8%** | Use color gradient |
| 11 | 11.4% | Noise modulation active |
| 12 | 42.5% | Billboard facing mode |
| 25 | 5.6% | Trail rendering |

### 14.2 Emitter Type (0x040)

| Value | Count | % | Type | M3 Equivalent |
|------:|------:|--:|:-----|:--------------|
| 1 | 19,827 | 91.8% | **Billboard** | M3 type 0 |
| 2 | 642 | 3.0% | **Tail** | M3 type 1 |
| 3 | 637 | 3.0% | **Cylinder** | M3 type 7 |
| 4 | 206 | 1.0% | **Ring / disc** | M3 type 9 |
| 5 | 142 | 0.7% | **Directional** | M3 type 5 |
| 11 | 32 | 0.1% | **Model particle** | M3 type 10 |

### 14.3 Interpolation Type

| Value | Meaning |
|------:|:--------|
| 1 | **Linear** interpolation |
| 2 | **Step** / hold |
| 3 | **Smooth** (cubic/Hermite) |
| 4 | **Smooth in, linear out** |
| 5 | **Linear in, smooth out** |
| 6 | **Bezier** |
| 7 | **Bezier smooth** |
| 9 | **Auto-tangent** / TCB |

---

## 15. Corpus Statistics

| Metric | Value |
|--------|-------|
| Total .prt files | 21,593 |
| Version (all files) | 180 |
| Size range | 3,140 – 8,464 bytes |
| Median size | 3,396 bytes |
| Mean duration | 149.1 ticks (~2.5 sec @60 tps) |
| Files with look variants | ~4.9% |
| AnimRef blocks (typical) | 41 (range: 40–43) |

### Blend Mode Distribution

| Mode | Count | % |
|-----:|------:|--:|
| 0 | 18,406 | 85.2% |
| 1 | 1,821 | 8.4% |
| 8 | 1,315 | 6.1% |

### Gap Region Constants (full corpus)

| Field | Value | Files |
|-------|-------|------:|
| gap1.emissionMidpoint (0x334) | 0.3 | 99.8% |
| gap1.emissionEndMult (0x33C) | 1.25 | 99.8% |
| gap0 scale X=Y=Z | uniform | 100% |

---

## 16. Known Unknowns

### Resolved (via Cross-Corpus Validation)

| Item | Resolution |
|------|:-----------|
| Block 0 = emission rate | **Confirmed** (overlap 1.000 vs M3 emissionRate) |
| Block 9 ≠ absolute lifespan | It is a **multiplier** to header `duration`, not absolute time |
| Emitter type mapping | Rank-aligned: D3 type 1 = M3 type 0, NOT a simple +1 offset |
| Blocks 22/23 = gravity | **Confirmed** negative kf0 = downward force |
| Block 36 = emission spread | **Confirmed** range overlap with M3 emissionSpreadX |

### Remaining Unknowns

| Area | Details |
|------|:--------|
| **Block 16** | `defaultValue`/`rangeMin`/`rangeMax` contain garbage floats (5.19e30). Likely packed quaternion or engine-specific struct. |
| **Blocks 18/20** | Animation rates (81.6%/69.9%) far exceed noise usage. May be repurposed over-life curves. |
| **Gradient matrix** | Provisionally 4×4 RGBA transform; alternative: 10 RGBA stops per segment. |
| **Vec3 padding** | 4-byte trailing zero in 28-byte keyframes — alignment or reserved field. |
| **Block 12 colorTimeScale** | f32 at 0x2B0 (typically 0.01); may be minimum alpha or color floor. |
| **Emitter type 11** | 32 files; likely model particle. Model path mechanism undecoded. |
| **Tick rate** | 60 tps inferred from common timings, not confirmed by engine symbols. |
| **Look variant overrides** | 344-byte gap between variant name pairs; exact format undetermined. |

---

## Appendix A — Reading a PRT File (C++)

```cpp
FILE* f = fopen("particle.prt", "rb");

// ── §3  SNO Preamble ──────────────────────────────────────────────────────────
SnoPreamble preamble;
fread(&preamble, sizeof(SnoPreamble), 1, f);
assert(preamble.magic == 0xDEADBEEF);
assert(preamble.version == 180);

// ── §4  Particle Header ───────────────────────────────────────────────────────
ParticleHeader header;
fread(&header, sizeof(ParticleHeader), 1, f);

printf("Duration: %u  Type: %u  Speed: %.2f  Scale: %.2f\n",
       header.duration, header.emitterType, header.speedMultiplier, header.globalScale);

// ── §5–6  Read AnimRef Blocks ─────────────────────────────────────────────────
// Region 0: blocks 0–11 at 0x068 (12 standard AnimRefs)
AnimRef region0[12];
fseek(f, 0x068, SEEK_SET);
fread(region0, sizeof(AnimRef), 12, f);

// Block 12 (special) at 0x2A8
Block12MaterialRef block12;
fseek(f, 0x2A8, SEEK_SET);
fread(&block12, sizeof(Block12MaterialRef), 1, f);

if (block12.materialSnoId != 0xFFFFFFFF)
    printf("Material SNO: 0x%08X\n", block12.materialSnoId);

// ── §9  Read Keyframes for Block 0 (Emission Rate) ───────────────────────────
if (region0[0].dataSize > 0) {
    u32 kfCount = region0[0].dataSize / 12;
    fseek(f, region0[0].dataOffset + 16, SEEK_SET);
    for (u32 k = 0; k < kfCount; k++) {
        ScalarKeyframe kf;
        fread(&kf, sizeof(ScalarKeyframe), 1, f);
        printf("  kf[%u]: value=%.3f range=%.3f time=%.3f\n",
               k, kf.value, kf.valueRange, kf.normalizedTime);
    }
}

// ── §8  Read Color Gradient ───────────────────────────────────────────────────
Block13ColorGradient gradient;
fseek(f, 0x304, SEEK_SET);
fread(&gradient, sizeof(Block13ColorGradient), 1, f);

if (gradient.dataSize > 12) {
    u32 stopCount = gradient.dataSize / 160;
    fseek(f, gradient.dataOffset + 16, SEEK_SET);
    for (u32 s = 0; s < stopCount; s++) {
        GradientStop stop;
        fread(&stop, sizeof(GradientStop), 1, f);
        printf("  Stop %u: R=%.2f G=%.2f B=%.2f A=%.2f\n",
               s, stop.colorMatrix[0][0], stop.colorMatrix[1][1],
               stop.colorMatrix[2][2], stop.colorMatrix[3][3]);
    }
}

fclose(f);
```

---

## Appendix B — All Structures Summary

```cpp
// §3 — SNO Preamble
struct SnoPreamble {                            // 32 bytes
    u32 magic; u32 version; u32 snoId; u32 _unk00C;
    u32 _unk010; u32 _unk014; u32 _unk018; u32 _unk01C;
};

// §4 — Particle Header
struct ParticleHeader {                         // 72 bytes
    u32 flags; u32 duration; u32 startDelay; u32 loopDelay;
    u32 _res030; u32 _res034; f32 loopScale; u32 _res03C;
    u32 emitterType; f32 emitterAngle; f32 globalScale; u32 renderLayer;
    u32 blendMode; f32 midpointBias; f32 speedMultiplier;
    u32 _res05C; u32 _res060; u32 _res064;
};

// §5 — AnimRef (48-byte animated parameter reference)
struct AnimRef {                                // 48 bytes
    u32 dataOffset; u32 dataSize; u32 interpType; u32 _pad;
    f32 defaultValue; u32 _pad2[2]; f32 rangeMin; f32 rangeMax;
    u32 _pad3[3];
};

// §7 — Block 12 Material Reference
struct Block12MaterialRef {                     // 48 bytes
    u32 dataOffset; u32 dataSize; f32 colorTimeScale; u32 _zero;
    u32 materialSnoId; f32 colorMult_R, colorMult_G, colorMult_B, colorMult_A;
    f32 colorMult2_R, colorMult2_G, colorMult2_B;
};

// §8 — Block 13 Color Gradient Header
struct Block13ColorGradient {                   // 48 bytes
    u32 dataOffset; u32 dataSize; u32 interpType; u32 _pad[4];
    u32 sentinel; f32 timeScale; u32 _rem[3];
};

// §8 — Gradient Stop
struct GradientStop {                           // 160 bytes
    u32 index; u32 _pad; u32 hash; u32 count;
    f32 colorMatrix[4][4]; f32 tangentData[8]; u8 _padding[48];
};

// §9 — Keyframe Structures
struct ScalarKeyframe {                         // 12 bytes
    f32 value; f32 valueRange; f32 normalizedTime;
};
struct VectorKeyframe {                         // 28 bytes
    f32 minX, minY, minZ; f32 maxX, maxY, maxZ; u32 _pad;
};

// §10 — Gap Region Structures
struct Gap0ScaleConstants { f32 scaleX; u32 _p0[3]; f32 scaleY; u32 _p1[3]; f32 scaleZ; u32 _p2[2]; };
struct Gap1TimingParams { f32 emissionMidpoint; f32 c338; f32 emissionEndMult; /* ... */ };
struct Gap2RenderConfig { u8 modelPath[96]; f32 noiseAmplitude; f32 noiseFrequency; /* ... */ };

// §11 — Look Variant Name
struct LookVariantName { char name[64]; u32 flags; };  // 68 bytes
```

---

*Specification derived from binary analysis of 21,593 PRT files from Diablo III: Reaper of Souls.
Cross-validated against 68,512 M3 PAR_ v24 entries from 50,742 SC2/HotS files.*
