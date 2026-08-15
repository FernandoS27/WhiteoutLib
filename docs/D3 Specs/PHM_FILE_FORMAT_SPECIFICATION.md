# PHM File Format Specification

**Format**: Diablo III Physics Collision Mesh (`.phm`)
**SNO group**: 61 (the legacy group-id table's 30 is Reverb)
**Byte Order**: Little-endian
**Magic**: `0xDEADBEEF`
**Version**: 24
**Corpus**: 2,700 files, 4,259 collision meshes, 792,315 triangles
**Registered revision**: 26 — the shipped data is v24, so the binary's compiled struct describes a *newer* layout (see below / README §4)

See [README.md](README.md) for the build these offsets come from, the generator pipeline
and the conventions used below.

---

## Table of Contents

1. [Overview](#1-overview)
2. [File Layout](#2-file-layout)
3. [PhysMesh Root Struct](#3-physmesh-root-struct)
4. [CollisionMesh](#4-collisionmesh)
5. [dmFloat4 — the vertex](#5-dmfloat4--the-vertex)
6. [dmMeshTriangle](#6-dmmeshtriangle)
7. [dmMeshNode — the BVH](#7-dmmeshnode--the-bvh)
8. [Corpus Statistics](#8-corpus-statistics)
9. [Known Unknowns](#9-known-unknowns)
10. [Appendix A — Reading a .phm](#appendix-a--reading-a-phm)

---

## 1. Overview

A `.phm` holds the static collision geometry for a scene tile or a prop: one or more
triangle meshes, each with its own bounding-volume hierarchy. It is consumed by
**Domino**, Blizzard's in-house physics library — the shipping executable still carries
source paths such as `Contrib/Contrib/Domino/Collision/Shapes/dmTreeMesh.cpp`, and the
element type names in the SNO registration (`dmMeshNode`, `dmMeshTriangle`, `dmFloat3`,
`dmFloat4`) are Domino's own.

The layout below comes from two sources that agree completely: the type registration
compiled into the Nintendo Switch 2.6.2 build (`PhysMesh_RegisterTypes`, `0x710063C280`)
and the 2,700-file corpus. For this group the registered revision and the shipped v24
have the same sizes, so the registration applies directly.

Field *names* are stripped from retail builds; the names here are curated. The *type*
names are the binary's.

---

## 2. File Layout

```
┌──────────────────────────────────────────────────────┐
│ SNO header                              (16 bytes)   │
│   0x00 magic 0xDEADBEEF                              │
│   0x04 version 24                                    │
│   0x08 8 zero bytes                                  │
├──────────────────────────────────────────────────────┤
│ PhysMesh struct                         (48 bytes)   │  file 0x10
├──────────────────────────────────────────────────────┤
│ CollisionMesh[dwMeshCount]         (112 bytes each)  │  file 0x40
├──────────────────────────────────────────────────────┤
│ per-mesh payload blocks:                             │
│   dmFloat4       vertices   (16 bytes each)          │
│   dmMeshTriangle triangles  (28 bytes each)          │
│   dmMeshNode     BVH nodes  (16 bytes each)          │
└──────────────────────────────────────────────────────┘
```

**Every stored offset is struct-relative: file position = 16 + offset.**

The mesh array's `SerializeData.byteOffset` is 48 — the struct size — in 2,678 of the
2,678 files that carry at least one mesh. 22 files carry none and write `{0, 0}`.

---

## 3. PhysMesh Root Struct

```cpp
struct PhysMesh {                       // 48 bytes @ struct+0
    u32           dwSnoId;              // +0
    u32           _pad04[2];            // +4   zero in 2700/2700
    u32           dwPhysMeshFlags;      // +12  0 in 2474, 1 in 226
    u32           dwMeshCount;          // +16  0..5
    SerializeData sdMeshes;             // +20  {byteOffset, byteSize}
    u32           _pad1C;               // +28  zero in 2700/2700
    void*         pMeshes;              // +32  zero in 2700/2700 (runtime pointer)
    u32           dwSourceToken;        // +40  see below
    u32           _pad2C;               // +44  zero in 2700/2700
};
```

`sdMeshes.byteSize / dwMeshCount == 112` exactly in every non-empty file, which is
`sizeof(CollisionMesh)` as registered.

### `dwSourceToken` (+40)

A 32-bit token, 2,682 distinct values over 2,700 files. Earlier tables called it
`flTotalVolume`; it is **not** a float — read as one, the values span −2.26e38 ..
3.28e38 with no clustering.

What it is not:

* not a payload hash — 17 of the 18 duplicated values sit on pairs whose payload bytes
  differ;
* not a hash of the asset name — `h*0x1003F + c` and FNV-1a, in any case folding, match
  0 of 2,700.

The duplicate pairs are always sibling art assets: `Rock_Large_A`/`Rock_Large_B`,
`Hive_B`/`Hive_C`, `BurntBodyC`/`BurntBodyD`, `LanternC`/`LanternE`,
`Body_SpikedB`/`Body_SpikedB_NoBody`. That is consistent with a token identifying the
DCC source file the mesh was exported from, but it is **not established**.

### `dwPhysMeshFlags` (+12)

Set to 1 in 226 files, 0 in 2,474. All 226 have 2, 3 or 4 collision meshes
(175 / 49 / 2) and none has 0 or 1 — but 938 files with 2..5 meshes have it clear, so
it is not a multi-mesh marker. **Meaning not established.**

---

## 4. CollisionMesh

```cpp
struct CollisionMesh {                  // 112 bytes
    void*         pNodes;               // +0    zero on disk
    void*         pVertices;            // +8    zero on disk
    void*         pTriangles;           // +16   zero on disk
    dmFloat3      vCentre;              // +24
    dmFloat3      vHalfExtent;          // +36
    dmFloat3      vQuantScale;          // +48
    u32           dwNodeCount;          // +60   always odd
    u32           dwVertexCount;        // +64
    u32           dwTriangleCount;      // +68
    u32           dwTreeHeight;         // +72
    u32           _pad4C;               // +76   zero in 4259/4259
    SerializeData sdVertices;           // +80   element size 16
    SerializeData sdTriangles;          // +88   element size 28
    SerializeData sdNodes;              // +96   element size 16
    u32           dwUnknown68;          // +104  {0,2,4,8,12}
    u32           dwUnknown6C;          // +108  {0,1,2,4,5,6}
};
```

The three pointers are zero in 4,259 of 4,259 meshes. The descriptor for each array
sits in the +80..+104 block, not next to its pointer — the registration's `rawRel`
values (+96, +72, +72 from fields 0, 8, 16) say so and the corpus confirms it.

### The quantisation frame

The baker re-centres every mesh: the stored vertices are **exactly symmetric about the
origin** (componentwise `min == −max`) in 4,259 of 4,259 meshes.

| field | relation | verified |
| --- | --- | --- |
| `vHalfExtent` | `max\|vertex\| + 0.1640` per axis — a fixed collision margin (measured range 0.1640015 .. 0.1640472 over 1,707 axes) | all sampled |
| `vQuantScale` | `2 * vHalfExtent / 65535`, to 7 significant digits | 4,259 / 4,259 |
| `vCentre` | the removed centre. `[vCentre − vHalfExtent, vCentre + vHalfExtent]` lands on D3 scene tiles — e.g. `A2dun_Swr_E_Entrance_01` mesh 0 spans −0.2..120.2 × −0.2..122.5 (a 120-unit tile), `A1dun_Leor_SEW_01` mesh 0 spans −0.3..242.9 × −2.9..242.5 (240-unit), `PvP_Cald_Swr_Appearance` mesh 2 spans −0.2..300.2 on both axes | inference, not proof |

### `dwTreeHeight` (+72)

Equals **maximum BVH node depth + 1** in 4,259 of 4,259 meshes — the traversal stack
size a query needs.

---

## 5. dmFloat4 — the vertex

```cpp
struct dmFloat4 { f32 x, y, z, pad; };  // 16 bytes
```

The fourth float is `0.0` in 529,653 of 529,653 vertices sampled: SIMD padding, not a
`w` component.

---

## 6. dmMeshTriangle

```cpp
struct dmMeshTriangle {                 // 28 bytes
    i32 nVertex[3];                     // +0    indices into the vertex array
    i32 nOpposite[3];                   // +12   apex vertex across each edge, -1 = boundary
    u16 wUnknown18;                     // +24   0      in 792315/792315
    u16 wMaterialIndex;                 // +26   0xFFFF in 792315/792315
};
```

`nVertex[0..2]` are in `[0, dwVertexCount)` in 792,315 of 792,315 triangles.

`nOpposite[k]` is **not** a triangle index. Used as one, the referenced triangle shares
two vertices with the referencing triangle only 5,298 times out of 2,182,130. It is the
**apex vertex of the triangle on the far side of edge `(nVertex[k], nVertex[k+1])`**,
with −1 for a boundary edge. Rebuilding that map from the triangle list reproduces the
stored value in **3,358,002 of 3,359,334 cases (99.96 %)**; the residue is non-manifold
edges. This is the internal-edge data a mesh-vs-convex collider needs in order to
reject false contacts on shared edges.

---

## 7. dmMeshNode — the BVH

```cpp
struct dmMeshNode {                     // 16 bytes
    s16 qMin[3];                        // +0
    s16 qMax[3];                        // +6
    i32 dwData;                         // +12
};
```

The registration declares six `DT_WORD`, but the words are **signed**. Dequantise
against the enclosing mesh:

```
world.axis = (int16)q[axis] * CollisionMesh.vQuantScale[axis]
```

so the quantisation origin is the mesh centre (which is the origin, because the baker
centres the mesh) and the ±32767 code range covers ±`vHalfExtent`.

`dwData` is a bitfield:

```
tag = dwData & 3

tag == 3           LEAF
                   triangleCount = (dwData >> 2) & 15      (1..4 observed)
                   firstTriangle =  dwData >> 6

tag == 0 | 1 | 2   INTERNAL, and the tag IS the split axis (X | Y | Z)
                   rightChild = thisNodeIndex + (dwData >> 6)
                   leftChild  = thisNodeIndex + 1
```

Nodes are stored in depth-first order, so an internal node's left child is always the
next node and only the right child needs an offset. `dwNodeCount` is always odd, as a
full binary tree requires.

Verified over the whole corpus:

| check | result |
| --- | --- |
| depth-first walk reaches every node exactly once, **and** the leaf triangle ranges tile `[0, dwTriangleCount)` with no gap and no overlap | **4,259 / 4,259** |
| every leaf's dequantised box contains the true bounding box of its own triangles | **4,259 / 4,259** |
| the root node's box contains the whole mesh | **4,259 / 4,259** |
| maximum leaf triangle count | 4 |
| internal split-axis distribution | X 117,981 · Y 121,364 · Z 66,744 |

Leaf triangle counts: 1 ×14,045 · 2 ×89,649 · 3 ×56,553 · 4 ×146,580.

The boxes are conservative — a leaf's stored box is slightly larger than its exact
triangle bounds (the quantiser floors the minimum and ceils the maximum).

---

## 8. Corpus Statistics

| metric | value |
| --- | --- |
| files | 2,700 (all version 24) |
| collision meshes | 4,259 |
| files by mesh count | 0 ×22 · 1 ×1,514 · 2 ×816 · 3 ×283 · 4 ×61 · 5 ×4 |
| triangles | 792,315 |
| BVH nodes | 214,533 distinct `dwData` values |
| `CollisionMesh` element size | 112 in 2,678 / 2,678 non-empty files |
| `(dwUnknown68, dwUnknown6C)` pairs | (0,0) ×2,392 · (8,0) ×750 · (2,5) ×485 · (0,6) ×223 · (12,0) ×165 · (4,1) ×75 · (8,1) ×45 · (12,1) ×30 |

---

## 9. Known Unknowns

| item | notes |
| --- | --- |
| `PhysMesh.dwPhysMeshFlags` | only bit 0 is ever set, and only on multi-mesh files; no engine reader found |
| `PhysMesh.dwSourceToken` | ruled out as a payload hash and as a name hash; the sibling-asset collisions suggest a source-file id |
| `CollisionMesh.dwUnknown68 / dwUnknown6C` | small flag word and small enum; no engine reader found |
| `dmMeshTriangle.wUnknown18 / wMaterialIndex` | constant across all 792,315 triangles, so the corpus cannot separate them; 0xFFFF reads like a "none" sentinel |

---

## Appendix A — Reading a .phm

```cpp
const u8* f = file;                       // whole file in memory
assert(rd32(f + 0) == 0xDEADBEEF);
assert(rd32(f + 4) == 24);
const u8* s = f + 16;                     // struct base

u32 meshCount  = rd32(s + 16);
u32 meshOffset = rd32(s + 20);            // 48

for (u32 m = 0; m < meshCount; ++m) {
    const u8* cm = s + meshOffset + m * 112;

    Vec3 centre = rdVec3(cm + 24);
    Vec3 half   = rdVec3(cm + 36);
    Vec3 scale  = rdVec3(cm + 48);        // == half * 2 / 65535

    u32 nodeCount = rd32(cm + 60);
    u32 vertCount = rd32(cm + 64);
    u32 triCount  = rd32(cm + 68);

    const u8* V = s + rd32(cm + 80);      // dmFloat4[vertCount]
    const u8* T = s + rd32(cm + 88);      // dmMeshTriangle[triCount]
    const u8* N = s + rd32(cm + 96);      // dmMeshNode[nodeCount]

    // depth-first traversal
    int stack[64]; int sp = 0; stack[sp++] = 0;
    while (sp) {
        int i = stack[--sp];
        const u8* n = N + i * 16;

        Vec3 lo, hi;
        for (int a = 0; a < 3; ++a) {
            lo[a] = (float)rd16s(n + a * 2)     * scale[a];
            hi[a] = (float)rd16s(n + 6 + a * 2) * scale[a];
        }
        // world-space box is [centre + lo, centre + hi]

        i32 data = rd32(n + 12);
        if ((data & 3) == 3) {
            u32 first = (u32)data >> 6;
            u32 count = ((u32)data >> 2) & 15;
            for (u32 k = first; k < first + count; ++k) {
                const u8* t = T + k * 28;
                u32 a = rd32(t + 0), b = rd32(t + 4), c = rd32(t + 8);
                // vertices at V + a*16, V + b*16, V + c*16 (add `centre` for world space)
            }
        } else {
            // (data & 3) is the split axis
            stack[sp++] = i + ((u32)data >> 6);   // right child
            stack[sp++] = i + 1;                  // left child
        }
    }
}
```

---

*Derived from the Diablo III Nintendo Switch 2.6.2 type registration at 0x710063C280
and all 2,700 `.phm` files in the corpus.*
