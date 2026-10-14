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

struct ConstraintComponentDiagnostic {
  std::vector<GeometryId> geometryIds;
  std::vector<ConstraintId> constraintIds;
  std::size_t variableCount{};
  std::size_t equationRank{};
  std::size_t degreesOfFreedom{};
  SolveStatus status{SolveStatus::SolvedUnderConstrained};
  double maxNormalizedResidual{};
  std::size_t iterations{};
  std::vector<ConstraintId> conflictingConstraintIds;
  std::vector<ConstraintId> redundantConstraintIds;
  std::vector<ConstraintId> partiallyRedundantConstraintIds;
  std::vector<GeometryId> remainingDofGeometryIds;
  bool conflictSetComplete{true};
  bool conflicting{false};
};

struct ConstraintDiagnostics {
  std::size_t variableCount{};
  std::size_t equationRank{};
  std::size_t degreesOfFreedom{};
  std::size_t userEquationCount{};
  SolveStatus status{SolveStatus::SolvedUnderConstrained};
  bool conflicting{false};
  bool fullyConstrained{false};
  double maxNormalizedResidual{};
  std::vector<ConstraintViolation> violations;
  std::vector<ConstraintId> conflictingConstraintIds;
  std::vector<ConstraintId> redundantConstraintIds;
  std::vector<ConstraintId> partiallyRedundantConstraintIds;
  std::vector<GeometryId> remainingDofGeometryIds;
  bool conflictSetComplete{true};
  std::vector<ConstraintComponentDiagnostic> components;
};

// Audit the complete current constraint system through the same immutable
// EquationSystem and forward-AD Jacobian consumed by the production solver.
[[nodiscard]] ConstraintDiagnostics analyzeConstraintSystem(
    const Sketch& sketch, bool computeDof = true);

[[nodiscard]] bool hasConstraintViolation(
    const ConstraintDiagnostics& diagnostics, ConstraintId id) noexcept;

}  // namespace solidar::sketch
