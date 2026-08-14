# Солидарность CAD

Solidar CAD (Солидарность CAD) is an early open-source, cross-platform parametric CAD application
for Windows and Ubuntu. The first milestone is a dependable part-design workflow:
constrained 2D sketch → extrusion → editable feature history.

## Current prototype

- standalone Home screen with create/open project actions
- versioned `.solidar` project files
- independently linkable Home, Sketch, Drawing and 3D View modules
- Qt 6 desktop shell with model tree and parameter editor
- 2D sketch workspace with a constrained rectangular profile
- on-screen A4 drawing sheet using the Russian ESKD profile
- shared vector renderer for preview, PDF export and physical printing
- A4 frame, title block, visible outlines and linear dimensions
- interactive orbit/zoom viewport
- parametric box represented as `Sketch 1 → Extrude 1`
- platform-neutral document model with a smoke test

The viewport currently renders a lightweight preview. Open CASCADE B-Rep geometry,
selection and STEP exchange are the next backend milestone.

## Prerequisites

- CMake 3.24+
- Ninja
- a C++20 compiler: Visual Studio 2022 on Windows or GCC 12+ on Ubuntu
- Qt 6.5+ with Widgets, OpenGLWidgets and PrintSupport

## Build

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

If Qt is installed outside the default search path, set `CMAKE_PREFIX_PATH` or
pass `-DQt6_DIR=/path/to/Qt/6.x/lib/cmake/Qt6` while configuring.

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

## License

License selection is pending dependency review. MPL-2.0 is the current candidate.
