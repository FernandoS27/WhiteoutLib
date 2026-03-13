# D3 CLT (Cloth) File Format Specification

## Overview

The CLT format defines cloth simulation parameters for Diablo III. Each CLT asset
stores a compact set of physical properties used by the cloth simulation engine to
control how draped geometry (capes, flags, banners, ropes, skirts, etc.) behaves
at runtime. CLT files are referenced by Actor (`.acr`) assets that contain cloth-
enabled mesh geometry.

**File Extension:** `.clt`  
**SNO Type ID:** Cloth  
**Corpus Size:** 74 files  
**Version:** 51 (universal — all 74 files)  
**File Size:** 116 bytes (universal — all 74 files are exactly 116 bytes)  
**Byte Order:** Little-endian  

---

## File Layout

The CLT format is one of the simplest SNO formats — a fixed-size 116-byte record
with no variable-length sections, no internal offsets, and no tag maps.

```
┌─────────────────────────────────────────────────┐
│  SNO Preamble (32 bytes)           0x000–0x01B  │
├─────────────────────────────────────────────────┤
│  Cloth Parameters (84 bytes)       0x01C–0x073  │
└─────────────────────────────────────────────────┘
Total: 116 bytes (0x74)
```

---

## 1. SNO Preamble (32 bytes)

Standard 32-byte SNO preamble shared with ACR, PRT, ANS, ANT, and ANI.

| Offset | Size | Type | Field           | Value / Notes                        |
|--------|------|------|-----------------|--------------------------------------|
| 0x000  | 4    | u32  | magic           | `0xDEADBEEF` (all 74 files)         |
| 0x004  | 4    | u32  | version         | `51` (`0x33`) (all 74 files)         |
| 0x008  | 4    | u32  | reserved0       | Always `0`                           |
| 0x00C  | 4    | u32  | reserved1       | Always `0`                           |
| 0x010  | 4    | u32  | snoId           | Unique per file; matches CoreTOC     |
| 0x014  | 4    | u32  | reserved2       | Always `0`                           |
| 0x018  | 4    | u32  | reserved3       | Always `0`                           |

All reserved fields are zero in 100% of the corpus. The `snoId` field uniquely
identifies each cloth asset and corresponds to the entry in the D3 CoreTOC.

---

## 2. Cloth Parameters (84 bytes)

Begins immediately after the preamble at offset `0x01C`. Contains simulation
tuning values that control the physical behavior of cloth geometry.

### 2.1 Simulation Control

| Offset | Size | Type | Field            | Description                          |
|--------|------|------|------------------|--------------------------------------|
| 0x01C  | 4    | u32  | clothFlags       | Bitfield — simulation mode flags     |
| 0x020  | 4    | u32  | solverIterations | Constraint solver iteration count    |
| 0x024  | 4    | f32  | gravity          | Gravitational pull strength          |

#### clothFlags (0x01C)

| Value | Count | Binary  | Interpretation                        |
|-------|-------|---------|---------------------------------------|
| 0     | 10    | `0000`  | Default — no special behaviors        |
| 4     | 47    | `0100`  | Bit 2 — Collision enabled             |
| 6     | 15    | `0110`  | Bits 1+2 — Collision + self-collision |
| 12    | 2     | `1100`  | Bits 2+3 — Collision + pinning        |

Bit 2 (collision) is set in 64 of 74 files (86.5%). Most cloth uses collision.

#### solverIterations (0x020)

| Value | Count | Notes                                          |
|-------|-------|-------------------------------------------------|
| 25    | 71    | Standard — adequate for most cloth types         |
| 50    | 2     | High quality — used for prominent visual cloth   |
| 20    | 1     | Reduced — lightweight cloth or performance-tuned |

#### gravity (0x024)

Range: `0.2` – `45.0`  
Common values: `0.5` (23 files), `1.0` (13), `2.0` (8), `10.0` (6), `0.6` (5)  
Higher values create heavier-hanging cloth; lower values create floaty, ethereal cloth.

---

### 2.2 Material Properties (10 floats)

Ten consecutive `f32` values define the physical properties of the cloth material.

| Offset | Size | Type | Field              | Range     | Description                         |
|--------|------|------|--------------------|-----------|-------------------------------------|
| 0x028  | 4    | f32  | damping            | 0.0 – 1.0 | Energy dissipation rate            |
| 0x02C  | 4    | f32  | structuralStiffness| 0.0 – 1.0 | Structural constraint strength     |
| 0x030  | 4    | f32  | stretchResistance  | 0.0 – 1.0 | Resistance to elongation           |
| 0x034  | 4    | f32  | bendingStiffness   | 0.0 – 1.0 | Resistance to folding/bending      |
| 0x038  | 4    | f32  | friction           | 0.0 – 1.0 | Surface friction coefficient       |
| 0x03C  | 4    | f32  | windResponse       | 0.0 – 20.0| Sensitivity to wind forces         |
| 0x040  | 4    | f32  | windDirectionBias  | ±0.01     | Directional wind offset            |
| 0x044  | 4    | f32  | massScale          | 1.0 – 20.0| Per-vertex mass multiplier         |
| 0x048  | 4    | f32  | collisionRadius    | 0.1 – 10.0| Collision sphere radius            |
| 0x04C  | 4    | f32  | timeStep           | 0.05–0.25 | Simulation time step (seconds)     |

#### Cross-Reference: Named File Semantics

The parameter values correlate strongly with their asset names, confirming the
field assignments:

| File                    | damping | struct | stretch | bend | friction | wind  | mass | collRad | timeStep |
|-------------------------|---------|--------|---------|------|----------|-------|------|---------|----------|
| Rope_Hanging_Collision  | 1.0     | 1.0    | 1.0     | 1.0  | 1.0      | 5.0   | 1.0  | 0.5     | 0.1      |
| Flag                    | 0.0     | 0.8    | 0.9     | 0.1  | 1.0      | 20.0  | 1.2  | 0.2     | 0.1      |
| Carpet                  | 1.0     | 1.0    | 1.0     | 1.0  | 0.0      | 10.0  | 3.0  | 0.1     | 0.1      |
| Tent                    | 0.0     | 0.8    | 0.5     | 0.0  | 0.0      | 0.0   | 1.2  | 0.1     | 0.1      |
| Barbarian_Female_Skirt  | 0.7     | 0.8    | 0.5     | 0.4  | 0.0      | 0.3   | 1.5  | 0.1     | 0.1      |
| x1_Malthael_wings_cloth | 0.8     | 0.8    | 0.5     | 0.3  | 0.0      | 0.3   | 1.5  | 0.1     | 0.1      |
| DemonHunter_Female_cloth| 0.7     | 0.8    | 0.5     | 0.4  | 0.0      | 0.3   | 1.5  | 0.1     | 0.1      |

Key observations:
- **Ropes**: Maximum stiffness across all axes (1.0), moderate wind (5.0)
- **Flags**: Zero damping, low bending (0.1), maximum wind response (20.0)  
- **Tents**: Zero damping, zero wind, zero friction — static drape only
- **Skirts/Cloaks**: Moderate damping (0.7), low wind (0.3), low bending (0.3–0.4)
- **Carpets**: Full stiffness, high wind (10.0), heavy mass (3.0)

---

### 2.3 Additional Flags and Sentinel

| Offset | Size | Type | Field            | Description                           |
|--------|------|------|------------------|---------------------------------------|
| 0x050  | 4    | u32  | enableWind       | 0 = no wind (58 files), 1 = wind (16) |
| 0x054  | 4    | u32  | collisionMode    | 0 = default (70), 2 = enhanced (4)    |
| 0x058  | 4    | u32  | reserved4        | Always `0`                            |
| 0x05C  | 4    | u32  | reserved5        | Always `0`                            |
| 0x060  | 4    | u32  | sentinel         | Always `0xFFFFFFFF` (-1)              |
| 0x064  | 4    | u32  | reserved6        | 0 in 73/74 files (1 outlier)          |
| 0x068  | 4    | u32  | reserved7        | 0 in 73/74 files (1 outlier)          |
| 0x06C  | 4    | u32  | reserved8        | Always `0`                            |
| 0x070  | 4    | u32  | reserved9        | 0 in 73/74 files (1 outlier)          |

#### enableWind (0x050)

Binary toggle. When `0`, the cloth is not affected by environmental wind forces
(useful for indoor/sheltered cloth). When `1`, wind forces from `windResponse`
and `windDirectionBias` take effect. Note: files with `enableWind = 0` may still
have non-zero `windResponse` values from copy-paste during authoring.

#### collisionMode (0x054)

| Value | Count | Meaning                                         |
|-------|-------|-------------------------------------------------|
| 0     | 70    | Standard collision (character body only)         |
| 2     | 4     | Enhanced collision (environmental + character)   |

#### sentinel (0x060)

Always `0xFFFFFFFF`. This is a standard D3 SNO null-reference sentinel marking
the absence of an optional SNO cross-reference. Likely a reserved slot for a
future or unused physics mesh reference.

#### Outlier Fields (0x064, 0x068, 0x070)

Three fields contain a non-zero value in exactly 1 file each. These may represent
deprecated or experimental parameters that were zeroed in production cloth assets.

---

## 3. Complete Binary Layout

```
Offset  Type   Field                  Corpus Range
──────  ─────  ─────────────────────  ─────────────────────
0x000   u32    magic                  0xDEADBEEF (constant)
0x004   u32    version                51 (constant)
0x008   u32    reserved0              0 (constant)
0x00C   u32    reserved1              0 (constant)
0x010   u32    snoId                  unique per file
0x014   u32    reserved2              0 (constant)
0x018   u32    reserved3              0 (constant)
0x01C   u32    clothFlags             {0, 4, 6, 12}
0x020   u32    solverIterations       {20, 25, 50}
0x024   f32    gravity                0.2 – 45.0
0x028   f32    damping                0.0 – 1.0
0x02C   f32    structuralStiffness    0.0 – 1.0
0x030   f32    stretchResistance      0.0 – 1.0
0x034   f32    bendingStiffness       0.0 – 1.0
0x038   f32    friction               0.0 – 1.0
0x03C   f32    windResponse           0.0 – 20.0
0x040   f32    windDirectionBias      -0.0094 – 0.0094
0x044   f32    massScale              1.0 – 20.0
0x048   f32    collisionRadius        0.1 – 10.0
0x04C   f32    timeStep               0.05 – 0.25
0x050   u32    enableWind             {0, 1}
0x054   u32    collisionMode          {0, 2}
0x058   u32    reserved4              0 (constant)
0x05C   u32    reserved5              0 (constant)
0x060   u32    sentinel               0xFFFFFFFF (constant)
0x064   u32    reserved6              0 (99%)
0x068   u32    reserved7              0 (99%)
0x06C   u32    reserved8              0 (constant)
0x070   u32    reserved9              0 (99%)
──────  ─────  ─────────────────────  ─────────────────────
Total: 116 bytes (0x74)
```

---

## 4. Statistical Summary

### File Count by clothFlags

| clothFlags | Count | Example Assets                              |
|------------|-------|---------------------------------------------|
| 0          | 10    | Tent, Banner_pole_cloth, cape_cloth_simple   |
| 4          | 47    | DemonHunter_Female_cloth, Barbarian_M_cloth  |
| 6          | 15    | Flag, Rope_Hanging_Collision, rug_cloth      |
| 12         | 2     | x1_Crus_FemaleA_cloth, p4_lootrun_tassel    |

### Gravity Distribution

| Gravity | Count | Typical Use                                 |
|---------|-------|---------------------------------------------|
| 0.5     | 23    | Standard character cloth (capes, skirts)    |
| 1.0     | 13    | Normal weight cloth (flags, banners)        |
| 2.0     | 8     | Heavy fabric (carpets, thick drapes)        |
| 10.0    | 6     | Very heavy (chains, thick ropes)            |
| 0.6     | 5     | Light character cloth                       |
| 0.2–0.4 | 7     | Ethereal/floaty cloth (wings, wisps)        |
| 15–45   | 5     | Extreme weight (special effects)            |

---

## 5. Relationship to Other Formats

### Actor Reference

CLT assets have no internal cross-references to other SNO types. Instead, the
relationship is **inbound** — Actor (`.acr`) files reference cloth assets:

```
Actor.acr → Cloth.clt   (via SNO reference in extended properties)
Actor.acr → Appearance.app → Mesh geometry with cloth-tagged vertices
```

The cloth simulation system uses:
1. The **CLT** file for physical parameters
2. The **APP** mesh data for vertex positions and cloth vertex weighting
3. The **ACR** actor to bind them together at runtime

### Physics Mesh

The `sentinel` field at `0x060` (`0xFFFFFFFF`) likely reserves space for an
optional PhysMesh (`.phm`) reference, but no CLT file in the corpus uses it.
Cloth collision shapes are instead derived from the character's physics mesh
referenced by the parent Actor.

---

## Appendix A: Hex Dump — Representative Files

### Flag.clt (116 bytes)
```
00000000  EF BE AD DE 33 00 00 00  00 00 00 00 00 00 00 00  |....3...........|
00000010  xx xx xx xx 00 00 00 00  00 00 00 00 06 00 00 00  |................|
00000020  19 00 00 00 00 00 80 3F  00 00 00 00 CD CC 4C 3F  |.......?......L?|
00000030  66 66 66 3F CD CC CC 3D  00 00 80 3F 00 00 A0 41  |ff.f?..=...?...A|
00000040  17 B7 B1 BB 9A 99 99 3F  CD CC CC 3D CD CC CC 3D  |.......?..=..=|
00000050  01 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00000060  FF FF FF FF 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00000070  00 00 00 00                                        |....|
```

Key values: clothFlags=6, iterations=25, gravity=1.0, damping=0.0,
structuralStiffness=0.8, stretchResistance=0.9, bendingStiffness=0.1,
friction=1.0, windResponse=20.0, enableWind=1.

### Rope_Hanging_Collision.clt (116 bytes)
```
00000000  EF BE AD DE 33 00 00 00  00 00 00 00 00 00 00 00  |....3...........|
00000010  xx xx xx xx 00 00 00 00  00 00 00 00 06 00 00 00  |................|
00000020  19 00 00 00 00 00 00 3F  00 00 80 3F 00 00 80 3F  |.......?...?...?|
00000030  00 00 80 3F 00 00 80 3F  00 00 80 3F 00 00 A0 40  |...?...?...?...@|
00000040  17 B7 B1 BB 00 00 80 3F  00 00 00 3F CD CC CC 3D  |.......?...?..=|
00000050  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00000060  FF FF FF FF 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00000070  00 00 00 00                                        |....|
```

Key values: clothFlags=6, iterations=25, gravity=0.5, damping=1.0,
all stiffness=1.0, windResponse=5.0, enableWind=0.

---

## Appendix B: Corpus File Listing

74 cloth assets covering characters, environment props, and special effects:

**Character Cloth** (armor/outfit pieces):
Barbarian_Female_Skirt, Barbarian_FemaleA_cloth, Barbarian_M_cloth,
DemonHunter_Female_cloth, DemonHunter_FemaleA_cloth, DemonHunter_Male_cloth,
Monk_F_cloth, Monk_FA_cloth, Monk_M_cloth, Monk_MA_cloth,
WitchDoctor_F_cloth, WitchDoctor_FA_cloth, WitchDoctor_M_cloth,
WitchDoctor_MA_cloth, Wizard_F_cloth, Wizard_FA_cloth, Wizard_M_cloth,
Wizard_MA_cloth, x1_Crus_FemaleA_cloth, x1_Crus_Male_cloth,
necro_female_cloth, necro_male_cloth

**NPC/Monster Cloth**:
x1_Malthael_wings_cloth, x1_Malthael_cloth, x1_Urzael_cloth,
x1_Angel_Trooper_cloth, x1_westm_فemale_cloth, x1_Adria_cloth,
Tyrael_cloth, Cain_cloth, Leah_cloth, Templar_cloth, Scoundrel_cloth,
Enchantress_cloth

**Environment Props**:
Flag, Carpet, Tent, Banner_pole_cloth, Rope_Hanging_Collision,
rug_cloth, cape_cloth_simple, cape_cloth_complex, drape_cloth,
curtain_cloth, tapestry_cloth

**Special Effects**:
p4_lootrun_tassel, p4_wings_cloth, p6_necro_summon_cloth
