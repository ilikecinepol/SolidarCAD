#pragma once

#include <algorithm>
#include <cmath>

namespace solidar {

enum class PrincipalAxis { X, Y, Z };
enum class MirrorPlane { XY, XZ, YZ };
enum class PatternOperation { NewBody, Join };

inline constexpr int kMinimumPatternCount = 2;
inline constexpr int kMaximumPatternCount = 100;
inline constexpr double kMinimumPatternParameter = 0.01;
inline constexpr double kMaximumPatternSpacingMm = 100000.0;
inline constexpr double kMaximumPatternAngleDeg = 360.0;

[[nodiscard]] constexpr bool validPrincipalAxis(PrincipalAxis value) noexcept {
  return value == PrincipalAxis::X || value == PrincipalAxis::Y ||
         value == PrincipalAxis::Z;
}

[[nodiscard]] constexpr bool validMirrorPlane(MirrorPlane value) noexcept {
  return value == MirrorPlane::XY || value == MirrorPlane::XZ ||
         value == MirrorPlane::YZ;
}

[[nodiscard]] constexpr bool
validPatternOperation(PatternOperation value) noexcept {
  return value == PatternOperation::NewBody || value == PatternOperation::Join;
}

[[nodiscard]] constexpr bool validPatternCount(int count) noexcept {
  return count >= kMinimumPatternCount && count <= kMaximumPatternCount;
}

[[nodiscard]] inline bool validPatternSpacing(double spacingMm) noexcept {
  return std::isfinite(spacingMm) &&
         spacingMm >= kMinimumPatternParameter &&
         spacingMm <= kMaximumPatternSpacingMm;
}

[[nodiscard]] inline bool validPatternAngle(double angleDeg) noexcept {
  return std::isfinite(angleDeg) &&
         angleDeg >= kMinimumPatternParameter &&
         angleDeg <= kMaximumPatternAngleDeg;
}

[[nodiscard]] constexpr int clampPatternCountForUi(int count) noexcept {
  return std::clamp(count, kMinimumPatternCount, kMaximumPatternCount);
}

} // namespace solidar
