# whiteout

Python bindings for [WhiteoutLib](https://github.com/Sahmkow/WhiteoutLib) —
pure-C++ parsers and writers for Blizzard model and texture formats.

**Scope:** models (MDX, M2, M3) and textures (BLP, DDS, PNG, JPEG, BMP,
TGA). Storage backends (CASC, MPQ) and CDN/networking are out of scope.

## Install

```bash
pip install whiteout
```

## Quick start

```python
import whiteout as w

# MDX (Warcraft III) -- snake_case fields, UPPER_SNAKE_CASE constants
mdl = w.mdx.Parser().parse(open("model.mdx", "rb").read())
print(f"{mdl.model_name} v{mdl.version}: {len(mdl.geosets)} geosets")
re_encoded = w.mdx.Writer().write(mdl)

# M2 (WoW) -- multi-file via in-memory FS
fs = w.InMemoryFileSystem()
fs.add_file("models/character.m2",     open("character.m2",     "rb").read())
fs.add_file("models/character00.skin", open("character00.skin", "rb").read())
m2 = w.m2.Parser().parse(fs, "models/character.m2")

# M3 (StarCraft II / HotS)
m3 = w.m3.Parser().parse(open("model.m3", "rb").read())

# Textures -- format facades at root for ergonomics
tex = w.BlpParser(w.BlpParseMode.LENIENT).parse(open("texture.blp", "rb").read())
print(f"{tex.width()}x{tex.height()}")
png_bytes = w.PngWriter().write(tex)

# Math types take positional or keyword args, and pretty-print
v = w.Vector3f(1.0, 2.0, 3.0)
q = w.Quaternion(x=0, y=0, z=0, w=1)
print(repr(v))                  # Vector3f(x=1, y=2, z=3)
```

## NumPy zero-copy access

Vectors of primitive types (`u8`/`u16`/`u32`/`f32`/...) and the standard
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
pivots = np.asarray(model.pivot_points)   # shape=(N, 3), dtype=float32
tangents = np.asarray(geo.tangents)       # shape=(N, 4), dtype=float32
```

**Caveat — resize invalidates the view.** `append`/`extend`/`clear` may
reallocate the C++ buffer, in which case the previously-acquired numpy
view points at freed memory. Re-acquire (`np.asarray(vec)`) after any
size-changing operation.

## Type checking

The wheel ships PEP 561 type stubs (`whiteout-stubs/`). PyCharm,
VSCode/Pyright, and mypy pick them up automatically — autocomplete and
type checking work for every generated class, enum, and constant.

## What's not included

- CASC / MPQ archive reading
- HTTP / CDN fetching
- Multithreading (BC7 encode is single-threaded; slow but correct)

## License

BSD-3-Clause
