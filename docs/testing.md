# Test and regression policy

`ctest --preset ci` is the mandatory gate and runs every registered test.
Labels provide focused local diagnosis without duplicating CI execution:

```bash
ctest --preset ci -L unit
ctest --preset ci -L regression
ctest --preset ci -L ui-smoke
ctest --preset ci -L compliance
```

## Coverage audit

| Scenario | Existing coverage before this gate | Complete before? | Gap addressed |
|---|---|---:|---|
| Document, IDs, Dirty/Valid/Error | `document_tests` | Partial | The critical path still depended on C `assert`; the new E2E checks stable IDs and states independently of `NDEBUG`. |
| Sketch geometry | `document_tests`, `sketch_regression_tests` | Partial | Line, rectangle and circle were covered; the core model has no Arc primitive yet, so Arc remains outside the current supported API. |
| Sketch constraints | `sketch_regression_tests` | Mostly | Added an explicit Point-on-Circle followed by geometry creation, movement, solve and constraint removal. |
| Extrude | `extrude_feature_tests`, three Extrude regression tests | Yes | Added it to the unified user workflow and parameter cascade. |
| Sketch-on-Face and topology reference | `extrude_feature_tests`, `parametric_feature_chain_tests` | Partial | Added attachment persistence and rebuild checks; references still use the documented legacy subshape index. |
| Pocket | `pocket_feature_tests`, `parametric_feature_chain_tests` | Yes | Added parameter editing inside the unified workflow. |
| Fillet | `fillet_feature_tests`, `parametric_feature_chain_tests` | Mostly | Added unified workflow, OCCT Error-to-recovery and round-trip checks. |
| Chamfer | `chamfer_feature_tests`, `part_design_regression_tests` | Yes | Covers one/multiple edges, live preview, parameter edits, topology resolution, Error-to-recovery and v2 round-trip. |
| Shell | `shell_feature_tests` | Yes | Covers persistent removed faces, inside/outside thickness, live linear manipulator, invalid-thickness recovery, stable FeatureId and save/load rebuild. |
| Draft | `draft_feature_tests` | Yes | Covers persistent drafted faces, neutral plane and pull direction, angle/reversal, live angular manipulator, Error-to-recovery and save/load rebuild. |
| Revolve | `revolve_feature_tests` | Mostly | Added a second Body to the unified workflow, parameter editing and round-trip checks. |
| Cascading recompute | `document_tests`, `parametric_feature_chain_tests` | Partial | Added real B-Rep change, state and ID invariants across the complete Part Design body. |
| Project save/load and v1 compatibility | `project_file_tests` | Partial | Existing v1 loading remains; the E2E persists and rebuilds Extrude/Pocket/Fillet/Chamfer/Revolve history. |
| Rebuild diagnostics | `document_tests`, feature tests | Partial | Added NDEBUG-independent message/fallback/no-false-positive checks and fixed the valid-document result. |
| SBOM/compliance | `compliance_artifacts_tests` | Yes | Classified with the `compliance` label. |
| UI command smoke | `editor_smoke_tests`, `model_ribbon_tests` | Yes | Classified with the `ui-smoke` label; no mouse simulation was added. |

`part_design_regression_tests` is the critical CAD-core gate. It deliberately
uses volumes, non-null shapes, feature states, parameters and stable model IDs
instead of OCCT face/edge ordering or exact topology counts.

`parametric_history_tests` is the scheduler-level regression gate. It verifies
targeted dirty propagation, upstream-to-downstream evaluation, stale-result
removal, explicit blocked diagnostics, recovery with stable feature IDs, and
continued recompute of independent bodies. The Part Design regression test adds
the real Sketch -> Extrude -> Sketch-on-Face -> Pocket -> Fillet -> Chamfer path,
an independent Revolve body, and an edit after project save/load.

Mirror and pattern coverage is split into `mirror_feature_tests`,
`linear_pattern_feature_tests`, `circular_pattern_feature_tests`, and
`pattern_persistence_tests`. These verify occurrence counts, bounds/volume,
parameter edits with stable IDs, invalid-input recovery, full versus partial
circular distribution, upstream edits, and project-format round trips.

## Viewport Rendering 2.0 gate

Headless CI covers render-mesh generation, planar and curved per-vertex
normals, sharp face boundaries, reversed-face orientation, Normal/High chord
error and edge sampling, persistent-reference independence, and camera matrix
agreement. Existing viewport picking and preview-selection tests remain part of
the mandatory regression suite. A real OpenGL context is intentionally not a
hard CI dependency because availability differs between Windows and Ubuntu
runners.

Before release, perform the manual GPU gate on both Windows and Ubuntu:

- inspect Box, Cylinder, Fillet, Chamfer, Shell, and Extrude -> Chamfer -> Shell;
- verify Shaded, Shaded with Edges, and Wireframe modes;
- hover/select faces and edges in every mode and verify no rear edges leak;
- exercise Extrude, Revolve, Fillet, Chamfer, Shell, and Draft previews and
  manipulators, including changing an existing preview parameter;
- orbit, zoom, pan, resize, Fit, ISO, timeline scrub, delete, and Undo;
- verify HUD alignment at 100%, 125%, 150%, and 200% display scaling;
- compare Normal and High quality and confirm no remesh during camera movement.

## Viewport & Tool UX Polish manual recipe

Use a real GPU context at 100%, 125%, 150%, and 200% display scaling. Run each
scene in Shaded, Shaded with Edges, and Wireframe, orbiting through two complete
turns and pitching from -85 to +85 degrees while a preview is active:

1. Rectangle Sketch -> Extrude -> Fillet two edges -> Chamfer other edges ->
   Sketch-on-Face -> Extrude -> Shell.
2. Closed Sketch -> Revolve; drag through 30, 90, 180, and 360 degrees.
3. Extrude -> Linear Pattern -> Circular Pattern -> Mirror.

For Extrude, Pocket, Revolve, Fillet, Chamfer, Mirror, Linear Pattern, Circular
Pattern, Shell, and Draft verify mouse selection, hover, retained input
highlight, preview material, visible handle/HUD, panel synchronisation, numeric
Enter, HUD Escape, tool Cancel, Tab traversal, Apply, edit-existing, orbit and
zoom. No handle may be hidden by the opaque body or orientation cube. Invalid
input must leave the tool recoverable and must never display a broken mesh.

The automated `viewport_ux_polish_tests` gate covers camera matrix agreement,
combined source/preview depth safety across representative yaw/pitch extremes,
DPI-independent logical layout, body and overlay avoidance, visual sign
selection, expanded angular radius, and translation-invariant local edge
directions. Pixel-perfect rendering and driver-specific flicker remain manual
GPU checks.

## Part Design Stability Gate 2

Run this manual gate for at least 15–20 minutes with a real GPU context. Repeat
each tool two or three times, including both Apply/reopen and Cancel/reopen:

1. Rectangle, circle and polygon sketches followed by Extrude.
2. Sketch-on-Face followed by Extrude and Pocket.
3. Fillet, Chamfer, Mirror, Linear Pattern and Circular Pattern.
4. Revolve at 30, 90, 180 and 360 degrees.
5. Switch directly between Extrude, Revolve, Mirror and Circular Pattern; verify
   that only valid Sketch/Feature/Plane/Axis targets highlight.
6. Edit an existing feature, scrub the timeline backward and forward, then
   continue modelling.
7. Save, close, reopen, continue editing and Undo.
8. Perform at least twenty sequential tool sessions, including invalid input.

Throughout the run, Apply and Cancel must remain visible and clickable; docks,
menus, timeline and feature tree must exist exactly once; selection, preview and
manipulators must clear after Apply/Cancel; invalid input must be recoverable;
and no tool may require restarting the application.

The automated companion is `part_design_ui_state_regression_tests`. It performs
twenty repeated controller cycles and checks the single-active-tool invariant,
typed re-selection cancellation, presentation cleanup and duplicate registration
guard.
