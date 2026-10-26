#include "sketch/SketchSchema.h"

#include <cmath>
#include <type_traits>
#include <unordered_set>

namespace solidar::sketch {
namespace {

SketchSchemaValidation valid() { return {}; }

SketchSchemaValidation geometryError(SketchSchemaErrorCode code,
                                     ConstraintId constraintId,
                                     std::initializer_list<GeometryId> ids) {
  SketchSchemaError error;
  error.code = code;
  for (const auto id : ids)
    if (id != kInvalidGeometryId) error.geometryIds.push_back(id);
  if (constraintId != kInvalidConstraintId)
    error.constraintIds.push_back(constraintId);
  return {std::move(error)};
}

SketchSchemaValidation pointError(const PointReference& reference,
                                  SketchSchemaErrorCode code) {
  SketchSchemaError error;
  error.code = code;
  std::visit([&](const auto& source) {
    using T = std::decay_t<decltype(source)>;
    if constexpr (std::is_same_v<T, LineEndpointReference>)
      error.geometryIds.push_back(source.lineId);
    else if constexpr (std::is_same_v<T, CircleCenterReference>)
      error.geometryIds.push_back(source.circleId);
    else if constexpr (std::is_same_v<T, ArcEndpointReference>)
      error.geometryIds.push_back(source.arcId);
    else if constexpr (std::is_same_v<T, BezierPointReference>)
      error.geometryIds.push_back(source.bezierId);
  }, reference.payload());
  return {std::move(error)};
}

bool isDimensional(ConstraintType type) noexcept {
  switch (type) {
    case ConstraintType::Distance:
    case ConstraintType::Length:
    case ConstraintType::Radius:
    case ConstraintType::Diameter:
    case ConstraintType::Angle:
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY:
    case ConstraintType::LineDistance:
      return true;
    default:
      return false;
  }
}

std::optional<double> drivingValue(const ConstraintPayload& payload) {
  return std::visit([](const auto& value) -> std::optional<double> {
    if constexpr (requires { value.drivingValue; })
      return value.drivingValue;
    return std::nullopt;
  }, payload);
}

}  // namespace

SketchSchemaValidation validatePointReferenceSchema(
    const Sketch& sketch, const PointReference& reference) {
  return std::visit([&](const auto& source) -> SketchSchemaValidation {
    using T = std::decay_t<decltype(source)>;
    if constexpr (std::is_same_v<T, DatumOriginReference>) {
      return valid();
    } else if constexpr (std::is_same_v<T, LineEndpointReference>) {
      if (source.lineId != kInvalidGeometryId && sketch.lineIndex(source.lineId))
        return valid();
    } else if constexpr (std::is_same_v<T, CircleCenterReference>) {
      if (source.circleId != kInvalidGeometryId &&
          sketch.circleIndex(source.circleId))
        return valid();
    } else if constexpr (std::is_same_v<T, ArcEndpointReference>) {
      if (source.arcId != kInvalidGeometryId && sketch.arcIndex(source.arcId))
        return valid();
    } else if constexpr (std::is_same_v<T, BezierPointReference>) {
      if (source.bezierId != kInvalidGeometryId && source.pointIndex <= 3 &&
          sketch.bezierIndex(source.bezierId))
        return valid();
      if (source.pointIndex > 3)
        return pointError(reference, SketchSchemaErrorCode::InvalidParameterRange);
    } else if constexpr (std::is_same_v<T, CompositeCenterReference>) {
      if (source.elementId != 0 &&
          sketch.elementCenterPoint(static_cast<std::size_t>(source.elementId)))
        return valid();
    }
    return pointError(reference, SketchSchemaErrorCode::StaleGeometryReference);
  }, reference.payload());
}

SketchSchemaValidation validateConstraintSchema(
    const Sketch& sketch, const Constraint& constraint) {
  const auto type = static_cast<ConstraintType>(constraint.type);
  if (!isProductionConstraintType(type))
    return geometryError(SketchSchemaErrorCode::UnsupportedConstraintType,
                         constraint.id, {});
  const int mode = static_cast<int>(constraint.mode);
  if (mode < static_cast<int>(ConstraintMode::Driving) ||
      mode > static_cast<int>(ConstraintMode::Disabled))
    return geometryError(SketchSchemaErrorCode::UnsupportedConstraintMode,
                         constraint.id, {});
  if (constraint.mode == ConstraintMode::Reference && !isDimensional(type))
    return geometryError(SketchSchemaErrorCode::UnsupportedConstraintMode,
                         constraint.id, {});

  const auto line = [&](GeometryId id) {
    return id != kInvalidGeometryId && sketch.lineIndex(id).has_value();
  };
  const auto circle = [&](GeometryId id) {
    return id != kInvalidGeometryId && sketch.circleIndex(id).has_value();
  };
  const auto arc = [&](GeometryId id) {
    return id != kInvalidGeometryId && sketch.arcIndex(id).has_value();
  };
  const auto geometry = [&](GeometryId id) {
    return id != kInvalidGeometryId && sketch.geometryLocation(id).has_value();
  };
  const auto point = [&](const PointReference& reference) {
    return validatePointReferenceSchema(sketch, reference).valid();
  };
  const auto positive = [&](const std::optional<double>& value) {
    return constraint.mode == ConstraintMode::Reference
        ? !value.has_value()
        : constraint.mode == ConstraintMode::Disabled
              ? !value.has_value() ||
                    (std::isfinite(*value) && *value > 0.0)
        : value && std::isfinite(*value) && *value > 0.0;
  };

  const auto pointValidation = [&](const PointReference& reference)
      -> std::optional<SketchSchemaValidation> {
    auto result = validatePointReferenceSchema(sketch, reference);
    if (result) return std::nullopt;
    if (constraint.id != kInvalidConstraintId)
      result.error->constraintIds.push_back(constraint.id);
    return result;
  };

  switch (type) {
    case ConstraintType::Coincident:
    case ConstraintType::Distance:
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY:
      if (auto result = pointValidation(constraint.firstPoint)) return *result;
      if (auto result = pointValidation(constraint.secondPoint)) return *result;
      if (static_cast<PointReference>(constraint.firstPoint) ==
          static_cast<PointReference>(constraint.secondPoint))
        return geometryError(SketchSchemaErrorCode::SelfReference,
                             constraint.id, {});
      break;
    case ConstraintType::PointOnLine:
    case ConstraintType::PointOnCircle:
    case ConstraintType::PointOnArc:
    case ConstraintType::Midpoint:
    case ConstraintType::PointOnXAxis:
    case ConstraintType::PointOnYAxis:
      if (auto result = pointValidation(constraint.secondPoint)) return *result;
      break;
    case ConstraintType::Parallel:
    case ConstraintType::Perpendicular:
    case ConstraintType::Equal:
    case ConstraintType::Angle:
    case ConstraintType::Tangent:
    case ConstraintType::LineDistance:
      if (static_cast<GeometryId>(constraint.firstGeometry) ==
              static_cast<GeometryId>(constraint.secondGeometry) &&
          static_cast<GeometryId>(constraint.firstGeometry) !=
              kInvalidGeometryId)
        return geometryError(SketchSchemaErrorCode::SelfReference,
                             constraint.id,
                             {static_cast<GeometryId>(constraint.firstGeometry)});
      break;
    default:
      break;
  }

  if (isDimensional(type)) {
    const auto value = drivingValue(constraint.payload());
    const bool invalidValue =
        constraint.mode == ConstraintMode::Reference
            ? value.has_value()
        : constraint.mode == ConstraintMode::Disabled
            ? value && (!std::isfinite(*value) || *value <= 0.0)
            : (!value || !std::isfinite(*value) || *value <= 0.0);
    if (invalidValue ||
        (type == ConstraintType::Angle && value && *value >= 180.0))
      return geometryError(SketchSchemaErrorCode::InvalidParameterRange,
                           constraint.id, {});
  }

  const bool ok = std::visit([&](const auto& payload) {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, HorizontalConstraint> ||
                  std::is_same_v<T, VerticalConstraint>)
      return line(payload.lineId);
    else if constexpr (std::is_same_v<T, CoincidentConstraint>)
      return point(payload.first) && point(payload.second) &&
             payload.first != payload.second;
    else if constexpr (std::is_same_v<T, PointOnLineConstraint> ||
                       std::is_same_v<T, MidpointConstraint>)
      return line(payload.lineId) && point(payload.point);
    else if constexpr (std::is_same_v<T, DistanceConstraint> ||
                       std::is_same_v<T, DistanceXConstraint> ||
                       std::is_same_v<T, DistanceYConstraint>)
      return point(payload.first) && point(payload.second) &&
             payload.first != payload.second && positive(payload.drivingValue);
    else if constexpr (std::is_same_v<T, LengthConstraint>)
      return line(payload.lineId) && positive(payload.drivingValue);
    else if constexpr (std::is_same_v<T, RadiusConstraint> ||
                       std::is_same_v<T, DiameterConstraint>)
      return circle(payload.circleId) && positive(payload.drivingValue);
    else if constexpr (std::is_same_v<T, ParallelConstraint> ||
                       std::is_same_v<T, PerpendicularConstraint>)
      return line(payload.firstLineId) && line(payload.secondLineId) &&
             payload.firstLineId != payload.secondLineId;
    else if constexpr (std::is_same_v<T, EqualConstraint>) {
      const auto first = sketch.geometryLocation(payload.firstGeometryId);
      const auto second = sketch.geometryLocation(payload.secondGeometryId);
      return first && second && payload.firstGeometryId != payload.secondGeometryId &&
             first->kind == second->kind &&
             (first->kind == GeometryKind::Line ||
              first->kind == GeometryKind::Circle);
    } else if constexpr (std::is_same_v<T, AngleConstraint>)
      return line(payload.firstLineId) && line(payload.secondLineId) &&
             payload.firstLineId != payload.secondLineId &&
             positive(payload.drivingValue) &&
             (constraint.mode == ConstraintMode::Reference ||
              *payload.drivingValue < 180.0);
    else if constexpr (std::is_same_v<T, PointOnCircleConstraint>)
      return circle(payload.circleId) && point(payload.point);
    else if constexpr (std::is_same_v<T, TangentConstraint>) {
      const auto first = sketch.geometryLocation(payload.firstGeometryId);
      const auto second = sketch.geometryLocation(payload.secondGeometryId);
      return first && second &&
             payload.firstGeometryId != payload.secondGeometryId &&
             !(first->kind == GeometryKind::Line &&
               second->kind == GeometryKind::Line);
    }
    else if constexpr (std::is_same_v<T, LineDistanceConstraint>)
      return line(payload.firstLineId) && line(payload.secondLineId) &&
             payload.firstLineId != payload.secondLineId &&
             positive(payload.drivingValue);
    else if constexpr (std::is_same_v<T, LockConstraint>)
      return geometry(payload.geometryId);
    else if constexpr (std::is_same_v<T, PointOnArcConstraint>)
      return arc(payload.arcId) && point(payload.point);
    else if constexpr (std::is_same_v<T, PointOnXAxisConstraint> ||
                       std::is_same_v<T, PointOnYAxisConstraint>)
      return point(payload.point);
    else
      return false;
  }, constraint.payload());

  if (ok) return valid();
  return geometryError(SketchSchemaErrorCode::WrongGeometryKind,
                       constraint.id,
                       {static_cast<GeometryId>(constraint.firstGeometry),
                        static_cast<GeometryId>(constraint.secondGeometry)});
}

SketchSchemaValidation validateSketchSchema(const Sketch& sketch) {
  std::unordered_set<GeometryId> geometryIds;
  const auto addGeometry = [&](GeometryId id) -> SketchSchemaValidation {
    if (id == kInvalidGeometryId)
      return geometryError(SketchSchemaErrorCode::InvalidGeometryId,
                           kInvalidConstraintId, {});
    if (!geometryIds.insert(id).second)
      return geometryError(SketchSchemaErrorCode::DuplicateGeometryId,
                           kInvalidConstraintId, {id});
    return valid();
  };
  for (std::size_t index = 0; index < sketch.lines().size(); ++index)
    if (auto result = addGeometry(sketch.lineId(index)); !result) return result;
  for (std::size_t index = 0; index < sketch.circles().size(); ++index)
    if (auto result = addGeometry(sketch.circleId(index)); !result) return result;
  for (std::size_t index = 0; index < sketch.arcs().size(); ++index)
    if (auto result = addGeometry(sketch.arcId(index)); !result) return result;
  for (std::size_t index = 0; index < sketch.beziers().size(); ++index)
    if (auto result = addGeometry(sketch.bezierId(index)); !result) return result;

  std::unordered_set<ConstraintId> constraintIds;
  for (const auto& constraint : sketch.constraints()) {
    if (constraint.id == kInvalidConstraintId)
      return geometryError(SketchSchemaErrorCode::InvalidConstraintId,
                           constraint.id, {});
    if (!constraintIds.insert(constraint.id).second)
      return geometryError(SketchSchemaErrorCode::DuplicateConstraintId,
                           constraint.id, {});
    if (auto result = validateConstraintSchema(sketch, constraint); !result)
      return result;
  }

  std::unordered_set<DimensionId> dimensionIds;
  std::unordered_set<ConstraintId> annotatedConstraintIds;
  for (const auto& dimension : sketch.dimensions()) {
    if (dimension.id == kInvalidDimensionId) {
      SketchSchemaError error;
      error.code = SketchSchemaErrorCode::InvalidDimensionId;
      return {std::move(error)};
    }
    if (!dimensionIds.insert(dimension.id).second) {
      SketchSchemaError error;
      error.code = SketchSchemaErrorCode::DuplicateDimensionId;
      error.dimensionIds.push_back(dimension.id);
      return {std::move(error)};
    }
    if (dimension.constraintId == kInvalidConstraintId ||
        !sketch.constraintIndex(dimension.constraintId) ||
        !annotatedConstraintIds.insert(dimension.constraintId).second) {
      SketchSchemaError error;
      error.code = SketchSchemaErrorCode::StaleConstraintReference;
      error.constraintIds.push_back(dimension.constraintId);
      error.dimensionIds.push_back(dimension.id);
      return {std::move(error)};
    }
    const auto linked = sketch.constraintIndex(dimension.constraintId);
    const auto type = static_cast<ConstraintType>(
        sketch.constraints()[*linked].type);
    if (type != ConstraintType::Length &&
        type != ConstraintType::Radius &&
        type != ConstraintType::Diameter &&
        type != ConstraintType::Distance &&
        type != ConstraintType::DistanceX &&
        type != ConstraintType::DistanceY &&
        type != ConstraintType::Angle &&
        type != ConstraintType::LineDistance) {
      SketchSchemaError error;
      error.code = SketchSchemaErrorCode::WrongGeometryKind;
      error.constraintIds.push_back(dimension.constraintId);
      error.dimensionIds.push_back(dimension.id);
      return {std::move(error)};
    }
  }
  return valid();
}

}  // namespace solidar::sketch
