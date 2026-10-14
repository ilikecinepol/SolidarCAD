#pragma once

#include <cstddef>
#include <vector>

#include "sketch/Sketch.h"

namespace solidar::sketch {

class Sketch;

// The single production transaction adapter around the equation-based
// nonlinear solver. It owns publication, caching, and rollback; the backend
// only produces candidate parameter vectors.
class SketchSolver final {
 public:
  [[nodiscard]] static SolveResult solveStable(
      Sketch& sketch, int maxPasses = 16);
  [[nodiscard]] static SolveResult solveStableComponent(
      Sketch& sketch, const std::vector<GeometryId>& dirtyGeometry,
      int maxPasses = 16,
      ConstraintId preferredConstraintId = kInvalidConstraintId,
      bool useDirtyGeometryHints = false);
  [[nodiscard]] static SolveResult translateThenSolveForBenchmark(
      Sketch& sketch, const std::vector<GeometryId>& dirtyGeometry,
      double dxMm, double dyMm);
};

}  // namespace solidar::sketch
