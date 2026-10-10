#include "ui/SketchHitSceneAdapter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace solidar {
namespace {

bool contains(const std::vector<sketch::GeometryId>& ids,
              sketch::GeometryId id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Projected sketch geometry is represented by the model's established
// dashed+Lock contract. The Lock must name this exact persistent geometry ID:
// Sketch::isGeometryLocked deliberately propagates through composite
// elementIds for solver behaviour and would incorrectly project siblings.
bool isDefaultPointReference(const sketch::PointReference& point) noexcept {
  return point.lineId == sketch::kInvalidGeometryId && point.start &&
      point.circleId == sketch::kInvalidGeometryId &&
      point.elementCenterId == 0 &&
      point.arcId == sketch::kInvalidGeometryId && !point.origin;
}

// Keep this exhaustive predicate in parity with ProjectFile's Lock semantic
// validation. The adapter intentionally does not broaden model acceptance: it
// only refuses malformed historical/in-memory constraints as projection tags.
bool isCanonicalProjectedLock(const sketch::Constraint& constraint,
                              sketch::GeometryId id) noexcept {
  return id != sketch::kInvalidGeometryId &&
      constraint.id != sketch::kInvalidConstraintId &&
      constraint.type == sketch::ConstraintType::Lock &&
      constraint.firstGeometry == id &&
      constraint.secondGeometry == sketch::kInvalidGeometryId &&
      isDefaultPointReference(constraint.firstPoint) &&
      isDefaultPointReference(constraint.secondPoint) &&
      std::isfinite(constraint.value);
}

bool isProjectedGeometry(const sketch::Sketch& sketch,
                         sketch::GeometryId id, bool dashed) {
  return dashed && std::any_of(
      sketch.constraints().begin(), sketch.constraints().end(),
      [id](const sketch::Constraint& constraint) {
        return isCanonicalProjectedLock(constraint, id);
      });
}

void appendCurveSegments(std::vector<SketchScreenSegment>& segments,
                         sketch::Point center, double radius,
                         double startAngle, double sweepAngle, int samples,
                         const SketchHitSceneAdapter::Projector& projector) {
  const int count = std::clamp(samples, 8, 512);
  auto previous = projector(
      {center.xMm + radius * std::cos(startAngle),
       center.yMm + radius * std::sin(startAngle)});
  for (int sample = 1; sample <= count; ++sample) {
    const double angle =
        startAngle + sweepAngle * static_cast<double>(sample) / count;
    const auto current = projector(
        {center.xMm + radius * std::cos(angle),
         center.yMm + radius * std::sin(angle)});
    segments.push_back({previous, current});
    previous = current;
  }
}

std::optional<SketchScreenBox> curveBounds(
    const std::vector<SketchScreenSegment>& segments) {
  if (segments.empty()) return std::nullopt;
  double minX = std::numeric_limits<double>::infinity();
  double minY = std::numeric_limits<double>::infinity();
  double maxX = -std::numeric_limits<double>::infinity();
  double maxY = -std::numeric_limits<double>::infinity();
  for (const auto& segment : segments) {
    for (const auto point : {segment.first, segment.second}) {
      if (!SketchHitTester::finite(point)) return std::nullopt;
      minX = std::min(minX, point.x);
      minY = std::min(minY, point.y);
      maxX = std::max(maxX, point.x);
      maxY = std::max(maxY, point.y);
    }
  }
  return SketchScreenBox{{minX, minY}, {maxX, maxY}};
}

}  // namespace

SketchHitScene SketchHitSceneAdapter::build(
    const sketch::Sketch& sketch, const Projector& projector,
    const SketchHitSceneOptions& options) {
  SketchHitScene scene;
  if (!projector || !options.tolerance.valid()) return scene;
  std::size_t order = 0;

  for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
    const auto id = sketch.lineId(index);
    if (id == sketch::kInvalidGeometryId) continue;
    const auto& line = sketch.lines()[index];
    const bool visible = !contains(options.hiddenGeometry, id);
    const bool projected = isProjectedGeometry(sketch, id, line.dashed);
    const bool construction = line.dashed && !projected;
    scene.candidates.push_back(
        {SketchPickEntityRef{SketchPickEntityKind::Line, id, line.elementId,
                              line.dashed, construction, projected},
         {{projector(line.start), projector(line.end)}}, std::nullopt,
         options.tolerance.entityPx, 20, order++, visible, true});

    for (const bool start : {true, false}) {
      const auto point = start ? line.start : line.end;
      scene.candidates.push_back(
          {SketchPickPointRef{SketchPickPointKind::LineEndpoint,
                              sketch::PointReference{id, start}, point, id,
                              line.elementId},
           {}, projector(point), options.tolerance.endpointPx, 0, order++,
           visible, true});
    }
    const sketch::Point midpoint{(line.start.xMm + line.end.xMm) * 0.5,
                                 (line.start.yMm + line.end.yMm) * 0.5};
    scene.candidates.push_back(
        {SketchPickPointRef{SketchPickPointKind::LineMidpoint, {}, midpoint,
                            id, line.elementId},
         {}, projector(midpoint), options.tolerance.midpointPx, 5, order++,
         visible, true});
  }

  for (std::size_t index = 0; index < sketch.circles().size(); ++index) {
    const auto id = sketch.circleId(index);
    if (id == sketch::kInvalidGeometryId) continue;
    const auto& circle = sketch.circles()[index];
    const bool visible = !contains(options.hiddenGeometry, id);
    const bool projected = isProjectedGeometry(sketch, id, circle.dashed);
    const bool construction = circle.dashed && !projected;
    SketchPickCandidate candidate;
    candidate.target = SketchPickEntityRef{
        SketchPickEntityKind::Circle, id, 0, circle.dashed, construction,
        projected};
    appendCurveSegments(candidate.segments, circle.center, circle.radiusMm,
                        0.0, 2.0 * std::numbers::pi, options.curveSamples,
                        projector);
    candidate.boxHitPolicy = SketchBoxHitPolicy::CurveBoundsOrCenter;
    candidate.selectionBounds = curveBounds(candidate.segments);
    candidate.selectionCenter = projector(circle.center);
    candidate.tolerancePx = options.tolerance.entityPx;
    candidate.priority = 20;
    candidate.stableOrder = order++;
    candidate.visible = visible;
    scene.candidates.push_back(std::move(candidate));

    sketch::PointReference center;
    center.circleId = id;
    scene.candidates.push_back(
        {SketchPickPointRef{SketchPickPointKind::CircleCenter, center,
                            circle.center, id, 0},
         {}, projector(circle.center), options.tolerance.pointPx, 0, order++,
         visible, true});
  }

  for (std::size_t index = 0; index < sketch.arcs().size(); ++index) {
    const auto id = sketch.arcId(index);
    if (id == sketch::kInvalidGeometryId) continue;
    const auto& arc = sketch.arcs()[index];
    const bool visible = !contains(options.hiddenGeometry, id);
    const bool projected = isProjectedGeometry(sketch, id, arc.dashed);
    const bool construction = arc.dashed && !projected;
    SketchPickCandidate candidate;
    candidate.target = SketchPickEntityRef{SketchPickEntityKind::Arc, id, 0,
                                             arc.dashed, construction,
                                             projected};
    appendCurveSegments(candidate.segments, arc.center, arc.radiusMm,
                        arc.startAngleRad, arc.sweepAngleRad,
                        options.curveSamples, projector);
    candidate.boxHitPolicy = SketchBoxHitPolicy::CurveBoundsOrCenter;
    candidate.selectionBounds = curveBounds(candidate.segments);
    candidate.selectionCenter = projector(arc.center);
    candidate.tolerancePx = options.tolerance.entityPx;
    candidate.priority = 20;
    candidate.stableOrder = order++;
    candidate.visible = visible;
    scene.candidates.push_back(std::move(candidate));
    for (const bool start : {true, false}) {
      const auto point = start ? sketch::arcStartPoint(arc)
                               : sketch::arcEndPoint(arc);
      sketch::PointReference reference;
      reference.arcId = id;
      reference.start = start;
      scene.candidates.push_back(
          {SketchPickPointRef{SketchPickPointKind::ArcEndpoint, reference,
                              point, id, 0},
           {}, projector(point), options.tolerance.endpointPx, 0, order++,
           visible, true});
    }
  }

  for (const auto elementId : sketch.centerNodeElementIds()) {
    const auto point = sketch.elementCenterPoint(elementId);
    if (!point) continue;
    sketch::PointReference reference;
    reference.elementCenterId = elementId;
    scene.candidates.push_back(
        {SketchPickPointRef{SketchPickPointKind::ElementCenter, reference,
                            *point, sketch::kInvalidGeometryId, elementId},
         {}, projector(*point), options.tolerance.pointPx, 0, order++, true,
         true});
  }

  if (options.origin) {
    scene.candidates.push_back(
        {SketchPickDatumRef{SketchPickDatumKind::Origin, {0.0, 0.0}}, {},
         options.origin, options.tolerance.pointPx, -1, order++, true, true});
  }
  if (options.xAxis) {
    scene.candidates.push_back(
        {SketchPickDatumRef{SketchPickDatumKind::XAxis, {}}, {*options.xAxis},
         std::nullopt, options.tolerance.datumAxisPx, 30, order++, true, true});
  }
  if (options.yAxis) {
    scene.candidates.push_back(
        {SketchPickDatumRef{SketchPickDatumKind::YAxis, {}}, {*options.yAxis},
         std::nullopt, options.tolerance.datumAxisPx, 30, order++, true, true});
  }
  return scene;
}

}  // namespace solidar
