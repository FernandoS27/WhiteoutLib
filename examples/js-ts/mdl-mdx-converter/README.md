# MDX ↔ MDL Converter (browser, WASM + TypeScript)

A minimal Vite + TypeScript app that converts Warcraft III models between
binary `.mdx` and text `.mdl`, entirely client-side via the
[WhiteoutLib](https://github.com/FernandoSahmkow/WhiteoutLib) WASM bindings.

Drop a file → pick the MDL dialect → download the result. Nothing leaves
the browser.

![screenshot placeholder — the UI is a dark drop-zone with a status line and download link]

---

## Prerequisites

1. Node.js ≥ 20 (any recent LTS).
2. A built `whiteout-wasm` package. From the repo root:
   ```pwsh
   pwsh scripts/build-wasm.ps1
   ```
   This produces `packages/js-ts/whiteout.{js,wasm}` alongside the
   hand-written facade and type declarations. The example's
   `package.json` already points to that local path via:
   ```json
   "dependencies": { "whiteout-wasm": "file:../../../packages/js-ts" }
   ```

## Run it

From this folder (`examples/js-ts/mdl-mdx-converter/`):

```sh
npm install
npm run dev
```

Open the URL Vite prints (usually `http://localhost:5173/`). Drop or pick
a `.mdx` or `.mdl` file — the matching output appears as a download
link.

## Build a static bundle

```sh
npm run build
npm run preview        # serve the bundle locally
```

The build emits a self-contained `dist/` directory you can deploy to any
static host (`dist/index.html`, the bundled JS, and `whiteout.wasm` as
an asset). No server-side runtime is needed.

## What it shows

- **Loading the WASM module.** `Whiteout()` returns a promise; the
  example awaits it once and reuses the resolved instance for every
  conversion.
- **The lower-level `mdx.Parser` / `mdx.Writer` classes.** The example
  uses these directly (rather than the `whiteout.mdx.parse(bytes)`
  facade) so it can surface `parser.getIssues()` — a list of recoverable
  problems the lenient parser collected. The library is exception-free
  after v0.2, so a corrupt file produces a best-effort `Model` plus
  issues, not an exception.
- **MDL dialect selection.** When converting `.mdx → .mdl`, choose
  between `MdlFormat.WarcraftIII` (engine-faithful, default) and
  `MdlFormat.Hiveworkshop` (HiveWorkshop community tooling). The dialect
  only affects MDL text output.
- **Embind memory hygiene.** Every native handle (`Parser`, `Writer`,
  `Model`, and the `std::vector<u8>` returned by `Writer.write_*`) is
  released in a `try/finally`. `vecToBytes` copies the vector's contents
  out before releasing the proxy so the resulting `Uint8Array` is safe
  to put in a Blob/URL.

## File map

```
examples/js-ts/mdl-mdx-converter/
├── index.html          UI skeleton (drop zone, dialect picker, status, download).
├── styles.css          Standalone CSS — dark surface, no framework.
├── src/main.ts         Module: factory + DOM wiring + Parser/Writer round-trip.
├── package.json        Vite + TS devdeps; whiteout-wasm via file: link.
├── tsconfig.json       Strict TS targeting the bundler resolver.
├── vite.config.ts      Excludes whiteout-wasm from pre-bundling so its
│                       `.wasm` sibling resolves via import.meta.url.
└── README.md           This file.
```

## Troubleshooting

- **`Cannot find module 'whiteout-wasm'` in your editor.** That's the
  pre-install state — run `npm install` and reload the editor's TS
  server.
- **`Failed to load whiteout.wasm` at runtime.** The `whiteout-wasm`
  package wasn't built yet — run `pwsh scripts/build-wasm.ps1` from the
  repo root, then re-install (`npm install`) so Vite picks up the
  freshly-staged `.wasm` from `packages/js-ts/`.
- **Browser shows "Conversion failed".** The status line will quote the
  error; for parse-level problems also expand the *Parser issues* panel
  for the list of warnings the lenient parser collected.
