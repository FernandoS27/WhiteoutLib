// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

import { defineConfig } from 'vite';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
// whiteout-wasm is installed via `file:../../../packages/js-ts`, so its
// `.wasm` sibling lives outside this example's project root. Vite blocks
// `/@fs/` reads outside the project root by default — explicitly allow
// the WhiteoutLib repo root so the dev server can serve whiteout.wasm.
const repoRoot = resolve(here, '../../..');

export default defineConfig({
    server: {
        fs: {
            // Allow serving files from the repo root (covers `packages/js-ts/`
            // where whiteout.wasm lives).
            allow: [here, repoRoot],
        },
        // Cross-origin isolation isn't required for the web build (no
        // pthreads / SharedArrayBuffer), but setting these headers keeps
        // future-proofs the example for the Node-thread WASM target.
        headers: {
            'Cross-Origin-Opener-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
        },
    },

    // whiteout-wasm ships its own .js loader that dynamically resolves
    // the sibling .wasm via `import.meta.url`. Keeping it out of Vite's
    // dep optimizer preserves that relative resolution.
    optimizeDeps: {
        exclude: ['whiteout-wasm'],
    },

    // Make sure the .wasm file in node_modules is bundled as an asset.
    assetsInclude: ['**/*.wasm'],
});
