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
    i32     nMode;              // 0x00: usually 0
    f32     flMin;              // 0x04: usually 0.0
    f32     flMax;              // 0x08: usually 1.0
};

struct InterpolationPathHeader {                // 28 bytes
    i32                 eInterpolation; // 0x00: curve type, see §11.1
    f32                 flBias;         // 0x04: usually 0.0
    f32                 flScale;        // 0x08: usually 1.0 -- scales the nodes
    i32                 nFlags;         // 0x0C: usually 0
    InterpolationScalar tRandom;        // 0x10: per-particle randomisation
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

    f32                 flUnknown2A0;       // 0x2A0: default 0.01, meaning unknown
    u8                  _pad2[4];           // 0x2A4
    UberMaterial        tMaterial;          // 0x2A8 .. 0x310   (see §6)

    i32                 snoPhysics;         // 0x310: SNO ref, -1 = none
    f32                 flMass;             // 0x314: default 0.0310589
    i32                 nMaxInstances;      // 0x318: instance cap, 0 = unlimited
    f32                 flPhysicsParam0;    // 0x31C: no default
    f32                 flPhysicsParam1;    // 0x320: default 1.0
    f32                 flPhysicsParam2;    // 0x324: default 0.3
    f32                 flPhysicsParam3;    // 0x328: default 1.0
    f32                 flPhysicsParam4;    // 0x32C: default 1.25
    f32                 flPhysicsParam5;    // 0x330: default 0.0
    f32                 flPhysicsParam6;    // 0x334: default 1.0
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
| 9 | Velocity | 33 or 30 | 5.1% | one of the two below — order not established |
| 10 | Velocity | 30 or 33 | 6.5% | one of the two below — order not established |
| 11 | **Vector** | — | 3.6% | no counterpart in the registered revision; type only |
| 12 | Float | — | 5.3% | no counterpart in the registered revision; type only |

Slots 9 and 10 are channels 33 and 30, but **which is which is not established**:
both are `VelocityPath`, both are used by a similar share of files, and the
ordering constraints permit either assignment.

* **ch 33** — particles emitted per unit of *distance the emitter travels*; the
  sample is multiplied by the emitter's world speed and by `dt`.
* **ch 30** — shortens the particle lifetime in proportion to emitter speed,
  `life *= 1 − (speed × value) × dt`, floored at one frame.

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
| 5 | **Angle** | 24 | Rotation angle |
| 6 | **AngularVelocity** | 25 | …and its rate (rad/s after ×60) |
| 7 | **AngularVelocity** | 15 | A second rotation rate |
| 8 | **Angle** | 16 | …and its angle. Note the pair is ordered rate-then-angle here, the reverse of 5/6 |
| 9 | **Vector** | 23 | A direction / axis; default `(0,1,0)` when absent |
| 10 | Float | 7 | A scalar… |
| 11 | **Velocity** | 8 | …and its rate. Quantity not established |
| 12 | **AngularVelocity** | 9 | A third angular rate |
| 13 | **Vector** | 10 | A second direction / axis; default `(0,0,1)`, **normalised to unit length** at spawn |
| 14 | Float | 11 | A scalar… |
| 15 | **Velocity** | 12 | …and its rate. Quantity not established |
| 16 | **Vector** | 17 | Offset ⎫ |
| 17 | **VelocityVector** | 18 | Velocity ⎬ kinematic triple A |
| 18 | **AccelVector** | 19 | Acceleration ⎭ |
| 19 | **Vector** | 20 | Offset ⎫ |
| 20 | **VelocityVector** | 21 | Velocity ⎬ kinematic triple B |
| 21 | **AccelVector** | 22 | Acceleration ⎭ |
| 22 | **Velocity** | 13 | A rate… |
| 23 | Float | 14 | …and its scalar. Quantity not established |

Slots 16–21 are two identical *(offset, velocity, acceleration)* triples read back
to back by `ParticleSystem_UpdateParticles`, typed by their ×60 / ×3600 scaling.
They almost certainly differ by coordinate frame (emitter-local vs world), but
**which is which is not established**.

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

### 11.1 Interpolation type (`InterpolationPathHeader.eInterpolation`)

| Value | Meaning |
|------:|:--------|
| 1 | Linear |
| 2 | Step / hold |
| 3 | Smooth (cubic / Hermite) |
| 4 | Smooth in, linear out |
| 5 | Linear in, smooth out |
| 6 | Bezier |
| 7 | Bezier smooth |
| 9 | Auto-tangent / TCB |

Values up to 31 occur. Channel 0 of the emitter set uses `1` in 91.5% of files —
this is the field the earlier draft reported as "emitter type = billboard,
91.8%".

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
| `0x10000000` | bypass the 300-unit emitter-speed clamp on distance-based emission |

The remaining bits are **not established**.

### 11.3 `eSystemType` (0x00C)

Ten distinct values over the corpus: 0 (16,685), 1 (4,790), 2 (31), 8 (30),
6 (28), 9 (21), 4 (4), 7 (2), 3 (1), 10 (1). The engine branches on it in at
least six places, which bounds what it can mean even though the individual values
are not named: types **6 and 8** suppress the rotation channels, types **7 and 8**
return early from the emitter tick and skip the frustum test, and `{1,3,4}`,
`{2,3,9}` and `{4,5,6}` each take a dedicated path elsewhere.

### 11.4 `nRenderMode` (0x8D8)

Exactly 14 distinct values, and they are precisely 0..13 with nothing outside
that range (0 ×9,181, 1 ×3,917, 7 ×2,312, 2 ×2,124, 12 ×1,428, 13 ×1,326,
10 ×398, 4 ×351, 6 ×176, 5 ×145, …). `ParticleSystem_EmitParticle` and the
batching code test it for **equality** with 1, which reads as an enum rather than
a bit field. The individual values are **not established**.

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
| `flUnknown2A0` (`0x2A0`) | Registered default 0.01, and 0.01 in 19,468 of 21,593 files; other values seen are 0.2, 1.0, 0.1, 0.09 and 0. **No engine read was located** anywhere in the particle module. |
| `flPhysicsParam0..6` (`0x31C..0x334`) | Seven floats following `snoPhysics` and `flMass`. Only their registered defaults are known; no engine read was traced to any individual member. |
| Emitter channels 9 and 10 | They are engine channels 33 and 30, but the assignment between the two slots is not resolved — see §8. |
| Emitter channels 11 and 12 | Retired in the registered revision, so only their storage (vector / float) is measurable. |
| Particle channels 4, 10, 11, 14, 15, 22, 23 | Their *unit* is known from the path type and their evaluation order is known, but the quantity each drives is not established. |
| Kinematic triples (particle 16–18 vs 19–21) | Two identical *(offset, velocity, acceleration)* sets; which is emitter-local and which is world is not established. |
| `eSystemType`, `nRenderMode` | Small integer enums; value sets and several engine branches known (§11.3, §11.4), individual values not named. |
| `dwPrtFlags` | Five bits traced to engine reads (§11.2); the rest unknown. The previous set-rate-derived table is withdrawn. |
| Triggered-event record | 412 bytes each, confirmed by `dwEventSize / dwEventCount` on 1,388 files. Internals undecoded; the registered revision's equivalent is 192 bytes, so it is not a shared layout. |
| `dwUnknown2C` (`0x02C`) | A registered field, not padding — but zero in 21,593 / 21,593 files. |

**No longer unknown**, for the record: the 104-byte block is `UberMaterial` and
the "160-byte gradient stop" is `MaterialTextureEntry` (§6); `eEmitterShape` has
nine values with known engine behaviour (§7); the tick rate is confirmed at 60 fps
by the `0.016667` conversions in the engine; and `nCollisionFlags` is
`nMaxInstances` (§5).

---

*Struct shapes from the Diablo III Nintendo Switch 2.6.2 build (`exefs/main`,
type registration at `0x71001AEAC0`). Offsets and enumerations verified against
21,593 `.prt` files. Field names are curated: retail builds compile them out.*
