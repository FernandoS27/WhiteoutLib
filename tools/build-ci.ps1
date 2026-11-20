# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Unified CI build — one cmake configure + one cmake build that produces:
#
#   build-ci/c-dist/whiteout_native.dll               Java FFM native
#   build-ci/python-dist/whiteout.cp<ver>-<plat>.pyd  pybind11 module
#   build-ci/<lib + examples>                         C++ static lib + examples
#
# Counterpart to tools/build-ci.sh (Linux/macOS). Both use Ninja; this one
# first enters the Visual Studio developer environment for cl and ninja.
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
# Pin Python 3.12 across every CI worker. The published wheels share a
# single cp tag (cp312-cp312-{platform}) so PyPI can serve the right one
# to consumers regardless of host platform.
$pythonCandidates = @('python3.12', 'python3.13', 'python3.11', 'python3.10', 'python3', 'python')
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
#
# AppVeyor CI runs codegen in a dedicated job and ships the result as a
# shared artifact; per-OS jobs download + extract it, then set
# `WHITEOUT_SKIP_CODEGEN=1` so this section becomes a no-op. The flag
# guarantees every OS compiles byte-identical generated sources.

$modules = @('textures', 'mdx', 'm2', 'm3', 'utils', 'host', 'mpq', 'casc')

# Sentinel file alternative to WHITEOUT_SKIP_CODEGEN. AppVeyor's `ps:`
# step on Windows doesn't expose the `appveyor SetVariable` CLI in the
# pwsh PATH, so cross-step env var persistence via the CLI is unreliable;
# the CI poller drops `.codegen-done` after extracting the codegen tarball.
$codegenSentinel = Join-Path $repoRoot ".codegen-done"
if ((Test-Path $codegenSentinel) -and ($env:WHITEOUT_SKIP_CODEGEN -ne "1")) {
    $env:WHITEOUT_SKIP_CODEGEN = "1"
}

if ($env:WHITEOUT_SKIP_CODEGEN -eq "1") {
    Write-Host "WHITEOUT_SKIP_CODEGEN=1 — skipping codegen step (using pre-generated sources)"
} else {
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
}

# ── 2. Configure ─────────────────────────────────────────────────────────
#
# Single configure with every binding enabled. The C++ static lib + the
# JNI/FFM native library + the pybind11 module all link against the same
# object files — compiling them in one tree avoids the double-compile that
# you'd hit by running build-java.ps1 and build-python.ps1 sequentially.
#
# Ninja, not the Visual Studio generator: MSBuild builds the libraries as a
# chain of projects, so the two-core worker compiled one file at a time for
# half an hour. Ninja schedules every file of every target together.

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "no Visual Studio with the x64 C++ tools found" }
Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { throw "ninja not found in the VS environment" }

# clang-cl, with cl's ABI, runtime and linker: on the pybind11 TUs cl takes
# about five times as long (wem_bindings.cpp: 254 s at /O1 against 52 s at
# /O2 on one core), which no setting of cl brings inside the 60 minutes.
function Find-ClangCl {
    @(
        (Join-Path $vsPath "VC\Tools\Llvm\x64\bin\clang-cl.exe"),
        (Join-Path $env:ProgramFiles "LLVM\bin\clang-cl.exe"),
        (Get-Command clang-cl -ErrorAction SilentlyContinue).Source
    ) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
$clangCl = Find-ClangCl
if (-not $clangCl -and $env:APPVEYOR) {
    Write-Host "clang-cl not on the image; installing LLVM"
    & choco install llvm -y --no-progress | Write-Host
    $clangCl = Find-ClangCl
}
if ($clangCl) {
    Write-Host "compiler: $clangCl"
    $compiler = $clangCl
} else {
    Write-Warning "clang-cl not found; building with cl, which the AppVeyor job cap does not fit"
    $compiler = "cl"
}

$cmakeArgs = @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=$compiler",
    "-DCMAKE_CXX_COMPILER=$compiler",
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
    # Defined, so pybind11 adds no /GL + /LTCG of its own: the LTCG of the
    # Python module alone took 20 of AppVeyor's 60 minutes.
    "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF",
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.15"
)
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

# ── 3. Build ─────────────────────────────────────────────────────────────
#
# Two jobs for the two-vCPU worker; that also keeps the pybind11 TUs (~2 GB
# peak apiece) inside its 8 GB.

$parallel = if ($env:CMAKE_BUILD_PARALLEL_LEVEL) { $env:CMAKE_BUILD_PARALLEL_LEVEL } else { 2 }
& cmake --build $buildDir --parallel $parallel
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
