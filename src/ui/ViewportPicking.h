#pragma once

#include <QPointF>
#include <QRectF>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ui/BodyRenderMesh.h"
#include "ui/ViewportCamera.h"

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
  double depthEpsilon{};
};

// A mesh edge polyline projected into logical screen coordinates, tagged with
// the owning edge ordinal.
struct ProjectedEdge {
  std::vector<ProjectedPoint> points;
  std::size_t edgeIndex{};
};

struct PickingQueryCounters {
  std::uint64_t buildCount{};
  std::uint64_t buildFailures{};
  std::uint64_t pointQueries{};
  std::uint64_t rectangleQueries{};
  std::uint64_t triangleCandidates{};
  std::uint64_t segmentCandidates{};
  std::uint64_t nodeVisits{};
  std::uint64_t occluderTests{};
  std::uint64_t intervalTests{};
  std::uint64_t subdivisions{};
  std::uint64_t uncertainVisible{};
  std::uint64_t vertexCandidates{};
  std::uint64_t vertexVisibilityQueries{};
};

struct PickingMeshInput {
  const BodyRenderMesh* mesh{};
  std::size_t faceOffset{};
  std::size_t edgeOffset{};
  // Full structural identity; hash collisions can never alias two bodies.
  BodyMeshKey identity;
};

struct PickingFaceHit {
  std::size_t faceIndex{static_cast<std::size_t>(-1)};
  double depth{};
  double depthEpsilon{};
};

struct PickingEdgeHit {
  std::size_t edgeIndex{static_cast<std::size_t>(-1)};
  double depth{};
  double distance{};
  Point3d world{};
  QPointF screen{};
};

// Interactive hover is deliberately work-bounded. Exact is used only at
// interaction boundaries (click/release/Apply) and completes the indexed
// visibility query before accepting a candidate.
enum class PickingQueryPrecision { Interactive, Exact };

enum class PickingSnapKind { Surface, Edge, Vertex };

struct PickingPointHit {
  Point3d world{};
  QPointF screen{};
  double depth{};
  PickingSnapKind kind{PickingSnapKind::Surface};
};

// Camera-revisioned projected scene shared by hover, click, marquee and ruler.
// It stores one projected value per indexed vertex/edge sample and two compact
// 2D BVHs; world-space positions remain owned only by BodyRenderMesh.
class ProjectedPickingScene final {
 public:
  [[nodiscard]] bool ensure(const std::vector<PickingMeshInput>& meshes,
                            const ViewportCameraState& camera);
  void invalidate() noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::optional<PickingFaceHit> faceAt(QPointF point) const;
  [[nodiscard]] std::optional<PickingEdgeHit> edgeAt(
      QPointF point, double radiusPx = kEdgeHitRadiusPx,
      PickingQueryPrecision precision = PickingQueryPrecision::Interactive) const;
  [[nodiscard]] std::vector<std::size_t> facesInRect(const QRectF& rect) const;
  [[nodiscard]] std::vector<std::size_t> edgesInRect(const QRectF& rect) const;
  [[nodiscard]] std::optional<std::size_t> frontmostFace(
      const std::vector<std::size_t>& faces) const;
  [[nodiscard]] std::optional<std::size_t> frontmostEdge(
      const std::vector<std::size_t>& edges) const;
  [[nodiscard]] std::optional<PickingPointHit> snapAt(
      QPointF point, double radiusPx = 11.0,
      PickingQueryPrecision precision = PickingQueryPrecision::Interactive) const;
  [[nodiscard]] std::vector<ProjectedTriangle> trianglesForFace(
      std::size_t faceIndex) const;
  [[nodiscard]] double depthEpsilon() const noexcept;
  [[nodiscard]] const PickingQueryCounters& counters() const noexcept;
  void resetQueryCounters() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
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
// The default range is (0, 360], so positive U represents a complete
// revolution. A range spanning zero resolves the same cursor to a signed
// angle in [-180, 180] before clamping.
// Returns nullopt when the projected basis is degenerate (|det| < 1e-9, e.g. an
// edge-on pose), meaning the angle is indeterminate; callers must preserve the
// currently accepted angle rather than substituting a fabricated value.
[[nodiscard]] std::optional<double> angularValueFromProjectedBasis(
    QPointF cursor, QPointF origin, QPointF uPoint, QPointF vPoint,
    double minimumDeg = 0.01, double maximumDeg = 360.0);

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
    double depthEpsilon, PickingQueryCounters* counters = nullptr);
// Edges whose any projected segment intersects rect AND passes occlusion
// (edge depth + depthEpsilon >= surfaceDepth at the segment contact point),
// reusing the projected triangles for surfaceDepth.
[[nodiscard]] std::vector<std::size_t> collectEdgesInRect(
    const std::vector<ProjectedTriangle>& triangles,
    const std::vector<ProjectedEdge>& edges, const QRectF& rect,
    double depthEpsilon, PickingQueryCounters* counters = nullptr);

}  // namespace solidar
