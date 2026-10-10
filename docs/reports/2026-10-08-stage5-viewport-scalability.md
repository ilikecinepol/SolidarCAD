# Stage 5 viewport scalability report

Date: 2026-10-08
Preset: `ci` (Release, MSVC, Qt 6.8.3, vcpkg OCCT)
Raw data: `build/ci/stage5-results.csv` (44 rows, SHA-256
`B94DCF8CE2B640982ACE427EDCEF21AE12125096B3A07A8F83BFC473B05BB459`)

## Measurement contract

The opt-in runner executes historical and production implementations against
the same immutable `BodyRenderMesh`, camera and query coordinates. It rejects
each pair when the semantic result hash or input-mesh byte count differs.
Historical marquee selection is a frozen literal candidate x triangle scan;
it does not call the production BVH collectors. Camera-reprojection hashes
real face, edge and marquee query results after every rebuild. The benchmark
also executes clipped-overlap, historical-control-flow and near-depth boundary
fixtures before every case; both implementations use the mesh-diagonal depth
tolerance.

Defaults were used: 5 warmups and 101 samples for the 10k/100k/1M grid,
11 samples for dense actual calibration, and one real 120-request OCCT drag.
All 44 rows completed and every comparable pair passed the runner's semantic
checks.

## 1M grid: before/after

Times are milliseconds.

| Scenario | Historical p50 | Historical p95 | Historical p99 | Production p50 | Production p95 | Production p99 |
|---|---:|---:|---:|---:|---:|---:|
| hover face | 2.3346 | 2.4310 | 2.4566 | 0.0021 | 0.0032 | 0.0035 |
| hover edge | 14.3997 | 17.8302 | 17.9839 | 0.0007 | 0.0014 | 0.0017 |
| marquee face | 63.2440 | 65.5195 | 66.0526 | 0.0031 | 0.0042 | 0.0046 |
| marquee edge | 22.1114 | 24.0337 | 24.1693 | 0.0010 | 0.0017 | 0.0021 |
| camera reproject | 237.7089 | 242.0542 | 246.8752 | 1655.9963 | 1676.6031 | 1679.7887 |

Memory is the benchmark's input plus derived owned capacity. At 1M, that is
469.4 MiB historical versus 1360.9 MiB production. RSS delta is the
operating-system peak for the individual process; camera-reproject measured
301.7 MiB historical versus 1313.5 MiB production.

The production sparse-grid hover and marquee paths are indexed and comfortably
below the 16.7-33 ms interaction target. Camera changes deliberately rebuild
projected vertices plus the triangle, segment and vertex BVHs; at 1M triangles
this is a dominant remaining bottleneck and is not a mouse-hover query.

The acceleration structures trade memory for query latency. On the synthetic
1M fixture, production owned memory is 1.33 GiB and its camera-reproject RSS
delta is 1.28 GiB, versus 469.4 MiB owned and 301.8 MiB RSS delta for the
historical projected full-scan representation. Production does not duplicate
world-space triangle positions: it stores projected vertex references and
compact topology references, but three BVHs dominate at this deliberately
pathological scale. Reducing BVH node/index width or rebuilding only the
camera-dependent bounds is the concrete follow-up if 1M interactive camera
motion becomes a product requirement.

## Dense occlusion calibration

| Triangles | Historical actual p95 | Production actual p95 | Historical occluder tests | Production occluder tests |
|---:|---:|---:|---:|---:|
| 256 | 0.2682 ms | 0.0350 ms | 720,896 | 2,794 |
| 1,024 | 4.2003 ms | 0.1458 ms | 11,534,336 | 11,242 |
| 4,096 | 67.1202 ms | 0.6211 ms | 184,549,376 | 45,034 |

Large historical dense rows are work-only estimates, as documented by the
runner: 100M, 10B and 1T occluder tests at 10k, 100k and 1M. The paired
production 1M actual run completed with p95 194.6084 ms and 10,998,889
occluder tests. Thus exact dense 1M occlusion remains outside the interaction
target even though the sparse-grid query paths meet it.

## Preview drag

For the same 120 parameter requests, historical mode performed 120 preview
executions and 120 mesh builds (p95 313.4754 ms). Production coalesced 119
requests, performed one preview execution and one mesh build, and completed at
p95 5.8959 ms. Its maximum synchronous phase was 4.1127 ms, below the 50 ms
gate.

## Result

Stage 5's sparse-grid hover/marquee and drag targets are met on this machine,
with matching deterministic semantics. The remaining measured scalability
limits are exact dense 1M occlusion and camera-dependent projected-scene/BVH
rebuild time and memory at the synthetic 1M scale; they are explicitly
reported rather than hidden behind a timing gate.
