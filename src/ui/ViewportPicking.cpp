#include "ui/ViewportPicking.h"

#include <QLineF>

#include <algorithm>
#include <cmath>

namespace solidar {

std::optional<double> triangleDepthAt(QPointF point, const ProjectedPoint& a,
                                      const ProjectedPoint& b,
                                      const ProjectedPoint& c) {
  const QPointF v0 = b.screen - a.screen;
  const QPointF v1 = c.screen - a.screen;
  const QPointF v2 = point - a.screen;
  const double denominator = v0.x() * v1.y() - v1.x() * v0.y();
  if (std::abs(denominator) < 1e-12) return std::nullopt;
  const double u = (v2.x() * v1.y() - v1.x() * v2.y()) / denominator;
  const double v = (v0.x() * v2.y() - v2.x() * v0.y()) / denominator;
  const double w = 1.0 - u - v;
  constexpr double tolerance = 1e-9;
  if (u < -tolerance || v < -tolerance || w < -tolerance)
    return std::nullopt;
  return a.depth * w + b.depth * u + c.depth * v;
}

SegmentHit closestSegmentHit(QPointF point, const ProjectedPoint& a,
                             const ProjectedPoint& b) {
  const QPointF segment = b.screen - a.screen;
  const double lengthSquared = QPointF::dotProduct(segment, segment);
  const double parameter = lengthSquared < 1e-12
                               ? 0.0
                               : std::clamp(QPointF::dotProduct(
                                                point - a.screen, segment) /
                                                lengthSquared,
                                            0.0, 1.0);
  const QPointF closest = a.screen + segment * parameter;
  return {QLineF(point, closest).length(), parameter,
          a.depth + (b.depth - a.depth) * parameter};
}

}  // namespace solidar
