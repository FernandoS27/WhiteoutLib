#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Unified WASM CI build — emscripten configure + build of the Node.js
# WASM bindings, then `npm pack` to produce a publishable .tgz.
#
# Output:
#   packages/js-node/whiteout-node.{js,wasm,worker.js}    runtime
#   packages/js-node/whiteout-node-<ver>.tgz              publishable archive
#
# Linux-only (AppVeyor's emscripten install path is bash-centric; the local
# dev path on Windows uses scripts/build-wasm-node.ps1 with the user's own
# emsdk install).
#
# Requires (set up by the caller — appveyor.yml's `for:` install block):
#   - emsdk activated:           `source emsdk_env.sh`  (puts emcc on PATH)
#   - Node.js + npm on PATH
#   - Python 3 + pip-installed libclang (for codegen)

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")"/.. && pwd)"
build_dir="${repo_root}/build-wasm-node"
pkg_dir="${repo_root}/packages/js-node"
tpl_dir="${repo_root}/bindings/templates/js-node"
ts_types_dir="${repo_root}/packages/js-ts/types"

command -v emcmake >/dev/null || { echo "emcmake not on PATH -- activate emsdk first" >&2; exit 1; }
command -v cmake   >/dev/null || { echo "cmake not on PATH" >&2; exit 1; }
command -v npm     >/dev/null || { echo "npm not on PATH" >&2; exit 1; }

PYTHON="$(command -v python3 || command -v python)"
[ -n "${PYTHON}" ] || { echo "python not on PATH" >&2; exit 1; }

# ── 1. Stage the publishable layout ──────────────────────────────────────
#
# Hand-written templates first (index.js, package.json, README, …). The
# compiled .js / .wasm + generated .d.ts files overwrite/augment them below.
rm -rf "${pkg_dir}"
mkdir -p "${pkg_dir}"
cp -R "${tpl_dir}"/. "${pkg_dir}/"

# ── 2. Codegen — embind bindings + TypeScript ambient declarations ───────

cd "${repo_root}"
export PYTHONIOENCODING=utf-8
for mod in mdx m2 m3 textures mpq utils; do
    "${PYTHON}" -m tools.codegen.codegen "${mod}"
    "${PYTHON}" -m tools.codegen.codegen "${mod}" --backend dts
done

# ── 3. Configure ─────────────────────────────────────────────────────────
#
# Only the Node WASM target is built — the web variant uses a different
# emscripten link line and isn't needed for the npm package.

emcmake cmake -S "${repo_root}" -B "${build_dir}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.15 \
    -DWHITEOUT_BUILD_WASM_BINDINGS=ON \
    -DWHITEOUT_BUILD_WASM_NODE_BINDINGS=ON \
    -DWHITEOUT_ENABLE_MPQ=ON \
    -DWHITEOUT_WASM_NODE_PTHREAD_POOL_SIZE=8

# ── 4. Build (Node target only) ──────────────────────────────────────────

parallel="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
cmake --build "${build_dir}" --target whiteout_wasm_node --parallel "${parallel}"

# ── 5. Stage compiled artefacts into the package dir ─────────────────────

dist="${build_dir}/wasm-dist"
[ -f "${dist}/whiteout-node.wasm" ] || {
    echo "expected ${dist}/whiteout-node.wasm not produced" >&2; exit 1; }

cp "${dist}/whiteout-node.js"   "${pkg_dir}/"
cp "${dist}/whiteout-node.wasm" "${pkg_dir}/"
# pthreads worker — only present when pool size > 0.
[ -f "${dist}/whiteout-node.worker.js" ] && cp "${dist}/whiteout-node.worker.js" "${pkg_dir}/"

# Mirror generated .d.ts into the Node package (canonical home is js-ts/types).
# _shared.d.ts is hand-written per-target — keep the Node template's copy.
mkdir -p "${pkg_dir}/types"
if [ -d "${ts_types_dir}" ]; then
    for f in "${ts_types_dir}"/*.d.ts; do
        [ -f "$f" ] || continue
        case "$(basename "$f")" in
            _shared.d.ts) ;;  # don't overwrite the Node-specific shim
            *) cp "$f" "${pkg_dir}/types/" ;;
        esac
    done
fi

# ── 6. npm pack → whiteout-node-<ver>.tgz ────────────────────────────────
#
# Produces the publishable archive in the same dir; appveyor.yml's artifacts
# rule globs *.tgz under packages/js-node/.

cd "${pkg_dir}"
npm pack

echo "── build-ci-wasm summary ──"
echo "package dir : ${pkg_dir}"
ls -lah "${pkg_dir}"/*.tgz "${pkg_dir}"/whiteout-node.* 2>/dev/null || true
