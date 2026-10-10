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

- `solidar_model` is Qt-free, but links Open CASCADE for B-Rep feature rebuilds.
- Generated OCCT shapes are transient results; parameters, stable IDs and
  topology references remain the persistent source of truth.
- UI modifies a document through commands; it never owns canonical geometry.
- Geometry adapters return values and diagnostics without referencing widgets.
- Persisted entities use stable IDs, not pointers or tree indices.
- Every operation must be reproducible from its parameters and upstream IDs.

## Implemented foundation

The current model rebuilds Open CASCADE `TopoDS_Shape` results for Extrude,
Pocket, Revolve, Fillet, Chamfer, Shell, Draft, Mirror and pattern features.
Sketch edits propagate through the feature history, while save/load persists
only the definitions needed to reproduce those shapes.
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

## Universal Part Design tool framework

Part Design interaction is defined by `PartDesignToolDefinition` and executed
through one `PartDesignToolController`. The controller enforces that at most one
tool is active and routes Escape either to the current nested re-selection or
to cancellation of the complete tool. Persistent and preview data live in a
`ToolSession`; a dock, combo box, manipulator, or HUD is never authoritative.

The common lifecycle is `Inactive`, `SelectingInput`, `SelectingReference`,
`EditingParameters`, `PreviewValid`, and `PreviewInvalid`. Selection is declared
with `SelectionRequirement` (`Sketch`, `SketchLine`, `Edge`, `Face`, `Axis`,
`Plane`, `Body`, or `Feature`). Parameters are declared with
`ToolParameterDescriptor`; `ToolParameterHud` renders all HUD-editable numeric
parameters, keeps Enter separate from Apply, uses normal Tab/Shift+Tab traversal,
and consumes the first Escape to restore the last committed field value.

The standard registry contains Extrude, Pocket, Revolve, Fillet, Chamfer,
Shell, Draft, Mirror, Linear Pattern, and Circular Pattern. Revolve, Fillet,
Chamfer, Shell, and Draft use
the common runtime controller and session parameter contracts; the registry is
the mandatory migration boundary for the remaining legacy command bodies.

Shell stores persistent references to the faces removed from the immediately
preceding feature, thickness, and the inside/outside mode. Draft stores the
drafted faces, neutral plane, pull direction, angle, and reversal. Both use
OCCT only while rebuilding or previewing; generated B-Rep is never serialized.
Selections are resolved again after upstream recompute. Compound inputs keep
untouched solids. Shell v1 deliberately requires all removed faces to belong
to one solid; selecting faces from several solids produces a recoverable tool
error.

To add a Part Design tool:

1. implement `FooFeature` and `FooToolSession`;
2. add its selection requirements and parameter descriptors to the registry;
3. expose its manipulator and build temporary preview geometry in the session;
4. register commit/cancel bindings with `PartDesignToolController`;
5. bind the generic HUD and parameters panel to session parameters;
6. commit the feature only on Apply and reuse the same session with an
   `editingFeatureId` for history editing.

New tools must use this template. They must not introduce a new dialog-driven
selection/preview/Apply pipeline in `MainWindow`.

## Viewport Rendering Architecture

The CAD model remains authoritative and the viewport consumes immutable
world-space display data:

```text
OCCT B-Rep
  -> scale-aware tessellation cache (BodyRenderMesh)
  -> separate source/preview GPU meshes (VAO + VBO + EBO)
  -> depth-tested smooth surface pass
  -> topological B-Rep edge pass
  -> CPU picking plus Qt selection/tool/HUD overlays
```

`BodyRenderMesh` owns canonical indexed vertices/triangles and sampled
topological edges; it does not retain a second expanded triangle array.
Vertices are deliberately not welded between topological faces, preserving
sharp CAD boundaries. Its structural `BodyMeshKey` contains body ID, feature
ID, model-owned shape revision, shape identity and quality. Camera orbit, zoom,
pan, resize, hover, and selection only update projection/index data, matrices or
shader uniforms; they never invoke OCCT meshing. A monotonic fallback revision
protects the legacy same-pointer `Viewport::setBodyShape` route from in-place
shape mutation.

`ViewportRenderer` owns OpenGL resources and shaders. Every currently displayed
body remains independently cached; temporary tool/cut previews have separate
entries. Resource pruning is an explicit scene transaction, so clearing a scene
also releases buffers when no render pass follows. Destruction is accepted only
under the owning OpenGL context or its share group.

`ProjectedPickingScene` is camera- and mesh-revisioned and provides the shared
BVH-backed hover/click/marquee/snap/ruler path. Interactive queries may return an
unpublished uncertainty when their work budget is exhausted. Interaction
boundaries use exact indexed visibility, including output-sensitive face
refinement and interval-based edge occlusion.

`PreviewUpdateCoordinator` owns GUI-thread preview cadence. It coalesces each
stream to the newest request on a periodic tick, captures immutable source
identity, and rejects stale generations/sequences before publication. Project
replacement, import, Undo/Redo and tool teardown invalidate pending work before
destroying or replacing its inputs. Release, Enter and Apply flush the latest
request and perform Fillet/Chamfer/Shell boundary refinement exactly once.

`Viewport` retains camera interaction, picking-scene construction, reference
geometry, manipulators, and QWidget HUD placement. `MainWindow` coordinates
preview sessions and only forwards display-mode/quality choices; it contains no
OpenGL implementation details. OCCT rebuild and preview calls remain
synchronous on the GUI thread until a separate thread-safety/lifetime design is
proven.
