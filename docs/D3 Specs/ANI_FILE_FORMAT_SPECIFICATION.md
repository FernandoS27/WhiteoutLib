# ANI File Format Specification

**Format**: Diablo III Animation Clip (`.ani`)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**Version**: 118
**Corpus**: 15,258 files analyzed

---

## Table of Contents

1. [Overview](#1-overview)
2. [File Layout](#2-file-layout)
3. [SNO Preamble](#3-sno-preamble)
4. [Sub-Animation Block](#4-sub-animation-block)
5. [Bone Name Section](#5-bone-name-section)
6. [Track Descriptor Sections](#6-track-descriptor-sections)
7. [Keyframe Data](#7-keyframe-data)
8. [Look Entry Section](#8-look-entry-section)
9. [Per-Frame Auxiliary Sections](#9-per-frame-auxiliary-sections)
10. [Offset Convention](#10-offset-convention)
11. [Corpus Statistics](#11-corpus-statistics)
12. [Cross-References](#12-cross-references)
13. [Known Unknowns](#13-known-unknowns)
14. [Appendix A — Reading an ANI File (C++)](#appendix-a--reading-an-ani-file-c)
15. [Appendix B — All Structures Summary](#appendix-b--all-structures-summary)

---

## 1. Overview

ANI files store skeletal animation clips for Diablo III models. Each file contains one or more
sub-animation blocks (typically one), referencing per-bone keyframe data for translation,
rotation, and scale channels. Animations are linked to models via a `modelSnoRef` field in the
preamble and organized into animation state slots by AnimSet (`.ans`) files.

The animation pipeline:

```
AnimTree (.ant)  →  AnimSet (.ans)  →  Animation (.ani)  →  Model (.app)
  state machine      state→clip map      keyframe data       skeleton
```

---

## 2. File Layout

```
┌─────────────────────────────────────────────────────────┐
│  SNO Preamble                               (56 bytes)  │
│    0x000: magic, version, snoId,                        │
│           animFlags, modelSnoRef, block ptr              │
├─────────────────────────────────────────────────────────┤
│  Sub-Animation Block(s)              (N × 408 bytes)    │
│    0x038: animation name, timing, section refs          │
├─────────────────────────────────────────────────────────┤
│  Bone Name Section                 (boneCount × 64)     │
├─────────────────────────────────────────────────────────┤
│  Translation Descriptors           (boneCount × 24)     │
├─────────────────────────────────────────────────────────┤
│  Translation Keyframes                      (variable)  │
├─────────────────────────────────────────────────────────┤
│  Rotation Descriptors              (boneCount × 24)     │
├─────────────────────────────────────────────────────────┤
│  Rotation Keyframes                         (variable)  │
├─────────────────────────────────────────────────────────┤
│  Scale Descriptors                 (boneCount × 24)     │
├─────────────────────────────────────────────────────────┤
│  Scale Keyframes                            (variable)  │
├─────────────────────────────────────────────────────────┤
│  Look Entry Section                (N × 412 bytes)      │
├─────────────────────────────────────────────────────────┤
│  Per-Frame Auxiliary Sections ×2   (frameCount × 12)    │
└─────────────────────────────────────────────────────────┘
```

Data sections are tightly packed. All stored offsets use the +16 convention (§10).

---

## 3. SNO Preamble

**Tag**: ANI | **Version**: 118 | **Size**: 56 bytes

The ANI preamble extends the standard 32-byte SNO header with 24 bytes of animation-specific
fields.

```cpp
struct AniPreamble {                            // 56 bytes
    // ─── Standard SNO Header (0x000–0x01F) ─────────────────────────────────────
    u32     magic;              // 0x000: Always 0xDEADBEEF
    u32     version;            // 0x004: Always 118 for .ani
    u32     _reserved008[2];    // 0x008: Unknown (typically zeros)
    u32     snoId;              // 0x010: Unique SNO identifier
    u32     _reserved014[3];    // 0x014: Unknown

    // ─── ANI-Specific Fields (0x020–0x037) ─────────────────────────────────────
    u32     animFlags;          // 0x020: 0 = normal (94%), 2 = alternate (6%)
    u32     modelSnoRef;        // 0x024: SNO ref to parent .app model
    u32     blockOffset;        // 0x028: Offset to first sub-anim block (always 56)
    u32     blockSize;          // 0x02C: Total size of all blocks (blockCount × 408)
    u32     blockCount;         // 0x030: Number of sub-animation blocks (typically 1)
    u32     _reserved034;       // 0x034: Unknown (typically 0)
};
```

**`animFlags`**: Observed values are 0 (most files) and 2 (a subset). Exact semantics unknown,
possibly related to looping or root motion behavior.

**`modelSnoRef`**: Links this animation to a specific model. Multiple `.ani` files can reference
the same model — 55 unique model references were found across a 500-file sample, with the most
common model (`0x0031B8`) having 247 animations.

---

## 4. Sub-Animation Block

**Tag**: SubAnimBlock | **Version**: — | **Size**: 408 bytes

Each sub-animation block describes one animation clip with its timing parameters, bone track
references, and auxiliary data sections.

```cpp
struct SubAnimBlock {                           // 408 bytes (0x198)
    // ─── Alignment / Identification ────────────────────────────────────────────
    u32     _pad000[2];         // +0x000: Alignment padding (zeros)
    u32     nameHash;           // +0x008: Hash of animation name
    u32     _unknown00C;        // +0x00C: Unknown (typically 0)
    u32     flag;               // +0x010: 0 or 1 (6% have flag=1)

    // ─── Animation Name ────────────────────────────────────────────────────────
    char    animationName[64];  // +0x014: Null-padded ASCII name

    // ─── Timing Parameters ─────────────────────────────────────────────────────
    f32     _unknownFloat054;   // +0x054: Unknown float (typically 0)
    f32     velocity;           // +0x058: Movement velocity (typically 0.5)
    f32     blendWeight;        // +0x05C: Blend weight (typically 5.0, also 3.0, 2.0)
    f32     endTime;            // +0x060: End time in engine ticks
    f32     totalDuration;      // +0x064: Total duration in engine ticks
    f32     loopDuration;       // +0x068: Loop point / duration variant
    u32     flags1;             // +0x06C: Behavioral flags
    u32     flags2;             // +0x070: Behavioral flags
    u32     fps;                // +0x074: Frames per second (always 100)
    f32     speed;              // +0x078: Playback speed multiplier (typically 1.0–1.5)
    f32     _unknownFloats07C[3]; // +0x07C: Unknown (typically 0)

    // ─── Bone Name Section Reference ───────────────────────────────────────────
    u32     boneCount;          // +0x088: Number of animated bones
    u32     boneNameOffset;     // +0x08C: Offset to bone name array (+16)
    u32     boneNameSize;       // +0x090: Size = boneCount × 64
    u32     _pad094[3];         // +0x094: Padding (zeros)

    // ─── Translation Descriptor Section ────────────────────────────────────────
    u32     frameCount;         // +0x0A0: Total frame count (= max_frame_index + 1)
    u32     translDescOffset;   // +0x0A4: Offset to translation descriptors (+16)
    u32     translDescSize;     // +0x0A8: Size = boneCount × 24
    u32     _pad0AC[3];         // +0x0AC: Padding (zeros)

    // ─── Rotation Descriptor Section ───────────────────────────────────────────
    u32     rotDescOffset;      // +0x0B8: Offset to rotation descriptors (+16)
    u32     rotDescSize;        // +0x0BC: Size = boneCount × 24
    u32     _pad0C0[2];         // +0x0C0: Padding (zeros)

    // ─── Scale Descriptor Section ──────────────────────────────────────────────
    u32     scaleDescOffset;    // +0x0C8: Offset to scale descriptors (+16)
    u32     scaleDescSize;      // +0x0CC: Size = boneCount × 24
    u32     _pad0D0[2];         // +0x0D0: Padding (zeros)

    // ─── Reference Transform ───────────────────────────────────────────────────
    f32     restQuaternion[4];  // +0x0D8: Reference/rest quaternion
                                //         Common: (-1.0, 1.0, 0.0, 1.0) or
                                //         (value, 1.0, 0.0, 1.0)

    // ─── Additional Parameters ─────────────────────────────────────────────────
    f32     _unknownFloats0E8[12]; // +0x0E8: Unknown float parameters
    f32     sentinel;           // +0x118: Sentinel value (typically -1.0)

    // ─── Look Entry Section Reference ──────────────────────────────────────────
    u32     lookEntryOffset;    // +0x11C: Offset to look entries (+16)
    u32     lookEntrySize;      // +0x120: Size = lookEntryCount × 412
    u32     lookEntryCount;     // +0x124: Number of look/variant entries
    u32     _pad128[2];         // +0x128: Padding (zeros)

    // ─── Per-Frame Auxiliary Section 1 ─────────────────────────────────────────
    u32     perFrameOffset1;    // +0x130: Offset to per-frame data 1 (+16)
    u32     perFrameSize1;      // +0x134: Size = frameCount × 12
    u32     _pad138[2];         // +0x138: Padding (zeros)

    // ─── Per-Frame Auxiliary Section 2 ─────────────────────────────────────────
    u32     perFrameOffset2;    // +0x140: Offset to per-frame data 2 (+16)
    u32     perFrameSize2;      // +0x144: Size = frameCount × 12
    u32     _pad148[20];        // +0x148: Padding / unused (zeros)
};
```

**`frameCount`** at +0x0A0 is definitively the total number of frames in the animation, equal
to the maximum frame index across all keyframes plus one. Verified at 100% (500/500 files).

**`fps`** is always 100 across all tested files, meaning frame indices translate to time as
`time_seconds = frame_index / 100.0`.

---

## 5. Bone Name Section

**Entry size**: 64 bytes per bone
**Count**: `boneCount` from sub-animation block
**Total size**: `boneCount × 64`

```cpp
struct BoneNameEntry {                          // 64 bytes
    char    boneName[64];       // 0x00: Null-padded ASCII bone name
};
```

Bone names correspond to the skeleton defined in the referenced `.app` model file.
Examples: `"Pelvis"`, `"Spine1"`, `"Head"`, `"R_Hand"`.

The bone name section is accessed at `boneNameOffset + 16` (see §10).

---

## 6. Track Descriptor Sections

Three separate descriptor arrays exist — one each for translation, rotation, and scale. Each
array has `boneCount` entries.

### 6.1 Per-Bone Track Descriptor

```cpp
struct BoneTrackDescriptor {                    // 24 bytes
    u32     keyframeCount;      // 0x00: Number of keyframes for this bone/channel
    u32     keyframeOffset;     // 0x04: Offset to keyframe array (+16)
    u32     keyframeSize;       // 0x08: Total keyframe data size (count × kfSize)
    u32     _pad[3];            // 0x0C: Padding (zeros)
};
```

### 6.2 Descriptor Arrays

| Channel     | Location              | Keyframe Size | Section Size           |
|-------------|-----------------------|---------------|------------------------|
| Translation | `translDescOffset+16` | 16 bytes/kf   | `boneCount × 24`      |
| Rotation    | `rotDescOffset+16`    | 12 bytes/kf   | `boneCount × 24`      |
| Scale       | `scaleDescOffset+16`  | 8 bytes/kf    | `boneCount × 24`      |

---

## 7. Keyframe Data

### 7.1 Translation Keyframe

```cpp
struct TranslationKeyframe {                    // 16 bytes
    u32         frame;          // 0x00: Frame index (0-based, time = frame / 100.0)
    Vector3f    position;       // 0x04: Translation vector (x, y, z)
};
```

**Coordinate transform**: When importing into 3D applications, apply ×17.0 scale factor and
negate Y: `pos = (x × 17.0, -y × 17.0, z × 17.0)`. See `APP_FILE_FORMAT_SPECIFICATION.md` §16.

### 7.2 Rotation Keyframe

```cpp
struct RotationKeyframe {                       // 12 bytes
    u32     frame;              // 0x00: Frame index
    i16     qx;                 // 0x04: Quaternion X (short)
    i16     qy;                 // 0x06: Quaternion Y (short)
    i16     qz;                 // 0x08: Quaternion Z (short)
    i16     qw;                 // 0x0A: Quaternion W (short)
};
```

**Decoding**: Normalize `i16` components by dividing by 32767.0:

```cpp
Quaternion rot = {
     qx / 32767.0f,
    -qy / 32767.0f,            // Y is negated for coordinate system conversion
     qz / 32767.0f,
     qw / 32767.0f
};
```

### 7.3 Scale Keyframe

```cpp
struct ScaleKeyframe {                          // 8 bytes
    u32     frame;              // 0x00: Frame index
    f32     scale;              // 0x04: Uniform scale factor
};
```

### 7.4 Example Data

From `NPC_Human_Male_Cellar_Arm_idle_01.ani`, bone "Pelvis" (bone 0):

| Frame | Channel     | Values                              |
|-------|-------------|-------------------------------------|
| 0     | Translation | (-0.1936, -0.3746, 0.2859)          |
| 2     | Translation | (-0.1936, -2.2535, 1.6196)          |
| 0     | Rotation    | (0.0012, -0.0008, 0.0001, 1.0000)   |
| 2     | Rotation    | (0.0012, -0.0008, 0.0001, 1.0000)   |

---

## 8. Look Entry Section

**Entry size**: 412 bytes
**Count**: `lookEntryCount` from sub-animation block

Look entries define animation variants for different visual configurations ("looks") of the
model.

```cpp
struct LookEntry {                              // 412 bytes
    f32     restValue;          // +0x000: Rest/reference value
    u32     parameter1;         // +0x004: Variant parameter
    u32     parameter2;         // +0x008: Typically 0xFF (255)
    u8      _unknown00C[36];    // +0x00C: Unknown fields
    u32     sentinel1;          // +0x030: Often 0xFFFFFFFF
    u32     sentinel2;          // +0x034: Often 0xFFFFFFFF
    u8      _unknown038[16];    // +0x038: Unknown
    char    lookName[64];       // +0x048: Variant name (null-padded ASCII)
    u8      _remainingData[276]; // +0x088: Remaining variant-specific data
};
```

### Known Look Names

| Name           | Meaning                            |
|----------------|------------------------------------|
| `"Default"`    | Default animation variant          |
| `"A_riderless"`| Riderless mount variant            |

---

## 9. Per-Frame Auxiliary Sections

Two per-frame data sections exist, both with identical sizing:

**Size**: `frameCount × 12` bytes each
**Entry size**: 12 bytes per frame

```cpp
struct PerFrameEntry {                          // 12 bytes
    f32     data[3];            // 0x00: Three float values per frame
};
```

These sections likely store per-frame bounding volumes or root motion deltas. In many files,
both sections contain all zeros. The relationship `sectionSize = frameCount × 12` was verified
at 100% across 500 files.

---

## 10. Offset Convention

All stored offsets in `.ani` files (and D3 SNO files generally) use the **+16 convention**:

```
actual_data_position = stored_offset + 16
```

This is a global offset adjustment applied to all section references. Data sections are tightly
packed — the convention likely derives from internal engine serialization where a 16-byte
metadata header precedes each data array.

**Example**: If `boneNameOffset = 464`, the actual bone name data begins at file position
464 + 16 = 480.

---

## 11. Corpus Statistics

### 11.1 General

| Metric               | Value         |
|----------------------|---------------|
| Total files          | 15,258        |
| Version              | 118 (all)     |
| FPS                  | 100 (all)     |
| Smallest file        | 264 bytes     |
| Largest file         | 3,649,304 bytes |
| Block count          | 1 (typical)   |
| Block size           | 408 bytes     |

### 11.2 Field Distributions (N=1000)

| Field            | Offset   | Most Common Value | Percentage |
|------------------|----------|-------------------|------------|
| `animFlags`      | 0x020    | 0                 | 94%        |
| `flag`           | +0x010   | 0                 | 94%        |
| `fps`            | +0x074   | 100               | 100%       |
| `velocity`       | +0x058   | 0.5               | ~95%       |
| `blendWeight`    | +0x05C   | 5.0               | ~85%       |
| `speed`          | +0x078   | 1.0               | ~70%       |
| `restQuaternion` | +0x0D8   | (-1.0, 1.0, 0.0, 1.0) | ~80%  |

### 11.3 Model References

From a 500-file sample: 55 unique `modelSnoRef` values. The most referenced model
(`0x0031B8`) has 247 animations, indicating character models with large animation sets.

---

## 12. Cross-References

| Related Format | Extension | Relationship                                      |
|----------------|-----------|---------------------------------------------------|
| Appearance     | `.app`    | Parent model, skeleton definition (via `modelSnoRef`) |
| AnimSet        | `.ans`    | Maps animation state slots → `.ani` SNO references |
| AnimTree       | `.ant`    | State machine (insufficient data to reverse-engineer) |
| Material       | `.mat`    | Referenced by `.app`, not directly by `.ani`        |

```
Actor (.acr)
  └── AnimSet (.ans)
        └── Animation (.ani)  ← this format
              └── Appearance (.app)  — skeleton via modelSnoRef
```

---

## 13. Known Unknowns

| Field(s)                  | Offset(s)      | Notes                                          |
|---------------------------|----------------|-------------------------------------------------|
| `_reserved008[2]`         | 0x008–0x00F    | Standard SNO header, purpose unknown            |
| `_reserved014[3]`         | 0x014–0x01F    | Standard SNO header, purpose unknown            |
| `nameHash`                | +0x008         | Appears to be a hash; algorithm unknown         |
| `flag`                    | +0x010         | 0 or 1 (6% have 1); exact meaning unknown      |
| `_unknownFloat054`        | +0x054         | Float, typically 0; might be start time         |
| `endTime/duration` fields | +0x060–0x068   | Three float values; exact semantics unclear     |
| `flags1/flags2`           | +0x06C–0x070   | Behavioral flags, bit meanings unknown          |
| `restQuaternion`          | +0x0D8         | 4 floats; relationship to animation unclear     |
| `_unknownFloats0E8`       | +0x0E8–0x117   | 12 floats; mostly zero, some have 4.0 constant  |
| `sentinel`                | +0x118         | Typically -1.0; sentinel/flag purpose unknown   |
| Look entry internals      | §8             | Most of the 412-byte entry structure is unknown |
| Per-frame aux data        | §9             | 12 bytes/frame × 2 sections; often all zeros   |

---

## Appendix A — Reading an ANI File (C++)

```cpp
FILE* f = fopen("animation.ani", "rb");

// ── §3  SNO Preamble ──────────────────────────────────────────────────────────
AniPreamble preamble;
fread(&preamble, sizeof(AniPreamble), 1, f);
assert(preamble.magic == 0xDEADBEEF);
assert(preamble.version == 118);

printf("SNO ID: 0x%08X  Model: 0x%08X  Blocks: %u\n",
       preamble.snoId, preamble.modelSnoRef, preamble.blockCount);

// ── §4  Sub-Animation Block ───────────────────────────────────────────────────
SubAnimBlock block;
fseek(f, preamble.blockOffset, SEEK_SET);
fread(&block, sizeof(SubAnimBlock), 1, f);

printf("Animation: '%s'  FPS: %u  Frames: %u  Bones: %u\n",
       block.animationName, block.fps, block.frameCount, block.boneCount);

// ── §5  Bone Names ────────────────────────────────────────────────────────────
std::vector<BoneNameEntry> boneNames(block.boneCount);
fseek(f, block.boneNameOffset + 16, SEEK_SET);
fread(boneNames.data(), sizeof(BoneNameEntry), block.boneCount, f);

// ── §6  Translation Descriptors ───────────────────────────────────────────────
std::vector<BoneTrackDescriptor> translDescs(block.boneCount);
fseek(f, block.translDescOffset + 16, SEEK_SET);
fread(translDescs.data(), sizeof(BoneTrackDescriptor), block.boneCount, f);

// ── §7  Read Translation Keyframes for Each Bone ──────────────────────────────
for (u32 i = 0; i < block.boneCount; i++) {
    BoneTrackDescriptor& desc = translDescs[i];
    fseek(f, desc.keyframeOffset + 16, SEEK_SET);

    for (u32 k = 0; k < desc.keyframeCount; k++) {
        TranslationKeyframe kf;
        fread(&kf, sizeof(TranslationKeyframe), 1, f);
        // Apply coordinate transform (§16 in APP spec):
        f32 px =  kf.position.x * 17.0f;
        f32 py = -kf.position.y * 17.0f;
        f32 pz =  kf.position.z * 17.0f;
        addTranslationKey(i, kf.frame, px, py, pz);
    }
}

// ── §6–7  Rotation Descriptors + Keyframes (same pattern) ─────────────────────
std::vector<BoneTrackDescriptor> rotDescs(block.boneCount);
fseek(f, block.rotDescOffset + 16, SEEK_SET);
fread(rotDescs.data(), sizeof(BoneTrackDescriptor), block.boneCount, f);

for (u32 i = 0; i < block.boneCount; i++) {
    BoneTrackDescriptor& desc = rotDescs[i];
    fseek(f, desc.keyframeOffset + 16, SEEK_SET);

    for (u32 k = 0; k < desc.keyframeCount; k++) {
        RotationKeyframe kf;
        fread(&kf, sizeof(RotationKeyframe), 1, f);
        f32 qx =  kf.qx / 32767.0f;
        f32 qy = -kf.qy / 32767.0f;   // Y negated
        f32 qz =  kf.qz / 32767.0f;
        f32 qw =  kf.qw / 32767.0f;
        addRotationKey(i, kf.frame, qx, qy, qz, qw);
    }
}

// ── §6–7  Scale Descriptors + Keyframes ───────────────────────────────────────
std::vector<BoneTrackDescriptor> scaleDescs(block.boneCount);
fseek(f, block.scaleDescOffset + 16, SEEK_SET);
fread(scaleDescs.data(), sizeof(BoneTrackDescriptor), block.boneCount, f);

for (u32 i = 0; i < block.boneCount; i++) {
    BoneTrackDescriptor& desc = scaleDescs[i];
    fseek(f, desc.keyframeOffset + 16, SEEK_SET);

    for (u32 k = 0; k < desc.keyframeCount; k++) {
        ScaleKeyframe kf;
        fread(&kf, sizeof(ScaleKeyframe), 1, f);
        addScaleKey(i, kf.frame, kf.scale);
    }
}

fclose(f);
```

---

## Appendix B — All Structures Summary

```cpp
// §3 — ANI Preamble (extended 56-byte variant)
struct AniPreamble {                            // 56 bytes
    u32 magic;                  // 0xDEADBEEF
    u32 version;                // 118
    u32 _reserved08[2];
    u32 snoId;
    u32 _reserved14[3];
    u32 animFlags;              // 0 or 2
    u32 modelSnoRef;            // → .app model
    u32 blockOffset;            // Always 56
    u32 blockSize;              // blockCount × 408
    u32 blockCount;             // Typically 1
    u32 _reserved034;
};

// §4 — Sub-Animation Block (see full struct for all fields)
struct SubAnimBlock {                           // 408 bytes
    u32 _pad[2]; u32 nameHash; u32 _unk; u32 flag;
    char animationName[64];
    f32 _unkF054; f32 velocity; f32 blendWeight;
    f32 endTime; f32 totalDuration; f32 loopDuration;
    u32 flags1; u32 flags2; u32 fps; f32 speed; f32 _unkF07C[3];
    u32 boneCount; u32 boneNameOffset; u32 boneNameSize; u32 _p094[3];
    u32 frameCount; u32 translDescOffset; u32 translDescSize; u32 _p0AC[3];
    u32 rotDescOffset; u32 rotDescSize; u32 _p0C0[2];
    u32 scaleDescOffset; u32 scaleDescSize; u32 _p0D0[2];
    f32 restQuaternion[4]; f32 _unkF0E8[12]; f32 sentinel;
    u32 lookEntryOffset; u32 lookEntrySize; u32 lookEntryCount; u32 _p128[2];
    u32 perFrameOffset1; u32 perFrameSize1; u32 _p138[2];
    u32 perFrameOffset2; u32 perFrameSize2; u32 _p148[20];
};

// §5 — Bone Name Entry
struct BoneNameEntry { char boneName[64]; };    // 64 bytes

// §6 — Bone Track Descriptor
struct BoneTrackDescriptor {                    // 24 bytes
    u32 keyframeCount; u32 keyframeOffset; u32 keyframeSize; u32 _pad[3];
};

// §7 — Keyframe Structures
struct TranslationKeyframe {                    // 16 bytes
    u32 frame; Vector3f position;
};
struct RotationKeyframe {                       // 12 bytes
    u32 frame; i16 qx, qy, qz, qw;
};
struct ScaleKeyframe {                          // 8 bytes
    u32 frame; f32 scale;
};

// §8 — Look Entry
struct LookEntry {                              // 412 bytes
    f32 restValue; u32 parameter1; u32 parameter2;
    u8 _unk[36]; u32 sentinel1; u32 sentinel2; u8 _unk2[16];
    char lookName[64]; u8 _remaining[276];
};

// §9 — Per-Frame Auxiliary Entry
struct PerFrameEntry { f32 data[3]; };          // 12 bytes
```

---

*Specification derived from binary analysis of 15,258 ANI files from Diablo III: Reaper of Souls.*
