# Viewport Rendering 2.0

SolidarCAD keeps OCCT B-Rep shapes as the source of truth and uses a separate,
immutable display cache. `BodyRenderMesh` tessellates only after a shape or
viewport-quality change. Orbit, pan, zoom, resize, hover, and selection never
invoke OCCT meshing.

## Render pipeline

`BodyRenderMesh` stores indexed vertices (`position`, smooth `normal`, and
topological `faceIndex`) plus sampled B-Rep edges. Vertices are local to one
topological face, so analytic curved faces shade smoothly without rounding
sharp boundaries between faces. Reversed face orientation flips both winding
and normals.

`ViewportRenderer` maintains separate GPU caches for the authoritative body and
the visual tool preview. It uploads VBO, EBO, and VAO data only when the mesh
revision changes. A shader renders neutral CAD surfaces using ambient, diffuse,
and restrained specular lighting into a 24-bit depth buffer. A second,
depth-tested line pass draws only sampled B-Rep edges; tessellation edges are
never displayed. Selection and hover tint the shaded material.

`Viewport` remains responsible for camera input, CPU picking, construction
geometry, sketches, tool manipulators, the orientation cube, and Qt HUD widgets.
Its OpenGL matrix implements the same orthographic projection used by CPU
picking. Qt painting is bracketed with `beginNativePainting()` and
`endNativePainting()` for the GPU pass.

## Quality profiles

Both profiles scale with the body bounding-box diagonal:

| Profile | Linear deflection | Angular deflection |
| --- | --- | --- |
| Normal | `clamp(diagonal × 0.0012, 0.02, 0.9)` | `0.15 rad` |
| High | `clamp(diagonal × 0.00035, 0.005, 0.35)` | `0.08 rad` |

Curved edges use 65% of the profile's surface linear deflection. The View menu
offers Shaded, Shaded with Edges (default), Wireframe, Normal, and High.

## Diagnostics and limits

The CPU cache exposes vertex, triangle, edge-sample, deflection, and rebuild
timing data. The renderer exposes its most recent GPU upload time for optional
debug diagnostics; no production FPS overlay is shown.

The first version intentionally retains CPU picking and an orthographic camera.
GPU ID-buffer picking, perspective, hidden-line wireframe, materials, and AIS
Viewer integration are outside this backend migration.
