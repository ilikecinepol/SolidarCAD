#pragma once

#include <cstddef>
#include <vector>

#include "sketch/Sketch.h"

namespace solidar::sketch {

class Sketch;

struct SolveResult {
  std::size_t applied{};
  std::size_t unsupported{};
  std::size_t invalidReferences{};
  bool converged{true};
  std::size_t violatedConstraints{};
  double maxNormalizedResidual{};
  std::size_t passes{};
  std::size_t componentsVisited{};
  std::size_t geometriesVisited{};
  std::size_t constraintsVisited{};
  std::size_t lockedSnapshotSize{};
  std::size_t fullDiagnosticsCount{};
  std::size_t allocationCount{};
  std::size_t allocatedBytes{};
  std::size_t peakOwnedBytes{};
  bool deferredByActiveSolve{false};
};

class BasicSketchSolver final {
 public:
  [[nodiscard]] static SolveResult solve(Sketch& sketch);
  [[nodiscard]] static SolveResult solveStable(
      Sketch& sketch, int maxPasses = 16);
  [[nodiscard]] static SolveResult solveStableComponent(
      Sketch& sketch, const std::vector<GeometryId>& dirtyGeometry,
      int maxPasses = 16);
  // Frozen pre-Stage-6 orchestration used only by the opt-in benchmark. It
  // intentionally snapshots the complete Sketch so baseline and optimized
  // runs share the exact same fixture and executable schema.
  [[nodiscard]] static SolveResult solveStableHistoricalForBenchmark(
      Sketch& sketch, int maxPasses = 16);
  [[nodiscard]] static SolveResult translateThenSolveForBenchmark(
      Sketch& sketch, const std::vector<GeometryId>& dirtyGeometry,
      double dxMm, double dyMm, bool historical);

 private:
  [[nodiscard]] static SolveResult solveStableLowLevel(
      Sketch& sketch, int maxPasses);
};

}  // namespace solidar::sketch
