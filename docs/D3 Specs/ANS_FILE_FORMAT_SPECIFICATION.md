# ANS File Format Specification

**Format**: Diablo III AnimSet (`.ans`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**Version**: 24
**Corpus**: 3,212 files analyzed (all of them, in the 2026-08-15 revision)
**SNO Group**: 8 (`AnimSet`)
**Registered revision**: 28 — the shipped data is v24, so the binary's compiled struct describes a *newer* layout (see below / README §4)

See [README.md](README.md) for the build these offsets come from, the generator pipeline
and the conventions used below.

---


> ## ⚠ REVISION 2026-08-15 — the slot model in the original document was wrong
>
> This document previously described **29 anonymous slots** that "appear to
> represent visual variants of the entity (e.g., different equipment
> configurations)". That is not what they are, and there are not 29 of them.
>
> The Switch 2.6.2 binary registers the struct in `AnimSet_RegisterTypes`
> (`0x710060CA20`), and it is **one core tag map plus a fixed array of 28**:
>
> ```
>  16  tCoreTagMap                AnimSetTagMap      — hand-to-hand / default
>  32  arWeaponClassTagMap[28]    AnimSetTagMap[28]  — indexed by eWeaponClass
> ```
>
> The array is indexed by the engine's **`eWeaponClass`** enum, which survives
> with its names intact at `g_WeaponClassEnumTable` (`0x71010C0860`):
> `WEAPONCLASS_HTH = 0` … `WEAPONCLASS_ON_HORSE = 27`, `NUM_WEAPON_CLASSES = 28`.
> See §8, which now carries the full table and the corpus evidence for it.
>
> **Corrections made in this revision**, each marked `[CORRECTED 2026-08-15]`
> in the body:
>
> | § | was | is |
> | --- | --- | --- |
> | 1 | 29 slots = "visual variants" | 1 core map + 28 weapon-class maps |
> | 3 | `_reserved08[2]`, `_reserved14[3]` | `snoBaseAnimSet` identified at file `0x1C` |
> | 4 | `_pad[2]` = "padding (zeros)" | the nulled runtime payload pointer |
> | 6 | `entryType` "possibly a discriminator" | `nValueType`, D3 TagMap: 0 int / 1 float / **2 SNO ref** |
> | **7** | **a 18-row table of guessed tag meanings** | **REMOVED — names are stripped from retail and are not recoverable** |
> | 8 | slot-usage guesswork from a 500-file sample | the `eWeaponClass` enum, whole corpus |
> | 9 | 500-file sample statistics | whole-corpus statistics |
>
> The §7 tag dictionary (`0x011000 = Idle`, `0x011010 = Walk / movement`,
> `0x011050 = Death`, …) was **invented**. Those rows are frequency rankings, not
> meanings, and several of the "player" rows (`0x040000`, `0x063A00`,
> `0x064205`, `0x080106`) are not tag ids that occur in the corpus at all.
> They have been deleted rather than corrected.

---

## Table of Contents

1. [Overview](#1-overview)
2. [File Layout](#2-file-layout)
3. [SNO Preamble](#3-sno-preamble)
4. [Tag Map Block Table](#4-tag-map-block-table)
5. [Tag Map Blocks](#5-tag-map-blocks)
6. [Tag Map Entries](#6-tag-map-entries)
7. [Tag ID System](#7-tag-id-system)
8. [The Weapon Class Array](#8-the-weapon-class-array)
9. [Corpus Statistics](#9-corpus-statistics)
10. [Cross-References](#10-cross-references)
11. [Known Unknowns](#11-known-unknowns)
12. [Appendix A — Reading an ANS File (C++)](#appendix-a--reading-an-ans-file-c)
13. [Appendix B — All Structures Summary](#appendix-b--all-structures-summary)

---

## 1. Overview

AnimSet files define **animation state lookup tables** for Diablo III entities.
Each AnimSet maps animation state tags to specific `.ani` animation clips.

**[CORRECTED 2026-08-15]** An AnimSet holds **one core tag map plus 28
weapon-class tag maps**. The core map is the complete, default mapping (authored
from the entity's hand-to-hand animations). Each of the 28 array entries is an
**override map for one weapon class**: an entry whose animation reference is
`0xFFFFFFFF` carries no override for that tag.

The animation pipeline:

```
AnimTree (.ant)  →  AnimSet (.ans)  →  Animation (.ani)
  state machine      state→clip map      keyframe data

Engine: "Play idle animation, actor is wielding a two-handed sword"
  → AnimSet.arWeaponClassTagMap[WEAPONCLASS_2HSWING] → find tag 0x11000
  → if the ref is 0xFFFFFFFF, fall back to AnimSet.tCoreTagMap → load .ani
```

> The fallback step is **inferred, not proven**: the engine-side accessor was not
> located in the 2.6.2 binary. What the corpus proves is that 93.6% of array
> entries are `0xFFFFFFFF` and that every non-null array entry's tag is also
> present in the core map (24,638 / 24,638, no exceptions).

---

## 2. File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO Header                                 (16 bytes)  │
│    0x000: magic, version, reserved                      │
├─────────────────────────────────────────────────────────┤
│  AnimSet struct image                      (480 bytes)  │
│    0x010: dwSnoId, snoBaseAnimSet          (§3)         │
│    0x020: 29 × 16-byte tag map blocks      (§4)         │
├─────────────────────────────────────────────────────────┤
│  Payload                                     (variable) │
│    0x1F0: tightly packed tag map data      (§5)         │
└─────────────────────────────────────────────────────────┘
```

**Every stored offset is struct-relative: file position = 16 + offset.** The
struct begins at file `0x010`, so the 29 blocks at struct `+16 … +480` occupy
file `0x020 … 0x1F0`, and the payload starts at file `0x1F0`.

The payload blocks are tightly packed in block order (core first, weapon class 27
last). Verified: the last block ends exactly at EOF in **3,212 / 3,212** files.

---

## 3. SNO Preamble

**Tag**: ANS | **Version**: 24 | **Struct size**: 480

```cpp
struct AnimSetHeader {
    u32     magic;              // file 0x00: always 0xDEADBEEF
    u32     version;            // file 0x04: always 24 for .ans
    u32     _reserved08[2];     // file 0x08: SNO header, not part of the struct
    // ---- the AnimSet struct starts here, at file 0x10 ----
    u32     snoId;              // struct +0  : unique SNO identifier
    u32     _unknown04;         // struct +4  : not established
    u32     _unknown08;         // struct +8  : not established
    s32     snoBaseAnimSet;     // struct +12 : SNO ref to another AnimSet, -1 = none
};
```

**[CORRECTED 2026-08-15]** The word at file `0x01C` (struct +12), previously
listed inside `_reserved14[3]`, is **`snoBaseAnimSet`** — a reference to another
AnimSet. `AnimSet_RegisterTypes` registers it through the enum registrar with
SNO **group 8** (AnimSet). Corpus: it is −1 in 2,834 of 3,212 files; of the 170
distinct other values, **all 170 are known AnimSet ids and none is an Anim id**.

---

## 4. Tag Map Block Table

**Location**: file `0x020` – `0x1EF` (struct `+16` – `+480`)
**Size**: 464 bytes — **1 core block + 28 weapon-class blocks**, 16 bytes each

```cpp
struct AnimSetTagMap {                          // 16 bytes — the engine's name
    u32     dataOffset;         // +0x00: struct-relative offset of the payload
    u32     dataSize;           // +0x04: payload size in bytes
    void*   pRuntimePayload;    // +0x08: nulled on disk, filled in at load
};
```

**[CORRECTED 2026-08-15]** The 8 bytes at +0x08 are **not padding**. They are the
runtime payload pointer of a D3 variable array. The engine registers the TagMap
field at +8 with `rel = -8`, meaning its `SerializeData {offset, size}` companion
sits at the block base — which is exactly why reading `{offset, size}` from +0
works. Verified zero in **93,148 / 93,148** blocks.

**Layout of the 29 blocks:**

| block | struct offset | meaning |
| --- | --- | --- |
| 0 | +16 | `tCoreTagMap` — the default / hand-to-hand map |
| 1 … 28 | +32 + 16·*wc* | `arWeaponClassTagMap[wc]`, *wc* = 0 … 27 (§8) |

`32 + 28 × 16 = 480` — the struct size exactly.

**Offset calculation**: payload begins at `dataOffset + 16` (file position).

**Empty blocks**: `dataSize = 4` and the count word is 0.
**Populated blocks**: `dataSize = 4 + entryCount × 12`.

---

## 5. Tag Map Blocks

Each block's payload is a **D3 TagMap** — the same format the engine uses for
Material and Appearance shader parameters:

```cpp
struct TagMap {
    u32         entryCount;                     // +0x00
    TagMapEntry entries[entryCount];            // +0x04
};
```

**Total size**: `4 + entryCount × 12` bytes. Verified in **93,148 / 93,148**
blocks with no exceptions.

---

## 6. Tag Map Entries

**Size**: 12 bytes per entry

```cpp
struct TagMapEntry {                            // 12 bytes
    u32     nValueType;         // +0x00: 0 = int, 1 = float, 2 = SNO reference
    u32     dwTagId;            // +0x04: animation state tag (§7)
    u32     dwValue;            // +0x08: for AnimSet, always an Anim SNO ref
                                //        0xFFFFFFFF = no override for this tag
};
```

**[CORRECTED 2026-08-15]** `entryType` is the generic D3 TagMap **`nValueType`**
discriminator, not an AnimSet-specific field. It is `2` (SNO reference) in
**405,581 / 405,581** AnimSet entries — expected, because all 453 registered ids
in the animation namespace share one value-type descriptor. The other values do
occur in other TagMap consumers (`.mat` files use 0 and 1 as well).

---

## 7. Tag ID System

**[CORRECTED 2026-08-15] The table of guessed tag meanings that stood here has
been removed. Tag names are not recoverable from a retail build.**

Tag ids are drawn from a **per-namespace, closed** id space. The engine has
`TagMap_GetNamespaceTable` (`0x71006A2360`), which maps a namespace token to a
table of 64-byte records via `case = (token - 0x10000) >> 16`.

**AnimSet's namespace is `0xC0000` (case 11).** This is stated by the binary
itself: `AnimSet_RegisterTypes` passes `786432 = 0xC0000` as the namespace
argument of `TypeDesc_RegisterField_TagMap` for both of its TagMap fields.

| | |
| --- | --- |
| table | `g_AnimTagTable` = `0x71010729F0` |
| count | `g_AnimTagCount` = `0x7100E577B0` = **453 ids** |

The namespace is closed and the fit is exact: those 453 ids cover
**397 / 397** distinct tag ids and **405,581 / 405,581** entries in the corpus.
No other namespace comes close — case 0 covers 26/397, case 1 covers 7/397, and
every other case 6/397 or fewer. 56 of the 453 ids are registered but unused by
shipped data.

### 7.1 Record layout

Pinned by `TagMap_FindTagIdByName` (`0x71006A2530`), which walks a `const void**`
at `v5 += 8` (= 64-byte stride) and `strcasecmp`s `v5[3]` (= +24):

| offset | field |
| --- | --- |
| +0 | `u32 tagId` |
| +4 | `u32 valueTypeCode` — 3 for all 453 animation records |
| +8 | pointer to the value's type descriptor — one shared pointer for all 453 |
| +24 | `const char* name` — **stripped in retail** |
| +48 | `u32 category` — exactly 8 values, 63 … 70; meaning not established |
| +52 | `u32 pairedTagId` — −1 for 417 records, an adjacent sibling id for 36 |

### 7.2 Why names are not recoverable

All 453 records' name pointers point at the **same empty string**
(`0x7100D6A4A3`) — as does every record in every other namespace, Material's 23
included. `TagMap_FindTagIdByName` therefore cannot succeed in this build.
Recovering tag names needs a build that retains them, or the client's string
tables.

`0x11000` is the most frequently used tag (54,374 of 405,581 entries) and is the
first record in the table, but **that is frequency and table order, not
evidence of meaning**.

### 7.3 The 36 paired ids

36 records point at a sibling, always an adjacent id — `0x11070 ↔ 0x11071`,
`0x11060 ↔ 0x11061`, `0x110C4 ↔ 0x110C3`, `0x111D1 ↔ 0x111D0`. Enter/loop or
left/right pairing is the natural reading but is **inferred from id adjacency and
is not established**.

---

## 8. The Weapon Class Array

**[CORRECTED 2026-08-15] This section replaces the former "Slot Usage Patterns",
whose slot-range percentages came from a 500-file sample and whose interpretation
("29 possible visual configuration variants") was wrong.**

`arWeaponClassTagMap[28]` at struct +32 is indexed by the engine's `eWeaponClass`
enum. The enum table at `g_WeaponClassEnumTable` (`0x71010C0860`) holds 48-byte
`{const char* name, s32 value}` records and **keeps its names in retail**:

| idx | enum name | idx | enum name |
| --- | --- | --- | --- |
| 0 | `WEAPONCLASS_HTH` | 14 | `WEAPONCLASS_DUALWIELD_FIST_FIST` |
| 1 | `WEAPONCLASS_1HSWING` | 15 | `WEAPONCLASS_1HFIST` |
| 2 | `WEAPONCLASS_1HTHRUST` | 16 | `WEAPONCLASS_2H_AXE_MACE` |
| 3 | `WEAPONCLASS_2HSWING` | 17 | `WEAPONCLASS_HANDXBOW` |
| 4 | `WEAPONCLASS_2HTHRUST` | 18 | `WEAPONCLASS_WAND_WITH_ORB` |
| 5 | `WEAPONCLASS_STAFF` | 19 | `WEAPONCLASS_1HSWING_WITH_SHIELD` |
| 6 | `WEAPONCLASS_BOW` | 20 | `WEAPONCLASS_1HTHRUST_WITH_SHIELD` |
| 7 | `WEAPONCLASS_XBOW` | 21 | `WEAPONCLASS_HTH_WITH_SHIELD` |
| 8 | `WEAPONCLASS_WAND` | 22 | `WEAPONCLASS_2HSWING_WITH_SHIELD` |
| 9 | `WEAPONCLASS_DUALWIELD` | 23 | `WEAPONCLASS_2HTHRUST_WITH_SHIELD` |
| 10 | `WEAPONCLASS_HTH_WITH_ORB` | 24 | `WEAPONCLASS_STAFF_WITH_SHIELD` |
| 11 | `WEAPONCLASS_1HSWING_WITH_ORB` | 25 | `WEAPONCLASS_2H_FLAIL` |
| 12 | `WEAPONCLASS_1HTHRUST_WITH_ORB` | 26 | `WEAPONCLASS_2HFLAIL_WITH_SHIELD` |
| 13 | `WEAPONCLASS_DUALWIELD_SWORD_FIST` | 27 | `WEAPONCLASS_ON_HORSE` |

`WEAPONCLASS_NONE = -1` has no slot; the sentinel is `NUM_WEAPON_CLASSES = 28`,
which is exactly the registered array count.

### 8.1 How the mapping was proved

For every array entry, resolve its Anim SNO ref to its `.ani` filename, resolve
the **core** map's ref for the *same tag id*, and take the differing token. The
token matches the enum name at every index that carries distinct data:

| idx | token in `.ani` filenames | share | files |
| --- | --- | --- | --- |
| 1 | `1HS` | 519/880 | 47 |
| 2 | `1HT` | 275/479 | 16 |
| 3 | `2HS` | 473/497 | 45 |
| 4 | `2HT` | 375/378 | 32 |
| 5 | `STF` (Crusader spells it `2HMace`) | 383/438 | 36 |
| 6 | `BOW` | 348/350 | 31 |
| 7 | `XBOW` | 94/94 | 9 |
| 9 | `DW`, `DW_SS`, `DW_XBow` | 122/126, 42/42, 28/28 | 13, 3, 3 |
| 10 | `Orb_` / `MOJO_` **inserted** | — | 37, 25 entries |
| 11 | `1HS_MOJO`, `1HS_Orb` | 22/22 | 2 |
| 12 | `1HT_MOJO` | 24/24 | 2 |
| 13 | `DW_SF` | 45/46 | 3 |
| 14 | `DW_FF` | 44/45 | 3 |
| 15 | `1HF` | 46/46 | 3 |
| 16 | `2HMace` (Crusader) / `STF` (Monk) | — | — |
| 17 | `1HXBow` | 25/26 | 2 |
| 18 | `1HS_Orb` (Wizard) | — | — |
| 19 | `1HS_Shield` | 61/164 | 5 |
| 20 | `1HT_Shield` | 43/43 | 3 |
| 21 | `Shield_` **inserted** | — | 43 entries |
| 22 | `2HS_Shield` | 49/49 | 3 |
| 23 | `2HT_Shield` | 51/53 | 3 |
| 24 | `2HMace_Shield` | 33/35 | 3 |
| 25 | `2HFlail` | 54/54 | 3 |
| 26 | `2HFlail_Shield` | 36/36 | 3 |
| 27 | `SteedCharge` (the Crusader's mount) | — | — |

Two structural checks inside that table are worth noting, because neither was
designed for:

* **Indices 10 and 21 are the only two whose diff is an insertion**
  (`'' → 'Orb_'`, `'' → 'Shield_'`) rather than a replacement of the `HTH`
  token. Those are exactly the two enum entries that name an *empty main hand* —
  `HTH_WITH_ORB` and `HTH_WITH_SHIELD`.
* **Index 27's token is `SteedCharge`**, and index 27 is `ON_HORSE`. The
  Crusader's mount is the only mounted player animation in the game.

`x1_Crusader_Male.ans` (largest file, 39,564 bytes, 119 tags per populated map)
reads straight down the enum on its own.

**Lower confidence:** indices **8 (`WAND`)** and **18 (`WAND_WITH_ORB`)** have no
file whose token is unique to them, so their names rest on ordinal position
alone — which their 26 neighbours confirm, and the enum leaves no alternative.

### 8.2 Index 0 is degenerate

`arWeaponClassTagMap[0]` (`WEAPONCLASS_HTH`) is populated in only **10 of 3,212**
files, and in every one the single entry's value is `0xFFFFFFFF`. All ten are
town-portal / marker props (`townPortal`, `MarkerLocation`,
`OpenWorld_Tiered_Rift_Obelisk_portal`, …).

Hand-to-hand is served by `tCoreTagMap` at struct +16 instead: populated in
**3,211 / 3,212** files, carrying the full 397-tag superset, with `_HTH_` `.ani`
references.

### 8.3 The array holds overrides

| statistic | value |
| --- | --- |
| array entries that are `0xFFFFFFFF` | 358,509 of 383,147 (93.6%) |
| non-null array entries whose tag is also in the core map | **24,638 / 24,638** |
| indices that introduce a tag the core map lacks | **0 of 28** |

The per-index tag sets are strictly nested: 397 tags (indices 1–13) → 379
(14–16) → 372 → 371 → 360 → 300 (20–25) → 298 → 297 → 225.

---

## 9. Corpus Statistics

**[CORRECTED 2026-08-15]** The original figures came from 500-file and
3,245-entry samples. These cover all 3,212 files and all 405,581 entries.

### 9.1 General

| Metric | Value |
| --- | --- |
| Total files | 3,212 |
| Version | 24 (3,212 / 3,212) |
| Smallest file | 624 bytes |
| Largest file | 39,564 bytes (`x1_Crusader_Male.ans`) |
| Struct size | 480 |
| Tag map blocks per file | 29 (1 core + 28 weapon classes) |
| Total blocks | 93,148 |
| Total entries | 405,581 (22,434 core + 383,147 array) |
| `nValueType == 2` | 405,581 / 405,581 |
| Distinct tag ids | 397, all within namespace 0xC0000's 453 |
| Most common tag | `0x11000` — 54,374 entries |

### 9.2 Structural invariants — no exceptions found

| Check | Result |
| --- | --- |
| `size == 4 + 12 × count` | 93,148 / 93,148 |
| block +8 (runtime pointer) is zero | 93,148 / 93,148 |
| last block ends exactly at EOF | 3,212 / 3,212 |
| tag ids inside the registered namespace | 405,581 / 405,581 |
| `snoBaseAnimSet` non-−1 values are AnimSet ids | 170 / 170 (none is an Anim id) |

### 9.3 Size Formula

```
file_size = 16  (SNO header)
          + 464 (29 tag map blocks; the struct's first 16 bytes precede them)
          + 16  (dwSnoId .. snoBaseAnimSet)
          + Σ(4 + entryCount[i] × 12) for i = 0..28
```

Simplest case (1 entry in the core map, 28 empty array blocks):
`16 + 480 + 16 + 28 × 4 = 624 bytes` — matches the observed minimum.

### 9.4 Block population

```
core 3211 | wc0   10 | wc1 2885 | wc2 2885 | wc3 2885 | wc4 2885 | wc5 2885
wc6  2885 | wc7 2885 | wc8 2884 | wc9 2883 | wc10 2849 | wc11 2849 | wc12 2849
wc13 2486 | wc14 2486 | wc15 2486 | wc16 2121 | wc17 2105 | wc18 2020
wc19 1460 | wc20 1459 | wc21 1459 | wc22 1459 | wc23 1459 | wc24 1459
wc25 1403 | wc26 1399 | wc27 1178
```

---

## 10. Cross-References

| Related Format | Extension | Relationship |
| --- | --- | --- |
| Animation | `.ani` | clips referenced by every tag map entry |
| AnimSet | `.ans` | `snoBaseAnimSet` points at another AnimSet |
| AnimTree | `.ant` | state machine that selects animations via AnimSet |
| Appearance | `.app` | model whose skeleton the animations drive |

```
Actor (.acr)
  └── AnimSet (.ans)         ← this format
        └── Animation (.ani)
              └── Appearance (.app)  — skeleton
```

---

## 11. Known Unknowns

**[CORRECTED 2026-08-15]** — "Slot purpose mapping", "`entryType` values" and
"Slot 1 special purpose" are resolved and removed.

| Item | Notes |
| --- | --- |
| **Tag id names** | **Not recoverable from a retail build.** All 453 name pointers in the namespace table are the same stripped empty string (§7.2). |
| Tag `category` (+48, 63 … 70) | Eight values, meaning not established. |
| Tag `pairedTagId` (+52) | 36 records; adjacency suggests enter/loop or left/right pairs, not established. |
| Fallback semantics | The engine-side accessor was not located; "null ref → use the core map" is inferred from the corpus distribution (§8.3), not cited to code. |
| Indices 8, 18 (`WAND`, `WAND_WITH_ORB`) | Named by ordinal position only — no corpus file distinguishes them. |
| 56 unused tag ids | Registered in namespace 0xC0000 but referenced by no shipped file. |
| struct +4, +8 | Not established. |

---

## Appendix A — Reading an ANS File (C++)

```cpp
FILE* f = fopen("animset.ans", "rb");

// ── §3  header + struct head ──────────────────────────────────────────────────
u32 magic, version;
fread(&magic, 4, 1, f);   assert(magic == 0xDEADBEEF);
fread(&version, 4, 1, f); assert(version == 24);

fseek(f, 0x10, SEEK_SET);                 // the AnimSet struct starts here
u32 snoId, unk04, unk08; s32 snoBaseAnimSet;
fread(&snoId, 4, 1, f); fread(&unk04, 4, 1, f);
fread(&unk08, 4, 1, f); fread(&snoBaseAnimSet, 4, 1, f);

// ── §4  the 29 tag map blocks (1 core + 28 weapon classes) ────────────────────
struct AnimSetTagMap { u32 dataOffset; u32 dataSize; u64 pRuntime; };
AnimSetTagMap blocks[29];
fread(blocks, sizeof(AnimSetTagMap), 29, f);   // file 0x020 .. 0x1F0

// ── §5–6  read one block ──────────────────────────────────────────────────────
auto readBlock = [&](const AnimSetTagMap& b, const char* label) {
    if (b.dataSize <= 4) return;               // empty
    fseek(f, b.dataOffset + 16, SEEK_SET);     // struct-relative → file
    u32 entryCount; fread(&entryCount, 4, 1, f);
    assert(b.dataSize == 4 + entryCount * 12);
    for (u32 i = 0; i < entryCount; i++) {
        u32 e[3]; fread(e, 4, 3, f);           // nValueType, dwTagId, dwValue
        if (e[2] != 0xFFFFFFFF)
            printf("%-24s tag=0x%05X → ani=%u\n", label, e[1], e[2]);
    }
};

readBlock(blocks[0], "core (HTH/default)");    // struct +16
for (u32 wc = 0; wc < 28; wc++)                // struct +32 + 16*wc
    readBlock(blocks[1 + wc], kWeaponClassNames[wc]);

fclose(f);
```

---

## Appendix B — All Structures Summary

```cpp
// §3 — file header + AnimSet struct head
struct AnimSetHeader {
    u32 magic;                  // 0xDEADBEEF
    u32 version;                // 24
    u32 _reserved08[2];
    // AnimSet struct begins (file 0x10)
    u32 snoId;                  // struct +0
    u32 _unknown04;             // struct +4
    u32 _unknown08;             // struct +8
    s32 snoBaseAnimSet;         // struct +12, SNO group 8, -1 = none
};

// §4 — one tag map block; 29 of them at struct +16
struct AnimSetTagMap {          // 16 bytes
    u32   dataOffset;           // struct-relative; file pos = dataOffset + 16
    u32   dataSize;             // 4 + entryCount * 12
    void* pRuntimePayload;      // zero on disk
};

// the struct as the engine registers it
struct AnimSet {                // 480 bytes
    u32           snoId;                        // +0
    u32           _unknown04, _unknown08;       // +4, +8
    s32           snoBaseAnimSet;               // +12
    AnimSetTagMap tCoreTagMap;                  // +16
    AnimSetTagMap arWeaponClassTagMap[28];      // +32, indexed by eWeaponClass
};                                              // 32 + 28*16 == 480

// §6 — payload entry
struct TagMapEntry {            // 12 bytes
    u32 nValueType;             // 0 int, 1 float, 2 SNO ref (always 2 here)
    u32 dwTagId;                // namespace 0xC0000
    u32 dwValue;                // Anim SNO id, 0xFFFFFFFF = no override
};
```

---

*Original specification derived from binary analysis of 3,212 ANS files from
Diablo III: Reaper of Souls. Revised 2026-08-15 against the Diablo III Nintendo
Switch 2.6.2 build (`DiabloIIINX64r`, sha256 `9f0cb00d…6115`): struct layout from
`AnimSet_RegisterTypes` (0x710060CA20), tag namespace from
`TagMap_GetNamespaceTable` (0x71006A2360), weapon class enum from
`g_WeaponClassEnumTable` (0x71010C0860).*
