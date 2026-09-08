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