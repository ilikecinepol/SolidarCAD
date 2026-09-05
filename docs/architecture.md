# Architecture

Solidar CAD separates persistent parametric intent from generated geometry and
presentation. This keeps the document testable without a GUI and prevents UI
objects from becoming the source of truth.

```text
Qt Widgets UI
  ├── feature tree and property editor
  └── viewport and selection
             │ commands / immutable IDs
Document model
  ├── parameters and feature dependency graph
  ├── undo/redo transaction log
  └── regeneration scheduler and diagnostics
             │ feature inputs
Geometry adapters
  ├── sketch constraint solver
  └── Open CASCADE B-Rep operations and exchange
             │ triangulation
Renderer
```

## Dependency rules

- `model` has no Qt or Open CASCADE dependency.
- UI modifies a document through commands; it never owns canonical geometry.
- Geometry adapters return values and diagnostics without referencing widgets.
- Persisted entities use stable IDs, not pointers or tree indices.
- Every operation must be reproducible from its parameters and upstream IDs.

## First vertical slice

The prototype already models a sketch/extrude history and rebuilds its preview
when dimensions change. The next slice will replace the preview with an Open
CASCADE `TopoDS_Shape`, while preserving the same document-facing interface.
# Revolve feature

`RevolveFeature` is a history-based `ShapeFeature`. It stores only its profile
sketch id, stable sketch-axis reference, angle, direction and shared solid
operation; OCCT B-Rep is rebuilt through `Document::recompute()` and is never
serialized.

## Parametric history 1.0

Each `Body` is an ordered dependency chain. A feature stores stable IDs and
topology references to its inputs; generated OCCT shapes are transient. Editing
a sketch marks every dependent feature dirty, while editing a feature marks that
feature and the following features in its body dirty. Recompute always proceeds
from upstream to downstream and preserves the IDs of existing nodes.

If a feature cannot rebuild, its stale result is discarded. Every downstream
feature is moved to `Error`, its stale shape is also discarded, and its
diagnostic identifies the invalid upstream feature. Other bodies still rebuild
independently. Correcting the source retries the same nodes and restores the
chain without recreating IDs. Face-supported sketches are repositioned after
each rebuilt producer so Pocket and later operations consume the updated face.

The persisted source of truth is the ordered feature definitions, parameters,
stable IDs, sketch geometry, and topology references. Save/load never treats a
cached B-Rep as authoritative. Version 1 intentionally remains linear within a
body: feature reorder, suppression, branching history, and additional Part
Design operation types are outside this milestone.

## Mirror and pattern history nodes

`MirrorFeature`, `LinearPatternFeature`, and `CircularPatternFeature` consume the
immediately preceding feature identified by a stable `FeatureId`. They create
OCCT B-Rep occurrences and return a compound containing the original and every
generated occurrence; render meshes are never transformed as source geometry.
Mirror v1 supports the global XY/XZ/YZ planes. Linear Pattern uses a global
X/Y/Z direction, with `count` meaning total occurrences and positive spacing in
millimetres. Circular Pattern uses a global X/Y/Z axis. A full 360 degree pattern
uses `angle/count` and therefore never duplicates 360 degrees; a partial pattern
uses `angle/(count-1)` and includes both range endpoints.

The three definitions and source IDs are persisted in project format v2. They
use the normal Body scheduler, including downstream Dirty propagation, stale
shape removal, blocked diagnostics, recovery, and stable IDs. Arbitrary datum
planes/axes, per-occurrence suppression, and boolean fusion are v1 limitations.

Multi-solid downstream modifiers use `ShapeContainerUtils`: global edge
selection is mapped to its owning solid, affected solids are rebuilt in
isolation, and the container is reconstructed with every unaffected occurrence.
Fillet and Chamfer therefore preserve Pattern compounds instead of rejecting or
silently truncating multi-solid results.
