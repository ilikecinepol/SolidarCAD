#pragma once

#include "sketch/Sketch.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace solidar::sketch {

enum class EquationStatus {
  Valid,
  MalformedReference,
  UnsupportedCombination,
  DegenerateGeometry,
  NonFiniteInput,
};

enum class EquationUnit { Length, Angle, Dimensionless };

enum class ParameterRole : std::uint8_t {
  LineStartX,
  LineStartY,
  LineEndX,
  LineEndY,
  CircleCenterX,
  CircleCenterY,
  CircleRadius,
  ArcCenterX,
  ArcCenterY,
  ArcRadius,
  ArcStartAngle,
  ArcSweepAngle,
  BezierPoint0X,
  BezierPoint0Y,
  BezierPoint1X,
  BezierPoint1Y,
  BezierPoint2X,
  BezierPoint2Y,
  BezierPoint3X,
  BezierPoint3Y,
};

struct ParameterProvenance {
  GeometryId geometryId{kInvalidGeometryId};
  ParameterRole role{ParameterRole::LineStartX};
  bool operator==(const ParameterProvenance&) const = default;
};

struct ParameterDescriptor {
  ParameterProvenance provenance;
  double value{};
  bool fixed{false};
  std::optional<std::size_t> variableIndex;
};

struct EquationProvenance {
  ConstraintId constraintId{kInvalidConstraintId};
  ConstraintType constraintType{ConstraintType::Horizontal};
  std::uint16_t scalarIndex{};
  bool operator==(const EquationProvenance&) const = default;
};

struct EquationRow {
  double residual{};
  double normalizedResidual{};
  double scale{1.0};
  double tolerance{1e-6};
  EquationUnit unit{EquationUnit::Length};
  EquationStatus status{EquationStatus::Valid};
  EquationProvenance provenance;
};

struct ConstraintGraphComponent {
  std::vector<GeometryId> geometryIds;
  std::vector<ConstraintId> constraintIds;
  bool operator==(const ConstraintGraphComponent&) const = default;
};

class ConstraintGraph final {
 public:
  [[nodiscard]] static ConstraintGraph build(const Sketch& sketch);
  [[nodiscard]] const std::vector<ConstraintGraphComponent>& components()
      const noexcept {
    return components_;
  }
  [[nodiscard]] const ConstraintGraphComponent* componentContaining(
      GeometryId id) const noexcept;

 private:
  std::vector<ConstraintGraphComponent> components_;
};

class ParameterLayout final {
 public:
  [[nodiscard]] static ParameterLayout build(
      const Sketch& sketch, std::span<const GeometryId> geometryIds,
      std::span<const ConstraintId> ignoredConstraintIds = {});

  [[nodiscard]] const std::vector<ParameterDescriptor>& parameters()
      const noexcept {
    return parameters_;
  }
  [[nodiscard]] const std::vector<double>& values() const noexcept {
    return values_;
  }
  [[nodiscard]] std::size_t variableCount() const noexcept {
    return variableCount_;
  }
  [[nodiscard]] double sketchScale() const noexcept { return sketchScale_; }
  [[nodiscard]] std::optional<std::size_t> parameterIndex(
      GeometryId geometryId, ParameterRole role) const noexcept;
  // Returns a new immutable layout whose non-fixed values are replaced in
  // variableIndex order. Fixed values always remain those of the snapshot.
  [[nodiscard]] std::optional<ParameterLayout> withVariableValues(
      std::span<const double> candidate) const;

 private:
  std::vector<ParameterDescriptor> parameters_;
  std::vector<double> values_;
  std::size_t variableCount_{};
  double sketchScale_{1.0};
};

class EquationSystem final {
 public:
  EquationSystem() = default;
  EquationSystem(ConstraintGraphComponent component, ParameterLayout layout,
                 std::vector<EquationRow> rows,
                 std::vector<double> normalizedJacobian,
                 EquationStatus status);

  [[nodiscard]] const ConstraintGraphComponent& component() const noexcept {
    return component_;
  }
  [[nodiscard]] const ParameterLayout& layout() const noexcept {
    return layout_;
  }
  [[nodiscard]] const std::vector<EquationRow>& rows() const noexcept {
    return rows_;
  }
  // Dense row-major Jacobian of normalizedResidual with respect to every
  // domain parameter. Fixed-parameter columns are exactly zero.
  [[nodiscard]] const std::vector<double>& jacobian() const noexcept {
    return jacobian_;
  }
  [[nodiscard]] EquationStatus status() const noexcept { return status_; }
  [[nodiscard]] std::size_t parameterCount() const noexcept {
    return layout_.parameters().size();
  }
  [[nodiscard]] std::size_t variableCount() const noexcept {
    return layout_.variableCount();
  }
  [[nodiscard]] std::size_t equationCount() const noexcept {
    return rows_.size();
  }
  [[nodiscard]] std::size_t nonZeroJacobianCount(
      double threshold = 1e-14) const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;
  [[nodiscard]] std::uint64_t semanticHash() const noexcept;

 private:
  ConstraintGraphComponent component_;
  ParameterLayout layout_;
  std::vector<EquationRow> rows_;
  std::vector<double> jacobian_;
  EquationStatus status_{EquationStatus::Valid};
};

struct ConstraintEquationRule {
  ConstraintType type{ConstraintType::Horizontal};
  std::uint16_t minimumEquationCount{};
  std::uint16_t maximumEquationCount{};
  EquationUnit unit{EquationUnit::Length};
  bool fixesParameters{false};
};

class ConstraintEquationRegistry final {
 public:
  // Residual expressions and their forward-mode automatic derivatives are
  // assembled by the same evaluator. No Sketch mutator is called.
  [[nodiscard]] static EquationSystem assemble(
      const Sketch& sketch, const ConstraintGraphComponent& component);
  [[nodiscard]] static EquationSystem assemble(
      const Sketch& sketch, const ConstraintGraphComponent& component,
      std::span<const ConstraintId> ignoredConstraintIds);
  // Candidate contains exactly ParameterLayout::variableCount() values in
  // variableIndex order. Evaluation never mutates Sketch.
  [[nodiscard]] static std::optional<EquationSystem> assemble(
      const Sketch& sketch, const ConstraintGraphComponent& component,
      std::span<const double> candidate);
  [[nodiscard]] static std::optional<EquationSystem> assemble(
      const Sketch& sketch, const ConstraintGraphComponent& component,
      std::span<const double> candidate,
      std::span<const ConstraintId> ignoredConstraintIds);
  [[nodiscard]] static const ConstraintEquationRule* rule(
      ConstraintType type) noexcept;
  [[nodiscard]] static std::span<const ConstraintEquationRule> rules()
      noexcept;
};

struct EquationDiagnosticSummary {
  std::size_t equationRank{};
  std::size_t degreesOfFreedom{};
  double maxNormalizedResidual{};
  EquationStatus status{EquationStatus::Valid};
};

class EquationDiagnostics final {
 public:
  [[nodiscard]] static EquationDiagnosticSummary analyze(
      const EquationSystem& system, bool computeRank = true);
};

}  // namespace solidar::sketch
