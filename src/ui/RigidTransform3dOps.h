#pragma once

#include <array>
#include <cmath>
#include <optional>

#include "model/RigidTransform3d.h"
#include "model/SketchPlacement.h"

namespace solidar {

// Authoritative row-major WXYZ rigid matrix shared by CPU world-bounds and
// the OpenGL renderer. Keeping the formula here prevents the two paths from
// silently adopting different quaternion conventions.
[[nodiscard]] inline std::optional<std::array<double, 16>>
rigidTransformMatrix(const RigidTransform3d& transform) noexcept {
  const auto& translation = transform.translationMm();
  const auto& q = transform.quaternionWxyz();
  for (const double value : translation)
    if (!std::isfinite(value)) return std::nullopt;
  for (const double value : q)
    if (!std::isfinite(value)) return std::nullopt;
  const double w = q[0];
  const double x = q[1];
  const double y = q[2];
  const double z = q[3];
  std::array<double, 16> values{
      1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w),
      2.0 * (x * z + y * w), translation[0],
      2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z),
      2.0 * (y * z - x * w), translation[1],
      2.0 * (x * z - y * w), 2.0 * (y * z + x * w),
      1.0 - 2.0 * (x * x + y * y), translation[2],
      0.0, 0.0, 0.0, 1.0};
  for (const double value : values)
    if (!std::isfinite(value)) return std::nullopt;
  return values;
}

[[nodiscard]] inline std::optional<Point3d> transformPoint(
    const std::array<double, 16>& matrix, Point3d point) noexcept {
  const Point3d transformed{
      std::fma(matrix[0], point.x,
               std::fma(matrix[1], point.y,
                        std::fma(matrix[2], point.z, matrix[3]))),
      std::fma(matrix[4], point.x,
               std::fma(matrix[5], point.y,
                        std::fma(matrix[6], point.z, matrix[7]))),
      std::fma(matrix[8], point.x,
               std::fma(matrix[9], point.y,
                        std::fma(matrix[10], point.z, matrix[11])))};
  if (!std::isfinite(transformed.x) || !std::isfinite(transformed.y) ||
      !std::isfinite(transformed.z))
    return std::nullopt;
  return transformed;
}

[[nodiscard]] inline std::optional<Point3d> transformPoint(
    const RigidTransform3d& transform, Point3d point) noexcept {
  const auto matrix = rigidTransformMatrix(transform);
  return matrix ? transformPoint(*matrix, point) : std::nullopt;
}

}  // namespace solidar
