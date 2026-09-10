#include <cmath>
#include <cstdlib>
#include <iostream>

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
  CHECK(std::abs(solidar::angularValueFromProjectedBasis(
                     angularV, angularOrigin, angularU, angularV) -
                 90.0) < 1e-9);
  CHECK(std::abs(solidar::angularValueFromProjectedBasis(
                     {60.0, 100.0}, angularOrigin, angularU, angularV) -
                 180.0) < 1e-9);
  CHECK(std::abs(solidar::angularValueFromProjectedBasis(
                     angularU, angularOrigin, angularU, angularV) -
                 360.0) < 1e-9);
  CHECK(solidar::angularValueFromProjectedBasis(
            {140.0, 99.99}, angularOrigin, angularU, angularV) > 359.0);
  CHECK(solidar::angularValueFromProjectedBasis(
            {140.0, 100.01}, angularOrigin, angularU, angularV) < 1.0);

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

  return EXIT_SUCCESS;
}
