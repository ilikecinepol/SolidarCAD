#include "ui/ViewportPicking.h"

#include <QLineF>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

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

double linearValueFromDrag(const LinearDragSnapshot& drag, QPointF cursor,
                           double minimum, double maximum) {
  if (!std::isfinite(drag.initialValue) || !std::isfinite(minimum) ||
      !std::isfinite(maximum) || minimum > maximum)
    return minimum;
  const double lengthSquared =
      QPointF::dotProduct(drag.projectedUnitAxis, drag.projectedUnitAxis);
  if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-9)
    return std::clamp(drag.initialValue, minimum, maximum);
  const double signedDelta = QPointF::dotProduct(
      cursor - drag.cursorStart, drag.projectedUnitAxis) / lengthSquared;
  const double candidate = drag.initialValue + signedDelta;
  return std::isfinite(candidate)
             ? std::clamp(candidate, minimum, maximum)
             : std::clamp(drag.initialValue, minimum, maximum);
}

QPointF robustLinearDragAxis(QPointF projectedUnitAxis, QPointF drawnDirection,
                             double nearEndOnThresholdPx) {
  const double threshold = std::max(1e-9, nearEndOnThresholdPx);
  if (std::isfinite(projectedUnitAxis.x()) &&
      std::isfinite(projectedUnitAxis.y())) {
    const double magnitude = QLineF({}, projectedUnitAxis).length();
    if (magnitude >= threshold) return projectedUnitAxis;
  }
  if (std::isfinite(drawnDirection.x()) &&
      std::isfinite(drawnDirection.y())) {
    const double drawnLength = QLineF({}, drawnDirection).length();
    if (drawnLength >= 1e-9)
      return (drawnDirection / drawnLength) * threshold;
  }
  return QPointF(0.0, -threshold);
}

std::optional<double> angularValueFromProjectedBasis(QPointF cursor,
                                                     QPointF origin,
                                                     QPointF uPoint,
                                                     QPointF vPoint) {
  const QPointF u = uPoint - origin;
  const QPointF v = vPoint - origin;
  const QPointF delta = cursor - origin;
  const double determinant = u.x() * v.y() - u.y() * v.x();
  // A near-zero determinant means the projected basis is degenerate (edge-on
  // pose or collapsed basis): the inverse projection is ill-conditioned and no
  // fabricated angle is trustworthy. Report "indeterminate" so the caller can
  // preserve the currently accepted angle instead of jumping to a bogus value.
  if (std::abs(determinant) < 1e-9) return std::nullopt;
  const double uCoordinate =
      (delta.x() * v.y() - delta.y() * v.x()) / determinant;
  const double vCoordinate =
      (u.x() * delta.y() - u.y() * delta.x()) / determinant;
  double angle = std::atan2(vCoordinate, uCoordinate) * 180.0 /
                 std::numbers::pi;
  if (angle <= 0.0) angle += 360.0;
  return std::clamp(angle, 0.01, 360.0);
}

namespace {
// Standard barycentric point-in-triangle test. On-edge counts as inside.
bool pointInTriangle(QPointF point, QPointF a, QPointF b, QPointF c) {
  const auto sign = [](QPointF p1, QPointF p2, QPointF p3) {
    return (p1.x() - p3.x()) * (p2.y() - p3.y()) -
           (p2.x() - p3.x()) * (p1.y() - p3.y());
  };
  const double d1 = sign(point, a, b);
  const double d2 = sign(point, b, c);
  const double d3 = sign(point, c, a);
  const bool hasNeg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
  const bool hasPos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
  return !(hasNeg && hasPos);
}
}  // namespace

bool segmentIntersectsRect(const ProjectedPoint& a, const ProjectedPoint& b,
                           const QRectF& rect) {
  if (rect.contains(a.screen) || rect.contains(b.screen)) return true;
  const QLineF segment(a.screen, b.screen);
  const QLineF top(rect.topLeft(), rect.topRight());
  const QLineF right(rect.topRight(), rect.bottomRight());
  const QLineF bottom(rect.bottomRight(), rect.bottomLeft());
  const QLineF left(rect.bottomLeft(), rect.topLeft());
  QPointF intersection;
  return segment.intersects(top, &intersection) == QLineF::BoundedIntersection ||
         segment.intersects(right, &intersection) ==
             QLineF::BoundedIntersection ||
         segment.intersects(bottom, &intersection) ==
             QLineF::BoundedIntersection ||
         segment.intersects(left, &intersection) ==
             QLineF::BoundedIntersection;
}

bool triangleIntersectsRect(const ProjectedPoint& a, const ProjectedPoint& b,
                            const ProjectedPoint& c, const QRectF& rect) {
  if (rect.contains(a.screen) || rect.contains(b.screen) ||
      rect.contains(c.screen))
    return true;
  if (segmentIntersectsRect(a, b, rect) || segmentIntersectsRect(b, c, rect) ||
      segmentIntersectsRect(c, a, rect))
    return true;
  // Rect fully enclosed by the triangle (no vertex inside, no edge crossing).
  return pointInTriangle(rect.topLeft(), a.screen, b.screen, c.screen) &&
         pointInTriangle(rect.topRight(), a.screen, b.screen, c.screen) &&
         pointInTriangle(rect.bottomRight(), a.screen, b.screen, c.screen) &&
         pointInTriangle(rect.bottomLeft(), a.screen, b.screen, c.screen);
}

std::optional<std::pair<ProjectedPoint, ProjectedPoint>> clipSegmentToRect(
    const ProjectedPoint& a, const ProjectedPoint& b, const QRectF& rect) {
  const QRectF r = rect.normalized();
  const double x0 = a.screen.x();
  const double y0 = a.screen.y();
  const double x1 = b.screen.x();
  const double y1 = b.screen.y();
  const double dx = x1 - x0;
  const double dy = y1 - y0;
  double t0 = 0.0;
  double t1 = 1.0;

  // Liang-Barsky parametric clip. Each boundary contributes a half-plane test
  // p * t >= q; p == 0 means the segment is parallel to that boundary.
  const auto clip = [&](double p, double q) {
    if (std::abs(p) < 1e-12) {
      if (q < 0.0) return false;  // entirely on the outside
      return true;
    }
    const double r = q / p;
    if (p < 0.0) {
      if (r > t1) return false;
      if (r > t0) t0 = r;
    } else {
      if (r < t0) return false;
      if (r < t1) t1 = r;
    }
    return true;
  };
  if (!clip(-dx, x0 - r.left())) return std::nullopt;
  if (!clip(dx, r.right() - x0)) return std::nullopt;
  if (!clip(-dy, y0 - r.top())) return std::nullopt;
  if (!clip(dy, r.bottom() - y0)) return std::nullopt;
  if (t1 < t0) return std::nullopt;

  const auto lerp = [&](double t) {
    ProjectedPoint p;
    p.screen = QPointF(x0 + dx * t, y0 + dy * t);
    p.depth = a.depth + (b.depth - a.depth) * t;
    return p;
  };
  return std::pair<ProjectedPoint, ProjectedPoint>{lerp(t0), lerp(t1)};
}

std::vector<ProjectedPoint> clipTriangleToRect(const ProjectedPoint& a,
                                               const ProjectedPoint& b,
                                               const ProjectedPoint& c,
                                               const QRectF& rect) {
  const QRectF r = rect.normalized();
  std::vector<ProjectedPoint> polygon{a, b, c};

  // Sutherland-Hodgman convex clip against one half-plane. The crossing point
  // between an inside and an outside vertex interpolates both screen position
  // and depth (depth is affine over the projected triangle).
  const auto clipHalfPlane = [](std::vector<ProjectedPoint>& poly,
                                const auto& inside, const auto& crossing) {
    if (poly.size() < 1) return;
    std::vector<ProjectedPoint> clipped;
    clipped.reserve(poly.size() + 1);
    ProjectedPoint previous = poly.back();
    bool previousInside = inside(previous);
    for (const ProjectedPoint& current : poly) {
      const bool currentInside = inside(current);
      if (currentInside != previousInside)
        clipped.push_back(crossing(previous, current));
      if (currentInside) clipped.push_back(current);
      previous = current;
      previousInside = currentInside;
    }
    poly = std::move(clipped);
  };

  const auto crossX = [](const ProjectedPoint& p, const ProjectedPoint& q,
                         double x) {
    const double dx = q.screen.x() - p.screen.x();
    if (std::abs(dx) < 1e-12)
      return ProjectedPoint{QPointF(x, p.screen.y()), p.depth};
    const double t = (x - p.screen.x()) / dx;
    return ProjectedPoint{
        QPointF(x, p.screen.y() + (q.screen.y() - p.screen.y()) * t),
        p.depth + (q.depth - p.depth) * t};
  };
  const auto crossY = [](const ProjectedPoint& p, const ProjectedPoint& q,
                         double y) {
    const double dy = q.screen.y() - p.screen.y();
    if (std::abs(dy) < 1e-12)
      return ProjectedPoint{QPointF(p.screen.x(), y), p.depth};
    const double t = (y - p.screen.y()) / dy;
    return ProjectedPoint{
        QPointF(p.screen.x() + (q.screen.x() - p.screen.x()) * t, y),
        p.depth + (q.depth - p.depth) * t};
  };

  clipHalfPlane(polygon,
                [&](const ProjectedPoint& p) { return p.screen.x() >= r.left(); },
                [&](const ProjectedPoint& p, const ProjectedPoint& q) {
                  return crossX(p, q, r.left());
                });
  clipHalfPlane(polygon,
                [&](const ProjectedPoint& p) { return p.screen.x() <= r.right(); },
                [&](const ProjectedPoint& p, const ProjectedPoint& q) {
                  return crossX(p, q, r.right());
                });
  clipHalfPlane(polygon,
                [&](const ProjectedPoint& p) { return p.screen.y() >= r.top(); },
                [&](const ProjectedPoint& p, const ProjectedPoint& q) {
                  return crossY(p, q, r.top());
                });
  clipHalfPlane(polygon,
                [&](const ProjectedPoint& p) { return p.screen.y() <= r.bottom(); },
                [&](const ProjectedPoint& p, const ProjectedPoint& q) {
                  return crossY(p, q, r.bottom());
                });
  return polygon;
}

std::vector<std::size_t> collectFacesInRect(
    const std::vector<ProjectedTriangle>& triangles, const QRectF& rect,
    double depthEpsilon) {
  std::vector<std::size_t> result;
  for (const auto& triangle : triangles) {
    if (!triangleIntersectsRect(triangle.a, triangle.b, triangle.c, rect))
      continue;
    // Frontmost must be evaluated INSIDE the rectangle. Clip the triangle to
    // rect and sample at the clipped-polygon centroid (guaranteed inside the
    // clipped region); the raw triangle centroid can lie outside the rect and
    // sample unrelated/occluding geometry.
    const auto clipped =
        clipTriangleToRect(triangle.a, triangle.b, triangle.c, rect);
    if (clipped.size() < 3) continue;  // degenerate or empty clip
    QPointF sample;
    double sampleDepth = 0.0;
    for (const auto& vertex : clipped) {
      sample += vertex.screen;
      sampleDepth += vertex.depth;
    }
    sample /= static_cast<double>(clipped.size());
    sampleDepth /= static_cast<double>(clipped.size());
    bool frontmost = true;
    for (const auto& other : triangles) {
      if (other.faceIndex == triangle.faceIndex) continue;
      const auto otherDepth =
          triangleDepthAt(sample, other.a, other.b, other.c);
      if (otherDepth && *otherDepth > sampleDepth + depthEpsilon) {
        frontmost = false;
        break;
      }
    }
    if (!frontmost) continue;
    if (std::find(result.begin(), result.end(), triangle.faceIndex) ==
        result.end())
      result.push_back(triangle.faceIndex);
  }
  return result;
}

std::vector<std::size_t> collectEdgesInRect(
    const std::vector<ProjectedTriangle>& triangles,
    const std::vector<ProjectedEdge>& edges, const QRectF& rect,
    double depthEpsilon) {
  std::vector<std::size_t> result;
  for (const auto& edge : edges) {
    for (std::size_t index = 1; index < edge.points.size(); ++index) {
      const auto& a = edge.points[index - 1];
      const auto& b = edge.points[index];
      if (!segmentIntersectsRect(a, b, rect)) continue;
      // Occlusion must be evaluated INSIDE the rectangle. Clip the segment to
      // rect and sample at the clipped-segment midpoint (guaranteed inside the
      // clipped region); the raw segment midpoint can lie outside the rect and
      // sample unrelated/occluding geometry.
      const auto clipped = clipSegmentToRect(a, b, rect);
      if (!clipped) continue;
      const QPointF midpoint =
          (clipped->first.screen + clipped->second.screen) * 0.5;
      const double edgeDepth =
          (clipped->first.depth + clipped->second.depth) * 0.5;
      double surfaceDepth = -std::numeric_limits<double>::max();
      for (const auto& triangle : triangles) {
        if (const auto depth =
                triangleDepthAt(midpoint, triangle.a, triangle.b, triangle.c))
          surfaceDepth = std::max(surfaceDepth, *depth);
      }
      if (edgeDepth + depthEpsilon < surfaceDepth) continue;
      if (std::find(result.begin(), result.end(), edge.edgeIndex) ==
          result.end())
        result.push_back(edge.edgeIndex);
      break;
    }
  }
  return result;
}

}  // namespace solidar
