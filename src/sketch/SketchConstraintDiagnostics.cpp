#include "sketch/SketchConstraintDiagnostics.h"

#include "sketch/EquationKernel.h"
#include "sketch/NonlinearSketchSolver.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace solidar::sketch {
namespace {

void appendUnique(auto& target, const auto& source) {
  target.insert(target.end(), source.begin(), source.end());
  std::sort(target.begin(), target.end());
  target.erase(std::unique(target.begin(), target.end()), target.end());
}

bool failedStatus(SolveStatus status) {
  return status == SolveStatus::Conflicting ||
         status == SolveStatus::Malformed ||
         status == SolveStatus::Unsupported ||
         status == SolveStatus::NonConverged ||
         status == SolveStatus::BackendFailure;
}

void mergeSystem(const EquationSystem& system, const SolveReport& report,
                 bool computeDof,
                 ConstraintDiagnostics& result, bool appendComponent) {
  const auto summary = EquationDiagnostics::analyze(system, computeDof);
  if (summary.status != EquationStatus::Valid)
    result.conflicting = true;
  const std::size_t fixedParameterCount =
      system.parameterCount() - system.variableCount();
  // Preserve the public Solver-v1 diagnostic contract: fixed parameters are
  // part of the domain parameter count and contribute independent rank, while
  // the kernel itself exposes them as non-variable columns.
  result.variableCount += system.parameterCount();
  result.equationRank += report.equationRank + fixedParameterCount;
  result.degreesOfFreedom += report.degreesOfFreedom;
  result.userEquationCount += system.equationCount();
  result.maxNormalizedResidual =
      std::max(result.maxNormalizedResidual, summary.maxNormalizedResidual);

  std::unordered_map<ConstraintId, std::pair<ConstraintType, double>> worst;
  for (const auto& row : system.rows()) {
    const double normalized =
        std::abs(row.residual) / std::max(row.tolerance, 1e-15);
    if (row.status == EquationStatus::Valid && normalized <= 1.0) continue;
    auto& entry = worst[row.provenance.constraintId];
    entry.first = row.provenance.constraintType;
    entry.second = std::max(entry.second, normalized);
  }
  for (const auto& [id, entry] : worst) {
    result.violations.push_back({id, entry.first, entry.second});
    result.conflicting = true;
  }
  SolveStatus componentStatus = report.status;
  if (!worst.empty()) componentStatus = SolveStatus::Conflicting;
  result.conflicting = result.conflicting || failedStatus(componentStatus);
  if (failedStatus(componentStatus)) result.status = componentStatus;
  else if (!failedStatus(result.status)) {
    if (componentStatus == SolveStatus::PartiallyRedundant ||
        result.status == SolveStatus::PartiallyRedundant)
      result.status = SolveStatus::PartiallyRedundant;
    else if (componentStatus == SolveStatus::Redundant ||
             result.status == SolveStatus::Redundant)
      result.status = SolveStatus::Redundant;
    else if (report.degreesOfFreedom != 0 || result.degreesOfFreedom != 0)
      result.status = SolveStatus::SolvedUnderConstrained;
    else
      result.status = SolveStatus::SolvedFullyConstrained;
  }
  appendUnique(result.conflictingConstraintIds,
               report.conflictingConstraintIds);
  appendUnique(result.redundantConstraintIds, report.redundantConstraintIds);
  appendUnique(result.partiallyRedundantConstraintIds,
               report.partiallyRedundantConstraintIds);
  appendUnique(result.remainingDofGeometryIds,
               report.remainingDofGeometryIds);
  result.conflictSetComplete =
      result.conflictSetComplete && report.conflictSetComplete;
  if (appendComponent)
    result.components.push_back({system.component().geometryIds,
                                 system.component().constraintIds,
                                 system.parameterCount(),
                                 report.equationRank + fixedParameterCount,
                                 report.degreesOfFreedom,
                                 componentStatus,
                                 report.maxNormalizedResidual,
                                 report.iterations,
                                 report.conflictingConstraintIds,
                                 report.redundantConstraintIds,
                                 report.partiallyRedundantConstraintIds,
                                 report.remainingDofGeometryIds,
                                 report.conflictSetComplete,
                                 failedStatus(componentStatus)});
}

}  // namespace

ConstraintDiagnostics analyzeConstraintSystem(const Sketch& sketch,
                                              bool computeDof) {
  ConstraintDiagnostics result;
  const auto graph = ConstraintGraph::build(sketch);
  for (const auto& component : graph.components()) {
    const auto initialSystem =
        ConstraintEquationRegistry::assemble(sketch, component);
    SolveRequest request{sketch, component};
    request.localizeConflicts = computeDof;
    request.diagnoseRedundancy = computeDof;
    request.calculateRank = computeDof;
    request.maximumIterations = 256;
    const auto report = productionSketchSolver().solve(request);
    const auto solvedSystem = report.candidate.empty()
                                  ? std::optional<EquationSystem>{}
                                  : ConstraintEquationRegistry::assemble(
                                        sketch, component, report.candidate);
    mergeSystem(solvedSystem ? *solvedSystem : initialSystem, report,
                computeDof, result, true);
  }
  std::sort(result.violations.begin(), result.violations.end(),
            [](const auto& first, const auto& second) {
              return first.id < second.id;
            });
  result.fullyConstrained = !result.conflicting && result.variableCount > 0 &&
                            result.degreesOfFreedom == 0;
  if (result.components.empty())
    result.status = SolveStatus::SolvedUnderConstrained;
  else if (result.conflicting && !failedStatus(result.status))
    result.status = SolveStatus::Conflicting;
  return result;
}

bool hasConstraintViolation(const ConstraintDiagnostics& diagnostics,
                            ConstraintId id) noexcept {
  return std::any_of(diagnostics.violations.begin(),
                     diagnostics.violations.end(),
                     [id](const ConstraintViolation& item) {
                       return item.id == id;
                     });
}

}  // namespace solidar::sketch
