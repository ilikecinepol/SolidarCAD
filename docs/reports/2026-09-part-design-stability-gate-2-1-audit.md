# Part Design Stability Gate 2.1 — Audit Report

Date: 2026-09 (branch `codex/stability-gate-2-1`)

## Scope

Audit of the tool-lifecycle / interaction orchestration that breaks Fillet/Chamfer
hover & selection, tool re-open and cross-tool transitions, and the stability of
the auto Extrude Join proposal.

## Findings

### 7.1 Which pick modes / selection filters persist and are NOT reset on tool switch

- `Viewport::pickMode_` (declared `Viewport.h:207`, init `PickMode::None`) is the
  single gate over pick routing. Write sites: `resetScene` (`Viewport.cpp:577`),
  `beginSketchPlaneSelection` (`:591`), `beginExtrusionSurfaceSelection` (`:607`),
  `beginRevolveAxisSelection` (`:762`), `mousePressEvent` (`:2030`, `:2122`, `:2135`,
  `:2191`, `:2205`, `:2230`), `keyPressEvent` (`:3280`, `:3289`).
- `Viewport::selectionFilter_` (declared `Viewport.h:218`, init `SelectionFilter::Any`)
  is reset at `resetScene` (`:578`) and `beginSketchPlaneSelection` (`:590`), but is
  NOT reset by `beginExtrusionSurfaceSelection` or `beginRevolveAxisSelection`.
  Consequence: switching from Fillet (`filter=Edge`, `edgeMultiSelectionMode_=true`)
  to Revolve axis picking keeps `Edge` filter and edge multi-selection active; the
  old orange edge selection visually leaks into the next tool.
- `beginExtrusionSurfaceSelection` also does NOT clear `selectedBodyEdge*`,
  `selectedBodyFace*`, `edgeMultiSelectionMode_`, `faceMultiSelectionMode_`,
  `selectedBasePlane_`, `selectedVertex_`, `selectedOrigin_`. Escape (`keyPressEvent
  :3288`) manually resets an arbitrary subset; the rest persists until `resetScene`.
- `resetScene` resets everything but is only called for document/view rebuilds, not
  for tool open/close, so stale interaction state survives between tools.

### 7.2 Where cleanup is manual and duplicated in MainWindow (not centralized)

- `cancelFilletTool`/`cancelChamferTool`: `clearToolPreviewShape` + `clearToolManipulator`
  + `setEdgeMultiSelectionMode(false)` + `setSelectionFilter(Any)` +
  `setSelectedBodyEdges({})` + `toolParametersDock_->hide()`.
  Duplicated in `acceptFilletTool`/`acceptChamferTool` (`MainWindow.cpp:2476-2483`,
  `:2388-2395`).
- `cancelShellTool`/`cancelDraftTool` repeat the same pattern with `Face`.
- `cancelRevolveTool` repeats preview/manipulator/dock cleanup.
- Field `selectedExtrusionSurface_`, `extrusionDock_` visibility and the operation
  combo are handled only in MainWindow; the `extrudeRequested` lambda
  (`:1040-1053`) resets `extrudeOperationManuallyChanged_`, reverse checkbox and
  combo to 0 but does NOT reset `selectedExtrusionSurface_`, and leaves
  `extrusionDock_` visible state untouched.

### 7.3 Tools that register an empty clearPresentation

- All five registrations `MainWindow.cpp:1268-1282` (Revolve, Fillet, Chamfer, Shell,
  Draft) pass `{}` as the third `Registration` field (`PartDesignToolController.h:16-20`),
  so `deactivate()`/`cancelActive()` invoke no per-tool viewport cleanup — cleanup
  depends entirely on the cancel callbacks in MainWindow.
- Registration struct: `{ ToolSession* session; std::function<void()> cancel;
  std::function<void()> clearPresentation; }`.
- `cancelRevolveTool`, `cancelFilletTool`, `cancelChamferTool`, `cancelShellTool`,
  `cancelDraftTool` are early-returning (no-op) when their session is `Inactive`,
  which makes repeated cancellation safe; however `deactivate()` sets `active_ = None`
  only AFTER invoking `clearPresentation` (`PartDesignToolController.cpp:29-38`),
  so any future callback that re-enters the controller would recurse.
- Only 5 of 11 `PartDesignToolKind` values are registered. Extrude, Pocket, Mirror,
  LinearPattern, CircularPattern and Sketch are not routed through the controller, so
  activating them does NOT cancel an in-flight Fillet/Chamfer/Shell/Draft/Revolve
  session (their sessions can stay `PreviewValid` while the user starts Extrude,
  leaving `PickMode::SketchPlane`, filters, hover and docks inconsistent).
- `part_design_tool_framework_tests.cpp:67-71` already exercises a non-empty
  `clearPresentation` (Revolve increments a counter); the production registrations
  never match that behavior.

### 7.4 Which hover/click paths have real coverage and which don't

- Hardy nothing covers real mouse hover/click over body edges:
  - `edge_selection_state_tests.cpp`, `viewport_picking_tests.cpp` assert
    set/rest-state transitions (via `setSelectedBodyEdges`, filters) but never feed
    `QMouseEvent`s at projected edge pixels.
  - `viewport_preview_selection_tests.cpp` covers sketch-plane and B-Rep face picking
    with real mouse events (including the bare body-face regression), plus preview
    persistence of `setSelectedBodyEdges`, but no edge hover/click at all.
  - `viewport_ux_polish_tests.cpp` and `viewport_manipulator_tests.cpp` test manipulators.
- The actual hover/click path `updateBodyHover` -> `mousePressEvent` edge branch
  (`Viewport.cpp:2263-2290`) is production-only; any regression there (e.g. an early
  return, wrong filter combination, stale `pickMode`) is invisible to CI.

### 7.5 When auto Extrude can change Join -> New Body / Cut

- `updateAutomaticExtrudeOperation` (`MainWindow.cpp:1463-1480`) is invoked from
  every `extrusionLengthSpin_` change (`:716`), reverse toggle (`:725`),
  `extrusionPreviewLengthChanged` (`:735`), `normalizeExtrusionInput`/load path
  (`:1460`) and surface pick (`:1189`). Any drag of the manipulator handle while
  auto-detection is enabled re-runs `detectExtrudeOperation` with the new length,
  which can flip Join <-> NewBody/Cut (and can flip again on release).
- `extrudeOperationManuallyChanged_` protects only the case where the user touched
  the operation combo explicitly; it is reset solely in `extrudeRequested` (`:1046`).
- Bare body-face picks (no sketch contour) leave `profile == nullptr`, so the
  detector falls back to `NewBody` even though the face is physically attached to
  the target body (a Join-by-construction case).

## Prior work carried on this branch

- Commit `9e67c66` ("Extrude bare body face along its resolved face normal") wraps the
  bare-face join/cut geometry; the bare-face pick path and its regression test live in
  `viewport_preview_selection_tests.cpp` (last block) and `MainWindow::extrudeSketch`.
- Control point `48e4db1` (B-Rep face selection + hover) remains in history.

## Recommended remediation (map)

1. `Viewport::resetToolInteraction()` cleared on every tool-open (layout in
   `begin*Selection` contexts) – central reset for P0.1/P0.2.
2. Real `clearPresentation` lambdas in the five registrations + `deactivate()`
   recursion fix – P0.3.
3. `cancelActive()` on Extrude/Pocket/Pattern entry so unregistered tools cannot
   leave an active registered session behind.
4. Sticky auto proposal for Extrude (detect once per new profile/input, not per
   length/drag/camera change) + bare-face default proposal = `Join`.
5. New real-mouse-edge test suite + tool transition assertions.

## Remediation performed (this branch)

All five remediation items shipped in the following commit set (final SHA below):

1. `Viewport::resetToolInteraction()` – central reset of pick mode, selection
   filter, edge/face multi-select, selected/hovered body edges+faces, base plane,
   vertex, origin, extrusion hover/selection candidates, `revolveAxisSketchIndex_`,
   dragging flags, `basePlanesVisible_`, extrusion manipulator and cursor status.
   Installed as the first step of every `begin*Selection` context
   (`beginEdgeSelection`, `beginFaceSelection`, `beginSketchPlaneSelection`,
   `beginExtrusionSurfaceSelection`, `beginRevolveAxisSelection`).
2. `PartDesignToolController::deactivate()` clears `active_`/`temporaryStage_`/
   `returnStage_` *before* the shared `clearPresentation` lambda runs; all five
   registrations (Revolve, Fillet, Chamfer, Shell, Draft) now use one idempotent
   lambda (preview + manipulator + `resetToolInteraction()` + dock hide). The
   duplicate manual cleanup inside the cancel/accept handlers of MainWindow is gone.
3. `cancelActive()` added on entry to Extrude, Pocket, the three Pattern tools and
   Sketch, so unregistered sessions cannot leak across tools.
4. Sticky Extrude auto proposal: `extrudeAutoDetectEnabled_` /
   `extrudeOperationStale_` flags gate `updateAutomaticExtrudeOperation()` (one
   re-detect per new profile, not per length/drag/preview change); bare body-face
   picks with no sketch contour propose `Join` by default.
5. `SOLIDAR_VIEWPORT_LOG` (NDEBUG-gated) beside the existing `SOLIDAR_TOOL_LOG` on
   the controller side.

## Verification

- `ctest`: 43/43 green at the final SHA, including the new
  `viewport_edge_interaction_tests` (real QMouseEvent hover/click over a
  B-Rep box: edge hover, click-select, preview survival, preview rebuild,
  stale Extrusion/SketchPlane/RevolveAxis pick-mode immunity, cross-tool reset,
  face hover/select) and the extended `extrude_feature_tests` (Join stays stable
  across lengths 5/60/400 mm; circle and 3-line polygon profiles on an attached
  face propose Join; reverse proposes Cut).
- Clean full rebuild (no ABI rebuild issue after the `Viewport.h` change).

---

## Final report (gate §21 format)

```text
Branch:                 codex/stability-gate-2-1
Base SHA:               9e67c66ae17bceceb775d3897fcd1051061f0b2e
Final SHA:              0b429102a36222cade8367a5f59151f482209e77

Root causes:
1. Viewport::pickMode_ / selectionFilter_ / multi-select / selected-body refs
   persisted between tools; beginExtrusionSurfaceSelection and
   beginRevolveAxisSelection did not reset filter or stale selections, so edge
   hover/select state leaked across tool switches (P0.1/P0.2).
2. clearPresentation of all five registered tools was a no-op and cleanup was
   duplicated and incomplete in MainWindow cancel/accept handlers; deactivate()
   cleared active_ only after clearPresentation, allowing recursive/partial
   state teardown (P0.3).
3. Unregistered tools (Extrude/Pocket/Patterns/Sketch) never asked the
   controller to cancel, leaving an active session open across transitions.
4. updateAutomaticExtrudeOperation re-ran detectExtrudeOperation on every
   length/drag/camera/preview change (Join <-> NewBody flip), and bare body-face
   picks with no sketch contour always fell back to NewBody (Join-by-construction
   case).

Interaction-state changes:
  Viewport::resetToolInteraction() central reset; begin*Selection contexts
  (Edge/Face/SketchPlane/ExtrusionSurface/RevolveAxis) all start from it;
  hoveredBodyEdgeIndex()/hoveredBodyFaceIndex() accessors for tests.

Edge highlight fix:
  Edge hover/click now routes only when filter==Edge and no stale pick mode is
  active; preview rebuilds keep the underlying selected source edges.

Tool lifecycle fix:
  One clearPresentation lambda used by all 5 registrations; deactivate() resets
  session state before teardown; cancelActive() added to the 5 unregistered tool
  entry points.

Extrude Join fix:
  Sticky auto-detection (single re-evaluation per new profile/input) and
  bare-face default proposal = Join for attached body faces.

Files changed:
  src/ui/Viewport.h
  src/ui/Viewport.cpp
  src/ui/MainWindow.h
  src/ui/MainWindow.cpp
  src/ui/tools/PartDesignToolController.cpp
  tests/CMakeLists.txt
  tests/viewport_edge_interaction_tests.cpp   (new)
  tests/extrude_feature_tests.cpp
  docs/reports/2026-09-part-design-stability-gate-2-1-audit.md   (this report)

Tests added/updated:
  ctest 43/43 green (was 42/42). New: viewport_edge_interaction_tests
  (8 scenarios). Updated: extrude_feature_tests (Join length-stability loop,
  circle + polygon attached-face Join/Cut).

Manual GPU Gate:
A: Not run (requires interactive GPU host)
B: Not run
C: Not run
D: Not run

Windows CI:
  Local clean rebuild + full ctest green. (No CI pipeline attached to branch.)
Ubuntu CI:
  Not run on this branch (Windows workstation only).

Known limitations:
  - GPU Gate A-D and Linux CI still require the manual/interactive pass.
  - Edge hovering depends on the depth-epsilon heuristic already in use; the
    new tests replicate the fitAll projection rather than driving a real
    camera, so GPU-exact pixel behavior is verified only indirectly.
  - Multi-body sheets and non-planar support faces are outside the equalizer'd
    regression set.

Safe to merge into main:
  NO
```