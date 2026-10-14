#include "sketch/NonlinearSketchSolver.h"

#include <MathLin_SVD.hxx>
#include <math_Matrix.hxx>
#include <math_SVD.hxx>
#include <math_Vector.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace solidar::sketch {
namespace {

struct LinearizedSystem {
  std::size_t rows{};
  std::size_t columns{};
  std::vector<double> residual;
  std::vector<double> jacobian;
  double maxNormalizedResidual{};
};

struct RegularizationTerm {
  std::size_t positive{};
  std::size_t negative{};
  double weight{1.0};
};

struct BackendSvdAnalysis {
  bool done{true};
  std::vector<double> singularValues;
  // Row-major square matrix whose columns are right singular vectors.
  std::vector<double> rightVectors;
};

[[nodiscard]] BackendSvdAnalysis backendSvd(
    std::span<const double> matrix, std::size_t rows, std::size_t columns) {
  BackendSvdAnalysis result;
  result.singularValues.assign(columns, 0.0);
  result.rightVectors.assign(columns * columns, 0.0);
  if (columns == 0) return result;
  if (rows == 0) {
    for (std::size_t index = 0; index < columns; ++index)
      result.rightVectors[index * columns + index] = 1.0;
    return result;
  }
  math_Matrix backendMatrix(1, static_cast<int>(rows), 1,
                            static_cast<int>(columns), 0.0);
  for (std::size_t row = 0; row < rows; ++row)
    for (std::size_t column = 0; column < columns; ++column)
      backendMatrix(static_cast<int>(row + 1),
                    static_cast<int>(column + 1)) =
          matrix[row * columns + column];
  const auto decomposition = MathLin::SVD(backendMatrix, 0.0);
  result.done = decomposition.IsDone() &&
                decomposition.SingularValues.has_value() &&
                decomposition.V.has_value();
  if (!result.done) return result;
  for (std::size_t column = 0; column < columns; ++column) {
    result.singularValues[column] =
        (*decomposition.SingularValues)(static_cast<int>(column + 1));
    for (std::size_t row = 0; row < columns; ++row)
      result.rightVectors[row * columns + column] =
          (*decomposition.V)(static_cast<int>(row + 1),
                             static_cast<int>(column + 1));
  }
  return result;
}

[[nodiscard]] double singularValueTolerance(
    const BackendSvdAnalysis& analysis, std::size_t rows,
    std::size_t columns) {
  const double maximum = analysis.singularValues.empty()
                             ? 0.0
                             : *std::max_element(
                                   analysis.singularValues.begin(),
                                   analysis.singularValues.end());
  return std::numeric_limits<double>::epsilon() *
         static_cast<double>(std::max(rows, columns)) * maximum * 64.0;
}

[[nodiscard]] LinearizedSystem linearize(const EquationSystem& system) {
  LinearizedSystem result;
  result.rows = system.rows().size();
  result.columns = system.variableCount();
  result.residual.reserve(result.rows);
  result.jacobian.assign(result.rows * result.columns, 0.0);
  for (const auto& row : system.rows()) {
    result.residual.push_back(row.normalizedResidual);
    result.maxNormalizedResidual = std::max(
        result.maxNormalizedResidual,
        std::abs(row.residual) / std::max(row.tolerance, 1e-15));
  }
  for (std::size_t domainColumn = 0;
       domainColumn < system.layout().parameters().size(); ++domainColumn) {
    const auto variable =
        system.layout().parameters()[domainColumn].variableIndex;
    if (!variable) continue;
    for (std::size_t row = 0; row < result.rows; ++row)
      result.jacobian[row * result.columns + *variable] =
          system.jacobian()[row * system.parameterCount() + domainColumn];
  }
  return result;
}

[[nodiscard]] std::optional<std::size_t> backendRank(
    std::span<const double> matrix, std::size_t rows, std::size_t columns) {
  if (rows == 0 || columns == 0) return std::size_t{0};
  const auto analysis = backendSvd(matrix, rows, columns);
  if (!analysis.done) return std::nullopt;
  const double tolerance = singularValueTolerance(analysis, rows, columns);
  std::size_t rank = 0;
  for (const double singularValue : analysis.singularValues)
    if (singularValue > tolerance) ++rank;
  return rank;
}

[[nodiscard]] bool dampedStep(const LinearizedSystem& system, double damping,
                              std::span<const double> penalties,
                              std::span<const RegularizationTerm> terms,
                              std::vector<double>& step) {
  if (system.columns == 0) {
    step.clear();
    return true;
  }
  const std::size_t augmentedRows =
      system.rows + system.columns + terms.size();
  math_Matrix matrix(1, static_cast<int>(augmentedRows), 1,
                     static_cast<int>(system.columns), 0.0);
  math_Vector rhs(1, static_cast<int>(augmentedRows), 0.0);
  for (std::size_t row = 0; row < system.rows; ++row) {
    rhs(static_cast<int>(row + 1)) = -system.residual[row];
    for (std::size_t column = 0; column < system.columns; ++column)
      matrix(static_cast<int>(row + 1), static_cast<int>(column + 1)) =
          system.jacobian[row * system.columns + column];
  }
  const double regularization = std::sqrt(std::max(damping, 1e-16));
  for (std::size_t column = 0; column < system.columns; ++column)
    matrix(static_cast<int>(system.rows + column + 1),
           static_cast<int>(column + 1)) =
        regularization * (column < penalties.size() ? penalties[column] : 1.0);
  for (std::size_t index = 0; index < terms.size(); ++index) {
    const auto& term = terms[index];
    const int row = static_cast<int>(system.rows + system.columns + index + 1);
    matrix(row, static_cast<int>(term.positive + 1)) =
        regularization * term.weight;
    matrix(row, static_cast<int>(term.negative + 1)) =
        -regularization * term.weight;
  }
  math_SVD decomposition(matrix);
  if (!decomposition.IsDone()) return false;
  math_Vector solution(1, static_cast<int>(system.columns), 0.0);
  decomposition.Solve(rhs, solution, 1e-12);
  step.resize(system.columns);
  for (std::size_t column = 0; column < system.columns; ++column) {
    step[column] = solution(static_cast<int>(column + 1));
    if (!std::isfinite(step[column])) return false;
  }
  return true;
}

[[nodiscard]] std::vector<RegularizationTerm> semanticRegularization(
    const Sketch& sketch, const EquationSystem& system,
    ConstraintId preferredConstraintId) {
  std::vector<RegularizationTerm> terms;
  if (preferredConstraintId == kInvalidConstraintId) return terms;
  const auto addDifference = [&](GeometryId id, ParameterRole first,
                                 ParameterRole second) {
    const auto firstDomain = system.layout().parameterIndex(id, first);
    const auto secondDomain = system.layout().parameterIndex(id, second);
    if (!firstDomain || !secondDomain) return;
    const auto firstVariable =
        system.layout().parameters()[*firstDomain].variableIndex;
    const auto secondVariable =
        system.layout().parameters()[*secondDomain].variableIndex;
    if (firstVariable && secondVariable) {
      const double weight =
          system.component().constraintIds.size() >= 16 ? 1e7 : 1e9;
      terms.push_back({*firstVariable, *secondVariable, weight});
    }
  };
  for (const auto id : system.component().geometryIds) {
    const auto lineIndex = sketch.lineIndex(id);
    if (!lineIndex) continue;
    const auto elementId = sketch.lines()[*lineIndex].elementId;
    const auto memberCount = static_cast<std::size_t>(std::count_if(
        sketch.lines().begin(), sketch.lines().end(),
        [elementId](const Line& line) { return line.elementId == elementId; }));
    if (elementId == 0 || memberCount < 2) continue;
    addDifference(id, ParameterRole::LineEndX, ParameterRole::LineStartX);
    addDifference(id, ParameterRole::LineEndY, ParameterRole::LineStartY);
  }
  if (const auto preferred = sketch.constraintIndex(preferredConstraintId)) {
    const auto payload = sketch.constraints()[*preferred].payload();
    if (const auto* distance = std::get_if<LineDistanceConstraint>(&payload)) {
      addDifference(distance->secondLineId, ParameterRole::LineEndX,
                    ParameterRole::LineStartX);
      addDifference(distance->secondLineId, ParameterRole::LineEndY,
                    ParameterRole::LineStartY);
    }
  }
  return terms;
}

[[nodiscard]] std::vector<double> semanticPenalties(
    const Sketch& sketch, const EquationSystem& system,
    ConstraintId preferredConstraintId) {
  std::vector<double> penalties(system.variableCount(), 1.0);
  if (preferredConstraintId == kInvalidConstraintId) {
    for (const auto& descriptor : system.layout().parameters()) {
      if (!descriptor.variableIndex) continue;
      switch (descriptor.provenance.role) {
        case ParameterRole::CircleRadius:
        case ParameterRole::ArcRadius:
        case ParameterRole::ArcStartAngle:
        case ParameterRole::ArcSweepAngle:
          penalties[*descriptor.variableIndex] = 1e4;
          break;
        default:
          break;
      }
    }
    return penalties;
  }
  const auto preserveGeometry = [&](GeometryId id) {
    for (const auto& descriptor : system.layout().parameters())
      if (descriptor.variableIndex && descriptor.provenance.geometryId == id)
        penalties[*descriptor.variableIndex] =
            std::max(penalties[*descriptor.variableIndex], 1e4);
  };
  const auto preserveParameter = [&](GeometryId id, ParameterRole role) {
    const auto domain = system.layout().parameterIndex(id, role);
    if (!domain) return;
    const auto variable = system.layout().parameters()[*domain].variableIndex;
    if (variable) penalties[*variable] = std::max(penalties[*variable], 1e4);
  };
  const auto preservePoint = [&](const PointReference& reference) {
    std::visit(
        [&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, LineEndpointReference>) {
            preserveParameter(
                value.lineId, value.endpoint == Endpoint::Start
                                  ? ParameterRole::LineStartX
                                  : ParameterRole::LineEndX);
            preserveParameter(
                value.lineId, value.endpoint == Endpoint::Start
                                  ? ParameterRole::LineStartY
                                  : ParameterRole::LineEndY);
          } else if constexpr (std::is_same_v<T, CircleCenterReference>) {
            preserveParameter(value.circleId, ParameterRole::CircleCenterX);
            preserveParameter(value.circleId, ParameterRole::CircleCenterY);
          }
          else if constexpr (std::is_same_v<T, ArcEndpointReference>)
            preserveGeometry(value.arcId);
          else if constexpr (std::is_same_v<T, BezierPointReference>) {
            const auto offset = static_cast<int>(value.pointIndex) * 2;
            preserveParameter(value.bezierId,
                static_cast<ParameterRole>(
                    static_cast<int>(ParameterRole::BezierPoint0X) + offset));
            preserveParameter(value.bezierId,
                static_cast<ParameterRole>(
                    static_cast<int>(ParameterRole::BezierPoint0Y) + offset));
          }
          else if constexpr (std::is_same_v<T, CompositeCenterReference>)
            for (std::size_t index = 0; index < sketch.lines().size(); ++index)
              if (sketch.lines()[index].elementId == value.elementId)
                preserveGeometry(sketch.lineId(index));
        },
        reference.payload());
  };
  for (const auto id : system.component().constraintIds) {
    const auto index = sketch.constraintIndex(id);
    if (!index) continue;
    const auto& constraint = sketch.constraints()[*index];
    if (constraint.mode != ConstraintMode::Driving) continue;
    std::visit(
        [&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, HorizontalConstraint> ||
                        std::is_same_v<T, VerticalConstraint> ||
                        std::is_same_v<T, LengthConstraint>) {
            const auto line = std::is_same_v<T, HorizontalConstraint>
                                  ? value.lineId
                                  : value.lineId;
            const auto startX = system.layout().parameterIndex(
                line, ParameterRole::LineStartX);
            const auto startY = system.layout().parameterIndex(
                line, ParameterRole::LineStartY);
            for (const auto domain : {startX, startY})
              if (domain) {
                const auto variable =
                    system.layout().parameters()[*domain].variableIndex;
                if (variable) penalties[*variable] = 1e4;
              }
          } else if constexpr (std::is_same_v<T, RadiusConstraint> ||
                               std::is_same_v<T, DiameterConstraint>) {
            const auto centerX = system.layout().parameterIndex(
                value.circleId, ParameterRole::CircleCenterX);
            const auto centerY = system.layout().parameterIndex(
                value.circleId, ParameterRole::CircleCenterY);
            for (const auto domain : {centerX, centerY})
              if (domain) {
                const auto variable =
                    system.layout().parameters()[*domain].variableIndex;
                if (variable) penalties[*variable] = 1e4;
              }
          } else if constexpr (std::is_same_v<T, CoincidentConstraint> ||
                               std::is_same_v<T, DistanceConstraint> ||
                               std::is_same_v<T, DistanceXConstraint> ||
                               std::is_same_v<T, DistanceYConstraint>) {
            preservePoint(value.first);
          } else if constexpr (std::is_same_v<T, PointOnLineConstraint> ||
                               std::is_same_v<T, MidpointConstraint>) {
            preserveGeometry(value.lineId);
          } else if constexpr (std::is_same_v<T, PointOnCircleConstraint>) {
            preserveGeometry(value.circleId);
          } else if constexpr (std::is_same_v<T, PointOnArcConstraint>) {
            preserveGeometry(value.arcId);
          } else if constexpr (std::is_same_v<T, ParallelConstraint> ||
                               std::is_same_v<T, PerpendicularConstraint> ||
                               std::is_same_v<T, AngleConstraint> ||
                               std::is_same_v<T, LineDistanceConstraint>) {
            preserveGeometry(value.firstLineId);
          } else if constexpr (std::is_same_v<T, EqualConstraint> ||
                               std::is_same_v<T, TangentConstraint>) {
            preserveGeometry(value.firstGeometryId);
          }
        },
        constraint.payload());
  }
  const bool hasMovableStandalone = std::any_of(
      system.layout().parameters().begin(), system.layout().parameters().end(),
      [&](const ParameterDescriptor& descriptor) {
        if (!descriptor.variableIndex) return false;
        const auto lineIndex = sketch.lineIndex(descriptor.provenance.geometryId);
        if (!lineIndex) return true;
        const auto elementId = sketch.lines()[*lineIndex].elementId;
        const auto memberCount = static_cast<std::size_t>(std::count_if(
            sketch.lines().begin(), sketch.lines().end(),
            [elementId](const Line& line) {
              return line.elementId == elementId;
            }));
        return elementId == 0 || memberCount < 2;
      });
  if (hasMovableStandalone) {
    for (const auto& descriptor : system.layout().parameters()) {
      if (!descriptor.variableIndex) continue;
      const auto lineIndex = sketch.lineIndex(descriptor.provenance.geometryId);
      if (!lineIndex) continue;
      const auto elementId = sketch.lines()[*lineIndex].elementId;
      const auto memberCount = static_cast<std::size_t>(std::count_if(
          sketch.lines().begin(), sketch.lines().end(),
          [elementId](const Line& line) {
            return line.elementId == elementId;
          }));
      if (elementId != 0 && memberCount >= 2)
        penalties[*descriptor.variableIndex] =
            std::max(penalties[*descriptor.variableIndex], 1e8);
    }
  }
  for (const auto& descriptor : system.layout().parameters()) {
    if (!descriptor.variableIndex) continue;
    switch (descriptor.provenance.role) {
      case ParameterRole::CircleRadius:
      case ParameterRole::ArcRadius:
      case ParameterRole::ArcStartAngle:
      case ParameterRole::ArcSweepAngle:
        penalties[*descriptor.variableIndex] =
            std::max(penalties[*descriptor.variableIndex], 1e4);
        break;
      default:
        break;
    }
  }
  return penalties;
}

[[nodiscard]] double squaredNorm(std::span<const double> values) {
  double result = 0.0;
  for (const double value : values) result += value * value;
  return result;
}

[[nodiscard]] EquationStatus worse(EquationStatus first,
                                   EquationStatus second) {
  return static_cast<int>(first) >= static_cast<int>(second) ? first : second;
}

[[nodiscard]] SolveStatus typedFailure(EquationStatus status) {
  switch (status) {
    case EquationStatus::MalformedReference:
    case EquationStatus::DegenerateGeometry:
    case EquationStatus::NonFiniteInput:
      return SolveStatus::Malformed;
    case EquationStatus::UnsupportedCombination:
      return SolveStatus::Unsupported;
    case EquationStatus::Valid:
      return SolveStatus::NonConverged;
  }
  return SolveStatus::BackendFailure;
}

struct CoreResult {
  SolveStatus status{SolveStatus::BackendFailure};
  std::vector<double> candidate;
  std::size_t iterations{};
  std::size_t rank{};
  std::size_t dof{};
  bool rankComputed{true};
  std::vector<GeometryId> remainingDofGeometryIds;
  double maxResidual{};
  EquationSystem finalSystem;
};

[[nodiscard]] CoreResult solveCore(const SolveRequest& request,
                                   std::span<const ConstraintId> ignored) {
  CoreResult result;
  const auto initial = ConstraintEquationRegistry::assemble(
      request.sketch, request.component, ignored);
  if (initial.status() != EquationStatus::Valid) {
    result.status = typedFailure(initial.status());
    result.finalSystem = initial;
    return result;
  }
  result.candidate.assign(initial.variableCount(), 0.0);
  for (const auto& descriptor : initial.layout().parameters())
    if (descriptor.variableIndex)
      result.candidate[*descriptor.variableIndex] = descriptor.value;
  if (!request.previousCandidate.empty()) {
    if (request.previousCandidate.size() != result.candidate.size()) {
      result.status = SolveStatus::Malformed;
      result.finalSystem = initial;
      return result;
    }
    result.candidate = request.previousCandidate;
  }

  double damping = 1e-6;
  auto current = ConstraintEquationRegistry::assemble(
      request.sketch, request.component, result.candidate, ignored);
  if (!current) {
    result.status = SolveStatus::Malformed;
    result.finalSystem = initial;
    return result;
  }
  const auto penalties = semanticPenalties(
      request.sketch, *current, request.preferredConstraintId);
  const auto regularization = semanticRegularization(
      request.sketch, *current, request.preferredConstraintId);
  const std::vector<double> neutralPenalties(initial.variableCount(), 1.0);
  for (std::size_t iteration = 0; iteration < request.maximumIterations;
       ++iteration) {
    ++result.iterations;
    if (current->status() != EquationStatus::Valid) {
      result.status = typedFailure(current->status());
      result.finalSystem = std::move(*current);
      return result;
    }
    const auto linear = linearize(*current);
    result.maxResidual = linear.maxNormalizedResidual;
    // Converge well inside the public equation tolerance.  Stopping near the
    // acceptance boundary is visibly unstable at branch points (for example
    // a perpendicular line meeting the bottom of a circle, where the free x
    // coordinate is proportional to the square root of the radial residual).
    if (linear.maxNormalizedResidual <= 1e-4) break;
    std::vector<double> step;
    const bool guideStep = iteration < 64;
    if (!dampedStep(linear, damping,
                    guideStep ? std::span<const double>(penalties)
                              : std::span<const double>(neutralPenalties),
                    guideStep
                        ? std::span<const RegularizationTerm>(regularization)
                        : std::span<const RegularizationTerm>{},
                    step)) {
      result.status = SolveStatus::BackendFailure;
      result.finalSystem = std::move(*current);
      return result;
    }
    const double oldCost = squaredNorm(linear.residual);
    bool accepted = false;
    double alpha = 1.0;
    for (int lineSearch = 0; lineSearch < 12; ++lineSearch) {
      auto trialCandidate = result.candidate;
      for (std::size_t column = 0; column < step.size(); ++column)
        trialCandidate[column] += alpha * step[column];
      auto trial = ConstraintEquationRegistry::assemble(
          request.sketch, request.component, trialCandidate, ignored);
      if (trial && trial->status() == EquationStatus::Valid) {
        const auto trialLinear = linearize(*trial);
        const bool lowersLeastSquares =
            squaredNorm(trialLinear.residual) < oldCost;
        const bool lowersWorstEquation =
            trialLinear.maxNormalizedResidual <
            linear.maxNormalizedResidual * (1.0 - 1e-12);
        if (lowersLeastSquares || lowersWorstEquation) {
          result.candidate = std::move(trialCandidate);
          current = std::move(trial);
          damping = std::max(damping * 0.25, 1e-30);
          accepted = true;
          break;
        }
      }
      alpha *= 0.5;
    }
    if (!accepted) {
      damping *= 16.0;
      if (damping > 1e12) break;
    }
  }

  result.finalSystem = std::move(*current);
  const auto finalLinear = linearize(result.finalSystem);
  result.maxResidual = finalLinear.maxNormalizedResidual;
  if (request.calculateRank) {
    const auto decomposition = backendSvd(
        finalLinear.jacobian, finalLinear.rows, finalLinear.columns);
    if (!decomposition.done) {
      result.status = SolveStatus::BackendFailure;
      return result;
    }
    const double tolerance = singularValueTolerance(
        decomposition, finalLinear.rows, finalLinear.columns);
    for (const double singularValue : decomposition.singularValues)
      if (singularValue > tolerance) ++result.rank;
    result.dof = finalLinear.columns > result.rank
                     ? finalLinear.columns - result.rank
                     : 0;
    if (result.dof != 0) {
      for (const auto& descriptor : result.finalSystem.layout().parameters()) {
        if (!descriptor.variableIndex) continue;
        bool participates = false;
        for (std::size_t singular = 0;
             singular < decomposition.singularValues.size(); ++singular) {
          if (decomposition.singularValues[singular] > tolerance) continue;
          const double coefficient = decomposition.rightVectors[
              *descriptor.variableIndex * finalLinear.columns + singular];
          if (std::abs(coefficient) > 1e-8) {
            participates = true;
            break;
          }
        }
        if (participates)
          result.remainingDofGeometryIds.push_back(
              descriptor.provenance.geometryId);
      }
      std::sort(result.remainingDofGeometryIds.begin(),
                result.remainingDofGeometryIds.end());
      result.remainingDofGeometryIds.erase(
          std::unique(result.remainingDofGeometryIds.begin(),
                      result.remainingDofGeometryIds.end()),
          result.remainingDofGeometryIds.end());
    }
  } else {
    // Lightweight interactive solve deliberately defers the dense rank pass
    // to debounced/on-release diagnostics. Conservatively report every
    // variable as potentially free; nullspace hint projection still computes
    // its local basis only when an actual hint needs it.
    result.rank = 0;
    result.dof = finalLinear.columns;
    result.rankComputed = false;
  }
  if (result.maxResidual > 1.0) {
    result.status = SolveStatus::NonConverged;
  } else {
    result.status = result.dof == 0 ? SolveStatus::SolvedFullyConstrained
                                    : SolveStatus::SolvedUnderConstrained;
  }
  return result;
}

[[nodiscard]] std::vector<double> selectRows(
    const LinearizedSystem& system, const EquationSystem& equations,
    ConstraintId excluded) {
  std::vector<double> selected;
  selected.reserve(system.jacobian.size());
  for (std::size_t row = 0; row < system.rows; ++row) {
    if (equations.rows()[row].provenance.constraintId == excluded) continue;
    selected.insert(selected.end(),
                    system.jacobian.begin() + row * system.columns,
                    system.jacobian.begin() + (row + 1) * system.columns);
  }
  return selected;
}

[[nodiscard]] bool diagnoseRedundancy(SolveReport& report,
                                      const EquationSystem& system) {
  const auto linear = linearize(system);
  std::map<ConstraintId, std::size_t> rowCounts;
  for (const auto& row : system.rows()) ++rowCounts[row.provenance.constraintId];
  for (const auto& [id, rowCount] : rowCounts) {
    const auto reduced = selectRows(linear, system, id);
    const auto reducedRows = linear.rows - rowCount;
    const auto reducedRank = backendRank(reduced, reducedRows, linear.columns);
    if (!reducedRank) return false;
    const auto contribution = report.equationRank > *reducedRank
                                  ? report.equationRank - *reducedRank
                                  : 0;
    if (contribution == 0)
      report.redundantConstraintIds.push_back(id);
    else if (contribution < rowCount)
      report.partiallyRedundantConstraintIds.push_back(id);
  }
  return true;
}

void applyNullspaceHints(const SolveRequest& request, CoreResult& core) {
  if (core.dof == 0 || core.candidate.empty())
    return;
  for (int hintPass = 0; hintPass < 12; ++hintPass) {
    const auto linear = linearize(core.finalSystem);
    if (linear.columns == 0) return;
    std::vector<double> desired(linear.columns, 0.0);
    std::vector<bool> hinted(linear.columns, false);
    std::vector<double> hintWeights(linear.columns, 1.0);
    // The previous valid pose is the canonical preference for every free
    // parameter.  Starting LM from that pose is not sufficient around rank
    // deficient nonlinear branches: numerical drift can remain in the final
    // nullspace.  Projecting the original pose back into that nullspace makes
    // the selected solution independent from equation/constraint order.
    const auto preserveWeights = semanticPenalties(
        request.sketch, core.finalSystem, request.preferredConstraintId);
    if (request.transientHints.empty())
      for (const auto& descriptor : core.finalSystem.layout().parameters()) {
        if (!descriptor.variableIndex) continue;
        const auto variable = *descriptor.variableIndex;
        const double target = request.previousCandidate.size() == linear.columns
                                  ? request.previousCandidate[variable]
                                  : descriptor.value;
        const double delta = target - core.candidate[variable];
        const bool canonicalFreePose =
            request.preferredConstraintId == kInvalidConstraintId;
        const bool numericalDriftInPreservedParameter =
            preserveWeights[variable] > 1.0 && std::abs(delta) <= 1e-3;
        if (!canonicalFreePose && !numericalDriftInPreservedParameter)
          continue;
        desired[variable] = delta;
        hinted[variable] = true;
      }
    for (const auto& hint : request.transientHints) {
      const auto domain = core.finalSystem.layout().parameterIndex(
          hint.parameter.geometryId, hint.parameter.role);
      if (!domain) continue;
      const auto variable =
          core.finalSystem.layout().parameters()[*domain].variableIndex;
      if (!variable || !std::isfinite(hint.target)) continue;
      desired[*variable] = hint.target - core.candidate[*variable];
      hinted[*variable] = true;
      switch (hint.parameter.role) {
        case ParameterRole::CircleRadius:
        case ParameterRole::ArcRadius:
        case ParameterRole::ArcStartAngle:
        case ParameterRole::ArcSweepAngle:
          hintWeights[*variable] = 1e4;
          break;
        default:
          break;
      }
    }
    if (std::none_of(hinted.begin(), hinted.end(),
                     [](bool value) { return value; }))
      return;
    double desiredNorm = squaredNorm(desired);
    if (desiredNorm <= 1e-20) return;

    // A common interactive hint is already tangent to the hard-constraint
    // manifold (for example translating both endpoints of a horizontal
    // segment). Accept that direction directly and avoid a dense nullspace
    // decomposition whose cost scales with the entire connected component.
    double hardDisplacementNorm = 0.0;
    for (std::size_t row = 0; row < linear.rows; ++row) {
      double value = 0.0;
      for (std::size_t column = 0; column < linear.columns; ++column)
        value += linear.jacobian[row * linear.columns + column] *
                 desired[column];
      hardDisplacementNorm += value * value;
    }
    std::vector<double> projected;
    if (hardDisplacementNorm <= 1e-20 * std::max(1.0, desiredNorm)) {
      projected = desired;
    } else {
      const auto decomposition = backendSvd(
          linear.jacobian, linear.rows, linear.columns);
      if (!decomposition.done) return;
      const double nullTolerance = singularValueTolerance(
          decomposition, linear.rows, linear.columns);
      std::vector<std::vector<double>> basis;
      for (std::size_t index = 0; index < linear.columns; ++index) {
        if (decomposition.singularValues[index] > nullTolerance)
          continue;
        auto& vector = basis.emplace_back(linear.columns, 0.0);
        for (std::size_t column = 0; column < linear.columns; ++column)
          vector[column] = decomposition.rightVectors[
              column * linear.columns + index];
      }
      if (basis.empty()) return;
      std::vector<std::size_t> hintedColumns;
      for (std::size_t column = 0; column < hinted.size(); ++column)
        if (hinted[column]) hintedColumns.push_back(column);
      math_Matrix hintMatrix(1, static_cast<int>(hintedColumns.size()), 1,
                             static_cast<int>(basis.size()), 0.0);
      math_Vector hintTarget(1, static_cast<int>(hintedColumns.size()), 0.0);
      for (std::size_t row = 0; row < hintedColumns.size(); ++row) {
        const double weight = hintWeights[hintedColumns[row]];
        hintTarget(static_cast<int>(row + 1)) =
            desired[hintedColumns[row]] * weight;
        for (std::size_t column = 0; column < basis.size(); ++column)
          hintMatrix(static_cast<int>(row + 1),
                     static_cast<int>(column + 1)) =
              basis[column][hintedColumns[row]] * weight;
      }
      math_SVD hintSolve(hintMatrix);
      if (!hintSolve.IsDone()) return;
      math_Vector coefficients(1, static_cast<int>(basis.size()), 0.0);
      hintSolve.Solve(hintTarget, coefficients, 1e-10);
      projected.assign(linear.columns, 0.0);
      for (std::size_t basisIndex = 0; basisIndex < basis.size(); ++basisIndex)
        for (std::size_t column = 0; column < linear.columns; ++column)
          projected[column] +=
              basis[basisIndex][column] *
              coefficients(static_cast<int>(basisIndex + 1));
    }
    if (squaredNorm(projected) <= 1e-24) return;
    const auto candidateBeforeProjection = core.candidate;
    for (std::size_t column = 0; column < linear.columns; ++column)
      core.candidate[column] += projected[column];

    SolveRequest correction{request.sketch, request.component};
    correction.previousCandidate = core.candidate;
    correction.ignoredConstraintIds = request.ignoredConstraintIds;
    correction.maximumIterations = request.maximumIterations;
    correction.localizeConflicts = false;
    correction.diagnoseRedundancy = false;
    correction.calculateRank = request.calculateRank;
    auto corrected = solveCore(correction, correction.ignoredConstraintIds);
    if (corrected.status != SolveStatus::SolvedFullyConstrained &&
        corrected.status != SolveStatus::SolvedUnderConstrained) {
      // A transient preference is never allowed to invalidate an otherwise
      // accepted candidate. Keep the status, residual and final system paired
      // with the exact candidate from which they were computed.
      core.candidate = candidateBeforeProjection;
      return;
    }
    core = std::move(corrected);
  }
}

}  // namespace

bool SolveReport::solved() const noexcept {
  return status == SolveStatus::SolvedFullyConstrained ||
         status == SolveStatus::SolvedUnderConstrained ||
         status == SolveStatus::Redundant ||
         status == SolveStatus::PartiallyRedundant;
}

SolveReport OcctSketchSolver::solve(const SolveRequest& request) const {
  SolveReport report;
  report.component = request.component;
  try {
    auto core = solveCore(request, request.ignoredConstraintIds);
    if (core.status == SolveStatus::SolvedFullyConstrained ||
        core.status == SolveStatus::SolvedUnderConstrained) {
      applyNullspaceHints(request, core);
      report.usedTransientHints = !request.transientHints.empty();
    }
    report.status = core.status;
    report.candidate = core.candidate;
    report.iterations = core.iterations;
    report.equationRank = core.rank;
    report.degreesOfFreedom = core.dof;
    report.rankComputed = core.rankComputed;
    report.remainingDofGeometryIds = core.remainingDofGeometryIds;
    report.maxNormalizedResidual = core.maxResidual;
    report.equationProvenance.reserve(core.finalSystem.rows().size());
    for (const auto& row : core.finalSystem.rows())
      report.equationProvenance.push_back(row.provenance);

    if (report.solved() && request.diagnoseRedundancy) {
      if (!diagnoseRedundancy(report, core.finalSystem)) {
        report.status = SolveStatus::BackendFailure;
      } else if (!report.partiallyRedundantConstraintIds.empty())
        report.status = SolveStatus::PartiallyRedundant;
      else if (!report.redundantConstraintIds.empty())
        report.status = SolveStatus::Redundant;
    }
    if (report.status == SolveStatus::NonConverged &&
        request.localizeConflicts) {
      if (request.component.constraintIds.size() > 64) {
        // Never present arbitrary IDs as a proven conflict. Large-component
        // localization is deliberately bounded and explicitly incomplete.
        report.conflictSetComplete = false;
      } else {
        const auto solvedStatus = [](SolveStatus status) {
          return status == SolveStatus::SolvedFullyConstrained ||
                 status == SolveStatus::SolvedUnderConstrained;
        };
        const auto inconsistent = [](const CoreResult& candidate) {
          return candidate.status == SolveStatus::NonConverged &&
                 candidate.maxResidual > 1.0;
        };
        const auto probe = [&](std::vector<ConstraintId> ignored) {
          std::sort(ignored.begin(), ignored.end());
          ignored.erase(std::unique(ignored.begin(), ignored.end()),
                        ignored.end());
          SolveRequest diagnostic{request.sketch, request.component};
          const auto probeLayout = ParameterLayout::build(
              request.sketch, request.component.geometryIds, ignored);
          if (request.previousCandidate.size() == probeLayout.variableCount())
            diagnostic.previousCandidate = request.previousCandidate;
          diagnostic.ignoredConstraintIds = ignored;
          diagnostic.preferredConstraintId = request.preferredConstraintId;
          diagnostic.maximumIterations =
              std::max<std::size_t>(256, request.maximumIterations);
          diagnostic.localizeConflicts = false;
          diagnostic.diagnoseRedundancy = false;
          diagnostic.calculateRank = false;
          return solveCore(diagnostic, ignored);
        };

        // First distinguish a low iteration budget from a genuine conflict.
        const auto robustFull = probe(request.ignoredConstraintIds);
        if (inconsistent(robustFull)) {
          std::vector<ConstraintId> active;
          for (const auto id : request.component.constraintIds)
            if (std::find(request.ignoredConstraintIds.begin(),
                          request.ignoredConstraintIds.end(), id) ==
                request.ignoredConstraintIds.end())
              active.push_back(id);
          std::vector<ConstraintId> removed = request.ignoredConstraintIds;
          bool reliable = true;
          for (const auto id : std::vector<ConstraintId>(active)) {
            auto trialIgnored = removed;
            trialIgnored.push_back(id);
            const auto trial = probe(std::move(trialIgnored));
            if (inconsistent(trial)) {
              removed.push_back(id);
              active.erase(std::remove(active.begin(), active.end(), id),
                           active.end());
            } else if (!solvedStatus(trial.status)) {
              reliable = false;
              break;
            }
          }

          // Prove local minimality: the retained subset is inconsistent and
          // removing any one of its members makes it solvable.
          if (reliable && !active.empty() &&
              inconsistent(probe(removed))) {
            for (const auto id : active) {
              auto trialIgnored = removed;
              trialIgnored.push_back(id);
              if (!solvedStatus(probe(std::move(trialIgnored)).status)) {
                reliable = false;
                break;
              }
            }
          } else {
            reliable = false;
          }
          if (reliable) {
            std::sort(active.begin(), active.end());
            report.conflictingConstraintIds = std::move(active);
            report.status = SolveStatus::Conflicting;
          }
        }
      }
    }
  } catch (...) {
    report.status = SolveStatus::BackendFailure;
  }
  return report;
}

const ISketchSolver& productionSketchSolver() noexcept {
  static const OcctSketchSolver solver;
  return solver;
}

}  // namespace solidar::sketch
