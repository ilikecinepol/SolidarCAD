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

void Sketch::addCircle(Point center, double radiusMm) {
  if (radiusMm <= 0.0) return;
  circles_.push_back({center, radiusMm});
  updateBounds();
}

void Sketch::removeLine(std::size_t index) {
  if (index < lines_.size()) lines_.erase(lines_.begin() + index);
  updateBounds();
}

void Sketch::removeCircle(std::size_t index) {
  if (index < circles_.size()) circles_.erase(circles_.begin() + index);
  updateBounds();
}

void Sketch::removeElement(std::size_t elementId) {
  std::erase_if(lines_, [elementId](const Line& line) {
    return line.elementId == elementId;
  });
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

void Sketch::translateCircle(std::size_t index, double dxMm, double dyMm) {
  if (index >= circles_.size()) return;
  circles_[index].center.xMm += dxMm;
  circles_[index].center.yMm += dyMm;
  updateBounds();
}

double Sketch::widthMm() const noexcept { return widthMm_; }
double Sketch::heightMm() const noexcept { return heightMm_; }
const std::vector<Line>& Sketch::lines() const noexcept { return lines_; }
const std::vector<Circle>& Sketch::circles() const noexcept { return circles_; }

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
  if (lines_.empty()) return false;
  const auto samePoint = [](Point first, Point second) {
    return std::abs(first.xMm - second.xMm) <= 1e-7 &&
           std::abs(first.yMm - second.yMm) <= 1e-7;
  };
  // Every vertex of one or several independent closed loops has degree two.
  // This also permits Ctrl-selection of multiple extrusion regions.
  for (const auto& line : lines_) {
    for (const Point vertex : {line.start, line.end}) {
      std::size_t degree = 0;
      for (const auto& candidate : lines_) {
        if (samePoint(vertex, candidate.start)) ++degree;
        if (samePoint(vertex, candidate.end)) ++degree;
      }
      if (degree != 2) return false;
    }
  }
  return true;
}

}  // namespace solidar::sketch
