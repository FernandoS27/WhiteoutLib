# whiteout-node

Node.js-only WhiteoutLib build. Same parser/writer surface as
[`whiteout-wasm`](../js-ts/README.md) but compiled with:

- **`-sNODERAWFS=1`** — `std::ifstream` / `std::filesystem` calls hit the
  host disk directly; no `Module.FS.mount()` choreography required.
- **`-pthread`** — Emscripten pthreads back `std::thread`, which under
  Node maps to `worker_threads`. Lets `Texture::generateMipmaps`,
  BC7 encode, and CASC/MPQ decode run multi-threaded.
- **CASC opt-in** (set `-DWHITEOUT_ENABLE_CASC=ON` at build time).

If you don't need disk I/O or threading, prefer
[`whiteout-wasm`](../js-ts/README.md) — it's smaller, has no
`SharedArrayBuffer` requirement, and runs in browsers.

## Install

```bash
npm install whiteout-node
```

Requires Node 18+ (Atomics / SharedArrayBuffer enabled by default).

## Quick start

```js
import { Whiteout } from "whiteout-node";

const whiteout = await Whiteout();

// ── Disk I/O via OsFileSystem ───────────────────────────────────────────
// M2 (and any future parser that takes a VirtualPathFileSystem) can read
// straight from disk — no need to slurp every sibling file into a Record.
// The Node build talks to whiteout's VirtualPathFileSystem interface
// directly via OsFileSystem; there is no in-memory shim in this build.
const model = whiteout.m2.parse(
    "C:/Games/Wow/Data/character/human",
    "character.m2",
);
model.delete();

// ── Threaded mipmap / BC7 generation ────────────────────────────────────
const pool = whiteout.threadPool(8);
try {
    const tex = whiteout.png.parse(pngBytes);
    // Generates the full mip chain across all 8 worker_threads.
    whiteout.textureGenerateMipmaps(tex, { pool });
    pool.waitIdle();
    tex.delete();
} finally {
    pool.delete();
}

// ── MPQ on disk with optional parallel decompression ────────────────────
const archive = whiteout.mpq.open("War3.mpq", pool);
if (archive) {
    const data = archive.readFile("war3map.j");
    archive.delete();
}

// ── Online CASC via your own HTTP client ────────────────────────────────
// `complete` MUST be called exactly once per request.
const handler = whiteout.makeHttpHandler({
    async getAsync(url, complete) {
        const r = await fetch(url);
        complete({ statusCode: r.status,
                   body: new Uint8Array(await r.arrayBuffer()) });
    },
    async getRangeAsync(url, start, end, complete) {
        const r = await fetch(url, { headers: { Range: `bytes=${start}-${end}` } });
        complete({ statusCode: r.status,
                   body: new Uint8Array(await r.arrayBuffer()) });
    },
});
// handler is passed to whiteout.casc.openOnline({...}) once that surface
// is wired up (CASC online options are not yet exposed via this build).
handler.delete();
```

## Threading model

`whiteout.threadPool(n)` is a `utils::SimpleThreadPool` bound through
Embind. The hosting WASM binary was linked with a fixed number of
pre-spawned `worker_threads` (default 8, configurable via the CMake
cache variable `WHITEOUT_WASM_NODE_PTHREAD_POOL_SIZE`). Asking for
more workers than that is legal but blocks until additional pthreads
are created.

Threads share the WASM linear memory through `SharedArrayBuffer`.
Pointers passed between threads remain valid as long as the C++ object
lives — but **Embind `.delete()` MUST be called from the main thread**;
calling it from a worker is undefined behaviour.

## File I/O model

`OsFileSystem` is constructed with a root path; subsequent
`readFile("relative/path")` calls resolve `root / relative/path`
and read via `std::ifstream`. Because the binary was linked with
`-sNODERAWFS=1`, that maps directly to Node's `fs` syscalls — no
intermediate copy through Emscripten's MEMFS.

Paths may use either `/` or `\` separators; they are normalised on
the C++ side.

## What's not included (vs. whiteout-wasm)

Everything in `whiteout-wasm` is also here. Additions only.

## What's still not implemented

- `casc.openOnline({...})` — the online CASC OpenOptions struct is not
  bound yet; only local-disk CASC is exposed. Once the bindings ship,
  pair it with `makeHttpHandler` for full CDN access.
- `blizzard_game_finder` (Windows registry) — out of scope for WASM.
- Pool-driven parallelism inside MDX/M2/M3 parsers — the parsers are
  already CPU-light; only texture and archive paths benefit today.

## License

BSD-3-Clause
