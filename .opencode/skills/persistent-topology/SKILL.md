---
name: persistent-topology
description: Rules for using and preserving SolidarCAD persistent face/edge topology references across recompute and serialization.
---

# Persistent topology rules

Use the existing topology reference/resolver layer rather than introducing a parallel identity system.

When a feature consumes an edge or face:
1. create/store the appropriate persistent reference representation;
2. resolve it against the current owning shape at use time;
3. treat resolution failure as an explicit state;
4. update serialization through the existing project format path;
5. test recompute and upstream parameter changes;
6. test save/load when the reference is persisted.

Never rely only on raw subshape ordinal/index identity for downstream persistent features.
