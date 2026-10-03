#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPOSITORY_ROOT / "scripts"))

import finalize_release  # noqa: E402


class ReleaseArtifactScriptTests(unittest.TestCase):
    def temporary_directory(self) -> tempfile.TemporaryDirectory[str]:
        build_directory = REPOSITORY_ROOT / "build"
        build_directory.mkdir(exist_ok=True)
        return tempfile.TemporaryDirectory(
            prefix="release-artifact-test-", dir=build_directory)

    def make_archive(self, directory: Path, *, complete: bool = True) -> Path:
        version = finalize_release.project_version(REPOSITORY_ROOT)
        archive = directory / f"SolidarCAD-{version}-Windows-AMD64.zip"
        root = archive.stem
        required = list(finalize_release.REQUIRED_ARCHIVE_SUFFIXES)
        if not complete:
            required.remove("plugins/platforms/qwindows.dll")
        with zipfile.ZipFile(archive, "w") as release_zip:
            for suffix in required:
                release_zip.writestr(f"{root}/{suffix}", suffix)
        return archive

    def test_finalizer_creates_matching_checksum_and_release_sbom(self) -> None:
        with self.temporary_directory() as temporary_directory:
            archive = self.make_archive(Path(temporary_directory))
            checksum_path, sbom_path, digest = finalize_release.finalize(
                REPOSITORY_ROOT, archive)

            expected = hashlib.sha256(archive.read_bytes()).hexdigest()
            self.assertEqual(expected, digest)
            self.assertEqual(
                f"{expected}  {archive.name}\n",
                checksum_path.read_text("ascii"),
            )
            sbom = json.loads(sbom_path.read_text("utf-8"))
            self.assertEqual("SolidarCAD build profile: release", sbom["comment"])
            project_package = next(
                package for package in sbom["packages"]
                if package["name"] == "SolidarCAD")
            self.assertEqual(
                finalize_release.project_version(REPOSITORY_ROOT),
                project_package["versionInfo"],
            )
            qt_package = next(
                package for package in sbom["packages"]
                if package["name"] == "Qt")
            self.assertEqual(
                finalize_release.release_qt_version(REPOSITORY_ROOT),
                qt_package["versionInfo"],
            )

    def test_finalizer_rejects_incomplete_runtime(self) -> None:
        with self.temporary_directory() as temporary_directory:
            archive = self.make_archive(
                Path(temporary_directory), complete=False)
            with self.assertRaisesRegex(ValueError, "qwindows.dll"):
                finalize_release.finalize(REPOSITORY_ROOT, archive)


if __name__ == "__main__":
    unittest.main()
