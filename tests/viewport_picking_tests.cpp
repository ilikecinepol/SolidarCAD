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
  return EXIT_SUCCESS;
}
