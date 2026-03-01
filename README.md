# WhiteoutLib

WhiteoutLib is a C++ library for reading and writing 3D model formats used in Blizzard Entertainment games.
It is inspired by StormLib, a library used by older Blizzard games to handle proprietary file formats like BLP, MDX, and MPQ.

## Current Support

- **Warcraft III (`.mdx`)**
  - Classic
  - Reforged
- **World of Warcraft (`.m2`)**

## Planned Support

- **StarCraft II / Heroes of the Storm (`.m3/.m3a`)**
- **Diablo III (`.app`, appearance format)**

## Format References

### MDX

- https://www.hiveworkshop.com/threads/mdx-specifications.240487/
- https://github.com/stijnherfst/HiveWE

### M2

- https://wowdev.wiki/M2

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
