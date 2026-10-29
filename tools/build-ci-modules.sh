#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# One module configuration of WhiteoutLib (WHITEOUTLIB_MODULES_DESIGN.md §8.3):
# the library alone with its tests ON, so every test the switches leave in has
# to link against the modules it names, and the switch paths themselves
# configure. Nothing is packaged and the corpus tests are not run; the module
# graph check is.
#
#   tools/build-ci-modules.sh -DWHITEOUT_WEM_EDIT=OFF
#   tools/build-ci-modules.sh -DWHITEOUT_ENABLE_MODELS=OFF -DWHITEOUT_WEM_CONVERT=OFF -DWHITEOUT_WEM_EDIT=OFF
#
# On Linux this is also the GNU ld build: it resolves archives in link-line
# order, so a module linked in the wrong order or a cycle between two modules
# fails here when the Windows and Apple linkers let it pass.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${WHITEOUT_BUILD_DIR:-${repo_root}/build-modules}"

cmake -S "${repo_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWHITEOUT_BUILD_TESTS=ON \
    -DWHITEOUT_WARNINGS_AS_ERRORS=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.15 \
    "$@"
cmake --build "${build_dir}" --config Release --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
ctest --test-dir "${build_dir}" -C Release -R '^module_graph$' --output-on-failure
