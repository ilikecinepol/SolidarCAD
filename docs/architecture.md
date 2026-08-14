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
