#pragma once

#include <cstddef>

namespace solidar::sketch {

class Sketch;

struct SolveResult {
  std::size_t applied{};
  std::size_t unsupported{};
  std::size_t invalidReferences{};
  bool converged{true};
  std::size_t violatedConstraints{};
  double maxNormalizedResidual{};
};

class BasicSketchSolver final {
 public:
  [[nodiscard]] static SolveResult solve(Sketch& sketch);
  [[nodiscard]] static SolveResult solveStable(
      Sketch& sketch, int maxPasses = 16);
};

}  // namespace solidar::sketch