#include "ui/SketchRenderer.h"

#include <QPainter>
#include <QLineF>
#include <QPolygonF>
#include <QRectF>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_set>

namespace solidar {

bool SketchRenderFrame::hasCanonicalOrder() const noexcept {
  if (layers.size() != kSketchRenderPassOrder.size()) return false;
  for (std::size_t index = 0; index < layers.size(); ++index) {
    if (layers[index].pass != kSketchRenderPassOrder[index]) return false;
  }
  return true;
}

std::size_t SketchRenderFrame::layerCount(SketchRenderPass pass) const noexcept {
  return static_cast<std::size_t>(std::count_if(
      layers.begin(), layers.end(),
      [pass](const SketchRenderLayer& layer) { return layer.pass == pass; }));
}

namespace {

QColor withAlpha(QColor color, int alpha) {
  color.setAlpha(alpha);
  return color;
}

std::optional<SketchRenderGeometry> captureRenderGeometry(
    const sketch::Sketch& source) {
  SketchRenderGeometry result;
  result.linePrimitives = source.lines();
  result.circlePrimitives = source.circles();
  result.arcPrimitives = source.arcs();
  result.bezierPrimitives = source.beziers();
  result.lineIds.reserve(result.linePrimitives.size());
  result.circleIds.reserve(result.circlePrimitives.size());
  result.arcIds.reserve(result.arcPrimitives.size());
  result.bezierIds.reserve(result.bezierPrimitives.size());
  for (std::size_t index = 0; index < result.linePrimitives.size(); ++index)
    result.lineIds.push_back(source.lineId(index));
  for (std::size_t index = 0; index < result.circlePrimitives.size(); ++index)
    result.circleIds.push_back(source.circleId(index));
  for (std::size_t index = 0; index < result.arcPrimitives.size(); ++index)
    result.arcIds.push_back(source.arcId(index));
  for (std::size_t index = 0; index < result.bezierPrimitives.size(); ++index)
    result.bezierIds.push_back(source.bezierId(index));
  result.dimensionPrimitives = source.dimensions();
  result.constraintPrimitives = source.constraints();
  result.centerNodeElementIdsData = source.centerNodeElementIds();
  if (!result.buildIndexesAndMetadata()) return std::nullopt;
  return result;
}

bool isDefaultPointReference(const sketch::PointReference& point) noexcept {
  return point.lineId == sketch::kInvalidGeometryId && point.start &&
         point.circleId == sketch::kInvalidGeometryId &&
         point.elementCenterId == 0 &&
         point.arcId == sketch::kInvalidGeometryId &&
         point.bezierId == sketch::kInvalidGeometryId && !point.origin;
}

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

}  // namespace

std::optional<std::size_t> SketchRenderGeometry::lineIndex(
    sketch::GeometryId id) const noexcept {
  const auto found = lineIndexById_.find(id);
  return found == lineIndexById_.end() ? std::nullopt
                                       : std::optional{found->second};
}

std::optional<std::size_t> SketchRenderGeometry::circleIndex(
    sketch::GeometryId id) const noexcept {
  const auto found = circleIndexById_.find(id);
  return found == circleIndexById_.end() ? std::nullopt
                                         : std::optional{found->second};
}

std::optional<std::size_t> SketchRenderGeometry::arcIndex(
    sketch::GeometryId id) const noexcept {
  const auto found = arcIndexById_.find(id);
  return found == arcIndexById_.end() ? std::nullopt
                                      : std::optional{found->second};
}

std::optional<std::size_t> SketchRenderGeometry::bezierIndex(
    sketch::GeometryId id) const noexcept {
  const auto found = bezierIndexById_.find(id);
  return found == bezierIndexById_.end() ? std::nullopt
                                         : std::optional{found->second};
}

std::optional<std::size_t> SketchRenderGeometry::dimensionIndex(
    sketch::DimensionId id) const noexcept {
  const auto found = dimensionIndexById_.find(id);
  return found == dimensionIndexById_.end() ? std::nullopt
                                             : std::optional{found->second};
}

sketch::GeometryId SketchRenderGeometry::lineId(
    std::size_t index) const noexcept {
  return index < lineIds.size() ? lineIds[index]
                                : sketch::kInvalidGeometryId;
}

sketch::GeometryId SketchRenderGeometry::circleId(
    std::size_t index) const noexcept {
  return index < circleIds.size() ? circleIds[index]
                                  : sketch::kInvalidGeometryId;
}

sketch::GeometryId SketchRenderGeometry::arcId(
    std::size_t index) const noexcept {
  return index < arcIds.size() ? arcIds[index]
                               : sketch::kInvalidGeometryId;
}

sketch::GeometryId SketchRenderGeometry::bezierId(
    std::size_t index) const noexcept {
  return index < bezierIds.size() ? bezierIds[index]
                                  : sketch::kInvalidGeometryId;
}

bool SketchRenderGeometry::isGeometryLocked(
    sketch::GeometryId id) const noexcept {
  const auto metadata = style(id);
  return metadata && metadata->locked;
}

std::optional<SketchRenderGeometry::Style> SketchRenderGeometry::style(
    sketch::GeometryId id) const noexcept {
  const auto found = styleById_.find(id);
  return found == styleById_.end() ? std::nullopt
                                   : std::optional{found->second};
}

bool SketchRenderGeometry::buildIndexesAndMetadata() noexcept {
  lineIndexById_.clear();
  circleIndexById_.clear();
  arcIndexById_.clear();
  bezierIndexById_.clear();
  dimensionIndexById_.clear();
  styleById_.clear();
  elementCenterById_.clear();
  indexStats_ = {};
  if (linePrimitives.size() != lineIds.size() ||
      circlePrimitives.size() != circleIds.size() ||
      arcPrimitives.size() != arcIds.size() ||
      bezierPrimitives.size() != bezierIds.size())
    return false;

  const auto geometryCount =
      lineIds.size() + circleIds.size() + arcIds.size() + bezierIds.size();
  lineIndexById_.reserve(lineIds.size());
  circleIndexById_.reserve(circleIds.size());
  arcIndexById_.reserve(arcIds.size());
  bezierIndexById_.reserve(bezierIds.size());
  dimensionIndexById_.reserve(dimensionPrimitives.size());
  styleById_.reserve(geometryCount);
  std::unordered_set<sketch::GeometryId> allIds;
  allIds.reserve(geometryCount);
  const auto indexIds = [&allIds](
                            const std::vector<sketch::GeometryId>& ids,
                            auto& index) {
    for (std::size_t slot = 0; slot < ids.size(); ++slot) {
      const auto id = ids[slot];
      if (id == sketch::kInvalidGeometryId || !allIds.insert(id).second)
        return false;
      index.emplace(id, slot);
    }
    return true;
  };
  if (!indexIds(lineIds, lineIndexById_) ||
      !indexIds(circleIds, circleIndexById_) ||
      !indexIds(arcIds, arcIndexById_) ||
      !indexIds(bezierIds, bezierIndexById_)) {
    lineIndexById_.clear();
    circleIndexById_.clear();
    arcIndexById_.clear();
    bezierIndexById_.clear();
    return false;
  }

  for (std::size_t slot = 0; slot < dimensionPrimitives.size(); ++slot) {
    const auto id = dimensionPrimitives[slot].id;
    if (id == sketch::kInvalidDimensionId ||
        !dimensionIndexById_.emplace(id, slot).second) {
      lineIndexById_.clear();
      circleIndexById_.clear();
      arcIndexById_.clear();
      bezierIndexById_.clear();
      dimensionIndexById_.clear();
      return false;
    }
  }
  indexStats_.indexedDimensions = dimensionPrimitives.size();

  std::unordered_set<std::size_t> centerElements;
  centerElements.reserve(centerNodeElementIdsData.size());
  for (const auto elementId : centerNodeElementIdsData) {
    if (elementId == 0 || !centerElements.insert(elementId).second) return false;
  }
  struct CenterAccumulator {
    sketch::Point sum{};
    std::size_t count{};
  };
  std::unordered_map<std::size_t, CenterAccumulator> centerAccumulators;
  centerAccumulators.reserve(centerElements.size());
  if (!centerElements.empty()) {
    for (const auto& line : linePrimitives) {
      ++indexStats_.centerLineVisits;
      if (!centerElements.contains(line.elementId)) continue;
      auto& accumulator = centerAccumulators[line.elementId];
      accumulator.sum.xMm += line.start.xMm;
      accumulator.sum.yMm += line.start.yMm;
      ++accumulator.count;
    }
    elementCenterById_.reserve(centerAccumulators.size());
    for (const auto& [elementId, accumulator] : centerAccumulators) {
      if (accumulator.count != 4) continue;
      elementCenterById_.emplace(
          elementId,
          sketch::Point{accumulator.sum.xMm * 0.25,
                        accumulator.sum.yMm * 0.25});
    }
  }

  std::unordered_set<sketch::GeometryId> lockedExact;
  std::unordered_set<sketch::GeometryId> projectedExact;
  std::unordered_set<std::size_t> lockedLineElements;
  lockedExact.reserve(constraintPrimitives.size());
  projectedExact.reserve(constraintPrimitives.size());
  lockedLineElements.reserve(constraintPrimitives.size());
  for (const auto& constraint : constraintPrimitives) {
    ++indexStats_.constraintVisits;
    if (constraint.type != sketch::ConstraintType::Lock ||
        constraint.firstGeometry == sketch::kInvalidGeometryId)
      continue;
    const auto id = constraint.firstGeometry;
    if (allIds.find(id) == allIds.end()) continue;
    lockedExact.insert(id);
    if (const auto line = lineIndexById_.find(id);
        line != lineIndexById_.end())
      lockedLineElements.insert(linePrimitives[line->second].elementId);
    if (isCanonicalProjectedLock(constraint, id)) projectedExact.insert(id);
  }

  for (std::size_t slot = 0; slot < lineIds.size(); ++slot) {
    const auto id = lineIds[slot];
    const bool projected = linePrimitives[slot].dashed &&
                           projectedExact.contains(id);
    styleById_.emplace(
        id, Style{lockedLineElements.contains(linePrimitives[slot].elementId),
                  projected, linePrimitives[slot].dashed && !projected});
  }
  for (std::size_t slot = 0; slot < circleIds.size(); ++slot) {
    const auto id = circleIds[slot];
    const bool projected = circlePrimitives[slot].dashed &&
                           projectedExact.contains(id);
    styleById_.emplace(
        id, Style{lockedExact.contains(id), projected,
                  circlePrimitives[slot].dashed && !projected});
  }
  for (std::size_t slot = 0; slot < arcIds.size(); ++slot) {
    const auto id = arcIds[slot];
    const bool projected = arcPrimitives[slot].dashed &&
                           projectedExact.contains(id);
    styleById_.emplace(
        id, Style{lockedExact.contains(id), projected,
                  arcPrimitives[slot].dashed && !projected});
  }
  for (std::size_t slot = 0; slot < bezierIds.size(); ++slot) {
    const auto id = bezierIds[slot];
    const bool projected = bezierPrimitives[slot].dashed &&
                           projectedExact.contains(id);
    styleById_.emplace(
        id, Style{lockedExact.contains(id), projected,
                  bezierPrimitives[slot].dashed && !projected});
  }
  indexStats_.indexedGeometry = geometryCount;
  return true;
}

const SketchRenderGeometry::IndexStats& SketchRenderGeometry::indexStats()
    const noexcept {
  return indexStats_;
}

bool SketchRenderGeometry::hasCompleteIndexInvariant() const noexcept {
  const auto geometryCount = lineIds.size() + circleIds.size() + arcIds.size() +
                             bezierIds.size();
  return lineIndexById_.size() == lineIds.size() &&
         circleIndexById_.size() == circleIds.size() &&
         arcIndexById_.size() == arcIds.size() &&
         bezierIndexById_.size() == bezierIds.size() &&
         dimensionIndexById_.size() == dimensionPrimitives.size() &&
         styleById_.size() == geometryCount &&
         indexStats_.indexedGeometry == geometryCount &&
         indexStats_.indexedDimensions == dimensionPrimitives.size();
}

std::optional<sketch::Point> SketchRenderGeometry::elementCenterPoint(
    std::size_t elementId) const noexcept {
  const auto found = elementCenterById_.find(elementId);
  return found == elementCenterById_.end() ? std::nullopt
                                           : std::optional{found->second};
}

std::optional<sketch::Point> SketchRenderGeometry::referencedPoint(
    sketch::PointReference reference) const noexcept {
  if (reference.origin) return sketch::Point{};
  if (reference.elementCenterId != 0)
    return elementCenterPoint(reference.elementCenterId);
  if (reference.circleId != sketch::kInvalidGeometryId) {
    const auto index = circleIndex(reference.circleId);
    return index ? std::optional{circlePrimitives[*index].center}
                 : std::nullopt;
  }
  if (reference.arcId != sketch::kInvalidGeometryId) {
    const auto index = arcIndex(reference.arcId);
    if (!index) return std::nullopt;
    return reference.start ? sketch::arcStartPoint(arcPrimitives[*index])
                           : sketch::arcEndPoint(arcPrimitives[*index]);
  }
  if (reference.bezierId != sketch::kInvalidGeometryId) {
    const auto index = bezierIndex(reference.bezierId);
    if (!index || reference.bezierPoint >= 4) return std::nullopt;
    return bezierPrimitives[*index].points[reference.bezierPoint];
  }
  const auto index = lineIndex(reference.lineId);
  if (!index) return std::nullopt;
  return reference.start ? linePrimitives[*index].start
                         : linePrimitives[*index].end;
}

std::shared_ptr<const SketchRenderScene> SketchRenderSceneCache::resolve(
    std::uint64_t sourceRevision, const sketch::Sketch& sketch,
    const sketch::Sketch& referenceProfile,
    std::span<const SketchSceneReference> sceneSketches,
    std::shared_ptr<const BodyRenderMesh> referenceBodyMesh,
    std::shared_ptr<const BodyRenderMesh> referenceFaceMesh,
    std::span<const std::shared_ptr<const BodyRenderMesh>> sceneBodyMeshes,
    const SketchPlacement& referencePlacement,
    double referenceWidthMm, double referenceDepthMm,
    double referenceHeightMm, bool referenceBodyVisible,
    bool realReferenceBodyVisible, bool referenceProfileVisible,
    std::span<const SketchSceneImageReference> sceneImages) {
  if (cached_ && cached_->sourceRevision == sourceRevision) return cached_;

  auto committed = captureRenderGeometry(sketch);
  auto profile = captureRenderGeometry(referenceProfile);
  std::vector<SketchRenderSceneReference> capturedReferences;
  capturedReferences.reserve(sceneSketches.size());
  bool valid = committed.has_value() && profile.has_value();
  if (valid) {
    for (const auto& reference : sceneSketches) {
      auto geometry = captureRenderGeometry(reference.geometry);
      if (!geometry) {
        valid = false;
        capturedReferences.clear();
        break;
      }
      capturedReferences.push_back(
          {std::move(*geometry), reference.placement});
    }
  }
  if (!valid) {
    auto invalidScene = std::make_shared<SketchRenderScene>();
    invalidScene->sourceRevision = sourceRevision;
    cached_ = std::move(invalidScene);
    ++buildCount_;
    return cached_;
  }

  auto scene = std::make_shared<SketchRenderScene>();
  scene->sourceRevision = sourceRevision;
  scene->sketch = std::move(*committed);
  scene->referenceProfile = std::move(*profile);
  scene->sceneSketches = std::move(capturedReferences);
  scene->sceneImages.assign(sceneImages.begin(), sceneImages.end());
  scene->valid = true;
  scene->referenceBodyMesh = std::move(referenceBodyMesh);
  scene->referenceFaceMesh = std::move(referenceFaceMesh);
  scene->sceneBodyMeshes.assign(sceneBodyMeshes.begin(), sceneBodyMeshes.end());
  scene->referencePlacement = referencePlacement;
  scene->referenceWidthMm = referenceWidthMm;
  scene->referenceDepthMm = referenceDepthMm;
  scene->referenceHeightMm = referenceHeightMm;
  scene->referenceBodyVisible = referenceBodyVisible;
  scene->realReferenceBodyVisible = realReferenceBodyVisible;
  scene->referenceProfileVisible = referenceProfileVisible;
  cached_ = std::move(scene);
  ++buildCount_;
  return cached_;
}

void SketchRenderSceneCache::invalidate() noexcept { cached_.reset(); }

std::size_t SketchRenderSceneCache::buildCount() const noexcept {
  return buildCount_;
}

SketchRenderPalette sketchRenderPalette(const ThemeColors& theme,
                                        const QPalette& widgetPalette) {
  SketchRenderPalette result;
  result.background = theme.sketchBackground;
  result.gridMinor = theme.gridMinor;
  result.rulerBackground = theme.sketchRulerBackground;
  result.rulerBorder = theme.borderStrong;
  result.rulerText = theme.textSecondary;
  result.axisX = theme.axisX;
  result.axisY = theme.axisY;
  result.datum = theme.sketchDatum;
  result.sceneFill = withAlpha(widgetPalette.color(QPalette::Mid), 72);
  result.sceneEdge = withAlpha(widgetPalette.color(QPalette::Text), 205);
  result.sceneHover = widgetPalette.color(QPalette::Highlight);
  result.sceneSketch = widgetPalette.color(QPalette::Highlight);
  result.referenceFill = withAlpha(theme.borderStrong, 75);
  result.referenceEdge = theme.sketchDatum;
  result.committed = theme.sketchCommitted;
  result.committedLocked = theme.sketchCommittedLocked;
  result.selected = theme.sketchSelected;
  result.selectedLocked = theme.sketchSelectedLocked;
  result.endpointFill = theme.sketchEndpointFill;
  result.trim = theme.sketchTrim;
  result.projection = theme.sketchProjection;
  result.constraint = theme.sketchConstraint;
  result.dimension = theme.sketchDimension;
  result.dimensionSelected = theme.sketchDimensionSelected;
  result.dimensionLabelBackground = withAlpha(theme.sketchBackground, 235);
  result.dimensionText = theme.sketchDimensionText;
  result.dimensionSelectedText = theme.sketchDimensionSelectedText;
  result.transient = theme.sketchTransient;
  result.transientFill = withAlpha(theme.sketchTransient, 48);
  result.transientSurface = withAlpha(theme.sketchEndpointFill, 235);
  result.selectionBoxOutline = theme.sketchSelected;
  result.selectionBoxFill = withAlpha(theme.sketchSelected, 32);
  result.hudText = theme.sketchHud;
  return result;
}

namespace {

constexpr double kRulerTop = 30.0;
constexpr double kRulerLeft = 44.0;
constexpr double kTrimTwoPi = 2.0 * std::numbers::pi;

double niceRulerStep(double pixelsPerMm) {
  const double targetMm = 75.0 / std::max(0.01, pixelsPerMm);
  const double magnitude = std::pow(10.0, std::floor(std::log10(targetMm)));
  const double normalized = targetMm / magnitude;
  const double factor = normalized <= 1.0 ? 1.0
                        : normalized <= 2.0 ? 2.0
                        : normalized <= 5.0 ? 5.0
                                            : 10.0;
  return factor * magnitude;
}

std::pair<sketch::Point, sketch::Point> pointDimensionWitness(
    sketch::Point first, sketch::Point second, sketch::DimensionKind kind) {
  switch (kind) {
    case sketch::DimensionKind::PointDistanceX:
      return {first, {second.xMm, first.yMm}};
    case sketch::DimensionKind::PointDistanceY:
      return {first, {first.xMm, second.yMm}};
    default:
      return {first, second};
  }
}

double angularDimensionRadiusPx(double offsetMm, double pixelsPerMm,
                                double viewportExtentPx) noexcept {
  const double safeScale = std::isfinite(pixelsPerMm)
                               ? std::max(0.0, pixelsPerMm)
                               : 0.0;
  const double extent = std::isfinite(viewportExtentPx)
                            ? std::max(0.0, viewportExtentPx)
                            : 0.0;
  const double maximum = std::clamp(extent * 0.22, 48.0, 180.0);
  const double requested = std::isfinite(offsetMm)
                               ? std::abs(offsetMm) * safeScale
                               : 16.0;
  return std::clamp(requested, 16.0, maximum);
}

QString pointDimensionModeName(SketchPointDimensionMode mode) {
  switch (mode) {
    case SketchPointDimensionMode::X:
      return QStringLiteral("x");
    case SketchPointDimensionMode::Y:
      return QStringLiteral("y");
    case SketchPointDimensionMode::Aligned:
      return QStringLiteral("aligned");
  }
  return QStringLiteral("aligned");
}

template <typename MapPointFn>
std::optional<QPointF> lineIntersectionScreen(const sketch::Line& first,
                                              const sketch::Line& second,
                                              MapPointFn&& mapPointFn) {
  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double determinant = ax * by - ay * bx;
  if (std::abs(determinant) <= 1e-9) return std::nullopt;

  const double dx = second.start.xMm - first.start.xMm;
  const double dy = second.start.yMm - first.start.yMm;
  const double t = (dx * by - dy * bx) / determinant;

  return mapPointFn(sketch::Point{
      first.start.xMm + t * ax,
      first.start.yMm + t * ay});
}
QPointF angleRayTowardSegment(QPointF intersection,
                              QPointF start,
                              QPointF end) {
  const QPointF toStart = start - intersection;
  const QPointF toEnd = end - intersection;

  const double startLength =
      std::hypot(toStart.x(), toStart.y());
  const double endLength =
      std::hypot(toEnd.x(), toEnd.y());

  constexpr double epsilon = 1e-6;

  if (startLength <= epsilon && endLength <= epsilon)
    return {};
  if (startLength <= epsilon)
    return toEnd;
  if (endLength <= epsilon)
    return toStart;

  const double dot = QPointF::dotProduct(toStart, toEnd);

  if (dot >= 0.0)
    return startLength <= endLength ? toStart : toEnd;

  return startLength >= endLength ? toStart : toEnd;
}

template <typename MapPointFn>
std::optional<std::pair<QPointF, QPointF>> angleSectorRays(
    const sketch::Line& firstLine,
    const sketch::Line& secondLine,
    QPointF center,
    double offsetMm,
    MapPointFn&& mapPointFn) {
  QPointF firstDirection = angleRayTowardSegment(
      center, mapPointFn(firstLine.start), mapPointFn(firstLine.end));
  QPointF secondDirection = angleRayTowardSegment(
      center, mapPointFn(secondLine.start), mapPointFn(secondLine.end));

  const auto sameScreenVertex = [&mapPointFn](sketch::Point a,
                                              sketch::Point b) {
    return QLineF(mapPointFn(a), mapPointFn(b)).length() <= 0.5;
  };

  if (sameScreenVertex(firstLine.start, secondLine.start)) {
    firstDirection =
        mapPointFn(firstLine.end) - mapPointFn(firstLine.start);
    secondDirection =
        mapPointFn(secondLine.end) - mapPointFn(secondLine.start);
  } else if (sameScreenVertex(firstLine.start, secondLine.end)) {
    firstDirection =
        mapPointFn(firstLine.end) - mapPointFn(firstLine.start);
    secondDirection =
        mapPointFn(secondLine.start) - mapPointFn(secondLine.end);
  } else if (sameScreenVertex(firstLine.end, secondLine.start)) {
    firstDirection =
        mapPointFn(firstLine.start) - mapPointFn(firstLine.end);
    secondDirection =
        mapPointFn(secondLine.end) - mapPointFn(secondLine.start);
  } else if (sameScreenVertex(firstLine.end, secondLine.end)) {
    firstDirection =
        mapPointFn(firstLine.start) - mapPointFn(firstLine.end);
    secondDirection =
        mapPointFn(secondLine.start) - mapPointFn(secondLine.end);
  }

  const double firstLength =
      std::hypot(firstDirection.x(), firstDirection.y());
  const double secondLength =
      std::hypot(secondDirection.x(), secondDirection.y());
  if (firstLength <= 1.0 || secondLength <= 1.0)
    return std::nullopt;

  firstDirection /= firstLength;
  secondDirection /= secondLength;

  if (offsetMm < 0.0) {
    firstDirection = -firstDirection;
    secondDirection = -secondDirection;
  }

  return std::pair{firstDirection, secondDirection};
}

std::optional<QPointF> nearestSegmentEndpointTo(
    QPointF point, QPointF start, QPointF end) {
  const double firstDistance = QLineF(point, start).length();
  const double secondDistance = QLineF(point, end).length();

  if (firstDistance <= 0.5 || secondDistance <= 0.5)
    return std::nullopt;

  return firstDistance <= secondDistance
             ? std::optional<QPointF>{start}
             : std::optional<QPointF>{end};
}
double pointSegmentDistance(QPointF point, QPointF start, QPointF end) {
  const QPointF segment = end - start;
  const double lengthSquared = QPointF::dotProduct(segment, segment);
  if (lengthSquared == 0.0) return QLineF(point, start).length();
  const double t = std::clamp(
      QPointF::dotProduct(point - start, segment) / lengthSquared, 0.0, 1.0);
  return QLineF(point, start + segment * t).length();
}

std::optional<std::pair<sketch::Point, double>> circleThroughThreePoints(
    sketch::Point first, sketch::Point second, sketch::Point third) {
  const double determinant = 2.0 *
      (first.xMm * (second.yMm - third.yMm) +
       second.xMm * (third.yMm - first.yMm) +
       third.xMm * (first.yMm - second.yMm));
  if (std::abs(determinant) < 1e-9) return std::nullopt;
  const double a = first.xMm * first.xMm + first.yMm * first.yMm;
  const double b = second.xMm * second.xMm + second.yMm * second.yMm;
  const double c = third.xMm * third.xMm + third.yMm * third.yMm;
  sketch::Point center{
      (a * (second.yMm - third.yMm) + b * (third.yMm - first.yMm) +
       c * (first.yMm - second.yMm)) / determinant,
      (a * (third.xMm - second.xMm) + b * (first.xMm - third.xMm) +
       c * (second.xMm - first.xMm)) / determinant};
  return std::pair{center, std::hypot(center.xMm - first.xMm,
                                      center.yMm - first.yMm)};
}

std::optional<sketch::Arc> arcThroughThreePoints(
    sketch::Point first, sketch::Point through, sketch::Point last) {
  const auto circle = circleThroughThreePoints(first, through, last);
  if (!circle || circle->second <= 1e-9)
    return std::nullopt;

  constexpr double kTwoPi = 6.28318530717958647692;
  const auto normalize = [](double angle) {
    constexpr double twoPi = 6.28318530717958647692;
    angle = std::fmod(angle, twoPi);
    if (angle < 0.0) angle += twoPi;
    return angle;
  };

  const auto& center = circle->first;
  const double start = std::atan2(first.yMm - center.yMm,
                                  first.xMm - center.xMm);
  const double middle = std::atan2(through.yMm - center.yMm,
                                   through.xMm - center.xMm);
  const double end = std::atan2(last.yMm - center.yMm,
                                last.xMm - center.xMm);

  const double ccwSweep = normalize(end - start);
  const double middleFromStart = normalize(middle - start);

  if (ccwSweep > 1e-9 &&
      middleFromStart <= ccwSweep + 1e-9) {
    return sketch::Arc{center, circle->second, normalize(start), ccwSweep,
                       false};
  }

  const double reverseSweep = normalize(start - end);
  if (reverseSweep <= 1e-9 || reverseSweep >= kTwoPi - 1e-9)
    return std::nullopt;

  return sketch::Arc{center, circle->second, normalize(end), reverseSweep,
                     false};
}

sketch::Point arcSagittaPoint(sketch::Point first,
                              sketch::Point last,
                              double signedSagittaMm) {
  const double dx = last.xMm - first.xMm;
  const double dy = last.yMm - first.yMm;
  const double chord = std::hypot(dx, dy);
  if (chord <= 1e-9) return first;

  const sketch::Point middle{(first.xMm + last.xMm) * 0.5,
                             (first.yMm + last.yMm) * 0.5};
  const double nx = -dy / chord;
  const double ny = dx / chord;
  return {middle.xMm + nx * signedSagittaMm,
          middle.yMm + ny * signedSagittaMm};
}

double signedArcSagitta(sketch::Point first,
                        sketch::Point last,
                        sketch::Point point) {
  const double dx = last.xMm - first.xMm;
  const double dy = last.yMm - first.yMm;
  const double chord = std::hypot(dx, dy);
  if (chord <= 1e-9) return 0.0;

  const sketch::Point middle{(first.xMm + last.xMm) * 0.5,
                             (first.yMm + last.yMm) * 0.5};
  const double nx = -dy / chord;
  const double ny = dx / chord;
  return (point.xMm - middle.xMm) * nx +
         (point.yMm - middle.yMm) * ny;
}

std::optional<sketch::Arc> arcFromChordSagitta(
    sketch::Point first, sketch::Point last, double signedSagittaMm) {
  if (std::hypot(last.xMm - first.xMm, last.yMm - first.yMm) <= 1e-9 ||
      std::abs(signedSagittaMm) <= 1e-9)
    return std::nullopt;
  return arcThroughThreePoints(
      first, arcSagittaPoint(first, last, signedSagittaMm), last);
}

double pointLineDistance(sketch::Point point, const sketch::Line& line) {
  const QPointF p(point.xMm, point.yMm);
  return pointSegmentDistance(p, QPointF(line.start.xMm, line.start.yMm),
                              QPointF(line.end.xMm, line.end.yMm));
}

double infiniteLineDistance(sketch::Point point, const sketch::Line& line) {
  const double dx = line.end.xMm - line.start.xMm;
  const double dy = line.end.yMm - line.start.yMm;
  const double length = std::hypot(dx, dy);
  if (length < 1e-9) return std::numeric_limits<double>::max();
  return std::abs(dy * point.xMm - dx * point.yMm +
                  line.end.xMm * line.start.yMm -
                  line.end.yMm * line.start.xMm) / length;
}

struct TwoTangentCirclePreview {
  sketch::Point center;
  double radiusMm{};
};

std::optional<TwoTangentCirclePreview>
twoTangentCircleForRadius(
    const sketch::Line& first,
    const sketch::Line& second,
    double radiusMm,
    sketch::Point hint) {
  if (!std::isfinite(radiusMm) ||
      radiusMm <= 1e-6)
    return std::nullopt;

  const double firstDx =
      first.end.xMm - first.start.xMm;
  const double firstDy =
      first.end.yMm - first.start.yMm;
  const double secondDx =
      second.end.xMm - second.start.xMm;
  const double secondDy =
      second.end.yMm - second.start.yMm;

  const double firstLengthSquared =
      firstDx * firstDx + firstDy * firstDy;
  const double secondLengthSquared =
      secondDx * secondDx + secondDy * secondDy;

  if (firstLengthSquared <= 1e-12 ||
      secondLengthSquared <= 1e-12)
    return std::nullopt;

  const double firstLength =
      std::sqrt(firstLengthSquared);
  const double secondLength =
      std::sqrt(secondLengthSquared);

  const double n1x = -firstDy / firstLength;
  const double n1y = firstDx / firstLength;
  const double n2x = -secondDy / secondLength;
  const double n2y = secondDx / secondLength;

  const double determinant =
      n1x * n2y - n1y * n2x;

  if (std::abs(determinant) <= 1e-10)
    return std::nullopt;

  const double c1 =
      n1x * first.start.xMm +
      n1y * first.start.yMm;
  const double c2 =
      n2x * second.start.xMm +
      n2y * second.start.yMm;

  std::optional<TwoTangentCirclePreview> best;
  double bestMovement =
      std::numeric_limits<double>::max();

  for (const double s1 : {-1.0, 1.0}) {
    for (const double s2 : {-1.0, 1.0}) {
      const double r1 = c1 + s1 * radiusMm;
      const double r2 = c2 + s2 * radiusMm;

      const sketch::Point center{
          (r1 * n2y - n1y * r2) / determinant,
          (n1x * r2 - r1 * n2x) / determinant};

      if (!std::isfinite(center.xMm) ||
          !std::isfinite(center.yMm))
        continue;

      // TWO-TANGENT FINITE SEGMENT DIAMETER LIMIT
      const double firstT =
          ((center.xMm - first.start.xMm) * firstDx +
           (center.yMm - first.start.yMm) * firstDy) /
          firstLengthSquared;

      const double secondT =
          ((center.xMm - second.start.xMm) * secondDx +
           (center.yMm - second.start.yMm) * secondDy) /
          secondLengthSquared;

      constexpr double finiteTolerance = 1e-8;

      if (firstT < -finiteTolerance ||
          firstT > 1.0 + finiteTolerance ||
          secondT < -finiteTolerance ||
          secondT > 1.0 + finiteTolerance)
        continue;

      const double movement =
          std::hypot(
              center.xMm - hint.xMm,
              center.yMm - hint.yMm);

      if (movement < bestMovement) {
        bestMovement = movement;
        best = TwoTangentCirclePreview{
            center,
            radiusMm};
      }
    }
  }

  return best;
}

std::optional<TwoTangentCirclePreview>
clampedTwoTangentCircleForRadius(
    const sketch::Line& first,
    const sketch::Line& second,
    double requestedRadiusMm,
    sketch::Point hint) {
  const double requested =
      std::max(0.01, requestedRadiusMm);

  if (const auto exact =
          twoTangentCircleForRadius(
              first,
              second,
              requested,
              hint))
    return exact;

  double low = 0.01;
  double high = requested;

  std::optional<TwoTangentCirclePreview> best =
      twoTangentCircleForRadius(
          first,
          second,
          low,
          hint);

  if (!best)
    return std::nullopt;

  for (int iteration = 0;
       iteration < 48;
       ++iteration) {
    const double mid =
        (low + high) * 0.5;

    const auto candidate =
        twoTangentCircleForRadius(
            first,
            second,
            mid,
            hint);

    if (candidate) {
      low = mid;
      best = candidate;
    } else {
      high = mid;
    }
  }

  return best;
}
std::optional<TwoTangentCirclePreview>
twoTangentCircleFromCursor(
    const sketch::Line& first,
    const sketch::Line& second,
    sketch::Point cursor) {
  const double firstDx =
      first.end.xMm - first.start.xMm;
  const double firstDy =
      first.end.yMm - first.start.yMm;
  const double secondDx =
      second.end.xMm - second.start.xMm;
  const double secondDy =
      second.end.yMm - second.start.yMm;

  const double firstLength =
      std::hypot(firstDx, firstDy);
  const double secondLength =
      std::hypot(secondDx, secondDy);

  if (firstLength <= 1e-9 ||
      secondLength <= 1e-9)
    return std::nullopt;

  const double n1x = -firstDy / firstLength;
  const double n1y = firstDx / firstLength;
  const double n2x = -secondDy / secondLength;
  const double n2y = secondDx / secondLength;

  const double determinant =
      n1x * n2y - n1y * n2x;

  if (std::abs(determinant) <= 1e-10)
    return std::nullopt;

  const double c1 =
      n1x * first.start.xMm +
      n1y * first.start.yMm;
  const double c2 =
      n2x * second.start.xMm +
      n2y * second.start.yMm;

  const sketch::Point base{
      (c1 * n2y - n1y * c2) /
          determinant,
      (n1x * c2 - c1 * n2x) /
          determinant};

  std::optional<TwoTangentCirclePreview> best;
  double bestScore =
      std::numeric_limits<double>::max();

  for (const double s1 : {-1.0, 1.0}) {
    for (const double s2 : {-1.0, 1.0}) {
      const sketch::Point direction{
          (s1 * n2y - n1y * s2) /
              determinant,
          (n1x * s2 - s1 * n2x) /
              determinant};

      const double directionSquared =
          direction.xMm * direction.xMm +
          direction.yMm * direction.yMm;

      if (directionSquared <= 1e-12)
        continue;

      const double cursorX =
          cursor.xMm - base.xMm;
      const double cursorY =
          cursor.yMm - base.yMm;

      double radiusMm =
          (cursorX * direction.xMm +
           cursorY * direction.yMm) /
          directionSquared;

      radiusMm =
          std::max(0.01, radiusMm);

      const sketch::Point center{
          base.xMm +
              direction.xMm * radiusMm,
          base.yMm +
              direction.yMm * radiusMm};

      double score =
          std::hypot(
              center.xMm - cursor.xMm,
              center.yMm - cursor.yMm);

      const auto finitePenalty =
          [center](const sketch::Line& line) {
            const double dx =
                line.end.xMm - line.start.xMm;
            const double dy =
                line.end.yMm - line.start.yMm;
            const double lengthSquared =
                dx * dx + dy * dy;

            if (lengthSquared <= 1e-12)
              return 1000000.0;

            const double t =
                ((center.xMm -
                      line.start.xMm) *
                     dx +
                 (center.yMm -
                      line.start.yMm) *
                     dy) /
                lengthSquared;

            if (t < 0.0)
              return -t *
                     std::sqrt(lengthSquared);

            if (t > 1.0)
              return (t - 1.0) *
                     std::sqrt(lengthSquared);

            return 0.0;
          };

      score +=
          0.25 *
          (finitePenalty(first) +
           finitePenalty(second));

      if (score < bestScore) {
        bestScore = score;
        best =
            TwoTangentCirclePreview{
                center,
                radiusMm};
      }
    }
  }

  return best;
}

bool parallelLinePair(const sketch::Line& first,
                      const sketch::Line& second) {
  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double al = std::hypot(ax, ay);
  const double bl = std::hypot(bx, by);
  return al > 1e-9 && bl > 1e-9 &&
         std::abs(ax * by - ay * bx) / (al * bl) <= 1e-6;
}

std::optional<std::pair<sketch::Point, sketch::Point>>
parallelLineDistanceWitness(const sketch::Line& first,
                            const sketch::Line& second) {
  if (!parallelLinePair(first, second)) return std::nullopt;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double length2 = bx * bx + by * by;
  if (length2 <= 1e-12) return std::nullopt;
  const sketch::Point middle{
      (first.start.xMm + first.end.xMm) * 0.5,
      (first.start.yMm + first.end.yMm) * 0.5};
  const double t =
      ((middle.xMm - second.start.xMm) * bx +
       (middle.yMm - second.start.yMm) * by) / length2;
  return std::pair{
      middle,
      sketch::Point{second.start.xMm + bx * t,
                    second.start.yMm + by * t}};
}

double parallelLineDistanceMm(const sketch::Line& first,
                              const sketch::Line& second) {
  const auto witness = parallelLineDistanceWitness(first, second);
  return witness
             ? std::hypot(witness->second.xMm - witness->first.xMm,
                          witness->second.yMm - witness->first.yMm)
             : 0.0;
}

}  // namespace

namespace {

class SketchFrameRecorder final {
 public:
  explicit SketchFrameRecorder(const SketchRenderSnapshot& source)
      : snapshot(source),
        scene_(*source.scene),
        sketch_(scene_.sketch),
        selectedLineIds_(source.selectedLineIds.begin(),
                         source.selectedLineIds.end()),
        selectedElementIds_(source.selectedElementIds.begin(),
                            source.selectedElementIds.end()),
        selectedCircleIds_(source.selectedCircleIds.begin(),
                           source.selectedCircleIds.end()),
        selectedArcIds_(source.selectedArcIds.begin(),
                        source.selectedArcIds.end()),
        selectedBezierIds_(source.selectedBezierIds.begin(),
                           source.selectedBezierIds.end()),
        selectionKind_(source.selectionKind),
        selectionLineId_(source.selectionLineId),
        selectionCircleId_(source.selectionCircleId),
        selectionArcId_(source.selectionArcId),
        selectionBezierId_(source.selectionBezierId),
        constructionHover_(source.constructionHover),
        hoveredProjectionEdge_(source.hoveredProjectionEdge),
        hoverPoint_(source.hoverPoint),
        pixelsPerMm_(source.pixelsPerMm),
        snapStepMm_(source.snapStepMm),
        snapEnabled_(source.snapEnabled),
        gridVisible_(source.gridVisible),
        viewRotationDeg_(source.viewRotationDeg),
        viewYawDeg_(source.viewYawDeg),
        viewPitchDeg_(source.viewPitchDeg),
        circleMode_(source.circleMode),
        circleDiameterMm_(source.circleDiameterMm),
        rectangleMode_(source.rectangleMode),
        realReferenceBodyVisible_(scene_.realReferenceBodyVisible),
        referenceBodyVisible_(scene_.referenceBodyVisible),
        referenceProfileVisible_(scene_.referenceProfileVisible),
        referencePlacement_(scene_.referencePlacement),
        referenceProfile_(scene_.referenceProfile),
        sceneBodyMeshes_(scene_.sceneBodyMeshes),
        sceneSketches_(scene_.sceneSketches),
        sceneImages_(scene_.sceneImages),
        referenceBodyMesh_(scene_.referenceBodyMesh
                               ? *scene_.referenceBodyMesh
                               : emptyMesh_),
        referenceFaceMesh_(scene_.referenceFaceMesh
                               ? *scene_.referenceFaceMesh
                               : emptyMesh_),
        referenceBox_{scene_.referenceWidthMm, scene_.referenceDepthMm,
                      scene_.referenceHeightMm} {}

  void record(SketchRenderPass pass, QPainter& painter) const {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (pass != SketchRenderPass::BackgroundGridDatum) {
      painter.setClipRect(QRectF(kRulerLeft, kRulerTop,
                                 width() - kRulerLeft,
                                 height() - kRulerTop));
    }
    switch (pass) {
      case SketchRenderPass::BackgroundGridDatum:
        recordBackgroundGridDatum(painter);
        break;
      case SketchRenderPass::ReferenceGeometry:
        recordReferenceGeometry(painter);
        break;
      case SketchRenderPass::CommittedGeometry:
        recordCommittedGeometry(painter);
        break;
      case SketchRenderPass::TransientUnderlay:
        recordTransientUnderlay(painter);
        break;
      case SketchRenderPass::ConstraintsDimensions:
        recordConstraintsDimensions(painter);
        break;
      case SketchRenderPass::TransientTools:
        recordTransientTools(painter);
        break;
      case SketchRenderPass::SelectionHover:
        recordSelectionHover(painter);
        break;
      case SketchRenderPass::HudOverlays:
        recordHudOverlays(painter);
        break;
    }
    painter.restore();
  }

 private:
  using Tool = SketchInteractionTool;
  using CircleMode = SketchRenderCircleMode;
  using RectangleMode = SketchRenderRectangleMode;
  using SelectionKind = SketchRenderSelectionKind;
  using ConstructionSnapKind = SketchRenderSnapKind;
  using TrimGeometryKind = SketchTrimGeometryKind;

  struct ReferenceBox {
    double widthMm{};
    double depthMm{};
    double heightMm{};
  };

  [[nodiscard]] int width() const noexcept {
    return snapshot.viewportSize.width();
  }
  [[nodiscard]] int height() const noexcept {
    return snapshot.viewportSize.height();
  }
  [[nodiscard]] QRect rect() const {
    return QRect(QPoint{}, snapshot.viewportSize);
  }
  [[nodiscard]] const SketchRenderPalette& palette() const noexcept {
    return snapshot.palette;
  }
  [[nodiscard]] Tool tool() const noexcept {
    return snapshot.interaction.tool;
  }
  [[nodiscard]] const SketchRenderTransientState& interactionState()
      const noexcept {
    return snapshot.interaction;
  }
  [[nodiscard]] bool viewAlignedToSketchPlane() const noexcept {
    return std::abs(viewYawDeg_) <= 1e-9 &&
           std::abs(viewPitchDeg_) <= 1e-9;
  }
  [[nodiscard]] std::array<double, 3> projectLocalPoint(
      double xMm, double yMm, double zMm) const noexcept {
    const double yaw = viewYawDeg_ * std::numbers::pi / 180.0;
    const double pitch = viewPitchDeg_ * std::numbers::pi / 180.0;
    const double yawX = std::cos(yaw) * xMm + std::sin(yaw) * zMm;
    const double yawZ = -std::sin(yaw) * xMm + std::cos(yaw) * zMm;
    const double pitchY = std::cos(pitch) * yMm - std::sin(pitch) * yawZ;
    const double depth = std::sin(pitch) * yMm + std::cos(pitch) * yawZ;
    const double roll = viewRotationDeg_ * std::numbers::pi / 180.0;
    return {std::cos(roll) * yawX + std::sin(roll) * pitchY,
            -std::sin(roll) * yawX + std::cos(roll) * pitchY, depth};
  }
  [[nodiscard]] QPointF mapPoint(sketch::Point point) const {
    const auto projected = projectLocalPoint(point.xMm, point.yMm, 0.0);
    const double centerX = kRulerLeft + (width() - kRulerLeft) * 0.5 +
                           interactionState().camera.panX;
    const double centerY = kRulerTop + (height() - kRulerTop) * 0.5 +
                           interactionState().camera.panY;
    return {centerX + projected[0] * pixelsPerMm_,
            centerY - projected[1] * pixelsPerMm_};
  }
  [[nodiscard]] std::array<double, 3> worldProjection(Point3d point) const {
    const auto local = referencePlacement_.toLocal(point);
    const Vector3d normal = referencePlacement_.normal();
    const Vector3d delta{point.x - referencePlacement_.origin.x,
                         point.y - referencePlacement_.origin.y,
                         point.z - referencePlacement_.origin.z};
    const double localZ = delta.x * normal.x + delta.y * normal.y +
                          delta.z * normal.z;
    return projectLocalPoint(local.x, local.y, localZ);
  }
  [[nodiscard]] QPointF mapWorldPoint(Point3d point) const {
    const auto projected = worldProjection(point);
    const double centerX = kRulerLeft + (width() - kRulerLeft) * 0.5 +
                           interactionState().camera.panX;
    const double centerY = kRulerTop + (height() - kRulerTop) * 0.5 +
                           interactionState().camera.panY;
    return {centerX + projected[0] * pixelsPerMm_,
            centerY - projected[1] * pixelsPerMm_};
  }
  [[nodiscard]] double worldPointDepth(Point3d point) const {
    return worldProjection(point)[2];
  }
  [[nodiscard]] QPolygonF circlePolyline(
      sketch::Point center, double radiusMm, double startAngleRad = 0.0,
      double sweepAngleRad = kTrimTwoPi, int segmentCount = 72) const {
    QPolygonF result;
    segmentCount = std::max(8, segmentCount);
    result.reserve(segmentCount + 1);
    for (int segment = 0; segment <= segmentCount; ++segment) {
      const double t = static_cast<double>(segment) / segmentCount;
      const double angle = startAngleRad + sweepAngleRad * t;
      result << mapPoint({center.xMm + radiusMm * std::cos(angle),
                          center.yMm + radiusMm * std::sin(angle)});
    }
    return result;
  }
  [[nodiscard]] QPolygonF bezierPolyline(
      const sketch::Bezier& bezier, int segmentCount = 48) const {
    QPolygonF result;
    segmentCount = std::max(8, segmentCount);
    result.reserve(segmentCount + 1);
    for (int segment = 0; segment <= segmentCount; ++segment)
      result << mapPoint(sketch::bezierPointAt(
          bezier, static_cast<double>(segment) / segmentCount));
    return result;
  }
  [[nodiscard]] bool lineSelected(sketch::GeometryId id) const {
    return selectedLineIds_.contains(id);
  }
  [[nodiscard]] bool lineElementSelected(std::size_t id) const {
    return selectedElementIds_.contains(id);
  }
  [[nodiscard]] bool circleSelected(sketch::GeometryId id) const {
    return selectedCircleIds_.contains(id);
  }
  [[nodiscard]] bool arcSelected(sketch::GeometryId id) const {
    return selectedArcIds_.contains(id);
  }
  [[nodiscard]] bool bezierSelected(sketch::GeometryId id) const {
    return selectedBezierIds_.contains(id);
  }
  [[nodiscard]] std::optional<std::size_t> resolveDimensionIndex(
      const SketchDimensionReference& reference) const {
    return sketch_.dimensionIndex(reference.id);
  }
  [[nodiscard]] QPointF dimensionLabelCenter(
      std::size_t index, QPointF first, QPointF second) const {
    QPointF direction = second - first;
    const double length = std::hypot(direction.x(), direction.y());
    if (length < 1.0) return (first + second) * 0.5;
    direction /= length;
    const QPointF normal(-direction.y(), direction.x());
    const auto& alongValues = interactionState().dimension.labelAlongMm;
    const auto& offsetValues = interactionState().dimension.labelOffsetMm;
    const double along = index < alongValues.size() ? alongValues[index] : 0.0;
    const double offset =
        index < offsetValues.size() ? offsetValues[index] : 2.0;
    return (first + second) * 0.5 + direction * along * pixelsPerMm_ +
           normal * offset * pixelsPerMm_;
  }
  [[nodiscard]] std::optional<std::vector<sketch::Line>>
  resolvedCircleGuideLines() const {
    std::vector<sketch::Line> result;
    result.reserve(interactionState().creation.circleGuideIds.size());
    for (const auto id : interactionState().creation.circleGuideIds) {
      const auto index = sketch_.lineIndex(id);
      if (!index) return std::nullopt;
      result.push_back(sketch_.lines()[*index]);
    }
    return result;
  }
  void recordBackgroundGridDatum(QPainter& painter) const {
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(rect(), palette().background);

  const QPointF origin = mapPoint({0, 0});
  if (gridVisible_) {
    painter.setPen(QPen(palette().gridMinor, 1.0));
    if (viewAlignedToSketchPlane()) {
      const double minorGrid = snapStepMm_ * pixelsPerMm_;
      double firstX = kRulerLeft +
                      std::fmod(origin.x() - kRulerLeft, minorGrid);
      if (firstX < kRulerLeft) firstX += minorGrid;
      double firstY = kRulerTop +
                      std::fmod(origin.y() - kRulerTop, minorGrid);
      if (firstY < kRulerTop) firstY += minorGrid;
      for (double x = firstX; x < width(); x += minorGrid)
        painter.drawLine(QPointF(x, kRulerTop), QPointF(x, height()));
      for (double y = firstY; y < height(); y += minorGrid)
        painter.drawLine(QPointF(kRulerLeft, y), QPointF(width(), y));
    } else {
      // In the free camera view the grid belongs to the actual sketch plane,
      // so it must tilt together with the sketch rather than stay screen-flat.
      const double extent = 2.0 * std::max(width(), height()) /
                            std::max(0.05, pixelsPerMm_);
      const double step = niceRulerStep(pixelsPerMm_);
      const int lineCount = std::min(80, static_cast<int>(
          std::ceil(extent / std::max(0.01, step))));
      for (int index = -lineCount; index <= lineCount; ++index) {
        const double coordinate = index * step;
        painter.drawLine(mapPoint({coordinate, -extent}),
                         mapPoint({coordinate, extent}));
        painter.drawLine(mapPoint({-extent, coordinate}),
                         mapPoint({extent, coordinate}));
      }
    }
  }

  painter.fillRect(QRectF(0, 0, width(), kRulerTop), palette().rulerBackground);
  painter.fillRect(QRectF(0, 0, kRulerLeft, height()), palette().rulerBackground);
  painter.setPen(QPen(palette().rulerBorder, 1.0));
  painter.drawLine(QPointF(kRulerLeft, kRulerTop), QPointF(width(), kRulerTop));
  painter.drawLine(QPointF(kRulerLeft, kRulerTop), QPointF(kRulerLeft, height()));
  painter.setPen(palette().rulerText);
  const double rulerStepMm = niceRulerStep(pixelsPerMm_);
  if (viewAlignedToSketchPlane()) {
    // Rulers describe the CURRENT VIEW axes. At 90 degrees screen X
    // represents sketch Y, so labels must use view coordinates directly.
    const double visibleLeftMm =
        (kRulerLeft - origin.x()) / pixelsPerMm_;
    const double visibleRightMm =
        (static_cast<double>(width()) - origin.x()) / pixelsPerMm_;
    const double firstHorizontalMm =
        std::ceil(visibleLeftMm / rulerStepMm) * rulerStepMm;
    for (double mm = firstHorizontalMm; mm <= visibleRightMm;
         mm += rulerStepMm) {
      const double x = origin.x() + mm * pixelsPerMm_;
      painter.drawLine(QPointF(x, 20), QPointF(x, kRulerTop));
      painter.drawText(QRectF(x - 24, 2, 48, 17), Qt::AlignCenter,
                       QString::number(mm, 'f', 0));
    }

    const double visibleTopMm =
        (origin.y() - kRulerTop) / pixelsPerMm_;
    const double visibleBottomMm =
        (origin.y() - static_cast<double>(height())) / pixelsPerMm_;
    const double firstVerticalMm =
        std::ceil(visibleBottomMm / rulerStepMm) * rulerStepMm;
    for (double mm = firstVerticalMm; mm <= visibleTopMm;
         mm += rulerStepMm) {
      const double y = origin.y() - mm * pixelsPerMm_;
      painter.drawLine(QPointF(34, y), QPointF(kRulerLeft, y));
      painter.save();
      painter.translate(3, y + 22);
      painter.rotate(-90);
      painter.drawText(QRectF(0, 0, 44, 17), Qt::AlignCenter,
                       QString::number(mm, 'f', 0));
      painter.restore();
    }
  }

  painter.setClipRect(QRectF(kRulerLeft, kRulerTop,
                             width() - kRulerLeft,
                             height() - kRulerTop));

  const double axisExtentMm =
      2.0 * std::max(width(), height()) / std::max(0.05, pixelsPerMm_);
  painter.setPen(QPen(palette().axisX, 1.1));
  painter.drawLine(mapPoint({-axisExtentMm, 0.0}),
                   mapPoint({axisExtentMm, 0.0}));
  painter.setPen(QPen(palette().axisY, 1.1));
  painter.drawLine(mapPoint({0.0, -axisExtentMm}),
                   mapPoint({0.0, axisExtentMm}));
  }

  void recordReferenceGeometry(QPainter& painter) const {
  const auto projectScenePoint = [&](Point3d point) {
    return mapWorldPoint(point);
  };
  QColor imageBorder = palette().sceneEdge;
  imageBorder.setAlpha(72);
  for (const auto& image : sceneImages_) {
    if (image.pixelWidth <= 0 || image.pixelHeight <= 0 ||
        !std::isfinite(image.scale) || image.scale <= 0.0)
      continue;
    const double widthMm = image.pixelWidth * 0.1 * image.scale;
    const double heightMm = image.pixelHeight * 0.1 * image.scale;
    const std::array<sketch::Point, 4> local{{
        {image.offsetXMm - widthMm * 0.5,
         image.offsetYMm + heightMm * 0.5},
        {image.offsetXMm + widthMm * 0.5,
         image.offsetYMm + heightMm * 0.5},
        {image.offsetXMm + widthMm * 0.5,
         image.offsetYMm - heightMm * 0.5},
        {image.offsetXMm - widthMm * 0.5,
         image.offsetYMm - heightMm * 0.5}}};
    QPolygonF destination;
    const Vector3d normal = image.placement.normal();
    for (const auto& point : local) {
      Point3d world = image.placement.toWorld(point.xMm, point.yMm);
      world.x += normal.x * image.offsetZMm;
      world.y += normal.y * image.offsetZMm;
      world.z += normal.z * image.offsetZMm;
      destination << projectScenePoint(world);
    }
    if (image.pixels.isNull()) {
      QColor placeholder = palette().sceneFill;
      placeholder.setAlpha(28);
      painter.setBrush(placeholder);
      painter.setPen(QPen(imageBorder, 1.0, Qt::DashLine));
      painter.drawPolygon(destination);
      painter.drawText(destination.boundingRect(), Qt::AlignCenter,
                       image.name);
      continue;
    }
    QPolygonF source;
    source << QPointF(0.0, 0.0)
           << QPointF(image.pixels.width(), 0.0)
           << QPointF(image.pixels.width(), image.pixels.height())
           << QPointF(0.0, image.pixels.height());
    QTransform transform;
    if (!QTransform::quadToQuad(source, destination, transform)) continue;
    painter.save();
    painter.setOpacity(0.32);
    painter.setWorldTransform(transform, true);
    painter.drawImage(QPointF(0.0, 0.0), image.pixels);
    painter.restore();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(imageBorder, 1.0));
    painter.drawPolygon(destination);
  }

  std::size_t sceneBodySlot = 0;
  QColor sceneFill = palette().sceneFill;
  sceneFill.setAlpha(38);
  QColor sceneEdge = palette().sceneEdge;
  sceneEdge.setAlpha(82);
  QColor sceneHover = palette().sceneHover;
  sceneHover.setAlpha(220);
  for (const auto& mesh : sceneBodyMeshes_) {
    if (!mesh) {
      ++sceneBodySlot;
      continue;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(sceneFill);
    for (const auto& triangle : mesh->triangles()) {
      painter.drawPolygon(QPolygonF{projectScenePoint(triangle.a),
                                    projectScenePoint(triangle.b),
                                    projectScenePoint(triangle.c)});
    }
    painter.setBrush(Qt::NoBrush);
    for (std::size_t edgeIndex = 0; edgeIndex < mesh->edges().size();
         ++edgeIndex) {
      const bool hovered =
          tool() == Tool::Projection && hoveredProjectionEdge_ &&
          hoveredProjectionEdge_->source == SketchProjectionSource::SceneBody &&
          hoveredProjectionEdge_->sceneBodySlot == sceneBodySlot &&
          hoveredProjectionEdge_->meshRevision == mesh->revision() &&
          hoveredProjectionEdge_->edgeSlot == edgeIndex;
      painter.setPen(hovered ? QPen(sceneHover, 3.2, Qt::SolidLine,
                                    Qt::RoundCap)
                             : QPen(sceneEdge, 1.0));
      QPolygonF curve;
      for (const auto& point : mesh->edges()[edgeIndex].points)
        curve << projectScenePoint(point);
      painter.drawPolyline(curve);
    }
    ++sceneBodySlot;
  }

  QColor sceneSketch = palette().sceneSketch;
  sceneSketch.setAlpha(96);
  painter.setBrush(Qt::NoBrush);
  for (const auto& reference : sceneSketches_) {
    const auto projectSketchPoint = [&](sketch::Point point) {
      return projectScenePoint(
          reference.placement.toWorld(point.xMm, point.yMm));
    };
    for (const auto& line : reference.geometry.lines()) {
      painter.setPen(QPen(sceneSketch, 1.2,
                          line.dashed ? Qt::DashLine : Qt::SolidLine));
      painter.drawLine(projectSketchPoint(line.start),
                       projectSketchPoint(line.end));
    }
    for (const auto& circle : reference.geometry.circles()) {
      painter.setPen(QPen(sceneSketch, 1.2,
                          circle.dashed ? Qt::DashLine : Qt::SolidLine));
      QPolygonF curve;
      for (int step = 0; step <= 72; ++step) {
        const double angle = 2.0 * std::numbers::pi * step / 72.0;
        curve << projectSketchPoint(
            {circle.center.xMm + circle.radiusMm * std::cos(angle),
             circle.center.yMm + circle.radiusMm * std::sin(angle)});
      }
      painter.drawPolyline(curve);
    }
    for (const auto& arc : reference.geometry.arcs()) {
      painter.setPen(QPen(sceneSketch, 1.2,
                          arc.dashed ? Qt::DashLine : Qt::SolidLine));
      QPolygonF curve;
      for (int step = 0; step <= 48; ++step) {
        const double angle =
            arc.startAngleRad + arc.sweepAngleRad * step / 48.0;
        curve << projectSketchPoint(
            {arc.center.xMm + arc.radiusMm * std::cos(angle),
             arc.center.yMm + arc.radiusMm * std::sin(angle)});
      }
      painter.drawPolyline(curve);
    }
    for (const auto& bezier : reference.geometry.beziers()) {
      painter.setPen(QPen(sceneSketch, 1.2,
                          bezier.dashed ? Qt::DashLine : Qt::SolidLine));
      QPolygonF curve;
      for (int step = 0; step <= 48; ++step)
        curve << projectSketchPoint(sketch::bezierPointAt(
            bezier, static_cast<double>(step) / 48.0));
      painter.drawPolyline(curve);
    }
  }

  if (realReferenceBodyVisible_) {
    struct ProjectedTriangle {
      QPolygonF polygon;
      double depth{};
      double facing{};
    };
    const Vector3d planeNormal = referencePlacement_.normal();
    const double yaw = viewYawDeg_ * std::numbers::pi / 180.0;
    const double pitch = viewPitchDeg_ * std::numbers::pi / 180.0;
    const double depthX = -std::cos(pitch) * std::sin(yaw);
    const double depthY = std::sin(pitch);
    const double depthZ = std::cos(pitch) * std::cos(yaw);
    const Vector3d viewNormal{
        referencePlacement_.xDirection.x * depthX +
            referencePlacement_.yDirection.x * depthY +
            planeNormal.x * depthZ,
        referencePlacement_.xDirection.y * depthX +
            referencePlacement_.yDirection.y * depthY +
            planeNormal.y * depthZ,
        referencePlacement_.xDirection.z * depthX +
            referencePlacement_.yDirection.z * depthY +
            planeNormal.z * depthZ};
    const auto depthOf = [&](Point3d point) {
      return worldPointDepth(point);
    };
    const auto projected = [&](Point3d point) {
      return mapWorldPoint(point);
    };
    std::vector<ProjectedTriangle> triangles;
    triangles.reserve(referenceBodyMesh_.triangles().size());
    for (const auto& triangle : referenceBodyMesh_.triangles()) {
      triangles.push_back(
          {{projected(triangle.a), projected(triangle.b), projected(triangle.c)},
           (depthOf(triangle.a) + depthOf(triangle.b) + depthOf(triangle.c)) /
               3.0,
           std::abs(triangle.normal.x * viewNormal.x +
                    triangle.normal.y * viewNormal.y +
                    triangle.normal.z * viewNormal.z)});
    }
    std::sort(triangles.begin(), triangles.end(),
              [](const auto& first, const auto& second) {
                return first.depth < second.depth;
              });
    painter.setPen(Qt::NoPen);
    for (const auto& triangle : triangles) {
      QColor triangleFill = palette().sceneFill;
      triangleFill.setAlpha(static_cast<int>(56 + triangle.facing * 24.0));
      painter.setBrush(triangleFill);
      painter.drawPolygon(triangle.polygon);
    }
    painter.setBrush(Qt::NoBrush);
    for (std::size_t edgeIndex = 0;
         edgeIndex < referenceBodyMesh_.edges().size(); ++edgeIndex) {
      const auto& edge = referenceBodyMesh_.edges()[edgeIndex];
      const bool hovered =
          tool() == Tool::Projection &&
          hoveredProjectionEdge_ &&
          hoveredProjectionEdge_->source ==
              SketchProjectionSource::ReferenceBody &&
          hoveredProjectionEdge_->meshRevision ==
              referenceBodyMesh_.revision() &&
          hoveredProjectionEdge_->edgeSlot == edgeIndex;
      painter.setPen(
          hovered
              ? QPen(palette().sceneHover, 3.2, Qt::SolidLine, Qt::RoundCap)
              : QPen(palette().sceneEdge, 1.0));
      QPolygonF curve;
      for (const auto& point : edge.points) curve << projected(point);
      painter.drawPolyline(curve);
    }
    painter.setBrush(palette().referenceFill);
    painter.setPen(Qt::NoPen);
    for (const auto& triangle : referenceFaceMesh_.triangles()) {
      QPolygonF polygon{projected(triangle.a), projected(triangle.b),
                        projected(triangle.c)};
      painter.drawPolygon(polygon);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(palette().sceneEdge, 2.0));
    for (const auto& edge : referenceFaceMesh_.edges()) {
      QPolygonF boundary;
      for (const auto& point : edge.points) boundary << projected(point);
      painter.drawPolyline(boundary);
    }
  }
  if (!realReferenceBodyVisible_ && sceneBodyMeshes_.empty() &&
      referenceBodyVisible_ &&
      !referenceProfileVisible_) {
    double bodyWidth = referenceBox_.widthMm;
    double bodyHeight = referenceBox_.depthMm;
    const Vector3d normal = referencePlacement_.normal();
    if (std::abs(normal.y) > 0.9) {
      bodyHeight = referenceBox_.heightMm;
    } else if (std::abs(normal.x) > 0.9) {
      bodyWidth = referenceBox_.depthMm;
      bodyHeight = referenceBox_.heightMm;
    }
    const QRectF bodyRect(mapPoint({-bodyWidth * 0.5, bodyHeight * 0.5}),
                          mapPoint({bodyWidth * 0.5, -bodyHeight * 0.5}));
    painter.setBrush(palette().referenceFill);
    painter.setPen(QPen(palette().referenceEdge, 1.6));
    painter.drawRect(bodyRect.normalized());
  }
  if (!realReferenceBodyVisible_ && sceneBodyMeshes_.empty() &&
      referenceBodyVisible_ &&
      referenceProfileVisible_) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(palette().referenceEdge, 1.6));
    for (const auto& line : referenceProfile_.lines())
      painter.drawLine(mapPoint(line.start), mapPoint(line.end));
    for (const auto& circle : referenceProfile_.circles()) {
      painter.setPen(QPen(palette().referenceEdge, 1.6,
                          circle.dashed ? Qt::DashLine : Qt::SolidLine));
      painter.drawPolyline(circlePolyline(circle.center, circle.radiusMm));
    }
    for (const auto& bezier : referenceProfile_.beziers()) {
      painter.setPen(QPen(palette().referenceEdge, 1.6,
                          bezier.dashed ? Qt::DashLine : Qt::SolidLine));
      painter.drawPolyline(bezierPolyline(bezier));
    }
  }
  }

  void recordCommittedGeometry(QPainter& painter) const {
  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const auto currentLineId = sketch_.lineId(index);
    const bool selected =
        lineSelected(currentLineId) || lineElementSelected(line.elementId) ||
        (selectedLineIds_.empty() && selectedElementIds_.empty() &&
         selectionKind_ == SelectionKind::Line &&
         selectionLineId_ == currentLineId);
    const bool locked =
        sketch_.isGeometryLocked(currentLineId);
    const bool snapHovered =
        constructionHover_ &&
        ((constructionHover_->kind == ConstructionSnapKind::LineMidpoint ||
          constructionHover_->kind == ConstructionSnapKind::LineBody) &&
             constructionHover_->geometryId == currentLineId ||
         constructionHover_->kind == ConstructionSnapKind::ElementCenter &&
             constructionHover_->elementId != 0 &&
             constructionHover_->elementId == line.elementId);
    const QColor baseColor =
        locked ? palette().committedLocked : palette().committed;
    const QColor selectedColor =
        locked ? palette().selectedLocked : palette().selected;
    const QColor snapColor = palette().sceneHover;

    painter.setPen(QPen(snapHovered
                            ? snapColor
                            : selected ? selectedColor : baseColor,
                        snapHovered ? 3.8 : selected ? 3.0 : 2.0,
                        line.dashed ? Qt::DashLine
                                    : Qt::SolidLine,
                        Qt::RoundCap));
    painter.drawLine(mapPoint(line.start), mapPoint(line.end));
    painter.setBrush(
        locked ? palette().selectedLocked : palette().endpointFill);
    painter.drawEllipse(mapPoint(line.start), 3.5, 3.5);
    painter.drawEllipse(mapPoint(line.end), 3.5, 3.5);
  }
  for (std::size_t index = 0; index < sketch_.circles().size(); ++index) {
    const auto& circle = sketch_.circles()[index];
    const auto circleId = sketch_.circleId(index);
    const bool selected =
        circleSelected(circleId) ||
        (selectedCircleIds_.empty() &&
         selectionKind_ == SelectionKind::Circle &&
         selectionCircleId_ == circleId);
    const bool locked =
        sketch_.isGeometryLocked(circleId);
    const bool snapHovered =
        constructionHover_ &&
        (constructionHover_->kind == ConstructionSnapKind::CircleCenter ||
         constructionHover_->kind == ConstructionSnapKind::CircleBody) &&
        constructionHover_->geometryId == circleId;
    const QColor baseColor =
        locked ? palette().committedLocked : palette().committed;
    const QColor selectedColor =
        locked ? palette().selectedLocked : palette().selected;
    const QColor snapColor = palette().sceneHover;

    painter.setPen(QPen(snapHovered
                            ? snapColor
                            : selected ? selectedColor : baseColor,
                        snapHovered ? 3.8 : selected ? 3.0 : 2.0,
                        circle.dashed ? Qt::DashLine
                                      : Qt::SolidLine));
    painter.setBrush(Qt::NoBrush);
    const QPointF center = mapPoint(circle.center);
    painter.drawPolyline(circlePolyline(circle.center, circle.radiusMm));
    painter.setBrush(palette().endpointFill);
    painter.drawEllipse(center, 3.5, 3.5);
  }

  const auto drawSketchArc = [&](const sketch::Arc& arc) {
    const int segmentCount = std::max(
        8, static_cast<int>(std::ceil(
               72.0 * arc.sweepAngleRad / (2.0 * std::numbers::pi))));
    painter.drawPolyline(circlePolyline(
        arc.center, arc.radiusMm, arc.startAngleRad, arc.sweepAngleRad,
        segmentCount));
  };

  for (std::size_t index = 0; index < sketch_.arcs().size(); ++index) {
    const auto& arc = sketch_.arcs()[index];
    const auto arcId = sketch_.arcId(index);
    const bool locked = sketch_.isGeometryLocked(arcId);
    const bool selected =
        arcSelected(arcId) ||
        (selectedArcIds_.empty() &&
         selectionKind_ == SelectionKind::Arc &&
         selectionArcId_ == arcId);
    const bool snapHovered =
        constructionHover_ &&
        constructionHover_->kind == ConstructionSnapKind::CircleBody &&
        constructionHover_->geometryId == arcId;

    painter.setPen(
        QPen(snapHovered
                 ? palette().sceneHover
                 : selected ? (locked ? palette().selectedLocked
                                      : palette().selected)
                            : locked ? palette().committedLocked
                                     : palette().committed,
             snapHovered ? 3.8 : selected ? 3.0 : 2.0,
             arc.dashed ? Qt::DashLine : Qt::SolidLine,
             Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    drawSketchArc(arc);

    painter.setBrush(locked ? palette().selectedLocked
                            : palette().endpointFill);
    painter.drawEllipse(mapPoint(sketch::arcStartPoint(arc)), 3.5, 3.5);
    painter.drawEllipse(mapPoint(sketch::arcEndPoint(arc)), 3.5, 3.5);
  }
  for (std::size_t index = 0; index < sketch_.beziers().size(); ++index) {
    const auto& bezier = sketch_.beziers()[index];
    const auto bezierId = sketch_.bezierId(index);
    const bool locked = sketch_.isGeometryLocked(bezierId);
    const bool selected =
        bezierSelected(bezierId) ||
        (selectedBezierIds_.empty() &&
         selectionKind_ == SelectionKind::Bezier &&
         selectionBezierId_ == bezierId);
    painter.setPen(QPen(selected ? (locked ? palette().selectedLocked
                                          : palette().selected)
                                 : locked ? palette().committedLocked
                                          : palette().committed,
                        selected ? 3.0 : 2.0,
                        bezier.dashed ? Qt::DashLine : Qt::SolidLine,
                        Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(bezierPolyline(bezier));
    if (selected) {
      painter.setPen(QPen(withAlpha(palette().selected, 140), 1.0,
                          Qt::DashLine));
      painter.drawLine(mapPoint(bezier.points[0]), mapPoint(bezier.points[1]));
      painter.drawLine(mapPoint(bezier.points[2]), mapPoint(bezier.points[3]));
    }
    for (std::size_t point = 0; point < bezier.points.size(); ++point) {
      painter.setBrush(point == 1 || point == 2
                           ? withAlpha(palette().selected, 170)
                           : locked ? palette().selectedLocked
                                    : palette().endpointFill);
      painter.drawEllipse(mapPoint(bezier.points[point]),
                          point == 1 || point == 2 ? 3.0 : 3.5,
                          point == 1 || point == 2 ? 3.0 : 3.5);
    }
  }
  }

  void recordTransientUnderlay(QPainter& painter) const {
  const auto drawSketchArc = [&, this](const sketch::Arc& arc) {
    const int segmentCount = std::max(
        8, static_cast<int>(std::ceil(
               72.0 * arc.sweepAngleRad / (2.0 * std::numbers::pi))));
    painter.drawPolyline(circlePolyline(
        arc.center, arc.radiusMm, arc.startAngleRad, arc.sweepAngleRad,
        segmentCount));
  };
  // Scissors hover previews the exact interval that the next click removes.
  // Paint it after the normal geometry so the destructive target is
  // unambiguous for lines, arcs and circles alike.
  if (tool() == Tool::Trim && interactionState().trim.preview) {
    painter.save();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(palette().trim, 5.0, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    const auto& preview = *interactionState().trim.preview;
    if (preview.kind == TrimGeometryKind::Line) {
      const auto index = sketch_.lineIndex(preview.geometryId);
      if (index) {
        const auto& line = sketch_.lines()[*index];
        const auto pointAt = [&line](double parameter) {
          return sketch::Point{
              line.start.xMm + (line.end.xMm - line.start.xMm) * parameter,
              line.start.yMm + (line.end.yMm - line.start.yMm) * parameter};
        };
        painter.drawLine(mapPoint(pointAt(preview.firstParameter)),
                         mapPoint(pointAt(preview.secondParameter)));
      }
    } else if (preview.kind == TrimGeometryKind::Circle) {
      const auto index = sketch_.circleIndex(preview.geometryId);
      if (index) {
        const auto& circle = sketch_.circles()[*index];
        if (preview.fullGeometry) {
          painter.drawPolyline(circlePolyline(circle.center, circle.radiusMm));
        } else {
          sketch::Arc interval;
          interval.center = circle.center;
          interval.radiusMm = circle.radiusMm;
          interval.startAngleRad = preview.firstParameter * kTrimTwoPi;
          interval.sweepAngleRad =
              (preview.secondParameter - preview.firstParameter) * kTrimTwoPi;
          drawSketchArc(interval);
        }
      }
    } else if (preview.kind == TrimGeometryKind::Arc) {
      const auto index = sketch_.arcIndex(preview.geometryId);
      if (index) {
        const auto& source = sketch_.arcs()[*index];
        sketch::Arc interval = source;
        interval.startAngleRad =
            source.startAngleRad +
            source.sweepAngleRad * preview.firstParameter;
        interval.sweepAngleRad =
            source.sweepAngleRad *
            (preview.secondParameter - preview.firstParameter);
        drawSketchArc(interval);
      }
    } else {
      const auto index = sketch_.bezierIndex(preview.geometryId);
      if (index) {
        const auto& source = sketch_.beziers()[*index];
        QPolygonF interval;
        constexpr int samples = 48;
        for (int step = 0; step <= samples; ++step) {
          const double parameter =
              preview.firstParameter +
              (preview.secondParameter - preview.firstParameter) *
                  static_cast<double>(step) / samples;
          interval << mapPoint(sketch::bezierPointAt(source, parameter));
        }
        painter.drawPolyline(interval);
      }
    }
    painter.restore();
  }

  if (tool() == Tool::Arc && !interactionState().creation.arcPoints.empty()) {
    painter.save();
    painter.setPen(QPen(withAlpha(palette().transient, 190), 1.8, Qt::DashLine,
                        Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);

    if (interactionState().creation.arcPoints.size() == 1) {
      const auto first = interactionState().creation.arcPoints.front();
      const auto last = hoverPoint_;
      const double chord =
          std::hypot(last.xMm - first.xMm, last.yMm - first.yMm);

      if (chord > 1e-9) {
        const auto preview =
            arcFromChordSagitta(first, last, chord * 0.5);
        if (preview) drawSketchArc(*preview);

        painter.setPen(
            QPen(withAlpha(palette().transient, 120), 1.1, Qt::DashLine));
        painter.drawLine(mapPoint(first), mapPoint(last));
      }
    } else if (interactionState().creation.arcPoints.size() == 2) {
      const auto first = interactionState().creation.arcPoints[0];
      const auto last = interactionState().creation.arcPoints[1];
      const double sagitta = signedArcSagitta(first, last, hoverPoint_);
      const auto preview = arcFromChordSagitta(first, last, sagitta);
      if (preview) drawSketchArc(*preview);

      const auto middle = sketch::Point{
          (first.xMm + last.xMm) * 0.5,
          (first.yMm + last.yMm) * 0.5};

      painter.setPen(
          QPen(withAlpha(palette().transient, 120), 1.1, Qt::DashLine));
      painter.drawLine(mapPoint(first), mapPoint(last));
      painter.drawLine(mapPoint(middle), mapPoint(hoverPoint_));
    }

    painter.setPen(QPen(withAlpha(palette().transient, 210), 1.4));
    painter.setBrush(withAlpha(palette().transient, 80));
    for (const auto& point : interactionState().creation.arcPoints)
      painter.drawEllipse(mapPoint(point), 4.0, 4.0);
    painter.restore();
  }

  if (tool() == Tool::Bezier &&
      !interactionState().creation.bezierPoints.empty()) {
    painter.save();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(withAlpha(palette().transient, 190), 1.8,
                        Qt::DashLine, Qt::RoundCap));
    const auto& points = interactionState().creation.bezierPoints;
    if (points.size() == 1) {
      painter.drawLine(mapPoint(points[0]), mapPoint(hoverPoint_));
    } else {
      sketch::Bezier preview;
      preview.points[0] = points[0];
      preview.points[1] = points[1];
      preview.points[2] = points.size() >= 3 ? points[2] : hoverPoint_;
      preview.points[3] = hoverPoint_;
      painter.drawPolyline(bezierPolyline(preview));
      painter.setPen(QPen(withAlpha(palette().transient, 110), 1.0,
                          Qt::DashLine));
      painter.drawLine(mapPoint(preview.points[0]),
                       mapPoint(preview.points[1]));
      painter.drawLine(mapPoint(preview.points[2]),
                       mapPoint(preview.points[3]));
    }
    painter.setBrush(palette().transientSurface);
    for (const auto point : points)
      painter.drawEllipse(mapPoint(point), 3.5, 3.5);
    painter.restore();
  }

  if (constructionHover_) {
    const QPointF snapPoint = mapPoint(constructionHover_->point);
    painter.setPen(QPen(palette().sceneHover, 1.8));

    if (constructionHover_->kind == ConstructionSnapKind::LineMidpoint) {
      painter.setBrush(withAlpha(palette().sceneHover, 55));
      painter.drawEllipse(snapPoint, 6.0, 6.0);
      painter.setBrush(palette().sceneHover);
      painter.drawEllipse(snapPoint, 2.0, 2.0);
    } else if (constructionHover_->kind == ConstructionSnapKind::Origin) {
      painter.setBrush(palette().transientSurface);
      painter.drawRect(QRectF(snapPoint - QPointF(5.0, 5.0),
                              QSizeF(10.0, 10.0)));
      painter.drawLine(snapPoint + QPointF(-8.0, 0.0),
                       snapPoint + QPointF(8.0, 0.0));
      painter.drawLine(snapPoint + QPointF(0.0, -8.0),
                       snapPoint + QPointF(0.0, 8.0));
      painter.drawText(snapPoint + QPointF(9.0, -7.0), QStringLiteral("O"));
    } else if (constructionHover_->kind == ConstructionSnapKind::XAxis ||
               constructionHover_->kind == ConstructionSnapKind::YAxis) {
      const bool xAxis =
          constructionHover_->kind == ConstructionSnapKind::XAxis;
      QPointF direction =
          mapPoint(xAxis ? sketch::Point{1.0, 0.0}
                         : sketch::Point{0.0, 1.0}) -
          mapPoint({0.0, 0.0});
      const double length = std::hypot(direction.x(), direction.y());
      if (length > 1e-9) direction /= length;
      painter.setBrush(palette().transientSurface);
      painter.drawEllipse(snapPoint, 5.0, 5.0);
      painter.drawLine(snapPoint - direction * 9.0,
                       snapPoint + direction * 9.0);
      painter.drawText(snapPoint + QPointF(8.0, -7.0),
                       xAxis ? QStringLiteral("X") : QStringLiteral("Y"));
    } else {
      painter.setBrush(palette().transientSurface);
      painter.drawEllipse(snapPoint, 5.0, 5.0);
    }
  }
  }

  void recordConstraintsDimensions(QPainter& painter) const {
  const auto drawArrow = [&painter](QPointF tip, QPointF direction) {
    const double length = std::hypot(direction.x(), direction.y());
    if (length < 1e-6) return;
    direction /= length;
    const QPointF normal(-direction.y(), direction.x());
    QPolygonF arrow;
    arrow << tip << tip - direction * 8.0 + normal * 3.5
          << tip - direction * 8.0 - normal * 3.5;
    painter.drawPolygon(arrow);
  };
  for (const auto& dimension : sketch_.dimensions()) {
    const std::size_t dimensionIndex =
        static_cast<std::size_t>(&dimension - sketch_.dimensions().data());

    if (dimension.kind == sketch::DimensionKind::LineAngle) {
      const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
      const auto secondIndex =
          sketch_.lineIndex(dimension.secondPoint.lineId);
      if (!firstIndex || !secondIndex) continue;

      const auto& firstLine = sketch_.lines()[*firstIndex];
      const auto& secondLine = sketch_.lines()[*secondIndex];
      const auto center = lineIntersectionScreen(
          firstLine, secondLine,
          [&](sketch::Point point) { return mapPoint(point); });
      if (!center) continue;

      const auto rays = angleSectorRays(
          firstLine, secondLine, *center, dimension.offsetMm,
          [&](sketch::Point point) { return mapPoint(point); });
      if (!rays) continue;
      QPointF firstDirection = rays->first;
      QPointF secondDirection = rays->second;

      const double radius = angularDimensionRadiusPx(
          dimension.offsetMm, pixelsPerMm_, std::min(width(), height()));
      const QPointF arcFirst = *center + firstDirection * radius;
      const QPointF arcSecond = *center + secondDirection * radius;

      double startDeg =
          -std::atan2(firstDirection.y(), firstDirection.x()) *
          180.0 / 3.14159265358979323846;
      double endDeg =
          -std::atan2(secondDirection.y(), secondDirection.x()) *
          180.0 / 3.14159265358979323846;
      double spanDeg = endDeg - startDeg;
      while (spanDeg <= -180.0) spanDeg += 360.0;
      while (spanDeg > 180.0) spanDeg -= 360.0;

      const bool selectedDimension =
          interactionState().dimension.selected.has_value() &&
          resolveDimensionIndex(*interactionState().dimension.selected) ==
              dimensionIndex;
      const QColor dimensionColor =
          selectedDimension ? palette().dimensionSelected
                            : palette().dimension;

            // STORED ANGLE CARRIER EXTENSIONS
      const QPointF firstStartScreen = mapPoint(firstLine.start);
      const QPointF firstEndScreen = mapPoint(firstLine.end);
      const QPointF secondStartScreen = mapPoint(secondLine.start);
      const QPointF secondEndScreen = mapPoint(secondLine.end);

      painter.setBrush(Qt::NoBrush);
      painter.setPen(
          QPen(dimensionColor,
               selectedDimension ? 1.5 : 1.0,
               Qt::DashLine));

      if (pointSegmentDistance(*center,
                               firstStartScreen,
                               firstEndScreen) > 0.75) {
        if (const auto endpoint =
                nearestSegmentEndpointTo(*center,
                                         firstStartScreen,
                                         firstEndScreen))
          painter.drawLine(*center, *endpoint);
      }

      if (pointSegmentDistance(*center,
                               secondStartScreen,
                               secondEndScreen) > 0.75) {
        if (const auto endpoint =
                nearestSegmentEndpointTo(*center,
                                         secondStartScreen,
                                         secondEndScreen))
          painter.drawLine(*center, *endpoint);
      }

      painter.setPen(
          QPen(dimensionColor,
               selectedDimension ? 2.2 : 1.2));
      painter.drawLine(*center, arcFirst);
      painter.drawLine(*center, arcSecond);

      QRectF arcRect(center->x() - radius, center->y() - radius,
                     radius * 2.0, radius * 2.0);
      painter.drawArc(arcRect,
                      qRound(startDeg * 16.0),
                      qRound(spanDeg * 16.0));

      const double midRad =
          (startDeg + spanDeg * 0.5) *
          3.14159265358979323846 / 180.0;
      QPointF textCenter =
          *center + QPointF(std::cos(midRad), -std::sin(midRad)) *
                        (radius + 18.0);

      // Angular dimension labels use the same auxiliary arrays as linear
      // dimensions, but store free screen-X / screen-Y offsets in millimetres.
      const auto& angleLabelX = interactionState().dimension.labelAlongMm;
      const auto& angleLabelY = interactionState().dimension.labelOffsetMm;

      const double labelOffsetX =
          dimensionIndex < static_cast<std::size_t>(angleLabelX.size())
              ? angleLabelX[dimensionIndex]
              : 0.0;
      const double labelOffsetY =
          dimensionIndex < static_cast<std::size_t>(angleLabelY.size())
              ? angleLabelY[dimensionIndex]
              : 0.0;

      textCenter += QPointF(labelOffsetX * pixelsPerMm_,
                            labelOffsetY * pixelsPerMm_);

      const QString label =
          QString::fromUtf8("%1°")
              .arg(std::abs(spanDeg), 0, 'f', 2);

      const QRectF textRect(-42.0, -10.0, 84.0, 20.0);
      painter.save();
      painter.translate(textCenter);
      painter.setPen(Qt::NoPen);
      painter.setBrush(palette().dimensionLabelBackground);
      painter.drawRoundedRect(textRect, 4.0, 4.0);
      painter.setPen(selectedDimension ? palette().dimensionSelectedText
                                       : palette().dimensionText);
      painter.drawText(textRect, Qt::AlignCenter, label);
      painter.restore();
      continue;
    }
    QPointF first;
    QPointF second;
    QPointF geometryFirst;
    QPointF geometrySecond;
    QString label;
    const bool diameterDimension =
        dimension.kind == sketch::DimensionKind::CircleDiameter;
    const bool lineDistanceDimension =
        dimension.kind == sketch::DimensionKind::LineDistance;

    if (lineDistanceDimension) {
      const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
      const auto secondIndex =
          sketch_.lineIndex(dimension.secondPoint.lineId);
      if (!firstIndex || !secondIndex) continue;
      const auto witness =
          parallelLineDistanceWitness(sketch_.lines()[*firstIndex],
                                      sketch_.lines()[*secondIndex]);
      if (!witness) continue;
      geometryFirst = mapPoint(witness->first);
      geometrySecond = mapPoint(witness->second);
      first = geometryFirst;
      second = geometrySecond;
      label = QString::fromUtf8("%1 РјРј").arg(
          parallelLineDistanceMm(sketch_.lines()[*firstIndex],
                                 sketch_.lines()[*secondIndex]),
          0, 'f', 2);
    } else if (diameterDimension) {
      const auto circleIndex = sketch_.circleIndex(dimension.geometryId);
      if (!circleIndex) continue;
      const auto& circle = sketch_.circles()[*circleIndex];
      const double dx = std::cos(dimension.angleRad) * circle.radiusMm;
      const double dy = std::sin(dimension.angleRad) * circle.radiusMm;
      first = mapPoint({circle.center.xMm - dx, circle.center.yMm - dy});
      second = mapPoint({circle.center.xMm + dx, circle.center.yMm + dy});
      geometryFirst = first;
      geometrySecond = second;
      label = QString::fromUtf8("Ø %1 мм").arg(circle.radiusMm * 2.0, 0, 'f', 2);
    } else {
      if (dimension.kind == sketch::DimensionKind::LineLength) {
        const auto lineIndex = sketch_.lineIndex(dimension.geometryId);
        if (!lineIndex) continue;
        const auto& line = sketch_.lines()[*lineIndex];
        geometryFirst = mapPoint(line.start);
        geometrySecond = mapPoint(line.end);
        first = geometryFirst;
        second = geometrySecond;
      } else {
        const auto firstPoint = sketch_.referencedPoint(dimension.firstPoint);
        const auto secondPoint = sketch_.referencedPoint(dimension.secondPoint);
        if (!firstPoint || !secondPoint) continue;

        geometryFirst = mapPoint(*firstPoint);
        geometrySecond = mapPoint(*secondPoint);

        const auto witness = pointDimensionWitness(
            *firstPoint, *secondPoint, dimension.kind);
        first = mapPoint(witness.first);
        second = mapPoint(witness.second);
      }

      label = QString::fromUtf8("%1 мм").arg(
          QLineF(first, second).length() / pixelsPerMm_, 0, 'f', 2);
    }

    QPointF direction = second - first;
    const double length = std::hypot(direction.x(), direction.y());
    if (length < 1.0) continue;
    direction /= length;

    const QPointF normal(-direction.y(), direction.x());
    const QPointF offset = diameterDimension
                               ? QPointF{}
                               : normal * dimension.offsetMm * pixelsPerMm_;

    const QPointF dimensionFirst = first + offset;
    const QPointF dimensionSecond = second + offset;
    const bool selectedDimension =
        interactionState().dimension.selected.has_value() &&
        resolveDimensionIndex(*interactionState().dimension.selected) ==
            dimensionIndex;
    const QColor dimensionColor =
        selectedDimension ? palette().dimensionSelected
                          : palette().dimension;
    painter.setPen(QPen(dimensionColor, selectedDimension ? 2.2 : 1.2));
    painter.setBrush(dimensionColor);
    if (!diameterDimension) {
      painter.drawLine(geometryFirst, dimensionFirst);
      painter.drawLine(geometrySecond, dimensionSecond);
    }
    painter.drawLine(dimensionFirst, dimensionSecond);
    drawArrow(dimensionFirst, direction);
    drawArrow(dimensionSecond, -direction);
    const QPointF textCenter = dimensionLabelCenter(
        dimensionIndex, dimensionFirst, dimensionSecond);
    double textAngle = std::atan2(direction.y(), direction.x()) *
                       180.0 / 3.141592653589793;
    if (textAngle > 90.0 || textAngle < -90.0) textAngle += 180.0;
    const QRectF textRect(-42.0, -10.0, 84.0, 20.0);
    painter.save();
    painter.translate(textCenter);
    painter.rotate(textAngle);
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().dimensionLabelBackground);
    painter.drawRoundedRect(textRect, 4.0, 4.0);
    painter.setPen(selectedDimension ? palette().dimensionSelectedText
                                     : palette().dimensionText);
    painter.drawText(textRect, Qt::AlignCenter, label);
    painter.restore();
  }
  // RECTANGLE CENTER NODES
  //
  // Virtual CAD nodes owned by rectangles created with FromCenter.
  // Coordinates are derived from the current rectangle geometry, so the
  // node always follows move / resize / rotation.
  painter.save();
  painter.setPen(QPen(palette().constraint, 1.8));
  painter.setBrush(palette().background);

  for (const auto elementId : sketch_.centerNodeElementIds()) {
    const auto center = sketch_.elementCenterPoint(elementId);
    if (!center) continue;

    const QPointF screenCenter = mapPoint(*center);

    painter.drawEllipse(screenCenter, 4.2, 4.2);
    painter.drawLine(screenCenter + QPointF(-6.0, 0.0),
                     screenCenter + QPointF(6.0, 0.0));
    painter.drawLine(screenCenter + QPointF(0.0, -6.0),
                     screenCenter + QPointF(0.0, 6.0));
  }

  painter.restore();
  }

  void recordTransientTools(QPainter& painter) const {
  const auto drawArrow = [&painter](QPointF tip, QPointF direction) {
    const double length = std::hypot(direction.x(), direction.y());
    if (length < 1e-6) return;
    direction /= length;
    const QPointF normal(-direction.y(), direction.x());
    QPolygonF arrow;
    arrow << tip << tip - direction * 8.0 + normal * 3.5
          << tip - direction * 8.0 - normal * 3.5;
    painter.drawPolygon(arrow);
  };
  // CRASH-FREE 13: NO DUPLICATE PREVIEW WHILE EDITING
  //
  // A stored dimension is already rendered by the permanent dimension loop
  // above. Drawing the transient AutoDimension preview at the same time
  // produces a second dimension line. Preview remains enabled for NEW
  // dimensions only.
  if (tool() == Tool::AutoDimension &&
      snapshot.primaryDimensionVisible &&
      !interactionState().dimension.editing.has_value()) {
    const auto target = interactionState().autoDimension.target;

    if (target == SketchAutoDimensionTarget::Angle) {
      const auto firstId = static_cast<sketch::GeometryId>(
          interactionState().autoDimension.angleFirstLine.value_or(sketch::kInvalidGeometryId));
      const auto secondId = static_cast<sketch::GeometryId>(
          interactionState().autoDimension.angleSecondLine.value_or(sketch::kInvalidGeometryId));
      const auto firstIndex = sketch_.lineIndex(firstId);
      const auto secondIndex = sketch_.lineIndex(secondId);

      if (firstIndex && secondIndex) {
        const auto& firstLine = sketch_.lines()[*firstIndex];
        const auto& secondLine = sketch_.lines()[*secondIndex];
        const auto center = lineIntersectionScreen(
            firstLine, secondLine,
            [&](sketch::Point point) { return mapPoint(point); });

        if (center) {
          const auto rays = angleSectorRays(
              firstLine, secondLine, *center,
              interactionState().autoDimension.offsetMm.value_or(0.0),
              [&](sketch::Point point) { return mapPoint(point); });

          if (rays) {
            const QPointF firstDirection = rays->first;
            const QPointF secondDirection = rays->second;

            const double radius =
                std::max(16.0,
                         std::abs(interactionState().autoDimension.offsetMm.value_or(0.0)) *
                             pixelsPerMm_);
            const QPointF arcFirst = *center + firstDirection * radius;
            const QPointF arcSecond = *center + secondDirection * radius;

            double startDeg =
                -std::atan2(firstDirection.y(), firstDirection.x()) *
                180.0 / 3.14159265358979323846;
            double endDeg =
                -std::atan2(secondDirection.y(), secondDirection.x()) *
                180.0 / 3.14159265358979323846;
            double spanDeg = endDeg - startDeg;
            while (spanDeg <= -180.0) spanDeg += 360.0;
            while (spanDeg > 180.0) spanDeg -= 360.0;

                        // PREVIEW ANGLE CARRIER EXTENSIONS
            const QPointF firstStartScreen = mapPoint(firstLine.start);
            const QPointF firstEndScreen = mapPoint(firstLine.end);
            const QPointF secondStartScreen = mapPoint(secondLine.start);
            const QPointF secondEndScreen = mapPoint(secondLine.end);

            painter.setBrush(Qt::NoBrush);
            painter.setPen(
                QPen(palette().transient, 1.0, Qt::DashLine));

            if (pointSegmentDistance(*center,
                                     firstStartScreen,
                                     firstEndScreen) > 0.75) {
              if (const auto endpoint =
                      nearestSegmentEndpointTo(*center,
                                               firstStartScreen,
                                               firstEndScreen))
                painter.drawLine(*center, *endpoint);
            }

            if (pointSegmentDistance(*center,
                                     secondStartScreen,
                                     secondEndScreen) > 0.75) {
              if (const auto endpoint =
                      nearestSegmentEndpointTo(*center,
                                               secondStartScreen,
                                               secondEndScreen))
                painter.drawLine(*center, *endpoint);
            }

            painter.setPen(
                QPen(palette().transient, 1.4, Qt::DashLine));
            painter.drawLine(*center, arcFirst);
            painter.drawLine(*center, arcSecond);
            QRectF arcRect(center->x() - radius, center->y() - radius,
                           radius * 2.0, radius * 2.0);
            painter.drawArc(arcRect,
                            qRound(startDeg * 16.0),
                            qRound(spanDeg * 16.0));
          }
        }
      }
    }

    const bool diameterDimension = target == SketchAutoDimensionTarget::Circle;

    std::optional<QPointF> first;
    std::optional<QPointF> second;
    std::optional<QPointF> rawFirst;
    std::optional<QPointF> rawSecond;

    if (target == SketchAutoDimensionTarget::Angle) {
      // Angular preview is painted above.
    } else if (target == SketchAutoDimensionTarget::Line) {
      const auto id = static_cast<sketch::GeometryId>(
          interactionState().autoDimension.geometryId.value_or(sketch::kInvalidGeometryId));
      const auto index = sketch_.lineIndex(id);
      if (index) {
        first = mapPoint(sketch_.lines()[*index].start);
        second = mapPoint(sketch_.lines()[*index].end);
      }
    } else if (target == SketchAutoDimensionTarget::Circle) {
      const auto id = static_cast<sketch::GeometryId>(
          interactionState().autoDimension.geometryId.value_or(sketch::kInvalidGeometryId));
      const auto index = sketch_.circleIndex(id);
      if (index) {
        const auto& circle = sketch_.circles()[*index];
        const double angle =
            interactionState().autoDimension.angleRad.value_or(0.0);
        const double dx = std::cos(angle) * circle.radiusMm;
        const double dy = std::sin(angle) * circle.radiusMm;
        first =
            mapPoint({circle.center.xMm - dx, circle.center.yMm - dy});
        second =
            mapPoint({circle.center.xMm + dx, circle.center.yMm + dy});
      }
    } else if (target == SketchAutoDimensionTarget::Points) {
      sketch::PointReference firstReference{
          static_cast<sketch::GeometryId>(
              interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).lineId),
          interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).start,
          static_cast<sketch::GeometryId>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).circleId),
        static_cast<std::size_t>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).elementCenterId)};
      firstReference.origin =
          interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).origin;
      sketch::PointReference secondReference{
          static_cast<sketch::GeometryId>(
              interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).lineId),
          interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).start,
          static_cast<sketch::GeometryId>(
            interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).circleId),
        static_cast<std::size_t>(
            interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).elementCenterId)};
      secondReference.origin =
          interactionState().autoDimension.secondPoint.value_or(sketch::PointReference{}).origin;

      const auto firstPoint =
          sketch_.referencedPoint(firstReference);
      const auto secondPoint =
          sketch_.referencedPoint(secondReference);

      if (firstPoint && secondPoint) {
        rawFirst = mapPoint(*firstPoint);
        rawSecond = mapPoint(*secondPoint);
        const QString pointMode =
            pointDimensionModeName(interactionState().autoDimension.pointMode);
        const sketch::DimensionKind kind =
            pointMode == QStringLiteral("x")
                ? sketch::DimensionKind::PointDistanceX
                : pointMode == QStringLiteral("y")
                    ? sketch::DimensionKind::PointDistanceY
                    : sketch::DimensionKind::PointDistance;
        const auto witness =
            pointDimensionWitness(*firstPoint, *secondPoint, kind);
        first = mapPoint(witness.first);
        second = mapPoint(witness.second);
      }
    }

    if (first && second) {
      const QPointF geometryFirst = rawFirst ? *rawFirst : *first;
      const QPointF geometrySecond = rawSecond ? *rawSecond : *second;

      QPointF baseFirst = *first;
      QPointF baseSecond = *second;

      QPointF direction = baseSecond - baseFirst;
      const double length =
          std::hypot(direction.x(), direction.y());

      if (length > 1.0) {
        direction /= length;
        const QPointF normal(-direction.y(), direction.x());

        const QPointF offset =
            diameterDimension
                ? QPointF{}
                : normal *
                      interactionState().autoDimension.offsetMm.value_or(0.0) *
                      pixelsPerMm_;

        const QPointF dimensionFirst = baseFirst + offset;
        const QPointF dimensionSecond = baseSecond + offset;

        painter.setPen(
            QPen(palette().transient, 1.4, Qt::DashLine));
        painter.setBrush(palette().transient);

        if (!diameterDimension) {
          painter.drawLine(geometryFirst, dimensionFirst);
          painter.drawLine(geometrySecond, dimensionSecond);
        }

        painter.drawLine(dimensionFirst, dimensionSecond);
        drawArrow(dimensionFirst, direction);
        drawArrow(dimensionSecond, -direction);
      }
    }
  }  if (tool() == Tool::AutoDimension &&
      interactionState().autoDimension.firstPoint.has_value() &&
      interactionState().autoDimension.target == SketchAutoDimensionTarget::None) {
    sketch::PointReference first{
          static_cast<sketch::GeometryId>(
              interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).lineId),
          interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).start,
          static_cast<sketch::GeometryId>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).circleId),
        static_cast<std::size_t>(
            interactionState().autoDimension.firstPoint.value_or(sketch::PointReference{}).elementCenterId)};
    if (const auto point = sketch_.referencedPoint(first)) {
      painter.setPen(QPen(palette().transient, 2.0));
      painter.setBrush(withAlpha(palette().transient, 55));
      painter.drawEllipse(mapPoint(*point), 7.0, 7.0);
    }
  }

  if (interactionState().creation.anchor) {
    painter.setPen(QPen(palette().projection, 1.5, Qt::DashLine));
    painter.setBrush(withAlpha(palette().projection, 25));
    const QPointF first = mapPoint(*interactionState().creation.anchor);
    const QPointF current = mapPoint(hoverPoint_);
    if (tool() == Tool::Line) {
      painter.drawLine(first, current);
    } else if (tool() == Tool::Rectangle) {
      if (rectangleMode_ == RectangleMode::FromCenter) {
        const QPointF delta = current - first;
        painter.drawRect(QRectF(first - delta, first + delta).normalized());
      } else {
        painter.drawRect(QRectF(first, current).normalized());
      }

      // Keep the live width/height values attached to conventional CAD
      // dimension lines instead of presenting them as an unrelated W/H HUD.
      const QRectF rectangle = rectangleMode_ == RectangleMode::FromCenter
                                   ? QRectF(first - (current - first),
                                            first + (current - first))
                                         .normalized()
                                   : QRectF(first, current).normalized();
      if (rectangle.width() > 1.0 || rectangle.height() > 1.0) {
        constexpr double offset = 24.0;
        constexpr double extension = 5.0;
        const QColor dimensionColor = palette().dimension;
        painter.save();
        painter.setPen(QPen(dimensionColor, 1.2));
        painter.setBrush(dimensionColor);

        const auto drawInwardArrow = [&painter](QPointF tip,
                                                 QPointF direction) {
          const double length = std::hypot(direction.x(), direction.y());
          if (length < 1e-6) return;
          direction /= length;
          const QPointF normal(-direction.y(), direction.x());
          QPolygonF arrow;
          arrow << tip << tip + direction * 8.0 + normal * 3.5
                << tip + direction * 8.0 - normal * 3.5;
          painter.drawPolygon(arrow);
        };

        const double dimensionY = rectangle.bottom() + offset;
        painter.drawLine(QPointF(rectangle.left(), rectangle.bottom()),
                         QPointF(rectangle.left(), dimensionY + extension));
        painter.drawLine(QPointF(rectangle.right(), rectangle.bottom()),
                         QPointF(rectangle.right(), dimensionY + extension));
        painter.drawLine(QPointF(rectangle.left(), dimensionY),
                         QPointF(rectangle.right(), dimensionY));
        drawInwardArrow(QPointF(rectangle.left(), dimensionY), QPointF(1, 0));
        drawInwardArrow(QPointF(rectangle.right(), dimensionY), QPointF(-1, 0));

        const double dimensionX = rectangle.right() + offset;
        painter.drawLine(QPointF(rectangle.right(), rectangle.top()),
                         QPointF(dimensionX + extension, rectangle.top()));
        painter.drawLine(QPointF(rectangle.right(), rectangle.bottom()),
                         QPointF(dimensionX + extension, rectangle.bottom()));
        painter.drawLine(QPointF(dimensionX, rectangle.top()),
                         QPointF(dimensionX, rectangle.bottom()));
        drawInwardArrow(QPointF(dimensionX, rectangle.top()), QPointF(0, 1));
        drawInwardArrow(QPointF(dimensionX, rectangle.bottom()), QPointF(0, -1));
        painter.restore();
      }
    } else if (tool() == Tool::Circle) {
      const double radiusMm =
          std::hypot(hoverPoint_.xMm - interactionState().creation.anchor->xMm,
                     hoverPoint_.yMm - interactionState().creation.anchor->yMm);
      painter.drawPolyline(circlePolyline(*interactionState().creation.anchor, radiusMm));
    }
  }

  if (tool() == Tool::Circle && circleMode_ != CircleMode::CenterRadius) {
    painter.setPen(QPen(palette().projection, 1.6, Qt::DashLine));
    painter.setBrush(withAlpha(palette().projection, 24));
    const auto circleGuides = resolvedCircleGuideLines();
    if (circleGuides)
      for (const auto& line : *circleGuides) {
        painter.setPen(QPen(palette().selected, 3.0));
        painter.drawLine(mapPoint(line.start), mapPoint(line.end));
      }
    painter.setPen(QPen(palette().projection, 1.6, Qt::DashLine));
    if (circleMode_ == CircleMode::TwoPoints && interactionState().creation.circlePoints.size() == 1) {
      const auto first = interactionState().creation.circlePoints.front();
      const sketch::Point center{(first.xMm + hoverPoint_.xMm) * 0.5,
                                 (first.yMm + hoverPoint_.yMm) * 0.5};
      const double radiusMm = std::hypot(hoverPoint_.xMm - first.xMm,
                                         hoverPoint_.yMm - first.yMm) * 0.5;
      painter.drawPolyline(circlePolyline(center, radiusMm));
    } else if (circleMode_ == CircleMode::ThreePoints &&
               interactionState().creation.circlePoints.size() == 2) {
      const auto preview = circleThroughThreePoints(
          interactionState().creation.circlePoints[0], interactionState().creation.circlePoints[1], hoverPoint_);
      if (preview)
        painter.drawPolyline(circlePolyline(preview->first, preview->second));
    }
    // TWO-TANGENT SEMITRANSPARENT PREVIEW
    if (circleMode_ ==
            CircleMode::TwoTangentsRadius &&
        circleGuides && circleGuides->size() == 2 &&
        interactionState().twoTangentRadiusPreviewActive) {
      const double previewDiameter =
          snapshot.primaryDimensionVisible
              ? snapshot.primaryDimensionValue
              : circleDiameterMm_;

      const auto preview =
          clampedTwoTangentCircleForRadius(
              (*circleGuides)[0], (*circleGuides)[1],
              std::max(
                  0.01,
                  previewDiameter * 0.5),
              hoverPoint_);

      if (preview) {
        painter.save();

        painter.setPen(
            QPen(
                withAlpha(palette().projection, 190),
                1.8,
                Qt::DashLine));

        painter.setBrush(
            withAlpha(palette().projection, 48));

        painter.drawPolyline(
            circlePolyline(preview->center, preview->radiusMm));

        painter.restore();
      }
    }
    painter.setBrush(palette().projection);
    for (const auto& point : interactionState().creation.circlePoints)
      painter.drawEllipse(mapPoint(point), 4.0, 4.0);
  }

  if (tool() == Tool::Rectangle && rectangleMode_ == RectangleMode::ThreePoints &&
      !interactionState().creation.rectanglePoints.empty()) {
    painter.setPen(QPen(palette().projection, 1.6, Qt::DashLine));
    painter.setBrush(withAlpha(palette().projection, 24));
    const auto first = interactionState().creation.rectanglePoints[0];
    if (interactionState().creation.rectanglePoints.size() == 1) {
      painter.drawLine(mapPoint(first), mapPoint(hoverPoint_));
    } else {
      const auto second = interactionState().creation.rectanglePoints[1];
      const double dx = second.xMm - first.xMm;
      const double dy = second.yMm - first.yMm;
      const double length = std::hypot(dx, dy);
      if (length > 1e-9) {
        const double nx = -dy / length;
        const double ny = dx / length;
        const double rectangleHeight = (hoverPoint_.xMm - first.xMm) * nx +
                                       (hoverPoint_.yMm - first.yMm) * ny;
        const sketch::Point third{second.xMm + nx * rectangleHeight,
                                  second.yMm + ny * rectangleHeight};
        const sketch::Point fourth{first.xMm + nx * rectangleHeight,
                                   first.yMm + ny * rectangleHeight};
        QPolygonF polygon;
        polygon << mapPoint(first) << mapPoint(second) << mapPoint(third)
                << mapPoint(fourth);
        painter.drawPolygon(polygon);
      }
    }
  }
  }

  void recordSelectionHover(QPainter& painter) const {
  // CONSTRAINT TOOL SELECTION HIGHLIGHT
  //
  // Constraint tools keep their first picked entity in existing transient
  // state/properties. Render that entity in yellow so the user can clearly
  // see what has already been selected before choosing the second object.
  {
    const QColor constraintHighlight = palette().dimensionSelected;
    const QColor constraintHighlightFill =
        withAlpha(palette().dimensionSelected, 42);

    const auto drawHighlightedLine =
        [&](
            sketch::GeometryId id) {
          if (id == sketch::kInvalidGeometryId) return;

          const auto index = sketch_.lineIndex(id);
          if (!index) return;

          const auto& line = sketch_.lines()[*index];
          painter.save();
          painter.setPen(QPen(constraintHighlight, 4.0,
                              Qt::SolidLine, Qt::RoundCap));
          painter.setBrush(Qt::NoBrush);
          painter.drawLine(mapPoint(line.start), mapPoint(line.end));
          painter.restore();
        };

    const auto drawHighlightedPoint =
        [&](
            sketch::PointReference reference) {
          const auto point = sketch_.referencedPoint(reference);
          if (!point) return;

          painter.save();
          painter.setPen(QPen(constraintHighlight, 2.6));
          painter.setBrush(constraintHighlightFill);
          painter.drawEllipse(mapPoint(*point), 7.0, 7.0);
          painter.restore();
        };

    const auto drawHighlightedCircle =
        [&](
            sketch::GeometryId id) {
          if (id == sketch::kInvalidGeometryId) return;

          const auto index = sketch_.circleIndex(id);
          if (!index) return;

          const auto& circle = sketch_.circles()[*index];
          painter.save();
          painter.setPen(QPen(constraintHighlight, 4.0));
          painter.setBrush(Qt::NoBrush);
          painter.drawPolyline(
              circlePolyline(circle.center, circle.radiusMm));
          painter.restore();
        };

    const auto drawHighlightedArc =
        [&](
            sketch::GeometryId id) {
          if (id == sketch::kInvalidGeometryId) return;
          const auto index = sketch_.arcIndex(id);
          if (!index) return;
          const auto& arc = sketch_.arcs()[*index];
          const int segments = std::max(
              12, static_cast<int>(std::ceil(
                      96.0 * arc.sweepAngleRad /
                      (2.0 * std::numbers::pi))));
          painter.save();
          painter.setPen(QPen(constraintHighlight, 4.0, Qt::SolidLine,
                              Qt::RoundCap));
          painter.setBrush(Qt::NoBrush);
          painter.drawPolyline(circlePolyline(
              arc.center, arc.radiusMm, arc.startAngleRad,
              arc.sweepAngleRad, segments));
          painter.restore();
        };

    // The object under the cursor is deliberately distinct from the already
    // selected first operand. This makes the next click predictable for every
    // Sketcher constraint tool.
    if (interactionState().constraint.hoverOperand) {
      const auto hover = *interactionState().constraint.hoverOperand;
      const auto hoverKind = hover.kind;
      const auto hoverId = hover.geometryId;
      const QColor hoverColor = palette().sceneHover;
      if (hoverKind == SketchGeometryOperandKind::Line) {
        const auto index = sketch_.lineIndex(hoverId);
        if (index) {
          painter.save();
          painter.setPen(QPen(hoverColor, 4.0, Qt::SolidLine,
                              Qt::RoundCap));
          painter.drawLine(mapPoint(sketch_.lines()[*index].start),
                           mapPoint(sketch_.lines()[*index].end));
          painter.restore();
        }
      } else if (hoverKind == SketchGeometryOperandKind::Circle) {
        const auto index = sketch_.circleIndex(hoverId);
        if (index) {
          painter.save();
          painter.setPen(QPen(hoverColor, 4.0));
          painter.setBrush(Qt::NoBrush);
          const auto& circle = sketch_.circles()[*index];
          painter.drawPolyline(circlePolyline(circle.center,
                                               circle.radiusMm));
          painter.restore();
        }
      } else if (hoverKind == SketchGeometryOperandKind::Arc) {
        const auto index = sketch_.arcIndex(hoverId);
        if (index) {
          const auto& arc = sketch_.arcs()[*index];
          const int segments = std::max(
              12, static_cast<int>(std::ceil(
                      96.0 * arc.sweepAngleRad /
                      (2.0 * std::numbers::pi))));
          painter.save();
          painter.setPen(QPen(hoverColor, 4.0, Qt::SolidLine,
                              Qt::RoundCap));
          painter.setBrush(Qt::NoBrush);
          painter.drawPolyline(circlePolyline(
              arc.center, arc.radiusMm, arc.startAngleRad,
              arc.sweepAngleRad, segments));
          painter.restore();
        }
      } else if (hoverKind == SketchGeometryOperandKind::Point &&
                 interactionState().constraint.hoverPoint) {
        painter.save();
        painter.setPen(QPen(hoverColor, 2.6));
        painter.setBrush(withAlpha(palette().sceneHover, 48));
        painter.drawEllipse(
            mapPoint(*interactionState().constraint.hoverPoint),
            7.0, 7.0);
        painter.restore();
      } else if (hoverKind == SketchGeometryOperandKind::XAxis ||
                 hoverKind == SketchGeometryOperandKind::YAxis) {
        const QPointF datumOrigin = mapPoint({0.0, 0.0});
        QPointF direction =
            mapPoint(hoverKind == SketchGeometryOperandKind::XAxis
                         ? sketch::Point{1.0, 0.0}
                         : sketch::Point{0.0, 1.0}) -
            datumOrigin;
        const double length = std::hypot(direction.x(), direction.y());
        if (length > 1e-9) {
          direction /= length;
          painter.save();
          painter.setPen(QPen(hoverColor, 3.5, Qt::SolidLine,
                              Qt::RoundCap));
          const double extent = std::hypot(width(), height());
          painter.drawLine(datumOrigin - direction * extent,
                           datumOrigin + direction * extent);
          painter.restore();
        }
      }
    }

    if (tool() == Tool::CoincidentConstraint) {
      // point -> point
      if (interactionState().constraint.coincidentFirstPoint)
        drawHighlightedPoint(*interactionState().constraint.coincidentFirstPoint);

      // line body -> point (merged PointOnLine workflow)
      if (interactionState().constraint.pointOnLineCarrier.has_value()) {
        drawHighlightedLine(
            static_cast<sketch::GeometryId>(
                interactionState().constraint.pointOnLineCarrier.value_or(sketch::kInvalidGeometryId)));
      }
      // POINT-ON-CIRCLE CARRIER HIGHLIGHT V3
      if (interactionState().constraint.pointOnCircleCarrier.has_value()) {
        drawHighlightedCircle(
            static_cast<sketch::GeometryId>(
                interactionState().constraint.pointOnCircleCarrier.value_or(sketch::kInvalidGeometryId)));
      }
    }

    if (tool() == Tool::PerpendicularConstraint &&
        interactionState().constraint.perpendicularFirstLine.has_value()) {
      drawHighlightedLine(
          static_cast<sketch::GeometryId>(
              interactionState().constraint.perpendicularFirstLine.value_or(sketch::kInvalidGeometryId)));
    }

    if (tool() == Tool::ParallelConstraint &&
        interactionState().constraint.parallelFirstLine.has_value()) {
      drawHighlightedLine(
          static_cast<sketch::GeometryId>(
              interactionState().constraint.parallelFirstLine.value_or(sketch::kInvalidGeometryId)));
    }

    if (tool() == Tool::EqualConstraint &&
        interactionState().constraint.equalFirst) {
      const auto operand = *interactionState().constraint.equalFirst;
      if (operand.kind == SketchGeometryOperandKind::Circle)
        drawHighlightedCircle(operand.geometryId);
      else
        drawHighlightedLine(operand.geometryId);
    }
    if (tool() == Tool::TangentConstraint &&
        interactionState().constraint.tangentFirst) {
      const auto operand = *interactionState().constraint.tangentFirst;
      if (operand.kind == SketchGeometryOperandKind::Circle)
        drawHighlightedCircle(operand.geometryId);
      else if (operand.kind == SketchGeometryOperandKind::Line)
        drawHighlightedLine(operand.geometryId);
      else if (operand.kind == SketchGeometryOperandKind::Arc)
        drawHighlightedArc(operand.geometryId);
    }
  }
  }

  void recordHudOverlays(QPainter& painter) const {
  if (interactionState().selectionBox.active) {
    const QRectF selectionRect(QPointF(interactionState().selectionBox.start.x, interactionState().selectionBox.start.y), QPointF(interactionState().selectionBox.current.x, interactionState().selectionBox.current.y));
    const QRectF normalized = selectionRect.normalized();

    painter.setPen(QPen(palette().selectionBoxOutline, 1.4, Qt::DashLine));
    painter.setBrush(palette().selectionBoxFill);
    painter.drawRect(normalized);
  }

  painter.setPen(palette().hudText);
  const QString viewDescription =
      viewAlignedToSketchPlane()
          ? QString::fromUtf8("плоскость %1°").arg(qRound(viewRotationDeg_))
          : QString::fromUtf8("3D: азимут %1°, наклон %2°")
                .arg(qRound(viewYawDeg_))
                .arg(qRound(viewPitchDeg_));
  painter.drawText(
      QRectF(kRulerLeft + 12, height() - 30, width() - 70, 22),
      Qt::AlignLeft | Qt::AlignVCenter,
      QString::fromUtf8(
          "Шаг сетки: %1 мм   •   Привязка: %2   •   Масштаб: %3%   •   Вид: %4")
          .arg(snapStepMm_)
          .arg(snapEnabled_ ? QString::fromUtf8("ВКЛ")
                            : QString::fromUtf8("ВЫКЛ"))
          .arg(qRound(pixelsPerMm_ / 5.0 * 100.0))
          .arg(viewDescription));
  }

  const SketchRenderSnapshot& snapshot;
  const SketchRenderScene& scene_;
  const SketchRenderGeometry& sketch_;
  std::unordered_set<sketch::GeometryId> selectedLineIds_;
  std::unordered_set<std::size_t> selectedElementIds_;
  std::unordered_set<sketch::GeometryId> selectedCircleIds_;
  std::unordered_set<sketch::GeometryId> selectedArcIds_;
  std::unordered_set<sketch::GeometryId> selectedBezierIds_;
  SketchRenderSelectionKind selectionKind_;
  sketch::GeometryId selectionLineId_;
  sketch::GeometryId selectionCircleId_;
  sketch::GeometryId selectionArcId_;
  sketch::GeometryId selectionBezierId_;
  const std::optional<SketchRenderSnap>& constructionHover_;
  const std::optional<SketchProjectionEdgeToken>& hoveredProjectionEdge_;
  sketch::Point hoverPoint_;
  double pixelsPerMm_;
  double snapStepMm_;
  bool snapEnabled_;
  bool gridVisible_;
  double viewRotationDeg_;
  double viewYawDeg_;
  double viewPitchDeg_;
  SketchRenderCircleMode circleMode_;
  double circleDiameterMm_;
  SketchRenderRectangleMode rectangleMode_;
  bool realReferenceBodyVisible_;
  bool referenceBodyVisible_;
  bool referenceProfileVisible_;
  const SketchPlacement& referencePlacement_;
  const SketchRenderGeometry& referenceProfile_;
  const std::vector<std::shared_ptr<const BodyRenderMesh>>& sceneBodyMeshes_;
  const std::vector<SketchRenderSceneReference>& sceneSketches_;
  const std::vector<SketchSceneImageReference>& sceneImages_;
  BodyRenderMesh emptyMesh_;
  const BodyRenderMesh& referenceBodyMesh_;
  const BodyRenderMesh& referenceFaceMesh_;
  ReferenceBox referenceBox_;
};

}  // namespace
SketchRenderFrame SketchRenderer::buildFrame(
    SketchRenderSnapshot snapshot) const {
  SketchRenderFrame frame;
  frame.snapshot = std::move(snapshot);
  frame.layers.reserve(kSketchRenderPassOrder.size());
  for (const auto pass : kSketchRenderPassOrder)
    frame.layers.push_back({pass});
  return frame;
}

void SketchRenderer::render(QPainter& painter,
                            const SketchRenderFrame& frame) const {
  if (!frame.hasCanonicalOrder() || !frame.snapshot.scene ||
      !frame.snapshot.scene->valid)
    return;
  SketchFrameRecorder recorder(frame.snapshot);
  for (const auto& layer : frame.layers)
    recorder.record(layer.pass, painter);
}

}  // namespace solidar
