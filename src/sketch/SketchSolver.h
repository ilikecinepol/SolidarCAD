#pragma once

#include <cstddef>

namespace solidar::sketch {

class Sketch;

struct SolveResult {
  std::size_t applied{};
  std::size_t unsupported{};
  std::size_t invalidReferences{};
};

class BasicSketchSolver final {
 public:
  [[nodiscard]] static SolveResult solve(Sketch& sketch);
};

}  // namespace solidar::sketch