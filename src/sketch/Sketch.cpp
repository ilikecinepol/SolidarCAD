#include "sketch/Sketch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace solidar::sketch {
namespace {

using DimensionRegistry =
    std::unordered_map<const Sketch*, std::vector<Dimension>>;

DimensionRegistry& dimensionRegistry() {
  static DimensionRegistry registry;
  return registry;
}

std::vector<Dimension>& storedDimensions(const Sketch* sketch) {
  return dimensionRegistry()[sketch];
}

}  // namespace

Sketch::Sketch() { clear(); }

void Sketch::clear() {
  lines_.clear();
  circles_.clear();
  storedDimensions(this).clear();
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
  updateBounds();
}

void Sketch::addLine(Point start, Point end) {
  if (start.xMm == end.xMm && start.yMm == end.yMm) return;
  lines_.push_back({start, end, nextElementId_++});
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
  lines_.push_back({second, oppositeCorner, elementId});
  lines_.push_back({oppositeCorner, fourth, elementId});
  lines_.push_back({fourth, firstCorner, elementId});
  updateBounds();
}

void Sketch::addRectangle(Point first, Point second, Point third, Point fourth) {
  const auto elementId = nextElementId_++;
  lines_.push_back({first, second, elementId});
  lines_.push_back({second, third, elementId});
  lines_.push_back({third, fourth, elementId});
  lines_.push_back({fourth, first, elementId});
  updateBounds();
}

void Sketch::addCircle(Point center, double radiusMm) {
  if (radiusMm <= 0.0) return;
  circles_.push_back({center, radiusMm});
  updateBounds();
}

void Sketch::removeLine(std::size_t index) {
  if (index < lines_.size()) lines_.erase(lines_.begin() + index);
  storedDimensions(this).clear();
  updateBounds();
}

void Sketch::removeCircle(std::size_t index) {
  if (index < circles_.size()) circles_.erase(circles_.begin() + index);
  storedDimensions(this).clear();
  updateBounds();
}

void Sketch::removeElement(std::size_t elementId) {
  std::erase_if(lines_, [elementId](const Line& line) {
    return line.elementId == elementId;
  });
  storedDimensions(this).clear();
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

std::optional<Point> Sketch::referencedPoint(
    PointReference reference) const noexcept {
  if (reference.lineIndex >= lines_.size()) return std::nullopt;
  const auto& line = lines_[reference.lineIndex];
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
    storedDimensions(this).push_back(dimension);
}

void Sketch::clearDimensions() { storedDimensions(this).clear(); }

bool Sketch::setDimensionPlacement(std::size_t index, double offsetMm,
                                   double angleRad) {
  auto& dimensions = storedDimensions(this);
  if (index >= dimensions.size()) return false;
  dimensions[index].offsetMm = offsetMm;
  dimensions[index].angleRad = angleRad;
  return true;
}

bool Sketch::setDimensionValue(std::size_t index, double valueMm) {
  auto& dimensions = storedDimensions(this);
  if (index >= dimensions.size() || valueMm <= 0.0) return false;
  dimensions[index].valueMm = valueMm;
  return true;
}

double Sketch::widthMm() const noexcept { return widthMm_; }
double Sketch::heightMm() const noexcept { return heightMm_; }
const std::vector<Line>& Sketch::lines() const noexcept { return lines_; }
const std::vector<Circle>& Sketch::circles() const noexcept { return circles_; }
const std::vector<Dimension>& Sketch::dimensions() const {
  return storedDimensions(this);
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
