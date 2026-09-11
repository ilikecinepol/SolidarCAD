#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "ui/ViewportPicking.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

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

    // Edge collection: a coplanar (visible) edge is selected; a rear edge
    // behind the front surface is rejected.
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
      rear.points = {{{-10.0, 50.0}, 1.0}, {{110.0, 50.0}, 1.0}};

      std::vector<ProjectedEdge> edges{coplanar, rear};
      const auto result = solidar::collectEdgesInRect(
          surface, edges, QRectF(0.0, 0.0, 100.0, 100.0), 1e-6);
      CHECK(std::find(result.begin(), result.end(), 11u) != result.end());
      CHECK(std::find(result.begin(), result.end(), 12u) == result.end());
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
  }

  return EXIT_SUCCESS;
}
