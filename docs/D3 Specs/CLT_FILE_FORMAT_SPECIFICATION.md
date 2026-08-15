# CLT File Format Specification

**Format**: Diablo III Cloth Simulation Parameters (`.clt`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**SNO Group**: 11 (`Cloth`)
**Version**: 51 (all 74 corpus files)
**Corpus**: 74 files, every one exactly 116 bytes

See [README.md](README.md) for the build these offsets come from, the generator pipeline and
the conventions used below.

---

> ## Correction pass — 2026-08-16
>
> **Every field name in the previous revision of this document was wrong, and the offsets were
> displaced by 16 bytes.** The byte-level *observations* were all correct — every value census
> below reproduces exactly — but they were attached to the wrong fields.
>
> Two independent errors compounded:
>
> 1. **There is no "32-byte SNO preamble".** The file is a **16-byte SNO header** followed by
>    the **100-byte `Cloth` struct** (16 + 100 = 116). The old document treated the struct's
>    own first 16 bytes as part of the header, so every field it named was one struct slot too
>    early relative to the registered layout.
> 2. **The names were guesses from value ranges**, made before the Switch 2.6.2 build's type
>    metadata was available. The metadata registers all 18 fields with their types and
>    defaults, which renames essentially the whole struct:
>
> | Old name | Old offset | Actually | struct offset |
> | --- | --- | --- | --- |
> | `gravity` | 0x024 | **`flMass`** | +0x14 |
> | `damping` | 0x028 | **`flSkinBlendRate`** | +0x18 |
> | `structuralStiffness` | 0x02C | **`flStretchStiffness0`** | +0x1C |
> | `stretchResistance` | 0x030 | **`flStretchStiffness1`** | +0x20 |
> | `bendingStiffness` | 0x034 | `flBendStiffness` | +0x24 |
> | `friction` | 0x038 | **`flExternalForceScale`** | +0x28 |
> | `windResponse` | 0x03C | **`flDragCoefficient`** | +0x2C |
> | `windDirectionBias` | 0x040 | **`flGravity`** | +0x30 |
> | `massScale` | 0x044 | **`flRootStiffness`** | +0x34 |
> | `collisionRadius` | 0x048 | **`flLinearDamping`** | +0x38 |
> | `timeStep` | 0x04C | **`flContactDamping`** | +0x3C |
> | `enableWind` / `collisionMode` | 0x050/054 | **`nCollisionPlane0/1`** | +0x40/44 |
> | `sentinel` (0xFFFFFFFF) | 0x060 | **`snoAmbientSound`** (a null SNO ref) | +0x50 |
> | `reserved6` | 0x064 | **`nUseCustomWind`** | +0x54 |
> | `reserved7/8/9` | 0x068–070 | **`vWindVelocity`** (one `Vector3D`) | +0x58 |
>
> The old "gravity, range 0.2–45.0" was mass; real gravity is the field the old document
> called `windDirectionBias` and dismissed as a ±0.01 bias — it is an acceleration per
> 1/60 s tick², which is exactly the `DT_ACCEL` type the binary registers for it.
>
> **Appendix A's hex dumps were also fabricated** and have been replaced with a real one.
> The old `Flag.clt` dump shows `dwFlags = 6` and a different float set; the actual file has
> `dwFlags = 4`.

---

## 1. Overview

`.clt` assets hold the tuning parameters for one cloth setup — capes, flags, banners, ropes,
skirts, carpets. They are the simplest SNO format in the game: a **fixed-size record with no
variable-length sections, no internal offsets and no tag maps**, so the whole file is the
struct.

Cloth assets carry no outbound references except an (unused) ambient-sound slot. The binding
is inbound: an Actor references the cloth parameters, and the Appearance supplies the mesh
whose vertices are cloth-tagged.

```
Actor.acr ──► Cloth.clt          physical parameters (this file)
    └───────► Appearance.app     mesh + cloth vertex weighting
```

---

## 2. File Layout

```
┌──────────────────────────────────────────────┐
│  SNO file header      16 bytes  0x000–0x00F  │
├──────────────────────────────────────────────┤
│  Cloth struct        100 bytes  0x010–0x073  │
└──────────────────────────────────────────────┘
Total: 116 bytes (0x74) — all 74 files
```

The struct's own offsets are what the tables below use; **file position = 16 + struct offset**.
The struct ends exactly at its declared 100 bytes: there are zero trailing bytes in all 74
files.

### 2.1 SNO file header (16 bytes)

| File offset | Type | Field | Value |
| --- | --- | --- | --- |
| 0x000 | u32 | `magic` | `0xDEADBEEF`, 74/74 |
| 0x004 | u16 | `version` | `51`, 74/74 |
| 0x006 | u16 | — | 0 |
| 0x008 | u64 | — | 0 |

---

## 3. The Cloth Struct (100 bytes)

```cpp
struct Cloth {                              // 100 bytes (0x64)
    u32   dwSnoId;              // +0x00: this asset's own SNO id; unique per file
    u32   _pad04;               // +0x04: 0 in 74/74
    u32   _pad08;               // +0x08: 0 in 74/74
    u32   dwFlags;              // +0x0C: {0, 4, 6, 12}

    i32   dwRelaxIterations;    // +0x10: constraint relaxation passes. default 25
    f32   flMass;               // +0x14: per-particle mass. default 0.5
    f32   flSkinBlendRate;      // +0x18: blend back toward the skinned pose, 0..1
    f32   flStretchStiffness0;  // +0x1C: stretch constraint, axis 0. default 1.0
    f32   flStretchStiffness1;  // +0x20: stretch constraint, axis 1. default 0.5
    f32   flBendStiffness;      // +0x24: default 0.1
    f32   flExternalForceScale; // +0x28: response to external force. default 1.0
    f32   flDragCoefficient;    // +0x2C: air drag. default 1.0
    f32   flGravity;            // +0x30: DT_ACCEL — units per (1/60 s)²
    f32   flRootStiffness;      // +0x34: how hard pinned roots hold. default 1.5
    f32   flLinearDamping;      // +0x38: default 0.1
    f32   flContactDamping;     // +0x3C: damping while in contact. default 0.1

    i32   nCollisionPlane[4];   // +0x40: DT_FIXEDARRAY of 4 ints
    i32   snoAmbientSound;      // +0x50: DT_SNO, group 5. −1 in 74/74 (never used)
    i32   nUseCustomWind;       // +0x54: 0 or 1
    f32   vWindVelocity[3];     // +0x58: only meaningful when nUseCustomWind == 1
};
```

The binary supplies a default for ten of these fields. **Six land exactly on the corpus mode**,
which is a useful independent check that the field assignment is right, and the other four are
the *second* or third most common value rather than something absent:

| Field | Registered default | Corpus mode | |
| --- | --- | --- | --- |
| `dwRelaxIterations` | 25 | 25 (71/74) | ✔ |
| `flMass` | 0.5 | 0.5 (23) | ✔ |
| `flStretchStiffness0` | 1.0 | 1.0 (46) | ✔ |
| `flDragCoefficient` | 1.0 | 1.0 (12) | ✔ |
| `flRootStiffness` | 1.5 | 1.5 (32) | ✔ |
| `flContactDamping` | 0.1 | 0.1 (57) | ✔ |
| `flStretchStiffness1` | 0.5 | 1.0 (22) | 2nd (0.5 ×20) |
| `flBendStiffness` | 0.1 | 1.0 (17) | 2nd (0.1 ×13) |
| `flExternalForceScale` | 1.0 | 0.0 (28) | 2nd (1.0 ×21) |
| `flLinearDamping` | 0.1 | 0.5 (21) | 3rd (0.1 ×10) |

Every registered default occurs in the corpus. That is the check that matters: a default
landing on a value the field never takes would mean the offset assignment is wrong.

### 3.1 `dwFlags` (+0x0C)

| Value | Count | Example assets |
| --- | ---: | --- |
| 0 | 10 | `Rope`, `Rope_Hanging`, `Axe Bad Data` |
| 4 | 47 | `Adria_Cloth`, `Barbarian_Female_Skirt`, `Carpet` |
| 6 | 15 | `Gore_ClothA_caOut`, `Rope_Hanging_Collision` |
| 12 | 2 | `DemonHunter_Female_cloth`, `caOut_CaldExt_Cloth_A` |

Bit 2 is set in 64 of 74. The individual bit meanings are **not** established — the previous
document's "collision / self-collision / pinning" reading was a guess and is withdrawn.

### 3.2 Value ranges over all 74 files

| Field | Min | Max | Distinct | Most common |
| --- | ---: | ---: | ---: | --- |
| `dwRelaxIterations` | 20 | 50 | 3 | 25 (71), 50 (2), 20 (1) |
| `flMass` | 0.2 | 45.0 | 16 | 0.5 (23), 1.0 (13), 2.0 (8) |
| `flSkinBlendRate` | 0.0 | 1.0 | 12 | 0.0 (22), 1.0 (14), 0.1 (10) |
| `flStretchStiffness0` | 0.1 | 1.0 | 9 | 1.0 (46), 0.8 (8), 0.9 (6) |
| `flStretchStiffness1` | 0.0 | 1.0 | 11 | 1.0 (22), 0.5 (20), 0.4 (7) |
| `flBendStiffness` | 0.0 | 1.0 | 12 | 1.0 (17), 0.1 (13), 0.2 (10) |
| `flExternalForceScale` | 0.0 | 25.0 | 12 | 0.0 (28), 1.0 (21), 0.2 (9) |
| `flDragCoefficient` | 0.0 | 30.0 | 21 | 1.0 (12), 0.5 (10), 0.3 (8) |
| `flGravity` | −0.01789 | 0.27778 | 19 | 0.0 (18), −0.00833 (11), −0.00278 (8) |
| `flRootStiffness` | 1.0 | 20.0 | 9 | 1.5 (32), 1.0 (14), 2.0 (14) |
| `flLinearDamping` | 0.0 | 10.0 | 16 | 0.5 (21), 1.0 (11), 0.1 (10) |
| `flContactDamping` | 0.0 | 10.0 | 10 | 0.1 (57), 0.5 (3), 0.4 (3) |

`flGravity` is signed and small because it is a **per-tick² acceleration**: −0.00833 units per
(1/60 s)² is −30 units/s². It is exactly zero in 18 files, which switch gravity off entirely.

### 3.3 Collision planes (+0x40)

A fixed array of four ints, and only three combinations occur:

| `nCollisionPlane[0..3]` | Count |
| --- | ---: |
| `(0, 0, 0, 0)` | 58 |
| `(1, 0, 0, 0)` | 12 |
| `(1, 2, 0, 0)` | 4 |

Slots 2 and 3 are zero in every file. The values are small integers rather than SNO ids, so
they index a plane set rather than reference an asset; which set is not established.

### 3.4 Custom wind (+0x54, +0x58)

`nUseCustomWind` is 1 in exactly one file, **`g_Cloth_Ribbon_Float.clt`**, and that is the only
file whose `vWindVelocity` is non-zero (`(−5, 0, 9)`). The other 73 pair `0` with `(0, 0, 0)`.

That perfect agreement is what confirms both names: a flag and a vector that are non-default in
the same single file, and nowhere else.

---

## 4. Cross-reference: parameters vs asset semantics

Re-derived with the corrected field names. The values now read the way the asset names suggest,
which the old table could not — it had `Carpet` at "gravity 3.0" and `Flag` at "windResponse
20.0".

| File | mass | skinBlend | stretch0 | stretch1 | bend | extForce | drag | gravity | root | linDamp | contactDamp |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `Rope_Hanging_Collision` | 10.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 5.00 | −0.0179 | 1.00 | 0.50 | 0.10 |
| `Flag` | 0.50 | 0.10 | 0.80 | 0.90 | 0.20 | 1.00 | 4.00 | −0.0056 | 1.00 | 0.20 | 0.10 |
| `Carpet` | 45.00 | 1.00 | 1.00 | 1.00 | 1.00 | 0.20 | 0.50 | 0.0000 | 1.00 | 2.00 | 10.00 |
| `Tent` | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 0.20 | 1.00 | 0.0000 | 2.00 | 0.50 | 0.10 |
| `Barbarian_Female_Skirt` | 0.50 | 0.70 | 0.80 | 0.50 | 0.40 | 0.00 | 0.30 | −0.0056 | 1.50 | 0.10 | 0.10 |
| `x1_Malthael_wings_cloth` | 0.50 | 0.90 | 0.30 | 0.30 | 0.00 | 0.00 | 0.50 | −0.0083 | 1.35 | 0.50 | 0.10 |
| `DemonHunter_Female_cloth` | 0.60 | 0.70 | 1.00 | 0.60 | 0.60 | 0.00 | 0.50 | −0.0042 | 2.00 | 0.40 | 0.10 |

* **Carpet** is the heaviest thing in the corpus (mass 45) and the most damped
  (`flContactDamping` 10.0) — a rug that barely moves and stops dead on contact.
* **Rope** is heavy (10) and fully stiff on every axis, with the strongest gravity.
* **Flag** is light (0.5) with high drag (4.0) and near-zero skin blend — it flies free.
* **Malthael's wings** have `flBendStiffness` 0.0, the floppiest bend in the corpus.
* **Character cloth** clusters tightly: mass 0.5–0.6, skin blend 0.7–0.9, contact damping 0.1.
  `flExternalForceScale` is 0.0 for all of them, so character cloth is driven by body motion
  alone and ignores environmental force.

---

## 5. Known Unknowns

| Field | Offset | What is known |
| --- | --- | --- |
| `dwFlags` bits | +0x0C | only 0/4/6/12 occur; bit 2 set in 64/74. No engine read located, so bit meanings are open |
| `nCollisionPlane[0..3]` | +0x40 | three tuples only, slots 2–3 always 0; small ints, not SNO ids. What they index is open |
| `flStretchStiffness0` vs `1` | +0x1C/20 | two separate stretch axes (warp/weft is the natural reading, unconfirmed) |
| `snoAmbientSound` | +0x50 | registered `DT_SNO` group 5; −1 in every file, so the feature ships unused |

The `_pad04` / `_pad08` words are zero in all 74 files and have no registered field.

---

## Appendix A — Real hex dump

`Flag.clt`, 116 bytes, verbatim:

```
00000000  EF BE AD DE 33 00 00 00 00 00 00 00 00 00 00 00   magic, version 51
00000010  DF 44 00 00 00 00 00 00 00 00 00 00 04 00 00 00   snoId=17631, dwFlags=4
00000020  19 00 00 00 00 00 00 3F CD CC CC 3D CD CC 4C 3F   relax=25 mass=0.5 skin=0.1 str0=0.8
00000030  66 66 66 3F CD CC 4C 3E 00 00 80 3F 00 00 80 40   str1=0.9 bend=0.2 extF=1.0 drag=4.0
00000040  61 0B B6 BB 00 00 80 3F CD CC 4C 3E CD CC CC 3D   grav=-0.00556 root=1.0 lin=0.2 con=0.1
00000050  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   collision planes all 0
00000060  FF FF FF FF 00 00 00 00 00 00 00 00 00 00 00 00   snoAmbientSound=-1, wind off
00000070  00 00 00 00                                       vWindVelocity.z
```

---

## Appendix B — Related documents

* [README.md](README.md) — derivation basis, pipeline, status of every D3 spec.
* [D3_PHYSICS_FORMAT_SPECIFICATION.md](D3_PHYSICS_FORMAT_SPECIFICATION.md) — the physics family
  umbrella (`.phy`, `.clt`, `.phm`); this document is the authority for `.clt`.
* [APP_FILE_FORMAT_SPECIFICATION.md](APP_FILE_FORMAT_SPECIFICATION.md) — the mesh side of a
  cloth setup.
