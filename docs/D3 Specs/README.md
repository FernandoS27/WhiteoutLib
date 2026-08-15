# Diablo III SNO Format Specifications

Index and methodology for the `.acr` / `.ani` / `.app` / … format specifications in this
folder. **Read this before trusting or editing any of them** — it records what each document
was derived from, which are current, and which are not.

---

## 1. What everything here is derived from

One binary, one corpus, one metadata dump.

| | |
| --- | --- |
| **Build** | Diablo III **Nintendo Switch 2.6.2**, `DiabloIIINX64r` |
| **File** | `exefs/main` (ARM64) |
| **sha256** | `9f0cb00de2ae3c1bd05757d100e834d5480bb56d51aa664103058a7c7006c115` |
| **IDB** | `D:\ReverseEngineer\Diablo\d3\exefs\main.i64` |
| **Corpus** | `Corpus/D3/<Group>/` — 87k+ assets across 16 directories |

The Switch build ships the game's **own type-reflection metadata**: a table of every SNO
structure with per-field offsets, types, defaults and array descriptors. That table is the
authority for layout, and it is why the 2026-08-15 documents supersede everything derived
before them.

**Two hard limits on that metadata, both of which shape every document here:**

1. **Field NAMES are stripped.** **5,038 of 5,069** field registrations pass a pointer to the
   same empty string, so the metadata gives a field's offset, type and default but almost
   never its name. (For `AnimPermutation` it is all 45 of 45.) *Type* names do survive as real
   strings, which is why every structure here has its shipped name and few fields do. A field
   name in these documents therefore comes from an engine function that reads it, from a
   corpus-verified relation, or it is explicitly marked a guess.
2. **The metadata describes a NEWER revision than the shipped data.** Each group registers a
   version array whose first entry is the version the shipped assets actually use (v0) and
   whose "current" entry is the revision the compiled structs describe. Where these differ,
   the on-disk layout differs too — see §4. **A matching root struct size does not mean the
   layouts agree.**

Anything a document asserts should be backed by a cited engine function address, a registered
field entry, or an explicit corpus count. Claims without one of those are marked as guesses.

---

## 2. The pipeline

```
 IDA (Switch 2.6.2)
   ├─ scripts/ida/d3_export_reflection.py ──► data/d3_sno_reflection.json   (offsets/types/defaults)
   └─ scripts/ida/d3_export_group_versions.py ► data/d3_group_versions.json (group id, size, versions)
 hand-verified corrections
   └─ scripts/seed_d3_type_overrides.py ───► data/d3_type_overrides.json    (names + v0 layouts)
                                             ▲ this is where RE findings are written down
 generators
   ├─ scripts/gen_d3_sno_defs.py ──────────► src/whiteout/sno/d3/sno_defs.{h,cpp}
   │                                          the reflection-driven registry (generic reader)
   └─ scripts/gen_d3_native.py ────────────► include/whiteout/sno/d3/native/types.h   (public)
                                             include/whiteout/sno/d3/native/d3_native.h (public)
                                             src/whiteout/sno/d3/native/layout.h        (private)
                                             src/whiteout/sno/d3/native/binary_parse_visitor/*
```

`data/d3_type_overrides.json` and both generated layers are **build artefacts — never edit
them by hand.** Change `scripts/seed_d3_type_overrides.py` and re-run:

```sh
python scripts/seed_d3_type_overrides.py    # -> data/d3_type_overrides.json
python scripts/gen_d3_sno_defs.py           # -> the reflection registry
python scripts/gen_d3_native.py             # -> the native/public layer
```

All three are idempotent: a second run is byte-identical. `gen_d3_native.py` also prunes types
that nothing can reach and prints what it dropped — check each dropped type is a group with no
native entry point, not a field that lost its type.

There are two independent readers on purpose:

* the **reflection registry** (`sno_defs.cpp`) walks the registered type table generically;
* the **native layer** (`types.h` + the visitors) is hand-shaped C++ structs.

Corpus tests run claims through **both**, so a green suite means the registered type table
really describes the format rather than one bespoke reader agreeing with itself.

---

## 3. Verification gates

| Test | Covers | Scale |
| --- | --- | --- |
| `d3_graphics_sno_test` | 13 groups through the generic reader | every group with a corpus |
| `d3_payload_test` | payload/array bounds across 9 groups | 2,948 files, 135,241 arrays |
| `d3_app_corpus_test` | Appearance, every structure | 11,347 / 11,347 |
| `d3_ani_corpus_test` | Anim via the **reflection** path | 15,258 files, 11,556,940 rotation keys |
| `d3_native_test` | Anim / AnimSet / AnimTree / Appearance via the **native** path | 92 assertions |

A structural claim that does not reach 100 % on its gate is not treated as a fact in these
documents.

---

## 4. Status of each specification

`data ver` is the version **measured in every corpus file** (not taken from the registration);
`reg` is the revision the binary's compiled structs describe. **Where they differ the layouts
differ**, and the document must carry a version-skew table (ANI §14 is the worked example).
`struct` is the size the binary registers, except where an override supersedes it for the
shipped version — the two ANI-style cases are marked.

| Spec | Group | id | data ver | reg | struct | corpus | Derivation |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| [ACR](ACR_FILE_FORMAT_SPECIFICATION.md) | Actor | 1 | 282 | 288 | 880 (reg 448) | 19,177 | reflection, 2026-08-15 |
| [ANI](ANI_FILE_FORMAT_SPECIFICATION.md) | Anim | 6 | 118 | 129 | 56 | 15,258 | reflection + engine fns, pass 4 |
| [ANS](ANS_FILE_FORMAT_SPECIFICATION.md) | AnimSet | 8 | 24 | 28 | 480 | 3,212 | reflection, 2026-08-15 |
| [APP](APP_FILE_FORMAT_SPECIFICATION.md) | Appearance | 9 | 260 | 306 | 552 | 11,347 | reflection, 2026-08-15 |
| [CLT](CLT_FILE_FORMAT_SPECIFICATION.md) | Cloth | 11 | 51 | 52 | 100 | 74 | reflection, 2026-08-16 |
| [EFG](EFG_FILE_FORMAT_SPECIFICATION.md) | EffectGroup | 14 | 47 | 51 | 120 | 6,426 | reflection, 2026-08-15 |
| [PRT](PRT_FILE_FORMAT_SPECIFICATION.md) | Particle | 27 | 180 | 213 | 2,296 (reg 704) | 21,593 | reflection + engine fns, 2026-08-15 |
| [D3_PHYSICS](D3_PHYSICS_FORMAT_SPECIFICATION.md) | Physics ∪ Cloth ∪ PhysMesh | 28/11/61 | 37/51/24 | 38/52/26 | 68/100/48 | 2,848 | reflection, 2026-08-16 |
| [SHM](SHM_FILE_FORMAT_SPECIFICATION.md) | ShaderMap | 36 | 26 | 31 | 32 | 1,192 | reflection, 2026-08-15 |
| [SHD](SHD_FILE_FORMAT_SPECIFICATION.md) | Shaders | 37 | 150 | 187 | 296 | **1 usable** | reflection, 2026-08-16; n=1 |
| [TEX](TEX_FILE_FORMAT_SPECIFICATION.md) | Textures | 44 | — | 66 | 632 | **0** | **bytes-only, NOT re-derived** |
| [MAT](MAT_FILE_FORMAT_SPECIFICATION.md) | Material | 57 | 25 | 37 | 136 | 3,843 | reflection, 2026-08-15 |
| [PHM](PHM_FILE_FORMAT_SPECIFICATION.md) | PhysMesh | 61 | 24 | 26 | 48 | 2,700 | reflection, 2026-08-15 |
| [ANT](ANT_FILE_FORMAT_SPECIFICATION.md) | AnimTree | 67 | 30 | 31 | 96 | **1** | reflection, 2026-08-15; element size predicted |

Every group's corpus is **version-uniform** — one version value across every file, magic
`0xDEADBEEF` in all of them. One caveat worth knowing:

> **Actor is the only group whose data version is not the registered array's first entry.**
> The array reads `{281, 292, 287, 287}` with current 288, but all 19,177 `.acr` files are
> **v282**. So `versions[0]` is a good default for "the shipped version" but is not a
> substitute for measuring — check the corpus before relying on it.

### Where the gaps are

* **TEX is the one document not re-derived.** There is no `Corpus/D3/Textures` directory, so
  nothing in it can be corpus-checked, and no override entry exists. The binary registers
  `Textures` at **632 bytes with 27 fields** and a v0→reg skew of 47→66, which is large. Treat
  the current TEX document as pre-reflection and verify against the registered table before
  relying on it.
* **SHD and ANT have corpora too small to gate them.** `Corpus/D3/Shaders` holds 2 files but
  **only one carries the `0xDEADBEEF` magic** — the other is not an SNO asset — so SHD rests
  on a single sample. ANT has exactly one file (`Axe Bad Data.ant`, 148 bytes) and its
  `AnimTreeNode` element size is predicted, not measured.
* **Two corpora have no specification at all**: `CompiledShader` (14,518 files) and
  `OpenGLShaders` (7,236). Neither is an SNO group in the sense above.
* Open items are listed per document under "Known Unknowns". The general reason a field stays
  unnamed is §1's two limits: if no engine function reads it, and the metadata has no name for
  it, only the corpus is left — which fixes *behaviour* (ANI §4.1) but rarely a *name*.

---

## 5. What changed on 2026-08-16

The pass that produced this README. Four documents were behind the reflection work and are now
caught up, and one recurring class of error was found and swept for:

* **The "oversized preamble" misread.** Several documents predating the reflection metadata
  described a 32- or 48-byte "SNO preamble". There is no such thing — a SNO file is a
  **16-byte header plus the struct**, and everything stored inside is struct-relative. Where a
  document treated the struct's own first 16 bytes as part of the header, **every field name it
  assigned was displaced by one slot**, while every value census it recorded stayed correct.
  That is what made the error survive so long: the numbers all checked out.
  - **[CLT](CLT_FILE_FORMAT_SPECIFICATION.md) was rewritten** — essentially every field was
    misnamed. The old `gravity` was `flMass`; the old `windDirectionBias` was the real
    `flGravity`; the old `sentinel = 0xFFFFFFFF` was an unused `snoAmbientSound` SNO reference.
    Its Appendix A hex dump had also been fabricated and is replaced with a real one.
  - **[D3_PHYSICS](D3_PHYSICS_FORMAT_SPECIFICATION.md)**: `.phy` corrected the same way, its
    `.clt` section reduced to a summary that defers to CLT, and its Domino↔M3 mapping tables
    marked where the pairings no longer follow.
  - **[SHD](SHD_FILE_FORMAT_SPECIFICATION.md)**: reframed from a "48-byte preamble with
    repurposed fields" to the ordinary header + 296-byte struct with a standard count +
    `SerializeData` array idiom.
  - **[ACR](ACR_FILE_FORMAT_SPECIFICATION.md), [ANT](ANT_FILE_FORMAT_SPECIFICATION.md) and
    [MAT](MAT_FILE_FORMAT_SPECIFICATION.md)** use the same "preamble" wording but their offsets
    are file-relative and **correct**; each now carries a note saying so, so the wording cannot
    mislead a future reader into repeating the shift.
* **[TEX](TEX_FILE_FORMAT_SPECIFICATION.md) is flagged as not re-derived** — no corpus exists
  in this tree and the v47→rev66 skew is the largest of any group. The registered field table
  and a predicted v47 size are recorded so the work can be picked up.
* Every specification now states its **SNO group id** and the **registered revision** beside
  the shipped version, and links here.

---

## 6. Conventions used throughout

* **All stored offsets are struct-relative.** File position = `16 + offset`, because the
  16-byte SNO file header precedes the struct image. Every document repeats this; it is the
  single most common source of an off-by-16 misread.
* **A variable array on disk** is `{SerializeData, pointer}` = `{u32 offsetFromStruct, u32
  byteSize}` + 8 bytes of runtime pointer (zero on disk). Element size is
  `SerializeData.byteSize / count`, measured over the corpus rather than assumed.
* **Runtime pointer fields are zero on disk** and are shown as `_ptr` / `_runtimePtr`.
* Field-name prefixes follow the game's own Hungarian style: `dw` u32, `n` i32, `fl` f32,
  `v` vector, `sz` inline string, `ar` array, `sd` SerializeData, `sno` SNO reference,
  `t` nested struct.
* A field named `_unknownNNN` / `flUnknownNNN` has an established offset and type but no
  established meaning. That is deliberate: a plausible-sounding wrong name is worse than none.

---

## 7. Related documents

* [`../SNO_FILE_FORMAT_SPECIFICATION.md`](../SNO_FILE_FORMAT_SPECIFICATION.md) — the shared SNO
  container (header, magic, group ids) across Diablo III and IV.
* [`../D4 Specs/`](../D4%20Specs/) — the Diablo IV formats, which share the container but not
  the type tables.
* [`../../data/README_d3_sno.md`](../../data/README_d3_sno.md) — what each JSON in `data/` is
  and how it is produced.
