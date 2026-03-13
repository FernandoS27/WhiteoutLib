# APP File Format Specification

**Format**: Diablo III Appearance Model Format (`.app`)  
**Byte Order**: Little-endian  
**Magic**: `0xDEADBEEF`  
**Version**: 260  
**Corpus**: 11,347 files analyzed

---

## Table of Contents

1.  [File Structure Overview](#1-file-structure-overview)
2.  [Primitive Types](#2-primitive-types)
3.  [File Header](#3-file-header)
4.  [Data Access Convention](#4-data-access-convention)
5.  [Material Entries](#5-material-entries)
6.  [Look Table](#6-look-table)
7.  [Bones](#7-bones)
8.  [Submeshes (Geosets)](#8-submeshes-geosets)
9.  [Vertices](#9-vertices)
10. [Vertex Weights](#10-vertex-weights)
11. [Indices (Faces)](#11-indices-faces)
12. [Reference Points (Hardpoints)](#12-reference-points-hardpoints)
13. [Collision Data](#13-collision-data)
14. [Extra Data Blocks](#14-extra-data-blocks)
15. [Animation Pipeline](#15-animation-pipeline)
16. [Coordinate System & Import Conventions](#16-coordinate-system--import-conventions)
17. [Reading an APP File](#17-reading-an-app-file)
18. [Known Unknowns](#18-known-unknowns)
19. [Appendix A — File Statistics](#appendix-a--file-statistics)
20. [Appendix B — All Structures Summary](#appendix-b--all-structures-summary)
21. [Appendix C — Credits & Sources](#appendix-c--credits--sources)
22. [Appendix D — Comparison with Related Formats](#appendix-d--comparison-with-related-formats)

---

## 1. File Structure Overview

APP files are binary 3D model containers used by Diablo III's appearance (`.app`) system. Each file contains a single model with geometry, skeleton, materials, collision volumes, and attachment points. Textures are referenced externally or embedded as material shader data blocks.

```
┌─────────────────────────────────────────────────────────────┐
│  File Header                                  (552 bytes)   │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Core Header Fields                      (536 bytes)  │  │
│  │  Padding                                  (16 bytes)  │  │
│  └───────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│  Material Entries              (N × 144 bytes + 16 pad)     │
├─────────────────────────────────────────────────────────────┤
│  Material Data Blocks          (variable, per-material)     │
├─────────────────────────────────────────────────────────────┤
│  Look Table                    (N × 64 bytes + 16 pad)      │
├─────────────────────────────────────────────────────────────┤
│  Bone Entries                  (N × 312 bytes + 16 pad)     │
├─────────────────────────────────────────────────────────────┤
│  Submesh Entries               (N × 400 bytes + 16 pad)     │
├─────────────────────────────────────────────────────────────┤
│  Vertex Buffers                (per submesh, 44 bytes/vert) │
├─────────────────────────────────────────────────────────────┤
│  Weight Buffers                (per submesh, 24 bytes/vert) │
├─────────────────────────────────────────────────────────────┤
│  Index Buffers                 (per submesh, 6 bytes/tri)   │
├─────────────────────────────────────────────────────────────┤
│  Reference Points              (N × 96 bytes + 16 pad)      │
├─────────────────────────────────────────────────────────────┤
│  Collision Capsule Entries     (N × variable + 16 pad)      │
├─────────────────────────────────────────────────────────────┤
│  Extra Data Blocks 1-3         (optional, large files only) │
└─────────────────────────────────────────────────────────────┘
```

The ordering of data chunks within the file is not strictly fixed, but the header provides absolute offsets to each section. All offsets stored in the header require adding 16 to reach the actual data (see [Data Access Convention](#4-data-access-convention)).

---

## 2. Primitive Types

```cpp
using u8  = uint8_t;     // 1 byte unsigned
using u16 = uint16_t;    // 2 bytes unsigned
using u32 = uint32_t;    // 4 bytes unsigned
using i32 = int32_t;     // 4 bytes signed
using f32 = float;       // 4 bytes IEEE 754

struct Vector3f  { f32 x, y, z; };           // 12 bytes
struct Vector4f  { f32 x, y, z, w; };        // 16 bytes (quaternion or vec4)
struct PackedNormal { u8 x, y, z, pad; };    //  4 bytes (see §9 for decoding)
struct PackedUV  { u16 u, v; };              //  4 bytes (see §9 for decoding)
```

---

## 3. File Header

**Size**: 552 bytes (0x228) | **Always at file offset 0**

The file header occupies the first 552 bytes of the file. The first 536 bytes contain structured fields; the final 16 bytes are zero-padding before the first data chunk. All offset fields require adding +16 to reach actual data (see [§4](#4-data-access-convention)).

```cpp
struct AppHeader {                              // 552 bytes total (536 data + 16 pad)
    // ─── Identification (0x000–0x01F) ──────────────────────────────────────────
    u32     magic;                  // 0x000: 0xDEADBEEF – file signature
    u16     version;                // 0x004: Format version (260 for Reaper of Souls)
    u16     _pad06;                 // 0x006: Zero
    u8      _reserved008[8];        // 0x008: Always zero
    u32     snoId;                  // 0x010: SNO hash – unique file ID in D3 asset database
    u8      _reserved014[12];       // 0x014: Always zero

    // ─── Object Configuration (0x020–0x02F) ────────────────────────────────────
    u32     objectType;             // 0x020: Object type (1=static, 2=animated, 6=player, 18=scene)
    u32     boneCount;              // 0x024: Number of bones (0 for static models)
    u32     boneOffset;             // 0x028: Offset to bone data (+16 to reach entries)
    u32     boneDataSize;           // 0x02C: Total bone data size (boneCount × 312)

    // ─── Character-Specific Data (0x030–0x0A7) ─────────────────────────────────
    //      120-byte region, all zeros for static props.
    //      For character models, may contain look-at constraint data.
    u8      _unknown030[8];         // 0x030: Unknown
    u32     _flags038;              // 0x038: Unknown flag (1 for some characters)
    char    lookAtBoneName[64];     // 0x03C: Null-terminated bone name (e.g. "Neck")
    f32     lookAtMaxYaw;           // 0x07C: Look-at yaw limit (radians)
    f32     lookAtMinYaw;           // 0x080: Look-at min yaw (radians)
    u8      _unknown084[4];         // 0x084: Unknown
    f32     lookAtMinPitch;         // 0x088: Look-at pitch limit (radians)
    f32     lookAtMaxPitch;         // 0x08C: Look-at max pitch (radians)
    f32     lookAtRange;            // 0x090: Look-at angular range (degrees; observed: 90.0)
    u8      _reserved094[20];       // 0x094: Zero padding

    // ─── Submesh Reference (0x0A8–0x0B3) ───────────────────────────────────────
    u32     submeshCount;           // 0x0A8: Number of submesh/geoset entries
    u32     submeshOffset;          // 0x0AC: Offset to submesh entries (+16)
    u32     submeshDataSize;        // 0x0B0: Total submesh data (submeshCount × 400)

    // ─── Collision Data (0x0B4–0x0F3) ──────────────────────────────────────────
    u8      _reserved0B4[36];       // 0x0B4: Zero padding
    f32     collSphereX;            // 0x0D8: Bounding sphere center X
    f32     collSphereY;            // 0x0DC: Bounding sphere center Y
    f32     collSphereZ;            // 0x0E0: Bounding sphere center Z
    f32     collSphereRadius;       // 0x0E4: Bounding sphere radius
    u32     collCapsuleCount;       // 0x0E8: Number of collision capsules
    u32     collCapsuleOffset;      // 0x0EC: Offset to capsule data (+16)
    u32     collCapsuleDataSize;    // 0x0F0: Total capsule data size

    // ─── Reference Points / Hardpoints (0x0F4–0x10B) ───────────────────────────
    u8      _reserved0F4[12];       // 0x0F4: Zero padding
    u32     refPointCount;          // 0x100: Number of reference points
    u32     refPointOffset;         // 0x104: Offset to reference point data (+16)
    u32     refPointDataSize;       // 0x108: Total ref point data (refPointCount × 96)

    // ─── Scale & Extended Data (0x10C–0x167) ────────────────────────────────────
    u8      _reserved10C[20];       // 0x10C: Zero padding
    f32     modelScale;             // 0x120: Global model scale (always 1.0)
    u8      _reserved124[4];        // 0x124: Zero padding

    // ─── Octree Partitioning Parameters (0x128–0x13F) ───────────────────────────
    //      Non-zero in exactly 3,137 files — the same files that have extra data
    //      blocks. These six fields define the octree used for visibility culling
    //      and scene rendering in complex dungeon/scene models.
    u32     octreeNodeCount;        // 0x128: Number of octree nodes (Block 1 entry count)
    u32     octreeNodeCount2;       // 0x12C: Usually == octreeNodeCount (validation copy)
    u32     octreeDataSize;         // 0x130: Octree auxiliary data size
                                    //        (often == octreeNodeCount × 8)
    u32     octreeCellCount;        // 0x134: Number of octree leaf cells (Block 2 entries)
    u32     octreeCellDataSize;     // 0x138: Octree cell data size
    u32     octreeIndexSize;        // 0x13C: Octree index mapping data size

    // ─── Extra Data Blocks / Octree Data (0x140–0x167) ──────────────────────────
    u32     extraData1Offset;       // 0x140: Offset to extra data block 1 (+16)
    u32     extraData1Size;         // 0x144: Size of extra data block 1
    u8      _pad148[8];             // 0x148: Zero padding
    u32     extraData2Offset;       // 0x150: Offset to extra data block 2 (+16)
    u32     extraData2Size;         // 0x154: Size of extra data block 2
    u8      _pad158[8];             // 0x158: Zero padding
    u32     extraData3Offset;       // 0x160: Offset to extra data block 3 (+16)
    u32     extraData3Size;         // 0x164: Size of extra data block 3

    // ─── Bounding Box (0x168–0x1AF) ────────────────────────────────────────────
    u8      _pad168[8];             // 0x168: Zero padding
    f32     aabbCenterX;            // 0x170: AABB center X (often == collSphereX)
    f32     aabbCenterY;            // 0x174: AABB center Y
    f32     aabbCenterZ;            // 0x178: AABB center Z
    f32     aabbExtentX;            // 0x17C: AABB half-extent X
    f32     aabbExtentY;            // 0x180: AABB half-extent Y
    f32     aabbExtentZ;            // 0x184: AABB half-extent Z
    u8      _reserved188[24];       // 0x188: Zero padding
    u32     _unknownCount1A0;       // 0x1A0: Unknown count (1 or 3 for characters)
    u8      _reserved1A4[12];       // 0x1A4: Zero padding

    // ─── Material & Look Table References (0x1B0–0x1CF) ────────────────────────
    u32     lookCount;              // 0x1B0: Number of look entries
    u32     materialCount;          // 0x1B4: Number of material entries
    u32     materialOffset;         // 0x1B8: Offset to material table (+16). Always 536
    u32     materialDataSize;       // 0x1BC: Total material data (materialCount × 144)
    u8      _pad1C0[8];             // 0x1C0: Zero padding
    u32     lookTableOffset;        // 0x1C8: Offset to look name table (+16)
    u32     lookTableSize;          // 0x1CC: Total look table size (lookCount × 64)

    // ─── Trailing Constants (0x1D0–0x217) ──────────────────────────────────────
    u8      _pad1D0[52];            // 0x1D0: Zero padding
    u32     _const204;              // 0x204: Always 480 (0x1E0)
    u32     _const208;              // 0x208: Always 30 (0x1E)
    u32     _const20C;              // 0x20C: Always 30
    u32     _const210;              // 0x210: Always 30
    u32     _const214;              // 0x214: Always 30

    // ─── Alignment Padding (0x218–0x227) ───────────────────────────────────────
    u8      _headerPad[16];         // 0x218: Zero padding before first data chunk
};
```

> **Note**: The character-specific region (0x030–0x0A7) is almost entirely unused. Analysis of 200 character files shows only **1 file** with any non-zero values in the look-at fields (0x03C–0x094). The single populated instance contains the bone name `"Root_Joint"` at 0x03C. The look-at constraint system appears to be a vestigial feature that was implemented in the format but almost never used in shipped content.

> **Note**: The octree parameters (0x128–0x13C) are non-zero in exactly 3,137 files — the same set of files that have octree data blocks at 0x140/0x150/0x160. See §14 for details.

> **Note**: The relationship between the trailing constants 480 and 30 is unclear. 480 ÷ 16 = 30, suggesting a possible array of 30 sixteen-byte blocks, but their purpose is unknown.

**Object Type Values**:
| Value | Description |
|-------|-------------|
| 0 | Minimal/effect models |
| 1 | Static props, dungeon tiles |
| 2 | Animated models (NPCs, monsters) |
| 6 | Player characters with full skeleton |
| 18 | Complex scenes, multi-mesh dungeon segments |

---

## 4. Data Access Convention

All offsets stored in the header point to 16-byte alignment padding that precedes each data chunk. To reach the actual data entries, add 16 to the stored offset:

```
actual_data_position = stored_offset + 16
```

The 16-byte padding region is always filled with zeros (it is **not** a chunk header with metadata, unlike M3 format index entries).

**Example**: If `submeshOffset = 1160`, the submesh entries begin at file position `1160 + 16 = 1176`.

---

## 5. Material Entries

**Size**: 144 bytes per entry

Materials define the visual properties associated with each submesh. The material table is always the first data chunk in the file, located at offset 536 + 16 = 552 (0x228).

```cpp
struct MaterialEntry {                          // 144 bytes
    char    materialName[128];  // 0x00: Null-terminated ASCII name (e.g. "Skeleton_mat", "Painting")
    u32     shaderDataOffset;   // 0x80: Offset to per-material shader/texture data block
    u32     shaderDataSize;     // 0x84: Size of the material's shader data block
    u32     _reserved88;        // 0x88: Always 0
    u32     _reserved8C;        // 0x8C: Always 0
};
```

The `shaderDataOffset` points to an internal block containing material parameters (shader constants, texture SNO references, render state flags, etc.). This block uses the same data format as standalone `.mat` material files (see `MAT_FILE_FORMAT_SPECIFICATION.md`).

### 5.1 Material Shader Data Block

Each material's shader data block (pointed to by `shaderDataOffset`, with size `shaderDataSize`) contains the full material definition — identical in structure to the data section of standalone `.mat` files. The block layout:

```cpp
struct AppMaterialDataBlock {                   // Variable size (typically ~248 bytes)
    // ─── APP-Specific Header (0x00–0x17) ───────────────────────────────────────
    u32     renderGroup;        // 0x00: Render group / object type identifier
    i32     _flags04;           // 0x04: Usually -1 (0xFFFFFFFF)
    u32     paramSectionOffset; // 0x08: Absolute file offset to shader param overrides (+16)
    u32     paramSectionSize;   // 0x0C: Size of param data = 4 + N × 12 (N = param count)
    u8      _reserved10[8];     // 0x10: Always zero

    // ─── Material Colors (0x18–0x63) — same layout as .mat header ──────────────
    u32     shaderMapSno;       // 0x18: SNO reference to ShaderMap (.shm) asset
    f32     diffuseR;           // 0x1C: Diffuse color red   (typically 1.0)
    f32     diffuseG;           // 0x20: Diffuse color green
    f32     diffuseB;           // 0x24: Diffuse color blue
    f32     diffuseA;           // 0x28: Diffuse color alpha
    f32     specularR;          // 0x2C: Specular color red
    f32     specularG;          // 0x30: Specular color green
    f32     specularB;          // 0x34: Specular color blue
    f32     specularA;          // 0x38: Specular color alpha
    f32     emissiveRGBA[4];    // 0x3C: Emissive color (R, G, B, A)
    f32     ambientRGBA[4];     // 0x4C: Ambient color (R, G, B, A)
    f32     specularPower;      // 0x5C: Specular shininess exponent
    i32     materialFlags;      // 0x60: Material flags / render priority

    // ─── Texture Data Reference (0x64–0x77) ────────────────────────────────────
    u32     texDataOffset;      // 0x64: Absolute file offset to texture entries (+16)
    u32     texDataSize;        // 0x68: Total texture data = M × 160 bytes
    u8      _reserved6C[12];    // 0x6C: Zero padding
};
```

**ShaderMap reference**: The `shaderMapSno` field references a `.shm` ShaderMap asset (see `SHM_FILE_FORMAT_SPECIFICATION.md`), which maps rendering configurations to compiled shader variants.

### 5.2 Shader Parameter Overrides

At `paramSectionOffset + 16` (absolute file position):

```cpp
struct ShaderParamSection {
    u32     count;              // Number of parameter override entries
    struct ParamEntry {         // 12 bytes per entry
        u32 paramType;          // 0 = integer, 1 = float, 2 = SNO reference
        u32 paramId;            // Shader parameter ID (0x0003XXYY)
        u32 value;              // Type-dependent value (int, IEEE 754, or SNO hash)
    } entries[count];
};
```

Many APP materials have 0 parameter overrides (`paramSectionSize = 4`, count = 0).

### 5.3 Texture Entries

At `texDataOffset + 16` (absolute file position), an array of 160-byte texture entries:

```cpp
struct TextureEntry {                           // 160 bytes
    u32     slotIndex;          // 0x00: Shader sampler slot number (1=diffuse, 3=normal, etc.)
    u32     _reserved04;        // 0x04: Always 0
    u32     textureSno;         // 0x08: SNO reference to .tex texture asset
    u32     textureFlags;       // 0x0C: 0=default, 1=has UV transform, 2=has UV scroll
    f32     uvMatrix[16];       // 0x10: 4×4 UV coordinate transform matrix (64 bytes)
                                //       [0]=scaleU, [5]=scaleV for simple tiling
    f32     uvAnimParams[16];   // 0x50: UV animation parameters (scroll speeds, etc.)
    u32     _reserved90[2];     // 0x90: Rarely non-zero
    u32     textureType;        // 0x98: Texture format/sampler type (0=default, 3=DXT5, etc.)
    u32     _reserved9C;        // 0x9C: Always 0
};
```

The number of texture entries is `texDataSize / 160`. Common texture slot assignments:

| Slot | Purpose |
|------|---------|
| 1 | Diffuse map (primary color texture) |
| 2 | Second diffuse / blend layer |
| 3 | Normal map |
| 4 | Specular / gloss map |
| 5 | Emissive / glow map |
| 6 | Environment / reflection map |

> **Full format reference**: See `MAT_FILE_FORMAT_SPECIFICATION.md` for complete documentation of all texture slots, shader parameter IDs, UV animation fields, and corpus statistics.

Each submesh references a material by name (see [§8](#8-submeshes-geosets)).

---

## 6. Look Table

Looks represent different visual configurations for the model (e.g. alternate costumes, damage states, LOD variants). The Diablo III appearance system uses looks to swap between visual states at runtime.

Each look entry is a **64-byte** null-terminated ASCII string:

```cpp
struct LookEntry {
    char name[64];  // e.g. "Look 1", "A", "B", "C"
};
```

Simple static models typically have 1 look (`"Look 1"`). Character models may have dozens of looks corresponding to different gear/appearance slots identified by single letters (`"A"`, `"B"`, `"C"`, ...) or descriptive names.

---

## 7. Bones

**Size**: 312 bytes per entry (v260)

Bone data is only present in animated models (`boneCount > 0`).

### 7.1 PRS Transform (32 bytes)

Each PRS transform encodes a bone's pose as a quaternion rotation, translation, and uniform scale:

```cpp
struct PRSTransform {                           // 32 bytes
    Vector4f    rotation;       // 0x00: Quaternion (XYZW). See §16 for coordinate conversion
    Vector3f    position;       // 0x10: Translation. See §16 for coordinate conversion
    f32         scale;          // 0x1C: Uniform scale factor
};
```

### 7.2 Bone Entry Layout

```cpp
struct BoneEntry {                              // 312 bytes
    char            name[64];           // 0x000: Null-terminated name (e.g. "Pelvis", "Spine1", "R_Hand")
    u32             parentId;           // 0x040: Index of parent bone. 0xFFFFFFFF = root (no parent)
    Vector3f        aabbMin;            // 0x044: AABB minimum corner
    Vector3f        aabbMax;            // 0x050: AABB maximum corner
    f32             sphereRadius;       // 0x05C: Bounding sphere radius
    Vector3f        sphereCenter;       // 0x060: Bounding sphere center
    PRSTransform    transforms[3];      // 0x06C: Three PRS transforms (3 × 32 = 96 bytes)

    // ─── Extended Bone Data (0x0CC–0x137) ──────────────────────────────────────
    //      108 bytes of additional bone data following the three core transforms.
    //      Analysis of 2,777 bones across character models reveals structured data.
    PRSTransform    inverseBind;        // 0x0CC: Inverse bind pose transform (32 bytes)
                                        //        scale = 1.0 in 2,591/2,777 bones
    PRSTransform    restPose;           // 0x0EC: Additional rest/reference pose (32 bytes)
                                        //        scale = 1.0 in 2,591/2,777 bones
    f32             boneParams[5];      // 0x10C: Bone constraint parameters (20 bytes)
                                        //        Tiny floats near 0.0 in 2,234/2,777 bones
                                        //        Possibly IK stiffness, damping, or limit angles
    u32             _sparse120[3];      // 0x120: Sparse data (12 bytes) — non-zero in only
                                        //        734/2,777 bones. Purpose unclear
    u32             mirrorBoneRef;      // 0x12C: Bone reference — 0xFFFFFFFF in 2,772/2,777 bones
                                        //        5 bones have actual small int values (mirror/IK ref)
    u8              _reserved130[8];    // 0x130: Trailing padding (always zero)
};
```

The three transforms stored per bone are:
- **transforms[0]**: Rest/bind pose (used for mesh construction)
- **transforms[1]**: Inverse bind pose
- **transforms[2]**: Duplicate/reference pose

The two additional PRS transforms in the extended bone data section are:
- **inverseBind** (0x0CC): Secondary inverse bind matrix — used by the engine for bone-space to model-space conversion. Scale is 1.0 for 93.3% of bones.
- **restPose** (0x0EC): Additional reference pose — possibly used for animation blending or physics reset. Same scale distribution as `inverseBind`.

### 7.3 Bone Hierarchy

Bones are stored in a flat array. The parent–child relationship is defined by `parentId`:

```cpp
// Build bone hierarchy
for (u32 i = 0; i < header.boneCount; i++) {
    BoneEntry& bone = bones[i];
    if (bone.parentId != 0xFFFFFFFF) {
        BoneEntry& parent = bones[bone.parentId];
        setParent(bone.node, parent.node);
    }
}
```

Common bone chains observed:
```
Pelvis → Spine1 → Spine2 → Spine3 → Neck → Head
                                    ├→ L_Clavicle → L_UpperArm → L_ForeArm → L_Hand
                                    └→ R_Clavicle → R_UpperArm → R_ForeArm → R_Hand
       ├→ L_Thigh → L_Calf → L_Foot → L_Toe
       └→ R_Thigh → R_Calf → R_Foot → R_Toe
```

---

## 8. Submeshes (Geosets)

**Size**: 400 bytes per entry (v260)

Each submesh represents a renderable mesh group with its own vertex/index buffers, material reference, and bounding volume.

```cpp
struct SubmeshEntry {                           // 400 bytes
    u32     renderFlags;            // 0x000: Rendering flags bitfield (see §8.2)
    u32     vertexCount;            // 0x004: Number of vertices
    u32     vertexOffset;           // 0x008: Offset to vertex data (+16)
    u32     vertexDataSize;         // 0x00C: Total vertex data size (vertexCount × 44)
    u32     _reserved010;           // 0x010: Always 0 (possibly secondary vertex buffer)
    u32     _reserved014;           // 0x014: Always 0
    u32     weightOffset;           // 0x018: Offset to vertex weight data (+16). 0 if no weights
    u32     weightDataSize;         // 0x01C: Total weight data (vertexCount × 24)
    u32     _reserved020;           // 0x020: Always 0 (possibly secondary weight buffer)
    u32     _reserved024;           // 0x024: Always 0
    u32     indexCount;             // 0x028: Number of face indices (÷3 for triangle count)
    u32     indexOffset;            // 0x02C: Offset to index data (+16)
    u32     indexDataSize;          // 0x030: Total index data (indexCount × 2)
    u8      _reserved034[12];       // 0x034: Always 0

    // ─── Cloth Simulation Data ─────────────────────────────────────────────────
    u32     clothDataOffset;        // 0x040: Offset to inline cloth parameter block (0 if none)
    u32     clothDataSize;          // 0x044: Size of cloth data — ALWAYS 128 bytes when present.
                                    //        98 submeshes across ~50 files have cloth data
                                    //        (sarcophagi, banners, vehicle chairs, cloaks)
    u8      _reserved048[8];        // 0x048: Always 0

    // ─── Render Mode / Material Binding ────────────────────────────────────────
    u32     renderModeFlags;        // 0x050: Render mode flags. Common values:
                                    //        0x904D (36,941 submeshes), 0xFFFFFFFF (12,581),
                                    //        0x9047, 0x9049, 0x0001. Zero = unused/disabled
    u32     materialSlotRef;        // 0x054: Material slot reference. 0xFFFFFFFF = none (71,938/78,586).
                                    //        When set, indexes into a material or shader parameter slot
    u8      _reserved058[4];        // 0x058: Always 0
    char    meshName[128];          // 0x05C: Null-terminated mesh name (e.g. "A_skeleton_mat")
    char    fullMeshPath[128];      // 0x0DC: Null-terminated Maya mesh path
                                    //        (e.g. "BarbM_Skeleton_geoShape_A_skeleton_mat_001")

    // ─── Per-Submesh Bounding & LOD ────────────────────────────────────────────
    f32     boundCenter[3];         // 0x15C: Per-mesh AABB center (x, y, z)
    f32     boundExtent[3];         // 0x168: Per-mesh AABB half-extents (x, y, z)
    f32     _reserved174[7];        // 0x174: Nearly always zero — only 1/78,586 submeshes
                                    //        has any non-zero bytes here. Possibly reserved
                                    //        for runtime LOD distance overrides
};
```

### 8.1 Cloth Simulation Data

When `clothDataOffset > 0`, the submesh has an inline 128-byte **cloth mesh descriptor** that references five data arrays stored elsewhere in the APP file. This is NOT a parameter block — it follows the same count/offset/size reference pattern used by the APP header. The cloth data describes a separate simulation mesh overlaid on the render mesh.

Present in **936 submeshes** across **631 files** (banners, cloaks, ropes, sarcophagus lids, angelic wings, vehicle covers). `clothDataSize` is always exactly **128 bytes**.

```cpp
struct ClothDescriptor {                        // 128 bytes
    // ─── Array 1: Cloth Vertices (84 bytes each) ──────────────────────────────
    u32     vertexCount;            // 0x00: Number of cloth simulation vertices
    u32     vertexOffset;           // 0x04: Offset to vertex data (+16)
    u32     vertexDataSize;         // 0x08: = vertexCount × 84
    u32     _pad0C[3];              // 0x0C: Always 0

    // ─── Array 2: Cloth Triangles (28 bytes each) ─────────────────────────────
    u32     triangleCount;          // 0x18: Number of cloth simulation triangles
    u32     triangleOffset;         // 0x1C: Offset to triangle data (+16)
    u32     triangleDataSize;       // 0x20: = triangleCount × 28
    u32     _pad24[3];              // 0x24: Always 0

    // ─── Array 3: Anchor Constraints (28 bytes each) ──────────────────────────
    u32     anchorCount;            // 0x30: Number of anchor/pin constraints
    u32     anchorOffset;           // 0x34: Offset to anchor data (+16)
    u32     anchorDataSize;         // 0x38: = anchorCount × 28
    u32     _pad3C[3];              // 0x3C: Always 0

    // ─── Array 4: Stretch Constraints (24 bytes each) ─────────────────────────
    u32     stretchCount;           // 0x48: Number of stretch/distance constraints
    u32     stretchOffset;          // 0x4C: Offset to stretch data (+16)
    u32     stretchDataSize;        // 0x50: = stretchCount × 24
    u32     _pad54[3];              // 0x54: Always 0

    // ─── Array 5: Bend Constraints (24 bytes each) ────────────────────────────
    u32     bendCount;              // 0x60: Number of bend/shear constraints
    u32     bendOffset;             // 0x64: Offset to bend data (+16)
    u32     bendDataSize;           // 0x68: = bendCount × 24
    u32     _pad6C[3];              // 0x6C: Always 0

    // ─── Simulation Parameters ────────────────────────────────────────────────
    u32     simIterations;          // 0x78: Constraint solver iteration count
                                    //       Range: 1–117 (most common: 1, 2, 4)
    f32     maxExtent;              // 0x7C: Maximum cloth extent / simulation radius
                                    //       Range: 0.06 – 2798.0
};
```

Arrays 4 and 5 (stretch/bend constraints) have count = 0 in **12 of 936** cloth blocks — these are simple cloth meshes that rely only on triangle-based constraints.

#### 8.1.1 Cloth Vertex (84 bytes)

Each cloth vertex stores its current and rest positions, inverse mass, a pin reference, and neighbor connectivity:

```cpp
struct ClothVertex {                            // 84 bytes (21 × f32)
    Vector3f    position;           // 0x00: Current simulation position (XYZ)
    f32         invMass;            // 0x0C: Inverse mass (1.0 = standard mass)
    f32         _pad10[2];          // 0x10: Always 0
    Vector3f    restPosition;       // 0x18: Rest/initial position (== position at start)
    f32         _pad24[3];          // 0x24: Always 0
    f32         _pad30;             // 0x30: Always 0
    u32         pinBoneRef;         // 0x34: 0xFFFFFFFF = free vertex (not pinned).
                                    //       Otherwise: bone or constraint reference
    f32         _pad38;             // 0x38: Always 0
    u32         neighborIdx0;       // 0x3C: First neighbor vertex index (0 for first vertex)
    u32         neighborIdx1;       // 0x40: Second neighbor vertex index
    f32         _pad44[5];          // 0x44: Always 0
};
```

The `position` and `restPosition` fields are identical at load time — the engine updates `position` during simulation while `restPosition` stays fixed for constraint resolution.

#### 8.1.2 Cloth Triangle (28 bytes)

Defines the triangulated cloth mesh topology. Each entry references three cloth vertices and stores the triangle's rest-state area for strain computation:

```cpp
struct ClothTriangle {                          // 28 bytes
    u32         _reserved[3];       // 0x00: Always 0 (computed at runtime: inverse rest matrix)
    f32         restArea;           // 0x0C: Rest-state triangle area (used for
                                    //       area-preserving strain constraints)
    u32         v0;                 // 0x10: Vertex index 0
    u32         v1;                 // 0x14: Vertex index 1
    u32         v2;                 // 0x18: Vertex index 2
};
```

#### 8.1.3 Cloth Anchor (28 bytes)

Anchor constraints pin cloth vertices to positions on the skeleton. Each anchor attaches one cloth vertex to an interpolated point between two bone-attached reference vertices using barycentric weights:

```cpp
struct ClothAnchor {                            // 28 bytes
    u32         vertexIndex;        // 0x00: Cloth vertex being pinned
    u32         anchorVert0;        // 0x04: First bone-attached anchor vertex
    u32         anchorVert1;        // 0x08: Second bone-attached anchor vertex
    u32         _reserved;          // 0x0C: Always 0
    f32         weight0;            // 0x10: Barycentric weight for anchorVert0
    f32         weight1;            // 0x14: Barycentric weight for anchorVert1
                                    //       weight0 + weight1 ≈ 1.0
    u32         _pad;               // 0x18: Always 0
};
```

For fully pinned vertices (e.g. rope endpoints), `weight0 = 1.0` and `weight1 = 0.0`.

#### 8.1.4 Cloth Stretch Constraint (24 bytes)

Distance constraints maintain the rest length between connected vertex pairs (structural springs):

```cpp
struct ClothStretchLink {                       // 24 bytes
    u32         vertIdx0;           // 0x00: First vertex index
    u32         vertIdx1;           // 0x04: Second vertex index
    f32         restLength;         // 0x08: Rest distance between vertices
    u32         _reserved[2];       // 0x0C: Always 0
    f32         stiffness;          // 0x14: Constraint stiffness (0.0 or 1.0)
};
```

#### 8.1.5 Cloth Bend Constraint (24 bytes)

Bend constraints resist folding by maintaining distance between non-adjacent vertices (diagonal springs across quad faces):

```cpp
struct ClothBendLink {                          // 24 bytes
    u32         vertIdx0;           // 0x00: First vertex index (across-quad diagonal)
    u32         vertIdx1;           // 0x04: Second vertex index
    f32         restLength;         // 0x08: Rest diagonal distance (typically larger
                                    //       than stretch rest lengths)
    u32         _reserved[3];       // 0x0C: Always 0
};
```

### 8.2 Render Flags Bitfield

The `renderFlags` field is a bitmask that describes the rendering properties of the submesh:

| Bit(s) | Mask   | Description |
|--------|--------|-------------|
| 0      | 0x0001 | Has geometry (always set) |
| 1      | 0x0002 | Has normals |
| 2      | 0x0004 | Has UV coordinates |
| 3      | 0x0008 | Has tangent data |
| 4      | 0x0010 | Has binormal / secondary UV |
| 5      | 0x0020 | Unknown (rarely set) |
| 6      | 0x0040 | Has vertex color / alpha |
| 7      | 0x0080 | Reserved (not observed) |
| 8      | 0x0100 | Has bone weights (skinned mesh) |
| 9      | 0x0200 | Reserved |
| 10     | 0x0400 | Has cloth simulation |
| 11     | 0x0800 | Has transparency / alpha blending |
| 12     | 0x1000 | Has texture (standard rendering) |

**Common flag combinations**:

| Value    | Hex      | Description |
|----------|----------|-------------|
| 4111     | 0x100F   | Standard textured mesh with normals/tangents |
| 6159     | 0x180F   | Transparent textured mesh |
| 5191     | 0x1447   | Textured mesh with vertex color and cloth |
| 4105     | 0x1009   | Textured mesh (minimal: position + tangent + texture) |
| 71       | 0x0047   | Untextured mesh with vertex color |
| 103      | 0x0067   | Untextured mesh with vertex color and secondary UV |

---

## 9. Vertices

**Stride**: 44 bytes (fixed, regardless of render flags — unused channels are zeroed)

```cpp
struct Vertex {                                 // 44 bytes
    Vector3f    position;       // 0x00: Vertex position (XYZ)
    u8          normal[3];      // 0x0C: Packed normal vector (see §9.2 for decoding)
    u8          _pad0F;         // 0x0F: Zero padding
    u8          boneIndices[4]; // 0x10: Per-vertex bone indices for GPU skinning.
                                //       0xFF for unused slots. All zeros for static meshes
    u8          _unknown14[4];  // 0x14: Always zero (possibly reserved for extra bone weights)
    PackedUV    texCoord;       // 0x18: Packed texture coordinates (see §9.3)
    u8          _pad1C[4];      // 0x1C: Zero padding
    u8          tangent[3];     // 0x20: Packed tangent vector (same encoding as normal)
    u8          _pad23;         // 0x23: Zero padding
    u8          binormal[3];    // 0x24: Packed binormal/bitangent vector (same encoding)
    u8          _pad27;         // 0x27: Zero padding
    u8          _pad28[4];      // 0x28: Zero padding
};
```

### 9.2 Normal / Tangent / Binormal Decoding

Packed normals, tangents, and binormals use the same encoding. Each component is stored as an unsigned byte and decoded as:

```
component = (byte - 127.0) / 127.0
```

This maps the byte range [0, 254] to approximately [-1.0, +1.0]:
- Byte **127** → 0.0
- Byte **0** → -1.0
- Byte **254** → +1.0
- Byte **255** → +1.008 (slight overshoot, treat as +1.0)

### 9.3 UV Coordinate Decoding

Texture coordinates are stored as two unsigned 16-bit integers and decoded as:

```
u = -(32767.0 - raw_u) / 512.0
v = (32767.0 - raw_v) / 512.0 + 1.0
```

The U axis is negated and V axis is offset by 1.0 to match the Maya/3ds Max UV space convention. This encoding provides a UV range of approximately [−64, +64] with ~0.002 precision.

---

## 10. Vertex Weights

**Size**: 24 bytes per entry

Vertex weights are stored in a separate buffer (parallel to the vertex buffer) and are only present for skinned meshes (`weightOffset > 0`). Each vertex has up to **3 bone influences**. Unused slots have `boneId = 0` and `weight = 0.0`. Weights should sum to approximately 1.0.

```cpp
struct VertexWeight {                           // 24 bytes
    u32     boneId1;        // 0x00: Index of first influence bone
    f32     weight1;        // 0x04: Weight for first bone (0.0–1.0)
    u32     boneId2;        // 0x08: Index of second influence bone
    f32     weight2;        // 0x0C: Weight for second bone
    u32     boneId3;        // 0x10: Index of third influence bone
    f32     weight3;        // 0x14: Weight for third bone
};
```

Applying weights to a skinned vertex:

```cpp
// Apply vertex weights for GPU skinning
for (u32 v = 0; v < submesh.vertexCount; v++) {
    VertexWeight& w = weights[v];
    skinOps.addWeight(v, w.boneId1, w.weight1);
    skinOps.addWeight(v, w.boneId2, w.weight2);
    skinOps.addWeight(v, w.boneId3, w.weight3);
}
```

---

## 11. Indices (Faces)

Face indices are stored as a flat array of unsigned 16-bit integers, grouped into triangles.

```cpp
struct Triangle {
    u16 index0;
    u16 index1;
    u16 index2;
};
```

Each triangle is **6 bytes**. The `indexCount` field in the submesh entry gives the total number of individual indices (not triangles). Triangle count = `indexCount / 3`.

Indices are **zero-based** (the first vertex is index 0). When importing into 1-based systems (e.g. 3ds Max), add 1 to each index.

---

## 12. Reference Points (Hardpoints)

**Size**: 96 bytes per entry

Reference points (also called hardpoints or attachment points) define named attachment locations on the model skeleton. Every model has at least one reference point named `"Default"`.

```cpp
struct ReferencePoint {                         // 96 bytes
    char        name[64];       // 0x00: Null-terminated name (e.g. "Default", "Weapon_L", "Shield")
    u32         parentId;       // 0x40: Index of parent bone (0-based). 0xFFFFFFFF if no parent
    Vector4f    rotation;       // 0x44: Quaternion rotation (XYZW). Y component is negated on import
    Vector3f    position;       // 0x54: Translation. See §16 for coordinate conversion
};
```

---

## 13. Collision Data

### 13.1 Bounding Sphere

The global bounding sphere is stored directly in the header at offsets 0x0D8–0x0E7 (see `AppHeader::collSphere*` fields):

```cpp
// Read from header
Vector3f sphereCenter = { header.collSphereX, header.collSphereY, header.collSphereZ };
f32      sphereRadius = header.collSphereRadius;
```

For static models with no spatial extent, the center is `(0, 0, 0)` and the radius encodes a small default value.

### 13.2 Collision Capsules

**Size**: 104 bytes per entry

The collision capsule array is referenced by `header.collCapsuleCount`, `header.collCapsuleOffset`, and `header.collCapsuleDataSize`. Present in **356 files** (primarily character and animated models used for hit detection and ragdoll physics).

```cpp
struct CollisionCapsule {                       // 104 bytes
    char        name[64];           // 0x00: Null-terminated capsule name
                                    //       (e.g. "Spine1_capsule", "L_Hand_capsule")
    u32         _reserved40;        // 0x40: Always 0
    u32         _reserved44;        // 0x44: Always 0
    u32         boneIndex;          // 0x48: Parent bone index for attachment.
                                    //       0xFFFFFFFF = no parent (world-space capsule)
                                    //       Typical values: small integers (0–60) matching
                                    //       bone hierarchy indices
    Vector4f    rotation;           // 0x4C: Capsule orientation as quaternion (XYZW)
                                    //       Common values include 0.7071 (45° rotations)
    Vector3f    capsuleParams;      // 0x5C: Capsule geometry: likely (halfLength, radius, offset)
                                    //       or (centerOffset, halfHeight, radius)
    u32         _reserved68;        // 0x68: Always 0
};
```

Each capsule is typically parented to a skeleton bone and oriented to enclose a body segment. The capsule name usually matches the parent bone name with a `"_capsule"` suffix.

### 13.3 Per-Submesh Bounding Volume

Each `SubmeshEntry` contains its own bounding volume at offset 0x15C:

```cpp
// SubmeshEntry::boundCenter and SubmeshEntry::boundExtent
struct SubmeshBounds {
    f32 centerX, centerY, centerZ;      // AABB center
    f32 extentX, extentY, extentZ;      // AABB half-extents
};
```

---

## 14. Octree System (Extra Data Blocks)

Large files (complex dungeon tiles and scenes) contain an **octree-based spatial partitioning system** stored across three data blocks and controlled by the six header fields at 0x128–0x13C. Present in **3,137 files** — always all three blocks together, never individually. Static props and character models have all offsets/sizes set to zero.

Octrees recursively subdivide 3D space into eight axis-aligned children per node. D3 uses this to accelerate visibility culling and rendering of complex scenes — the engine traverses the tree, tests each node's bounding sphere against the view frustum, and skips entire subtrees that are off-screen.

### 14.1 Overview

The octree is split across three data blocks:

| Block | Header Fields | Alignment | Entry Size | Role |
|-------|--------------|-----------|------------|------|
| Block 1 | 0x140/0x144 | 48-byte | 48 bytes | Octree nodes (bounding sphere + child/leaf refs) |
| Block 2 | 0x150/0x154 | 12/24-byte | Variable | Leaf cells with bounding spheres |
| Block 3 | 0x160/0x164 | 8-byte | 8 bytes | Index mapping (geometry-to-node references) |

### 14.2 Block 1 — Octree Nodes

Each 48-byte entry represents a node in the octree. The entry begins with a bounding sphere that encloses all geometry within that octant:

```cpp
struct OctreeNode {                             // 48 bytes
    f32     centerX;            // 0x00: Bounding sphere center X
    f32     centerY;            // 0x04: Bounding sphere center Y
    f32     centerZ;            // 0x08: Bounding sphere center Z
    f32     radius;             // 0x0C: Bounding sphere radius
    u8      nodeData[32];       // 0x10: Child node indices (up to 8 children),
                                //       leaf flags, and geometry range references
};
```

| Metric | Value |
|--------|-------|
| Minimum size | 48 bytes (1 node) |
| Maximum size | 1.63 MB (~33,900 nodes) |
| Always divisible by | 48 |

### 14.3 Block 2 — Leaf Cells

Leaf cell data containing the actual geometry references for each terminal octree node. Entry alignment varies between 12 and 24 bytes. Each leaf includes a bounding sphere for its spatial extent plus references to the submeshes/triangles it contains.

| Metric | Value |
|--------|-------|
| Minimum size | 192 bytes |
| Maximum size | 5.7 MB |

### 14.4 Block 3 — Index Mapping

Small integer pairs that map geometry (submeshes or triangle groups) to octree nodes. Each entry is 8 bytes:

```cpp
struct OctreeIndexEntry {                       // 8 bytes
    u32     geometryIndex;      // 0x00: Submesh or triangle group index
    u32     nodeIndex;          // 0x04: Target octree node in Block 1
};
```

| Metric | Value |
|--------|-------|
| Minimum size | 64 bytes (8 entries) |
| Maximum size | 10.6 MB (~1.3 million entries) |
| Always divisible by | 8 |

### 14.5 Header Parameters (0x128–0x13C)

The six octree parameters in the header correlate with the block sizes:

| Offset | Field | Relationship |
|--------|-------|-------------|
| 0x128 | `octreeNodeCount` | Number of Block 1 entries (octree nodes) |
| 0x12C | `octreeNodeCount2` | Usually equals `octreeNodeCount` (validation copy) |
| 0x130 | `octreeDataSize` | Often equals `octreeNodeCount × 8` — auxiliary tree data |
| 0x134 | `octreeCellCount` | Block 2 leaf cell count (small integers, top value = 1 in 100 files) |
| 0x138 | `octreeCellDataSize` | Block 2 data sizing parameter |
| 0x13C | `octreeIndexSize` | Block 3 data sizing parameter |

In the largest observed files (37+ MB), these three blocks account for the majority of file size:

| Block | Typical Size Range |
|-------|-------------------|
| Block 1 (Nodes) | 48 B – 1.63 MB |
| Block 2 (Cells) | 192 B – 5.7 MB |
| Block 3 (Indices) | 64 B – 10.6 MB |

---

## 15. Animation Pipeline

Diablo III animations use a three-tier system: **AnimTree** (.ant) state machines select clips through **AnimSet** (.ans) state-to-clip lookup tables, which reference individual **Animation** (.ani) clip files containing per-bone keyframe data.

```
AnimTree (.ant)  →  AnimSet (.ans)  →  Animation (.ani)  →  Model (.app)
  state machine      state→clip map      keyframe data       skeleton
```

> **Full format references**: See `ANI_FILE_FORMAT_SPECIFICATION.md`, `ANS_FILE_FORMAT_SPECIFICATION.md`, and `ANT_FILE_FORMAT_SPECIFICATION.md` for complete documentation of all animation formats including corpus statistics, field distributions, and verified offset maps.

### 15.1 ANI File Summary (.ani — Version 118, 15,258 files)

Each `.ani` file contains a 56-byte SNO preamble followed by one or more 408-byte sub-animation blocks, then tightly-packed data sections for bone names, track descriptors, and keyframes.

```cpp
// SNO preamble (file offsets)
u32     magic;              // 0x000: 0xDEADBEEF
u32     version;            // 0x004: 118
u32     snoId;              // 0x010: Unique identifier
u32     animFlags;          // 0x020: 0 or 2
u32     modelSnoRef;        // 0x024: Parent .app model SNO reference
u32     blockOffset;        // 0x028: Always 56
u32     blockSize;          // 0x02C: blockCount × 408
u32     blockCount;         // 0x030: Typically 1
```

Key sub-animation block fields (block-relative offsets):

| Block Offset | Type      | Field                 | Notes                          |
|-------------|-----------|------------------------|--------------------------------|
| +0x014      | char[64]  | animationName          | Null-padded ASCII              |
| +0x074      | u32       | fps                    | Always 100                     |
| +0x078      | f32       | speed                  | Playback multiplier (typ. 1.0) |
| +0x088      | u32       | boneCount              | Number of animated bones       |
| +0x08C      | u32       | boneNameOffset         | Bone name array (+16)          |
| +0x0A0      | u32       | frameCount             | Max frame index + 1            |
| +0x0A4      | u32       | translDescOffset       | Translation descriptors (+16)  |
| +0x0B8      | u32       | rotDescOffset          | Rotation descriptors (+16)     |
| +0x0C8      | u32       | scaleDescOffset        | Scale descriptors (+16)        |

Per-bone track descriptors are stored in **three separate arrays** (one per channel), each with `boneCount` entries of 24 bytes:

```cpp
struct BoneTrackDescriptor {                    // 24 bytes
    u32     keyframeCount;      // 0x00: Keyframes for this bone/channel
    u32     keyframeOffset;     // 0x04: Offset to keyframe data (+16)
    u32     keyframeSize;       // 0x08: count × keyframeElementSize
    u32     _pad[3];            // 0x0C: Zeros
};
```

### 15.2 Keyframe Structures

```cpp
struct TranslationKeyframe {                    // 16 bytes
    u32         frame;          // 0x00: Frame number (time = frame / 100.0)
    Vector3f    position;       // 0x04: Translation (Y negated, all axes × 17.0)
};

struct RotationKeyframe {                       // 12 bytes
    u32     frame;              // 0x00: Frame number
    i16     rotation[4];        // 0x04: Quaternion as 4× signed short
                                //       Decode: component / 32767.0. Y is negated
};

struct ScaleKeyframe {                          // 8 bytes
    u32     frame;              // 0x00: Frame number
    f32     scale;              // 0x04: Uniform scale
};
```

Reading a bone's animation track:

```cpp
// Read translation descriptors for all bones
fseek(f, translDescOffset + 16, SEEK_SET);
BoneTrackDescriptor translDescs[boneCount];
fread(translDescs, 24, boneCount, f);

// Read translation keyframes for bone i
BoneTrackDescriptor& desc = translDescs[i];
fseek(f, desc.keyframeOffset + 16, SEEK_SET);
for (u32 k = 0; k < desc.keyframeCount; k++) {
    TranslationKeyframe key;
    fread(&key, sizeof(key), 1, f);
    // Apply coordinate transform (§16):
    Vec3f pos = { key.position.x * 17.0f, -key.position.y * 17.0f, key.position.z * 17.0f };
    addTranslationKey(bone, key.frame, pos);
}

// Rotation and scale descriptors are at separate offsets:
// rotDescOffset + 16, scaleDescOffset + 16
```

### 15.3 AnimSet Summary (.ans — Version 24, 3,212 files)

AnimSet files map animation states to `.ani` clip references using 29 fixed slots (likely visual variants):

```cpp
// 0x020: 29 slot references, each 16 bytes: {u32 offset, u32 size, u32[2] pad}
// Slot data: {u32 count, count × AnimEntry}
struct AnimEntry {                              // 12 bytes
    u32     entryType;          // Always 2
    u32     tagId;              // Animation state tag (e.g., 0x011000 = idle)
    u32     animSnoRef;         // SNO ref to .ani, or 0xFFFFFFFF = empty
};
```

### 15.4 AnimTree Summary (.ant — Version 30, 1 file)

Only one AnimTree file exists (`"Axe Bad Data.ant"`, 148 bytes) — insufficient for format analysis. Animation state machine logic appears to be predominantly code-driven rather than data-driven.

### 15.5 Particle System Summary (.prt — Version 180, 21,593 files)

PRT files define **particle emitters** — the visual effects layer of the D3 pipeline. Each file encodes a single emitter with ~41 animated parameter channels (AnimRef blocks) controlling emission rate, speed, lifespan, size curves, color gradients, physics, and noise modulation. Key characteristics:

- **Fixed structure** (2,320 bytes) followed by variable-length keyframe data and optional look variants
- **48-byte AnimRef** blocks (vs. M3's 20/36-byte variants) with 12-byte scalar or 28-byte vec3 keyframe entries
- **4 AnimRef regions** separated by gap regions containing material references, timing params, and rendering flags
- **Color gradient** system using 160-byte color transform matrix stops (1–4 stops per emitter)
- **Material binding** via SNO hash in Block 12, referencing `.mat` texture/shader definitions
- Structurally analogous to the **M3 PAR_ v24** chunk (SC2/HotS particle emitters)

Pipeline: `Appearance (.app) → Particle (.prt) → Material (.mat)`

Full specification: `Models/PRT_FILE_FORMAT_SPECIFICATION.md`

---

## 16. Coordinate System & Import Conventions

APP files use a Maya-style coordinate system. When importing into other applications, the following transformations are applied:

### 16.1 Position Scaling

All positions (vertices, bones, reference points) are scaled by a factor of **17.0** along each axis, with the **Y axis negated**:

```cpp
// Convert raw APP position to import space
Vec3f convertPosition(Vec3f raw) {
    return {
         raw.x * 17.0f,        // X scaled
        -raw.y * 17.0f,        // Y negated and scaled
         raw.z * 17.0f         // Z scaled
    };
}
```

The read order in the Taylor Mouse MaxScript importer is `(Y, X, Z)` with Y negated, effectively performing the same transform.

### 16.2 Quaternion Rotation

Bone and reference point rotations are stored as quaternions `(X, Y, Z, W)`:

```cpp
// Convert raw APP quaternion to import space
Quat convertRotation(Vector4f raw) {
    return {
         raw.x,        // X unchanged
        -raw.y,        // Y negated
         raw.z,        // Z unchanged
         raw.w         // W unchanged
    };
}
```

### 16.3 Scale Factor

The 17× scale factor converts from the game's internal coordinate system (where character models are approximately 0.1–5.0 units tall in raw file coordinates) to a more typical modeling scale (characters ~2–85 units tall).

---

## 17. Reading an APP File

This section provides a complete C++ walkthrough for loading an APP model file, similar in spirit to the M3 specification's geometry reading section.

### 17.1 Validating the Header

```cpp
FILE* f = fopen(filepath, "rb");

AppHeader header;
fread(&header, sizeof(AppHeader), 1, f);

// Validate magic and version
assert(header.magic == 0xDEADBEEF);    // "Not an APP file"
assert(header.version == 260);          // "Unsupported version"
```

### 17.2 Reading Materials

```cpp
std::vector<MaterialEntry> materials(header.materialCount);
fseek(f, header.materialOffset + 16, SEEK_SET);
fread(materials.data(), sizeof(MaterialEntry), header.materialCount, f);

// Each material's name is a null-terminated string in materialName[128]
for (auto& mat : materials) {
    printf("Material: %s (shader data: %u bytes at 0x%X)\n",
           mat.materialName, mat.shaderDataSize, mat.shaderDataOffset);
}
```

### 17.3 Reading Bones

```cpp
std::vector<BoneEntry> bones(header.boneCount);
if (header.boneCount > 0) {
    fseek(f, header.boneOffset + 16, SEEK_SET);
    fread(bones.data(), sizeof(BoneEntry), header.boneCount, f);

    // Build skeleton hierarchy
    for (u32 i = 0; i < header.boneCount; i++) {
        BoneEntry& bone = bones[i];
        // Apply rest pose (transforms[0]) with coordinate conversion (§16)
        PRSTransform& rest = bone.transforms[0];
        Vec3f pos = {
             rest.position.x * 17.0f,
            -rest.position.y * 17.0f,
             rest.position.z * 17.0f
        };
        Quat rot = {
             rest.rotation.x,
            -rest.rotation.y,
             rest.rotation.z,
             rest.rotation.w
        };
        createBoneNode(bone.name, pos, rot, rest.scale);

        if (bone.parentId != 0xFFFFFFFF)
            setParent(bone.name, bones[bone.parentId].name);
    }
}
```

### 17.4 Reading Submeshes, Vertices, Weights, and Indices

```cpp
std::vector<SubmeshEntry> submeshes(header.submeshCount);
fseek(f, header.submeshOffset + 16, SEEK_SET);
fread(submeshes.data(), sizeof(SubmeshEntry), header.submeshCount, f);

for (u32 s = 0; s < header.submeshCount; s++) {
    SubmeshEntry& sub = submeshes[s];

    // ── Read vertices ──
    std::vector<Vertex> verts(sub.vertexCount);
    fseek(f, sub.vertexOffset + 16, SEEK_SET);
    fread(verts.data(), sizeof(Vertex), sub.vertexCount, f);

    for (auto& v : verts) {
        // Decode packed normal (§9.2)
        Vec3f normal = {
            (v.normal[0] - 127.0f) / 127.0f,
            (v.normal[1] - 127.0f) / 127.0f,
            (v.normal[2] - 127.0f) / 127.0f
        };

        // Decode packed UV (§9.3)
        f32 u = -(32767.0f - v.texCoord.u) / 512.0f;
        f32 t =  (32767.0f - v.texCoord.v) / 512.0f + 1.0f;

        // Apply coordinate transform (§16)
        Vec3f pos = {
             v.position.x * 17.0f,
            -v.position.y * 17.0f,
             v.position.z * 17.0f
        };

        addVertex(pos, normal, u, t);
    }

    // ── Read weights (if skinned) ──
    std::vector<VertexWeight> weights;
    if (sub.weightOffset > 0) {
        weights.resize(sub.vertexCount);
        fseek(f, sub.weightOffset + 16, SEEK_SET);
        fread(weights.data(), sizeof(VertexWeight), sub.vertexCount, f);

        for (u32 v = 0; v < sub.vertexCount; v++) {
            VertexWeight& w = weights[v];
            if (w.weight1 > 0.0f) addSkinWeight(v, w.boneId1, w.weight1);
            if (w.weight2 > 0.0f) addSkinWeight(v, w.boneId2, w.weight2);
            if (w.weight3 > 0.0f) addSkinWeight(v, w.boneId3, w.weight3);
        }
    }

    // ── Read indices ──
    std::vector<u16> indices(sub.indexCount);
    fseek(f, sub.indexOffset + 16, SEEK_SET);
    fread(indices.data(), sizeof(u16), sub.indexCount, f);

    // Build triangles
    for (u32 i = 0; i < sub.indexCount; i += 3) {
        addTriangle(indices[i], indices[i + 1], indices[i + 2]);
    }

    // Assign material by name match
    for (auto& mat : materials) {
        if (strstr(sub.meshName, mat.materialName))
            assignMaterial(s, mat.materialName);
    }
}
```

### 17.5 Reading Reference Points

```cpp
std::vector<ReferencePoint> refPoints(header.refPointCount);
fseek(f, header.refPointOffset + 16, SEEK_SET);
fread(refPoints.data(), sizeof(ReferencePoint), header.refPointCount, f);

for (auto& rp : refPoints) {
    Vec3f pos = {
         rp.position.x * 17.0f,
        -rp.position.y * 17.0f,
         rp.position.z * 17.0f
    };
    Quat rot = { rp.rotation.x, -rp.rotation.y, rp.rotation.z, rp.rotation.w };
    createHelper(rp.name, pos, rot);

    if (rp.parentId != 0xFFFFFFFF)
        setParent(rp.name, bones[rp.parentId].name);
}

fclose(f);
```

---

## 18. Known Unknowns

The following areas of the format remain undocumented or only partially understood:

| Area | Details |
|------|---------|
| **Header 0x030–0x0A7** | 120-byte character-specific block. Partially decoded as look-at constraints. **Nearly unused**: only 1/200 character files has non-zero values (contains `"Root_Joint"` bone name). The look-at system appears vestigial. |
| **Header 0x204–0x214** | Five constant values (480, 30, 30, 30, 30). Purpose unknown. 480 ÷ 16 = 30 suggests a possible array of 30 sixteen-byte blocks. |
| **Submesh +0x050 render mode flags** | Values like 0x904D (47%), 0xFFFFFFFF (16%), 0x9047, 0x9049, 0x0001 observed across 78,586 submeshes. Exact bit meanings within these flags are not decoded. |
| **Submesh +0x054 material slot ref** | 0xFFFFFFFF in 91.5% of submeshes. When set to another value, purpose is unclear — possibly an index into a material slot or shader parameter table. |
| **Cloth internal runtime data** | The 12 reserved bytes at ClothTriangle +0x00 are likely filled at runtime with inverse rest matrices for strain computation. Exact runtime struct not confirmed. |
| **Bone constraint params** | 20 bytes (+0x10C in BoneEntry) of tiny floats near zero. Likely IK or physics constraint parameters but specific meanings unknown. |
| **Bone sparse data** | 12 bytes (+0x120 in BoneEntry) non-zero in only 734/2,777 bones. Purpose unclear. |
| **Bone mirror ref** | 4 bytes (+0x12C in BoneEntry) — 0xFFFFFFFF in all but 5 bones. The 5 non-null values may be mirror bone indices or IK chain references. |
| **Octree node internals** | Block 1 entries are 48 bytes starting with a bounding sphere (16 bytes). The remaining 32 bytes (child indices for up to 8 octants, leaf flags, geometry references) are not fully decoded. |
| **Octree leaf cell format** | Block 2 has variable entry sizes (12 or 24-byte alignment). Internal structure not decoded. |
| **Collision capsule params** | The `capsuleParams` (12 bytes at +0x5C) likely encode half-length and radius but exact field assignment is unconfirmed. |
| **Version 247** | Original Diablo III release format with 236-byte bones and different submesh layout. No v247 files in analyzed corpus (all 11,347 are v260). |

### 18.1 Resolved Items

The following items from earlier versions of this specification have been resolved:

| Area | Resolution |
|------|-----------|
| **Header 0x128–0x13C** | ~~Unknown large-file parameters~~ — **RESOLVED**: Octree parameters. Non-zero in exactly 3,137 files (same as extra data blocks). See §14.5. |
| **Bone trailing 108 bytes** | ~~Unknown~~ — **RESOLVED**: Two additional PRS transforms (`inverseBind` + `restPose`, 64 bytes) + constraint parameters (20 bytes) + sparse data (12 bytes) + bone reference (4 bytes) + padding (8 bytes). See §7.2. |
| **Extra data blocks 1–3** | ~~Possibly DDS textures or LOD data~~ — **RESOLVED**: Octree system (node hierarchy with bounding spheres + leaf cells + geometry-to-node index mapping). See §14. |
| **Collision capsules** | ~~Format not documented~~ — **RESOLVED**: 104-byte entries with name[64] + boneIndex + quaternion rotation + capsule params. See §13.2. |
| **Submesh sentinel fields** | ~~0x050/0x054 always 0xFFFFFFFF~~ — **RESOLVED**: `renderModeFlags` (0x050) and `materialSlotRef` (0x054) — not sentinels. 0x050 has multiple distinct values; 0x054 is 0xFFFFFFFF for "none". See §8. |
| **Cloth param block** | ~~128-byte inline block, internals unknown~~ — **RESOLVED**: `ClothDescriptor` with 5 count/offset/size array references (cloth vertices 84B, triangles 28B, anchors 28B, stretch links 24B, bend links 24B) plus solver iteration count and max extent float. 936 blocks across 631 files. See §8.1. |
| **Material shader data** | ~~Unknown~~ — **RESOLVED**: Fully documented in §5.1–5.3. Contains shaderMapSno, color constants, shader param overrides, and 160-byte texture entries. |
| **Animation pipeline** | ~~Unknown~~ — **RESOLVED**: `.ani` (v118), `.ans` (v24), `.ant` (v30) fully documented. See §15. |
| **SNO references to physics/particles** | ~~Possibly embedded~~ — **RESOLVED**: APP contains **NO** direct SNO references to `.phy`, `.prt`, `.clt`, or `.phm` files. All connections are through the ACR (Actor) hub system and naming conventions. See §19. |

---

## 19. Cross-Format References & Asset Connections

### 19.1 APP Has No Direct SNO References to Physics or Particles

A comprehensive scan of all 11,347 APP files against a database of 47,461 SNO identifiers from all other D3 formats reveals that **APP files contain NO intentional SNO references to physics, particle, cloth, or physics mesh files**.

All apparent u32 "matches" found during binary scanning are false positives from random collisions against the large SNO database. With ~47,000 possible IDs in a u32 space of ~4.3 billion, any random u32 has a ~0.001% chance of matching an existing SNO ID — and with millions of u32 values per large file, hundreds of coincidental matches are expected. The "matches" show nonsensical pairings (e.g., a `boneOffset` field value matching an unrelated `.acr` file's snoId).

### 19.2 The Actor (ACR) Hub System

The Diablo III asset system uses **Actor files (.acr) as the central hub** that connects all asset types for a game entity. The APP file is purely a visual/geometry container — it does not know about its physics, particles, or animations. Instead, the Actor file references each component:

```
                          Actor (.acr)
                              |
          +-------------------+-------------------+
          |                   |                   |
    Appearance (.app)    PhysMesh (.phm)     AnimSet (.ans)
    [snoId at 0x024]     [snoId at 0x028]    [snoId at 0x078]
          |
     Materials (.mat)
     [embedded inline]
          |
     ShaderMap (.shm)
     [shaderMapSno ref]
          |
     Shader (.shd) + Textures (.tex)
     [SNO references]
```

**ACR reference statistics** (from 19,177 Actor files):

| ACR Field | Offset | Non-zero Count | Percentage | Target Format |
|-----------|--------|---------------|------------|---------------|
| `appearanceSno` | 0x024 | 19,177 | 100% | .app |
| `physMeshSno` | 0x028 | 340 | 1.8% | .phm |
| `animSetSno` | 0x078 | 8,636 | 45.0% | .ans |

### 19.3 Name-Based Asset Pairing

Rather than embedding SNO cross-references, the D3 pipeline relies heavily on **naming conventions** to associate related assets:

**APP ↔ PhysMesh (.phm)**: 2,528 of 2,700 PhysMesh files (**93.6%**) share the exact same base filename as an APP file:
```
Appearances/barricade_b.app  ↔  PhysMesh/barricade_b.phm
Appearances/a1dun_Leor_*.app ↔  PhysMesh/a1dun_Leor_*.phm
```

**APP ↔ Particle (.prt)**: 502 exact name matches, plus 9,707 partial matches where the particle file appends a descriptive suffix:
```
Appearances/barricade_b.app     →  Particle/barricade_b.prt (exact)
Appearances/barricade_b.app     →  Particle/barricade_b_dust.prt (partial)
Appearances/barricade_b.app     →  Particle/barricade_b_woodchipspray.prt (partial)
```

**APP ↔ Cloth (.clt)**: 7 exact name matches for models with cloth simulation:
```
Appearances/a1dun_Crypts_Dual_Sarcophagus.app  ↔  Cloth/a1dun_Crypts_Dual_Sarcophagus.clt
Appearances/player_banner_*.app                ↔  Cloth/player_banner_*.clt
Appearances/skeletonKing_*.app                 ↔  Cloth/skeletonKing_*.clt
```

### 19.4 SNO Database Summary

The D3 asset corpus contains the following file type counts used in this analysis:

| Format | Extension | File Count | Description |
|--------|-----------|-----------|-------------|
| Appearance | .app | 11,347 | 3D models (this format) |
| Actor | .acr | 19,177 | Game entity definitions (hub) |
| Particle | .prt | 21,593 | Particle system definitions |
| Material | .mat | 3,843 | Standalone material definitions |
| PhysMesh | .phm | 2,700 | Physics collision meshes |
| Physics | .phy | 74 | Rigid body physics presets |
| Cloth | .clt | 74 | Cloth simulation presets |

---

## Appendix A — File Statistics

Analysis of 11,347 `.app` files from the Diablo III game data:

| Metric | Value |
|--------|-------|
| Total files | 11,347 |
| Total size | 13,742.6 MB |
| Version distribution | 100% version 260 |
| Vertex stride | 100% 44 bytes |
| Submesh entry size | 100% 400 bytes |
| Bone entry size | 100% 312 bytes |
| Material entry size | 100% 144 bytes |
| Look entry size | 100% 64 bytes |
| Ref point entry size | 100% 96 bytes |
| Weight entry size | 100% 24 bytes |

### Object Type Distribution (sample of 200 files)

| `objectType` (+0x20) | Count | Typical Content |
|-----------------------|-------|----------------|
| 1 | 126 | Static props, dungeon tiles |
| 2 | 25 | Animated models (NPCs, monsters) |
| 18 | 22 | Complex scenes, multi-mesh dungeon segments |
| 6 | 17 | Player characters with full skeleton |
| 0 | 10 | Minimal/effect models |

### Most Common Render Flags (18,599 submeshes from 500 files)

| Value | Hex    | Count | Percentage |
|-------|--------|-------|-----------|
| 4111  | 0x100F | 7,225 | 38.8% |
| 6159  | 0x180F | 2,755 | 14.8% |
| 5191  | 0x1447 | 2,047 | 11.0% |
| 4105  | 0x1009 | 940   | 5.1% |
| 4127  | 0x101F | 774   | 4.2% |

---

## Appendix B — All Structures Summary

For quick reference, all APP structs defined in this specification:

```cpp
// §2 – Primitive Types
struct Vector3f   { f32 x, y, z; };             // 12 bytes
struct Vector4f   { f32 x, y, z, w; };          // 16 bytes
struct PackedNormal { u8 x, y, z, pad; };       //  4 bytes
struct PackedUV   { u16 u, v; };                //  4 bytes

// §3 – File Header
struct AppHeader  { /* 552 bytes */ };

// §5 – Materials
struct MaterialEntry {                          // 144 bytes
    char materialName[128]; u32 shaderDataOffset; u32 shaderDataSize;
    u32 _reserved[2];
};

// §5.1 – Material Shader Data Block
struct AppMaterialDataBlock {                   // ~248 bytes
    u32 renderGroup; i32 _flags04; u32 paramSectionOffset; u32 paramSectionSize;
    u8 _res10[8]; u32 shaderMapSno;
    f32 diffuseRGBA[4]; f32 specularRGBA[4]; f32 emissiveRGBA[4]; f32 ambientRGBA[4];
    f32 specularPower; i32 materialFlags; u32 texDataOffset; u32 texDataSize;
    u8 _res6C[12];
};

// §5.2 – Shader Parameter Override
struct ShaderParamEntry {                       // 12 bytes
    u32 paramType;      // 0=int, 1=float, 2=SNO ref
    u32 paramId;        // 0x0003XXYY identifier
    u32 value;          // Type-dependent
};

// §5.3 – Texture Entry
struct TextureEntry {                           // 160 bytes
    u32 slotIndex; u32 _res04; u32 textureSno; u32 textureFlags;
    f32 uvMatrix[16]; f32 uvAnimParams[16]; u32 _res90[2]; u32 textureType; u32 _res9C;
};

// §6 – Look Table
struct LookEntry  { char name[64]; };           // 64 bytes

// §7 – Bones
struct PRSTransform {                           // 32 bytes
    Vector4f rotation; Vector3f position; f32 scale;
};
struct BoneEntry {                              // 312 bytes
    char name[64]; u32 parentId; Vector3f aabbMin; Vector3f aabbMax;
    f32 sphereRadius; Vector3f sphereCenter; PRSTransform transforms[3];
    PRSTransform inverseBind; PRSTransform restPose;
    f32 boneParams[5]; u32 _sparse[3]; u32 mirrorBoneRef; u8 _pad[8];
};

// §8 – Submeshes
struct SubmeshEntry { /* 400 bytes */ };

// §9 – Vertices
struct Vertex {                                 // 44 bytes
    Vector3f position; u8 normal[3]; u8 _pad; u8 boneIndices[4];
    u8 _unknown[4]; PackedUV texCoord; u8 _pad2[4]; u8 tangent[3];
    u8 _pad3; u8 binormal[3]; u8 _pad4; u8 _pad5[4];
};

// §10 – Vertex Weights
struct VertexWeight {                           // 24 bytes
    u32 boneId1; f32 weight1; u32 boneId2; f32 weight2;
    u32 boneId3; f32 weight3;
};

// §11 – Indices
struct Triangle { u16 index0, index1, index2; };// 6 bytes

// §12 – Reference Points
struct ReferencePoint {                         // 96 bytes
    char name[64]; u32 parentId; Vector4f rotation; Vector3f position;
};

// §13.2 – Collision Capsules
struct CollisionCapsule {                       // 104 bytes
    char name[64]; u32 _res[2]; u32 boneIndex; Vector4f rotation;
    Vector3f capsuleParams; u32 _res2;
};

// §8.1 – Cloth Simulation
struct ClothDescriptor {                        // 128 bytes
    u32 vertexCount, vertexOffset, vertexDataSize; u32 _p1[3];
    u32 triangleCount, triangleOffset, triangleDataSize; u32 _p2[3];
    u32 anchorCount, anchorOffset, anchorDataSize; u32 _p3[3];
    u32 stretchCount, stretchOffset, stretchDataSize; u32 _p4[3];
    u32 bendCount, bendOffset, bendDataSize; u32 _p5[3];
    u32 simIterations; f32 maxExtent;
};
struct ClothVertex      { Vector3f pos; f32 invMass; f32 _p[2]; Vector3f rest;  // 84B
    f32 _p2[4]; u32 pinBoneRef; f32 _p3; u32 n0, n1; f32 _p4[5]; };
struct ClothTriangle    { u32 _res[3]; f32 restArea; u32 v0, v1, v2; };         // 28B
struct ClothAnchor      { u32 vtx, a0, a1, _res; f32 w0, w1; u32 _p; };       // 28B
struct ClothStretchLink { u32 v0, v1; f32 restLen; u32 _r[2]; f32 stiff; };   // 24B
struct ClothBendLink    { u32 v0, v1; f32 restLen; u32 _r[3]; };              // 24B

// §14 – Octree
struct OctreeNode {                             // 48 bytes
    f32 centerX, centerY, centerZ, radius; u8 nodeData[32];
};
struct OctreeIndexEntry {                       // 8 bytes
    u32 geometryIndex; u32 nodeIndex;
};

// §15 – Animation (see ANI_FILE_FORMAT_SPECIFICATION.md for full details)
struct BoneTrackDescriptor {                    // 24 bytes × 3 channels × boneCount
    u32 keyframeCount, keyframeOffset, keyframeSize; u32 _pad[3];
};
struct TranslationKeyframe { u32 frame; Vector3f position; };   // 16 bytes
struct RotationKeyframe    { u32 frame; i16 rotation[4]; };     // 12 bytes
struct ScaleKeyframe       { u32 frame; f32 scale; };           //  8 bytes
```

---

## Appendix C — Credits & Sources

| Source | Author | Description |
|--------|--------|-------------|
| Import Diablo III Models v2.0.ms | Taylor Mouse (2014) | 3ds Max importer script for .app and .ani files |
| Import Diablo III Animations v2.0.ms | Taylor Mouse (2014) | 3ds Max animation importer |
| [Diablo3-model-exporter](https://github.com/powerjasper/Diablo3-model-exporter) | powerjasper (2021) | C++ OBJ exporter with AppFile.h/AppFile.cpp struct definitions |
| Binary corpus analysis | WhiteoutLib project | Statistical analysis of 11,347 .app files from Diablo III game data |

---

## Appendix D — Comparison with Related Formats

### D3 Material Pipeline Specifications

The APP material system is documented across three companion specification documents:

| Document | Format | Description |
|----------|--------|-------------|
| `MAT_FILE_FORMAT_SPECIFICATION.md` | `.mat` | Standalone material definitions (3,843 files) — full documentation of colors, shader params, texture entries |
| `SHM_FILE_FORMAT_SPECIFICATION.md` | `.shm` | ShaderMap lookup tables (1,192 files) — maps render configs to compiled shaders |
| `SHD_FILE_FORMAT_SPECIFICATION.md` | `.shd` | Shader definitions (2 files) — source metadata with FX file refs, entry points |

### Cross-Format Comparison

The APP format shares design patterns with other Blizzard model formats:

| Feature | APP (D3) | M2 (WoW) | MDX (WC3) | M3 (SC2/HotS) |
|---------|----------|-----------|-----------|----------------|
| Magic | 0xDEADBEEF | "MD20" | "MDLX" | "MD34" |
| Byte order | Little-endian | Little-endian | Little-endian | Little-endian |
| Chunk alignment padding | 16 bytes (zeros) | — | — | 16 bytes (0xAA fill) |
| Index table | None (direct offsets) | Embedded | Tag-based chunks | Central index table |
| Vertex stride | Fixed 44 bytes | 48 bytes | Variable | Variable |
| Max bone influences | 3 | 4 | Unlimited (groups) | 4 |
| UV encoding | Packed u16 | Float32 | Float32 | Float16/32 |
| Normal encoding | Packed u8×3 | Float32 | Float32 | Packed 8-bit |
| Skeleton | Flat array + parentId | Flat + parentId | Hierarchical chunks | Flat + parentId |
| Animation | Separate .ani files | Embedded tracks | Embedded tracks | Embedded tracks |
| Materials | 144-byte name+offset entries | M2 material chunks | Chunk-based | MATM/MAT_/LAYR |
