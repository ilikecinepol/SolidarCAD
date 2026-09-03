# Солидарность CAD

Solidar CAD (Солидарность CAD) is an early open-source, cross-platform parametric CAD application
for Windows and Ubuntu. The first milestone is a dependable part-design workflow:
constrained 2D sketch → extrusion → editable feature history.

## Current prototype

- standalone Home screen with create/open project actions
- versioned `.solidar` project files with editable Sketch/Extrude/Pocket/Fillet history
- independently linkable Home, Sketch, Drawing and 3D View modules
- Qt 6 desktop shell with model tree and parameter editor
- 2D sketch workspace with a constrained rectangular profile
- on-screen A4 drawing sheet using the Russian ESKD profile
- shared vector renderer for preview, PDF export and physical printing
- A4 frame, title block, visible outlines and linear dimensions
- interactive orbit/zoom viewport
- cascading parametric history with Dirty/Valid/Error states
- platform-neutral document model with a smoke test

The 3D workflow uses Open CASCADE B-Rep geometry for extrusion, boolean pocket,
fillet, topology selection and body rendering. Persistent topological naming
remains a future step; current references retain an explicit legacy-index
fallback behind `TopologyReference`.

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
cmake --install build/release --prefix build/stage
```

The repository provides only the install foundation (`solidar` into `bin`) at
this stage. Runtime deployment and installer/package generation remain future
packaging work.

## Roadmap

1. Add interactive line/arc/circle tools and snapping.
2. Integrate a constraint-solver adapter (PlaneGCS evaluation first).
3. Complete the ESKD dimension and title-block variants and bundle a licensed
   stroke font conforming to GOST 2.304.
4. Integrate Open CASCADE and replace the preview box with a `TopoDS_Shape`.
5. Add picking, STEP/STL exchange, dependency diagnostics and undo/redo.

See [docs/eskd-profile.md](docs/eskd-profile.md) for the implemented standards
profile and its current conformance boundary.
See [docs/modules.md](docs/modules.md) for feature ownership and module boundaries.
See [docs/testing.md](docs/testing.md) for CTest labels and the mandatory CAD
regression coverage.
See [docs/reports/2026-08-28-parametric-3d-status.md](docs/reports/2026-08-28-parametric-3d-status.md)
for the current parametric 3D stabilization status and verification gate.

## License and third-party components

The license for SolidarCAD's own code has not been selected yet. Third-party
components retain their respective licenses; see [DEPENDENCIES.md](DEPENDENCIES.md)
and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
# Parametric 3D features

SolidarCAD supports history-based Extrude, Pocket, Fillet and Revolve features.
The Russian UI exposes Revolve as **«Инструмент вращения»**.
