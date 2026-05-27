# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Unified CI build — one cmake configure + one cmake build that produces:
#
#   build-ci/c-dist/Release/whiteout_native.dll       Java FFM native
#   build-ci/python-dist/whiteout.cp<ver>-<plat>.pyd  pybind11 module
#   build-ci/Release/<lib + examples>                 C++ static lib + examples
#
# Counterpart to tools/build-ci.sh (Linux/macOS). Splitting Windows and
# Unix scripts keeps the cmake generator handling readable; everything else
# is identical between them.
#
# The packaging steps (Maven natives jar, Python wheel) run after this and
# *consume* what this script produced — no second compile.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build-ci"

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    throw "python not found on PATH"
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not found on PATH"
}

# Resolve a Python ≥3.10 — same selection logic as build-ci.sh, so codegen
# and cmake's find_package(Python) end up using the same interpreter (and
# therefore the same pybind11 / libclang install).
$pythonCandidates = @('python3.13', 'python3.12', 'python3.11', 'python3.10', 'python3', 'python')
$pythonExe = $null
foreach ($cand in $pythonCandidates) {
    $cmd = Get-Command $cand -ErrorAction SilentlyContinue
    if (-not $cmd) { continue }
    $ok = & $cmd.Path -c "import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)" 2>$null
    if ($LASTEXITCODE -eq 0) { $pythonExe = $cmd.Path; break }
}
if (-not $pythonExe) { throw "no python >=3.10 on PATH" }
Write-Host "using python: $pythonExe ($(& $pythonExe --version))"

# ── 1. Codegen ───────────────────────────────────────────────────────────
#
# Generates the per-binding source/header files. Runs once; the same
# outputs feed Java FFM, Python pybind11, and the .pyi stubs.

$modules = @('textures', 'mdx', 'm2', 'm3', 'utils', 'host', 'mpq', 'casc')

Push-Location $repoRoot
try {
    $env:PYTHONIOENCODING = "utf-8"

    # C ABI (consumed by the JNI/FFM layer).
    & $pythonExe -m tools.codegen.codegen textures --backend c-common-header
    if ($LASTEXITCODE -ne 0) { throw "codegen c-common-header failed" }
    & $pythonExe -m tools.codegen.codegen textures --backend c-common
    if ($LASTEXITCODE -ne 0) { throw "codegen c-common failed" }
    foreach ($mod in $modules) {
        foreach ($backend in @('c-header', 'c-source')) {
            & $pythonExe -m tools.codegen.codegen $mod --backend $backend
            if ($LASTEXITCODE -ne 0) { throw "codegen $mod $backend failed" }
        }
    }

    # Java FFM (whiteout.common + per-module).
    & $pythonExe -m tools.codegen.codegen textures --backend java-common
    if ($LASTEXITCODE -ne 0) { throw "codegen java-common failed" }
    foreach ($mod in $modules) {
        & $pythonExe -m tools.codegen.codegen $mod --backend java
        if ($LASTEXITCODE -ne 0) { throw "codegen $mod java failed" }
    }

    # Python pybind11 + .pyi stubs.
    foreach ($mod in $modules) {
        & $pythonExe -m tools.codegen.codegen $mod --backend pybind11
        if ($LASTEXITCODE -ne 0) { throw "codegen $mod pybind11 failed" }
        & $pythonExe -m tools.codegen.codegen $mod --backend pyi
        if ($LASTEXITCODE -ne 0) { throw "codegen $mod pyi failed" }
    }
} finally {
    Pop-Location
}

# ── 2. Configure ─────────────────────────────────────────────────────────
#
# Single configure with every binding enabled. The C++ static lib + the
# JNI/FFM native library + the pybind11 module all link against the same
# object files — compiling them in one tree avoids the double-compile that
# you'd hit by running build-java.ps1 and build-python.ps1 sequentially.

$cmakeArgs = @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DPython_EXECUTABLE=$pythonExe",
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    "-DWHITEOUT_BUILD_C_BINDINGS=ON",
    "-DWHITEOUT_BUILD_JNI_BINDINGS=ON",
    "-DWHITEOUT_BUILD_PYTHON_BINDINGS=ON",
    "-DWHITEOUT_BUILD_PYTHON_WHEEL=ON",   # enables install() rules used by pack_python_wheel.py
    "-DWHITEOUT_BUILD_EXAMPLES=ON",
    "-DWHITEOUT_BUILD_TESTS=OFF",
    "-DWHITEOUT_ENABLE_MPQ=ON",
    "-DWHITEOUT_ENABLE_CASC=ON",
    "-DWHITEOUT_WARNINGS_AS_ERRORS=ON",
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.15"
)
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

# ── 3. Build ─────────────────────────────────────────────────────────────

$parallel = if ($env:CMAKE_BUILD_PARALLEL_LEVEL) { $env:CMAKE_BUILD_PARALLEL_LEVEL } else { 4 }
& cmake --build $buildDir --config Release --parallel $parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

# ── 4. Report outputs ────────────────────────────────────────────────────
#
# Surfaces the paths that downstream packaging steps need. AppVeyor scrapes
# the build log for these to populate the artifact uploads.

$nativeDll = Get-ChildItem (Join-Path $buildDir "c-dist") -Recurse -Filter "whiteout_native.dll" `
    | Select-Object -First 1
$pyExt = Get-ChildItem (Join-Path $buildDir "python-dist") -Recurse -Include "*.pyd","*.so" `
    | Select-Object -First 1

Write-Host "── build-ci summary ──"
Write-Host "build dir       : $buildDir"
Write-Host "java native     : $($nativeDll.FullName)"
Write-Host "python extension: $($pyExt.FullName)"
