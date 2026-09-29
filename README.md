# Солидарность CAD

Solidar CAD (Солидарность CAD) is an early open-source, cross-platform parametric CAD application
for Windows and Ubuntu. The first milestone is a dependable part-design workflow:
constrained 2D sketch → extrusion → editable feature history.

## MVP scope

- standalone Home screen with create/open project actions
- versioned `.solidar` project files with editable Sketch/Extrude/Pocket/Fillet/Chamfer history
- independently linkable Home, Sketch and 3D View modules
- Qt 6 desktop shell with model tree and parameter editor
- 2D sketch workspace with lines, rectangles, circles, arcs, projection and
  geometric constraints
- interactive orbit/zoom viewport
- cascading parametric history with Dirty/Valid/Error states
- platform-neutral document model with a smoke test

The 3D workflow uses Open CASCADE B-Rep geometry for extrusion, boolean pocket,
fillet, equal-distance chamfer, topology selection and body rendering.
Persistent face and edge references use semantic tags and geometric signatures,
with an explicit legacy-index fallback for older v2 projects.

## Prerequisites

- CMake 3.24+
- Ninja
- a C++20 compiler: Visual Studio 2022 on Windows or GCC 12+ on Ubuntu
- Qt 6.5+ with Widgets, OpenGLWidgets and PrintSupport
- Open CASCADE Technology 8.0.1 (provided reproducibly by the pinned vcpkg manifest)

## Build profiles

### Development

The `dev` profile is the flexible local workflow. It builds Debug binaries and
all tests, accepts any Qt 6.5 or newer supplied by Qt Creator or
`CMAKE_PREFIX_PATH`, and can use a local OCCT package or the existing Windows
Qt Creator OCCT build-tree fallback. Qt 6.11.1 is the recommended development
version, but is not required.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

If Qt is installed outside the default search path, set `CMAKE_PREFIX_PATH` or
pass `-DQt6_DIR=/path/to/Qt/6.x/lib/cmake/Qt6` while configuring.
The build type may be changed locally to `RelWithDebInfo` with
`-DCMAKE_BUILD_TYPE=RelWithDebInfo`.

### CI

The `ci` profile is the automated validation configuration: Release compiler
flags, tests enabled, Qt 6.8.3, OCCT 8.0.1, and the pinned vcpkg baseline. GitHub
Actions runs configure, build, SBOM generation/upload, and all CTest tests on
Windows and Ubuntu. It bootstraps vcpkg and sets `VCPKG_ROOT` and the platform
triplet before running:

```bash
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
```

### Release

The `release` profile is the official foundation for production binaries. It
uses Release flags, disables test executables, requires manifest-mode OCCT
8.0.1 through the pinned vcpkg toolchain, and deliberately disables the local
OCCT build-tree fallback. It rejects Qt versions other than 6.8.3; Qt must be
installed separately. From a clean checkout, first clone vcpkg at baseline
`00c5775211f45cd08b37fce0484b4cb940e422ab`, bootstrap it, install Qt 6.8.3,
then set `VCPKG_ROOT`, `VCPKG_DEFAULT_TRIPLET` (`ci-x64-windows` or
`ci-x64-linux`), and the Qt prefix before running:

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/release
cpack --config build/release/CPackConfig.cmake
```

The install step creates a runnable tree in `build/release/stage`. CPack creates
a portable Windows ZIP including runtime dependencies and third-party notices;
GitHub Actions builds and uploads it as a workflow artifact. The Linux install
tree remains a developer validation artifact rather than an MVP distribution.
Publishing a public release still requires a clean-machine smoke test. The
portable archive already includes the canonical Qt 6.8.3 license texts and the
binary-package SPDX inventories used to verify the deployed Qt modules.

## Roadmap

The MVP is intentionally limited to single-part parametric modelling on
Windows x64: sketch, Extrude, Pocket, Revolve, Fillet, Chamfer, Shell, Draft,
project save/load and STEP/STL exchange. Polygon, Slot, Text, sketch mirroring,
assemblies and the drawing workbench are post-MVP and are not exposed as
available commands.

Post-MVP priorities are solver hardening, dependency diagnostics, complete
ESKD drawing workflows, assemblies, signed installers and broader Linux QA.

See [docs/eskd-profile.md](docs/eskd-profile.md) for the implemented standards
profile and its current conformance boundary.
See [docs/modules.md](docs/modules.md) for feature ownership and module boundaries.
See [docs/testing.md](docs/testing.md) for CTest labels and the mandatory CAD
regression coverage.
See [docs/persistent-topology.md](docs/persistent-topology.md) for the v1 face
and edge reference model, fallback rules and known limitations.
See [docs/reports/2026-08-28-parametric-3d-status.md](docs/reports/2026-08-28-parametric-3d-status.md)
for the current parametric 3D stabilization status and verification gate.

## License and third-party components

Unless a file or directory states otherwise, SolidarCAD source code and project
documentation are licensed under the [Mozilla Public License 2.0](LICENSE),
copyright (c) 2026 Молотков Михаил Алексеевич. See [NOTICE](NOTICE) for the
project notice and scope. Separately distributed paid modules and hosted
services may use other terms and are not covered by this license unless stated.
Third-party components retain their respective licenses; see
[DEPENDENCIES.md](DEPENDENCIES.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
# Parametric 3D features

SolidarCAD supports history-based Extrude, Pocket, Fillet, Chamfer and Revolve features.
The Russian UI exposes Revolve as **«Инструмент вращения»**.
