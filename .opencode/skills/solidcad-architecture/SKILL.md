---
name: solidcad-architecture
description: Architecture map and dependency boundaries for SolidarCAD C++/Qt/OpenCASCADE development.
---

# SolidarCAD architecture

Treat SolidarCAD as a layered C++20 CAD application.

Core boundaries visible in `src/CMakeLists.txt`:
- `solidar_model`: document/model/features/Sketch/SketchSolver/topology references.
- `solidar_project`: project persistence and IO.
- `solidar_sketch_ui`: SketchRibbon and SketchCanvas.
- `solidar_drawing_ui`: drawing/ESKD UI.
- `solidar_viewport3d`: viewport, rendering, picking, manipulators and 3D UI support.
- `solidar_editor`: editor controllers and MainWindow.
- `solidar_home`: home/start UI.
- `solidar`: final desktop executable.

Preserve dependency direction. Avoid putting UI ownership into model code or model mutation logic into rendering helpers.

When modifying behavior:
1. identify the authoritative state owner;
2. identify all observers/caches/selections that mirror that state;
3. invalidate/update observers atomically enough that callbacks cannot see stale state;
4. prefer stable references/IDs over positional indexes when objects survive mutations;
5. keep preview/transient tool state separate from committed document state;
6. keep serialization compatible unless a format migration is explicitly part of the task.
