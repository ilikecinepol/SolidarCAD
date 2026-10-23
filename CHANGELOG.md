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
- single-part `.solidar` v5 product documents with stable project identity,
  explicit conversion from eligible standalone V3/V4 documents, identity-
  preserving Save and Save As clone with a fresh project identity;
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
- product v5 currently supports exactly one embedded part: Relocate, multiple
  definitions, occurrences, linked components and identity-conflict resolution
  are not implemented;
- historical v1 prism projects whose committed application state contains the
  legacy extrusion marker cannot yet be converted to v5; conversion
  unconditionally fails closed before chooser/prepare/writer/I/O until a
  semantics-preserving migration is implemented;
- Polygon, Slot, Text and sketch mirroring are outside the exposed command set;
- the drawing workbench is not yet a complete production ESKD workflow;
- the Linux binary package currently targets Ubuntu 24.04 x86_64 only;
- Windows binaries are portable and unsigned;
- complex topology-changing edits can require reselecting a face or edge.

The October 18 components/product program is a deliberate user-authorized
pre-release expansion of the 0.1.0 scope. The October 4 automatic RC evidence
predates this expansion and is historical; all release-checklist, automated
CI/package, field, GPU/DPI, clean-machine and tag gates must be repeated for the
expanded final RC.

Release the 0.1.0 section only after every item in
`docs/release-checklist.md` is complete.
