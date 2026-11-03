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

The runner copies the checked-in minimal v1 and v5 seeds into a fresh mutable
corpus, creates crash artifacts under the same fresh system temporary root and
prints that path. Each input is scanned directly by the disconnected v5
scanner before the existing legacy `ProjectFile::stageLoad` file path runs.
The harness uses a fixed filename inside its own `QTemporaryDir`; fuzz bytes
cannot select an output path. The MSVC compiler profile is intentionally
rejected because it does not provide the libFuzzer driver used by this target.

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
| Persistent topology V1 | `persistent_topology_tests`, `step_exchange_tests`, feature regressions | Covered with V1 limits | Semantic tags/signatures resolve before legacy index; arbitrary split/merge and geometrically indistinguishable symmetry can remain `Missing`/`Ambiguous`. |
| Persistent topology V2 | Lineage/producer/consumer regressions, `runtime_topology_diagnostics_tests`, `project_format_v4_*_tests`, `project_application_service_tests`, `project_switch_ui_tests` | P1-P3 covered | Frozen provenance and strict runtime failures, staged v4 decode/materialize/recompute, atomic V4 writer, explicit format state and V3 native-V2 gate are covered. Это не закрывает весь этап 0.3: DocumentSketch migration, history reorder, branching и suppression остаются открытыми. |
| Headless planar mates | `assembly_mate_solver_tests`, public-header compliance, topology regressions | A7a core: localized solve, sealed private candidate, exact bounded production work plan, scale-invariant typed rank, strict V2 identity, authoritative ambiguity/reversal and transactional stale/rejection coverage | UI/history/persistence and additional mate families are intentionally deferred. |
| Fillet and Chamfer | `fillet_feature_tests`, `chamfer_feature_tests`, `part_design_regression_tests`, `edge_reference_v2_consumer_tests` | Covered | Они exact V2 consumers, но их P1 output намеренно terminal и не публикует lineage; сложная геометрия всё ещё может потребовать reselection через V1. |
| Shell and Draft | `shell_feature_tests`, `shell_complex_topology_tests`, `draft_feature_tests`, `edge_reference_v2_consumer_tests` | Covered | Они exact V2 consumers, но их P1 output намеренно terminal и не публикует lineage; Shell v1 отклоняет removed faces из нескольких solids. |
| Revolve | `revolve_feature_tests`, `part_design_regression_tests` | Covered | Manual preview/manipulator verification remains. |
| Mirror, Move and patterns | Dedicated feature tests and `pattern_persistence_tests` | Covered | Arbitrary datum references, occurrence suppression and fusion are post-MVP. |
| Dependency-graph scheduling and cascading recompute | `document_tests`, `parametric_feature_chain_tests`, `parametric_history_tests`, `feature_removal_tests` | Covered for G0 | Feature reorder, branching and suppression remain open. |
| Project save/load and v1 compatibility | `project_file_tests`, feature persistence regressions | Covered | Cached B-Rep is intentionally never serialized. |
| ProductDocument and single-part V5 | `product_document_tests`, `product_envelope_v5_*_tests`, `product_conversion_application_service_tests`, `product_clone_application_service_tests`, `project_switch_ui_tests`, decoder fuzz corpus | Implemented and CI-regression-covered; final A2 evidence open | Open/Save/Convert/Save As clone preserve graph, IDs and topology. A committed-state legacy-extrusion marker unconditionally fail-closes before chooser/prepare/writer/I/O; its migration is A2g. Recorded Linux Clang non-MSVC fuzz remains required. A2f still needs alias/volume/overwrite/dirty/delete/recovery/replica policy; A6a conflict enforcement, multiple definitions and assemblies remain open. |
| Rebuild diagnostics | `document_tests`, feature tests | Covered | Diagnostics must remain exception-safe at OCCT boundaries. |
| SBOM/compliance | `compliance_artifacts_tests`, `release_artifact_script_tests` | Covered | Vulnerability scanning and update diagnostics are post-MVP. |
| UI command smoke | UI-smoke tests | Covered headlessly | Real GPU, DPI and driver-specific behavior remain manual gates. |

`part_design_regression_tests` is the critical CAD-core gate. It deliberately
uses volumes, non-null shapes, feature states, parameters and stable model IDs
instead of OCCT face/edge ordering or exact topology counts.

`parametric_history_tests` is the scheduler-level regression gate. It verifies
targeted dirty propagation, upstream-to-downstream evaluation, current-result
gating with last-valid retention, explicit blocked diagnostics, recovery with
stable feature IDs, and continued recompute of independent bodies. The Part
Design regression test adds
the real Sketch -> Extrude -> Sketch-on-Face -> Pocket -> Fillet -> Chamfer path,
an independent Revolve body, and an edit after project save/load.

The G0 structural cases in `parametric_history_tests` remove a declared Feature,
remove a declared Sketch, and mismatch a face-support owner after a successful
build. They require deterministic root/blocked diagnostics, atomic scheduler
abort, retained last-valid geometry and revisions, unchanged independent Dirty
state, and same-ID recovery. `feature_removal_tests` is a regression-labelled
gate for dependency-closure removal and exact stale-plan rejection. Body vector
storage order is exercised independently of graph order; this does not claim or
test Feature history reorder, which remains unsupported together with branching
and suppression.

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

Assembly instancing adds two complementary gates. The automated
`assembly_render_scene_tests` and `viewport_renderer_tests` cover guarded CPU
publication, transactional late-second CPU/GPU failure, exact epoch/definition
source identity, three occurrences of a two-body part as two CPU/GPU sources
and six draws, placement-only reuse, per-occurrence tint, visibility and
last-user pruning, stale/empty publications, exact model matrices, exact GL
context refusal, idempotent release/reinitialization, and the unchanged legacy
framebuffer path. They use deterministic counters and failure checkpoints; no
runtime callback is installed.

The A4c1 additions to those same targets cover transactional local AABB
retention/reset, internal `TopLoc_Location`, identity/translation/Z90/arbitrary
world bounds, visible-only aggregate bounds, overflow rollback, the shared
CPU/renderer quaternion convention, and placement-only camera depth/Fit with
unchanged CPU/GPU source identities and upload generations. Pure Fit cases
exercise wide/tall logical sizes, multiple orientations, the 0.88 margin and
fixed anchor, deterministic repeated calls, both zoom clamps, invalid size and
NaN rejection, stale/empty publications, and DPR-independent projection.
Renderer regression checkpoints also simulate exact-context mismatch after
assembly preflight and after draw without callbacks. They verify unchanged
committed generation/source/camera/draw diagnostics, nonresident staged slots,
deferred wrapper ownership, and exactly-once cleanup on the next owner-context
entry.

The A4c2 `assembly_viewport_picking_tests` gate exercises exact CPU face and
edge ray picking against the immutable assembly scene: frontmost occlusion,
coplanar deterministic ties, arbitrary occurrence transforms and camera
orientations, hidden/suppressed exclusion, occurrence-scoped references,
same-surface edge acceptance, stale/foreign publication rejection, and
transactional topology/mapping failures. It also verifies that placement-only
republish reuses mesh/topology sources while rebuilding spatial instance data.
`viewport_renderer_tests` drives guarded occurrence highlighting from a real
picker hit and proves that stale or foreign targets paint no occurrence without
causing a GPU upload.

The A4c3 `assembly_viewport_spatial_tests` gate covers occurrence-scoped face
and edge rectangles, full/partial/coplanar global occlusion, edge boundary
contact with strictly-positive-area face filtering,
world-space Vertex/Edge/Surface snap and ruler distance, transformed
occurrences, yaw/pitch/pan/zoom and logical DPR invariance. It also checks
hidden/suppressed/body-hidden exclusion, placement-only topology reuse,
camera-only projection invalidation, bounded Vertex/Edge/Surface snap fallback,
same-number foreign publications,
stale/expired hits, invalid and degenerate input, and the transactional
`Uncertain` result when a work budget is exhausted. Public results contain
persistent face/edge addresses rather than flattened ordinals or mesh/OCCT
pointers.
Snap coverage also exercises a deliberately small valid near/far range:
out-of-clip vertices, edges, and surfaces are skipped without `InvalidInput`,
in-range alternatives win, all-outside queries miss, and both clip boundaries
match the A4c2 ray epsilon contract.

`assembly_gpu_smoke_classification_tests` is the pure automated guard for the
native gate classifier. The native run rejects case-insensitive software
renderer signatures (including GDI Generic, softpipe/llvmpipe/lavapipe and
SwiftShader) and non-visible Qt backends such as offscreen, minimal,
minimalegl, headless and linuxfb before it can print `REAL_GPU=PASS`.

The visible native `assembly_renderer_gpu_smoke` executable is intentionally
not registered with CTest and does not request software OpenGL or an offscreen
Qt platform. Build it with the normal configured tree, then run it from an MSVC
developer shell with the Qt and OCCT runtime directories on `PATH`:

```bat
cmake --build --preset ci --target assembly_renderer_gpu_smoke
build\ci\tests\assembly_renderer_gpu_smoke.exe
```

Treat the run as valid only when it prints a hardware `GL_VENDOR`,
`GL_RENDERER`, `GL_VERSION`, the expected Qt platform, and `PASS` for
`REAL_GPU`, `SHARED_MULTI_BODY`, `ONE_OCCURRENCE_TINT`,
`PLACEMENT_ORBIT_ZOOM_NO_UPLOAD`, `EPOCH_GEOMETRY_REUPLOAD`,
`LAST_USER_PRUNE`, `CONTEXT_TEARDOWN_RECREATE`, and `A4B_GPU_SMOKE`. A software
renderer warning/failure is not release evidence. Visually confirm the three
placements and the one-occurrence tint while orbit/zoom states are rendered;
the automated gate remains authoritative for exact counters and matrices, but
cannot replace this driver/context check.

For the A4c1 gate the same visible executable additionally must print `PASS`
for `TRANSFORMED_WORLD_BOUNDS`, `HIDDEN_SUPPRESSED_BOUNDS`, `FIT_TO_VIEW`,
`PLACEMENT_FIT_NO_UPLOAD`, and `A4C1_GPU_SMOKE`. Its initial and placement
frames use the actual pure Fit outputs; the placement frame must retain the
same GPU upload generations while changing aggregate bounds and camera state.

For the A4c2 gate it must retain every A4b/A4c1 marker and additionally print
`PASS` for `A4C2_FACE_PATH_IDENTITY`, `A4C2_FRONTMOST_OCCLUSION`,
`A4C2_EDGE_PICK`, `A4C2_INDEPENDENT_HIGHLIGHT`,
`A4C2_HIDDEN_SUPPRESSED_EXCLUDED`, `A4C2_ORBIT_ZOOM_PICK`,
`A4C2_PLACEMENT_NO_REMESH_UPLOAD`, and `A4C2_GPU_SMOKE`. The selected
occurrence in the visible frames must come from an actual current guarded CPU
hit; placement-only frames must change the hit location without remeshing or
uploading a new GPU source.

For the A4c3 gate it must retain every earlier marker and additionally print
`PASS` for `A4C3_RECT_PATH_IDENTITY`, `A4C3_RECT_OCCLUSION`,
`A4C3_SNAP_WORLD`, `A4C3_RULER_WORLD`,
`A4C3_HIDDEN_SUPPRESSED_EXCLUDED`,
`A4C3_PLACEMENT_NO_REMESH_UPLOAD`, `A4C3_LOGICAL_DPR_INVARIANT`, and
`A4C3_GPU_SMOKE`. These markers are computed from current guarded spatial
queries; the placement marker also requires the existing renderer upload and
topology-build counters to remain unchanged.

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

## A5a read-only assembly workspace

Run the focused automated gate after any workspace/session/tree change:

```bat
cmake --build --preset ci --target assembly_viewport_session_tests assembly_tree_model_tests project_switch_ui_tests --clean-first
ctest --test-dir build/ci -R "assembly_viewport_session_tests|assembly_tree_model_tests|project_switch_ui_tests" --output-on-failure
```

The session test covers empty and populated products, every deterministic
preparation failure seam, exact publication identity (including a foreign
coordinator with equal numeric epoch/generation), and hidden/body-hidden
highlight exclusion. The tree-model test covers deterministic typed structure,
orphan definitions, Russian status labels, occurrence lookup, and the complete
read-only model contract. `project_switch_ui_tests` covers part-to-assembly,
assembly-to-part, assembly-to-assembly with repeated local IDs, atomic rollback
for part and assembly incumbents, an active SketchCanvas/reference-image/
manipulator replacement seam, typed tree/viewport synchronization, stale and
same-number-foreign publication rejection, programmatic hidden-widget callback
guards, the disabled New action/direct slot, action state, and editor-factory
acceptance.

The native visible A5a gate is not evidence when run with an offscreen or
software backend. For manual evidence, use the real-driver harness and record
the platform scale as reported by that process; do not describe a logical-size
matrix as native DPR coverage. At 1920x1080, 1536x864, 1280x720, and 960x540,
check that the Russian read-only ribbon/tree labels fit, the timeline stays
hidden, Fit/orbit/zoom work, hidden/suppressed rows cannot highlight, and no
mutation command becomes enabled.

```bat
cmake --build --preset ci --target assembly_workspace_gpu_smoke
build\ci\tests\assembly_workspace_gpu_smoke.exe
```

The run is valid only when it reports hardware `GL_VENDOR`/`GL_RENDERER`, a
visible Qt platform, `A5A_NATIVE_SCALE=<actual process DPR>`, and `PASS` for
`A5A_OPEN_READ_ONLY`, `A5A_COMPLETE_SCENE_FIRST_COMMIT`,
`A5A_TREE_TO_VIEWPORT_SELECTION`, `A5A_VIEWPORT_TO_TREE_SELECTION`,
`A5A_SWITCH_PART_ASSEMBLY`, `A5A_SWITCH_ASSEMBLY_ASSEMBLY`,
`A5A_HIDDEN_SUPPRESSED`, `A5A_TIMELINE_HIDDEN`,
`A5A_NO_MUTATION_COMMANDS`, `A5A_LAYOUT_MATRIX`, and the conjunctive
`A5A_GPU_SMOKE`. The four window sizes are logical layout checks only and do not
claim four native scale factors.

## A5b Insert transaction

`assembly_insert_placement_session_tests` covers canonical move/rotation,
non-finite rejection with last-valid retention, CommitPending re-entry guards,
idempotent Cancel and the two-step nested Escape contract.
`assembly_command_history_tests` covers paired definition/occurrence Apply,
prepare-before-commit rollback, the unified cold command variant, guarded Undo,
stable-ID/placement Redo reconstruction, runtime-shape expiry, revision/
savepoint matrices, bounded retained bytes, re-entrancy and cursor stability on
failed successor preparation. It also covers A5c0 empty-delta Apply/Revert as a
typed `NoChange`, complete root/path/target/incarnation guards, redo-tail
retention on no-op, and private two-step forward/reverse rollback injection.
`assembly_command_history_public_headers_compile_tests` compiles the public
history/delta boundary without Qt or OCCT include/link assistance. It checks
the unique exact typed-reference signatures with `decltype(&method)` (so a
parallel legacy overload fails compilation) and scans the headers with
whitespace-tolerant patterns for output pointers, raw callbacks/context,
QObject, runtime geometry/GPU, and filesystem carriers. Resource-failure
regressions compare exact payload `Document` address/fingerprint, definition
and occurrence node identity/all fields, product root/incarnation/identity,
and every public history cursor/revision/savepoint/selection proxy.
`assembly_insert_application_service_tests` covers an in-memory empty assembly,
embedded-part staging without source-file mutation, assembly-source rejection
and corrupt input.

Run the focused gate with a clean rebuild after header changes:

```bat
cmake --build --preset ci --target assembly_insert_placement_session_tests assembly_command_history_tests assembly_command_history_public_headers_compile_tests assembly_insert_application_service_tests project_switch_ui_tests assembly_workspace_gpu_smoke --clean-first
ctest --test-dir build/ci -R "^(assembly_insert_(placement_session|application_service)_tests|assembly_command_history(_public_headers_compile)?_tests|project_switch_ui_tests)$" --output-on-failure
```

`project_switch_ui_tests` additionally covers a failed successor projection
with exact graph/tool/ghost rollback, Apply/Undo/Redo publication refresh,
runtime-scene expiry, failed Open preservation, Cancel and successful
replacement teardown. The visible native harness retains all A5a evidence and
adds `A5B_NATIVE_SCALE=<actual process DPR>` plus `PASS` for
`A5B_NEW_ASSEMBLY`, `INSERT_GHOST`, `MOVE_ROTATE_GHOST`, `APPLY_ONE_UNDO`,
`CANCEL_EXACT_RESTORE`, `ESCAPE_NESTED_THEN_TOOL`,
`FAILED_STAGE_PRESERVES_PREVIEW`, `FAILED_OPEN_PRESERVES_PREVIEW`,
`REPLACEMENT_TEARDOWN_ORDER`, `UNDO_REDO_STABLE_IDS`, `HIDDEN_SUPPRESSED`,
`SELECTION_PUBLICATION_REFRESH`, `LAYOUT_MATRIX`, and the conjunctive
`A5B_GPU_SMOKE`. As with A5a, compiling the harness is not native GPU evidence;
the marker set is valid only from a visible real-driver run.

## A5c1 Rename occurrence

`assembly_command_history_tests` covers the strict UTF-8/name boundary,
address/current-value guards, exact only-name Apply/Undo/Redo, no-op redo-tail
preservation, retained-byte accounting, resource-failure rollback, two
occurrences sharing one definition, and the Insert→Rename chain including the
stale Insert-Undo guard. `assembly_tree_model_tests` checks authored Russian and
emoji labels, deterministic unnamed fallback and immutable tree data.
`product_envelope_v6_encoder_tests` proves that V6 fails closed instead of
silently dropping an authored name. `project_switch_ui_tests` exercises ribbon,
tree-menu and F2 entry points, modal Apply/Cancel, disabled/live-tool states,
replacement cancellation, tree-successor failure, and stable viewport session,
publication epoch, selection and scene across Rename/Undo/Redo.

The visible harness adds `A5C1_NATIVE_SCALE`, `A5C1_ENTRYPOINTS`,
`A5C1_APPLY_CANCEL`, `A5C1_UNDO_REDO`, `A5C1_ONLY_NAME`, `A5C1_DISABLED`,
`A5C1_SELECTION_PUBLICATION_STABLE`, `A5C1_NO_UPLOAD`,
`A5C1_REPLACEMENT_TEARDOWN`, `A5C1_LAYOUT_MATRIX`, and the conjunctive
`A5C1_GPU_SMOKE`. Offscreen execution is only a bounded functional transport;
only a visible native real-driver run can satisfy the conjunctive GPU marker.

## A5c2 Visibility and isolate

`assembly_command_history_tests` verifies exact visible-bit Apply/Revert,
current-value/address guards, resource rollback, one-entry Undo/Redo and
redo-tail preservation. `assembly_viewport_session_tests` covers same-epoch
fresh publication, shared CPU source reuse, hidden bounds/spatial exclusion,
same-publication isolate/show-all, effective-visibility rejection and retained
base sources. `assembly_tree_model_tests` checks authored hidden/suppressed
roles separately from `изолирован` and `временно скрыт (изоляция)`.
`project_switch_ui_tests` exercises ribbon/context/Space/Escape eligibility,
selection/highlight rebinding, camera preservation, history disabling during
isolate, failed Open preservation and successful replacement teardown.
`assembly_visibility_ui_failure_tests` owns the deterministic successor matrix:
it rejects presentation, scene, picking, spatial, selection,
resource/finalization and tree-resource preparation for Execute, Undo and
Redo, then compares the complete product/history/publication/tree/interaction/
camera/renderer state. Isolate has separate scene, picking, resource and tree
failure seams and proves exact no-op both before isolation and while an
existing isolation remains active.

The native harness adds `A5C2_NATIVE_SCALE`, `A5C2_ENTRYPOINTS`,
`A5C2_VISIBILITY_UNDO_REDO`, `A5C2_ONLY_VISIBLE`,
`A5C2_HIDDEN_SPATIAL_EXCLUDED`, `A5C2_SHARED_SOURCE_NO_REMESH_UPLOAD`,
`A5C2_LAST_USER_PRUNE_REACQUIRE`, `A5C2_ISOLATE_SHOW_ALL`,
`A5C2_ISOLATE_PRESENTATION_ONLY`, `A5C2_ISOLATE_NO_UPLOAD_PRUNE`,
`A5C2_REPLACEMENT_TEARDOWN`, `A5C2_LAYOUT_MATRIX` and conjunctive
`A5C2_GPU_SMOKE`. Offscreen output is functional evidence only; native GPU
evidence still requires a visible real-driver context.

`assembly_viewport_session_tests` runs actual `faceAt`, rectangle, `snapAt`,
ruler and fit queries over separated visible peers while keeping separate
authored-hidden, suppressed and body-hidden peers excluded. The native
`A5C2_HIDDEN_SPATIAL_EXCLUDED` and isolate markers run the same consumer set
against the occurrence hidden or isolated by the live UI. The classifier is
conjunctive for every consumer, so stale publication, rectangle, snap, ruler
or fit evidence makes the marker fail; instance counts alone cannot pass it.

## A5c3 Ground / un-ground

`assembly_command_history_tests` covers exact bit-only Apply/Revert,
current-value/resource rollback, unified-history Undo/Redo/no-op, grounded
placement rejection, successful placement after Un-ground and a reentrant
successor receiving `Busy`. `assembly_viewport_session_tests` proves the
same-epoch/fresh-publication grounding projection changes only grounded
metadata while reusing exact source meshes, transforms, bounds and picking
membership. `assembly_visibility_ui_failure_tests` runs every deterministic
projection and tree-resource failure point through Execute, Undo and Redo and
compares the full product/history/tree/publication/selection/camera/renderer
state. Tree tests keep the appended authored grounded role distinct from
visibility, suppression and isolation roles; existing V6 encoder/materializer
tests retain exact grounded flags without a schema change.

The native harness adds `A5C3_NATIVE_SCALE`, `A5C3_ENTRYPOINTS`,
`A5C3_GROUND_UNGROUND`, `A5C3_UNDO_REDO`, `A5C3_ONLY_GROUNDED`,
`A5C3_PLACEMENT_GUARD`, `A5C3_VISIBILITY_SUPPRESSION_INDEPENDENT`,
`A5C3_SELECTION_PUBLICATION_REFRESH`, `A5C3_NO_REMESH_UPLOAD`,
`A5C3_INSERT_CHAIN`, `A5C3_ISOLATE_DISABLED`,
`A5C3_REPLACEMENT_TEARDOWN`, `A5C3_LAYOUT_MATRIX` and conjunctive
`A5C3_GPU_SMOKE`. Offscreen runs validate functional marker transport only;
real GPU evidence still requires a visible hardware-driver context.

## A5c4 Suppress / unsuppress

`assembly_command_history_tests` covers exact bit-only Apply/Revert,
current-value/resource rollback, unified-history Undo/Redo/no-op and the
legacy whole-flags reentrancy guard returning `Busy`.
`assembly_viewport_session_tests` proves the same-epoch/fresh-publication
suppression projection excludes the occurrence while retaining its semantic
selection without a highlight, preserves neighbouring authored bits and runs
every projection failure seam. `assembly_visibility_ui_failure_tests` executes
the full projection/tree failure matrix for Apply, Undo and Redo against exact
product/history/tree/publication/selection/camera/renderer snapshots.
`project_switch_ui_tests` covers ribbon, tree context and non-ambiguous
Shift+Space versus Space entrypoints, dynamic text, selection rebinding and
isolate disable. Existing V6 tests retain exact suppressed flags without a
schema change.

The native harness adds `A5C4_NATIVE_SCALE`, `A5C4_ENTRYPOINTS`,
`A5C4_SUPPRESS_UNSUPPRESS`, `A5C4_UNDO_REDO`, `A5C4_ONLY_SUPPRESSED`,
`A5C4_VISIBILITY_GROUNDED_INDEPENDENT`,
`A5C4_SUPPRESSED_SPATIAL_EXCLUDED`,
`A5C4_SHARED_SOURCE_NO_REMESH_UPLOAD`,
`A5C4_LAST_USER_PRUNE_REACQUIRE`,
`A5C4_SELECTION_PUBLICATION_REFRESH`, `A5C4_INSERT_CHAIN`,
`A5C4_ISOLATE_DISABLED`, `A5C4_REPLACEMENT_TEARDOWN`,
`A5C4_LAYOUT_MATRIX` and conjunctive `A5C4_GPU_SMOKE`. Shared-source evidence
uses one definition with two separated occurrences; last-user evidence forces
a rendered frame before inspecting prune/upload counters. Offscreen validates
functional transport only, never native GPU evidence.

## A5c5 Duplicate occurrence

`assembly_command_history_tests` covers lowest-free-ID allocation, exact cold
payload guards, allocation/node/successor/finalization failures, shared
Document/B-Rep identity, duplicate-of-duplicate, stable-ID Undo/Redo and mixed
Insert/Rename/Visibility/Ground/Suppress history. `assembly_viewport_session_tests`
proves fresh same-epoch publication, selection rebinding, semantic-only hidden
copies, shared mesh/topology identity, zero topology rebuild attempts, and a
real rectangle query returning both exact current-publication identities for
coincident source/duplicate occurrences. It also covers every projection
failure seam.
`assembly_visibility_ui_failure_tests` runs Execute/Undo/Redo projection and
tree failures against exact product/history/session/tree/camera/renderer
snapshots. `project_switch_ui_tests` covers ribbon, tree context and Ctrl+D,
redo-tail replacement, selection, renderer reuse and isolate disabling. V6
tests prove two unnamed occurrence IDs restore against one Part/Document while
named occurrences remain fail-closed.

The native harness adds `A5C5_NATIVE_SCALE`, `A5C5_ENTRYPOINTS`,
`A5C5_NEW_STABLE_OCCURRENCE_ID`, `A5C5_SAME_DEFINITION_DOCUMENT_BREP`,
`A5C5_EXACT_PLACEMENT_NAME_FLAGS`, `A5C5_SELECTION_APPLY_UNDO_REDO`,
`A5C5_HIDDEN_SUPPRESSED_GROUNDED`,
`A5C5_SAME_EPOCH_PUBLICATION_REFRESH`,
`A5C5_SHARED_SOURCE_NO_REMESH_UPLOAD`, `A5C5_INSTANCE_DRAW_PICKING`,
`A5C5_TOPOLOGY_REUSE`,
`A5C5_INSERT_RENAME_ISOLATE_DISABLED`, `A5C5_FAILURE_ATOMICITY`,
`A5C5_REPLACEMENT_TEARDOWN`, `A5C5_LAYOUT_MATRIX` and conjunctive
`A5C5_GPU_SMOKE`. On a native surface the draw marker requires an actual frame
with an exact +1 submitted/drawn instance delta; picking requires both exact
identities, and topology reuse requires zero candidate rebuilds. Offscreen
runs validate every functional marker but cannot claim the real-GPU conjunct.

## A5c6 Remove occurrence / Undo

`assembly_command_history_tests` covers exact cold extraction/restoration,
allocation/node/successor failures, ID-reuse collision, Busy reentrancy,
stable-ID Undo/Redo, and retention of the PartDefinition, Document, and B-Rep.
`assembly_viewport_session_tests` verifies the fresh same-epoch remove
projection, selection clear/restore, shared-source reuse, hidden semantic-only
removal, and all projection failure seams. `assembly_tree_model_tests` covers
pre-existing and newly orphaned definition rows. UI failure tests exercise
Execute/Undo/Redo against exact product/history/publication/camera/renderer
snapshots; project-switch tests cover ribbon, context, tree-scoped Delete,
Part Delete non-collision, empty roots, and replacement. V6 tests round-trip
an empty assembly with retained orphan Part definitions.

The native harness adds `A5C6_NATIVE_SCALE`, `A5C6_ENTRYPOINTS`,
`A5C6_REMOVE_UNDO_REDO`, `A5C6_ONLY_OCCURRENCE_NODE`,
`A5C6_ORPHAN_DEFINITION_RETAINED`, `A5C6_ORPHAN_TREE_FEEDBACK`,
`A5C6_LAST_OCCURRENCE_EMPTY_ROOT`, `A5C6_HIDDEN_SUPPRESSED_GROUNDED`,
`A5C6_SELECTION_CLEAR_RESTORE`, `A5C6_SAME_EPOCH_PUBLICATION_REFRESH`,
`A5C6_SHARED_SOURCE_NO_REMESH_UPLOAD`, `A5C6_LAST_USER_PRUNE_REACQUIRE`,
`A5C6_INSTANCE_DRAW_PICKING`, `A5C6_UNDO_COLLISION_ROLLBACK`,
`A5C6_INSERT_DUPLICATE_FIELDS_CHAIN`, `A5C6_EMPTY_CAMERA`,
`A5C6_TOPOLOGY_PRUNE_REACQUIRE`, `A5C6_ISOLATE_DISABLED`,
`A5C6_FAILURE_ATOMICITY`, `A5C6_REPLACEMENT_TEARDOWN`,
`A5C6_LAYOUT_MATRIX`, and conjunctive `A5C6_GPU_SMOKE`.
The chain marker is produced by one real unified
Insert→Duplicate→Rename→Visibility→Ground→Suppress→Remove history followed by
all Undo/Redo boundaries. The two independent last-user markers additionally
prove the empty-root camera contract and topology resident prune/rebuild with a
current-publication picking hit; the classifier flip-tests each bit.

## A5d Embedded definition edit context

`assembly_command_history_tests` covers the fifth cold command alternative,
stable authoritative `Document` address, definition-state guards, successful
recompute, Apply/Undo/Redo through the single assembly cursor, re-entrant
registry/document mutation rejection, and the full before-recompute,
before-commit, successor and history-finalization rollback matrix. Exact
snapshots include the state token, B-Rep handles, command cursor/revisions,
savepoint and retained bytes. The public-header test
keeps `DocumentDelta` and `DefinitionEditColdCommand` free of live Document,
B-Rep, Qt, publication and callback ownership, and proves the sole embedded
document lookup is const-only.

`project_switch_ui_tests` enters through a PartDefinition row, verifies the
Russian breadcrumb/Back navigation, keeps `ModelCommandHistory` empty, commits
the actual Move ribbon command through body selection, numeric parameters and
Apply into assembly history, performs Undo/Redo both inside and outside the
edit context, and proves stable embedded-document address, same presentation
epoch, fresh publication, definition-row selection and exact camera
restoration. The same real UI path injects candidate recompute failure and
proves the authoritative fingerprint, B-Rep handle, definition-state token,
history cursor/revision/savepoint, dirty state and publication remain exact.
Save/Save As/import/export remain disabled. Successful project replacement
clears active Part tools, sketch workspace, previews, queued callbacks, the
context and suspended session before the ProductDocument swap. Back refuses
to navigate until the edit workspace is quiescent. Session regression uses
two definitions and multiple occurrences to prove same-epoch/fresh-
publication projection, one rebuild for the changed source and exact reuse of
the unchanged source.
`viewport_renderer_tests` independently detaches an assembly into the
definition-edit suspension state, runs an empty Part resource synchronization,
and verifies that resume preserves the exact source upload generations and
fingerprints without another upload.
The projection oracle records every edited occurrence identity/path,
local-to-world transform, authored flags and name, proves both occurrences use
one new mesh/topology source, and verifies an unaffected definition retains
its exact mesh, topology, identity and transform. An orphan definition is
entered through the real row action and edited through the same Move UI with
Apply/Undo/Redo while the root occurrence count remains unchanged.

Sketch authoring remains deliberately unported in A5d. The lifecycle test
therefore clicks the real disabled Sketch command and proves no working copy
is created; standalone Sketch replacement tests remain the baseline. A5d
replacement itself is exercised with a real active Move and pending preview,
including cancellation-before-document-destruction trace and stale callback
suppression.

The native harness adds `A5D_NATIVE_SCALE`,
`A5D_ENTRY_RETURN_BREADCRUMB`, `A5D_ONE_OWNER_ONE_HISTORY`,
`A5D_SHARED_EDIT_ALL_OCCURRENCES`, `A5D_IDS_PLACEMENTS_FLAGS_STABLE`,
`A5D_FAILED_RECOMPUTE_ROLLBACK`, `A5D_FAILED_PROJECTION_ROLLBACK`,
`A5D_UNDO_REDO_SHARED`, `A5D_TOOL_REPLACEMENT_TEARDOWN`,
`A5D_EDITED_SOURCE_ONE_UPLOAD`, `A5D_OTHER_SOURCES_REUSED`,
`A5D_HIDDEN_SUPPRESSED_STABLE`, `A5D_SELECTION_CAMERA_RESTORE`,
`A5D_ORPHAN_DEFINITION_EDIT`, `A5D_LAYOUT_MATRIX` and conjunctive
`A5D_GPU_SMOKE`. Offscreen runs require every functional bit but never claim
the real-GPU conjunct; native evidence requires one upload for the changed
definition source while the incumbent unrelated source generation/fingerprint
remains exact. Expected failed Apply paths are closed through the real Move
Cancel control before Back is tested, so quiescence is evidence rather than a
friend-helper shortcut.

## A6a read-only linked resolver

`linked_part_source_resolver_tests` uses a capability-limited fake filesystem
to exercise Windows and POSIX lexical/containment semantics, safe Missing
versus escape, network/device rejection before any byte read, exact/limit+1
root/path/component/file/aggregate/alias/work budgets, source-change handling,
identity mismatch, outdated hashes, permutation-independent identity conflict,
same-file read dedupe, equal-content replica decode dedupe, V1--V4 no-identity
classification, future-V5/V6 rejection and late-materialization atomic failure.
Inspect/read/decode counters make the ordering and dedupe claims observable.
The fake exposes no scan, write, sidecar, URL or network operation.
An injectable native-root seam additionally proves that invalid lexical input
performs zero root/target calls, root network/loop/case-policy failures perform
zero target calls, and root plus target alias/work charges share exact and
limit+1 budgets. A nonzero rebuild fixture covers its exact and limit+1 edge.
On Windows, a deterministic syscall-backend drives the production component
walker itself: local reparse-to-UNC rejection precedes any target open,
repeated file identity reports a loop, depth and complete syscall work use
exact/limit+1 boundaries, and case-sensitive directories fail closed. A native
exact-work integration includes held `ReadFile` and its final metadata query.
Throwing-backend cases inject `std::bad_alloc` during canonical-name lookup and
immediately after successful canonicalization; the resolver reports resource
failure and every opened walker token is closed exactly once.

`linked_part_source_resolver_native_tests` reads a hand-authored V5 file through
the native held-handle boundary and verifies exact bytes, modification time and
directory entries are unchanged; it also covers native Missing and directory
non-regular rejection.  Symlink coverage is conditional with an explicit skip
when Windows does not grant link-creation privilege, while the fake coverage
remains deterministic.  The public-header compile target keeps the staged
result move-only and the external identity strongly typed.  These tests
do not imply V6 round-trip, Reload/Repair, nesting or UI publication; those
remain later gates.

## A6b0 strict linked assembly envelope V7

`product_envelope_v7_tests` covers deterministic embedded+linked round-trip,
external/local definition identity separation, original locator/hash binding,
the mandatory last-valid V4 cache, authored occurrence name/placement/flags,
disconnected atomic materialization and encoder self-scan. Детерминированный
failure seam пропускает успешные materialize/recompute первой детали, затем
реальные materialize и успешный `recomputeDetailed()` второй детали, и только
после этого отклоняет staged результат до acceptance/registry publication;
candidate, Product incarnation и Body ID cursors остаются неизменными.
Negative mutations
exercise exact fields/capabilities, newer version handling, definition order,
dangling targets, lowercase SHA-256, lexical path escape, missing cache, exact
budget arithmetic and late candidate tampering. The focused compatibility gate
also runs existing V6 golden and A6a resolver tests. No V7 `ProjectFile`/UI
route is enabled yet. V7 scanner остаётся отдельным disconnected API, поэтому
общий `ProjectFile` decoder/fuzz route его пока не вызывает; corpus integration
отложена до явного подключения этого route.

## A6b1 explicit Reload and Repair Link

`linked_part_source_resolver_tests` keeps exact resolution fail-closed on a
changed hash while proving that the explicit Reload intent accepts that
revision only after exact identity validation. The Repair probe returns the
actual hash and no materialized product. Both lifecycle intents retain the
same identity, conflict, containment, no-network and budget behavior.

`assembly_command_history_tests` covers linked-only model guards, rejection of
ordinary definition edits, definition-wide Reload for two stable occurrences,
locator-only Repair, and a real V2 persistent-face consumer that re-resolves
after changed upstream geometry without a raw face ordinal. A deliberately
broken persistent provenance is a no-op with exact Document/B-Rep/binding/
state/projection/history preservation. Reload and Repair each exercise Undo
and Redo rollback at model resource, successor rejection/resource and history
finalization seams. The linked retained-byte gate accepts exactly 64 MiB and
rejects limit+1 without truncating an existing redo tail or moving its
cursor/savepoint/retained-byte state.
`linked_part_lifecycle_application_service_tests` exercises resolver-to-history
Reload and Repair end to end, proves Undo/Redo after the source disappears
performs no further reads on success or successor failure, keeps an outdated
Repair cache/hash unchanged,
rejects Missing and wrong identity as exact no-ops, and round-trips the
resulting linked state through the disconnected V7 codec.

## A6b2 V7 editor persistence and diagnostics

`project_application_service_tests` covers exact V7/V8 dispatch, last-valid
materialization before exact linked resolution, Exact/Outdated/Missing runtime
states without implicit acceptance, first Save and existing Save, same-directory
Missing Save As, cross-directory fail-closed behavior, destination/source alias
rejection, V7 round-trip and strict full-product export preflight. A narrow
read-only metadata seam deterministically covers file-identity aliases,
canonical/reparse aliases, cross-directory common-source success and injected
metadata-query failure; the latter preserves target/source bytes, identity and
history state exactly. Native hardlink/symlink cases remain supplemental when
the host grants link privilege. The V7 atomic writer tests use the common
transaction seam for success, short-write rollback and unopened invalid
destinations.

`project_switch_ui_tests` opens real V7 linked assemblies and checks the Russian
tree diagnostics and lifecycle actions. A changed source remains a clean
Outdated cache until explicit Reload; Reload updates one definition shared by
two stable occurrences while preserving authored names/flags and occurrence
selection. After deleting the source, Undo/Redo restore both cold revisions,
reset status to Unchecked and preserve the assembly-history savepoint/dirty
semantics. The same test records source bytes, mtime and directory entries to
prove Reload has no write cascade, and verifies malformed V7 Open is an exact
no-op on the live product, path, history and publication. Actual assembly
STEP/STL writers are intentionally absent: both actions stay disabled with the
A9 diagnostic. `assembly_tree_model_tests` covers the application-owned Russian
status labels, including orphan linked definitions.

The opt-in native `assembly_workspace_gpu_smoke` now emits
`A6B2_GPU_SMOKE=PASS` only after a real-driver V7 linked lifecycle scenario has
published one shared GPU source for two visible occurrences, performed explicit
Reload and cold Undo/Redo after source removal, and verified replacement of the
tree/session/publication/source plus retirement of every preceding scene/mesh.
The occurrence IDs, authored fields, selection and hover stay stable, while
each geometry revision builds and uploads exactly one shared source. As with the
earlier native markers, the final runner repeats this executable at the required
DPR matrix; the scenario itself contains no pixel-coordinate assumptions.

The focused A6b2 gate is:

```bat
cmake --build --preset ci --clean-first --target product_envelope_v7_tests project_application_service_tests linked_part_lifecycle_application_service_tests assembly_command_history_tests assembly_tree_model_tests project_switch_ui_tests
ctest --test-dir build/ci -R "^(product_envelope_v7_tests|project_application_service_tests|linked_part_lifecycle_application_service_tests|assembly_command_history_tests|assembly_tree_model_tests|project_switch_ui_tests)$" --output-on-failure
```

The always-on marker classifier and the opt-in native executable are built with:

```bat
cmake --build --preset ci --clean-first --target assembly_gpu_smoke_classification_tests assembly_workspace_gpu_smoke
ctest --test-dir build/ci -R "^assembly_gpu_smoke_classification_tests$" --output-on-failure
```

## A6c0 nested graph and disconnected V8

`assembly_graph_analysis_tests` covers direct, indirect and orphan cycles,
shared-diamond DAGs, deterministic full paths, non-commutative three-level
transform composition, ancestor visibility/suppression, authored grounded
semantics, dangling/root failures, and exact/limit+1/overflow checks for every
central graph budget, orphan source accounting and the pre-path-allocation
resource-failure seam. `product_envelope_v8_tests` starts from a literal
hand-authored V8/V4 fixture that never passes through the encoder. It covers
deterministic self-scan/roundtrip/materialization, linked terminal parts,
exact and limit+1 definition/occurrence/depth/work/leaf/source/string/file/
locator-byte/locator-component/rebuild-work boundaries, arithmetic overflow,
the aggregate payload arithmetic ceiling (valid payloads meet tighter
string/schema/work limits first),
strict quaternion canonicality (literal `-0`, `-0.0`, `-0e0` in every
component and both nested transform contexts), every missing/extra schema
field, capability/order/duplicate/reference/cycle/identity failures, and
unchanged V7 rejection of nested products. Its deepest-part late-failure case
snapshots Body/Sketch/Feature/Product incarnation cursors, the candidate and an
existing product byte-for-byte. Public model/V8 headers compile in the
compliance targets.

The focused A6c0 gate is:

```bat
cmake --build --preset ci --clean-first --target assembly_graph_analysis_tests product_envelope_v8_tests model_public_headers_compile_tests assembly_definition_tests product_envelope_v7_tests
ctest --test-dir build/ci -R "^(assembly_graph_analysis_tests|product_envelope_v8_tests|model_public_headers_compile_tests|assembly_definition_tests|product_envelope_v7_tests)$" --output-on-failure
```

No editor V8 route or native nested GPU scenario belongs to this microgate;
those are A6c1b.

## A6c1a recursive presentation/tree/session identity

The focused tests extend common graph analysis with intermediate and leaf
visits, same-local-ID/different-prefix paths, deterministic permutation,
non-commutative transforms and inherited visibility/suppression. Presentation
proves one source with multiple nested instances. Tree/session/picking tests
cover hierarchy, orphan part/assembly rows, full-path selection/hover,
terminal isolation, disabled intermediate isolation, ray/rectangle/snap and
highlight identity. History covers full-path restoration and the exact
64 MiB/+1 retained-byte boundary including path-vector capacities, plus an
injected semantic-state `bad_alloc` before Execute/Undo/Redo proving exact
model/history/cursor/savepoint/redo/publication no-op. MainWindow coverage
includes a dedicated full-projection selection-allocation failure after the
candidate workspace is built, plus exact root/non-root `AssemblyDefinition`
rows through FullProjection and command Undo/Redo. Insert history additionally
injects payload-copy failure with a maximum-size valid authored name before
Undo/Redo touches the model. DefinitionEdit and Linked Reload inject the same
payload-preparation failure in both Undo and Redo, checking exact model/B-Rep,
history cursor/savepoint/redo/retained bytes, successor and full-path selection
no-op. Linked lifecycle covers end-to-end MainWindow
nested Reload and path-only Repair against a same-key newer hash, resource
rollback, and cold no-filesystem Undo/Redo with the full selected leaf path and
runtime status reset to `Unchecked`.

The focused A6c1a gate is:

```bat
cmake --build --preset ci --clean-first --target assembly_graph_analysis_tests assembly_definition_tests assembly_presentation_builder_tests assembly_presentation_public_headers_compile_tests assembly_viewport_picking_tests assembly_viewport_session_tests assembly_tree_model_tests assembly_command_history_tests assembly_command_history_public_headers_compile_tests linked_part_lifecycle_application_service_tests assembly_visibility_ui_failure_tests project_switch_ui_tests
ctest --test-dir build/ci -R "^(assembly_graph_analysis_tests|assembly_definition_tests|assembly_presentation_builder_tests|assembly_presentation_public_headers_compile_tests|assembly_viewport_picking_tests|assembly_viewport_session_tests|assembly_tree_model_tests|assembly_command_history_tests|assembly_command_history_public_headers_compile_tests|linked_part_lifecycle_application_service_tests|assembly_visibility_ui_failure_tests|project_switch_ui_tests)$" --output-on-failure
```

V8 editor routing and the final native nested multi-DPR GPU marker are not
A6c1a evidence; they remain A6c1b.

## A6c1b V8 editor lifecycle and native recursive GPU evidence

`project_application_service_tests` now opens actual V8 bytes and covers exact
V6/V7/V8/future dispatch, Exact/Missing/Outdated/IdentityMismatch/security/
invalid-source/resource/source-changed diagnostics on `lastValid`, multiple
linked failures without a false Exact state, and hard rejection of unsafe
locators, invalid schema and invalid serialized caches. It also covers
recursive `[1,7]`/`[2,7]` identity, V8 Save and Save As round trips, format
preservation and post-publication identity forking. The common V7/V8 Save As
metadata seam checks missing leaves below aliased physical parents, multi-level
suffixes, raw mixed-separator/drive/UNC `.`/`..` rejection before metadata,
drive-relative `C:foo`/`C:..\foo`/bare-drive rejection, ASCII case and NFC/NFD
Unicode aliases under the insensitive policy, NFC/NFD equivalence with case
retention under the macOS-style normalized-sensitive policy, exact POSIX/Linux
differences and fail-closed unknown/error results. A
conditional native directory-symlink supplement covers both a missing suffix
and a `..` spelling without opening the writer. Full-product export preflight visits reachable,
orphaned and suppressed linked definitions; Exact passes, every diagnostic
class blocks before output, and a source change after production Reload is
detected. No assembly exporter is invoked.
`project_switch_ui_tests` opens V8 through MainWindow, preserves the full leaf
path through definition-wide Reload and locator-only Repair, performs cold
Undo/Redo after source removal with status `Unchecked`, retains redo across V8
Save As, and checks source bytes/mtime remain unchanged.

`product_envelope_v8_tests` additionally drives the V8 writer preparation
seams (encode, self-scan and disconnected materialization) before transaction
open and the common open/write/flush/sync/replace failure matrix after open;
every failure preserves the sentinel destination. The fuzz entry point and
seed corpus include a valid nested V8 envelope.

The always-on classifier flips every field of `A6c1GpuSmokeEvidence`
individually. The opt-in `assembly_workspace_gpu_smoke` emits
`A6C1_GPU_SMOKE=PASS` only when production V8 Open, shared recursive paths,
interaction/isolation, ancestor filtering, cold linked lifecycle, exact upload
counts and resource retirement are all true. One local native sample is
sufficient during implementation; the final runner supplies the required DPR
matrix.

The focused A6c1b gate is:

```bat
cmake --build --preset ci --clean-first --target product_envelope_v8_tests product_envelope_v8_public_headers_compile_tests project_application_service_tests project_switch_ui_tests assembly_gpu_smoke_classification_tests assembly_workspace_gpu_smoke
ctest --test-dir build/ci -R "^(product_envelope_v8_tests|product_envelope_v8_public_headers_compile_tests|project_application_service_tests|project_switch_ui_tests|assembly_gpu_smoke_classification_tests)$" --output-on-failure
```

The native executable is intentionally opt-in:

```bat
build\ci\tests\assembly_workspace_gpu_smoke.exe
```

The A7b0 persistence gate is disconnected and has no editor or GPU scope:

```bat
cmake --build --preset ci --clean-first --target assembly_mate_solver_tests product_envelope_v9_tests product_envelope_v9_public_headers_compile_tests product_envelope_v8_tests product_envelope_v7_tests product_envelope_v6_decoder_tests product_envelope_v6_encoder_tests product_envelope_v6_materializer_tests model_public_headers_compile_tests
ctest --test-dir build/ci -R "^(assembly_mate_solver_tests|product_envelope_v9_tests|product_envelope_v9_public_headers_compile_tests|product_envelope_v8_tests|product_envelope_v7_tests|product_envelope_v6_(decoder|encoder|materializer)_tests|model_public_headers_compile_tests)$" --output-on-failure
```

V9 tests cover strict root/assembly/options schemas, capability dispatch,
canonical numeric spelling, deterministic authored-only output, disconnected
materialization and model-owned option seals. The fixture is literal V9 JSON,
not output from a production encoder or a mutated V8 fixture. Focused tests also
cover every capability permutation, raw exponent/negative-zero token spelling,
real-topology child-first solving with two components, fixed-point bytes, every
materialization failure seam, rejected apply/resource failures and exact
Body/Sketch/Feature/Product cursor preservation. A7b1 adds editor routing, atomic
writer failure injection and fuzz corpus coverage; those are not evidence for
A7b0.

The A7b1 focused gate adds `product_envelope_v9_atomic_writer_tests`, exact
V9/V10 application routing and editor publication checks. The writer test
drives every preparation seam and the common open/write/flush/sync/replace
matrix, asserting byte-identical source/destination rollback and exact global
cursor preservation. It also proves a real solved mate-bearing fixed point
publishes and that the corresponding organically unsolved `B1 != B0` source is
rejected before transaction open without changing source, destination or any
model cursor. Application/UI coverage keeps retained semantic failures open,
rejects a hard V9 resource failure over a dirty isolated workspace with exact
product/history/redo/savepoint/path/identity/selection/hover/link/diagnostic
rollback, and explicitly exercises V9 Save As success plus alias and
cross-directory rejection without cascading linked writes. It preserves V9
across empty Save/Save As, rejects V7/V8 mate semantics, and checks diagnostics
become Unchecked only after successful authored/history mutations. The scanner-only
fuzz route includes hand-authored minimal, full V2 provenance, unknown
capability, corrupt mate and boundary-option seeds.

The A7c1 focused gate keeps UI out of scope and extends the existing model/V9
targets. It covers aligned/opposed parallel solving, collinear perpendicular
fallback, endpoint permutation, mixed redundancy/conflicts, exact angular work
ceilings, conditional capability/schema classification, real-topology mixed
fixed-point bytes and V7/V8 rejection:

The correction matrix also freezes the accepted pre-A7c mate-bearing V9 output
(one exact `planar-coincident-v1` relation with orientation, offset and both V2
endpoints) as an immutable literal byte oracle, checks the hand-authored angular
fixture through materialize/encode with exact IDs/endpoints/orientation, and
runs every V9 materialization failure seam plus low-work rejection on a real
angular product with exact source/authored-root/global-cursor rollback.

The A7c2 focused matrix exercises the unified assembly solver with rank-four
axis-concentric relations for datum/datum, cylinder/cylinder and mixed
endpoints. It covers coaxial, parallel off-axis, intersecting and skew lines,
the no-axial-slide invariant, endpoint/ID/free-grounded permutations,
duplicate redundancy, invalid and non-cylindrical references, stale candidate
seals, apply rollback and exact N/N-1 work ceilings. V9 tests use hand-authored
datum and cylindrical endpoint schemas, all four conditional capability sets,
unknown/extra/mismatch rejection, real nested fixed points, V7/V8 fail-closed
behavior, and the full seven-seam plus low-work cursor/source rollback table.

A7c2 review coverage additionally calls the total endpoint-view API on a valid
concentric variant, mutates a nested tail occurrence and terminal cylinder
topology after solve, and proves Stale occurs before mutation/successor. Real
mixed components cover Concentric+Parallel redundancy,
Concentric+Coincident rank 5/DOF 1, Concentric+Perpendicular conflict and a
three-node axis/planar chain under insertion/MateId permutations. Exact
shortest-quaternion, anchor, axial-coordinate and no-twist checks complement
ambiguous-cylinder, V1, invalid-owner and missing-path negatives. The V9 gate
freezes an independent datum+cylindrical A7c2 literal byte oracle and runs a
real authoritative cylinder/datum nested fixed point.

The A7c3 focused matrix adds symmetric datum-origin and authoritative V2
circular-center point distance relations. It covers rank-one/5-DOF translation,
grounded and endpoint/MateId permutations, already-satisfied and
solve/apply/solve fixed points, duplicate/conflicting distances, mixed
concentric rank-five behavior, typed non-circular and degenerate-direction
failures, topology-stale apply rollback, and exact N/N-1 work limits for datum
versus metered circular edge lookup, including a 72+-edge authoritative index.
A terminal-Part recompute proves the old V2 candidate becomes Stale before
publication while the same provenance resolves again and reaches a stable
solve/apply/solve fixed point. V9 tests exercise all eight exact
capability combinations, strict disjoint point schemas, raw negative zero,
unknown reference classification, deterministic authored-only nested fixed
points, V7/V8 fail-closed behavior, and literal valid/corrupt/unknown fuzz seeds
wired into `run_project_decoder_fuzz.sh`. Solver regressions also cover exact
oblique 3D projection, translation-invariant direction validation, tiny-target
and shared-node post-sweep collapse, tolerance/step boundaries, circular V2
failure taxonomy, disconnected localization and nested-tail stale rollback.

The A7c4 focused matrix adds acute planar-angle relations with aligned/opposed
branches, endpoint and ID permutations, rank-one diagnostics, pivot-preserving
rotation, collinear typed failure, duplicate/conflicting angles and operational
option revalidation. A 72+-face authoritative index proves the shared planar
V2 lookup is charged before resolution with exact N/N-1 ceilings. V9 coverage
checks all sixteen independent capability combinations, exact angle schema,
raw negative zero, real nested child-first solve/encode fixed point, all seven
materialization failure seams plus low-work rollback, atomic-writer success and
organic-unsolved rejection, V7/V8 fail-closed behavior, and valid/corrupt/
unknown Angle seeds wired into `run_project_decoder_fuzz.sh`.

The consolidated A7c4 correction gate uses one scale-free cone tangent for
both degeneracy classification and projection, plus an `atan2` constraint row
and shortest cone rotation. A synthetic authoritative plane pair proves a
representable sub-`1e-15` tangent solves with rank one while exact zero remains
typed Broken; nextafter tests cover both operational angle boundaries.
Additional cases cover real INTERNAL/EXTERNAL orientation rejection, reversal
laws on geometry-identical oriented faces, mixed Concentric/Distance rank,
Angle-specific apply/stale/recompute rollback, and differential small-versus-
72+-face work accounting for both planar and cylindrical lookups. A resolved
null-face execution seam is intentionally not fabricated: public
`TopologyLineage::create` rejects every null binding before an authoritative
`TopologyIndex` can be built; the production null check remains defensive for
foreign/corrupted geometry providers.

The second A7c4 correction freezes an independent arbitrary-3D aligned/opposed
quaternion and pivot-compensated translation oracle, a common non-principal
rigid-frame covariance case at `1e9` translation, exact mixed
Angle+Concentric/Distance placements, an Angle-bearing three-node chain and a
localized broken Angle component. It also checks the exact `2^-42` model/V9
tolerance boundary and its predecessor, an exact versus representably-short
one-iteration angular-step boundary, already-satisfied identity and
solve/apply/solve zero-pose fixed points. Topology resolver allocation faults
cannot be injected through the immutable public index; every face/edge V1/V2
resolver/creation catch boundary explicitly rethrows `std::bad_alloc`, and the
existing outer solver resource-failure cases prove the application-level
no-candidate boundary.

```bat
cmake --build --preset ci --clean-first --target assembly_mate_solver_tests product_envelope_v9_tests product_envelope_v9_public_headers_compile_tests product_envelope_v9_atomic_writer_tests product_envelope_v8_tests product_envelope_v7_tests product_envelope_v6_decoder_tests product_envelope_v6_encoder_tests product_envelope_v6_materializer_tests model_public_headers_compile_tests
ctest --test-dir build/ci -R "^(assembly_mate_solver_tests|product_envelope_v9(_atomic_writer|_public_headers_compile)?_tests|product_envelope_v8_tests|product_envelope_v7_tests|product_envelope_v6_(decoder|encoder|materializer)_tests|model_public_headers_compile_tests)$" --output-on-failure
```

```bat
cmake --build --preset ci --clean-first --target product_envelope_v9_atomic_writer_tests product_envelope_v9_tests product_envelope_v9_public_headers_compile_tests project_application_service_tests project_switch_ui_tests product_envelope_v8_tests product_envelope_v7_tests assembly_mate_solver_tests model_public_headers_compile_tests
ctest --test-dir build/ci -R "^(product_envelope_v9(_atomic_writer|_public_headers_compile)?_tests|project_application_service_tests|project_switch_ui_tests|product_envelope_v8_tests|product_envelope_v7_tests|assembly_mate_solver_tests|model_public_headers_compile_tests)$" --output-on-failure
```

```bat
cmake --build --preset ci --clean-first --target assembly_definition_tests model_public_headers_compile_tests linked_part_source_resolver_tests linked_part_source_resolver_native_tests linked_part_source_resolver_public_headers_compile_tests product_envelope_v5_decoder_tests product_envelope_v5_encoder_tests product_envelope_v5_materializer_tests product_envelope_v6_decoder_tests product_envelope_v6_encoder_tests product_envelope_v6_materializer_tests product_envelope_v7_tests project_decoder_property_tests
ctest --preset ci -R "^(assembly_definition_tests|model_public_headers_compile_tests|linked_part_source_resolver(_native|_public_headers_compile)?_tests|product_envelope_v(5|6)_(decoder|encoder|materializer)_tests|product_envelope_v7_tests|project_decoder_property_tests)$" --output-on-failure
```

### A8a focused Mate tool gate

The A8a tests separate model/session/history/UI responsibilities. The solver
test proves prospective overlay is read-only and atomic apply/revert restores
exact registry and placements. The session test covers typed V2 filtering,
publication identity, same-path rejection, allocation failure at every
allocating session boundary, aligned/opposed recovery, retained last-valid
preview with exact presentation acknowledgement and three-stage Escape.
Viewport coverage checks candidate scenes reuse committed sources and freezes
committed picking while candidate geometry is displayed. History coverage
checks one cold entry and exact apply/undo/redo. The UI scenario executes V7
and V8 pick/preview/Cancel and pick/Apply flows, verifies bytes/state do not
promote before successful Apply, then proves V9 promotion and Undo/Redo.
Deterministic presentation prepare/install failures retain the exact installed
scene while disabling Apply; an in-handler reentry seam proves stale outer
callbacks cannot publish after cancel/rebegin. The native workspace harness
now emits all conjunctive `A8A_*` interaction/layout bits and
`A8A_GPU_SMOKE`; `realGpu` comes only from the actual OpenGL context. A single
native 150% run is automated evidence, not the pending manual 100/125/150/200
driver matrix. Its layout cases request the actual logical window sizes
1920x1080, 1536x864, 1280x720 and 960x540 and inspect the real mate widgets;
they never multiply those sizes by a synthetic DPR. Each case reports the
OS-accepted window size and the one actual native DPR separately.

The UI preview oracle derives its preview-only logical pixel from a triangle
centroid transformed by the installed candidate instance. The committed
`faceAt` index must miss that pixel and hit the separately captured committed
pixel. Both clicks are frozen while preview is installed; after Escape clears
the visual, the committed pixel is separately proven to recover the second
pick and a new valid preview.

```bat
cmake --build --preset dev --clean-first --target assembly_mate_solver_tests assembly_mate_tool_session_tests assembly_mate_ui_tests assembly_command_history_tests assembly_command_history_public_headers_compile_tests assembly_viewport_session_tests project_switch_ui_tests
ctest --test-dir build/dev -R "^(assembly_mate_solver_tests|assembly_mate_tool_session_tests|assembly_mate_ui_tests|assembly_command_history(_public_headers_compile)?_tests|assembly_viewport_session_tests|project_switch_ui_tests)$" --output-on-failure
```

The focused CTest gate is headless/offscreen. The separately runnable native
workspace harness may provide real-driver evidence, but does not replace the
manual multi-DPR matrix or the final clean `ci`/verifier gate.
