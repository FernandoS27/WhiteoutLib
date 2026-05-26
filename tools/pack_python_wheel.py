# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Pack a Python wheel from an existing CMake build.
#
# Why this exists: scikit-build-core (driven by `python -m build`) configures
# CMake in an isolated PEP-517 temp dir and recompiles everything from
# scratch. In CI we already do one unified cmake build per platform that
# produces the pybind11 module alongside the rest of the library — re-running
# the full C++ compile inside a build-isolation env wastes 5–10 minutes per
# job.
#
# This script bypasses scikit-build entirely:
#
#   1. `cmake --install <build-dir>` into a staging dir — picks up the
#      install() rules in bindings/python/CMakeLists.txt that already lay
#      out the wheel content (whiteout.cpXY-<plat>.{pyd,so} + whiteout-stubs/).
#   2. Write the `<name>-<version>.dist-info/` metadata (METADATA, WHEEL,
#      LICENSE) read from pyproject.toml.
#   3. Invoke `python -m wheel pack` to compute RECORD and assemble the .whl.
#
# Output: a single wheel in `--out-dir`, tagged for the running Python
# interpreter and host platform (cp3X-cp3X-{win_amd64,linux_x86_64,…}).
#
# Usage:
#   python tools/pack_python_wheel.py \
#       --build-dir build-ci \
#       --out-dir   packages/python/dist
#
# Requires: cmake on PATH, `wheel` Python package (`pip install wheel`).

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import sysconfig
import tempfile
from pathlib import Path

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:
    import tomli as tomllib  # type: ignore[no-redef]


# ── pyproject.toml helpers ────────────────────────────────────────────────


def load_pyproject(repo_root: Path) -> dict:
    with (repo_root / "pyproject.toml").open("rb") as fp:
        return tomllib.load(fp)


def metadata_2_1_lines(project: dict, readme_path: Path | None) -> list[str]:
    """Build METADATA per Core Metadata 2.1 (PEP 566)."""
    name = project["name"]
    version = project["version"]
    lines: list[str] = [
        "Metadata-Version: 2.1",
        f"Name: {name}",
        f"Version: {version}",
    ]

    summary = project.get("description")
    if summary:
        lines.append(f"Summary: {summary}")

    for author in project.get("authors", []):
        if isinstance(author, dict):
            n, e = author.get("name"), author.get("email")
            if n and e:
                lines.append(f"Author-email: {n} <{e}>")
            elif n:
                lines.append(f"Author: {n}")

    if project.get("license"):
        lic = project["license"]
        if isinstance(lic, str):
            lines.append(f"License-Expression: {lic}")

    for url_name, url in project.get("urls", {}).items():
        lines.append(f"Project-URL: {url_name}, {url}")

    keywords = project.get("keywords", [])
    if keywords:
        # Single comma-separated line per PEP 314 / Core Metadata 2.1.
        lines.append(f"Keywords: {','.join(keywords)}")

    for cls in project.get("classifiers", []):
        lines.append(f"Classifier: {cls}")

    req_python = project.get("requires-python")
    if req_python:
        lines.append(f"Requires-Python: {req_python}")

    for dep in project.get("dependencies", []):
        lines.append(f"Requires-Dist: {dep}")

    if readme_path and readme_path.exists():
        lines.append("Description-Content-Type: text/markdown")
        lines.append("")
        lines.append(readme_path.read_text(encoding="utf-8"))

    return lines


# ── Wheel-tag computation ─────────────────────────────────────────────────


def python_tag() -> str:
    return f"cp{sys.version_info.major}{sys.version_info.minor}"


def abi_tag() -> str:
    """Match the ABI tag baked into the extension's SOABI (`.cp314-...`)."""
    soabi = sysconfig.get_config_var("SOABI") or ""
    # SOABI on CPython looks like 'cpython-314-x86_64-linux-gnu' or
    # 'cp314-win_amd64' on Windows. Normalize to PEP-425's cpXY form.
    m = re.match(r"^(?:cpython-|cp)(\d+)(\d)?", soabi)
    if m:
        major, minor = m.group(1), m.group(2) or ""
        if len(major) >= 2:  # already cp314 form
            return f"cp{major}"
        return f"cp{major}{minor}"
    return python_tag()


def platform_tag() -> str:
    """PEP-425 platform tag for the current interpreter."""
    # Use packaging.tags if available — it knows about manylinux/musllinux.
    try:
        from packaging.tags import sys_tags  # type: ignore

        for tag in sys_tags():
            return tag.platform
    except ImportError:
        pass
    # Fallback: distutils-style.
    plat = sysconfig.get_platform().replace("-", "_").replace(".", "_")
    return plat


# ── Stage + pack ──────────────────────────────────────────────────────────


def cmake_install(build_dir: Path, prefix: Path) -> None:
    # The cmake project's install rules aren't componentized, so we run a
    # full install and rely on the fact that only the pybind11 module + its
    # stubs are dropped under the install prefix (the C lib targets install
    # under a deeper prefix; see bindings/python/CMakeLists.txt).
    subprocess.run(
        [
            "cmake", "--install", str(build_dir),
            "--prefix", str(prefix),
            "--config", "Release",
        ],
        check=True,
    )


def find_extension(stage_dir: Path) -> Path:
    """Locate the installed whiteout.*.{pyd,so} at the stage-dir root."""
    for entry in stage_dir.iterdir():
        if entry.is_file() and entry.suffix in (".pyd", ".so"):
            if entry.name.startswith("whiteout."):
                return entry
    raise FileNotFoundError(
        f"no whiteout.*.{{pyd,so}} found under {stage_dir} -- did cmake --install run?"
    )


def prune_non_wheel_content(stage_dir: Path) -> None:
    """Strip everything from `cmake --install` that doesn't belong in a wheel.

    The cmake project's top-level install() rules drop C++ headers, CMake
    export files, and licenses under <prefix>/{include,lib,share}/. None of
    that belongs in a Python wheel; only the pybind11 module + the stubs
    package are wheel content. We keep:

        whiteout.<abi>.<plat>.{pyd,so}      at the stage root
        whiteout-stubs/                     at the stage root
        <name>-<version>.dist-info/         (written separately, see below)

    and delete every other top-level entry. Anchoring on a small whitelist
    rather than a blacklist makes this robust as the C++ install set grows.
    """
    keep_dirs = {"whiteout-stubs"}
    for entry in stage_dir.iterdir():
        if entry.is_dir() and entry.name in keep_dirs:
            continue
        if entry.is_dir() and entry.name.endswith(".dist-info"):
            continue
        if entry.is_file() and entry.name.startswith("whiteout.") and \
                entry.suffix in (".pyd", ".so"):
            continue
        if entry.is_dir():
            shutil.rmtree(entry)
        else:
            entry.unlink()


def write_dist_info(
    stage_dir: Path,
    project: dict,
    repo_root: Path,
    tag: str,
) -> Path:
    name = project["name"]
    version = project["version"]
    di = stage_dir / f"{name}-{version}.dist-info"
    di.mkdir(parents=True, exist_ok=True)

    # METADATA — read README path from pyproject if specified.
    readme_rel = project.get("readme")
    readme_path = repo_root / readme_rel if isinstance(readme_rel, str) else None
    metadata_text = "\n".join(metadata_2_1_lines(project, readme_path)) + "\n"
    (di / "METADATA").write_text(metadata_text, encoding="utf-8")

    # WHEEL — PEP 427.
    (di / "WHEEL").write_text(
        f"Wheel-Version: 1.0\n"
        f"Generator: whiteout-pack-python-wheel\n"
        f"Root-Is-Purelib: false\n"
        f"Tag: {tag}\n",
        encoding="utf-8",
    )

    # LICENSE — if present at repo root.
    license_src = repo_root / "LICENSE"
    if license_src.exists():
        shutil.copyfile(license_src, di / "LICENSE")

    return di


def wheel_pack(stage_dir: Path, out_dir: Path, build_tag: str) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        "-m",
        "wheel",
        "pack",
        str(stage_dir),
        "--dest-dir",
        str(out_dir),
        "--build-number",
        build_tag,
    ] if build_tag else [
        sys.executable,
        "-m",
        "wheel",
        "pack",
        str(stage_dir),
        "--dest-dir",
        str(out_dir),
    ]
    subprocess.run(cmd, check=True)
    wheels = sorted(out_dir.glob("*.whl"), key=lambda p: p.stat().st_mtime)
    if not wheels:
        raise RuntimeError(f"wheel pack produced no .whl in {out_dir}")
    return wheels[-1]


# ── Entry point ───────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[3] if __doc__ else None)
    parser.add_argument("--build-dir", required=True, type=Path,
                        help="CMake build directory (must already be built).")
    parser.add_argument("--out-dir", required=True, type=Path,
                        help="Directory where the .whl will be written.")
    parser.add_argument("--repo-root", type=Path,
                        default=Path(__file__).resolve().parent.parent,
                        help="Repo root (where pyproject.toml lives). Defaults "
                             "to the parent of this script's directory.")
    parser.add_argument("--build-tag", default="",
                        help="Optional build number tag (PEP 427); typically empty.")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    out_dir = args.out_dir.resolve()
    repo_root = args.repo_root.resolve()

    if not build_dir.exists():
        sys.stderr.write(f"build-dir does not exist: {build_dir}\n")
        return 2
    if not (repo_root / "pyproject.toml").exists():
        sys.stderr.write(f"pyproject.toml not found under {repo_root}\n")
        return 2

    pyproject = load_pyproject(repo_root)
    project = pyproject["project"]

    with tempfile.TemporaryDirectory(prefix="whiteout-wheel-") as tmp:
        stage = Path(tmp) / "stage"
        stage.mkdir()

        # 1. Install cmake outputs into the stage dir.
        cmake_install(build_dir, stage)

        # 2. Verify the extension landed; use its filename to confirm the
        #    ABI tag we're about to write into the WHEEL file.
        ext = find_extension(stage)
        print(f"staged extension: {ext.relative_to(stage)}")

        # 3. Drop everything that isn't wheel content (cmake also installed
        #    headers, lib/cmake exports, share/licenses — all unwelcome here).
        prune_non_wheel_content(stage)

        # 4. Compute the wheel tag from the running interpreter.
        tag = f"{python_tag()}-{abi_tag()}-{platform_tag()}"
        print(f"wheel tag: {tag}")

        # 5. Write <name>-<ver>.dist-info/.
        write_dist_info(stage, project, repo_root, tag)

        # 6. Pack.
        whl = wheel_pack(stage, out_dir, args.build_tag)
        print(f"wrote {whl}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
