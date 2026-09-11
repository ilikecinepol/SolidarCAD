#pragma once

#include <QPointF>
#include <QRectF>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace solidar {

inline constexpr double kEdgeHitRadiusPx = 9.0;
inline constexpr double kVertexHitRadiusPx = 9.0;
inline constexpr double kDepthEpsilonScale = 1e-4;

struct ProjectedPoint {
  QPointF screen;
  double depth{};
};

// A mesh triangle projected into logical screen coordinates, tagged with the
// owning face ordinal. Pure data: no GPU/OCCT state, DPR-independent.
struct ProjectedTriangle {
  ProjectedPoint a;
  ProjectedPoint b;
  ProjectedPoint c;
  std::size_t faceIndex{};
};

// A mesh edge polyline projected into logical screen coordinates, tagged with
// the owning edge ordinal.
struct ProjectedEdge {
  std::vector<ProjectedPoint> points;
  std::size_t edgeIndex{};
};

struct SegmentHit {
  double distance{};
  double parameter{};
  double depth{};
};

struct LinearDragSnapshot {
  QPointF cursorStart;
  QPointF projectedUnitAxis;
  double initialValue{};
};

[[nodiscard]] double linearValueFromDrag(const LinearDragSnapshot& drag,
                                         QPointF cursor,
                                         double minimum = 0.0,
                                         double maximum = 100000.0);

// Derives a well-conditioned drag axis (px per unit value) aligned with the
// drawn manipulator arrow. When the true projected unit axis is near end-on
// (magnitude below nearEndOnThresholdPx) its direction is unreliable, so the
// axis is replaced by the drawn arrow direction with bounded gain. The result
// guarantees |axis| >= nearEndOnThresholdPx so linearValueFromDrag never
// dead-zones or hyper-jumps.
[[nodiscard]] QPointF robustLinearDragAxis(QPointF projectedUnitAxis,
                                           QPointF drawnDirection,
                                           double nearEndOnThresholdPx);

[[nodiscard]] std::optional<double> triangleDepthAt(
    QPointF point, const ProjectedPoint& a, const ProjectedPoint& b,
    const ProjectedPoint& c);
[[nodiscard]] SegmentHit closestSegmentHit(
    QPointF point, const ProjectedPoint& a, const ProjectedPoint& b);
// Resolves a cursor against the projected basis of a 3D angular manipulator.
// The range is (0, 360], so positive U represents a complete revolution.
// Returns nullopt when the projected basis is degenerate (|det| < 1e-9, e.g. an
// edge-on pose), meaning the angle is indeterminate; callers must preserve the
// currently accepted angle rather than substituting a fabricated value.
[[nodiscard]] std::optional<double> angularValueFromProjectedBasis(
    QPointF cursor, QPointF origin, QPointF uPoint, QPointF vPoint);

// Marquee (rectangle) candidate predicates. All operate purely on logical
// screen coordinates and are deterministic / DPR-independent. "Hits" when the
// projected geometry intersects or lies inside the rect; touching the rect
// boundary counts as a hit.

// True when the projected segment intersects (or lies inside) rect.
[[nodiscard]] bool segmentIntersectsRect(const ProjectedPoint& a,
                                         const ProjectedPoint& b,
                                         const QRectF& rect);
// True when the projected triangle intersects (or lies inside) rect, including
// the case where rect is fully enclosed by the triangle.
[[nodiscard]] bool triangleIntersectsRect(const ProjectedPoint& a,
                                          const ProjectedPoint& b,
                                          const ProjectedPoint& c,
                                          const QRectF& rect);

// Clips a projected segment to rect (Liang-Barsky). Returns the clipped
// endpoints, with depth linearly interpolated along the segment, when the
// segment intersects rect; nullopt when it lies entirely outside.
[[nodiscard]] std::optional<std::pair<ProjectedPoint, ProjectedPoint>>
clipSegmentToRect(const ProjectedPoint& a, const ProjectedPoint& b,
                  const QRectF& rect);

// Clips a projected triangle to rect (Sutherland-Hodgman convex clip). Returns
// the clipped polygon vertices, with depth linearly interpolated, or an empty
// vector when the triangle does not intersect rect or clips to a degenerate
// region. The polygon is convex, so the vertex average always lies inside it.
[[nodiscard]] std::vector<ProjectedPoint> clipTriangleToRect(
    const ProjectedPoint& a, const ProjectedPoint& b, const ProjectedPoint& c,
    const QRectF& rect);
// Faces whose any projected triangle intersects rect and is frontmost
// (nearest, i.e. largest depth) at its representative triangle-centroid sample
// point. Depth-only occlusion, matching Viewport::updateBodyHover (no winding
// heuristic, since the renderer disables back-face culling).
[[nodiscard]] std::vector<std::size_t> collectFacesInRect(
    const std::vector<ProjectedTriangle>& triangles, const QRectF& rect,
    double depthEpsilon);
// Edges whose any projected segment intersects rect AND passes occlusion
// (edge depth + depthEpsilon >= surfaceDepth at the segment contact point),
// reusing the projected triangles for surfaceDepth.
[[nodiscard]] std::vector<std::size_t> collectEdgesInRect(
    const std::vector<ProjectedTriangle>& triangles,
    const std::vector<ProjectedEdge>& edges, const QRectF& rect,
    double depthEpsilon);

}  // namespace solidar
