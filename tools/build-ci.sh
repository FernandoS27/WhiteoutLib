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

command -v cmake >/dev/null || { echo "cmake not found on PATH" >&2; exit 1; }

# Pick a Python ≥3.10 (cmake's `find_package(Python 3.10 REQUIRED)` in
# bindings/python/CMakeLists.txt enforces that). The CI install step writes
# the resolved interpreter to ci_python.env so this script, cmake, and the
# downstream after_build step all use the same one — important on macOS,
# where the first `python3` on PATH is Xcode's 3.9 but cmake finds brew's
# 3.12 (different interpreter, different site-packages, different pybind11).
PYTHON="${CI_PYTHON:-}"
if [ -z "${PYTHON}" ]; then
    # Pin Python 3.12. The published wheels must share a single cp tag
    # across all platform jobs (cp312-cp312-{platform}) so PyPI can serve
    # the right wheel for any consumer. If 3.12 isn't on PATH, fall back
    # to the highest 3.x but warn loudly — a mismatched cp tag means the
    # wheel collation step ships an inconsistent matrix.
    for cand in python3.12 python3.13 python3.11 python3.10 python3 python; do
        if command -v "$cand" >/dev/null 2>&1; then
            if "$cand" -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null; then
                PYTHON="$(command -v "$cand")"
                break
            fi
        fi
    done
fi
[ -n "${PYTHON}" ] || { echo "no python >=3.10 on PATH" >&2; exit 1; }
echo "using python: ${PYTHON} ($(${PYTHON} --version))"

# ── 1. Codegen ───────────────────────────────────────────────────────────
#
# AppVeyor CI runs codegen in a dedicated job and ships the result as a
# shared artifact; per-OS jobs download + extract it, then set
# `WHITEOUT_SKIP_CODEGEN=1` so this section becomes a no-op. The flag
# guarantees every OS compiles byte-identical generated sources.

modules=(textures mdx m2 m3 utils host mpq casc)

cd "${repo_root}"
export PYTHONIOENCODING=utf-8

# Surface the picked Python to subprocesses so cmake's find_package(Python)
# (and the codegen call inside bindings/python/CMakeLists.txt when
# WHITEOUT_BUILD_PYTHON_WHEEL=ON) inherit it via the env.
export Python_EXECUTABLE="${PYTHON}"

# Sentinel file alternative to WHITEOUT_SKIP_CODEGEN — keeps the build
# script in sync with build-ci.ps1, where the appveyor CLI isn't reliably
# on PATH for cross-step env var persistence. CI's poller drops the
# `.codegen-done` marker after extracting the codegen tarball.
if [ -f "${repo_root}/.codegen-done" ] && [ "${WHITEOUT_SKIP_CODEGEN:-}" != "1" ]; then
    WHITEOUT_SKIP_CODEGEN=1
fi

if [ "${WHITEOUT_SKIP_CODEGEN:-}" = "1" ]; then
    echo "WHITEOUT_SKIP_CODEGEN=1 — skipping codegen step (using pre-generated sources)"
else
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
fi

# ── 2. Configure ─────────────────────────────────────────────────────────

cmake -S "${repo_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPython_EXECUTABLE="${PYTHON}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
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
