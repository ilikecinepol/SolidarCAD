#include "model/RigidTransform3d.h"

#include <algorithm>
#include <cmath>

namespace solidar {
namespace {

constexpr double kStabilizationScale = 281474976710656.0;  // 2^48
constexpr double kStabilizationQuantum = 1.0 / kStabilizationScale;
constexpr double kPivotMargin = 4.0 * kStabilizationQuantum;
constexpr std::size_t kMaximumPivotAdjustments = 32;

double positiveZero(double value) noexcept {
  return value == 0.0 ? 0.0 : value;
}

double stabilizedComponent(double value) noexcept {
  // Four guard bits below binary64's significand absorb the last-bit drift
  // introduced by an otherwise equivalent positive scale while retaining
  // substantially more precision than persistence and CAD tolerances need.
  return std::round(value * kStabilizationScale) / kStabilizationScale;
}

std::size_t selectStablePivot(
    const std::array<double, 4>& quaternionWxyz) noexcept {
  std::size_t pivot = 0;
  for (std::size_t index = 1; index < quaternionWxyz.size(); ++index) {
    if (std::abs(quaternionWxyz[index]) >
        std::abs(quaternionWxyz[pivot]))
      pivot = index;
  }
  return pivot;
}

double reduceMagnitudeOneQuantum(double value) noexcept {
  const double magnitude = std::abs(value);
  if (magnitude <= kStabilizationQuantum) return 0.0;
  return std::copysign(magnitude - kStabilizationQuantum, value);
}

}  // namespace

RigidTransform3d RigidTransform3d::identity() noexcept {
  return RigidTransform3d({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
}

RigidTransform3dCreateResult RigidTransform3d::create(
    std::array<double, 3> translationMm,
    std::array<double, 4> quaternionWxyz) noexcept {
  for (double& value : translationMm) {
    if (!std::isfinite(value))
      return {RigidTransform3dStatus::NonFiniteTranslation, std::nullopt};
    value = positiveZero(value);
  }

  double maximum = 0.0;
  for (const double value : quaternionWxyz) {
    if (!std::isfinite(value))
      return {RigidTransform3dStatus::NonFiniteRotation, std::nullopt};
    maximum = std::max(maximum, std::abs(value));
  }
  if (maximum == 0.0)
    return {RigidTransform3dStatus::DegenerateRotation, std::nullopt};

  std::array<double, 4> scaled{};
  double scaledNormSquared = 0.0;
  for (std::size_t index = 0; index < quaternionWxyz.size(); ++index) {
    scaled[index] = quaternionWxyz[index] / maximum;
    scaledNormSquared += scaled[index] * scaled[index];
  }
  const double scaledNorm = std::sqrt(scaledNormSquared);
  if (!std::isfinite(scaledNorm) || scaledNorm == 0.0)
    return {RigidTransform3dStatus::DegenerateRotation, std::nullopt};
  for (std::size_t index = 0; index < quaternionWxyz.size(); ++index)
    quaternionWxyz[index] = scaled[index] / scaledNorm;

  // Select the reconstruction pivot from the stabilized direction, not from
  // normalization round-off.  Equal (and quantization-equivalent near-equal)
  // maxima therefore retain the lowest-index pivot across positive scaling
  // and repeated canonicalization.
  for (double& component : quaternionWxyz)
    component = stabilizedComponent(component);
  const std::size_t pivot = selectStablePivot(quaternionWxyz);
  bool fixedPointEstablished = false;
  for (std::size_t adjustment = 0;
       adjustment <= kMaximumPivotAdjustments; ++adjustment) {
    double remainingSquared = 1.0;
    for (std::size_t index = 0; index < quaternionWxyz.size(); ++index) {
      if (index == pivot) continue;
      remainingSquared = std::fma(-quaternionWxyz[index],
                                  quaternionWxyz[index], remainingSquared);
    }
    if (remainingSquared < 0.0) remainingSquared = 0.0;
    quaternionWxyz[pivot] =
        std::copysign(std::sqrt(remainingSquared), quaternionWxyz[pivot]);

    std::size_t competitor = pivot == 0 ? 1 : 0;
    for (std::size_t index = 0; index < quaternionWxyz.size(); ++index) {
      if (index == pivot) continue;
      if (std::abs(quaternionWxyz[index]) >
          std::abs(quaternionWxyz[competitor]))
        competitor = index;
    }
    if (std::abs(quaternionWxyz[pivot]) >=
        std::abs(quaternionWxyz[competitor]) + kPivotMargin) {
      fixedPointEstablished = true;
      break;
    }
    if (adjustment == kMaximumPivotAdjustments) break;

    // Move the largest competitor inward by one exact grid quantum. The raw
    // four-quantum margin absorbs the normalization/stabilization round-off on
    // the next create() call, making the emitted representation a fixed point.
    quaternionWxyz[competitor] =
        reduceMagnitudeOneQuantum(quaternionWxyz[competitor]);
  }
  // For a normalized binary64 quaternion the reconstruction error is bounded
  // by a few grid quanta, so at most 32 one-quantum adjustments are sufficient.
  // Keep a defensive failure path if that numerical invariant is ever broken.
  if (!fixedPointEstablished)
    return {RigidTransform3dStatus::DegenerateRotation, std::nullopt};

  for (const double value : quaternionWxyz) {
    if (value == 0.0) continue;
    if (std::signbit(value)) {
      for (double& component : quaternionWxyz) component = -component;
    }
    break;
  }
  for (double& value : quaternionWxyz) value = positiveZero(value);

  return {RigidTransform3dStatus::Valid,
          RigidTransform3d(translationMm, quaternionWxyz)};
}

RigidTransform3dCreateResult RigidTransform3d::compose(
    const RigidTransform3d& parent,
    const RigidTransform3d& local) noexcept {
  const auto& p = parent.quaternionWxyz();
  const auto& q = local.quaternionWxyz();
  const std::array<double, 4> rotation{
      p[0] * q[0] - p[1] * q[1] - p[2] * q[2] - p[3] * q[3],
      p[0] * q[1] + p[1] * q[0] + p[2] * q[3] - p[3] * q[2],
      p[0] * q[2] - p[1] * q[3] + p[2] * q[0] + p[3] * q[1],
      p[0] * q[3] + p[1] * q[2] - p[2] * q[1] + p[3] * q[0]};
  const auto& v = local.translationMm();
  const std::array<double, 3> u{p[1], p[2], p[3]};
  const std::array<double, 3> uv{
      u[1] * v[2] - u[2] * v[1],
      u[2] * v[0] - u[0] * v[2],
      u[0] * v[1] - u[1] * v[0]};
  const std::array<double, 3> uuv{
      u[1] * uv[2] - u[2] * uv[1],
      u[2] * uv[0] - u[0] * uv[2],
      u[0] * uv[1] - u[1] * uv[0]};
  const auto& t = parent.translationMm();
  std::array<double, 3> translation{};
  for (std::size_t index = 0; index < translation.size(); ++index)
    translation[index] = t[index] + v[index] +
                         2.0 * (p[0] * uv[index] + uuv[index]);
  return create(translation, rotation);
}

}  // namespace solidar
