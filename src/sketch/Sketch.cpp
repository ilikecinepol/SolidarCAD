#include "sketch/Sketch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace solidar::sketch {

Sketch::Sketch() { clear(); }

void Sketch::clear() {
  lines_.clear();
  circles_.clear();
  lineIds_.clear();
  circleIds_.clear();
  dimensions_.clear();
  widthMm_ = 0.0;
  heightMm_ = 0.0;
}

void Sketch::setRectangle(double widthMm, double heightMm) {
  if (widthMm <= 0.0 || heightMm <= 0.0)
    throw std::invalid_argument("Sketch dimensions must be positive");

  clear();
  const double halfWidth = widthMm * 0.5;
  const double halfHeight = heightMm * 0.5;
  const Point bottomLeft{-halfWidth, -halfHeight};
  const Point bottomRight{halfWidth, -halfHeight};
  const Point topRight{halfWidth, halfHeight};
  const Point topLeft{-halfWidth, halfHeight};
  const auto elementId = nextElementId_++;
  lines_ = {{bottomLeft, bottomRight, elementId},
            {bottomRight, topRight, elementId},
            {topRight, topLeft, elementId},
            {topLeft, bottomLeft, elementId}};
  lineIds_.clear();
  lineIds_.reserve(lines_.size());
  for (std::size_t index = 0; index < lines_.size(); ++index)
    lineIds_.push_back(nextGeometryId_++);
  updateBounds();
}

void Sketch::addLine(Point start, Point end) {
  if (start.xMm == end.xMm && start.yMm == end.yMm) return;
  lines_.push_back({start, end, nextElementId_++});
  lineIds_.push_back(nextGeometryId_++);
  updateBounds();
}

void Sketch::addRectangle(Point firstCorner, Point oppositeCorner) {
  if (firstCorner.xMm == oppositeCorner.xMm ||
      firstCorner.yMm == oppositeCorner.yMm)
    return;
  const Point second{oppositeCorner.xMm, firstCorner.yMm};
  const Point fourth{firstCorner.xMm, oppositeCorner.yMm};
  const auto elementId = nextElementId_++;
  lines_.push_back({firstCorner, second, elementId});
  lineIds_.push_back(nextGeometryId_++);
  lines_.push_back({second, oppositeCorner, elementId});
  lineIds_.push_back(nextGeometryId_++);
  lines_.push_back({oppositeCorner, fourth, elementId});
  lineIds_.push_back(nextGeometryId_++);
  lines_.push_back({fourth, firstCorner, elementId});
  lineIds_.push_back(nextGeometryId_++);
  updateBounds();
}

void Sketch::addRectangle(Point first, Point second, Point third, Point fourth) {
  const auto elementId = nextElementId_++;
  lines_.push_back({first, second, elementId});
  lineIds_.push_back(nextGeometryId_++);
  lines_.push_back({second, third, elementId});
  lineIds_.push_back(nextGeometryId_++);
  lines_.push_back({third, fourth, elementId});
  lineIds_.push_back(nextGeometryId_++);
  lines_.push_back({fourth, first, elementId});
  lineIds_.push_back(nextGeometryId_++);
  updateBounds();
}

void Sketch::addCircle(Point center, double radiusMm) {
  if (radiusMm <= 0.0) return;
  circles_.push_back({center, radiusMm});
  circleIds_.push_back(nextGeometryId_++);
  updateBounds();
}

void Sketch::removeLine(std::size_t index) {
  if (index >= lines_.size()) return;

  const GeometryId removedId = lineIds_[index];
  lines_.erase(lines_.begin() + index);
  lineIds_.erase(lineIds_.begin() + index);

  auto& dimensions = dimensions_;
  std::erase_if(dimensions, [removedId](const Dimension& dimension) {
    if (dimension.kind == DimensionKind::LineLength)
      return dimension.geometryId == removedId;
    if (dimension.kind == DimensionKind::PointDistance)
      return dimension.firstPoint.lineId == removedId ||
             dimension.secondPoint.lineId == removedId;
    return false;
  });

  updateBounds();
}

void Sketch::removeCircle(std::size_t index) {
  if (index >= circles_.size()) return;

  const GeometryId removedId = circleIds_[index];
  circles_.erase(circles_.begin() + index);
  circleIds_.erase(circleIds_.begin() + index);

  auto& dimensions = dimensions_;
  std::erase_if(dimensions, [removedId](const Dimension& dimension) {
    return dimension.kind == DimensionKind::CircleDiameter &&
           dimension.geometryId == removedId;
  });

  updateBounds();
}

void Sketch::removeElement(std::size_t elementId) {
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

    auto& dimensions = dimensions_;
    std::erase_if(dimensions, [&wasRemoved](const Dimension& dimension) {
      if (dimension.kind == DimensionKind::LineLength)
        return wasRemoved(dimension.geometryId);
      if (dimension.kind == DimensionKind::PointDistance)
        return wasRemoved(dimension.firstPoint.lineId) ||
               wasRemoved(dimension.secondPoint.lineId);
      return false;
    });
  }

  updateBounds();
}

void Sketch::translateElement(std::size_t elementId, double dxMm,
                              double dyMm) {
  for (auto& line : lines_) {
    if (line.elementId != elementId) continue;
    line.start.xMm += dxMm;
    line.start.yMm += dyMm;
    line.end.xMm += dxMm;
    line.end.yMm += dyMm;
  }
  updateBounds();
}

void Sketch::setElementDashed(std::size_t elementId, bool dashed) {
  for (auto& line : lines_) {
    if (line.elementId == elementId) line.dashed = dashed;
  }
}

void Sketch::setCircleDashed(std::size_t index, bool dashed) {
  if (index < circles_.size()) circles_[index].dashed = dashed;
}

void Sketch::translateCircle(std::size_t index, double dxMm, double dyMm) {
  if (index >= circles_.size()) return;
  circles_[index].center.xMm += dxMm;
  circles_[index].center.yMm += dyMm;
  updateBounds();
}

void Sketch::setCircleDashedById(GeometryId id, bool dashed) {
  const auto index = circleIndex(id);
  if (index) setCircleDashed(*index, dashed);
}

void Sketch::translateCircleById(GeometryId id, double dxMm, double dyMm) {
  const auto index = circleIndex(id);
  if (index) translateCircle(*index, dxMm, dyMm);
}

bool Sketch::setLineLengthById(GeometryId id, double lengthMm) {
  const auto index = lineIndex(id);
  return index ? setLineLength(*index, lengthMm) : false;
}

bool Sketch::setCircleDiameterById(GeometryId id, double diameterMm) {
  const auto index = circleIndex(id);
  return index ? setCircleDiameter(*index, diameterMm) : false;
}

GeometryId Sketch::lineId(std::size_t index) const noexcept {
  return index < lineIds_.size() ? lineIds_[index] : kInvalidGeometryId;
}

GeometryId Sketch::circleId(std::size_t index) const noexcept {
  return index < circleIds_.size() ? circleIds_[index] : kInvalidGeometryId;
}

std::optional<std::size_t> Sketch::lineIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  const auto found = std::find(lineIds_.begin(), lineIds_.end(), id);
  if (found == lineIds_.end()) return std::nullopt;
  return static_cast<std::size_t>(std::distance(lineIds_.begin(), found));
}

std::optional<std::size_t> Sketch::circleIndex(GeometryId id) const noexcept {
  if (id == kInvalidGeometryId) return std::nullopt;
  const auto found = std::find(circleIds_.begin(), circleIds_.end(), id);
  if (found == circleIds_.end()) return std::nullopt;
  return static_cast<std::size_t>(std::distance(circleIds_.begin(), found));
}

std::optional<Point> Sketch::referencedPoint(
    PointReference reference) const noexcept {
  const auto index = lineIndex(reference.lineId);
  if (!index) return std::nullopt;
  const auto& line = lines_[*index];
  return reference.start ? line.start : line.end;
}

bool Sketch::setLineLength(std::size_t index, double lengthMm) {
  if (index >= lines_.size() || lengthMm <= 0.0) return false;
  const Point start = lines_[index].start;
  const Point oldEnd = lines_[index].end;
  const double dx = oldEnd.xMm - start.xMm;
  const double dy = oldEnd.yMm - start.yMm;
  const double oldLength = std::hypot(dx, dy);
  if (oldLength <= 1e-9) return false;
  const Point newEnd{start.xMm + dx / oldLength * lengthMm,
                     start.yMm + dy / oldLength * lengthMm};
  const auto same = [](Point a, Point b) {
    return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) <= 1e-7;
  };
  for (auto& line : lines_) {
    if (same(line.start, oldEnd)) line.start = newEnd;
    if (same(line.end, oldEnd)) line.end = newEnd;
  }
  updateBounds();
  return true;
}

bool Sketch::setPointDistance(PointReference firstReference,
                              PointReference secondReference,
                              double distanceMm) {
  const auto first = referencedPoint(firstReference);
  const auto second = referencedPoint(secondReference);
  if (!first || !second || distanceMm <= 0.0) return false;
  const double dx = second->xMm - first->xMm;
  const double dy = second->yMm - first->yMm;
  const double oldDistance = std::hypot(dx, dy);
  if (oldDistance <= 1e-9) return false;
  const Point moved{first->xMm + dx / oldDistance * distanceMm,
                    first->yMm + dy / oldDistance * distanceMm};
  const auto same = [](Point a, Point b) {
    return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) <= 1e-7;
  };
  for (auto& line : lines_) {
    if (same(line.start, *second)) line.start = moved;
    if (same(line.end, *second)) line.end = moved;
  }
  updateBounds();
  return true;
}

bool Sketch::setCircleDiameter(std::size_t index, double diameterMm) {
  if (index >= circles_.size() || diameterMm <= 0.0) return false;
  circles_[index].radiusMm = diameterMm * 0.5;
  updateBounds();
  return true;
}

void Sketch::addDimension(Dimension dimension) { storeDimension(dimension); }

void Sketch::storeDimension(const Dimension& dimension) {
  if (dimension.valueMm > 0.0)
    dimensions_.push_back(dimension);
}

void Sketch::clearDimensions() { dimensions_.clear(); }

bool Sketch::setDimensionPlacement(std::size_t index, double offsetMm,
                                   double angleRad) {
  auto& dimensions = dimensions_;
  if (index >= dimensions.size()) return false;
  dimensions[index].offsetMm = offsetMm;
  dimensions[index].angleRad = angleRad;
  return true;
}

bool Sketch::setDimensionValue(std::size_t index, double valueMm) {
  auto& dimensions = dimensions_;
  if (index >= dimensions.size() || valueMm <= 0.0) return false;
  dimensions[index].valueMm = valueMm;
  return true;
}

double Sketch::widthMm() const noexcept { return widthMm_; }
double Sketch::heightMm() const noexcept { return heightMm_; }
const std::vector<Line>& Sketch::lines() const noexcept { return lines_; }
const std::vector<Circle>& Sketch::circles() const noexcept { return circles_; }
const std::vector<Dimension>& Sketch::dimensions() const {
  return dimensions_;
}

void Sketch::updateBounds() noexcept {
  if (lines_.empty() && circles_.empty()) {
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
  widthMm_ = maxX - minX;
  heightMm_ = maxY - minY;
}

bool Sketch::isClosed() const noexcept {
  if (std::none_of(lines_.begin(), lines_.end(),
                   [](const Line& line) { return !line.dashed; }))
    return false;
  const auto samePoint = [](Point first, Point second) {
    return std::abs(first.xMm - second.xMm) <= 1e-7 &&
           std::abs(first.yMm - second.yMm) <= 1e-7;
  };
  // Every vertex of one or several independent closed loops has degree two.
  // This also permits Ctrl-selection of multiple extrusion regions.
  for (const auto& line : lines_) {
    if (line.dashed) continue;
    for (const Point vertex : {line.start, line.end}) {
      std::size_t degree = 0;
      for (const auto& candidate : lines_) {
        if (candidate.dashed) continue;
        if (samePoint(vertex, candidate.start)) ++degree;
        if (samePoint(vertex, candidate.end)) ++degree;
      }
      if (degree != 2) return false;
    }
  }
  return true;
}

}  // namespace solidar::sketch
