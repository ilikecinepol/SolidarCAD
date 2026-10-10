#include "ui/SketchCommandController.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace solidar {
namespace {

bool finite(double value) noexcept { return std::isfinite(value); }

bool finite(sketch::Point point) noexcept {
  return finite(point.xMm) && finite(point.yMm);
}

bool zeroTranslation(double dxMm, double dyMm) noexcept {
  return dxMm == 0.0 && dyMm == 0.0;
}

bool elementExists(const sketch::Sketch& model, std::size_t elementId) {
  return elementId != 0 &&
         std::any_of(model.lines().begin(), model.lines().end(),
                     [elementId](const sketch::Line& line) {
                       return line.elementId == elementId;
                     });
}

std::vector<sketch::GeometryId> elementGeometryIds(
    const sketch::Sketch& model, const std::vector<std::size_t>& elementIds) {
  std::vector<sketch::GeometryId> result;
  for (std::size_t index = 0; index < model.lines().size(); ++index) {
    if (std::find(elementIds.begin(), elementIds.end(),
                  model.lines()[index].elementId) != elementIds.end())
      result.push_back(model.lineId(index));
  }
  return result;
}

bool samePoint(sketch::Point first, sketch::Point second,
               double tolerance = 1e-6) noexcept {
  return std::hypot(first.xMm - second.xMm,
                    first.yMm - second.yMm) <= tolerance;
}

SketchCommandResult rejected(SketchCommandError error) {
  SketchCommandResult result;
  result.error = error;
  return result;
}

SketchCommandResult accepted(SketchCommandEffects effects = {}) {
  SketchCommandResult result;
  result.accepted = true;
  result.effects = effects;
  return result;
}

SketchCommandEffects constraintEffects();

SketchCommandResult fromConstraintApply(
    sketch::ConstraintApplyResult applied, bool redundantIsSuccess = false) {
  if (applied.accepted()) {
    auto result = accepted(constraintEffects());
    result.changedConstraintIds.push_back(applied.constraintId);
    result.constraintApplyResult = std::move(applied);
    return result;
  }
  if (redundantIsSuccess &&
      applied.status == sketch::ConstraintApplyStatus::Redundant) {
    auto result = accepted();
    result.constraintApplyResult = std::move(applied);
    return result;
  }
  SketchCommandError error = SketchCommandError::Conflict;
  switch (applied.status) {
    case sketch::ConstraintApplyStatus::Redundant:
      error = SketchCommandError::Duplicate;
      break;
    case sketch::ConstraintApplyStatus::InvalidReference:
      error = SketchCommandError::StaleReference;
      break;
    case sketch::ConstraintApplyStatus::Unsupported:
      error = SketchCommandError::Unsupported;
      break;
    case sketch::ConstraintApplyStatus::SolverFailed:
      error = SketchCommandError::SolverFailed;
      break;
    case sketch::ConstraintApplyStatus::Conflicting:
      error = SketchCommandError::Conflict;
      break;
    case sketch::ConstraintApplyStatus::Accepted:
      break;
  }
  auto result = rejected(error);
  result.constraintApplyResult = std::move(applied);
  return result;
}

sketch::ConstraintId tryApplyConstraintId(sketch::Sketch& model,
                                          sketch::Constraint constraint,
                                          bool commitRedundant = false) {
  const auto result = model.tryApplyConstraint(std::move(constraint),
                                               commitRedundant);
  return result.accepted()
             ? result.constraintId
             : sketch::kInvalidConstraintId;
}

SketchCommandEffects geometryEffects() {
  return {.committedRenderSceneDirty = true,
          .geometryChanged = true,
          .constraintsChanged = false,
          .dimensionsChanged = false,
          .selectionMayBeStale = false,
          .diagnosticsRequired = true};
}

SketchCommandEffects constraintEffects() {
  auto effects = geometryEffects();
  effects.constraintsChanged = true;
  return effects;
}

SketchCommandEffects dimensionEffects() {
  auto effects = geometryEffects();
  effects.dimensionsChanged = true;
  return effects;
}

bool validPointReference(const sketch::Sketch& model,
                         sketch::PointReference reference) {
  const int sourceCount = (reference.origin ? 1 : 0) +
                          (reference.elementCenterId != 0 ? 1 : 0) +
                          (reference.lineId != sketch::kInvalidGeometryId ? 1 : 0) +
                          (reference.circleId != sketch::kInvalidGeometryId ? 1 : 0) +
                          (reference.arcId != sketch::kInvalidGeometryId ? 1 : 0) +
                          (reference.bezierId != sketch::kInvalidGeometryId ? 1 : 0);
  if (sourceCount != 1) return false;
  if (reference.origin) return true;
  return model.referencedPoint(reference).has_value();
}

bool validConstraintReferences(const sketch::Sketch& model,
                               const sketch::Constraint& constraint) {
  const auto emptyPoint = [](sketch::PointReference point) {
    return !point.origin && point.elementCenterId == 0 &&
           point.lineId == sketch::kInvalidGeometryId &&
           point.circleId == sketch::kInvalidGeometryId &&
           point.arcId == sketch::kInvalidGeometryId &&
           point.bezierId == sketch::kInvalidGeometryId;
  };
  if (!finite(constraint.value)) return false;
  const auto first = model.geometryLocation(constraint.firstGeometry);
  const auto second = model.geometryLocation(constraint.secondGeometry);
  const bool firstPoint = !emptyPoint(constraint.firstPoint);
  const bool secondPoint = !emptyPoint(constraint.secondPoint);
  if ((firstPoint && !validPointReference(model, constraint.firstPoint)) ||
      (secondPoint && !validPointReference(model, constraint.secondPoint)))
    return false;
  const bool noPoints = !firstPoint && !secondPoint;
  const bool distinctGeometry =
      first && second && constraint.firstGeometry != constraint.secondGeometry;
  const auto firstIs = [&](sketch::GeometryKind kind) {
    return first && first->kind == kind;
  };
  const auto secondIs = [&](sketch::GeometryKind kind) {
    return second && second->kind == kind;
  };
  switch (constraint.type) {
    case sketch::ConstraintType::Horizontal:
    case sketch::ConstraintType::Vertical:
      return firstIs(sketch::GeometryKind::Line) && !second && noPoints;
    case sketch::ConstraintType::Length:
      return firstIs(sketch::GeometryKind::Line) && !second && noPoints &&
             constraint.value > 0.0;
    case sketch::ConstraintType::Radius:
    case sketch::ConstraintType::Diameter:
      return firstIs(sketch::GeometryKind::Circle) && !second && noPoints &&
             constraint.value > 0.0;
    case sketch::ConstraintType::Lock:
      return first.has_value() && !second && noPoints;
    case sketch::ConstraintType::Parallel:
    case sketch::ConstraintType::Perpendicular:
      return firstIs(sketch::GeometryKind::Line) &&
             secondIs(sketch::GeometryKind::Line) && distinctGeometry &&
             noPoints;
    case sketch::ConstraintType::Angle:
      return firstIs(sketch::GeometryKind::Line) &&
             secondIs(sketch::GeometryKind::Line) && distinctGeometry &&
             noPoints && constraint.value > 0.0 && constraint.value < 180.0;
    case sketch::ConstraintType::LineDistance:
      return firstIs(sketch::GeometryKind::Line) &&
             secondIs(sketch::GeometryKind::Line) && distinctGeometry &&
             noPoints && constraint.value > 0.0;
    case sketch::ConstraintType::Equal:
      return distinctGeometry && noPoints &&
             ((firstIs(sketch::GeometryKind::Line) &&
               secondIs(sketch::GeometryKind::Line)) ||
              (firstIs(sketch::GeometryKind::Circle) &&
               secondIs(sketch::GeometryKind::Circle)));
    case sketch::ConstraintType::Tangent:
      return firstIs(sketch::GeometryKind::Line) && distinctGeometry &&
             (secondIs(sketch::GeometryKind::Circle) ||
              secondIs(sketch::GeometryKind::Arc)) && noPoints;
    case sketch::ConstraintType::Coincident:
      return !first && !second && firstPoint && secondPoint;
    case sketch::ConstraintType::Distance:
    case sketch::ConstraintType::DistanceX:
    case sketch::ConstraintType::DistanceY:
      return !first && !second && firstPoint && secondPoint &&
             constraint.value > 0.0;
    case sketch::ConstraintType::Midpoint:
    case sketch::ConstraintType::PointOnLine:
      return firstIs(sketch::GeometryKind::Line) && !second && !firstPoint &&
             secondPoint;
    case sketch::ConstraintType::PointOnCircle:
      return firstIs(sketch::GeometryKind::Circle) && !second && !firstPoint &&
             secondPoint;
    case sketch::ConstraintType::PointOnArc:
      return firstIs(sketch::GeometryKind::Arc) && !second && !firstPoint &&
             secondPoint;
    case sketch::ConstraintType::PointOnXAxis:
    case sketch::ConstraintType::PointOnYAxis:
      return !first && !second && !firstPoint && secondPoint;
  }
  return false;
}

bool validDimensionReferences(const sketch::Sketch& model,
                              const sketch::Dimension& dimension) {
  if (!finite(dimension.valueMm) || !finite(dimension.offsetMm) ||
      !finite(dimension.angleRad))
    return false;
  switch (dimension.kind) {
    case sketch::DimensionKind::LineLength:
      return model.lineIndex(dimension.geometryId).has_value();
    case sketch::DimensionKind::CircleDiameter:
      return model.circleIndex(dimension.geometryId).has_value();
    case sketch::DimensionKind::PointDistance:
    case sketch::DimensionKind::PointDistanceX:
    case sketch::DimensionKind::PointDistanceY:
      return validPointReference(model, dimension.firstPoint) &&
             validPointReference(model, dimension.secondPoint);
    case sketch::DimensionKind::LineAngle:
    case sketch::DimensionKind::LineDistance:
      return model.lineIndex(dimension.geometryId).has_value() &&
             model.lineIndex(dimension.secondPoint.lineId).has_value();
  }
  return false;
}

bool samePointReference(sketch::PointReference first,
                        sketch::PointReference second) {
  if (first.origin || second.origin) return first.origin && second.origin;
  if (first.elementCenterId != 0 || second.elementCenterId != 0)
    return first.elementCenterId != 0 && second.elementCenterId != 0 &&
           first.elementCenterId == second.elementCenterId;
  if (first.circleId != sketch::kInvalidGeometryId ||
      second.circleId != sketch::kInvalidGeometryId)
    return first.circleId != sketch::kInvalidGeometryId &&
           second.circleId != sketch::kInvalidGeometryId &&
           first.circleId == second.circleId;
  if (first.arcId != sketch::kInvalidGeometryId ||
      second.arcId != sketch::kInvalidGeometryId)
    return first.arcId != sketch::kInvalidGeometryId &&
           second.arcId != sketch::kInvalidGeometryId &&
           first.arcId == second.arcId && first.start == second.start;
  if (first.bezierId != sketch::kInvalidGeometryId ||
      second.bezierId != sketch::kInvalidGeometryId)
    return first.bezierId != sketch::kInvalidGeometryId &&
           second.bezierId != sketch::kInvalidGeometryId &&
           first.bezierId == second.bezierId &&
           first.bezierPoint == second.bezierPoint;
  return first.lineId != sketch::kInvalidGeometryId &&
         second.lineId != sketch::kInvalidGeometryId &&
         first.lineId == second.lineId && first.start == second.start;
}

double lineLength(const sketch::Line& line) {
  return std::hypot(line.end.xMm - line.start.xMm,
                    line.end.yMm - line.start.yMm);
}

double lineAngleDegrees(const sketch::Line& first,
                        const sketch::Line& second) {
  const double firstAngle = std::atan2(first.end.yMm - first.start.yMm,
                                       first.end.xMm - first.start.xMm);
  const double secondAngle = std::atan2(second.end.yMm - second.start.yMm,
                                        second.end.xMm - second.start.xMm);
  double value = std::abs((secondAngle - firstAngle) * 180.0 /
                          3.14159265358979323846);
  while (value >= 360.0) value -= 360.0;
  if (value > 180.0) value = 360.0 - value;
  return value;
}

double visibleLineAngleDegrees(const sketch::Line& first,
                               const sketch::Line& second) {
  const double primitive = lineAngleDegrees(first, second);
  return std::min(primitive, 180.0 - primitive);
}

double parallelLineDistanceMm(const sketch::Line& first,
                              const sketch::Line& second) {
  const double dx = first.end.xMm - first.start.xMm;
  const double dy = first.end.yMm - first.start.yMm;
  const double length = std::hypot(dx, dy);
  if (length <= 1e-12) return 0.0;
  return std::abs((second.start.xMm - first.start.xMm) * dy -
                  (second.start.yMm - first.start.yMm) * dx) /
         length;
}

void autoCoincidentNewGeometry(
    sketch::Sketch& sketch,
    const std::vector<sketch::GeometryId>& newGeometryIds,
    double toleranceMm) {
  struct ReferenceCandidate {
    sketch::PointReference reference;
    sketch::Point initialPoint;
  };
  const std::unordered_set<sketch::GeometryId> newIds(
      newGeometryIds.begin(), newGeometryIds.end());
  const auto isNew = [&newIds](sketch::GeometryId id) {
    return newIds.contains(id);
  };

  const auto sameReference =
      [](sketch::PointReference first,
         sketch::PointReference second) {
        if (first.elementCenterId != 0 ||
            second.elementCenterId != 0) {
          return first.elementCenterId != 0 &&
                 first.elementCenterId ==
                     second.elementCenterId;
        }

        if (first.circleId != sketch::kInvalidGeometryId ||
            second.circleId != sketch::kInvalidGeometryId) {
          return first.circleId != sketch::kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        if (first.arcId != sketch::kInvalidGeometryId ||
            second.arcId != sketch::kInvalidGeometryId) {
          return first.arcId != sketch::kInvalidGeometryId &&
                 second.arcId != sketch::kInvalidGeometryId &&
                 first.arcId == second.arcId &&
                 first.start == second.start;
        }

        if (first.bezierId != sketch::kInvalidGeometryId ||
            second.bezierId != sketch::kInvalidGeometryId)
          return first.bezierId != sketch::kInvalidGeometryId &&
                 second.bezierId != sketch::kInvalidGeometryId &&
                 first.bezierId == second.bezierId &&
                 first.bezierPoint == second.bezierPoint;

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  std::vector<sketch::PointReference> oldReferences;

  const auto addOldReference =
      [&oldReferences, &sameReference](
          sketch::PointReference reference) {
        const bool duplicate =
            std::any_of(
                oldReferences.begin(),
                oldReferences.end(),
                [reference, &sameReference](
                    sketch::PointReference existing) {
                  return sameReference(existing, reference);
                });

        if (!duplicate)
          oldReferences.push_back(reference);
      };

  for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
    const auto id = sketch.lineId(index);
    if (id == sketch::kInvalidGeometryId || isNew(id))
      continue;

    addOldReference(sketch::PointReference{id, true});
    addOldReference(sketch::PointReference{id, false});
  }

  for (std::size_t index = 0; index < sketch.circles().size(); ++index) {
    const auto id = sketch.circleId(index);
    if (id == sketch::kInvalidGeometryId || isNew(id))
      continue;

    sketch::PointReference center;
    center.circleId = id;
    addOldReference(center);
  }

  for (std::size_t index = 0; index < sketch.arcs().size(); ++index) {
    const auto id = sketch.arcId(index);
    if (id == sketch::kInvalidGeometryId || isNew(id)) continue;

    sketch::PointReference endpoint;
    endpoint.arcId = id;
    endpoint.start = true;
    addOldReference(endpoint);
    endpoint.start = false;
    addOldReference(endpoint);
  }
  for (std::size_t index = 0; index < sketch.beziers().size(); ++index) {
    const auto id = sketch.bezierId(index);
    if (id == sketch::kInvalidGeometryId || isNew(id)) continue;
    for (std::uint8_t pointIndex = 0; pointIndex < 4; ++pointIndex) {
      sketch::PointReference reference;
      reference.bezierId = id;
      reference.bezierPoint = pointIndex;
      addOldReference(reference);
    }
  }

  // Existing virtual centers of composite elements are CAD points too.
  // Only centers whose element already existed before this creation pass
  // belong in oldReferences.
  for (const auto elementId : sketch.centerNodeElementIds()) {
    bool belongsToOldGeometry = false;

    for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
      if (!isNew(sketch.lineId(index)) &&
          sketch.lines()[index].elementId == elementId) {
        belongsToOldGeometry = true;
        break;
      }
    }

    if (!belongsToOldGeometry)
      continue;

    sketch::PointReference center;
    center.elementCenterId = elementId;
    addOldReference(center);
  }

  std::vector<ReferenceCandidate> newReferences;

  const auto addNewReference =
      [&sketch, &newReferences](
          sketch::PointReference reference) {
        const auto point =
            sketch.referencedPoint(reference);

        if (!point)
          return;

        // Rectangle corners may be represented by two coincident
        // primitive endpoints. One external constraint per geometric
        // point is enough.
        const bool duplicatePoint =
            std::any_of(
                newReferences.begin(),
                newReferences.end(),
                [&point](const ReferenceCandidate& item) {
                  return std::hypot(
                             item.initialPoint.xMm -
                                 point->xMm,
                             item.initialPoint.yMm -
                                 point->yMm) <= 1e-7;
                });

        if (!duplicatePoint)
          newReferences.push_back({reference, *point});
      };

  for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
    const auto id = sketch.lineId(index);

    if (id == sketch::kInvalidGeometryId || !isNew(id))
      continue;

    addNewReference(
        sketch::PointReference{id, true});
    addNewReference(
        sketch::PointReference{id, false});
  }

  for (std::size_t index = 0; index < sketch.circles().size(); ++index) {
    const auto id = sketch.circleId(index);

    if (id == sketch::kInvalidGeometryId || !isNew(id))
      continue;

    sketch::PointReference center;
    center.circleId = id;
    addNewReference(center);
  }

  for (std::size_t index = 0; index < sketch.arcs().size(); ++index) {
    const auto id = sketch.arcId(index);
    if (id == sketch::kInvalidGeometryId || !isNew(id)) continue;

    sketch::PointReference endpoint;
    endpoint.arcId = id;
    endpoint.start = true;
    addNewReference(endpoint);
    endpoint.start = false;
    addNewReference(endpoint);
  }
  for (std::size_t index = 0; index < sketch.beziers().size(); ++index) {
    const auto id = sketch.bezierId(index);
    if (id == sketch::kInvalidGeometryId || !isNew(id)) continue;
    for (std::uint8_t pointIndex = 0; pointIndex < 4; ++pointIndex) {
      sketch::PointReference reference;
      reference.bezierId = id;
      reference.bezierPoint = pointIndex;
      addNewReference(reference);
    }
  }

  // Newly created center-based rectangles expose a virtual center node.
  // Add it as a new reference so a rectangle created from an existing
  // CAD point receives a real Coincident constraint at its center.
  for (const auto elementId : sketch.centerNodeElementIds()) {
    bool belongsToNewGeometry = false;

    for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
      if (isNew(sketch.lineId(index)) &&
          sketch.lines()[index].elementId == elementId) {
        belongsToNewGeometry = true;
        break;
      }
    }

    if (!belongsToNewGeometry)
      continue;

    sketch::PointReference center;
    center.elementCenterId = elementId;
    addNewReference(center);
  }

  const auto constraintExists =
      [&sketch, &sameReference](
          sketch::PointReference first,
          sketch::PointReference second) {
        return std::any_of(
            sketch.constraints().begin(),
            sketch.constraints().end(),
            [first, second, &sameReference](
                const sketch::Constraint& constraint) {
              if (constraint.type !=
                  sketch::ConstraintType::Coincident)
                return false;

              return
                  (sameReference(
                       constraint.firstPoint, first) &&
                   sameReference(
                       constraint.secondPoint, second)) ||
                  (sameReference(
                       constraint.firstPoint, second) &&
                   sameReference(
                       constraint.secondPoint, first));
            });
      };

  for (const auto& candidate : newReferences) {
    const auto currentPoint =
        sketch.referencedPoint(candidate.reference);

    if (!currentPoint)
      continue;

    std::optional<sketch::PointReference> nearest;
    double bestDistance = toleranceMm;

    for (const auto oldReference : oldReferences) {
      const auto oldPoint =
          sketch.referencedPoint(oldReference);

      if (!oldPoint)
        continue;

      const double distance =
          std::hypot(
              currentPoint->xMm - oldPoint->xMm,
              currentPoint->yMm - oldPoint->yMm);

      if (distance < bestDistance) {
        bestDistance = distance;
        nearest = oldReference;
      }
    }

    if (!nearest ||
        sameReference(
            *nearest, candidate.reference) ||
        constraintExists(
            *nearest, candidate.reference))
      continue;

    sketch::Constraint constraint;
    constraint.type =
        sketch::ConstraintType::Coincident;

    // Existing geometry is the reference;
    // newly created geometry moves to it.
    constraint.firstPoint = *nearest;
    constraint.secondPoint =
        candidate.reference;

    (void)sketch.tryApplyConstraint(constraint);
  }

  // BODY SNAP PERSISTENCE
  //
  // Coincident above handles discrete CAD points. If a newly-created point was
  // explicitly projected onto an existing carrier body, retain that semantic
  // relation as PointOnLine / PointOnCircle so later dimensions cannot detach
  // it. Projected locked lines keep their historical wider tolerance; ordinary
  // geometry uses an exact tolerance because constructionSnapAt() already
  // projects the clicked point onto the carrier.
  const auto oldReference =
      [&oldReferences, &sameReference](sketch::PointReference r) {
        return std::any_of(
            oldReferences.begin(), oldReferences.end(),
            [r, &sameReference](sketch::PointReference old) {
              return sameReference(r, old);
            });
      };

  const auto hasOldCoincident =
      [&sketch, &sameReference, &oldReference](
          sketch::PointReference r) {
        for (const auto& c : sketch.constraints()) {
          if (c.type != sketch::ConstraintType::Coincident)
            continue;
          if (sameReference(c.firstPoint, r) &&
              oldReference(c.secondPoint))
            return true;
          if (sameReference(c.secondPoint, r) &&
              oldReference(c.firstPoint))
            return true;
        }
        return false;
      };

  constexpr double kExactBodyToleranceMm = 1e-5;

  for (const auto& candidate : newReferences) {
    if (hasOldCoincident(candidate.reference))
      continue;

    const auto point =
        sketch.referencedPoint(candidate.reference);
    if (!point) continue;

    sketch::GeometryId bestLine =
        sketch::kInvalidGeometryId;
    sketch::GeometryId bestCircle =
        sketch::kInvalidGeometryId;
    sketch::GeometryId bestArc =
        sketch::kInvalidGeometryId;
    double bestDistance =
        std::numeric_limits<double>::max();

    for (std::size_t i = 0; i < sketch.lines().size(); ++i) {
      const auto carrierId = sketch.lineId(i);
      if (carrierId == sketch::kInvalidGeometryId || isNew(carrierId))
        continue;

      const auto& carrier = sketch.lines()[i];
      const double dx =
          carrier.end.xMm - carrier.start.xMm;
      const double dy =
          carrier.end.yMm - carrier.start.yMm;
      const double l2 = dx * dx + dy * dy;
      if (l2 <= 1e-12) continue;

      const double t =
          ((point->xMm - carrier.start.xMm) * dx +
           (point->yMm - carrier.start.yMm) * dy) / l2;

      if (t < 0.0 || t > 1.0)
        continue;

      const sketch::Point q{
          carrier.start.xMm + t * dx,
          carrier.start.yMm + t * dy};

      const double distance =
          std::hypot(
              point->xMm - q.xMm,
              point->yMm - q.yMm);

      const bool projectedReference =
          carrier.dashed &&
          sketch.isGeometryLocked(carrierId);
      const double acceptedDistance =
          projectedReference
              ? toleranceMm
              : kExactBodyToleranceMm;

      if (distance <= acceptedDistance &&
          distance < bestDistance) {
        bestDistance = distance;
        bestLine = carrierId;
        bestCircle = sketch::kInvalidGeometryId;
        bestArc = sketch::kInvalidGeometryId;
      }
    }

    for (std::size_t i = 0; i < sketch.circles().size(); ++i) {
      const auto circleId = sketch.circleId(i);
      if (circleId == sketch::kInvalidGeometryId || isNew(circleId))
        continue;

      const auto& circle = sketch.circles()[i];
      const double distance =
          std::abs(
              std::hypot(
                  point->xMm - circle.center.xMm,
                  point->yMm - circle.center.yMm) -
              circle.radiusMm);

      if (distance <= kExactBodyToleranceMm &&
          distance < bestDistance) {
        bestDistance = distance;
        bestLine = sketch::kInvalidGeometryId;
        bestCircle = circleId;
        bestArc = sketch::kInvalidGeometryId;
      }
    }

    for (std::size_t i = 0; i < sketch.arcs().size(); ++i) {
      const auto arcId = sketch.arcId(i);
      if (arcId == sketch::kInvalidGeometryId || isNew(arcId)) continue;
      const auto& arc = sketch.arcs()[i];
      if (arc.radiusMm <= 1e-9) continue;
      const double dx = point->xMm - arc.center.xMm;
      const double dy = point->yMm - arc.center.yMm;
      const double angle = std::atan2(dy, dx);
      constexpr double kTwoPi = 6.28318530717958647692;
      const auto normalize = [](double value) {
        constexpr double twoPi = 6.28318530717958647692;
        value = std::fmod(value, twoPi);
        return value < 0.0 ? value + twoPi : value;
      };
      const double parameter =
          normalize(angle - normalize(arc.startAngleRad));
      if (parameter > arc.sweepAngleRad + 1e-9 ||
          arc.sweepAngleRad >= kTwoPi)
        continue;
      const double distance =
          std::abs(std::hypot(dx, dy) - arc.radiusMm);
      if (distance <= kExactBodyToleranceMm && distance < bestDistance) {
        bestDistance = distance;
        bestLine = sketch::kInvalidGeometryId;
        bestCircle = sketch::kInvalidGeometryId;
        bestArc = arcId;
      }
    }

    if (bestLine != sketch::kInvalidGeometryId) {
      const bool duplicate = std::any_of(
          sketch.constraints().begin(),
          sketch.constraints().end(),
          [bestLine, &candidate, &sameReference](
              const sketch::Constraint& c) {
            return c.type ==
                       sketch::ConstraintType::PointOnLine &&
                   c.firstGeometry == bestLine &&
                   sameReference(
                       c.secondPoint,
                       candidate.reference);
          });

      if (!duplicate) {
        sketch::Constraint c;
        c.type = sketch::ConstraintType::PointOnLine;
        c.firstGeometry = bestLine;
        c.secondPoint = candidate.reference;
        (void)sketch.tryApplyConstraint(c);
      }

      continue;
    }

    if (bestCircle != sketch::kInvalidGeometryId) {
      const bool duplicate = std::any_of(
          sketch.constraints().begin(),
          sketch.constraints().end(),
          [bestCircle, &candidate, &sameReference](
              const sketch::Constraint& c) {
            return c.type ==
                       sketch::ConstraintType::PointOnCircle &&
                   c.firstGeometry == bestCircle &&
                   sameReference(
                       c.secondPoint,
                       candidate.reference);
          });

      if (!duplicate) {
        sketch::Constraint c;
        c.type = sketch::ConstraintType::PointOnCircle;
        c.firstGeometry = bestCircle;
        c.secondPoint = candidate.reference;
        (void)sketch.tryApplyConstraint(c);
      }

      continue;
    }

    if (bestArc != sketch::kInvalidGeometryId) {
      const bool duplicate = std::any_of(
          sketch.constraints().begin(), sketch.constraints().end(),
          [bestArc, &candidate, &sameReference](
              const sketch::Constraint& c) {
            return c.type == sketch::ConstraintType::PointOnArc &&
                   c.firstGeometry == bestArc &&
                   sameReference(c.secondPoint, candidate.reference);
          });
      if (!duplicate) {
        sketch::Constraint c;
        c.type = sketch::ConstraintType::PointOnArc;
        c.firstGeometry = bestArc;
        c.secondPoint = candidate.reference;
        (void)sketch.tryApplyConstraint(c);
      }
    }
  }

  // DATUM AXIS SNAP PERSISTENCE
  // constructionSnapAt() projects explicit axis/origin hits to exact datum
  // coordinates. Preserve those coordinates as semantic constraints for every
  // new point, including rectangle corners and circle/element centres.
  constexpr double kAxisCoordinateToleranceMm = 1e-7;
  for (const auto& candidate : newReferences) {
    const auto point = sketch.referencedPoint(candidate.reference);
    if (!point) continue;

    const auto addAxisConstraint =
        [&sketch, &candidate, &sameReference](sketch::ConstraintType type) {
          const bool duplicate = std::any_of(
              sketch.constraints().begin(), sketch.constraints().end(),
              [type, &candidate, &sameReference](
                  const sketch::Constraint& constraint) {
                return constraint.type == type &&
                       sameReference(constraint.secondPoint,
                                     candidate.reference);
              });
          if (duplicate) return;
          sketch::Constraint constraint;
          constraint.type = type;
          constraint.secondPoint = candidate.reference;
          (void)sketch.tryApplyConstraint(constraint);
        };

    if (std::abs(point->yMm) <= kAxisCoordinateToleranceMm)
      addAxisConstraint(sketch::ConstraintType::PointOnXAxis);
    if (std::abs(point->xMm) <= kAxisCoordinateToleranceMm)
      addAxisConstraint(sketch::ConstraintType::PointOnYAxis);
  }
}

}  // namespace

SketchCommandResult SketchCommandController::execute(
    sketch::Sketch& model, const SketchCommand& command) const {
  const auto journalDepthBefore = model.deltaJournalDepth();
  SketchCommandResult result;
  try {
    model.beginDeltaJournal();
    result = std::visit(
        [&model](const auto& typed) -> SketchCommandResult {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, SetSketchRectangleCommand>) {
            if (!finite(typed.widthMm) || !finite(typed.heightMm) ||
                typed.widthMm <= 0.0 || typed.heightMm <= 0.0)
              return rejected(SketchCommandError::InvalidInput);
            model.setRectangle(typed.widthMm, typed.heightMm);
            return accepted(geometryEffects());
          } else if constexpr (std::is_same_v<T, ClearSketchCommand>) {
            model.clear();
            auto value = accepted(geometryEffects());
            value.effects.selectionMayBeStale = true;
            return value;
          } else if constexpr (std::is_same_v<T, AddLineCommand>) {
            if (!finite(typed.start) || !finite(typed.end))
              return rejected(SketchCommandError::InvalidInput);
            const auto before = model.lines().size();
            if (typed.elementId)
              model.addLine(typed.start, typed.end, *typed.elementId);
            else
              model.addLine(typed.start, typed.end);
            if (model.lines().size() != before + 1)
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(model.lineId(before));
            return value;
          } else if constexpr (std::is_same_v<T, AddRectangleCommand>) {
            if (!finite(typed.first) || !finite(typed.second) ||
                (typed.third && !finite(*typed.third)) ||
                (typed.fourth && !finite(*typed.fourth)) ||
                typed.third.has_value() != typed.fourth.has_value())
              return rejected(SketchCommandError::InvalidInput);
            const auto before = model.lines().size();
            if (typed.third)
              model.addRectangle(typed.first, typed.second, *typed.third,
                                 *typed.fourth);
            else
              model.addRectangle(typed.first, typed.second);
            if (model.lines().size() != before + 4)
              return rejected(SketchCommandError::MutationRejected);
            if (typed.markCenter)
              model.markElementCenterNode(model.lines()[before].elementId);
            auto value = accepted(geometryEffects());
            for (std::size_t index = before; index < before + 4; ++index)
              value.changedGeometryIds.push_back(model.lineId(index));
            return value;
          } else if constexpr (std::is_same_v<T, AddCircleCommand>) {
            if (!finite(typed.center) || !finite(typed.radiusMm) ||
                typed.radiusMm <= 0.0)
              return rejected(SketchCommandError::InvalidInput);
            const auto before = model.circles().size();
            model.addCircle(typed.center, typed.radiusMm);
            if (model.circles().size() != before + 1)
              return rejected(SketchCommandError::MutationRejected);
            const auto id = model.circleId(before);
            if (typed.dashed) model.setCircleDashedById(id, true);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(id);
            return value;
          } else if constexpr (std::is_same_v<T, AddArcCommand>) {
            if (!finite(typed.center) || !finite(typed.radiusMm) ||
                !finite(typed.startAngleRad) || !finite(typed.sweepAngleRad) ||
                typed.radiusMm <= 0.0 || typed.sweepAngleRad == 0.0)
              return rejected(SketchCommandError::InvalidInput);
            const auto before = model.arcs().size();
            model.addArc(typed.center, typed.radiusMm, typed.startAngleRad,
                         typed.sweepAngleRad, typed.dashed);
            if (model.arcs().size() != before + 1)
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(model.arcId(before));
            return value;
          } else if constexpr (std::is_same_v<T, AddBezierCommand>) {
            if (!finite(typed.start) || !finite(typed.control1) ||
                !finite(typed.control2) || !finite(typed.end))
              return rejected(SketchCommandError::InvalidInput);
            const auto before = model.beziers().size();
            model.addBezier(typed.start, typed.control1, typed.control2,
                            typed.end, typed.dashed);
            if (model.beziers().size() != before + 1)
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(model.bezierId(before));
            return value;
          } else if constexpr (std::is_same_v<T, ProjectGeometryCommand>) {
            auto value = accepted(constraintEffects());
            const bool created = std::visit(
                [&model, &value](const auto& projected) {
                  using P = std::decay_t<decltype(projected)>;
                  if constexpr (std::is_same_v<P, ProjectLineChain>) {
                    std::vector<ProjectedLineSegment> unique;
                    unique.reserve(projected.segments.size());
                    for (const auto& segment : projected.segments) {
                      if (!finite(segment.first) || !finite(segment.second) ||
                          samePoint(segment.first, segment.second))
                        continue;
                      const auto sameSegment = [&segment](
                                                   const auto& other) {
                        return (samePoint(segment.first, other.first) &&
                                samePoint(segment.second, other.second)) ||
                               (samePoint(segment.first, other.second) &&
                                samePoint(segment.second, other.first));
                      };
                      const bool inModel = std::any_of(
                          model.lines().begin(), model.lines().end(),
                          [&segment](const sketch::Line& line) {
                            return line.dashed &&
                                   ((samePoint(segment.first, line.start) &&
                                     samePoint(segment.second, line.end)) ||
                                    (samePoint(segment.first, line.end) &&
                                     samePoint(segment.second, line.start)));
                          });
                      if (!inModel &&
                          std::none_of(unique.begin(), unique.end(), sameSegment))
                        unique.push_back(segment);
                    }
                    if (unique.empty()) return false;
                    const auto firstIndex = model.lines().size();
                    model.addLine(unique.front().first, unique.front().second);
                    if (model.lines().size() != firstIndex + 1) return false;
                    const auto elementId = model.lines()[firstIndex].elementId;
                    for (std::size_t index = 1; index < unique.size(); ++index)
                      model.addLine(unique[index].first, unique[index].second,
                                    elementId);
                    if (model.lines().size() != firstIndex + unique.size())
                      return false;
                    model.setElementDashed(elementId, true);
                    for (std::size_t index = firstIndex;
                         index < model.lines().size(); ++index)
                      value.changedGeometryIds.push_back(model.lineId(index));
                  } else if constexpr (std::is_same_v<P, ProjectCircle>) {
                    if (!finite(projected.center) ||
                        !finite(projected.radiusMm) ||
                        projected.radiusMm <= 1e-9)
                      return false;
                    if (std::any_of(model.circles().begin(),
                                    model.circles().end(),
                                    [&projected](const sketch::Circle& circle) {
                                      return circle.dashed &&
                                             samePoint(circle.center,
                                                       projected.center) &&
                                             std::abs(circle.radiusMm -
                                                      projected.radiusMm) <= 1e-6;
                                    }))
                      return false;
                    const auto index = model.circles().size();
                    model.addCircle(projected.center, projected.radiusMm);
                    if (model.circles().size() != index + 1) return false;
                    const auto id = model.circleId(index);
                    model.setCircleDashedById(id, true);
                    value.changedGeometryIds.push_back(id);
                  } else {
                    if (!finite(projected.center) ||
                        !finite(projected.radiusMm) ||
                        !finite(projected.startAngleRad) ||
                        !finite(projected.sweepAngleRad) ||
                        projected.radiusMm <= 1e-9 ||
                        projected.sweepAngleRad <= 1e-9)
                      return false;
                    const sketch::Point projectedStart{
                        projected.center.xMm + projected.radiusMm *
                            std::cos(projected.startAngleRad),
                        projected.center.yMm + projected.radiusMm *
                            std::sin(projected.startAngleRad)};
                    const sketch::Point projectedEnd{
                        projected.center.xMm + projected.radiusMm *
                            std::cos(projected.startAngleRad +
                                     projected.sweepAngleRad),
                        projected.center.yMm + projected.radiusMm *
                            std::sin(projected.startAngleRad +
                                     projected.sweepAngleRad)};
                    if (std::any_of(model.arcs().begin(), model.arcs().end(),
                                    [&](const sketch::Arc& arc) {
                                      if (!arc.dashed ||
                                          std::abs(arc.radiusMm -
                                                   projected.radiusMm) > 1e-6)
                                        return false;
                                      const auto first = sketch::arcStartPoint(arc);
                                      const auto second = sketch::arcEndPoint(arc);
                                      return (samePoint(first, projectedStart) &&
                                              samePoint(second, projectedEnd)) ||
                                             (samePoint(first, projectedEnd) &&
                                              samePoint(second, projectedStart));
                                    }))
                      return false;
                    const auto index = model.arcs().size();
                    model.addArc(projected.center, projected.radiusMm,
                                 projected.startAngleRad,
                                 projected.sweepAngleRad, true);
                    if (model.arcs().size() != index + 1) return false;
                    value.changedGeometryIds.push_back(model.arcId(index));
                  }
                  sketch::Constraint lock;
                  lock.type = sketch::ConstraintType::Lock;
                  lock.firstGeometry = value.changedGeometryIds.front();
                  const auto lockId = tryApplyConstraintId(model, lock);
                  if (lockId == sketch::kInvalidConstraintId) return false;
                  value.changedConstraintIds.push_back(lockId);
                  return true;
                },
                typed.geometry);
            if (!created)
              return rejected(SketchCommandError::Duplicate);
            return value;
          } else if constexpr (std::is_same_v<T, AddPrimitiveBatchCommand>) {
            if (typed.primitives.empty())
              return rejected(SketchCommandError::InvalidInput);
            auto value = accepted(geometryEffects());
            for (const auto& primitive : typed.primitives) {
              const bool created = std::visit(
                  [&model, &value](const auto& item) {
                    using P = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<P, AddLineCommand>) {
                      if (!finite(item.start) || !finite(item.end)) return false;
                      const auto index = model.lines().size();
                      if (item.elementId)
                        model.addLine(item.start, item.end, *item.elementId);
                      else
                        model.addLine(item.start, item.end);
                      if (model.lines().size() != index + 1) return false;
                      value.changedGeometryIds.push_back(model.lineId(index));
                    } else if constexpr (std::is_same_v<P,
                                                        AddRectangleCommand>) {
                      if (!finite(item.first) || !finite(item.second) ||
                          item.third.has_value() != item.fourth.has_value() ||
                          (item.third && !finite(*item.third)) ||
                          (item.fourth && !finite(*item.fourth)))
                        return false;
                      const auto index = model.lines().size();
                      if (item.third)
                        model.addRectangle(item.first, item.second, *item.third,
                                           *item.fourth);
                      else
                        model.addRectangle(item.first, item.second);
                      if (model.lines().size() != index + 4) return false;
                      if (item.markCenter)
                        model.markElementCenterNode(
                            model.lines()[index].elementId);
                      for (std::size_t line = index; line < index + 4; ++line)
                        value.changedGeometryIds.push_back(model.lineId(line));
                    } else if constexpr (std::is_same_v<P, AddCircleCommand>) {
                      if (!finite(item.center) || !finite(item.radiusMm) ||
                          item.radiusMm <= 0.0)
                        return false;
                      const auto index = model.circles().size();
                      model.addCircle(item.center, item.radiusMm);
                      if (model.circles().size() != index + 1) return false;
                      const auto id = model.circleId(index);
                      if (item.dashed) model.setCircleDashedById(id, true);
                      value.changedGeometryIds.push_back(id);
                    } else if constexpr (std::is_same_v<P, AddArcCommand>) {
                      if (!finite(item.center) || !finite(item.radiusMm) ||
                          !finite(item.startAngleRad) ||
                          !finite(item.sweepAngleRad) ||
                          item.radiusMm <= 0.0 || item.sweepAngleRad == 0.0)
                        return false;
                      const auto index = model.arcs().size();
                      model.addArc(item.center, item.radiusMm,
                                   item.startAngleRad, item.sweepAngleRad,
                                   item.dashed);
                      if (model.arcs().size() != index + 1) return false;
                      value.changedGeometryIds.push_back(model.arcId(index));
                    } else {
                      if (!finite(item.start) || !finite(item.control1) ||
                          !finite(item.control2) || !finite(item.end))
                        return false;
                      const auto index = model.beziers().size();
                      model.addBezier(item.start, item.control1, item.control2,
                                      item.end, item.dashed);
                      if (model.beziers().size() != index + 1) return false;
                      value.changedGeometryIds.push_back(model.bezierId(index));
                    }
                    return true;
                  },
                  primitive);
              if (!created)
                return rejected(SketchCommandError::MutationRejected);
            }
            return value;
          } else if constexpr (std::is_same_v<T, RemoveGeometryCommand>) {
            const auto location = model.geometryLocation(typed.id);
            if (!location)
              return rejected(SketchCommandError::StaleReference);
            switch (location->kind) {
              case sketch::GeometryKind::Line:
                model.removeLine(location->index);
                break;
              case sketch::GeometryKind::Circle:
                model.removeCircle(location->index);
                break;
              case sketch::GeometryKind::Arc:
                model.removeArc(location->index);
                break;
              case sketch::GeometryKind::Bezier:
                model.removeBezier(location->index);
                break;
            }
            auto value = accepted(geometryEffects());
            value.effects.constraintsChanged = true;
            value.effects.dimensionsChanged = true;
            value.effects.selectionMayBeStale = true;
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, RemoveElementCommand>) {
            if (typed.elementId == 0)
              return rejected(SketchCommandError::InvalidInput);
            const bool exists = std::any_of(
                model.lines().begin(), model.lines().end(),
                [&typed](const sketch::Line& line) {
                  return line.elementId == typed.elementId;
                });
            if (!exists) return rejected(SketchCommandError::StaleReference);
            model.removeElement(typed.elementId);
            auto value = accepted(geometryEffects());
            value.effects.constraintsChanged = true;
            value.effects.dimensionsChanged = true;
            value.effects.selectionMayBeStale = true;
            return value;
          } else if constexpr (std::is_same_v<T, DeleteSelectionCommand>) {
            if (typed.geometryIds.empty() && typed.elementIds.empty())
              return rejected(SketchCommandError::InvalidInput);
            if (std::any_of(typed.geometryIds.begin(), typed.geometryIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.geometryLocation(id);
                            }))
              return rejected(SketchCommandError::StaleReference);
            for (const auto elementId : typed.elementIds) {
              if (elementId == 0 ||
                  std::none_of(model.lines().begin(), model.lines().end(),
                               [elementId](const sketch::Line& line) {
                                 return line.elementId == elementId;
                               }))
                return rejected(SketchCommandError::StaleReference);
            }
            for (const auto id : typed.geometryIds) {
              const auto location = model.geometryLocation(id);
              if (!location) return rejected(SketchCommandError::StaleReference);
              switch (location->kind) {
                case sketch::GeometryKind::Line:
                  model.removeLine(location->index);
                  break;
                case sketch::GeometryKind::Circle:
                  model.removeCircle(location->index);
                  break;
                case sketch::GeometryKind::Arc:
                  model.removeArc(location->index);
                  break;
                case sketch::GeometryKind::Bezier:
                  model.removeBezier(location->index);
                  break;
              }
            }
            for (const auto elementId : typed.elementIds)
              model.removeElement(elementId);
            auto value = accepted(geometryEffects());
            value.effects.constraintsChanged = true;
            value.effects.dimensionsChanged = true;
            value.effects.selectionMayBeStale = true;
            value.changedGeometryIds = typed.geometryIds;
            return value;
          } else if constexpr (std::is_same_v<T, AddConstraintCommand>) {
            return fromConstraintApply(
                model.tryApplyConstraint(typed.constraint, true),
                typed.bestEffort);
          } else if constexpr (std::is_same_v<T, BindPointCommand>) {
            if (!validPointReference(model, typed.movingPoint))
              return rejected(SketchCommandError::StaleReference);
            const auto addUnique = [&model](sketch::Constraint constraint) {
              for (const auto& existing : model.constraints()) {
                if (existing.type != constraint.type) continue;
                if (constraint.type == sketch::ConstraintType::Coincident) {
                  if ((samePointReference(existing.firstPoint,
                                           constraint.firstPoint) &&
                       samePointReference(existing.secondPoint,
                                           constraint.secondPoint)) ||
                      (samePointReference(existing.firstPoint,
                                          constraint.secondPoint) &&
                       samePointReference(existing.secondPoint,
                                          constraint.firstPoint)))
                    return existing.id;
                } else if (existing.firstGeometry ==
                               constraint.firstGeometry &&
                           samePointReference(existing.secondPoint,
                                              constraint.secondPoint)) {
                  return existing.id;
                }
              }
              return tryApplyConstraintId(model, constraint);
            };
            auto makeConstraint = [&typed](sketch::ConstraintType type) {
              sketch::Constraint constraint;
              constraint.type = type;
              constraint.secondPoint = typed.movingPoint;
              return constraint;
            };
            std::vector<sketch::ConstraintId> ids;
            if (typed.kind == SketchPointBindingKind::Origin) {
              for (const auto type : {sketch::ConstraintType::PointOnXAxis,
                                      sketch::ConstraintType::PointOnYAxis}) {
                const auto id = addUnique(makeConstraint(type));
                if (id == sketch::kInvalidConstraintId)
                  return rejected(SketchCommandError::Conflict);
                ids.push_back(id);
              }
            } else {
              sketch::Constraint constraint;
              constraint.secondPoint = typed.movingPoint;
              switch (typed.kind) {
                case SketchPointBindingKind::Coincident:
                  if (!validPointReference(model, typed.targetPoint) ||
                      samePointReference(typed.targetPoint, typed.movingPoint))
                    return rejected(SketchCommandError::StaleReference);
                  constraint.type = sketch::ConstraintType::Coincident;
                  constraint.firstPoint = typed.targetPoint;
                  break;
                case SketchPointBindingKind::Midpoint:
                  constraint.type = sketch::ConstraintType::Midpoint;
                  constraint.firstGeometry = typed.targetGeometry;
                  break;
                case SketchPointBindingKind::PointOnLine:
                  constraint.type = sketch::ConstraintType::PointOnLine;
                  constraint.firstGeometry = typed.targetGeometry;
                  break;
                case SketchPointBindingKind::PointOnCircle:
                  constraint.type = sketch::ConstraintType::PointOnCircle;
                  constraint.firstGeometry = typed.targetGeometry;
                  break;
                case SketchPointBindingKind::PointOnArc:
                  constraint.type = sketch::ConstraintType::PointOnArc;
                  constraint.firstGeometry = typed.targetGeometry;
                  break;
                case SketchPointBindingKind::XAxis:
                  constraint.type = sketch::ConstraintType::PointOnXAxis;
                  break;
                case SketchPointBindingKind::YAxis:
                  constraint.type = sketch::ConstraintType::PointOnYAxis;
                  break;
                case SketchPointBindingKind::Origin:
                  break;
              }
              if ((constraint.firstGeometry != sketch::kInvalidGeometryId &&
                   !model.geometryLocation(constraint.firstGeometry)) ||
                  !validConstraintReferences(model, constraint))
                return rejected(SketchCommandError::StaleReference);
              const auto id = addUnique(constraint);
              if (id == sketch::kInvalidConstraintId)
                return rejected(SketchCommandError::Conflict);
              ids.push_back(id);
            }
            auto value = accepted(constraintEffects());
            value.changedConstraintIds = std::move(ids);
            return value;
          } else if constexpr (
              std::is_same_v<T, AutoConstrainNewGeometryCommand>) {
            if (!finite(typed.toleranceMm) || typed.toleranceMm < 0.0 ||
                typed.newGeometryIds.empty())
              return rejected(SketchCommandError::InvalidInput);
            std::unordered_set<sketch::GeometryId> uniqueIds;
            for (const auto id : typed.newGeometryIds) {
              if (id == sketch::kInvalidGeometryId ||
                  !model.geometryLocation(id) || !uniqueIds.insert(id).second)
                return rejected(SketchCommandError::StaleReference);
            }
            const auto beforeConstraintCount = model.constraints().size();
            autoCoincidentNewGeometry(model, typed.newGeometryIds,
                                      typed.toleranceMm);
            auto value = accepted(constraintEffects());
            for (std::size_t index = beforeConstraintCount;
                 index < model.constraints().size(); ++index)
              value.changedConstraintIds.push_back(model.constraints()[index].id);
            return value;
          } else if constexpr (std::is_same_v<T, RemoveConstraintCommand>) {
            if (!model.constraintIndex(typed.id))
              return rejected(SketchCommandError::StaleReference);
            if (!model.removeConstraint(typed.id))
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(constraintEffects());
            value.changedConstraintIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, SetConstraintValueCommand>) {
            if (!finite(typed.value))
              return rejected(SketchCommandError::InvalidInput);
            if (!model.constraintIndex(typed.id))
              return rejected(SketchCommandError::StaleReference);
            if (!model.setConstraintValue(typed.id, typed.value))
              return rejected(SketchCommandError::Conflict);
            auto value = accepted(constraintEffects());
            value.changedConstraintIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, StoreDimensionCommand>) {
            if (!validDimensionReferences(model, typed.dimension))
              return rejected(SketchCommandError::StaleReference);
            const auto before = model.dimensions().size();
            model.storeDimension(typed.dimension);
            if (model.dimensions().size() != before + 1)
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(dimensionEffects());
            value.changedDimensionIds.push_back(model.dimensions().back().id);
            return value;
          } else if constexpr (
              std::is_same_v<T, UpsertDrivingDimensionCommand>) {
            if (!validDimensionReferences(model, typed.dimension) ||
                !validConstraintReferences(model, typed.constraint) ||
                !finite(typed.constraint.value) || typed.constraint.value <= 0.0)
              return rejected(SketchCommandError::StaleReference);
            const auto samePointPair = [&typed](
                                           const sketch::Constraint& value) {
              return (samePointReference(value.firstPoint,
                                         typed.constraint.firstPoint) &&
                      samePointReference(value.secondPoint,
                                         typed.constraint.secondPoint)) ||
                     (samePointReference(value.firstPoint,
                                         typed.constraint.secondPoint) &&
                      samePointReference(value.secondPoint,
                                         typed.constraint.firstPoint));
            };
            const auto matches = [&typed, &samePointPair](
                                      const sketch::Constraint& value) {
              if (value.type != typed.constraint.type) return false;
              if (typed.constraint.type == sketch::ConstraintType::Distance ||
                  typed.constraint.type == sketch::ConstraintType::DistanceX ||
                  typed.constraint.type == sketch::ConstraintType::DistanceY)
                return samePointPair(value);
              if (typed.constraint.secondGeometry !=
                  sketch::kInvalidGeometryId)
                return (value.firstGeometry ==
                            typed.constraint.firstGeometry &&
                        value.secondGeometry ==
                            typed.constraint.secondGeometry) ||
                       (value.firstGeometry ==
                            typed.constraint.secondGeometry &&
                        value.secondGeometry ==
                            typed.constraint.firstGeometry);
              return value.firstGeometry == typed.constraint.firstGeometry;
            };
            std::vector<sketch::ConstraintId> existing;
            for (const auto& value : model.constraints())
              if (matches(value)) existing.push_back(value.id);

            auto result = accepted(dimensionEffects());
            result.effects.constraintsChanged = true;
            if (typed.existingDimensionId) {
              const auto dimensionIndex =
                  model.dimensionIndex(*typed.existingDimensionId);
              if (!dimensionIndex || existing.size() != 1)
                return rejected(SketchCommandError::StaleReference);
              if (!model.setConstraintValue(existing.front(),
                                            typed.constraint.value) ||
                  !model.setDimensionValue(*dimensionIndex,
                                           typed.dimension.valueMm) ||
                  !model.setDimensionPlacement(*dimensionIndex,
                                               typed.dimension.offsetMm,
                                               typed.dimension.angleRad))
                return rejected(SketchCommandError::Conflict);
              result.changedConstraintIds = existing;
              result.changedDimensionIds.push_back(*typed.existingDimensionId);
              return result;
            }
            if (!existing.empty())
              return rejected(SketchCommandError::Duplicate);
            if (typed.ensureParallel) {
              const auto first = typed.constraint.firstGeometry;
              const auto second = typed.constraint.secondGeometry;
              const bool parallelExists = std::any_of(
                  model.constraints().begin(), model.constraints().end(),
                  [first, second](const sketch::Constraint& value) {
                    return value.type == sketch::ConstraintType::Parallel &&
                           ((value.firstGeometry == first &&
                             value.secondGeometry == second) ||
                            (value.firstGeometry == second &&
                             value.secondGeometry == first));
                  });
              if (!parallelExists) {
                sketch::Constraint parallel;
                parallel.type = sketch::ConstraintType::Parallel;
                parallel.firstGeometry = first;
                parallel.secondGeometry = second;
                const auto id = tryApplyConstraintId(model, parallel);
                if (id == sketch::kInvalidConstraintId)
                  return rejected(SketchCommandError::Conflict);
                result.changedConstraintIds.push_back(id);
              }
            }
            const auto constraintId =
                tryApplyConstraintId(model, typed.constraint, true);
            if (constraintId == sketch::kInvalidConstraintId)
              return rejected(SketchCommandError::Conflict);
            model.storeDimension(typed.dimension);
            if (model.dimensions().empty())
              return rejected(SketchCommandError::MutationRejected);
            result.changedConstraintIds.push_back(constraintId);
            result.changedDimensionIds.push_back(model.dimensions().back().id);
            return result;
          } else if constexpr (std::is_same_v<T, RemoveDimensionCommand>) {
            const auto index = model.dimensionIndex(typed.id);
            if (!index) return rejected(SketchCommandError::StaleReference);
            const auto dimension = model.dimensions()[*index];
            const auto samePointPair = [&dimension](
                                           const sketch::Constraint& value) {
              return (samePointReference(value.firstPoint,
                                         dimension.firstPoint) &&
                      samePointReference(value.secondPoint,
                                         dimension.secondPoint)) ||
                     (samePointReference(value.firstPoint,
                                         dimension.secondPoint) &&
                      samePointReference(value.secondPoint,
                                         dimension.firstPoint));
            };
            std::vector<sketch::ConstraintId> dependencies;
            for (const auto& constraint : model.constraints()) {
              bool matches = false;
              switch (dimension.kind) {
                case sketch::DimensionKind::LineLength:
                  matches = constraint.type == sketch::ConstraintType::Length &&
                            constraint.firstGeometry == dimension.geometryId;
                  break;
                case sketch::DimensionKind::CircleDiameter:
                  matches =
                      constraint.type == sketch::ConstraintType::Diameter &&
                      constraint.firstGeometry == dimension.geometryId;
                  break;
                case sketch::DimensionKind::PointDistance:
                  matches = constraint.type == sketch::ConstraintType::Distance &&
                            samePointPair(constraint);
                  break;
                case sketch::DimensionKind::PointDistanceX:
                  matches = constraint.type == sketch::ConstraintType::DistanceX &&
                            samePointPair(constraint);
                  break;
                case sketch::DimensionKind::PointDistanceY:
                  matches = constraint.type == sketch::ConstraintType::DistanceY &&
                            samePointPair(constraint);
                  break;
                case sketch::DimensionKind::LineAngle:
                  matches = constraint.type == sketch::ConstraintType::Angle &&
                            ((constraint.firstGeometry == dimension.geometryId &&
                              constraint.secondGeometry ==
                                  dimension.secondPoint.lineId) ||
                             (constraint.secondGeometry == dimension.geometryId &&
                              constraint.firstGeometry ==
                                  dimension.secondPoint.lineId));
                  break;
                case sketch::DimensionKind::LineDistance:
                  matches =
                      constraint.type == sketch::ConstraintType::LineDistance &&
                      ((constraint.firstGeometry == dimension.geometryId &&
                        constraint.secondGeometry ==
                            dimension.secondPoint.lineId) ||
                       (constraint.secondGeometry == dimension.geometryId &&
                        constraint.firstGeometry ==
                            dimension.secondPoint.lineId));
                  break;
              }
              if (matches) dependencies.push_back(constraint.id);
            }
            for (const auto constraintId : dependencies)
              if (!model.removeConstraint(constraintId))
                return rejected(SketchCommandError::MutationRejected);
            if (!model.removeDimension(*index))
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(dimensionEffects());
            value.effects.constraintsChanged = !dependencies.empty();
            value.effects.selectionMayBeStale = true;
            value.changedDimensionIds.push_back(typed.id);
            value.changedConstraintIds = std::move(dependencies);
            return value;
          } else if constexpr (std::is_same_v<T, SetDimensionValueCommand>) {
            if (!finite(typed.value))
              return rejected(SketchCommandError::InvalidInput);
            const auto index = model.dimensionIndex(typed.id);
            if (!index) return rejected(SketchCommandError::StaleReference);
            if (!model.setDimensionValue(*index, typed.value))
              return rejected(SketchCommandError::Conflict);
            auto value = accepted(dimensionEffects());
            value.changedDimensionIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, SetDimensionPlacementCommand>) {
            if (!finite(typed.offsetMm) || !finite(typed.angleRad))
              return rejected(SketchCommandError::InvalidInput);
            const auto index = model.dimensionIndex(typed.id);
            if (!index) return rejected(SketchCommandError::StaleReference);
            if (!model.setDimensionPlacement(*index, typed.offsetMm,
                                             typed.angleRad))
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(dimensionEffects());
            value.changedDimensionIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, SetDimensionDrivingCommand>) {
            const auto dimensionIndex = model.dimensionIndex(typed.id);
            if (!dimensionIndex)
              return rejected(SketchCommandError::StaleReference);
            const auto dimension = model.dimensions()[*dimensionIndex];
            const auto samePointPair = [&dimension](
                                           const sketch::Constraint& value) {
              return (samePointReference(value.firstPoint,
                                         dimension.firstPoint) &&
                      samePointReference(value.secondPoint,
                                         dimension.secondPoint)) ||
                     (samePointReference(value.firstPoint,
                                         dimension.secondPoint) &&
                      samePointReference(value.secondPoint,
                                         dimension.firstPoint));
            };
            const auto matchesDimension = [&dimension, &samePointPair](
                                               const sketch::Constraint& value) {
              switch (dimension.kind) {
                case sketch::DimensionKind::LineLength:
                  return value.type == sketch::ConstraintType::Length &&
                         value.firstGeometry == dimension.geometryId;
                case sketch::DimensionKind::CircleDiameter:
                  return value.type == sketch::ConstraintType::Diameter &&
                         value.firstGeometry == dimension.geometryId;
                case sketch::DimensionKind::PointDistance:
                  return value.type == sketch::ConstraintType::Distance &&
                         samePointPair(value);
                case sketch::DimensionKind::PointDistanceX:
                  return value.type == sketch::ConstraintType::DistanceX &&
                         samePointPair(value);
                case sketch::DimensionKind::PointDistanceY:
                  return value.type == sketch::ConstraintType::DistanceY &&
                         samePointPair(value);
                case sketch::DimensionKind::LineDistance:
                  return value.type == sketch::ConstraintType::LineDistance &&
                         ((value.firstGeometry == dimension.geometryId &&
                           value.secondGeometry == dimension.secondPoint.lineId) ||
                          (value.secondGeometry == dimension.geometryId &&
                           value.firstGeometry == dimension.secondPoint.lineId));
                case sketch::DimensionKind::LineAngle:
                  return value.type == sketch::ConstraintType::Angle &&
                         ((value.firstGeometry == dimension.geometryId &&
                           value.secondGeometry == dimension.secondPoint.lineId) ||
                          (value.secondGeometry == dimension.geometryId &&
                           value.firstGeometry == dimension.secondPoint.lineId));
              }
              return false;
            };
            std::vector<sketch::ConstraintId> existing;
            for (const auto& value : model.constraints())
              if (matchesDimension(value)) existing.push_back(value.id);
            if (!typed.driving) {
              for (const auto id : existing)
                if (!model.removeConstraint(id))
                  return rejected(SketchCommandError::MutationRejected);
              auto value = accepted(constraintEffects());
              value.changedConstraintIds = std::move(existing);
              return value;
            }
            if (!existing.empty()) return accepted({});

            sketch::Constraint constraint;
            double visibleValue{};
            switch (dimension.kind) {
              case sketch::DimensionKind::LineLength: {
                const auto index = model.lineIndex(dimension.geometryId);
                if (!index) return rejected(SketchCommandError::StaleReference);
                visibleValue = lineLength(model.lines()[*index]);
                constraint.type = sketch::ConstraintType::Length;
                constraint.firstGeometry = dimension.geometryId;
                break;
              }
              case sketch::DimensionKind::CircleDiameter: {
                const auto index = model.circleIndex(dimension.geometryId);
                if (!index) return rejected(SketchCommandError::StaleReference);
                visibleValue = model.circles()[*index].radiusMm * 2.0;
                constraint.type = sketch::ConstraintType::Diameter;
                constraint.firstGeometry = dimension.geometryId;
                break;
              }
              case sketch::DimensionKind::PointDistance:
              case sketch::DimensionKind::PointDistanceX:
              case sketch::DimensionKind::PointDistanceY: {
                const auto first = model.referencedPoint(dimension.firstPoint);
                const auto second = model.referencedPoint(dimension.secondPoint);
                if (!first || !second)
                  return rejected(SketchCommandError::StaleReference);
                if (dimension.kind == sketch::DimensionKind::PointDistanceX) {
                  visibleValue = std::abs(second->xMm - first->xMm);
                  constraint.type = sketch::ConstraintType::DistanceX;
                } else if (dimension.kind ==
                           sketch::DimensionKind::PointDistanceY) {
                  visibleValue = std::abs(second->yMm - first->yMm);
                  constraint.type = sketch::ConstraintType::DistanceY;
                } else {
                  visibleValue = std::hypot(second->xMm - first->xMm,
                                            second->yMm - first->yMm);
                  constraint.type = sketch::ConstraintType::Distance;
                }
                constraint.firstPoint = dimension.firstPoint;
                constraint.secondPoint = dimension.secondPoint;
                break;
              }
              case sketch::DimensionKind::LineDistance: {
                const auto first = model.lineIndex(dimension.geometryId);
                const auto second = model.lineIndex(dimension.secondPoint.lineId);
                if (!first || !second)
                  return rejected(SketchCommandError::StaleReference);
                visibleValue = parallelLineDistanceMm(model.lines()[*first],
                                                      model.lines()[*second]);
                constraint.type = sketch::ConstraintType::LineDistance;
                constraint.firstGeometry = dimension.geometryId;
                constraint.secondGeometry = dimension.secondPoint.lineId;
                break;
              }
              case sketch::DimensionKind::LineAngle: {
                const auto first = model.lineIndex(dimension.geometryId);
                const auto second = model.lineIndex(dimension.secondPoint.lineId);
                if (!first || !second)
                  return rejected(SketchCommandError::StaleReference);
                const auto primitive = lineAngleDegrees(model.lines()[*first],
                                                        model.lines()[*second]);
                visibleValue = visibleLineAngleDegrees(model.lines()[*first],
                                                       model.lines()[*second]);
                constraint.type = sketch::ConstraintType::Angle;
                constraint.firstGeometry = dimension.geometryId;
                constraint.secondGeometry = dimension.secondPoint.lineId;
                const auto supplement = 180.0 - primitive;
                constraint.value =
                    std::abs(visibleValue - supplement) <
                            std::abs(visibleValue - primitive)
                        ? 180.0 - visibleValue
                        : visibleValue;
                break;
              }
            }
            if (!finite(visibleValue) || visibleValue <= 1e-9)
              return rejected(SketchCommandError::InvalidInput);
            if (dimension.kind != sketch::DimensionKind::LineAngle)
              constraint.value = visibleValue;
            const auto constraintId = tryApplyConstraintId(model, constraint);
            if (constraintId == sketch::kInvalidConstraintId)
              return rejected(SketchCommandError::Conflict);
            if (!model.setDimensionValue(*dimensionIndex, visibleValue))
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(constraintEffects());
            value.effects.dimensionsChanged = true;
            value.changedConstraintIds.push_back(constraintId);
            value.changedDimensionIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, SetLineDashedCommand>) {
            const auto index = model.lineIndex(typed.id);
            if (!index)
              return rejected(SketchCommandError::StaleReference);
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            if (model.lines()[*index].dashed == typed.dashed)
              return accepted();
            model.setLineDashedById(typed.id, typed.dashed);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, SetCircleDashedCommand>) {
            const auto index = model.circleIndex(typed.id);
            if (!index)
              return rejected(SketchCommandError::StaleReference);
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            if (model.circles()[*index].dashed == typed.dashed)
              return accepted();
            model.setCircleDashedById(typed.id, typed.dashed);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, SetArcDashedCommand>) {
            const auto index = model.arcIndex(typed.id);
            if (!index)
              return rejected(SketchCommandError::StaleReference);
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            if (model.arcs()[*index].dashed == typed.dashed)
              return accepted();
            model.setArcDashedById(typed.id, typed.dashed);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, SetBezierDashedCommand>) {
            const auto index = model.bezierIndex(typed.id);
            if (!index)
              return rejected(SketchCommandError::StaleReference);
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            if (model.beziers()[*index].dashed == typed.dashed)
              return accepted();
            model.setBezierDashedById(typed.id, typed.dashed);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, SetElementDashedCommand>) {
            if (typed.elementId == 0)
              return rejected(SketchCommandError::InvalidInput);
            if (!elementExists(model, typed.elementId))
              return rejected(SketchCommandError::StaleReference);
            if (model.isElementLocked(typed.elementId))
              return rejected(SketchCommandError::Conflict);
            const bool changed = std::any_of(
                model.lines().begin(), model.lines().end(),
                [&typed](const sketch::Line& line) {
                  return line.elementId == typed.elementId &&
                         line.dashed != typed.dashed;
                });
            if (!changed) return accepted();
            model.setElementDashed(typed.elementId, typed.dashed);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds =
                elementGeometryIds(model, {typed.elementId});
            return value;
          } else if constexpr (std::is_same_v<T, SetSelectionDashedCommand>) {
            if (typed.lineIds.empty() && typed.elementIds.empty() &&
                typed.circleIds.empty() && typed.arcIds.empty() &&
                typed.bezierIds.empty())
              return rejected(SketchCommandError::InvalidInput);
            if (std::any_of(typed.lineIds.begin(), typed.lineIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.lineIndex(id);
                            }) ||
                std::any_of(typed.circleIds.begin(), typed.circleIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.circleIndex(id);
                            }) ||
                std::any_of(typed.arcIds.begin(), typed.arcIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.arcIndex(id);
                            }) ||
                std::any_of(typed.bezierIds.begin(), typed.bezierIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.bezierIndex(id);
                            }))
              return rejected(SketchCommandError::StaleReference);
            for (const auto elementId : typed.elementIds)
              if (!elementExists(model, elementId))
                return rejected(SketchCommandError::StaleReference);
            if (std::any_of(typed.lineIds.begin(), typed.lineIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }) ||
                std::any_of(typed.elementIds.begin(), typed.elementIds.end(),
                            [&model](std::size_t id) {
                              return model.isElementLocked(id);
                            }) ||
                std::any_of(typed.circleIds.begin(), typed.circleIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }) ||
                std::any_of(typed.arcIds.begin(), typed.arcIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }) ||
                std::any_of(typed.bezierIds.begin(), typed.bezierIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }))
              return rejected(SketchCommandError::Conflict);
            auto value = accepted(geometryEffects());
            for (const auto id : typed.lineIds) {
              const auto index = model.lineIndex(id);
              if (model.lines()[*index].dashed != typed.dashed)
                value.changedGeometryIds.push_back(id);
            }
            for (const auto id : elementGeometryIds(model, typed.elementIds)) {
              const auto index = model.lineIndex(id);
              if (model.lines()[*index].dashed != typed.dashed)
                value.changedGeometryIds.push_back(id);
            }
            for (const auto id : typed.circleIds) {
              const auto index = model.circleIndex(id);
              if (model.circles()[*index].dashed != typed.dashed)
                value.changedGeometryIds.push_back(id);
            }
            for (const auto id : typed.arcIds) {
              const auto index = model.arcIndex(id);
              if (model.arcs()[*index].dashed != typed.dashed)
                value.changedGeometryIds.push_back(id);
            }
            for (const auto id : typed.bezierIds) {
              const auto index = model.bezierIndex(id);
              if (model.beziers()[*index].dashed != typed.dashed)
                value.changedGeometryIds.push_back(id);
            }
            if (value.changedGeometryIds.empty()) return accepted();
            for (const auto id : typed.lineIds)
              model.setLineDashedById(id, typed.dashed);
            for (const auto id : typed.elementIds)
              model.setElementDashed(id, typed.dashed);
            for (const auto id : typed.circleIds)
              model.setCircleDashedById(id, typed.dashed);
            for (const auto id : typed.arcIds)
              model.setArcDashedById(id, typed.dashed);
            for (const auto id : typed.bezierIds)
              model.setBezierDashedById(id, typed.dashed);
            return value;
          } else if constexpr (std::is_same_v<T, TranslatePointCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                !validPointReference(model, typed.point))
              return rejected(SketchCommandError::StaleReference);
            if (!model.translatePoint(typed.point, typed.dxMm, typed.dyMm))
              return rejected(SketchCommandError::Conflict);
            return accepted(geometryEffects());
          } else if constexpr (std::is_same_v<T, MoveArcEndpointCommand>) {
            if (!finite(typed.target) || !model.arcIndex(typed.arcId))
              return rejected(SketchCommandError::StaleReference);
            if (!model.moveArcEndpointReshapeById(typed.arcId, typed.start,
                                                  typed.target))
              return rejected(SketchCommandError::Conflict);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.arcId);
            return value;
          } else if constexpr (std::is_same_v<T, TranslateLinesCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                typed.lineIds.empty())
              return rejected(SketchCommandError::InvalidInput);
            if (std::any_of(typed.lineIds.begin(), typed.lineIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.lineIndex(id);
                            }))
              return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (std::any_of(typed.lineIds.begin(), typed.lineIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }))
              return rejected(SketchCommandError::Conflict);
            model.translateLinesByIds(typed.lineIds, typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds = typed.lineIds;
            return value;
          } else if constexpr (std::is_same_v<T, TranslateSelectionCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                (typed.elementIds.empty() && typed.circleIds.empty() &&
                 typed.arcIds.empty() && typed.bezierIds.empty()))
              return rejected(SketchCommandError::InvalidInput);
            if (std::any_of(typed.circleIds.begin(), typed.circleIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.circleIndex(id);
                            }) ||
                std::any_of(typed.arcIds.begin(), typed.arcIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.arcIndex(id);
                            }) ||
                std::any_of(typed.bezierIds.begin(), typed.bezierIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.bezierIndex(id);
                            }))
              return rejected(SketchCommandError::StaleReference);
            for (const auto elementId : typed.elementIds)
              if (!elementExists(model, elementId))
                return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (std::any_of(typed.elementIds.begin(), typed.elementIds.end(),
                            [&model](std::size_t id) {
                              return model.isElementLocked(id);
                            }) ||
                std::any_of(typed.circleIds.begin(), typed.circleIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }) ||
                std::any_of(typed.arcIds.begin(), typed.arcIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }) ||
                std::any_of(typed.bezierIds.begin(), typed.bezierIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }))
              return rejected(SketchCommandError::Conflict);
            model.translateSelection(typed.elementIds, typed.circleIds,
                                     typed.arcIds, typed.bezierIds,
                                     typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds =
                elementGeometryIds(model, typed.elementIds);
            value.changedGeometryIds.insert(value.changedGeometryIds.end(),
                                            typed.circleIds.begin(),
                                            typed.circleIds.end());
            value.changedGeometryIds.insert(value.changedGeometryIds.end(),
                                            typed.arcIds.begin(),
                                            typed.arcIds.end());
            value.changedGeometryIds.insert(value.changedGeometryIds.end(),
                                            typed.bezierIds.begin(),
                                            typed.bezierIds.end());
            return value;
          } else if constexpr (std::is_same_v<T, TranslateCircleCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                !model.circleIndex(typed.id))
              return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            model.translateCircleById(typed.id, typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, TranslateArcCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                !model.arcIndex(typed.id))
              return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            model.translateArcById(typed.id, typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, TranslateBezierCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                !model.bezierIndex(typed.id))
              return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            model.translateBezierById(typed.id, typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, MirrorGeometryCommand>) {
            const auto axisIndex = model.lineIndex(typed.axisId);
            if (!axisIndex || typed.sourceIds.empty() ||
                std::find(typed.sourceIds.begin(), typed.sourceIds.end(),
                          typed.axisId) != typed.sourceIds.end())
              return rejected(SketchCommandError::StaleReference);
            const auto axis = model.lines()[*axisIndex];
            const double ax = axis.end.xMm - axis.start.xMm;
            const double ay = axis.end.yMm - axis.start.yMm;
            const double lengthSquared = ax * ax + ay * ay;
            if (lengthSquared <= 1e-12)
              return rejected(SketchCommandError::InvalidInput);
            for (const auto id : typed.sourceIds)
              if (!model.geometryLocation(id))
                return rejected(SketchCommandError::StaleReference);
            const auto mirrored = [&](sketch::Point point) {
              const double parameter =
                  ((point.xMm - axis.start.xMm) * ax +
                   (point.yMm - axis.start.yMm) * ay) /
                  lengthSquared;
              const sketch::Point projection{
                  axis.start.xMm + ax * parameter,
                  axis.start.yMm + ay * parameter};
              return sketch::Point{2.0 * projection.xMm - point.xMm,
                                   2.0 * projection.yMm - point.yMm};
            };
            auto value = accepted(geometryEffects());
            for (const auto sourceId : typed.sourceIds) {
              const auto location = model.geometryLocation(sourceId);
              if (!location)
                return rejected(SketchCommandError::StaleReference);
              switch (location->kind) {
                case sketch::GeometryKind::Line: {
                  const auto source = model.lines()[location->index];
                  const auto index = model.lines().size();
                  model.addLine(mirrored(source.start), mirrored(source.end));
                  if (model.lines().size() != index + 1)
                    return rejected(SketchCommandError::MutationRejected);
                  const auto id = model.lineId(index);
                  if (source.dashed) model.setLineDashedById(id, true);
                  value.changedGeometryIds.push_back(id);
                  break;
                }
                case sketch::GeometryKind::Circle: {
                  const auto source = model.circles()[location->index];
                  const auto index = model.circles().size();
                  model.addCircle(mirrored(source.center), source.radiusMm);
                  if (model.circles().size() != index + 1)
                    return rejected(SketchCommandError::MutationRejected);
                  const auto id = model.circleId(index);
                  if (source.dashed) model.setCircleDashedById(id, true);
                  value.changedGeometryIds.push_back(id);
                  break;
                }
                case sketch::GeometryKind::Arc: {
                  const auto source = model.arcs()[location->index];
                  const auto center = mirrored(source.center);
                  const auto reflectedEnd = mirrored(sketch::arcEndPoint(source));
                  const double startAngle = std::atan2(
                      reflectedEnd.yMm - center.yMm,
                      reflectedEnd.xMm - center.xMm);
                  const auto index = model.arcs().size();
                  model.addArc(center, source.radiusMm, startAngle,
                               source.sweepAngleRad, source.dashed);
                  if (model.arcs().size() != index + 1)
                    return rejected(SketchCommandError::MutationRejected);
                  value.changedGeometryIds.push_back(model.arcId(index));
                  break;
                }
                case sketch::GeometryKind::Bezier: {
                  const auto source = model.beziers()[location->index];
                  std::array<sketch::Point, 4> points{};
                  for (std::size_t point = 0; point < points.size(); ++point)
                    points[point] = mirrored(source.points[point]);
                  const auto index = model.beziers().size();
                  model.addBezier(points[0], points[1], points[2], points[3],
                                  source.dashed);
                  if (model.beziers().size() != index + 1)
                    return rejected(SketchCommandError::MutationRejected);
                  value.changedGeometryIds.push_back(model.bezierId(index));
                  break;
                }
              }
            }
            struct Endpoint {
              sketch::PointReference reference;
              sketch::Point point;
            };
            std::vector<Endpoint> endpoints;
            for (const auto id : value.changedGeometryIds) {
              const auto location = model.geometryLocation(id);
              if (!location) continue;
              if (location->kind == sketch::GeometryKind::Line) {
                const auto& line = model.lines()[location->index];
                endpoints.push_back({{id, true}, line.start});
                endpoints.push_back({{id, false}, line.end});
              } else if (location->kind == sketch::GeometryKind::Arc) {
                const auto& arc = model.arcs()[location->index];
                sketch::PointReference first;
                first.arcId = id;
                first.start = true;
                auto second = first;
                second.start = false;
                endpoints.push_back({first, sketch::arcStartPoint(arc)});
                endpoints.push_back({second, sketch::arcEndPoint(arc)});
              } else if (location->kind == sketch::GeometryKind::Bezier) {
                const auto& bezier = model.beziers()[location->index];
                sketch::PointReference first;
                first.bezierId = id;
                first.bezierPoint = 0;
                auto second = first;
                second.bezierPoint = 3;
                endpoints.push_back({first, bezier.points[0]});
                endpoints.push_back({second, bezier.points[3]});
              }
            }
            for (std::size_t first = 0; first < endpoints.size(); ++first) {
              for (std::size_t second = first + 1; second < endpoints.size();
                   ++second) {
                if (!samePoint(endpoints[first].point, endpoints[second].point,
                               1e-7))
                  continue;
                const bool sameGeometry =
                    (endpoints[first].reference.lineId !=
                         sketch::kInvalidGeometryId &&
                     endpoints[first].reference.lineId ==
                         endpoints[second].reference.lineId) ||
                    (endpoints[first].reference.arcId !=
                         sketch::kInvalidGeometryId &&
                     endpoints[first].reference.arcId ==
                         endpoints[second].reference.arcId) ||
                    (endpoints[first].reference.bezierId !=
                         sketch::kInvalidGeometryId &&
                     endpoints[first].reference.bezierId ==
                         endpoints[second].reference.bezierId);
                if (sameGeometry) continue;
                sketch::Constraint coincident;
                coincident.type = sketch::ConstraintType::Coincident;
                coincident.firstPoint = endpoints[first].reference;
                coincident.secondPoint = endpoints[second].reference;
                const auto id = tryApplyConstraintId(model, coincident);
                if (id != sketch::kInvalidConstraintId) {
                  value.effects.constraintsChanged = true;
                  value.changedConstraintIds.push_back(id);
                }
              }
            }
            return value;
          } else if constexpr (std::is_same_v<T, TrimGeometryCommand>) {
            if (!finite(typed.firstParameter) ||
                !finite(typed.secondParameter) ||
                typed.firstParameter < 0.0 ||
                typed.secondParameter > 1.0 ||
                typed.firstParameter > typed.secondParameter)
              return rejected(SketchCommandError::InvalidInput);
            const auto location = model.geometryLocation(typed.geometryId);
            if (!location)
              return rejected(SketchCommandError::StaleReference);
            constexpr double minimumInterval = 1e-8;
            constexpr double twoPi = 6.28318530717958647692;
            auto value = accepted(geometryEffects());
            value.effects.constraintsChanged = true;
            value.effects.dimensionsChanged = true;
            value.effects.selectionMayBeStale = true;
            const auto addArc = [&model, &value](
                                    sketch::Point center, double radius,
                                    double start, double sweep, bool dashed) {
              const auto index = model.arcs().size();
              model.addArc(center, radius, start, sweep, dashed);
              if (model.arcs().size() != index + 1) return false;
              value.changedGeometryIds.push_back(model.arcId(index));
              return true;
            };
            if (location->kind == sketch::GeometryKind::Line) {
              const auto source = model.lines()[location->index];
              const auto pointAt = [&source](double parameter) {
                return sketch::Point{
                    source.start.xMm +
                        (source.end.xMm - source.start.xMm) * parameter,
                    source.start.yMm +
                        (source.end.yMm - source.start.yMm) * parameter};
              };
              model.removeLine(location->index);
              for (const auto interval :
                   {std::pair{0.0, typed.firstParameter},
                    std::pair{typed.secondParameter, 1.0}}) {
                if (interval.second - interval.first <= minimumInterval)
                  continue;
                const auto index = model.lines().size();
                model.addLine(pointAt(interval.first), pointAt(interval.second));
                if (model.lines().size() != index + 1)
                  return rejected(SketchCommandError::MutationRejected);
                const auto id = model.lineId(index);
                if (source.dashed) model.setLineDashedById(id, true);
                value.changedGeometryIds.push_back(id);
              }
            } else if (location->kind == sketch::GeometryKind::Circle) {
              const auto source = model.circles()[location->index];
              model.removeCircle(location->index);
              if (!typed.fullGeometry) {
                const double sweep =
                    (1.0 - (typed.secondParameter - typed.firstParameter)) *
                    twoPi;
                if (sweep > minimumInterval) {
                  double start = typed.secondParameter * twoPi;
                  start = std::fmod(start, twoPi);
                  if (start < 0.0) start += twoPi;
                  if (!addArc(source.center, source.radiusMm, start, sweep,
                              source.dashed))
                    return rejected(SketchCommandError::MutationRejected);
                }
              }
            } else if (location->kind == sketch::GeometryKind::Arc) {
              const auto source = model.arcs()[location->index];
              model.removeArc(location->index);
              for (const auto interval :
                   {std::pair{0.0, typed.firstParameter},
                    std::pair{typed.secondParameter, 1.0}}) {
                const double sweep = source.sweepAngleRad *
                                     (interval.second - interval.first);
                if (sweep <= minimumInterval) continue;
                if (!addArc(source.center, source.radiusMm,
                            source.startAngleRad +
                                source.sweepAngleRad * interval.first,
                            sweep, source.dashed))
                  return rejected(SketchCommandError::MutationRejected);
              }
            } else {
              const auto source = model.beziers()[location->index];
              const auto lerp = [](sketch::Point first, sketch::Point second,
                                   double parameter) {
                return sketch::Point{
                    first.xMm + (second.xMm - first.xMm) * parameter,
                    first.yMm + (second.yMm - first.yMm) * parameter};
              };
              const auto split = [&lerp](const std::array<sketch::Point, 4>& p,
                                         double parameter) {
                const auto p01 = lerp(p[0], p[1], parameter);
                const auto p12 = lerp(p[1], p[2], parameter);
                const auto p23 = lerp(p[2], p[3], parameter);
                const auto p012 = lerp(p01, p12, parameter);
                const auto p123 = lerp(p12, p23, parameter);
                const auto p0123 = lerp(p012, p123, parameter);
                return std::pair{
                    std::array<sketch::Point, 4>{p[0], p01, p012, p0123},
                    std::array<sketch::Point, 4>{p0123, p123, p23, p[3]}};
              };
              const auto firstSplit = split(source.points,
                                            typed.firstParameter);
              const double rightParameter =
                  typed.firstParameter >= 1.0
                      ? 1.0
                      : (typed.secondParameter - typed.firstParameter) /
                            (1.0 - typed.firstParameter);
              const auto secondSplit = split(firstSplit.second,
                                             rightParameter);
              model.removeBezier(location->index);
              if (typed.firstParameter > minimumInterval) {
                const auto index = model.beziers().size();
                model.addBezier(firstSplit.first[0], firstSplit.first[1],
                                firstSplit.first[2], firstSplit.first[3],
                                source.dashed);
                if (model.beziers().size() != index + 1)
                  return rejected(SketchCommandError::MutationRejected);
                value.changedGeometryIds.push_back(model.bezierId(index));
              }
              if (1.0 - typed.secondParameter > minimumInterval) {
                const auto index = model.beziers().size();
                model.addBezier(secondSplit.second[0], secondSplit.second[1],
                                secondSplit.second[2], secondSplit.second[3],
                                source.dashed);
                if (model.beziers().size() != index + 1)
                  return rejected(SketchCommandError::MutationRejected);
                value.changedGeometryIds.push_back(model.bezierId(index));
              }
            }
            return value;
          } else if constexpr (std::is_same_v<T, ApplySketchDeltaCommand>) {
            if (!typed.delta)
              return rejected(SketchCommandError::InvalidInput);
            if (!model.applyDelta(*typed.delta, typed.forward))
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(geometryEffects());
            value.effects.constraintsChanged = true;
            value.effects.dimensionsChanged = true;
            value.effects.selectionMayBeStale = true;
            return value;
          }
        },
        command);
  } catch (...) {
    static_cast<void>(
        model.rollbackDeltaJournalsToDepth(journalDepthBefore));
    return rejected(SketchCommandError::InternalFailure);
  }

  if (!result.accepted) {
    if (!model.rollbackDeltaJournalsToDepth(journalDepthBefore))
      return rejected(SketchCommandError::InternalFailure);
    return result;
  }
  if (model.deltaJournalDepth() != journalDepthBefore + 1) {
    static_cast<void>(
        model.rollbackDeltaJournalsToDepth(journalDepthBefore));
    return rejected(SketchCommandError::InternalFailure);
  }
  try {
    result.delta = model.finishDeltaJournal();
  } catch (...) {
    static_cast<void>(
        model.rollbackDeltaJournalsToDepth(journalDepthBefore));
    return rejected(SketchCommandError::InternalFailure);
  }
  return result;
}

SketchCommandResult SketchCommandController::executeInTransaction(
    sketch::Sketch& model, SketchTransactionToken token,
    std::uint64_t sketchGeneration, const SketchLiveCommand& command) {
  if (!matches(model, token, sketchGeneration) ||
      model.deltaJournalDepth() != token.journalDepthBefore + 1)
    return rejected(SketchCommandError::TransactionMismatch);
  try {
    return std::visit(
        [&model](const auto& typed) -> SketchCommandResult {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, SetDimensionPlacementCommand>) {
            if (!finite(typed.offsetMm) || !finite(typed.angleRad))
              return rejected(SketchCommandError::InvalidInput);
            const auto index = model.dimensionIndex(typed.id);
            if (!index) return rejected(SketchCommandError::StaleReference);
            if (!model.setDimensionPlacement(*index, typed.offsetMm,
                                             typed.angleRad))
              return rejected(SketchCommandError::MutationRejected);
            auto value = accepted(dimensionEffects());
            value.changedDimensionIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, TranslatePointCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                !validPointReference(model, typed.point))
              return rejected(SketchCommandError::StaleReference);
            if (!model.translatePoint(typed.point, typed.dxMm, typed.dyMm))
              return rejected(SketchCommandError::Conflict);
            return accepted(geometryEffects());
          } else if constexpr (std::is_same_v<T, MoveArcEndpointCommand>) {
            if (!finite(typed.target) || !model.arcIndex(typed.arcId))
              return rejected(SketchCommandError::StaleReference);
            if (!model.moveArcEndpointReshapeById(typed.arcId, typed.start,
                                                  typed.target))
              return rejected(SketchCommandError::Conflict);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.arcId);
            return value;
          } else if constexpr (std::is_same_v<T, TranslateLinesCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                typed.lineIds.empty())
              return rejected(SketchCommandError::InvalidInput);
            if (std::any_of(typed.lineIds.begin(), typed.lineIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.lineIndex(id);
                            }))
              return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (std::any_of(typed.lineIds.begin(), typed.lineIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }))
              return rejected(SketchCommandError::Conflict);
            model.translateLinesByIds(typed.lineIds, typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds = typed.lineIds;
            return value;
          } else if constexpr (std::is_same_v<T, TranslateSelectionCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                (typed.elementIds.empty() && typed.circleIds.empty() &&
                 typed.arcIds.empty() && typed.bezierIds.empty()))
              return rejected(SketchCommandError::InvalidInput);
            if (std::any_of(typed.circleIds.begin(), typed.circleIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.circleIndex(id);
                            }) ||
                std::any_of(typed.arcIds.begin(), typed.arcIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.arcIndex(id);
                            }) ||
                std::any_of(typed.bezierIds.begin(), typed.bezierIds.end(),
                            [&model](sketch::GeometryId id) {
                              return !model.bezierIndex(id);
                            }))
              return rejected(SketchCommandError::StaleReference);
            for (const auto elementId : typed.elementIds)
              if (!elementExists(model, elementId))
                return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (std::any_of(typed.elementIds.begin(), typed.elementIds.end(),
                            [&model](std::size_t id) {
                              return model.isElementLocked(id);
                            }) ||
                std::any_of(typed.circleIds.begin(), typed.circleIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }) ||
                std::any_of(typed.arcIds.begin(), typed.arcIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }) ||
                std::any_of(typed.bezierIds.begin(), typed.bezierIds.end(),
                            [&model](sketch::GeometryId id) {
                              return model.isGeometryLocked(id);
                            }))
              return rejected(SketchCommandError::Conflict);
            model.translateSelection(typed.elementIds, typed.circleIds,
                                     typed.arcIds, typed.bezierIds,
                                     typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds =
                elementGeometryIds(model, typed.elementIds);
            value.changedGeometryIds.insert(value.changedGeometryIds.end(),
                                            typed.circleIds.begin(),
                                            typed.circleIds.end());
            value.changedGeometryIds.insert(value.changedGeometryIds.end(),
                                            typed.arcIds.begin(),
                                            typed.arcIds.end());
            value.changedGeometryIds.insert(value.changedGeometryIds.end(),
                                            typed.bezierIds.begin(),
                                            typed.bezierIds.end());
            return value;
          } else if constexpr (std::is_same_v<T, TranslateCircleCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                !model.circleIndex(typed.id))
              return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            model.translateCircleById(typed.id, typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else if constexpr (std::is_same_v<T, TranslateArcCommand>) {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                !model.arcIndex(typed.id))
              return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            model.translateArcById(typed.id, typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          } else {
            if (!finite(typed.dxMm) || !finite(typed.dyMm) ||
                !model.bezierIndex(typed.id))
              return rejected(SketchCommandError::StaleReference);
            if (zeroTranslation(typed.dxMm, typed.dyMm)) return accepted();
            if (model.isGeometryLocked(typed.id))
              return rejected(SketchCommandError::Conflict);
            model.translateBezierById(typed.id, typed.dxMm, typed.dyMm);
            auto value = accepted(geometryEffects());
            value.changedGeometryIds.push_back(typed.id);
            return value;
          }
        },
        command);
  } catch (...) {
    static_cast<void>(
        model.rollbackDeltaJournalsToDepth(token.journalDepthBefore));
    activeTransaction_.reset();
    return rejected(SketchCommandError::InternalFailure);
  }
}

std::optional<SketchTransactionToken> SketchCommandController::beginTransaction(
    sketch::Sketch& model, std::uint64_t sketchGeneration) {
  if (activeTransaction_ || sketchGeneration == 0) return std::nullopt;
  if (nextTransactionSerial_ == 0) nextTransactionSerial_ = 1;
  const auto journalDepthBefore = model.deltaJournalDepth();
  SketchTransactionToken token{
      nextTransactionSerial_++, sketchGeneration, model.semanticFingerprint(),
      reinterpret_cast<std::uintptr_t>(&model), journalDepthBefore};
  try {
    model.beginDeltaJournal();
  } catch (...) {
    static_cast<void>(model.rollbackDeltaJournalsToDepth(journalDepthBefore));
    return std::nullopt;
  }
  activeTransaction_ = ActiveTransaction{token};
  return token;
}

std::optional<sketch::SketchDelta> SketchCommandController::finishTransaction(
    sketch::Sketch& model, SketchTransactionToken token,
    std::uint64_t sketchGeneration) {
  if (!matches(model, token, sketchGeneration) ||
      model.deltaJournalDepth() != token.journalDepthBefore + 1)
    return std::nullopt;
  sketch::SketchDelta delta;
  try {
    delta = model.finishDeltaJournal();
  } catch (...) {
    static_cast<void>(
        model.rollbackDeltaJournalsToDepth(token.journalDepthBefore));
    activeTransaction_.reset();
    return std::nullopt;
  }
  activeTransaction_.reset();
  return delta;
}

bool SketchCommandController::cancelTransaction(
    sketch::Sketch& model, SketchTransactionToken token,
    std::uint64_t sketchGeneration) noexcept {
  if (!matches(model, token, sketchGeneration) ||
      model.deltaJournalDepth() < token.journalDepthBefore + 1)
    return false;
  if (!model.rollbackDeltaJournalsToDepth(token.journalDepthBefore)) {
    if (model.deltaJournalDepth() <= token.journalDepthBefore)
      activeTransaction_.reset();
    return false;
  }
  activeTransaction_.reset();
  return true;
}

bool SketchCommandController::hasActiveTransaction() const noexcept {
  return activeTransaction_.has_value();
}

bool SketchCommandController::invalidateTransactions(
    sketch::Sketch& model, std::uint64_t sketchGeneration) noexcept {
  if (!activeTransaction_) return true;
  return cancelTransaction(model, activeTransaction_->token,
                           sketchGeneration);
}

bool SketchCommandController::replaceSketch(
    sketch::Sketch& target, const sketch::Sketch& replacement,
    std::uint64_t currentGeneration, std::uint64_t replacementGeneration) {
  if (activeTransaction_ || currentGeneration == 0 ||
      replacementGeneration == 0 ||
      replacementGeneration == currentGeneration)
    return false;
  try {
    sketch::Sketch staged(replacement);
    target = std::move(staged);
  } catch (...) {
    return false;
  }
  return true;
}

bool SketchCommandController::matches(
    const sketch::Sketch& model, SketchTransactionToken token,
    std::uint64_t generation) const noexcept {
  return activeTransaction_ && token.valid() &&
         activeTransaction_->token == token &&
         token.sketchGeneration == generation &&
         token.sketchIdentity == reinterpret_cast<std::uintptr_t>(&model);
}

}  // namespace solidar
