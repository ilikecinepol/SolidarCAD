#pragma once

#include <optional>

#include "ui/Bounds3d.h"

namespace solidar::detail {

struct ViewportFitGeometry final {
  double zoom{1.0};
  double panX{};
  double panY{};
  Point3d center{};
  double depthExtent{1.0};
  bool fullyContained{};
  bool minimumZoomClamped{};
  bool maximumZoomClamped{};
};

[[nodiscard]] std::optional<ViewportFitGeometry> calculateViewportFitGeometry(
    const Bounds3d& bounds, int logicalWidth, int logicalHeight,
    float yawDeg, float pitchDeg, double contentFraction) noexcept;

}  // namespace solidar::detail
