#pragma once

#include <cstddef>
#include <vector>

#include "sketch/Sketch.h"

namespace solidar::sketch {

struct ConstraintViolation {
  ConstraintId id{kInvalidConstraintId};
  ConstraintType type{ConstraintType::Horizontal};
  double normalizedResidual{};
};

struct ConstraintDiagnostics {
  std::size_t variableCount{};
  std::size_t equationRank{};
  std::size_t degreesOfFreedom{};
  std::size_t userEquationCount{};
  bool conflicting{false};
  bool fullyConstrained{false};
  double maxNormalizedResidual{};
  std::vector<ConstraintViolation> violations;
};

// Audit the COMPLETE current constraint system. With computeDof=true a
// numerical Jacobian rank is used, so dependent/redundant constraints do not
// incorrectly make the sketch look fully constrained.
[[nodiscard]] ConstraintDiagnostics analyzeConstraintSystem(
    const Sketch& sketch, bool computeDof = true);

[[nodiscard]] bool hasConstraintViolation(
    const ConstraintDiagnostics& diagnostics, ConstraintId id) noexcept;

}  // namespace solidar::sketch
