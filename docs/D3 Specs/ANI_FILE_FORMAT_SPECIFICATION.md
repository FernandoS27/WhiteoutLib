# ANI File Format Specification

**Format**: Diablo III Animation Clip (`.ani`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**Version**: 118 (all 15,258 corpus files)
**Corpus**: 15,258 files analyzed — 17,249 animation permutations
**SNO Group**: 6 (`Anim`)
**Registered revision**: 129 — the shipped data is v118, so the binary's compiled struct describes a *newer* layout (see below / README §4)

See [README.md](README.md) for the build these offsets come from, the generator pipeline
and the conventions used below.

---


> ## Correction pass 4 — 2026-08-15
>
> A dedicated sweep of the seven remaining `_unknown*` floats in `AnimPermutation`.
>
> * **`+0x50`, `+0x54` and `+0x58` are baked per-channel keyframe density**, not times.
>   Each is the clip's mean frames-per-key for one curve channel, in the same order as the
>   three curve arrays: `(float)(dwFrameCount × dwBoneCount) / (float)totalKeys`. That is
>   **bit-exact in `float32` over 51,747 of 51,747 channel records** (17,249 permutations ×
>   3 channels). It explains the `frameCount/2` pattern pass 3 could only observe: a channel
>   holding just a first and last key per bone gives exactly `frameCount/2`. See §4.1.
>   This retires the curated legacy names `flEndTime` / `flTotalDuration` / `flLoopDuration`.
> * **The engine's permutation read set is now closed.** Every access in 2.6.2 goes through
>   `perm = *(Anim+40) + 360*index`, and the playback state never caches the pointer
>   (`Anim_InitPlaybackState` re-acquires the SNO and redoes the multiply each call). Tainting
>   that multiply across all 167 functions in the binary that materialise 360 or 720, plus a
>   separate sweep for an index-0 access with no multiply — and running down each of the 14
>   residual hits individually — proves **no code in this build reads `+0x4C`, `+0x50`,
>   `+0x54`, `+0x58`, `+0x6C`, `+0x70` or `+0x74`.** Those are authoring-side fields, so §13
>   now cites the corpus for them and never a function. See §13.1.
> * **The four `vPoint` share a non-zero registered default of `(0, 0, 4)`** —
>   `Anim_RegisterTypeDescriptors` passes `&qword_710185A38C` for all four, and the prologue at
>   `0x710060B63C`–`0x710060B64C` fills that block with `{0.0, 0.0, 4.0f}`. 10,447 of the
>   68,996 stored points are exactly it. See §9.2.
> * **Field names are stripped in this build** and cannot be recovered: every
>   `TypeDesc_RegisterField_*` call in `Anim_RegisterTypeDescriptors` passes the same
>   empty-string placeholder `unk_7100D6A4A3`. Type names survive; field names do not.
>
> ## Correction pass 3 — 2026-08-15
>
> Two of pass 2's "no engine read located" entries are now resolved, and the whole layout is
> gated on the complete corpus rather than samples.
>
> * **The four `Vector3D` at perm +0x0D8 are an indexed point array**, not an unknown.
>   `Anim_GetPermutationPointWorld` (`0x71003FF0B0`) and `…_FromActor` (`0x71003FEEC0`) both
>   compute `perm + 0x0D8 + 12*i` under a literal `if (i <= 3)` and transform the result into
>   world space by the actor's rotation, scale and position, falling back to the actor's own
>   position when no clip is playing. See §9.2.
> * **Root-motion array B is array A with the net velocity removed** — the "play it in place"
>   track: `B[k] == A[k] − A[last]·k/(frameCount−1)`, **bit-exact on 32 of the 35** permutations
>   that populate it, with three hand-authored exceptions listed in §9.1. The two descriptors
>   address different payload offsets in 17,249/17,249, so they are two arrays, not one written
>   twice. See §9.1.
> * **Full-corpus gate**: every structural claim in this document now holds on 15,258/15,258
>   files, 17,249/17,249 permutations, 1,959,441/1,959,441 curves and
>   **11,556,940/11,556,940 rotation keys** (§11.4). Pass 2's signed-`i16/32767` quaternion
>   finding, previously tested on 1,008,455 keys, holds on all of them; worst deviation of
>   |q| from 1.0 is 5.96e-05, which is the 1/32767 quantisation step.
> * Four engine functions were named in the IDB during this pass:
>   `Anim_GetPermutationPointWorld`, `Anim_GetPermutationPointWorld_FromActor`,
>   `Anim_GetCurrentAnimAndPermutation` and `Anim_ResolvePermutationHardpointIndex`.
>
> ## Correction pass 2 — 2026-08-15
>
> This document has been re-derived end to end against the Diablo III **Nintendo Switch
> 2.6.2** build (`DiabloIIINX64r`, sha256 `9f0cb00d…6115`) and re-measured over the whole
> 15,258-file corpus. Every offset below is now backed by either a cited engine function or an
> explicit corpus count. **Sections 3–13 and both appendices were rewritten.** The changes that
> matter:
>
> * **`fps` did not exist.** The `u32` at permutation +100 is a **random-selection weight**:
>   `Anim_PickRandomPermutationByWeight` (`0x710041A5F0`) sums it over every permutation of the
>   file and uses the total as a weighted-random denominator. 100 is simply the default
>   (15,216 of 17,249); 50/20/30/10/40/5/60/90/80 and 50 other values also occur, so
>   "always 100" was an artefact of sampling.
> * **The real frame rate is at permutation +72**, the field the old text called `velocity`.
>   `Anim_InitPlaybackState` (`0x710033B9D0`) computes `fps = field72 * 60.0`. The corpus
>   values × 60 give exactly 30 fps (14,785 permutations), then 25, 27, 45, 40 and 35 fps.
>   Clip length in 1/60 s ticks is `(frameCount − 1) / field72`.
> * **The "Look Entry Section" is a keyframed event list.** Its element is the engine's
>   `KeyframedAttachment` = `{ float flFrame; TriggerEvent tEvent; }`. The 64-byte string the
>   old text called `lookName` is the **first of two hardpoint names** inside the trigger
>   (`Default`, `HP_chest`, `HP_Head`, `HP_leftFoot`, …). A separate look name and a physics
>   constraint name live further in.
> * **The "per-frame auxiliary sections" are root motion**, not "likely bounding volumes":
>   `frameCount` × `Vector3D` of cumulative root translation. Proven by a closed identity
>   against the movement-velocity vector (§9).
> * **Two whole fields were missing**: the permutation's movement velocity (`+320`) and its
>   hardpoint name (`+332`).
> * **The keyframe encoding is confirmed, with one fix**: rotations are signed `i16 / 32767`
>   (1,008,455 keys tested), but the "negate Y" step is a DCC **import convention**, not part
>   of the format — nothing in the engine negates a component.
> * **The payload now tiles completely**: header + struct + every section named by every
>   permutation accounts for every byte of all 15,258 files, with no overlaps, no leftover
>   tail, and only 184,124 four-byte zero alignment pads.
>
> ## Correction pass 1 (retained — still correct)
>
> The 56-byte header / N × 408-byte block layout is right, but the block starts **16 bytes
> later than the original §4 said**, at file `0x048` (struct +56), not `0x038`. The engine's
> own descriptor settles it: the field at struct +24 holds the block array's offset and it is
> **56 in 15,258 of 15,258 files**, with `size / count == 408` in all of them.
>
> * Every offset inside a block is 16 lower than the original document said.
> * `_pad000[2]`, `nameHash` and `_unknown00C` are **not block fields**; they are the tail of
>   the 56-byte file header.
> * A block ends with 8 zero bytes, which the shifted reading mistook for a leading pad.
>
> The engine calls a block an **`AnimPermutation`**. The build's *current* revision (129)
> shrinks it to 360 bytes; those numbers do **not** apply to version 118. See §14.

---

## Table of Contents

1. [Overview](#1-overview)
2. [File Layout](#2-file-layout)
3. [Anim Header](#3-anim-header)
4. [AnimPermutation](#4-animpermutation)
5. [Bone Name Section](#5-bone-name-section)
6. [Curve Descriptor Sections](#6-curve-descriptor-sections)
7. [Keyframe Data](#7-keyframe-data)
8. [Keyframed Attachments (events)](#8-keyframed-attachments-events)
9. [Root Motion](#9-root-motion)
10. [Offset Convention](#10-offset-convention)
11. [Corpus Statistics](#11-corpus-statistics)
12. [Cross-References](#12-cross-references)
13. [Known Unknowns](#13-known-unknowns)
14. [Version 118 vs the build's revision 129](#14-version-118-vs-the-builds-revision-129)
15. [Appendix A — Reading an ANI File (C++)](#appendix-a--reading-an-ani-file-c)
16. [Appendix B — All Structures Summary](#appendix-b--all-structures-summary)

---

## 1. Overview

ANI files store skeletal animation clips for Diablo III models. A file holds **one or more
`AnimPermutation`s** — independent takes of the same logical animation, from which the engine
picks one at random, weighted by each permutation's `dwSelectionWeight`. Each permutation
carries its own bone list, three keyframe channels per bone, an event list and a root-motion
track.

```
AnimTree (.ant)  →  AnimSet (.ans)  →  Animation (.ani)  →  Appearance (.app)
  state machine      tag → clip map     keyframe data        skeleton
```

**8.5 % of files carry more than one permutation** (1,311 of 15,258), so a reader that only
decodes permutation 0 silently drops animation data.

---

## 2. File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO header                                 (16 bytes)  │
│    magic 0xDEADBEEF, u16 version at +4                  │
├─────────────────────────────────────────────────────────┤
│  Anim struct                                (56 bytes)  │
├─────────────────────────────────────────────────────────┤
│  AnimPermutation[permCount]          (N × 408 bytes)    │
├─────────────────────────────────────────────────────────┤
│  per permutation, in payload order:                     │
│    BoneName[boneCount]              (boneCount × 64)    │
│    TranslationCurve[boneCount]      (boneCount × 24)    │
│    translation keys                         (variable)  │
│    RotationCurve[boneCount]         (boneCount × 24)    │
│    rotation keys                            (variable)  │
│    ScaleCurve[boneCount]            (boneCount × 24)    │
│    scale keys                               (variable)  │
│    KeyframedAttachment[attachCount]  (count × 412)      │
│    root motion A                    (frameCount × 12)   │
│    root motion B                    (frameCount × 12)   │
└─────────────────────────────────────────────────────────┘
```

Sections are packed with 8-byte alignment: a keyframe block whose size is not a multiple of 8
is followed by 4 zero bytes. Tiling all 15,258 files this way leaves **no unaccounted byte,
no overlap and no tail**; the only holes are 184,124 such 4-byte pads.

---

## 3. Anim Header

**Size**: 16-byte SNO header + 56-byte struct. **All stored offsets are struct-relative:
file position = 16 + offset** (§10).

```cpp
struct AniFile {
    u32 magic;                  // 0x000: 0xDEADBEEF
    u16 version;                // 0x004: 118
    u8  _hdr[10];               // 0x006: rest of the 16-byte SNO header
};

struct Anim {                                   // 56 bytes, struct-relative
    u32 dwSnoId;                // +0x00: unique per file (15,258 distinct)
    u32 _zero04;                // +0x04: 0 in 15,258/15,258
    u32 _zero08;                // +0x08: 0 in 15,258/15,258
    u32 dwFlags;                // +0x0C: 0 (13,765) / 2 (873) / 8 (343) / 18 (95); 13 distinct
    u32 dwFlags2;               // +0x10: 0 (10,948) / 2 (4,310) — only two values
    u32 snoAppearance;          // +0x14: SNO ref, group 9 = Appearance
    u32 permOffset;             // +0x18: 56 in 15,258/15,258
    u32 permSize;               // +0x1C: 408 × permCount
    u32 dwPermutationCount;     // +0x20: 1 (13,947) … 10
    u32 _pad24;                 // +0x24: 0 in all
    u64 _runtimePtr;            // +0x28: 0 on disk in all
    u32 dwNameHash;             // +0x30: 11,599 distinct, never 0; algorithm unknown
    u32 _pad34;                 // +0x34: 0 in all
};
```

`Anim_FindBoneIndexByName` (`0x710020FD90`) and `Anim_PickRandomPermutationByWeight`
(`0x710041A5F0`) both read +0x20 as the permutation count and +0x28 as the array base.

---

## 4. AnimPermutation

**Size**: 408 bytes.

```cpp
struct AnimPermutation {                        // 408 bytes (0x198)
    u32   dwFlags;              // +0x000: only 0 (14,534) or 1 (2,715).  Bit 0 is read by
                                //         Anim_GetPermutationFlagBit0 (0x710041A8B0) and
                                //         gates a branch in the playback-state init.
    char  szName[68];           // +0x004: NUL-terminated ASCII in 17,247/17,249; max len 63.
                                //         Length 68 comes from the registration, not 64.

    // ─── Playback ─────────────────────────────────────────────────────────────
    f32   flFramesPerTick;      // +0x048: frames advanced per 1/60 s tick.
                                //         fps = this × 60  (Anim_InitPlaybackState).
                                //         0.5 (= 30 fps) in 14,785 of 17,249.
    f32   _unknown04C;          // +0x04C: registered default 5.0; 5.0 in 98.96%.
                                //         Unread by the engine; see §13.1.
    f32   flFramesPerTranslationKey;  // +0x050: baked keyframe density, one per curve
    f32   flFramesPerRotationKey;     // +0x054: channel, in curve-array order. See §4.1.
    f32   flFramesPerScaleKey;        // +0x058: (frames × bones) / totalKeys, f32-exact.
    i32   nBlendTicksFromOtherAnim; // +0x05C: blend-in ticks when the ANIMATION changes
    i32   nBlendTicksSamePermSwap;  // +0x060: blend-in ticks when only the PERMUTATION changes
                                //         both × 0.016667 → seconds, clamped to clip length
    u32   dwSelectionWeight;    // +0x064: weighted-random pick.  NOT fps.
    f32   flSpeedScalar;        // +0x068: scales vMovementVelocity; default 1.0 (94.1%)
    f32   _unknown06C;          // +0x06C: 0.0 in 95.3%   ─┐ one group of three authored
    f32   _unknown070;          // +0x070: default 1.0     ├─ fractions, unread by the
    f32   _unknown074;          // +0x074: default 1.0    ─┘ engine.  See §13.1.

    // ─── Bones ────────────────────────────────────────────────────────────────
    u32   dwBoneCount;          // +0x078: boneNameSize == 64 × this in 17,249/17,249
    u32   boneNameOffset;       // +0x07C
    u32   boneNameSize;         // +0x080
    u32   _pad084;              // +0x084: 0 in all
    u64   _runtimePtr088;       // +0x088: 0 in all

    // ─── Channels ─────────────────────────────────────────────────────────────
    u32   dwFrameCount;         // +0x090: == last keyframe index + 1, in every track measured
    u32   translCurveOffset;    // +0x094
    u32   translCurveSize;      // +0x098: 24 × dwBoneCount
    u32   _pad09C;              // +0x09C
    u64   _runtimePtr0A0;       // +0x0A0
    u32   rotCurveOffset;       // +0x0A8
    u32   rotCurveSize;         // +0x0AC: 24 × dwBoneCount
    u64   _runtimePtr0B0;       // +0x0B0
    u32   scaleCurveOffset;     // +0x0B8
    u32   scaleCurveSize;       // +0x0BC: 24 × dwBoneCount
    u64   _runtimePtr0C0;       // +0x0C0

    // ─── Markers ──────────────────────────────────────────────────────────────
    f32   flMarkerFrame[4];     // +0x0C8: frame numbers, negative = unset.
                                //         Anim_GetMarkerFrameTimeInTicks (0x710041A800)
                                //         reads perm + 4*i + 0x0C8 with i ∈ {0,1,2,3} and
                                //         converts frame → ticks by / flFramesPerTick.
    f32   vPoint[4][3];         // +0x0D8: four Vector3D in animation-local space.
                                //         Anim_GetPermutationPointWorld (0x71003FF0B0)
                                //         reads perm + 0x0D8 + 12*i under `if (i <= 3)`,
                                //         then rotates/scales/translates it by the actor's
                                //         transform to get a world position.
    f32   flEventFrame;         // +0x108: frame number, −1.0 in 93.0%.  Same conversion;
                                //         bound-checked 0 ≤ f < clipFrames.

    // ─── Events ───────────────────────────────────────────────────────────────
    u32   attachOffset;         // +0x10C
    u32   attachSize;           // +0x110: 412 × attachCount
    u32   dwAttachmentCount;    // +0x114
    u64   _runtimePtr118;       // +0x118

    // ─── Root motion (two parallel tracks, see §9) ────────────────────────────
    u32   rootMotionTotalOffset;   // +0x120: motion as authored, cumulative
    u32   rootMotionTotalSize;     // +0x124: 12 × dwFrameCount
    u64   _runtimePtr128;          // +0x128
    u32   rootMotionInPlaceOffset; // +0x130: the SAME motion with the net velocity
                                   //         removed ("in place"); rev 129 dropped it
    u32   rootMotionInPlaceSize;   // +0x134: 12 × dwFrameCount
    u64   _runtimePtr138;          // +0x138

    f32   vMovementVelocity[3]; // +0x140: world units per 1/60 s tick.
                                //         (0,0,0) in 15,861/17,249; else X-only.
    char  szHardpointName[64];  // +0x14C: non-empty in 63 permutations
                                //         (HP_rightWeapon, HP_leftHand, HP_leftWeapon, …)
    i32   nHardpointIndex;      // +0x18C: 0 in 17,249/17,249
    u8    _tail[8];             // +0x190: 0 in 17,249/17,249
};
```

### 4.1 Keyframe density (`+0x050`, `+0x054`, `+0x058`)

These three floats are a **baked authoring statistic**, one per curve channel, laid out in the
same order as the three curve arrays. Each is the clip's mean number of frames per keyframe:

```
flFramesPerTranslationKey == (float)(dwFrameCount × dwBoneCount) / (float)Σ dwKeyCount over arTranslationCurves
flFramesPerRotationKey    == (float)(dwFrameCount × dwBoneCount) / (float)Σ dwKeyCount over arRotationCurves
flFramesPerScaleKey       == (float)(dwFrameCount × dwBoneCount) / (float)Σ dwKeyCount over arScaleCurves
```

**Bit-exact in `float32` over 51,747 of 51,747 channel records** — 17,249 permutations × 3
channels, zero mismatches — with that exact evaluation order: the multiply in integer, then a
single `float` divide. Two other orderings (`n / (k/b)` and `n × (b/k)`) reproduce only 88.9 %
and 88.0 % bit-exactly, so the order above is the one the exporter used.

Two consequences worth stating, because both were previously read as separate mysteries:

* A channel that stores only a first and last key per bone has `Σ keys == 2 × bones`, giving
  exactly `dwFrameCount / 2`. Scale is constant in almost every clip, which is why `+0x058`
  equals `frameCount/2` in 16,762 of 17,249 (97.18 %) — a consequence, not a rule about time.
* All three are **≥ 1.0 in 17,249 / 17,249**, never zero and never negative. That is forced by
  the formula: a bone's keys carry distinct frame indices in `[0, dwFrameCount)`, so
  `Σ keys ≤ dwFrameCount × dwBoneCount` and the quotient cannot drop below 1. A time or
  duration field would have an unset or sentinel state; these three structurally cannot.

This retires the curated legacy names `flEndTime` (`+0x050`), `flTotalDuration` (`+0x054`) and
`flLoopDuration` (`+0x058`): none of the three is a time. The same curated table's guess at
`+0x048`, `flVelocity`, *is* corroborated — the engine registers that field as `DT_VELOCITY`.

The engine never reads any of the three (§13.1); they exist for the tools.

---

## 5. Bone Name Section

**Entry size**: 64 bytes. **Count**: `dwBoneCount`. **Total**: `dwBoneCount × 64`, verified in
17,249 of 17,249 permutations.

```cpp
struct BoneName { char szBoneName[64]; };       // NUL-padded ASCII
```

**A curve binds to a bone purely by array index.** `Anim_FindBoneIndexByName`
(`0x710020FD90`) scans this array and returns the index `i`; that same `i` selects
`TranslationCurve[i]`, `RotationCurve[i]` and `ScaleCurve[i]`. There is no bone id inside a
curve or a key. Names correspond to `BoneStructure.szName` in the referenced `.app`
(`Pelvis`, `Spine1`, `Head`, `R_Hand`, …).

---

## 6. Curve Descriptor Sections

Three arrays, one per channel, each with `dwBoneCount` entries.

```cpp
struct AnimCurve {                              // 24 bytes
    u32 dwKeyCount;             // 0x00
    u32 keyOffset;              // 0x04: file position = 16 + keyOffset
    u32 keyByteSize;            // 0x08: == dwKeyCount × elemSize
    u32 _pad;                   // 0x0C: 0 in 653,147 / 653,147 descriptors
    u64 _runtimePtr;            // 0x10: 0 in 653,147 / 653,147
};
```

| Channel     | Descriptor array at | Key size | Section size        |
|-------------|---------------------|----------|---------------------|
| Translation | `translCurveOffset` | 16 bytes | `boneCount × 24`    |
| Rotation    | `rotCurveOffset`    | 12 bytes | `boneCount × 24`    |
| Scale       | `scaleCurveOffset`  |  8 bytes | `boneCount × 24`    |

Key sizes measured as `keyByteSize / dwKeyCount` over **653,147 of 653,147** descriptors:
16, 12, 8, with no exceptions.

---

## 7. Keyframe Data

**There is no block compression.** Every channel is a flat array of fixed-size keys; the only
packing is the 16-bit quaternion.

### 7.1 Frame axis

Over 156,339 tracks sampled from 1,200 files:

* frame indices are **strictly increasing** in 156,339 / 156,339 tracks
* the first key is frame **0** in 156,339 / 156,339
* the last key is **`dwFrameCount − 1`** in 156,339 / 156,339

So every channel spans the whole clip, and `dwFrameCount == lastKeyIndex + 1` exactly.

```
seconds(frame) = frame / (flFramesPerTick × 60)
clip length in 1/60 s ticks = (dwFrameCount − 1) / flFramesPerTick
```

(the tick form is `Anim_GetDurationInTicks`, `0x710041A710`).

### 7.2 Translation key

```cpp
struct TranslationKey {                         // 16 bytes
    i32      nFrame;
    Vector3f vPosition;
};
```

### 7.3 Rotation key

```cpp
struct RotationKey {                            // 12 bytes
    i32 nFrame;
    i16 x, y, z, w;             // signed, divide by 32767.0f
};
```

**Signed / 32767 is proven, not assumed.** Over **1,008,455** rotation keys:

| decode | mean Σq² | sd | within 1 % of unity |
| --- | --- | --- | --- |
| `i16 / 32767` | **0.999960** | 0.000020 | **100.00 %** |
| `(u16/65535)·2 − 1` | 2.296039 | 0.471537 | 0.10 % |

*(The engine's reflection types the four components as `DT_WORD`; the data is signed
regardless.)*

### 7.4 Scale key

```cpp
struct ScaleKey {                               // 8 bytes
    i32 nFrame;
    f32 flScale;                // uniform; 1.0 in the large majority of keys
};
```

### 7.5 Coordinate conventions — **importer convention, not format**

The `× 17.0` scale and the `−y` negation the previous revision of this document listed under
"Decoding" are a **DCC import convention** carried over from the APP spec. Nothing in the
engine negates a translation or quaternion component. Apply them only if you are matching
that toolchain.

---

## 8. Keyframed Attachments (events)

**Entry size**: 412 bytes. **Count**: `dwAttachmentCount` (11,677 of 17,249 permutations have
at least one; 5,572 have none).

This is what the previous revision called the "Look Entry Section". It is a list of **timed
events** — spawn a sound, spawn a particle, attach something to a hardpoint, switch a look,
break a physics constraint.

```cpp
struct KeyframedAttachment {                    // 412 bytes
    f32          flFrame;       // +0x000: when in the clip it fires
    TriggerEvent tEvent;        // +0x004: 408 bytes
};

struct TriggerEvent {                           // 408 bytes
    u32  eEventType;            // +0x000: 22 distinct; correlates 1:1 with nTargetSnoGroup
    TriggerConditions tCond;    // +0x004: 36 bytes (percent, 4 × time, 2 × impulse, 2 × int)
    u32  _unknown028;           // +0x028: 3 (55.9%) / 1 (29.6%) / 0 (11.3%)
    i32  nTargetSnoGroup;       // +0x02C: 40=Sound (28,069), −1 (10,555), 27=Particle (7,570),
                                //         14 (3,234), 1=Actor, 38, 17=Explosion, 32, 45, 5
    u32  snoTarget;             // +0x030: 11,711 distinct ids
    u32  _unknown034;           // +0x034
    u32  _unknown038;           // +0x038
    u32  _zero03C;              // +0x03C: 0 in all 52,138
    u32  _zero040;              // +0x040: 0 in all 52,138
    char szHardpoint0[64];      // +0x044
    i32  nHardpointIndex0;      // +0x084
    char szHardpoint1[64];      // +0x088
    i32  nHardpointIndex1;      // +0x0C8
    char szLookName[64];        // +0x0CC
    char szConstraintName[64];  // +0x10C
    u32  dwAnimTagId;           // +0x14C: 69632 (0x11000) in >99.9%; AnimSet's tag id space
    f32  _unknown150;           // +0x150
    u32  _unknown154;           // +0x154
    u32  _unknown158;           // +0x158: 4 in 96.8%
    u32  _unknown15C;           // +0x15C: 2 (65.1%) / 0 (29.8%)
    u32  _unknown160;           // +0x160
    u32  _unknown164;           // +0x164
    f32  _unknown168;           // +0x168: default 1.0; 1.0 in 52,138/52,138
    f32  _unknown16C;           // +0x16C: default 2.0; 2.0 in 99.6%
    u32  _unknown170;           // +0x170
    f32  _unknown174;           // +0x174
    u32  _zero178;              // +0x178: 0 in all
    f32  flVelocity;            // +0x17C: 0 in all
    u32  _unknown180;           // +0x180
    i32  nLifetimeTicks;        // +0x184: default 600 (= 10 s at 60 Hz); 600 in 99.4%
    u32  dwColorA;              // +0x188: 0xFF000000 (61.4%), 0xFF808080 (35.1%), 0xFFFFFFFF
    i32  nFadeInTicks;          // +0x18C: default 15 (= 0.25 s); 15 in 38.3%, else 0
    u32  dwColorB;              // +0x190: 0xFFFFFFFF (65.5%), 0xFF808080 (34.3%)
    i32  nFadeOutTicks;         // +0x194: default 15; 15 in 38.3%
};
```

`DT_TIME` in this group is an **integer tick count** (1/60 s), not a float — the registered
defaults 600 and 15 appear in the corpus as the integers 600 and 15.

### 8.1 The names, over 52,138 attachment records

| slot | most common values |
| --- | --- |
| hardpoint (2 per record, 104,276 slots) | `Default` 63,203 · *(empty)* 17,661 · `HP_chest` 5,824 · `HP_Head` 2,898 · `HP_leftFoot` 2,015 · `HP_rightFoot` 1,748 · `HP_rightHand` 1,398 · `HP_leftHand` 1,146 · `HP_Pelvis` 865 · `HP_mouth` 596 |
| look name (2,904 non-empty) | `B` 1,280 · `A` 233 · `C` 194 · `invisible` 139 · `Invis` 139 · `A_death` 51 · `D` 50 · `Invisible` 39 |
| constraint name (999 non-empty) | `right_shoulder_right_elbow_rigidConstraint1` 85 · `left_shoulder_left_elbow_rigidConstraint1` 78 · `right_thigh_right_knee_rigidConstraint1` 70 · `Spine1_Neck_rigidConstraint1` 69 |

The look names are the Appearance look enum registered at `0x710069E4F0`
(`A`…`Z`, `Unique01`…`Unique99`); the constraint names are Appearance
`ConstraintParameters.szName`; the hardpoint names are Appearance `Hardpoint.szName`.
`"Default"` and `"A_riderless"` — the two names the previous revision listed as "look names" —
are in fact a **hardpoint** name and a **look** name respectively: `Default` appears 63,203
times in a hardpoint slot and **never** in a look slot, `A_riderless` appears once in a look
slot and **never** in a hardpoint slot.

---

## 9. Root Motion

Two arrays, each `dwFrameCount` × 12 bytes (verified 17,249 / 17,249 for both).
`Anim_GetRootMotionAtFrame` (`0x71003397A0`) indexes array A as `base + 12 × frameIndex`.

```cpp
struct RootMotionEntry { f32 x, y, z; };        // 12 bytes, one per frame
```

Array A holds **cumulative** displacement, and `vMovementVelocity` (permutation +0x140) is in
world units **per 1/60 s tick**. Both follow from one identity measured over the whole corpus:
for every permutation with a non-zero `vMovementVelocity.x` and a populated array,

```
rootMotion.x[frameCount−1] − rootMotion.x[0]
        ==  vMovementVelocity.x × ((frameCount−1) / flFramesPerTick)
```

held in **1353 of 1353** cases, and the x track is monotone in 1,345 of those.
Example — `Adria_BSS_event_walk_01.ani`, 45 frames:
`(0, 0.1167, 0.2333, 0.35, 0.4667, 0.5833, 0.7, 0.8167, …)`, a constant 0.11667/frame ramp.

Array A is non-zero in 2,050 of 17,249 permutations (walks, runs, charges).

### 9.1 Array B is the same motion "in place"

Array B is **array A with the constant-velocity component subtracted out**:

```
B[k]  ==  A[k]  −  A[frameCount−1] × k / (frameCount − 1)
```

So A is "play the clip exactly as authored, translation included", and B is "play it in place,
and let the game supply the translation" — which is why B always returns to (0,0,0) on the last
frame while A ends at the net displacement.

The two descriptors point at **different payload offsets in 17,249 / 17,249** permutations, so
these are genuinely two arrays and not one described twice. B is populated (non-zero) in only
**35** of them, and those 35 are the only records that can distinguish the formula from the
trivially-satisfied all-zero case. On **32 of the 35 it is bit-exact** — worst component error
0.00000, not merely within a tolerance.

Three permutations deviate, and they look hand-authored rather than exported:

| file | frames | `A[last]` | worst error |
| --- | ---: | --- | ---: |
| `SandShark_idle_intro_end_01.ani` | 20 | (15.991, 0, 0) | 12.481 |
| `SoulRipper_attack_04.ani` | 41 | (5.515, 0, 0) | 1.000 |
| `SandShark_idle_intro_01.ani` | 23 | (12.481, 0, 0) | 0.094 |

The first one is the giveaway: its error, 12.481, is exactly the *other* SandShark clip's total
displacement, so its in-place track was derived against `idle_intro`'s motion rather than its
own — the two clips are meant to be played back to back. Treat the identity as how the
exporter computes B, not as an invariant a reader may rely on.

`fastMummy_dodge_Left_01.ani` is the clearest witness. The dodge lunges −6.0 in Y over the
first six frames and then holds, while B traces the residual back to zero:

| frame | A (total) | straight line 0 → −6 | B (in place) |
| ---: | ---: | ---: | ---: |
| 0 | 0.0 | 0.0 | 0.0 |
| 3 | −4.5 | −1.0 | −3.5 |
| 6 | −6.0 | −2.0 | −4.0 |
| 9 | −6.0 | −3.0 | −3.0 |
| 12 | −6.0 | −4.0 | −2.0 |
| 15 | −6.0 | −5.0 | −1.0 |
| 18 | −6.0 | −6.0 | 0.0 |

The build's current revision keeps only array A, at +0x138/+0x140 — the one
`Anim_GetRootMotionAtFrame` samples.

### 9.2 The four permutation points

The four `Vector3D` at perm +0x0D8 are not per-frame data and not bounds; they are four fixed
points in animation-local space, addressed by index. Two engine functions read them, and both
spell the indexing out:

```c
// Anim_GetPermutationPointWorld (0x71003FF0B0)
if (index <= 3) {
    base = permutationArray + 360*permIndex + 240 + 12*index;   // v129 offsets
    p = { base[0], base[1], base[2] };
    p = rotate(p, actorQuat); p *= actorScale; p += actorPos;   // → world space
}
```

`0x71003FF0B0` takes the anim and permutation explicitly and bounds-checks the permutation
index against `Anim.dwPermutationCount` first; `Anim_GetPermutationPointWorld_FromActor`
(`0x71003FEEC0`) instead pulls the currently-playing anim and permutation from the actor via
`Anim_GetCurrentAnimAndPermutation` (`0x710041D720`). When nothing is playing, both fall back
to the **Actor's own position** (Actor +0x090), which is what a spawn origin does when it has
no animation to hang off.

The index is content-driven, not hard-coded: callers pass a field out of a power/skill
definition (`sub_7100937760` uses `*(u32*)(power + 1132)`). This is the same 0..3 index space
the marker frames use — `Anim_GetMarkerFrameTimeInTicks` is called with the immediates 0, 1, 2
and 3 at its twelve call sites — but nothing reads a frame and a point together, so the
pairing stays a hypothesis and the fields are named neutrally.

Occupancy over 17,249 permutations: point 0 non-zero in 2,062, point 1 in 3,150, point 2 in
3,132, point 3 in 3,124.

**All four share one registered default, and it is not the zero vector.**
`Anim_RegisterTypeDescriptors` passes `&qword_710185A38C` as the default for each of the four,
and the function prologue fills that block in three instructions:

```asm
0x710060B63C  STR   XZR, [X12, #0xC]!        ; X12 = 0x710185A38C; +0x0,+0x4 = 0.0, 0.0
0x710060B640  MOV   W9,  #0x40800000         ; 4.0f
0x710060B64C  STP   X9,  XZR, [X12, #8]      ; +0x8 = 4.0f
```

So the default point is **`(0, 0, 4)`** — 4 world units straight up from the actor origin,
which is a sensible "chest height" fallback for a spawn or aim target. It shows in the data:
10,447 of the 68,996 stored points are exactly `(0, 0, 4)`, against 57,528 left at all-zero.

---

## 10. Offset Convention

Every stored offset in a `.ani` (and in D3 SNO files generally) is **struct-relative**:

```
file position = 16 + stored_offset
```

The 16 bytes are the SNO header that precedes the struct image. The `Anim` struct itself
therefore begins at file `0x010`, and `permOffset == 56` puts the first permutation at file
`0x048`.

---

## 11. Corpus Statistics

### 11.1 General

| Metric | Value |
| --- | --- |
| Total files | 15,258 |
| Version | 118 (15,258 / 15,258) |
| Total permutations | 17,249 |
| Files with >1 permutation | 1,311 (8.6 %) |
| Permutation size | 408 (15,258 / 15,258) |
| Smallest file | 264 bytes |
| Largest file | 3,649,304 bytes |

### 11.2 Field distributions (all 17,249 permutations unless noted)

| Field | Offset | Most common | Share |
| --- | --- | --- | --- |
| `Anim.dwFlags` | struct +0x0C | 0 | 90.2 % |
| `Anim.dwFlags2` | struct +0x10 | 0 | 71.8 % |
| `dwFlags` | perm +0x000 | 0 | 84.3 % |
| `flFramesPerTick` | perm +0x048 | 0.5 (= 30 fps) | 85.7 % |
| `_unknown04C` | perm +0x04C | 5.0 | 99.0 % |
| `nBlendTicksFromOtherAnim` | perm +0x05C | 8 | 36.2 % |
| `nBlendTicksSamePermSwap` | perm +0x060 | 0 | 76.7 % |
| `dwSelectionWeight` | perm +0x064 | 100 | 88.2 % |
| `flSpeedScalar` | perm +0x068 | 1.0 | 94.1 % |
| `flMarkerFrame[0]` | perm +0x0C8 | −1.0 | 66.0 % |
| `flEventFrame` | perm +0x108 | −1.0 | 93.0 % |
| `vMovementVelocity` | perm +0x140 | (0,0,0) | 91.9 % |

`flFramesPerTick × 60` over the whole corpus yields exactly 30, 25, 27, 45, 40, 35 fps — real
authoring frame rates, which is the strongest single argument that this field is the frame
rate and +0x064 is not.

### 11.3 Model references

`Anim.snoAppearance` has 2,168 distinct values across the 15,258 files; the most-referenced
Appearance carries 391 animations.

### 11.4 Structural gate

Every structural claim in this document, checked on every file — not a sample. A claim that
does not reach 100 % here is not treated as a fact.

| Check | Passed | Of |
| --- | ---: | ---: |
| magic `0xDEADBEEF` | 15,258 | 15,258 |
| `arPermutations` element size == 408 | 15,258 | 15,258 |
| bone-name block == 64 × `dwBoneCount` | 17,249 | 17,249 |
| attachment block == 412 × `dwAttachmentCount` | 17,249 | 17,249 |
| curve block == 24 × `dwBoneCount` (×3 channels) | 51,747 | 51,747 |
| root-motion A == 12 × `dwFrameCount` | 17,249 | 17,249 |
| root-motion B sized as A | 17,249 | 17,249 |
| A and B at distinct payload offsets | 17,249 | 17,249 |
| permutation tail +0x190…0x198 all zero | 17,249 | 17,249 |
| curve key block == `dwKeyCount` × stride | 1,959,441 | 1,959,441 |
| key frames strictly ascending | 1,959,441 | 1,959,441 |
| first key at frame 0 | 1,959,441 | 1,959,441 |
| last key < `dwFrameCount` | 1,959,441 | 1,959,441 |
| rotation key unit-norm under signed `i16/32767` | 11,556,940 | 11,556,940 |
| keyframe density `+0x50/54/58` bit-exact in f32 (§4.1) | 51,747 | 51,747 |

Worst deviation of a rotation key's |q| from 1.0: **5.96e-05** — the 1/32767 quantisation step.
Read unsigned instead, the same keys average |q| ≈ 1.65.

---

## 12. Cross-References

| Related format | Ext | Relationship |
| --- | --- | --- |
| Appearance | `.app` | skeleton (`BoneStructure`), hardpoints, looks, constraints — all referenced by name from `.ani` |
| AnimSet | `.ans` | maps tag ids → `.ani` SNO references; same tag-id space as `TriggerEvent.dwAnimTagId` |
| AnimTree | `.ant` | state machine over AnimSets |
| Sound / Particle / Actor / Explosion | — | spawned by `TriggerEvent.snoTarget` |

---

## 13. Known Unknowns

| Field | Offset | What is known |
| --- | --- | --- |
| `Anim.dwNameHash` | struct +0x30 | 11,599 distinct over 15,258 files, never 0; hash algorithm not identified |
| `Anim.dwFlags` / `dwFlags2` | +0x0C / +0x10 | 13 and 2 distinct values; bit meanings unknown |
| `AnimPermutation.dwFlags` bit 0 | perm +0x000 | **what it does is now exact; what it is called is not.** `Anim_InitPlaybackState` stores `Anim_GetPermutationFlagBit0` (`perm[0] & 1`) into playback state +80 and then branches `if (bImmediate \|\| (P(owner) && !bit0))` → **blend time 0, i.e. an instant cut**; otherwise it computes the cross-fade from `+0x05C`/`+0x060`. So **bit 0 means "cross-fade even here"**: it suppresses the snap that predicate `P` would otherwise force. `P` is `sub_71000890E0`, which compares a field of the owner against `g_GameContext[24] == 18` — not named, so the *situation* the bit overrides is still open. Only ever 0 (14,534) or 1 (2,715) |
| `_unknown04C` | perm +0x04C | registered default 5.0, carried by 17,069 / 17,249 (98.96 %). The 180 that deviate are strikingly uniform: 3.0 ×85, 2.0 ×24 and 1.0 ×4 are **almost entirely character-selection-screen idle loops** (`*_Selection_Idle_*`, `*_Selection_Screen_idle`, `*_SelectScreen`) across every playable class; 0.0 ×64 is a mixed set of `*_Evade_*`, `Skeleton_assemble_*`, `OmniNPC_*_talk_*` and other looping idles; 10.0 ×3 is `assaultBeast_killGuard_foreshadow_idle_01` and `lacuniMale_attack_combo`. Consistent with a per-clip idle timing knob the selection screen tunes down, but **unread by the engine** (§13.1), so not named. The curated legacy name `flBlendWeight` is an unverified guess |
| `_unknown06C/070/074` | perm +0x06C…074 | One group of three hand-authored fractions, **unread by the engine** (§13.1). Invariants over 17,249 permutations: `+0x074` lies in [0,1] in **all** of them (a hard bound — note `+0x070` does *not*, reaching 10.0 in 199 records, correcting pass 3); all three are multiples of 0.05 in ≥ 99.5 %, so they are authored rather than computed; `+0x070 == 0` exactly when `+0x074 == 0` in 17,178 (99.59 %), so the pair gates together; `+0x070 == +0x074` in 16,132 (93.5 %) and `+0x070 >= +0x074` in 16,762 (97.18 %); `+0x06C != 0` implies `+0x070 == +0x074` in 17,226 (99.87 %); and the triple is constant across a file's permutations in 15,030 / 15,258 files. Two triples cover 87 % — `(0,0,0)` ×10,197 and the registered default `(0,1,1)` ×4,788 — the rest being 0.2/0.3/0.5-style fractions |
| `vPoint[i]` pairing | perm +0x0D8…0x104 | the array itself is **resolved** (§4, §9.2), including its `(0, 0, 4)` registered default — what remains open is only whether point `i` belongs to marker frame `i`. They share the same 0..3 index space, but no engine path reads both together |
| `TriggerEvent.eEventType` | attach +0x004 | 22 distinct values, 1:1 with the target SNO group; enum names not recovered |
| 15 `_unknown*` in `TriggerEvent` | see §8 | offsets and value censuses are established; meanings are not |

Two of 17,249 permutations have a non-ASCII or unterminated `szName`; not investigated.

`_unknown050` / `_unknown054` / `_unknown058` have left this table: they are keyframe density, §4.1.

### 13.1 Why these will not be named from this build

Two independent limits, both established rather than assumed.

**The engine's permutation read set is closed, and none of the above is in it.** Every
permutation access in 2.6.2 is the same idiom — `perm = *(Anim + 40) + 360 * index` — and the
playback state never caches the resulting pointer: `Anim_InitPlaybackState` (`0x710033B9D0`)
re-acquires the SNO and redoes the multiply on every call, as does its sibling
`sub_71003393A0`. Tainting the destination of that multiply and following it to its loads
therefore enumerates the engine's entire read set. Run over **all 167 functions** in the binary
that materialise 360 or 720 as an immediate, the readers found are `+0`, `+72`, `+92`, `+96`,
`+100`, `+104`, `+120`, `+144`, `+200`, `+216`, `+264`, `+320` and the array descriptors — and
**no reader for `+0x4C`, `+0x50`, `+0x54`, `+0x58`, `+0x6C`, `+0x70` or `+0x74`.**

Both residues were run down rather than waved off:

* The taint pass flags **5** loads at one of those seven offsets. All five are inspected and
  all five are artefacts of the taint not being killed on clobber: `0x71001FE958` and
  `0x71008B11D0` are stack accesses (base `SP`), `0x71008B115C` is a vtable slot
  (`X8 = *X20`, then `BLR X23`), and `0x71008B056C` / `0x71008B1388` load off `X0`
  immediately after a `BL`, i.e. off a call's return value.
* A separate sweep for an index-0 access with no multiply at all — `LDR Xd,[Xn,#40]` followed
  by a 32-bit load at one of the seven — returns **9** hits, all in functions that reference
  neither the Anim group descriptor nor any `SNO_*` entry point.

These fields are consumed by the authoring tools, which did not ship.

**Field names are stripped.** Every `TypeDesc_RegisterField_*` call in
`Anim_RegisterTypeDescriptors` (`0x710060B610`) passes the same placeholder pointer
`unk_7100D6A4A3` where a name would go, for all 45 fields. Type names *do* survive as real
strings (`"AnimPermutation"`, `"TranslationCurve"`, `"BoneName"`, …), which is why every
structure here has its shipped name and no field does. So for an unread field the only
remaining evidence is its registered type and default plus the corpus — which is exactly what
resolved `+0x50/54/58`, and is not enough for the four above.

---

## 14. Version 118 vs the build's revision 129

The Switch 2.6.2 build registers Anim with the version array `{118, 128, 128, 130, 129}`:
118 is the shipped data version (this corpus), 129 is the revision its compiled structs
describe. `Anim` is 56 bytes in **both**, but nothing under it matches:

| type | rev 129 | rule | v118 | corpus says |
| --- | --- | --- | --- | --- |
| `AnimPermutation` | 360 | −24 +56 +16 | 408 | 408 |
| `BoneName` | 4 | −4 +64 | 64 | 64 |
| `TranslationCurve` / `RotationCurve` / `ScaleCurve` | 16 | +8 | 24 | 24 |
| `KeyframedAttachment` | 192 | 4 + `TriggerEvent` | 412 | 412 |
| `TriggerEvent` | 188 | +2×56 +56 +56 −4 | 408 | 408 |
| `HardpointLink` | 12 | −8 +64 | 68 | 68 |
| `LookLink` / `ConstraintLink` | 8 | −8 +64 | 64 | 64 |

The `AnimPermutation` terms are: **−24** for the four fields at rev-129 +144…+168 (an int, an
int, a `SerializeData` and a `BoneBlendWeights[]`) — all four flagged `0x700000`, i.e. added
after the shipped revision; **+56** for the tail hardpoint growing from an 8-byte hash to an
inline `char[64] + i32`; **+16** for the second root-motion array that v118 has and 129 does
not. So everything from v118 +0x090 onwards sits 24 bytes below the registered offset (and a
further 16 below after +0x130).

**A matching root size does not mean the layouts agree.** Any reader keyed on the header
version must apply the v118 table above.

---

## Appendix A — Reading an ANI File (C++)

```cpp
FILE* f = fopen("animation.ani", "rb");

u8 hdr[16];  fread(hdr, 16, 1, f);
assert(*(u32*)hdr == 0xDEADBEEF);
assert(*(u16*)(hdr + 4) == 118);

Anim anim;  fread(&anim, sizeof(Anim), 1, f);          // 56 bytes, file 0x010

// ── §4  Every permutation, not just the first ────────────────────────────────
std::vector<AnimPermutation> perms(anim.dwPermutationCount);
fseek(f, 16 + anim.permOffset, SEEK_SET);              // permOffset == 56 always
fread(perms.data(), sizeof(AnimPermutation), perms.size(), f);

for (const AnimPermutation& p : perms) {
    float fps      = p.flFramesPerTick * 60.0f;        // NOT the field at +0x064
    float seconds  = (p.dwFrameCount - 1) / fps;
    printf("'%s'  %u bones  %u frames  %.2f fps  %.3f s  weight %u\n",
           p.szName, p.dwBoneCount, p.dwFrameCount, fps, seconds, p.dwSelectionWeight);

    // ── §5  Bone names — the index into this array IS the track index ────────
    std::vector<BoneName> bones(p.dwBoneCount);
    fseek(f, 16 + p.boneNameOffset, SEEK_SET);
    fread(bones.data(), sizeof(BoneName), bones.size(), f);

    // ── §6/§7  One channel; repeat for rot (12 B keys) and scale (8 B keys) ──
    std::vector<AnimCurve> tc(p.dwBoneCount);
    fseek(f, 16 + p.translCurveOffset, SEEK_SET);
    fread(tc.data(), sizeof(AnimCurve), tc.size(), f);

    for (u32 i = 0; i < p.dwBoneCount; ++i) {
        fseek(f, 16 + tc[i].keyOffset, SEEK_SET);
        for (u32 k = 0; k < tc[i].dwKeyCount; ++k) {
            TranslationKey kf;  fread(&kf, sizeof(kf), 1, f);
            addTranslationKey(bones[i].szBoneName, kf.nFrame / fps, kf.vPosition);
        }
    }

    // Rotation keys decode as: q = { x, y, z, w } / 32767.0f   (signed)

    // ── §9  Root motion: cumulative translation, one Vector3D per frame ──────
    std::vector<float> root(3 * p.dwFrameCount);
    if (p.rootMotionTotalSize) {
        fseek(f, 16 + p.rootMotionTotalOffset, SEEK_SET);
        fread(root.data(), 1, p.rootMotionTotalSize, f);
    }

    // ── §8  Timed events ─────────────────────────────────────────────────────
    std::vector<KeyframedAttachment> ev(p.dwAttachmentCount);
    if (p.dwAttachmentCount) {
        fseek(f, 16 + p.attachOffset, SEEK_SET);
        fread(ev.data(), sizeof(KeyframedAttachment), ev.size(), f);
    }
}
fclose(f);
```

---

## Appendix B — All Structures Summary

```cpp
struct Anim {                                   // 56 bytes
    u32 dwSnoId; u32 _z04; u32 _z08;
    u32 dwFlags; u32 dwFlags2; u32 snoAppearance;
    u32 permOffset;  u32 permSize;  u32 dwPermutationCount;
    u32 _pad; u64 _ptr; u32 dwNameHash; u32 _pad2;
};

struct AnimPermutation {                        // 408 bytes
    u32  dwFlags;  char szName[68];
    f32  flFramesPerTick; f32 _u04C;
    f32  flFramesPerTranslationKey, flFramesPerRotationKey, flFramesPerScaleKey;
    i32  nBlendTicksFromOtherAnim, nBlendTicksSamePermSwap;
    u32  dwSelectionWeight; f32 flSpeedScalar, _u06C, _u070, _u074;
    u32  dwBoneCount,   boneNameOffset,   boneNameSize;   u32 _p; u64 _ptr;
    u32  dwFrameCount,  translCurveOffset, translCurveSize; u32 _p; u64 _ptr;
    u32  rotCurveOffset,   rotCurveSize;   u64 _ptr;
    u32  scaleCurveOffset, scaleCurveSize; u64 _ptr;
    f32  flMarkerFrame[4]; f32 _unknownVec[4][3]; f32 flEventFrame;
    u32  attachOffset, attachSize, dwAttachmentCount; u64 _ptr;
    u32  rootMotionTotalOffset, rootMotionTotalSize; u64 _ptr;
    u32  rootMotionInPlaceOffset, rootMotionInPlaceSize; u64 _ptr;
    f32  vMovementVelocity[3];
    char szHardpointName[64]; i32 nHardpointIndex; u8 _tail[8];
};

struct BoneName  { char szBoneName[64]; };      // 64 bytes
struct AnimCurve { u32 dwKeyCount, keyOffset, keyByteSize, _pad; u64 _ptr; }; // 24

struct TranslationKey { i32 nFrame; f32 x, y, z; };       // 16
struct RotationKey    { i32 nFrame; i16 x, y, z, w; };    // 12, /32767.0f
struct ScaleKey       { i32 nFrame; f32 flScale; };       //  8

struct KeyframedAttachment { f32 flFrame; TriggerEvent tEvent; };  // 412
struct RootMotionEntry     { f32 x, y, z; };                       //  12
```

---

*Derived from the Diablo III Nintendo Switch 2.6.2 binary (`DiabloIIINX64r`) and 15,258 ANI
files from Diablo III: Reaper of Souls. Correction pass 2 dated 2026-08-15; see the
correction block at the top for what changed and why.*
