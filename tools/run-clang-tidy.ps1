# Run clang-tidy across every whiteout source file in compile_commands.json.
#
# Usage:
#   pwsh tools/run-clang-tidy.ps1 [-BuildDir build/clang] [-Path 'src/whiteout/storages']
#
# -BuildDir : CMake build dir containing compile_commands.json. Default: build/clang.
# -Path     : Restrict to files whose path matches this regex. Default: all whiteout sources.
# -Fix      : Apply suggested fixes in place (-fix). Off by default.
# -Jobs     : Parallel jobs (run-clang-tidy -j). Default: number of CPUs.
#
# Notes:
#   - Requires clang-tidy (and ideally run-clang-tidy) on PATH.
#   - Reads .clang-tidy at repo root for check selection.

[CmdletBinding()]
param(
    [string]$BuildDir = "build/clang",
    [string]$Path     = "(include|src)[/\\]whiteout[/\\]",
    [switch]$Fix,
    [int]$Jobs        = 0
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$compileDb = Join-Path $BuildDir "compile_commands.json"
if (-not (Test-Path $compileDb)) {
    Write-Error "compile_commands.json not found at $compileDb. Run cmake to configure first."
}

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

$runner = Get-Command run-clang-tidy -ErrorAction SilentlyContinue
$tidy   = Get-Command clang-tidy     -ErrorAction SilentlyContinue
if (-not $tidy) { Write-Error "clang-tidy not found on PATH." }

$headerFilter = '(include|src)[/\\]whiteout[/\\]'

if ($runner) {
    $args = @("-p", $BuildDir, "-j", $Jobs, "-header-filter=$headerFilter", "-quiet")
    if ($Fix) { $args += "-fix" }
    $args += $Path
    & $runner.Source @args
} else {
    # Fallback: walk compile_commands.json ourselves.
    $entries = Get-Content $compileDb -Raw | ConvertFrom-Json
    $files   = $entries | Where-Object { $_.file -match $Path } | ForEach-Object { $_.file }
    Write-Host "Found $($files.Count) files to check (no run-clang-tidy, sequential mode)."
    foreach ($f in $files) {
        $args = @("-p", $BuildDir, "--header-filter=$headerFilter", "--quiet")
        if ($Fix) { $args += "--fix" }
        $args += $f
        & $tidy.Source @args
    }
}
