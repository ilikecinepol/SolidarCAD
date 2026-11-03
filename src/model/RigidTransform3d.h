#pragma once

#include <array>
#include <optional>

namespace solidar {

enum class RigidTransform3dStatus {
  Valid,
  NonFiniteTranslation,
  NonFiniteRotation,
  DegenerateRotation,
};

struct RigidTransform3dCreateResult;

// Authoritative rigid placement from definition-local coordinates to its
// parent assembly. A translation plus a unit quaternion is deliberately the
// only representation: scale, shear and reflection cannot enter the domain.
//
// Quaternion canonicalization uses a 2^-48 component grid and guarantees
// exact C(C(q)) == C(q), C(-q) == C(q), and equal results for exact
// representable power-of-two rescaling that neither overflows nor underflows.
// Zeros are positive and the emitted norm differs from one by at most ordinary
// binary64 rounding. The grid plus fixed-point margin bounds rotation error to
// the order of 1e-13 radians. Inputs that were mathematically on the same ray
// but cease to be exactly collinear after independent IEEE-754 rounding are
// not guaranteed to have byte-identical canonical forms.
class RigidTransform3d final {
 public:
  [[nodiscard]] static RigidTransform3d identity() noexcept;
  [[nodiscard]] static RigidTransform3dCreateResult create(
      std::array<double, 3> translationMm,
      std::array<double, 4> quaternionWxyz) noexcept;
  // Composition from child-local coordinates to the parent's parent:
  // compose(parent, local) == parent ∘ local. The result always passes
  // through create(), so persistence canonicalization and finite checks stay
  // identical to authored transforms.
  [[nodiscard]] static RigidTransform3dCreateResult compose(
      const RigidTransform3d& parent,
      const RigidTransform3d& local) noexcept;

  [[nodiscard]] const std::array<double, 3>& translationMm() const noexcept {
    return translationMm_;
  }
  [[nodiscard]] const std::array<double, 4>& quaternionWxyz() const noexcept {
    return quaternionWxyz_;
  }

  friend bool operator==(const RigidTransform3d&,
                         const RigidTransform3d&) = default;

 private:
  RigidTransform3d(std::array<double, 3> translationMm,
                   std::array<double, 4> quaternionWxyz) noexcept
      : translationMm_(translationMm), quaternionWxyz_(quaternionWxyz) {}

  std::array<double, 3> translationMm_{};
  std::array<double, 4> quaternionWxyz_{1.0, 0.0, 0.0, 0.0};
};

struct RigidTransform3dCreateResult {
  RigidTransform3dStatus status{RigidTransform3dStatus::DegenerateRotation};
  std::optional<RigidTransform3d> transform;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == RigidTransform3dStatus::Valid && transform.has_value();
  }
};

}  // namespace solidar
