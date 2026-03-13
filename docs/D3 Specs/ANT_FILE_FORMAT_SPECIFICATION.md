# ANT File Format Specification

**Format**: Diablo III AnimTree (`.ant`)  
**Byte Order**: Little-endian  
**Magic**: `0xDEADBEEF`  
**Version**: 30  
**Corpus**: 1 file (insufficient data for complete analysis)  
**Status**: Stub — single file `Axe Bad Data.ant` (148 bytes)

---

## Table of Contents

1. [Overview](#1-overview)
2. [File Layout](#2-file-layout)
3. [SNO Preamble](#3-sno-preamble)
4. [Observed Fields](#4-observed-fields)
5. [Hex Dump](#5-hex-dump)
6. [Speculation](#6-speculation)
7. [Cross-References](#7-cross-references)
8. [Known Unknowns](#8-known-unknowns)

---

## 1. Overview

AnimTree files define **animation state machines** that control how animations are selected, blended, and transitioned for Diablo III entities. The AnimTree sits at the top of the animation pipeline:

```
AnimTree (.ant)  →  AnimSet (.ans)  →  Animation (.ani)  →  Appearance (.app)
  state machine      state→clip map      keyframe data       skeleton
```

Only a single AnimTree file exists in the corpus, labeled "Axe Bad Data" — likely a leftover test or corrupted asset. The animation state machine logic for most Diablo III entities is apparently implemented in code rather than data-driven via `.ant` files.

---

## 2. File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO Preamble                               (32 bytes)  │
│    0x000: magic, version, snoId                         │
├─────────────────────────────────────────────────────────┤
│  ANT-Specific Header                        (16 bytes)  │
│    0x020: unknown, nodeCount, dataOffset, dataSize      │
├─────────────────────────────────────────────────────────┤
│  Empty Region                               (48 bytes)  │
│    0x030–0x05F: zeros                                   │
├─────────────────────────────────────────────────────────┤
│  Node Data Section                          (52 bytes)  │
│    0x060–0x093: single tree node (see §4)               │
└─────────────────────────────────────────────────────────┘
```

Total file size: **148 bytes**.

---

## 3. SNO Preamble

```cpp
struct SnoPreamble {                            // 32 bytes @ 0x000
    u32     magic;              // 0x000: Always 0xDEADBEEF
    u32     version;            // 0x004: 30 (0x1E)
    u32     _reserved008[2];    // 0x008: Zeros
    u32     snoId;              // 0x010: 0x3B86B
    u32     _reserved014;       // 0x014: Always 0
    u32     _reserved018;       // 0x018: Always 0
    f32     _unknown01C;        // 0x01C: 110076.5469 (0x47D6FE8C) — possibly a timestamp
};
```

---

## 4. Observed Fields

```cpp
struct AntHeader {                              // 16 bytes @ 0x020
    u32     _unknown020;        // 0x020: 0
    u32     nodeCount;          // 0x024: 1 — single tree node
    u32     dataOffset;         // 0x028: 96 (0x60) — offset to node data
    u32     dataSize;           // 0x02C: 36 (0x24)
};
// 0x030–0x05F: 48 bytes of zeros (padding / empty arrays)

struct AntNodeData {                            // 52 bytes @ 0x060
    u32     _pad[2];            // 0x060: Zeros
    u32     flagOrNegZero;      // 0x068: 0x80000000 (negative zero or flag)
    u32     _pad06C;            // 0x06C: 0
    u32     snoRef1;            // 0x070: 0x00001C90 (7312) — possibly an SNO reference
    i32     sentinel1;          // 0x074: 0xFFFFFFFF (-1)
    u32     _pad078[2];         // 0x078: Zeros
    f32     blendWeight;        // 0x080: 1.0f — possible blend weight
    i32     sentinel2;          // 0x084: 0xFFFFFFFF (-1)
    u32     _pad088;            // 0x088: 0
    u32     nodeTypeOrSize;     // 0x08C: 16 (0x10) — node type or data size
    u32     hashValue;          // 0x090: 0x811C9DC5 — FNV-1a hash initial value
};
```

---

## 5. Hex Dump

Complete hex dump of the single file (`Axe Bad Data.ant`, 148 bytes):

```
0x000: EF BE AD DE 1E 00 00 00  00 00 00 00 00 00 00 00   ................
0x010: 6B B8 03 00 00 00 00 00  00 00 00 00 8C FE D6 47   k..............G
0x020: 00 00 00 00 01 00 00 00  60 00 00 00 24 00 00 00   ........`...$...
0x030: 00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00   ................
0x040: 00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00   ................
0x050: 00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00   ................
0x060: 00 00 00 00 00 00 00 00  00 00 00 80 00 00 00 00   ................
0x070: 00 00 00 00 90 1C 00 00  FF FF FF FF 00 00 00 00   ................
0x080: 00 00 80 3F FF FF FF FF  00 00 00 00 10 00 00 00   ...?............
0x090: C5 9D 1C 81                                        ....
```

---

## 6. Speculation

Based on observed field patterns and the name "AnimTree," this format likely defines:

- **State nodes**: Each node in a blending tree with transitions
- **Blend parameters**: The `1.0f` value at 0x080 is consistent with a blend weight
- **AnimSet references**: The `0xFFFFFFFF` sentinel values are likely empty/unused SNO references
- **Transition rules**: How to move between animation states

The single 36-byte data section (at offset 96 with size 36) represents one minimal tree node with mostly empty references, consistent with the "Bad Data" label.

---

## 7. Cross-References

```
AnimTree (.ant)
└── AnimSet (.ans)        — state→clip mapping selected by AnimTree
    └── Animation (.ani)    — individual animation clips
        └── Appearance (.app) — model and skeleton
```

| Related Format | Extension | Relationship |
|----------------|-----------|--------------|
| AnimSet | `.ans` | State→clip mapping selected by AnimTree |
| Animation | `.ani` | Individual animation clips |
| Appearance | `.app` | Model and skeleton definition |

---

## 8. Known Unknowns

| Item | Notes |
|------|-------|
| Complete format structure | Only 1 file exists — impossible to determine field semantics with confidence |
| Node data format | The 52-byte node at 0x060 has recognizable patterns (sentinels, floats) but no structural confirmation |
| Hash at 0x090 | `0x811C9DC5` matches the FNV-1a initial hash value — could be a hash seed or coincidence |
| Timestamp at 0x01C | The float `110076.5` could be a build timestamp, version marker, or unrelated data |
| State machine logic | May be entirely code-driven, with `.ant` files being a vestigial data-driven path |

---

*Stub specification — derived from a single 148-byte file. Format analysis is incomplete due to insufficient data.*
