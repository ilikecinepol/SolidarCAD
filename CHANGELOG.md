# Changelog

All notable user-visible changes are recorded in this file.

## 0.1.0 MVP Preview — Unreleased

### Added

- constrained 2D sketches with lines, rectangles, circles, native arcs,
  projection and geometric constraints;
- parametric Extrude, Pocket, Revolve, Fillet, Chamfer, Shell, Draft, Mirror,
  Move, Linear Pattern and Circular Pattern features;
- direct 3D selection and interactive manipulators for Part Design tools;
- an in-viewport ruler for point, edge and face measurements;
- editable feature history with Dirty, Valid and Error states;
- versioned `.solidar` project save/load with B-Rep regeneration;
- STEP import/export and STL export;
- shaded, shaded-with-edges and wireframe 3D views;
- light, dark and system themes;
- portable Windows x64 packaging with Qt/OCCT runtime files, notices and
  license materials, plus automated archive validation, SHA-256 and SBOM
  sidecars;
- an Ubuntu 24.04 x86_64 Debian package with an isolated Qt runtime, desktop
  menu integration, `.solidar` file association, automated installation smoke
  test, SHA-256 and SBOM sidecars.

### Known limitations

- the MVP supports single-part modelling; assemblies are not implemented;
- Polygon, Slot, Text and sketch mirroring are outside the exposed command set;
- the drawing workbench is not yet a complete production ESKD workflow;
- the Linux binary package currently targets Ubuntu 24.04 x86_64 only;
- Windows binaries are portable and unsigned;
- complex topology-changing edits can require reselecting a face or edge.

Release the 0.1.0 section only after every item in
`docs/release-checklist.md` is complete.
