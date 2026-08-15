# D3 Physics File Format Specification

**Format**: Diablo III Physics Descriptors (`.phy`, `.clt`, `.phm`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**Versions**: PHY v37 | CLT v51 | PHM v24
**Corpus**: 74 `.phy` + 74 `.clt` + 2,700 `.phm` = 2,848 files analyzed

See [README.md](README.md) for the build these offsets come from, the generator pipeline and
the conventions used below.

---

> ## Correction pass — 2026-08-16
>
> **There is no 32-byte "SNO preamble".** Every SNO file is a **16-byte header** followed by
> the struct image, and all stored offsets are struct-relative. This document previously
> assumed a 32-byte preamble, which displaced every named field in §2 and §3 by one 4-byte
> slot against the layout the binary actually registers.
>
> The *observations* were right — every value census below reproduces exactly against the
> corpus — but they were attached to the wrong fields. Sizes resolve as:
>
> | Format | File | = header | + struct (registered) |
> | --- | ---: | ---: | ---: |
> | `.phy` | 84 | 16 | **68** (`Physics`, group 28) |
> | `.clt` | 116 | 16 | **100** (`Cloth`, group 11) |
> | `.phm` | varies | 16 | **48** + payload (`PhysMesh`, group 61) |
>
> What changed:
>
> * **§2 (`.phy`) is corrected below.** `physicsSubType` → `nBodyClass`, and everything from
>   the old `restitution` onward shifts by one slot. The registered *types* also add
>   information the old pass could not have: the tail is four `DT_ACCEL` fields and one
>   `DT_VELOCITY`, not the guessed `buoyancy` / `windInfluence` / `breakForce` / `spinRate`.
> * **§3 (`.clt`) is superseded** by [CLT_FILE_FORMAT_SPECIFICATION.md](CLT_FILE_FORMAT_SPECIFICATION.md),
>   which carries the full corrected table. Its summary here is now correct but short.
> * **§4 (`.phm`)** — see [PHM_FILE_FORMAT_SPECIFICATION.md](PHM_FILE_FORMAT_SPECIFICATION.md),
>   which is the authority and was already re-derived.
> * **§6's Domino mapping is affected.** Its D3 columns named fields that have since changed
>   meaning, so the semantic pairings drawn from those names no longer follow. The D3 columns
>   are corrected and the now-unsupported rows are flagged in place.
>
> Verified over the whole corpus: `.phy` 74/74 at 84 bytes v37, `.clt` 74/74 at 116 bytes v51,
> `.phm` 2,700/2,700 v24 with 4,259 meshes at a uniform 112-byte element.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Physics Descriptor — `.phy`](#2-physics-descriptor--phy)
3. [Cloth Descriptor — `.clt`](#3-cloth-descriptor--clt)
4. [Physics Mesh — `.phm`](#4-physics-mesh--phm)
5. [M3 Physics System Cross-Reference](#5-m3-physics-system-cross-reference)
6. [Domino Engine Parameter Mapping](#6-domino-engine-parameter-mapping)
7. [Corpus Statistics](#7-corpus-statistics)
8. [Known Unknowns](#8-known-unknowns)
9. [Appendix A — Reading Physics Files (C++)](#appendix-a--reading-physics-files-c)
10. [Appendix B — All Structures Summary](#appendix-b--all-structures-summary)

---

## 1. Overview

Diablo III and the StarCraft II / Heroes of the Storm M3 engine both use the **Domino physics
engine** for rigid body simulation, cloth dynamics, and ragdoll articulation. In D3 these
capabilities are distributed across three SNO file types:

| Type | Extension | Version | Count | Size | Purpose |
|------|-----------|---------|------:|-----:|:--------|
| **Physics** | `.phy` | 37 | 74 | 84 B fixed | Rigid body / ragdoll parameter profiles |
| **Cloth** | `.clt` | 51 | 74 | 116 B fixed | Cloth simulation parameter profiles |
| **PhysMesh** | `.phm` | 24 | 2,700 | 64 B – 5.7 MB | Collision geometry (convex hulls, triangle meshes) |

The M3 engine stores equivalent information in embedded binary chunks within `.m3` containers:
PHRB (rigid bodies), PHSH (physics shapes), PHCL (cloth simulation), PHCC (cloth colliders),
PHAC (cloth proxies), PHYJ (physics joints / ragdoll), IKJT (IK joints), and DMSE
(destruction mesh entries).

**Key architectural difference**: D3 **externalizes** physics profiles as standalone SNO files
(74 presets shared across actors). M3 **embeds** per-bone records directly in the model file.

---

## 2. Physics Descriptor — `.phy`

**Tag**: PHY | **Version**: 37 (74/74) | **Size**: 84 bytes fixed = 16 header + 68 struct

### 2.1 File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO file header                            (16 bytes)  │
│    0x000: magic 0xDEADBEEF, u16 version 37              │
├─────────────────────────────────────────────────────────┤
│  Physics struct                             (68 bytes)  │
│    file 0x010 = struct +0x00                            │
└─────────────────────────────────────────────────────────┘
Total: 84 bytes — all 74 files
```

### 2.2 The Physics Struct (68 bytes)

Offsets below are **struct-relative**; file position = 16 + offset. Registered defaults are
noted where the binary supplies one.

```cpp
struct Physics {                                // 68 bytes (0x44), group 28
    u32     dwSnoId;            // +0x00: this asset's own SNO id; unique per file
    u32     _pad04;             // +0x04: 0
    u32     _pad08;             // +0x08: 0
    u32     dwFlags;            // +0x0C: 34 distinct values; 131092 most common (13)

    i32     nBodyClass;         // +0x10: 3 in 73/74, 2 in 1 file
    f32     flFriction;         // +0x14: [0.0, 3.35]; modes 0.3(24), 0.0(21). default 0.3
    f32     flMaterial2;        // +0x18: [0.1, 1.0]; 0.1 in 72/74. default 0.1
    f32     flRestitution;      // +0x1C: [0.0, 1.0]; 0.0 in 64/74
    f32     flLinearDamping;    // +0x20: [0.0, 6.0]; 0.0 in 55/74
    f32     flAngularDamping;   // +0x24: [0.0, 50.0]; modes 0.01(22), 0.1(22). default 0.01
    u16     wCollisionMask;     // +0x28: DT_WORD. default 0xFFBF (65471)
                                //        top values 0xFFE0(20), 0xFFBF(11), 0xFFFF(10), 0xFFFB(9)
    f32     flUnread2C;         // +0x2C: [0.0, 5.45]; 0.0 in 49/74
    f32     accUnread30;        // +0x30: DT_ACCEL  [−0.1389, 0.0178]; 0.0 in 46/74
    f32     accUnread34;        // +0x34: DT_ACCEL  [−0.1, 0.65];     0.0 in 54/74
    f32     velUnread38;        // +0x38: DT_VELOCITY [0.0, 5.0];     0.0 in 62/74
    f32     accUnread3C;        // +0x3C: DT_ACCEL  [−0.8, 0.0006];   0.0 in 58/74
    f32     accUnread40;        // +0x40: DT_ACCEL  [−0.1, 1.0];      0.0 in 50/74
};
```

### 2.3 What the registered types tell us

The five tail fields carry **physical dimensions in their registered types**, which is
information no byte-level pass could recover:

| Offset | Registered type | Meaning of the unit |
| --- | --- | --- |
| +0x30, +0x34, +0x3C, +0x40 | `DT_ACCEL` | units per (1/60 s)² |
| +0x38 | `DT_VELOCITY` | units per 1/60 s |

So the tail is **four accelerations and one velocity**, not the previously guessed
`buoyancy` / `windInfluence` / `breakForce` / `angularVelocityDamp` / `spinRate`. Their
names are left as `accUnreadNN` / `velUnread38` deliberately: the dimension is established,
the role is not, and no engine function reading them has been located.

`wCollisionMask` is a **16-bit** field (`DT_WORD`), not the u32 the old text described. Its
default `0xFFBF` clears exactly one bit, and the observed values (`0xFFE0`, `0xFFBF`, `0xFFFF`,
`0xFFFB`) are all near-full masks with a few layers cleared — consistent with a collision
filter, as the filename evidence in §2.4 suggests.

### 2.4 Field identification rationale

| D3 field | Evidence |
|----------|:--------|
| `nBodyClass` (+0x10) | small enum, 3 in 73/74 and 2 in one. Registered `DT_INT` with no default |
| `flFriction` (+0x14) | registered default 0.3 matches the corpus mode (24/74). Range [0, 3.35] |
| `flMaterial2` (+0x18) | registered default 0.1 matches the mode (72/74). Named non-committally: it sits where a second material coefficient would, but nothing reads it |
| `flAngularDamping` (+0x24) | registered default 0.01 matches one of the two modes (22/74 each at 0.01 and 0.1) |
| `wCollisionMask` (+0x28) | registered default `0xFFBF`, which appears in 11 files. Filename evidence: `Ragdoll_Base_Collision.phy` vs `RagdollBreakable_Non_Scene_Col.phy` |

**Withdrawn:** the previous document's `restitution`/`density`/`gravityScale`/`buoyancy`
identifications rested on matching D3 distributions to M3 PHRB fields *under the displaced
offsets*. With the offsets corrected those pairings no longer line up, and none of them is
re-asserted here.

### 2.5 Filename Categories

| Category | Count | Examples |
|----------|------:|:--------|
| Ragdoll variants | ~35 | `Ragdoll.phy`, `RagdollBreakable.phy`, `RagdollHanging.phy`, `RagdollSelfCollide.phy` |
| Particle / weather | ~10 | `Rain.phy`, `Snow.phy`, `Fire.phy`, `Smoke.phy`, `Embers.phy` |
| Environmental | ~8 | `a1dun_Leor_bloodPitFlies.phy`, `a4dunGarden_Props_Barrel_Float.phy` |
| Creature death | ~5 | `assaultBeast_death.phy`, `Ballista_Death_Float.phy` |
| Character-specific | ~5 | `Ragdoll_Crusader_deflection.phy`, `p6_necro_simulacrum_veinPhysics.phy` |
| Expansion (x1_) | ~7 | `x1_Ragdoll_Flail.phy`, `x1_Fortress_Crate_Float.phy` |
| Debug | 1 | `Axe Bad Data.phy` (test file) |

Ragdoll profiles are **presets** referenced by actor SNOs — the same profile is shared across
many actors (e.g. `Ragdoll.phy` is the default for most humanoid enemies).

---

## 3. Cloth Descriptor — `.clt`

**Tag**: CLT | **Version**: 51 (74/74) | **Size**: 116 bytes fixed = 16 header + 100 struct

> **[CLT_FILE_FORMAT_SPECIFICATION.md](CLT_FILE_FORMAT_SPECIFICATION.md) is the authority for
> this format.** It carries the full field table, value censuses, the corrected asset
> cross-reference and the old→new name mapping. What follows is a summary only.

### 3.1 File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO file header                            (16 bytes)  │
├─────────────────────────────────────────────────────────┤
│  Cloth struct                              (100 bytes)  │
│    file 0x010 = struct +0x00                            │
└─────────────────────────────────────────────────────────┘
Total: 116 bytes — all 74 files, zero trailing bytes
```

### 3.2 The Cloth Struct (100 bytes)

```cpp
struct Cloth {                                  // 100 bytes (0x64), group 11
    u32   dwSnoId;              // +0x00
    u32   _pad04, _pad08;       // +0x04, +0x08: 0
    u32   dwFlags;              // +0x0C: {0, 4, 6, 12}
    i32   dwRelaxIterations;    // +0x10: default 25 (71/74)
    f32   flMass;               // +0x14: [0.2, 45.0], default 0.5
    f32   flSkinBlendRate;      // +0x18: [0.0, 1.0]
    f32   flStretchStiffness0;  // +0x1C: default 1.0 (46/74)
    f32   flStretchStiffness1;  // +0x20: default 0.5
    f32   flBendStiffness;      // +0x24: default 0.1
    f32   flExternalForceScale; // +0x28: default 1.0
    f32   flDragCoefficient;    // +0x2C: default 1.0
    f32   flGravity;            // +0x30: DT_ACCEL, per (1/60 s)²
    f32   flRootStiffness;      // +0x34: default 1.5 (32/74)
    f32   flLinearDamping;      // +0x38: default 0.1
    f32   flContactDamping;     // +0x3C: default 0.1 (57/74)
    i32   nCollisionPlane[4];   // +0x40: only (0,0,0,0), (1,0,0,0), (1,2,0,0) occur
    i32   snoAmbientSound;      // +0x50: DT_SNO group 5; −1 in 74/74
    i32   nUseCustomWind;       // +0x54: 1 in exactly one file
    f32   vWindVelocity[3];     // +0x58: non-zero only in that same file
};
```

The previously documented `density` / `stretchStiffness` / `damping` / `gravity` / `tracking`
/ `windScale` / `dragFactor` / `liftFactor` / `flatten` / `materialSnoId` names were all
displaced by one slot and are withdrawn; see the CLT document's correction table.

### 3.3 Field Identification Rationale

Cloth fields map directly to Domino simulation parameters, confirmed by statistical correlation
with M3 PHCL:

| D3 .clt Field | M3 PHCL Field | D3 Mean | M3 Mean | Notes |
|:---------------|:--------------|--------:|--------:|:------|
| `density` | `density` | 2.53 | 3.82 | Same range, same role |
| `stretchStiffness` | `stretchStiffness` | 0.39 | 0.39 | **Exact mean match** |
| `horizontalStiffness` | `horizontalStiffness` | 0.87 | 0.34 | Same role; D3 defaults higher |
| `bendingStiffness` | `bendingStiffness` | 0.64 | 0.28 | Same [0–1] domain |
| `shearStiffness` | `shearStiffness` | 0.46 | 0.28 | Same [0–1] domain |
| `damping` | `damping` | 0.78 | 2.05 | Same extended range |
| `gravity` | `gravity` | 2.18 | 0.77 | D3 uses higher gravity for heavier drape |
| `windScale` | `windScale` | 1.79 | 0.58 | Same role |
| `dragFactor` | `dragFactor` | 1.12 | 0.62 | Aerodynamic drag coefficient |
| `liftFactor` | `liftFactor` | 0.34 | 0.60 | Aerodynamic lift coefficient |
| `flatten` | `flatten` | flag | flag | Boolean; 16/74 D3 vs 15/378 M3 |

**Not present in D3 .clt** (present in M3 PHCL): `explosionScale`, `sphereStiffness`, `friction`,
`useSkinCollision`, `skinOffset`, `skinExponent`, `skinStiffness`, `localChannels`. M3 carries
additional fields for skin-collision proxies and animation channel bindings that D3 handles at
the actor pipeline level.

**D3-only fields**: `materialSnoId` (0x060, always −1), `collisionMode` (0x054),
`localWindY`/`localWindZ` (0x068/0x070).

### 3.4 Filename Categories

| Category | Count | Examples |
|----------|------:|:--------|
| Generic presets | ~12 | `g_Cloth_Skirt_Heavy.clt`, `g_Cloth_Ribbon_Float.clt`, `g_Cloth_Helm_Stiff.clt` |
| Environment | ~16 | `Flag.clt`, `Rope.clt`, `Tent.clt`, `Carpet.clt`, `caOut_Awning_cloth_a.clt` |
| Character | ~10 | `Barbarian_Female_Skirt.clt`, `DemonHunter_Female_cloth.clt` |
| Dungeon-specific | ~10 | `trDun_Cath_Banners.clt`, `a2dun_Spider_Web.clt` |
| NPC / boss | ~8 | `Adria_Cloth.clt`, `SkeletonKing.clt`, `Tyrael_Cloth.clt` |
| Caldeum outdoor | ~6 | `caOut_CaldExt_Cloth_A.clt`, `caOut_Rope_Windy.clt` |
| Creature | ~4 | `assaultBeast_gut_cloth.clt`, `DuneDervish_cloth.clt` |

### 3.5 Sample Records

**Adria_Cloth.clt** — NPC cloth (light, responsive):

```cpp
// density=0.60  stretchStiffness=0.98  horizontalStiffness=0.80
// bendingStiffness=0.30  shearStiffness=0.30  damping=0.00
// gravity=0.10  tracking=-0.006  windScale=1.50
// dragFactor=0.50  liftFactor=0.20  flatten=0  collisionMode=0
```

**Carpet.clt** — Heavy environmental cloth:

```cpp
// density=45.00  stretchStiffness=1.00  horizontalStiffness=1.00
// bendingStiffness=1.00  shearStiffness=1.00  damping=0.20
// gravity=0.50  tracking=0.00  windScale=1.00
// dragFactor=2.00  liftFactor=10.00  flatten=0  collisionMode=0
```

**g_Cloth_Skirt_Heavy.clt** — Generic preset (heavy skirt):

```cpp
// density=2.00  stretchStiffness=0.00  horizontalStiffness=0.90
// bendingStiffness=0.50  shearStiffness=0.10  damping=0.00
// gravity=1.00  tracking=-0.003  windScale=1.50
// dragFactor=0.10  liftFactor=0.10  flatten=0  collisionMode=0
```

---

## 4. Physics Mesh — `.phm`

**Tag**: PHM | **Version**: 24 | **Size**: 64 B – 5.7 MB (variable)

### 4.1 File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO Preamble                               (32 bytes)  │
│    0x000: magic, version, snoId, reserved               │
├─────────────────────────────────────────────────────────┤
│  PhysMesh Header                            (32 bytes)  │
│    0x020: mesh count, offsets, bounding volume           │
├─────────────────────────────────────────────────────────┤
│  Mesh Descriptor Table                     (variable)   │
│    0x040+: per-mesh collision primitives                │
├─────────────────────────────────────────────────────────┤
│  Collision Geometry Data                   (variable)   │
│    convex hulls, triangle meshes                        │
└─────────────────────────────────────────────────────────┘
```

### 4.2 PhysMesh Header

```cpp
struct PhysMeshHeader {                         // 32+ bytes at 0x020
    u32     meshCount;          // 0x020: Number of collision sub-meshes (0–N)
    u32     meshDataOffset;     // 0x024: Offset to first mesh descriptor (48 when present)
    u32     meshDataEnd;        // 0x028: End offset of mesh descriptor table
    u32     _reserved02C;       // 0x02C: Always 0
    u32     _reserved030;       // 0x030: Always 0
    u32     _reserved034;       // 0x034: Always 0
    f32     totalVolume;        // 0x038: Bounding volume metric
                                //        Mean=5,583, varies widely
    u32     _reserved03C;       // 0x03C: Always 0
    u8      _reserved040[12];   // 0x040: Always 0
    u32     _reserved04C;       // 0x04C: Always 0
    u8      _reserved050[8];    // 0x050: Always 0
    f32     boundsX;            // 0x058: Bounding box X extent
                                //        Mean≈97, range [−9, 742]
    f32     boundsY;            // 0x05C: Bounding box Y extent
                                //        Mean≈96, range [−5, 786]
};
```

### 4.3 Mesh Data

Each sub-mesh is a collision primitive (convex hull or triangle mesh) for a scene tile or prop.
Files with `meshCount=0` are 64-byte stubs (header only, no geometry). Large files (>1 MB)
contain detailed triangle-mesh collision for complex level tiles.

### 4.4 Size Distribution

| Bucket | Count | Description |
|--------|------:|:------------|
| < 100 bytes | 22 | Empty stubs (no collision) |
| 100 B – 1 KB | 111 | Minimal collision (single box or capsule) |
| 1 – 10 KB | 289 | Simple convex hulls |
| 10 – 100 KB | 1,295 | Moderate geometry (single-room tiles) |
| 100 KB – 1 MB | 969 | Complex level tiles |
| > 1 MB | 14 | Large multi-section dungeon tiles |

### 4.5 Naming Convention

PhysMesh files are named after the **scene tile** or **prop** they provide collision for:

- Level tiles: `a1dun_Leor_EW_01.phm`, `a2dun_Swr_NSE_01.phm`
- Props: `trOut_OldTristram_Cart_Burnt.phm`, `a1dun_Caves_RopeBridge_b_Destructable.phm`
- Event areas: `1000MonsterFight_E01_S02.phm`, `x1_Pand_Ext_240_NSW_Event_Sandrock_Ambush.phm`

---

## 5. M3 Physics System Cross-Reference

### 5.1 Chunk Census (50,742 M3 files)

| Chunk | Tag | Version(s) | Files | Entries | Purpose |
|:------|:----|:-----------|------:|--------:|:--------|
| Rigid Body | PHRB | v2, v3, **v4** | 4,036 | ~24,000 | Per-bone rigid body parameters |
| Physics Shape | PHSH | v1, v2, **v3** | 24,810 | 24,814 | Collision shape geometry |
| Cloth | PHCL | v2, **v4** | 382 | 382 | Cloth simulation parameters |
| Cloth Collider | PHCC | v0 | 375 | ~2,100 | Capsule colliders for cloth |
| Cloth Proxy | PHAC | v0 | 216 | 216 | Cloth-to-bone proxy bindings |
| Physics Joint | PHYJ | v0 | 1,081 | 12,796 | Ragdoll joint constraints |
| IK Joint | IKJT | v0 | 37 | 148 | Inverse kinematics (SC2 only) |
| Destruction Mesh | DMSE | v0 | 4,366 | ~202,000 | Destruction mesh face entries |

**Prevalence**: HotS > SC2 for cloth (363 vs 19 files) and joints (767 vs 314). SC2 models
more commonly use rigid bodies without ragdoll. IKJT is SC2-exclusive (37 files, always 4
joints per model — likely quadruped ground-contact IK for Zerg units).

### 5.2 MODL Reference Graph

```
MODL
├── rigidBodies       → PHRB[]  (all versions)
│   └── rigidBodyShape  → PHSH   (per rigid body)
├── physicsJoints     → PHYJ[]  (MODL v24+)
├── clothPhysics      → PHCL[]  (all versions)
│   ├── colliders       → PHCC[]  (capsule colliders)
│   └── proxies         → PHAC[]  (bone proxies)
│       ├── proxyVertices → U64_
│       └── proxyWeights  → U32_
├── clothPhysicsCopy  → PHCL[]  (MODL v28+, always NULL)
└── ikJoints          → IKJT[]  (all versions)
```

### 5.3 PHRB — Rigid Body (v4, 80 bytes)

```cpp
struct PHRB_v4 {                                // 80 bytes
    u16     simulationType;     // 0x00: 1(98.6%), 2(1.4%)
    u16     parentBoneIndex;    // 0x02: Per-bone index
    u32     physicsType;        // 0x04: 4=34%, 0=30%, 3=10%, 18=6%
    f32     density;            // 0x08: Mean=2038, P5=400, P95=4000 [kg/m³]
    f32     friction;           // 0x0C: Mean=0.93, mode 0.5(51%), 0.7(31%)
    f32     restitution;        // 0x10: Mean=0.11, mode 0.1(79%)
    f32     linearDamping;      // 0x14: Mean=0.06, mode 0.01(80%)
    f32     angularDamping;     // 0x18: Mean=0.06, mode 0.01(80%)
    f32     gravityScale;       // 0x1C: Mean=0.99, mode 1.0(98%)
    u8      dynamicState[16];   // 0x20: AnimRef — animated u32
    f32     dynamicBlendOut;    // 0x30: Blend-out duration
    u8      rigidBodyShape[12]; // 0x34: Ref → PHSH
    u32     flags;              // 0x40: 0(84.2%), sparse bit patterns
    u32     localChannels;      // 0x44: 33=33%, 97=27%, 37=20%
    u32     priority;           // 0x48: 131072(94.6%), 0(4.9%)
    u32     _padding;           // 0x4C: Alignment padding
};
```

### 5.4 PHYJ — Physics Joint (v0, 180 bytes)

Each PHYJ entry defines a **Domino constraint** between two bones for ragdoll articulation.

```cpp
struct PHYJ_v0 {                                // 180 bytes
    u32     jointType;          // 0x00: 2=ball-socket(74%), 1=hinge(23%), 3=twist(3%)
    u32     boneIndex1;         // 0x04: First body
    u32     boneIndex2;         // 0x08: Second body
    f32     matrixBody1[16];    // 0x0C: Joint frame body 1 (4×4 matrix, 64 bytes)
    f32     matrixBody2[16];    // 0x4C: Joint frame body 2 (4×4 matrix, 64 bytes)
    u32     enableLimits;       // 0x8C: 1=88%; 32513(0x7F01)=12% — packed byte
    f32     limitMin;           // 0x90: Mean=−0.65, mode −π/4(48%), radians
    f32     limitMax;           // 0x94: Mean=0.83, mode +π/4(48%), radians
    f32     coneAngle;          // 0x98: Mean=0.71, mode π/4(72%), radians
    u32     enableFriction;     // 0x9C: 1=71%, 32513(0x7F01)=28% — packed byte
    f32     friction;           // 0xA0: Mean=0.22, mode 0.2(89%)
    f32     dampingRatio;       // 0xA4: Mean=0.71, mode 0.7(98%)
    f32     angularFrequency;   // 0xA8: Mean=5.03, mode 5.0(97%) [Hz]
    f32     breakThreshold;     // 0xAC: Always 1.0 (100%)
    u8      enableShape;        // 0xB0: 0=99.9%, 1=0.1%
    u8      _padding[3];        // 0xB1: Alignment padding
};
```

**Joint type mapping**: Type 2 (ball-and-socket, 74%) dominates for spine/shoulder/hip joints.
Type 1 (hinge, 23%) for elbows and knees. Type 3 (3%) may be twist-only.

The `enableLimits` field shows anomalous values (32513 = 0x7F01) — likely a packed byte where
0x01 = enabled and the high byte carries additional flags.

**Consistent Domino defaults**: dampingRatio=0.7 and angularFrequency=5.0 Hz together define
the spring-damper response. breakThreshold=1.0 everywhere means joints never break during gameplay.

Ragdoll complexity: Mean=11.8 joints, median=11, range [1, 40]. Humanoid skeletons typically
use 15–21 joints.

### 5.5 PHCL — Cloth Physics (v4, 192 bytes)

```cpp
struct PHCL_v4 {                                // 192 bytes
    // ─── Mesh References ───────────────────────────────────────────────────────
    u32     clothMeshCount;     // 0x00: 0=50%, 3=14%, 1=11%, 2=9%
    u32     skinBoneCount;      // 0x04: 0=43%, typical 3–20
    u8      skinBones[12];      // 0x08: Ref → U16_ (bone indices)
    u8      simEnabled[12];     // 0x14: Ref → U8__ (per-vertex enable)
    u8      vertexBones[12];    // 0x20: Ref → U32_ (bone mapping)
    u8      vertexWeights[12];  // 0x2C: Ref → U32_ (bone weights)
    u8      colliders[12];      // 0x38: Ref → PHCC (capsule colliders)
    u8      proxies[12];        // 0x44: Ref → PHAC (bone proxies)

    // ─── Cloth Simulation Parameters ───────────────────────────────────────────
    f32     density;            // 0x50: Mean=3.82, range [0, 60]
    f32     tracking;           // 0x54: Mean=0.24, range [0, 1]
    f32     stretchStiffness;   // 0x58: Mean=0.39, range [0, 1]
    f32     horizontalStiffness;// 0x5C: Mean=0.34, range [0, 1]
    f32     bendingStiffness;   // 0x60: Mean=0.28, range [0, 1]
    f32     damping;            // 0x64: Mean=2.05, range [0, 15]
    f32     friction;           // 0x68: Mean=0.15, range [0, 1.5]
    f32     gravity;            // 0x6C: Mean=0.77, range [0, 6]
    f32     explosionScale;     // 0x70: Mean=0.55, range [0, 1]
    f32     windScale;          // 0x74: Mean=0.58, range [0, 5]
    f32     shearStiffness;     // 0x78: Mean=0.28, range [0, 1.5]
    f32     dragFactor;         // 0x7C: Mean=0.62, range [0, 5]
    f32     liftFactor;         // 0x80: Mean=0.60, range [0, 7]
    f32     sphereStiffness;    // 0x84: Mean=0.32, range [0, 3]
    u32     flatten;            // 0x88: 0=96%, 1=4%
    u8      active[16];         // 0x8C: AnimRef — animated u32
    u32     useSkinCollision;   // 0x9C: 0xFFFFFFFF=99.2% (disabled)
    f32     skinOffset;         // 0xA0: Mostly 0
    f32     skinExponent;       // 0xA4: Mean=0.48, bimodal: 1.0 or 0.0
    f32     skinStiffness;      // 0xA8: Mean=0.48, bimodal: 1.0 or 0.0
    u32     localChannels;      // 0xAC: Channel binding
    f32     localWind[3];       // 0xB0: Local wind vector (XYZ)
};
```

**Bimodal distribution**: Many fields show bimodal patterns (~164 entries at 0.0, ~200 non-zero).
The 164 zero-entries correspond to `clothMeshCount=0` (dummy entries with no active cloth mesh).

### 5.6 PHSH — Physics Shape

Physics shapes define collision geometry for rigid bodies.

| Version | Size | Entries | Description |
|---------|-----:|--------:|:------------|
| v1 | 132 B | 770 | Basic: transform + shape type + dimensions |
| v2 | 292 B | 115 | +convex hull vertices, face normals, indices |
| v3 | 300 B | 23,925 | +mesh collision data (vertices, faces, normals) |

v3 is dominant (96.4%). Shape structure:

```cpp
struct PHSH_v3 {                                // 300 bytes
    f32     transform[16];      // 0x00: Local-space 4×4 matrix (64 bytes)
    u8      shapeType;          // 0x40: 0=box, 1=sphere, 2=capsule,
                                //       3=cylinder, 4=convex hull, 5=mesh
    u8      _pad41[3];          // 0x41: Alignment
    f32     sizeX;              // 0x44: Shape dimension X
    f32     sizeY;              // 0x48: Shape dimension Y
    f32     sizeZ;              // 0x4C: Shape dimension Z
    // ... v2+: convex hull data; v3: triangle mesh data
};
```

### 5.7 Supporting Chunks

**PHCC — Cloth Collider** (v0, 76 bytes): Capsule colliders attached to bones for cloth
interpenetration prevention. Mean 5.6 colliders per cloth model (range 1–17). Fields:
transform (4×4 matrix), radius, height.

**PHAC — Cloth Proxy** (v0, 32 bytes): Proxy bindings mapping cloth vertices to skeletal bones.
Always 1 proxy per PHAC chunk. Fields: proxyIndex, clothIndex, vertex/weight references.

**IKJT — IK Joint** (v0, 32 bytes): Inverse kinematics joints, SC2-exclusive (37 models, always
4 joints). Fields: 12B unknown, boneIndex1/2, raycastUp/Down, maxSpeed, goalThreshold. Likely
ground-contact IK for Zerg quadrupeds.

**DMSE — Destruction Mesh Entry** (v0, 4 bytes each): Per-face destruction state for breakable
meshes. 4,366 models (8.6% of corpus). Mean 46 entries per model. Each entry is a single u32.

---

## 6. Domino Engine Parameter Mapping

Both D3 and M3 feed the same **Domino physics engine** developed by Vicarious Visions for
Activision Blizzard titles. The parameter sets are architecturally identical but differ in
encoding strategy.

### 6.1 Rigid Body: D3 `.phy` ↔ M3 PHRB

> **Read this table with care.** Its D3 column originally used file offsets under the
> 32-byte-preamble assumption; those are corrected to struct offsets below. The *distribution*
> comparisons still stand — they were computed from bytes — but a pairing is only meaningful
> where the D3 field's identity survived the correction. Rows whose D3 side changed meaning
> are marked ⚠ and should be treated as unsupported until re-derived.

| Domino Parameter | D3 field (struct off) | D3 Range | M3 Location | M3 Range | Notes |
|:-----------------|:----------------------|:---------|:------------|:---------|:------|
| Friction | `flFriction` +0x14 | [0, 3.35] | PHRB 0x0C | [0, 100] | ✔ D3 name and slot both survive; same coefficient |
| Restitution | `flMaterial2` +0x18 | [0.1, 1.0] | PHRB 0x10 | [0, 10] | ⚠ distributions still match (mean 0.11 both) but the D3 field is no longer identified as restitution |
| Linear damping | `flRestitution` +0x1C | [0, 1.0] | PHRB 0x14 | [0, 2.0] | ⚠ zero-heavy in both; D3 identity changed |
| Angular damping | `flLinearDamping` +0x20 | [0, 6.0] | PHRB 0x18 | [0, 2.0] | ⚠ D3 identity changed |
| Density | `flAngularDamping` +0x24 | [0, 50] | PHRB 0x08 | [10, 10000] | ⚠ D3 identity changed |
| Collision layers | `wCollisionMask` +0x28 | u16 bitmask | PHRB 0x40 | sparse u32 | ✔ same concept; D3 is 16-bit, default 0xFFBF |
| Gravity scale | `flUnread2C` +0x2C | [0, 5.45] | PHRB 0x1C | [0.25, 2.0] | ⚠ D3 field is unnamed; 49/74 are 0.0 |

**Key difference**: D3 externalizes physics profiles as standalone SNO files (74 presets shared
across actors). M3 embeds per-bone PHRB entries directly inside the model file, producing
5,005 individual rigid body records — each bone gets its own density, friction, etc.

### 6.2 Cloth Simulation: D3 `.clt` ↔ M3 PHCL

> **This mapping was built entirely on the displaced D3 names and does not survive the
> correction.** With the real `Cloth` layout the D3 side reads mass → skin-blend → two stretch
> axes → bend → external-force → drag → gravity → root → linear damping → contact damping,
> which is a different parameter set from the one tabulated before. The D3 column is corrected
> below; the M3 column is unchanged and unverified against it.

| D3 field (struct off) | D3 Range | Plausible M3 PHCL counterpart | Status |
|:----------------------|:---------|:------------------------------|:-------|
| `flMass` +0x14 | [0.2, 45] | 0x50 density | ✔ aligned ranges, and both are the per-particle mass term |
| `flSkinBlendRate` +0x18 | [0, 1] | 0x54 tracking | plausible — both blend toward the skinned pose |
| `flStretchStiffness0` +0x1C | [0.1, 1] | 0x58 stretch | plausible |
| `flStretchStiffness1` +0x20 | [0, 1] | 0x5C horizontal | plausible — the two D3 axes vs M3's stretch/horizontal pair |
| `flBendStiffness` +0x24 | [0, 1] | 0x60 bending | ✔ same name, same domain |
| `flExternalForceScale` +0x28 | [0, 25] | 0x74 windScale | unverified |
| `flDragCoefficient` +0x2C | [0, 30] | 0x7C drag | plausible |
| `flGravity` +0x30 | [−0.018, 0.28] | 0x6C gravity | ✔ **the scale difference is now explained**: D3 stores acceleration per (1/60 s)², M3 per second² |
| `flRootStiffness` +0x34 | [1, 20] | — | no obvious counterpart |
| `flLinearDamping` +0x38 | [0, 10] | 0x64 damping | plausible |
| `flContactDamping` +0x3C | [0, 10] | — | no obvious counterpart |

The old table's "very different scale" note on tracking/gravity was the strongest hint that
something was misaligned: D3's `flGravity` is a **per-tick²** acceleration, so a factor of
3600 against a per-second² figure is expected, not anomalous.

**D3-only fields**: `nCollisionPlane[4]` (+0x40), `snoAmbientSound` (+0x50, always −1),
`nUseCustomWind` + `vWindVelocity` (+0x54/+0x58).

### 6.3 Collision Geometry: D3 `.phm` ↔ M3 PHSH

| Aspect | D3 `.phm` | M3 PHSH |
|:-------|:----------|:--------|
| Container | Standalone SNO file | Embedded chunk in .m3 |
| Scope | Per-scene-tile / per-prop | Per-rigid-body |
| Version | 24 | v1=132B, v2=292B, v3=300B |
| Size range | 64 B – 5.7 MB | Fixed per-version |
| Count | 2,700 files | 24,810 entries |
| Geometry types | Convex hull, triangle mesh | Box, sphere, capsule, cylinder, convex hull, mesh |

D3 PhysMesh files contain **world-space collision volumes** for level tiles and props — large
file sizes (mean 110 KB) reflect detailed triangle-mesh collision for dungeon geometry. M3 PHSH
entries are **local-space shape primitives** (64-byte transform + dimensions) per bone.

### 6.4 Ragdoll Architecture

| Aspect | D3 | M3 |
|:-------|:---|:---|
| Profile storage | External `.phy` SNO preset | Per-bone PHRB embedded in model |
| Joint definition | Actor/skeleton level | PHYJ chunk — 180B per joint |
| Joint count | N/A (actor-level) | Mean=12, range [1, 40] |
| Joint types | Domino (ball-socket, hinge) | 2=ball-socket(74%), 1=hinge(23%), 3=twist(3%) |
| Shape data | `.phm` files for level collision | PHSH per rigid body |

D3 decouples physics profiles from joint topology: a single `Ragdoll.phy` provides material
parameters while the actor SNO defines the joint graph. M3 couples everything: PHRB (material),
PHYJ (joints), PHSH (shapes) co-located in the same .m3 file.

---

## 7. Corpus Statistics

### 7.1 Physics Prevalence

**D3 Corpus**: 74 physics profiles + 74 cloth profiles + 2,700 collision meshes (shared presets).

**M3 Corpus** (50,742 files):

| Feature | HotS | SC2 | Total | % |
|:--------|-----:|----:|------:|--:|
| Any physics | 1,689 | 2,363 | 4,052 | 8.0% |
| Rigid bodies (PHRB) | 1,682 | 2,354 | 4,036 | 8.0% |
| Cloth (PHCL) | 363 | 19 | 382 | 0.8% |
| Joints (PHYJ) | 767 | 314 | 1,081 | 2.1% |
| IK (IKJT) | 0 | 37 | 37 | 0.07% |
| Destruction (DMSE) | 2,001 | 2,365 | 4,366 | 8.6% |

### 7.2 PHRB physicsType Enumeration

| Value | Count | % | Interpretation |
|------:|------:|--:|:---------------|
| 4 | 1,700 | 34.0% | Default rigid body |
| 0 | 1,507 | 30.1% | Static/kinematic |
| 3 | 479 | 9.6% | Dynamic (medium mass) |
| 18 | 274 | 5.5% | Debris/prop |
| 14 | 200 | 4.0% | Character bone |
| 20 | 159 | 3.2% | Projectile |
| 2 | 151 | 3.0% | Dynamic (light) |
| 6 | 125 | 2.5% | Vehicle/mount |
| 5 | 100 | 2.0% | Weapon |
| 19 | 94 | 1.9% | Environmental |

### 7.3 PHSH Version Distribution

| Version | Entries | Size | % | Era |
|---------|--------:|-----:|--:|:----|
| v3 | 23,925 | 300 B | 96.4% | Current (HotS / SC2 LotV) |
| v1 | 770 | 132 B | 3.1% | Legacy (WoL-era) |
| v2 | 115 | 292 B | 0.5% | Transitional (HotS-era) |

### 7.4 Cloth Collider Counts

PHCC entries per PHCL model (n=375): Mode range 4–7 colliders (humanoid capes/skirts),
median 5, max 17 (Tyrael's wings).

---

## 8. Known Unknowns

| Area | Details |
|:-----|:--------|
| **PHY `nBodyClass` (+0x10)** | 73/74 files = 3, 1 file = 2. Registered `DT_INT`; a small enum, not decoded. No engine read located. |
| **PHY `dwFlags` (+0x0C)** | 34 distinct values, most common 131092 (13/74). Bit meanings unknown. |
| **PHY tail +0x2C…+0x40** | Sparse and mostly zero. The registered **types** are established — four `DT_ACCEL` and one `DT_VELOCITY` — so the dimensions are known and the roles are not. Names deliberately left as `accUnreadNN` / `velUnread38`. |
| **PHY `flMaterial2` (+0x18)** | registered default 0.1, matches the mode 72/74. Sits where a second material coefficient would; nothing reads it, so the name is provisional. |
| **CLT `dwFlags` / `nCollisionPlane`** | see [CLT_FILE_FORMAT_SPECIFICATION.md](CLT_FILE_FORMAT_SPECIFICATION.md) §5. |
| **CLT ↔ M3 PHCL pairing** | §6.2's mapping was built on displaced names and is now only partly supported. The gravity scale difference **is** resolved: D3 stores per-(1/60 s)², M3 per second². |
| **PHM mesh format** | Internal vertex/face structure not fully parsed beyond header. |
| **PHSH collision data** | Convex hull vertices and mesh faces in v2/v3 — not extracted. |
| **PHRB physicsType** | Enumeration inferred from frequency patterns, not confirmed by engine symbols. |
| **PHYJ packed bytes** | `enableLimits`/`enableFriction` contain 0x7F01 — undocumented bit packing. |
| **IKJT semantics** | 12-byte unknown prefix; only 37 files — insufficient data for confident identification. |

---

## Appendix A — Reading Physics Files (C++)

```cpp
#include <cstdio>
#include <cstdint>
#include <cassert>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using f32 = float;

// ── §2  Read a .phy file ──────────────────────────────────────────────────────
void readPhy(const char* path) {
    FILE* f = fopen(path, "rb");

    SnoPreamble preamble;
    fread(&preamble, sizeof(SnoPreamble), 1, f);
    assert(preamble.magic == 0xDEADBEEF);
    assert(preamble.version == 37);

    PhysicsPayload phy;
    fread(&phy, sizeof(PhysicsPayload), 1, f);

    printf("PHY: friction=%.2f restitution=%.2f density=%.2f gravity=%.2f\n",
           phy.friction, phy.restitution, phy.density, phy.gravityScale);
    printf("     damping: linear=%.2f angular=%.2f flags=0x%04X\n",
           phy.linearDamping, phy.angularDamping, phy.flags);

    fclose(f);
}

// ── §3  Read a .clt file ──────────────────────────────────────────────────────
void readClt(const char* path) {
    FILE* f = fopen(path, "rb");

    SnoPreamble preamble;
    fread(&preamble, sizeof(SnoPreamble), 1, f);
    assert(preamble.magic == 0xDEADBEEF);
    assert(preamble.version == 51);

    ClothPayload clt;
    fread(&clt, sizeof(ClothPayload), 1, f);

    printf("CLT: density=%.2f stretch=%.2f bend=%.2f shear=%.2f\n",
           clt.density, clt.stretchStiffness, clt.bendingStiffness, clt.shearStiffness);
    printf("     damping=%.2f gravity=%.2f wind=%.2f drag=%.2f lift=%.2f\n",
           clt.damping, clt.gravity, clt.windScale, clt.dragFactor, clt.liftFactor);

    fclose(f);
}

// ── §4  Read a .phm file header ───────────────────────────────────────────────
void readPhm(const char* path) {
    FILE* f = fopen(path, "rb");

    SnoPreamble preamble;
    fread(&preamble, sizeof(SnoPreamble), 1, f);
    assert(preamble.magic == 0xDEADBEEF);
    assert(preamble.version == 24);

    PhysMeshHeader phm;
    fread(&phm, sizeof(PhysMeshHeader), 1, f);

    printf("PHM: meshCount=%u volume=%.1f bounds=(%.1f, %.1f)\n",
           phm.meshCount, phm.totalVolume, phm.boundsX, phm.boundsY);

    if (phm.meshCount > 0) {
        printf("     meshData: offset=%u end=%u\n",
               phm.meshDataOffset, phm.meshDataEnd);
    }

    fclose(f);
}
```

---

## Appendix B — All Structures Summary

```cpp
// §2 — SNO Preamble (shared by PHY, CLT, PHM)
struct SnoPreamble {                            // 32 bytes
    u32 magic; u32 version; u32 snoGroupId; u32 snoId;
    u32 _res010; u32 _res014; u32 _res018; u32 _res01C;
};

// §2 — Physics Payload (.phy)
struct PhysicsPayload {                         // 52 bytes
    u32 physicsSubType;
    f32 friction; f32 restitution; f32 linearDamping; f32 angularDamping; f32 density;
    u32 flags; f32 gravityScale; f32 buoyancy; f32 windInfluence;
    f32 breakForce; f32 angularVelocityDamp; f32 spinRate;
};

// §3 — Cloth Payload (.clt)
struct ClothPayload {                           // 84 bytes
    u32 clothSubType;
    f32 density; f32 stretchStiffness; f32 horizontalStiffness;
    f32 bendingStiffness; f32 shearStiffness;
    f32 damping; f32 gravity; f32 tracking; f32 windScale;
    f32 dragFactor; f32 liftFactor;
    u32 flatten; u32 collisionMode; u32 _res058; u32 _res05C;
    u32 materialSnoId; u32 extraFlags; f32 localWindY; u32 _res06C; f32 localWindZ;
};

// §4 — PhysMesh Header (.phm)
struct PhysMeshHeader {                         // 64 bytes
    u32 meshCount; u32 meshDataOffset; u32 meshDataEnd;
    u32 _res[3]; f32 totalVolume; u32 _res2; u8 _res3[12]; u32 _res4; u8 _res5[8];
    f32 boundsX; f32 boundsY;
};

// §5.3 — M3 Rigid Body
struct PHRB_v4 {                                // 80 bytes
    u16 simulationType; u16 parentBoneIndex; u32 physicsType;
    f32 density; f32 friction; f32 restitution;
    f32 linearDamping; f32 angularDamping; f32 gravityScale;
    u8 dynamicState[16]; f32 dynamicBlendOut; u8 rigidBodyShape[12];
    u32 flags; u32 localChannels; u32 priority; u32 _pad;
};

// §5.4 — M3 Physics Joint
struct PHYJ_v0 {                                // 180 bytes
    u32 jointType; u32 boneIndex1; u32 boneIndex2;
    f32 matrixBody1[16]; f32 matrixBody2[16];
    u32 enableLimits; f32 limitMin; f32 limitMax; f32 coneAngle;
    u32 enableFriction; f32 friction; f32 dampingRatio; f32 angularFrequency;
    f32 breakThreshold; u8 enableShape; u8 _pad[3];
};

// §5.5 — M3 Cloth Physics
struct PHCL_v4 {                                // 192 bytes
    u32 clothMeshCount; u32 skinBoneCount;
    u8 skinBones[12]; u8 simEnabled[12]; u8 vertexBones[12]; u8 vertexWeights[12];
    u8 colliders[12]; u8 proxies[12];
    f32 density; f32 tracking; f32 stretchStiffness; f32 horizontalStiffness;
    f32 bendingStiffness; f32 damping; f32 friction; f32 gravity;
    f32 explosionScale; f32 windScale; f32 shearStiffness; f32 dragFactor;
    f32 liftFactor; f32 sphereStiffness; u32 flatten; u8 active[16];
    u32 useSkinCollision; f32 skinOffset; f32 skinExponent; f32 skinStiffness;
    u32 localChannels; f32 localWind[3];
};

// §5.6 — M3 Physics Shape
struct PHSH_v3 {                                // 300 bytes (header shown)
    f32 transform[16]; u8 shapeType; u8 _pad[3];
    f32 sizeX; f32 sizeY; f32 sizeZ;
    // ... collision geometry data follows
};
```

---

*Specification derived from binary analysis of 2,848 D3 physics files and 50,742 M3 files.
Cross-validated against M3 PHRB (5,005 entries), PHCL (378 entries), and PHYJ (12,796 entries).*
