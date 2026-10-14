# Opt-in performance benchmarks

Benchmarks are excluded from ordinary builds and are not registered with
CTest. Stage 5 emits CSV only; it has no unstable timing assertion.

## Reproducible Windows build

Run from an x64 Native Tools command prompt, or initialize the same environment
explicitly. Quoted `set` syntax is intentional: it prevents trailing spaces in
the custom triplet name.

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "VCPKG_ROOT=%CD%\vcpkg"
set "VCPKG_DEFAULT_TRIPLET=ci-x64-windows"
set "CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64"
cmake --preset ci -DSOLIDAR_BUILD_BENCHMARKS=ON
cmake --build --preset ci --clean-first --target stage5_viewport_scalability_benchmark
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%CD%\build\ci\vcpkg_installed\ci-x64-windows\bin;%PATH%"
```

The full matrix is intentionally opt-in and can take a long time, especially
the real 120-step historical OCCT drag:

```powershell
./benchmarks/run_stage5_viewport_benchmark.ps1 `
  -Executable ./build/ci/benchmarks/stage5_viewport_scalability_benchmark.exe `
  -OutputPath ./build/ci/stage5-results.csv
```

Defaults are 5 warmups and 101 samples for the 10k/100k/1M sparse grid, 11
samples for dense actual calibration at 256/1024/4096 triangles, and one real
drag sample. Override `-Counts`, `-Samples`, or `-DenseSamples` for a smoke run.

## Measurement contract

`historical` and `production` use the same immutable `BodyRenderMesh`, camera,
and query coordinates. The runner rejects mismatched semantic result hashes or
input-mesh byte counts.

- Grid historical rows materialize projected triangles/edge samples and run
  the removed full scans. Production rows drive the real
  `ProjectedPickingScene` BVHs.
- Dense `actual` rows execute the historical edge-candidate × triangle scan at
  bounded sizes. Large dense historical rows are explicitly labelled
  `measurement=estimate`, report exact N² work, and have zero latency fields;
  the paired production rows are still actual measurements.
- Drag rows use a real square `DocumentSketch`, `ExtrudeToolSession`, OCCT
  preview construction, and `BodyRenderMesh::tryRebuild`. Historical executes
  all 120 values. Production submits the identical sequence through the real
  `PreviewUpdateCoordinator` and flushes the last value once.

Memory columns are comparable: `input_mesh_bytes` is the shared
`BodyRenderMesh::ownedBytes()`, `derived_peak_bytes` is either the historical
projected scratch plus the modeled removed expanded `RenderTriangle` copy, or
`ProjectedPickingScene::ownedBytes()`. RSS before/peak/delta is also reported.
The CSV includes p50/p95/p99, actual versus estimated occlusion work, BVH node
visits, preview/coalescing counts, OCCT and mesh build counts, request maximum,
flush time, and maximum synchronous phase.

## Stage 6 sketch solver

`stage6_sketch_solver_benchmark` is also opt-in. Its runner executes the exact
100/500/1000 entity matrix for localized drag, one fully connected component,
an unchanged solve, full DOF/conflict diagnostics, all line/circle/arc/Bezier
tangency combinations and Bezier-heavy diagnostics:

```powershell
cmake --build --preset ci --target stage6_sketch_solver_benchmark
./benchmarks/run_stage6_sketch_solver_benchmark.ps1 `
  -Executable ./build/ci/benchmarks/stage6_sketch_solver_benchmark.exe `
  -OutputPath ./build/ci/stage6-sketch-solver-production.csv
```

The runner enforces the R4 interactive boundaries for the 100-geometry
reference fixtures (p95 <= 16.7 ms and p99 <= 50 ms), finite latency, accepted
solver status, normalized residual, non-zero semantic hash and deterministic
work counters. A localized drag visits exactly one component/geometry/
constraint, a connected drag visits the requested entity count, unchanged
solve performs zero iterations, and every diagnostics mode invokes full
`analyzeConstraintSystem()` exactly once. Larger workloads remain report-only
and publish their dense connected-component bottleneck. There is no runtime
Solver-v1 branch or fallback. The final pre-cutover V1/V2 comparison is
versioned in `docs/reports/data/2026-10-14-r4-shadow-comparison.csv`.
`allocation_count`, `allocated_bytes`, and `peak_owned_bytes` come from the
benchmark executable's scoped complete `operator new/new[]` override (regular,
sized, aligned, and nothrow forms). They therefore count actual allocation
events, requested bytes, and peak simultaneously live bytes during the measured
operation. The CSV also records fixture component, parameter and equation
counts. The override is not linked into production; process RSS remains
report-only and is intentionally not used as a deterministic gate.
