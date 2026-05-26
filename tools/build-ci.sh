#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Unified CI build — one cmake configure + one cmake build that produces:
#
#   build-ci/c-dist/libwhiteout_native.{so,dylib}      Java FFM native
#   build-ci/python-dist/whiteout.cp<ver>-<plat>.so    pybind11 module
#   build-ci/<lib + examples>                          C++ static lib + examples
#
# Counterpart to tools/build-ci.ps1 (Windows). Identical configure flags;
# different cmake generator (Unix Makefiles vs Visual Studio).

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")"/.. && pwd)"
build_dir="${repo_root}/build-ci"

command -v python3 >/dev/null || command -v python >/dev/null || {
    echo "python not found on PATH" >&2; exit 1; }
command -v cmake >/dev/null || { echo "cmake not found on PATH" >&2; exit 1; }

# Prefer python3 if both are available (Linux default).
PYTHON="$(command -v python3 || command -v python)"

# ── 1. Codegen ───────────────────────────────────────────────────────────

modules=(textures mdx m2 m3 utils host mpq casc)

cd "${repo_root}"
export PYTHONIOENCODING=utf-8

# C ABI.
"${PYTHON}" -m tools.codegen.codegen textures --backend c-common-header
"${PYTHON}" -m tools.codegen.codegen textures --backend c-common
for mod in "${modules[@]}"; do
    for backend in c-header c-source; do
        "${PYTHON}" -m tools.codegen.codegen "${mod}" --backend "${backend}"
    done
done

# Java FFM.
"${PYTHON}" -m tools.codegen.codegen textures --backend java-common
for mod in "${modules[@]}"; do
    "${PYTHON}" -m tools.codegen.codegen "${mod}" --backend java
done

# Python pybind11 + .pyi stubs.
for mod in "${modules[@]}"; do
    "${PYTHON}" -m tools.codegen.codegen "${mod}" --backend pybind11
    "${PYTHON}" -m tools.codegen.codegen "${mod}" --backend pyi
done

# ── 2. Configure ─────────────────────────────────────────────────────────

cmake -S "${repo_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWHITEOUT_BUILD_C_BINDINGS=ON \
    -DWHITEOUT_BUILD_JNI_BINDINGS=ON \
    -DWHITEOUT_BUILD_PYTHON_BINDINGS=ON \
    -DWHITEOUT_BUILD_PYTHON_WHEEL=ON \
    -DWHITEOUT_BUILD_EXAMPLES=ON \
    -DWHITEOUT_BUILD_TESTS=OFF \
    -DWHITEOUT_ENABLE_MPQ=ON \
    -DWHITEOUT_ENABLE_CASC=ON \
    -DWHITEOUT_WARNINGS_AS_ERRORS=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.15

# ── 3. Build ─────────────────────────────────────────────────────────────

parallel="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
cmake --build "${build_dir}" --config Release --parallel "${parallel}"

# ── 4. Report outputs ────────────────────────────────────────────────────

case "$(uname -s)" in
    Darwin) native_lib_pat="libwhiteout_native.dylib" ;;
    *)      native_lib_pat="libwhiteout_native.so"    ;;
esac
native_lib="$(find "${build_dir}/c-dist" -name "${native_lib_pat}" 2>/dev/null | head -n1 || true)"
py_ext="$(find "${build_dir}/python-dist" -name 'whiteout*.so' 2>/dev/null | head -n1 || true)"

echo "── build-ci summary ──"
echo "build dir       : ${build_dir}"
echo "java native     : ${native_lib:-<not found>}"
echo "python extension: ${py_ext:-<not found>}"
