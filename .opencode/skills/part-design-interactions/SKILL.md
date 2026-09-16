---
name: part-design-interactions
description: Interaction rules for SolidarCAD Part Design selection, hover, manipulators, previews, fillet/chamfer and numeric parameter tools.
---

# Part Design interaction rules

For fillet, chamfer, shell, draft, extrude/revolve and similar tools:

- hover must be visually distinguishable from committed selection;
- hover state must never mutate the model;
- selected edges/faces must survive ordinary repaint but be invalidated/re-resolved when the base shape changes;
- numeric manipulation should begin from the actual current parameter, not an out-of-range hidden value;
- zero or tool-specific minimum should be a stable initial state when appropriate;
- dragging should map monotonically to parameter change unless the UX explicitly states otherwise;
- at the maximum valid geometry value, clamp or stop growth safely; do not commit an OCCT failure;
- preview failure must not destroy the last valid committed state;
- manipulator geometry should be derived from selected geometry and remain visually compact;
- clicking Apply/OK commits once; Cancel restores pre-tool state.

Add regression coverage for selection state and numeric parameter boundaries when changing these interactions.
