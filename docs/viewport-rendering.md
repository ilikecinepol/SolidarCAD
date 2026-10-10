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

`ViewportRenderer` maintains one structural GPU cache entry per displayed body,
plus independent visual-preview and cut-preview entries. CPU and GPU reuse use
the collision-safe `BodyMeshKey` tuple: body ID, feature ID, shape revision,
shape identity, and quality. Hashes choose buckets only; the complete key is
always compared. All bodies in the current display transaction remain resident,
including scenes with more than 32 bodies. Only a bounded tail of obsolete CPU
revisions is retained for undo/redo.

The renderer uploads VBO, EBO, and VAO data only when that structural key
changes. A shader renders neutral CAD surfaces using ambient, diffuse, and
restrained specular lighting into a 24-bit depth buffer. A second, depth-tested
line pass draws only sampled B-Rep edges; tessellation edges are never displayed.
Selection and hover overlays use dynamically sized buffers rather than a fixed
face/edge limit.

`Viewport` remains responsible for camera input, CPU picking, construction
geometry, sketches, tool manipulators, the orientation cube, and Qt HUD widgets.
Its OpenGL matrix implements the same orthographic projection used by CPU
picking. Qt painting is bracketed with `beginNativePainting()` and
`endNativePainting()` for the GPU pass.

## Indexed picking and visibility

`ProjectedPickingScene` is the single route used by body/tool/sketch hover,
click, marquee selection, snapping, and the ruler. It projects each canonical
mesh vertex or edge sample once per camera/mesh revision, then queries compact
triangle and segment BVHs. Per-body face and edge offsets preserve stable
selection IDs in multi-body scenes.

Interactive hover is work-bounded. If the bounded visibility proof is
inconclusive, it returns `Uncertain` internally and does not publish a new hover;
it is never treated as either proven visible or proven hidden. Click, release,
marquee, ruler confirmation, Enter, and Apply use the exact path. Exact edge
visibility incrementally merges occluded parameter intervals and exits as soon
as `[0, 1]` is covered. Exact face visibility uses output-sensitive polygon
subtraction, so a tiny real hole remains selectable while thousands of identical
full occluders terminate early.

## Preview cadence and exact boundaries

`PreviewUpdateCoordinator` is GUI-thread-only. It is a periodic frame throttle,
not a trailing debounce: a continuous stream publishes the latest request at
most once per cadence tick instead of starving until input stops. Every request
captures the document generation, body/feature identity, shape revision, and
source pointer. Generation/sequence checks before the build and immediately
before publication reject cancelled, replaced, re-entrant, and project-switch
results. Release, Enter, and Apply synchronously flush the newest request.

Fillet, Chamfer, and Shell manipulator/panel updates perform one ordinary preview
build and reuse a cached topology index. Their potentially multi-build boundary
search is deferred to the exact interaction boundary. Passing the unchanged
value is a zero-build memoized no-op. OCCT remains on the GUI thread; moving it
to workers would require a separate proof of immutable inputs and thread-safe
OCCT lifetime.

## Quality profiles

Both profiles scale with the body bounding-box diagonal:

| Profile | Linear deflection | Angular deflection |
| --- | --- | --- |
| Normal | `clamp(diagonal × 0.0012, 0.02, 0.9)` | `0.15 rad` |
| High | `clamp(diagonal × 0.00035, 0.005, 0.35)` | `0.08 rad` |

Curved edges use 65% of the profile's surface linear deflection. The View menu
offers Shaded, Shaded with Edges (default), Wireframe, Normal, and High.

## OpenGL lifetime

OpenGL resources belong to the context that initialized the renderer (or its
share group). `release()` refuses an unrelated current context. Viewport scene
transactions synchronize the live body-key set while the owning context is
current, including nonempty-to-empty transitions where `paintGL()` has no draw
work. Removing a preview/cut preview destroys its GPU entry; resetting or
switching a document also clears the CPU mesh cache. Context destruction and
reinitialization therefore cannot retain stale body or preview buffers.

## Diagnostics, tests, and benchmark

The CPU cache exposes vertex, triangle, edge-sample, deflection, rebuild timing,
attempt, hit/miss, and owned-byte data. Picking exposes BVH/candidate/occlusion
counters. The renderer exposes upload time plus resident-resource and dynamic
overlay counts for deterministic tests; no production FPS overlay is shown.

`benchmarks/stage5_viewport_scalability_benchmark.cpp` keeps frozen
`legacy`/`indexed` surrogate modes for before/after hash comparison and has a
separate `production` mode that drives `BodyRenderMesh`,
`ProjectedPickingScene`, and the real `PreviewUpdateCoordinator`. CSV output
separates setup time from p50/p95/p99 query time and reports work counters,
owned bytes, peak RSS, and maximum synchronous preview time. The legacy
million-primitive nested occlusion cost is reported as an explicit dry work
count rather than making the benchmark spend quadratic wall time.

The implementation intentionally retains CPU picking and an orthographic
camera. GPU ID-buffer picking, perspective, hidden-line wireframe, materials,
and AIS Viewer integration remain outside this backend migration.
