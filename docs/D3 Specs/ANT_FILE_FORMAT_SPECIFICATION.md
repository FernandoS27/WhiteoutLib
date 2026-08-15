# ANT File Format Specification

**Format**: Diablo III AnimTree (`.ant`)  
**Byte Order**: Little-endian  
**Magic**: `0xDEADBEEF`  
**Version**: 30  
**Corpus**: 1 file (insufficient data for complete analysis)  
**Status**: Layout derived from the binary (2026-08-15) — see *Correction 2*
below, which supersedes §§2, 4, 6 and 8. The corpus still holds a single file,
`Axe Bad Data.ant` (148 bytes), so `AnimTreeNode`'s element size is predicted,
not measured.
**SNO Group**: 67 (`AnimTree`)
**Registered revision**: 31 — the shipped data is v30, so the binary's compiled struct describes a *newer* layout (see below / README §4)

See [README.md](README.md) for the build these offsets come from, the generator pipeline
and the conventions used below.

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

> **Correction (§2, §4).** The header fields in §4 are right, but the struct is
> **96 bytes** and the block at the end is a payload entry, not part of it.
> `dataOffset` = 96 is struct-relative like every other D3 offset (file `0x070`),
> so the block runs file `0x070..0x094` — 36 bytes, which is `sizeof(AnimTreeLeaf)`
> exactly, and `96 + 36` is the end of the file. §2's "Node Data Section @ 0x060"
> is 16 bytes early: file `0x060..0x070` is the struct's own tail.
>
> The engine registers three arrays here, each `{count, offset, size, pointer}`:
> leaves at struct +20/+24/+32, nodes at +40/+44/+56, and an index array at
> +64/+68/+80. Only the leaf array is populated in this file, so the header's
> `_unknown020` and the "48 bytes of zeros" in §2 are the node and index
> descriptors sitting empty.
>
> Reading §4's `AntNodeData` as an `AnimTreeLeaf` at file `0x070` accounts for
> every field, and confirms two guesses in it: `snoRef1` (7312) is the leaf's
> **Anim** reference, and `hashValue` = `0x811C9DC5` is a layer-name field
> holding the FNV-1a hash of the empty string — the name is simply unset.
>
> **This rests on one file.** The corpus has a single `.ant`, and its node and
> index arrays are empty, so the node element's size for version 30 is *not*
> established. The current build's `AnimTreeNode` is 300 bytes, but that build
> shrank several animation types, so the number cannot be carried over; the
> parser leaves the node block as raw bytes rather than guess.

---

> **Correction 2 (2026-08-15) — the format is now derived from the engine, and
> §§2, 4, 6 and 8 below are superseded.**
>
> The layouts were re-derived from the Switch 2.6.2 registration
> (`AnimTree_RegisterTypes` @ `0x710042A5C0`) *and* from the tree-evaluation
> code, which the earlier passes had not found. Retail strips field names but
> **keeps enum value names**, and the four enum tables this group registers are
> what unlock the semantics.
>
> ```cpp
> struct AnimTree {                    // 96 bytes, struct-relative
>     u32   dwSnoId;                   //  0   (+4, +8 are zero on disk)
>     u32   dwUnknown0C;               // 12   no engine reader; 0x47D6FE8C here
>     u32   dwFlags;                   // 16   registered as a bitfield; 0
>     u32   dwLeafCount;               // 20
>     SerializeData leaves;            // 24   ptr at +32
>     u32   dwNodeCount;               // 40
>     SerializeData nodes;             // 44   ptr at +56
>     u32   dwSyncGroupCount;          // 64
>     SerializeData syncGroups;        // 68   ptr at +80, element u32
>     i32   nRootRef;                  // 88   see the reference encoding below
> };
> struct AnimTreeLeaf {                // 36 bytes  (corpus-verified)
>     i32   eLeafType;                 //  0   Animation(0) or Pose(4)
>     i32   snoAnim;                   //  4   Anim SNO, -1 => use dwAnimTag
>     i32   dwAnimTag;                 //  8   AnimSet tag/GBID fallback
>     f32   flPoseTime;                // 12   Pose leaves only
>     f32   flPlaybackRate;            // 16   divides the clip duration; 1.0
>     i32   nSyncGroup;                // 20   -1 = none
>     i32   eAdvanceMode;              // 24   Continuous(0) / InactiveReset(1)
>     i32   eBoneMask;                 // 28   Action(0) / GetHit(1) / None(16)
>     u32   dwLayerNameHash;           // 32   FNV-1a; 0x811C9DC5 == hash("")
> };
> struct AnimTreeNode {                // 300 bytes  (PREDICTED for v30)
>     i32   eNodeType;                 //   0
>     i32   dwEntryCount;              //   4
>     i32   arChildren[16];            //   8
>     i32   eVariable;                 //  72   None(17) => use the node's own value
>     AnimTreeBlendCase arCases[16];   //  76   {i32 eVariable; i32 nValue; f32 flValue}
>     AnimTreeBlendRamp ramp;          // 268   {rampIn, rampOut, duration,
>                                      //        startValue, holdValue, endValue}
>     i32   nSyncGroup;                // 292   -1 = none
>     u32   dwLayerNameHash;           // 296
> };
> ```
>
> **Node and child references carry a leaf bit.** Bit 31 set means "leaf, index
> = `ref & 0x7FFFFFFF`"; clear means "node index". The word at file `0x068` that
> §4 called `flagOrNegZero` is `AnimTree.nRootRef` = `0x80000000` = **leaf 0** —
> the file's single leaf *is* its root. `AnimTree_EvalNode` (`0x710033A3C0`)
> decodes it with exactly that test.
>
> **Node types** (`EAnimTreeNodeType`, table `0x7101068628`): `Animation` 0,
> `PiecewiseLinearBlend` 1, `BoneWeightedBlend` 2, `SwitchBlend` 3, `Pose` 4,
> `AdditiveBlend` 5, `BoneWeightsLinearBlend` 6, `BoneWeightsMultiplyBlend` 7.
> Only 0 and 4 appear on leaves.
>
> **Blend variables** (`EAnimTreeVariable`, table `0x71010686B8`):
> `ForwardSpeed` 0, `TurnSpeed` 1, `IsIdle` 2, `WeaponClass` 3, `WalkSlowSpeed`
> 4, `WalkSpeed` 5, `RunSpeed` 6, `SprintSpeed` 7, `InTown` 8, `IsSpecialMove`
> 9, `AbsTurnSpeed` 10, `AimYaw` 11, `AimBlend` 12, `IsAlive` 13, `IsTurning`
> 14, `ForceWalk` 15, an unnamed slot 16, and `None` 17. The runtime table is 18
> slots of `{float, int}` = 144 bytes. `None` means "use the inline literal in
> the blend case" (or, on a node, "use the node's own ramp value").
>
> **The third array is the sync-group table.** `AnimTreeInstance_Init`
> (`0x710033BC20`) allocates `40 * dwSyncGroupCount` runtime bytes from it, and
> `AnimTreeLeaf.nSyncGroup` / `AnimTreeNode.nSyncGroup` index that table so a
> set of clips shares one normalised cursor. This build reads the array's
> **count only** — the stored ints are never dereferenced.
>
> §6's guesses are wrong in detail: the `1.0f` at `0x080` is the leaf's
> **playback rate**, not a blend weight, and the `0xFFFFFFFF` at `0x084` is a
> sync-group index of -1, not an empty SNO reference. §8's "hash at 0x090" is
> resolved: it is the layer-name hash, and `0x811C9DC5` is the FNV-1a basis
> because the name is empty. `dwUnknown0C` at file `0x01C` (§3's
> `_unknown01C`, "possibly a timestamp") remains unexplained — it is a
> registered `DT_INT` that no engine function reads.
>
> **Still unverified:** the 300-byte `AnimTreeNode` size for version 30. No
> AnimTree-family field carries the 0x700000 "added after the shipped revision"
> marker, and both `AnimTree` (96) and `AnimTreeLeaf` (36) match their
> registered sizes against this file, so 300 is the best prediction — but the
> node array is empty here, so it is not measured. One `.ant` with
> `dwNodeCount > 0` would settle it outright.

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

> **Terminology note (2026-08-16).** There is no 32-byte preamble: a `.ant` is a **16-byte SNO
> header** followed by the **96-byte `AnimTree` struct**. The block below is that header plus
> the struct's first 16 bytes. **The offsets are file offsets and are correct**; struct-relative
> equivalents are 16 lower, so `snoId` = struct +0 and the float at file `0x01C` = struct +0x0C.

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

> **Correction (2026-08-15).** The chain below is not what the engine does. An
> AnimTree is owned by an **Actor** — `Actor+108` is the only registered
> `DT_SNO` reference to group 67 in the whole reflection, and it carries flag
> 0x700000, so the *shipped* Actor revision does not have it. Scanning the
> corpus confirms that: the id 243819 appears **0 times** in 19177 `.acr`,
> 3212 `.ans` and 15258 `.ani` files. Each `AnimTreeLeaf` names an **Anim**
> directly (`snoAnim`), and only falls back to an **AnimSet** tag lookup
> (`dwAnimTag`, resolver `sub_710060C5D0`) when `snoAnim == -1`. The AnimSet is
> supplied by the actor at runtime, not by the `.ant`.

```
Actor (.acr)  ──(Actor+108, post-shipped-revision field)──▶  AnimTree (.ant)
                                                              │
                     leaf.snoAnim ────────────────────────────┴──▶ Anim (.ani)
                     leaf.dwAnimTag ──(fallback)──▶ AnimSet (.ans) ──▶ Anim (.ani)
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
