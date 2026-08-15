# ACR File Format Specification

**Format**: Diablo III Actor Definition (`.acr`)  
**Byte Order**: Little-endian  
**Magic**: `0xDEADBEEF`  
**Version**: 282  
**Corpus**: 19,177 files analyzed
**SNO Group**: 1 (`Actor`)
**Registered revision**: 288 — the shipped data is v282, so the binary's compiled struct describes a *newer* layout (see below / README §4)

See [README.md](README.md) for the build these offsets come from, the generator pipeline
and the conventions used below.

---


## CORRECTION PASS — 2026-08-15

> The sections below this notice were derived from file bytes alone, before the
> Switch 2.6.2 build's own reflection metadata was available. The layout has now
> been re-derived from `Actor_RegisterTypeDescriptors` (0x710060A4C0) in the
> Diablo III Nintendo Switch 2.6.2 binary and re-verified against all 19,177
> `.acr` files. **Where this notice disagrees with a later section, this notice
> is correct.** The authoritative machine-readable layout is the `Actor` entry in
> `data/d3_type_overrides.json`.
>
> The struct is **880 bytes** (file offsets 0x10..0x37F). The binary registers
> **448** because it describes the *current* revision, not the shipped one; the
> two are connected exactly by the documented `0x700000` rule
> (−8 for two post-shipped fields, +448 for `WeightedLook` 12→68, −8 for
> `ActorCollisionData` 76→68). 880 is confirmed by the tag-map descriptor, which
> holds offset 880 in 19,177/19,177 files.
>
> **Corrections, by section:**
>
> * **§5/§10 — there is no `walkSpeed` / `runSpeedScale` / `selectionRadius`.**
>   File 0x98..0xA3 is a single registered `DT_VECTOR3D` whose registered default
>   is `(0, 0, 4.0)`. `0x71008E48A0` rotates it by the actor's orientation and adds
>   it to the actor's world position; it is the fallback for the per-animation
>   root displacement. All-zero in 18,401/19,177 files.
> * **§6 — the actor-type enum names are wrong for five of eleven values.** The
>   engine's own table (0x710106A028, referenced by the `DT_ENUM` registration)
>   is: 0 Invalid, 1 **Monster** (3,974), 2 **Gizmo** (3,101), 3 **Client Effect**
>   (4,503), 4 **Server Prop** (1,056), 5 **Environment** (738), 6 **Critter**
>   (22), 7 **Player** (26), 8 **Item** (5,258), 9 **Axe Symbol** (46),
>   10 **Projectile** (438), 11 **Custom Brain** (15). There is no "Spawner",
>   "Effect", "Encounter" or "CharSelect".
> * **§7 — the "AABB" holds centre + half-extent, not min/max.** The second
>   vector is non-negative in 19,177/19,177 files. File 0x2C..0x3F is an
>   `AxialCylinder {Vector3D centre; float radius; float height}`, 0x40..0x4F a
>   `Sphere {Vector3D centre; float radius}`, 0x50..0x67 the `AABB`.
> * **§8 — 0x028 is PhysMesh (group 61); the group-28 Physics reference is at
>   file 0x2C4**, not an "alternative physics reference". Cross-check: 0x024
>   matches a real `.app` id in 19,177/19,177, 0x028 a `.phm` in 340/340,
>   0x078 a `.ans` in 8,628/8,628, 0x2C4 a `.phy` in 5,180/5,180 — 0 unmatched
>   in every case. 0x07C is a **Monster** SNO (group 25) and is set on
>   3,974/3,974 type-1 actors.
> * **§9 — the 412-byte entry is `MsgTriggeredEvent`**, `{int eMessageType;
>   TriggerEvent tEvent;}`. Its interior is now mapped: two 64-byte hardpoint
>   names at +72 and +140, a 64-byte look name at +208, a 64-byte constraint name
>   at +272, a `{group, handle}` SNO name at +48, and two `{ARGB colour, DT_TIME}`
>   pairs at +396/+400 and +404/+408. All four name fields are NUL-padded
>   printable ASCII in 27,362/27,362 events, and their values are exactly the
>   hardpoint literals the engine embeds in 0x710060D280 (`Default` ×14,158,
>   `HP_chest` ×4,092, `HP_trail1`, `HP_uniqueFX`, ...).
> * **§11 — the look slot's trailing int is a selection WEIGHT, not an alpha.**
>   `Actor_PickWeightedLookIndex` (0x710060A010) sums `max(0, weight)` over the
>   eight slots, takes `hash(seed) % (total+1)` and walks the cumulative weights
>   to pick a slot. Weights observed: 100 ×13,071, 1 ×241, 20 ×15, 25 ×14,
>   50 ×8, 33 ×6, 24 ×4, 80 ×3, 150 ×3, 40 ×2, 75 ×2, 0 ×140,044.
> * **§12 — the "extended actor properties" block is wrong throughout.** File
>   0x2C4 `snoPhysics` (group 28), 0x2C8 a flag word (zero in 19,177/19,177),
>   0x2CC an int, 0x2D0/0x2D4/0x2D8 three floats with registered defaults
>   1.0/1.0/0.5 (cloth parameters), 0x2DC an `ActorCollisionData` of **68** bytes
>   = `{ActorCollisionFlags[4 words]; int nCollisionEnabled (default 1);
>   AxialCylinder (default radius 4.0, height 1.25); AABB; float (default 0.8)}`,
>   0x320 an `InventoryImages[7]` of `{u32, u32}` hashed icon-name handles, and
>   0x358 one more such handle. There is no `renderFlags`, `altAnimRef`,
>   `altAnimCount`, `hasCollision` or "navigation AABB".
> * **§12/§14 — 0x358..0x37F are two C-string `{pointer, SerializeData}` pairs**,
>   not "end-of-header metadata". The first is the VO **casting direction**
>   (`"Male - Forties - Medium - Slight British - Contemptous"`), the second the
>   **voice-over role name** (`"Dark Cultist"`, `"Kyr the Weaponsmith"`,
>   `"Radek"`). The spec's "0x378 = fileSize−17, 0x37C = 1" is the second
>   string's descriptor: a 1-byte payload (a lone NUL) at the last byte of the file.
> * **§13 — the tag-map entry field order is reversed.** Entries are
>   `{u32 valueType, u32 tagId, u32 value}`. valueType: 0 int ×151,364, 2 SNO
>   ×51,814, 1 float ×28,139, 7 ×130, 3 ×98. 462 distinct tag ids.
>   `size == 4 + 12*count` holds in 19,177/19,177.
> * **§14 — the payload blocks are not in a fixed order.** Events precede the tag
>   map in 9,233 files, the tag map precedes events in 8,044, and four other
>   orders account for the remaining 1,900. Tiling the four `SerializeData`
>   blocks accounts for every byte of every file with zero overlaps.

---

## Table of Contents

1.  [File Structure Overview](#1-file-structure-overview)
2.  [Primitive Types](#2-primitive-types)
3.  [SNO Preamble](#3-sno-preamble)
4.  [Data Access Convention](#4-data-access-convention)
5.  [Actor Header](#5-actor-header)
6.  [Actor Type Enum](#6-actor-type-enum)
7.  [Bounding Volumes](#7-bounding-volumes)
8.  [SNO References](#8-sno-references)
9.  [Serialized Data Array (MsgData)](#9-serialized-data-array-msgdata)
10. [Movement & Selection Properties](#10-movement--selection-properties)
11. [Look Slots](#11-look-slots)
12. [Extended Actor Properties](#12-extended-actor-properties)
13. [Serialized Tag Map](#13-serialized-tag-map)
14. [Complete File Layout](#14-complete-file-layout)
15. [Cross-References to Other SNO Types](#15-cross-references-to-other-sno-types)
16. [Known Unknowns](#16-known-unknowns)
17. [Appendix A — Reading an ACR File (C++)](#appendix-a--reading-an-acr-file-c)
18. [Appendix B — All Structures Summary](#appendix-b--all-structures-summary)

---

## 1. File Structure Overview

ACR files are actor definition assets in Diablo III's SNO (Structured Numbered Object) system. Every entity in the game world — monsters, NPCs, items, projectiles, scene objects, gizmos, and player characters — is defined by an actor. Actors do not contain geometry or animation data directly; instead, they reference other SNO assets (appearances, animation sets, physics meshes) and provide configuration data such as bounding volumes, look variants, and behavior parameters.

```
┌─────────────────────────────────────────────────────────────┐
│  SNO Preamble                                 (32 bytes)    │
│  Actor Header                                 (128 bytes)   │
│  Look Slots                            (8 × 68 = 544 bytes)│
│  Extended Actor Properties                    (172 bytes)   │
├────────────────────────────── 0x380 ────────────────────────┤
│  Serialized Tag Map            (4 + N×12 bytes, variable)   │
├─────────────────────────────────────────────────────────────┤
│  Serialized Data Array          (optional, M × 412 bytes)   │
├─────────────────────────────────────────────────────────────┤
│  Trailing Padding                     (up to 16 bytes)      │
└─────────────────────────────────────────────────────────────┘
```

**Fixed-size region**: The first 0x370 bytes (880 bytes) of every ACR file are always the same structure. The bytes at 0x370–0x37F form a **serialized array descriptor** (not simple zero padding): offsets 0x370–0x377 are always zero, 0x378 contains the file data-end offset (= fileSize − 17), and 0x37C is always 1. The serialized tag map data begins at file offset **0x380**.

---

## 2. Primitive Types

```cpp
using u8  = uint8_t;     // 1 byte unsigned
using u16 = uint16_t;    // 2 bytes unsigned
using u32 = uint32_t;    // 4 bytes unsigned
using i32 = int32_t;     // 4 bytes signed
using f32 = float;       // 4 bytes IEEE 754

// SNO reference: a u32 snoId that identifies another asset.
// A value of 0xFFFFFFFF indicates a null/empty reference.
using SnoRef = u32;

// Fixed-length null-terminated ASCII string, padded with zeros.
using String64 = char[64];
```

---

## 3. SNO Preamble

**Size**: 32 bytes (0x20) | **Offset**: 0x000

> **Terminology note (2026-08-16).** There is no "32-byte preamble variant". Every SNO file
> is a **16-byte header** followed by the struct image — here the **880-byte `Actor` struct**,
> which is why the fixed region ends at file `0x370` (16 + 880 = 0x380, where the tag map
> begins). What this section calls the preamble is that header plus the struct's own first 16
> bytes. **The offsets below are file offsets and are correct**; their struct-relative
> equivalents are 16 lower, so `snoId` = struct +0 and `flags` = struct +0x0C — the same slot
> that holds `dwFlags` in every other D3 group.
>
> This is flagged because the "oversized preamble" reading is what displaced every field name
> in the CLT, SHD and `.phy` specifications until they were corrected on 2026-08-16.

```cpp
struct SnoPreamble {                            // 32 bytes
    u32     magic;                  // 0x000: 0xDEADBEEF — file signature
    u32     version;                // 0x004: Format version (282 = 0x11A for all ACR files)
    u32     _reserved008;           // 0x008: Always 0
    u32     _reserved00C;           // 0x00C: Always 0
    u32     snoId;                  // 0x010: Unique asset hash — identifies this actor in the SNO database
    u32     _reserved014;           // 0x014: Always 0
    u32     _reserved018;           // 0x018: Always 0
    u32     flags;                  // 0x01C: Actor flags (see §5)
};
```

### Field Notes

| Field     | Details |
|-----------|---------|
| `magic`   | Always `0xDEADBEEF`. Used to validate the file is a D3 SNO asset. |
| `version` | All 19,177 analyzed ACR files use version **282**. |
| `snoId`   | Each actor has a unique 32-bit identifier. Every ACR file has a distinct `snoId`. |
| `flags`   | Bitfield controlling actor behavior. See [§5 Actor Header](#5-actor-header) for observed values. |

---

## 4. Data Access Convention

Like all D3 SNO files, stored offsets require adding **+16** to reach the actual data in the file. This accounts for 16 bytes of zero-padding that precede every serialized data chunk.

```
actual_file_offset = stored_offset + 16
```

For example, the tag map offset stored at 0x068 is always **0x370**, meaning the tag map data begins at file offset **0x370 + 16 = 0x380**.

---

## 5. Actor Header

**Size**: 128 bytes (0x80) | **Offset**: 0x020–0x09F

The actor header immediately follows the SNO preamble and contains the core actor definition: type, appearance reference, bounding volumes, animation references, and data section pointers.

```cpp
struct ActorHeader {
    // ─── Actor Identity (0x020–0x023) ──────────────────────────────────────────
    u32     actorType;              // 0x020: Actor type enum (see §6)

    // ─── Primary SNO References (0x024–0x02B) ──────────────────────────────────
    SnoRef  appearanceSno;          // 0x024: → Appearance (.app) file. NEVER null — every actor has one.
    SnoRef  physMeshSno;            // 0x028: → PhysMesh (.phm) file. 0xFFFFFFFF if none (98.2% of actors).

    // ─── Bounding Volumes (0x02C–0x067) ────────────────────────────────────────
    f32     cylCenterX;             // 0x02C: Bounding cylinder center X
    f32     cylCenterY;             // 0x030: Bounding cylinder center Y
    f32     cylCenterZ;             // 0x034: Bounding cylinder center Z
    f32     cylRadius;              // 0x038: Bounding cylinder radius
    f32     cylHeight;              // 0x03C: Bounding cylinder height (0 for many actors)
    f32     aabbMinX;               // 0x040: Axis-aligned bounding box center/min X
    f32     aabbMinY;               // 0x044: AABB center/min Y
    f32     aabbMinZ;               // 0x048: AABB center/min Z
    f32     aabbExtentA;            // 0x04C: AABB extent / bounding sphere radius A
    f32     aabbMaxX;               // 0x050: AABB center/max X (often ≈ aabbMinX)
    f32     aabbMaxY;               // 0x054: AABB center/max Y
    f32     aabbMaxZ;               // 0x058: AABB center/max Z
    f32     aabbExtentB;            // 0x05C: AABB extent / bounding sphere radius B
    f32     boundingSphereRadius;   // 0x060: Overall bounding sphere radius
    f32     _unknown064;            // 0x064: Bounding-related float (often ≈ aabbMinZ)

    // ─── Serialized Tag Map Pointer (0x068–0x06F) ──────────────────────────────
    u32     tagMapOffset;           // 0x068: Offset to serialized tag map (+16). ALWAYS 0x370.
    u32     tagMapSize;             // 0x06C: Size of tag map in bytes. Range: 28–476.

    // ─── Reserved (0x070–0x077) ────────────────────────────────────────────────
    u32     _reserved070;           // 0x070: Always 0
    u32     _reserved074;           // 0x074: Always 0

    // ─── Animation SNO References (0x078–0x07F) ────────────────────────────────
    SnoRef  animSetSno;             // 0x078: → AnimSet (.ans) file. 0xFFFFFFFF if none (55% of actors).
    SnoRef  monsterSno;             // 0x07C: → Monster definition SNO. 0xFFFFFFFF if none (79% of actors).

    // ─── Serialized Data Array Pointer (0x080–0x08B) ───────────────────────────
    u32     msgDataOffset;          // 0x080: Offset to data array (+16). 0 if no data array.
    u32     msgDataSize;            // 0x084: Total data array size in bytes. 0 if none.
    u32     msgDataCount;           // 0x088: Number of entries. Entry size is always 412 bytes.

    // ─── Reserved (0x08C–0x097) ────────────────────────────────────────────────
    u32     _reserved08C;           // 0x08C: Always 0
    u32     _reserved090;           // 0x090: Always 0
    u32     _reserved094;           // 0x094: Always 0

    // ─── Movement & Selection Properties (0x098–0x0A3) ─────────────────────────
    f32     walkSpeed;              // 0x098: Walk/movement speed (0 for most actors; ~7.7 for NPCs)
    f32     runSpeedScale;          // 0x09C: Run speed multiplier (0 for most; ~0.2 for NPCs)
    f32     selectionRadius;        // 0x0A0: Selection/click radius (0 for most; 4.0–6.0 for NPCs)
};
```

### Flags Field (0x01C)

The `flags` field in the SNO preamble is a bitfield with the following observed values:

| Value | Hex    | Count  | Description |
|------:|--------|-------:|-------------|
|     0 | 0x00   | 16,368 | Default (no flags) |
|     1 | 0x01   |  1,604 | Flag bit 0 |
|     2 | 0x02   |    212 | Flag bit 1 |
|     3 | 0x03   |    292 | Flag bits 0+1 |
|     4 | 0x04   |     10 | Flag bit 2 |
|     5 | 0x05   |      3 | Flag bits 0+2 |
|     8 | 0x08   |     21 | Flag bit 3 |
|     9 | 0x09   |     32 | Flag bits 0+3 |
|    16 | 0x10   |      5 | Flag bit 4 |
|    32 | 0x20   |    467 | Flag bit 5 |
|    33 | 0x21   |     40 | Flag bits 0+5 |
|    34 | 0x22   |     92 | Flag bits 1+5 |
|    35 | 0x23   |     20 | Flag bits 0+1+5 |
|    64 | 0x40   |     11 | Flag bit 6 |

---

## 6. Actor Type Enum

The `actorType` field at offset 0x020 categorizes the actor. Every actor has exactly one type.

| Value | Count  | Type Name        | Description | Examples |
|------:|-------:|------------------|-------------|----------|
|     1 |  3,974 | **Monster**      | Enemies, NPCs, and allied characters | TriuneCultist, graveDigger, ZombieSkinny |
|     2 |  3,101 | **Spawner**      | Monster/object spawners | CultistSpawner, ShadowVermin_Spawner |
|     3 |  4,503 | **Effect**       | Visual effects, trail actors, spell effects | DH_strafe_shadows, PitFire_Actor |
|     4 |  1,056 | **Gizmo**        | Interactive objects: doors, gates, switches | SK_Throne_Gate, SiegebreakerGate |
|     5 |    738 | **Environment**  | Non-interactive scene objects, cloth, awnings | CannibalCellar_caOut, AwningClothL |
|     6 |     22 | **Critter**      | Ambient creatures: rats, chickens, cockroaches | CritterRat, CritterChicken, Cockroach |
|     7 |     26 | **Player**       | Playable hero characters | Barbarian_Male, Wizard_Female, Crusader_Male |
|     8 |  5,258 | **Item**         | Inventory items, equipment, gems, quest items | BlackMushroom, NecromancerCrystal |
|     9 |     46 | **Encounter**    | Encounter volumes, symbol/AoE templates | BatSwarm_Symbol, Encounter_Adventure |
|    10 |    438 | **Projectile**   | Projectile actors shot by skills/monsters | orbOfAnnihilation_projectile, BarbarianAxe |
|    11 |     15 | **CharSelect**   | Character selection screen models | Barbarian_Male_characterSelect |

---

## 7. Bounding Volumes

The bounding volume data at offsets 0x02C–0x067 defines collision and visibility boundaries for the actor. The data encodes:

1. **Bounding Cylinder** (0x02C–0x03C): Center position (X, Y, Z), radius, and height. Used for navigation collision.
2. **Axis-Aligned Bounding Box** (0x040–0x05C): Two sets of center/extent values. One appears to be the visual AABB and the other the selection AABB.
3. **Bounding Sphere** (0x060–0x064): Overall bounding sphere radius used for coarse culling.

### Example: 43_AD_TriuneCultist_C.acr

```
Cylinder:  center=(0.522, -0.101, 0.003)  radius=7.464  height=2.0
AABB set1: center=(0.480, -0.119, 3.735)  extent=6.148
AABB set2: center=(0.480, -0.119, 3.735)  extent=1.604
Sphere:    radius=3.594                    height=3.732
```

---

## 8. SNO References

ACR files reference other SNO assets through `SnoRef` fields (u32 values that match the `snoId` at offset 0x10 in the target file). A null reference is `0xFFFFFFFF`.

### Reference Fields

| Offset | Field | Target Type | Match Rate | Description |
|-------:|-------|-------------|------------|-------------|
| 0x024 | `appearanceSno` | Appearance (`.app`) | **100%** (19,177/19,177) | Every actor references exactly one appearance model. |
| 0x028 | `physMeshSno` | PhysMesh (`.phm`) | 1.8% (340/19,177) | Physical mesh for collision; used by environment and scene objects. |
| 0x078 | `animSetSno` | AnimSet (`.ans`) | 45% (8,628/19,177) | Animation set defining available animations. Monsters and NPCs typically have one. |
| 0x07C | `monsterSno` | Monster definition | 21% (4,091/19,177) | Monster/NPC behavior definition. References a SNO type not present in the extracted data set. |

### Cross-Reference Statistics

```
appearanceSno (0x024): 19,177 valid  |    0 null  |    0 unmatched
physMeshSno   (0x028):    340 valid  | 18,837 null |    0 unmatched
animSetSno    (0x078):  8,628 valid  | 10,549 null |    0 unmatched
monsterSno    (0x07C):  4,091 valid  | 15,086 null |    — (type not in dataset)
```

---

## 9. Serialized Data Array (MsgData)

**Entry Size**: 412 bytes (fixed) | **Present in**: 44% of actors (8,489/19,177)

The MsgData array contains behavioral configuration entries — likely animation events, spawn rules, or state machine data. The array pointer is at offsets 0x080–0x088.

```cpp
struct MsgDataPointer {
    u32     offset;     // 0x080: Offset to data array (+16). 0 if absent.
    u32     size;       // 0x084: Total size in bytes (count × 412).
    u32     count;      // 0x088: Number of entries.
};
```

### Entry Structure (412 bytes)

Each entry begins with a recognizable pattern:

```cpp
struct MsgDataEntry {
    u32     eventType;          // +0x00: Event/behavior type ID (e.g., 0x3E8=1000, 0x19=25, 0x16=22)
    u32     paramA;             // +0x04: Parameter A (animation tag, effect ID, etc.)
    u32     paramB;             // +0x08: Parameter B (often 0xFF = 255, acts as sentinel)
    u32     paramC;             // +0x0C: Parameter C (often 0, sometimes sub-flags)
    u8      data[396];          // +0x10: Remaining entry data (mostly zeros with sparse config values)
};
```

### Common Entry Patterns

| eventType | Hex    | Count | Likely Meaning |
|----------:|--------|------:|----------------|
|      1000 | 0x3E8  | Most  | Default/idle behavior |
|        25 | 0x19   | Many  | Standard combat animation event |
|        22 | 0x16   | Many  | Death/destruction animation event |
|        17 | 0x11   | Some  | Hit reaction event |
|      2021 | 0x7E5  | Some  | Chest/container open event |

### Size Distribution

Actors have between **1 and 30+** MsgData entries (most have 1–4):

| Count | Files | Total Size |
|------:|------:|-----------:|
|     1 | 2,577 | 412 bytes  |
|     2 | 2,080 | 824 bytes  |
|     3 | 1,229 | 1,236 bytes |
|     4 |   847 | 1,648 bytes |
|     5 |   719 | 2,060 bytes |
|    12 |   Few  | 4,944 bytes |
|   30+ |   Few  | 12,360+ bytes |

---

## 10. Movement & Selection Properties

Three float fields at the end of the actor header control movement and targeting behavior:

```cpp
f32     walkSpeed;          // 0x098: Base walk speed. 0.0 for static actors.
                            //        ~7.7 for walking NPCs, 3.0 for slow movers.
f32     runSpeedScale;      // 0x09C: Run speed multiplier. 0.0 for most.
                            //        ~0.2 for NPCs (scale factor applied to walkSpeed).
f32     selectionRadius;    // 0x0A0: Mouse selection/click radius for targeting.
                            //        0.0 for non-targetable actors.
                            //        4.0–6.0 for NPCs and monsters.
```

These fields are nonzero in only ~3–4% of actors (primarily monsters and NPCs of type 1, 6, 7).

---

## 11. Look Slots

**Size**: 544 bytes (8 × 68) | **Offset**: 0x0A4–0x2C3

Each actor can define up to **8 appearance look variants** (alternate visual states). Each look slot consists of a 64-byte null-terminated ASCII name string followed by a 4-byte integer value (typically an opacity/alpha percentage).

```cpp
struct LookSlot {
    char    name[64];       // Null-terminated ASCII look name (e.g., "Look 1", "flippy", "Corpse")
    u32     alpha;          // Opacity value: 100 (0x64) = fully visible, 0 = hidden, 1–99 = partial
};

struct LookArray {
    LookSlot slots[8];      // 0x0A4–0x2C3: 8 consecutive look slots (stride = 68 bytes)
};
```

### Look Slot Offsets

| Slot | Name Offset | Alpha Offset | Usage |
|-----:|------------:|-------------:|-------|
|    0 | 0x0A4       | 0x0E4        | Primary look — used by 72% of actors |
|    1 | 0x0E8       | 0x128        | Secondary look — rare (0.6% of actors) |
|    2 | 0x12C       | 0x16C        | Tertiary look — very rare (0.3%) |
|    3 | 0x170       | 0x1B0        | Quaternary look — very rare (0.2%) |
|    4 | 0x1B4       | 0x1F4        | Fifth look — very rare (0.1%) |
|    5 | 0x1F8       | 0x238        | Sixth look — very rare (0.1%) |
|    6 | 0x23C       | 0x27C        | Seventh look — almost never used (~6 actors) |
|    7 | 0x280       | 0x2C0        | Eighth look — always empty in all analyzed files |

### Common Look Names

| Name | Count | Description |
|------|------:|-------------|
| `"Look 1"` | 6,688 | Default appearance state |
| `""` (empty) | 5,425 | No look defined |
| `"A"` | 3,748 | First variant (letter-based naming for multi-state actors) |
| `"flippy"` | 303 | Item drop/spin animation look |
| `"A_physDeath"` | 159 | Physics-based death ragdoll variant A |
| `"Companion"` | 134 | Follower/companion visual state |
| `"Leather"` | 117 | Material variant |
| `"B"`, `"C"`, `"D"` | Various | Sequential look variants for complex actors |
| `"Corpse"` | 32 | Dead body visual state |

### Alpha Value Distribution

| Value | Meaning | Count |
|------:|---------|------:|
|   100 | Fully opaque (default) | 12,964 |
|     0 | Hidden / not set | 5,958 |
|     1 | Near-invisible | 233 |
|    25 | Quarter opacity | 5 |
|    20 | 20% opacity | 3 |
|    50 | Half opacity | 3 |

---

## 12. Extended Actor Properties

**Size**: 172 bytes | **Offset**: 0x2C4–0x36F

This region follows the look array and contains additional actor configuration. Many fields are zero for simple actors and only populated for monsters, NPCs, and complex interactive objects.

```cpp
struct ExtendedActorProperties {
    // ─── Physics / Collision References ────────────────────────────────────────
    SnoRef  altPhysRef;             // 0x2C4: Alternative physics/mesh reference (0xFFFFFFFF if none)
    u32     _reserved2C8[2];        // 0x2C8: Reserved (always 0)

    // ─── Scale & Rendering ─────────────────────────────────────────────────────
    f32     scaleX;                 // 0x2D0: Scale factor X (1.0 default)
    f32     scaleY;                 // 0x2D4: Scale factor Y (1.0 default)
    f32     scaleZ;                 // 0x2D8: Scale factor Z (often 0.5)
    u32     renderFlags;            // 0x2DC: Render/physics flags (0, 1, or 3)

    // ─── Animation / Behavior ──────────────────────────────────────────────────
    u32     _reserved2E0;           // 0x2E0: Reserved
    SnoRef  altAnimRef;             // 0x2E4: Alternative animation/behavior reference
    u32     altAnimCount;           // 0x2E8: Count associated with altAnimRef
    u32     hasCollision;           // 0x2EC: Collision enabled flag (0 or 1)

    // ─── Reserved Block ────────────────────────────────────────────────────────
    u32     _reserved2F0[3];        // 0x2F0: Reserved (always 0)

    // ─── Secondary Bounding Volume ─────────────────────────────────────────────
    f32     collRadius;             // 0x2FC: Secondary collision radius
    f32     collHeight;             // 0x300: Secondary collision height

    // ─── Navigation Bounding Box ───────────────────────────────────────────────
    f32     navMinX;                // 0x304: Navigation AABB min X
    f32     navMinY;                // 0x308: Navigation AABB min Y
    f32     navMaxZ;                // 0x30C: Navigation AABB max Z
    f32     navHalfWidth;           // 0x310: Navigation half-width
    f32     navExtentX;             // 0x314: Navigation extent X
    f32     navExtentZ;             // 0x318: Navigation extent Z
    f32     navScale;               // 0x31C: Navigation scale factor (often 0.8)

    // ─── Reserved / Sparse Data ────────────────────────────────────────────────
    u32     _reserved320[18];       // 0x320–0x367: Mostly zero region

    // ─── End-of-Header Metadata ────────────────────────────────────────────────
    u32     endMetaOffset;          // 0x368: Additional metadata offset or value
    u32     endMetaCount;           // 0x36C: Count/flags associated with endMetaOffset
};
```

> **Note**: This region is sparsely populated. For simple actors (effects, items, projectiles), nearly all fields are zero. Only monsters (type 1), NPCs, and complex gizmos (type 4) typically have nonzero values here.

---

## 13. Serialized Tag Map

**Offset**: Always file offset **0x380** (stored offset 0x370 + 16 padding)  
**Size**: Variable, stored at offset 0x06C  
**Entry Format**: 4-byte header + N × 12-byte entries

The serialized tag map is a key-value store of actor configuration overrides — properties set by designers in the game editor that supplement the fixed header fields. This is the D3 SNO serialized data format (also called "TagMap" or "SerializeData").

```cpp
struct TagMapHeader {
    u32     entryCount;             // Number of tag entries
};

struct TagMapEntry {
    u32     tagId;                  // Tag identifier (e.g., 0x00010450, 0x0001003D)
    u32     typeAndFlags;           // Tag type/flags
    u32     value;                  // Tag value (integer, float bits, or enum)
};
```

### Size Pattern

Tag map sizes follow the formula: **size = 4 + (entryCount × 12)**

| Size (bytes) | Entry Count | Files |
|-------------:|------------:|------:|
|           28 |           2 |   367 |
|           40 |           3 | 2,088 |
|           52 |           4 | 1,417 |
|           64 |           5 | 1,593 |
|           76 |           6 | 1,743 |
|           88 |           7 | 1,095 |
|          100 |           8 | 1,345 |
|          136 |          11 | 1,072 |
|          172 |          14 |   794 |
|          196 |          16 |   759 |

### Common Tag IDs

| Tag ID       | Frequency | Likely Meaning |
|:-------------|----------:|----------------|
| `0x00010450` | Very high | Actor behavior configuration |
| `0x0001003D` | Very high | Interaction type / quest flags |
| `0x0001003E` | Common    | Spawn limit / lifetime |
| `0x00010099` | Common    | Combat/damage configuration |
| `0x00010008` | Common    | Scale/display flags |
| `0x0001014C` | Moderate  | NPC conversation reference |
| `0x00010180` | Moderate  | Spawner configuration |

---

## 14. Complete File Layout

The complete file layout with actual offsets:

```
Offset    Size     Description
──────    ────     ───────────────────────────────────
0x000     32       SNO Preamble (magic, version, snoId, flags)
0x020     4        actorType
0x024     4        appearanceSno → .app
0x028     4        physMeshSno → .phm
0x02C     60       Bounding volumes (cylinder + AABB + sphere)
0x068     4        tagMapOffset (always 0x370)
0x06C     4        tagMapSize
0x070     8        Reserved (zeros)
0x078     4        animSetSno → .ans
0x07C     4        monsterSno → monster definition
0x080     4        msgDataOffset (0 if none)
0x084     4        msgDataSize
0x088     4        msgDataCount
0x08C     12       Reserved (zeros)
0x098     4        walkSpeed (float)
0x09C     4        runSpeedScale (float)
0x0A0     4        selectionRadius (float)
0x0A4     544      Look slots (8 × 68 bytes: 64-char name + u32 alpha)
0x2C4     172      Extended actor properties
0x370     16       Serialized array descriptor (0x378: data-end offset = fileSize−17, 0x37C: always 1)
0x380     var      Serialized tag map (4 + N×12 bytes)
var       16       Zero padding before data array (if present)
var       var      MsgData array (M × 412 bytes, if msgDataCount > 0)
var       ≤16      Trailing zero bytes to EOF
```

### Size Breakdown by Region

| Region | Size | Percentage of Median File |
|--------|-----:|:-------------------------:|
| SNO Preamble | 32 | 2.7% |
| Actor Header | 128 | 10.7% |
| Look Slots | 544 | 45.3% |
| Extended Properties | 172 | 14.3% |
| Zero Padding | 16 | 1.3% |
| Tag Map | ~76 (median) | ~6.3% |
| MsgData | ~412 (if present) | ~34.3% |

---

## 15. Cross-References to Other SNO Types

ACR actors sit at the top of D3's asset reference hierarchy. An actor connects to the full rendering, animation, and physics pipeline through SNO references:

```
Actor (.acr)
├── Appearance (.app)          [0x024] — 3D model, materials, skeleton
│   ├── Material (.mat)         — per-submesh material definition
│   │   ├── ShaderMap (.shm)    — shader parameter map
│   │   │   └── Shader (.shd)  — compiled shader
│   │   └── Texture (.tex)     — texture atlas/diffuse/normal maps
│   └── Look Table              — material assignments per look variant
├── PhysMesh (.phm)            [0x028] — collision mesh (for env objects)
├── AnimSet (.ans)             [0x078] — animation set
│   └── Anim (.ani)            — individual animation clips
├── Monster Definition         [0x07C] — behavior/AI (type not in dataset)
└── Look Slots                 [0x0A4] — appearance variant names
    └── (select which Look in the .app file to use)
```

### Reference Patterns by Actor Type

| Actor Type | Has Appearance | Has PhysMesh | Has AnimSet | Has Monster |
|:-----------|:--------------:|:------------:|:-----------:|:-----------:|
| Monster (1) | ✓ always | rare | ✓ often | ✓ often |
| Spawner (2) | ✓ always | rare | sometimes | sometimes |
| Effect (3) | ✓ always | never | sometimes | never |
| Gizmo (4) | ✓ always | sometimes | sometimes | sometimes |
| Environment (5) | ✓ always | ✓ always | never | never |
| Critter (6) | ✓ always | never | ✓ often | sometimes |
| Player (7) | ✓ always | never | ✓ always | never |
| Item (8) | ✓ always | never | rare | never |
| Encounter (9) | ✓ always | sometimes | rare | never |
| Projectile (10) | ✓ always | never | sometimes | never |
| CharSelect (11) | ✓ always | never | ✓ always | never |

---

## 16. Known Unknowns

The following aspects of the ACR format remain partially or fully undocumented:

| Area | Details |
|------|---------|
| **Monster Reference (0x07C)** | 4,091 non-null references do not match any SNO file type in the extracted dataset. Likely reference a Monster SNO type (`.mon` or similar) for AI behavior, loot tables, and stat blocks. |
| **MsgData Entry Details** | 412-byte entries have understood header (event type, two parameters), but 400 bytes of sparse config data are not fully mapped. Likely encode animation triggers, effect spawns, sound events, and state transitions. |
| **Tag Map Semantics** | Binary structure understood (4-byte count + 12-byte entries), but most tag IDs’ exact meanings are unknown. IDs use `0x0001XXXX` namespace. |
| **Extended Properties (0x2C4–0x36F)** | Field assignments inferred from value patterns. Navigation AABB (0x304–0x31C) and scale factors (0x2D0–0x2D8) are tentatively identified. |
| **Flags Bitfield (0x01C)** | Values 0–64 suggest at least 7 distinct flag bits, but specific effects on actor behavior are undocumented. |
| **Alt Physics Reference (0x2C4)** | Some resolve to PhysMesh snoIds, many reference IDs outside the available dataset. May reference collision or cloth simulation assets. |

---

## Appendix A — Reading an ACR File (C++)

```cpp
FILE* f = fopen("actor.acr", "rb");

// ── §3  SNO Preamble ──────────────────────────────────────────────────────────────
SnoPreamble preamble;
fread(&preamble, sizeof(SnoPreamble), 1, f);
assert(preamble.magic == 0xDEADBEEF);
assert(preamble.version == 282);

printf("SNO ID: 0x%08X  Flags: %u\n", preamble.snoId, preamble.flags);

// ── §5  Actor Header ──────────────────────────────────────────────────────────────
ActorHeader header;
fread(&header, sizeof(ActorHeader), 1, f);

printf("Type: %u  Appearance: 0x%08X\n", header.actorType, header.appearanceSno);

if (header.physMeshSno != 0xFFFFFFFF)
    printf("PhysMesh: 0x%08X\n", header.physMeshSno);
if (header.animSetSno != 0xFFFFFFFF)
    printf("AnimSet: 0x%08X\n", header.animSetSno);
if (header.monsterSno != 0xFFFFFFFF)
    printf("Monster: 0x%08X\n", header.monsterSno);

// ── §7  Bounding Volumes ──────────────────────────────────────────────────────────
printf("Cylinder: center=(%.3f, %.3f, %.3f) radius=%.3f height=%.3f\n",
       header.cylCenterX, header.cylCenterY, header.cylCenterZ,
       header.cylRadius, header.cylHeight);

// ── §11  Look Slots ───────────────────────────────────────────────────────────────
LookSlot looks[8];
fseek(f, 0x0A4, SEEK_SET);
fread(looks, sizeof(LookSlot), 8, f);

for (u32 i = 0; i < 8; i++) {
    if (looks[i].name[0] != '\0')
        printf("  Look %u: '%s' alpha=%u\n", i, looks[i].name, looks[i].alpha);
}

// ── §13  Tag Map ─────────────────────────────────────────────────────────────────
fseek(f, 0x380, SEEK_SET);  // tagMapOffset (0x370) + 16
u32 tagCount;
fread(&tagCount, sizeof(u32), 1, f);

for (u32 i = 0; i < tagCount; i++) {
    TagMapEntry entry;
    fread(&entry, sizeof(TagMapEntry), 1, f);
    printf("  Tag 0x%08X: type=0x%08X value=0x%08X\n",
           entry.tagId, entry.typeAndFlags, entry.value);
}

// ── §9  MsgData Array ─────────────────────────────────────────────────────────────
if (header.msgDataOffset > 0 && header.msgDataCount > 0) {
    fseek(f, header.msgDataOffset + 16, SEEK_SET);
    for (u32 i = 0; i < header.msgDataCount; i++) {
        MsgDataEntry entry;
        fread(&entry, sizeof(MsgDataEntry), 1, f);
        printf("  MsgData[%u]: type=%u paramA=0x%08X paramB=0x%08X\n",
               i, entry.eventType, entry.paramA, entry.paramB);
    }
}

fclose(f);
```

---

## Appendix B — All Structures Summary

```cpp
// §2 — Primitive Types
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using i32 = int32_t;
using f32 = float;
using SnoRef  = u32;    // 0xFFFFFFFF = null
using String64 = char[64];

// §3 — SNO Preamble
struct SnoPreamble {                            // 32 bytes
    u32 magic; u32 version; u32 _res008; u32 _res00C;
    u32 snoId; u32 _res014; u32 _res018; u32 flags;
};

// §5 — Actor Header
struct ActorHeader {                            // 128 bytes
    u32 actorType;
    SnoRef appearanceSno; SnoRef physMeshSno;
    f32 cylCenterX, cylCenterY, cylCenterZ, cylRadius, cylHeight;
    f32 aabbMinX, aabbMinY, aabbMinZ, aabbExtentA;
    f32 aabbMaxX, aabbMaxY, aabbMaxZ, aabbExtentB;
    f32 boundingSphereRadius; f32 _unk064;
    u32 tagMapOffset; u32 tagMapSize;
    u32 _res070; u32 _res074;
    SnoRef animSetSno; SnoRef monsterSno;
    u32 msgDataOffset; u32 msgDataSize; u32 msgDataCount;
    u32 _res08C; u32 _res090; u32 _res094;
    f32 walkSpeed; f32 runSpeedScale; f32 selectionRadius;
};

// §11 — Look Slot
struct LookSlot { char name[64]; u32 alpha; };  // 68 bytes

// §12 — Extended Actor Properties (see full struct for all fields)
struct ExtendedActorProperties { /* 172 bytes */ };

// §13 — Tag Map
struct TagMapHeader { u32 entryCount; };
struct TagMapEntry {                            // 12 bytes
    u32 tagId; u32 typeAndFlags; u32 value;
};

// §9 — MsgData Entry
struct MsgDataEntry {                           // 412 bytes
    u32 eventType; u32 paramA; u32 paramB; u32 paramC;
    u8 data[396];
};
```

---

*Specification derived from binary analysis of 19,177 ACR files from Diablo III: Reaper of Souls.*
