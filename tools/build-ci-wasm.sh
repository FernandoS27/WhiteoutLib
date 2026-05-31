#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Unified WASM CI build — emscripten configure + build of both WASM
# targets, then `npm pack` each into a publishable .tgz.
#
# Outputs:
#   packages/js-ts/whiteout.{js,wasm}                     web runtime
#   packages/js-ts/whiteout-wasm-<ver>.tgz                web npm archive
#   packages/js-node/whiteout-node.{js,wasm,worker.js}    Node runtime
#   packages/js-node/whiteout-node-<ver>.tgz              Node npm archive
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
node_pkg_dir="${repo_root}/packages/js-node"
node_tpl_dir="${repo_root}/bindings/templates/js-node"
web_pkg_dir="${repo_root}/packages/js-ts"
web_tpl_dir="${repo_root}/bindings/templates/js-ts"
ts_types_dir="${repo_root}/packages/js-ts/types"

command -v emcmake >/dev/null || { echo "emcmake not on PATH -- activate emsdk first" >&2; exit 1; }
command -v cmake   >/dev/null || { echo "cmake not on PATH" >&2; exit 1; }
command -v npm     >/dev/null || { echo "npm not on PATH" >&2; exit 1; }

PYTHON="$(command -v python3 || command -v python)"
[ -n "${PYTHON}" ] || { echo "python not on PATH" >&2; exit 1; }

# ── 1. Stage the publishable layouts ─────────────────────────────────────
#
# Hand-written templates first (index.js, package.json, README, …). The
# compiled .js / .wasm + generated .d.ts files overwrite/augment them below.
rm -rf "${node_pkg_dir}" "${web_pkg_dir}"
mkdir -p "${node_pkg_dir}" "${web_pkg_dir}"
cp -R "${node_tpl_dir}"/. "${node_pkg_dir}/"
cp -R "${web_tpl_dir}"/.  "${web_pkg_dir}/"

# ── 2. Codegen — embind bindings + TypeScript ambient declarations ───────

cd "${repo_root}"
export PYTHONIOENCODING=utf-8
for mod in mdx m2 m3 textures mpq utils; do
    "${PYTHON}" -m tools.codegen.codegen "${mod}"
    "${PYTHON}" -m tools.codegen.codegen "${mod}" --backend dts
done

# ── 3. Configure ─────────────────────────────────────────────────────────
#
# Both targets share the configure call. `whiteout_wasm_node` is opt-in via
# WHITEOUT_BUILD_WASM_NODE_BINDINGS=ON; the plain `whiteout_wasm` (web) is
# always built when WHITEOUT_BUILD_WASM_BINDINGS=ON.

emcmake cmake -S "${repo_root}" -B "${build_dir}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.15 \
    -DWHITEOUT_BUILD_WASM_BINDINGS=ON \
    -DWHITEOUT_BUILD_WASM_NODE_BINDINGS=ON \
    -DWHITEOUT_ENABLE_MPQ=ON \
    -DWHITEOUT_WASM_NODE_PTHREAD_POOL_SIZE=8

# ── 4. Build (both targets) ──────────────────────────────────────────────
#
# Web variant (whiteout_wasm) and Node variant (whiteout_wasm_node) share
# source files but link with different emscripten flags (pthread, NODERAWFS,
# pthread-aware `_mt` archives). Their `.o` files don't overlap because of
# the `_mt` ABI split, so this roughly doubles wall time vs Node-only.

parallel="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
cmake --build "${build_dir}" \
    --target whiteout_wasm whiteout_wasm_node \
    --parallel "${parallel}"

# ── 5. Stage compiled artefacts into the package dirs ────────────────────

dist="${build_dir}/wasm-dist"

# Node package.
[ -f "${dist}/whiteout-node.wasm" ] || {
    echo "expected ${dist}/whiteout-node.wasm not produced" >&2; exit 1; }
cp "${dist}/whiteout-node.js"   "${node_pkg_dir}/"
cp "${dist}/whiteout-node.wasm" "${node_pkg_dir}/"
# pthreads worker — only present when pool size > 0.
[ -f "${dist}/whiteout-node.worker.js" ] && \
    cp "${dist}/whiteout-node.worker.js" "${node_pkg_dir}/"

# Web package.
[ -f "${dist}/whiteout.wasm" ] || {
    echo "expected ${dist}/whiteout.wasm not produced" >&2; exit 1; }
cp "${dist}/whiteout.js"   "${web_pkg_dir}/"
cp "${dist}/whiteout.wasm" "${web_pkg_dir}/"

# Mirror generated .d.ts into both packages (canonical home is js-ts/types).
# _shared.d.ts is hand-written per-target — keep each template's copy.
mkdir -p "${node_pkg_dir}/types"
if [ -d "${ts_types_dir}" ]; then
    for f in "${ts_types_dir}"/*.d.ts; do
        [ -f "$f" ] || continue
        case "$(basename "$f")" in
            _shared.d.ts) ;;  # don't overwrite the per-target shim
            *) cp "$f" "${node_pkg_dir}/types/" ;;
        esac
    done
fi

# ── 6. npm pack each → whiteout-{wasm,node}-<ver>.tgz ────────────────────
#
# Produces a publishable archive in each package dir; appveyor.yml's artifacts
# rules glob *.tgz under packages/js-node/ and packages/js-ts/.

(cd "${node_pkg_dir}" && npm pack)
(cd "${web_pkg_dir}"  && npm pack)

echo "── build-ci-wasm summary ──"
echo "node package : ${node_pkg_dir}"
ls -lah "${node_pkg_dir}"/*.tgz "${node_pkg_dir}"/whiteout-node.* 2>/dev/null || true
echo "web package  : ${web_pkg_dir}"
ls -lah "${web_pkg_dir}"/*.tgz "${web_pkg_dir}"/whiteout.* 2>/dev/null || true
