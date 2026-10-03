#include "ui/ExtrusionPreviewGeometry.h"

#include <QPolygonF>

#include <algorithm>

namespace solidar {
namespace {

void appendUniqueGenerator(std::vector<QLineF>& result, QPointF point,
                           QPointF offset) {
  constexpr double kDuplicateTolerancePx = 0.01;
  const bool duplicate = std::any_of(
      result.begin(), result.end(), [point](const QLineF& line) {
        return QLineF(line.p1(), point).length() < kDuplicateTolerancePx;
      });
  if (!duplicate) result.emplace_back(point, point + offset);
}

}  // namespace

std::vector<QLineF> extrusionPreviewGenerators(const QPainterPath& basePath,
                                               QPointF offset,
                                               bool curvedBoundary) {
  std::vector<QLineF> result;
  if (offset.manhattanLength() <= 1.0) return result;

  const QPointF perpendicular(-offset.y(), offset.x());
  for (QPolygonF boundary : basePath.toSubpathPolygons()) {
    if (boundary.size() > 1 &&
        QLineF(boundary.front(), boundary.back()).length() < 0.01)
      boundary.removeLast();
    if (boundary.isEmpty()) continue;

    if (!curvedBoundary) {
      for (const QPointF& point : boundary)
        appendUniqueGenerator(result, point, offset);
      continue;
    }

    QPointF minimum = boundary.front();
    QPointF maximum = boundary.front();
    double minimumProjection = QPointF::dotProduct(minimum, perpendicular);
    double maximumProjection = minimumProjection;
    for (const QPointF& point : boundary) {
      const double projection = QPointF::dotProduct(point, perpendicular);
      if (projection < minimumProjection) {
        minimumProjection = projection;
        minimum = point;
      }
      if (projection > maximumProjection) {
        maximumProjection = projection;
        maximum = point;
      }
    }
    appendUniqueGenerator(result, minimum, offset);
    appendUniqueGenerator(result, maximum, offset);
  }
  return result;
}

}  // namespace solidar
