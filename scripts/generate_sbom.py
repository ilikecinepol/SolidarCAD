#!/usr/bin/env python3
"""Generate a deterministic SPDX 2.3 inventory for direct build inputs."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def package(name: str, spdx_id: str, version: str, license_id: str,
            download: str) -> dict:
    return {
        "name": name,
        "SPDXID": spdx_id,
        "versionInfo": version,
        "downloadLocation": download,
        "filesAnalyzed": False,
        "licenseConcluded": "NOASSERTION",
        "licenseDeclared": license_id,
        "copyrightText": "NOASSERTION",
    }


def generate(root: Path, qt_version: str | None, occt_version: str | None,
             build_profile: str | None = None) -> dict:
    metadata = json.loads((root / "sbom/components.json").read_text("utf-8"))
    manifest = json.loads((root / "vcpkg.json").read_text("utf-8"))
    cmake_text = (root / "CMakeLists.txt").read_text("utf-8")
    match = re.search(r"cmake_minimum_required\s*\(VERSION\s+([^\s\)]+)", cmake_text)
    if not match:
        raise ValueError("cmake_minimum_required version was not found")
    qt = qt_version or metadata["qt_version"]
    occt = occt_version or metadata["occt_version"]
    project = metadata["project_version"]
    baseline = manifest["builtin-baseline"]
    packages = [
        package("SolidarCAD", "SPDXRef-SolidarCAD", project,
                "NOASSERTION", "NOASSERTION"),
        package("Qt", "SPDXRef-Qt", qt, "LGPL-3.0-only",
                f"https://download.qt.io/official_releases/qt/{'.'.join(qt.split('.')[:2])}/{qt}/"),
        package("Open CASCADE Technology", "SPDXRef-OCCT", occt,
                "LGPL-2.1-only WITH OCCT-exception-1.0",
                "https://github.com/Open-Cascade-SAS/OCCT"),
        package("vcpkg registry baseline", "SPDXRef-vcpkg-baseline", baseline,
                "MIT", f"https://github.com/microsoft/vcpkg/commit/{baseline}"),
        package("CMake minimum build profile", "SPDXRef-CMake", match.group(1),
                "BSD-3-Clause", "https://cmake.org/"),
    ]
    packages[2]["externalRefs"] = [{
        "referenceCategory": "PACKAGE-MANAGER",
        "referenceType": "purl",
        "referenceLocator": f"pkg:vcpkg/opencascade@{occt}",
    }]
    dependencies = ["SPDXRef-Qt", "SPDXRef-OCCT",
                    "SPDXRef-vcpkg-baseline", "SPDXRef-CMake"]
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"SolidarCAD-{project}-direct-build-inputs",
        "documentNamespace": f"https://solidarcad.example/sbom/{project}/direct-build-inputs",
        "creationInfo": {
            "created": "2026-08-28T00:00:00Z",
            "creators": ["Tool: scripts/generate_sbom.py"],
        },
        "documentDescribes": ["SPDXRef-SolidarCAD"],
        "packages": packages,
        "relationships": [{
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": "SPDXRef-SolidarCAD",
        }] + [{
            "spdxElementId": "SPDXRef-SolidarCAD",
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": item,
        } for item in dependencies],
    }
    if build_profile:
        document["comment"] = f"SolidarCAD build profile: {build_profile}"
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--qt-version")
    parser.add_argument("--occt-version")
    parser.add_argument("--build-profile", choices=("dev", "ci", "release"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    output = args.output or args.root / "sbom/solidarcad.spdx.json"
    rendered = json.dumps(generate(args.root, args.qt_version, args.occt_version,
                                   args.build_profile),
                          ensure_ascii=False, indent=2) + "\n"
    if args.check:
        if not output.exists() or output.read_text("utf-8") != rendered:
            print(f"SBOM is stale; regenerate {output}", file=sys.stderr)
            return 1
        return 0
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(rendered, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
