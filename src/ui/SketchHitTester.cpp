#include "ui/SketchHitTester.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace solidar {
namespace {

constexpr double kTieEpsilon = 1e-9;

bool finiteSegment(const SketchScreenSegment& segment) noexcept {
  return SketchHitTester::finite(segment.first) &&
         SketchHitTester::finite(segment.second);
}

bool pointInside(SketchHitPoint point, double minX, double minY,
                 double maxX, double maxY) noexcept {
  return point.x >= minX && point.x <= maxX && point.y >= minY &&
         point.y <= maxY;
}

double orientation(SketchHitPoint a, SketchHitPoint b,
                   SketchHitPoint c) noexcept {
  return (b.x - a.x) * (c.y - a.y) -
         (b.y - a.y) * (c.x - a.x);
}

bool segmentsIntersect(SketchScreenSegment first,
                       SketchScreenSegment second) noexcept {
  const double o1 = orientation(first.first, first.second, second.first);
  const double o2 = orientation(first.first, first.second, second.second);
  const double o3 = orientation(second.first, second.second, first.first);
  const double o4 = orientation(second.first, second.second, first.second);
  constexpr double epsilon = 1e-12;
  const auto within = [=](double value, double firstValue,
                          double secondValue) {
    return value >= std::min(firstValue, secondValue) - epsilon &&
           value <= std::max(firstValue, secondValue) + epsilon;
  };
  const auto onSegment = [&within](SketchScreenSegment segment,
                                    SketchHitPoint point) {
    return within(point.x, segment.first.x, segment.second.x) &&
           within(point.y, segment.first.y, segment.second.y);
  };
  const bool proper =
      ((o1 < -epsilon && o2 > epsilon) ||
       (o1 > epsilon && o2 < -epsilon)) &&
      ((o3 < -epsilon && o4 > epsilon) ||
       (o3 > epsilon && o4 < -epsilon));
  if (proper) return true;
  return (std::abs(o1) <= epsilon && onSegment(first, second.first)) ||
         (std::abs(o2) <= epsilon && onSegment(first, second.second)) ||
         (std::abs(o3) <= epsilon && onSegment(second, first.first)) ||
         (std::abs(o4) <= epsilon && onSegment(second, first.second));
}

bool validPointReference(const sketch::PointReference& reference) noexcept {
  return reference.origin ||
         reference.lineId != sketch::kInvalidGeometryId ||
         reference.circleId != sketch::kInvalidGeometryId ||
         reference.arcId != sketch::kInvalidGeometryId ||
         reference.bezierId != sketch::kInvalidGeometryId ||
         reference.elementCenterId != 0;
}

}  // namespace

bool SketchHitTolerancePolicy::valid() const noexcept {
  const double values[]{entityPx, pointPx, endpointPx, midpointPx,
                        constructionBodyPx, datumAxisPx, dimensionPx,
                        projectionPx, trimPx};
  return std::all_of(std::begin(values), std::end(values), [](double value) {
    return std::isfinite(value) && value >= 0.0;
  });
}

const SketchPickEntityRef* SketchPickResult::entity() const noexcept {
  return std::get_if<SketchPickEntityRef>(&target);
}

const SketchPickPointRef* SketchPickResult::point() const noexcept {
  return std::get_if<SketchPickPointRef>(&target);
}

const SketchPickDatumRef* SketchPickResult::datum() const noexcept {
  return std::get_if<SketchPickDatumRef>(&target);
}

const SketchProjectionEdgeToken* SketchPickResult::projection() const noexcept {
  return std::get_if<SketchProjectionEdgeToken>(&target);
}

const SketchDimensionToken* SketchPickResult::dimension() const noexcept {
  return std::get_if<SketchDimensionToken>(&target);
}

bool SketchHitTester::finite(SketchHitPoint point) noexcept {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

double SketchHitTester::distanceToSegment(
    SketchHitPoint point, SketchScreenSegment segment) noexcept {
  if (!finite(point) || !finiteSegment(segment))
    return std::numeric_limits<double>::infinity();
  const double dx = segment.second.x - segment.first.x;
  const double dy = segment.second.y - segment.first.y;
  const double lengthSquared = dx * dx + dy * dy;
  if (lengthSquared <= 1e-18)
    return std::hypot(point.x - segment.first.x,
                      point.y - segment.first.y);
  const double parameter = std::clamp(
      ((point.x - segment.first.x) * dx +
       (point.y - segment.first.y) * dy) /
          lengthSquared,
      0.0, 1.0);
  const double closestX = segment.first.x + parameter * dx;
  const double closestY = segment.first.y + parameter * dy;
  return std::hypot(point.x - closestX, point.y - closestY);
}

double SketchHitTester::distanceToSegments(
    SketchHitPoint point,
    const std::vector<SketchScreenSegment>& segments) noexcept {
  double distance = std::numeric_limits<double>::infinity();
  for (const auto& segment : segments)
    distance = std::min(distance, distanceToSegment(point, segment));
  return distance;
}

bool SketchHitTester::accepts(const SketchPickTarget& target,
                              const SketchPickFilter& filter) noexcept {
  if (const auto* entity = std::get_if<SketchPickEntityRef>(&target)) {
    if (!filter.entities) return false;
    if (entity->geometryId == sketch::kInvalidGeometryId) return false;
    if (!filter.includeConstruction && entity->construction) return false;
    if (!filter.includeProjected && entity->projected) return false;
    switch (entity->kind) {
      case SketchPickEntityKind::Line: return filter.lines;
      case SketchPickEntityKind::Circle: return filter.circles;
      case SketchPickEntityKind::Arc: return filter.arcs;
      case SketchPickEntityKind::Bezier: return filter.beziers;
    }
    return false;
  }
  if (const auto* point = std::get_if<SketchPickPointRef>(&target)) {
    if (!filter.points) return false;
    // A line midpoint is a derived snap target.  It deliberately has no
    // persistent PointReference; the stable carrier geometry id is what must
    // be revalidated by the caller before applying the hit.
    if (point->kind == SketchPickPointKind::LineMidpoint) {
      return filter.lineMidpoints &&
             point->carrierId != sketch::kInvalidGeometryId;
    }
    if (!validPointReference(point->reference)) return false;
    switch (point->kind) {
      case SketchPickPointKind::LineEndpoint: return filter.lineEndpoints;
      case SketchPickPointKind::CircleCenter: return filter.circleCenters;
      case SketchPickPointKind::ArcEndpoint: return filter.arcEndpoints;
      case SketchPickPointKind::BezierControlPoint:
        return filter.bezierControlPoints;
      case SketchPickPointKind::ElementCenter: return filter.elementCenters;
      case SketchPickPointKind::LineMidpoint: return false;
    }
    return false;
  }
  if (std::holds_alternative<SketchPickDatumRef>(target))
    return filter.datums;
  if (const auto* projection =
          std::get_if<SketchProjectionEdgeToken>(&target))
    return filter.projections && projection->meshRevision != 0;
  if (const auto* dimension = std::get_if<SketchDimensionToken>(&target)) {
    if (!filter.dimensions ||
        dimension->dimensionId == sketch::kInvalidDimensionId)
      return false;
    if ((dimension->hitKind == SketchDimensionHitKind::Geometry &&
         !filter.dimensionGeometry) ||
        (dimension->hitKind == SketchDimensionHitKind::Label &&
         !filter.dimensionLabels))
      return false;
    switch (dimension->kind) {
      case sketch::DimensionKind::LineLength:
      case sketch::DimensionKind::CircleDiameter:
      case sketch::DimensionKind::LineAngle:
      case sketch::DimensionKind::LineDistance:
        return dimension->geometryId != sketch::kInvalidGeometryId;
      case sketch::DimensionKind::PointDistance:
      case sketch::DimensionKind::PointDistanceX:
      case sketch::DimensionKind::PointDistanceY:
        return validPointReference(dimension->firstPoint) &&
               validPointReference(dimension->secondPoint);
    }
    return false;
  }
  return false;
}

std::optional<double> SketchHitTester::candidateDistance(
    const SketchPickCandidate& candidate,
    SketchHitPoint cursor) noexcept {
  if (!candidate.visible || !candidate.enabled || !finite(cursor) ||
      !std::isfinite(candidate.tolerancePx) || candidate.tolerancePx < 0.0)
    return std::nullopt;
  double distance = std::numeric_limits<double>::infinity();
  if (candidate.point && finite(*candidate.point))
    distance = std::hypot(cursor.x - candidate.point->x,
                          cursor.y - candidate.point->y);
  for (const auto& box : candidate.boxes) {
    if (!finite(box.minimum) || !finite(box.maximum)) continue;
    const double minX = std::min(box.minimum.x, box.maximum.x);
    const double maxX = std::max(box.minimum.x, box.maximum.x);
    const double minY = std::min(box.minimum.y, box.maximum.y);
    const double maxY = std::max(box.minimum.y, box.maximum.y);
    if (pointInside(cursor, minX, minY, maxX, maxY)) distance = 0.0;
  }
  if (candidate.orientedBox) {
    const auto& box = *candidate.orientedBox;
    if (finite(box.center) && std::isfinite(box.halfWidth) &&
        std::isfinite(box.halfHeight) && std::isfinite(box.angleRad) &&
        box.halfWidth >= 0.0 && box.halfHeight >= 0.0) {
      const double dx = cursor.x - box.center.x;
      const double dy = cursor.y - box.center.y;
      const double localX = std::cos(box.angleRad) * dx +
                            std::sin(box.angleRad) * dy;
      const double localY = -std::sin(box.angleRad) * dx +
                            std::cos(box.angleRad) * dy;
      if (std::abs(localX) <= box.halfWidth &&
          std::abs(localY) <= box.halfHeight)
        distance = 0.0;
    }
  }
  distance = std::min(distance,
                      distanceToSegments(cursor, candidate.segments));
  // Historical SketchCanvas hit loops used a strict boundary. A candidate
  // exactly at tolerance is intentionally outside.
  if (!std::isfinite(distance) || !(distance < candidate.tolerancePx))
    return std::nullopt;
  return distance;
}

std::optional<SketchPickResult> SketchHitTester::pick(
    const SketchHitScene& scene, SketchHitPoint cursor,
    const SketchPickFilter& filter) noexcept {
  std::optional<SketchPickResult> best;
  std::size_t bestOrder = std::numeric_limits<std::size_t>::max();
  for (const auto& candidate : scene.candidates) {
    if (!accepts(candidate.target, filter)) continue;
    const auto distance = candidateDistance(candidate, cursor);
    if (!distance) continue;
    const bool better =
        !best || candidate.priority < best->priority ||
        (candidate.priority == best->priority &&
         (*distance < best->distancePx - kTieEpsilon ||
          (std::abs(*distance - best->distancePx) <= kTieEpsilon &&
           candidate.stableOrder < bestOrder)));
    if (!better) continue;
    best = SketchPickResult{candidate.target, *distance, candidate.priority};
    bestOrder = candidate.stableOrder;
  }
  return best;
}

std::vector<SketchPickTarget> SketchHitTester::pickInBox(
    const SketchHitScene& scene, SketchHitPoint first,
    SketchHitPoint second, const SketchPickFilter& filter) {
  std::vector<SketchPickTarget> hits;
  if (!finite(first) || !finite(second)) return hits;
  const double minX = std::min(first.x, second.x);
  const double maxX = std::max(first.x, second.x);
  const double minY = std::min(first.y, second.y);
  const double maxY = std::max(first.y, second.y);
  const SketchScreenSegment borders[]{
      {{minX, minY}, {maxX, minY}}, {{maxX, minY}, {maxX, maxY}},
      {{maxX, maxY}, {minX, maxY}}, {{minX, maxY}, {minX, minY}}};
  for (const auto& candidate : scene.candidates) {
    if (!candidate.visible || !candidate.enabled ||
        !accepts(candidate.target, filter))
      continue;
    bool hit = false;
    if (candidate.boxHitPolicy == SketchBoxHitPolicy::CurveBoundsOrCenter) {
      if (candidate.selectionCenter && finite(*candidate.selectionCenter))
        hit = pointInside(*candidate.selectionCenter, minX, minY, maxX, maxY);
      if (!hit && candidate.selectionBounds &&
          finite(candidate.selectionBounds->minimum) &&
          finite(candidate.selectionBounds->maximum)) {
        const auto& bounds = *candidate.selectionBounds;
        const double boundsMinX = std::min(bounds.minimum.x, bounds.maximum.x);
        const double boundsMaxX = std::max(bounds.minimum.x, bounds.maximum.x);
        const double boundsMinY = std::min(bounds.minimum.y, bounds.maximum.y);
        const double boundsMaxY = std::max(bounds.minimum.y, bounds.maximum.y);
        // Match QRectF::intersects: touching edges have zero intersection area
        // and are not a hit. Center containment intentionally remains inclusive.
        hit = maxX > minX && maxY > minY &&
              boundsMaxX > boundsMinX && boundsMaxY > boundsMinY &&
              boundsMaxX > minX && boundsMinX < maxX &&
              boundsMaxY > minY && boundsMinY < maxY;
      }
      if (hit) hits.push_back(candidate.target);
      continue;
    }
    hit = candidate.point &&
          pointInside(*candidate.point, minX, minY, maxX, maxY);
    for (const auto& box : candidate.boxes) {
      if (hit || !finite(box.minimum) || !finite(box.maximum)) break;
      const double boxMinX = std::min(box.minimum.x, box.maximum.x);
      const double boxMaxX = std::max(box.minimum.x, box.maximum.x);
      const double boxMinY = std::min(box.minimum.y, box.maximum.y);
      const double boxMaxY = std::max(box.minimum.y, box.maximum.y);
      hit = boxMaxX >= minX && boxMinX <= maxX &&
            boxMaxY >= minY && boxMinY <= maxY;
    }
    for (const auto& segment : candidate.segments) {
      if (hit) break;
      hit = pointInside(segment.first, minX, minY, maxX, maxY) ||
            pointInside(segment.second, minX, minY, maxX, maxY) ||
            std::any_of(std::begin(borders), std::end(borders),
                        [&segment](const SketchScreenSegment& border) {
                          return segmentsIntersect(segment, border);
                        });
    }
    if (hit) hits.push_back(candidate.target);
  }
  return hits;
}

}  // namespace solidar
