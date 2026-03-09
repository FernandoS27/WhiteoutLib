# WhiteoutLib

WhiteoutLib is a C++ library for reading and writing 3D model & texture formats used in Blizzard Entertainment games.
It is inspired by StormLib, a library used by older Blizzard games to handle proprietary file formats like BLP, MDX, and MPQ. 

## Current 3D Format Support

- **Warcraft III (`.mdx`)**
  - Classic
  - Reforged
- **World of Warcraft (`.m2`)** (partial, still has issues with .anim and .skel files)
- **StarCraft II / Heroes of the Storm (`.m3/.m3a`)**

## Planned Support

- **Diablo III & IV (`.acr,.app,.ani,.ans,.mat,.prt`, format family)**

## Current Texture Format Support

- **Warcraft III & World of Warcraft (`.blp`)**
    Both BLP1 (Warcraft III Classic) & BLP2 (World of Warcraft)
- **Direct3D Surface (`.dds`)**
- **Diablo 3 (`.tex`)**

## Language Compatibility

The library internals use C++20, but all public headers are C++11 compatible to make it easier to create bindings for other languages.

## Format References

look in `docs/`

## Build

This project uses CMake.

```bash
cmake -S . -B build
cmake --build build --config Release
```

## Examples

Example programs are available in `examples/` for loading and writing supported formats.

## License

BSD 3-Clause. See [LICENSE](LICENSE).

## Disclaimer

This project is not affiliated with or endorsed by Blizzard Entertainment.
