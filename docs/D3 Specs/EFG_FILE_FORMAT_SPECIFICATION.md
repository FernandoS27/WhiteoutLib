# EFG File Format Specification

**Format**: Diablo III Effect Group (`.efg`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**SNO Group**: 14
**Version**: 47
**Corpus**: 6,426 files / 30,463 effect items analyzed
**Registered revision**: 51 — the shipped data is v47, so the binary's compiled struct describes a *newer* layout (see below / README §4)

See [README.md](README.md) for the build these offsets come from, the generator pipeline
and the conventions used below.

---

## Table of Contents

1. [Overview](#1-overview)
2. [SNO Header and Struct-Relative Offsets](#2-sno-header-and-struct-relative-offsets)
3. [EffectGroup Root Struct](#3-effectgroup-root-struct)
4. [Selection Modes](#4-selection-modes)
5. [EffectItem](#5-effectitem)
6. [MsgTriggeredEvent and TriggerEvent](#6-msgtriggeredevent-and-triggerevent)
7. [Links: Hardpoint, Look, Constraint](#7-links-hardpoint-look-constraint)
8. [SNO References](#8-sno-references)
9. [Version Skew: v47 vs the Binary's Revision 51](#9-version-skew-v47-vs-the-binarys-revision-51)
10. [Corpus Statistics](#10-corpus-statistics)
11. [Known Unknowns](#11-known-unknowns)

---

## 1. Overview

An Effect Group is D3's indirection layer between "something happened" and
"play these assets". It holds a list of **EffectItem**s, each of which is a
weight plus a full **TriggerEvent** describing one asset to spawn — a particle
system, a sound, a screen shake, an explosion, a nested effect group, and so on
— together with the hardpoint to attach it to, a delay/chance gate, a tint and
a duration.

The group itself carries a **selection mode** that decides which of its items
actually fire: all of them, one weighted-random pick, a random repeat count, the
one matching the actor's appearance look-link, or the one matching the active
rune of a Power.

Everything in this document is either read out of the shipping binary
(`DiabloIIINX64r 2.6.2`, Nintendo Switch `exefs/main`) or measured over the full
6,426-file corpus; each claim carries its count.

---

## 2. SNO Header and Struct-Relative Offsets

A `.efg` is the standard D3 SNO container:

```
+0x00  u32 magic       0xDEADBEEF        (6,426/6,426)
+0x04  u16 version     47                (6,426/6,426)
+0x06  u16 reserved    0                 (6,426/6,426)
+0x08  u64 reserved    0                 (6,426/6,426)
+0x10  EffectGroup struct (120 bytes)
+0x88  payload: EffectItem[count]
```

**Every offset stored inside the file is struct-relative**: file position =
`16 + offset`. The root struct's array descriptor stores `120`, which is file
position `136` — immediately after the struct.

`file size == 16 + 120 + SerializeData.size` in **6,426/6,426** files.

---

## 3. EffectGroup Root Struct

120 bytes. The struct size is **identical in the shipped revision (47) and in
the revision the 2.6.2 binary registers (51)**, so no version fix-up is needed
for the root — unlike Actor, Appearance, Particle, ShaderMap and Textures.

| offset | size | field | type | notes |
| --- | --- | --- | --- | --- |
| 0x00 | 4 | `dwSnoId` | u32 | unique across 6,426/6,426 files |
| 0x04 | 8 | *(reserved header words)* | — | 0 in 6,426/6,426 |
| 0x0C | 4 | `dwFlags` | u32 | **bit 0 = do not repeat an item until all have played**. `1` in 8 files, `2` in 7, `0` in 6,411 |
| 0x10 | 4 | `arEffectItems.offset` | u32 | 120 in 6,388 files, 0 in the 38 empty ones |
| 0x14 | 4 | `arEffectItems.size` | u32 | `size / count == 480` in 6,388/6,388 |
| 0x18 | 4 | `dwEffectItemCount` | u32 | 0..28 |
| 0x1C | 4 | *(padding)* | — | 0 in 6,426/6,426 |
| 0x20 | 8 | *(runtime pointer)* | — | 0 in 6,426/6,426 |
| 0x28 | 4 | `nRepeatMin` | i32 | read only by selection mode 1 |
| 0x2C | 4 | `nRepeatMax` | i32 | read only by selection mode 1 |
| 0x30 | 4 | `eSelectMode` | i32 | see §4; default 2 |
| 0x34 | 4 | `snoPower` | i32 (SNO, group 29 = Power) | −1 in 5,926 files |
| 0x38 | 64 | `dwPlayedItemMask[16]` | u32[16] | runtime bitmask; **0 in 102,816/102,816 words** |

The array group at 0x10..0x27 is D3's usual
`{SerializeData(8), count(4), pad(4), pointer(8)}`. The binary registers the
array field at +32 with a `rawRel` of −16 (its `SerializeData` lives at +16) and
the count at +24 with a `rawRel` of −8.

### The bitmask and the no-repeat flag

`EffectGroup_PickWeightedRandomItem` (0x710008C7F0) reads `dwFlags` as a byte:

```c
if (*(_BYTE *)(pGroup + 0x0C) & 1) {
    // candidates = items whose bit is CLEAR in the mask
    if ((*(u32 *)(pGroup + 4*(i>>5) + 0x38) & (1 << (i & 31))) == 0) { ... }
    // after picking:
    *(u32 *)(pGroup + 4*(pick>>5) + 0x38) |= 1 << (pick & 31);
    // when only one candidate remains, clear the mask
}
```

The mask is asset-resident scratch, which is why it is zero in every shipped
file. (Engine quirk: the reset clears only 16 bytes — 4 of the 16 dwords — so
items ≥ 128 would never be re-enabled. Harmless: the largest group has 28 items.)

---

## 4. Selection Modes

`EffectGroup_Play` (0x710008B230) fetches the group by
`TriggerEvent.SNOName.handle` and then does `switch (pGroup[0x30])`.

The switch has cases `0, 1, 2, 3, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17` —
**4 and 6 are missing, and 4 and 6 never occur in the corpus either.** Every one
of the 6,426 files carries a mode the dispatcher handles.

| mode | files | behaviour |
| --- | --- | --- |
| 0 | 79 | play ONE item, weighted-random over `EffectItem.nWeight` |
| 1 | 8 | repeat that pick `rand() % ((nRepeatMax − nRepeatMin) + 1) + nRepeatMin` times |
| **2** | **4,888** | play EVERY item, `i = 0 .. dwEffectItemCount−1` — the constructor default |
| 3 | 1 | actor-driven |
| 5 | 10 | actor-driven |
| 7 | 8 | actor-driven |
| 9 | 169 | actor-driven, requires items |
| 10 | 73 | play the item whose `szLookLink` matches the actor's Appearance look link |
| 11 | 1 | actor-driven |
| 12 | 138 | actor-driven |
| 13 | 667 | actor-driven (all 667 have exactly 2 items) |
| 14 | 2 | actor-driven |
| 15 | 280 | **rune selector**: test attributes 690..694 against `snoPower`, play item 1..5, else item 0 |
| 16 | 102 | actor-driven (6 or 7 items) |

`EffectGroupHandler_ConstructDefaultAsset` (0x71001ABA90) zero-fills the 120-byte
struct and stores `2` at 0x30 — matching the 4,888 files that use mode 2.

Mode 1 is confirmed by the data: **all 8** mode-1 files carry a non-zero
`nRepeatMin`/`nRepeatMax` pair with `min ≤ max` (1..3, 1..2, 4..6, 1..1, 3..5),
while only 12 of the other 6,418 files have a non-zero pair at all.

Mode 15 is confirmed by the data: 262 of its 280 files set `snoPower`
(the other 222 `snoPower` users are mode 2).

---

## 5. EffectItem

**480 bytes** on disk (`SerializeData.size / count == 480` in 6,388/6,388
non-empty files).

| offset | size | field | notes |
| --- | --- | --- | --- |
| 0x000 | 4 | `nWeight` | random-selection weight; `100` in 30,254/30,463 |
| 0x004 | 64 | `szLookLink[64]` | appearance look-link tag; empty in 30,024/30,463 |
| 0x044 | 412 | `tEvent` (`MsgTriggeredEvent`) | see §6 |

### `nWeight`

`EffectGroup_PickWeightedRandomItem` sums `*(u32 *)(arItems + stride*i)` across
the candidates, draws `rand() % total`, and walks the running sum. Corpus values
are `100` (30,254), then 50, 30, 20, 15, 10, 3, 0 — a 0..100 weight, uniform by
default. Five items hold `0xDEADBEEF`, which is authoring garbage.

### `szLookLink`

Selection mode 10 compares this field's hash against a look-link name hash taken
from the actor's Appearance. The corpus agrees exactly: `szLookLink` is
non-empty in **73 of 73** mode-10 files, and in only 12 of the other 6,353.
Values are D3 look-link tags — `A`, `B`, `C`, `D`, `E`, `A_rare`,
`A_champion` — 95 distinct strings.

---

## 6. MsgTriggeredEvent and TriggerEvent

`MsgTriggeredEvent` is **412 bytes**: a leading `i32` (constant `5000` in
30,463/30,463 items) followed by a **408-byte `TriggerEvent`**.

This is the same structure Particle, Actor, Rope and Scene embed in their
`arTriggeredEvents` arrays. Independent confirmation of the 412: over the 1,388
`.prt` files (v180) that populate that array, the **gcd of every
`SerializeData.size` is exactly 412**.

### TriggerEvent — offsets given relative to the EffectItem

| item offset | TE offset | field | type | notes |
| --- | --- | --- | --- | --- |
| 0x048 | +0 | `eEventType` | i32 | **16 = play an EffectGroup**; default 25 |
| 0x04C | +4 | `tConditions` | `TriggerConditions` (36) | see below |
| 0x070 | +40 | `eTargetKind` | i32 | mirrors the target group; default 1 |
| 0x074 | +44 | `tTarget.eSnoGroup` | i32 | SNO group id, −1 = none |
| 0x078 | +48 | `tTarget.snoHandle` | i32 | SNO id in that group, −1 = none |
| 0x07C | +52 | `nInstanceTag` | i32 | runtime tracking tag; 0 in 30,076/30,463 |
| 0x080 | +56 | *(unknown)* | i32 | 1 in 2/30,463 |
| 0x084 | +60 | `nRuneIndexOverride` | i32 | never serialised; 0 in 30,463/30,463 |
| 0x088 | +64 | `bUseRuneOverride` | i32 | never serialised; 0 in 30,463/30,463 |
| 0x08C | +68 | `tHardpoint0` | `HardpointLink` (68) | §7 |
| 0x0D0 | — | `tHardpoint1` | `HardpointLink` (68) | §7 |
| 0x114 | +92 | `tLook` | `LookLink` (64) | §7 |
| 0x154 | +100 | `tConstraint` | `ConstraintLink` (64) | §7 |
| 0x194 | +108 | `dwFlags` | u32 | default `0x11000` (bits 12 and 16) |
| 0x198 | +112 | *(float)* | f32 | 0.0 in 30,463/30,463 |
| 0x19C | +116 | *(unknown)* | i32 | non-zero in 4/30,463 |
| 0x1A0 | +120 | *(unknown)* | i32 | default 4 (30,266); 2 in 189 |
| 0x1A4 | +124 | `dwFlags2` | u32 | default 2; callers OR in `0x2000` |
| 0x1A8 | +128 | *(unknown)* | i32 | 1 in 247/30,463 |
| 0x1AC | +132 | *(unknown)* | i32 | 1 in 3,300/30,463 |
| 0x1B0 | +136 | `flScale` | f32 | default 1.0f; 1.0 in **30,463/30,463** |
| 0x1B4 | +140 | *(float)* | f32 | default 2.0f; 2.0 in 30,459 |
| 0x1B8 | +144 | *(gate)* | i32 | pairs with the next field in 96.2 % of items |
| 0x1BC | +148 | *(float)* | f32 | 0/1.0/0.5/0.25/0.75/10/20/1.5 … |
| 0x1C0 | +152 | *(unknown)* | i32 | 0 in 30,463/30,463 |
| 0x1C4 | +156 | `flVelocity` | f32 (`DT_VELOCITY`) | 0.0 in 30,463/30,463 |
| 0x1C8 | +160 | *(unknown)* | i32 | 0 in 30,463/30,463 |
| — | +164 | *(does not exist in v47)* | — | added after the shipped revision |
| 0x1CC | +168 | `tDuration` | i32 (`DT_TIME`) | default 600; 600 in 30,357 |
| 0x1D0 | +172 | `dwColor0` | RGBA8 `{r,g,b,a}` | 0xFF000000 in 15,754, 0xFF808080 in 11,905 |
| 0x1D4 | +176 | `tColor0Time` | i32 (`DT_TIME`) | default 15; 15 in 14,239 |
| 0x1D8 | +180 | `dwColor1` | RGBA8 `{r,g,b,a}` | 0xFFFFFFFF in 18,127, 0xFF808080 in 11,888 |
| 0x1DC | +184 | `tColor1Time` | i32 (`DT_TIME`) | default 15; 15 in 14,185 |

`DT_RGBACOLOR` is registered (`sub_7100980940`) as four byte members
`r`, `g`, `b`, `a` at offsets 0, 1, 2, 3 — so the little-endian dword
`0xFF808080` is `r=0x80 g=0x80 b=0x80 a=0xFF`.

### TriggerConditions — 36 bytes, at item offset 0x04C

| item offset | field | type | notes |
| --- | --- | --- | --- |
| 0x04C | `nChance` | `DT_PERCENT` | **one byte, 0..255**; 255 in 29,574, 0 in 813, graded values 127/153/51/229/140/63 in the rest. The three bytes above it are zero in 30,463/30,463 |
| 0x050 | *(time)* | `DT_TIME` | non-zero in 3,734/30,463; values 6/12/3/9/18/30/15 (29,899 are multiples of 3) |
| 0x054 | *(time)* | `DT_TIME` | non-zero in 566/30,463 |
| 0x058 | *(time)* | `DT_TIME` | non-zero in 3/30,463 |
| 0x05C | *(time)* | `DT_TIME` | non-zero in 2/30,463 |
| 0x060 | *(impulse)* | `DT_IMPULSE` | non-zero in 2/30,463 |
| 0x064 | *(impulse)* | `DT_IMPULSE` | non-zero in 3/30,463 |
| 0x068 | *(unknown)* | i32 | non-zero in 6/30,463 |
| 0x06C | *(unknown)* | i32 | non-zero in 1/30,463 |

### `eEventType` = 16 means "play an EffectGroup"

Three independent engine sites build a TriggerEvent that plays an effect group —
`EffectGroup_PlayOnActor` (0x710008CA50), `EffectGroup_PlayOnActorTracked`
(0x710008CB90) and the marker path at 0x710034AFC0 — and all three write:

```c
TriggerEvent_SetDefaults(ev);
ev[0]  = 16;                 // eEventType
ev[11] = 14;                 // tTarget.eSnoGroup  (14 = EffectGroup)
ev[12] = snoEffectGroup;     // tTarget.snoHandle
```

Corpus: `eEventType == 16` in **4,747** items, `eSnoGroup == 14` in **the same
4,747** — a perfect 1:1. Other exact pairings: `11 ↔ Rope` (199/199) and
`4 ↔ Trail` (60/60). Values 32, 13, 40, 14, 26, 7, 42, 6, 39, 9, 10 appear
**only** on the 1,290 records with a null SNO reference — assetless actions.

---

## 7. Links: Hardpoint, Look, Constraint

In revision 51 each of these stores an 8-byte name **hash**; the shipped
revision 47 stores the **literal string** in an inline `char[64]`. This is
visible directly in the data — the corpus holds `"Default"`, `"HP_chest"`,
`"HP_leftWeapon"`, and every one of the five `char[64]` runs is NUL-terminated
printable ASCII with a zero-filled tail in **30,463/30,463** elements.

| type | v47 layout | size |
| --- | --- | --- |
| `HardpointLink` | `char szHardpoint[64]; i32 nFlags;` | 68 |
| `LookLink` | `char szLook[64];` | 64 |
| `ConstraintLink` | `char szConstraint[64];` | 64 |

`TriggerEvent` holds a `HardpointLink[2]` fixed array, then one `LookLink`, then
one `ConstraintLink`.

`MsgTriggeredEvent_RegisterTypeDescriptors` (0x710060D280) interns roughly 60
hardpoint literals, and `TriggerEvent_GetPredefinedHardpointHash` (0x710060CF50)
maps index 0..18 onto the first 19:

```
0 Default          1 HP_chest        2 HP_mouth        3 HP_leftHand
4 HP_rightHand     5 HP_ManaGather   6 HP_back         7 HP_head
8 HP_leftFoot      9 HP_rightFoot   10 HP_pelvis      11 HP_leftWeapon
12 HP_rightWeapon 13 HP_Emitter     14 HP_Emitter_Floor
15 HP_Explosion   16 HP_beamCenter  17 HP_beamLeft    18 HP_beamRight
```

At runtime `Appearance_FindHardpointIndex` (0x71001AB1D0) scans the Appearance's
hardpoint array by hash and falls back to `Default` when the request is unset.

Corpus:

| field | distinct | interned-name uses | most common |
| --- | --- | --- | --- |
| `tHardpoint0.szHardpoint` | 42 | 24,318 / 30,463 | `Default` 18,322 · `Don't Override` 3,853 · `HP_chest` 3,831 · empty 2,254 |
| `tHardpoint1.szHardpoint` | 37 | 18,820 / 30,463 | `Default` 17,858 · empty 8,523 · `Don't Override` 2,822 |
| `tLook.szLook` | 203 | — | empty 29,932 · `A_death` 79 · `A` 28 · `Invisible` 23 |
| `tConstraint.szConstraint` | 1 | — | **empty in 30,463/30,463** |

Names outside the interned set are per-Appearance hardpoints
(`HP_left_shoulder`, `HP_right_wrist_mid`, `Tail`, `Head`) plus editor sentinels
(`Don't Override`, `- None -`, `None`).

---

## 8. SNO References

`tTarget` is the engine's `SNOName` type: `{ i32 eSnoGroup; i32 snoHandle; }`.
Every group id observed is a real D3 SNO group, and where a corpus for that
group exists, the handles resolve inside it and nowhere else.

| `eSnoGroup` | group | refs | unique | resolves in that group | resolves elsewhere |
| --- | --- | --- | --- | --- | --- |
| 27 | **Particle** | 16,943 | 11,850 | **16,942 / 16,943** (11,849/11,850 unique `.prt`) | 0 in Actor, 0 in EffectGroup |
| 40 | Sound | 5,313 | 1,954 | *(no corpus)* | — |
| 14 | **EffectGroup** | 4,747 | 3,146 | **4,747 / 4,747** (3,146/3,146 unique `.efg`) | 0 in Particle, 0 in Actor |
| −1 | *(none)* | 1,290 | 1 | `snoHandle == −1` in **1,290/1,290** | — |
| 17 | Explosion | 825 | 104 | *(no corpus)* | — |
| 38 | Shakes | 573 | 113 | *(no corpus)* | — |
| 32 | Rope | 199 | 94 | *(no corpus)* | — |
| 5 | AmbientSound | 198 | 83 | *(no corpus)* | — |
| 1 | **Actor** | 162 | 142 | **162 / 162** (142/142 unique `.acr`) | 0 in Particle, 0 in EffectGroup |
| 68 | Vibrations | 146 | 8 | *(no corpus)* | — |
| 45 | Trail | 60 | 28 | *(no corpus)* | — |
| 23 | Light | 7 | 6 | *(no corpus)* | — |

`EffectGroup.snoPower` is a separate reference, registered as `DT_SNO` with
**enum-table id 29 = the Power group**. Selection mode 15 feeds it to the five
rune attributes (690..694) to pick an item index. It is set in 500 files with
163 distinct ids, none of which collides with the Particle, Actor, Appearance,
Material, AnimSet, Anim, PhysMesh or EffectGroup id maps.

---

## 9. Version Skew: v47 vs the Binary's Revision 51

The 2.6.2 binary registers revision 51 field descriptors, not the shipped
revision 47. Reading one onto the other needs the `0x700000` rule: a field whose
registration flags carry `0x700000` did not exist in the shipped revision, and
where the new revision stores an 8-byte name hash the old one stores an inline
`char[64]` (−8 +64 = +56 per name).

| type | rev 51 | rule | v47 | corpus |
| --- | --- | --- | --- | --- |
| `HardpointLink` | 12 | −8 +64 | 68 | 68 |
| `LookLink` | 8 | −8 +64 | 64 | 64 |
| `ConstraintLink` | 8 | −8 +64 | 64 | 64 |
| `TriggerConditions` | 36 | — | 36 | 36 |
| `TriggerEvent` | 188 | +2×56 +56 +56 −4 | 408 | — |
| `MsgTriggeredEvent` | 192 | 4 + TriggerEvent | 412 | **412** (Particle gcd) |
| `EffectItem` | 200 | −8 +64 | 480 | **480** |
| `EffectGroup` | 120 | — | 120 | 120 |

The lone `−4` is the `i32` at `TriggerEvent+164`, whose flags are `0x700001`.
Both deltas it produces are observable: fields at rev51 +136/+140 sit at
`+296` from their v47 position, while +168/+176/+184 sit at `+292`.

`TriggerEvent_SetDefaults` (0x710060CDD0) is the alignment proof. Every one of
the 20 constants it writes is the dominant value at the matching corpus offset:

| rev51 offset | value | corpus offset (in EffectItem) | count |
| --- | --- | --- | --- |
| +0 | 25 | 0x048 | 9,042 |
| +4 (byte) | 255 | 0x04C | 29,574 |
| +40 | 1 | 0x070 | 21,758 |
| +68 / +80 | hash("Default") | 0x08C / 0x0D0 = `"Default"` | 18,322 / 17,858 |
| +108 | 0x11000 | 0x194 | 30,454 |
| +120 | 4 | 0x1A0 | 30,266 |
| +136 | 1.0f | 0x1B0 | 30,463 |
| +140 | 2.0f | 0x1B4 | 30,459 |
| +168 | 600 | 0x1CC | 30,357 |
| +172 | 0xFF000000 | 0x1D0 | 15,754 |
| +180 | 0xFFFFFFFF | 0x1D8 | 18,127 |

Registered *field* defaults (registrar argument 4) land on the same offsets:
1.0f → 0x1B0, 2.0f → 0x1B4, 600 → 0x1CC, 15 → 0x1D4 and 0x1DC, and 0xFF808080
→ 0x1D0 and 0x1D8, where 0xFF808080 is the second most common value at both.
**Both defaults — the editor's and the code's — are visible in the shipped data.**

---

## 10. Corpus Statistics

* 6,426 files. Magic and version 47 in 6,426/6,426.
* `file size == 16 + 120 + SerializeData.size` in 6,426/6,426.
* 30,463 EffectItems; element size 480 in 6,388/6,388 non-empty files.
* 38 files carry zero items (file size exactly 136).
* Item counts 0..28. Most common: 2 (1,760 files), 6 (657), 7 (629), 3 (618),
  4 (610), 1 (571), 5 (433), 10 (290), 8 (246), 9 (202), 12 (113), 11 (99).
* The 480-byte `EffectItem` tiling covers **480/480 bytes with no gap and no
  overlap**.
* Root reserved words (+4, +8, +28, +32, +36) zero in 6,426/6,426.
* `dwPlayedItemMask` zero in **102,816/102,816** words.
* All five `char[64]` runs clean (NUL-terminated printable ASCII, zero-filled
  tail) in **30,463/30,463** elements.
* `DT_PERCENT`'s upper three bytes zero in 30,463/30,463.

---

## 11. Known Unknowns

* `MsgTriggeredEvent`'s leading `i32` is `5000` in 30,463/30,463 items. Being
  constant, the corpus cannot distinguish a version stamp from a duration or a
  message id. **Not established.**
* `TriggerEvent.eEventType`: only `16` (EffectGroup), `11` (Rope) and `4` (Trail)
  are pinned; `25` is the constructor default; `0` covers Particle/Sound/Actor
  records; and 32/13/40/14/26/7/42/6/39/9/10 appear only on assetless records.
  The full enum is not recovered.
* `TriggerEvent.eTargetKind` (+40) is a near-deterministic mirror of the target
  group — `1↔Particle` 16,943/16,943, `3↔Sound` 5,313/5,313, `6↔Explosion`
  825/825, `11↔Shakes` 573/573, `5↔AmbientSound` 198/198, `12↔Vibrations`
  146/146, `2↔Light` 7/7, `0↔Actor` 162/162 — but EffectGroup, Rope and Trail
  records disagree with themselves. **Not established.**
* `TriggerConditions` members past `nChance`: registered types are known
  (4× `DT_TIME`, 2× `DT_IMPULSE`, 2× `i32`), no engine reader located, corpus
  sparse.
* `TriggerEvent+108` and `+124` flag words: only the defaults (`0x11000`, `2`)
  and the `0x2000` bit at +124 (set by `EffectGroup_PlayOnActor`) have a
  provable meaning. The remaining bits — +108 bits 0/4/5/6/7/8/18 (1–3
  occurrences each), +124 bits 0/2/3/7/8/10/11/12/13 — are unnamed.
* `TriggerEvent+144/+148` behave like a gate/value pair (29,305 of 30,463 items
  agree, 96.2 %) but the 1,158 exceptions keep it a correlation.
* `HardpointLink.nFlags` is 1 in 29 items (slot 0) and 575 (slot 1); no engine
  reader located.
* `EffectGroup.dwFlags` bit 1 (7 files) has no engine reader; only bit 0 does.
* Selection modes 3, 5, 7, 8, 9, 11, 12, 13, 14, 16 and 17 all take the
  actor-driven path in `EffectGroup_Play` and were not walked in detail.
* `snoPower`'s membership in the Power group rests on the registered enum-table
  id (29) plus zero collisions with eight other groups' id maps; no Power corpus
  exists to confirm it positively.
