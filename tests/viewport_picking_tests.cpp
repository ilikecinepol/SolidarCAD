#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include "ui/ViewportPicking.h"
#include "ui/ViewportRuler.h"

namespace solidar {

class BodyRenderMeshBenchmarkAdapter final {
 public:
  static BodyRenderMesh denseUnresolvedEdgeFixture(
      std::size_t triangleCount) {
    BodyRenderMesh mesh;
    mesh.vertices_.reserve(triangleCount * 3);
    mesh.triangleIndices_.reserve(triangleCount * 3);
    for (std::size_t index = 0; index < triangleCount; ++index) {
      const auto first = static_cast<std::uint32_t>(mesh.vertices_.size());
      const auto face = static_cast<std::uint32_t>(index + 1);
      // Every projected AABB overlaps the candidate edge, but the triangle
      // itself touches y=0 only at x=-20, outside the edge [0, 10]. This
      // forces the interactive occluder budget to report Uncertain without
      // accidentally proving either visible or hidden.
      mesh.vertices_.push_back({{-20.0, 0.0, 10.0}, {0.0, 0.0, 1.0}, face});
      mesh.vertices_.push_back({{20.0, 20.0, 10.0}, {0.0, 0.0, 1.0}, face});
      mesh.vertices_.push_back({{-20.0, 20.0, 10.0}, {0.0, 0.0, 1.0}, face});
      mesh.triangleIndices_.insert(mesh.triangleIndices_.end(),
                                   {first, first + 1, first + 2});
    }
    mesh.edges_.push_back({{{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}}, 0});
    mesh.center_ = {0.0, 10.0, 5.0};
    mesh.diagonal_ = 50.0;
    mesh.faceCount_ = triangleCount;
    mesh.edgeSampleCount_ = 2;
    mesh.revision_ = 1;
    return mesh;
  }

  static BodyRenderMesh denseVertexLayers(std::size_t layerCount) {
    BodyRenderMesh mesh;
    mesh.vertices_.reserve(layerCount * 3);
    mesh.triangleIndices_.reserve(layerCount * 3);
    for (std::size_t layer = 0; layer < layerCount; ++layer) {
      const double z = static_cast<double>(layer);
      const auto first = static_cast<std::uint32_t>(mesh.vertices_.size());
      const auto face = static_cast<std::uint32_t>(layer);
      mesh.vertices_.push_back({{0.0, 0.0, z}, {0.0, 0.0, 1.0}, face});
      mesh.vertices_.push_back({{0.001, 0.0, z}, {0.0, 0.0, 1.0}, face});
      mesh.vertices_.push_back({{0.0, 0.001, z}, {0.0, 0.0, 1.0}, face});
      mesh.triangleIndices_.insert(mesh.triangleIndices_.end(),
                                   {first, first + 1, first + 2});
    }
    mesh.center_ = {0.0, 0.0, static_cast<double>(layerCount) * 0.5};
    mesh.diagonal_ = static_cast<double>(layerCount) + 1.0;
    mesh.faceCount_ = layerCount;
    mesh.revision_ = 1;
    return mesh;
  }

  static BodyRenderMesh invalidTriangleIndexFixture() {
    BodyRenderMesh mesh;
    mesh.vertices_ = {
        {{-1.0, -1.0, 1.0}, {0.0, 0.0, 1.0}, 0},
        {{1.0, -1.0, 1.0}, {0.0, 0.0, 1.0}, 0},
        {{0.0, 1.0, 1.0}, {0.0, 0.0, 1.0}, 0}};
    mesh.triangleIndices_ = {0, 1, 99};
    mesh.center_ = {};
    mesh.diagonal_ = 3.0;
    mesh.faceCount_ = 1;
    mesh.revision_ = 7;
    return mesh;
  }
};

}  // namespace solidar

int main() {
  using solidar::ProjectedPoint;
  const solidar::LinearDragSnapshot drag{{100.0, 80.0}, {4.0, 0.0}, 0.0};
  CHECK(std::abs(solidar::linearValueFromDrag(drag, {100.0, 80.0})) < 1e-9);
  CHECK(std::abs(solidar::linearValueFromDrag(drag, {102.0, 80.0}) - 0.5) < 1e-9);
  CHECK(std::abs(solidar::linearValueFromDrag(drag, {108.0, 80.0}) - 2.0) < 1e-9);
  CHECK(std::abs(solidar::linearValueFromDrag(drag, {104.0, 80.0}) - 1.0) < 1e-9);
  CHECK(solidar::linearValueFromDrag(drag, {-100.0, 80.0}) == 0.0);

  // Flipped axis: dragging along the arrow still increases the value; moving
  // opposite reaches the minimum without overshooting.
  {
    const solidar::LinearDragSnapshot flipped{{100.0, 80.0}, {-4.0, 0.0}, 0.0};
    CHECK(std::abs(solidar::linearValueFromDrag(flipped, {96.0, 80.0}) - 1.0) <
          1e-9);
    CHECK(std::abs(solidar::linearValueFromDrag(flipped, {104.0, 80.0})) <
          1e-9);
    CHECK(std::abs(solidar::linearValueFromDrag(flipped, {92.0, 80.0}) - 2.0) <
          1e-9);
  }

  // Near-zero axis: no division blow-up, no jump to maximum, no NaN.
  {
    const solidar::LinearDragSnapshot nearZero{{100.0, 80.0}, {1e-5, 0.0}, 5.0};
    const double value =
        solidar::linearValueFromDrag(nearZero, {400.0, 80.0}, 0.0, 100.0);
    CHECK(std::isfinite(value));
    CHECK(std::abs(value - 5.0) < 1e-9);
  }

  // robustLinearDragAxis: a well-conditioned axis passes through unchanged.
  {
    const QPointF axis =
        solidar::robustLinearDragAxis({4.0, 0.0}, {1.0, 0.0}, 2.0);
    CHECK(std::abs(axis.x() - 4.0) < 1e-9);
    CHECK(std::abs(axis.y()) < 1e-9);
  }
  // Near end-on: replaced by the drawn direction with bounded gain (|axis| ==
  // threshold), so the following drag is neither dead nor hypersensitive.
  {
    const QPointF axis =
        solidar::robustLinearDragAxis({0.1, 0.0}, {1.0, 0.0}, 2.0);
    CHECK(std::abs(axis.x() - 2.0) < 1e-9);
    CHECK(std::abs(axis.y()) < 1e-9);
    const solidar::LinearDragSnapshot bounded{{100.0, 80.0}, axis, 0.0};
    CHECK(std::abs(solidar::linearValueFromDrag(bounded, {102.0, 80.0}) -
                   1.0) < 1e-9);
  }
  // Non-finite true axis falls back deterministically.
  {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const QPointF axis =
        solidar::robustLinearDragAxis({nan, 0.0}, {0.0, -1.0}, 2.0);
    CHECK(std::isfinite(axis.x()) && std::isfinite(axis.y()));
    CHECK(std::abs(axis.y() + 2.0) < 1e-9);
  }

  const ProjectedPoint backA{{0.0, 0.0}, 1.0};
  const ProjectedPoint backB{{100.0, 0.0}, 1.0};
  const ProjectedPoint backC{{0.0, 100.0}, 1.0};
  const ProjectedPoint frontA{{0.0, 0.0}, 9.0};
  const ProjectedPoint frontB{{100.0, 0.0}, 9.0};
  const ProjectedPoint frontC{{0.0, 100.0}, 9.0};

  const auto backDepth =
      solidar::triangleDepthAt({25.0, 25.0}, backA, backB, backC);
  const auto frontDepth =
      solidar::triangleDepthAt({25.0, 25.0}, frontA, frontB, frontC);
  CHECK(backDepth && frontDepth && *frontDepth > *backDepth);

  const ProjectedPoint slopedA{{0.0, 0.0}, 2.0};
  const ProjectedPoint slopedB{{100.0, 0.0}, 12.0};
  const auto hit = solidar::closestSegmentHit({75.0, 3.0}, slopedA, slopedB);
  CHECK(std::abs(hit.parameter - 0.75) < 1e-9);
  CHECK(std::abs(hit.depth - 9.5) < 1e-9);
  CHECK(std::abs(hit.distance - 3.0) < 1e-9);

  CHECK(!solidar::triangleDepthAt({90.0, 90.0}, frontA, frontB, frontC));

  const QPointF angularOrigin{100.0, 100.0};
  const QPointF angularU{140.0, 100.0};
  const QPointF angularV{100.0, 120.0};
  CHECK(std::abs(*solidar::angularValueFromProjectedBasis(
                     angularV, angularOrigin, angularU, angularV) -
                 90.0) < 1e-9);
  CHECK(std::abs(*solidar::angularValueFromProjectedBasis(
                     {60.0, 100.0}, angularOrigin, angularU, angularV) -
                 180.0) < 1e-9);
  CHECK(std::abs(*solidar::angularValueFromProjectedBasis(
                     angularU, angularOrigin, angularU, angularV) -
                 360.0) < 1e-9);
  CHECK(*solidar::angularValueFromProjectedBasis(
            {140.0, 99.99}, angularOrigin, angularU, angularV) > 359.0);
  CHECK(*solidar::angularValueFromProjectedBasis(
            {140.0, 100.01}, angularOrigin, angularU, angularV) < 1.0);
  CHECK(std::abs(*solidar::angularValueFromProjectedBasis(
                     {100.0, 80.0}, angularOrigin, angularU, angularV,
                     -89.99, 89.99) +
                 89.99) < 1e-9);
  CHECK(std::abs(*solidar::angularValueFromProjectedBasis(
                     angularU, angularOrigin, angularU, angularV,
                     -89.99, 89.99)) < 1e-9);

  // Degenerate projected basis reports an indeterminate angle (nullopt) rather
  // than a fabricated complete revolution; the caller preserves the current
  // accepted angle instead of jumping to 360.
  CHECK(!solidar::angularValueFromProjectedBasis(
             {30.0, 0.0}, angularOrigin, angularOrigin, angularOrigin)
             .has_value());
  // Collinear (non-zero) basis vectors are equally indeterminate.
  CHECK(!solidar::angularValueFromProjectedBasis(
             {30.0, 0.0}, angularOrigin, angularU, {180.0, 100.0})
             .has_value());

  // Occlusion: a rear edge projected behind a front surface must be rejected
  // for hover. Mirrors Viewport::updateBodyHover, which rejects an edge when
  // its depth is behind the nearest surface at the same screen position.
  {
    const ProjectedPoint frontA{{0.0, 0.0}, 9.0};
    const ProjectedPoint frontB{{100.0, 0.0}, 9.0};
    const ProjectedPoint frontC{{0.0, 100.0}, 9.0};
    const ProjectedPoint rearA{{20.0, 20.0}, 1.0};
    const ProjectedPoint rearB{{80.0, 20.0}, 1.0};
    const QPointF cursor{50.0, 20.0};
    const auto hit = solidar::closestSegmentHit(cursor, rearA, rearB);
    CHECK(hit.distance <= solidar::kEdgeHitRadiusPx);
    const QPointF closest =
        rearA.screen + (rearB.screen - rearA.screen) * hit.parameter;
    const auto surfaceDepth =
        solidar::triangleDepthAt(closest, frontA, frontB, frontC);
    CHECK(surfaceDepth.has_value());
    CHECK(hit.depth + 1e-6 < *surfaceDepth);
  }

  // Positive control: an edge coplanar with the front surface is not occluded.
  {
    const ProjectedPoint frontA{{0.0, 0.0}, 9.0};
    const ProjectedPoint frontB{{100.0, 0.0}, 9.0};
    const ProjectedPoint frontC{{0.0, 100.0}, 9.0};
    const ProjectedPoint edgeA{{20.0, 20.0}, 9.0};
    const ProjectedPoint edgeB{{80.0, 20.0}, 9.0};
    const QPointF cursor{50.0, 20.0};
    const auto hit = solidar::closestSegmentHit(cursor, edgeA, edgeB);
    CHECK(hit.distance <= solidar::kEdgeHitRadiusPx);
    const QPointF closest =
        edgeA.screen + (edgeB.screen - edgeA.screen) * hit.parameter;
    const auto surfaceDepth =
        solidar::triangleDepthAt(closest, frontA, frontB, frontC);
    CHECK(surfaceDepth.has_value());
    CHECK(!(hit.depth + 1e-6 < *surfaceDepth));
  }

  // Marquee rect candidate predicates. All in logical coordinates.
  {
    using solidar::ProjectedTriangle;
    using solidar::ProjectedEdge;

    // Shared projected triangle geometry. Depth is not used by the pure
    // rect-intersection predicates.
    const ProjectedPoint baseA{{0.0, 0.0}, 1.0};
    const ProjectedPoint baseB{{0.0, 100.0}, 1.0};
    const ProjectedPoint baseC{{100.0, 0.0}, 1.0};

    // Inside / touching / disjoint.
    const QRectF insideRect(10.0, 10.0, 20.0, 20.0);
    CHECK(solidar::triangleIntersectsRect(baseA, baseB, baseC, insideRect));
    // Rect fully enclosed by the triangle (no vertex inside, no edge crossing).
    CHECK(solidar::triangleIntersectsRect(baseA, baseB, baseC,
                                          QRectF(20.0, 20.0, 5.0, 5.0)));
    // Touching the triangle vertex on the rect corner counts as a hit.
    CHECK(solidar::triangleIntersectsRect(baseA, baseB, baseC,
                                          QRectF(0.0, 0.0, 10.0, 10.0)));
    // Disjoint.
    CHECK(!solidar::triangleIntersectsRect(baseA, baseB, baseC,
                                           QRectF(200.0, 200.0, 10.0, 10.0)));

    // Segment predicates: endpoint on the boundary, crossing, disjoint.
    const ProjectedPoint segA{{0.0, 0.0}, 1.0};
    const ProjectedPoint segB{{100.0, 100.0}, 1.0};
    CHECK(solidar::segmentIntersectsRect(segA, segB,
                                         QRectF(0.0, 0.0, 10.0, 10.0)));
    const ProjectedPoint crossA{{-10.0, 50.0}, 1.0};
    const ProjectedPoint crossB{{110.0, 50.0}, 1.0};
    CHECK(solidar::segmentIntersectsRect(crossA, crossB,
                                         QRectF(0.0, 0.0, 100.0, 100.0)));
    CHECK(!solidar::segmentIntersectsRect(crossA, crossB,
                                          QRectF(200.0, 200.0, 10.0, 10.0)));

    // Face collection: depth-based occlusion parity with updateBodyHover. The
    // front triangle (larger cameraDepth = nearer) overlapping the back
    // triangle (smaller depth) in the same screen region yields only the front
    // face; the occluded back face is excluded.
    {
      const ProjectedPoint nearA{{0.0, 0.0}, 9.0};
      const ProjectedPoint nearB{{0.0, 100.0}, 9.0};
      const ProjectedPoint nearC{{100.0, 0.0}, 9.0};
      std::vector<ProjectedTriangle> tris;
      tris.push_back({baseA, baseB, baseC, 7});  // back face (depth 1.0)
      tris.push_back({nearA, nearB, nearC, 8});  // front face (depth 9.0)
      const auto faces = solidar::collectFacesInRect(tris, insideRect, 1e-6);
      CHECK((faces == std::vector<std::size_t>{8}));
    }

    // Edge collection: a coplanar edge is selected; a rear edge completely
    // behind the front surface is rejected; a partially visible rear edge is
    // retained (selection is conservative, like the renderer).
    {
      const ProjectedPoint fA{{0.0, 0.0}, 9.0};
      const ProjectedPoint fB{{0.0, 100.0}, 9.0};
      const ProjectedPoint fC{{100.0, 0.0}, 9.0};
      std::vector<ProjectedTriangle> surface;
      surface.push_back({fA, fB, fC, 0});

      ProjectedEdge coplanar;
      coplanar.edgeIndex = 11;
      coplanar.points = {{{-10.0, 50.0}, 9.0}, {{110.0, 50.0}, 9.0}};
      ProjectedEdge rear;
      rear.edgeIndex = 12;
      rear.points = {{{10.0, 50.0}, 1.0}, {{40.0, 50.0}, 1.0}};
      ProjectedEdge partiallyVisible;
      partiallyVisible.edgeIndex = 13;
      partiallyVisible.points = {
          {{-10.0, 50.0}, 1.0}, {{110.0, 50.0}, 1.0}};

      std::vector<ProjectedEdge> edges{coplanar, rear, partiallyVisible};
      const auto result = solidar::collectEdgesInRect(
          surface, edges, QRectF(0.0, 0.0, 100.0, 100.0), 1e-6);
      CHECK(std::find(result.begin(), result.end(), 11u) != result.end());
      CHECK(std::find(result.begin(), result.end(), 12u) == result.end());
      CHECK(std::find(result.begin(), result.end(), 13u) != result.end());
    }

    // Clipped-visibility regression: the occlusion sample must lie INSIDE the
    // rectangle, not at the raw triangle centroid / segment midpoint, which
    // can fall outside the rect and sample unrelated geometry.
    {
      const QRectF cornerRect(0.0, 0.0, 10.0, 10.0);

      // (a) Edge whose in-rect portion is fully hidden by a front triangle is
      // rejected, even though its raw midpoint (50,5) lies outside both the
      // rect and the front triangle (hence "visible" at the midpoint).
      {
        const ProjectedPoint frontA{{0.0, 0.0}, 9.0};
        const ProjectedPoint frontB{{20.0, 0.0}, 9.0};
        const ProjectedPoint frontC{{0.0, 20.0}, 9.0};
        std::vector<ProjectedTriangle> surface;
        surface.push_back({frontA, frontB, frontC, 0});

        ProjectedEdge hidden;
        hidden.edgeIndex = 21;
        hidden.points = {{{-100.0, 5.0}, 1.0}, {{200.0, 5.0}, 1.0}};

        const auto result = solidar::collectEdgesInRect(
            surface, {hidden}, cornerRect, 1e-6);
        CHECK(std::find(result.begin(), result.end(), 21u) == result.end());
      }

      // (b) Edge whose in-rect portion is visible is selected, even though its
      // raw midpoint (50,5) is hidden behind the front triangle (which covers
      // (50,5) but not the rect).
      {
        const ProjectedPoint frontA{{40.0, 0.0}, 9.0};
        const ProjectedPoint frontB{{60.0, 0.0}, 9.0};
        const ProjectedPoint frontC{{50.0, 20.0}, 9.0};
        std::vector<ProjectedTriangle> surface;
        surface.push_back({frontA, frontB, frontC, 0});

        ProjectedEdge visible;
        visible.edgeIndex = 22;
        visible.points = {{{-100.0, 5.0}, 1.0}, {{200.0, 5.0}, 1.0}};

        const auto result = solidar::collectEdgesInRect(
            surface, {visible}, cornerRect, 1e-6);
        CHECK(std::find(result.begin(), result.end(), 22u) != result.end());
      }

      // (c) Back face whose in-rect portion is covered by the front face is
      // rejected, even though its raw centroid (66.67, 66.67) lies outside the
      // rect and outside the front triangle.
      {
        const ProjectedPoint backA{{0.0, 0.0}, 1.0};
        const ProjectedPoint backB{{200.0, 0.0}, 1.0};
        const ProjectedPoint backC{{0.0, 200.0}, 1.0};
        const ProjectedPoint frontA{{0.0, 0.0}, 9.0};
        const ProjectedPoint frontB{{20.0, 0.0}, 9.0};
        const ProjectedPoint frontC{{0.0, 20.0}, 9.0};
        std::vector<ProjectedTriangle> tris;
        tris.push_back({backA, backB, backC, 31});
        tris.push_back({frontA, frontB, frontC, 32});
        const auto faces = solidar::collectFacesInRect(tris, cornerRect, 1e-6);
        CHECK(std::find(faces.begin(), faces.end(), 31u) == faces.end());
        CHECK(std::find(faces.begin(), faces.end(), 32u) != faces.end());
      }

      // (d) Back face whose in-rect portion is uncovered is selected, even
      // though its raw centroid (66.67, 66.67) is covered by a front triangle
      // (which covers the centroid but not the rect).
      {
        const ProjectedPoint backA{{0.0, 0.0}, 1.0};
        const ProjectedPoint backB{{200.0, 0.0}, 1.0};
        const ProjectedPoint backC{{0.0, 200.0}, 1.0};
        const ProjectedPoint frontA{{50.0, 50.0}, 9.0};
        const ProjectedPoint frontB{{90.0, 50.0}, 9.0};
        const ProjectedPoint frontC{{70.0, 90.0}, 9.0};
        std::vector<ProjectedTriangle> tris;
        tris.push_back({backA, backB, backC, 41});
        tris.push_back({frontA, frontB, frontC, 42});
        const auto faces = solidar::collectFacesInRect(tris, cornerRect, 1e-6);
        CHECK(std::find(faces.begin(), faces.end(), 41u) != faces.end());
        CHECK(std::find(faces.begin(), faces.end(), 42u) == faces.end());
      }
    }

    // Scale invariance and NaN safety: same result at a different scale, and a
    // NaN depth does not crash or produce NaN ordinals.
    {
      const double scale = 3.7;
      const ProjectedPoint a{{0.0, 0.0}, 1.0};
      const ProjectedPoint b{{0.0, 100.0 * scale}, 1.0};
      const ProjectedPoint c{{100.0 * scale, 0.0}, 1.0};
      const auto faces = solidar::collectFacesInRect(
          {{a, b, c, 3}}, QRectF(0.0, 0.0, 50.0 * scale, 50.0 * scale), 1e-6);
      CHECK((faces == std::vector<std::size_t>{3}));

      const ProjectedPoint nanA{
          {0.0, 0.0}, std::numeric_limits<double>::quiet_NaN()};
      const ProjectedPoint nanB{{0.0, 100.0}, 1.0};
      const ProjectedPoint nanC{{100.0, 0.0}, 1.0};
      const auto nanFaces = solidar::collectFacesInRect(
          {{nanA, nanB, nanC, 5}}, QRectF(10.0, 10.0, 20.0, 20.0), 1e-6);
      CHECK((nanFaces == std::vector<std::size_t>{5}));
    }

    // A sub-pixel opening between two occluders is still visibility. The
    // interval union is exact along an edge segment; it must not disappear
    // because a few midpoint samples happen to land on the front surfaces.
    {
      const auto triangle = [](QPointF a, QPointF b, QPointF c,
                               std::size_t face) {
        return ProjectedTriangle{{a, 9.0}, {b, 9.0}, {c, 9.0}, face};
      };
      const std::vector<ProjectedTriangle> splitOccluder{
          triangle({-1.0, -1.0}, {4.99, -1.0}, {4.99, 11.0}, 60),
          triangle({-1.0, -1.0}, {4.99, 11.0}, {-1.0, 11.0}, 60),
          triangle({5.01, -1.0}, {11.0, -1.0}, {11.0, 11.0}, 61),
          triangle({5.01, -1.0}, {11.0, 11.0}, {5.01, 11.0}, 61)};
      ProjectedEdge rear;
      rear.edgeIndex = 62;
      rear.points = {{{0.0, 5.0}, 1.0}, {{10.0, 5.0}, 1.0}};
      const auto result = solidar::collectEdgesInRect(
          splitOccluder, {rear}, QRectF(0.0, 0.0, 10.0, 10.0), 1e-6);
      CHECK((result == std::vector<std::size_t>{62}));
    }

    // Depth epsilon is symmetric and strict: z-fighting-scale differences do
    // not hide an edge, while a surface beyond the tolerance does.
    {
      const auto coveringSurface = [](double depth) {
        return std::vector<ProjectedTriangle>{
            {{{-1.0, -1.0}, depth}, {{11.0, -1.0}, depth},
             {{11.0, 11.0}, depth}, 70},
            {{{-1.0, -1.0}, depth}, {{11.0, 11.0}, depth},
             {{-1.0, 11.0}, depth}, 70}};
      };
      ProjectedEdge candidate;
      candidate.edgeIndex = 71;
      candidate.points = {{{0.0, 5.0}, 1.0}, {{10.0, 5.0}, 1.0}};
      CHECK((solidar::collectEdgesInRect(
                 coveringSurface(1.0 + 0.5e-6), {candidate},
                 QRectF(0.0, 0.0, 10.0, 10.0), 1e-6) ==
             std::vector<std::size_t>{71}));
      CHECK(solidar::collectEdgesInRect(
                coveringSurface(1.0 + 2.0e-6), {candidate},
                QRectF(0.0, 0.0, 10.0, 10.0), 1e-6)
                .empty());
    }

    // A dense overlap must not exhaust the interactive budget before noticing
    // the common exact case: one front triangle already covers the complete
    // rear segment. Incremental interval union stops immediately and the rear
    // edge is never leaked as selectable.
    {
      std::vector<ProjectedTriangle> occluders;
      occluders.reserve(5000);
      for (std::size_t index = 0; index < 5000; ++index)
        occluders.push_back(
            {{{0.0, 0.0}, 9.0}, {{10.0, 0.0}, 9.0},
             {{0.0, 10.0}, 9.0}, index + 100});
      ProjectedEdge rear;
      rear.edgeIndex = 99;
      rear.points = {{{1.0, 1.0}, 1.0}, {{2.0, 1.0}, 1.0}};
      solidar::PickingQueryCounters counters;
      const auto result = solidar::collectEdgesInRect(
          occluders, {rear}, QRectF(0.0, 0.0, 10.0, 10.0), 1e-6,
          &counters);
      CHECK(result.empty());
      CHECK(counters.uncertainVisible == 0);
      CHECK(counters.occluderTests < 16);
    }

    // Exact face visibility is based on polygon subtraction, not a fixed set
    // of samples. A 1x1 interior hole remains selectable even though the rear
    // triangle vertices and old centroid samples are all covered; filling that
    // hole proves the same rear face hidden.
    {
      const auto addRect = [](std::vector<ProjectedTriangle>& triangles,
                              double left, double top, double right,
                              double bottom, std::size_t face) {
        const ProjectedPoint a{{left, top}, 9.0};
        const ProjectedPoint b{{right, top}, 9.0};
        const ProjectedPoint c{{right, bottom}, 9.0};
        const ProjectedPoint d{{left, bottom}, 9.0};
        triangles.push_back({a, b, c, face});
        triangles.push_back({a, c, d, face});
      };
      std::vector<ProjectedTriangle> ring{
          {{{0.0, 0.0}, 1.0}, {{10.0, 0.0}, 1.0},
           {{10.0, 10.0}, 1.0}, 200},
          {{{0.0, 0.0}, 1.0}, {{10.0, 10.0}, 1.0},
           {{0.0, 10.0}, 1.0}, 200}};
      addRect(ring, 0.0, 0.0, 10.0, 4.5, 201);
      addRect(ring, 0.0, 5.5, 10.0, 10.0, 202);
      addRect(ring, 0.0, 4.5, 4.5, 5.5, 203);
      addRect(ring, 5.5, 4.5, 10.0, 5.5, 204);
      auto faces = solidar::collectFacesInRect(
          ring, QRectF(0.0, 0.0, 10.0, 10.0), 1e-6);
      CHECK(std::find(faces.begin(), faces.end(), 200u) != faces.end());
      addRect(ring, 4.5, 4.5, 5.5, 5.5, 205);
      faces = solidar::collectFacesInRect(
          ring, QRectF(0.0, 0.0, 10.0, 10.0), 1e-6);
      CHECK(std::find(faces.begin(), faces.end(), 200u) == faces.end());
    }

    // Face certification also stops on the first full-cell occluder rather
    // than gathering thousands of duplicates before doing useful work.
    {
      std::vector<ProjectedTriangle> layers{
          {{{1.0, 1.0}, 1.0}, {{2.0, 1.0}, 1.0},
           {{1.0, 2.0}, 1.0}, 300}};
      for (std::size_t index = 0; index < 5000; ++index)
        layers.push_back(
            {{{0.0, 0.0}, 9.0}, {{10.0, 0.0}, 9.0},
             {{0.0, 10.0}, 9.0}, 301});
      solidar::PickingQueryCounters counters;
      const auto faces = solidar::collectFacesInRect(
          layers, QRectF(0.0, 0.0, 10.0, 10.0), 1e-6, &counters);
      CHECK(std::find(faces.begin(), faces.end(), 300u) == faces.end());
      CHECK(counters.occluderTests < 16);
    }
  }

  // Production projected-scene cache: identical mesh/camera revisions build
  // once, camera changes rebuild once, and all query routes share that scene.
  {
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(20.0, 15.0, 10.0).Shape());
    solidar::BodyRenderMesh mesh;
    CHECK(mesh.tryRebuild(*shape));
    const solidar::BodyMeshKey key{1, 2, 3, shape.get(),
                                   mesh.quality()};
    const std::size_t secondFaceOffset = mesh.faceCount();
    const std::size_t secondEdgeOffset = mesh.edges().size();
    const solidar::BodyMeshKey secondKey{4, 5, 6, shape.get(),
                                         mesh.quality()};
    std::vector<solidar::PickingMeshInput> inputs{
        {&mesh, 0, 0, key},
        {&mesh, secondFaceOffset, secondEdgeOffset, secondKey}};
    solidar::ViewportCameraState camera{-35.0F, 25.0F, 1.0F, {},
                                         QSize(800, 600), 1.0F,
                                         mesh.center(),
                                         std::max(1.0,
                                                  mesh.diagonal() * 3.0)};
    solidar::ProjectedPickingScene scene;
    CHECK(scene.ensure(inputs, camera));
    CHECK(scene.counters().buildCount == 1);
    CHECK(!scene.ensure(inputs, camera));
    CHECK(scene.counters().buildCount == 1);
    const QPointF projectedCenter = camera.worldToScreen(mesh.center());
    CHECK(scene.faceAt(projectedCenter).has_value());
    CHECK(!scene.facesInRect(QRectF(projectedCenter - QPointF(20.0, 20.0),
                                    QSizeF(40.0, 40.0)))
               .empty());
    CHECK(scene.snapAt(projectedCenter).has_value());
    std::optional<solidar::PickingEdgeHit> exactEdge;
    QPointF edgePoint;
    for (const auto& edge : mesh.edges()) {
      for (const auto& point : edge.points) {
        edgePoint = camera.worldToScreen(point);
        exactEdge = scene.edgeAt(edgePoint, solidar::kEdgeHitRadiusPx,
                                 solidar::PickingQueryPrecision::Exact);
        if (exactEdge) break;
      }
      if (exactEdge) break;
    }
    CHECK(exactEdge.has_value());
    const auto rectangleEdges = scene.edgesInRect(
        QRectF(edgePoint - QPointF(2.0, 2.0), QSizeF(4.0, 4.0)));
    CHECK(!rectangleEdges.empty());
    CHECK(std::any_of(rectangleEdges.begin(), rectangleEdges.end(),
                      [secondEdgeOffset](std::size_t edge) {
                        return edge >= secondEdgeOffset;
                      }));
    solidar::ViewportRuler ruler;
    ruler.begin();
    CHECK(ruler.updateHover(scene, edgePoint, true));
    CHECK(ruler.hoverPoint().has_value());
    const auto queryCounters = scene.counters();
    CHECK(queryCounters.pointQueries >= 2);
    CHECK(queryCounters.rectangleQueries >= 1);
    CHECK(queryCounters.nodeVisits > 0);

    // Failed rebuilds are transactional: malformed indexed data neither
    // replaces the last valid query scene nor advances its published key.
    auto malformed = solidar::BodyRenderMeshBenchmarkAdapter::
        invalidTriangleIndexFixture();
    const auto beforeFailure = scene.counters();
    const auto faceBeforeFailure = scene.faceAt(projectedCenter);
    CHECK(faceBeforeFailure.has_value());
    const solidar::BodyMeshKey malformedKey{
        90, 91, 92, &malformed, malformed.quality()};
    CHECK(!scene.ensure({{&malformed, 0, 0, malformedKey}}, camera));
    CHECK(scene.counters().buildCount == beforeFailure.buildCount);
    CHECK(scene.counters().buildFailures ==
          beforeFailure.buildFailures + 1);
    const auto faceAfterFailure = scene.faceAt(projectedCenter);
    CHECK(faceAfterFailure.has_value());
    CHECK(faceAfterFailure->faceIndex == faceBeforeFailure->faceIndex);
    CHECK(!scene.ensure(inputs, camera));

    camera.yawDeg += 1.0F;
    CHECK(scene.ensure(inputs, camera));
    CHECK(scene.counters().buildCount == 2);
    CHECK(mesh.tryRebuild(*shape));
    CHECK(scene.ensure(inputs, camera));
    CHECK(scene.counters().buildCount == 3);
    CHECK(scene.ownedBytes() > 0);
  }

  // Dense ruler vertex snapping uses a dedicated point index and one bounded
  // interactive budget. More than the cap is Uncertain (so the ruler keeps
  // its prior proven hover); Exact traverses all layers and returns the
  // frontmost visible vertex deterministically.
  {
    constexpr std::size_t layerCount = 600;
    auto mesh =
        solidar::BodyRenderMeshBenchmarkAdapter::denseVertexLayers(layerCount);
    const solidar::BodyMeshKey key{41, 42, 43, &mesh, mesh.quality()};
    const solidar::ViewportCameraState camera{
        0.0F, 0.0F, 1.0F, {}, QSize(800, 600), 1.0F,
        mesh.center(), 2000.0};
    solidar::ProjectedPickingScene scene;
    CHECK(scene.ensure({{&mesh, 0, 0, key}}, camera));
    const QPointF cursor = camera.worldToScreen({0.0, 0.0, 0.0});
    scene.resetQueryCounters();
    CHECK(!scene.snapAt(cursor, 11.0,
                        solidar::PickingQueryPrecision::Interactive));
    CHECK(scene.counters().uncertainVisible == 1);
    CHECK(scene.counters().vertexCandidates <= 513);
    CHECK(scene.counters().vertexVisibilityQueries == 0);
    scene.resetQueryCounters();
    const auto exact = scene.snapAt(
        cursor, 11.0, solidar::PickingQueryPrecision::Exact);
    CHECK(exact.has_value());
    CHECK(exact->kind == solidar::PickingSnapKind::Vertex);
    CHECK(std::abs(exact->world.z - static_cast<double>(layerCount - 1)) <
          1e-9);
    CHECK(scene.counters().vertexVisibilityQueries > 0);
  }

  // Interactive visibility exhaustion is an explicit uncertainty: it never
  // publishes a partially resolved edge or clears the ruler's last proven
  // hover. Exact release/commit traverses the same production BVH to a result.
  {
    auto mesh = solidar::BodyRenderMeshBenchmarkAdapter::
        denseUnresolvedEdgeFixture(5000);
    const solidar::BodyMeshKey key{1, 1, 1, &mesh, mesh.quality()};
    const solidar::ViewportCameraState camera{
        0.0F, 0.0F, 1.0F, {}, QSize(800, 600), 1.0F,
        mesh.center(), 150.0};
    solidar::ProjectedPickingScene scene;
    CHECK(scene.ensure({{&mesh, 0, 0, key}}, camera));
    const QPointF cursor = camera.worldToScreen({5.0, 0.0, 0.0});
    const auto uncertaintyBefore = scene.counters().uncertainVisible;
    CHECK(!scene.edgeAt(cursor, solidar::kEdgeHitRadiusPx,
                        solidar::PickingQueryPrecision::Interactive));
    CHECK(scene.counters().uncertainVisible == uncertaintyBefore + 1);
    CHECK(scene.counters().occluderTests <= 4097);
    CHECK(scene.edgeAt(cursor, solidar::kEdgeHitRadiusPx,
                       solidar::PickingQueryPrecision::Exact));

    solidar::ViewportRuler ruler;
    ruler.begin();
    CHECK(ruler.updateHover(scene, cursor, true));
    CHECK(ruler.hoverPoint().has_value());
    const auto proven = *ruler.hoverPoint();
    CHECK(!ruler.updateHover(scene, cursor, false));
    CHECK(ruler.hoverPoint().has_value());
    CHECK(ruler.hoverPoint()->world.x == proven.world.x);
    CHECK(ruler.hoverPoint()->world.y == proven.world.y);
    CHECK(ruler.hoverPoint()->world.z == proven.world.z);
  }

  return EXIT_SUCCESS;
}
