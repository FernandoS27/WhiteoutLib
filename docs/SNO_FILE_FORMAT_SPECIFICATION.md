# SNO File Format Specification

**System**: Structured Numbered Object (SNO) Asset System  
**Games**: Diablo III, Diablo IV  
**Byte Order**: Little-endian throughout  
**File Magic**: `0xDEADBEEF`  
**CoreTOC Magic (D4 New)**: `0xBCDE6611`

---

## Table of Contents

1.  [Overview](#1-overview)
2.  [Primitive Types](#2-primitive-types)
3.  [SNO Groups](#3-sno-groups)
4.  [CoreTOC — Master Asset Index](#4-coretoc--master-asset-index)
    1.  [Format Detection](#41-format-detection)
    2.  [D3 Legacy Format](#42-d3-legacy-format)
    3.  [D4 Old Format](#43-d4-old-format)
    4.  [D4 New Format](#44-d4-new-format)
    5.  [Entry Record (Common)](#45-entry-record-common)
5.  [SNO File Header](#5-sno-file-header)
    1.  [D3 Preamble (32 bytes)](#51-d3-preamble-32-bytes)
    2.  [D4 Header (16 bytes)](#52-d4-header-16-bytes)
6.  [D4 Type System](#6-d4-type-system)
    1.  [Type Registry Overview](#61-type-registry-overview)
    2.  [SnoTypeDef](#62-snotypedefs)
    3.  [SnoFieldDef](#63-snofielddefs)
    4.  [Format Hash Map](#64-format-hash-map)
    5.  [Registry Lookup](#65-registry-lookup)
7.  [D4 Basic Types](#7-d4-basic-types)
    1.  [Scalar Types](#71-scalar-types)
    2.  [Vector and Color Types](#72-vector-and-color-types)
    3.  [Reference Types](#73-reference-types)
    4.  [String Types](#74-string-types)
    5.  [Container Types](#75-container-types)
    6.  [Composite Types](#76-composite-types)
8.  [D4 Alignment Rules](#8-d4-alignment-rules)
9.  [D4 Deserialization Pipeline](#9-d4-deserialization-pipeline)
10. [D4 TagMap Format](#10-d4-tagmap-format)
11. [D4 Polymorphic Variable Array](#11-d4-polymorphic-variable-array)
12. [External Data](#12-external-data)
13. [Key Constants Summary](#13-key-constants-summary)
14. [Appendix A — Full SNO Group Table](#appendix-a--full-sno-group-table)
15. [Appendix B — D4 Basic Type Hashes](#appendix-b--d4-basic-type-hashes)

---

## 1. Overview

The **SNO** (Structured Numbered Object) system is the unified asset serialization format used by Blizzard's Diablo series. Every asset in the game — actors, animations, textures, materials, particle effects, quests, and so on — is stored as an SNO file belonging to a specific **SNO group** (asset category).

A central index file, **CoreTOC.dat** (Core Table of Contents), maps each asset's numeric identifier (`snoId`) to its human-readable name and group. Individual SNO files all begin with a common header containing the magic number `0xDEADBEEF`, followed by type-specific payload data.

The system evolved across game versions:

| Aspect | Diablo III | Diablo IV |
|--------|-----------|-----------|
| CoreTOC | No magic, 70 fixed groups | Magic `0xBCDE6611` (new) or plain (old) |
| File Header | 32-byte preamble | 16-byte header |
| Type System | Implicit (version per group) | Explicit registry with 2,915 types |
| Storage | Bare files on disk | CASC archive or disk |
| Group IDs | 0–69 | 0–180+ (extensible) |

---

## 2. Primitive Types

All multi-byte values are stored in **little-endian** byte order.

```
Type    C equiv          Size    Description
──────  ───────────────  ──────  ───────────────────────────
u8      uint8_t          1       Unsigned byte
u16     uint16_t         2       Unsigned 16-bit integer
u32     uint32_t         4       Unsigned 32-bit integer
u64     uint64_t         8       Unsigned 64-bit integer
i8      int8_t           1       Signed byte
i16     int16_t          2       Signed 16-bit integer
i32     int32_t          4       Signed 32-bit integer
i64     int64_t          8       Signed 64-bit integer
f32     float            4       IEEE 754 single-precision
f64     double           8       IEEE 754 double-precision
```

---

## 3. SNO Groups

Each SNO asset belongs to exactly one **group**, identified by a signed 32-bit integer. Groups define the asset category and determine the file extension. Special sentinel values exist for internal use.

**Sentinel Values**:

| ID | Name | Meaning |
|----|------|---------|
| −3 | `Unknown` | Unrecognized group |
| −2 | `Code` | Engine code reference |
| −1 | `None` | Null / no group |

Groups 0–180 are assigned to specific asset types. The full table is in [Appendix A](#appendix-a--full-sno-group-table). Common groups include:

| ID | Name | Extension | Description |
|----|------|-----------|-------------|
| 1 | Actor | `.acr` | World entities (monsters, NPCs, items, etc.) |
| 6 | Animation | `.ani` | Skeletal animations |
| 8 | AnimSet | `.ans` | Animation set definitions |
| 9 | Appearance | `.app` | Visual appearance descriptors |
| 27 | Particle | `.prt` | Particle system definitions |
| 28 | Physics | `.phy` | Physics / collision meshes |
| 33 | Scene | `.scn` | 3D scene layouts |
| 44 | Texture | `.tex` | Texture data |
| 57 | Material | `.mat` | Material / shader bindings |
| 67 | AnimTree | `.ant` | Animation blend trees |

In CASC-based storage (D4), each group has a corresponding directory name (e.g., `"Actor"`, `"Texture"`) used in virtual file paths.

---

## 4. CoreTOC — Master Asset Index

**File**: `CoreTOC.dat`

The CoreTOC is the master index mapping every asset's `(SnoGroup, snoId)` pair to its human-readable name. Three format variants exist across game versions.

### 4.1 Format Detection

Detection proceeds by inspecting the first 4 bytes of the file:

```
firstWord = read_u32(offset 0)

if firstWord == 0xBCDE6611:
    → D4 New format (§4.4)

elif firstWord == 0 AND fileSize > 1184:
    → D3 Legacy format (§4.2)

else:
    → D4 Old format (§4.3)
    firstWord is interpreted as snoGroupsCount
```

The D3 heuristic works because group 0 always has zero entries, making the first `u32` in the counts array equal to `0`. The size threshold (1184 = 1120 header + 64 byte minimum data) guards against false positives.

---

### 4.2 D3 Legacy Format

**Magic**: None  
**Fixed group count**: 70  
**Header size**: 1120 bytes (0x460)  
**Game**: Diablo III

#### Header Layout

The header consists of four arrays of 70 `u32` values each, laid out contiguously:

```
Offset    Size           Array               Description
──────    ────           ─────               ───────────
0x000     70 × 4 = 280  entryCounts[70]      Number of entries per group (inflated*)
0x118     70 × 4 = 280  sectionOffsets[70]    Byte offset of group section (relative to data start)
0x230     70 × 4 = 280  hashCounts[70]        Unused hash-related array
0x348     70 × 4 = 280  hashData[70]          Unused hash-related array
──────
0x460                    ← Data section begins here
```

> \*The `entryCounts` values are slightly inflated vs. the actual number of entries (typically ~23 higher). The true count must be determined by scanning entries until the `snoGroup` field no longer matches.

#### Data Section

The data section contains **per-group sections**, each located at `0x460 + sectionOffsets[g]`. A group section consists of a contiguous block of 12-byte entry records followed by an **inline name pool**:

```
┌─────── Per-Group Section ────────────────────────────────────┐
│  Entry Record 0       (12 bytes)                             │
│  Entry Record 1       (12 bytes)                             │
│  ...                                                         │
│  Entry Record N-1     (12 bytes)                             │
├─────── Name Pool ────────────────────────────────────────────┤
│  "FirstAssetName\0"                                          │
│  "SecondAssetName\0"                                         │
│  ...                                                         │
└──────────────────────────────────────────────────────────────┘
```

- **Section boundaries**: To determine where one section ends and the next begins, sort all non-empty group sections by their `sectionOffset`. The end of each section is the start of the next, or EOF for the last.
- **Entry scanning**: Rather than trusting `entryCounts[g]`, scan entries starting from the section beginning. Each entry's first field (`snoGroup`) must equal the expected group ID `g`. Stop when it doesn't match.
- **Name offsets** are relative to `nameBase`, which is `sectionStart + actualEntryCount × 12`.

---

### 4.3 D4 Old Format

**Magic**: None  
**First word**: `snoGroupsCount` (variable, must be ≤ 1024)  
**Header arrays**: 3  
**Game**: Diablo IV (earlier builds)

#### Header Layout

```
Offset    Size                          Content
──────    ────                          ───────
0x00      4                             snoGroupsCount
0x04      snoGroupsCount × 4           entryCounts[]
+         snoGroupsCount × 4           sectionOffsets[]
+         snoGroupsCount × 4           unknown[]           (unused)
+         4                             padding word
──────
          ← Entry data begins
```

**Data start** = `4 + 3 × snoGroupsCount × 4 + 4` = `8 + 12 × snoGroupsCount`

Entry layout per group section is identical to D3 — see [§4.5](#45-entry-record-common).

---

### 4.4 D4 New Format

**Magic**: `0xBCDE6611` at offset 0  
**Header arrays**: 4  
**Game**: Diablo IV (current builds)

#### Header Layout

```
Offset    Size                          Content
──────    ────                          ───────
0x00      4                             magic (0xBCDE6611)
0x04      4                             snoGroupsCount
0x08      snoGroupsCount × 4           entryCounts[]
+         snoGroupsCount × 4           sectionOffsets[]
+         snoGroupsCount × 4           unknown[]           (unused)
+         snoGroupsCount × 4           formatHashes[]      (per-group format hash)
+         4                             padding word
──────
          ← Entry data begins
```

**Data start** = `8 + 4 × snoGroupsCount × 4 + 4` = `12 + 16 × snoGroupsCount`

The `formatHashes[]` array provides the format hash for each SNO group's root type definition. Non-zero entries map `groupIndex → formatHash`, which is used during deserialization when a file's own `formatHash` field is zero.

---

### 4.5 Entry Record (Common)

All three CoreTOC variants use the same 12-byte entry record within each group section:

```
Offset    Size    Type    Field
──────    ────    ────    ─────
0x00      4       i32     snoGroup        Group ID (must match containing section)
0x04      4       i32     snoId           Unique asset identifier
0x08      4       i32     nameRelOffset   Relative offset into name pool
```

#### Name Resolution

- **D3**: `nameBase` = section start + (scanned entry count × 12). Names are null-terminated, up to 512 bytes.
- **D4**: `nameBase` = group data start + (entry count × 12). Names are null-terminated, up to 256 bytes.
- In both cases: `nameAddress = nameBase + nameRelOffset`

#### Output

Parsing produces a flat list of entries:

```
{ group: SnoGroup, snoId: i32, name: string }
```

Indexed by per-group spans and an `snoId → entry` hash map for fast lookup.

---

## 5. SNO File Header

Every SNO file begins with the magic number `0xDEADBEEF`. The header layout differs slightly between D3 and D4.

### 5.1 D3 Preamble (32 bytes)

```
Offset    Size    Type    Field           Description
──────    ────    ────    ─────           ───────────
0x00      4       u32     magic           0xDEADBEEF — file signature
0x04      4       u32     version         Format version (unique per group)
0x08      4       u32     _reserved       Always 0
0x0C      4       u32     _reserved       Always 0
0x10      4       u32     snoId           Unique asset hash
0x14      4       u32     _reserved       Always 0
0x18      4       u32     _reserved       Always 0
0x1C      4       u32     flags           Asset-specific flags
```

Each SNO group has a fixed version number. Known values:

| Group | Version |
|-------|---------|
| Actor (1) | 282 (0x11A) |
| Animation (6) | 118 (0x76) |
| Particle (27) | 180 (0xB4) |
| Material (57) | 25 (0x19) |

The version identifies the data layout of the payload following the preamble. All files within the same group share the same version.

### 5.2 D4 Header (16 bytes)

```
Offset    Size    Type    Field           Description
──────    ────    ────    ─────           ───────────
0x00      4       u32     magic           0xDEADBEEF — file signature
0x04      4       u32     formatHash      Hash identifying root type definition
0x08      4       u32     unknown08       Unknown / version
0x0C      4       u32     unknown0C       Unknown
```

The payload begins immediately at offset `0x10`. The first 4 bytes of payload (`offset 0x10`) are the `snoId` (i32).

The `formatHash` is used to resolve the root type definition from the type registry (§6.4). If it is `0`, the per-group format hash from `CoreTOC.dat` (§4.4) is used as a fallback.

---

## 6. D4 Type System

Diablo IV uses a self-describing type system to serialize and deserialize SNO file payloads. The type registry is compiled from the game's metadata and embedded into the library.

### 6.1 Type Registry Overview

| Metric | Value |
|--------|-------|
| Type definitions | 2,915 |
| Field definitions | 19,571 |
| Field name strings | 9,640 |
| Format hash → type hash entries | 130 |
| Hash table buckets | 8,192 (open addressing) |

### 6.2 SnoTypeDefs

Each type in the registry is described by a `SnoTypeDef`:

```
Field           Type    Description
─────           ────    ───────────
hash            u32     Unique type hash
isBasic         u32     1 = basic/leaf type, 0 = complex struct
size            u32     Serialized size in bytes (0 for variable-size basics)
flags           u32     Bit flags (0x8000 = has sub-type hash)
fieldsOffset    u32     Start index into global field array
fieldsCount     u32     Number of fields
dwFormatHash    u32     Non-zero for root SNO types
```

- **Basic types** are leaf types (scalars, vectors, strings, built-in containers) dispatched by hash.
- **Complex types** are structs composed of named fields, each at a specific byte offset.

### 6.3 SnoFieldDefs

Each field within a complex type is described by a `SnoFieldDef`:

```
Field               Type        Description
─────               ────        ───────────
typeHashes[3]       u32[3]      Type hash chain: [primary, subType1, subType2]
nameIndex           u32         Index into field name string table
flags               u32         Bit flags (see below)
offset              i32         Byte offset within parent struct
arrayLength         i32         Fixed array length (−1 if not a fixed array)
group               i32         SNO group for DT_SNO / DT_GBID refs (−1 if N/A)
serializedBitCount  i32         If 1, value is boolean; otherwise bit width hint
```

**Type hash chain**: `typeHashes[0]` is always the primary type. For container types (variable array, fixed array, optional, range, polymorphic array), `typeHashes[1]` is the element type hash. `typeHashes[2]` provides a further sub-type if needed. The chain is terminated by `DT_NULL` (hash `1028442418`).

**Field flags**:

| Flag | Meaning |
|------|---------|
| `0x200000` | External data (variable array) — data lives outside the file |
| `0x400000` | External data (polymorphic variable array) |

### 6.4 Format Hash Map

A sorted array of 130 `(formatHash, typeHash)` pairs. Given a file's `formatHash` (from the SNO header at offset 0x04), a binary search yields the root `typeHash`, which identifies the `SnoTypeDef` used to deserialize the payload.

### 6.5 Registry Lookup

- **`findType(hash)`** → `SnoTypeDef*` via open-addressing hash table (8,192 buckets)
- **`typeHashFromFormatHash(fh)`** → binary search on the format hash map
- **`fields(typeDef)`** → span of `SnoFieldDef` from global array at `[fieldsOffset .. fieldsOffset + fieldsCount)`
- **`fieldName(fieldDef)`** → string from `kFieldNames[nameIndex]`

---

## 7. D4 Basic Types

Basic types are leaf types dispatched by their type hash. Each has a fixed wire size and specific serialization rules.

### 7.1 Scalar Types

| Type | Hash | Wire Size | Value | Notes |
|------|------|-----------|-------|-------|
| `DT_NULL` | 1028442418 | 0 | — | Sentinel / chain terminator |
| `DT_BYTE` | 1028015787 | 1 | u8 | Boolean if `serializedBitCount == 1` |
| `DT_WORD` | 1028759507 | 2 | u16 | Boolean if `serializedBitCount == 1` |
| `DT_ENUM` | 1028111660 | 4 | i32 | Enumeration constant |
| `DT_INT` | 2764320258 | 4 | i32 | Boolean if `serializedBitCount == 1` |
| `DT_UINT` | 1028680983 | 4 | u32 | Boolean if `serializedBitCount == 1` |
| `DT_FLOAT` | 3864020909 | 4 | f32 | IEEE 754 single-precision |
| `DT_INT64` | 3867655596 | 8 | i64 | Boolean if `serializedBitCount == 1` |
| `DT_ACD_NETWORK_NAME` | 2866333320 | 8 | u64 | Network entity identifier |
| `DT_SHARED_SERVER_DATA_ID` | 3045283369 | 8 | u64 | Shared server data reference |
| `DT_STARTLOC_NAME` | 2193642883 | 4 | u32 | Start location identifier |

When `serializedBitCount == 1`, the value is interpreted as a boolean (`!= 0` → `true`).

### 7.2 Vector and Color Types

| Type | Hash | Wire Size | Layout |
|------|------|-----------|--------|
| `DT_BCVEC2I` | 1931092405 | 8 | `[i32 x, i32 y]` |
| `DT_VECTOR2D` | 3124492544 | 8 | `[f32 x, f32 y]` |
| `DT_VECTOR3D` | 3124492577 | 12 | `[f32 x, f32 y, f32 z]` |
| `DT_VECTOR4D` | 3124492610 | 16 | `[f32 x, f32 y, f32 z, f32 w]` |
| `DT_RGBACOLOR` | 2384880434 | 4 | `[u8 r, u8 g, u8 b, u8 a]` |
| `DT_RGBACOLORVALUE` | 3212271855 | 16 | `[f32 r, f32 g, f32 b, f32 a]` |

### 7.3 Reference Types

| Type | Hash | Wire Size | Layout | Notes |
|------|------|-----------|--------|-------|
| `DT_SNO` | 2764331143 | 4 | `i32 snoId` | References another SNO asset. `−1`, `0`, or `0xFFFFFFFF` = null. `field.group` gives the target SNO group. |
| `DT_SNO_NAME` | 3339108615 | 8 | `[i32 group, i32 snoId]` | Fully qualified reference. If `group == 0`, falls back to `field.group`. |
| `DT_GBID` | 1028170061 | 4 | `u32 raw` | Game Balance ID (hashed name). `0xFFFFFFFF` = null. `field.group` identifies the lookup table. |

### 7.4 String Types

| Type | Hash | Wire Size | Layout |
|------|------|-----------|--------|
| `DT_CSTRING` | 3846829457 | 16 | `[pad(8), i32 dataOffset, i32 dataSize]` |
| `DT_CHARARRAY` | 2175310548 | `field.arrayLength` | Fixed-length inline char buffer, null-trimmed |
| `DT_STRING_FORMULA` | 2450313795 | 32 | `[pad(8), i32 dataOffset, i32 dataSize, pad(16)]` |

For `DT_CSTRING` and `DT_STRING_FORMULA`, the actual string data lives out-of-line at `dataOffset` within the payload (offset 0x10 of the file). The inline descriptor's first 8 bytes are padding. `dataSize` includes the null terminator; the decoded string excludes it.

`DT_CHARARRAY` stores a fixed-length inline character buffer directly in the struct. The string is read up to the first null byte or `field.arrayLength`, whichever comes first.

### 7.5 Container Types

| Type | Hash | Wire Size | Description |
|------|------|-----------|-------------|
| `DT_FIXEDARRAY` | 2388214534 | `arrayLength × elemSize` | `field.arrayLength` consecutive elements of `typeHashes[1]` |
| `DT_VARIABLEARRAY` | 3244749660 | 16 | `[pad(4), pad(4), i32 dataOffset, i32 dataSize]` |
| `DT_OPTIONAL` | 3121633597 | `4 + subtypeSize` | `[i32 present]` then inline sub-value; null if `present == 0` |
| `DT_RANGE` | 3877855748 | `2 × subtypeSize` | Two consecutive sub-values → `{rangeValue1, rangeValue2}` |
| `DT_POLYMORPHIC_VARIABLEARRAY` | 1683664497 | 24 | See [§11](#11-d4-polymorphic-variable-array) |

For `DT_VARIABLEARRAY`: the inline 16-byte descriptor contains `dataOffset` and `dataSize`. At `dataOffset` within the payload, elements of `typeHashes[1]` are packed contiguously. The element count is `dataSize / elementSize`.

For `DT_FIXEDARRAY`: elements are stored inline, one after another. The element type and count are known from `typeHashes[1]` and `field.arrayLength`.

### 7.6 Composite Types

| Type | Hash | Wire Size | Description |
|------|------|-----------|-------------|
| `DT_TAGMAP` | 3493213809 | 16 | `[pad(8), i32 dataOffset, i32 dataSize]` — see [§10](#10-d4-tagmap-format) |
| `DT_BINDABLEPROPERTY` | 322094989 | 48 + subtypeSize | `[DT_CSTRING(16) dataStore, DT_CSTRING(16) dataPath, u32 formatterId, pad(12), sub-value]` — aligned to 8 bytes |

`DT_BINDABLEPROPERTY` wraps a sub-value with data-binding metadata. The total size is 48 bytes of binding header + the sub-value's size, with 8-byte alignment.

---

## 8. D4 Alignment Rules

Fields within a struct are laid out at their `offset` as specified in the `SnoFieldDef`. When reading variable-length data that requires alignment (e.g., TagMap contents, sequential reads), the following rules apply:

| Type | Normal Alignment | TagMap Alignment |
|------|:----------------:|:----------------:|
| `DT_POLYMORPHIC_VARIABLEARRAY` | 8 | 4 |
| `DT_STRING_FORMULA` | 8 | 4 |
| `DT_VARIABLEARRAY` | 8 | 4 |
| `DT_TAGMAP` | 8 | 4 |
| `DT_CSTRING` | 8 | 4 |
| `DT_CHARARRAY` | 1 | 1 |
| `DT_SNO_NAME` | 4 | 4 |
| `DT_BINDABLEPROPERTY` | 8 | 8 |
| `DT_FIXEDARRAY` | alignment of element | same |
| `DT_OPTIONAL` | alignment of element | same |
| `DT_RANGE` | alignment of element | same |
| Other basic types | `typeDef.size` (min 4) | same |
| Complex (struct) types | max alignment of all fields | same |

Inside a TagMap, pointer-like containers (`DT_CSTRING`, `DT_VARIABLEARRAY`, etc.) use 4-byte alignment instead of 8-byte, matching the TagMap's serialized layout.

---

## 9. D4 Deserialization Pipeline

Reading a D4 SNO file proceeds as follows:

```
1.  Validate: size ≥ 20 bytes, magic == 0xDEADBEEF
2.  Read formatHash from offset 0x04
    - If zero and an SnoGroup hint is available, look up in CoreTOC's
      per-group formatHashes (§4.4 formatHashes[] array)
3.  Resolve root type: formatHash → typeHash via binary search (§6.4)
4.  Look up root SnoTypeDef via findType(typeHash) (§6.5)
5.  Extract payload: data[0x10 ..]  — all type-system offsets are relative
    to this base
6.  Recursively deserialize via the root SnoTypeDef:
    a.  If typeDef.isBasic → dispatch to basic type reader (§7)
    b.  If complex → iterate fields, read each at (payloadBase + field.offset)
    c.  Container types recurse into element types
7.  Return SnoFile { magic, formatHash, snoId, typeName, root }
```

The result is a tree of `SnoValue` nodes — a tagged union with 20 variants:

| Variant | Source Types |
|---------|-------------|
| null | `DT_NULL` |
| bool | Any type with `serializedBitCount == 1` |
| i32 | `DT_INT`, `DT_ENUM` |
| u32 | `DT_UINT`, `DT_STARTLOC_NAME` |
| f32 | `DT_FLOAT` |
| i64 | `DT_INT64` |
| u64 | `DT_ACD_NETWORK_NAME`, `DT_SHARED_SERVER_DATA_ID` |
| u8 | `DT_BYTE` |
| u16 | `DT_WORD` |
| string | `DT_CSTRING`, `DT_CHARARRAY`, `DT_STRING_FORMULA` |
| vec2 | `DT_VECTOR2D` |
| vec3 | `DT_VECTOR3D` |
| vec4 | `DT_VECTOR4D` |
| ivec2 | `DT_BCVEC2I` |
| color | `DT_RGBACOLOR` |
| colorf | `DT_RGBACOLORVALUE` |
| ref | `DT_SNO`, `DT_SNO_NAME` |
| gbid | `DT_GBID` |
| array | `DT_VARIABLEARRAY`, `DT_FIXEDARRAY`, `DT_POLYMORPHIC_VARIABLEARRAY` |
| object | Complex structs, `DT_TAGMAP`, `DT_RANGE`, `DT_OPTIONAL`, `DT_BINDABLEPROPERTY` |

Objects are ordered string-keyed maps. Arrays are ordered value lists.

---

## 10. D4 TagMap Format

`DT_TAGMAP` is a self-describing serialized key-value store. The inline descriptor is 16 bytes: `[pad(8), i32 dataOffset, i32 dataSize]`. The actual data at `dataOffset` is:

```
Offset    Content
──────    ───────
0x00      i32 fieldCount    — number of fields in the map

For each field (fieldCount times):
+0        u32 nameHash      — field name hash
+4        u32 typeHash       — primary type hash

If the resolved type has flag 0x8000:
          u32 subTypeHash1   — first sub-type hash
          (recurse: if subType1 also has 0x8000, read another sub-type hash)

After all field headers:
          Aligned field values (alignment per §8, with TagMap rules)
```

Field names within a TagMap are rendered as `field_XXXXXXXX` (hex hash) since the string table for TagMap field names is external (`attributes.json`) and not embedded in the binary.

Values are read sequentially with alignment padding matching `getTypeAlignment(type, inTagMap=true)`.

---

## 11. D4 Polymorphic Variable Array

`DT_POLYMORPHIC_VARIABLEARRAY` allows an array where each element can be a different concrete type derived from a common base.

**Inline descriptor** (24 bytes):

```
Offset    Size    Field
──────    ────    ─────
0x00      8       padding
0x08      4       i32 dataOffset      — absolute offset in payload
0x0C      4       i32 dataSize        — total byte size of data region
0x10      4       i32 dataCount       — number of elements
0x14      4       padding
```

**Data region layout** at `dataOffset`:

```
┌──────────────────────────────────────────────────────────────┐
│  Pointer table: dataCount × 8 bytes  (skipped / unused)     │
├──────────────────────────────────────────────────────────────┤
│  Element 0:                                                  │
│    u32 dwType     — concrete type hash (0 = use default)     │
│    ... element data ...                                      │
│  Element 1:                                                  │
│    u32 dwType                                                │
│    ... element data ...                                      │
│  ...                                                         │
└──────────────────────────────────────────────────────────────┘
```

- The first `dataCount × 8` bytes are a pointer table that is skipped during deserialization.
- Each element begins with a `u32 dwType`. If non-zero, it identifies the concrete type. If zero, the default subtype from `typeHashes[1]` is used.
- The polymorphic base type hash is `0x5D4BAC71`. When resolved, the reader reads `dwType` to determine the actual type, then deserializes the element's fields starting at the `u32` following `dwType` (offset +4 within the element).

---

## 12. External Data

Fields with specific flag bits indicate that the variable-length data they reference exists **outside** the current file:

| Flag | Applies To | Meaning |
|------|-----------|---------|
| `0x200000` | `DT_VARIABLEARRAY` | Element data is external |
| `0x400000` | `DT_POLYMORPHIC_VARIABLEARRAY` | Element data is external |

When either flag is set, the inline descriptor's `dataOffset` and `dataSize` still describe the data's location and extent, but the data must be fetched from a companion file or secondary storage rather than from the current SNO file's payload.

External fields are represented in the value tree as objects with metadata:

```json
{ "__external__": true, "dataOffset": <int>, "dataSize": <int> }
```

---

## 13. Key Constants Summary

| Constant | Value | Context |
|----------|-------|---------|
| SNO file magic | `0xDEADBEEF` | All SNO files, offset 0x00 |
| D4 CoreTOC magic | `0xBCDE6611` | New CoreTOC format, offset 0x00 |
| D3 CoreTOC group count | 70 | Fixed; 4 header arrays × 70 |
| D3 CoreTOC header size | 1,120 bytes | 70 × 4 × 4 |
| CoreTOC entry size | 12 bytes | `[snoGroup(4), snoId(4), nameOff(4)]` |
| Polymorphic base hash | `0x5D4BAC71` | DT_POLYMORPHIC_VARIABLEARRAY |
| DT_NULL hash | `1028442418` | Type hash chain terminator |
| Max group ID (D3) | 69 | Fixed 70-slot header |
| Max group ID (D4) | 180 | `SnoGroup::Indicator` |
| snoGroupsCount limit | 1,024 | Sanity check for D4 formats |
| Registry types | 2,915 | D4 compiled type definitions |
| Registry fields | 19,571 | D4 compiled field definitions |
| Hash table buckets | 8,192 | Open-addressing type lookup |
| Format hash map entries | 130 | Root SNO type mappings |
| External data flag (VA) | `0x200000` | On `field.flags` |
| External data flag (PVA) | `0x400000` | On `field.flags` |
| Sub-type flag | `0x8000` | On `typeDef.flags` |

---

## Appendix A — Full SNO Group Table

| ID | Name | Extension | ID | Name | Extension |
|----|------|-----------|----|------|-----------|
| 1 | Actor | `.acr` | 2 | NpcComponentSet | `.npc` |
| 3 | AiBehavior | `.aib` | 4 | AiState | `.ais` |
| 5 | AmbientSound | `.ams` | 6 | Animation | `.ani` |
| 7 | Animation2D | `.an2` | 8 | AnimSet | `.ans` |
| 9 | Appearance | `.app` | 10 | Hero | `.hro` |
| 11 | Cloth | `.clt` | 12 | Conversation | `.cnv` |
| 13 | ConversationList | `.cnl` | 14 | EffectGroup | `.efg` |
| 15 | Encounter | `.enc` | 17 | Explosion | `.xpl` |
| 18 | FlagSet | `.flg` | 19 | Font | `.fnt` |
| 20 | GameBalance | `.gam` | 21 | Global | `.glo` |
| 22 | LevelArea | `.lvl` | 23 | Light | `.lit` |
| 24 | MarkerSet | `.mrk` | 26 | Observer | `.obs` |
| 27 | Particle | `.prt` | 28 | Physics | `.phy` |
| 29 | Power | `.pow` | 31 | Quest | `.qst` |
| 32 | Rope | `.rop` | 33 | Scene | `.scn` |
| 35 | Script | `.scr` | 36 | ShaderMap | `.shm` |
| 37 | Shader | `.shd` | 38 | Shake | `.shk` |
| 39 | SkillKit | `.skl` | 40 | Sound | `.snd` |
| 42 | StringList | `.stl` | 43 | Surface | `.srf` |
| 44 | Texture | `.tex` | 45 | Trail | `.trl` |
| 46 | UI | `.ui` | 47 | Weather | `.wth` |
| 48 | World | `.wrl` | 49 | Recipe | `.rcp` |
| 51 | Condition | `.cnd` | 52 | TreasureClass | `.trs` |
| 53 | Account | `.acc` | 57 | Material | `.mat` |
| 59 | Lore | `.lor` | 60 | Reverb | `.rev` |
| 62 | Music | `.mus` | 63 | Tutorial | `.tut` |
| 65 | ControlScheme | `.ctr` | 67 | AnimTree | `.ant` |
| 68 | Vibration | `.vib` | 71 | wWiseSoundBank | `.wsb` |
| 72 | Speaker | `.spk` | 73 | Item | `.itm` |
| 74 | PlayerClass | `.pcl` | 76 | FogVolume | `.fog` |
| 77 | Biome | `.bio` | 78 | Wall | `.wal` |
| 79 | SoundTable | `.sdt` | 80 | SubZone | `.sbz` |
| 81 | MaterialValue | `.mtv` | 82 | MonsterFamily | `.mfm` |
| 83 | TileSet | `.tst` | 84 | Population | `.pop` |
| 85 | MaterialValueSet | `.mvs` | 86 | WorldState | `.wst` |
| 87 | Schedule | `.sch` | 88 | VectorField | `.vfd` |
| 89 | PvPMode | `.pvp` | 90 | StoryBoard | `.stb` |
| 91 | POI | `.poi` | 92 | Territory | `.ter` |
| 93 | AudioContext | `.auc` | 94 | VoProcess | `.vop` |
| 95 | DemonScroll | `.dss` | 96 | QuestChain | `.qc` |
| 97 | LoudnessPreset | `.lou` | 98 | ItemType | `.itt` |
| 99 | Achievement | `.ach` | 100 | Crafter | `.crf` |
| 101 | HoudiniParticlesSim | `.hps` | 102 | Movie | `.vid` |
| 103 | TiledStyle | `.tsl` | 104 | Affix | `.aff` |
| 105 | Reputation | `.rep` | 106 | ParagonNode | `.pgn` |
| 107 | MonsterAffix | `.maf` | 108 | ParagonBoard | `.pbd` |
| 109 | SetItemBonus | `.set` | 110 | StoreProduct | `.prd` |
| 111 | ParagonGlyph | `.gph` | 112 | ParagonGlyphAffix | `.gaf` |
| 114 | Challenge | `.cha` | 115 | MarkingShape | `.msh` |
| 116 | ItemRequirement | `.irq` | 117 | Boost | `.bst` |
| 118 | Emote | `.emo` | 119 | Jewelry | `.jwl` |
| 120 | PlayerTitle | `.pt` | 121 | Emblem | `.emb` |
| 122 | Dye | `.dye` | 123 | FogOfWar | `.fow` |
| 124 | ParagonThreshold | `.pth` | 125 | AiAwareness | `.aia` |
| 126 | TrackedReward | `.trd` | 127 | CollisionSettings | `.col` |
| 128 | Aspect | `.asp` | 129 | AbTest | `.abt` |
| 130 | Stagger | `.stg` | 131 | EyeColor | `.eye` |
| 132 | Makeup | `.mak` | 133 | MarkingColor | `.mcl` |
| 134 | HairColor | `.hcl` | 135 | DungeonAffix | `.dax` |
| 136 | Activity | `.act` | 137 | Season | `.sea` |
| 138 | HairStyle | `.har` | 139 | FacialHair | `.fhr` |
| 140 | Face | `.fac` | 141 | MercenaryClass | `.mrc` |
| 142 | PassivePowerContainer | `.ppc` | 143 | MountProfile | `.mpp` |
| 144 | AICoordinator | `.aic` | 145 | CrafterTab | `.ctb` |
| 146 | TownPortalCosmetic | `.tpc` | 147 | AxeTest | `.axe` |
| 148 | Wizard | `.wiz` | 149 | FootstepTable | `.fst` |
| 150 | Modal | `.mdl` | 151 | CollectiblePower | `.cpw` |
| 152 | AppearanceSet | `.aps` | 153 | Preset | `.pst` |
| 154 | PreviewComposition | `.pvc` | 155 | SpawnPool | `.spn` |
| 156 | Raid | `.rdx` | 157 | BattlePassTier | `.bpt` |
| 158 | Zone | `.zon` | 160 | DeathKit | `.dtk` |
| 161 | Snippet | `.snp` | 162 | CommunityModifier | `.cmo` |
| 163 | GenericNodeGraph | `.gng` | 164 | UserDefinedData | `.udd` |
| 165 | DataStore | `.fds` | 166 | BehaviorContainer | `.bvr` |
| 167 | ActorService | `.asv` | 168 | DamageRemap | `.dmg` |
| 169 | Vendor | `.vnd` | 170 | GenericSkillTree | `.gst` |
| 172 | Crowd | `.crd` | 175 | VisualRemap | `.vrm` |
| 176 | PowerModifier | `.pmd` | 177 | UIDesignerNotification | `.udn` |
| 178 | HoudiniDigitalAsset | `.hds` | 179 | HoudiniDigitalAssetPreset | `.hdp` |
| 180 | Indicator | `.ind` | | | |

**Gap IDs** (no assigned group): 0, 16, 25, 30, 34, 41, 50, 54–56, 58, 61, 64, 66, 69–70, 75, 113, 159, 171, 173–174.

---

## Appendix B — D4 Basic Type Hashes

| Type Name | Hash (u32) | Wire Size (bytes) |
|-----------|------------|:------------------:|
| `DT_NULL` | 1028442418 | 0 |
| `DT_BYTE` | 1028015787 | 1 |
| `DT_WORD` | 1028759507 | 2 |
| `DT_ENUM` | 1028111660 | 4 |
| `DT_INT` | 2764320258 | 4 |
| `DT_UINT` | 1028680983 | 4 |
| `DT_FLOAT` | 3864020909 | 4 |
| `DT_INT64` | 3867655596 | 8 |
| `DT_ACD_NETWORK_NAME` | 2866333320 | 8 |
| `DT_SHARED_SERVER_DATA_ID` | 3045283369 | 8 |
| `DT_SNO` | 2764331143 | 4 |
| `DT_SNO_NAME` | 3339108615 | 8 |
| `DT_GBID` | 1028170061 | 4 |
| `DT_STARTLOC_NAME` | 2193642883 | 4 |
| `DT_CSTRING` | 3846829457 | 16 |
| `DT_CHARARRAY` | 2175310548 | variable |
| `DT_STRING_FORMULA` | 2450313795 | 32 |
| `DT_RGBACOLOR` | 2384880434 | 4 |
| `DT_RGBACOLORVALUE` | 3212271855 | 16 |
| `DT_BCVEC2I` | 1931092405 | 8 |
| `DT_VECTOR2D` | 3124492544 | 8 |
| `DT_VECTOR3D` | 3124492577 | 12 |
| `DT_VECTOR4D` | 3124492610 | 16 |
| `DT_OPTIONAL` | 3121633597 | 4 + sub |
| `DT_RANGE` | 3877855748 | 2 × sub |
| `DT_FIXEDARRAY` | 2388214534 | N × elem |
| `DT_VARIABLEARRAY` | 3244749660 | 16 |
| `DT_POLYMORPHIC_VARIABLEARRAY` | 1683664497 | 24 |
| `DT_TAGMAP` | 3493213809 | 16 |
| `DT_BINDABLEPROPERTY` | 322094989 | 48 + sub |
