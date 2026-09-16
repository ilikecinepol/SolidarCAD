---
name: occt-safety
description: OpenCASCADE shape, Handle, builder, selection and feature-recompute safety rules for SolidarCAD.
---

# OCCT safety rules

- Treat `TopoDS_Shape` values as handles to topology that may become obsolete after recompute even when the C++ value remains non-null.
- Validate builder success and produced shape validity before committing new model state.
- Keep preview shapes separate from committed feature results.
- Parameter-driven operations such as fillet/chamfer/shell/draft must handle the valid-domain boundary deliberately; excessive values should fail/stop growing safely rather than corrupt state.
- Never assume edge/face ordinal positions remain stable after topology-changing operations.
- Downstream feature references must resolve through the project's topology reference layer where applicable.
- Selection state must be invalidated/re-resolved when the owning shape is replaced.
- Do not store transient OCCT subshape references as if they were persistent identities.

Prefer preserving the last valid committed shape over replacing it with a failed/invalid preview result.
