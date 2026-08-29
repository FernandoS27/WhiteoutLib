# PRT File Format Specification

**Format**: Diablo III Particle Emitter (`.prt`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**Version**: 180
**Corpus**: 21,593 files, all validated against this layout

**SNO Group**: 27 (`Particle`)
**Registered revision**: 213 — the shipped data is v180, so the binary's compiled struct describes a *newer* layout (see below / README §4)

See [README.md](README.md) for the build these offsets come from, the generator pipeline
and the conventions used below.

---


> **Corrections of 2026-08-15.** The structural model below (40 inline 48-byte
> paths, header first and array descriptor last, struct 2,296) is confirmed and
> unchanged. What was wrong was the *meaning* of almost everything outside the
> paths, because the previous pass had no engine functions to read — only corpus
> statistics and an M3 analogy. The particle simulation code has now been reverse
> engineered, and it overturns six things:
>
> 1. **There is no `ParticleColorSet`.** The 104-byte block at `0x2A8` is the
>    engine's `UberMaterial` — byte for byte the same type a `.mat` embeds at
>    `+32` and an `.app` `SubObjectAppearance` embeds at `+24`. §6 said "the same
>    104-byte type is referenced from the Appearance, Material and Rope
>    registrations", which was the clue. Its first dword is a **ShaderMap**
>    reference, the four `vColorN` are diffuse/specular/emissive/ambient, and the
>    "gradient" is the material's `MaterialTextureEntry[]` (160 bytes each — that
>    is why the size was always a multiple of 160). See §6.
> 2. **`EmitterParams`' three paths are the emitter shape's dimensions**, not
>    rate/speed/direction. The emission rate is emitter channel 8 and the initial
>    velocity is emitter channel 6. See §7.
> 3. **The 40 channels are individually identified.** `ParticleSystem_Spawn`
>    builds a 24-bit "this channel is constant" mask whose bit order *is* the
>    declaration order of the 24 particle channels, and every channel carries a
>    fixed engine channel id. See §8; the old inferred-function column is gone.
> 4. **`nCollisionFlags` (`0x318`) is the maximum number of simultaneous
>    instances** of the effect, and **`flSortBias` (`0x8DC`) is a kill radius in
>    world units**. Neither had anything to do with collision or sorting.
> 5. **`dwDuration` / `dwStartDelay` / `dwLoopDelay` are lifetime / emission
>    period / pre-simulation time.** The 60 fps tick rate is now confirmed, not
>    inferred: the engine multiplies all three by `0.016667`.
> 6. **The `dwPrtFlags` bit table in §11.2 and the M3 correspondences in §13 are
>    withdrawn.** Five flag bits are now known from the engine and none of them
>    matches the guessed table; all three M3 channel correspondences are wrong.
>
> **Second pass, same day.** A further sweep closed most of what §14 still listed:
> `eSystemType` is named for all ten shipped values (§11.3), the emitter shapes have
> real distributions rather than just an arity (§7), six of the seven
> `flPhysicsParam` floats are a **wind spring** and a burst Z-offset (§14), the
> `dwPrtFlags` read-set is proved exhaustive at five bits, and `flUnknown2A0` is
> proved to have **no runtime read at all**. One correction lands with them: this
> document and `D3_PARTICLE_DESIGN.md` both said there is no wind anywhere in the
> particle system. There is -- it just is not in `UpdateParticles`. Foliage and
> clutter (`eSystemType` 6 and 8) are routed to it *instead of* the particle
> simulation.
>
> **Revision note (earlier).** An earlier draft of this document split the file into
> 48-byte "AnimRef" blocks that began at the keyframe `(offset, size)` pair. That
> boundary is 40 bytes off. The engine's own struct — read out of the Diablo III
> Switch 2.6.2 binary at `0x71001AEAC0` — puts the header **first** and the array
> descriptor **last**. Splitting there removes every field the old draft listed as
> `_padding`, and explains all three "gap regions". Concretely, the old
> `ParticleHeader.emitterType` / `emitterAngle` / `globalScale` / `renderLayer` /
> `blendMode` / `midpointBias` / `speedMultiplier` are not emitter settings at
> all: they are the first animated channel's interpolation header. That is why
> the old §14.2 "emitter type" enum and §14.3 "interpolation type" enum listed the
> same values — they were the same field.

---

> **Corrections of 2026-08-28.** The particle *simulation* was read end to end
> this pass (`ParticleSystem_UpdateParticles`, `Particle_BuildOrientationBasis`,
> `Particle_ComputeInitialVelocity`, `sub_7100376430`). Four things below are
> wrong and are corrected in place; the rest stands.
>
> 1. **`eInterpolation` is the NODE COUNT, not a curve type.** The engine uses
>    `header[0]` as the loop bound when scanning the node array. Verified on
>    **863,720 / 863,720** paths across all 21,593 files:
>    `header[0] * sizeof(node) == nodeSize` exactly, and the 12- vs 28-byte split
>    reproduces the vector-slot list of section 8 independently. **Section 11.1's
>    enum is withdrawn** -- interpolation is always LINEAR; the format has no
>    curve-type field at all.
> 2. **`flBias` / `flScale` are a loop sub-range in normalised time**, not a bias
>    and a node scale. Once `t` passes `flScale` it wraps back into
>    `[flBias, flScale]`.
> 3. **`nFlags` is a random-distribution selector, 0..8** -- nine remap curves
>    applied to the uniform draw. The corpus holds exactly 0..8 over 798,941
>    paths and nothing outside.
> 4. **`tRandom` is a game-state DRIVER, not a randomiser.** `nMode` selects a
>    source (a game scalar, distance from the emitter, height, an actor
>    attribute, a health fraction) and the result *multiplies* the sampled value.
>    Mode 10 *is* a uniform random and appears **zero** times in the corpus. The
>    per-particle randomness lives in each node's `start..end` range instead.
>
> Also settled: **emitter slot 9 is channel 33 and slot 10 is channel 30**
> (section 8), and the kinematic triples' frames -- ids 17/18/19 are WORLD space,
> ids 20/21/22 are EMITTER-LOCAL, rotated by the emitter quaternion frozen on the
> particle at birth. Flag `0x10000000` *enables* the 300-unit clamp rather than
> bypassing it (section 11.2).
>
> Full derivations -- the nine distributions, the driver table, the fourteen
> render modes and the five motion models -- are in **`D3_PARTICLE_DESIGN.md`**
> in the WhiteoutFlakes repo root.

---

## Table of Contents

1. [Overview](#1-overview)
2. [File Layout](#2-file-layout)
3. [Interpolation Path](#3-interpolation-path)
4. [Keyframe Nodes](#4-keyframe-nodes)
5. [Particle](#5-particle)
6. [UberMaterial](#6-ubermaterial)
7. [EmitterParams](#7-emitterparams)
8. [Channel Catalog](#8-channel-catalog)
9. [How the Layout Was Derived](#9-how-the-layout-was-derived)
10. [Verification](#10-verification)
11. [Enumerations](#11-enumerations)
12. [Corpus Statistics](#12-corpus-statistics)
13. [M3 PAR_ Cross-Reference](#13-m3-par_-cross-reference)
14. [Known Unknowns](#14-known-unknowns)

---

## 1. Overview

A `.prt` file defines one particle emitter. Like every D3 SNO asset it is a raw
struct image: a 16-byte file header followed by the struct itself, followed by a
payload region that the struct's `(offset, size)` descriptors point into.

```
Appearance (.app)  →  Particle (.prt)  →  Material (.mat)
   model/actor          emitter def          texture/shader
```

The struct is **2,296 bytes**. Most of it — 1,920 bytes, 84% — is 40 animated
channels laid out inline, each a 48-byte `InterpolationPath`. The rest is
emitter configuration, an embedded `UberMaterial`, and a triggered-event list.

---

## 2. File Layout

```
┌────────────────────────────────────────────────────────────────┐
│  File header                                    16 bytes       │
│    +0  magic 0xDEADBEEF, +4  version 180, +8  unused           │
├────────────────────────────────────────────────────────────────┤
│  Particle struct                             2,296 bytes       │
│    +0     asset id and timing                                  │
│    +48    13 emitter channels    (13 × 48 = 624)               │
│    +672   UberMaterial (shader map, colours, textures)         │
│    +784   physics parameters                                   │
│    +832   EmitterParams          (280)                         │
│    +1112  24 particle channels   (24 × 48 = 1152)              │
│    +2264  render flags, triggered-event descriptor             │
├────────────────────────────────────────────────────────────────┤
│  Payload                                        variable       │
│    keyframe node arrays, material texture entries, events     │
│    every block 8-byte aligned; first block always at +2296     │
└────────────────────────────────────────────────────────────────┘
```

All payload offsets are relative to the **start of the struct**, i.e. file
position = 16 + offset. Blocks are packed in ascending order and padded to an
8-byte boundary, so a 12-byte node array is followed by 4 bytes of slack.

---

## 3. Interpolation Path

The unit of animation. 48 bytes, and the reason the old block split was wrong:
the array descriptor is the **last** 8 bytes, not the first.

```cpp
struct InterpolationScalar {                    // 12 bytes
    i32     nMode;              // 0x00: DRIVER SOURCE, 0 = disabled
    f32     flMin;              // 0x04: driver output range low
    f32     flMax;              // 0x08: driver output range high (default 1.0)
};

struct InterpolationPathHeader {                // 28 bytes
    i32                 nNodeCount;     // 0x00: NODE COUNT (was "eInterpolation")
    f32                 flLoopStart;    // 0x04: loop sub-range start, normalised time
    f32                 flLoopEnd;      // 0x08: loop sub-range end,   normalised time
    i32                 eDistribution;  // 0x0C: random distribution 0..8, see §11.1
    InterpolationScalar tDriver;        // 0x10: game-state driver, MULTIPLIES the sample
};

struct InterpolationPath {                      // 48 bytes
    InterpolationPathHeader header;     // 0x00 .. 0x1C
    u8                      _align[4];  // 0x1C: alignment
    void*                   pNodes;     // 0x20: runtime pointer, zero on disk
    i32                     nodeOffset; // 0x28: byte offset into the payload
    i32                     nodeSize;   // 0x2C: byte length
};
```

The binary declares ten of these — `FloatPath`, `IntPath`, `TimePath`,
`AnglePath`, `VelocityPath`, `ColorPath`, `VectorPath`, `VelocityVectorPath`,
`AngularVelocityPath`, `AccelVectorPath` — all 48 bytes and identical apart from
the node type they point at.

**The 12 bytes at `0x1C..0x28` are zero in 21,593 of 21,593 files.** Under the old
split the equivalent 12 bytes are non-zero in roughly half the files at the end
of each region, which is what identifies this as the correct boundary.

---

## 4. Keyframe Nodes

Every node is `{startValue, endValue, time}`. Only the value type varies.

```cpp
struct FloatNode  { f32 flStart;  f32 flEnd;  f32 flTime; };   // 12 bytes
struct IntNode    { i32 nStart;   i32 nEnd;   f32 flTime; };   // 12 bytes
struct ColorNode  { u32 dwStart;  u32 dwEnd;  f32 flTime; };   // 12 bytes, packed ARGB
struct VectorNode { Vector3f vStart; Vector3f vEnd; f32 flTime; }; // 28 bytes
```

Ten node types share those four layouts, and **the type name carries the unit**,
which the runtime acts on:

| Node type | Layout | Unit, and what the engine does with it |
|---|---|---|
| `FloatNode` | 12 | dimensionless |
| `TimeNode` | 12, **integer** | frames; multiplied by `0.016667` |
| `IntNode` | 12, integer | a count |
| `ColorNode` | 12, packed | a colour |
| `AngleNode` | 12 | **radians** |
| `VelocityNode` | 12 | per frame; **×60** |
| `AngularVelocityNode` | 12 | radians per frame; **×60** |
| `VectorNode` | 28 | dimensionless |
| `VelocityVectorNode` | 28 | per frame; **×60** |
| `AccelVectorNode` | 28 | per frame squared; **×60 ×60** |

Two of these are checkable straight off the disk. `TimeNode` values read as
integers are 1 / 30 / 18 / 24 / 60 / 20 / 22, while reading the same words as
floats yields denormals (max 8.4e-41 over 21,593 files). `AngularVelocityNode`'s
commonest non-zero value in particle slot 6 is −0.017453 — exactly one degree per
frame.

`nodeSize` is always an exact multiple of the node size — checked across the
whole corpus. The old draft's `VectorKeyframe._padding` at `0x18` is `flTime`.

Which node type a channel uses is fixed by the struct, and is recoverable from
the data: an `IntNode` channel's first word is a small integer (`0`, `1`, `2`,
`30`…) with a zero high byte, a `ColorNode` channel's is a packed colour
(`0xFFFFFFFF`, `0xFF000000`, `0x00FFFFFF`), and a `FloatNode` channel's is a
normal float. The three sets do not overlap in any file.

---

## 5. Particle

```cpp
struct Particle {                               // 2296 bytes
    i32                 dwSnoId;            // 0x000: asset id
    u8                  _unused[8];         // 0x004: zero in every file
    i32                 eSystemType;        // 0x00C: 10 values; the engine
                                            //   branches on it everywhere
    i32                 dwPrtFlags;         // 0x010: see §11.2
    i32                 tmLifetime;         // 0x014: system lifetime, frames @60
    i32                 tmEmissionPeriod;   // 0x018: emitter period; also the
                                            //   0..1 time base every path uses
    i32                 tmPreSimulate;      // 0x01C: spawn-time catch-up cap
    InterpolationScalar tLifetimeRandom;    // 0x020: multiplies tmLifetime
    i32                 dwUnknown2C;        // 0x02C: registered, zero in every file

    InterpolationPath   arEmitterPath[13];  // 0x030 .. 0x2A0   (see §8)

    f32                 flUnknown2A0;       // 0x2A0: default 0.01; NEVER READ by the
                                            //        shipped runtime (§14)
    u8                  _pad2[4];           // 0x2A4
    UberMaterial        tMaterial;          // 0x2A8 .. 0x310   (see §6)

    i32                 snoPhysics;         // 0x310: SNO ref, -1 = none
    f32                 flMass;             // 0x314: default 0.0310589
    i32                 nMaxInstances;      // 0x318: instance cap, 0 = unlimited
    f32                 flBurstZOffset;     // 0x31C: +Z on scripted bursts, default 0
    f32                 flSwayFrequency;    // 0x320: Hz, x2pi -> w.  default 1.0
    f32                 flSwayDamping;      // 0x324: zeta.           default 0.3
    f32                 flSwayMaxOffset;    // 0x328: x particle scale. default 1.0
    f32                 flSwayGustAmount;   // 0x32C: default 1.25
    f32                 flSwayBaseAmount;   // 0x330: default 0.0
    f32                 flPhysicsParam6;    // 0x334: default 1.0, still unread
    i32                 snoActor;           // 0x338: SNO ref, -1 = none
    u8                  _pad3[4];           // 0x33C

    EmitterParams       tEmitter;           // 0x340 .. 0x458   (see §7)

    InterpolationPath   arParticlePath[24]; // 0x458 .. 0x8D8   (see §8)

    i32                 nRenderMode;        // 0x8D8: enum, values 0..13 only
    f32                 flMaxDistance;      // 0x8DC: kill radius, default 10.0
    f32                 flCameraDistScale;  // 0x8E0: default 0.8
    i32                 dwEventOffset;      // 0x8E4: triggered events
    i32                 dwEventSize;        // 0x8E8: N * 412 bytes
    i32                 dwEventCount;       // 0x8EC: N
    void*               pEvents;            // 0x8F0: runtime pointer, zero
};                                          // 0x8F8 = 2296
```

**Timing.** `ParticleSystem_Spawn` stores `tmLifetime * 0.016667` as the system's
lifetime in seconds and `ParticleSystem_TickEmitter` releases the system once the
elapsed timer reaches it, which pins the tick rate at 60 fps. `tLifetimeRandom`
is fed to `InterpolationScalar_Evaluate` and, when that reports "applied", its
result *multiplies* the lifetime. `tmEmissionPeriod` drives a second timer whose
normalised value `clamp(elapsed / (tmEmissionPeriod/60), 0, 1)` is the `t` every
channel is sampled at — so it is the time base of the whole animation, and
reaching it also stops the emitter. `tmPreSimulate` caps the loop at the end of
`ParticleSystem_Spawn` that pre-runs the system by
`min(lifetime - 1/60, tmPreSimulate/60, 1000)` seconds so an effect can start
already established; it is 0 in 19,061 of 21,593 files.

**`nMaxInstances` (`0x318`) is an instance budget, not collision flags.**
`ParticleSystem_Spawn` keeps a per-`snoId` counter in a hash map and refuses the
spawn once the live count reaches this value. The corpus agrees: 15 distinct
values, 0 (unlimited) in 20,788 files and then 10, 25, 20, 7, 5, 15, 8, 4, 3.

**`flMaxDistance` (`0x8DC`) is a radius in world units, not a sort bias.**
`ParticleSystem_UpdateParticles` retires a particle when
`(sysX-pX)² + (sysY-pY)² > r²`. Default 10.0, present in 20,017 of 21,593 files.

**`flCameraDistScale` (`0x8E0`) is used only on the camera-relative placement
branch** of `ParticleSystem_TickEmitter`, which positions the system at
`cameraPos + value * (distance * direction)`. Whether it doubles as a LOD scale
is not established.

The seven floats at `0x31C .. 0x334` follow `snoPhysics` and `flMass`, so
"physics parameters" is safe as a group label, but **no engine read was traced to
any individual member**. They are deliberately left unnamed rather than carrying
the previous guesses (`flCollisionScale` / `flBounce` / `flFriction` /
`flDamping` / `flMassVariance` / `flPhysicsDelay` / `flPhysicsScale`), none of
which was ever supported by anything but the defaults.

`flMass` is confirmed: `ParticleSystem_EmitParticle` stores
`1 / max(flMass, 1e-6)` as the particle's inverse mass.

The `snoPhysics` and `snoActor` fields carry the SNO **group** in the
registration (`28` = Physics, `1` = Actor), and the corpus confirms both exactly.
Of the 526 files that set `snoPhysics`, all 20 distinct values are ids present in
the `.phy` corpus; of the 4,796 that set `snoActor`, all 3,512 distinct values are
ids present in the `.acr` corpus. Neither field ever holds an id belonging to any
other group. A previous table named the second one `snoTexturePrt`; it is Actor.

---

## 6. UberMaterial

104 bytes — and it is **not** a particle-specific type. The slot the registrar
reaches it through (`0x71010E95B8`) is the engine's `UberMaterial` descriptor,
the same one `.mat` embeds at `+32` and an `.app` `SubObjectAppearance` embeds at
`+24`. The previous draft noted that "the same 104-byte type is referenced from
the Appearance, Material and Rope registrations" and then described it as a
colour set anyway; every field below is different as a result.

```cpp
struct UberMaterial {                           // 104 bytes
    i32            snoShaderMap;    // 0x00: ShaderMap SNO ref, -1 = none
    MaterialColors tColors;         // 0x04 .. 0x4C
    //   0x04 vDiffuse   Vector4f
    //   0x14 vSpecular  Vector4f
    //   0x24 vEmissive  Vector4f
    //   0x34 vAmbient   Vector4f
    //   0x44 flShininess
    //   0x48 dwMaterialFlags
    i32            dwTextureOffset; // 0x4C: payload offset  }  SerializeData
    i32            dwTextureSize;   // 0x50: N * 160 bytes   }
    u8             _pad[4];         // 0x54
    void*          pTextures;       // 0x58: runtime pointer, zero on disk
    u8             _reserved[8];    // 0x60
};
```

The array is `MaterialTextureEntry[]`, 160 bytes each — which is exactly why the
"gradient size" was always a multiple of 160. That element type is already fully
described by the `.mat` / `.app` work; nothing here needs a separate decode.

**What the entries are for (2026-08-29).** `Particle_DrawBatch` @`0x71000B63A0`
binds exactly four of them, by `EMaterialTextureType`, and skips the rest of the
material system entirely -- no `Render_ResolveMaterialTextureStages`, no
`Material` SNO, pass index literal 0:

| Stage type | Runtime field |
|-----------:|:--------------|
| 1 | `sys+300` |
| 19 | `sys+304` |
| 12 | `sys+308` |
| 14 | `sys+312` |

Measured over all 21,593 files / 35,631 entries: type 1 x 18,462, 19 x 13,157,
12 x 2,066, 14 x 1,622, plus 300 entries of twelve other types that no particle
pass declares. Entries per file: 0 x 3,120, 1 x 5,327, 2 x 11,030, 3 x 544,
4 x 1,572; the leading sets are `{1,19}` x 10,904 and `{1}` x 5,311.

`snoShaderMap` resolves through the ordinary `ShaderMap_ResolveShaderOpaque`
chain, and what it lands on is the FIXED-FUNCTION billboard family: of the
18,420 files that resolve a pass, **16,420 are `Billboard.fx`** and 1,989 are
`SoftBillboard.fx` (the same chain with stage type 39, the scene-colour copy,
in front for a depth fade). Nineteen of the twenty corpus `SoftBillboard.fx`
shaders still bind `ps_legacy`. So the `Legacy.fx` per-stage combine block --
tens digit the op, units digit the MODULATE2X/4X gain -- is what governs the
chain, and shipped gains reach x4 on the colour and x32 on the alpha. The
render state is the pass's own: blend `(5,2)` x 7,450, `(5,6)` x 6,665,
`(11,5)` x 3,142; depth write on 6 of 18,420; alpha test on 367; and the
lighting tag `0xA000F` is 0, so particles are unlit.

Every claim above is checked against all 21,593 files:

| Check | Result |
|---|---|
| `dwTextureSize % 160 == 0` | 21,593 / 21,593 |
| `pTextures` (`0x58..0x60`) zero | 21,593 / 21,593 |
| `snoShaderMap` is a known ShaderMap id | 18,420 / 18,420 files that set it (252 / 252 distinct ids) |
| …and is never an id of Physics, Actor, Appearance, Material, Particle, Anim, AnimSet or Cloth | 0 hits in all eight groups |
| `flShininess` (`0x44`) zero | 21,593 / 21,593 |
| `vDiffuse` and `vSpecular` move together | all-1.0 in 11,435 files, all-0.0 in 10,158 |
| `vEmissive` zero | 21,586 / 21,593 |

The engine read that settles it: `ParticleSystem_Spawn` loads
`*(u32*)(particle + 0x38)` — the first dword of this block in the registered
revision — and resolves it in the **ShaderMap** SNO group, then does a tag-map
query on the result. `ParticleSystem_TickEmitter` reaches the atlas texture
through the same block.

---

## 7. EmitterParams

280 bytes. Three animated channels plus the name of the DCC node the emitter
shape was authored from.

**The three paths are the emitter shape's dimensions, not rate/speed/direction.**
`ParticleSystem_TickEmitter` switches on `eEmitterShape` and reads them
selectively; the switch's `default` case asserts, so the value set is closed.

```cpp
struct EmitterParams {                          // 280 bytes
    InterpolationPath tShapeExtent0;    // 0x00: FloatNode  — shapes 4,5,9,10
    InterpolationPath tShapeExtent1;    // 0x30: FloatNode  — shapes 5,9
    InterpolationPath tShapeExtent2;    // 0x60: VectorNode — shape 8
    i32               eEmitterShape;    // 0x90: see below
    char              szDccShapeName[128]; // 0x94: NUL-terminated, may be empty
    u8                _pad[4];          // 0x114: alignment, always zero
};
```

### Emitter shapes

| Value | Files | What the engine reads |
|------:|------:|:----------------------|
| 1 | 0 | nothing — a point emitter (legal, unused in this corpus) |
| 4 | 10,433 | `tShapeExtent0` only |
| 5 | 7,631 | `tShapeExtent0` **and** `tShapeExtent1` |
| 6 | 1,253 | no path — driven by an actor / bone |
| 7 | 17 | no path — a narrower actor-driven case |
| 8 | 220 | `tShapeExtent2`, sampled against `(0,0,0)` and `(1,1,1)`: a box extent |
| 9 | 1,590 | same as 5 |
| 10 | 265 | same as 4 |
| 11 | 184 | actor / bone driven, and advances a sequential emission index |

### What each shape actually samples

Resolved 2026-08-28 from `ParticleSystem_SampleEmitterShape` @`0x7100372D50`
(previously `sub_7100372D50`, and not even defined as a function in the IDB).
`TickEmitter` only *precomputes* the extents into a 0x50-byte emit context; the
sampling happens per particle inside that function, and the arity above was only
half of it.

**A `(lo, hi)` extent pair is an annulus, not a diameter.** `Rand_RadiusInAnnulus`
@`0x71005BC350` draws an **area-uniform** radius:

```
r = sqrt(lo^2 + U * (hi^2 - lo^2))       // U in [0,1), one MWC draw,
                                          // and NO draw at all when lo == hi
```

| Shape | Distribution |
|------:|:-------------|
| 1 | **Point.** `pos = base + offset`. No random draw. |
| 4 | **Sphere shell.** `r` from `tShapeExtent0`; direction uniform on the full sphere (`elev = asin(2U-1)`, `azim = 2*pi*U`). |
| 10 | **Hemisphere shell.** Identical, except the elevation draw is `asin(U)` with `U` in `[0,1)` -- the **+Z half only**. That is the *only* difference between shapes 4 and 10. |
| 5 | **Cylinder.** `r` from `tShapeExtent0` in the XY plane (`azim = 2*pi*U`), then `z = ext1.lo + ext1.span * U`. An annular tube when `ext0.lo > 0`. |
| 9 | **Spoked ring.** Same radius and Z as shape 5, but the azimuth is **deterministic and evenly spaced across the tick**: `phi = phi0 + (i * 2*pi) / n`, where `n` is the number of particles being emitted this tick, `i` the index within it, and `phi0` a single random draw taken at `i == 0`. |
| 8 | **Box.** Each axis independently uniform between `tShapeExtent2` evaluated at `r = (0,0,0)` and at `r = (1,1,1)` -- the node range low and high corners. This case adds the base position but **not** the emit-context offset; it is the only one that skips it. |
| 6, 7, 11 | **Skinned mesh surface** of the bound scene object. Sub-mesh index = the hardpoint, or uniform-random when the hardpoint is -1. Shapes 6 and 7 pick an area-weighted random triangle plus barycentric coordinates; **11 walks triangles in order** (`sequentialIndex % triangleCount`), which is what the sequential counter is for. Vertices are posed by the live skeleton and the dominant bone is returned to the caller. Shapes 6/11 resolve the object through the scene-object table at `gameCtx+2712`; shape 7 uses a different table and only for attachment kind 4. |

**Any actor lookup failure falls through to the point case.** A tool with no bound
actor therefore reproduces the engine exactly by treating shapes 6, 7 and 11 as
shape 1.

Two further details that only show up here:

* The base position is normally `lerp(sys.prevPos, sys.pos, U)` -- a **sub-frame
  emission interpolation** that draws one more value from the stream. It is the
  current position only when `sys+13 & 1` is set, and the **world origin** when
  `eSystemType == 10`.
* The emit context carries a `Vec3` offset at `+0x04`, zeroed by `TickEmitter` and
  used by `ParticleSystem_EmitBurst` -- which is where `flBurstZOffset` (`0x31C`) goes (§14).

The corpus corroborates the actor/mesh split independently: `szDccShapeName` is
populated for exactly the shapes the engine drives from geometry — 1,149 / 1,253
of shape 6, 178 / 184 of shape 11 and 17 / 17 of shape 7 carry a name, against
11 %, 14 %, 8 % and 17 % for shapes 4, 5, 9 and 10.

Observed names are Maya node names: `EmitShape_emit_001`,
`fxMeshShape_fxMesh_mat_001`, `oldActiveMeshShape_b_Column_001`,
`FX_EMITTER | FX_EMIT`. The buffer is empty in 17,819 of 21,593 files and the
longest name is 66 bytes; every byte after the terminator is zero in
21,593 / 21,593. `0x94 + 128 = 0x114`, and the final 4 bytes are the alignment
padding that rounds the struct to 280.

The emission rate lives at **emitter channel 8** and the initial velocity at
**emitter channel 6** — see §8.

---

## 8. Channel Catalog

`arEmitterPath[N]` sits at `0x030 + 48N`, `arParticlePath[N]` at `0x458 + 48N`.
Both the node type and the meaning now come from the engine, not from statistics.

### How the channels were identified

Three engine facts do the work.

**The path *type* carries the unit.** The binary registers ten distinct 48-byte
path types and the runtime converts by unit: a `VelocityPath` / `VelocityVectorPath`
sample is multiplied by **60** (per frame → per second) and an `AccelVectorPath`
sample by **60 × 60**. `TimePath` values are integers in frames and are multiplied
by `0.016667`. So the type is not cosmetic — it is how the data is interpreted.

**Every channel has a fixed engine channel id.** It is argument 3 of
`InterpolationPath_EvalScalar` / `_EvalVector` / `_EvalColor` / `_EvalInt` and,
together with the per-system random seed, selects that channel's random stream.
Ids 1–25 are particle channels and 28–40 emitter channels.

**`ParticleSystem_Spawn` enumerates the particle channels in order.** It builds a
24-bit mask, one bit per channel, by testing "one node, `start == end`, no
randomisation" — the constant-channel fast path. Bit *N* of that mask is slot *N*
of `arParticlePath`, and the vector/scalar split agrees on all 24 slots
(vectors at 9, 13, 16, 17, 18, 19, 20, 21 in both). Three further checks land
independently: slot 0 is the only slot in the file holding packed colours, slot 5
holds π/2, π/4 and π where the mask says `AnglePath`, and slots 18 and 21 hold the
tiny per-frame-squared values the mask says are `AccelVectorPath`s.

### Emitter channels (`0x030 + 48N`)

"Used" is the share of the 21,593 files in which the channel has any non-zero
node value.

| Slot | Path type | ch | Used | Meaning |
|-----:|:----------|---:|-----:|:--------|
| 0 | Float | 34 | 99.3% | Emitter-wide **size multiplier**, default 1.0 |
| 1 | **Int** | 31 | 77.1% | **Target live particle count.** The emitter emits `max(rateAccumulator, target − live)`, capped at `4096 − live`. Most-animated channel in the format (3 nodes in 11,956 files) |
| 2 | Float | 35 | 99.9% | **Overall effect scale**, default 1.0; further multiplied by a game-supplied per-instance scale |
| 3 | **Time** | 29 | 100% | **Particle lifetime**, in frames |
| 4 | Float | 28 | 100% | **Particle base size at birth** |
| 5 | **Angle** | 39 | 8.2% | **Emission cone half-angle**, radians. The initial velocity is rotated by this angle about a uniformly random azimuth |
| 6 | **VelocityVector** | 38 | 1.1% | **Initial velocity**, emitter-local — the vector the cone perturbs, then rotated by the emitter orientation |
| 7 | **VelocityVector** | 40 | 2.6% | **Initial velocity**, added *after* the emitter rotation (world-space term) |
| 8 | Velocity | 32 | 50.5% | **Emission rate**, particles per second. The *only* emission driver in 3,884 files that have no count path |
| 9 | Velocity | **33** | 5.1% | **Distance-based emission** — particles per unit of distance the emitter travels. Settled 2026-08-28, see below |
| 10 | Velocity | **30** | 6.5% | **Speed-based life shortening** — `life *= 1 - (speed x value) x dt`, floored at one frame |
| 11 | **Vector** | — | 3.6% | no counterpart in the registered revision; type only |
| 12 | Float | — | 5.3% | no counterpart in the registered revision; type only |

**Slots 9 and 10 are settled (2026-08-28).** Declaration order permits either
assignment, so the corpus decides it: **194 files set slot 9 while setting
neither the count channel (slot 1) nor the rate channel (slot 8)**. If slot 9
were ch 30 those files would emit nothing at all. Exactly **one** file
(`g_rainImpact.prt`) is in that position for slot 10.

The names corroborate: all 194, and all 54 files that set both, are trails --
`a3dun_Keep_Falling_Grate_Trail_Dust_Large.prt`,
`Actor_decapitate_blood_red_groundTrail.prt`,
`a1Dun_random_sparkleTrail_sparkles.prt`, `Actor_gib_blood_arcane_trails.prt`.
A trail is exactly "emit per unit of distance moved".

* **slot 9 = ch 33** — particles emitted per unit of *distance the emitter
  travels*. `ParticleSystem_TickEmitter` accumulates
  `speed x (value x 60 x dt) x dt`, where `speed = |sysPos - sysPrevPos| / dt`.
  The `dt` appears twice; that is as measured, and frame-rate coupled.
* **slot 10 = ch 30** — shortens the particle lifetime in proportion to emitter
  speed, `life *= 1 - (speed x value) x dt`, floored at one frame. Read in
  `Particle_InitLifeAndSize` @0x71000B6BB0 from SNO+384.

`EmitterParams`' own three paths (§7) are the shape extents and are not part of
this table.

### Particle channels (`0x458 + 48N`)

| Slot | Path type | ch | Meaning |
|-----:|:----------|---:|:--------|
| 0 | **Color** | 3 | **Colour over life.** Packed colour nodes; written to the particle's RGBA |
| 1 | Float | 5 | Scale curve over life; the runtime computes `this × effectScale`. Range 0..1 across the whole corpus |
| 2 | Float | 6 | **Alpha.** The sample is multiplied by 255 and clamped into the alpha byte of channel 0's colour |
| 3 | Float | 1 | **Size over life:** `size = this × baseSize(ch 28) × sizeMultiplier(ch 34)` |
| 4 | Float | 2 | A second size/scale factor, default 1.0. Which quantity it drives is not established |
| 5 | **Angle** | 24 | **Roll angle** — a scalar roll, integrated and wrapped into `[0, 2π]` |
| 6 | **AngularVelocity** | 25 | **Roll rate** (rad/s after ×60); the sum is clamped to ±16π before wrapping |
| 7 | **AngularVelocity** | 15 | **Spin rate** — a free 3-D spin about the ch 23 axis |
| 8 | **Angle** | 16 | **Spin angle**, accumulated into the particle's orientation quaternion at `particle+216`. The difference sign is reversed relative to every other (scalar, rate) pair |
| 9 | **Vector** | 23 | **Spin axis**; default `(0,1,0)` when absent, normalised |
| 10 | Float | 7 | **Orbit radial offset** — cylindrical orbit about the ch 10 axis |
| 11 | **Velocity** | 8 | **Orbit radial speed** |
| 12 | **AngularVelocity** | 9 | **Orbit angular speed** about that axis |
| 13 | **Vector** | 10 | **Orbit axis**; default `(0,0,1)`, **normalised to unit length** at spawn |
| 14 | Float | 11 | **Radial offset** — push along `normalize(particlePos - sysPos)` |
| 15 | **Velocity** | 12 | **Radial speed** along the same direction |
| 16 | **Vector** | 17 | Offset ⎫ |
| 17 | **VelocityVector** | 18 | Velocity ⎬ kinematic triple A — **WORLD space** |
| 18 | **AccelVector** | 19 | Acceleration ⎭ |
| 19 | **Vector** | 20 | Offset ⎫ |
| 20 | **VelocityVector** | 21 | Velocity ⎬ kinematic triple B — **EMITTER-LOCAL** |
| 21 | **AccelVector** | 22 | Acceleration ⎭ |
| 22 | **Velocity** | 13 | **Target-seek speed** toward the bound spawn target at `sys+352` |
| 23 | Float | 14 | **Target-seek offset**, optionally scaled by the seek distance |

Slots 16-21 are two *(offset, velocity, acceleration)* triples read back to back
by `ParticleSystem_UpdateParticles`, typed by their x60 / x3600 scaling. **Their
frames are settled (2026-08-28): triple A (ids 17/18/19) is WORLD space** -- its
displacement is added to the particle position directly -- **and triple B (ids
20/21/22) is EMITTER-LOCAL**, its displacement rotated by the quaternion at
`particle+184..196`, which `Particle_InitLifeAndSize` copies from the emitter's
own orientation (`sys+100..112`) at birth and never updates. That is the same
quaternion `Particle_ComputeInitialVelocity` applies to the birth velocity.

The *offset* member of each triple is not applied to the position. It is
differentiated against its own previous sample, cached on the particle, and the
result added to the velocity: `v = velocityPath x 60 + (offsetNow - offsetPrev)/dt`.
The same idiom governs every (scalar, rate) pair in the format.

The twelve vector slots across both tables are exactly the twelve the earlier
corpus-only analysis identified as `vec3` — derived independently, and a useful
check on the split, since a 40-byte misalignment would not preserve them.

---

## 9. How the Layout Was Derived

The corpus is version 180. The Switch 2.6.2 binary describes version 213: its
`Particle` struct is 704 bytes because each channel became a *variable array of*
paths (16 bytes: a pointer plus an `(offset,size)` pair) instead of one path
inline (48 bytes). Everything outside the channels is unchanged, which is what
lets the two be aligned.

Ten registered default constants land on the matching v180 offset exactly
(the previous text said nine and then listed ten):

| v213 offset | default | v180 offset |
|---:|:---|---:|
| 0x030 | `0x3C23D70A` = 0.01 | 0x2A0 |
| 0x0A4 | `0x3CFE68F1` = 0.0310589 | 0x314 |
| 0x0B0 | `0x3F800000` = 1.0 | 0x320 |
| 0x0B4 | `0x3E99999A` = 0.3 | 0x324 |
| 0x0B8 | `0x3F800000` = 1.0 | 0x328 |
| 0x0BC | `0x3FA00000` = 1.25 | 0x32C |
| 0x0C0 | `0x00000000` = 0.0 | 0x330 |
| 0x0C4 | `0x3F800000` = 1.0 | 0x334 |
| 0x2A4 | `0x41200000` = 10.0 | 0x8DC |
| 0x2A8 | `0x3F4CCCCD` = 0.8 | 0x8E0 |

Both structs also end with the same 32-byte tail: an int, two floats, a
`SerializeData`, an element count, and a runtime pointer. In v213 that tail runs
`0x2A0..0x2C0` and the struct is 704; in v180 it runs `0x8D8..0x8F8` and the
struct is 2296.

---

## 10. Verification

Run over all 21,593 files:

| Check | Result |
|---|---|
| magic and version | 21,593 / 21,593 |
| 12 zero bytes before every one of the 40 array descriptors | 21,593 / 21,593 |
| every reserved slot listed above is zero | 21,593 / 21,593 |
| every `nodeSize` an exact multiple of its node size | 21,593 / 21,593 |
| `UberMaterial` texture-array size a multiple of 160 | 21,593 / 21,593 |
| `UberMaterial` runtime pointer zero | 21,593 / 21,593 |
| `snoShaderMap` is a known ShaderMap id | 18,420 / 18,420 populated |
| `dwEventSize == dwEventCount * 412` | 1,388 / 1,388 populated |
| payload accounted for, ignoring 8-byte alignment slack | 21,593 / 21,593 |

The last one is the strongest: with the struct at 2,296 bytes, every payload byte
in every file is either inside a block one of these descriptors points at, or one
of the 4-byte alignment gaps between blocks. Nothing is left over.

`tests/d3_native_test.cpp` re-checks the ragged-array, gradient-stride and
event-count invariants on every build.

---

## 11. Enumerations

### 11.1 `eDistribution` (`InterpolationPathHeader` + 0x0C)

**The interpolation-type enum previously on this line is withdrawn.** `+0x00` is
the node count and interpolation is always linear; this field is the random
distribution applied to the per-particle uniform draw before it lerps each
node's `start..end` range. With `u` the raw uniform and `c = u - 0.5`, all
results clamped to `[0,1]`:

| Value | Paths | Remap | Shape |
|------:|------:|:------|:------|
| 0 | 761,486 | `u` | uniform |
| 1 | 13,885 | `min(u^2, 1)` | biased to `start` |
| 2 | 1,020 | `0.5 + sign(c)*2c^2` | centre-weighted S |
| 3 | 2,705 | `1 - u^2` | biased to `end` |
| 4 | 4,897 | `c<0 ? 2c^2 : 1-2c^2` | tent |
| 5 | 7,616 | `1 - sqrt(u)` | strongly biased to `start` |
| 6 | 1,870 | `c<0 ? 0.5*sqrt(2|c|) : 1-0.5*sqrt(2|c|)` | tent, sqrt profile |
| 7 | 1,781 | `sqrt(u)` | strongly biased to `end` |
| 8 | 3,681 | `0.5 + sign(c)*0.5*sqrt(2|c|)` | edge-weighted inverse-S |

Anything >= 9 returns 0. Counts are over all 37 emitter + particle paths in all
21,593 files (798,941 samples); values outside 0..8 do not occur.
`sub_71003751F0` @0x71003751F0 is the switch.

### 11.1b `tDriver.nMode` (`InterpolationPathHeader` + 0x10)

Selects a **game-state** value in `[0,1]` (`sub_7100374760` @0x7100374760); the
result is mapped through `flMin..flMax` and multiplies the sampled channel.
Mode 0 disables it.

| Mode | Paths | Source |
|-----:|------:|:-------|
| 0 | 792,445 | disabled |
| 1 | 4,703 | a game-supplied scalar (eval ctx + 20) |
| 8 | 1,419 | an actor scalar, or actor field +960 |
| 6 | 232 | normalised height above the system origin |
| 3 | 55 | normalised distance from the system origin |
| 2 | 40 | a global game value |
| 13 | 32 | `clamp(actorAttr * 0.5, 0, 20) / 20` |
| 9 | 10 | actor attribute `0xFFFFF0A7` |
| 5 | 3 | an actor scalar |
| 7 | 2 | `1 - clamp((actorScalar - 0.25)/0.08, 0, 1)` |
| 10 | **0** | uniform random -- never authored |

The two normalising divisors are `flMaxDistance` (`0x8DC`) and
`flCameraDistScale` (`0x8E0`), which is a second role for both beyond section 5.

### 11.2 `dwPrtFlags` (0x010)

**The previous table on this line has been withdrawn.** It assigned a meaning to
fourteen bits purely from how often each was set — "system enabled", "use colour
gradient", "billboard facing mode" and so on — and none of it survives contact
with the engine. 870 distinct values occur over the corpus.

Only these bits have a traced read, all of them in `ParticleSystem_Spawn` and
`ParticleSystem_TickEmitter`:

| Bit | Meaning |
|-----|:--------|
| `0x00000008` | skip the visibility / frustum test |
| `0x00008000` | raise the live-system budget from 128 to 256 |
| `0x00080000` | early-out gate on spawn |
| `0x00100000` | force the 128-system budget |
| `0x10000000` | **enable** the 300-unit emitter-speed clamp on distance-based emission. The guard is `speed < 300 \|\| !(flags & 0x10000000)`, so distance emission is skipped only when the bit is SET and the emitter is moving faster than 300 u/s. (Corrected 2026-08-28; the previous line had this inverted.) |

**That list is now known to be exhaustive for this build** (2026-08-28). Only four
functions in the whole image `SNO_AcquireAsset` the Particle group, and a scan of
every dereference of the SNO pointer (`sys+856`) across all 350 functions of the
particle module finds exactly one read of `+16` -- the `TickEmitter` one. The
remaining 23 bits observed in the corpus have **no runtime read at all**; they are
authoring-tool data. (The read-set is per build; the PC build may consume more.)

Note also that the bits which gate *behaviour* are not these. `ParticleSystem_Spawn`
derives a separate **capability mask** at `sys+8` by asking each channel whether its
value range is non-trivial, and the simulation gates on that:

| `sys+8` bit | Set when |
|---|:--|
| `0x0001` | orbit -- ch 8 or ch 7 has a non-zero x60 range |
| `0x0002` | radial -- ch 12 |
| `0x0004` | kinematic triple A (ids 17/18/19), world space |
| `0x0008` | kinematic triple B (ids 20/21/22), emitter space |
| `0x0010` | free spin -- ch 16 / ch 15 |
| `0x0040` | spin axis ch 23 is not constant |
| `0x0080` | target seek -- ch 13 / ch 14 |
| `0x0100` | roll -- ch 24 / ch 25; **forced off for `eSystemType` 6 and 8** |
| `0x2000` | `eSystemType` is 1, 3 or 4 |

### 11.3 `eSystemType` (0x00C)

**Resolved 2026-08-28.** `ParticleSystem_UpdateDispatch` @`0x71000B1660` is the
per-frame entry point and switches on this field to pick an update path; the
corpus filenames then name the values without ambiguity.

| Value | Files | Update path | Reading |
|------:|------:|:------------|:--------|
| 0 | 16,685 | normal | **Standard** particle system |
| 1 | 4,790 | normal | **Child actor** -- `EmitParticle` takes a separate branch that spawns a whole `.acr` per emission and pools no particle at all. Names: `cos_wings_*`, `*_helix`, `discEmitter`, `blastWave`, `flailSweep`, `Portal_Backing_Vertical`. See below |
| 2 | 31 | normal + physics body | **Swarm** -- each particle also owns a 40-byte `{invMass, pos, vel}` body in a shared world-collision solver. Names: `g_gnats`, `flies_deadbodies`, `TorchEmbers`, `Random_Stars` |
| 3 | 1 | normal + physics body | child actor **and** physics (`assaultBeast_death_popper`) |
| 4 | 4 | **never updated** | **Light shaft / volumetric**: `P6_Church_lightShaft`, `Lightray_Blue`, `Coast_Mist`, `Misty_Wind`. A child-actor type too, but its emitter never ticks |
| 5 | 0 | never updated | unused in this corpus |
| 6 | 28 | **wind spring only** | **Wind-swayed foliage**: `BattleNet_Grass_C`, `Cattails*`, `DuneGrassA`, `Wheat*`, `Moss_Particle` |
| 7 | 2 | **never updated** | **Static ground clutter**: `rockPebble_A_particle`, `Gravel_Clutter` |
| 8 | 30 | **wind spring only** | **Wind-swayed clutter**: every name ends `_Clutter` |
| 9 | 21 | normal + physics body | **Weather**: `g_Rain_*`, `g_Snow`, `g_SandStorm_*`, `g_Spores` |
| 10 | 1 | normal | **World-anchored**: `g_rainRipple`. The emit base is the world origin, not the system position |

Two consequences worth stating plainly:

* Types **4, 5 and 7 are never simulated.** `UpdateDispatch` returns before doing
  anything at all. They are authored as one static frame.
* Types **6 and 8 never run the particle simulation either.** They run
  `ParticleSystem_StepWindSpring` instead (§14), so none of the five motion models,
  the emission accumulator or the channel evaluation applies to them.

**CORRECTED 2026-08-29 -- type 1 is not a ribbon.** The earlier reading came
from the `cos_wings_*` names and from a 176-byte record; both point elsewhere.
`ParticleSystem_EmitParticle` @`0x71000B1A00` opens on

```
type = sys+4
if ((type - 3) < 2 || type == 1)   -> the CHILD-ACTOR path      // 1, 3, 4
else if (type == 9)                -> the 176-byte record path  // weather
else                               -> the 664-byte particle pool
```

so the segment record belongs to **type 9**, and types 1, 3 and 4 never touch
the particle pool: the branch builds a 352-byte record on the *stack*, runs
`Particle_InitLifeAndSize` plus one `ParticleSystem_UpdateParticles` over it,
and calls `Actor_SpawnFromSno` (`0x710021FDA0`). Every emission is a whole ACD.

The corpus settles it from the other side. `snoActor` (`0x338`) is what the
branch's first line requires (`if (sys+440 == -1) return`), and it is set on

```
type 1: 4,790 of 4,790     type 3: 1 of 1     type 4: 4 of 4
type 0: 1 of 16,685 (banner_treasureGoblin_glow.prt -- never read)
everything else: 0
```

Consequences: the emit clamp counts **children**, not particles
(`sys+408 + sys+376`), which is why **4,189 of the 4,795 author a target count
of exactly 1** and 4,279 author no emission rate at all; the spawned actor is
then forgotten (no transform is ever pushed to it, and
`ParticleSystem_ReleaseAttachments` frees the link nodes and nothing else); and
the scale is the birth-size channel times the actor's own tags 65543/65544,
pre-multiplied by the emit path so `Actor_SpawnFromSno` does not apply it twice.

Also from the same pass: `ParticleSystem_TickEmitter` returns before the
accumulator for types **7 and 8** (`if (type - 7 < 2) return 0`), and
`ParticleSystem_Update` only reaches the emitter for types outside 4..8, so five
of the ten values never emit per frame at all.

### 11.4 `nRenderMode` (0x8D8)

Exactly 14 distinct values, and they are precisely 0..13 with nothing outside
that range (0 ×9,181, 1 ×3,917, 7 ×2,312, 2 ×2,124, 12 ×1,428, 13 ×1,326,
10 ×398, 4 ×351, 6 ×176, 5 ×145, …). `ParticleSystem_EmitParticle` and the
batching code test it for **equality** with 1, which reads as an enum rather than
a bit field.

**All fourteen cases are now characterised structurally (2026-08-28).**
`Particle_BuildOrientationBasis` @0x71000BAB30 is one switch on this field, and
every case leaves a normalised quaternion in its output. The differences are
which vector seeds the frame — the particle axis, the velocity, a direction to a
reference point, or a caller-supplied quaternion — and whether that seed is
flattened onto the XY plane first. Modes **1 and 8 are no-ops** (they return
immediately and leave the caller's frame); mode **13 discards the seed's Z**,
producing a ground-aligned quad; modes **4 and 6** are XY-only variants of 5 and
of the negated axis. The per-case table is in `D3_PARTICLE_DESIGN.md` §7.

Two per-mode behaviours sit *outside* that switch, added 2026-08-28:

* Modes **9 and 10 are ground-conforming.** `Particle_PrepareDrawFrame`
  @`0x71000BCB10` raycasts the world under the particle and caches the surface
  normal (defaulting to `(0,0,1)` on a miss); that normal is the vector the 9/10
  case of `BuildOrientationBasis` normalises. The cast is re-done only when the
  particle XY has changed, so a stationary particle casts once.
* Mode **1 replaces the frozen birth quaternion.** Every particle normally copies
  the emitter orientation at birth; when `nRenderMode == 1`,
  `ParticleSystem_EmitParticle` writes a runtime global quaternion instead, so
  mode-1 births align to a shared frame rather than to their emitter.

The artist-facing **names** are still not established — retail compiles the enum
tables out, and every field-name pointer in the type registration aims at the
same empty string. `Particle_RegisterTypes` @`0x71001AEAC0` does contain two
`TypeDesc_RegisterField_Plain_EnumTable` calls, but both are SNO-group references
(Physics = 28 at `0x318`, Actor = 1 at `0x338`), not artist enums.

---

## 12. Corpus Statistics

| Metric | Value |
|--------|-------|
| Files | 21,593 |
| Version | 180, no variation |
| Struct size | 2,296 bytes |
| File size range | 3,140 – 8,464 bytes |
| Median file size | 3,396 bytes |
| Animated channels | 40, all present in every file |
| Mean `tmLifetime` | 149.1 frames (≈2.5 s; 60 fps confirmed from the engine) |
| Files with material textures | 18,473 (85.6%) |
| Files with triggered events | 1,388 (6.4%) |
| Files with a DCC emitter shape name | 3,774 (17.5%) |
| Files with a ShaderMap reference | 18,420 (85.3%) |
| Files with a Physics reference | 526 (2.4%) |
| Files with an Actor reference | 4,796 (22.2%) |

---

## 13. M3 PAR_ Cross-Reference

`.prt` is a structural analogue of the M3/HotS **PAR_ v24** emitter chunk.

| Aspect | M3 PAR_ | D3 .prt |
|--------|---------|---------|
| Container | chunk inside .m3 | standalone SNO asset |
| Struct size | 1,496 fixed | 2,296 fixed + payload |
| Channel size | 20 (float) / 36 (vec3) | 48, all types |
| Default | `initValue` is the value | `flScale` ≈ 1.0, scales the nodes |
| Keyframes | inline SEQS/STC | payload pool at end of file |
| Colour | 3 × AnimRef&lt;color&gt; | one `ColorPath` + a gradient blob |
| Material | index | SNO reference |

**The channel correspondences previously claimed here are withdrawn — all three
are wrong.** They were matched on range overlap alone, and the engine says:

| Claimed | Actually |
|---|---|
| emitter channel 0 ↔ `emissionRate` | emitter 0 is the emitter-wide size multiplier (ch 34); the emission rate is emitter **8** |
| particle channels 5 and 6 ↔ `zAcceleration` | they are a rotation **angle** and its **angular rate** (ch 24 / ch 25) |
| particle channel 19 ↔ `emissionSpreadX` | it is a `VectorPath` offset (ch 20); the emission spread is emitter **5**, an `AnglePath` |

Range overlap between two corpora is not evidence of shared meaning when both
corpora are dominated by values near 0 and 1. The structural comparison in the
table above still stands; the per-channel mapping does not.

---

## 14. Known Unknowns

| Area | Details |
|------|:--------|
| ~~`flUnknown2A0` (`0x2A0`)~~ | **Settled 2026-08-28, negatively.** A scan of every dereference of the Particle SNO across the whole particle module produces the complete runtime read-set, and `0x2A0` (runtime `+48`) is not in it. The field is authoring-only in this build. Its registered default is 0.01 and 19,468 of 21,593 files carry exactly that. |
| ~~`flPhysicsParam0..6` (`0x31C..0x334`)~~ | **Six of the seven resolved 2026-08-28.** They are not one group: `0x31C` is consumed alone by `ParticleSystem_EmitBurst` as a **+Z spawn offset** for scripted bursts, and `0x320..0x330` are the five constants of the **wind spring** below. `0x334` (runtime `+196`, default 1.0) still has no traced read. |
| Wind sway (`0x320..0x330`) | `ParticleSystem_StepWindSpring` @`0x71000BD610` is a 2-D damped harmonic oscillator run **per particle, in XY only**, and it is the *only* external-force integrator in the system. `w = flSwayFrequency * 2pi`; `a = -2*zeta*w*v - w^2*x + F`; `v += a*dt`; `x += v*dt`; when \|x\| exceeds `flSwayMaxOffset * particleScale` the offset is renormalised to 0.95 of the limit and the velocity is zeroed. The force is `windDir2D * (flSwayBaseAmount - flSwayGustAmount * cos(particlePhase * 2pi + windPhase) * windStrength) / 60`, where `particlePhase` is a per-particle random -- which is what makes a gust travel across a field of grass rather than moving it in lockstep. Reached only for `eSystemType` 6 and 8, and **instead of** the particle simulation, never alongside it. |
| ~~Emitter channels 9 and 10~~ | **Resolved 2026-08-28**: slot 9 = ch 33, slot 10 = ch 30 — see §8. |
| Emitter channels 11 and 12 | Retired in the registered revision, so only their storage (vector / float) is measurable. |
| ~~Particle channels 10, 11, 14, 15, 22, 23~~ | **Resolved 2026-08-28**: 10/11 are an orbit radial offset and speed, 14/15 a radial offset and speed, 22/23 a target-seek speed and offset — see §8 and `D3_PARTICLE_DESIGN.md` §5. |
| Particle channel 4 (ch 2) | Still open. Default 1.0, median 1.0 over the corpus, max 139.6. Its only traced consumer copies it to a spawned child actor's field `+956`. |
| ~~Kinematic triples (particle 16–18 vs 19–21)~~ | **Resolved 2026-08-28**: 16–18 (ids 17/18/19) is WORLD space, 19–21 (ids 20/21/22) is EMITTER-LOCAL — see §8. |
| ~~`eSystemType`~~ | **Resolved 2026-08-28** -- all ten shipped values named from the update dispatcher plus corpus filenames. See §11.3. |
| `nRenderMode` | All fourteen cases are now characterised **structurally** — `Particle_BuildOrientationBasis` @0x71000BAB30 is one switch on this field and each case's basis construction is described in `D3_PARTICLE_DESIGN.md` §7. The artist-facing *names* are still unknown; retail compiles the enum tables out. |
| `dwPrtFlags` | Five bits traced to engine reads (§11.2); the rest unknown. The previous set-rate-derived table is withdrawn. |
| Triggered-event record | 412 bytes each in v180, confirmed by `dwEventSize / dwEventCount` on 1,388 files. The registered revision's entry is **192 bytes**, so it is not a shared layout and the v180 internals stay undecoded. What the runtime does with it *is* now known: `ParticleSystem_FireTriggeredEvents` walks the array, evaluates each entry's condition (`sub_7100342FF0`, entry+0 is a message type id), and dispatches under the particle-world mutex. Two ids are traced at their call sites -- **3500 = emission** and **3501 = the particle left `flMaxDistance`**. Ids 3502 and >3503 always dispatch as kind 1; the rest are suppressed by capability bit `0x1000`. |
| `dwUnknown2C` (`0x02C`) | A registered field, not padding — but zero in 21,593 / 21,593 files. |

| ~~Emitter-shape distributions~~ | **Resolved 2026-08-28.** Which distribution each extent describes is in §7: shape 4 is a sphere shell, 10 a hemisphere shell, 5 a cylinder, 9 an evenly-spoked ring, 8 a box, and 6/7/11 the skinned mesh surface. The `(lo, hi)` pairs are area-uniform annulus radii, not diameters. |
| Particle channel 16 sign | Confirmed at instruction level 2026-08-28 (`0x71000BF124`: `FSUB S1, prev, now`). The reversed difference is **real**, not a decompiler artifact; every other `(scalar, rate)` pair in the system uses `now - prev`. |

**No longer unknown**, for the record: the 104-byte block is `UberMaterial` and
the "160-byte gradient stop" is `MaterialTextureEntry` (§6); `eEmitterShape` has
nine values with known engine behaviour *and* known distributions (§7); the tick
rate is confirmed at 60 fps by the `0.016667` conversions in the engine; and
`nCollisionFlags` is `nMaxInstances` (§5).

---

*Struct shapes from the Diablo III Nintendo Switch 2.6.2 build (`exefs/main`,
type registration at `0x71001AEAC0`). Offsets and enumerations verified against
21,593 `.prt` files. Field names are curated: retail builds compile them out.*
