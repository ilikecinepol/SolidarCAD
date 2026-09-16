---
name: sketcher-safety
description: Crash-prevention checklist for SolidarCAD Sketcher geometry, constraints, tools, solver, Undo/Redo and persistence.
---

# Sketcher crash-safety rules

Before changing Sketch/SketchSolver/SketchCanvas or Sketcher tools, trace:

- geometry ownership and identity;
- constraints referring to geometry;
- selection/hover references;
- current active tool state;
- solver mirrors/caches;
- Undo/Redo snapshots or commands;
- persistence identifiers.

Mutation checklist:
1. deleting geometry must not leave constraints or active tools referencing it;
2. insertion/deletion must not silently invalidate stored positional indexes;
3. solver data must be rebuilt or updated consistently;
4. selection and hover state must be cleared/rebound when the referenced object disappears;
5. callbacks emitted during mutation must not see a half-updated model;
6. Undo must restore all related state, not only the visible geometry;
7. after Undo/Redo the user must be able to continue drawing/constraint operations;
8. save/load must preserve valid relationships and reject/repair invalid references deliberately;
9. never convert a crash into silent data loss by simply skipping invalid objects.

For each confirmed crash, add a deterministic regression sequence such as:
create -> constrain -> delete -> continue operation -> undo/redo -> save/load -> edit again, using only the portions relevant to the bug.
