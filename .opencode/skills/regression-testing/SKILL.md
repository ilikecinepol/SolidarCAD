---
name: regression-testing
description: Test-selection and regression design rules for SolidarCAD CMake/CTest suite.
---

# Regression testing

The repository has dedicated unit, regression, UI-smoke and topology tests.

Prefer extending an existing test executable whose responsibility already matches the behavior.

Important existing targets include:
- `sketch_regression_tests`
- `viewport_picking_tests`
- `viewport_preview_selection_tests`
- `viewport_manipulator_tests`
- `viewport_ux_polish_tests`
- `fillet_feature_tests`
- `chamfer_feature_tests`
- `part_design_regression_tests`
- `downstream_feature_preservation_tests`
- `persistent_topology_tests`
- `editor_smoke_tests`
- `project_lifecycle_tests`
- `project_switch_ui_tests`

A bug regression should:
1. reproduce the smallest pre-fix failure or invalid state;
2. assert the intended post-fix state;
3. cover the state transition that caused the defect;
4. avoid timing dependence where possible;
5. keep unrelated rendering/UI details out of model-level tests;
6. include boundary values for interactive numeric tools when the defect is parameter-range related.

Use CTest labels/targets already present rather than inventing a parallel test framework.
