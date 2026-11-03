#include "ui/internal/ViewportFitGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "ui/ViewportCamera.h"

namespace solidar::detail {

std::optional<ViewportFitGeometry> calculateViewportFitGeometry(
    const Bounds3d& bounds, int logicalWidth, int logicalHeight,
    float yawDeg, float pitchDeg, double contentFraction) noexcept {
  if (logicalWidth <= 0 || logicalHeight <= 0 || !std::isfinite(yawDeg) ||
      !std::isfinite(pitchDeg) || !std::isfinite(contentFraction) ||
      contentFraction <= 0.0 || contentFraction > 1.0)
    return std::nullopt;

  const Point3d center = bounds.center();
  const double radians = std::numbers::pi / 180.0;
  const double yaw = static_cast<double>(yawDeg) * radians;
  const double pitch = static_cast<double>(pitchDeg) * radians;
  const double cy = std::cos(yaw);
  const double sy = std::sin(yaw);
  const double cp = std::cos(pitch);
  const double sp = std::sin(pitch);
  if (!std::isfinite(cy) || !std::isfinite(sy) || !std::isfinite(cp) ||
      !std::isfinite(sp))
    return std::nullopt;

  double minimumU = std::numeric_limits<double>::infinity();
  double maximumU = -std::numeric_limits<double>::infinity();
  double minimumV = std::numeric_limits<double>::infinity();
  double maximumV = -std::numeric_limits<double>::infinity();
  for (const Point3d corner : bounds.corners()) {
    const double x = corner.x - center.x;
    const double y = corner.y - center.y;
    const double z = corner.z - center.z;
    const double u = std::fma(cy, x, -sy * y);
    const double v =
        std::fma(-sy * cp, x, std::fma(-cy * cp, y, sp * z));
    if (!std::isfinite(u) || !std::isfinite(v)) return std::nullopt;
    minimumU = std::min(minimumU, u);
    maximumU = std::max(maximumU, u);
    minimumV = std::min(minimumV, v);
    maximumV = std::max(maximumV, v);
  }

  const double width = static_cast<double>(logicalWidth);
  const double height = static_cast<double>(logicalHeight);
  const double baseScale = std::min(width, height) * 0.008;
  const double spanU = maximumU - minimumU;
  const double spanV = maximumV - minimumV;
  if (!std::isfinite(baseScale) || baseScale <= 0.0 ||
      !std::isfinite(spanU) || !std::isfinite(spanV))
    return std::nullopt;

  double requestedZoom = std::numeric_limits<double>::infinity();
  if (spanU > 0.0)
    requestedZoom = std::min(
        requestedZoom, contentFraction * width / (baseScale * spanU));
  if (spanV > 0.0)
    requestedZoom = std::min(
        requestedZoom, contentFraction * height / (baseScale * spanV));
  if (std::isinf(requestedZoom))
    requestedZoom = static_cast<double>(kMaximumViewportZoom) * 2.0;
  if (!std::isfinite(requestedZoom) || requestedZoom <= 0.0)
    return std::nullopt;
  const bool minimumClamped = requestedZoom < kMinimumViewportZoom;
  const bool maximumClamped = requestedZoom > kMaximumViewportZoom;
  const double zoom = std::clamp(
      requestedZoom, static_cast<double>(kMinimumViewportZoom),
      static_cast<double>(kMaximumViewportZoom));
  const double scale = baseScale * zoom;

  const double centerU = std::fma(cy, center.x, -sy * center.y);
  const double centerV = std::fma(
      -sy * cp, center.x,
      std::fma(-cy * cp, center.y, sp * center.z));
  const double panX = -scale * centerU;
  const double panY = scale * centerV;
  const double depthExtent = std::max(1.0, bounds.diagonal() * 3.0);
  const double maximumFloat =
      static_cast<double>(std::numeric_limits<float>::max());
  if (!std::isfinite(scale) || !std::isfinite(panX) ||
      !std::isfinite(panY) || std::abs(panX) > maximumFloat ||
      std::abs(panY) > maximumFloat || !std::isfinite(depthExtent))
    return std::nullopt;

  const double halfWidth = spanU * scale * 0.5;
  const double halfHeight = spanV * scale * 0.5;
  const double allowedHalfWidth = contentFraction * width * 0.5;
  const double allowedHalfHeight = contentFraction * height * 0.5;
  const double tolerance =
      std::max(width, height) * 64.0 * std::numeric_limits<double>::epsilon();
  const bool contained = halfWidth <= allowedHalfWidth + tolerance &&
                         halfHeight <= allowedHalfHeight + tolerance;

  return ViewportFitGeometry{zoom, panX, panY, center, depthExtent, contained,
                             minimumClamped, maximumClamped};
}

}  // namespace solidar::detail
