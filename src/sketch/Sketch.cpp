#include "sketch/Sketch.h"

#include "sketch/SketchConstraintDiagnostics.h"
#include "sketch/SketchSchema.h"
#include "sketch/SketchSolver.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <numbers>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace solidar::sketch {

PointReference::PointReference() noexcept = default;
PointReference::PointReference(GeometryId id, bool isStart) noexcept
    : payload_(LineEndpointReference{
          id, isStart ? Endpoint::Start : Endpoint::End}) {}
PointReference::PointReference(DatumOriginReference source) noexcept
    : payload_(source) {}
PointReference::PointReference(LineEndpointReference source) noexcept
    : payload_(source) {}
PointReference::PointReference(CircleCenterReference source) noexcept
    : payload_(source) {}
PointReference::PointReference(ArcEndpointReference source) noexcept
    : payload_(source) {}
PointReference::PointReference(BezierPointReference source) noexcept
    : payload_(source) {}
PointReference::PointReference(CompositeCenterReference source) noexcept
    : payload_(source) {}
PointReference::PointReference(const PointReference& other) noexcept
    : payload_(other.payload_) {}
PointReference::PointReference(PointReference&& other) noexcept
    : payload_(std::move(other.payload_)) {}
PointReference& PointReference::operator=(const PointReference& other) noexcept {
  if (this != &other) payload_ = other.payload_;
  return *this;
}
PointReference& PointReference::operator=(PointReference&& other) noexcept {
  if (this != &other) payload_ = std::move(other.payload_);
  return *this;
}

PointReference::GeometryIdView& PointReference::GeometryIdView::operator=(
    GeometryId id) noexcept {
  if (id == kInvalidGeometryId) return *this;
  switch (kind_) {
    case IdViewKind::Line:
      owner_->payload_ = LineEndpointReference{id, Endpoint::Start};
      break;
    case IdViewKind::Circle:
      owner_->payload_ = CircleCenterReference{id};
      break;
    case IdViewKind::Arc:
      owner_->payload_ = ArcEndpointReference{id, Endpoint::Start};
      break;
    case IdViewKind::Bezier:
      owner_->payload_ = BezierPointReference{id, 0};
      break;
  }
  return *this;
}

PointReference::GeometryIdView::operator GeometryId() const noexcept {
  switch (kind_) {
    case IdViewKind::Line:
      if (const auto* value =
              std::get_if<LineEndpointReference>(&owner_->payload_))
        return value->lineId;
      break;
    case IdViewKind::Circle:
      if (const auto* value =
              std::get_if<CircleCenterReference>(&owner_->payload_))
        return value->circleId;
      break;
    case IdViewKind::Arc:
      if (const auto* value =
              std::get_if<ArcEndpointReference>(&owner_->payload_))
        return value->arcId;
      break;
    case IdViewKind::Bezier:
      if (const auto* value =
              std::get_if<BezierPointReference>(&owner_->payload_))
        return value->bezierId;
      break;
  }
  return kInvalidGeometryId;
}

PointReference::BoolView& PointReference::BoolView::operator=(
    bool value) noexcept {
  if (kind_ == BoolViewKind::Origin) {
    if (value) owner_->payload_ = DatumOriginReference{};
    return *this;
  }
  const Endpoint endpoint = value ? Endpoint::Start : Endpoint::End;
  if (auto* line = std::get_if<LineEndpointReference>(&owner_->payload_))
    line->endpoint = endpoint;
  else if (auto* arc =
               std::get_if<ArcEndpointReference>(&owner_->payload_))
    arc->endpoint = endpoint;
  return *this;
}

PointReference::BoolView::operator bool() const noexcept {
  if (kind_ == BoolViewKind::Origin)
    return std::holds_alternative<DatumOriginReference>(owner_->payload_);
  if (const auto* line =
          std::get_if<LineEndpointReference>(&owner_->payload_))
    return line->endpoint == Endpoint::Start;
  if (const auto* arc = std::get_if<ArcEndpointReference>(&owner_->payload_))
    return arc->endpoint == Endpoint::Start;
  return true;
}

PointReference::ElementIdView& PointReference::ElementIdView::operator=(
    std::size_t id) noexcept {
  if (id != 0) owner_->payload_ = CompositeCenterReference{id};
  return *this;
}
PointReference::ElementIdView::operator std::size_t() const noexcept {
  if (const auto* value =
          std::get_if<CompositeCenterReference>(&owner_->payload_))
    return static_cast<std::size_t>(value->elementId);
  return 0;
}

PointReference::BezierPointView&
PointReference::BezierPointView::operator=(std::uint8_t index) noexcept {
  if (auto* value = std::get_if<BezierPointReference>(&owner_->payload_))
    value->pointIndex = index;
  return *this;
}
PointReference::BezierPointView::operator std::uint8_t() const noexcept {
  if (const auto* value =
          std::get_if<BezierPointReference>(&owner_->payload_))
    return value->pointIndex;
  return 0;
}

namespace {

ConstraintPayload defaultPayload(ConstraintType type) {
  switch (type) {
    case ConstraintType::Horizontal: return HorizontalConstraint{};
    case ConstraintType::Vertical: return VerticalConstraint{};
    case ConstraintType::Coincident: return CoincidentConstraint{};
    case ConstraintType::PointOnLine: return PointOnLineConstraint{};
    case ConstraintType::Distance: return DistanceConstraint{};
    case ConstraintType::Length: return LengthConstraint{};
    case ConstraintType::Radius: return RadiusConstraint{};
    case ConstraintType::Diameter: return DiameterConstraint{};
    case ConstraintType::Parallel: return ParallelConstraint{};
    case ConstraintType::Perpendicular: return PerpendicularConstraint{};
    case ConstraintType::Equal: return EqualConstraint{};
    case ConstraintType::Angle: return AngleConstraint{};
    case ConstraintType::DistanceX: return DistanceXConstraint{};
    case ConstraintType::DistanceY: return DistanceYConstraint{};
    case ConstraintType::PointOnCircle: return PointOnCircleConstraint{};
    case ConstraintType::Tangent: return TangentConstraint{};
    case ConstraintType::LineDistance: return LineDistanceConstraint{};
    case ConstraintType::Lock: return LockConstraint{};
    case ConstraintType::PointOnArc: return PointOnArcConstraint{};
    case ConstraintType::Midpoint: return MidpointConstraint{};
    case ConstraintType::PointOnXAxis: return PointOnXAxisConstraint{};
    case ConstraintType::PointOnYAxis: return PointOnYAxisConstraint{};
    case ConstraintType::Count: break;
  }
  return UnsupportedConstraint{type};
}

ConstraintType payloadType(const ConstraintPayload& payload) noexcept {
  if (const auto* unsupported = std::get_if<UnsupportedConstraint>(&payload))
    return unsupported->rawType;
  return static_cast<ConstraintType>(payload.index());
}

template <typename T>
void setOptionalValue(T& payload, double value) {
  payload.drivingValue = value;
}

template <typename T>
double optionalValue(const T& payload) noexcept {
  return payload.drivingValue.value_or(0.0);
}

bool assignDrivingValue(ConstraintPayload& value,
                        std::optional<double> drivingValue) {
  return std::visit([&](auto& payload) {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, DistanceConstraint> ||
                  std::is_same_v<T, LengthConstraint> ||
                  std::is_same_v<T, RadiusConstraint> ||
                  std::is_same_v<T, DiameterConstraint> ||
                  std::is_same_v<T, AngleConstraint> ||
                  std::is_same_v<T, DistanceXConstraint> ||
                  std::is_same_v<T, DistanceYConstraint> ||
                  std::is_same_v<T, LineDistanceConstraint>) {
      payload.drivingValue = drivingValue;
      return true;
    } else {
      return false;
    }
  }, value);
}

bool hasDrivingValue(const ConstraintPayload& value) noexcept {
  return std::visit([](const auto& payload) {
    using T = std::decay_t<decltype(payload)>;
    return std::is_same_v<T, DistanceConstraint> ||
           std::is_same_v<T, LengthConstraint> ||
           std::is_same_v<T, RadiusConstraint> ||
           std::is_same_v<T, DiameterConstraint> ||
           std::is_same_v<T, AngleConstraint> ||
           std::is_same_v<T, DistanceXConstraint> ||
           std::is_same_v<T, DistanceYConstraint> ||
           std::is_same_v<T, LineDistanceConstraint>;
  }, value);
}

std::optional<double> drivingValueOf(
    const ConstraintPayload& value) noexcept {
  return std::visit([](const auto& payload) -> std::optional<double> {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, DistanceConstraint> ||
                  std::is_same_v<T, LengthConstraint> ||
                  std::is_same_v<T, RadiusConstraint> ||
                  std::is_same_v<T, DiameterConstraint> ||
                  std::is_same_v<T, AngleConstraint> ||
                  std::is_same_v<T, DistanceXConstraint> ||
                  std::is_same_v<T, DistanceYConstraint> ||
                  std::is_same_v<T, LineDistanceConstraint>)
      return payload.drivingValue;
    else
      return std::nullopt;
  }, value);
}

}  // namespace

Constraint::Constraint() noexcept = default;
Constraint::Constraint(ConstraintPayload payload, ConstraintMode constraintMode,
                       ConstraintId constraintId) noexcept
    : id(constraintId), mode(constraintMode), payload_(std::move(payload)) {}
Constraint::Constraint(const Constraint& other) noexcept
    : id(other.id), mode(other.mode), payload_(other.payload_) {}
Constraint::Constraint(Constraint&& other) noexcept
    : id(other.id), mode(other.mode), payload_(std::move(other.payload_)) {}
Constraint& Constraint::operator=(const Constraint& other) noexcept {
  if (this != &other) {
    id = other.id;
    mode = other.mode;
    payload_ = other.payload_;
  }
  return *this;
}
Constraint& Constraint::operator=(Constraint&& other) noexcept {
  if (this != &other) {
    id = other.id;
    mode = other.mode;
    payload_ = std::move(other.payload_);
  }
  return *this;
}

Constraint::TypeView& Constraint::TypeView::operator=(
    ConstraintType type) noexcept {
  const GeometryId first = owner_->firstGeometry;
  const GeometryId second = owner_->secondGeometry;
  const PointReference oldFirstPoint = owner_->firstPoint;
  const PointReference oldSecondPoint = owner_->secondPoint;
  const auto oldDrivingValue = drivingValueOf(owner_->payload_);
  owner_->payload_ = defaultPayload(type);
  owner_->firstGeometry = first;
  owner_->secondGeometry = second;
  owner_->firstPoint = oldFirstPoint;
  owner_->secondPoint = oldSecondPoint;
  if (oldDrivingValue) assignDrivingValue(owner_->payload_, oldDrivingValue);
  return *this;
}
Constraint::TypeView::operator ConstraintType() const noexcept {
  return payloadType(owner_->payload_);
}

Constraint::GeometryView& Constraint::GeometryView::operator=(
    GeometryId id) noexcept {
  const bool first = first_;
  std::visit([&](auto& payload) {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, HorizontalConstraint> ||
                  std::is_same_v<T, VerticalConstraint> ||
                  std::is_same_v<T, LengthConstraint> ||
                  std::is_same_v<T, PointOnLineConstraint> ||
                  std::is_same_v<T, MidpointConstraint>) {
      if (first) payload.lineId = id;
    } else if constexpr (std::is_same_v<T, RadiusConstraint> ||
                         std::is_same_v<T, DiameterConstraint> ||
                         std::is_same_v<T, PointOnCircleConstraint>) {
      if (first) payload.circleId = id;
    } else if constexpr (std::is_same_v<T, PointOnArcConstraint>) {
      if (first) payload.arcId = id;
    } else if constexpr (std::is_same_v<T, LockConstraint>) {
      if (first) payload.geometryId = id;
    } else if constexpr (std::is_same_v<T, ParallelConstraint> ||
                         std::is_same_v<T, PerpendicularConstraint> ||
                         std::is_same_v<T, AngleConstraint> ||
                         std::is_same_v<T, LineDistanceConstraint>) {
      (first ? payload.firstLineId : payload.secondLineId) = id;
    } else if constexpr (std::is_same_v<T, EqualConstraint> ||
                         std::is_same_v<T, TangentConstraint>) {
      (first ? payload.firstGeometryId : payload.secondGeometryId) = id;
    }
  }, owner_->payload_);
  return *this;
}

Constraint::GeometryView::operator GeometryId() const noexcept {
  const bool first = first_;
  return std::visit([&](const auto& payload) -> GeometryId {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, HorizontalConstraint> ||
                  std::is_same_v<T, VerticalConstraint> ||
                  std::is_same_v<T, LengthConstraint> ||
                  std::is_same_v<T, PointOnLineConstraint> ||
                  std::is_same_v<T, MidpointConstraint>) {
      return first ? payload.lineId : kInvalidGeometryId;
    } else if constexpr (std::is_same_v<T, RadiusConstraint> ||
                         std::is_same_v<T, DiameterConstraint> ||
                         std::is_same_v<T, PointOnCircleConstraint>) {
      return first ? payload.circleId : kInvalidGeometryId;
    } else if constexpr (std::is_same_v<T, PointOnArcConstraint>) {
      return first ? payload.arcId : kInvalidGeometryId;
    } else if constexpr (std::is_same_v<T, LockConstraint>) {
      return first ? payload.geometryId : kInvalidGeometryId;
    } else if constexpr (std::is_same_v<T, ParallelConstraint> ||
                         std::is_same_v<T, PerpendicularConstraint> ||
                         std::is_same_v<T, AngleConstraint> ||
                         std::is_same_v<T, LineDistanceConstraint>) {
      return first ? payload.firstLineId : payload.secondLineId;
    } else if constexpr (std::is_same_v<T, EqualConstraint> ||
                         std::is_same_v<T, TangentConstraint>) {
      return first ? payload.firstGeometryId : payload.secondGeometryId;
    } else {
      return kInvalidGeometryId;
    }
  }, owner_->payload_);
}

Constraint::PointView& Constraint::PointView::operator=(
    const PointReference& point) noexcept {
  const bool first = first_;
  std::visit([&](auto& payload) {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, CoincidentConstraint> ||
                  std::is_same_v<T, DistanceConstraint> ||
                  std::is_same_v<T, DistanceXConstraint> ||
                  std::is_same_v<T, DistanceYConstraint>) {
      (first ? payload.first : payload.second) = point;
    } else if constexpr (std::is_same_v<T, PointOnLineConstraint> ||
                         std::is_same_v<T, PointOnCircleConstraint> ||
                         std::is_same_v<T, PointOnArcConstraint> ||
                         std::is_same_v<T, MidpointConstraint> ||
                         std::is_same_v<T, PointOnXAxisConstraint> ||
                         std::is_same_v<T, PointOnYAxisConstraint>) {
      if (!first) payload.point = point;
    }
  }, owner_->payload_);
  return *this;
}

Constraint::PointView::operator PointReference() const noexcept {
  const bool first = first_;
  return std::visit([&](const auto& payload) -> PointReference {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, CoincidentConstraint> ||
                  std::is_same_v<T, DistanceConstraint> ||
                  std::is_same_v<T, DistanceXConstraint> ||
                  std::is_same_v<T, DistanceYConstraint>) {
      return first ? payload.first : payload.second;
    } else if constexpr (std::is_same_v<T, PointOnLineConstraint> ||
                         std::is_same_v<T, PointOnCircleConstraint> ||
                         std::is_same_v<T, PointOnArcConstraint> ||
                         std::is_same_v<T, MidpointConstraint> ||
                         std::is_same_v<T, PointOnXAxisConstraint> ||
                         std::is_same_v<T, PointOnYAxisConstraint>) {
      return !first ? payload.point
                    : PointReference{DatumOriginReference{}};
    } else {
      return PointReference{DatumOriginReference{}};
    }
  }, owner_->payload_);
}

Constraint::PointView::GeometryIdMember&
Constraint::PointView::GeometryIdMember::operator=(GeometryId id) noexcept {
  PointReference point = static_cast<PointReference>(*owner_);
  switch (kind_) {
    case PointReference::IdViewKind::Line: point.lineId = id; break;
    case PointReference::IdViewKind::Circle: point.circleId = id; break;
    case PointReference::IdViewKind::Arc: point.arcId = id; break;
    case PointReference::IdViewKind::Bezier: point.bezierId = id; break;
  }
  *owner_ = point;
  return *this;
}
Constraint::PointView::GeometryIdMember::operator GeometryId() const noexcept {
  const PointReference point = static_cast<PointReference>(*owner_);
  switch (kind_) {
    case PointReference::IdViewKind::Line: return point.lineId;
    case PointReference::IdViewKind::Circle: return point.circleId;
    case PointReference::IdViewKind::Arc: return point.arcId;
    case PointReference::IdViewKind::Bezier: return point.bezierId;
  }
  return kInvalidGeometryId;
}

Constraint::PointView::BoolMember&
Constraint::PointView::BoolMember::operator=(bool value) noexcept {
  PointReference point = static_cast<PointReference>(*owner_);
  if (kind_ == PointReference::BoolViewKind::Start)
    point.start = value;
  else
    point.origin = value;
  *owner_ = point;
  return *this;
}
Constraint::PointView::BoolMember::operator bool() const noexcept {
  const PointReference point = static_cast<PointReference>(*owner_);
  return kind_ == PointReference::BoolViewKind::Start
      ? static_cast<bool>(point.start)
      : static_cast<bool>(point.origin);
}

Constraint::PointView::ElementIdMember&
Constraint::PointView::ElementIdMember::operator=(std::size_t id) noexcept {
  PointReference point = static_cast<PointReference>(*owner_);
  point.elementCenterId = id;
  *owner_ = point;
  return *this;
}
Constraint::PointView::ElementIdMember::operator std::size_t() const noexcept {
  const PointReference point = static_cast<PointReference>(*owner_);
  return point.elementCenterId;
}

Constraint::PointView::BezierPointMember&
Constraint::PointView::BezierPointMember::operator=(
    std::uint8_t index) noexcept {
  PointReference point = static_cast<PointReference>(*owner_);
  point.bezierPoint = index;
  *owner_ = point;
  return *this;
}
Constraint::PointView::BezierPointMember::operator std::uint8_t() const
    noexcept {
  const PointReference point = static_cast<PointReference>(*owner_);
  return point.bezierPoint;
}

Constraint::ValueView& Constraint::ValueView::operator=(double value) noexcept {
  std::visit([&](auto& payload) {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, DistanceConstraint> ||
                  std::is_same_v<T, LengthConstraint> ||
                  std::is_same_v<T, RadiusConstraint> ||
                  std::is_same_v<T, DiameterConstraint> ||
                  std::is_same_v<T, AngleConstraint> ||
                  std::is_same_v<T, DistanceXConstraint> ||
                  std::is_same_v<T, DistanceYConstraint> ||
                  std::is_same_v<T, LineDistanceConstraint>)
      setOptionalValue(payload, value);
  }, owner_->payload_);
  return *this;
}

Constraint::ValueView::operator double() const noexcept {
  return std::visit([](const auto& payload) -> double {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, DistanceConstraint> ||
                  std::is_same_v<T, LengthConstraint> ||
                  std::is_same_v<T, RadiusConstraint> ||
                  std::is_same_v<T, DiameterConstraint> ||
                  std::is_same_v<T, AngleConstraint> ||
                  std::is_same_v<T, DistanceXConstraint> ||
                  std::is_same_v<T, DistanceYConstraint> ||
                  std::is_same_v<T, LineDistanceConstraint>)
      return optionalValue(payload);
    else
      return 0.0;
  }, owner_->payload_);
}

namespace {
std::atomic_size_t fullSketchCopyCount{};
std::atomic_size_t deltaJournalBeginCount{};
std::atomic_bool failNextNestedConstraintJournal{};

SketchMutationResult mutationResult(
    const Sketch& sketch, const std::vector<GeometryId>& seeds,
    SketchMutationStatus status, SketchMutationDiagnosticCode code,
    SolveResult solve = {}) {
  SketchMutationResult result;
  result.status = status;
  result.diagnosticCode = code;
  result.solveResult = solve;
  result.component = sketch.connectedComponent(seeds);
  result.relatedConstraintIds = result.component.constraintIds;
  return result;
}

SketchMutationResult mutationFromSolve(
    const Sketch& sketch, const std::vector<GeometryId>& seeds,
    const SolveResult& solve) {
  if (solve.invalidReferences != 0)
    return mutationResult(sketch, seeds,
                          SketchMutationStatus::InvalidReference,
                          SketchMutationDiagnosticCode::InvalidGeometryReference,
                          solve);
  if (solve.unsupported != 0)
    return mutationResult(sketch, seeds, SketchMutationStatus::Unsupported,
                          SketchMutationDiagnosticCode::UnsupportedConstraint,
                          solve);
  if (solve.deferredByActiveSolve)
    return mutationResult(sketch, seeds,
                          SketchMutationStatus::DeferredByActiveSolve,
                          SketchMutationDiagnosticCode::ActiveSolve, solve);
  if (solve.nonFiniteGeometry != 0)
    return mutationResult(sketch, seeds, SketchMutationStatus::InternalError,
                          SketchMutationDiagnosticCode::NonFiniteGeometry,
                          solve);
  if (solve.degenerateGeometry != 0)
    return mutationResult(sketch, seeds, SketchMutationStatus::Conflicting,
                          SketchMutationDiagnosticCode::DegenerateGeometry,
                          solve);
  if (solve.violatedConstraints != 0)
    return mutationResult(
        sketch, seeds, SketchMutationStatus::Conflicting,
        SketchMutationDiagnosticCode::ConstraintResidualViolation, solve);
  if (!solve.converged)
    return mutationResult(sketch, seeds, SketchMutationStatus::NonConverged,
                          SketchMutationDiagnosticCode::SolverDidNotConverge,
                          solve);
  return mutationResult(sketch, seeds, SketchMutationStatus::Applied,
                        SketchMutationDiagnosticCode::None, solve);
}

SketchMutationResult noMutation(const Sketch& sketch,
                                const std::vector<GeometryId>& seeds = {}) {
  return mutationResult(sketch, seeds, SketchMutationStatus::NoChange,
                        SketchMutationDiagnosticCode::None);
}

SketchMutationResult rejectedMutation(
    const Sketch& sketch, const std::vector<GeometryId>& seeds,
    SketchMutationStatus status, SketchMutationDiagnosticCode code) {
  return mutationResult(sketch, seeds, status, code);
}

bool finitePoint(Point point) noexcept {
  return std::isfinite(point.xMm) && std::isfinite(point.yMm);
}

struct FiniteBoundsAccumulator {
  double minX{std::numeric_limits<double>::max()};
  double minY{std::numeric_limits<double>::max()};
  double maxX{std::numeric_limits<double>::lowest()};
  double maxY{std::numeric_limits<double>::lowest()};
  bool hasPoint{};

  bool include(Point point) noexcept {
    if (!finitePoint(point)) return false;
    minX = std::min(minX, point.xMm);
    minY = std::min(minY, point.yMm);
    maxX = std::max(maxX, point.xMm);
    maxY = std::max(maxY, point.yMm);
    hasPoint = true;
    return true;
  }

  [[nodiscard]] bool finiteSpan() const noexcept {
    return !hasPoint ||
           (std::isfinite(maxX - minX) && std::isfinite(maxY - minY));
  }
};

bool includeCircleBounds(FiniteBoundsAccumulator& bounds,
                         const Circle& circle) {
  return std::isfinite(circle.radiusMm) && circle.radiusMm > 0.0 &&
         bounds.include({circle.center.xMm - circle.radiusMm,
                         circle.center.yMm - circle.radiusMm}) &&
         bounds.include({circle.center.xMm + circle.radiusMm,
                         circle.center.yMm + circle.radiusMm});
}

bool includeArcBounds(FiniteBoundsAccumulator& bounds, const Arc& arc) {
  if (!bounds.include(arcStartPoint(arc)) ||
      !bounds.include(arcEndPoint(arc)))
    return false;
  constexpr double kPi = 3.14159265358979323846;
  const auto normalizedAngle = [](double angle) {
    constexpr double kTwoPi = 6.28318530717958647692;
    angle = std::fmod(angle, kTwoPi);
    return angle < 0.0 ? angle + kTwoPi : angle;
  };
  const double start = normalizedAngle(arc.startAngleRad);
  for (const double angle : {0.0, 0.5 * kPi, kPi, 1.5 * kPi}) {
    const double delta = normalizedAngle(normalizedAngle(angle) - start);
    if (delta > arc.sweepAngleRad + 1e-12) continue;
    if (!bounds.include({arc.center.xMm + arc.radiusMm * std::cos(angle),
                         arc.center.yMm + arc.radiusMm * std::sin(angle)}))
      return false;
  }
  return true;
}

bool prospectiveBoundsFinite(const Sketch& sketch,
                             std::initializer_list<Point> extraPoints,
                             std::optional<Circle> extraCircle = std::nullopt,
                             std::optional<Arc> extraArc = std::nullopt,
                             std::optional<Bezier> extraBezier = std::nullopt) {
  FiniteBoundsAccumulator bounds;
  for (const auto& line : sketch.lines())
    if (!bounds.include(line.start) || !bounds.include(line.end)) return false;
  for (const auto& circle : sketch.circles())
    if (!includeCircleBounds(bounds, circle)) return false;
  for (const auto& arc : sketch.arcs())
    if (!includeArcBounds(bounds, arc)) return false;
  for (const auto& bezier : sketch.beziers())
    for (const auto point : bezier.points)
      if (!bounds.include(point)) return false;
  for (const auto point : extraPoints)
    if (!bounds.include(point)) return false;
  if (extraCircle && !includeCircleBounds(bounds, *extraCircle)) return false;
  if (extraArc && !includeArcBounds(bounds, *extraArc)) return false;
  if (extraBezier)
    for (const auto point : extraBezier->points)
      if (!bounds.include(point)) return false;
  return bounds.finiteSpan();
}

bool translatedGeometryBoundsFinite(const Sketch& sketch,
                                    const std::vector<GeometryId>& movedIds,
                                    double dxMm, double dyMm) {
  const auto moved = [&movedIds](GeometryId id) {
    return std::find(movedIds.begin(), movedIds.end(), id) != movedIds.end();
  };
  const auto translated = [dxMm, dyMm](Point point) {
    point.xMm += dxMm;
    point.yMm += dyMm;
    return point;
  };
  FiniteBoundsAccumulator bounds;
  for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
    const bool move = moved(sketch.lineId(index));
    const auto& line = sketch.lines()[index];
    if (!bounds.include(move ? translated(line.start) : line.start) ||
        !bounds.include(move ? translated(line.end) : line.end))
      return false;
  }
  for (std::size_t index = 0; index < sketch.circles().size(); ++index) {
    Circle circle = sketch.circles()[index];
    if (moved(sketch.circleId(index))) circle.center = translated(circle.center);
    if (!includeCircleBounds(bounds, circle)) return false;
  }
  for (std::size_t index = 0; index < sketch.arcs().size(); ++index) {
    Arc arc = sketch.arcs()[index];
    if (moved(sketch.arcId(index))) arc.center = translated(arc.center);
    if (!includeArcBounds(bounds, arc)) return false;
  }
  for (std::size_t index = 0; index < sketch.beziers().size(); ++index) {
    const bool move = moved(sketch.bezierId(index));
    for (auto point : sketch.beziers()[index].points) {
      if (move) point = translated(point);
      if (!bounds.include(point)) return false;
    }
  }
  return bounds.finiteSpan();
}

bool translatedPointReferencesBoundsFinite(
    const Sketch& sketch, const std::vector<PointReference>& references,
    double dxMm, double dyMm) {
  const auto translated = [dxMm, dyMm](Point point, std::size_t count = 1) {
    for (std::size_t index = 0; index < count; ++index) {
      point.xMm += dxMm;
      point.yMm += dyMm;
    }
    return point;
  };
  const auto movesElement = [&references](std::size_t elementId) {
    return std::any_of(references.begin(), references.end(),
                       [elementId](PointReference reference) {
                         return reference.elementCenterId == elementId;
                       });
  };
  FiniteBoundsAccumulator bounds;
  for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
    auto start = sketch.lines()[index].start;
    auto end = sketch.lines()[index].end;
    if (movesElement(sketch.lines()[index].elementId)) {
      start = translated(start);
      end = translated(end);
    } else {
      const auto id = sketch.lineId(index);
      for (const auto reference : references) {
        if (reference.lineId != id) continue;
        if (reference.start)
          start = translated(start);
        else
          end = translated(end);
      }
    }
    if (!bounds.include(start) || !bounds.include(end)) return false;
  }
  for (std::size_t index = 0; index < sketch.circles().size(); ++index) {
    Circle circle = sketch.circles()[index];
    const auto id = sketch.circleId(index);
    if (std::any_of(references.begin(), references.end(),
                    [id](PointReference reference) {
                      return reference.circleId == id;
                    }))
      circle.center = translated(circle.center);
    if (!includeCircleBounds(bounds, circle)) return false;
  }
  for (std::size_t index = 0; index < sketch.arcs().size(); ++index) {
    Arc arc = sketch.arcs()[index];
    const auto id = sketch.arcId(index);
    const auto count = static_cast<std::size_t>(std::count_if(
        references.begin(), references.end(), [id](PointReference reference) {
          return reference.arcId == id;
        }));
    if (count != 0) arc.center = translated(arc.center, count);
    if (!includeArcBounds(bounds, arc)) return false;
  }
  for (std::size_t index = 0; index < sketch.beziers().size(); ++index) {
    const auto id = sketch.bezierId(index);
    for (std::size_t pointIndex = 0; pointIndex < 4; ++pointIndex) {
      auto point = sketch.beziers()[index].points[pointIndex];
      for (const auto reference : references)
        if (reference.bezierId == id &&
            reference.bezierPoint == pointIndex)
          point = translated(point);
      if (!bounds.include(point)) return false;
    }
  }
  return bounds.finiteSpan();
}

int pointReferenceSourceCount(PointReference reference) noexcept {
  return (reference.origin ? 1 : 0) +
         (reference.elementCenterId != 0 ? 1 : 0) +
         (reference.lineId != kInvalidGeometryId ? 1 : 0) +
         (reference.circleId != kInvalidGeometryId ? 1 : 0) +
         (reference.arcId != kInvalidGeometryId ? 1 : 0) +
         (reference.bezierId != kInvalidGeometryId ? 1 : 0);
}

bool emptyPointReference(PointReference reference) noexcept {
  return pointReferenceSourceCount(reference) == 0;
}

bool validPointReferenceForConstraint(const Sketch& sketch,
                                      PointReference reference) {
  return pointReferenceSourceCount(reference) == 1 &&
         (reference.origin || sketch.referencedPoint(reference).has_value());
}

bool samePointReference(PointReference first, PointReference second) noexcept {
  return first.lineId == second.lineId && first.start == second.start &&
         first.circleId == second.circleId &&
         first.elementCenterId == second.elementCenterId &&
         first.arcId == second.arcId && first.origin == second.origin &&
         first.bezierId == second.bezierId &&
         first.bezierPoint == second.bezierPoint;
}

bool equivalentConstraint(const Constraint& first,
                          const Constraint& second) noexcept {
  if (first.type != second.type ||
      std::abs(first.value - second.value) > 1e-9)
    return false;
  const bool sameGeometryOrder =
      first.firstGeometry == second.firstGeometry &&
      first.secondGeometry == second.secondGeometry;
  const bool reverseGeometryOrder =
      first.firstGeometry == second.secondGeometry &&
      first.secondGeometry == second.firstGeometry;
  const bool samePointOrder =
      samePointReference(first.firstPoint, second.firstPoint) &&
      samePointReference(first.secondPoint, second.secondPoint);
  const bool reversePointOrder =
      samePointReference(first.firstPoint, second.secondPoint) &&
      samePointReference(first.secondPoint, second.firstPoint);
  switch (first.type) {
    case ConstraintType::Parallel:
    case ConstraintType::Perpendicular:
    case ConstraintType::Equal:
    case ConstraintType::Angle:
    case ConstraintType::Tangent:
    case ConstraintType::LineDistance:
      return (sameGeometryOrder || reverseGeometryOrder) && samePointOrder;
    case ConstraintType::Coincident:
    case ConstraintType::Distance:
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY:
      return sameGeometryOrder && (samePointOrder || reversePointOrder);
    default:
      return sameGeometryOrder && samePointOrder;
  }
}

void canonicalizeConstraint(Constraint& constraint) noexcept {
  if (auto* tangent = std::get_if<TangentConstraint>(&constraint.payload());
      tangent && tangent->secondGeometryId < tangent->firstGeometryId)
    std::swap(tangent->firstGeometryId, tangent->secondGeometryId);
  if (auto* angle = std::get_if<AngleConstraint>(&constraint.payload());
      angle && angle->secondLineId < angle->firstLineId)
    std::swap(angle->firstLineId, angle->secondLineId);
}

bool supportedConstraintSchema(const Sketch& sketch,
                               const Constraint& constraint) {
  return validateConstraintSchema(sketch, constraint).valid();
}

bool annotatableConstraintType(ConstraintType type) noexcept {
  return type == ConstraintType::Distance ||
         type == ConstraintType::Length ||
         type == ConstraintType::Radius ||
         type == ConstraintType::Diameter ||
         type == ConstraintType::Angle ||
         type == ConstraintType::DistanceX ||
         type == ConstraintType::DistanceY ||
         type == ConstraintType::LineDistance;
}

std::vector<GeometryId> referencedGeometryIds(const Sketch& sketch,
                                              const Constraint& constraint) {
  std::vector<GeometryId> result;
  const auto add = [&result](GeometryId id) {
    if (id != kInvalidGeometryId &&
        std::find(result.begin(), result.end(), id) == result.end())
      result.push_back(id);
  };
  add(constraint.firstGeometry);
  add(constraint.secondGeometry);
  for (const auto point : {constraint.firstPoint, constraint.secondPoint}) {
    add(point.lineId);
    add(point.circleId);
    add(point.arcId);
    add(point.bezierId);
    if (point.elementCenterId != 0) {
      for (std::size_t i = 0; i < sketch.lines().size(); ++i)
        if (sketch.lines()[i].elementId == point.elementCenterId)
          add(sketch.lineId(i));
    }
  }
  return result;
}

[[nodiscard]] std::optional<DimensionId> firstFreeDimensionId(
    const std::vector<Dimension>& dimensions, DimensionId start,
    DimensionId additionallyReserved = kInvalidDimensionId) noexcept {
  DimensionId candidate = start == kInvalidDimensionId ? DimensionId{1} : start;
  // There are at most dimensions.size()+1 occupied values to inspect (the
  // optional reserved ID is the not-yet-inserted dimension). Pigeonhole
  // guarantees a free value within the following bounded scan.
  for (std::size_t attempts = 0; attempts <= dimensions.size() + 1;
       ++attempts) {
    const bool occupied = candidate == additionallyReserved ||
        std::any_of(dimensions.begin(), dimensions.end(),
                    [candidate](const Dimension& dimension) {
                      return dimension.id == candidate;
                    });
    if (!occupied) return candidate;
    candidate = candidate == std::numeric_limits<DimensionId>::max()
        ? DimensionId{1}
        : candidate + 1;
  }
  return std::nullopt;
}
}

Point arcStartPoint(const Arc& arc) noexcept {
  return {arc.center.xMm + arc.radiusMm * std::cos(arc.startAngleRad),
          arc.center.yMm + arc.radiusMm * std::sin(arc.startAngleRad)};
}

Point arcEndPoint(const Arc& arc) noexcept {
  const double angle = arc.startAngleRad + arc.sweepAngleRad;
  return {arc.center.xMm + arc.radiusMm * std::cos(angle),
          arc.center.yMm + arc.radiusMm * std::sin(angle)};
}

Point bezierPointAt(const Bezier& bezier, double t) noexcept {
  t = std::clamp(t, 0.0, 1.0);
  const double u = 1.0 - t;
  const double w0 = u * u * u;
  const double w1 = 3.0 * u * u * t;
  const double w2 = 3.0 * u * t * t;
  const double w3 = t * t * t;
  return {w0 * bezier.points[0].xMm + w1 * bezier.points[1].xMm +
              w2 * bezier.points[2].xMm + w3 * bezier.points[3].xMm,
          w0 * bezier.points[0].yMm + w1 * bezier.points[1].yMm +
              w2 * bezier.points[2].yMm + w3 * bezier.points[3].yMm};
}

std::vector<Bezier> interpolatingBezierChain(
    const std::vector<Point>& knots) {
  std::vector<Bezier> result;
  if (knots.size() < 2) return result;
  result.reserve(knots.size() - 1);

  for (std::size_t index = 0; index + 1 < knots.size(); ++index) {
    const Point start = knots[index];
    const Point end = knots[index + 1];
    if (std::hypot(end.xMm - start.xMm, end.yMm - start.yMm) <= 1e-9)
      continue;

    const Point previous = index > 0 ? knots[index - 1] : start;
    const Point next = index + 2 < knots.size() ? knots[index + 2] : end;
    Bezier segment;
    segment.points = {
        start,
        {start.xMm + (end.xMm - previous.xMm) / 6.0,
         start.yMm + (end.yMm - previous.yMm) / 6.0},
        {end.xMm - (next.xMm - start.xMm) / 6.0,
         end.yMm - (next.yMm - start.yMm) / 6.0},
        end};
    result.push_back(segment);
  }
  return result;
}

namespace {

// First safe Arc constraint primitive: keep the Arc shape rigid and translate
// the whole curve when one of its endpoint references has to move. This avoids
// rebuilding radius/sweep inside the sequential constraint solver.
bool moveArcEndpointRigid(Arc& arc, bool start, Point target) {
  const Point current = start ? arcStartPoint(arc) : arcEndPoint(arc);
  const double dx = target.xMm - current.xMm;
  const double dy = target.yMm - current.yMm;
  if (std::abs(dx) <= 1e-12 && std::abs(dy) <= 1e-12) return true;
  arc.center.xMm += dx;
  arc.center.yMm += dy;
  return true;
}

// Reshape an arc by moving one endpoint while keeping the center, radius and
// the opposite endpoint fixed. Used by interactive endpoint dragging (unlike
// moveArcEndpointRigid, which translates the whole curve for the solver).
bool moveArcEndpointReshape(Arc& arc, bool start, Point target) {
  const double dx = target.xMm - arc.center.xMm;
  const double dy = target.yMm - arc.center.yMm;
  if (std::hypot(dx, dy) <= 1e-12) return false;

  const double targetAngle = std::atan2(dy, dx);
  constexpr double kTwoPi = 6.28318530717958647692;
  const auto normalizeSweep = [kTwoPi](double sweep) {
    while (sweep <= 1e-9) sweep += kTwoPi;
    while (sweep >= kTwoPi - 1e-9) sweep -= kTwoPi;
    return sweep;
  };

  if (start) {
    const double endAngle = arc.startAngleRad + arc.sweepAngleRad;
    arc.startAngleRad = targetAngle;
    arc.sweepAngleRad = normalizeSweep(endAngle - targetAngle);
  } else {
    arc.sweepAngleRad = normalizeSweep(targetAngle - arc.startAngleRad);
  }
  return true;
}

bool translatedSelectionBoundsFinite(
    const Sketch& sketch, const std::vector<std::size_t>& elementIds,
    const std::vector<GeometryId>& circleIds,
    const std::vector<GeometryId>& arcIds,
    const std::vector<GeometryId>& bezierIds,
    const std::vector<PointReference>& movedReferences, double dxMm,
    double dyMm) {
  const auto selectedElement = [&elementIds](std::size_t id) {
    return std::find(elementIds.begin(), elementIds.end(), id) !=
           elementIds.end();
  };
  const auto selectedGeometry = [](const auto& ids, GeometryId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  const auto movedElementCenter = [&movedReferences](std::size_t id) {
    return std::any_of(movedReferences.begin(), movedReferences.end(),
                       [id](PointReference reference) {
                         return reference.elementCenterId == id;
                       });
  };
  const auto movedLinePoint = [&movedReferences](GeometryId id, bool start) {
    return std::any_of(movedReferences.begin(), movedReferences.end(),
                       [id, start](PointReference reference) {
                         return reference.lineId == id &&
                                reference.start == start;
                       });
  };
  const auto movedCircleCenter = [&movedReferences](GeometryId id) {
    return std::any_of(movedReferences.begin(), movedReferences.end(),
                       [id](PointReference reference) {
                         return reference.circleId == id;
                       });
  };
  const auto movedArcPoint = [&movedReferences](GeometryId id, bool start) {
    return std::any_of(movedReferences.begin(), movedReferences.end(),
                       [id, start](PointReference reference) {
                         return reference.arcId == id &&
                                reference.start == start;
                       });
  };
  const auto movedBezierPoint = [&movedReferences](GeometryId id,
                                                    std::uint8_t point) {
    return std::any_of(movedReferences.begin(), movedReferences.end(),
                       [id, point](PointReference reference) {
                         return reference.bezierId == id &&
                                reference.bezierPoint == point;
                       });
  };
  const auto translate = [dxMm, dyMm](Point point) {
    point.xMm += dxMm;
    point.yMm += dyMm;
    return point;
  };

  FiniteBoundsAccumulator bounds;
  for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
    const auto& source = sketch.lines()[index];
    auto start = source.start;
    auto end = source.end;
    if (selectedElement(source.elementId) ||
        movedElementCenter(source.elementId)) {
      start = translate(start);
      end = translate(end);
    } else {
      const auto id = sketch.lineId(index);
      if (movedLinePoint(id, true)) start = translate(start);
      if (movedLinePoint(id, false)) end = translate(end);
    }
    if (!bounds.include(start) || !bounds.include(end)) return false;
  }
  for (std::size_t index = 0; index < sketch.circles().size(); ++index) {
    Circle circle = sketch.circles()[index];
    const auto id = sketch.circleId(index);
    if (selectedGeometry(circleIds, id) || movedCircleCenter(id))
      circle.center = translate(circle.center);
    if (!includeCircleBounds(bounds, circle)) return false;
  }
  for (std::size_t index = 0; index < sketch.arcs().size(); ++index) {
    Arc arc = sketch.arcs()[index];
    const auto id = sketch.arcId(index);
    const bool moveStart = movedArcPoint(id, true);
    const bool moveEnd = movedArcPoint(id, false);
    if (selectedGeometry(arcIds, id) || (moveStart && moveEnd)) {
      arc.center = translate(arc.center);
    } else if (moveStart || moveEnd) {
      const bool start = moveStart;
      const auto current = start ? arcStartPoint(arc) : arcEndPoint(arc);
      const auto target = translate(current);
      if (!finitePoint(target)) return false;
      static_cast<void>(moveArcEndpointReshape(arc, start, target));
    }
    if (!includeArcBounds(bounds, arc)) return false;
  }
  for (std::size_t index = 0; index < sketch.beziers().size(); ++index) {
    const auto id = sketch.bezierId(index);
    const bool selected = selectedGeometry(bezierIds, id);
    for (std::uint8_t pointIndex = 0; pointIndex < 4; ++pointIndex) {
      auto point = sketch.beziers()[index].points[pointIndex];
      if (selected || movedBezierPoint(id, pointIndex))
        point = translate(point);
      if (!bounds.include(point)) return false;
    }
  }
  return bounds.finiteSpan();
}

bool replacementArcBoundsFinite(const Sketch& sketch, std::size_t arcIndex,
                                const Arc& replacement) {
  FiniteBoundsAccumulator bounds;
  for (const auto& line : sketch.lines())
    if (!bounds.include(line.start) || !bounds.include(line.end)) return false;
  for (const auto& circle : sketch.circles())
    if (!includeCircleBounds(bounds, circle)) return false;
  for (std::size_t index = 0; index < sketch.arcs().size(); ++index)
    if (!includeArcBounds(bounds,
                          index == arcIndex ? replacement
                                            : sketch.arcs()[index]))
      return false;
  for (const auto& bezier : sketch.beziers())
    for (const auto point : bezier.points)
      if (!bounds.include(point)) return false;
  return bounds.finiteSpan();
}

// Move one Arc endpoint exactly onto a constraint target while leaving the
// opposite endpoint fixed. Preserve the included angle, so the Arc keeps its
// overall shape while its radius and centre adapt to the new chord.
bool moveArcEndpointForConstraint(Arc& arc, bool start, Point target) {
  const Point fixed = start ? arcEndPoint(arc) : arcStartPoint(arc);
  const Point newStart = start ? target : fixed;
  const Point newEnd = start ? fixed : target;
  const double chordX = newEnd.xMm - newStart.xMm;
  const double chordY = newEnd.yMm - newStart.yMm;
  const double chordLength = std::hypot(chordX, chordY);
  if (chordLength <= 1e-12) return false;

  constexpr double kTwoPi = 6.28318530717958647692;
  const double sweep = std::clamp(arc.sweepAngleRad, 1e-9,
                                  kTwoPi - 1e-9);
  const double halfSweep = sweep * 0.5;
  const double sinHalfSweep = std::sin(halfSweep);
  if (std::abs(sinHalfSweep) <= 1e-12) return false;

  const double radius = chordLength / (2.0 * sinHalfSweep);
  const Point midpoint{(newStart.xMm + newEnd.xMm) * 0.5,
                       (newStart.yMm + newEnd.yMm) * 0.5};
  const double centerOffset = radius * std::cos(halfSweep);
  const double leftNormalX = -chordY / chordLength;
  const double leftNormalY = chordX / chordLength;

  arc.center = {midpoint.xMm + leftNormalX * centerOffset,
                midpoint.yMm + leftNormalY * centerOffset};
  arc.radiusMm = radius;
  arc.startAngleRad =
      std::atan2(newStart.yMm - arc.center.yMm,
                 newStart.xMm - arc.center.xMm);
  arc.sweepAngleRad = sweep;
  return true;
}

}  // namespace

Sketch::Sketch() { clear(); }

Sketch::Sketch(const Sketch& other)
    : widthMm_(other.widthMm_),
      heightMm_(other.heightMm_),
      lines_(other.lines_), circles_(other.circles_), arcs_(other.arcs_),
      beziers_(other.beziers_),
      lineIds_(other.lineIds_), circleIds_(other.circleIds_),
      arcIds_(other.arcIds_), bezierIds_(other.bezierIds_),
      dimensions_(other.dimensions_),
      constraints_(other.constraints_),
      centerNodeElementIds_(other.centerNodeElementIds_),
      nextElementId_(other.nextElementId_),
      nextGeometryId_(other.nextGeometryId_),
      nextConstraintId_(other.nextConstraintId_),
      nextDimensionId_(other.nextDimensionId_),
      structureIndexesDirty_(other.structureIndexesDirty_),
      connectivityDirty_(other.connectivityDirty_),
      geometryIndex_(other.geometryIndex_),
      constraintIndex_(other.constraintIndex_),
      connectivity_(other.connectivity_),
      geometryConstraints_(other.geometryConstraints_),
      lastSolvedFingerprint_(other.lastSolvedFingerprint_),
      hasLastSolvedFingerprint_(other.hasLastSolvedFingerprint_),
      lastSolveConverged_(other.lastSolveConverged_),
      lastSolveViolatedConstraints_(other.lastSolveViolatedConstraints_),
      lastSolveMaxNormalizedResidual_(other.lastSolveMaxNormalizedResidual_),
      lastSolveUnsupported_(other.lastSolveUnsupported_),
      lastSolveInvalidReferences_(other.lastSolveInvalidReferences_),
      lastSolveResult_(other.lastSolveResult_) {
  ++fullSketchCopyCount;
}

Sketch& Sketch::operator=(const Sketch& other) {
  if (this == &other) return *this;
  ++fullSketchCopyCount;
  widthMm_ = other.widthMm_; heightMm_ = other.heightMm_;
  lines_ = other.lines_; circles_ = other.circles_; arcs_ = other.arcs_;
  beziers_ = other.beziers_;
  lineIds_ = other.lineIds_; circleIds_ = other.circleIds_;
  arcIds_ = other.arcIds_; bezierIds_ = other.bezierIds_;
  dimensions_ = other.dimensions_;
  constraints_ = other.constraints_;
  centerNodeElementIds_ = other.centerNodeElementIds_;
  nextElementId_ = other.nextElementId_;
  nextGeometryId_ = other.nextGeometryId_;
  nextConstraintId_ = other.nextConstraintId_;
  nextDimensionId_ = other.nextDimensionId_;
  structureIndexesDirty_ = other.structureIndexesDirty_;
  connectivityDirty_ = other.connectivityDirty_;
  geometryIndex_ = other.geometryIndex_; constraintIndex_ = other.constraintIndex_;
  connectivity_ = other.connectivity_;
  geometryConstraints_ = other.geometryConstraints_;
  lastSolvedFingerprint_ = other.lastSolvedFingerprint_;
  hasLastSolvedFingerprint_ = other.hasLastSolvedFingerprint_;
  lastSolveConverged_ = other.lastSolveConverged_;
  lastSolveViolatedConstraints_ = other.lastSolveViolatedConstraints_;
  lastSolveMaxNormalizedResidual_ = other.lastSolveMaxNormalizedResidual_;
  lastSolveUnsupported_ = other.lastSolveUnsupported_;
  lastSolveInvalidReferences_ = other.lastSolveInvalidReferences_;
  lastSolveResult_ = other.lastSolveResult_;
  deltaJournals_.clear();
  return *this;
}

void Sketch::resetFullCopyCountForTesting() noexcept {
  fullSketchCopyCount.store(0);
}

std::size_t Sketch::fullCopyCountForTesting() noexcept {
  return fullSketchCopyCount.load();
}

void Sketch::resetDeltaJournalBeginCountForTesting() noexcept {
  deltaJournalBeginCount.store(0);
}

std::size_t Sketch::deltaJournalBeginCountForTesting() noexcept {
  return deltaJournalBeginCount.load();
}

void Sketch::failNextNestedConstraintJournalForTesting() noexcept {
  failNextNestedConstraintJournal.store(true);
}

bool Sketch::deltaJournalActive() const noexcept {
  return !deltaJournals_.empty();
}

std::size_t Sketch::deltaJournalDepth() const noexcept {
  return deltaJournals_.size();
}

bool Sketch::rollbackDeltaJournalsToDepth(std::size_t depth) noexcept {
  if (depth > deltaJournals_.size()) return false;
  bool success = true;
  while (deltaJournals_.size() > depth) {
    try {
      static_cast<void>(cancelDeltaJournal());
    } catch (...) {
      // cancelDeltaJournal removes the top frame before applying its inverse.
      // Keep unwinding any remaining controller-owned frames so a failed
      // rollback can never orphan a journal and wedge subsequent history.
      success = false;
    }
  }
  return success;
}

std::vector<ConstraintId> Sketch::invalidReferenceConstraintIds() const {
  std::unordered_map<GeometryId, GeometryKind> geometry;
  geometry.reserve(lineIds_.size() + circleIds_.size() + arcIds_.size() +
                   bezierIds_.size());
  for (const auto id : lineIds_) geometry.emplace(id, GeometryKind::Line);
  for (const auto id : circleIds_) geometry.emplace(id, GeometryKind::Circle);
  for (const auto id : arcIds_) geometry.emplace(id, GeometryKind::Arc);
  for (const auto id : bezierIds_) geometry.emplace(id, GeometryKind::Bezier);
  const auto validPoint = [this, &geometry](const PointReference& point) {
    const unsigned sourceCount = static_cast<unsigned>(point.origin) +
        static_cast<unsigned>(point.lineId != kInvalidGeometryId) +
        static_cast<unsigned>(point.circleId != kInvalidGeometryId) +
        static_cast<unsigned>(point.arcId != kInvalidGeometryId) +
        static_cast<unsigned>(point.bezierId != kInvalidGeometryId) +
        static_cast<unsigned>(point.elementCenterId != 0);
    if (sourceCount > 1) return false;
    const auto hasKind = [&geometry](GeometryId id, GeometryKind kind) {
      const auto found = geometry.find(id);
      return found != geometry.end() && found->second == kind;
    };
    return (point.lineId == kInvalidGeometryId ||
            hasKind(point.lineId, GeometryKind::Line)) &&
           (point.circleId == kInvalidGeometryId ||
            hasKind(point.circleId, GeometryKind::Circle)) &&
           (point.arcId == kInvalidGeometryId ||
            hasKind(point.arcId, GeometryKind::Arc)) &&
           (point.bezierId == kInvalidGeometryId ||
            (point.bezierPoint < 4 &&
             hasKind(point.bezierId, GeometryKind::Bezier))) &&
           (point.elementCenterId == 0 ||
            std::find(centerNodeElementIds_.begin(),
                      centerNodeElementIds_.end(), point.elementCenterId) !=
                centerNodeElementIds_.end());
  };
  std::vector<ConstraintId> result;
  for (const auto& item : constraints_)
    if ((item.firstGeometry != kInvalidGeometryId &&
         !geometry.contains(item.firstGeometry)) ||
        (item.secondGeometry != kInvalidGeometryId &&
         !geometry.contains(item.secondGeometry)) ||
        !validPoint(item.firstPoint) || !validPoint(item.secondPoint))
      result.push_back(item.id);
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

void Sketch::beginDeltaJournal() {
  ++deltaJournalBeginCount;
  std::vector<std::vector<std::size_t>> parentDimensionTokens;
  parentDimensionTokens.reserve(deltaJournals_.size());
  for (const auto& active : deltaJournals_)
    parentDimensionTokens.push_back(active.dimensionTokens);
  deltaJournals_.emplace_back();
  auto& journal = deltaJournals_.back();
  journal.parentDimensionTokensBefore = std::move(parentDimensionTokens);
  journal.beforeLineIds = lineIds_;
  journal.beforeCircleIds = circleIds_;
  journal.beforeArcIds = arcIds_;
  journal.beforeBezierIds = bezierIds_;
  journal.beforeConstraintIds.reserve(constraints_.size());
  for (const auto& constraint : constraints_)
    journal.beforeConstraintIds.push_back(constraint.id);
  journal.beforeDimensionCount = dimensions_.size();
  journal.nextDimensionToken = dimensions_.size();
  journal.dimensionTokens.resize(dimensions_.size());
  std::iota(journal.dimensionTokens.begin(), journal.dimensionTokens.end(),
            std::size_t{0});
  journal.beforeNextElementId = nextElementId_;
  journal.beforeNextGeometryId = nextGeometryId_;
  journal.beforeNextConstraintId = nextConstraintId_;
  journal.beforeNextDimensionId = nextDimensionId_;
  journal.beforeSemanticFingerprint = semanticFingerprint();
  journal.beforeInvalidConstraintIds = invalidReferenceConstraintIds();
  journal.beforeLastSolvedFingerprint = lastSolvedFingerprint_;
  journal.beforeHasLastSolvedFingerprint = hasLastSolvedFingerprint_;
  journal.beforeLastSolveConverged = lastSolveConverged_;
  journal.beforeLastSolveViolatedConstraints =
      lastSolveViolatedConstraints_;
  journal.beforeLastSolveMaxNormalizedResidual =
      lastSolveMaxNormalizedResidual_;
  journal.beforeLastSolveUnsupported = lastSolveUnsupported_;
  journal.beforeLastSolveInvalidReferences = lastSolveInvalidReferences_;
  journal.beforeLastSolveResult = lastSolveResult_;
}

void Sketch::clear() {
  if (!deltaJournals_.empty()) {
    for (const auto id : lineIds_) journalCaptureGeometry(id);
    for (const auto id : circleIds_) journalCaptureGeometry(id);
    for (const auto id : arcIds_) journalCaptureGeometry(id);
    for (const auto id : bezierIds_) journalCaptureGeometry(id);
    for (const auto& item : constraints_) journalCaptureConstraint(item.id);
    journalCaptureAllDimensions();
    journalCaptureCenters();
  }
  invalidateStructureIndexes();
  centerNodeElementIds_.clear();
  lines_.clear();
  circles_.clear();
  arcs_.clear();
  beziers_.clear();
  lineIds_.clear();
  circleIds_.clear();
  arcIds_.clear();
  bezierIds_.clear();
  dimensions_.clear();
  for (auto& journal : deltaJournals_) journal.dimensionTokens.clear();
  constraints_.clear();
  widthMm_ = 0.0;
  heightMm_ = 0.0;
}

SketchMutationResult Sketch::setRectangle(double widthMm, double heightMm) {
  if (!std::isfinite(widthMm) || !std::isfinite(heightMm))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  if (widthMm <= 0.0 || heightMm <= 0.0)
    return rejectedMutation(*this, {}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::DegenerateGeometry);
  const double halfWidth = widthMm * 0.5;
  const double halfHeight = heightMm * 0.5;
  if (!std::isfinite(halfWidth) || !std::isfinite(halfHeight))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  if (halfWidth <= 0.0 || halfHeight <= 0.0)
    return rejectedMutation(*this, {}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::DegenerateGeometry);

  const Point bottomLeft{-halfWidth, -halfHeight};
  const Point bottomRight{halfWidth, -halfHeight};
  const Point topRight{halfWidth, halfHeight};
  const Point topLeft{-halfWidth, halfHeight};
  clear();
  const auto elementId = nextElementId_++;
  lines_ = {{bottomLeft, bottomRight, elementId},
            {bottomRight, topRight, elementId},
            {topRight, topLeft, elementId},
            {topLeft, bottomLeft, elementId}};
  lineIds_.clear();
  lineIds_.reserve(lines_.size());
  for (std::size_t index = 0; index < lines_.size(); ++index)
    lineIds_.push_back(nextGeometryId_++);
  for (std::size_t index = 0; index < lines_.size(); ++index)
    journalRecordAddedGeometry(lineIds_[index], GeometryKind::Line, index);
  invalidateStructureIndexes();
  updateBounds();
  return mutationResult(*this, lineIds_, SketchMutationStatus::Applied,
                        SketchMutationDiagnosticCode::None);
}

SketchMutationResult Sketch::addLine(Point start, Point end) {
  if (!finitePoint(start) || !finitePoint(end))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  if (start.xMm == end.xMm && start.yMm == end.yMm)
    return noMutation(*this);
  if (!prospectiveBoundsFinite(*this, {start, end}))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  lines_.push_back({start, end, nextElementId_++});
  lineIds_.push_back(nextGeometryId_++);
  journalRecordAddedGeometry(lineIds_.back(), GeometryKind::Line,
                             lines_.size() - 1);
  invalidateStructureIndexes();
  updateBounds();
  return mutationResult(*this, {lineIds_.back()},
                        SketchMutationStatus::Applied,
                        SketchMutationDiagnosticCode::None);
}

SketchMutationResult Sketch::addLine(Point start, Point end,
                                     std::size_t elementId) {
  if (!finitePoint(start) || !finitePoint(end))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  if (start.xMm == end.xMm && start.yMm == end.yMm)
    return noMutation(*this);
  if (!prospectiveBoundsFinite(*this, {start, end}))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);

  // elementId is a group/composite identifier, not a GeometryId.
  // Serialized rectangles intentionally reuse the same elementId on
  // multiple line primitives.
  if (elementId == 0) {
    return addLine(start, end);
  }

  lines_.push_back({start, end, elementId});
  lineIds_.push_back(nextGeometryId_++);
  journalRecordAddedGeometry(lineIds_.back(), GeometryKind::Line,
                             lines_.size() - 1);
  invalidateStructureIndexes();

  // Prevent subsequently created elements from reusing a restored ID.
  nextElementId_ = std::max(nextElementId_, elementId + 1);
  updateBounds();
  return mutationResult(*this, {lineIds_.back()},
                        SketchMutationStatus::Applied,
                        SketchMutationDiagnosticCode::None);
}

SketchMutationResult Sketch::addRectangle(Point firstCorner,
                                          Point oppositeCorner) {
  if (!finitePoint(firstCorner) || !finitePoint(oppositeCorner))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  if (std::abs(firstCorner.xMm - oppositeCorner.xMm) < 1e-9 ||
      std::abs(firstCorner.yMm - oppositeCorner.yMm) < 1e-9)
    return noMutation(*this);

  const Point second{oppositeCorner.xMm, firstCorner.yMm};
  const Point fourth{firstCorner.xMm, oppositeCorner.yMm};
  if (!prospectiveBoundsFinite(
          *this, {firstCorner, second, oppositeCorner, fourth}))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  const auto elementId = nextElementId_++;

  const GeometryId firstLineId = nextGeometryId_++;
  const GeometryId secondLineId = nextGeometryId_++;
  const GeometryId thirdLineId = nextGeometryId_++;
  const GeometryId fourthLineId = nextGeometryId_++;

  lines_.push_back({firstCorner, second, elementId});
  lineIds_.push_back(firstLineId);
  lines_.push_back({second, oppositeCorner, elementId});
  lineIds_.push_back(secondLineId);
  lines_.push_back({oppositeCorner, fourth, elementId});
  lineIds_.push_back(thirdLineId);
  lines_.push_back({fourth, firstCorner, elementId});
  lineIds_.push_back(fourthLineId);
  journalRecordAddedGeometry(firstLineId, GeometryKind::Line,
                             lines_.size() - 4);
  journalRecordAddedGeometry(secondLineId, GeometryKind::Line,
                             lines_.size() - 3);
  journalRecordAddedGeometry(thirdLineId, GeometryKind::Line,
                             lines_.size() - 2);
  journalRecordAddedGeometry(fourthLineId, GeometryKind::Line,
                             lines_.size() - 1);
  invalidateStructureIndexes();

  const auto addCornerCoincident =
      [this](GeometryId firstId, bool firstStart,
             GeometryId secondId, bool secondStart) {
        Constraint constraint;
        constraint.type = ConstraintType::Coincident;
        constraint.firstPoint = PointReference{firstId, firstStart};
        constraint.secondPoint = PointReference{secondId, secondStart};
        addConstraint(constraint);
      };

  // Four persistent CAD corners.
  addCornerCoincident(firstLineId, false, secondLineId, true);
  addCornerCoincident(secondLineId, false, thirdLineId, true);
  addCornerCoincident(thirdLineId, false, fourthLineId, true);
  addCornerCoincident(fourthLineId, false, firstLineId, true);

  // Minimal non-redundant rectangle orientation system.
  // Opposite sides have equal lengths. Together with the four connected
  // corners and one right angle, this defines a rectangle without explicit
  // Parallel constraints.
  Constraint firstEqual;
  firstEqual.type = ConstraintType::Equal;
  firstEqual.firstGeometry = firstLineId;
  firstEqual.secondGeometry = thirdLineId;
  addConstraint(firstEqual);

  Constraint secondEqual;
  secondEqual.type = ConstraintType::Equal;
  secondEqual.firstGeometry = secondLineId;
  secondEqual.secondGeometry = fourthLineId;
  addConstraint(secondEqual);

  // Keep explicit parallel relationships as well. They are mathematically
  // redundant with the rectangle system, but make the current sequential
  // solver much more stable during interactive dragging.
  Constraint firstParallel;
  firstParallel.type = ConstraintType::Parallel;
  firstParallel.firstGeometry = firstLineId;
  firstParallel.secondGeometry = thirdLineId;
  addConstraint(firstParallel);

  Constraint secondParallel;
  secondParallel.type = ConstraintType::Parallel;
  secondParallel.firstGeometry = secondLineId;
  secondParallel.secondGeometry = fourthLineId;
  addConstraint(secondParallel);

  Constraint perpendicular;
  perpendicular.type = ConstraintType::Perpendicular;
  perpendicular.firstGeometry = firstLineId;
  perpendicular.secondGeometry = secondLineId;
  addConstraint(perpendicular);

  // A standard two-point rectangle is axis-aligned. Keep that absolute
  // orientation while allowing width and height to change independently.
  Constraint horizontal;
  horizontal.type = ConstraintType::Horizontal;
  horizontal.firstGeometry = firstLineId;
  addConstraint(horizontal);

  Constraint vertical;
  vertical.type = ConstraintType::Vertical;
  vertical.firstGeometry = secondLineId;
  addConstraint(vertical);

  updateBounds();
  return mutationResult(
      *this, {firstLineId, secondLineId, thirdLineId, fourthLineId},
      SketchMutationStatus::Applied, SketchMutationDiagnosticCode::None);
}
SketchMutationResult Sketch::addRectangle(Point first, Point second,
                                          Point third, Point fourth) {
  if (!finitePoint(first) || !finitePoint(second) || !finitePoint(third) ||
      !finitePoint(fourth))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  const auto validEdge = [](Point start, Point end) {
    const double dx = end.xMm - start.xMm;
    const double dy = end.yMm - start.yMm;
    return std::isfinite(dx) && std::isfinite(dy) &&
           (dx != 0.0 || dy != 0.0);
  };
  if (!validEdge(first, second) || !validEdge(second, third) ||
      !validEdge(third, fourth) || !validEdge(fourth, first))
    return rejectedMutation(*this, {}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::DegenerateGeometry);
  if (!prospectiveBoundsFinite(*this, {first, second, third, fourth}))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  const auto elementId = nextElementId_++;

  const GeometryId firstLineId = nextGeometryId_++;
  const GeometryId secondLineId = nextGeometryId_++;
  const GeometryId thirdLineId = nextGeometryId_++;
  const GeometryId fourthLineId = nextGeometryId_++;

  lines_.push_back({first, second, elementId});
  lineIds_.push_back(firstLineId);
  lines_.push_back({second, third, elementId});
  lineIds_.push_back(secondLineId);
  lines_.push_back({third, fourth, elementId});
  lineIds_.push_back(thirdLineId);
  lines_.push_back({fourth, first, elementId});
  lineIds_.push_back(fourthLineId);
  journalRecordAddedGeometry(firstLineId, GeometryKind::Line,
                             lines_.size() - 4);
  journalRecordAddedGeometry(secondLineId, GeometryKind::Line,
                             lines_.size() - 3);
  journalRecordAddedGeometry(thirdLineId, GeometryKind::Line,
                             lines_.size() - 2);
  journalRecordAddedGeometry(fourthLineId, GeometryKind::Line,
                             lines_.size() - 1);
  invalidateStructureIndexes();

  const auto addCornerCoincident =
      [this](GeometryId firstId, bool firstStart,
             GeometryId secondId, bool secondStart) {
        Constraint constraint;
        constraint.type = ConstraintType::Coincident;
        constraint.firstPoint = PointReference{firstId, firstStart};
        constraint.secondPoint = PointReference{secondId, secondStart};
        addConstraint(constraint);
      };

  addCornerCoincident(firstLineId, false, secondLineId, true);
  addCornerCoincident(secondLineId, false, thirdLineId, true);
  addCornerCoincident(thirdLineId, false, fourthLineId, true);
  addCornerCoincident(fourthLineId, false, firstLineId, true);

  // Opposite sides have equal lengths. Together with the four connected
  // corners and one right angle, this defines a rectangle without explicit
  // Parallel constraints.
  Constraint firstEqual;
  firstEqual.type = ConstraintType::Equal;
  firstEqual.firstGeometry = firstLineId;
  firstEqual.secondGeometry = thirdLineId;
  addConstraint(firstEqual);

  Constraint secondEqual;
  secondEqual.type = ConstraintType::Equal;
  secondEqual.firstGeometry = secondLineId;
  secondEqual.secondGeometry = fourthLineId;
  addConstraint(secondEqual);

  // Keep explicit parallel relationships as well. They are mathematically
  // redundant with the rectangle system, but make the current sequential
  // solver much more stable during interactive dragging.
  Constraint firstParallel;
  firstParallel.type = ConstraintType::Parallel;
  firstParallel.firstGeometry = firstLineId;
  firstParallel.secondGeometry = thirdLineId;
  addConstraint(firstParallel);

  Constraint secondParallel;
  secondParallel.type = ConstraintType::Parallel;
  secondParallel.firstGeometry = secondLineId;
  secondParallel.secondGeometry = fourthLineId;
  addConstraint(secondParallel);

  Constraint perpendicular;
  perpendicular.type = ConstraintType::Perpendicular;
  perpendicular.firstGeometry = firstLineId;
  perpendicular.secondGeometry = secondLineId;
  addConstraint(perpendicular);

  updateBounds();
  return mutationResult(
      *this, {firstLineId, secondLineId, thirdLineId, fourthLineId},
      SketchMutationStatus::Applied, SketchMutationDiagnosticCode::None);
}
SketchMutationResult Sketch::addCircle(Point center, double radiusMm) {
  if (!finitePoint(center) || !std::isfinite(radiusMm))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  if (radiusMm <= 0.0)
    return rejectedMutation(*this, {}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::DegenerateGeometry);
  if (!prospectiveBoundsFinite(*this, {}, Circle{center, radiusMm}))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  circles_.push_back({center, radiusMm});
  circleIds_.push_back(nextGeometryId_++);
  journalRecordAddedGeometry(circleIds_.back(), GeometryKind::Circle,
                             circles_.size() - 1);
  invalidateStructureIndexes();
  updateBounds();
  return mutationResult(*this, {circleIds_.back()},
                        SketchMutationStatus::Applied,
                        SketchMutationDiagnosticCode::None);
}

void Sketch::addArc(Point center, double radiusMm, double startAngleRad,
                    double sweepAngleRad, bool dashed) {
  constexpr double kTwoPi = 6.28318530717958647692;
  if (!std::isfinite(center.xMm) || !std::isfinite(center.yMm) ||
      !std::isfinite(radiusMm) || !std::isfinite(startAngleRad) ||
      !std::isfinite(sweepAngleRad) || radiusMm <= 0.0 ||
      sweepAngleRad <= 1e-9 || sweepAngleRad >= kTwoPi - 1e-9)
    return;

  const Arc candidate{center, radiusMm, startAngleRad, sweepAngleRad, dashed};
  if (!prospectiveBoundsFinite(*this, {}, std::nullopt, candidate)) return;
  arcs_.push_back(candidate);
  arcIds_.push_back(nextGeometryId_++);
  journalRecordAddedGeometry(arcIds_.back(), GeometryKind::Arc,
                             arcs_.size() - 1);
  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::addBezier(Point start, Point control1, Point control2, Point end,
                       bool dashed) {
  const auto finitePoint = [](Point point) {
    return std::isfinite(point.xMm) && std::isfinite(point.yMm);
  };
  if (!finitePoint(start) || !finitePoint(control1) ||
      !finitePoint(control2) || !finitePoint(end) ||
      (std::hypot(end.xMm - start.xMm, end.yMm - start.yMm) <= 1e-12 &&
       std::hypot(control1.xMm - start.xMm,
                  control1.yMm - start.yMm) <= 1e-12 &&
       std::hypot(control2.xMm - start.xMm,
                  control2.yMm - start.yMm) <= 1e-12))
    return;
  const Bezier candidate{{start, control1, control2, end}, dashed};
  if (!prospectiveBoundsFinite(*this, {}, std::nullopt, std::nullopt,
                               candidate))
    return;
  beziers_.push_back(candidate);
  bezierIds_.push_back(nextGeometryId_++);
  journalRecordAddedGeometry(bezierIds_.back(), GeometryKind::Bezier,
                             beziers_.size() - 1);
  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::removeLine(std::size_t index) {
  // LOCK CONSTRAINT: locked geometry cannot be deleted.
  if (index < lineIds_.size() &&
      isGeometryLocked(lineIds_[index]))
    return;
  if (index >= lines_.size()) return;

  const GeometryId removedId = lineIds_[index];
  journalCaptureComponents({removedId});
  journalCaptureAllDimensions();
  journalCaptureCenters();
  const std::size_t removedElementId = lines_[index].elementId;
  lines_.erase(lines_.begin() + index);
  lineIds_.erase(lineIds_.begin() + index);

  const bool invalidatedElementCenter =
      hasElementCenterNode(removedElementId) &&
      std::count_if(lines_.begin(), lines_.end(),
                    [removedElementId](const Line& line) {
                      return line.elementId == removedElementId;
                    }) != 4;
  if (invalidatedElementCenter)
    std::erase(centerNodeElementIds_, removedElementId);

  std::vector<std::size_t> dimensionsToRemove;
  for (std::size_t dimensionIndex = 0;
       dimensionIndex < dimensions_.size(); ++dimensionIndex) {
    const auto& dimension = dimensions_[dimensionIndex];
    const bool remove = [removedId, invalidatedElementCenter,
                         removedElementId](const Dimension& dimension) {
    if (dimension.kind == DimensionKind::LineLength)
      return dimension.geometryId == removedId;
    if (dimension.kind == DimensionKind::PointDistance ||
        dimension.kind == DimensionKind::PointDistanceX ||
        dimension.kind == DimensionKind::PointDistanceY)
      return dimension.firstPoint.lineId == removedId ||
             dimension.secondPoint.lineId == removedId ||
             (invalidatedElementCenter &&
              (dimension.firstPoint.elementCenterId == removedElementId ||
               dimension.secondPoint.elementCenterId == removedElementId));
    if (dimension.kind == DimensionKind::LineAngle ||
        dimension.kind == DimensionKind::LineDistance)
      return dimension.geometryId == removedId ||
             dimension.secondPoint.lineId == removedId;
    return false;
    }(dimension);
    if (remove) dimensionsToRemove.push_back(dimensionIndex);
  }
  for (auto it = dimensionsToRemove.rbegin();
       it != dimensionsToRemove.rend(); ++it)
    static_cast<void>(removeDimension(*it));

  std::erase_if(constraints_,
                [removedId, invalidatedElementCenter,
                 removedElementId](const Constraint& constraint) {
    return constraint.firstGeometry == removedId ||
           constraint.secondGeometry == removedId ||
           constraint.firstPoint.lineId == removedId ||
           constraint.secondPoint.lineId == removedId ||
           (invalidatedElementCenter &&
            (constraint.firstPoint.elementCenterId == removedElementId ||
             constraint.secondPoint.elementCenterId == removedElementId));
  });

  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::removeCircle(std::size_t index) {
  // LOCK CONSTRAINT: locked geometry cannot be deleted.
  if (index < circleIds_.size() &&
      isGeometryLocked(circleIds_[index]))
    return;
  if (index >= circles_.size()) return;

  const GeometryId removedId = circleIds_[index];
  journalCaptureComponents({removedId});
  journalCaptureAllDimensions();
  circles_.erase(circles_.begin() + index);
  circleIds_.erase(circleIds_.begin() + index);

  std::vector<std::size_t> dimensionsToRemove;
  for (std::size_t dimensionIndex = 0;
       dimensionIndex < dimensions_.size(); ++dimensionIndex) {
    const auto& dimension = dimensions_[dimensionIndex];
    const bool remove = [removedId](const Dimension& dimension) {
    if (dimension.kind == DimensionKind::CircleDiameter ||
        dimension.kind == DimensionKind::CircleRadius)
      return dimension.geometryId == removedId;

    // Point-to-point dimensions may reference a circle center through
    // PointReference::circleId. Remove them together with the circle so no
    // stale point reference survives deletion.
    if (dimension.kind == DimensionKind::PointDistance ||
        dimension.kind == DimensionKind::PointDistanceX ||
        dimension.kind == DimensionKind::PointDistanceY)
      return dimension.firstPoint.circleId == removedId ||
             dimension.secondPoint.circleId == removedId;

    return false;
    }(dimension);
    if (remove) dimensionsToRemove.push_back(dimensionIndex);
  }
  for (auto it = dimensionsToRemove.rbegin();
       it != dimensionsToRemove.rend(); ++it)
    static_cast<void>(removeDimension(*it));

  std::erase_if(constraints_, [removedId](const Constraint& constraint) {
    return constraint.firstGeometry == removedId ||
           constraint.secondGeometry == removedId ||
           constraint.firstPoint.circleId == removedId ||
           constraint.secondPoint.circleId == removedId;
  });

  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::removeArc(std::size_t index) {
  if (index < arcIds_.size() && isGeometryLocked(arcIds_[index]))
    return;
  if (index >= arcs_.size()) return;

  const GeometryId removedId = arcIds_[index];
  journalCaptureComponents({removedId});
  journalCaptureAllDimensions();
  arcs_.erase(arcs_.begin() + index);
  arcIds_.erase(arcIds_.begin() + index);

  std::vector<std::size_t> dimensionsToRemove;
  for (std::size_t dimensionIndex = 0;
       dimensionIndex < dimensions_.size(); ++dimensionIndex) {
    const auto& dimension = dimensions_[dimensionIndex];
    if (dimension.geometryId == removedId ||
        ((dimension.kind == DimensionKind::PointDistance ||
          dimension.kind == DimensionKind::PointDistanceX ||
          dimension.kind == DimensionKind::PointDistanceY) &&
         (dimension.firstPoint.arcId == removedId ||
          dimension.secondPoint.arcId == removedId)))
      dimensionsToRemove.push_back(dimensionIndex);
  }
  for (auto it = dimensionsToRemove.rbegin();
       it != dimensionsToRemove.rend(); ++it)
    static_cast<void>(removeDimension(*it));

  std::erase_if(constraints_, [removedId](const Constraint& constraint) {
    return constraint.firstGeometry == removedId ||
           constraint.secondGeometry == removedId ||
           constraint.firstPoint.arcId == removedId ||
           constraint.secondPoint.arcId == removedId;
  });

  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::removeBezier(std::size_t index) {
  if (index < bezierIds_.size() && isGeometryLocked(bezierIds_[index])) return;
  if (index >= beziers_.size()) return;
  const GeometryId removedId = bezierIds_[index];
  journalCaptureComponents({removedId});
  journalCaptureAllDimensions();
  beziers_.erase(beziers_.begin() + index);
  bezierIds_.erase(bezierIds_.begin() + index);
  std::vector<std::size_t> dimensionsToRemove;
  for (std::size_t slot = 0; slot < dimensions_.size(); ++slot) {
    const auto& dimension = dimensions_[slot];
    if (dimension.geometryId == removedId ||
        dimension.firstPoint.bezierId == removedId ||
        dimension.secondPoint.bezierId == removedId)
      dimensionsToRemove.push_back(slot);
  }
  for (auto it = dimensionsToRemove.rbegin(); it != dimensionsToRemove.rend();
       ++it)
    static_cast<void>(removeDimension(*it));
  std::erase_if(constraints_, [removedId](const Constraint& constraint) {
    return constraint.firstGeometry == removedId ||
           constraint.secondGeometry == removedId ||
           constraint.firstPoint.bezierId == removedId ||
           constraint.secondPoint.bezierId == removedId;
  });
  invalidateStructureIndexes();
  updateBounds();
}

void Sketch::removeElement(std::size_t elementId) {
  // LOCK CONSTRAINT: one Lock freezes the whole CAD element.
  if (isElementLocked(elementId))
    return;
  std::vector<GeometryId> journalSeeds;
  for (std::size_t index = 0; index < lines_.size(); ++index)
    if (lines_[index].elementId == elementId)
      journalSeeds.push_back(lineIds_[index]);
  journalCaptureComponents(journalSeeds);
  journalCaptureAllDimensions();
  journalCaptureCenters();
  std::vector<GeometryId> removedIds;
  for (std::size_t index = lines_.size(); index > 0; --index) {
    const std::size_t current = index - 1;
    if (lines_[current].elementId != elementId) continue;
    removedIds.push_back(lineIds_[current]);
    lines_.erase(lines_.begin() + current);
    lineIds_.erase(lineIds_.begin() + current);
  }

  if (!removedIds.empty()) {
    const auto wasRemoved = [&removedIds](GeometryId id) {
      return std::find(removedIds.begin(), removedIds.end(), id) !=
             removedIds.end();
    };

    std::vector<std::size_t> dimensionsToRemove;
    for (std::size_t index = 0; index < dimensions_.size(); ++index) {
      const auto& dimension = dimensions_[index];
      if (dimension.kind == DimensionKind::LineLength)
        { if (wasRemoved(dimension.geometryId)) dimensionsToRemove.push_back(index); }
      else if (dimension.kind == DimensionKind::PointDistance ||
               dimension.kind == DimensionKind::PointDistanceX ||
               dimension.kind == DimensionKind::PointDistanceY) {
        if (wasRemoved(dimension.firstPoint.lineId) ||
            wasRemoved(dimension.secondPoint.lineId))
          dimensionsToRemove.push_back(index);
      } else if (dimension.kind == DimensionKind::LineAngle ||
                 dimension.kind == DimensionKind::LineDistance) {
        if (wasRemoved(dimension.geometryId) ||
            wasRemoved(dimension.secondPoint.lineId))
          dimensionsToRemove.push_back(index);
      }
    }
    for (auto it = dimensionsToRemove.rbegin();
         it != dimensionsToRemove.rend(); ++it)
      static_cast<void>(removeDimension(*it));

    std::erase_if(constraints_, [&wasRemoved](const Constraint& constraint) {
      return wasRemoved(constraint.firstGeometry) ||
             wasRemoved(constraint.secondGeometry) ||
             wasRemoved(constraint.firstPoint.lineId) ||
             wasRemoved(constraint.secondPoint.lineId);
    });
  }

  // A centered composite element exposes a virtual PointReference identified
  // by elementCenterId rather than by a GeometryId. Clean those references
  // explicitly when the owning element is deleted.
  std::vector<std::size_t> centeredDimensionsToRemove;
  for (std::size_t index = 0; index < dimensions_.size(); ++index)
    if (dimensions_[index].firstPoint.elementCenterId == elementId ||
        dimensions_[index].secondPoint.elementCenterId == elementId)
      centeredDimensionsToRemove.push_back(index);
  for (auto it = centeredDimensionsToRemove.rbegin();
       it != centeredDimensionsToRemove.rend(); ++it)
    static_cast<void>(removeDimension(*it));

  std::erase_if(constraints_, [elementId](const Constraint& constraint) {
    return constraint.firstPoint.elementCenterId == elementId ||
           constraint.secondPoint.elementCenterId == elementId;
  });
  std::erase(centerNodeElementIds_, elementId);
  invalidateStructureIndexes();
  updateBounds();
}

SketchDeletionResult Sketch::removeSelection(
    const std::vector<GeometryId>& geometryIds,
    const std::vector<std::size_t>& elementIds) {
  if (geometryIds.empty() && elementIds.empty())
    return {SketchDeletionStatus::NoChange, {}};

  std::vector<GeometryId> targets;
  targets.reserve(geometryIds.size() + elementIds.size() * 4);
  const auto addTarget = [&targets](GeometryId id) {
    if (std::find(targets.begin(), targets.end(), id) == targets.end())
      targets.push_back(id);
  };
  for (const auto id : geometryIds) {
    if (!geometryLocation(id)) return {SketchDeletionStatus::Stale, {}};
    addTarget(id);
  }
  for (const auto elementId : elementIds) {
    if (elementId == 0) return {SketchDeletionStatus::Stale, {}};
    bool found = false;
    for (std::size_t index = 0; index < lines_.size(); ++index) {
      if (lines_[index].elementId != elementId) continue;
      found = true;
      addTarget(lineIds_[index]);
    }
    if (!found) return {SketchDeletionStatus::Stale, {}};
  }

  if (targets.empty()) return {SketchDeletionStatus::NoChange, {}};
  if (std::any_of(targets.begin(), targets.end(),
                  [this](GeometryId id) { return isGeometryLocked(id); }))
    return {SketchDeletionStatus::Locked, {}};

  for (const auto id : targets) {
    const auto location = geometryLocation(id);
    if (!location) return {SketchDeletionStatus::Stale, {}};
    switch (location->kind) {
      case GeometryKind::Line:
        removeLine(location->index);
        break;
      case GeometryKind::Circle:
        removeCircle(location->index);
        break;
      case GeometryKind::Arc:
        removeArc(location->index);
        break;
      case GeometryKind::Bezier:
        removeBezier(location->index);
        break;
    }
  }
  return {SketchDeletionStatus::Removed, std::move(targets)};
}

void Sketch::markElementCenterNode(std::size_t elementId) {
  if (elementId == 0 || hasElementCenterNode(elementId))
    return;

  std::size_t lineCount = 0;
  for (const auto& line : lines_) {
    if (line.elementId == elementId)
      ++lineCount;
  }

  // Current rectangle elements are exactly four perimeter lines.
  if (lineCount == 4)
    journalCaptureCenters();
  if (lineCount == 4)
    centerNodeElementIds_.push_back(elementId);
  invalidateStructureIndexes();
}

bool Sketch::hasElementCenterNode(std::size_t elementId) const noexcept {
  return std::find(centerNodeElementIds_.begin(),
                   centerNodeElementIds_.end(),
                   elementId) != centerNodeElementIds_.end();
}

std::optional<Point> Sketch::elementCenterPoint(
    std::size_t elementId) const noexcept {
  if (!hasElementCenterNode(elementId))
    return std::nullopt;

  std::vector<const Line*> elementLines;
  elementLines.reserve(4);

  for (const auto& line : lines_) {
    if (line.elementId == elementId)
      elementLines.push_back(&line);
  }

  if (elementLines.size() != 4)
    return std::nullopt;

  // Average the four perimeter starts. Rectangle lines are stored in
  // perimeter order, so these are exactly the four rectangle vertices.
  Point center{};

  for (const auto* line : elementLines) {
    center.xMm += line->start.xMm;
    center.yMm += line->start.yMm;
  }

  center.xMm *= 0.25;
  center.yMm *= 0.25;
  return center;
}

const std::vector<std::size_t>&
Sketch::centerNodeElementIds() const noexcept {
  return centerNodeElementIds_;
}
SketchMutationResult Sketch::translateElement(std::size_t elementId,
                                              double dxMm, double dyMm) {
  if (!std::isfinite(dxMm) || !std::isfinite(dyMm))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  // LOCK CONSTRAINT: direct drag may never move a locked element.
  if (isElementLocked(elementId))
    return rejectedMutation(*this, {}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::LockedGeometry);
  if (dxMm == 0.0 && dyMm == 0.0) return noMutation(*this);

  std::vector<GeometryId> movedIds;
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (lines_[index].elementId == elementId)
      movedIds.push_back(lineIds_[index]);
  }
  if (movedIds.empty())
    return rejectedMutation(
        *this, {}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  const auto isMoved = [&movedIds](GeometryId id) {
    return std::find(movedIds.begin(), movedIds.end(), id) != movedIds.end();
  };

  // If the selected element has Coincident constraints, remember the
  // connected endpoint(s) outside the selected element. They should follow
  // the drag instead of being left behind.
  std::vector<PointReference> connectedExternalPoints;
  for (const auto& constraint : constraints_) {
    if (!participatesInConstraintSystem(constraint) ||
        constraint.type != ConstraintType::Coincident)
      continue;

    const bool firstMoved = isMoved(constraint.firstPoint.lineId);
    const bool secondMoved = isMoved(constraint.secondPoint.lineId);
    if (firstMoved == secondMoved) continue;

    const PointReference external =
        firstMoved ? constraint.secondPoint : constraint.firstPoint;

    const auto duplicate = std::find_if(
        connectedExternalPoints.begin(), connectedExternalPoints.end(),
        [external](const PointReference& item) {
          if (external.circleId != kInvalidGeometryId ||
              item.circleId != kInvalidGeometryId)
            return external.circleId != kInvalidGeometryId &&
                   item.circleId == external.circleId;

          return item.lineId == external.lineId &&
                 item.start == external.start;
        });
    if (duplicate == connectedExternalPoints.end())
      connectedExternalPoints.push_back(external);
  }

  // Capture explicit external references before the selected element moves.
  struct ConnectedPointMove {
    PointReference reference;
    Point newPoint;
  };
  std::vector<ConnectedPointMove> connectedMoves;
  connectedMoves.reserve(connectedExternalPoints.size());
  for (const auto reference : connectedExternalPoints) {
    const auto point = referencedPoint(reference);
    if (!point) continue;
    connectedMoves.push_back(
        {reference, {point->xMm + dxMm, point->yMm + dyMm}});
  }

  // Points constrained to a moved carrier line belong to that moving frame.
  // Move them by the same delta before the final projection pass.
  std::vector<PointReference> pointOnLineFollowers;

  const auto samePointReference =
      [](PointReference first, PointReference second) {
        if (first.circleId != kInvalidGeometryId ||
            second.circleId != kInvalidGeometryId) {
          return first.circleId != kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  for (const auto& constraint : constraints_) {
    if (!participatesInConstraintSystem(constraint) ||
        constraint.type != ConstraintType::PointOnLine ||
        !isMoved(constraint.firstGeometry))
      continue;

    const auto duplicate = std::any_of(
        pointOnLineFollowers.begin(),
        pointOnLineFollowers.end(),
        [&constraint, &samePointReference](PointReference item) {
          return samePointReference(item, constraint.secondPoint);
        });

    if (!duplicate)
      pointOnLineFollowers.push_back(constraint.secondPoint);
  }

  std::vector<PointReference> movedReferences;
  movedReferences.reserve(connectedExternalPoints.size() +
                          pointOnLineFollowers.size());
  const auto addActuallyMovedReference = [&movedReferences](
                                           PointReference reference) {
    if (reference.circleId == kInvalidGeometryId &&
        reference.lineId == kInvalidGeometryId)
      return;
    movedReferences.push_back(reference);
  };
  for (const auto reference : connectedExternalPoints)
    addActuallyMovedReference(reference);
  for (const auto reference : pointOnLineFollowers)
    addActuallyMovedReference(reference);
  if (!translatedSelectionBoundsFinite(
          *this, {elementId}, {}, {}, {}, movedReferences, dxMm, dyMm))
    return rejectedMutation(*this, movedIds,
                            SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);

  const auto journalDepthBefore = deltaJournalDepth();
  const bool ownsJournal = journalDepthBefore == 0;
  if (ownsJournal) beginDeltaJournal();
  journalCaptureComponents(movedIds);

  for (auto& line : lines_) {
    if (line.elementId != elementId) continue;
    line.start.xMm += dxMm;
    line.start.yMm += dyMm;
    line.end.xMm += dxMm;
    line.end.yMm += dyMm;
  }

  for (const auto follower : pointOnLineFollowers) {
    if (follower.circleId != kInvalidGeometryId) {
      const auto circle = circleIndex(follower.circleId);
      if (!circle) continue;
      circles_[*circle].center.xMm += dxMm;
      circles_[*circle].center.yMm += dyMm;
      continue;
    }

    const auto followerIndex = lineIndex(follower.lineId);
    if (!followerIndex || isMoved(follower.lineId)) continue;

    Point& followerPoint =
        follower.start ? lines_[*followerIndex].start
                       : lines_[*followerIndex].end;
    followerPoint.xMm += dxMm;
    followerPoint.yMm += dyMm;
  }

  // Move only the explicitly referenced neighbour. Equal raw coordinates do
  // not extend this relationship to additional geometry.
  for (const auto& move : connectedMoves) {
    if (move.reference.circleId != kInvalidGeometryId) {
      const auto circle = circleIndex(move.reference.circleId);
      if (circle) circles_[*circle].center = move.newPoint;
      continue;
    }
    const auto line = lineIndex(move.reference.lineId);
    if (!line || isMoved(move.reference.lineId)) continue;
    if (move.reference.start)
      lines_[*line].start = move.newPoint;
    else
      lines_[*line].end = move.newPoint;
  }

  // Re-apply all active constraints after interactive geometry movement.
  const auto mutation = mutationFromSolve(
      *this, movedIds,
      SketchSolver::solveStableComponent(
          *this, movedIds, 16, kInvalidConstraintId, true));
  if (!mutation.committed()) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return mutation;
  }
  updateBounds();
  if (ownsJournal) static_cast<void>(finishDeltaJournal());
  return mutation;
}

SketchMutationResult Sketch::translateSelection(
    const std::vector<std::size_t>& elementIds,
    const std::vector<GeometryId>& circleIds,
    const std::vector<GeometryId>& arcIds,
    const std::vector<GeometryId>& bezierIds,
    double dxMm, double dyMm) {
  if (!std::isfinite(dxMm) || !std::isfinite(dyMm))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  // LOCK CONSTRAINT: mixed selections do not partially move.
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (std::find(elementIds.begin(), elementIds.end(),
                  lines_[index].elementId) != elementIds.end() &&
        isGeometryLocked(lineIds_[index]))
      return rejectedMutation(*this, {lineIds_[index]},
                              SketchMutationStatus::Conflicting,
                              SketchMutationDiagnosticCode::LockedGeometry);
  }
  for (const auto id : circleIds) {
    if (!circleIndex(id))
      return rejectedMutation(
          *this, {id}, SketchMutationStatus::InvalidReference,
          SketchMutationDiagnosticCode::InvalidGeometryReference);
    if (isGeometryLocked(id))
      return rejectedMutation(*this, {id}, SketchMutationStatus::Conflicting,
                              SketchMutationDiagnosticCode::LockedGeometry);
  }
  for (const auto id : arcIds) {
    if (!arcIndex(id))
      return rejectedMutation(
          *this, {id}, SketchMutationStatus::InvalidReference,
          SketchMutationDiagnosticCode::InvalidGeometryReference);
    if (isGeometryLocked(id))
      return rejectedMutation(*this, {id}, SketchMutationStatus::Conflicting,
                              SketchMutationDiagnosticCode::LockedGeometry);
  }
  for (const auto id : bezierIds) {
    if (!bezierIndex(id))
      return rejectedMutation(
          *this, {id}, SketchMutationStatus::InvalidReference,
          SketchMutationDiagnosticCode::InvalidGeometryReference);
    if (isGeometryLocked(id))
      return rejectedMutation(*this, {id}, SketchMutationStatus::Conflicting,
                              SketchMutationDiagnosticCode::LockedGeometry);
  }
  for (const auto elementId : elementIds)
    if (std::none_of(lines_.begin(), lines_.end(), [elementId](const Line& line) {
          return line.elementId == elementId;
        }))
      return rejectedMutation(
          *this, {}, SketchMutationStatus::InvalidReference,
          SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (dxMm == 0.0 && dyMm == 0.0) return noMutation(*this);
  if (elementIds.empty() && circleIds.empty() && arcIds.empty() &&
      bezierIds.empty())
    return rejectedMutation(
        *this, {}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  std::vector<GeometryId> journalSeeds = circleIds;
  journalSeeds.insert(journalSeeds.end(), arcIds.begin(), arcIds.end());
  journalSeeds.insert(journalSeeds.end(), bezierIds.begin(), bezierIds.end());
  for (std::size_t index = 0; index < lines_.size(); ++index)
    if (std::find(elementIds.begin(), elementIds.end(),
                  lines_[index].elementId) != elementIds.end())
      journalSeeds.push_back(lineIds_[index]);
  const auto elementSelected =
      [&elementIds](std::size_t elementId) {
        return std::find(
                   elementIds.begin(),
                   elementIds.end(),
                   elementId) !=
               elementIds.end();
      };

  const auto circleSelected =
      [&circleIds](GeometryId id) {
        return std::find(
                   circleIds.begin(),
                   circleIds.end(),
                   id) !=
               circleIds.end();
      };

  const auto arcSelected =
      [&arcIds](GeometryId id) {
        return std::find(arcIds.begin(), arcIds.end(), id) != arcIds.end();
      };

  const auto bezierSelected = [&bezierIds](GeometryId id) {
    return std::find(bezierIds.begin(), bezierIds.end(), id) !=
           bezierIds.end();
  };

  // CRASH-FREE 04: COMPLETE POINTREFERENCE IDENTITY IN GROUP DRAG
  const auto sameReference =
      [](PointReference first,
         PointReference second) {
        if (first.elementCenterId != 0 ||
            second.elementCenterId != 0) {
          return first.elementCenterId != 0 &&
                 second.elementCenterId != 0 &&
                 first.elementCenterId ==
                     second.elementCenterId;
        }

        if (first.circleId != kInvalidGeometryId ||
            second.circleId != kInvalidGeometryId) {
          return first.circleId != kInvalidGeometryId &&
                 second.circleId != kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        if (first.arcId != kInvalidGeometryId ||
            second.arcId != kInvalidGeometryId) {
          return first.arcId != kInvalidGeometryId &&
                 second.arcId != kInvalidGeometryId &&
                 first.arcId == second.arcId &&
                 first.start == second.start;
        }

        if (first.bezierId != kInvalidGeometryId ||
            second.bezierId != kInvalidGeometryId)
          return first.bezierId != kInvalidGeometryId &&
                 second.bezierId != kInvalidGeometryId &&
                 first.bezierId == second.bezierId &&
                 first.bezierPoint == second.bezierPoint;

        if (first.lineId == kInvalidGeometryId ||
            second.lineId == kInvalidGeometryId)
          return false;

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  std::vector<PointReference> movedReferences;

  const auto addReference =
      [&movedReferences,
       &sameReference](PointReference reference) {
        const bool exists =
            std::any_of(
                movedReferences.begin(),
                movedReferences.end(),
                [reference,
                 &sameReference](
                    PointReference item) {
                  return sameReference(
                      item,
                      reference);
                });

        if (!exists)
          movedReferences.push_back(reference);
      };

  for (std::size_t index = 0;
       index < lines_.size();
       ++index) {
    if (!elementSelected(
            lines_[index].elementId))
      continue;

    const GeometryId id =
        lineIds_[index];

    if (id == kInvalidGeometryId)
      continue;

    addReference(PointReference{id, true});
    addReference(PointReference{id, false});

    if (hasElementCenterNode(
            lines_[index].elementId)) {
      PointReference center;
      center.elementCenterId =
          lines_[index].elementId;
      addReference(center);
    }
  }

  for (std::size_t index = 0;
       index < circles_.size();
       ++index) {
    const GeometryId id =
        circleIds_[index];

    if (!circleSelected(id))
      continue;

    PointReference center;
    center.circleId = id;
    addReference(center);
  }

  for (std::size_t index = 0; index < arcs_.size(); ++index) {
    const GeometryId id = arcIds_[index];
    if (!arcSelected(id)) continue;
    PointReference endpoint;
    endpoint.arcId = id;
    endpoint.start = true;
    addReference(endpoint);
    endpoint.start = false;
    addReference(endpoint);
  }
  for (std::size_t index = 0; index < beziers_.size(); ++index) {
    const GeometryId id = bezierIds_[index];
    if (!bezierSelected(id)) continue;
    for (std::uint8_t pointIndex = 0; pointIndex < 4; ++pointIndex) {
      PointReference reference;
      reference.bezierId = id;
      reference.bezierPoint = pointIndex;
      addReference(reference);
    }
  }

  bool expanded = true;

  while (expanded) {
    expanded = false;

    for (const auto& constraint :
         constraints_) {
      if (!participatesInConstraintSystem(constraint) ||
          constraint.type !=
          ConstraintType::Coincident)
        continue;

      const auto first =
          constraint.firstPoint;
      const auto second =
          constraint.secondPoint;

      if (!referencedPoint(first) ||
          !referencedPoint(second))
        continue;

      const bool hasFirst =
          std::any_of(
              movedReferences.begin(),
              movedReferences.end(),
              [first,
               &sameReference](
                  PointReference item) {
                return sameReference(
                    item,
                    first);
              });

      const bool hasSecond =
          std::any_of(
              movedReferences.begin(),
              movedReferences.end(),
              [second,
               &sameReference](
                  PointReference item) {
                return sameReference(
                    item,
                    second);
              });

      if (hasFirst && !hasSecond) {
        addReference(second);
        expanded = true;
      }
      else if (hasSecond && !hasFirst) {
        addReference(first);
        expanded = true;
      }
    }
  }

  // LOCK CONSTRAINT: Coincident expansion reaches a locked reference.
  // Reject the complete drag rather than moving the lock or breaking the link.
  if (std::any_of(
          movedReferences.begin(), movedReferences.end(),
          [this](PointReference reference) {
            return isPointReferenceLocked(reference);
          })) {
    return rejectedMutation(*this, journalSeeds,
                            SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::LockedGeometry);
  }

  if (!translatedSelectionBoundsFinite(*this, elementIds, circleIds, arcIds,
                                       bezierIds, movedReferences, dxMm,
                                       dyMm))
    return rejectedMutation(*this, journalSeeds,
                            SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);

  const auto journalDepthBefore = deltaJournalDepth();
  const bool ownsJournal = journalDepthBefore == 0;
  if (ownsJournal) beginDeltaJournal();
  journalCaptureComponents(journalSeeds);

  const auto referenceMoves =
      [&movedReferences,
       &sameReference](
          PointReference reference) {
        return std::any_of(
            movedReferences.begin(),
            movedReferences.end(),
            [reference,
             &sameReference](
                PointReference item) {
              return sameReference(
                  item,
                  reference);
            });
      };

  std::vector<std::size_t>
      centerMovedElements;

  for (const auto reference :
       movedReferences) {
    if (reference.elementCenterId == 0)
      continue;

    if (std::find(
            centerMovedElements.begin(),
            centerMovedElements.end(),
            reference.elementCenterId) ==
        centerMovedElements.end())
      centerMovedElements.push_back(
          reference.elementCenterId);
  }

  const auto centerElementMoves =
      [&centerMovedElements](
          std::size_t elementId) {
        return std::find(
                   centerMovedElements.begin(),
                   centerMovedElements.end(),
                   elementId) !=
               centerMovedElements.end();
      };

  for (std::size_t index = 0;
       index < lines_.size();
       ++index) {
    auto& line = lines_[index];
    const GeometryId id =
        lineIds_[index];

    if (elementSelected(line.elementId) ||
        centerElementMoves(line.elementId)) {
      line.start.xMm += dxMm;
      line.start.yMm += dyMm;
      line.end.xMm += dxMm;
      line.end.yMm += dyMm;
      continue;
    }

    if (referenceMoves(
            PointReference{id, true})) {
      line.start.xMm += dxMm;
      line.start.yMm += dyMm;
    }

    if (referenceMoves(
            PointReference{id, false})) {
      line.end.xMm += dxMm;
      line.end.yMm += dyMm;
    }
  }

  for (std::size_t index = 0;
       index < circles_.size();
       ++index) {
    const GeometryId id =
        circleIds_[index];

    PointReference center;
    center.circleId = id;

    if (circleSelected(id) ||
        referenceMoves(center)) {
      circles_[index].center.xMm += dxMm;
      circles_[index].center.yMm += dyMm;
    }
  }


  for (std::size_t index = 0; index < arcs_.size(); ++index) {
    const GeometryId id = arcIds_[index];
    PointReference start;
    start.arcId = id;
    start.start = true;
    PointReference end = start;
    end.start = false;
    const bool moveStart = referenceMoves(start);
    const bool moveEnd = referenceMoves(end);

    if (arcSelected(id) || (moveStart && moveEnd)) {
      arcs_[index].center.xMm += dxMm;
      arcs_[index].center.yMm += dyMm;
    } else if (moveStart) {
      const Point current = arcStartPoint(arcs_[index]);
      (void)moveArcEndpointReshape(
          arcs_[index], true,
          {current.xMm + dxMm, current.yMm + dyMm});
    } else if (moveEnd) {
      const Point current = arcEndPoint(arcs_[index]);
      (void)moveArcEndpointReshape(
          arcs_[index], false,
          {current.xMm + dxMm, current.yMm + dyMm});
    }
  }

  for (std::size_t index = 0; index < beziers_.size(); ++index) {
    const GeometryId id = bezierIds_[index];
    if (bezierSelected(id)) {
      for (auto& point : beziers_[index].points) {
        point.xMm += dxMm;
        point.yMm += dyMm;
      }
      continue;
    }
    for (std::uint8_t pointIndex = 0; pointIndex < 4; ++pointIndex) {
      PointReference reference;
      reference.bezierId = id;
      reference.bezierPoint = pointIndex;
      if (!referenceMoves(reference)) continue;
      beziers_[index].points[pointIndex].xMm += dxMm;
      beziers_[index].points[pointIndex].yMm += dyMm;
    }
  }

  std::vector<GeometryId> dirtyIds;
  for (std::size_t index = 0; index < lines_.size(); ++index)
    if (elementSelected(lines_[index].elementId)) dirtyIds.push_back(lineIds_[index]);
  dirtyIds.insert(dirtyIds.end(), circleIds.begin(), circleIds.end());
  dirtyIds.insert(dirtyIds.end(), arcIds.begin(), arcIds.end());
  dirtyIds.insert(dirtyIds.end(), bezierIds.begin(), bezierIds.end());
  const auto mutation = mutationFromSolve(
      *this, dirtyIds,
      SketchSolver::solveStableComponent(
          *this, dirtyIds, 16, kInvalidConstraintId, true));
  if (!mutation.committed()) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return mutation;
  }
  updateBounds();
  if (ownsJournal) static_cast<void>(finishDeltaJournal());
  return mutation;
}

SketchMutationResult Sketch::translateLinesByIds(
    const std::vector<GeometryId>& lineIds, double dxMm, double dyMm) {
  if (!std::isfinite(dxMm) || !std::isfinite(dyMm))
    return rejectedMutation(*this, lineIds,
                            SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  if (lineIds.empty())
    return rejectedMutation(
        *this, {}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (dxMm == 0.0 && dyMm == 0.0) return noMutation(*this, lineIds);
  for (const auto id : lineIds)
    if (!lineIndex(id))
      return rejectedMutation(
          *this, lineIds, SketchMutationStatus::InvalidReference,
          SketchMutationDiagnosticCode::InvalidGeometryReference);
    else if (isGeometryLocked(id))
      return rejectedMutation(*this, lineIds,
                              SketchMutationStatus::Conflicting,
                              SketchMutationDiagnosticCode::LockedGeometry);
  if (!translatedGeometryBoundsFinite(*this, lineIds, dxMm, dyMm))
    return rejectedMutation(*this, lineIds,
                            SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  const auto journalDepthBefore = deltaJournalDepth();
  const bool ownsJournal = journalDepthBefore == 0;
  if (ownsJournal) beginDeltaJournal();
  journalCaptureComponents(lineIds);

  bool changed = false;
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (std::find(lineIds.begin(), lineIds.end(), lineIds_[index]) ==
        lineIds.end())
      continue;
    lines_[index].start.xMm += dxMm;
    lines_[index].start.yMm += dyMm;
    lines_[index].end.xMm += dxMm;
    lines_[index].end.yMm += dyMm;
    changed = true;
  }
  if (!changed) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return noMutation(*this, lineIds);
  }
  const auto mutation = mutationFromSolve(
      *this, lineIds,
      SketchSolver::solveStableComponent(
          *this, lineIds, 16, kInvalidConstraintId, true));
  if (!mutation.committed()) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return mutation;
  }
  updateBounds();
  if (ownsJournal) static_cast<void>(finishDeltaJournal());
  return mutation;
}

void Sketch::setElementDashed(std::size_t elementId, bool dashed) {
  // Locked/reference geometry cannot change its construction style.
  if (isElementLocked(elementId))
    return;
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (lines_[index].elementId == elementId) {
      journalCaptureGeometry(lineIds_[index]);
      lines_[index].dashed = dashed;
    }
  }
}

void Sketch::setLineDashedById(GeometryId id, bool dashed) {
  const auto index = lineIndex(id);
  if (!index || isGeometryLocked(id)) return;
  journalCaptureGeometry(id);
  lines_[*index].dashed = dashed;
}

void Sketch::setCircleDashed(std::size_t index, bool dashed) {
  if (index < circleIds_.size() &&
      isGeometryLocked(circleIds_[index]))
    return;
  if (index < circles_.size()) {
    journalCaptureGeometry(circleIds_[index]);
    circles_[index].dashed = dashed;
  }
}

SketchMutationResult Sketch::translateCircle(std::size_t index, double dxMm,
                                             double dyMm) {
  if (index >= circleIds_.size())
    return rejectedMutation(
        *this, {}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  return translateCircleById(circleIds_[index], dxMm, dyMm);
}

void Sketch::setCircleDashedById(GeometryId id, bool dashed) {
  const auto index = circleIndex(id);
  if (index) setCircleDashed(*index, dashed);
}

void Sketch::setArcDashedById(GeometryId id, bool dashed) {
  const auto index = arcIndex(id);
  if (!index || isGeometryLocked(id)) return;
  journalCaptureGeometry(id);
  arcs_[*index].dashed = dashed;
}

void Sketch::setBezierDashedById(GeometryId id, bool dashed) {
  const auto index = bezierIndex(id);
  if (!index || isGeometryLocked(id)) return;
  journalCaptureGeometry(id);
  beziers_[*index].dashed = dashed;
}

SketchMutationResult Sketch::translateCircleById(GeometryId id, double dxMm,
                                                 double dyMm) {
  if (id == kInvalidGeometryId || !circleIndex(id))
    return rejectedMutation(
        *this, {id}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (!std::isfinite(dxMm) || !std::isfinite(dyMm) ||
      !translatedGeometryBoundsFinite(*this, {id}, dxMm, dyMm))
    return rejectedMutation(*this, {id}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);

  PointReference centerReference;
  centerReference.circleId = id;

  // Move the circle center together with every Coincident-connected
  // endpoint/center. This also re-runs the active solver afterwards.
  return translatePoint(centerReference, dxMm, dyMm);
}

SketchMutationResult Sketch::translateArcById(GeometryId id, double dxMm,
                                              double dyMm) {
  if (!std::isfinite(dxMm) || !std::isfinite(dyMm))
    return rejectedMutation(*this, {id}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  const auto index = arcIndex(id);
  if (!index)
    return rejectedMutation(
        *this, {id}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (isGeometryLocked(id))
    return rejectedMutation(*this, {id}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::LockedGeometry);
  if (dxMm == 0.0 && dyMm == 0.0) return noMutation(*this, {id});
  if (!translatedGeometryBoundsFinite(*this, {id}, dxMm, dyMm))
    return rejectedMutation(*this, {id}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  const auto journalDepthBefore = deltaJournalDepth();
  const bool ownsJournal = journalDepthBefore == 0;
  if (ownsJournal) beginDeltaJournal();
  journalCaptureComponents({id});
  arcs_[*index].center.xMm += dxMm;
  arcs_[*index].center.yMm += dyMm;
  const auto mutation = mutationFromSolve(
      *this, {id}, SketchSolver::solveStableComponent(
                       *this, {id}, 16, kInvalidConstraintId, true));
  if (!mutation.committed()) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return mutation;
  }
  updateBounds();
  if (ownsJournal) static_cast<void>(finishDeltaJournal());
  return mutation;
}

SketchMutationResult Sketch::translateBezierById(GeometryId id, double dxMm,
                                                 double dyMm) {
  if (!std::isfinite(dxMm) || !std::isfinite(dyMm))
    return rejectedMutation(*this, {id}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  const auto index = bezierIndex(id);
  if (!index)
    return rejectedMutation(
        *this, {id}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (isGeometryLocked(id))
    return rejectedMutation(*this, {id}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::LockedGeometry);
  if (dxMm == 0.0 && dyMm == 0.0) return noMutation(*this, {id});
  if (!translatedGeometryBoundsFinite(*this, {id}, dxMm, dyMm))
    return rejectedMutation(*this, {id}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  const auto journalDepthBefore = deltaJournalDepth();
  const bool ownsJournal = journalDepthBefore == 0;
  if (ownsJournal) beginDeltaJournal();
  journalCaptureComponents({id});
  for (auto& point : beziers_[*index].points) {
    point.xMm += dxMm;
    point.yMm += dyMm;
  }
  const auto mutation = mutationFromSolve(
      *this, {id}, SketchSolver::solveStableComponent(
                       *this, {id}, 16, kInvalidConstraintId, true));
  if (!mutation.committed()) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return mutation;
  }
  updateBounds();
  if (ownsJournal) static_cast<void>(finishDeltaJournal());
  return mutation;
}

SketchMutationResult Sketch::moveArcEndpointReshapeById(
    GeometryId id, bool start, Point target) {
  const auto index = arcIndex(id);
  if (!index)
    return rejectedMutation(
        *this, {id}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (isGeometryLocked(id))
    return rejectedMutation(*this, {id}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::LockedGeometry);
  if (!std::isfinite(target.xMm) || !std::isfinite(target.yMm))
    return rejectedMutation(*this, {id}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  Arc candidate = arcs_[*index];
  if (!moveArcEndpointReshape(candidate, start, target))
    return rejectedMutation(*this, {id}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::DegenerateGeometry);
  if (!replacementArcBoundsFinite(*this, *index, candidate))
    return rejectedMutation(*this, {id}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  const auto journalDepthBefore = deltaJournalDepth();
  const bool ownsJournal = journalDepthBefore == 0;
  if (ownsJournal) beginDeltaJournal();
  journalCaptureComponents({id});
  arcs_[*index] = candidate;
  const auto mutation = mutationFromSolve(
      *this, {id}, SketchSolver::solveStableComponent(
                       *this, {id}, 16, kInvalidConstraintId, true));
  if (!mutation.committed()) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return mutation;
  }
  updateBounds();
  if (ownsJournal) static_cast<void>(finishDeltaJournal());
  return mutation;
}

GeometryId Sketch::lineId(std::size_t index) const noexcept {
  return index < lineIds_.size() ? lineIds_[index] : kInvalidGeometryId;
}

GeometryId Sketch::circleId(std::size_t index) const noexcept {
  return index < circleIds_.size() ? circleIds_[index] : kInvalidGeometryId;
}

GeometryId Sketch::arcId(std::size_t index) const noexcept {
  return index < arcIds_.size() ? arcIds_[index] : kInvalidGeometryId;
}

GeometryId Sketch::bezierId(std::size_t index) const noexcept {
  return index < bezierIds_.size() ? bezierIds_[index] : kInvalidGeometryId;
}

std::optional<std::size_t> Sketch::lineIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = geometryIndex_.find(id);
    if (found != geometryIndex_.end() &&
        found->second.kind == GeometryKind::Line)
      return found->second.index;
  } catch (...) {
    const auto found = std::find(lineIds_.begin(), lineIds_.end(), id);
    if (found != lineIds_.end())
      return static_cast<std::size_t>(std::distance(lineIds_.begin(), found));
  }
  return std::nullopt;
}

std::optional<std::size_t> Sketch::circleIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = geometryIndex_.find(id);
    if (found != geometryIndex_.end() &&
        found->second.kind == GeometryKind::Circle)
      return found->second.index;
  } catch (...) {
    const auto found = std::find(circleIds_.begin(), circleIds_.end(), id);
    if (found != circleIds_.end())
      return static_cast<std::size_t>(std::distance(circleIds_.begin(), found));
  }
  return std::nullopt;
}

std::optional<std::size_t> Sketch::arcIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = geometryIndex_.find(id);
    if (found != geometryIndex_.end() &&
        found->second.kind == GeometryKind::Arc)
      return found->second.index;
  } catch (...) {
    const auto found = std::find(arcIds_.begin(), arcIds_.end(), id);
    if (found != arcIds_.end())
      return static_cast<std::size_t>(std::distance(arcIds_.begin(), found));
  }
  return std::nullopt;
}

std::optional<std::size_t> Sketch::bezierIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = geometryIndex_.find(id);
    if (found != geometryIndex_.end() &&
        found->second.kind == GeometryKind::Bezier)
      return found->second.index;
  } catch (...) {
    const auto found = std::find(bezierIds_.begin(), bezierIds_.end(), id);
    if (found != bezierIds_.end())
      return static_cast<std::size_t>(std::distance(bezierIds_.begin(), found));
  }
  return std::nullopt;
}

std::optional<GeometryLocation> Sketch::geometryLocation(
    GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = geometryIndex_.find(id);
    if (found != geometryIndex_.end()) return found->second;
  } catch (...) {
  }
  return std::nullopt;
}

std::optional<std::size_t> Sketch::constraintIndex(ConstraintId id) const noexcept {
  if (id == kInvalidConstraintId) return std::nullopt;
  try {
    rebuildIdentityIndexes();
    const auto found = constraintIndex_.find(id);
    if (found != constraintIndex_.end()) return found->second;
  } catch (...) {
    const auto found = std::find_if(
        constraints_.begin(), constraints_.end(),
        [id](const Constraint& item) { return item.id == id; });
    if (found != constraints_.end())
      return static_cast<std::size_t>(std::distance(constraints_.begin(), found));
  }
  return std::nullopt;
}

std::optional<std::size_t> Sketch::dimensionIndex(DimensionId id) const noexcept {
  if (id == kInvalidDimensionId) return std::nullopt;
  const auto found = std::find_if(
      dimensions_.begin(), dimensions_.end(),
      [id](const Dimension& item) { return item.id == id; });
  if (found == dimensions_.end()) return std::nullopt;
  return static_cast<std::size_t>(std::distance(dimensions_.begin(), found));
}

void Sketch::invalidateStructureIndexes() noexcept {
  structureIndexesDirty_ = true;
  connectivityDirty_ = true;
  hasLastSolvedFingerprint_ = false;
}

void Sketch::rebuildIdentityIndexes() const {
  if (!structureIndexesDirty_) return;
  geometryIndex_.clear();
  constraintIndex_.clear();
  const std::size_t geometryCount =
      lineIds_.size() + circleIds_.size() + arcIds_.size() + bezierIds_.size();
  geometryIndex_.reserve(geometryCount);
  constraintIndex_.reserve(constraints_.size());

  const auto addGeometry = [this](GeometryId id, GeometryKind kind,
                                  std::size_t index) {
    if (id == kInvalidGeometryId) return;
    geometryIndex_.insert_or_assign(id, GeometryLocation{kind, index});
  };
  for (std::size_t i = 0; i < lineIds_.size(); ++i)
    addGeometry(lineIds_[i], GeometryKind::Line, i);
  for (std::size_t i = 0; i < circleIds_.size(); ++i)
    addGeometry(circleIds_[i], GeometryKind::Circle, i);
  for (std::size_t i = 0; i < arcIds_.size(); ++i)
    addGeometry(arcIds_[i], GeometryKind::Arc, i);
  for (std::size_t i = 0; i < bezierIds_.size(); ++i)
    addGeometry(bezierIds_[i], GeometryKind::Bezier, i);
  for (std::size_t index = 0; index < constraints_.size(); ++index)
    constraintIndex_.insert_or_assign(constraints_[index].id, index);
  structureIndexesDirty_ = false;
  connectivityDirty_ = true;
}

void Sketch::rebuildStructureIndexes() const {
  rebuildIdentityIndexes();
  if (!connectivityDirty_) return;
  connectivity_.clear();
  geometryConstraints_.clear();
  const std::size_t geometryCount = geometryIndex_.size();
  connectivity_.reserve(geometryCount);
  geometryConstraints_.reserve(geometryCount);
  for (const auto& [id, location] : geometryIndex_) {
    static_cast<void>(location);
    connectivity_.try_emplace(id);
    geometryConstraints_.try_emplace(id);
  }

  const auto connect = [this](GeometryId first, GeometryId second) {
    if (first == second || first == kInvalidGeometryId ||
        second == kInvalidGeometryId || !geometryIndex_.contains(first) ||
        !geometryIndex_.contains(second))
      return;
    connectivity_[first].push_back(second);
    connectivity_[second].push_back(first);
  };

  std::unordered_map<std::size_t, std::vector<GeometryId>> elementMembers;
  for (std::size_t i = 0; i < lines_.size() && i < lineIds_.size(); ++i)
    elementMembers[lines_[i].elementId].push_back(lineIds_[i]);
  for (const auto& [elementId, members] : elementMembers) {
    static_cast<void>(elementId);
    for (std::size_t i = 1; i < members.size(); ++i)
      connect(members.front(), members[i]);
  }

  const auto appendReference = [&elementMembers](
                                   std::vector<GeometryId>& ids,
                                   const PointReference& reference) {
    if (reference.lineId != kInvalidGeometryId) ids.push_back(reference.lineId);
    if (reference.circleId != kInvalidGeometryId) ids.push_back(reference.circleId);
    if (reference.arcId != kInvalidGeometryId) ids.push_back(reference.arcId);
    if (reference.bezierId != kInvalidGeometryId)
      ids.push_back(reference.bezierId);
    if (reference.elementCenterId != 0) {
      const auto found = elementMembers.find(reference.elementCenterId);
      if (found != elementMembers.end())
        ids.insert(ids.end(), found->second.begin(), found->second.end());
    }
    // The origin is an immutable anchor, deliberately not a graph vertex:
    // two otherwise-independent constraints to the datum stay independent.
  };

  for (std::size_t index = 0; index < constraints_.size(); ++index) {
    const auto& constraint = constraints_[index];
    std::vector<GeometryId> ids;
    if (constraint.firstGeometry != kInvalidGeometryId)
      ids.push_back(constraint.firstGeometry);
    if (constraint.secondGeometry != kInvalidGeometryId)
      ids.push_back(constraint.secondGeometry);
    appendReference(ids, constraint.firstPoint);
    appendReference(ids, constraint.secondPoint);
    std::erase_if(ids, [this](GeometryId id) {
      return id == kInvalidGeometryId || !geometryIndex_.contains(id);
    });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const auto id : ids)
      geometryConstraints_[id].push_back(constraint.id);
    if (participatesInConstraintSystem(constraint))
      for (std::size_t i = 1; i < ids.size(); ++i)
        connect(ids.front(), ids[i]);
  }

  for (auto& [id, adjacent] : connectivity_) {
    static_cast<void>(id);
    std::sort(adjacent.begin(), adjacent.end());
    adjacent.erase(std::unique(adjacent.begin(), adjacent.end()), adjacent.end());
  }
  connectivityDirty_ = false;
}

ConstraintComponent Sketch::connectedComponent(
    const std::vector<GeometryId>& seeds) const {
  rebuildStructureIndexes();
  ConstraintComponent result;
  std::vector<GeometryId> orderedSeeds = seeds;
  std::sort(orderedSeeds.begin(), orderedSeeds.end());
  orderedSeeds.erase(std::unique(orderedSeeds.begin(), orderedSeeds.end()),
                     orderedSeeds.end());
  std::queue<GeometryId> pending;
  std::unordered_map<GeometryId, bool> visited;
  std::unordered_set<ConstraintId> componentConstraints;
  for (const auto seed : orderedSeeds) {
    if (!geometryIndex_.contains(seed) || visited.contains(seed)) continue;
    visited.emplace(seed, true);
    pending.push(seed);
  }
  while (!pending.empty()) {
    const auto id = pending.front();
    pending.pop();
    result.geometryIds.push_back(id);
    if (const auto found = geometryConstraints_.find(id);
        found != geometryConstraints_.end())
      componentConstraints.insert(found->second.begin(), found->second.end());
    if (const auto found = connectivity_.find(id); found != connectivity_.end()) {
      for (const auto adjacent : found->second) {
        if (visited.emplace(adjacent, true).second) pending.push(adjacent);
      }
    }
  }
  std::sort(result.geometryIds.begin(), result.geometryIds.end());
  // Preserve persistence order for delta journals. The equation graph sorts
  // IDs independently, so solve status and pose never depend on this vector.
  for (const auto& constraint : constraints_)
    if (componentConstraints.contains(constraint.id))
      result.constraintIds.push_back(constraint.id);
  return result;
}

std::vector<ConstraintComponent> Sketch::constraintComponents() const {
  rebuildStructureIndexes();
  std::vector<GeometryId> ids;
  ids.reserve(geometryIndex_.size());
  for (const auto& [id, location] : geometryIndex_) {
    static_cast<void>(location);
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  std::unordered_map<GeometryId, bool> consumed;
  std::vector<ConstraintComponent> result;
  for (const auto id : ids) {
    if (consumed.contains(id)) continue;
    auto component = connectedComponent({id});
    for (const auto member : component.geometryIds) consumed.emplace(member, true);
    result.push_back(std::move(component));
  }
  return result;
}

std::size_t Sketch::ownedBytes() const noexcept {
  std::size_t bytes = lines_.capacity() * sizeof(Line) +
         circles_.capacity() * sizeof(Circle) +
         arcs_.capacity() * sizeof(Arc) +
         beziers_.capacity() * sizeof(Bezier) +
         lineIds_.capacity() * sizeof(GeometryId) +
         circleIds_.capacity() * sizeof(GeometryId) +
         arcIds_.capacity() * sizeof(GeometryId) +
         bezierIds_.capacity() * sizeof(GeometryId) +
         dimensions_.capacity() * sizeof(Dimension) +
         constraints_.capacity() * sizeof(Constraint) +
         centerNodeElementIds_.capacity() * sizeof(std::size_t);
  const auto mapBytes = [](const auto& map) {
    return map.bucket_count() * sizeof(void*) +
           map.size() * (sizeof(typename std::decay_t<decltype(map)>::value_type) +
                         2 * sizeof(void*));
  };
  bytes += mapBytes(geometryIndex_) + mapBytes(constraintIndex_) +
           mapBytes(connectivity_) + mapBytes(geometryConstraints_);
  for (const auto& [id, adjacent] : connectivity_) {
    static_cast<void>(id);
    bytes += adjacent.capacity() * sizeof(GeometryId);
  }
  for (const auto& [id, constraints] : geometryConstraints_) {
    static_cast<void>(id);
    bytes += constraints.capacity() * sizeof(ConstraintId);
  }
  return bytes;
}

std::size_t Sketch::ownedAllocationBlocks() const noexcept {
  std::size_t blocks = 0;
  const auto vectorBlock = [&blocks](const auto& values) {
    if (values.capacity() != 0) ++blocks;
  };
  vectorBlock(lines_); vectorBlock(circles_); vectorBlock(arcs_);
  vectorBlock(beziers_);
  vectorBlock(lineIds_); vectorBlock(circleIds_); vectorBlock(arcIds_);
  vectorBlock(bezierIds_);
  vectorBlock(dimensions_); vectorBlock(constraints_);
  vectorBlock(centerNodeElementIds_);
  const auto mapBlocks = [&blocks](const auto& map) {
    if (map.bucket_count() != 0) ++blocks;
    blocks += map.size();
  };
  mapBlocks(geometryIndex_); mapBlocks(constraintIndex_);
  mapBlocks(connectivity_); mapBlocks(geometryConstraints_);
  for (const auto& [id, adjacent] : connectivity_) {
    static_cast<void>(id); vectorBlock(adjacent);
  }
  for (const auto& [id, constraints] : geometryConstraints_) {
    static_cast<void>(id); vectorBlock(constraints);
  }
  return blocks;
}

void Sketch::journalCaptureGeometry(GeometryId id) {
  if (deltaJournals_.empty() || id == kInvalidGeometryId) return;
  const auto already = [id](const auto& records) {
    return std::any_of(records.begin(), records.end(),
                       [id](const auto& item) { return item.id == id; });
  };
  const auto location = geometryLocation(id);
  if (!location) return;
  for (auto& journal : deltaJournals_) {
    if (already(journal.lines) || already(journal.circles) ||
        already(journal.arcs) || already(journal.beziers)) continue;
    switch (location->kind) {
      case GeometryKind::Line:
        journal.lines.push_back({id, location->index, lines_[location->index]});
        break;
      case GeometryKind::Circle:
        journal.circles.push_back(
            {id, location->index, circles_[location->index]});
        break;
      case GeometryKind::Arc:
        journal.arcs.push_back({id, location->index, arcs_[location->index]});
        break;
      case GeometryKind::Bezier:
        journal.beziers.push_back(
            {id, location->index, beziers_[location->index]});
        break;
    }
  }
}

void Sketch::journalCapturePoint(PointReference reference) {
  std::vector<GeometryId> seeds;
  if (reference.lineId != kInvalidGeometryId) seeds.push_back(reference.lineId);
  if (reference.circleId != kInvalidGeometryId)
    seeds.push_back(reference.circleId);
  if (reference.arcId != kInvalidGeometryId) seeds.push_back(reference.arcId);
  if (reference.bezierId != kInvalidGeometryId)
    seeds.push_back(reference.bezierId);
  if (reference.elementCenterId != 0)
    for (std::size_t index = 0; index < lines_.size(); ++index)
      if (lines_[index].elementId == reference.elementCenterId)
        seeds.push_back(lineIds_[index]);
  journalCaptureComponents(seeds);
}

void Sketch::journalCaptureConstraint(ConstraintId id) {
  if (deltaJournals_.empty() || id == kInvalidConstraintId) return;
  const auto index = constraintIndex(id);
  if (!index) return;
  for (auto& journal : deltaJournals_) {
    if (std::any_of(journal.constraints.begin(), journal.constraints.end(),
                    [id](const auto& item) { return item.id == id; }))
      continue;
    journal.constraints.push_back({id, *index, constraints_[*index]});
  }
}

void Sketch::journalCaptureComponents(const std::vector<GeometryId>& seeds) {
  if (deltaJournals_.empty()) return;
  for (const auto seed : seeds) {
    if (seed == kInvalidGeometryId) continue;
    const auto component = connectedComponent({seed});
    if (component.geometryIds.empty()) {
      journalCaptureGeometry(seed);
      continue;
    }
    for (const auto id : component.geometryIds) journalCaptureGeometry(id);
    for (const auto id : component.constraintIds) journalCaptureConstraint(id);
  }
}

void Sketch::journalCaptureDimension(std::size_t index) {
  if (deltaJournals_.empty() || index >= dimensions_.size()) return;
  for (auto& journal : deltaJournals_) {
    if (index >= journal.dimensionTokens.size()) continue;
    const std::size_t token = journal.dimensionTokens[index];
    if (std::any_of(journal.dimensions.begin(), journal.dimensions.end(),
                    [token](const auto& item) {
                      return item.token == token;
                    }))
      continue;
    const bool existedBefore = token < journal.beforeDimensionCount;
    journal.dimensions.push_back(
        {token, existedBefore ? std::optional<std::size_t>{token}
                              : std::nullopt,
         existedBefore ? std::optional<Dimension>{dimensions_[index]}
                       : std::nullopt});
  }
}

void Sketch::journalCaptureAllDimensions() {
  if (deltaJournals_.empty()) return;
  for (std::size_t index = 0; index < dimensions_.size(); ++index)
    journalCaptureDimension(index);
}

void Sketch::journalCaptureCenters() {
  for (auto& journal : deltaJournals_)
    if (!journal.centersBefore) journal.centersBefore = centerNodeElementIds_;
}

void Sketch::journalRecordAddedGeometry(GeometryId id, GeometryKind kind,
                                        std::size_t index) {
  for (auto& journal : deltaJournals_) {
    switch (kind) {
      case GeometryKind::Line:
        journal.lines.push_back({id, index, std::nullopt}); break;
      case GeometryKind::Circle:
        journal.circles.push_back({id, index, std::nullopt}); break;
      case GeometryKind::Arc:
        journal.arcs.push_back({id, index, std::nullopt}); break;
      case GeometryKind::Bezier:
        journal.beziers.push_back({id, index, std::nullopt}); break;
    }
  }
}

void Sketch::journalRecordAddedConstraint(ConstraintId id,
                                          std::size_t index) {
  for (auto& journal : deltaJournals_)
    journal.constraints.push_back({id, index, std::nullopt});
}

void Sketch::journalRecordAddedDimension(std::size_t index) {
  for (auto& journal : deltaJournals_) {
    const std::size_t token = journal.nextDimensionToken++;
    const std::size_t insertion = std::min(index, journal.dimensionTokens.size());
    journal.dimensionTokens.insert(
        journal.dimensionTokens.begin() + static_cast<std::ptrdiff_t>(insertion),
        token);
    journal.dimensions.push_back({token, std::nullopt, std::nullopt});
  }
}

SketchDelta Sketch::finishDeltaJournal() {
  if (deltaJournals_.empty()) return {};
  DeltaJournalState journal = std::move(deltaJournals_.back());
  deltaJournals_.pop_back();
  SketchDelta delta;
  const auto lineEqual = [](const Line& a, const Line& b) {
    return a.start.xMm == b.start.xMm && a.start.yMm == b.start.yMm &&
           a.end.xMm == b.end.xMm && a.end.yMm == b.end.yMm &&
           a.elementId == b.elementId && a.dashed == b.dashed;
  };
  const auto circleEqual = [](const Circle& a, const Circle& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.dashed == b.dashed;
  };
  const auto arcEqual = [](const Arc& a, const Arc& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.startAngleRad == b.startAngleRad &&
           a.sweepAngleRad == b.sweepAngleRad && a.dashed == b.dashed;
  };
  const auto bezierEqual = [](const Bezier& a, const Bezier& b) {
    return a.points == b.points && a.dashed == b.dashed;
  };
  for (const auto& item : journal.lines) {
    const auto afterIndex = lineIndex(item.id);
    const auto beforeFound = std::find(journal.beforeLineIds.begin(),
                                       journal.beforeLineIds.end(), item.id);
    const auto beforeIndex = beforeFound == journal.beforeLineIds.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.beforeLineIds.begin(), beforeFound))};
    std::optional<Line> after;
    if (afterIndex) after = lines_[*afterIndex];
    if (!item.before && !after) continue;
    if (item.before && after && lineEqual(*item.before, *after)) continue;
    const auto oldIndex = beforeIndex.value_or(afterIndex.value_or(0));
    const auto finalIndex = afterIndex.value_or(oldIndex);
    delta.lines.push_back({oldIndex, finalIndex, item.before, after});
    delta.lineIds.push_back({oldIndex, finalIndex,
                             item.before ? std::optional<GeometryId>{item.id}
                                         : std::nullopt,
                             after ? std::optional<GeometryId>{item.id}
                                   : std::nullopt});
  }
  for (const auto& item : journal.circles) {
    const auto afterIndex = circleIndex(item.id);
    const auto beforeFound = std::find(journal.beforeCircleIds.begin(),
                                       journal.beforeCircleIds.end(), item.id);
    const auto beforeIndex = beforeFound == journal.beforeCircleIds.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.beforeCircleIds.begin(), beforeFound))};
    std::optional<Circle> after;
    if (afterIndex) after = circles_[*afterIndex];
    if (!item.before && !after) continue;
    if (item.before && after && circleEqual(*item.before, *after)) continue;
    const auto oldIndex = beforeIndex.value_or(afterIndex.value_or(0));
    const auto finalIndex = afterIndex.value_or(oldIndex);
    delta.circles.push_back({oldIndex, finalIndex, item.before, after});
    delta.circleIds.push_back({oldIndex, finalIndex,
                               item.before ? std::optional<GeometryId>{item.id}
                                           : std::nullopt,
                               after ? std::optional<GeometryId>{item.id}
                                     : std::nullopt});
  }
  for (const auto& item : journal.arcs) {
    const auto afterIndex = arcIndex(item.id);
    const auto beforeFound = std::find(journal.beforeArcIds.begin(),
                                       journal.beforeArcIds.end(), item.id);
    const auto beforeIndex = beforeFound == journal.beforeArcIds.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.beforeArcIds.begin(), beforeFound))};
    std::optional<Arc> after;
    if (afterIndex) after = arcs_[*afterIndex];
    if (!item.before && !after) continue;
    if (item.before && after && arcEqual(*item.before, *after)) continue;
    const auto oldIndex = beforeIndex.value_or(afterIndex.value_or(0));
    const auto finalIndex = afterIndex.value_or(oldIndex);
    delta.arcs.push_back({oldIndex, finalIndex, item.before, after});
    delta.arcIds.push_back({oldIndex, finalIndex,
                            item.before ? std::optional<GeometryId>{item.id}
                                        : std::nullopt,
                            after ? std::optional<GeometryId>{item.id}
                                  : std::nullopt});
  }
  for (const auto& item : journal.beziers) {
    const auto afterIndex = bezierIndex(item.id);
    const auto beforeFound = std::find(journal.beforeBezierIds.begin(),
                                       journal.beforeBezierIds.end(), item.id);
    const auto beforeIndex = beforeFound == journal.beforeBezierIds.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.beforeBezierIds.begin(), beforeFound))};
    std::optional<Bezier> after;
    if (afterIndex) after = beziers_[*afterIndex];
    if (!item.before && !after) continue;
    if (item.before && after && bezierEqual(*item.before, *after)) continue;
    const auto oldIndex = beforeIndex.value_or(afterIndex.value_or(0));
    const auto finalIndex = afterIndex.value_or(oldIndex);
    delta.beziers.push_back({oldIndex, finalIndex, item.before, after});
    delta.bezierIds.push_back({oldIndex, finalIndex,
                               item.before ? std::optional<GeometryId>{item.id}
                                           : std::nullopt,
                               after ? std::optional<GeometryId>{item.id}
                                     : std::nullopt});
  }
  for (const auto& item : journal.constraints) {
    const auto afterIndex = constraintIndex(item.id);
    const auto beforeFound = std::find(journal.beforeConstraintIds.begin(),
                                       journal.beforeConstraintIds.end(), item.id);
    const auto beforeIndex = beforeFound == journal.beforeConstraintIds.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.beforeConstraintIds.begin(), beforeFound))};
    std::optional<Constraint> after;
    if (afterIndex) after = constraints_[*afterIndex];
    if (!item.before && !after) continue;
    const auto pointEqual = [](const PointReference& a,
                               const PointReference& b) {
      return a.lineId == b.lineId && a.start == b.start &&
             a.circleId == b.circleId &&
             a.elementCenterId == b.elementCenterId && a.arcId == b.arcId &&
             a.origin == b.origin && a.bezierId == b.bezierId &&
             a.bezierPoint == b.bezierPoint;
    };
    if (item.before && after && item.before->id == after->id &&
        item.before->mode == after->mode &&
        item.before->type == after->type &&
        item.before->firstGeometry == after->firstGeometry &&
        item.before->secondGeometry == after->secondGeometry &&
        pointEqual(item.before->firstPoint, after->firstPoint) &&
        pointEqual(item.before->secondPoint, after->secondPoint) &&
        item.before->value == after->value)
      continue;
    const auto oldIndex = beforeIndex.value_or(afterIndex.value_or(0));
    delta.constraints.push_back({oldIndex,
                                 afterIndex.value_or(oldIndex),
                                 item.before, after});
  }
  for (const auto& item : journal.dimensions) {
    std::optional<Dimension> after;
    const auto afterFound = std::find(journal.dimensionTokens.begin(),
                                      journal.dimensionTokens.end(), item.token);
    const auto afterIndex = afterFound == journal.dimensionTokens.end()
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{static_cast<std::size_t>(
              std::distance(journal.dimensionTokens.begin(), afterFound))};
    if (afterIndex && *afterIndex < dimensions_.size())
      after = dimensions_[*afterIndex];
    if (!item.before && !after) continue;
    const auto pointEqual = [](const PointReference& a,
                               const PointReference& b) {
      return a.lineId == b.lineId && a.start == b.start &&
             a.circleId == b.circleId &&
             a.elementCenterId == b.elementCenterId && a.arcId == b.arcId &&
             a.origin == b.origin && a.bezierId == b.bezierId &&
             a.bezierPoint == b.bezierPoint;
    };
    if (item.before && after && item.before->id == after->id &&
        item.before->constraintId == after->constraintId &&
        item.before->offsetMm == after->offsetMm &&
        item.before->angleRad == after->angleRad) {
      const bool linked = item.before->constraintId != kInvalidConstraintId;
      if (linked ||
          (item.before->kind == after->kind &&
           item.before->geometryId == after->geometryId &&
           pointEqual(item.before->firstPoint, after->firstPoint) &&
           pointEqual(item.before->secondPoint, after->secondPoint) &&
           item.before->valueMm == after->valueMm))
        continue;
    }
    const auto oldIndex = item.beforeIndex.value_or(afterIndex.value_or(0));
    delta.dimensions.push_back(
        {oldIndex, afterIndex.value_or(oldIndex), item.before, after});
  }
  if (journal.centersBefore) {
    for (std::size_t index = 0; index < journal.centersBefore->size(); ++index) {
      const auto value = (*journal.centersBefore)[index];
      const auto found = std::find(centerNodeElementIds_.begin(),
                                   centerNodeElementIds_.end(), value);
      if (found == centerNodeElementIds_.end())
        delta.centerNodeElementIds.push_back(
            {index, index, value, std::nullopt});
    }
    for (std::size_t index = 0; index < centerNodeElementIds_.size(); ++index) {
      const auto value = centerNodeElementIds_[index];
      const auto found = std::find(journal.centersBefore->begin(),
                                   journal.centersBefore->end(), value);
      if (found == journal.centersBefore->end())
        delta.centerNodeElementIds.push_back(
            {index, index, std::nullopt, value});
    }
  }
  delta.beforeNextElementId = journal.beforeNextElementId;
  delta.afterNextElementId = nextElementId_;
  delta.beforeNextGeometryId = journal.beforeNextGeometryId;
  delta.afterNextGeometryId = nextGeometryId_;
  delta.beforeNextConstraintId = journal.beforeNextConstraintId;
  delta.afterNextConstraintId = nextConstraintId_;
  delta.beforeNextDimensionId = journal.beforeNextDimensionId;
  delta.afterNextDimensionId = nextDimensionId_;
  delta.beforeSemanticFingerprint = journal.beforeSemanticFingerprint;
  delta.afterSemanticFingerprint = semanticFingerprint();
  delta.beforeInvalidConstraintIds =
      std::move(journal.beforeInvalidConstraintIds);
  delta.afterInvalidConstraintIds = invalidReferenceConstraintIds();
  const auto vectorBytes = [](const auto& values) {
    using Value = typename std::decay_t<decltype(values)>::value_type;
    return values.capacity() * sizeof(Value);
  };
  delta.retainedBytes = vectorBytes(delta.lines) + vectorBytes(delta.circles) +
      vectorBytes(delta.arcs) + vectorBytes(delta.beziers) +
      vectorBytes(delta.lineIds) +
      vectorBytes(delta.circleIds) + vectorBytes(delta.arcIds) +
      vectorBytes(delta.bezierIds) +
      vectorBytes(delta.dimensions) + vectorBytes(delta.constraints) +
      vectorBytes(delta.centerNodeElementIds) +
      vectorBytes(delta.beforeInvalidConstraintIds) +
      vectorBytes(delta.afterInvalidConstraintIds);
  return delta;
}

SketchDelta Sketch::cancelDeltaJournal() {
  if (deltaJournals_.empty()) return {};
  const auto& journal = deltaJournals_.back();
  const auto parentDimensionTokens = journal.parentDimensionTokensBefore;
  const auto lastSolvedFingerprint = journal.beforeLastSolvedFingerprint;
  const bool hasLastSolvedFingerprint = journal.beforeHasLastSolvedFingerprint;
  const bool lastSolveConverged = journal.beforeLastSolveConverged;
  const auto lastSolveViolatedConstraints =
      journal.beforeLastSolveViolatedConstraints;
  const double lastSolveMaxNormalizedResidual =
      journal.beforeLastSolveMaxNormalizedResidual;
  const auto lastSolveUnsupported = journal.beforeLastSolveUnsupported;
  const auto lastSolveInvalidReferences =
      journal.beforeLastSolveInvalidReferences;
  const auto lastSolveResult = journal.beforeLastSolveResult;
  auto delta = finishDeltaJournal();
  if (!delta.empty() && !applyDelta(delta, false))
    throw std::logic_error("Sketch delta journal rollback failed");
  lastSolvedFingerprint_ = lastSolvedFingerprint;
  hasLastSolvedFingerprint_ = hasLastSolvedFingerprint;
  lastSolveConverged_ = lastSolveConverged;
  lastSolveViolatedConstraints_ = lastSolveViolatedConstraints;
  lastSolveMaxNormalizedResidual_ = lastSolveMaxNormalizedResidual;
  lastSolveUnsupported_ = lastSolveUnsupported;
  lastSolveInvalidReferences_ = lastSolveInvalidReferences;
  lastSolveResult_ = lastSolveResult;
  if (parentDimensionTokens.size() != deltaJournals_.size())
    throw std::logic_error("Sketch delta journal parent mismatch");
  for (std::size_t index = 0; index < deltaJournals_.size(); ++index)
    deltaJournals_[index].dimensionTokens = parentDimensionTokens[index];
  return delta;
}

bool SketchDelta::empty() const noexcept {
  return lines.empty() && circles.empty() && arcs.empty() && beziers.empty() &&
         lineIds.empty() && circleIds.empty() && arcIds.empty() &&
         bezierIds.empty() && dimensions.empty() &&
         constraints.empty() && centerNodeElementIds.empty() &&
         beforeNextElementId == afterNextElementId &&
         beforeNextGeometryId == afterNextGeometryId &&
         beforeNextConstraintId == afterNextConstraintId &&
         beforeNextDimensionId == afterNextDimensionId;
}

SketchDelta Sketch::makeDelta(const Sketch& before, const Sketch& after) {
  SketchDelta delta;
  const auto pointEqual = [](PointReference a, PointReference b) {
    return a.lineId == b.lineId && a.start == b.start &&
           a.circleId == b.circleId && a.elementCenterId == b.elementCenterId &&
           a.arcId == b.arcId && a.origin == b.origin &&
           a.bezierId == b.bezierId &&
           a.bezierPoint == b.bezierPoint;
  };
  const auto append = [&delta]<typename T>(
      const std::vector<T>& oldValues, const std::vector<T>& newValues,
      std::vector<IndexedValueDelta<T>>& output, auto equal) {
    const std::size_t count = std::max(oldValues.size(), newValues.size());
    for (std::size_t i = 0; i < count; ++i) {
      const bool hasOld = i < oldValues.size();
      const bool hasNew = i < newValues.size();
      if (hasOld && hasNew && equal(oldValues[i], newValues[i])) continue;
      IndexedValueDelta<T> item;
      item.beforeIndex = i;
      item.afterIndex = i;
      if (hasOld) item.before = oldValues[i];
      if (hasNew) item.after = newValues[i];
      output.push_back(std::move(item));
    }
  };
  append(before.lines_, after.lines_, delta.lines, [](const Line& a, const Line& b) {
    return a.start.xMm == b.start.xMm && a.start.yMm == b.start.yMm &&
           a.end.xMm == b.end.xMm && a.end.yMm == b.end.yMm &&
           a.elementId == b.elementId && a.dashed == b.dashed;
  });
  append(before.circles_, after.circles_, delta.circles,
         [](const Circle& a, const Circle& b) {
           return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
                  a.radiusMm == b.radiusMm && a.dashed == b.dashed;
         });
  append(before.arcs_, after.arcs_, delta.arcs, [](const Arc& a, const Arc& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.startAngleRad == b.startAngleRad &&
           a.sweepAngleRad == b.sweepAngleRad && a.dashed == b.dashed;
  });
  append(before.beziers_, after.beziers_, delta.beziers,
         [](const Bezier& a, const Bezier& b) {
           return a.points == b.points && a.dashed == b.dashed;
         });
  const auto sameId = [](auto a, auto b) { return a == b; };
  append(before.lineIds_, after.lineIds_, delta.lineIds, sameId);
  append(before.circleIds_, after.circleIds_, delta.circleIds, sameId);
  append(before.arcIds_, after.arcIds_, delta.arcIds, sameId);
  append(before.bezierIds_, after.bezierIds_, delta.bezierIds, sameId);
  append(before.centerNodeElementIds_, after.centerNodeElementIds_,
         delta.centerNodeElementIds, sameId);
  append(before.dimensions_, after.dimensions_, delta.dimensions,
         [&pointEqual](const Dimension& a, const Dimension& b) {
           if (a.id != b.id || a.constraintId != b.constraintId ||
               a.offsetMm != b.offsetMm || a.angleRad != b.angleRad)
             return false;
           if (a.constraintId != kInvalidConstraintId) return true;
           return a.kind == b.kind && a.geometryId == b.geometryId &&
                  pointEqual(a.firstPoint, b.firstPoint) &&
                  pointEqual(a.secondPoint, b.secondPoint) &&
                  a.valueMm == b.valueMm;
         });
  append(before.constraints_, after.constraints_, delta.constraints,
         [&pointEqual](const Constraint& a, const Constraint& b) {
           return a.id == b.id && a.mode == b.mode && a.type == b.type &&
                  a.firstGeometry == b.firstGeometry &&
                  a.secondGeometry == b.secondGeometry &&
                  pointEqual(a.firstPoint, b.firstPoint) &&
                  pointEqual(a.secondPoint, b.secondPoint) && a.value == b.value;
         });
  delta.beforeNextElementId = before.nextElementId_;
  delta.afterNextElementId = after.nextElementId_;
  delta.beforeNextGeometryId = before.nextGeometryId_;
  delta.afterNextGeometryId = after.nextGeometryId_;
  delta.beforeNextConstraintId = before.nextConstraintId_;
  delta.afterNextConstraintId = after.nextConstraintId_;
  delta.beforeNextDimensionId = before.nextDimensionId_;
  delta.afterNextDimensionId = after.nextDimensionId_;
  delta.beforeSemanticFingerprint = before.semanticFingerprint();
  delta.afterSemanticFingerprint = after.semanticFingerprint();
  delta.beforeInvalidConstraintIds = before.invalidReferenceConstraintIds();
  delta.afterInvalidConstraintIds = after.invalidReferenceConstraintIds();
  const auto vectorBytes = [](const auto& values) {
    using Value = typename std::decay_t<decltype(values)>::value_type;
    return values.capacity() * sizeof(Value);
  };
  delta.retainedBytes = vectorBytes(delta.lines) +
                        vectorBytes(delta.circles) +
                        vectorBytes(delta.arcs) +
                        vectorBytes(delta.beziers) +
                        vectorBytes(delta.lineIds) +
                        vectorBytes(delta.circleIds) +
                        vectorBytes(delta.arcIds) +
                        vectorBytes(delta.bezierIds) +
                        vectorBytes(delta.dimensions) +
                        vectorBytes(delta.constraints) +
                        vectorBytes(delta.centerNodeElementIds) +
                        vectorBytes(delta.beforeInvalidConstraintIds) +
                        vectorBytes(delta.afterInvalidConstraintIds);
  return delta;
}

bool Sketch::applyDelta(const SketchDelta& delta, bool forward) {
  // Build every affected container off to the side.  Validation and all
  // allocations finish before the first swap, which gives Undo/Redo a strong
  // transaction guarantee without retaining a full Sketch checkpoint.
  const auto expectedSourceFingerprint =
      forward ? delta.beforeSemanticFingerprint
              : delta.afterSemanticFingerprint;
  const auto sourceNextElement =
      forward ? delta.beforeNextElementId : delta.afterNextElementId;
  const auto sourceNextGeometry =
      forward ? delta.beforeNextGeometryId : delta.afterNextGeometryId;
  const auto sourceNextConstraint =
      forward ? delta.beforeNextConstraintId : delta.afterNextConstraintId;
  const auto sourceNextDimension =
      forward ? delta.beforeNextDimensionId : delta.afterNextDimensionId;
  if (nextElementId_ != sourceNextElement ||
      nextGeometryId_ != sourceNextGeometry ||
      nextConstraintId_ != sourceNextConstraint ||
      nextDimensionId_ != sourceNextDimension ||
      semanticFingerprint() != expectedSourceFingerprint)
    return false;

  const auto plan = [forward]<typename T>(
      const std::vector<T>& current,
      const std::vector<IndexedValueDelta<T>>& changes, auto equal,
      std::optional<std::vector<T>>& result) {
    if (changes.empty()) return true;

    std::vector<std::size_t> sourceIndices;
    std::vector<std::size_t> targetIndices;
    std::size_t removalCount = 0;
    std::size_t insertionCount = 0;
    sourceIndices.reserve(changes.size());
    targetIndices.reserve(changes.size());
    for (const auto& item : changes) {
      const auto& source = forward ? item.before : item.after;
      const auto& desired = forward ? item.after : item.before;
      const std::size_t sourceIndex =
          forward ? item.beforeIndex : item.afterIndex;
      const std::size_t targetIndex =
          forward ? item.afterIndex : item.beforeIndex;
      if (source) {
        if (sourceIndex >= current.size() ||
            !equal(current[sourceIndex], *source))
          return false;
        sourceIndices.push_back(sourceIndex);
      }
      if (desired) targetIndices.push_back(targetIndex);
      if (source && !desired) ++removalCount;
      if (!source && desired) ++insertionCount;
      if (!source && !desired) return false;
    }
    std::sort(sourceIndices.begin(), sourceIndices.end());
    if (std::adjacent_find(sourceIndices.begin(), sourceIndices.end()) !=
        sourceIndices.end())
      return false;
    std::sort(targetIndices.begin(), targetIndices.end());
    if (std::adjacent_find(targetIndices.begin(), targetIndices.end()) !=
        targetIndices.end())
      return false;
    if (removalCount > current.size()) return false;
    const std::size_t finalSize =
        current.size() - removalCount + insertionCount;
    if (!targetIndices.empty() && targetIndices.back() >= finalSize)
      return false;

    std::vector<T> planned = current;
    // Replacements address the untouched source layout.
    for (const auto& item : changes) {
      const auto& source = forward ? item.before : item.after;
      const auto& desired = forward ? item.after : item.before;
      if (!source || !desired) continue;
      const std::size_t sourceIndex =
          forward ? item.beforeIndex : item.afterIndex;
      planned[sourceIndex] = *desired;
    }
    // Structural edits are deliberately independent of delta record order.
    std::vector<std::size_t> removals;
    for (const auto& item : changes) {
      const auto& source = forward ? item.before : item.after;
      const auto& desired = forward ? item.after : item.before;
      if (source && !desired)
        removals.push_back(forward ? item.beforeIndex : item.afterIndex);
    }
    std::sort(removals.rbegin(), removals.rend());
    for (const auto index : removals)
      planned.erase(planned.begin() + static_cast<std::ptrdiff_t>(index));

    std::vector<const IndexedValueDelta<T>*> insertions;
    for (const auto& item : changes) {
      const auto& source = forward ? item.before : item.after;
      const auto& desired = forward ? item.after : item.before;
      if (!source && desired) insertions.push_back(&item);
    }
    std::sort(insertions.begin(), insertions.end(), [forward](const auto* a,
                                                              const auto* b) {
      return (forward ? a->afterIndex : a->beforeIndex) <
             (forward ? b->afterIndex : b->beforeIndex);
    });
    for (const auto* item : insertions) {
      const std::size_t targetIndex =
          forward ? item->afterIndex : item->beforeIndex;
      const auto& desired = forward ? item->after : item->before;
      if (!desired || targetIndex > planned.size()) return false;
      planned.insert(planned.begin() + static_cast<std::ptrdiff_t>(targetIndex),
                     *desired);
    }
    if (planned.size() != finalSize) return false;
    // Also prove the requested final layout.  This catches stale deltas where
    // an earlier insertion/removal shifted a changed entity unexpectedly.
    for (const auto& item : changes) {
      const auto& desired = forward ? item.after : item.before;
      if (!desired) continue;
      const std::size_t targetIndex =
          forward ? item.afterIndex : item.beforeIndex;
      if (targetIndex >= planned.size() ||
          !equal(planned[targetIndex], *desired))
        return false;
    }
    result.emplace(std::move(planned));
    return true;
  };

  const auto pointEqual = [](const PointReference& a,
                             const PointReference& b) {
    return a.lineId == b.lineId && a.start == b.start &&
           a.circleId == b.circleId &&
           a.elementCenterId == b.elementCenterId && a.arcId == b.arcId &&
           a.origin == b.origin && a.bezierId == b.bezierId &&
           a.bezierPoint == b.bezierPoint;
  };
  const auto lineEqual = [](const Line& a, const Line& b) {
    return a.start.xMm == b.start.xMm && a.start.yMm == b.start.yMm &&
           a.end.xMm == b.end.xMm && a.end.yMm == b.end.yMm &&
           a.elementId == b.elementId && a.dashed == b.dashed;
  };
  const auto circleEqual = [](const Circle& a, const Circle& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.dashed == b.dashed;
  };
  const auto arcEqual = [](const Arc& a, const Arc& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.startAngleRad == b.startAngleRad &&
           a.sweepAngleRad == b.sweepAngleRad && a.dashed == b.dashed;
  };
  const auto bezierEqual = [](const Bezier& a, const Bezier& b) {
    return a.points == b.points && a.dashed == b.dashed;
  };
  const auto dimensionEqual = [&pointEqual](const Dimension& a,
                                             const Dimension& b) {
    if (a.id != b.id || a.constraintId != b.constraintId ||
        a.offsetMm != b.offsetMm || a.angleRad != b.angleRad)
      return false;
    if (a.constraintId != kInvalidConstraintId) return true;
    return a.kind == b.kind && a.geometryId == b.geometryId &&
           pointEqual(a.firstPoint, b.firstPoint) &&
           pointEqual(a.secondPoint, b.secondPoint) &&
           a.valueMm == b.valueMm;
  };
  const auto constraintEqual = [&pointEqual](const Constraint& a,
                                              const Constraint& b) {
    return a.id == b.id && a.mode == b.mode && a.type == b.type &&
           a.firstGeometry == b.firstGeometry &&
           a.secondGeometry == b.secondGeometry &&
           pointEqual(a.firstPoint, b.firstPoint) &&
           pointEqual(a.secondPoint, b.secondPoint) && a.value == b.value;
  };
  const auto scalarEqual = [](const auto& a, const auto& b) { return a == b; };

  std::optional<std::vector<Line>> lines;
  std::optional<std::vector<Circle>> circles;
  std::optional<std::vector<Arc>> arcs;
  std::optional<std::vector<Bezier>> beziers;
  std::optional<std::vector<GeometryId>> lineIds;
  std::optional<std::vector<GeometryId>> circleIds;
  std::optional<std::vector<GeometryId>> arcIds;
  std::optional<std::vector<GeometryId>> bezierIds;
  std::optional<std::vector<Dimension>> dimensions;
  std::optional<std::vector<Constraint>> constraints;
  std::optional<std::vector<std::size_t>> centers;
  try {
    if (!plan(lines_, delta.lines, lineEqual, lines) ||
        !plan(circles_, delta.circles, circleEqual, circles) ||
        !plan(arcs_, delta.arcs, arcEqual, arcs) ||
        !plan(beziers_, delta.beziers, bezierEqual, beziers) ||
        !plan(lineIds_, delta.lineIds, scalarEqual, lineIds) ||
        !plan(circleIds_, delta.circleIds, scalarEqual, circleIds) ||
        !plan(arcIds_, delta.arcIds, scalarEqual, arcIds) ||
        !plan(bezierIds_, delta.bezierIds, scalarEqual, bezierIds) ||
        !plan(dimensions_, delta.dimensions, dimensionEqual, dimensions) ||
        !plan(constraints_, delta.constraints, constraintEqual, constraints) ||
        !plan(centerNodeElementIds_, delta.centerNodeElementIds, scalarEqual,
              centers))
      return false;

    const auto& finalLines = lines ? *lines : lines_;
    const auto& finalCircles = circles ? *circles : circles_;
    const auto& finalArcs = arcs ? *arcs : arcs_;
    const auto& finalBeziers = beziers ? *beziers : beziers_;
    const auto& finalLineIds = lineIds ? *lineIds : lineIds_;
    const auto& finalCircleIds = circleIds ? *circleIds : circleIds_;
    const auto& finalArcIds = arcIds ? *arcIds : arcIds_;
    const auto& finalBezierIds = bezierIds ? *bezierIds : bezierIds_;
    const auto& finalDimensions = dimensions ? *dimensions : dimensions_;
    const auto& finalConstraints = constraints ? *constraints : constraints_;
    const auto& finalCenters = centers ? *centers : centerNodeElementIds_;
    if (finalLines.size() != finalLineIds.size() ||
        finalCircles.size() != finalCircleIds.size() ||
        finalArcs.size() != finalArcIds.size() ||
        finalBeziers.size() != finalBezierIds.size())
      return false;

    std::unordered_map<GeometryId, GeometryKind> geometry;
    geometry.reserve(finalLineIds.size() + finalCircleIds.size() +
                     finalArcIds.size() + finalBezierIds.size());
    const auto addIds = [&geometry](const auto& ids, GeometryKind kind) {
      for (const auto id : ids)
        if (id == kInvalidGeometryId || !geometry.emplace(id, kind).second)
          return false;
      return true;
    };
    if (!addIds(finalLineIds, GeometryKind::Line) ||
        !addIds(finalCircleIds, GeometryKind::Circle) ||
        !addIds(finalArcIds, GeometryKind::Arc) ||
        !addIds(finalBezierIds, GeometryKind::Bezier))
      return false;
    std::vector<std::size_t> elementIds;
    elementIds.reserve(finalLines.size());
    for (const auto& line : finalLines) {
      if (line.elementId == 0 || !std::isfinite(line.start.xMm) ||
          !std::isfinite(line.start.yMm) ||
          !std::isfinite(line.end.xMm) || !std::isfinite(line.end.yMm) ||
          (line.start.xMm == line.end.xMm &&
           line.start.yMm == line.end.yMm))
        return false;
      elementIds.push_back(line.elementId);
    }
    for (const auto& circle : finalCircles)
      if (!std::isfinite(circle.center.xMm) ||
          !std::isfinite(circle.center.yMm) ||
          !std::isfinite(circle.radiusMm) || circle.radiusMm <= 0.0)
        return false;
    constexpr double kTwoPi = 6.28318530717958647692;
    for (const auto& arc : finalArcs)
      if (!std::isfinite(arc.center.xMm) ||
          !std::isfinite(arc.center.yMm) ||
          !std::isfinite(arc.radiusMm) ||
          !std::isfinite(arc.startAngleRad) ||
          !std::isfinite(arc.sweepAngleRad) || arc.radiusMm <= 0.0 ||
          arc.sweepAngleRad <= 1e-9 ||
          arc.sweepAngleRad >= kTwoPi - 1e-9)
        return false;
    for (const auto& bezier : finalBeziers)
      for (const auto point : bezier.points)
        if (!std::isfinite(point.xMm) || !std::isfinite(point.yMm))
          return false;
    const auto hasElement = [&elementIds](std::size_t id) {
      return id != 0 &&
             std::find(elementIds.begin(), elementIds.end(), id) !=
                 elementIds.end();
    };
    std::vector<std::size_t> uniqueCenters = finalCenters;
    std::sort(uniqueCenters.begin(), uniqueCenters.end());
    if (std::adjacent_find(uniqueCenters.begin(), uniqueCenters.end()) !=
        uniqueCenters.end())
      return false;
    for (const auto id : finalCenters) {
      if (!hasElement(id) ||
          std::count(elementIds.begin(), elementIds.end(), id) != 4)
        return false;
    }
    const auto validPoint = [&geometry, &hasElement,
                             &finalCenters](const PointReference& p) {
      const unsigned sourceCount = static_cast<unsigned>(p.origin) +
          static_cast<unsigned>(p.lineId != kInvalidGeometryId) +
          static_cast<unsigned>(p.circleId != kInvalidGeometryId) +
          static_cast<unsigned>(p.arcId != kInvalidGeometryId) +
          static_cast<unsigned>(p.bezierId != kInvalidGeometryId) +
          static_cast<unsigned>(p.elementCenterId != 0);
      if (sourceCount > 1) return false;
      if (p.lineId != kInvalidGeometryId) {
        const auto found = geometry.find(p.lineId);
        if (found == geometry.end() || found->second != GeometryKind::Line)
          return false;
      }
      if (p.circleId != kInvalidGeometryId) {
        const auto found = geometry.find(p.circleId);
        if (found == geometry.end() || found->second != GeometryKind::Circle)
          return false;
      }
      if (p.arcId != kInvalidGeometryId) {
        const auto found = geometry.find(p.arcId);
        if (found == geometry.end() || found->second != GeometryKind::Arc)
          return false;
      }
      if (p.bezierId != kInvalidGeometryId) {
        const auto found = geometry.find(p.bezierId);
        if (found == geometry.end() || found->second != GeometryKind::Bezier ||
            p.bezierPoint >= 4)
          return false;
      }
      return p.elementCenterId == 0 ||
             (hasElement(p.elementCenterId) &&
              std::find(finalCenters.begin(), finalCenters.end(),
                        p.elementCenterId) != finalCenters.end());
    };
    const auto validConstraintRefs = [](const Constraint& item,
                                        const auto& geometryMap,
                                        const auto& pointValidator) {
      return (item.firstGeometry == kInvalidGeometryId ||
              geometryMap.contains(item.firstGeometry)) &&
             (item.secondGeometry == kInvalidGeometryId ||
              geometryMap.contains(item.secondGeometry)) &&
             pointValidator(item.firstPoint) &&
             pointValidator(item.secondPoint);
    };
    auto allowedInvalid = forward ? delta.afterInvalidConstraintIds
                                  : delta.beforeInvalidConstraintIds;
    std::sort(allowedInvalid.begin(), allowedInvalid.end());
    if (std::adjacent_find(allowedInvalid.begin(), allowedInvalid.end()) !=
        allowedInvalid.end())
      return false;
    std::vector<ConstraintId> constraintIds;
    constraintIds.reserve(finalConstraints.size());
    for (const auto& item : finalConstraints) {
      if (item.id == kInvalidConstraintId ||
          static_cast<unsigned>(static_cast<ConstraintType>(item.type)) >
              static_cast<unsigned>(ConstraintType::PointOnYAxis) ||
          !std::isfinite(item.value))
        return false;
      if (!validConstraintRefs(item, geometry, validPoint)) {
        // Historical orphan constraints remain representable for full-solve
        // diagnostics, but only on the delta side where the journal recorded
        // them.  This permits delete/cancel/undo without allowing a crafted
        // delta to introduce a new orphan.
        if (!std::binary_search(allowedInvalid.begin(), allowedInvalid.end(),
                                item.id))
          return false;
      }
      constraintIds.push_back(item.id);
    }
    std::sort(constraintIds.begin(), constraintIds.end());
    if (std::adjacent_find(constraintIds.begin(), constraintIds.end()) !=
        constraintIds.end())
      return false;
    for (const auto id : allowedInvalid) {
      const auto found = std::find_if(
          finalConstraints.begin(), finalConstraints.end(),
          [id](const Constraint& item) { return item.id == id; });
      if (found == finalConstraints.end() ||
          validConstraintRefs(*found, geometry, validPoint))
        return false;
    }
    std::vector<DimensionId> dimensionIds;
    dimensionIds.reserve(finalDimensions.size());
    for (const auto& item : finalDimensions) {
      if (static_cast<unsigned>(item.kind) >
              static_cast<unsigned>(DimensionKind::CircleRadius) ||
          item.id == kInvalidDimensionId ||
          !std::isfinite(item.valueMm) || !std::isfinite(item.offsetMm) ||
          !std::isfinite(item.angleRad) || item.valueMm <= 0.0 ||
          (item.geometryId != kInvalidGeometryId &&
           !geometry.contains(item.geometryId)) ||
          !validPoint(item.firstPoint) || !validPoint(item.secondPoint))
        return false;
      dimensionIds.push_back(item.id);
    }
    std::sort(dimensionIds.begin(), dimensionIds.end());
    if (std::adjacent_find(dimensionIds.begin(), dimensionIds.end()) !=
        dimensionIds.end())
      return false;

    const auto finalNextElement =
        forward ? delta.afterNextElementId : delta.beforeNextElementId;
    const auto finalNextGeometry =
        forward ? delta.afterNextGeometryId : delta.beforeNextGeometryId;
    const auto finalNextConstraint =
        forward ? delta.afterNextConstraintId : delta.beforeNextConstraintId;
    const auto finalNextDimension =
        forward ? delta.afterNextDimensionId : delta.beforeNextDimensionId;
    const auto maxElement = elementIds.empty()
        ? std::size_t{0}
        : *std::max_element(elementIds.begin(), elementIds.end());
    const auto maxGeometry = geometry.empty()
        ? GeometryId{0}
        : std::max({finalLineIds.empty() ? GeometryId{0}
                                        : *std::max_element(finalLineIds.begin(),
                                                            finalLineIds.end()),
                    finalCircleIds.empty() ? GeometryId{0}
                                          : *std::max_element(finalCircleIds.begin(),
                                                              finalCircleIds.end()),
                    finalArcIds.empty() ? GeometryId{0}
                                       : *std::max_element(finalArcIds.begin(),
                                                           finalArcIds.end()),
                    finalBezierIds.empty() ? GeometryId{0}
                                          : *std::max_element(finalBezierIds.begin(),
                                                              finalBezierIds.end())});
    const auto maxConstraint = constraintIds.empty()
        ? ConstraintId{0}
        : constraintIds.back();
    if (finalNextElement == 0 || finalNextElement <= maxElement ||
        finalNextGeometry == kInvalidGeometryId ||
        finalNextGeometry <= maxGeometry ||
        finalNextConstraint == kInvalidConstraintId ||
        finalNextConstraint <= maxConstraint ||
        finalNextDimension == kInvalidDimensionId ||
        std::binary_search(dimensionIds.begin(), dimensionIds.end(),
                           finalNextDimension))
      return false;

    const auto projectedFingerprint = [&] {
      std::uint64_t hash = 1469598103934665603ULL;
      const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
      };
      const auto mixDouble = [&mix](double value) {
        if (value == 0.0) value = 0.0;
        mix(std::bit_cast<std::uint64_t>(value));
      };
      const auto mixPoint = [&mix, &mixDouble](const PointReference& point) {
        mix(point.lineId); mix(point.start ? 1U : 0U); mix(point.circleId);
        mix(point.elementCenterId); mix(point.arcId);
        mix(point.origin ? 1U : 0U);
        mix(point.bezierId); mix(point.bezierPoint);
      };
      mix(finalLines.size());
      for (std::size_t index = 0; index < finalLines.size(); ++index) {
        const auto& line = finalLines[index];
        mix(finalLineIds[index]);
        mixDouble(line.start.xMm); mixDouble(line.start.yMm);
        mixDouble(line.end.xMm); mixDouble(line.end.yMm);
        mix(line.elementId); mix(line.dashed ? 1U : 0U);
      }
      mix(finalCircles.size());
      for (std::size_t index = 0; index < finalCircles.size(); ++index) {
        const auto& circle = finalCircles[index];
        mix(finalCircleIds[index]);
        mixDouble(circle.center.xMm); mixDouble(circle.center.yMm);
        mixDouble(circle.radiusMm); mix(circle.dashed ? 1U : 0U);
      }
      mix(finalArcs.size());
      for (std::size_t index = 0; index < finalArcs.size(); ++index) {
        const auto& arc = finalArcs[index];
        mix(finalArcIds[index]);
        mixDouble(arc.center.xMm); mixDouble(arc.center.yMm);
        mixDouble(arc.radiusMm); mixDouble(arc.startAngleRad);
        mixDouble(arc.sweepAngleRad); mix(arc.dashed ? 1U : 0U);
      }
      mix(finalBeziers.size());
      for (std::size_t index = 0; index < finalBeziers.size(); ++index) {
        mix(finalBezierIds[index]);
        for (const auto point : finalBeziers[index].points) {
          mixDouble(point.xMm); mixDouble(point.yMm);
        }
        mix(finalBeziers[index].dashed ? 1U : 0U);
      }
      mix(finalCenters.size());
      for (const auto id : finalCenters) mix(id);
      mix(finalDimensions.size());
      for (const auto& dimension : finalDimensions) {
        mix(dimension.id);
        mix(dimension.constraintId);
        mixDouble(dimension.offsetMm); mixDouble(dimension.angleRad);
        if (dimension.constraintId == kInvalidConstraintId) {
          mix(static_cast<std::uint64_t>(dimension.kind));
          mix(dimension.geometryId); mixPoint(dimension.firstPoint);
          mixPoint(dimension.secondPoint); mixDouble(dimension.valueMm);
        }
      }
      mix(finalConstraints.size());
      for (const auto& constraint : finalConstraints) {
        mix(constraint.id);
        mix(static_cast<std::uint64_t>(constraint.mode));
        mix(static_cast<std::uint64_t>(
            static_cast<ConstraintType>(constraint.type)));
        mix(constraint.firstGeometry); mix(constraint.secondGeometry);
        mixPoint(constraint.firstPoint);
        mixPoint(constraint.secondPoint);
        mixDouble(constraint.value);
      }
      mix(finalNextElement); mix(finalNextGeometry); mix(finalNextConstraint);
      mix(finalNextDimension);
      return hash;
    };
    const auto expectedTargetFingerprint =
        forward ? delta.afterSemanticFingerprint
                : delta.beforeSemanticFingerprint;
    const auto projected = projectedFingerprint();
    if (projected != expectedTargetFingerprint) return false;

    if (lines) lines_.swap(*lines);
    if (circles) circles_.swap(*circles);
    if (arcs) arcs_.swap(*arcs);
    if (beziers) beziers_.swap(*beziers);
    if (lineIds) lineIds_.swap(*lineIds);
    if (circleIds) circleIds_.swap(*circleIds);
    if (arcIds) arcIds_.swap(*arcIds);
    if (bezierIds) bezierIds_.swap(*bezierIds);
    if (dimensions) dimensions_.swap(*dimensions);
    if (constraints) constraints_.swap(*constraints);
    if (centers) centerNodeElementIds_.swap(*centers);
    nextElementId_ = finalNextElement;
    nextGeometryId_ = finalNextGeometry;
    nextConstraintId_ = finalNextConstraint;
    nextDimensionId_ = finalNextDimension;
  } catch (...) {
    return false;
  }
  invalidateStructureIndexes();
  updateBounds();
  return true;
}

bool Sketch::semanticallyEqual(const Sketch& other) const noexcept {
  const auto pointEqual = [](const PointReference& a,
                             const PointReference& b) {
    return a.lineId == b.lineId && a.start == b.start &&
           a.circleId == b.circleId &&
           a.elementCenterId == b.elementCenterId && a.arcId == b.arcId &&
           a.origin == b.origin && a.bezierId == b.bezierId &&
           a.bezierPoint == b.bezierPoint;
  };
  const auto lineEqual = [](const Line& a, const Line& b) {
    return a.start.xMm == b.start.xMm && a.start.yMm == b.start.yMm &&
           a.end.xMm == b.end.xMm && a.end.yMm == b.end.yMm &&
           a.elementId == b.elementId && a.dashed == b.dashed;
  };
  const auto circleEqual = [](const Circle& a, const Circle& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.dashed == b.dashed;
  };
  const auto arcEqual = [](const Arc& a, const Arc& b) {
    return a.center.xMm == b.center.xMm && a.center.yMm == b.center.yMm &&
           a.radiusMm == b.radiusMm && a.startAngleRad == b.startAngleRad &&
           a.sweepAngleRad == b.sweepAngleRad && a.dashed == b.dashed;
  };
  const auto bezierEqual = [](const Bezier& a, const Bezier& b) {
    return a.points == b.points && a.dashed == b.dashed;
  };
  const auto dimensionEqual = [&pointEqual](const Dimension& a,
                                             const Dimension& b) {
    if (a.id != b.id || a.constraintId != b.constraintId ||
        a.offsetMm != b.offsetMm || a.angleRad != b.angleRad)
      return false;
    if (a.constraintId != kInvalidConstraintId) return true;
    return a.kind == b.kind && a.geometryId == b.geometryId &&
           pointEqual(a.firstPoint, b.firstPoint) &&
           pointEqual(a.secondPoint, b.secondPoint) &&
           a.valueMm == b.valueMm;
  };
  const auto constraintEqual = [&pointEqual](const Constraint& a,
                                              const Constraint& b) {
    return a.id == b.id && a.mode == b.mode && a.type == b.type &&
           a.firstGeometry == b.firstGeometry &&
           a.secondGeometry == b.secondGeometry &&
           pointEqual(a.firstPoint, b.firstPoint) &&
           pointEqual(a.secondPoint, b.secondPoint) && a.value == b.value;
  };
  return lines_.size() == other.lines_.size() &&
         std::equal(lines_.begin(), lines_.end(), other.lines_.begin(),
                    lineEqual) &&
         circles_.size() == other.circles_.size() &&
         std::equal(circles_.begin(), circles_.end(), other.circles_.begin(),
                    circleEqual) &&
         arcs_.size() == other.arcs_.size() &&
         std::equal(arcs_.begin(), arcs_.end(), other.arcs_.begin(),
                    arcEqual) &&
         beziers_.size() == other.beziers_.size() &&
         std::equal(beziers_.begin(), beziers_.end(), other.beziers_.begin(),
                    bezierEqual) &&
         lineIds_ == other.lineIds_ && circleIds_ == other.circleIds_ &&
         arcIds_ == other.arcIds_ && bezierIds_ == other.bezierIds_ &&
         dimensions_.size() == other.dimensions_.size() &&
         std::equal(dimensions_.begin(), dimensions_.end(),
                    other.dimensions_.begin(), dimensionEqual) &&
         constraints_.size() == other.constraints_.size() &&
         std::equal(constraints_.begin(), constraints_.end(),
                    other.constraints_.begin(), constraintEqual) &&
         centerNodeElementIds_ == other.centerNodeElementIds_ &&
         nextElementId_ == other.nextElementId_ &&
         nextGeometryId_ == other.nextGeometryId_ &&
         nextConstraintId_ == other.nextConstraintId_ &&
         nextDimensionId_ == other.nextDimensionId_;
}

std::uint64_t Sketch::semanticFingerprint() const noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  const auto mixDouble = [&mix](double value) {
    if (value == 0.0) value = 0.0;
    mix(std::bit_cast<std::uint64_t>(value));
  };
  const auto mixPoint = [&mix, &mixDouble](const PointReference& point) {
    mix(point.lineId);
    mix(point.start ? 1U : 0U);
    mix(point.circleId);
    mix(point.elementCenterId);
    mix(point.arcId);
    mix(point.origin ? 1U : 0U);
    mix(point.bezierId);
    mix(point.bezierPoint);
  };
  mix(lines_.size());
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    mix(lineIds_[index]);
    const auto& line = lines_[index];
    mixDouble(line.start.xMm);
    mixDouble(line.start.yMm);
    mixDouble(line.end.xMm);
    mixDouble(line.end.yMm);
    mix(line.elementId);
    mix(line.dashed ? 1U : 0U);
  }
  mix(circles_.size());
  for (std::size_t index = 0; index < circles_.size(); ++index) {
    mix(circleIds_[index]);
    const auto& circle = circles_[index];
    mixDouble(circle.center.xMm);
    mixDouble(circle.center.yMm);
    mixDouble(circle.radiusMm);
    mix(circle.dashed ? 1U : 0U);
  }
  mix(arcs_.size());
  for (std::size_t index = 0; index < arcs_.size(); ++index) {
    mix(arcIds_[index]);
    const auto& arc = arcs_[index];
    mixDouble(arc.center.xMm);
    mixDouble(arc.center.yMm);
    mixDouble(arc.radiusMm);
    mixDouble(arc.startAngleRad);
    mixDouble(arc.sweepAngleRad);
    mix(arc.dashed ? 1U : 0U);
  }
  mix(beziers_.size());
  for (std::size_t index = 0; index < beziers_.size(); ++index) {
    mix(bezierIds_[index]);
    for (const auto point : beziers_[index].points) {
      mixDouble(point.xMm);
      mixDouble(point.yMm);
    }
    mix(beziers_[index].dashed ? 1U : 0U);
  }
  mix(centerNodeElementIds_.size());
  for (const auto id : centerNodeElementIds_) mix(id);
  mix(dimensions_.size());
  for (const auto& dimension : dimensions_) {
    mix(dimension.id);
    mix(dimension.constraintId);
    mixDouble(dimension.offsetMm);
    mixDouble(dimension.angleRad);
    if (dimension.constraintId == kInvalidConstraintId) {
      mix(static_cast<std::uint64_t>(dimension.kind));
      mix(dimension.geometryId);
      mixPoint(dimension.firstPoint);
      mixPoint(dimension.secondPoint);
      mixDouble(dimension.valueMm);
    }
  }
  mix(constraints_.size());
  for (const auto& constraint : constraints_) {
    mix(constraint.id);
    mix(static_cast<std::uint64_t>(constraint.mode));
    mix(static_cast<std::uint64_t>(
        static_cast<ConstraintType>(constraint.type)));
    mix(constraint.firstGeometry);
    mix(constraint.secondGeometry);
    mixPoint(constraint.firstPoint);
    mixPoint(constraint.secondPoint);
    mixDouble(constraint.value);
  }
  mix(nextElementId_);
  mix(nextGeometryId_);
  mix(nextConstraintId_);
  mix(nextDimensionId_);
  return hash;
}

std::uint64_t Sketch::solverFingerprint() const noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (const auto id : lineIds_) mix(id);
  for (const auto& line : lines_) {
    mix(std::bit_cast<std::uint64_t>(line.start.xMm));
    mix(std::bit_cast<std::uint64_t>(line.start.yMm));
    mix(std::bit_cast<std::uint64_t>(line.end.xMm));
    mix(std::bit_cast<std::uint64_t>(line.end.yMm));
    mix(line.elementId);
  }
  for (std::size_t i = 0; i < circles_.size(); ++i) {
    mix(circleIds_[i]);
    mix(std::bit_cast<std::uint64_t>(circles_[i].center.xMm));
    mix(std::bit_cast<std::uint64_t>(circles_[i].center.yMm));
    mix(std::bit_cast<std::uint64_t>(circles_[i].radiusMm));
  }
  for (std::size_t i = 0; i < arcs_.size(); ++i) {
    mix(arcIds_[i]);
    mix(std::bit_cast<std::uint64_t>(arcs_[i].center.xMm));
    mix(std::bit_cast<std::uint64_t>(arcs_[i].center.yMm));
    mix(std::bit_cast<std::uint64_t>(arcs_[i].radiusMm));
    mix(std::bit_cast<std::uint64_t>(arcs_[i].startAngleRad));
    mix(std::bit_cast<std::uint64_t>(arcs_[i].sweepAngleRad));
  }
  for (std::size_t i = 0; i < beziers_.size(); ++i) {
    mix(bezierIds_[i]);
    for (const auto point : beziers_[i].points) {
      mix(std::bit_cast<std::uint64_t>(point.xMm));
      mix(std::bit_cast<std::uint64_t>(point.yMm));
    }
  }
  for (const auto elementId : centerNodeElementIds_) mix(elementId);
  const auto mixPoint = [&mix](const PointReference& point) {
    mix(point.lineId);
    mix(point.start ? 1U : 0U);
    mix(point.circleId);
    mix(point.elementCenterId);
    mix(point.arcId);
    mix(point.origin ? 1U : 0U);
    mix(point.bezierId);
    mix(point.bezierPoint);
  };
  for (const auto& constraint : constraints_) {
    mix(constraint.id);
    mix(static_cast<std::uint64_t>(
        static_cast<ConstraintType>(constraint.type)));
    mix(constraint.firstGeometry);
    mix(constraint.secondGeometry);
    mixPoint(constraint.firstPoint);
    mixPoint(constraint.secondPoint);
    mix(std::bit_cast<std::uint64_t>(
        static_cast<double>(constraint.value)));
  }
  return hash;
}

bool Sketch::isGeometryLocked(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return false;

  if (const auto line = lineIndex(id)) {
    const std::size_t elementId = lines_[*line].elementId;
    for (const auto& constraint : constraints_) {
      if (!participatesInConstraintSystem(constraint) ||
          constraint.type != ConstraintType::Lock ||
          constraint.firstGeometry == kInvalidGeometryId)
        continue;

      const auto lockedLine = lineIndex(constraint.firstGeometry);
      if (lockedLine &&
          lines_[*lockedLine].elementId == elementId)
        return true;
    }
    return false;
  }

  if (circleIndex(id) || arcIndex(id) || bezierIndex(id)) {
    return std::any_of(
        constraints_.begin(), constraints_.end(),
        [id](const Constraint& constraint) {
          return participatesInConstraintSystem(constraint) &&
                 constraint.type == ConstraintType::Lock &&
                 constraint.firstGeometry == id;
        });
  }

  return false;
}

bool Sketch::isElementLocked(std::size_t elementId) const noexcept {
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (lines_[index].elementId == elementId &&
        isGeometryLocked(lineIds_[index]))
      return true;
  }
  return false;
}

bool Sketch::isPointReferenceLocked(
    PointReference reference) const noexcept {
  if (reference.origin) return true;
  if (reference.elementCenterId != 0)
    return isElementLocked(reference.elementCenterId);
  if (reference.circleId != kInvalidGeometryId)
    return isGeometryLocked(reference.circleId);
  if (reference.arcId != kInvalidGeometryId)
    return isGeometryLocked(reference.arcId);
  if (reference.bezierId != kInvalidGeometryId)
    return isGeometryLocked(reference.bezierId);
  return isGeometryLocked(reference.lineId);
}

void Sketch::restoreLockedGeometryFrom(const Sketch& baseline) {
  for (const auto& constraint : constraints_) {
    if (!participatesInConstraintSystem(constraint) ||
        constraint.type != ConstraintType::Lock ||
        constraint.firstGeometry == kInvalidGeometryId)
      continue;

    if (const auto lockedLine = lineIndex(constraint.firstGeometry)) {
      const std::size_t elementId = lines_[*lockedLine].elementId;

      for (std::size_t index = 0; index < lines_.size(); ++index) {
        if (lines_[index].elementId != elementId) continue;

        const GeometryId id = lineIds_[index];
        const auto source = baseline.lineIndex(id);
        if (!source) continue;

        lines_[index].start = baseline.lines_[*source].start;
        lines_[index].end = baseline.lines_[*source].end;
      }
      continue;
    }

    if (const auto lockedCircle =
            circleIndex(constraint.firstGeometry)) {
      const auto source =
          baseline.circleIndex(constraint.firstGeometry);
      if (!source) continue;

      circles_[*lockedCircle].center =
          baseline.circles_[*source].center;
      circles_[*lockedCircle].radiusMm =
          baseline.circles_[*source].radiusMm;
      continue;
    }

    if (const auto lockedArc = arcIndex(constraint.firstGeometry)) {
      const auto source = baseline.arcIndex(constraint.firstGeometry);
      if (!source) continue;

      arcs_[*lockedArc] = baseline.arcs_[*source];
      continue;
    }

    if (const auto lockedBezier = bezierIndex(constraint.firstGeometry)) {
      const auto source = baseline.bezierIndex(constraint.firstGeometry);
      if (source) beziers_[*lockedBezier] = baseline.beziers_[*source];
    }
  }

  updateBounds();
}

std::optional<Point> Sketch::referencedPoint(
    PointReference reference) const noexcept {
  if (reference.origin) return Point{0.0, 0.0};
  if (reference.elementCenterId != 0)
    return elementCenterPoint(reference.elementCenterId);

  if (reference.circleId != kInvalidGeometryId) {
    const auto index = circleIndex(reference.circleId);
    if (!index) return std::nullopt;
    return circles_[*index].center;
  }

  if (reference.arcId != kInvalidGeometryId) {
    const auto index = arcIndex(reference.arcId);
    if (!index) return std::nullopt;
    return reference.start ? arcStartPoint(arcs_[*index])
                           : arcEndPoint(arcs_[*index]);
  }

  if (reference.bezierId != kInvalidGeometryId) {
    const auto index = bezierIndex(reference.bezierId);
    if (!index || reference.bezierPoint >= 4) return std::nullopt;
    return beziers_[*index].points[reference.bezierPoint];
  }

  const auto index = lineIndex(reference.lineId);
  if (!index) return std::nullopt;
  const auto& line = lines_[*index];
  return reference.start ? line.start : line.end;
}
SketchMutationResult Sketch::translatePoint(PointReference reference,
                                            double dxMm, double dyMm) {
  if (!std::isfinite(dxMm) || !std::isfinite(dyMm))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  if (!referencedPoint(reference))
    return rejectedMutation(
        *this, {}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (reference.origin)
    return (std::abs(dxMm) <= 1e-12 && std::abs(dyMm) <= 1e-12)
               ? noMutation(*this)
               : rejectedMutation(
                     *this, {}, SketchMutationStatus::Conflicting,
                     SketchMutationDiagnosticCode::LockedGeometry);
  if (dxMm == 0.0 && dyMm == 0.0) return noMutation(*this);

  // CRASH-FREE 04: COMPLETE POINTREFERENCE IDENTITY
  //
  // A virtual element centre is its own reference class. Never compare two
  // centre references through their default invalid lineId values.
  const auto sameReference =
      [](PointReference first,
         PointReference second) {
        if (first.origin || second.origin)
          return first.origin && second.origin;
        if (first.elementCenterId != 0 ||
            second.elementCenterId != 0) {
          return first.elementCenterId != 0 &&
                 second.elementCenterId != 0 &&
                 first.elementCenterId ==
                     second.elementCenterId;
        }

        if (first.circleId != kInvalidGeometryId ||
            second.circleId != kInvalidGeometryId) {
          return first.circleId != kInvalidGeometryId &&
                 second.circleId != kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        if (first.arcId != kInvalidGeometryId ||
            second.arcId != kInvalidGeometryId) {
          return first.arcId != kInvalidGeometryId &&
                 second.arcId != kInvalidGeometryId &&
                 first.arcId == second.arcId &&
                 first.start == second.start;
        }

        if (first.bezierId != kInvalidGeometryId ||
            second.bezierId != kInvalidGeometryId) {
          return first.bezierId != kInvalidGeometryId &&
                 second.bezierId != kInvalidGeometryId &&
                 first.bezierId == second.bezierId &&
                 first.bezierPoint == second.bezierPoint;
        }

        if (first.lineId == kInvalidGeometryId ||
            second.lineId == kInvalidGeometryId)
          return false;

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  std::vector<PointReference> connectedPoints{reference};
  bool changed = true;

  while (changed) {
    changed = false;

    for (const auto& constraint : constraints_) {
      if (!participatesInConstraintSystem(constraint) ||
          constraint.type != ConstraintType::Coincident)
        continue;

      const auto first = constraint.firstPoint;
      const auto second = constraint.secondPoint;

      if (!referencedPoint(first) ||
          !referencedPoint(second))
        continue;

      const bool containsFirst =
          std::any_of(
              connectedPoints.begin(),
              connectedPoints.end(),
              [first, &sameReference](
                  PointReference item) {
                return sameReference(item, first);
              });

      const bool containsSecond =
          std::any_of(
              connectedPoints.begin(),
              connectedPoints.end(),
              [second, &sameReference](
                  PointReference item) {
                return sameReference(item, second);
              });

      if (containsFirst && !containsSecond) {
        connectedPoints.push_back(second);
        changed = true;
      }
      else if (containsSecond && !containsFirst) {
        connectedPoints.push_back(first);
        changed = true;
      }
    }
  }

  // LOCK CONSTRAINT: connected point cluster contains a lock.
  if (std::any_of(
          connectedPoints.begin(), connectedPoints.end(),
          [this](PointReference item) {
            return isPointReferenceLocked(item);
          })) {
    return rejectedMutation(*this, {}, SketchMutationStatus::Conflicting,
                            SketchMutationDiagnosticCode::LockedGeometry);
  }

  if (!translatedPointReferencesBoundsFinite(*this, connectedPoints, dxMm,
                                             dyMm))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);

  const auto journalDepthBefore = deltaJournalDepth();
  const bool ownsJournal = journalDepthBefore == 0;
  if (ownsJournal) beginDeltaJournal();
  journalCapturePoint(reference);

  // CRASH-FREE 04: VIRTUAL CENTER MOVEMENT
  //
  // Virtual rectangle centres are derived coordinates. Moving one means
  // translating its whole owning element exactly once.
  std::vector<std::size_t> movedCenterElements;

  for (const auto pointReference : connectedPoints) {
    if (pointReference.elementCenterId == 0)
      continue;

    if (std::find(
            movedCenterElements.begin(),
            movedCenterElements.end(),
            pointReference.elementCenterId) !=
        movedCenterElements.end())
      continue;

    bool foundElement = false;

    for (auto& line : lines_) {
      if (line.elementId !=
          pointReference.elementCenterId)
        continue;

      foundElement = true;
      line.start.xMm += dxMm;
      line.start.yMm += dyMm;
      line.end.xMm += dxMm;
      line.end.yMm += dyMm;
    }

    if (foundElement)
      movedCenterElements.push_back(
          pointReference.elementCenterId);
  }

  for (const auto pointReference : connectedPoints) {
    if (pointReference.elementCenterId != 0)
      continue;

    if (pointReference.circleId !=
        kInvalidGeometryId) {
      const auto index =
          circleIndex(pointReference.circleId);

      if (!index)
        continue;

      circles_[*index].center.xMm += dxMm;
      circles_[*index].center.yMm += dyMm;
      continue;
    }

    if (pointReference.arcId != kInvalidGeometryId) {
      const auto index = arcIndex(pointReference.arcId);
      if (!index) continue;
      const auto current = referencedPoint(pointReference);
      if (!current) continue;
      const Point target{current->xMm + dxMm,
                         current->yMm + dyMm};
      (void)moveArcEndpointRigid(arcs_[*index], pointReference.start, target);
      continue;
    }

    if (pointReference.bezierId != kInvalidGeometryId) {
      const auto index = bezierIndex(pointReference.bezierId);
      if (!index || pointReference.bezierPoint >= 4) continue;
      auto& point = beziers_[*index].points[pointReference.bezierPoint];
      point.xMm += dxMm;
      point.yMm += dyMm;
      continue;
    }

    const auto index =
        lineIndex(pointReference.lineId);

    if (!index)
      continue;

    // If this endpoint belongs to an element already translated through its
    // virtual centre, do not apply the same displacement twice.
    if (std::find(
            movedCenterElements.begin(),
            movedCenterElements.end(),
            lines_[*index].elementId) !=
        movedCenterElements.end())
      continue;

    Point& point =
        pointReference.start
            ? lines_[*index].start
            : lines_[*index].end;

    point.xMm += dxMm;
    point.yMm += dyMm;
  }

  std::vector<GeometryId> movedLineIds;

  for (const auto pointReference :
       connectedPoints) {
    if (pointReference.elementCenterId != 0 ||
        pointReference.circleId !=
            kInvalidGeometryId ||
        pointReference.arcId !=
            kInvalidGeometryId ||
        pointReference.lineId ==
            kInvalidGeometryId)
      continue;

    if (std::find(
            movedLineIds.begin(),
            movedLineIds.end(),
            pointReference.lineId) ==
        movedLineIds.end())
      movedLineIds.push_back(
          pointReference.lineId);
  }

  // Lines translated through a virtual centre are also active moved
  // geometries for Equal propagation.
  for (std::size_t index = 0;
       index < lines_.size();
       ++index) {
    if (std::find(
            movedCenterElements.begin(),
            movedCenterElements.end(),
            lines_[index].elementId) ==
        movedCenterElements.end())
      continue;

    const auto id = lineIds_[index];

    if (id != kInvalidGeometryId &&
        std::find(
            movedLineIds.begin(),
            movedLineIds.end(),
            id) ==
            movedLineIds.end())
      movedLineIds.push_back(id);
  }

  std::vector<GeometryId> dirtyIds = movedLineIds;
  for (const auto pointReference : connectedPoints) {
    if (pointReference.circleId != kInvalidGeometryId)
      dirtyIds.push_back(pointReference.circleId);
    if (pointReference.arcId != kInvalidGeometryId)
      dirtyIds.push_back(pointReference.arcId);
    if (pointReference.bezierId != kInvalidGeometryId)
      dirtyIds.push_back(pointReference.bezierId);
    if (pointReference.elementCenterId != 0) {
      for (std::size_t index = 0; index < lines_.size(); ++index)
        if (lines_[index].elementId == pointReference.elementCenterId)
          dirtyIds.push_back(lineIds_[index]);
    }
  }
  const auto mutation = mutationFromSolve(
      *this, dirtyIds,
      SketchSolver::solveStableComponent(
          *this, dirtyIds, 16, kInvalidConstraintId, true));
  if (!mutation.committed()) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return mutation;
  }
  updateBounds();
  if (ownsJournal) static_cast<void>(finishDeltaJournal());
  return mutation;
}
bool Sketch::storeDimension(const Dimension& dimension) {
  const auto linkedIndex = constraintIndex(dimension.constraintId);
  if (dimension.constraintId == kInvalidConstraintId || !linkedIndex ||
      !annotatableConstraintType(static_cast<ConstraintType>(
          constraints_[*linkedIndex].type)) ||
      !std::isfinite(dimension.offsetMm) ||
      !std::isfinite(dimension.angleRad) ||
      std::any_of(dimensions_.begin(), dimensions_.end(),
                  [&](const Dimension& existing) {
                    return existing.constraintId == dimension.constraintId;
                  }))
    return false;

  Dimension stored = dimension;
  if (stored.id != kInvalidDimensionId && dimensionIndex(stored.id))
    return false;
  if (stored.id == kInvalidDimensionId) {
    const auto generated = firstFreeDimensionId(dimensions_, nextDimensionId_);
    if (!generated) return false;
    stored.id = *generated;
  }
  DimensionId nextStart = nextDimensionId_;
  if (stored.id >= nextStart) {
    nextStart = stored.id == std::numeric_limits<DimensionId>::max()
        ? DimensionId{1}
        : stored.id + 1;
  }
  const auto next = firstFreeDimensionId(dimensions_, nextStart, stored.id);
  if (!next) return false;
  journalRecordAddedDimension(dimensions_.size());
  dimensions_.push_back(std::move(stored));
  nextDimensionId_ = *next;
  static_cast<void>(dimensions());
  return true;
}

void Sketch::clearDimensions() {
  while (!dimensions_.empty())
    static_cast<void>(removeDimension(dimensions_.size() - 1));
}

bool Sketch::removeDimension(std::size_t index) {
  if (index >= dimensions_.size()) return false;
  for (const auto& journal : deltaJournals_)
    if (journal.dimensionTokens.size() != dimensions_.size())
      return false;
  journalCaptureDimension(index);
  dimensions_.erase(dimensions_.begin() + static_cast<std::ptrdiff_t>(index));
  for (auto& journal : deltaJournals_)
    journal.dimensionTokens.erase(
        journal.dimensionTokens.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

SketchMutationResult Sketch::setDimensionPlacement(std::size_t index,
                                                   double offsetMm,
                                                   double angleRad) {
  auto& dimensions = dimensions_;
  if (!std::isfinite(offsetMm) || !std::isfinite(angleRad))
    return rejectedMutation(*this, {}, SketchMutationStatus::InternalError,
                            SketchMutationDiagnosticCode::NonFiniteGeometry);
  if (index >= dimensions.size())
    return rejectedMutation(
        *this, {}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (dimensions[index].offsetMm == offsetMm &&
      dimensions[index].angleRad == angleRad)
    return noMutation(*this);
  journalCaptureDimension(index);
  dimensions[index].offsetMm = offsetMm;
  dimensions[index].angleRad = angleRad;
  return mutationResult(*this, {}, SketchMutationStatus::Applied,
                        SketchMutationDiagnosticCode::None);
}

bool Sketch::setDimensionValue(std::size_t index, double valueMm) {
  if (index >= dimensions_.size() || !std::isfinite(valueMm) ||
      valueMm <= 0.0)
    return false;
  const auto constraintId = dimensions_[index].constraintId;
  const auto constraint = constraintIndex(constraintId);
  if (!constraint ||
      !participatesInConstraintSystem(constraints_[*constraint]))
    return false;
  return setConstraintValue(constraintId, valueMm).committed();
}

bool Sketch::setDimensionConstraint(std::size_t index,
                                    ConstraintId constraintId) {
  const auto linkedIndex = constraintIndex(constraintId);
  if (index >= dimensions_.size() || constraintId == kInvalidConstraintId ||
      !linkedIndex ||
      !annotatableConstraintType(static_cast<ConstraintType>(
          constraints_[*linkedIndex].type)) ||
      std::any_of(dimensions_.begin(), dimensions_.end(),
                  [&](const Dimension& existing) {
                    return &existing != &dimensions_[index] &&
                           existing.constraintId == constraintId;
                  }))
    return false;
  journalCaptureDimension(index);
  dimensions_[index].constraintId = constraintId;
  static_cast<void>(dimensions());
  return true;
}

bool Sketch::restoreGeometryIds(std::vector<GeometryId> lineIds,
                                std::vector<GeometryId> circleIds,
                                std::vector<GeometryId> arcIds,
                                std::vector<GeometryId> bezierIds) {
  if (lineIds.size() != lines_.size() ||
      circleIds.size() != circles_.size() || arcIds.size() != arcs_.size() ||
      bezierIds.size() != beziers_.size())
    return false;
  std::unordered_set<GeometryId> unique;
  GeometryId maximum = kInvalidGeometryId;
  const auto consume = [&](const std::vector<GeometryId>& ids) {
    for (const auto id : ids) {
      if (id == kInvalidGeometryId || !unique.insert(id).second) return false;
      maximum = std::max(maximum, id);
    }
    return true;
  };
  if (!consume(lineIds) || !consume(circleIds) || !consume(arcIds) ||
      !consume(bezierIds))
    return false;
  lineIds_ = std::move(lineIds);
  circleIds_ = std::move(circleIds);
  arcIds_ = std::move(arcIds);
  bezierIds_ = std::move(bezierIds);
  nextGeometryId_ = maximum == std::numeric_limits<GeometryId>::max()
      ? GeometryId{1}
      : maximum + 1;
  invalidateStructureIndexes();
  hasLastSolvedFingerprint_ = false;
  return true;
}

GeometryId Sketch::nextGeometryId() const noexcept { return nextGeometryId_; }

ConstraintId Sketch::nextConstraintId() const noexcept {
  return nextConstraintId_;
}

DimensionId Sketch::nextDimensionId() const noexcept {
  return nextDimensionId_;
}

std::size_t Sketch::nextElementId() const noexcept { return nextElementId_; }

bool Sketch::restoreIdAllocators(std::size_t nextElementId,
                                 GeometryId nextGeometryId,
                                 ConstraintId nextConstraintId,
                                 DimensionId nextDimensionId) {
  std::size_t maximumElementId = 0;
  for (const auto& line : lines_)
    maximumElementId = std::max(maximumElementId, line.elementId);
  for (const auto elementId : centerNodeElementIds_)
    maximumElementId = std::max(maximumElementId, elementId);

  GeometryId maximumGeometryId = kInvalidGeometryId;
  const auto accountGeometryIds = [&](const auto& ids) {
    for (const auto id : ids)
      maximumGeometryId = std::max(maximumGeometryId, id);
  };
  accountGeometryIds(lineIds_);
  accountGeometryIds(circleIds_);
  accountGeometryIds(arcIds_);
  accountGeometryIds(bezierIds_);

  ConstraintId maximumConstraintId = kInvalidConstraintId;
  for (const auto& constraint : constraints_)
    maximumConstraintId = std::max(maximumConstraintId, constraint.id);
  DimensionId maximumDimensionId = kInvalidDimensionId;
  for (const auto& dimension : dimensions_)
    maximumDimensionId = std::max(maximumDimensionId, dimension.id);

  if (nextElementId == 0 || nextGeometryId == kInvalidGeometryId ||
      nextConstraintId == kInvalidConstraintId ||
      nextDimensionId == kInvalidDimensionId ||
      nextElementId <= maximumElementId ||
      nextGeometryId <= maximumGeometryId ||
      nextConstraintId <= maximumConstraintId ||
      nextDimensionId <= maximumDimensionId)
    return false;
  nextElementId_ = nextElementId;
  nextGeometryId_ = nextGeometryId;
  nextConstraintId_ = nextConstraintId;
  nextDimensionId_ = nextDimensionId;
  hasLastSolvedFingerprint_ = false;
  return true;
}

ConstraintId Sketch::addConstraint(
    Constraint constraint) {
  // Legacy structural builders retain the cheaper transactional solve, while
  // the public interactive boundary below owns full rank classification.
  // Narrow this path fail-closed: unsupported types, malformed schemas and
  // exact semantic duplicates never allocate an ID or enter the solver.
  const auto type = static_cast<ConstraintType>(constraint.type);
  if (!isProductionConstraintType(type)) return kInvalidConstraintId;
  canonicalizeConstraint(constraint);
  if (constraint.id != kInvalidConstraintId ||
      !validateConstraintSchema(*this, constraint) ||
      nextConstraintId_ == kInvalidConstraintId ||
      nextConstraintId_ == std::numeric_limits<ConstraintId>::max() ||
      constraintIndex(nextConstraintId_))
    return kInvalidConstraintId;

  const auto duplicate = std::find_if(
      constraints_.begin(), constraints_.end(),
      [&constraint](const Constraint& old) {
        return participatesInConstraintSystem(old) &&
               participatesInConstraintSystem(constraint) &&
               equivalentConstraint(old, constraint);
      });
  if (duplicate != constraints_.end()) return kInvalidConstraintId;

  ConstraintDiagnostics before;
  std::vector<ConstraintId> previouslySatisfied;

  beginDeltaJournal();
  if (failNextNestedConstraintJournal.exchange(false))
    throw std::runtime_error("injected nested journal failure");
  before = analyzeConstraintSystem(*this, false);
  for (const auto& old : constraints_)
    if (!hasConstraintViolation(before, old.id))
      previouslySatisfied.push_back(old.id);

  journalCaptureComponents(
      {constraint.firstGeometry, constraint.secondGeometry});
  journalCapturePoint(constraint.firstPoint);
  journalCapturePoint(constraint.secondPoint);

  constraint.id = nextConstraintId_++;
  const ConstraintId addedId = constraint.id;

  constraints_.push_back(constraint);
  journalRecordAddedConstraint(addedId, constraints_.size() - 1);
  invalidateStructureIndexes();
  static_cast<void>(SketchSolver::solveStableComponent(
      *this, referencedGeometryIds(*this, constraint), 16, addedId));

  const auto after = analyzeConstraintSystem(*this, false);
  bool oldConstraintBroken = false;
  for (const auto oldId : previouslySatisfied) {
    if (hasConstraintViolation(after, oldId)) {
      oldConstraintBroken = true;
      break;
    }
  }

  if (hasConstraintViolation(after, addedId) || oldConstraintBroken) {
    static_cast<void>(cancelDeltaJournal());
    return kInvalidConstraintId;
  }
  static_cast<void>(finishDeltaJournal());
  return addedId;
}

ConstraintApplyResult Sketch::tryApplyConstraint(Constraint constraint,
                                                 bool commitRedundant) {
  ConstraintApplyResult result;
  const auto reject = [&result](ConstraintApplyStatus status,
                                SketchMutationDiagnosticCode code,
                                std::string diagnostic) {
    result.status = status;
    result.diagnosticCode = code;
    result.diagnostic = std::move(diagnostic);
    return result;
  };

  const auto type = static_cast<ConstraintType>(constraint.type);
  if (!isProductionConstraintType(type) || !std::isfinite(constraint.value))
    return reject(ConstraintApplyStatus::Unsupported,
                  SketchMutationDiagnosticCode::UnsupportedConstraint,
                  "Unsupported constraint type or value");

  canonicalizeConstraint(constraint);

  if ((constraint.firstGeometry != kInvalidGeometryId &&
       !geometryLocation(constraint.firstGeometry)) ||
      (constraint.secondGeometry != kInvalidGeometryId &&
       !geometryLocation(constraint.secondGeometry)) ||
      (!emptyPointReference(constraint.firstPoint) &&
       !validPointReferenceForConstraint(*this, constraint.firstPoint)) ||
      (!emptyPointReference(constraint.secondPoint) &&
       !validPointReferenceForConstraint(*this, constraint.secondPoint)) ||
      (constraint.id != kInvalidConstraintId &&
       constraintIndex(constraint.id)))
    return reject(ConstraintApplyStatus::InvalidReference,
                  SketchMutationDiagnosticCode::InvalidGeometryReference,
                  "Constraint contains a stale or malformed reference");

  if (!supportedConstraintSchema(*this, constraint))
    return reject(ConstraintApplyStatus::Unsupported,
                  SketchMutationDiagnosticCode::UnsupportedConstraint,
                  "Constraint geometry combination is not supported");

  const auto seeds = referencedGeometryIds(*this, constraint);
  if (seeds.empty())
    return reject(ConstraintApplyStatus::InvalidReference,
                  SketchMutationDiagnosticCode::InvalidGeometryReference,
                  "Constraint does not reference sketch geometry");

  const auto affectedBefore = connectedComponent(seeds);
  result.component = affectedBefore;
  result.relatedConstraintIds = affectedBefore.constraintIds;

  const auto duplicate = std::find_if(
      constraints_.begin(), constraints_.end(),
      [&constraint](const Constraint& old) {
        return participatesInConstraintSystem(old) &&
               participatesInConstraintSystem(constraint) &&
               equivalentConstraint(old, constraint);
      });
  if (duplicate != constraints_.end()) {
    result.status = ConstraintApplyStatus::Redundant;
    result.diagnosticCode = SketchMutationDiagnosticCode::None;
    result.relatedConstraintIds = {duplicate->id};
    result.diagnostic = "Equivalent constraint already exists";
    return result;
  }

  const auto sameGeometryPair = [&constraint](const Constraint& old) {
    return (old.firstGeometry == constraint.firstGeometry &&
            old.secondGeometry == constraint.secondGeometry) ||
           (old.firstGeometry == constraint.secondGeometry &&
            old.secondGeometry == constraint.firstGeometry);
  };
  for (const auto& old : constraints_) {
    if (!participatesInConstraintSystem(old) ||
        !participatesInConstraintSystem(constraint))
      continue;
    bool conflicts = false;
    if (old.firstGeometry == constraint.firstGeometry) {
      conflicts =
          (constraint.type == ConstraintType::Horizontal &&
           old.type == ConstraintType::Vertical) ||
          (constraint.type == ConstraintType::Vertical &&
           old.type == ConstraintType::Horizontal);
    }
    if (sameGeometryPair(old)) {
      if (constraint.type == ConstraintType::Parallel)
        conflicts = conflicts || old.type == ConstraintType::Perpendicular ||
                    (old.type == ConstraintType::Angle &&
                     std::abs(old.value) > 1e-9 &&
                     std::abs(old.value - 180.0) > 1e-9);
      else if (constraint.type == ConstraintType::Perpendicular)
        conflicts = conflicts || old.type == ConstraintType::Parallel ||
                    (old.type == ConstraintType::Angle &&
                     std::abs(old.value - 90.0) > 1e-9);
      else if (constraint.type == ConstraintType::Angle)
        conflicts = conflicts ||
                    (old.type == ConstraintType::Parallel &&
                     std::abs(constraint.value) > 1e-9 &&
                     std::abs(constraint.value - 180.0) > 1e-9) ||
                    (old.type == ConstraintType::Perpendicular &&
                     std::abs(constraint.value - 90.0) > 1e-9);
    }
    if (conflicts) {
      result.status = ConstraintApplyStatus::Conflicting;
      result.diagnosticCode =
          SketchMutationDiagnosticCode::ConstraintResidualViolation;
      result.relatedConstraintIds = {old.id};
      result.diagnostic =
          "Constraint conflicts with an existing orientation relationship";
      return result;
    }
  }
  const auto before = analyzeConstraintSystem(*this, true);
  std::vector<ConstraintId> previouslySatisfied;
  for (const auto& old : constraints_)
    if (!hasConstraintViolation(before, old.id))
      previouslySatisfied.push_back(old.id);

  const auto originalFingerprint = semanticFingerprint();
  beginDeltaJournal();
  if (failNextNestedConstraintJournal.exchange(false))
    throw std::runtime_error("injected nested journal failure");
  journalCaptureComponents(seeds);
  journalCapturePoint(constraint.firstPoint);
  journalCapturePoint(constraint.secondPoint);

  if (constraint.id == kInvalidConstraintId)
    constraint.id = nextConstraintId_++;
  else
    nextConstraintId_ = std::max(nextConstraintId_, constraint.id + 1);
  result.constraintId = constraint.id;
  constraints_.push_back(constraint);
  journalRecordAddedConstraint(constraint.id, constraints_.size() - 1);
  invalidateStructureIndexes();

  const SolveResult solved =
      SketchSolver::solveStableComponent(*this, seeds, 16,
                                              constraint.id);
  result.solveResult = solved;
  const auto after = analyzeConstraintSystem(*this, true);
  bool oldConstraintBroken = false;
  for (const auto id : previouslySatisfied) {
    if (hasConstraintViolation(after, id)) {
      oldConstraintBroken = true;
      break;
    }
  }

  ConstraintApplyStatus failure = ConstraintApplyStatus::Accepted;
  std::string diagnostic;
  if (solved.invalidReferences != 0) {
    failure = ConstraintApplyStatus::InvalidReference;
    result.diagnosticCode =
        SketchMutationDiagnosticCode::InvalidGeometryReference;
    diagnostic = "Solver rejected a constraint reference";
  } else if (solved.unsupported != 0) {
    failure = ConstraintApplyStatus::Unsupported;
    result.diagnosticCode =
        SketchMutationDiagnosticCode::UnsupportedConstraint;
    diagnostic = "Solver does not support this constraint";
  } else if (hasConstraintViolation(after, constraint.id) ||
             oldConstraintBroken) {
    failure = ConstraintApplyStatus::Conflicting;
    result.diagnosticCode =
        SketchMutationDiagnosticCode::ConstraintResidualViolation;
    diagnostic = "Constraint conflicts with existing dependencies";
  } else if (!solved.converged) {
    failure = ConstraintApplyStatus::SolverFailed;
    result.diagnosticCode =
        SketchMutationDiagnosticCode::SolverDidNotConverge;
    diagnostic = "Constraint solver did not converge";
  } else if (after.equationRank <= before.equationRank) {
    failure = ConstraintApplyStatus::Redundant;
    result.diagnosticCode = SketchMutationDiagnosticCode::None;
    diagnostic = "Constraint does not add an independent equation";
  }

  if (failure == ConstraintApplyStatus::Redundant && commitRedundant) {
    static_cast<void>(finishDeltaJournal());
    updateBounds();
    result.status = failure;
    result.diagnostic = std::move(diagnostic);
    return result;
  }

  if (failure != ConstraintApplyStatus::Accepted) {
    static_cast<void>(cancelDeltaJournal());
    result.constraintId = kInvalidConstraintId;
    result.status = failure;
    result.diagnostic = std::move(diagnostic);
    if (semanticFingerprint() != originalFingerprint)
      result.diagnostic += "; rollback verification failed";
    return result;
  }

  static_cast<void>(finishDeltaJournal());
  updateBounds();
  result.status = ConstraintApplyStatus::Accepted;
  result.diagnosticCode = SketchMutationDiagnosticCode::None;
  result.diagnostic = "Constraint accepted";
  return result;
}

bool Sketch::restoreConstraints(std::vector<Constraint> constraints) {
  std::unordered_set<ConstraintId> ids;
  std::unordered_map<ConstraintId, ConstraintType> candidateTypes;
  for (auto& constraint : constraints) {
    const auto type = static_cast<ConstraintType>(constraint.type);
    if (!isProductionConstraintType(type))
      return false;
    canonicalizeConstraint(constraint);
    if (constraint.id == kInvalidConstraintId ||
        !ids.insert(constraint.id).second ||
        !validateConstraintSchema(*this, constraint))
      return false;
    candidateTypes.emplace(constraint.id, type);
  }
  for (const auto& dimension : dimensions_) {
    if (dimension.constraintId == kInvalidConstraintId) return false;
    const auto candidate = candidateTypes.find(dimension.constraintId);
    if (candidate == candidateTypes.end() ||
        !annotatableConstraintType(candidate->second))
      return false;
  }
  // Persistence restores the serialized pose verbatim. A solve is deliberately
  // not committed here: older files may contain a valid but currently
  // unsatisfied system, and first open must never become an implicit geometry
  // migration. Validate a disposable candidate so constraints that can only
  // produce non-finite or degenerate geometry remain rejected.
  Sketch validationCandidate = *this;
  validationCandidate.constraints_ = constraints;
  validationCandidate.invalidateStructureIndexes();
  validationCandidate.nextConstraintId_ = 1;
  for (const auto& constraint : validationCandidate.constraints_)
    validationCandidate.nextConstraintId_ = std::max(
        validationCandidate.nextConstraintId_, constraint.id + 1);
  const SolveResult validation = SketchSolver::solveStable(validationCandidate);
  if (validation.nonFiniteGeometry != 0 || validation.degenerateGeometry != 0)
    return false;
  constraints_ = std::move(constraints);
  invalidateStructureIndexes();
  nextConstraintId_ = validationCandidate.nextConstraintId_;
  // Diagnostics and the next explicit edit use the same equation kernel later.
  updateBounds();
  return true;
}

SketchMutationResult Sketch::setConstraintValue(ConstraintId id,
                                                double value) {
  if (id == kInvalidConstraintId)
    return rejectedMutation(
        *this, {}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (!std::isfinite(value) || value <= 0.0)
    return rejectedMutation(*this, {}, SketchMutationStatus::Unsupported,
                            SketchMutationDiagnosticCode::UnsupportedConstraint);
  const auto found = std::find_if(
      constraints_.begin(), constraints_.end(),
      [id](const Constraint& constraint) { return constraint.id == id; });
  if (found == constraints_.end())
    return rejectedMutation(
        *this, {}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);
  if (!participatesInConstraintSystem(*found))
    return rejectedMutation(*this, {}, SketchMutationStatus::Unsupported,
                            SketchMutationDiagnosticCode::UnsupportedConstraint);
  const auto seeds = referencedGeometryIds(*this, *found);
  if (std::abs(found->value - value) <= 1e-12)
    return noMutation(*this, seeds);

  const auto journalDepthBefore = deltaJournalDepth();
  const bool ownsJournal = journalDepthBefore == 0;
  if (ownsJournal) beginDeltaJournal();
  journalCaptureComponents(
      {found->firstGeometry, found->secondGeometry});
  journalCapturePoint(found->firstPoint);
  journalCapturePoint(found->secondPoint);
  journalCaptureConstraint(id);
  const auto before = analyzeConstraintSystem(*this, false);
  std::vector<ConstraintId> previouslySatisfied;
  for (const auto& old : constraints_)
    if (!hasConstraintViolation(before, old.id))
      previouslySatisfied.push_back(old.id);
  found->value = value;
  hasLastSolvedFingerprint_ = false;
  const auto solved = SketchSolver::solveStable(*this);
  auto mutation = mutationFromSolve(*this, seeds, solved);
  const auto after = analyzeConstraintSystem(*this, false);

  bool previouslyValidConstraintBroke = false;
  for (const auto oldId : previouslySatisfied) {
    if (hasConstraintViolation(after, oldId)) {
      previouslyValidConstraintBroke = true;
      break;
    }
  }
  if (hasConstraintViolation(after, id) || previouslyValidConstraintBroke) {
    mutation.status = SketchMutationStatus::Conflicting;
    mutation.diagnosticCode =
        SketchMutationDiagnosticCode::ConstraintResidualViolation;
  }
  if (!mutation.committed()) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return mutation;
  }
  if (ownsJournal) static_cast<void>(finishDeltaJournal());
  updateBounds();
  return mutation;
}

SketchMutationResult Sketch::setConstraintMode(
    ConstraintId id, ConstraintMode mode,
    std::optional<double> drivingValue) {
  const auto found = std::find_if(
      constraints_.begin(), constraints_.end(),
      [id](const Constraint& constraint) { return constraint.id == id; });
  if (found == constraints_.end())
    return rejectedMutation(
        *this, {}, SketchMutationStatus::InvalidReference,
        SketchMutationDiagnosticCode::InvalidGeometryReference);

  Constraint candidate = *found;
  candidate.mode = mode;
  if (mode == ConstraintMode::Reference) {
    if (hasDrivingValue(candidate.payload()) &&
        !assignDrivingValue(candidate.payload(), std::nullopt))
      return rejectedMutation(
          *this, {}, SketchMutationStatus::Unsupported,
          SketchMutationDiagnosticCode::UnsupportedConstraint);
  } else if (mode == ConstraintMode::Driving) {
    if (hasDrivingValue(candidate.payload()) &&
        (!drivingValue || !std::isfinite(*drivingValue) ||
         *drivingValue <= 0.0 ||
         !assignDrivingValue(candidate.payload(), drivingValue)))
      return rejectedMutation(
          *this, {}, SketchMutationStatus::Unsupported,
          SketchMutationDiagnosticCode::UnsupportedConstraint);
  }
  if (!validateConstraintSchema(*this, candidate))
    return rejectedMutation(
        *this, referencedGeometryIds(*this, candidate),
        SketchMutationStatus::Unsupported,
        SketchMutationDiagnosticCode::UnsupportedConstraint);
  if (found->mode == candidate.mode &&
      static_cast<double>(found->value) ==
          static_cast<double>(candidate.value))
    return noMutation(*this, referencedGeometryIds(*this, candidate));

  const auto seeds = referencedGeometryIds(*this, candidate);
  const auto journalDepthBefore = deltaJournalDepth();
  const bool ownsJournal = journalDepthBefore == 0;
  if (ownsJournal) beginDeltaJournal();
  journalCaptureComponents(seeds);
  journalCaptureConstraint(id);
  *found = candidate;
  invalidateStructureIndexes();
  hasLastSolvedFingerprint_ = false;

  SketchMutationResult mutation =
      mutationResult(*this, seeds, SketchMutationStatus::Applied,
                     SketchMutationDiagnosticCode::None);
  if (participatesInConstraintSystem(candidate)) {
    const auto solved = SketchSolver::solveStable(*this);
    mutation = mutationFromSolve(*this, seeds, solved);
    const auto diagnostics = analyzeConstraintSystem(*this, false);
    if (hasConstraintViolation(diagnostics, id)) {
      mutation.status = SketchMutationStatus::Conflicting;
      mutation.diagnosticCode =
          SketchMutationDiagnosticCode::ConstraintResidualViolation;
    }
  }
  if (!mutation.committed()) {
    if (ownsJournal)
      static_cast<void>(rollbackDeltaJournalsToDepth(journalDepthBefore));
    return mutation;
  }
  if (ownsJournal) static_cast<void>(finishDeltaJournal());
  updateBounds();
  return mutation;
}

bool Sketch::removeConstraint(ConstraintId id) {
  const auto found = constraintIndex(id);
  if (!found) return false;
  for (std::size_t index = dimensions_.size(); index > 0; --index) {
    if (dimensions_[index - 1].constraintId == id &&
        !removeDimension(index - 1))
      return false;
  }
  journalCaptureConstraint(id);
  std::erase_if(constraints_, [id](const Constraint& constraint) {
    return constraint.id == id;
  });
  invalidateStructureIndexes();
  return true;
}

void Sketch::clearConstraints() {
  clearDimensions();
  for (const auto& item : constraints_) journalCaptureConstraint(item.id);
  constraints_.clear();
  invalidateStructureIndexes();
}

const std::vector<Constraint>& Sketch::constraints() const noexcept {
  return constraints_;
}

double Sketch::widthMm() const noexcept { return widthMm_; }
double Sketch::heightMm() const noexcept { return heightMm_; }
const std::vector<Line>& Sketch::lines() const noexcept { return lines_; }
const std::vector<Circle>& Sketch::circles() const noexcept { return circles_; }
const std::vector<Arc>& Sketch::arcs() const noexcept { return arcs_; }
const std::vector<Bezier>& Sketch::beziers() const noexcept { return beziers_; }
const std::vector<Dimension>& Sketch::dimensions() const {
  for (auto& dimension : dimensions_) {
    if (dimension.constraintId == kInvalidConstraintId) continue;
    const auto linked = constraintIndex(dimension.constraintId);
    if (!linked) continue;
    const auto& constraint = constraints_[*linked];
    const auto type = static_cast<ConstraintType>(constraint.type);
    dimension.geometryId = kInvalidGeometryId;
    dimension.firstPoint = PointReference{DatumOriginReference{}};
    dimension.secondPoint = PointReference{DatumOriginReference{}};
    const auto lineLength = [this](GeometryId id) -> std::optional<double> {
      const auto index = lineIndex(id);
      if (!index) return std::nullopt;
      const auto& line = lines_[*index];
      return std::hypot(line.end.xMm - line.start.xMm,
                        line.end.yMm - line.start.yMm);
    };
    std::optional<double> measured;
    switch (type) {
      case ConstraintType::Length:
        dimension.kind = DimensionKind::LineLength;
        dimension.geometryId = constraint.firstGeometry;
        measured = lineLength(constraint.firstGeometry);
        break;
      case ConstraintType::Diameter: {
        dimension.kind = DimensionKind::CircleDiameter;
        dimension.geometryId = constraint.firstGeometry;
        const auto index = circleIndex(constraint.firstGeometry);
        if (index) measured = circles_[*index].radiusMm * 2.0;
        break;
      }
      case ConstraintType::Radius: {
        dimension.kind = DimensionKind::CircleRadius;
        dimension.geometryId = constraint.firstGeometry;
        const auto index = circleIndex(constraint.firstGeometry);
        if (index) measured = circles_[*index].radiusMm;
        break;
      }
      case ConstraintType::Distance:
      case ConstraintType::DistanceX:
      case ConstraintType::DistanceY: {
        dimension.kind = type == ConstraintType::DistanceX
            ? DimensionKind::PointDistanceX
            : type == ConstraintType::DistanceY
                  ? DimensionKind::PointDistanceY
                  : DimensionKind::PointDistance;
        dimension.firstPoint = constraint.firstPoint;
        dimension.secondPoint = constraint.secondPoint;
        const auto first = referencedPoint(dimension.firstPoint);
        const auto second = referencedPoint(dimension.secondPoint);
        if (first && second) {
          if (type == ConstraintType::DistanceX)
            measured = std::abs(second->xMm - first->xMm);
          else if (type == ConstraintType::DistanceY)
            measured = std::abs(second->yMm - first->yMm);
          else
            measured = std::hypot(second->xMm - first->xMm,
                                  second->yMm - first->yMm);
        }
        break;
      }
      case ConstraintType::Angle:
      case ConstraintType::LineDistance: {
        dimension.kind = type == ConstraintType::Angle
            ? DimensionKind::LineAngle : DimensionKind::LineDistance;
        dimension.geometryId = constraint.firstGeometry;
        dimension.secondPoint = PointReference{
            LineEndpointReference{constraint.secondGeometry,
                                  Endpoint::Start}};
        const auto first = lineIndex(constraint.firstGeometry);
        const auto second = lineIndex(constraint.secondGeometry);
        if (first && second) {
          const auto& a = lines_[*first];
          const auto& b = lines_[*second];
          const double ax = a.end.xMm - a.start.xMm;
          const double ay = a.end.yMm - a.start.yMm;
          const double bx = b.end.xMm - b.start.xMm;
          const double by = b.end.yMm - b.start.yMm;
          if (type == ConstraintType::Angle) {
            const double lengths = std::hypot(ax, ay) * std::hypot(bx, by);
            if (lengths > 1e-12)
              measured = std::acos(std::clamp((ax * bx + ay * by) / lengths,
                                              -1.0, 1.0)) *
                         180.0 / std::numbers::pi;
          } else {
            const double length = std::hypot(ax, ay);
            if (length > 1e-12)
              measured = std::abs(
                  ax * (b.start.yMm - a.start.yMm) -
                  ay * (b.start.xMm - a.start.xMm)) / length;
          }
        }
        break;
      }
      default:
        continue;
    }
    if (participatesInConstraintSystem(constraint))
      dimension.valueMm = static_cast<double>(constraint.value);
    else if (measured)
      dimension.valueMm = *measured;
  }
  return dimensions_;
}

std::optional<double> Sketch::dimensionValue(std::size_t index) const noexcept {
  if (index >= dimensions_.size()) return std::nullopt;
  static_cast<void>(dimensions());
  const double value = dimensions_[index].valueMm;
  return std::isfinite(value) && value >= 0.0
      ? std::optional<double>{value} : std::nullopt;
}

void Sketch::updateBounds() noexcept {
  hasLastSolvedFingerprint_ = false;
  connectivityDirty_ = true;
  if (lines_.empty() && circles_.empty() && arcs_.empty() && beziers_.empty()) {
    widthMm_ = 0.0;
    heightMm_ = 0.0;
    return;
  }
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  auto include = [&](Point point) {
    minX = std::min(minX, point.xMm);
    minY = std::min(minY, point.yMm);
    maxX = std::max(maxX, point.xMm);
    maxY = std::max(maxY, point.yMm);
  };
  for (const auto& line : lines_) {
    include(line.start);
    include(line.end);
  }
  for (const auto& circle : circles_) {
    include({circle.center.xMm - circle.radiusMm,
             circle.center.yMm - circle.radiusMm});
    include({circle.center.xMm + circle.radiusMm,
             circle.center.yMm + circle.radiusMm});
  }

  constexpr double kPi = 3.14159265358979323846;
  const auto normalizedAngle = [](double angle) {
    constexpr double twoPi = 6.28318530717958647692;
    angle = std::fmod(angle, twoPi);
    if (angle < 0.0) angle += twoPi;
    return angle;
  };
  const auto angleOnArc = [&](double angle, const Arc& arc) {
    const double start = normalizedAngle(arc.startAngleRad);
    const double delta = normalizedAngle(normalizedAngle(angle) - start);
    return delta <= arc.sweepAngleRad + 1e-12;
  };

  for (const auto& arc : arcs_) {
    include(arcStartPoint(arc));
    include(arcEndPoint(arc));
    for (const double angle : {0.0, 0.5 * kPi, kPi, 1.5 * kPi}) {
      if (!angleOnArc(angle, arc)) continue;
      include({arc.center.xMm + arc.radiusMm * std::cos(angle),
               arc.center.yMm + arc.radiusMm * std::sin(angle)});
    }
  }
  // The convex hull of a cubic Bezier's four control points contains the
  // complete curve, so it is a safe and deterministic sketch bound.
  for (const auto& bezier : beziers_)
    for (const auto point : bezier.points) include(point);
  widthMm_ = maxX - minX;
  heightMm_ = maxY - minY;
}

bool Sketch::isClosed() const noexcept {
  const auto samePoint = [](Point first, Point second) {
    // Match the wire builder's modelling tolerance. Interactive snapping and
    // imported decimal coordinates may leave sub-micron endpoint noise that
    // is topologically closed for OCCT and must not be rejected here first.
    return std::abs(first.xMm - second.xMm) <= 1e-6 &&
           std::abs(first.yMm - second.yMm) <= 1e-6;
  };

  std::vector<std::pair<Point, Point>> edges;
  for (const auto& line : lines_) {
    if (!line.dashed) edges.push_back({line.start, line.end});
  }
  for (const auto& arc : arcs_) {
    if (!arc.dashed) edges.push_back({arcStartPoint(arc), arcEndPoint(arc)});
  }
  for (const auto& bezier : beziers_) {
    if (!bezier.dashed)
      edges.push_back({bezier.points.front(), bezier.points.back()});
  }

  const std::size_t solidCircles = static_cast<std::size_t>(std::count_if(
      circles_.begin(), circles_.end(),
      [](const Circle& circle) { return !circle.dashed; }));

  if (edges.empty()) return solidCircles == 1;
  if (solidCircles != 0) return false;

  // Every vertex of one or several independent closed loops has degree two.
  for (const auto& edge : edges) {
    for (const Point vertex : {edge.first, edge.second}) {
      std::size_t degree = 0;
      for (const auto& candidate : edges) {
        if (samePoint(vertex, candidate.first)) ++degree;
        if (samePoint(vertex, candidate.second)) ++degree;
      }
      if (degree != 2) return false;
    }
  }
  return true;
}

}  // namespace solidar::sketch
