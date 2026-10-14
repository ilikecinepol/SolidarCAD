#pragma once

#include "sketch/EquationKernel.h"

#include <cstddef>
#include <span>
#include <vector>

namespace solidar::sketch {

struct ParameterHint {
  ParameterProvenance parameter;
  double target{};
};

struct SolveRequest {
  const Sketch& sketch;
  ConstraintGraphComponent component;
  std::vector<double> previousCandidate;
  std::vector<ParameterHint> transientHints;
  std::vector<ConstraintId> ignoredConstraintIds;
  ConstraintId preferredConstraintId{kInvalidConstraintId};
  std::size_t maximumIterations{48};
  bool localizeConflicts{true};
  bool diagnoseRedundancy{true};
  bool calculateRank{true};
};

struct SolveReport {
  SolveStatus status{SolveStatus::BackendFailure};
  ConstraintGraphComponent component;
  std::vector<double> candidate;
  std::vector<EquationProvenance> equationProvenance;
  std::vector<ConstraintId> conflictingConstraintIds;
  std::vector<ConstraintId> redundantConstraintIds;
  std::vector<ConstraintId> partiallyRedundantConstraintIds;
  std::vector<GeometryId> remainingDofGeometryIds;
  std::size_t iterations{};
  std::size_t equationRank{};
  std::size_t degreesOfFreedom{};
  bool rankComputed{true};
  double maxNormalizedResidual{};
  bool conflictSetComplete{true};
  bool usedTransientHints{false};

  [[nodiscard]] bool solved() const noexcept;
};

class ISketchSolver {
 public:
  virtual ~ISketchSolver() = default;
  [[nodiscard]] virtual SolveReport solve(const SolveRequest& request) const = 0;
};

// Thin adapter over OCCT TKMath. The nonlinear policy lives here, while all
// residuals and derivatives remain authoritative in EquationKernel.
class OcctSketchSolver final : public ISketchSolver {
 public:
  [[nodiscard]] SolveReport solve(const SolveRequest& request) const override;
};

[[nodiscard]] const ISketchSolver& productionSketchSolver() noexcept;

}  // namespace solidar::sketch
