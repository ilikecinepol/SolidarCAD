# SolidarCAD product envelope V9

V9 is the first product envelope that persists planar assembly mates. It is a
strict, separate codec; V8 decoding and byte output are unchanged.

The root has the V8 product fields and version `9`. Coincident-only documents
retain the exact original sorted capability list:
`assembly-planar-mates-v1`, `linked-parts-v1`, `nested-assemblies-v1`,
`product-envelope-v1`. A document containing a parallel or perpendicular mate
adds `assembly-planar-angular-mates-v1` as the first sorted capability. The
capability is required when used and invalid when unused. A document containing
an axis-concentric mate independently adds the sorted
`assembly-axis-concentric-mates-v1` capability. Thus the codec accepts exactly
the base, angular, concentric, or angular+concentric capability set. Part payloads,
occurrences and placements are exactly the V8 schemas.

Every assembly has exactly `definitionId`, `definitionKind`, `occurrences`,
`mateSolverOptions`, and `mates`. Solver options are authored model state and
are bounded by the model's hard ceilings. Mates are sorted by positive local
`mateId`; IDs are unique only within their owning assembly and are never used
as a global identity. `planar-coincident-v1` stores two endpoints,
`orientation: aligned|opposed`, and `offsetMm`; `planar-parallel-v1` stores the
same endpoints and orientation but no offset; `planar-perpendicular-v1` stores
only its two endpoints. `axis-concentric-v1` also stores only `mateId`, `type`
and its two endpoints; it has no orientation, offset, radius, point or
direction. Its endpoint `axis` is exactly either a `local-datum-axis-v1`
reference (`x|y|z`) or a `cylindrical-face-axis-v1` reference containing a full
authoritative V2 face reference. Extra or cross-family fields are invalid and an unknown
type is unsupported. Each endpoint contains a full
owner-relative occurrence path (depth 1..32, terminal target must be a Part)
and an exact V4 V2 face reference. V1 references are invalid; unknown reference
or provenance vocabulary is unsupported.

Only authored options, mates, paths, V2 references and placements are stored.
Solver status, rank, DOF, residuals, conflicts, work, candidates, registry
revision/fingerprint and the derived lowest-free mate ID are runtime data and
must not appear in JSON. Non-finite values are rejected. A bounded JSON-aware
token pass rejects authored negative zero specifically for the ten solver
option fields and `offsetMm`; exponent signs remain part of their number token,
so `1e-00` is valid and `-0e+3` is not. Strings and unrelated numeric fields do
not participate in this rule.

The disconnected scanner inherits all V8 budgets and additionally limits 128
mates per owner, 4096 mates, 8192 endpoints, 262144 path entries, 65536
provenance records, 256 origins per reference, 4096 solve components and
1,000,000,000 aggregate solve work units.

Materialization rescans the exact candidate, opens one ID-reservation
transaction, materializes the inherited V8
graph, restores registries child-first, and solves each connected component
once from its lowest unvisited mate ID. Solved, Underconstrained and Redundant
components require and apply a sealed candidate. Missing, Ambiguous, Broken,
Overconstrained and NonConvergent components retain authored mates and the
serialized last-valid placements. InvalidInput, Busy, Stale, ResourceFailure
or apply failure reject the whole disconnected candidate. The reservation
transaction spans part rebuild, product construction, all solve/apply work and
final graph validation. Product incarnation is provisional inside it and only
one real incarnation is consumed on full success; every failure leaves the
Body/Sketch/Feature/Product cursors unchanged. Runtime diagnostics are returned
out-of-band and are never serialized.

V7 and V8 writers fail closed if the source contains authored mates. A mate-free
V8 remains byte-compatible, while an empty V9 assembly remains V9.

## Application publication and atomic writing (A7b1)

Application dispatch is exact: version 9 is `ProductV9` and any exact integer
above 9 is Future. The part-only Open stage rejects V9; product Open performs
the strict scan and publication materialization, then carries the sorted mate
diagnostics out-of-band through the prepared assembly workspace. Retained
semantic solve statuses open on the serialized last-valid placements. Hard
solve, apply, resource or finalization failures publish neither product nor
diagnostic carrier and consume no global model IDs.

The V9 writer is side-effect-free before publication. It encodes `B0`, scans
and materializes it in `ValidationOnly` mode, re-encodes `B1` and requires
`B1 == B0`; it then repeats scan, validation materialization and encoding and
requires the same stable bytes. ValidationOnly performs full part rebuild,
registry restoration, child-first solve/apply and final graph validation while
the ID reservation pause is held, but never finalizes a Product incarnation.
Only after this fixed-point proof does the common atomic transaction perform
open/write/flush/sync/replace. Every preparation or I/O failure preserves the
source, destination and Body/Sketch/Feature/Product cursors.
The writer gate includes a real solved mate-bearing fixed point and its
organically unsolved counterpart: the latter changes after validation solve,
so `B1 != B0` rejects it before transaction open with exact source,
destination and cursor rollback.

Save and Save As preserve the exact active V7/V8/V9 format; no persistence API
silently promotes or downgrades. V7/V8 reject both nonempty mate registries and
non-default authoritative options. V9 stays V9 even when all registries are
empty. Promotion is an explicit application validation/state transaction for
future A8 UI work. Runtime diagnostics carry a validity seal consisting of the
assembly history revision and every assembly registry's revision, fingerprint
and options. Successful model/history mutations, Undo, Redo, Reload or Repair
make the carrier Unchecked; failed operations, selection/hover/isolation and
Save/Save As leave it current. Diagnostics never enter JSON, model history or
the authored graph.
The UI rollback gate stages a hard V9 resource failure over an already dirty
workspace with a redo tail and active selection, hover and isolation, then
checks the full workspace and all global cursors are unchanged. V9 Save As is
also exercised explicitly for successful identity forking, alias rejection,
cross-directory fail-closed behavior, redo/savepoint preservation and no
cascading write to a missing linked source.

## Planar angular family (A7c1)

Parallel and perpendicular relations share the owner-local registry and the
localized mixed solver with coincident mates. Parallel contributes rank two
and rotates only orientation about the resolved moving face pivot; it does not
coincide or offset planes. Perpendicular contributes rank one and drives the
normal dot product to zero, using a deterministic finite fallback for a
collinear starting pair. Type tags and parallel orientation participate in the
registry fingerprint/candidate stale seal. V7/V8 remain fail-closed for every
mate kind, and the coincident-only V9 encoder bytes remain unchanged.
The compatibility test freezes accepted pre-A7c canonical bytes containing one
complete `planar-coincident-v1` relation as a literal `QByteArray`; it includes
the exact type, opposed orientation, offset and both V2 endpoints and is not
regenerated from the current encoder.

## Axis concentric family (A7c2)

Concentric relations constrain two occurrence-scoped infinite axes without
giving either axis a direction. Local datum X/Y/Z axes originate at the Part
origin. Cylindrical references resolve the authoritative V2 face, require an
exact finite positive-radius cylinder and derive a canonical line independent
of face orientation and the surface axis location. The solver chooses the
shorter of the two equivalent direction rotations about the moving-axis
anchor, then applies only the perpendicular shortest-line translation. It
never introduces axial slide or twist. The relation contributes rank four and
leaves two degrees of freedom.

Axis reference discriminator, datum value or complete V2 provenance are part
of the registry fingerprint and sealed placement candidate. Concentric mates
share the localized mixed component pipeline, exact work accounting,
child-first V9 materialization and atomic stale/apply/resource rollback with
all planar families. The focused gate covers datum/datum, cylinder/cylinder,
cylinder/datum, coaxial/off-axis/intersecting/skew geometry, endpoint and ID
permutations, exact N/N-1 budgets, strict capability/schema mismatch, real
nested fixed points and all seven materialization failure seams. The frozen
coincident-only and angular-only compatibility bytes remain unchanged.

The accepted A7c2 bytes are also frozen independently with one local datum
endpoint and one complete cylindrical V2 endpoint. The compatibility gate
compares the current compact root byte-for-byte, then separately proves a real
cylinder/datum topology document reaches a child-first solve/encode fixed
point. Missing/extra cylindrical fields and V1 faces are invalid.

## Point distance family (A7c3)

`assembly-point-distance-mates-v1` is independently conditional and sorted
after `assembly-planar-mates-v1`, yielding the exact eight combinations of the
angular, concentric and point-distance capability bits. A relation has exact
type `point-distance-v1` and fields `mateId`, `type`, `first`, `second`, and
`distanceMm`. The authored distance is unsigned, canonical, finite and in
`(0, maximumAbsoluteOffsetMm]`; raw negative zero is invalid.

Each endpoint is exactly `{occurrencePath, point}`. A datum origin point is
`{"referenceKind":"local-datum-origin-v1"}`. A circular center is
`{"referenceKind":"circular-edge-center-v1","edge":<V2 EdgeReference>}`.
V1, missing/extra fields and known capability mismatch are Invalid. Unknown
capabilities, mate types, reference kinds or reference enums are Unsupported.
Runtime residuals, rank, DOF, topology failures and candidates are never
serialized, and V7/V8 continue to reject every nonempty mate registry.

## Planar angle family (A7c4)

`assembly-planar-angle-mates-v1` is an independently conditional capability;
it does not broaden `assembly-planar-angular-mates-v1`. With the four optional
axis-concentric, planar-angle, planar-angular and point-distance bits, the
decoder accepts exactly all sixteen canonical combinations. Capability order
is axis-concentric, planar-angle, planar-angular, planar-mates, point-distance,
then linked-parts, nested-assemblies and product-envelope.

An angle relation has exact type `planar-angle-v1` and exact fields `mateId`,
`type`, `first`, `second`, `orientation`, and `angleRad`. Both endpoints reuse
the complete authoritative V2 planar-face schema. Orientation is `aligned` or
`opposed`; the authored magnitude is canonical finite radians with
`0 < angleRad < pi/2` and must also remain outside both operational angular
tolerance boundaries. Raw negative zero is invalid. Unknown capability/type is
Unsupported; malformed known schema or a missing/unused capability is Invalid.
No derived diagnostic, residual, rank, candidate, pivot or OCCT value is
serialized. Older byte oracles remain unchanged, and V7/V8 fail closed.

`mateSolverOptions.angularToleranceRad` has the model-owned inclusive lower
bound `2^-42` rad and upper bound `pi`. The decoder rejects the immediate
representable predecessor of the lower bound; the exact boundary is valid and
round-trips canonically. The default remains `1e-9` rad.
