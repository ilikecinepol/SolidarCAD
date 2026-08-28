# Application modules

Each major feature is a separate CMake target with a narrow public interface.
This allows work on one function without rebuilding knowledge of the whole UI.

| Module | CMake target | Responsibility |
| --- | --- | --- |
| Home | `solidar_home` | Start screen and create/open project workflow |
| Project | `solidar_project` | Versioned `.solidar` file validation and creation |
| Sketch | `solidar_sketch_ui` | Interactive 2D sketch presentation |
| Drawing | `solidar_drawing_ui` | ESKD sheet preview, dimensions and vector rendering |
| 3D View | `solidar_viewport3d` | Camera and 3D presentation |
| Editor | `solidar_editor` | Composition shell that imports the three CAD views |
| Model | `solidar_model` | UI-independent parametric document and sketch data |

The executable imports only Home and Editor. Home emits a project path without
knowing about the editor; the application entrypoint owns the transition. The
editor composes Sketch, Drawing and 3D View through their public headers.

New feature work should stay inside its module. Cross-module data belongs in
`solidar_model`; project serialization belongs in `solidar_project`.
# Parametric revolve

The model module supports `RevolveFeature` with horizontal/vertical sketch
axes or a stable sketch-line id, partial/full angles, reverse direction and
New Body/Join/Cut operations.
