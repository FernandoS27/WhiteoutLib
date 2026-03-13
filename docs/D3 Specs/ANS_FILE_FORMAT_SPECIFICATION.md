# ANS File Format Specification

**Format**: Diablo III AnimSet (`.ans`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**Version**: 24
**Corpus**: 3,212 files analyzed

---

## Table of Contents

1. [Overview](#1-overview)
2. [File Layout](#2-file-layout)
3. [SNO Preamble](#3-sno-preamble)
4. [Slot Reference Table](#4-slot-reference-table)
5. [Slot Data Sections](#5-slot-data-sections)
6. [Animation Entries](#6-animation-entries)
7. [Tag ID System](#7-tag-id-system)
8. [Slot Usage Patterns](#8-slot-usage-patterns)
9. [Corpus Statistics](#9-corpus-statistics)
10. [Cross-References](#10-cross-references)
11. [Known Unknowns](#11-known-unknowns)
12. [Appendix A — Reading an ANS File (C++)](#appendix-a--reading-an-ans-file-c)
13. [Appendix B — All Structures Summary](#appendix-b--all-structures-summary)

---

## 1. Overview

AnimSet files define **animation state lookup tables** for Diablo III entities. Each AnimSet
maps animation state tags (e.g., idle, walk, attack) to specific `.ani` animation clips. The
29 fixed slots appear to represent visual variants of the entity (e.g., different equipment
configurations), where each slot can override which animation plays for a given state.

The animation pipeline:

```
AnimTree (.ant)  →  AnimSet (.ans)  →  Animation (.ani)
  state machine      state→clip map      keyframe data

Engine: "Play idle animation"
  → AnimSet slot[activeVariant] → find tag 0x011000 → resolve animSnoRef → load .ani
```

---

## 2. File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO Preamble                               (32 bytes)  │
│    0x000: magic, version, reserved, snoId               │
├─────────────────────────────────────────────────────────┤
│  Slot Reference Table             (29 × 16 = 464 bytes) │
│    0x020: offset/size pairs per slot (§4)               │
├─────────────────────────────────────────────────────────┤
│  Slot Data Sections                          (variable) │
│    0x1F0: tightly packed per-slot entry arrays (§5)     │
└─────────────────────────────────────────────────────────┘
```

The slot data sections are tightly packed immediately after the reference table, stored
sequentially in slot order (slot 0 first, slot 28 last).

---

## 3. SNO Preamble

**Tag**: ANS | **Version**: 24 | **Size**: 32 bytes

```cpp
struct SnoPreamble {                            // 32 bytes
    u32     magic;              // 0x00: Always 0xDEADBEEF
    u32     version;            // 0x04: Always 24 for .ans
    u32     _reserved08[2];     // 0x08: Unknown
    u32     snoId;              // 0x10: Unique SNO identifier
    u32     _reserved14[3];     // 0x14: Unknown
};
```

---

## 4. Slot Reference Table

**Location**: 0x020–0x1EF
**Size**: 464 bytes (29 slots × 16 bytes each)

```cpp
struct SlotReference {                          // 16 bytes
    u32     dataOffset;         // 0x00: Offset to slot data (+16 convention)
    u32     dataSize;           // 0x04: Size of slot data in bytes
    u32     _pad[2];            // 0x08: Padding (zeros)
};
```

**Offset calculation**: The actual slot data begins at `dataOffset + 16`
(see `ANI_FILE_FORMAT_SPECIFICATION.md` §10 for the +16 convention).

**Empty slots**: Have `dataSize = 4` (just the entry count field, which will be 0).

**Populated slots**: Have `dataSize = 4 + entryCount × 12`.

---

## 5. Slot Data Sections

Each slot's data is a variable-length array of animation entries preceded by a count:

```cpp
struct SlotData {
    u32         entryCount;                     // 0x00: Number of animation entries
    AnimEntry   entries[entryCount];            // 0x04: Array of animation entries
};
```

**Total size**: `4 + entryCount × 12` bytes.
For empty slots, `entryCount = 0` and the section is just 4 bytes.

---

## 6. Animation Entries

**Size**: 12 bytes per entry

```cpp
struct AnimEntry {                              // 12 bytes
    u32     entryType;          // 0x00: Always 2 (verified 100% across 3,245 entries)
    u32     tagId;              // 0x04: Animation state tag identifier (§7)
    u32     animSnoRef;         // 0x08: SNO reference to .ani file
                                //       0xFFFFFFFF = no animation (empty/inherited)
};
```

**`entryType`**: Always 2 in all observed data. Possibly a type discriminator for a union
type where type=2 means "animation reference."

**`animSnoRef`**: When set to `0xFFFFFFFF` (−1 as signed), the slot does not override the
animation for this tag. The engine presumably falls back to slot 0 or a default.

---

## 7. Tag ID System

Tag IDs identify animation states using a hierarchical numeric scheme. Tags are u32 values
that group related animation states.

### 7.1 Known Tag IDs

| Tag ID       | Frequency | Likely Meaning         |
|-------------|-----------|------------------------|
| `0x011000`  | 2,508     | Idle                   |
| `0x011010`  | 241       | Walk / movement        |
| `0x011050`  | 189       | Death                  |
| `0x011060`  | 2         | Attack variant 1       |
| `0x011061`  | 1         | Attack variant 2       |
| `0x011070`  | 2         | Ability variant 1      |
| `0x011071`  | 2         | Ability variant 2      |
| `0x011072`  | 2         | Ability variant 3      |
| `0x011090`  | 13        | Hit reaction           |
| `0x0111D1`  | 10        | Special action         |
| `0x011310`  | 107       | Opening (transition)   |
| `0x011320`  | 107       | Open (state)           |
| `0x011330`  | 34        | Closing (transition)   |
| `0x014020`  | 26        | Spawn / appearance     |
| `0x040000`  | varies    | Generic action (player)|
| `0x063A00`  | varies    | Combat (player)        |
| `0x064205`  | varies    | Equipment (player)     |
| `0x080106`  | varies    | Special ability (player)|

### 7.2 Tag ID Encoding

Tags follow a hierarchical `0x0XXYYY` pattern where the upper bits indicate the animation
category and lower bits distinguish variants within that category:

- `0x011XXX` — Entity state animations (idle, walk, death, open/close)
- `0x014XXX` — Lifecycle animations (spawn, despawn)
- `0x04XXXX` — Player generic actions
- `0x06XXXX` — Player combat / equipment
- `0x08XXXX` — Player special abilities

---

## 8. Slot Usage Patterns

### 8.1 Slot Distribution

From analysis of 500 AnimSet files:

| Slot Range  | Usage Rate | Typical Content                          |
|------------|------------|------------------------------------------|
| Slot 0     | 99.8%      | Primary/default animation set            |
| Slot 1     | 0.2%       | Rarely used                              |
| Slots 2–10 | 34.6%      | Secondary variants (complex entities)    |
| Slots 11–13| 31.2%      | Tertiary variants                        |
| Slots 14–16| 5.6%       | Extended variants (player characters)    |
| Slots 17–28| <5%        | Only in large character AnimSets         |

### 8.2 Entity Complexity

| Entity Type     | File Size | Slots Used | Entries/Slot | Example                  |
|----------------|-----------|------------|-------------|--------------------------|
| Simple prop    | 624 B     | 1          | 1           | Crown holder, barrel     |
| Interactive    | 1.2 KB    | 17         | 3           | Chest (idle/opening/open)|
| NPC            | 2–5 KB    | 17–20      | 5–15        | Merchants, quest NPCs    |
| Player class   | 39 KB     | 29         | 52–119      | Crusader, Barbarian      |

### 8.3 Slot Interpretation

For simple entities (props, interactables), most slots contain `0xFFFFFFFF` references,
inheriting from slot 0. For player characters, each slot carries a full animation mapping
with unique `.ani` references, suggesting these 29 slots correspond to the 29 possible visual
configuration variants (equipment slots, weapon types, or stance modes).

---

## 9. Corpus Statistics

### 9.1 General

| Metric                     | Value         |
|---------------------------|---------------|
| Total files               | 3,212         |
| Version                   | 24 (all)      |
| Smallest file             | 624 bytes     |
| Largest file              | 39,564 bytes  |
| Fixed slot count          | 29            |
| Entry type                | 2 (all, 100%) |
| Unique tag IDs observed   | 15+           |
| Most common tag           | 0x011000 (idle, 2,508 uses) |

### 9.2 Size Formula

```
file_size = 32 (preamble)
          + 464 (slot table)
          + Σ(4 + entryCount[i] × 12) for i = 0..28
```

For the simplest case (1 animation in slot 0, rest empty):
`32 + 464 + 16 + 28 × 4 = 624 bytes` (matches observed minimum).

---

## 10. Cross-References

| Related Format | Extension | Relationship                                  |
|----------------|-----------|-----------------------------------------------|
| Animation      | `.ani`    | Individual animation clips referenced by `animSnoRef` |
| AnimTree       | `.ant`    | State machine that selects animations via AnimSet |
| Appearance     | `.app`    | Model whose animations are organized by this AnimSet |

```
Actor (.acr)
  └── AnimSet (.ans)         ← this format
        └── Animation (.ani)
              └── Appearance (.app)  — skeleton
```

---

## 11. Known Unknowns

| Item                        | Notes                                              |
|-----------------------------|---------------------------------------------------|
| Slot purpose mapping        | The 29 slots likely represent visual variants, but exact mapping (which slot = which equipment config) is unknown |
| `entryType` values          | Only 2 observed; other possible values unknown     |
| Tag ID full dictionary      | Only 15 unique tags found in 500-file sample; player character AnimSets use many more (0x04XXXX, 0x06XXXX, 0x08XXXX ranges) |
| Slot inheritance rules      | When `animSnoRef = 0xFFFFFFFF`, fallback behavior (inherit from slot 0? from engine default?) is assumed but not confirmed |
| Slot 1 special purpose      | Used by only 0.2% of files; may have a reserved or deprecated role |

---

## Appendix A — Reading an ANS File (C++)

```cpp
FILE* f = fopen("animset.ans", "rb");

// ── §3  SNO Preamble ──────────────────────────────────────────────────────────
SnoPreamble preamble;
fread(&preamble, sizeof(SnoPreamble), 1, f);
assert(preamble.magic == 0xDEADBEEF);
assert(preamble.version == 24);

// ── §4  Slot Reference Table ──────────────────────────────────────────────────
SlotReference slots[29];
fread(slots, sizeof(SlotReference), 29, f);

// ── §5–6  Read animation entries from each populated slot ─────────────────────
for (u32 s = 0; s < 29; s++) {
    if (slots[s].dataSize <= 4) continue;       // empty slot

    fseek(f, slots[s].dataOffset + 16, SEEK_SET);

    u32 entryCount;
    fread(&entryCount, sizeof(u32), 1, f);

    for (u32 i = 0; i < entryCount; i++) {
        AnimEntry entry;
        fread(&entry, sizeof(AnimEntry), 1, f);

        if (entry.animSnoRef != 0xFFFFFFFF) {
            printf("Slot %2u: tag=0x%06X → ani=0x%06X\n",
                   s, entry.tagId, entry.animSnoRef);
        }
    }
}

fclose(f);
```

---

## Appendix B — All Structures Summary

```cpp
// §3 — SNO Preamble (32-byte variant)
struct SnoPreamble {                            // 32 bytes
    u32 magic;                  // 0xDEADBEEF
    u32 version;                // 24
    u32 _reserved08[2];
    u32 snoId;
    u32 _reserved14[3];
};

// §4 — Slot Reference
struct SlotReference {                          // 16 bytes
    u32 dataOffset;             // +16 convention
    u32 dataSize;               // 4 + entryCount × 12
    u32 _pad[2];
};

// §6 — Animation Entry
struct AnimEntry {                              // 12 bytes
    u32 entryType;              // Always 2
    u32 tagId;                  // Animation state tag
    u32 animSnoRef;             // SNO ref to .ani (0xFFFFFFFF = none)
};
```

---

*Specification derived from binary analysis of 3,212 ANS files from Diablo III: Reaper of Souls.*
