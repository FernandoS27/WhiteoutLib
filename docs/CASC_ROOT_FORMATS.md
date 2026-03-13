# CASC Root File Formats

> Derived from [CascLib](https://github.com/ladislav-zezula/CascLib) source code
> (Copyright © Ladislav Zezula 2014–2025, MIT License).

CASC (Content-Addressable Storage Container) is Blizzard's storage system used
across all modern titles.  Every CASC storage has a **ROOT** manifest that maps
human-readable file paths (or numeric file-data IDs) to **Content Keys**
(CKeys).  A CKey is the MD5 hash of a file's uncompressed data (`CONTENT_KEY`,
16 bytes).  The ROOT format is **game-specific** — each title ships its own
variant.  This document covers the three formats relevant to WhiteoutLib:
World of Warcraft, Diablo 3, and Diablo 4 (TVFS).

---

## Table of Contents

1. [Common Primitives](#common-primitives)
2. [World of Warcraft Root](#world-of-warcraft-root)
   - [Header Versions](#wow-header-versions)
   - [Group Layout](#wow-group-layout)
   - [Entry Formats (v1 & v2)](#wow-entry-formats)
   - [Content & Locale Flags](#wow-flags)
   - [Parsing Flow](#wow-parsing-flow)
3. [Diablo 3 Root](#diablo-3-root)
   - [Directory Structure](#d3-directory-structure)
   - [Asset & Named Entries](#d3-entries)
   - [CoreTOC.dat](#d3-coretoc)
   - [Packages.dat](#d3-packages)
   - [Parsing Flow](#d3-parsing-flow)
4. [Diablo 4 / TVFS Root](#diablo-4--tvfs-root)
   - [TVFS Header](#tvfs-header)
   - [Path Table](#tvfs-path-table)
   - [VFS Table](#tvfs-vfs-table)
   - [Container File Table (CFT)](#tvfs-cft)
   - [Multi-Span Files](#tvfs-multi-span)
   - [Parsing Flow](#tvfs-parsing-flow)
5. [Format Comparison](#format-comparison)

---

## Common Primitives

All three root formats share a few low-level types defined in CascLib:

| Type | Size | Description |
|------|------|-------------|
| `CONTENT_KEY` (CKey) | 16 bytes | MD5 hash of the file's uncompressed content. |
| `ENCODED_KEY` (EKey) | 9 bytes (typical) | Truncated MD5 hash of the BLTE-encoded file header.  Length is storage-dependent. |
| `DWORD` | 4 bytes | 32-bit unsigned integer.  Little-endian unless noted. |
| `ULONGLONG` | 8 bytes | 64-bit unsigned integer. |

**Jenkins Hash** — WoW root entries can carry a 64-bit Jenkins "hashlittle2"
hash of the normalised (lower-case, back-slash) file path.  This allows
CascLib to resolve file names without a listfile.

---

## World of Warcraft Root

**Source:** `CascRootFile_WoW.cpp`  
**Signature:** `CASC_WOW_ROOT_SIGNATURE` (magic DWORD at offset 0, present
since build 30080).  
**Reference build:** WoW 6.0.3.19116 (original), with major revisions at
builds 30080 (8.2.0), 50893 (10.1.7), and 58221 (11.1.0).

### WoW Header Versions

CascLib tries three capture functions in order; the first that succeeds wins:

#### 1. `FILE_ROOT_HEADER_50893` — Build 50893+ (WoW 10.1.7)

```c
struct FILE_ROOT_HEADER_50893 {
    DWORD Signature;            // CASC_WOW_ROOT_SIGNATURE
    DWORD SizeOfHeader;         // byte length of this header (≥ 4)
    DWORD Version;              // must be 1 or 2
    DWORD TotalFiles;           // total CKey entries in the root
    DWORD FilesWithNameHash;    // entries that carry a name hash
};
// Actual data starts at offset SizeOfHeader (not at sizeof this struct).
```

When `Version == 2` (build 58221, WoW 11.1.0), root groups use an expanded
group header — see below.

#### 2. `FILE_ROOT_HEADER_30080` — Build 30080+ (WoW 8.2.0)

```c
struct FILE_ROOT_HEADER_30080 {
    DWORD Signature;            // CASC_WOW_ROOT_SIGNATURE
    DWORD TotalFiles;
    DWORD FilesWithNameHash;
};
```

All arrays use the **v2** split layout (CKeys and hashes in separate arrays).

#### 3. No header — Build 18125+ (WoW 6.0.1)

The oldest supported layout has **no header**.  The file begins directly with
the first `FILE_ROOT_GROUP_HEADER`.  All arrays use interleaved
`FILE_ROOT_ENTRY` records (the **v1** layout).

### WoW Group Layout

The root file is a flat sequence of **groups**.  Each group shares common
locale and content flags and contains a batch of file entries.

#### Group Header (Version 0 / 1)

```c
struct FILE_ROOT_GROUP_HEADER {
    DWORD NumberOfFiles;    // entries in this group
    DWORD ContentFlags;     // CASC_CFLAG_XXX bitmask
    DWORD LocaleFlags;      // CASC_LOCALE_XXX bitmask
};
```

#### Group Header (Version 2 — Build 58221+)

```c
#pragma pack(push, 1)
struct FILE_ROOT_GROUPHEADER_58221 {
    DWORD NumberOfFiles;
    DWORD LocaleFlags;
    DWORD ContentFlags1;
    DWORD ContentFlags2;
    BYTE  ContentFlags3;        // packed into bit 17+ of ContentFlags
};
#pragma pack(pop)
```

`ContentFlags` is reconstructed as
`ContentFlags1 | ContentFlags2 | (ContentFlags3 << 17)`.

### WoW Entry Formats

After each group header comes an array of delta-encoded **FileDataId** DWORDs
(one per file).  The real ID for entry *i* is the running sum of the first *i*
deltas.

#### v1 Layout (Build 18125 — 30079)

The FileDataId array is followed by an interleaved array:

```c
struct FILE_ROOT_ENTRY {
    CONTENT_KEY CKey;         // 16 bytes — MD5 of file content
    ULONGLONG   FileNameHash; // 8 bytes  — Jenkins hash of normalised path
};
// Total per entry: 24 bytes
```

#### v2 Layout (Build 30080+)

CKeys and name-hashes are stored as **two separate flat arrays**:

```
[CKey₀ .. CKeyₙ]                       // n × 16 bytes
[FileNameHash₀ .. FileNameHashₙ]        // n × 8 bytes  (OPTIONAL)
```

The name-hash array is **absent** when `ContentFlags & CASC_CFLAG_NO_NAME_HASH`
(`0x10000000`) is set.  The header field `FilesWithNameHash` allows the loader
to know how many entries lack hashes; those entries are looked up by
FileDataId only.

### WoW Flags

#### Locale Flags (`CASC_LOCALE_*`)

| Flag | Locale | Flag | Locale |
|------|--------|------|--------|
| `0x00000002` | enUS | `0x00000200` | enGB |
| `0x00000004` | koKR | `0x00000400` | enCN |
| `0x00000010` | frFR | `0x00000800` | enTW |
| `0x00000020` | deDE | `0x00001000` | esMX |
| `0x00000040` | zhCN | `0x00002000` | ruRU |
| `0x00000080` | esES | `0x00004000` | ptBR |
| `0x00000100` | zhTW | `0x00008000` | itIT |
|              |      | `0x00010000` | ptPT |

#### Content Flags (`CASC_CFLAG_*`)

| Flag | Meaning |
|------|---------|
| `0x04` | Install |
| `0x08` | Load on Windows |
| `0x10` | Load on Mac |
| `0x20` | x86-32 |
| `0x40` | x86-64 |
| `0x80` | Low-violence (Chinese alternate assets) |
| `0x100` | Don't load (skip this group) |
| `0x800` | Update plugin |
| `0x8000` | ARM64 |
| `0x8000000` | Encrypted |
| `0x10000000` | No-name-hash (v2 only — name-hash array is absent) |
| `0x20000000` | Uncommon resolution |
| `0x40000000` | Bundle |
| `0x80000000` | No compression |

### WoW Parsing Flow

```
1. Try CaptureRootHeader_50893, _30080, _18125 (in that order).
2. Determine RootFormat (v1 or v2) and RootVersion (0, 1, or 2).
3. Loop over the remaining bytes:
   a. CaptureRootGroup → read group header + FileDataId array + CKey/hash arrays.
   b. Filter by ContentFlags (skip DONT_LOAD, LOW_VIOLENCE, audio-locale mismatch).
   c. Filter by LocaleFlags (skip if non-zero and doesn't match mask).
   d. Insert each entry into the file tree (by hash+FileDataId or FileDataId only).
4. Repeat with bAudioLocale=1 for the audio locale pass.
5. If enGB requested, also load enUS as fallback; similarly ptPT → ptBR.
```

---

## Diablo 3 Root

**Source:** `CascRootFile_Diablo3.cpp`  
**Signature:** `CASC_DIABLO3_ROOT_SIGNATURE` (root directory header),
`DIABLO3_SUBDIR_SIGNATURE = 0xEAF1FE87` (sub-directory header).  
**Reference build:** Diablo III 2.2.0.30013 (32-bit).

Diablo 3's root is a **hierarchical directory system** with asset-based file
naming.  Unlike WoW, files are identified by a combination of **asset type
index** + **file index** (+ optional sub-index), not by FileDataId.

### D3 Directory Structure

The root file and each sub-directory share the same binary layout:

```
┌─────────────────────────────────────────────┐
│ Signature (4 bytes)                         │  0xAABB0002 for root,
│                                             │  0xEAF1FE87 for sub-dirs
├─────────────────────────────────────────────┤
│ [Sub-dirs only] Asset Entry section         │
│   DWORD  dwAssetEntries                     │
│   DIABLO3_ASSET_ENTRY[dwAssetEntries]       │
├─────────────────────────────────────────────┤
│ [Sub-dirs only] AssetIdx Entry section      │
│   DWORD  dwAssetIdxEntries                  │
│   DIABLO3_ASSETIDX_ENTRY[dwAssetIdxEntries] │
├─────────────────────────────────────────────┤
│ Named Entry section                         │
│   DWORD  dwNamedEntries                     │
│   (Variable-length named entries)           │
└─────────────────────────────────────────────┘
```

### D3 Entries

#### `DIABLO3_ASSET_ENTRY` — file by index

```c
struct DIABLO3_ASSET_ENTRY {     // 20 bytes
    CONTENT_KEY CKey;            // 16 bytes
    DWORD       FileIndex;       // asset file index
};
```

#### `DIABLO3_ASSETIDX_ENTRY` — file by index + sub-index

```c
struct DIABLO3_ASSETIDX_ENTRY {  // 24 bytes
    CONTENT_KEY CKey;            // 16 bytes
    DWORD       FileIndex;       // asset file index
    DWORD       SubIndex;        // sub-item number (e.g. "SoundBank\3D Ambience\0000.smp")
};
```

#### Named Entry — arbitrary file name

```
[CONTENT_KEY CKey]      // 16 bytes
[ASCIIZ FileName]       // variable-length, zero-terminated
```

Named entries appear in both the root directory (where they denote
sub-directories that must be recursively loaded) and in sub-directories
(where they are plain files with arbitrary names).

### D3 CoreTOC

The file `Base\CoreTOC.dat` maps `(AssetIndex, FileIndex)` pairs to
human-readable plain names.

```c
struct DIABLO3_CORE_TOC_HEADER {
    DWORD EntryCounts[70];    // file count per asset type (max 70 asset types)
    DWORD EntryOffsets[70];   // byte offset to each asset's entry array (relative to after header)
    DWORD Unknowns[70];
    DWORD Alignment;
};

struct DIABLO3_CORE_TOC_ENTRY {
    DWORD AssetIndex;         // which asset directory (0x00–0x45)
    DWORD FileIndex;          // file index within that asset
    DWORD NameOffset;         // offset to the ASCIIZ plain name (relative to after header)
};
```

Each `DIABLO3_ASSET_ENTRY`'s `FileIndex` is looked up in this table to
reconstruct the directory path and extension.

### D3 Asset Types

A static table maps asset index → directory name + extension.  A selection:

| Index | Directory | Ext | Index | Directory | Ext |
|-------|-----------|-----|-------|-----------|-----|
| 0x01 | Actor | .acr | 0x21 | Scene | .scn |
| 0x06 | Anim | .ani | 0x28 | Sound | .snd |
| 0x09 | Appearance | .app | 0x29 | SoundBank | .sbk |
| 0x14 | GameBalance | .gam | 0x2A | StringList | .stl |
| 0x1B | Particle | .prt | 0x2C | Textures | .tex |
| 0x1D | Power | .pow | 0x30 | Worlds | .wrl |
| 0x1F | Quest | .qst | 0x39 | Material | .mat |

Missing indices (e.g. 0x00, 0x03, 0x04) are unused/reserved.

### D3 Packages

`Base\Data_D3\PC\Misc\Packages.dat` supplies the **real file extensions** for
sub-items, which otherwise would receive the parent asset's default extension.

```
DWORD Signature;     // 0xAABB0002
DWORD NumberOfNames;
char  Names[];       // concatenated NUL-terminated strings
```

### D3 Parsing Flow

```
1. Verify root directory signature (CASC_DIABLO3_ROOT_SIGNATURE).
2. Parse root named entries (Phase 1):
   a. For each named entry, insert it into the file tree.
   b. If it's in the root directory, treat it as a sub-folder:
      load sub-folder data from CASC, parse it recursively.
3. Load Base\CoreTOC.dat → build FileIndex→(AssetIndex, PlainName) lookup array.
4. Load Base\Data_D3\PC\Misc\Packages.dat → build extension map.
5. Parse asset entries (Phase 2):
   a. For each sub-folder's DIABLO3_ASSET_ENTRY and DIABLO3_ASSETIDX_ENTRY,
      look up the plain name via CoreTOC.
   b. Look up the real extension via Packages.dat.
   c. Construct full path: "{SubFolder}/{AssetDir}/{PlainName}.{ext}"
   d. Insert into file tree by name.
```

---

## Diablo 4 / TVFS Root

**Source:** `CascRootFile_TVFS.cpp`  
**Full name:** TACT Virtual File System (TACT = Trusted Application Content
Transfer).  
**Signature:** `CASC_TVFS_ROOT_SIGNATURE` (magic DWORD at offset 0).  
**Used by:** Diablo 4, Call of Duty, Warcraft III: Reforged, Overwatch 2 (newer
builds), and other modern Blizzard/Activision titles.

TVFS is the most generic root format.  It represents a full virtual filesystem
with hierarchical path names encoded in a **prefix tree** (trie), referencing
files via **Encoding Keys** (EKeys) rather than CKeys.

### TVFS Header

```c
struct TVFS_DIRECTORY_HEADER {
    DWORD  Signature;          // CASC_TVFS_ROOT_SIGNATURE
    BYTE   FormatVersion;      // must be 1
    BYTE   HeaderSize;         // byte length of this header (≥ 8)
    BYTE   EKeySize;           // typically 9 bytes
    BYTE   PatchKeySize;       // typically 9 bytes
    DWORD  Flags;              // TVFS_FLAG_XXX (little-endian!)

    // Offset table (all big-endian):
    DWORD  PathTableOffset;    // offset to the path table
    DWORD  PathTableSize;      // byte length of the path table
    DWORD  VfsTableOffset;     // offset to the VFS table
    DWORD  VfsTableSize;       // byte length of the VFS table
    DWORD  CftTableOffset;     // offset to the Container File Table
    DWORD  CftTableSize;       // byte length of the CFT
    USHORT MaxDepth;           // maximum depth of the path prefix tree

    // Optional (only if TVFS_FLAG_WRITE_SUPPORT is set):
    DWORD  EstTableOffset;     // offset to the Encoding Specifier Table
    DWORD  EstTableSize;       // byte length of the EST
};
```

#### TVFS Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `TVFS_FLAG_INCLUDE_CKEY` | `0x0001` | Include CKey in content file records |
| `TVFS_FLAG_WRITE_SUPPORT` | `0x0002` | Include encoding specifier table |
| `TVFS_FLAG_PATCH_SUPPORT` | `0x0004` | Include patch records |
| `TVFS_FLAG_LOWERCASE_MANIFEST` | `0x0008` | All paths are ASCII lower-case |

### TVFS Path Table

The path table is a prefix tree encoded as a flat byte sequence.  Each node
consists of:

```
[0x00]          (optional) — path-separator BEFORE the name fragment
[length byte]   length of the name fragment (1 byte)
[name bytes]    the name fragment (variable)
[0x00]          (optional) — path-separator AFTER the name fragment
[0xFF]          node-value marker (optional)
[4 bytes]       NodeValue (big-endian, optional — only if 0xFF marker present)
```

The **NodeValue** determines whether the node is a folder or a file:

- **Folder** (`NodeValue & 0x80000000`): Lower 31 bits give the byte length of
  the child data immediately following.  The parser recurses into this region.
- **File** (high bit clear): NodeValue is an offset into the **VFS Table**.

A path like `data/archive/maps/file.bmp` might be encoded as the prefix tree:

```
data/
  arc
    hive/
      maps/
        file.bmp → NodeValue (VFS offset)
```

#### Special Case: WoW via TVFS

Since WoW build 45779, WoW can also ship TVFS roots with "generic" file names
encoding locale, content flags, FileDataId, and CKey in a single string:

```
LLLLLLLLCCCCCCCC:IIIIIIIIKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK
└──8 hex──┘└──8 hex──┘ └──8 hex──┘└────────32 hex────────┘
 Locale     Content      FileDataId        CKey
```

CascLib detects this pattern (string length 53 or 57 with `:` at position 12
or 16) and falls back to the normal WoW ROOT handler (`ERROR_REPARSE_ROOT`) if
enabled via `TVFS_PARSE_WOW_ROOT`.

### TVFS VFS Table

Each VFS entry describes one or more **spans** of data:

```
[1 byte]   SpanCount (1–224 = valid file, 255 = deleted)
For each span:
  [4 bytes]  FileOffset (big-endian) — offset within the logical file
  [4 bytes]  SpanLength (big-endian) — number of bytes in this span
  [? bytes]  CftOffset — offset into the Container File Table
             (1–4 bytes, length determined by CftTableSize)
```

Most files have `SpanCount == 1`.  Multi-span files (e.g. CoD's `zone/base.xpak`
with 22 spans totalling 15+ GB) are supported for archives larger than 4 GB.

### TVFS Container File Table (CFT)

Each CFT entry maps an EKey to a file:

```
[EKeySize bytes]  EKey — encoded key (typically 9 bytes)
[4 bytes]         EncodedSize (big-endian) — optional, depends on format
```

The EKey is used to look up the actual file data in the CASC ENCODING manifest,
which in turn resolves to a CKey and the file's physical location in
`data.###` archives.

### TVFS Multi-Span Files

When `SpanCount > 1`, the parser allocates an array of `CASC_CKEY_ENTRY`
structs — one per span — and resolves each span's EKey independently.  The
first span's entry stores the total `SpanCount`; subsequent spans are tagged
with `CASC_CE_FILE_SPAN`.  This allows transparent reading across archive
boundaries.

### TVFS Sub-Directories

A single-span file whose EKey appears in the storage's VFS root list is treated
as a **sub-directory**.  Its content is loaded, parsed as another
`TVFS_DIRECTORY_HEADER`, and its path table is recursively walked.  The
sub-directory's entries are prefixed with the parent path plus `:` (colon).

### TVFS Parsing Flow

```
1. Capture and validate the TVFS_DIRECTORY_HEADER (signature, version, key sizes).
2. Byte-swap all offset-table values from big-endian.
3. Compute CftOffsSize from CftTableSize (1/2/3/4 bytes).
4. Locate the root directory within the path table:
   a. If the first byte is 0xFF, read NodeValue → folder; compute data range.
   b. Otherwise, the path table IS the root directory.
5. Recursively walk the path table:
   a. For each node, capture name fragment and separators.
   b. If NodeValue has the folder bit set → recurse into child data.
   c. If NodeValue is a VFS offset → resolve spans:
      - SpanCount == 1: check if it's a sub-directory (VFS root list); if so,
        load and recurse with ':' separator.  Otherwise, insert as file.
      - SpanCount > 1: resolve all spans, tag as multi-span file.
6. Insert each resolved file into the file tree by path.
```

---

## Format Comparison

| Feature | WoW Root | Diablo 3 Root | TVFS (Diablo 4) |
|---------|----------|---------------|-----------------|
| **File identification** | FileDataId + optional Jenkins hash | AssetIndex + FileIndex (+ SubIndex) | Full path in prefix tree |
| **Key type stored** | CKey (16 bytes) | CKey (16 bytes) | EKey (9 bytes) |
| **Locale support** | Per-group locale flags bitmask | None (single locale) | None (path-based) |
| **Content flags** | Per-group content flags bitmask | None | None (except WoW-via-TVFS) |
| **Hierarchical** | Flat (groups only) | Two-level (root → sub-dirs) | Full tree (prefix trie) |
| **Max file size** | 4 GB (DWORD ContentSize) | 4 GB | >4 GB via multi-span |
| **External metadata** | Listfile (optional) | CoreTOC.dat + Packages.dat | None (self-contained) |
| **Header signature** | `CASC_WOW_ROOT_SIGNATURE` | `CASC_DIABLO3_ROOT_SIGNATURE` / `0xEAF1FE87` | `CASC_TVFS_ROOT_SIGNATURE` |
| **Endianness** | Little-endian | Little-endian | Mixed (header LE flags, offsets BE) |
| **Sub-directory support** | No | Yes (named entries point to sub-folders) | Yes (EKey in VFS root list) |

---

*Document generated from CascLib source analysis. Struct layouts and field
names match CascLib's C headers and may differ from Blizzard's internal naming.*
