# whiteout

Python bindings for [WhiteoutLib](https://github.com/Sahmkow/WhiteoutLib) —
pure-C++ parsers and writers for Blizzard model, texture, and archive
formats.

| Domain    | Formats / surface                                                                |
| --------- | -------------------------------------------------------------------------------- |
| Models    | MDX / MDL (Warcraft III), M2 (WoW), M3 (StarCraft II / HotS)                     |
| Textures  | BLP, DDS, PNG, JPEG, BMP, TGA, GIF (write), APNG                                 |
| Archives  | MPQ (read + write), CASC (read, local or online)                                 |
| Host extras | `OsFileSystem` (disk I/O), `SimpleThreadPool`, subclassable `HttpHandler`      |
| Math      | `Vector{2,3,4}f`, `Quaternion`, `Matrix33f`, `Matrix44f` — NumPy-friendly        |

The wheel is a single self-contained extension module (`whiteout.pyd` on
Windows, `.so` on Linux / macOS). No native deps to install — everything
is statically linked.

---

## Install

```bash
pip install whiteout
```

The package ships PEP 561 type stubs (`whiteout-stubs/`), so PyCharm,
VS Code / Pyright, and mypy get full autocomplete + type checking on
every generated class, enum, and constant.

---

## Quick start

```python
import whiteout as w

# ── MDX (Warcraft III) ──────────────────────────────────────────────────
# Field names are snake_case in Python (camelCase in C++); enums are
# UPPER_SNAKE_CASE.
parser = w.mdx.Parser()
model = parser.parse_buffer_format(open("model.mdx", "rb").read(),
                                    w.mdx.MDLXFormat.MDX)
print(f"{model.model_name} v{model.version}: {len(model.geosets)} geosets")
if parser.has_issues():
    for issue in parser.get_issues():
        print(f"  warning: {issue}")

# Encode as MDL text (HiveWorkshop dialect):
mdl_bytes = w.mdx.Writer().write_mdx_format_mdl_format(
    model, w.mdx.MDLXFormat.MDL, w.mdx.MdlFormat.HIVEWORKSHOP,
)

# ── M2 (WoW) — multi-file via in-memory FS or disk ──────────────────────
fs = w.InMemoryFileSystem()
fs.add_file("models/character.m2",     open("character.m2",     "rb").read())
fs.add_file("models/character00.skin", open("character00.skin", "rb").read())
m2_model = w.m2.Parser().parse(fs, "models/character.m2")

# Real-disk variant (uses the host extras):
disk_fs = w.host.OsFileSystem("C:/path/to/models")
m2_model = w.m2.Parser().parse(disk_fs, "character.m2")

# ── M3 (StarCraft II / HotS) ────────────────────────────────────────────
m3_model = w.m3.Parser().parse(open("model.m3", "rb").read())

# ── Textures ────────────────────────────────────────────────────────────
tex = w.BlpParser().parse(open("texture.blp", "rb").read())
print(f"{tex.width()}x{tex.height()}")
png_bytes = w.PngWriter().write(tex)

# ── Math types: positional or keyword, pretty-printed ──────────────────
v = w.Vector3f(1.0, 2.0, 3.0)
q = w.Quaternion(x=0, y=0, z=0, w=1)
print(repr(v))                  # Vector3f(x=1, y=2, z=3)
```

The library is **exception-free**: a corrupt file produces a best-effort
result plus issues on `parser.get_issues()`, not a Python exception.

---

## NumPy zero-copy access

Vectors of primitive types (`u8`/`u16`/`u32`/`f32`/…) and the standard
math types (`Vector2f`, `Vector3f`, `Vector4f`, `Quaternion`, `ColorBGRA`)
expose the buffer protocol. `np.asarray(vec)` returns a view that shares
memory with the underlying C++ `std::vector` — **no copy**, mutations
write through.

```python
import numpy as np

# 1D buffer for primitive vectors: shape = (N,)
keys = np.asarray(track.keys)             # dtype=float32
keys[:] *= 2                              # writes through to C++

# 2D buffer for math-struct vectors: shape = (N, components)
pivots   = np.asarray(model.pivot_points) # shape=(N, 3), dtype=float32
tangents = np.asarray(geo.tangents)       # shape=(N, 4), dtype=float32
```

**Caveat — resize invalidates the view.** `append` / `extend` / `clear`
may reallocate the C++ buffer, in which case the previously-acquired
numpy view points at freed memory. Re-acquire (`np.asarray(vec)`) after
any size-changing operation.

---

## Archives (MPQ + CASC)

```python
import whiteout as w

# ── MPQ (Warcraft III, WoW Classic) ─────────────────────────────────────
mpq = w.mpq.Storage.open("war3.mpq")
try:
    data = mpq.read_file("textures/character/footman.blp")
    tex = w.BlpParser().parse(data)
finally:
    mpq.close()

# ── CASC (WoW retail, Diablo, Overwatch) — local install ────────────────
casc = w.casc.Storage.open_local("/path/to/Game/Data")
try:
    print(f"{casc.file_count()} files indexed")
    data = casc.read_file_by_name("World/Maps/Azeroth/Azeroth_31_46.adt")
finally:
    casc.close()

# ── CASC — online (CDN-backed, opt-in HTTP handler) ─────────────────────
http = w.host.SimpleHttpHandler()           # uses WinHTTP / libcurl
casc = w.casc.Storage.open_online("wow", http)
```

`open_local` / `open_online` accept an optional `WorkerPool` for parallel
BLTE decoding:

```python
pool = w.host.SimpleThreadPool(8)
casc = w.casc.Storage.open_local("/path/to/Game/Data", pool)
```

---

## Threaded texture pipelines

```python
import whiteout as w

pool = w.host.SimpleThreadPool(8)
tex  = w.PngParser().parse(open("source.png", "rb").read())

# Mipmap generation, BC7 encode — both fan out across the pool.
tex.generate_mipmaps_pool(pool)
tex.format(w.PixelFormat.BC7, pool)         # in-place transcode
open("out.dds", "wb").write(w.DdsWriter().write(tex))
```

Without a pool the library still runs the same operations on the calling
thread — slower but correct.

---

## Types & stubs

The wheel ships `whiteout-stubs/` (PEP 561 marker file `py.typed`). Every
class, enum, field, and method has a typed declaration:

```
whiteout-stubs/
├── __init__.pyi      whiteout.* (math types, format facades, InMemoryFileSystem)
├── mdx.pyi           whiteout.mdx
├── m2.pyi            whiteout.m2
├── m3.pyi            whiteout.m3
├── textures.pyi      whiteout.textures (low-level Texture API)
├── mpq.pyi
├── casc.pyi
├── host.pyi          OsFileSystem, SimpleThreadPool, SimpleHttpHandler
└── utils.pyi         VertexBuffer, VertexBufferBuilder, math helpers
```

A type-checker that walks `import whiteout` will resolve everything
through these stubs without loading the native extension.

---

## Building from source

Requires Python ≥ 3.10 with the `clang` package (`pip install clang`)
plus a C++ 20 toolchain (MSVC 2022 / Clang 16+ / GCC 13+) and CMake.

```pwsh
# From repo root
.\scripts\build-python.ps1
```

The script:

1. Regenerates `bindings/python/*_bindings.cpp` via the codegen for every
   module (`mdx`, `m2`, `m3`, `textures`, `mpq`, `utils`, `host`, `casc`).
2. Regenerates the matching `.pyi` stubs into `packages/python/whiteout-stubs/`.
3. Configures cmake (Release) and builds the extension.
4. Stages `whiteout.<abi>.pyd` into `packages/python/`.

Codegen-only (no native rebuild):

```pwsh
$env:Path = "C:\Projects\WhiteoutLib\.venv\Scripts;$env:Path"
foreach ($mod in 'mdx','m2','m3','textures','mpq','utils','host','casc') {
    python -m tools.codegen.codegen $mod --backend pybind11
    python -m tools.codegen.codegen $mod --backend pyi
}
```

---

## Layout

```
bindings/python/
├── CMakeLists.txt          pybind11_add_module target.
├── module.cpp              Hand-written umbrella: PYBIND11_MODULE, math
│                           types, InMemoryFileSystem, submodule wiring.
├── *_bindings.cpp          AUTOGENERATED by `tools/codegen/emit_pybind.py`.
└── README.md               (this file)
```

The `*_bindings.cpp` files carry an `AUTOGENERATED` banner — change the
C++ headers (or `tools/codegen/modules/`) and re-run codegen rather than
hand-editing.

---

## Memory & exception model

- **No exceptions** from the C++ side. The library is built with
  `-fno-exceptions`; parsing or writing a corrupt input never throws.
  Issues are surfaced through `parser.has_issues()` / `get_issues()`.
- **Automatic lifetime** — pybind11 manages ownership; you don't have
  to call `.delete()` like in the WASM bindings.
- **Buffer views are not stable across resize.** See the NumPy caveat
  above — re-acquire the view after any operation that may reallocate.
- **Native exceptions in Python** — programming errors that *do* abort
  (out-of-range access on a custom optional, etc.) surface as
  `RuntimeError`, not a clean Python exception, because the library is
  exception-free internally. This is rare in normal API use.

---

## Troubleshooting

| Symptom                                                  | Cause / fix                                                                                              |
| -------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `ImportError: DLL load failed` (Windows)                 | Missing VC++ 2022 runtime. Install [Microsoft VC++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe). |
| `AttributeError: module 'whiteout' has no attribute 'X'` | Stub / runtime drift. Reinstall (`pip install --force-reinstall whiteout`) to align stubs and `.pyd`.   |
| `RuntimeError: Cannot register type 'X' twice`           | Two binding TUs registered the same type. Hits only source builds — re-run codegen and rebuild.          |
| NumPy view returns garbage after `.append(...)`          | The C++ vector reallocated and the view points at freed memory. Re-acquire with `np.asarray(vec)`.       |
| Subclassing `HttpHandler` in Python crashes              | The trampoline keeps Python alive on the C++ side — make sure the handler isn't garbage-collected while CASC is still using it. |

---

## See also

- [`packages/python/`](../../packages/python/) — the built extension + stubs that ship in the wheel.
- [`bindings/wasm/README.md`](../wasm/README.md) — the browser / Node.js sibling bindings.
- [`tests/python/`](../../tests/python/) — pytest suite covering every binding category.
- [`tools/codegen/README.md`](../../tools/codegen/README.md) — how the `*_bindings.cpp` and `.pyi` files are generated.

## License

BSD-3-Clause
