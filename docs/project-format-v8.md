# Product envelope V8 (A6c0/A6c1b)

V8 is the first disconnected persistence schema for nested assemblies. It is
also the exact editor Open/Save/Save As route for recursive products. V5, V6
and V7 schemas and bytes remain unchanged; an existing V7 project stays V7,
an existing V8 project stays V8 even when its current graph is flat, and a new
assembly is still authored as V7. V6 remains read-only.

The top-level object has the exact V7 identity/metadata/definition fields,
`version: 8`, and the exact sorted capability set
`linked-parts-v1`, `nested-assemblies-v1`, `product-envelope-v1`. Part
definitions reuse the V7 `embedded-part-v4` and `linked-part-v1` payloads,
including `lastValid`. Every assembly definition serializes its authored
occurrences in strictly increasing local `occurrenceId` order. Definition IDs
are globally unique and strictly increasing; occurrence IDs need only be
unique in their owning assembly.

The scanner is disconnected and fail-closed. It rejects unknown/missing
fields, unknown capabilities, non-canonical IDs/transforms, dangling targets,
duplicate external source identities, any direct/indirect/orphan cycle, and
all graph budget violations before materialization. Persisted quaternions must
already equal the canonical value emitted by `RigidTransform3d::create`
component-for-component; V8 never silently normalizes a scaled quaternion or
flips its sign. A bounded Qt parse and exact format/version dispatch run first:
V6/V7 and non-Solidar JSON are NotApplicable, while future versions are
Unsupported. Only an exact V8 envelope then receives a JSON-aware lexical pass
over the original bytes, before semantic transform parsing. The pass rejects
negative-zero number tokens (`-0`, `-0.0`, `-0e0`, including
uppercase exponent spelling) in each quaternion component, because a DOM
double cannot retain that authored distinction. Strings, translations and
unrelated arrays are not treated as quaternion tokens. This exact
representation includes finite unit rotation,
deterministic sign and the model's documented 2^-48 stabilization grid.
The shared Qt-free graph
analyzer enforces 256 definitions, 4096 aggregate authored occurrences, depth
32, 65536 expanded visits, 32768 leaf instances and 128 graph-wide unique part
sources, including orphan parts. Reachable sources are reported separately.
Raw registry/JSON counts, aggregate strings, part caches/locators and rebuild
work are bounded before decoded graph/result cloning; leaf capacity is checked
before copying its path. Qt's initial bounded (64 MiB) JSON DOM and caller-owned
authored model containers necessarily already exist at this boundary. Shared
DAGs are permitted.

Materialization recomputes every part while explicit model-ID reservation is
paused, then creates assemblies in child-first order and publishes the whole
registry atomically. A failure in any part, including the deepest part, leaves
no product and does not consume global model-ID cursors. Encoding is
deterministic and self-scans its bytes. The V7 encoder continues to reject a
nested product.

Editor Open dispatches exact V6/V7/V8 versions before publication. V8 scanning
and disconnected materialization complete before linked-source resolution;
the common read-only batch policy visits every linked part definition,
including orphaned, hidden and suppressed definitions. Missing and Outdated
sources retain `lastValid` without implicit acceptance. Identity mismatch,
security-policy rejection, invalid source bytes, source change during the
bounded read and resolver resource/budget failure follow the same established
V7 rule: Open succeeds on `lastValid` and records an application-owned
diagnostic for the first failed definition. Because the bounded batch stops at
that failure, later definitions remain `Unchecked`; they are never falsely
reported as `Exact`. Only an unsafe lexical binding descriptor or invalid V8
schema/serialized `lastValid` cache fails the whole stage. Runtime link status
is application-owned and is never written to the envelope.

V8 writing completes encode, exact self-scan and disconnected
materialize/recompute before opening the destination transaction. The common
atomic publisher then owns write, flush, sync and replacement. Save and Save
As share the V7/V8 alias and no-cascade policy: source files are read-only,
same-file/canonical/hardlink/symlink/reparse aliases and metadata uncertainty
fail closed, and Save As forks product identity only after atomic publication.
For a missing authored leaf, alias detection resolves the longest existing
parent of each path, compares physical parent identity, then compares the full
missing suffix under bounded work. The raw destination is first rejected if
any component is exactly `.` or `..` (both slash spellings, drive and UNC forms
included). Windows drive-relative spelling (`C:foo`, `C:..\foo` or bare `C:`)
is ambiguous and is rejected; only separator-rooted drive paths are eligible.
The path is never cleaned before those checks and the exact validated spelling
is passed to the writer. The POSIX/Linux case-sensitive policy compares suffix
components exactly. A normalization-aware case-sensitive filesystem (macOS)
normalizes each component but retains case; a proven case-insensitive
filesystem normalizes and case-folds. Unknown sensitivity/
normalization, canonicalization, metadata or reparse failure rejects before
the destination is opened; no probing write is permitted.

The typed full-product export preflight applies the same strict batch to every
linked definition, including reachable, orphaned and suppressed definitions.
Only an all-`Exact` result may proceed; Missing, Outdated, identity, security,
invalid-source, source-changed and resource outcomes block before any output is
created. This is preflight evidence only: real assembly exporters remain A9.
