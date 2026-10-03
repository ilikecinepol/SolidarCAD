#!/usr/bin/env python3
"""Validate and finalize a portable Windows release archive.

The script fails closed when the CPack archive is incomplete.  On success it
writes a SHA-256 sidecar and a release-profile SPDX document next to the ZIP.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath

sys.dont_write_bytecode = True

from generate_sbom import generate


REQUIRED_ARCHIVE_SUFFIXES = (
    "bin/solidar.exe",
    "bin/Qt6Core.dll",
    "bin/Qt6Widgets.dll",
    "bin/TKernel.dll",
    "plugins/platforms/qwindows.dll",
    "share/SolidarCAD/LICENSE",
    "share/SolidarCAD/NOTICE",
    "share/SolidarCAD/README.md",
    "share/SolidarCAD/CHANGELOG.md",
    "share/SolidarCAD/DEPENDENCIES.md",
    "share/SolidarCAD/THIRD_PARTY_NOTICES.md",
    "share/SolidarCAD/LICENSES/Qt/README.md",
    "share/SolidarCAD/LICENSES/OCCT/README.md",
)


def project_version(root: Path) -> str:
    cmake_text = (root / "CMakeLists.txt").read_text("utf-8")
    match = re.search(
        r"project\s*\(\s*SolidarCAD\s+VERSION\s+([^\s\)]+)", cmake_text)
    if not match:
        raise ValueError("SolidarCAD project version was not found")
    return match.group(1)


def release_qt_version(root: Path) -> str:
    presets = json.loads((root / "CMakePresets.json").read_text("utf-8"))
    for preset in presets["configurePresets"]:
        required = preset.get("cacheVariables", {}).get(
            "SOLIDAR_REQUIRED_QT_VERSION")
        if required:
            return str(required)
    raise ValueError("No pinned SOLIDAR_REQUIRED_QT_VERSION was found")


def discover_archive(root: Path, build_dir: Path) -> Path:
    version = project_version(root)
    candidates = sorted(build_dir.glob(f"SolidarCAD-{version}-*.zip"))
    if not candidates:
        raise ValueError(f"No SolidarCAD {version} ZIP found in {build_dir}")
    if len(candidates) != 1:
        names = ", ".join(path.name for path in candidates)
        raise ValueError(
            f"Expected one SolidarCAD {version} ZIP, found: {names}; "
            "select one with --archive")
    return candidates[0]


def validate_archive(archive: Path) -> None:
    with zipfile.ZipFile(archive) as release_zip:
        names = [name.replace("\\", "/") for name in release_zip.namelist()]

    unsafe = []
    for name in names:
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts:
            unsafe.append(name)
    if unsafe:
        raise ValueError(
            "Archive contains unsafe paths: " + ", ".join(sorted(unsafe)))

    top_levels = {PurePosixPath(name).parts[0] for name in names if name}
    if len(top_levels) != 1:
        raise ValueError("Archive must contain exactly one top-level directory")
    expected_root = archive.stem
    if top_levels != {expected_root}:
        raise ValueError(
            f"Archive root must be {expected_root}, found "
            f"{', '.join(sorted(top_levels))}")

    missing = [
        suffix for suffix in REQUIRED_ARCHIVE_SUFFIXES
        if not any(name.endswith("/" + suffix) for name in names)
    ]
    if missing:
        raise ValueError(
            "Archive is missing required runtime or notice files: "
            + ", ".join(missing))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def finalize(root: Path, archive: Path,
             qt_version: str | None = None) -> tuple[Path, Path, str]:
    root = root.resolve()
    archive = archive.resolve()
    validate_archive(archive)

    digest = sha256(archive)
    checksum_path = archive.with_suffix(archive.suffix + ".sha256")
    checksum_path.write_text(
        f"{digest}  {archive.name}\n", encoding="ascii", newline="\n")

    metadata = json.loads((root / "sbom/components.json").read_text("utf-8"))
    document = generate(
        root,
        qt_version or release_qt_version(root),
        str(metadata["occt_version"]),
        "release",
    )
    sbom_path = archive.with_suffix(".spdx.json")
    sbom_path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return checksum_path, sbom_path, digest


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate a Windows CPack ZIP and create release sidecars")
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--qt-version")
    args = parser.parse_args()

    root = args.root.resolve()
    build_dir = args.build_dir or root / "build/release"
    if not build_dir.is_absolute():
        build_dir = root / build_dir
    try:
        archive = args.archive
        if archive is None:
            archive = discover_archive(root, build_dir)
        elif not archive.is_absolute():
            archive = root / archive
        checksum_path, sbom_path, digest = finalize(
            root, archive, args.qt_version)
    except (KeyError, OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"Release finalization failed: {error}", file=sys.stderr)
        return 1

    print(f"Validated: {archive}")
    print(f"SHA-256: {digest}")
    print(f"Checksum: {checksum_path}")
    print(f"SBOM: {sbom_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
