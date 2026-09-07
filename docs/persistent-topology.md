# Persistent Topological Naming v1

## Previous architecture

`FaceReference` and `EdgeReference` already identified their owning `BodyId`,
source `FeatureId`, subshape kind and a legacy OCCT enumeration index. The
common `TopologyReference` also reserved string fields for future persistent
data, but resolution used only `faceIndex` or `edgeIndex`. Sketch-on-Face and
Fillet were therefore vulnerable to a reordered OCCT face/edge traversal.

All shape-producing features (Extrude, Pocket, Fillet, Chamfer and Revolve) expose a
B-Rep through `ShapeFeature`. Sketch-on-Face consumes a face from a source
feature; Fillet consumes one or more edges from the immediately preceding
feature. Chamfer follows the same edge-reference contract as Fillet. Project
format v2 previously stored only owner IDs and legacy index.

## Reference structure

A reference now contains three identity levels:

1. owner identity: Body ID, Feature ID and Face/Edge kind;
2. optional semantic tag and structured geometric signature;
3. legacy enumeration index for old references only.

Planar faces receive a semantic extremum tag such as `planar:max-z` when it is
unambiguous. A face signature stores surface type, area, centroid, oriented
normal, bounding box, and optional cylindrical radius/axis. An edge signature
stores curve type, length, midpoint, tangent direction, bounding box, and
optional circle radius/center. References are created with
`makeFaceReference` and `makeEdgeReference`; selection code no longer creates
new persistent references directly from an index.

## Resolution and tolerances

Resolution is centralized in `TopologyReferenceResolver` and uses this order:

1. unique semantic-tag match;
2. unique best geometric-signature match;
3. legacy index only when persistent data is absent;
4. a specific diagnostic on missing or ambiguous matches.

The v1 centralized tolerance policy permits 20% of the source-shape diagonal
for normalized position, 35% relative change in area/length/radius, a direction
cosine of at least 0.996, and treats best scores within 0.01 as ambiguous.
Candidates are collected in one linear B-Rep traversal; no tessellation,
pointer identity or whole-shape hashing is used.

If a persistent signature exists but no safe match exists, the resolver does
not silently fall back to the old index. A failed Fillet or Chamfer becomes Error with the
resolver diagnostic. An unresolved face support prevents downstream profile
construction. Old v2 files omit signatures and therefore retain index fallback;
after a successful runtime resolution, face supports are enriched in memory.

Optional signature objects were added to project format v2, so the version was
not increased. Readers continue to accept v1/v2 files, and missing signature
fields preserve the previous behavior.

## Known limitations

Version 1 is geometric matching, not OCCT TNaming/OCAF. It does not guarantee
identity through arbitrary split/merge operations, strong topology changes,
symmetric indistinguishable candidates, pattern-generated duplicates, or
complex spline/NURBS evolution. Such cases intentionally produce ambiguity or
missing-match diagnostics. Semantic tags currently cover only obvious planar
shape extrema. A later version should add feature-history provenance and richer
operation-specific semantic naming before considering OCAF integration.
