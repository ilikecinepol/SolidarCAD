# Architecture

Solidar CAD separates persistent parametric intent from generated geometry and
presentation. This keeps the document testable without a GUI and prevents UI
objects from becoming the source of truth.

```text
Qt Widgets UI
  ├── feature tree and property editor
  └── viewport and selection
             │ commands / immutable IDs
ProductDocument (editor owner)
  ├── optional ProjectId + envelope metadata
  ├── DefinitionRegistry + root DefinitionId
  └── PartDefinition
        └── Document model
              ├── parameters and feature dependency graph
              ├── undo/redo transaction log
              └── regeneration scheduler and diagnostics
             │ feature inputs
Geometry adapters
  ├── sketch constraint solver
  └── Open CASCADE B-Rep operations and exchange
             │ triangulation
Renderer
```

## Dependency rules

- `solidar_model` is Qt-free, but links Open CASCADE for B-Rep feature rebuilds.
- Generated OCCT shapes are transient results; parameters, stable IDs and
  topology references remain the persistent source of truth.
- UI modifies a document through commands; it never owns canonical geometry.
- Geometry adapters return values and diagnostics without referencing widgets.
- Persisted entities use stable IDs, not pointers or tree indices.
- Every operation must be reproducible from its parameters and upstream IDs.

## Implemented foundation

The current model rebuilds Open CASCADE `TopoDS_Shape` results for Extrude,
Pocket, Revolve, Fillet, Chamfer, Shell, Draft, Mirror and pattern features.
Sketch edits propagate through the feature history, while save/load persists
only the definitions needed to reproduce those shapes.

## Product document ownership

`MainWindow` owns one authoritative `ProductDocument`. In the implemented
single-part slice its registry contains exactly one root `PartDefinition`, and
that definition exclusively owns the existing `Document`; UI subsystems borrow
that nested document and do not keep a parallel authoritative model. Replacing
a project replaces the carrier only after staged load succeeds.

The carrier's identity bundle is atomic and optional. Standalone V3/V4 has no
`ProjectId` or envelope metadata, while product V5 has both. The same nested
`Document`, root `DefinitionId`, model IDs and topology survive explicit
standalone-to-V5 conversion and V5 Save As clone; successful persistence changes
only the carrier identity/path state required by the command. Generated B-Rep,
selection, viewport presentation and undo/redo history are not re-created for
those identity transitions.

This ownership is not an assembly model. `AssemblyDefinition`, occurrences,
linked definitions and mates are absent. Relocate with preserved identity is
also deferred until its filesystem/replica transaction boundary specifies
canonical file aliases (symlink/junction/reparse/hardlink), same- and
cross-volume algorithms, overwrite, dirty/savepoint/external-change handling,
source-delete failure, rollback/crash recovery and two-replica state.

## A6a read-only linked-source resolver

The first linked-source slice is deliberately runtime-only.  An
`ExternalSourceKey` is the typed pair `{ProjectId, DefinitionId}`; neither the
locator nor its SHA-256 content revision participates in identity.  A
`LinkedPartSourceDescriptor` carries that key, the original relative locator
hint, and the expected hash of the exact source bytes.

`solidar_project` resolves a bounded batch into disconnected, move-only V5
`ProductDocument` results.  It preflights paths, bytes, hashes, identities,
conflicts and rebuild work before materializing in stable key order.  A batch
failure publishes no partial products and cannot mutate the active product,
history, UI, persistence state, or source file.  V1--V4 sources have no linked
identity, while V6/assembly sources are rejected until nested assemblies.
Persisted descriptors are added by A6b0 and explicit Reload/Repair lifecycle
by A6b1; the closed V6 schema remains unchanged and editor routing stays later.

The filesystem capability exposes a split exact inspect/held-handle read
operation.  It has no enumeration, glob, write, URL, or network method.
One shared pure preflight validates descriptor count/keys, raw UTF-8 sizes,
root count/bytes and lexical normalization before native root canonicalization
or any other filesystem I/O; it rejects absolute, escaping, drive/device/ADS
and network locators.
Metadata, canonical path, object identity and size are inspected first;
network/containment/regular-file/depth/work/byte checks then run before bytes
are read through that same handle/fd.  Windows walks components relative to
held directory handles without following reparse points implicitly, rejects
remote drives/UNC reparse targets before opening them, tracks reparse identity,
and fails closed for case-sensitive directories. Drive/open, metadata,
case-policy, reparse inspection, canonical-name, byte-read and verification
calls are individually precharged through the same bounded walker backend;
no native call occurs after work exhaustion. The final token remains under
move-only RAII ownership through canonicalization and all staging allocations,
then transfers directly into the held read handle. Windows containment uses
ordinal ignore-case segment comparison (not Unicode full case folding). POSIX
uses `openat`/`fstatat`/`readlinkat` with `O_NOFOLLOW`, resolves relative and
absolute link chains by `(dev, ino)`, and compares paths case-sensitively. All
byte/root/path/component/alias/work/rebuild totals are overflow-checked against
explicit limits. Root canonicalization and target traversal consume one shared
filesystem-work budget, while root and target alias depths are combined before
the target is opened. Physical aliases
reuse identity-cached bytes and equal-content replicas share one decode and
materialization.

## A7a headless planar mate kernel

Each `AssemblyDefinition` owns a Qt-free, ID-sorted `MateRegistry`. A planar
mate stores only its stable `MateId`, two assembly-relative full
`OccurrencePath` values, strict persistent-topology V2 face references,
aligned/opposed sense and a finite canonical offset. Grounding remains an
`OccurrenceFlags` property; it is not represented by a synthetic relation.

`solvePlanarMates` resolves both endpoints again from the current authoritative
`FeatureTopologySnapshot` and `TopologyIndex`. The terminal planar frame is
composed through the nested tail, while the first path segment identifies the
6-DOF variable in the owning assembly. Thus two paths through distinct direct
occurrences remain independent even when they reuse one nested definition.
The bounded deterministic kernel publishes rank, remaining DOF, finite
per-relation residuals, redundant IDs and a bounded conflict set. No OCCT
shape, traversal ordinal or pointer enters the registry or candidate.
Every solve has an explicit `MateId` and/or immediate-occurrence seed. Only
the deterministically derived relation-connected component is resolved and
solved, so a broken or conflicting disconnected component cannot block it.
Endpoint failures use a fixed status precedence independent of relation IDs.
One shared runtime work budget charges every executed component expansion,
normalization, iteration residual, central Jacobian perturbation, pivot,
division/elimination, redundancy prefix and augmented-consistency step. It
also precharges checked conservative bounds for provenance-aware comparisons,
sorts, prefix copies, candidate materialization and both registry/candidate
fingerprints. The published charge is therefore the exact production plan:
that exact limit succeeds, limit minus one fails, and saturated maximum
accounting never wraps. Rank is a typed result (`rank`, non-finite, or resource
failure). Angular and linear equations are scaled by their authored
tolerances; numerical-noise rows are discarded before normalization, then
columns are scaled before pivoting so large lever arms do not hide translation
rank.

A successful/underconstrained/redundant solve produces an addressed placement
candidate containing exact before snapshots for every relation-connected
direct occurrence. The candidate is sealed and cannot be publicly constructed
or mutated; no production-linked test access or reseal helper exists. It
records its seed plus exact sorted mate and node sets.
`ProductDocument::applyMatePlacementCandidate` re-derives that component and rechecks the
product incarnation, root and owner IDs, registry revision/fingerprint,
targets, placements and flags before the first mutation. It publishes one
`MateProjection` successor impact or restores every placement exactly on a
late rejection/resource/finalization failure. Documents, B-Rep, topology
indices and model IDs are never changed by that transaction. Mate UI,
persistence and JSON remain later milestones.

# Revolve feature

`RevolveFeature` is a history-based `ShapeFeature`. It stores only its profile
sketch id, stable sketch-axis reference, angle, direction and shared solid
operation; OCCT B-Rep is rebuilt through `Document::recompute()` and is never
serialized.

## Parametric history 1.0

Each `Body` is an ordered dependency chain. A feature stores stable IDs and
topology references to its inputs; generated OCCT shapes are transient. One
operation-local `DocumentIndex` combines the local history edges, declared
cross-Body Feature/Sketch dependencies, and face-support edges. Body storage
order does not define cross-Body evaluation order; the Feature order inside one
Body still defines its linear local history and is not a supported reorder API.
Editing a sketch marks every dependent feature dirty, while editing a feature
marks that feature and the following features in its body dirty. Recompute
proceeds from upstream to downstream and preserves the IDs of existing nodes.

Before topological scheduling, the index validates declared IDs and face-support
ownership while retaining every known valid dependency and reverse edge. A
missing Feature, missing Sketch, missing support Feature, or support-owner
mismatch produces a deterministic document diagnostic and aborts before any
Feature evaluation. The structural root and its dependent closure become
`Error`; an invalid support Sketch also becomes unresolved and placement-dirty.
Public `shape()` and the affected active `Body::resultShape()` are unavailable,
but cached last-valid B-Rep and shape revisions are retained. Independent Valid
or Dirty nodes keep exactly their prior state. Restoring the same IDs/support
allows the existing graph to recompute and recover.

If geometry evaluation of a structurally valid feature fails, its current result
is likewise unavailable and every downstream feature moves to `Error` with an
invalid-upstream diagnostic. Cached last-valid geometry remains available only
through the explicit last-valid APIs, while independent dirty Bodies continue
to rebuild. Correcting the source retries the same nodes without recreating IDs.
Face-supported sketches are repositioned after each rebuilt producer so Pocket
and later operations consume the updated face.

The persisted source of truth is the ordered feature definitions, parameters,
stable IDs, sketch geometry, and topology references. Save/load never treats a
cached B-Rep as authoritative. Version 1 intentionally remains linear within a
body: feature reorder, suppression, branching history, and additional Part
Design operation types are outside this milestone.

## Mirror and pattern history nodes

`MirrorFeature`, `LinearPatternFeature`, and `CircularPatternFeature` consume the
immediately preceding feature identified by a stable `FeatureId`. They create
OCCT B-Rep occurrences and return a compound containing the original and every
generated occurrence; render meshes are never transformed as source geometry.
Mirror v1 supports the global XY/XZ/YZ planes. Linear Pattern uses a global
X/Y/Z direction, with `count` meaning total occurrences and positive spacing in
millimetres. Circular Pattern uses a global X/Y/Z axis. A full 360 degree pattern
uses `angle/count` and therefore never duplicates 360 degrees; a partial pattern
uses `angle/(count-1)` and includes both range endpoints.

The three definitions and source IDs are persisted in project format v2. They
use the normal Body scheduler, including downstream Dirty propagation, stale
shape removal, blocked diagnostics, recovery, and stable IDs. Arbitrary datum
planes/axes, per-occurrence suppression, and boolean fusion are v1 limitations.

Multi-solid downstream modifiers use `ShapeContainerUtils`: global edge
selection is mapped to its owning solid, affected solids are rebuilt in
isolation, and the container is reconstructed with every unaffected occurrence.
Fillet and Chamfer therefore preserve Pattern compounds instead of rejecting or
silently truncating multi-solid results.

## Universal Part Design tool framework

Part Design interaction is defined by `PartDesignToolDefinition` and executed
through one `PartDesignToolController`. The controller enforces that at most one
tool is active and routes Escape either to the current nested re-selection or
to cancellation of the complete tool. Persistent and preview data live in a
`ToolSession`; a dock, combo box, manipulator, or HUD is never authoritative.

The common lifecycle is `Inactive`, `SelectingInput`, `SelectingReference`,
`EditingParameters`, `PreviewValid`, and `PreviewInvalid`. Selection is declared
with `SelectionRequirement` (`Sketch`, `SketchLine`, `Edge`, `Face`, `Axis`,
`Plane`, `Body`, or `Feature`). Parameters are declared with
`ToolParameterDescriptor`; `ToolParameterHud` renders all HUD-editable numeric
parameters, keeps Enter separate from Apply, uses normal Tab/Shift+Tab traversal,
and consumes the first Escape to restore the last committed field value.

The standard registry contains Extrude, Pocket, Revolve, Fillet, Chamfer,
Shell, Draft, Mirror, Linear Pattern, and Circular Pattern. Revolve, Fillet,
Chamfer, Shell, and Draft use
the common runtime controller and session parameter contracts; the registry is
the mandatory migration boundary for the remaining legacy command bodies.

Shell stores persistent references to the faces removed from the immediately
preceding feature, thickness, and the inside/outside mode. Draft stores the
drafted faces, neutral plane, pull direction, angle, and reversal. Both use
OCCT only while rebuilding or previewing; generated B-Rep is never serialized.
Selections are resolved again after upstream recompute. Compound inputs keep
untouched solids. Shell v1 deliberately requires all removed faces to belong
to one solid; selecting faces from several solids produces a recoverable tool
error.

To add a Part Design tool:

1. implement `FooFeature` and `FooToolSession`;
2. add its selection requirements and parameter descriptors to the registry;
3. expose its manipulator and build temporary preview geometry in the session;
4. register commit/cancel bindings with `PartDesignToolController`;
5. bind the generic HUD and parameters panel to session parameters;
6. commit the feature only on Apply and reuse the same session with an
   `editingFeatureId` for history editing.

New tools must use this template. They must not introduce a new dialog-driven
selection/preview/Apply pipeline in `MainWindow`.

## Viewport Rendering Architecture

The CAD model remains authoritative and the viewport consumes immutable
definition-local sources plus per-occurrence placement:

```text
OCCT B-Rep
  -> definition-local tessellation cache (BodyRenderMesh)
  -> shared source/preview GPU meshes (VAO + VBO + EBO)
  -> per-occurrence T * R model matrix and draw state
  -> depth-tested smooth surface pass
  -> topological B-Rep edge pass
  -> CPU picking plus Qt selection/tool/HUD overlays
```

`BodyRenderMesh` owns canonical indexed vertices/triangles and sampled
topological edges; it does not retain a second expanded triangle array.
Vertices are deliberately not welded between topological faces, preserving
sharp CAD boundaries. Its structural `BodyMeshKey` contains product epoch,
definition ID, body ID, feature ID, model-owned shape revision, exact shape
identity and an explicitly mapped viewport quality. This makes a
`BodyRenderMesh` a source/definition-local object: an internal B-Rep
`TopLoc_Location` is applied during extraction, while an assembly occurrence
transform is never baked into the B-Rep, tessellation, or shared GPU buffers.
Camera orbit, zoom, pan, resize, hover, and selection only update matrices,
index data, or shader uniforms; they never invoke OCCT meshing. A monotonic
fallback revision protects the legacy same-pointer `Viewport::setBodyShape`
route from in-place shape mutation.

`AssemblyViewportSceneAdapter` consumes only a guarded immutable assembly
publication. It validates the complete source/instance relationship, resolves
each effectively visible definition-local source once, and stages the whole
CPU cache update before swapping one complete scene generation. A stale handle
or any late mesh failure leaves the preceding committed scene/cache unchanged;
a valid empty publication commits empty and prunes every inactive source. OCCT
tessellation stays sequential on the owner thread (`BRepMesh` internal
parallelism is disabled until its ownership and determinism are proven).
Each successful `BodyRenderMesh` also retains the exact definition-local AABB
returned by `BRepBndLib`; failed rebuilds retain the previous AABB and `clear`
removes it. During the same pending scene transaction, the adapter transforms
all eight local corners with the shared double-precision WXYZ `T * R` helper,
publishes one immutable world AABB per effectively visible instance, and unions
those into the scene AABB. Hidden/suppressed occurrences and hidden bodies have
no render instance and therefore cannot affect bounds. Non-finite transform or
aggregate overflow is a typed `InvalidBounds` rejection that preserves the
previous scene and CPU cache.

`ViewportRenderer` owns OpenGL resources and shaders. Assembly instances share
one GPU slot per visible source and submit a separate draw with `T * R`; whole
occurrence hover/selection is a uniform draw state and never rewrites shared
face/edge highlight buffers. Missing slots and draw records are staged for the
entire guarded generation before commit, then inactive sources are pruned only
after the last visible user disappears. Temporary tool/cut previews retain the
legacy separate entries and face/edge overlay path. Destruction, upload, and
drawing are accepted only under the exact owning OpenGL context because VAOs
are not share-group objects. Explicit release is idempotent and permits clean
wrapper recreation under a new context; a call with no current context or a
different current context is an unchanged rejection. A delayed stale
publication is likewise an ignored input and cannot clear the newer active
generation. Assembly drawing preflights every required VAO before emitting a
pixel, renders the complete pending plan while the old state remains
authoritative, and only then publishes and prunes. A driver-reported GL error
after drawing clears the target deterministically and leaves the old logical
generation active; this is failure containment, not a promise that an external
compositor never observed an intermediate GPU command.
If the exact owner context disappears while an assembly transaction is being
rolled back, newly staged `GpuMesh` owners move into a renderer-owned deferred
destruction queue whose capacity was reserved before upload. Resident cache
membership still rolls back immediately, but Qt GL wrappers are destroyed only
on the next exact-owner-context entry (or compatible `release`); release under
any other context remains a pure refusal.
Assembly camera center and depth now come only from the committed aggregate
world AABB (`max(1, diagonal * 3)`). The pure `fitAssemblyViewport` helper
frames its eight corners at a 0.88 logical-pixel content fraction for the
requested yaw/pitch, anchors the center at `(w*0.5, h*0.52)`, and returns
generation-tagged zoom/pan/center/depth plus clamp/containment flags. It checks
the publication guard before and after calculation, has no DPR input, and
returns no camera value for stale, empty, non-finite, or invalid-size input.
This A4c1 path does not change legacy part `Viewport::fitAll` or any picking,
rectangle, snap, ruler, mouse, wheel, or resize contract.

`AssemblyViewportPicking` is a separate owner-thread CPU consumer of the same
guarded immutable scene. It retains one `TopologyIndex` per full
`BodyMeshKey`, built from the original shared definition shape, its exact shape
revision and the feature's captured last-valid `TopologyLineage`; it never asks
the model to lazily build an index and never transforms B-Rep. Face and edge
tables prefer an exact V2 factory result and use V1 only when the V2 factory
reports a true `Missing`; ambiguous, malformed and owner-mismatched evidence
fails the complete publication transaction. Public hits contain the exact
assembly occurrence identity, definition ID and existing `FaceReference` or
`EdgeReference`, never a flattened global ordinal or mesh index.

Picking constructs a double-precision orthographic segment from the same
right/up/view basis, logical-pixel scale, pan and 0.52 anchor as
`ViewportCameraState`. It broad-phases transformed world AABBs, inverse-rotates
the ray into definition-local meshes with the exact rigid transform, and uses
two-sided triangle intersection. Near-equal front hits are ordered by full
occurrence path and stable source identity. Edge candidates use logical-radius
world distance and a second exact surface ray through the candidate point, so
a surface strictly in front rejects a rear edge while same-surface and
silhouette edges remain selectable within a scale-aware tolerance. Every
publish and query rechecks the exact coordinator publication; equal numeric
generations from different coordinators cannot alias.

Whole-occurrence renderer tint now accepts only an
`AssemblyOccurrenceHighlightTarget` guarded by that exact publication. A
current face/edge hit can create the target; stale or foreign targets paint
nothing, and changing hover/selection never uploads or mutates the shared GPU
source. This A4c2 boundary deliberately leaves the legacy
`ProjectedPickingScene` rectangle, snap and ruler contracts unchanged for the
separate A4c3 migration.

A4c3 extends that same assembly-only CPU owner; it does not route assembly
state through the legacy part picker. Rectangle faces and edges, point snap,
and the headless `AssemblyViewportRuler` all retain the exact publication
handle and full occurrence-scoped persistent address. The one A4c2
`BodyMeshKey -> SourceTopology` cache remains authoritative. Placement
publication rebuilds only instance/world/projected records, while a camera
change rebuilds only a double-precision logical-pixel projection cache. DPR is
validated but is neither a projection-cache key nor a second radius scale.

Rectangle queries normalize finite corners and use global visibility across
every visible occurrence. A face requires scale-aware strictly positive
projected area after the initial clip and after every occluder subtraction;
face-only vertex/edge contact and edge-on collinear triangles are not face
hits. A clipped candidate face polygon is successively split by each
strictly-front projected triangle, so any nonzero surviving region is retained;
coplanar instances do not subtract one another. A clipped edge segment uses
the corresponding analytic union of strictly-front occlusion intervals;
boundary, same-surface and silhouette contact remain edge hits. Bounded work
exhaustion in any Vertex, Edge, Surface, or occlusion phase is `Uncertain`,
never a partial selection. Snap returns a guarded world point and logical point
with Vertex > Edge > Surface priority after global occlusion. Vertex identity is
explicitly transient and exact-publication-scoped, with an existing supporting
edge reference when available; no persistent vertex ordinal is invented.
Every snap kind is restricted to the same finite A4c2 camera ray segment;
finite geometry outside its near/far clip range is skipped as a normal miss,
while only invalid input or non-finite math produces `InvalidInput`.
`AssemblyViewportRuler` accepts only exact current snap hits from one
publication, computes Euclidean world distance, retains a proven hover across
`Uncertain`, and clears on stale/foreign replacement. It owns no QObject,
model mutation, persistence, Undo state, painter, or input event.

`ProjectedPickingScene` is camera- and mesh-revisioned and provides the shared
BVH-backed hover/click/marquee/snap/ruler path. Interactive queries may return an
unpublished uncertainty when their work budget is exhausted. Interaction
boundaries use exact indexed visibility, including output-sensitive face
refinement and interval-based edge occlusion.

`PreviewUpdateCoordinator` owns GUI-thread preview cadence. It coalesces each
stream to the newest request on a periodic tick, captures immutable source
identity, and rejects stale generations/sequences before publication. Project
replacement, import, Undo/Redo and tool teardown invalidate pending work before
destroying or replacing its inputs. Release, Enter and Apply flush the latest
request and perform Fillet/Chamfer/Shell boundary refinement exactly once.

`Viewport` retains camera interaction, picking-scene construction, reference
geometry, manipulators, and QWidget HUD placement. `MainWindow` coordinates
preview sessions and only forwards display-mode/quality choices; it contains no
OpenGL implementation details. OCCT rebuild and preview calls remain
synchronous on the GUI thread until a separate thread-safety/lifetime design is
proven.

### A5a: read-only assembly editor workspace

The editor has an explicit `EditorProjectKind`: a part workspace or a read-only
assembly workspace. An assembly is never installed incrementally. Before the
active editor changes, `AssemblyWorkspacePreparer` owns the staged
`ProductDocument` and builds a complete `PreparedAssemblyWorkspace`: an
`AssemblyViewportSession`, an immutable typed `AssemblyTreeModel`, and an empty
selection. Presentation, scene, picking, and final-preparation failures discard
that candidate and leave the active project and presentation epoch unchanged.

`AssemblyViewportSession` is a non-`QObject` owner. It retains the publication
coordinator longer than its scene adapter, picking index, scene, and guarded
interaction targets, and it never retains a raw pointer to the authoritative
product. `Viewport` owns only this prepared session. Assembly paint is a short
mode dispatch to `renderAssembly`; Fit and face hit-testing use the session's
single scene-bounds camera. Logical renderer detach drops retained assembly
draw state without destroying Qt GL wrappers outside their exact owner context.

`AssemblyTreeModel` stores stable immutable nodes and exposes only typed
addresses (`kind`, definition ID, optional exact publication handle plus full
occurrence identity/path). Numeric epoch/generation equality is insufficient:
selection from another coordinator is foreign even when every number and local
ID matches, and rejection never clears the newer current selection.
Display text, rows, `QModelIndex`, and internal pointers are presentation only.
The assembly tree includes the root definition, every occurrence, and every
part definition including orphans. Hidden and suppressed occurrences remain in
the tree but have no viewport pick/highlight target.

Project replacement first invalidates queued callbacks and cancels all Part
Design sessions while the old part document and B-Rep are alive. It then clears
transient UI, selection, tree connections, viewport state, and history before
moving the already-prepared workspace into place. No mesh/topology work, modal
dialog, or event processing occurs after teardown begins. A successful assembly
commit consumes exactly one monotonically increasing presentation epoch;
failed preparation consumes none. Immediately after the product move, the
semantic mode flips to read-only assembly before any model/view/stack/action
operation can emit a synchronous Qt callback. All part mutation actions and
handlers (including the stored New action and hidden-widget callbacks) fail
closed in assembly mode, while Open, settings, display mode, Fit, orbit, pan,
and zoom remain available.

### Bounded assembly Insert transaction (A5b)

The first assembly mutation boundary is intentionally narrower than the later
occurrence-command framework. `ProductDocument::applyEmbeddedPartInsert()` and
`extractEmbeddedPartInsert()` atomically transfer exactly one embedded part
definition together with one root occurrence. A stack-scoped typed
`ProductDocumentSuccessor` runs while the old viewport publication is still
current; it is never retained, its `noexcept` method reports prepared,
rejected, or resource failure explicitly, and rejection restores the exact map
nodes and detached `Document` ownership without exposing a partial model.

`AssemblyInsertToolSession` owns proposed project-local IDs and a canonical
rigid placement while the component is pending. Translation and rotation
updates never call `Document::recompute()` or mutate the product.

### Occurrence command foundation (A5c0)

`RootOccurrenceAddress` combines a non-persistent product-incarnation guard,
the root definition ID, the complete stable occurrence path and the expected
target definition ID. `OccurrenceDelta` is a closed typed variant: its empty
form remains the A5c0 test no-op, A5c1 adds occurrence Rename with exact
before/after UTF-8 bytes, A5c2 adds only the authored visible-bit pair, and
A5c3 adds only the authored grounded-bit pair.
Apply and revert validate the complete address, target and expected current
value. The address can represent a future full path,
but the current flat model rejects depth other than one.

An authored occurrence name is model data independent of its referenced
definition. Empty retains the deterministic UI fallback `Экземпляр <id>`.
Non-empty names pass one Qt-free strict UTF-8 validator (16 KiB maximum, no
embedded NUL, controls or line separators, and not whitespace-only) and are
installed by an allocation-free swap after the replacement value is prepared.
Rename changes no ID, target, placement, flag, `Document`, B-Rep or topology.
Its successor impact is tree-only: a complete replacement tree is prepared
while the existing viewport session/publication/epoch remains authoritative,
then selection is rebound semantically to the same occurrence. V6 encoding
fails closed for authored names until a future schema version explicitly owns
their persistence; unnamed legacy projects remain unchanged.

`AssemblyCommandHistory` is the single non-QObject assembly history. Its closed
variant contains the existing embedded-part cold insert command and the typed
occurrence-delta command. Empty deltas are never recorded. Insert Undo destroys
runtime geometry after guarded extraction; Redo rematerializes and recomputes
only from bounded canonical authored-v4 bytes, never from a source path.
One cursor, 256-entry/64-MiB budget and monotonic revision/savepoint relation
define dirty state; entries retain no `Document`, B-Rep, mesh, publication or UI
objects. Project replacement clears this history before replacing the product,
so equal local IDs in a later incarnation cannot reuse old commands.
Undo/Redo accept the typed successor by non-owning reference and return
semantic state in a value result. There is no raw callback/context,
`std::function`, or output-pointer carrier at the public command boundary. A
private stack adapter contains finalization failures and translates them into a
typed rejection before control returns across a `noexcept` boundary.

`ProjectApplicationService::stageNewAssembly()` creates an unsaved in-memory
V6-routed empty assembly without file I/O. `stageEmbeddedPart()` reuses the
validated open pipeline, accepts only one modern part root and strips source
project identity/path from the move carrier. V6 Save/Save As remains
unsupported.

### Visibility and transient isolate (A5c2)

Persistent occurrence visibility is a typed `OccurrenceDelta` alternative
with exact before/after guards. Its private model primitive changes only the
authored `visible` bit; suppression, grounding, name, placement, target and
definition geometry are never copied from a stale flags bundle. The dedicated
visibility successor keeps the product presentation epoch and immutable source
keys, but prepares a fresh publication, scene, spatial index and tree before
history commit. Definition-local CPU caches and renderer-owned GPU sources are
reused while another visible occurrence still references them. Hiding the last
visible user intentionally prunes the source, so a later Show may rebuild and
upload it.

Isolate is not a model command. `AssemblyViewportSession` retains its immutable
authored scene and transactionally prepares a filtered scene/picking index
under the exact same session, publication and epoch. The filtered scene keeps
all authored effective sources resident but exposes instances, bounds and
picking only for the isolated occurrence. Show All restores the retained base
scene. Authored hidden, suppressed and body-hidden content is never revealed;
history, dirty/savepoint and persistence remain unchanged.

### Ground / un-ground occurrence (A5c3)

Grounding is a persistent typed occurrence delta with exact before/after and
address/current-value guards. Its private model primitive changes only the
`grounded` bit. Placement, visibility, suppression, name, target, IDs and part
geometry are unchanged; the public committed-placement boundary rejects a
grounded occurrence and returns `Busy` during an active occurrence
transaction. This prevents a reentrant successor from bypassing rollback.

The dedicated grounding successor uses the shared same-epoch occurrence
projection machinery. It prepares a fresh publication and immutable tree,
rebinds semantic selection, clears hover and reuses source meshes/topology and
renderer resources. Instances, transforms, bounds, picking membership and
camera remain unchanged. Ground/Un-ground is disabled during transient
isolation, while authored hidden or suppressed occurrences remain eligible.

### Suppress / unsuppress occurrence (A5c4)

Suppression is a persistent typed occurrence delta with exact address,
before/after and current-value guards. Its private model primitive changes only
the authored `suppressed` bit; visibility, grounding, name, placement, target,
IDs and definition geometry remain untouched. The legacy whole-flags mutator
returns `Busy` during an occurrence transaction, closing captured-alias
reentrancy around exact rollback.

The distinct suppression successor reuses the private same-epoch occurrence
projection builder. It publishes a fresh scene/tree/picking view before history
commit, rebinds semantic tree selection, clears hover and omits suppressed
instances from bounds and all spatial consumers. A suppressed selected row has
no viewport highlight. Shared source meshes and GPU resources remain resident
while another effective occurrence uses them; suppressing the last effective
user intentionally permits prune, and a later Unsuppress may remesh/upload.
Authored hidden or body-hidden occurrences are never revealed. Suppression is
disabled during isolate; hidden and grounded occurrences remain eligible.

### Duplicate occurrence (A5c5)

Duplicate is a dedicated structural cold command rather than an occurrence
field delta. Its payload stores the exact source address and snapshot, the
lowest free positive root-occurrence ID, and the copied target, canonical
placement, flags and authored name. It never owns a `ComponentOccurrence`,
`Document`, B-Rep, mesh, publication or UI object. Apply stages a disposable
map node before touching the live assembly; Undo extracts that exact node and
Redo reinserts the same stable ID. Shared occurrence-mutation exclusion covers
delta, duplicate, embedded insert and legacy occurrence mutation paths.

The distinct duplicate successor prepares a fresh same-epoch publication,
tree and picking view before history commit. A visible duplicate adds one
instance with the same definition-local mesh/topology source and no remesh or
GPU upload. Native evidence requires the next rendered frame to submit and
draw exactly one additional instance, while a topology-backed rectangle query
returns both distinct, current-publication identities for coincident source
and duplicate occurrences. Undo removes only that node while the source
remains resident for the original. Hidden, suppressed or body-hidden copies
remain semantic tree rows without a rendered instance. Apply/Redo select the
duplicate and Undo selects the source. Duplicate is disabled during Insert,
Rename, isolate, replacement and history activity.

### Remove occurrence / Undo (A5c6)

Remove is the fourth structural cold command. Its payload contains only the
exact root-occurrence address and the occurrence's target, canonical
placement, flags, and authored name. Apply extracts the exact live map node;
Undo stages a disposable node and restores the same stable ID. Neither path
deletes, moves, or clones the referenced PartDefinition, Document, or B-Rep.
An assembly with no occurrences is valid, and unreferenced definitions remain
in the registry and appear in the tree as `Деталь N (не используется)`.

The distinct remove successor prepares a fresh same-epoch publication, tree,
scene, spatial index, and picking view before history commit. Apply/Redo clear
the removed selection and Undo restores it. A remaining effective peer keeps
the shared CPU topology/mesh and GPU source resident with no rebuild/upload;
removing the last effective user may prune the source, and Undo may reacquire
it. Hidden or suppressed occurrences perform no geometry work. Remove is
disabled during Insert, Rename, isolate, replacement, and history activity.

### Edit embedded definition context (A5d)

An assembly remains the active `ProductDocument` while a PartDefinition is
edited. `AssemblyDefinitionEditContext` keeps only stable product/root/target
IDs, a definition-state token, history revision floor and semantic navigation
state. The authoritative embedded `Document` remains owned by its
`PartDefinition`; the context and Part UI never acquire a second owner. The
assembly viewport session is suspended while the ordinary Part ribbon,
feature tree and timeline remain visible for context. Commands not yet ported
to detached-candidate authoring are disabled fail-closed; no UI callback may
mutate the authoritative embedded document first. The latest prepared
assembly session is resumed without fitting or resetting the saved camera.
Suspension detaches every assembly draw/picking instance but retains only its
already-resident GPU source keys; ordinary Part frames cannot prune them. The
first successful resumed assembly frame releases that retention atomically,
reuses unchanged definition slots and retires only obsolete edited slots.
`Move` is the first ported geometry command: its real ribbon/body-selection/
parameter/Apply flow edits a detached candidate, recomputes it, and only then
enters the guarded definition transaction. UI-wide document access is const;
the mutable accessor is standalone-Part-only and terminates on a missed
definition-context guard.

`DocumentDelta` is the reusable Qt-free cold value shared by standalone part
history and the fifth `AssemblyCommandHistory` alternative,
`DefinitionEditColdCommand`. A definition command clones the current
document, applies the cold delta to the candidate, requires a successful
recompute, and swaps validated committed state into the existing authoritative
`Document` object. `DefinitionStateId` guards stale commands. Any successor or
finalization failure swaps the exact old state back before returning; no live
invalid document, B-Rep, history cursor or presentation becomes observable.
`ProductDocument` exposes embedded documents read-only and one shared mutation
guard rejects registry, occurrence or nested definition edits re-entered from
successor preparation. The standalone `ModelCommandHistory` remains empty in
this context.

`DefinitionProjection` prepares a fresh publication under the same product
presentation epoch. Occurrence IDs, paths, placements, names and flags remain
unchanged. Reusable sources from unchanged definitions are retained; changed
definition bodies are rebuilt once and shared by every effective occurrence.
The UI exposes `Сборка N › Деталь N` and `Вернуться в сборку`; navigation is
not a history command. Assembly persistence/export remains disabled, orphan
PartDefinition rows are valid edit entrypoints, and project replacement tears
down the Part tool/session and edit presentation while the embedded document
is still alive.

### Linked part binding and V7 boundary (A6b0)

`SourceContentHash`, `ExternalSourceKey` and `LinkedPartBinding` are Qt-free
model values. A linked `PartDefinition` owns exactly one `Document`: its
last-valid materialized snapshot. The binding carries external identity, the
authored relocatable UTF-8 hint and accepted hash, but never a canonical path,
filesystem status or another document/B-Rep owner. External definition ID is
independent of the local registry ID.

V7 is a strict, flat product envelope layered beside unchanged V5/V6 codecs.
Its scanner/encoder/materializer are disconnected: scan and materialization do
not resolve paths or perform I/O, and publication happens only after every
parametric cache validates and recomputes. The full schema is documented in
`docs/project-format-v7.md`. Editor Open/Save, Reload/Repair and nested
assemblies remain outside A6b0.

### Linked part lifecycle microgate (A6b1)

Linked lifecycle is explicit and keeps three resolver intents separate. The
original exact resolver still rejects a changed SHA-256 as `Outdated`.
Reload accepts a changed revision only after the exact `ExternalSourceKey`
matches and returns a fully materialized, recomputed disconnected candidate.
Repair is an identity/currentness probe: it never materializes a `Document`
and never accepts a hash. All three intents share the same exact-read-only
filesystem capability, path/root checks and aggregate budgets; none can scan,
write or use a network API.

`ProductDocument` owns the atomic commit. Reload swaps the last-valid authored
`Document`, accepted hash and `DefinitionStateId` under one guarded
`DefinitionProjection`; the locator and external identity cannot change.
The staged recompute resolves authored persistent topology references against
the new feature topology index; it never substitutes raw face/edge ordinals.
An unresolved reference rejects the candidate before publication while the
last-valid Document, B-Rep, binding, state and projection remain unchanged.
Repair changes only the validated relative locator under a `TreeOnly`
successor; identity, accepted hash, cached document and B-Rep remain exact.
Failure before recompute, before commit, during successor preparation or
history finalization restores the previous model exactly. Ordinary definition
editing rejects linked definitions so the cache cannot diverge silently from
its accepted revision. A registry also rejects two local definitions with the
same `ExternalSourceKey`; occurrence reuse must target one shared definition,
so Reload never has an ambiguous multi-definition commit set.

The unified `AssemblyCommandHistory` stores cold Reload/Repair commands.
Reload retains a `DocumentDelta` plus before/after bindings; Repair retains
only bindings. Locator capacities and delta bytes count against the existing
64 MiB limit; exactly 64 MiB is accepted and limit+1 is rejected before model
mutation or redo-tail truncation. Undo/Redo apply these cold values without
any filesystem access, including their successor-failure rollback paths.
`LinkedPartLifecycleService` is the narrow application orchestrator from
read-only resolver result to the cold history command. It performs no UI,
publication, persistence or export work.

### V7 editor persistence and linked diagnostics (A6b2)

The editor dispatches V6, V7 and future envelopes exactly. V7 is scanned and
materialized completely before its linked descriptors are batch-resolved
relative to the assembly directory. Missing, Outdated, identity and bounded
resolver failures are application-owned diagnostics keyed by `DefinitionId`:
they are shown in the tree while the serialized last-valid cache remains the
authoritative model. The status carrier is never serialized. Unsafe V7 schema,
locator or cache input still rejects Open atomically. Undo and Redo reset the
affected runtime status to Unchecked because cold history deliberately performs
no filesystem read.

V7 Save uses the common atomic publication transaction and the assembly
history savepoint. Save As creates a new product identity only after successful
publication. Moving a linked assembly to another directory fails closed unless
the unchanged locator resolves from both roots to the same external identity,
accepted revision and canonical source; a same-directory fork remains possible
while a source is missing. A destination that aliases any linked source is
rejected. Alias checks use a read-only file-identity plus canonical-path
comparison; `Missing` is distinct from an identity-query error, and every
metadata/canonicalization error fails closed before publication. The authored
normalized locator target remains reserved even while the source is missing.
When the leaf itself is missing, the common V7/V8 policy resolves each longest
existing parent, compares its physical identity, and appends/compares the full
missing suffix under fixed depth/work budgets. Before resolution, the raw
destination rejects explicit `.`/`..` components for `/` and `\` spellings,
including drive and UNC forms, without lexical cleaning; the writer receives
that exact validated path. Windows drive-relative forms (`C:foo`, `C:..\foo`
and bare `C:`) are rejected as ambiguous, while rooted drive and UNC forms
continue through the ordinary checks. Suffix comparison uses a typed
filesystem policy: exact components for the POSIX/Linux policy, normalized
but case-sensitive components for macOS case-sensitive volumes, or normalized
plus case-folded components for proven insensitive directories.
Unknown sensitivity/normalization and reparse/metadata failure reject without
probing writes. Symlink/junction parents therefore cannot make a missing
authored target alias a Save As destination.
Save, Save As, Reload and failed Repair never write component files or place
temporary/sidecar files beside them.

The Russian Reload and Repair actions are available only for a selected linked
definition/occurrence in a quiescent V7 assembly. They cross the native
`LinkedPartLifecycleService` boundary, so widgets never receive an internal
filesystem capability. Repair converts the chosen file into a contained safe
relative hint and revalidates the complete assembly snapshot after the modal
loop. Projection publication remains atomic and preserves shared occurrences,
authored occurrence fields and selection. Full-product export has a typed
strict linked preflight (including orphaned and suppressed definitions), but
actual assembly STEP/STL commands remain disabled until A9.

### Nested assembly graph and disconnected V8 (A6c0)

`AssemblyGraphAnalysis` is the single Qt-free validator/traversal shared by
the model and V8 scanner. It walks iteratively (including orphan components),
uses active-stack colors so a shared DAG is legal while every direct or
indirect cycle is rejected, emits deterministic child-first assembly order,
and expands the root into lexicographically ordered full occurrence paths.
Each terminal leaf carries its part definition, canonical composed world
transform, authored leaf flags, and ancestor-derived effective
visibility/suppression; grounded remains an authored leaf property.

Nested model construction is available only through the atomic batch
`ProductDocument::createNested` boundary. Forward references are staged, the
whole registry (including global linked-source uniqueness) is validated, and
only then is a product published. Existing flat incremental authoring remains
fail-closed for assembly targets. Graph budgets are centralized at 256
definitions, 4096 authored occurrences, path depth 32, 65536 expanded visits,
32768 leaf instances and 128 graph-wide unique part sources (orphans included);
reachable sources are a separate diagnostic metric. Registry/JSON preflight
checks raw counts and aggregate sums before detached graph/result cloning, and
leaf capacity is checked before its full path is copied.

V8 is a separate strict disconnected codec with the exact
`nested-assemblies-v1` capability. It reuses V7 part payloads, linked bindings
and last-valid caches without changing V6/V7 schemas or bytes. Persisted
quaternions must already be the exact canonical
representation emitted by `RigidTransform3d::create`; scaled or sign-flipped
equivalents are rejected rather than silently normalized. Parts are
recomputed first and assemblies are constructed child-first before one atomic
model publication. A6c1b routes that same scanner/materializer through editor
Open and uses a self-validating V8 atomic writer without changing V6/V7.

### Recursive presentation identity without V8 editor routing (A6c1a)

The immutable presentation snapshot owns the exact successful
`AssemblyGraphAnalysis`. Presentation builds it once, creates one mesh source
per reachable `PartDefinition`, and creates terminal instances by full
`OccurrencePath` with composed world transforms and ancestor-effective
visibility/suppression. Session and tree consume that published graph instead
of launching independent recursive traversals. Intermediate assembly visits
remain addressable even though only terminal parts have render instances.

The tree mirrors occurrence hierarchy and keeps local occurrence IDs distinct
under different prefixes. Both part and assembly definitions have reachability
rows, including disconnected orphans. A terminal leaf may be selected,
hovered and isolated by full path; an intermediate assembly is selectable in
the tree but has no viewport highlight and its isolation action is disabled.
Picking, rectangle selection, snap, ruler and highlights preserve the same
full-path prefix while sharing definition-local topology/mesh sources.

`ProductDocument::resolveOccurrence` is the typed read-only recursive resolver.
Root mutation commands remain intentionally depth-1 and fail closed for nested
paths. Assembly semantic history stores full paths and charges the capacity of
both before/after path vectors to the existing 64 MiB retained-byte budget.
It prepares every vector-backed semantic result before touching the live
model or history cursor; post-commit publication consumes only no-throw moves.
The full MainWindow projection likewise resolves the occurrence selection,
highlight and exact Part/Assembly definition row against the candidate
session/tree before the model transaction commits; its noexcept boundary
converts selection-copy allocation failures into typed resource rejection.
Insert Undo/Redo similarly prepares the complete owning payload, including
the authored occurrence name, before entering the busy/model boundary.
DefinitionEdit and Linked Reload Undo/Redo also clone the live `Document` and
apply the directional cold delta into a typed owning candidate before that
boundary; the transactional model call consumes that candidate without a
second history-layer copy.
Linked Reload/Repair remains definition-wide, so all nested instances update;
cold Undo/Redo restores the complete selected path without filesystem access
and resets the application-owned linked status to Unchecked.

A6c1a does not route V8 through editor Open/Save/Save As and does not claim the
native multi-DPR nested GPU marker. Those are the explicit A6c1b boundary.

### V8 editor lifecycle and recursive native publication (A6c1b)

Application dispatch is exact: V6, V7 and V8 use distinct scanners, while a
version above 8 is Future. Assembly-only staging rejects V8; product staging
must finish V8 scan and disconnected materialization before the existing
read-only linked-source batch runs. That batch includes orphaned and
suppressed linked definitions. Missing/Outdated sources preserve last-valid
geometry without changing authored hashes. Identity mismatch, security-policy
rejection, invalid source bytes, bounded resource failure and source change
during resolution are likewise application-owned diagnostics on `lastValid`,
not hard Open failures. The bounded batch stops on its first failure, leaving
the other linked definitions `Unchecked` rather than falsely `Exact`. Only an
unsafe lexical descriptor or invalid V8 schema/serialized cache fails before
publication. Runtime diagnostics remain owned by the application.

V7 and V8 Save/Save As share one format-strategy policy for aliases,
cross-directory relocation and no-cascade source handling. Existing products
preserve their exact format, new assemblies remain V7, and V6 has no writer.
The V8 writer performs encode, exact self-scan and disconnected
materialize/recompute before opening the common atomic transaction. Save As
forks identity only after the target bytes have been atomically published.
MainWindow revalidates product incarnation/root/identity, every definition
state, full semantic selection, history/savepoint, presentation/publication,
hover/isolation and tool/dialog quiescence after the modal chooser.
The full-product export boundary resolves every linked definition, including
reachable, orphaned and suppressed definitions, and requires an all-Exact
batch before output creation. Missing, Outdated, identity, security,
invalid-source, source-changed and resource failures all block. No real
assembly exporter is claimed before A9.

V9 keeps mate semantics in the model: every `MateRegistry` owns one validated
`MateSolverOptions` value, and option replacement changes the same revision and
fingerprint seal as relation edits. Solver entry points without an explicit
policy always consume that authored value; an explicit mismatching policy is
rejected. The V9 persistence layer is disconnected from editor Open/Save and
returns application-neutral, sorted runtime diagnostics beside the staged
product. See `docs/project-format-v9.md`. Editor routing, atomic writing and
fuzz integration are intentionally A7b1.

The A7b0 materializer holds the explicit-ID reservation pause across its whole
disconnected transaction. `ProductDocument` uses a non-published provisional
incarnation while that pause is active and commits one global incarnation only
after final graph validation. No candidate, failure injection, semantic hard
failure or rejected successor can leak cursor movement or mutate a live source
product.

A7b1 publishes V9 through the same disconnected application boundary as V8,
but keeps mate diagnostics as a move-only workspace carrier. MainWindow owns
that carrier only after assembly replacement; it validates the carrier against
the command-history revision and registry revision/fingerprint/options seals.
The atomic V9 writer uses the materializer's private ValidationOnly mode twice
before opening the format-neutral transaction, so persistence validation never
consumes global IDs or mutates the live product. Exact V7/V8/V9 format routing
is shared by Save and Save As; promotion to V9 is explicit state, not a writer
side effect.
Regression coverage proves both a real mate-bearing writer fixed point and an
organic pre-open mismatch, plus exact rollback of a dirty V9 editor workspace
on hard materialization failure and the V9-specific Save As alias,
cross-directory, redo/savepoint, diagnostic-seal and no-cascade policies.

A7c1 extends the same Qt-free model boundary with a tagged planar mate registry:
coincident, parallel and perpendicular relations share IDs, component
localization, option ownership and candidate stale seals. Parallel is an
orientation-only rank-two constraint and perpendicular is a rank-one normal
dot constraint. The persistence layer advertises the conditional
`assembly-planar-angular-mates-v1` capability; no editor or mate-tool UI is
introduced by this step.

A7c2 extends that same tagged registry with `AxisConcentricMate`. An endpoint
is an owner-relative occurrence path plus either a local Part datum X/Y/Z axis
or an authoritative V2 cylindrical face reference. Resolution reuses the
generic nested occurrence traversal and produces an unoriented canonical
infinite line. The unified `solveAssemblyMates` pipeline rotates about the
moving line anchor and translates only across the fixed line, contributing
rank four while preserving axial slide and twist. `solvePlanarMates` remains a
source-compatible wrapper over the unified solver. V9 conditionally owns the
sorted `assembly-axis-concentric-mates-v1` capability; A7c2 remains headless and
adds no editor surface.

The A7c2 review hardens the model boundary in two places. Generic mate endpoint
inspection is a total typed view over planar and axis endpoints; explicitly
named planar accessors return null for an axis relation and never terminate on
a valid variant. A sealed placement candidate also records every occurrence
traversed by each nested path plus terminal Part state and referenced topology
revision/authority. Apply revalidates those deterministic dependency seals
before its first placement mutation, so nested-tail edits and direct topology
changes are stale no-ops with no successor publication.

A7c3 appends the headless symmetric `PointDistanceMate` family without changing
the established planar/concentric ordering. Each endpoint is an owner-relative
path plus either the Part-local datum origin or the center of an authoritative
V2 circular edge. The unsigned strictly-positive distance contributes rank one.
Projection translates only the deterministic free occurrence along the current
point-to-point direction and never rotates it. Representable small separation
remains valid below the authored solve tolerance; exact or numerically
unresolved coincidence is retained as typed `DegenerateDirection/Broken` with
no invented axis, candidate, or mutation. Circular-edge lookup work and all
nested, definition and topology dependencies participate in the same bounded
candidate seal and pre-mutation stale revalidation.

A7c4 appends the headless `PlanarAngleMate` family. It constrains oriented V2
planar-face normals to the unsigned acute authored magnitude
`n1 dot n2 = s*cos(angleRad)` and contributes one rotational row. Projection
rotates only the deterministic movable occurrence about the resolved moving
face pivot; translation compensation keeps that pivot fixed. An unsatisfied
collinear pair has no authored cone tangent, so it is retained as typed
`DegenerateDirection/Broken` rather than inventing a world-axis fallback. The
shared planar resolver meters the complete authoritative face index before
lookup and accepts only FORWARD/REVERSED OCCT orientations. Null/evaluation
failures remain typed and allocation failures reach the outer resource
boundary. A7c4 adds no editor surface.

Angle cone construction is scale-free: the same projected tangent validator is
used by pre/post-sweep degeneracy checks and projection, and only an exact or
unrepresentable zero direction is rejected. The rank row and cone rotation use
`atan2(norm(cross), dot)`, avoiding the loss of derivative near valid angular
boundaries and preserving representable sub-`1e-15` directions. An Angle
already within its authored tolerance is left byte-for-byte unchanged, while
the exact zero-sine/same-direction cone rotation is identity. The model-owned
minimum angular tolerance is `2^-42` rad: 64 quanta of the placement
quaternion's `2^-48` component grid cover rotation-matrix amplification and
the canonicalizer fixed-point margin without changing the `1e-9` default.
Persistence and registry mutation use this same lower boundary.

The opt-in A6c1 native scenario opens a real V8 file through MainWindow and
uses the shared subassembly paths `[1,7]` and `[2,7]`. Its marker is
conjunctive over non-commutative transforms, one source/two instances,
full-path picking/selection/hover/highlight, terminal isolation, disabled
intermediate isolation, ancestor hide/suppress without upload, linked Reload
followed by source deletion and cold Undo/Redo, one upload per geometry
revision, and old tree/scene/mesh/GPU retirement. The external runner remains
responsible for the native DPR matrix.

A8a adds the first editor authoring boundary for the existing root-owned
planar-coincident relation only. A typed face pick is publication-scoped and
carries a full occurrence path, terminal definition and authoritative V2 face;
the UI calls the model's shared planar endpoint classifier and never performs
its own OCCT classification. The value-only `AssemblyMateToolSession` owns the
two-stage selection/revision state and a sealed immutable authoring
transaction, but no
ProductDocument, QModelIndex or transient subshape.

Preview solving supplies a registry overlay to the same localized solver core,
so the live registry revision/fingerprint and every placement remain exact.
The viewport builds a visual-only scene from the sealed candidate while
reusing committed immutable mesh sources; the candidate scene is never
published to the committed picking index. While that
visual is installed, face hover/click is deliberately frozen so invisible
committed geometry cannot be picked. Presentation
preparation is swap-only, which preserves the previous last-valid visual when
the current solve or scene preparation fails while disabling Apply.

Apply performs a fresh solve and enters one ProductDocument transaction that
swaps the exact proposed registry and all connected placements before a single
`MateProjection` successor publication. Its inverse restores the exact prior
registry revision/fingerprint and placements. `PlanarMateColdCommand` is
appended to the existing history variant, so Undo/Redo reuse those atomic
boundaries. V7/V8 persistence state is validated before mutation and promoted
explicitly to V9 only after the history command commits; preview, Cancel,
failure, Save and Save As do not infer promotion.

Cold mate-history retained bytes conservatively charge each registry map for
one full sentinel/head node and each relation for its value plus five-pointer
node overhead, then add owned paths, V2 provenance/origins and candidate
dependency vectors with checked arithmetic. This is intentionally independent
of the current MSVC `std::map` node layout.
