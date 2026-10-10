# Солидарность CAD

Solidar CAD (Солидарность CAD) is an early open-source parametric CAD
application. Source builds and binary packages are validated on Windows and
Ubuntu 24.04. The first milestone is a dependable part-design workflow:
constrained 2D sketch → solid features → editable history.

## Download and run

The Windows MVP is distributed as a portable x64 ZIP. Extract the complete
archive and run `bin/solidar.exe`; installation is not required. The executable
is not code-signed in 0.1.0, so Windows may show a reputation warning on first
launch.

Ubuntu 24.04 x86_64 uses the release `.deb`. After downloading it, install and
launch SolidarCAD with:

```bash
sudo apt install ./solidarcad_0.1.0-1_amd64.deb
solidar
```

The Debian package contains the pinned Qt 6.8.3 runtime in an isolated
`/opt/solidarcad` tree and registers the application and `.solidar` project
files with the desktop environment.

## MVP scope

- standalone Home screen with create/open project actions
- versioned `.solidar` project files with editable Sketch, Extrude, Pocket,
  Revolve, Fillet, Chamfer, Shell, Draft, Mirror, Move and pattern history
- independently linkable Home, Sketch and 3D View modules
- Qt 6 desktop shell with model tree and parameter editor
- 2D sketch workspace with lines, rectangles, circles, arcs, projection and
  geometric constraints
- interactive orbit/zoom viewport with direct 3D selection, manipulators and
  measurement ruler
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

On Windows, finalize the generated ZIP and its sidecars with:

```bash
python scripts/finalize_release.py --root . --build-dir build/release
```

On Ubuntu, CPack directly produces
`build/release/solidarcad_0.1.0-1_amd64.deb`; CI validates, installs and
smoke-tests that package before publishing it as an artifact.

The install step creates a runnable tree in `build/release/stage`. On Windows,
CPack creates a portable ZIP including runtime dependencies and third-party
notices. On Ubuntu, CPack creates an installable `.deb` with a private Qt 6.8.3
runtime, desktop entry and `.solidar` MIME association. GitHub Actions installs
and smoke-tests the generated Debian package, then uploads it with SHA-256 and
release-profile SPDX sidecars. Publishing a public release still requires a
clean-machine smoke test. Release artifacts include the canonical Qt 6.8.3
license texts and the binary-package SPDX inventories used to verify the
deployed Qt modules.

## Known MVP limitations

- single-part modelling only; assemblies are not implemented;
- Polygon, Slot, Text and sketch mirroring are not exposed as commands;
- the drawing workbench is not a complete production ESKD workflow;
- the Windows package is portable and unsigned;
- the Linux binary package currently targets Ubuntu 24.04 x86_64 only;
- complex topological edits can still require reselecting a face or edge.

## Roadmap

The project is currently at the Windows x64 0.1.0 MVP release-candidate gate.
The implemented MVP is intentionally limited to single-part parametric
modelling: Sketch, Extrude, Pocket, Revolve, Fillet, Chamfer, Shell, Draft,
Mirror, Move, Linear Pattern, Circular Pattern, project save/load and STEP/STL
exchange. Polygon, Slot, Text, sketch mirroring, assemblies and the complete
drawing workbench remain post-MVP work and are not exposed as available
commands.

Post-MVP development starts with field stabilization and a complete Sketcher
2.0, followed by Parametric Core 2.0 with a single authoritative sketch state
and persistent topology v2. Production ESKD drawing workflows come after that
foundation, followed by assemblies and macro automation. Signed installers,
dependency diagnostics and broader Linux QA continue as parallel tracks.
The ordered phases, exit criteria and current status are maintained in
[docs/roadmap.md](docs/roadmap.md).

See [docs/eskd-profile.md](docs/eskd-profile.md) for the implemented standards
profile and its current conformance boundary.
See [docs/modules.md](docs/modules.md) for feature ownership and module boundaries.
See [docs/testing.md](docs/testing.md) for CTest labels and the mandatory CAD
regression coverage.
See [docs/build-ubuntu.md](docs/build-ubuntu.md) for the reproducible Ubuntu
24.04 source-build and local-install procedure.
See [docs/linux-build-deps-package.md](docs/linux-build-deps-package.md) for
the reusable Ubuntu build-dependency SDK artifact.
See [docs/release-checklist.md](docs/release-checklist.md) for the release gate
and clean-machine acceptance procedure.
See [docs/reports/2026-10-04-mvp-release-readiness.md](docs/reports/2026-10-04-mvp-release-readiness.md)
for the latest automated release-readiness evidence and remaining blockers.
See [docs/persistent-topology.md](docs/persistent-topology.md) for the v1 face
and edge reference model, fallback rules and known limitations.
See [docs/reports/2026-08-28-parametric-3d-status.md](docs/reports/2026-08-28-parametric-3d-status.md)
for the historical August 2026 parametric 3D stabilization snapshot.

## License and third-party components

Unless a file or directory states otherwise, SolidarCAD source code and project
documentation are licensed under the [Mozilla Public License 2.0](LICENSE),
copyright (c) 2026 Молотков Михаил Алексеевич. See [NOTICE](NOTICE) for the
project notice and scope. Separately distributed paid modules and hosted
services may use other terms and are not covered by this license unless stated.
Third-party components retain their respective licenses; see
[DEPENDENCIES.md](DEPENDENCIES.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Release changes and user-visible limitations are recorded in
[CHANGELOG.md](CHANGELOG.md).

# Parametric 3D features

SolidarCAD supports history-based Extrude, Pocket, Revolve, Fillet, Chamfer,
Shell, Draft, Mirror, Linear Pattern and Circular Pattern features. The Russian
UI exposes Revolve as **«Инструмент вращения»**.
