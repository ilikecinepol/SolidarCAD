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
