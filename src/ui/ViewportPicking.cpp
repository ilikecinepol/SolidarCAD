#include "ui/ViewportPicking.h"

#include <QLineF>

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <unordered_set>

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

double triangleMaximumDepth(const ProjectedPoint& a,
                            const ProjectedPoint& b,
                            const ProjectedPoint& c) {
  if (!std::isfinite(a.depth) || !std::isfinite(b.depth) ||
      !std::isfinite(c.depth))
    return std::numeric_limits<double>::infinity();
  return std::max({a.depth, b.depth, c.depth});
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
                                                     QPointF vPoint,
                                                     double minimumDeg,
                                                     double maximumDeg) {
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
  const bool signedRange = minimumDeg < 0.0 && maximumDeg > 0.0;
  if (!signedRange && angle <= 0.0) angle += 360.0;
  return std::clamp(angle, minimumDeg, maximumDeg);
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

namespace {

struct Bounds2d {
  double left{};
  double top{};
  double right{};
  double bottom{};
};

Bounds2d pointBounds(QPointF point, double radius = 1e-7) {
  return {point.x() - radius, point.y() - radius,
          point.x() + radius, point.y() + radius};
}

Bounds2d rectBounds(const QRectF& value) {
  const QRectF rect = value.normalized();
  return {rect.left(), rect.top(), rect.right(), rect.bottom()};
}

Bounds2d triangleBounds(const ProjectedPoint& a, const ProjectedPoint& b,
                        const ProjectedPoint& c) {
  return {std::min({a.screen.x(), b.screen.x(), c.screen.x()}),
          std::min({a.screen.y(), b.screen.y(), c.screen.y()}),
          std::max({a.screen.x(), b.screen.x(), c.screen.x()}),
          std::max({a.screen.y(), b.screen.y(), c.screen.y()})};
}

Bounds2d segmentBounds(const ProjectedPoint& a, const ProjectedPoint& b) {
  return {std::min(a.screen.x(), b.screen.x()),
          std::min(a.screen.y(), b.screen.y()),
          std::max(a.screen.x(), b.screen.x()),
          std::max(a.screen.y(), b.screen.y())};
}

Bounds2d merged(Bounds2d a, Bounds2d b) {
  return {std::min(a.left, b.left), std::min(a.top, b.top),
          std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

bool overlaps(Bounds2d a, Bounds2d b) {
  return a.left <= b.right && a.right >= b.left && a.top <= b.bottom &&
         a.bottom >= b.top;
}

class AabbBvh final {
 public:
  void build(std::vector<Bounds2d> boxes,
             std::vector<double> maximumDepths = {}) {
    boxes_ = std::move(boxes);
    maximumDepths_ = std::move(maximumDepths);
    if (maximumDepths_.size() != boxes_.size())
      maximumDepths_.assign(boxes_.size(),
                            std::numeric_limits<double>::infinity());
    order_.resize(boxes_.size());
    std::iota(order_.begin(), order_.end(), std::size_t{});
    nodes_.clear();
    if (!order_.empty()) {
      nodes_.reserve(order_.size() * 2);
      buildNode(0, order_.size());
    }
  }

  template <typename Callback>
  void query(Bounds2d queryBounds, Callback&& callback,
             std::uint64_t* nodeVisits = nullptr) const {
    const auto keepGoing = [&](std::size_t item) {
      callback(item);
      return true;
    };
    static_cast<void>(queryWhile(queryBounds, keepGoing, nodeVisits));
  }

  template <typename Callback>
  bool queryWhile(Bounds2d queryBounds, Callback&& callback,
                  std::uint64_t* nodeVisits = nullptr,
                  double minimumExclusiveDepth =
                      -std::numeric_limits<double>::infinity()) const {
    if (nodes_.empty()) return true;
    return queryNodeWhile(0, queryBounds, callback, nodeVisits,
                          minimumExclusiveDepth);
  }

  [[nodiscard]] std::size_t ownedBytes() const noexcept {
    return boxes_.capacity() * sizeof(Bounds2d) +
           maximumDepths_.capacity() * sizeof(double) +
           order_.capacity() * sizeof(std::size_t) +
           nodes_.capacity() * sizeof(Node);
  }

 private:
  struct Node {
    Bounds2d bounds;
    std::size_t begin{};
    std::size_t end{};
    std::size_t left{static_cast<std::size_t>(-1)};
    std::size_t right{static_cast<std::size_t>(-1)};
    double maximumDepth{-std::numeric_limits<double>::infinity()};
  };

  std::size_t buildNode(std::size_t begin, std::size_t end) {
    const std::size_t nodeIndex = nodes_.size();
    nodes_.push_back({});
    Bounds2d bounds = boxes_[order_[begin]];
    for (std::size_t index = begin + 1; index < end; ++index)
      bounds = merged(bounds, boxes_[order_[index]]);
    nodes_[nodeIndex].bounds = bounds;
    nodes_[nodeIndex].begin = begin;
    nodes_[nodeIndex].end = end;
    for (std::size_t index = begin; index < end; ++index)
      nodes_[nodeIndex].maximumDepth =
          std::max(nodes_[nodeIndex].maximumDepth,
                   maximumDepths_[order_[index]]);
    if (end - begin <= 8) return nodeIndex;
    const bool splitX = bounds.right - bounds.left >= bounds.bottom - bounds.top;
    const std::size_t middle = begin + (end - begin) / 2;
    std::nth_element(order_.begin() + static_cast<std::ptrdiff_t>(begin),
                     order_.begin() + static_cast<std::ptrdiff_t>(middle),
                     order_.begin() + static_cast<std::ptrdiff_t>(end),
                     [&](std::size_t lhs, std::size_t rhs) {
                       const Bounds2d a = boxes_[lhs];
                       const Bounds2d b = boxes_[rhs];
                       return splitX ? a.left + a.right < b.left + b.right
                                     : a.top + a.bottom < b.top + b.bottom;
                     });
    const std::size_t left = buildNode(begin, middle);
    const std::size_t right = buildNode(middle, end);
    nodes_[nodeIndex].left = left;
    nodes_[nodeIndex].right = right;
    return nodeIndex;
  }

  template <typename Callback>
  bool queryNodeWhile(std::size_t nodeIndex, Bounds2d queryBounds,
                      Callback& callback, std::uint64_t* nodeVisits,
                      double minimumExclusiveDepth) const {
    if (nodeVisits) ++*nodeVisits;
    const Node& node = nodes_[nodeIndex];
    if (!overlaps(node.bounds, queryBounds) ||
        node.maximumDepth <= minimumExclusiveDepth)
      return true;
    if (node.left == static_cast<std::size_t>(-1)) {
      for (std::size_t index = node.begin; index < node.end; ++index) {
        const std::size_t item = order_[index];
        if (overlaps(boxes_[item], queryBounds) &&
            maximumDepths_[item] > minimumExclusiveDepth && !callback(item))
          return false;
      }
      return true;
    }
    if (!queryNodeWhile(node.left, queryBounds, callback, nodeVisits,
                        minimumExclusiveDepth))
      return false;
    return queryNodeWhile(node.right, queryBounds, callback, nodeVisits,
                          minimumExclusiveDepth);
  }

  std::vector<Bounds2d> boxes_;
  std::vector<double> maximumDepths_;
  std::vector<std::size_t> order_;
  std::vector<Node> nodes_;
};

constexpr std::size_t kMaximumOccludersPerPrimitive = 4096;
constexpr std::size_t kMaximumVisibilityWorkPerQuery = 100000;
constexpr std::size_t kMaximumInteractiveVertexCandidates = 512;
constexpr int kMaximumVisibilitySubdivisionDepth = 9;
constexpr int kMaximumExactVisibilitySubdivisionDepth = 12;

struct VisibilityBudget {
  std::size_t remaining{kMaximumVisibilityWorkPerQuery};
  std::size_t candidatesPerCell{kMaximumOccludersPerPrimitive};
  int maximumSubdivisionDepth{kMaximumVisibilitySubdivisionDepth};
  bool exhausted{};
  bool exactMode{};

  [[nodiscard]] static VisibilityBudget exact() noexcept {
    return {std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max(),
            kMaximumExactVisibilitySubdivisionDepth, false, true};
  }

  [[nodiscard]] bool consume() noexcept {
    if (remaining == 0) {
      exhausted = true;
      return false;
    }
    if (remaining != std::numeric_limits<std::size_t>::max()) --remaining;
    return true;
  }
};

struct ParamInterval {
  double begin{};
  double end{1.0};
  bool includeBegin{true};
  bool includeEnd{true};
};

bool finite(ProjectedPoint point);

std::optional<ParamInterval> segmentDiskInterval(QPointF cursor,
                                                 const ProjectedPoint& a,
                                                 const ProjectedPoint& b,
                                                 double radius) {
  if (!finite(a) || !finite(b) || !std::isfinite(cursor.x()) ||
      !std::isfinite(cursor.y()) || !std::isfinite(radius) || radius < 0.0)
    return std::nullopt;
  const QPointF delta = b.screen - a.screen;
  const QPointF fromCenter = a.screen - cursor;
  const double aa = QPointF::dotProduct(delta, delta);
  const double cc = QPointF::dotProduct(fromCenter, fromCenter) -
                    radius * radius;
  if (aa <= 1e-15)
    return cc <= 0.0 ? std::optional<ParamInterval>(ParamInterval{})
                     : std::nullopt;
  const double bb = 2.0 * QPointF::dotProduct(fromCenter, delta);
  const double discriminant = bb * bb - 4.0 * aa * cc;
  if (!std::isfinite(discriminant) || discriminant < 0.0)
    return std::nullopt;
  const double root = std::sqrt(std::max(0.0, discriminant));
  ParamInterval interval{std::max(0.0, (-bb - root) / (2.0 * aa)),
                         std::min(1.0, (-bb + root) / (2.0 * aa)), true,
                         true};
  if (interval.end < interval.begin) return std::nullopt;
  return interval;
}

ProjectedPoint interpolateProjected(const ProjectedPoint& a,
                                    const ProjectedPoint& b, double t) {
  return {a.screen + (b.screen - a.screen) * t,
          a.depth + (b.depth - a.depth) * t};
}

bool finite(ProjectedPoint point) {
  return std::isfinite(point.screen.x()) && std::isfinite(point.screen.y()) &&
         std::isfinite(point.depth);
}

double strictDepthGuard(double epsilon) {
  return std::max(1e-12, std::abs(epsilon) * 1e-6);
}

bool intersectClosedLowerBound(ParamInterval& interval, double atBegin,
                               double atEnd, double lowerBound) {
  const double delta = atEnd - atBegin;
  if (!std::isfinite(atBegin) || !std::isfinite(atEnd)) return false;
  if (std::abs(delta) < 1e-15) return atBegin >= lowerBound;
  const double crossing = (lowerBound - atBegin) / delta;
  if (delta > 0.0) {
    if (crossing > interval.end) return false;
    if (crossing > interval.begin) {
      interval.begin = crossing;
      interval.includeBegin = true;
    }
  } else {
    if (crossing < interval.begin) return false;
    if (crossing < interval.end) {
      interval.end = crossing;
      interval.includeEnd = true;
    }
  }
  return interval.begin <= interval.end;
}

bool intersectStrictLowerBound(ParamInterval& interval, double atBegin,
                               double atEnd, double lowerBound) {
  const double delta = atEnd - atBegin;
  if (!std::isfinite(atBegin) || !std::isfinite(atEnd)) return false;
  if (std::abs(delta) < 1e-15) return atBegin > lowerBound;
  const double crossing = (lowerBound - atBegin) / delta;
  if (delta > 0.0) {
    if (crossing >= interval.end) return false;
    if (crossing >= interval.begin) {
      interval.begin = crossing;
      interval.includeBegin = false;
    }
  } else {
    if (crossing <= interval.begin) return false;
    if (crossing <= interval.end) {
      interval.end = crossing;
      interval.includeEnd = false;
    }
  }
  return interval.begin < interval.end ||
         (interval.begin == interval.end && interval.includeBegin &&
          interval.includeEnd);
}

std::optional<std::array<double, 3>> barycentricAt(
    QPointF point, const ProjectedTriangle& triangle) {
  const QPointF v0 = triangle.b.screen - triangle.a.screen;
  const QPointF v1 = triangle.c.screen - triangle.a.screen;
  const QPointF v2 = point - triangle.a.screen;
  const double denominator = v0.x() * v1.y() - v1.x() * v0.y();
  if (!std::isfinite(denominator) || std::abs(denominator) < 1e-12)
    return std::nullopt;
  const double u = (v2.x() * v1.y() - v1.x() * v2.y()) / denominator;
  const double v = (v0.x() * v2.y() - v2.x() * v0.y()) / denominator;
  const double w = 1.0 - u - v;
  if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(w))
    return std::nullopt;
  return std::array<double, 3>{w, u, v};
}

double planeDepth(const ProjectedTriangle& triangle,
                  const std::array<double, 3>& weights) {
  return triangle.a.depth * weights[0] + triangle.b.depth * weights[1] +
         triangle.c.depth * weights[2];
}

std::optional<ParamInterval> strictOcclusionInterval(
    const ProjectedPoint& a, const ProjectedPoint& b,
    double candidateEpsilon, const ProjectedTriangle& occluder,
    PickingQueryCounters* counters) {
  if (counters) ++counters->intervalTests;
  if (!finite(a) || !finite(b) || !finite(occluder.a) ||
      !finite(occluder.b) || !finite(occluder.c))
    return std::nullopt;
  const auto weightsA = barycentricAt(a.screen, occluder);
  const auto weightsB = barycentricAt(b.screen, occluder);
  if (!weightsA || !weightsB) return std::nullopt;
  ParamInterval interval;
  constexpr double kInsideTolerance = -1e-10;
  for (std::size_t coordinate = 0; coordinate < 3; ++coordinate)
    if (!intersectClosedLowerBound(interval, (*weightsA)[coordinate],
                                   (*weightsB)[coordinate],
                                   kInsideTolerance))
      return std::nullopt;
  const double differenceA = planeDepth(occluder, *weightsA) - a.depth;
  const double differenceB = planeDepth(occluder, *weightsB) - b.depth;
  const double epsilon =
      std::max(candidateEpsilon, occluder.depthEpsilon);
  if (!intersectStrictLowerBound(interval, differenceA, differenceB,
                                 epsilon + strictDepthGuard(epsilon)))
    return std::nullopt;
  interval.begin = std::clamp(interval.begin, 0.0, 1.0);
  interval.end = std::clamp(interval.end, 0.0, 1.0);
  return interval;
}

void insertMergedInterval(std::vector<ParamInterval>& values,
                          ParamInterval value) {
  if (value.end < value.begin) return;
  auto first = values.begin();
  while (first != values.end() && first->end < value.begin)
    ++first;
  while (first != values.end()) {
    const bool separated = value.end < first->begin;
    if (separated) break;
    if (first->begin < value.begin) {
      value.begin = first->begin;
      value.includeBegin = first->includeBegin;
    } else if (first->begin == value.begin) {
      value.includeBegin = value.includeBegin || first->includeBegin;
    }
    if (first->end > value.end) {
      value.end = first->end;
      value.includeEnd = first->includeEnd;
    } else if (first->end == value.end) {
      value.includeEnd = value.includeEnd || first->includeEnd;
    }
    first = values.erase(first);
  }
  values.insert(first, value);
}

bool intervalsCoverUnit(const std::vector<ParamInterval>& intervals) {
  return intervals.size() == 1 && intervals.front().begin <= 0.0 &&
         intervals.front().end >= 1.0;
}

struct HiddenIntervalsResult {
  std::vector<ParamInterval> intervals;
  bool complete{true};
  bool fullyCovered{};
};

template <typename TriangleAt>
HiddenIntervalsResult hiddenIntervals(
    const AabbBvh& triangleBvh, const ProjectedPoint& a,
    const ProjectedPoint& b, double candidateEpsilon,
    TriangleAt&& triangleAt, PickingQueryCounters* counters,
    VisibilityBudget& budget,
    std::optional<std::size_t> ignoredFace = std::nullopt) {
  HiddenIntervalsResult result;
  std::size_t candidateCount = 0;
  const double minimumDepth = std::min(a.depth, b.depth) + candidateEpsilon;
  const auto visit = [&](std::size_t index) {
    const ProjectedTriangle triangle = triangleAt(index);
    if (ignoredFace && triangle.faceIndex == *ignoredFace) return true;
    if (++candidateCount > budget.candidatesPerCell || !budget.consume()) {
      budget.exhausted = true;
      return false;
    }
    if (counters) {
      ++counters->triangleCandidates;
      ++counters->occluderTests;
    }
    if (auto interval = strictOcclusionInterval(
            a, b, candidateEpsilon, triangle, counters)) {
      insertMergedInterval(result.intervals, *interval);
      if (intervalsCoverUnit(result.intervals)) {
        result.fullyCovered = true;
        return false;
      }
    }
    return true;
  };
  const bool complete = triangleBvh.queryWhile(
      segmentBounds(a, b), visit, counters ? &counters->nodeVisits : nullptr,
      minimumDepth);
  if (!complete && !result.fullyCovered) {
    result.complete = false;
    if (counters) ++counters->uncertainVisible;
  }
  return result;
}

std::optional<double> closestVisibleParameter(
    QPointF cursor, const ProjectedPoint& a, const ProjectedPoint& b,
    const std::vector<ParamInterval>& hidden) {
  const QPointF delta = b.screen - a.screen;
  const double lengthSquared = QPointF::dotProduct(delta, delta);
  const double preferred = lengthSquared > 1e-15
      ? std::clamp(QPointF::dotProduct(cursor - a.screen, delta) /
                       lengthSquared,
                   0.0, 1.0)
      : 0.0;
  double bestParameter = 0.0;
  double bestDistance = std::numeric_limits<double>::max();
  double cursorBegin = 0.0;
  bool cursorBeginIncluded = true;
  const auto consider = [&](double begin, bool includeBegin, double end,
                            bool includeEnd, double& bestT,
                            double& bestDistanceValue) {
    if (begin > end ||
        (begin == end && (!includeBegin || !includeEnd)))
      return;
    double candidate = std::clamp(preferred, begin, end);
    if (candidate == begin && !includeBegin)
      candidate = std::nextafter(begin, end);
    if (candidate == end && !includeEnd)
      candidate = std::nextafter(end, begin);
    if (candidate < begin || candidate > end) return;
    const double distance =
        QLineF(cursor, a.screen + delta * candidate).length();
    if (distance < bestDistanceValue) {
      bestDistanceValue = distance;
      bestT = candidate;
    }
  };
  for (const auto& interval : hidden) {
    consider(cursorBegin, cursorBeginIncluded, interval.begin,
             !interval.includeBegin, bestParameter, bestDistance);
    cursorBegin = interval.end;
    cursorBeginIncluded = !interval.includeEnd;
  }
  consider(cursorBegin, cursorBeginIncluded, 1.0, true, bestParameter,
           bestDistance);
  if (!std::isfinite(bestDistance)) return std::nullopt;
  return bestParameter;
}

bool cellCertifiedHidden(const std::array<ProjectedPoint, 3>& cell,
                         double candidateEpsilon,
                         const ProjectedTriangle& occluder) {
  const double epsilon =
      std::max(candidateEpsilon, occluder.depthEpsilon);
  const double required = epsilon + strictDepthGuard(epsilon);
  for (const auto& vertex : cell) {
    if (!finite(vertex)) return false;
    const auto depth = triangleDepthAt(vertex.screen, occluder.a, occluder.b,
                                       occluder.c);
    if (!depth || !std::isfinite(*depth) ||
        !(*depth > vertex.depth + required))
      return false;
  }
  return true;
}

ProjectedPoint midpoint(ProjectedPoint a, ProjectedPoint b) {
  return {(a.screen + b.screen) * 0.5, (a.depth + b.depth) * 0.5};
}

enum class VisibilityConclusion { Visible, Hidden, Unresolved };

void normalizePolygon(std::vector<ProjectedPoint>& polygon) {
  const auto samePoint = [](const ProjectedPoint& a, const ProjectedPoint& b) {
    return std::abs(a.screen.x() - b.screen.x()) <= 1e-10 &&
           std::abs(a.screen.y() - b.screen.y()) <= 1e-10 &&
           std::abs(a.depth - b.depth) <= 1e-12;
  };
  std::vector<ProjectedPoint> normalized;
  normalized.reserve(polygon.size());
  for (const auto& point : polygon)
    if (normalized.empty() || !samePoint(normalized.back(), point))
      normalized.push_back(point);
  if (normalized.size() > 1 && samePoint(normalized.front(), normalized.back()))
    normalized.pop_back();
  polygon = std::move(normalized);
}

template <typename Evaluate>
std::pair<std::vector<ProjectedPoint>, std::vector<ProjectedPoint>>
splitConvexPolygon(const std::vector<ProjectedPoint>& polygon,
                   Evaluate&& evaluate, bool boundaryIsInside) {
  std::vector<ProjectedPoint> inside;
  std::vector<ProjectedPoint> outside;
  if (polygon.empty()) return {inside, outside};
  inside.reserve(polygon.size() + 1);
  outside.reserve(polygon.size() + 1);
  ProjectedPoint previous = polygon.back();
  double previousValue = evaluate(previous);
  auto classifiedInside = [boundaryIsInside](double value) {
    return value > 0.0 || (boundaryIsInside && value == 0.0);
  };
  bool previousInside = classifiedInside(previousValue);
  for (const auto& current : polygon) {
    const double currentValue = evaluate(current);
    const bool currentInside = classifiedInside(currentValue);
    if (currentInside != previousInside) {
      const double denominator = previousValue - currentValue;
      const double parameter = std::abs(denominator) > 1e-20
          ? std::clamp(previousValue / denominator, 0.0, 1.0)
          : 0.5;
      const ProjectedPoint crossing =
          interpolateProjected(previous, current, parameter);
      inside.push_back(crossing);
      outside.push_back(crossing);
    }
    (currentInside ? inside : outside).push_back(current);
    previous = current;
    previousValue = currentValue;
    previousInside = currentInside;
  }
  normalizePolygon(inside);
  normalizePolygon(outside);
  return {std::move(inside), std::move(outside)};
}

std::vector<std::vector<ProjectedPoint>> subtractOccluder(
    const std::vector<ProjectedPoint>& polygon,
    const ProjectedTriangle& occluder, double candidateEpsilon) {
  std::vector<std::vector<ProjectedPoint>> visible;
  std::vector<ProjectedPoint> hiddenCandidate = polygon;
  const QPointF v0 = occluder.b.screen - occluder.a.screen;
  const QPointF v1 = occluder.c.screen - occluder.a.screen;
  const double denominator = v0.x() * v1.y() - v1.x() * v0.y();
  if (!std::isfinite(denominator) || std::abs(denominator) < 1e-12) {
    visible.push_back(polygon);
    return visible;
  }
  constexpr double kInsideTolerance = -1e-10;
  const auto weight = [&](const ProjectedPoint& point, std::size_t coordinate) {
    const QPointF v2 = point.screen - occluder.a.screen;
    const double u = (v2.x() * v1.y() - v1.x() * v2.y()) / denominator;
    const double v = (v0.x() * v2.y() - v2.x() * v0.y()) / denominator;
    const std::array<double, 3> weights{1.0 - u - v, u, v};
    return weights[coordinate] - kInsideTolerance;
  };
  for (std::size_t coordinate = 0; coordinate < 3; ++coordinate) {
    auto [inside, outside] = splitConvexPolygon(
        hiddenCandidate,
        [&](const ProjectedPoint& point) { return weight(point, coordinate); },
        true);
    if (outside.size() >= 3) visible.push_back(std::move(outside));
    hiddenCandidate = std::move(inside);
    if (hiddenCandidate.size() < 3) return visible;
  }
  const double epsilon = std::max(candidateEpsilon, occluder.depthEpsilon);
  const double required = epsilon + strictDepthGuard(epsilon);
  auto [hidden, depthVisible] = splitConvexPolygon(
      hiddenCandidate,
      [&](const ProjectedPoint& point) {
        const auto weights = barycentricAt(point.screen, occluder);
        return weights ? planeDepth(occluder, *weights) - point.depth - required
                       : -std::numeric_limits<double>::infinity();
      },
      false);
  if (depthVisible.size() >= 3) visible.push_back(std::move(depthVisible));
  // The final `hidden` polygon is the only portion removed by this occluder.
  static_cast<void>(hidden);
  return visible;
}

template <typename TriangleAt>
VisibilityConclusion exactPolygonVisibility(
    const std::vector<ProjectedPoint>& polygon, double candidateEpsilon,
    std::size_t ignoredFace, const AabbBvh& triangleBvh,
    TriangleAt&& triangleAt, PickingQueryCounters* counters,
    VisibilityBudget& budget) {
  std::vector<std::vector<ProjectedPoint>> visible{polygon};
  Bounds2d bounds = pointBounds(polygon.front().screen, 0.0);
  double minimumDepth = polygon.front().depth;
  for (const auto& point : polygon) {
    bounds = merged(bounds, pointBounds(point.screen, 0.0));
    minimumDepth = std::min(minimumDepth, point.depth);
  }
  const auto visit = [&](std::size_t index) {
    const auto triangle = triangleAt(index);
    if (triangle.faceIndex == ignoredFace) return true;
    static_cast<void>(budget.consume());
    if (counters) {
      ++counters->triangleCandidates;
      ++counters->occluderTests;
    }
    std::vector<std::vector<ProjectedPoint>> next;
    for (const auto& piece : visible) {
      auto difference = subtractOccluder(piece, triangle, candidateEpsilon);
      next.insert(next.end(), std::make_move_iterator(difference.begin()),
                  std::make_move_iterator(difference.end()));
    }
    visible = std::move(next);
    return !visible.empty();
  };
  const bool complete = triangleBvh.queryWhile(
      bounds, visit, counters ? &counters->nodeVisits : nullptr,
      minimumDepth + candidateEpsilon);
  if (visible.empty()) return VisibilityConclusion::Hidden;
  return complete ? VisibilityConclusion::Visible
                  : VisibilityConclusion::Unresolved;
}

template <typename TriangleAt>
VisibilityConclusion pointVisibility(
    const ProjectedPoint& point, double candidateEpsilon,
    std::size_t ignoredFace, const AabbBvh& triangleBvh,
    TriangleAt&& triangleAt, PickingQueryCounters* counters,
    VisibilityBudget& budget) {
  bool hidden = false;
  std::size_t candidateCount = 0;
  const auto visit = [&](std::size_t index) {
    const auto triangle = triangleAt(index);
    if (triangle.faceIndex == ignoredFace) return true;
    if (++candidateCount > budget.candidatesPerCell || !budget.consume()) {
      budget.exhausted = true;
      return false;
    }
    if (counters) {
      ++counters->triangleCandidates;
      ++counters->occluderTests;
    }
    const auto depth = triangleDepthAt(point.screen, triangle.a, triangle.b,
                                       triangle.c);
    const double epsilon = std::max(candidateEpsilon, triangle.depthEpsilon);
    if (depth && *depth > point.depth + epsilon + strictDepthGuard(epsilon)) {
      hidden = true;
      return false;
    }
    return true;
  };
  const bool complete = triangleBvh.queryWhile(
      pointBounds(point.screen), visit,
      counters ? &counters->nodeVisits : nullptr,
      point.depth + candidateEpsilon);
  if (hidden) return VisibilityConclusion::Hidden;
  if (!complete) return VisibilityConclusion::Unresolved;
  return VisibilityConclusion::Visible;
}

template <typename TriangleAt>
VisibilityConclusion cellVisibility(
    const std::array<ProjectedPoint, 3>& cell, double candidateEpsilon,
    std::size_t ignoredFace, const AabbBvh& triangleBvh,
    TriangleAt&& triangleAt, PickingQueryCounters* counters,
    VisibilityBudget& budget, int depth) {
  Bounds2d bounds = triangleBounds(cell[0], cell[1], cell[2]);
  const double minimumDepth =
      std::min({cell[0].depth, cell[1].depth, cell[2].depth});
  bool certifiedHidden = false;
  bool sawOccluder = false;
  std::size_t candidateCount = 0;
  const auto visit = [&](std::size_t index) {
    const auto triangle = triangleAt(index);
    if (triangle.faceIndex == ignoredFace) return true;
    sawOccluder = true;
    if (++candidateCount > budget.candidatesPerCell || !budget.consume()) {
      budget.exhausted = true;
      return false;
    }
    if (counters) {
      ++counters->triangleCandidates;
      ++counters->occluderTests;
    }
    if (cellCertifiedHidden(cell, candidateEpsilon, triangle)) {
      certifiedHidden = true;
      return false;
    }
    return true;
  };
  const bool complete = triangleBvh.queryWhile(
      bounds, visit, counters ? &counters->nodeVisits : nullptr,
      minimumDepth + candidateEpsilon);
  if (certifiedHidden) return VisibilityConclusion::Hidden;
  if (!complete) {
    if (counters) ++counters->uncertainVisible;
    return VisibilityConclusion::Unresolved;
  }
  if (!sawOccluder) return VisibilityConclusion::Visible;

  const ProjectedPoint center{
      (cell[0].screen + cell[1].screen + cell[2].screen) / 3.0,
      (cell[0].depth + cell[1].depth + cell[2].depth) / 3.0};
  const std::array<ProjectedPoint, 4> samples{
      cell[0], cell[1], cell[2], center};
  bool unresolvedSample = false;
  for (const auto& sample : samples) {
    const auto visibility = pointVisibility(
        sample, candidateEpsilon, ignoredFace, triangleBvh, triangleAt,
        counters, budget);
    if (visibility == VisibilityConclusion::Visible)
      return VisibilityConclusion::Visible;
    unresolvedSample = unresolvedSample ||
                       visibility == VisibilityConclusion::Unresolved;
  }
  if (unresolvedSample || depth >= budget.maximumSubdivisionDepth) {
    if (counters) ++counters->uncertainVisible;
    return VisibilityConclusion::Unresolved;
  }
  if (counters) ++counters->subdivisions;
  const ProjectedPoint ab = midpoint(cell[0], cell[1]);
  const ProjectedPoint bc = midpoint(cell[1], cell[2]);
  const ProjectedPoint ca = midpoint(cell[2], cell[0]);
  const std::array<std::array<ProjectedPoint, 3>, 4> children{{
      {cell[0], ab, ca}, {ab, cell[1], bc}, {ca, bc, cell[2]}, {ab, bc, ca}}};
  bool unresolvedChild = false;
  for (const auto& child : children) {
    const auto visibility = cellVisibility(
        child, candidateEpsilon, ignoredFace, triangleBvh, triangleAt,
        counters, budget, depth + 1);
    if (visibility == VisibilityConclusion::Visible)
      return VisibilityConclusion::Visible;
    unresolvedChild = unresolvedChild ||
                      visibility == VisibilityConclusion::Unresolved;
  }
  return unresolvedChild ? VisibilityConclusion::Unresolved
                         : VisibilityConclusion::Hidden;
}

template <typename TriangleAt>
VisibilityConclusion polygonVisibility(
    const std::vector<ProjectedPoint>& polygon, double candidateEpsilon,
    std::size_t ignoredFace, const AabbBvh& triangleBvh,
    TriangleAt&& triangleAt, PickingQueryCounters* counters,
    VisibilityBudget& budget) {
  if (polygon.size() < 3) return VisibilityConclusion::Visible;
  for (const auto& point : polygon) {
    if (!finite(point)) return VisibilityConclusion::Visible;
  }
  if (budget.exactMode)
    return exactPolygonVisibility(polygon, candidateEpsilon, ignoredFace,
                                  triangleBvh, triangleAt, counters, budget);
  bool unresolved = false;
  for (std::size_t index = 1; index + 1 < polygon.size(); ++index) {
    const std::array<ProjectedPoint, 3> cell{
        polygon[0], polygon[index], polygon[index + 1]};
    const auto visibility = cellVisibility(
        cell, candidateEpsilon, ignoredFace, triangleBvh, triangleAt,
        counters, budget, 0);
    if (visibility == VisibilityConclusion::Visible)
      return VisibilityConclusion::Visible;
    unresolved = unresolved || visibility == VisibilityConclusion::Unresolved;
  }
  return unresolved ? VisibilityConclusion::Unresolved
                    : VisibilityConclusion::Hidden;
}

Point3d interpolatePoint(Point3d a, Point3d b, double t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
          a.z + (b.z - a.z) * t};
}

}  // namespace

struct ProjectedPickingScene::Impl {
  struct MeshKey {
    const BodyRenderMesh* mesh{};
    std::uint64_t revision{};
    BodyMeshKey identity;
    std::size_t faceOffset{};
    std::size_t edgeOffset{};
    bool operator==(const MeshKey&) const noexcept = default;
  };
  struct CameraKey {
    float yaw{};
    float pitch{};
    float zoom{};
    QSize size;
    Point3d center{};
    double depthExtent{};
    bool operator==(const CameraKey& other) const noexcept {
      return yaw == other.yaw && pitch == other.pitch && zoom == other.zoom &&
             size == other.size && center.x == other.center.x &&
             center.y == other.center.y && center.z == other.center.z &&
             depthExtent == other.depthExtent;
    }
  };
  struct VertexRef {
    ProjectedPoint projected;
    std::size_t mesh{};
    std::size_t local{};
  };
  struct TriangleRef {
    std::array<std::size_t, 3> vertices{};
    std::size_t face{};
    std::size_t mesh{};
  };
  struct EdgePointRef {
    ProjectedPoint projected;
    std::size_t mesh{};
    std::size_t edge{};
    std::size_t local{};
  };
  struct SegmentRef {
    std::size_t a{};
    std::size_t b{};
    std::size_t edge{};
  };

  std::vector<MeshKey> keyMeshes;
  CameraKey cameraKey;
  std::vector<PickingMeshInput> meshes;
  std::vector<double> meshDepthEpsilons;
  std::vector<VertexRef> vertices;
  std::vector<TriangleRef> triangles;
  std::vector<EdgePointRef> edgePoints;
  std::vector<SegmentRef> segments;
  AabbBvh triangleBvh;
  AabbBvh segmentBvh;
  AabbBvh vertexBvh;
  double epsilon{};
  mutable PickingQueryCounters counters;

  [[nodiscard]] ProjectedTriangle triangleAt(std::size_t index) const {
    const auto& triangle = triangles[index];
    return {vertices[triangle.vertices[0]].projected,
            vertices[triangle.vertices[1]].projected,
            vertices[triangle.vertices[2]].projected, triangle.face,
            meshDepthEpsilons[triangle.mesh]};
  }

  [[nodiscard]] double segmentDepthEpsilon(std::size_t index) const {
    return meshDepthEpsilons[edgePoints[segments[index].a].mesh];
  }

  [[nodiscard]] Point3d worldVertex(std::size_t index) const {
    const auto& ref = vertices[index];
    return meshes[ref.mesh].mesh->vertices()[ref.local].position;
  }

  [[nodiscard]] Point3d worldEdgePoint(std::size_t index) const {
    const auto& ref = edgePoints[index];
    return meshes[ref.mesh].mesh->edges()[ref.edge].points[ref.local];
  }

};

bool ProjectedPickingScene::ensure(const std::vector<PickingMeshInput>& meshes,
                                   const ViewportCameraState& camera) {
  const auto fail = [this]() {
    if (impl_) ++impl_->counters.buildFailures;
    return false;
  };
  try {
    std::vector<Impl::MeshKey> keys;
    keys.reserve(meshes.size());
    for (const auto& input : meshes) {
      if (!input.mesh) continue;
      keys.push_back({input.mesh, input.mesh->revision(), input.identity,
                      input.faceOffset, input.edgeOffset});
    }
    const Impl::CameraKey cameraKey{camera.yawDeg, camera.pitchDeg,
                                    camera.zoom, camera.logicalSize,
                                    camera.center, camera.depthExtent};
    if (impl_ && keys == impl_->keyMeshes && cameraKey == impl_->cameraKey)
      return false;

    // Build into an unpublished scene. Invalid geometry or any allocation/
    // projection exception leaves the last valid scene and cache key intact.
    auto pending = std::make_shared<Impl>();
    pending->keyMeshes = std::move(keys);
    pending->cameraKey = cameraKey;
    if (impl_) pending->counters = impl_->counters;
    std::vector<Bounds2d> triangleBoxes;
    std::vector<double> triangleMaximumDepths;
    std::vector<Bounds2d> segmentBoxes;
    std::vector<Bounds2d> vertexBoxes;
    std::vector<double> vertexDepths;

    ViewportCameraState projectionCamera = camera;
    projectionCamera.pan = {};
    const QMatrix4x4 matrix = projectionCamera.worldToClip();
    const double yaw = projectionCamera.yawDeg * std::numbers::pi / 180.0;
    const double pitch = projectionCamera.pitchDeg * std::numbers::pi / 180.0;
    const double sinYaw = std::sin(yaw);
    const double cosYaw = std::cos(yaw);
    const double sinPitch = std::sin(pitch);
    const double cosPitch = std::cos(pitch);
    const auto project = [&](Point3d point) {
      const QVector4D clip =
          matrix * QVector4D(point.x, point.y, point.z, 1.0F);
      return ProjectedPoint{
          {(clip.x() + 1.0) * projectionCamera.logicalSize.width() * 0.5,
           (1.0 - clip.y()) * projectionCamera.logicalSize.height() * 0.5},
          (point.x * sinYaw + point.y * cosYaw) * sinPitch +
              point.z * cosPitch};
    };

    for (const auto& input : meshes) {
      if (!input.mesh) continue;
      if (!std::isfinite(input.mesh->diagonal()) ||
          input.mesh->triangleIndices().size() % 3 != 0)
        return fail();
      const std::size_t meshIndex = pending->meshes.size();
      pending->meshes.push_back(input);
      const double meshDepthEpsilon =
          std::max(0.0, input.mesh->diagonal() * kDepthEpsilonScale);
      pending->meshDepthEpsilons.push_back(meshDepthEpsilon);
      pending->epsilon = std::max(pending->epsilon, meshDepthEpsilon);
      const std::size_t firstVertex = pending->vertices.size();
      pending->vertices.reserve(firstVertex + input.mesh->vertices().size());
      for (std::size_t index = 0; index < input.mesh->vertices().size();
           ++index) {
        const Point3d world = input.mesh->vertices()[index].position;
        if (!std::isfinite(world.x) || !std::isfinite(world.y) ||
            !std::isfinite(world.z))
          return fail();
        const ProjectedPoint projected = project(world);
        if (!finite(projected)) return fail();
        pending->vertices.push_back({projected, meshIndex, index});
        vertexBoxes.push_back(pointBounds(projected.screen));
        vertexDepths.push_back(projected.depth);
      }
      const auto& indices = input.mesh->triangleIndices();
      for (std::size_t index = 0; index < indices.size(); index += 3) {
        if (indices[index] >= input.mesh->vertices().size() ||
            indices[index + 1] >= input.mesh->vertices().size() ||
            indices[index + 2] >= input.mesh->vertices().size())
          return fail();
        const std::array<std::size_t, 3> projected{
            firstVertex + indices[index], firstVertex + indices[index + 1],
            firstVertex + indices[index + 2]};
        const std::size_t face = input.faceOffset +
            input.mesh->vertices()[indices[index]].faceIndex;
        pending->triangles.push_back({projected, face, meshIndex});
        triangleBoxes.push_back(triangleBounds(
            pending->vertices[projected[0]].projected,
            pending->vertices[projected[1]].projected,
            pending->vertices[projected[2]].projected));
        triangleMaximumDepths.push_back(triangleMaximumDepth(
            pending->vertices[projected[0]].projected,
            pending->vertices[projected[1]].projected,
            pending->vertices[projected[2]].projected));
      }
      for (std::size_t edgeIndex = 0;
           edgeIndex < input.mesh->edges().size(); ++edgeIndex) {
        const auto& edge = input.mesh->edges()[edgeIndex];
        const std::size_t firstPoint = pending->edgePoints.size();
        for (std::size_t pointIndex = 0; pointIndex < edge.points.size();
             ++pointIndex) {
          const Point3d world = edge.points[pointIndex];
          if (!std::isfinite(world.x) || !std::isfinite(world.y) ||
              !std::isfinite(world.z))
            return fail();
          const ProjectedPoint projected = project(world);
          if (!finite(projected)) return fail();
          pending->edgePoints.push_back(
              {projected, meshIndex, edgeIndex, pointIndex});
        }
        for (std::size_t pointIndex = 1; pointIndex < edge.points.size();
             ++pointIndex) {
          const std::size_t a = firstPoint + pointIndex - 1;
          const std::size_t b = firstPoint + pointIndex;
          pending->segments.push_back(
              {a, b, input.edgeOffset + edge.edgeIndex});
          segmentBoxes.push_back(segmentBounds(
              pending->edgePoints[a].projected,
              pending->edgePoints[b].projected));
        }
      }
    }
    pending->triangleBvh.build(std::move(triangleBoxes),
                               std::move(triangleMaximumDepths));
    pending->segmentBvh.build(std::move(segmentBoxes));
    pending->vertexBvh.build(std::move(vertexBoxes), std::move(vertexDepths));
    ++pending->counters.buildCount;
    impl_ = std::move(pending);
    return true;
  } catch (...) {
    return fail();
  }
}

void ProjectedPickingScene::invalidate() noexcept { impl_.reset(); }
bool ProjectedPickingScene::empty() const noexcept {
  return !impl_ || impl_->triangles.empty();
}

std::optional<PickingFaceHit> ProjectedPickingScene::faceAt(QPointF point) const {
  if (!impl_) return std::nullopt;
  ++impl_->counters.pointQueries;
  std::optional<PickingFaceHit> hit;
  auto visit = [&](std::size_t index) {
    ++impl_->counters.triangleCandidates;
    const auto triangle = impl_->triangleAt(index);
    const auto depth = triangleDepthAt(point, triangle.a, triangle.b, triangle.c);
    if (!depth) return;
    const double epsilon =
        hit ? std::max(hit->depthEpsilon, triangle.depthEpsilon)
            : triangle.depthEpsilon;
    if (!hit || *depth > hit->depth + epsilon ||
        (std::abs(*depth - hit->depth) <= epsilon &&
         triangle.faceIndex < hit->faceIndex))
      hit = PickingFaceHit{triangle.faceIndex, *depth,
                           triangle.depthEpsilon};
  };
  impl_->triangleBvh.query(pointBounds(point), visit,
                           &impl_->counters.nodeVisits);
  return hit;
}

std::optional<PickingEdgeHit> ProjectedPickingScene::edgeAt(
    QPointF point, double radiusPx, PickingQueryPrecision precision) const {
  if (!impl_) return std::nullopt;
  ++impl_->counters.pointQueries;
  std::optional<PickingEdgeHit> result;
  double resultEpsilon = 0.0;
  bool unresolved = false;
  VisibilityBudget budget = precision == PickingQueryPrecision::Exact
                                ? VisibilityBudget::exact()
                                : VisibilityBudget{};
  auto visit = [&](std::size_t index) {
    if (unresolved) return;
    ++impl_->counters.segmentCandidates;
    const auto& segment = impl_->segments[index];
    const auto& a = impl_->edgePoints[segment.a].projected;
    const auto& b = impl_->edgePoints[segment.b].projected;
    const auto disk = segmentDiskInterval(point, a, b, radiusPx);
    if (!disk) return;
    const ProjectedPoint clippedA = interpolateProjected(a, b, disk->begin);
    const ProjectedPoint clippedB = interpolateProjected(a, b, disk->end);
    const double candidateEpsilon = impl_->segmentDepthEpsilon(index);
    const auto hidden = hiddenIntervals(
        impl_->triangleBvh, clippedA, clippedB, candidateEpsilon,
        [this](std::size_t triangle) { return impl_->triangleAt(triangle); },
        &impl_->counters, budget);
    if (hidden.fullyCovered) return;
    // A bounded hover query that could not classify every occluder is not
    // allowed to turn uncertainty into a false-positive selection. Exact
    // click/release queries use an effectively unbounded indexed traversal.
    if (!hidden.complete) {
      unresolved = true;
      return;
    }
    const auto localParameter =
        closestVisibleParameter(point, clippedA, clippedB, hidden.intervals);
    if (!localParameter) return;
    const double parameter =
        disk->begin + (disk->end - disk->begin) * *localParameter;
    const ProjectedPoint visible = interpolateProjected(a, b, parameter);
    const double distance = QLineF(point, visible.screen).length();
    if (distance > radiusPx + 1e-9) return;
    const double comparisonEpsilon =
        result ? std::max(resultEpsilon, candidateEpsilon) : candidateEpsilon;
    if (result &&
        !(visible.depth > result->depth + comparisonEpsilon ||
          (std::abs(visible.depth - result->depth) <= comparisonEpsilon &&
           (distance < result->distance ||
            (distance == result->distance &&
             segment.edge < result->edgeIndex)))))
      return;
    result = PickingEdgeHit{
        segment.edge, visible.depth, distance,
        interpolatePoint(impl_->worldEdgePoint(segment.a),
                         impl_->worldEdgePoint(segment.b), parameter),
        visible.screen};
    resultEpsilon = candidateEpsilon;
  };
  impl_->segmentBvh.query(
      {point.x() - radiusPx, point.y() - radiusPx,
       point.x() + radiusPx, point.y() + radiusPx}, visit,
      &impl_->counters.nodeVisits);
  return unresolved ? std::nullopt : result;
}

std::vector<std::size_t> ProjectedPickingScene::facesInRect(
    const QRectF& rect) const {
  std::vector<std::size_t> result;
  if (!impl_) return result;
  ++impl_->counters.rectangleQueries;
  std::unordered_set<std::size_t> seen;
  VisibilityBudget budget = VisibilityBudget::exact();
  auto visit = [&](std::size_t index) {
    ++impl_->counters.triangleCandidates;
    const auto triangle = impl_->triangleAt(index);
    if (seen.contains(triangle.faceIndex)) return;
    const auto clipped =
        clipTriangleToRect(triangle.a, triangle.b, triangle.c, rect);
    if (clipped.size() < 3) return;
    const auto visibility = polygonVisibility(
        clipped, triangle.depthEpsilon, triangle.faceIndex,
        impl_->triangleBvh,
        [this](std::size_t triangleIndex) {
          return impl_->triangleAt(triangleIndex);
        },
        &impl_->counters, budget);
    if (visibility != VisibilityConclusion::Visible) return;
    seen.insert(triangle.faceIndex);
    result.push_back(triangle.faceIndex);
  };
  impl_->triangleBvh.query(rectBounds(rect), visit,
                           &impl_->counters.nodeVisits);
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::size_t> ProjectedPickingScene::edgesInRect(
    const QRectF& rect) const {
  std::vector<std::size_t> result;
  if (!impl_) return result;
  ++impl_->counters.rectangleQueries;
  std::unordered_set<std::size_t> seen;
  VisibilityBudget budget = VisibilityBudget::exact();
  auto visit = [&](std::size_t index) {
    ++impl_->counters.segmentCandidates;
    const auto& segment = impl_->segments[index];
    if (seen.contains(segment.edge)) return;
    const auto clipped = clipSegmentToRect(
        impl_->edgePoints[segment.a].projected,
        impl_->edgePoints[segment.b].projected, rect);
    if (!clipped) return;
    const auto hidden = hiddenIntervals(
        impl_->triangleBvh, clipped->first, clipped->second,
        impl_->segmentDepthEpsilon(index),
        [this](std::size_t triangleIndex) {
          return impl_->triangleAt(triangleIndex);
        },
        &impl_->counters, budget);
    if (hidden.fullyCovered || !hidden.complete) return;
    seen.insert(segment.edge);
    result.push_back(segment.edge);
  };
  impl_->segmentBvh.query(rectBounds(rect), visit,
                          &impl_->counters.nodeVisits);
  std::sort(result.begin(), result.end());
  return result;
}

std::optional<std::size_t> ProjectedPickingScene::frontmostFace(
    const std::vector<std::size_t>& faces) const {
  if (!impl_ || faces.empty()) return std::nullopt;
  const std::unordered_set<std::size_t> eligible(faces.begin(), faces.end());
  std::optional<std::size_t> result;
  double best = -std::numeric_limits<double>::max();
  double bestEpsilon = 0.0;
  for (std::size_t index = 0; index < impl_->triangles.size(); ++index) {
    const auto triangle = impl_->triangleAt(index);
    if (!eligible.contains(triangle.faceIndex)) continue;
    const double depth =
        (triangle.a.depth + triangle.b.depth + triangle.c.depth) / 3.0;
    const double epsilon = std::max(bestEpsilon, triangle.depthEpsilon);
    if (!result || depth > best + epsilon ||
        (std::abs(depth - best) <= epsilon &&
         triangle.faceIndex < *result)) {
      result = triangle.faceIndex;
      best = depth;
      bestEpsilon = triangle.depthEpsilon;
    }
  }
  return result;
}

std::optional<std::size_t> ProjectedPickingScene::frontmostEdge(
    const std::vector<std::size_t>& edges) const {
  if (!impl_ || edges.empty()) return std::nullopt;
  const std::unordered_set<std::size_t> eligible(edges.begin(), edges.end());
  std::optional<std::size_t> result;
  double best = -std::numeric_limits<double>::max();
  double bestEpsilon = 0.0;
  for (std::size_t index = 0; index < impl_->segments.size(); ++index) {
    const auto& segment = impl_->segments[index];
    if (!eligible.contains(segment.edge)) continue;
    const double depth = (impl_->edgePoints[segment.a].projected.depth +
                          impl_->edgePoints[segment.b].projected.depth) * 0.5;
    const double candidateEpsilon = impl_->segmentDepthEpsilon(index);
    const double epsilon = std::max(bestEpsilon, candidateEpsilon);
    if (!result || depth > best + epsilon ||
        (std::abs(depth - best) <= epsilon && segment.edge < *result)) {
      result = segment.edge;
      best = depth;
      bestEpsilon = candidateEpsilon;
    }
  }
  return result;
}

std::optional<PickingPointHit> ProjectedPickingScene::snapAt(
    QPointF point, double radiusPx, PickingQueryPrecision precision) const {
  if (!impl_) return std::nullopt;
  ++impl_->counters.pointQueries;
  const Bounds2d area{point.x() - radiusPx, point.y() - radiusPx,
                      point.x() + radiusPx, point.y() + radiusPx};
  struct VertexCandidate {
    std::size_t index{};
    double distance{};
  };
  std::vector<VertexCandidate> vertexCandidates;
  VisibilityBudget vertexBudget =
      precision == PickingQueryPrecision::Exact ? VisibilityBudget::exact()
                                                : VisibilityBudget{};
  const auto gatherVertex = [&](std::size_t index) {
    const auto& projected = impl_->vertices[index].projected;
    const double distance = QLineF(point, projected.screen).length();
    if (distance > radiusPx) return true;
    ++impl_->counters.vertexCandidates;
    if ((precision == PickingQueryPrecision::Interactive &&
         vertexCandidates.size() >= kMaximumInteractiveVertexCandidates) ||
        !vertexBudget.consume()) {
      vertexBudget.exhausted = true;
      return false;
    }
    vertexCandidates.push_back({index, distance});
    return true;
  };
  const bool gatheredAll = impl_->vertexBvh.queryWhile(
      area, gatherVertex, &impl_->counters.nodeVisits);
  if (!gatheredAll) {
    ++impl_->counters.uncertainVisible;
    return std::nullopt;
  }
  std::sort(vertexCandidates.begin(), vertexCandidates.end(),
            [](const VertexCandidate& lhs, const VertexCandidate& rhs) {
              if (lhs.distance != rhs.distance)
                return lhs.distance < rhs.distance;
              return lhs.index < rhs.index;
            });
  for (const auto candidate : vertexCandidates) {
    const std::size_t index = candidate.index;
    const auto& projected = impl_->vertices[index].projected;
    const double candidateEpsilon =
        impl_->meshDepthEpsilons[impl_->vertices[index].mesh];
    ++impl_->counters.vertexVisibilityQueries;
    const auto visibility = pointVisibility(
        projected, candidateEpsilon, std::numeric_limits<std::size_t>::max(),
        impl_->triangleBvh,
        [this](std::size_t triangle) { return impl_->triangleAt(triangle); },
        &impl_->counters, vertexBudget);
    if (visibility == VisibilityConclusion::Unresolved) {
      ++impl_->counters.uncertainVisible;
      return std::nullopt;
    }
    if (visibility == VisibilityConclusion::Visible)
      return PickingPointHit{impl_->worldVertex(index), projected.screen,
                             projected.depth, PickingSnapKind::Vertex};
  }

  const std::uint64_t uncertaintyBefore = impl_->counters.uncertainVisible;
  if (const auto edge = edgeAt(point, radiusPx, precision))
    return PickingPointHit{edge->world, edge->screen, edge->depth,
                           PickingSnapKind::Edge};
  if (precision == PickingQueryPrecision::Interactive &&
      impl_->counters.uncertainVisible != uncertaintyBefore)
    return std::nullopt;

  std::optional<PickingPointHit> surface;
  double bestDepth = -std::numeric_limits<double>::max();
  auto visit = [&](std::size_t index) {
    ++impl_->counters.triangleCandidates;
    const auto projected = impl_->triangleAt(index);
    const auto depth = triangleDepthAt(point, projected.a, projected.b,
                                       projected.c);
    if (!depth || *depth <= bestDepth) return;
    const QPointF v0 = projected.b.screen - projected.a.screen;
    const QPointF v1 = projected.c.screen - projected.a.screen;
    const QPointF v2 = point - projected.a.screen;
    const double denominator = v0.x() * v1.y() - v1.x() * v0.y();
    if (std::abs(denominator) < 1e-12) return;
    const double wb = (v2.x() * v1.y() - v1.x() * v2.y()) / denominator;
    const double wc = (v0.x() * v2.y() - v2.x() * v0.y()) / denominator;
    const double wa = 1.0 - wb - wc;
    const auto& tri = impl_->triangles[index];
    const Point3d a = impl_->worldVertex(tri.vertices[0]);
    const Point3d b = impl_->worldVertex(tri.vertices[1]);
    const Point3d c = impl_->worldVertex(tri.vertices[2]);
    bestDepth = *depth;
    surface = PickingPointHit{
        {a.x * wa + b.x * wb + c.x * wc,
         a.y * wa + b.y * wb + c.y * wc,
         a.z * wa + b.z * wb + c.z * wc},
        point, *depth, PickingSnapKind::Surface};
  };
  impl_->triangleBvh.query(pointBounds(point), visit,
                           &impl_->counters.nodeVisits);
  return surface;
}

std::vector<ProjectedTriangle> ProjectedPickingScene::trianglesForFace(
    std::size_t faceIndex) const {
  std::vector<ProjectedTriangle> result;
  if (!impl_) return result;
  for (std::size_t index = 0; index < impl_->triangles.size(); ++index)
    if (impl_->triangles[index].face == faceIndex)
      result.push_back(impl_->triangleAt(index));
  return result;
}

double ProjectedPickingScene::depthEpsilon() const noexcept {
  return impl_ ? impl_->epsilon : 0.0;
}
const PickingQueryCounters& ProjectedPickingScene::counters() const noexcept {
  static const PickingQueryCounters empty;
  return impl_ ? impl_->counters : empty;
}
void ProjectedPickingScene::resetQueryCounters() const noexcept {
  if (!impl_) return;
  const std::uint64_t builds = impl_->counters.buildCount;
  const std::uint64_t buildFailures = impl_->counters.buildFailures;
  impl_->counters = {};
  impl_->counters.buildCount = builds;
  impl_->counters.buildFailures = buildFailures;
}
std::size_t ProjectedPickingScene::ownedBytes() const noexcept {
  if (!impl_) return 0;
  return impl_->vertices.capacity() * sizeof(Impl::VertexRef) +
         impl_->triangles.capacity() * sizeof(Impl::TriangleRef) +
         impl_->edgePoints.capacity() * sizeof(Impl::EdgePointRef) +
         impl_->segments.capacity() * sizeof(Impl::SegmentRef) +
         impl_->triangleBvh.ownedBytes() + impl_->segmentBvh.ownedBytes() +
         impl_->vertexBvh.ownedBytes();
}

std::vector<std::size_t> collectFacesInRect(
    const std::vector<ProjectedTriangle>& triangles, const QRectF& rect,
    double depthEpsilon, PickingQueryCounters* counters) {
  std::vector<Bounds2d> boxes;
  std::vector<double> maximumDepths;
  boxes.reserve(triangles.size());
  maximumDepths.reserve(triangles.size());
  for (const auto& triangle : triangles) {
    boxes.push_back(triangleBounds(triangle.a, triangle.b, triangle.c));
    maximumDepths.push_back(
        triangleMaximumDepth(triangle.a, triangle.b, triangle.c));
  }
  AabbBvh bvh;
  bvh.build(std::move(boxes), std::move(maximumDepths));
  if (counters) ++counters->buildCount;
  std::vector<std::size_t> result;
  std::unordered_set<std::size_t> seen;
  VisibilityBudget budget = VisibilityBudget::exact();
  const auto triangleAt = [&](std::size_t index) {
    ProjectedTriangle triangle = triangles[index];
    triangle.depthEpsilon =
        std::max(triangle.depthEpsilon, std::max(0.0, depthEpsilon));
    return triangle;
  };
  auto visit = [&](std::size_t index) {
    if (counters) ++counters->triangleCandidates;
    const auto triangle = triangleAt(index);
    if (seen.contains(triangle.faceIndex)) return;
    const auto clipped =
        clipTriangleToRect(triangle.a, triangle.b, triangle.c, rect);
    if (clipped.size() < 3) return;
    const auto visibility = polygonVisibility(
        clipped, triangle.depthEpsilon, triangle.faceIndex, bvh, triangleAt,
        counters, budget);
    if (visibility != VisibilityConclusion::Visible) return;
    seen.insert(triangle.faceIndex);
    result.push_back(triangle.faceIndex);
  };
  if (counters) ++counters->rectangleQueries;
  bvh.query(rectBounds(rect), visit, counters ? &counters->nodeVisits : nullptr);
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::size_t> collectEdgesInRect(
    const std::vector<ProjectedTriangle>& triangles,
    const std::vector<ProjectedEdge>& edges, const QRectF& rect,
    double depthEpsilon, PickingQueryCounters* counters) {
  std::vector<Bounds2d> triangleBoxes;
  std::vector<double> triangleMaximumDepths;
  triangleBoxes.reserve(triangles.size());
  triangleMaximumDepths.reserve(triangles.size());
  for (const auto& triangle : triangles) {
    triangleBoxes.push_back(triangleBounds(triangle.a, triangle.b, triangle.c));
    triangleMaximumDepths.push_back(
        triangleMaximumDepth(triangle.a, triangle.b, triangle.c));
  }
  AabbBvh triangleBvh;
  triangleBvh.build(std::move(triangleBoxes),
                    std::move(triangleMaximumDepths));
  struct Segment { ProjectedPoint a; ProjectedPoint b; std::size_t edge; };
  std::vector<Segment> segments;
  std::vector<Bounds2d> segmentBoxes;
  for (const auto& edge : edges)
    for (std::size_t index = 1; index < edge.points.size(); ++index) {
      segments.push_back({edge.points[index - 1], edge.points[index],
                          edge.edgeIndex});
      segmentBoxes.push_back(segmentBounds(edge.points[index - 1],
                                           edge.points[index]));
    }
  AabbBvh segmentBvh;
  segmentBvh.build(std::move(segmentBoxes));
  if (counters) ++counters->buildCount;
  std::vector<std::size_t> result;
  std::unordered_set<std::size_t> seen;
  VisibilityBudget budget = VisibilityBudget::exact();
  const auto triangleAt = [&](std::size_t index) {
    ProjectedTriangle triangle = triangles[index];
    triangle.depthEpsilon =
        std::max(triangle.depthEpsilon, std::max(0.0, depthEpsilon));
    return triangle;
  };
  auto visit = [&](std::size_t index) {
    if (counters) ++counters->segmentCandidates;
    const auto& segment = segments[index];
    if (seen.contains(segment.edge)) return;
    const auto clipped = clipSegmentToRect(segment.a, segment.b, rect);
    if (!clipped) return;
    const auto hidden = hiddenIntervals(
        triangleBvh, clipped->first, clipped->second,
        std::max(0.0, depthEpsilon), triangleAt, counters, budget);
    if (hidden.fullyCovered || !hidden.complete) return;
    seen.insert(segment.edge);
    result.push_back(segment.edge);
  };
  if (counters) ++counters->rectangleQueries;
  segmentBvh.query(rectBounds(rect), visit,
                   counters ? &counters->nodeVisits : nullptr);
  std::sort(result.begin(), result.end());
  return result;
}

}  // namespace solidar
