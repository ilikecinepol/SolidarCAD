# Test and regression policy

`ctest --preset ci` is the mandatory gate and runs every registered test.
Labels provide focused local diagnosis without duplicating CI execution:

```bash
ctest --preset ci -L unit
ctest --preset ci -L regression
ctest --preset ci -L ui-smoke
ctest --preset ci -L compliance
```

Every CTest entry is registered through `solidar_add_test`. The registration
boundary applies labels, a 120-second timeout, and the Windows Qt/OCCT runtime
path. Configuration fails if a direct `add_test` bypasses that boundary.

## Sanitizers

The `sanitizers` preset is a supported test profile. It instruments project
targets with AddressSanitizer on MSVC and with AddressSanitizer plus
UndefinedBehaviorSanitizer on GCC/Clang. Qt and OCCT remain the pinned binary
dependencies and are not themselves instrumented. Leak detection is disabled
because those libraries retain process-global allocations; memory safety and
undefined-behavior failures still stop the test immediately.
The sanitizer preset applies the same centralized timeout rule with a
300-second value because instrumentation makes the longest UI regression about
an order of magnitude slower; ordinary CI remains at 120 seconds.

Initialize the same vcpkg and Qt environment as the `ci` preset, then run:

```bash
cmake --preset sanitizers
cmake --build --preset sanitizers --clean-first
ctest --preset sanitizers
```

The Ubuntu `sanitizers` job in `.github/workflows/build.yml` runs this profile
for every push and pull request. MSVC x64 is also supported locally; 32-bit
MSVC and other compilers fail configuration with an explicit diagnostic rather
than silently producing an uninstrumented build.

## Static analysis

The opt-in profile uses the repository `.clang-tidy` checks and requires a
`clang-tidy` executable on `PATH`:

```bash
cmake --preset static-analysis
cmake --build --preset static-analysis --clean-first
```

Missing tooling is a configure-time error. Static analysis is kept separate
from the normal CI gate until the existing codebase has a reviewed warning
baseline; enabling it cannot change release compiler flags.

## Project decoder property and fuzz tests

`project_decoder_property_tests` is part of ordinary CI. It applies 192
fixed-seed byte mutations plus malformed boundary cases to a valid v2 file.
For every rejected input it verifies deterministic classification, a non-empty
diagnostic, unchanged destination `Document`, and unchanged input bytes.

The libFuzzer target is opt-in and supported with Clang's non-MSVC driver on
Linux. Initialize the reproducible Linux vcpkg/Qt environment and run:

```bash
cmake --preset fuzz
cmake --build --preset fuzz --clean-first --target project_decoder_fuzz
bash fuzz/run_project_decoder_fuzz.sh build/fuzz/fuzz/project_decoder_fuzz 60
```

The runner creates its mutable corpus and crash artifacts under a fresh system
temporary directory and prints that path. The harness uses a fixed filename
inside its own `QTemporaryDir`; fuzz bytes cannot select an output path. The
MSVC compiler profile is intentionally rejected because it does not provide
the libFuzzer driver used by this target.

## Performance profiles

Performance executables are opt-in and never registered with CTest:

```bash
cmake --preset performance
cmake --build --preset performance --clean-first
```

See `benchmarks/README.md` for the stage-specific runners and CSV schemas.
They report p50/p95/p99, memory, allocation and deterministic work counters.
Wall-clock thresholds are deliberately absent from ordinary CI until a stable,
dedicated measurement environment exists.

## Current coverage audit

| Scenario | Automated coverage | MVP status | Remaining boundary |
|---|---|---:|---|
| Document, IDs, Dirty/Valid/Error | `document_tests`, `parametric_history_tests` | Covered | Branching history, reorder and suppression are post-MVP. |
| Sketch geometry | `document_tests`, `sketch_regression_tests`, `project_file_tests` | Covered | Polygon, Slot, Text and sketch mirroring are post-MVP. |
| Sketch constraints | `sketch_regression_tests`, `sketch_constraint_integrity_tests` | Covered | Solver hardening remains a post-MVP priority. |
| Extrude and Pocket | Extrude regressions, `pocket_feature_tests`, `parametric_feature_chain_tests` | Covered | Manual GPU interaction remains part of the release gate. |
| Sketch-on-Face and topology references | `persistent_topology_tests`, `step_exchange_tests`, feature regressions | Covered with limits | Resolution uses semantic tags and geometric signatures, with legacy-index fallback only for old references. Split/merge and ambiguous symmetric topology remain documented limits. |
| Fillet and Chamfer | `fillet_feature_tests`, `chamfer_feature_tests`, `part_design_regression_tests` | Covered | Complex topology changes can require reselection. |
| Shell and Draft | `shell_feature_tests`, `shell_complex_topology_tests`, `draft_feature_tests` | Covered | Shell v1 intentionally rejects removed faces from several solids. |
| Revolve | `revolve_feature_tests`, `part_design_regression_tests` | Covered | Manual preview/manipulator verification remains. |
| Mirror, Move and patterns | Dedicated feature tests and `pattern_persistence_tests` | Covered | Arbitrary datum references, occurrence suppression and fusion are post-MVP. |
| Cascading recompute | `document_tests`, `parametric_feature_chain_tests`, `parametric_history_tests` | Covered | Independent-body and recovery invariants remain mandatory regressions. |
| Project save/load and v1 compatibility | `project_file_tests`, feature persistence regressions | Covered | Cached B-Rep is intentionally never serialized. |
| Rebuild diagnostics | `document_tests`, feature tests | Covered | Diagnostics must remain exception-safe at OCCT boundaries. |
| SBOM/compliance | `compliance_artifacts_tests`, `release_artifact_script_tests` | Covered | Vulnerability scanning and update diagnostics are post-MVP. |
| UI command smoke | UI-smoke tests | Covered headlessly | Real GPU, DPI and driver-specific behavior remain manual gates. |

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

Before the Windows x64 MVP release, perform the manual GPU gate on the target
Windows configuration. Repeat it on Ubuntu before advertising a supported
Linux binary; Ubuntu remains a CI-validated source-build target for this MVP.

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

1. Rectangle, circle and closed Line + Arc sketches followed by Extrude.
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
