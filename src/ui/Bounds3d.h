#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "model/SketchPlacement.h"

namespace solidar {

// Small Qt/OCCT-free axis-aligned bounds value. Construction validates all
// derived values as well as the endpoints, so every published instance can
// expose center/diagonal/corners without a second failure channel.
class Bounds3d final {
 public:
  [[nodiscard]] static std::optional<Bounds3d> create(
      Point3d minimum, Point3d maximum) noexcept {
    const auto finite = [](Point3d point) {
      return std::isfinite(point.x) && std::isfinite(point.y) &&
             std::isfinite(point.z);
    };
    if (!finite(minimum) || !finite(maximum) || maximum.x < minimum.x ||
        maximum.y < minimum.y || maximum.z < minimum.z)
      return std::nullopt;
    const double dx = maximum.x - minimum.x;
    const double dy = maximum.y - minimum.y;
    const double dz = maximum.z - minimum.z;
    const Point3d center{minimum.x * 0.5 + maximum.x * 0.5,
                         minimum.y * 0.5 + maximum.y * 0.5,
                         minimum.z * 0.5 + maximum.z * 0.5};
    const double diagonal = std::hypot(std::hypot(dx, dy), dz);
    if (!finite(center) || !std::isfinite(dx) || !std::isfinite(dy) ||
        !std::isfinite(dz) || !std::isfinite(diagonal))
      return std::nullopt;
    return Bounds3d(minimum, maximum, center, diagonal);
  }

  [[nodiscard]] Point3d minimum() const noexcept { return minimum_; }
  [[nodiscard]] Point3d maximum() const noexcept { return maximum_; }
  [[nodiscard]] Point3d center() const noexcept { return center_; }
  [[nodiscard]] double diagonal() const noexcept { return diagonal_; }

  [[nodiscard]] std::array<Point3d, 8> corners() const noexcept {
    return {{{minimum_.x, minimum_.y, minimum_.z},
             {maximum_.x, minimum_.y, minimum_.z},
             {minimum_.x, maximum_.y, minimum_.z},
             {maximum_.x, maximum_.y, minimum_.z},
             {minimum_.x, minimum_.y, maximum_.z},
             {maximum_.x, minimum_.y, maximum_.z},
             {minimum_.x, maximum_.y, maximum_.z},
             {maximum_.x, maximum_.y, maximum_.z}}};
  }

  [[nodiscard]] std::optional<Bounds3d> united(
      const Bounds3d& other) const noexcept {
    return create({std::min(minimum_.x, other.minimum_.x),
                   std::min(minimum_.y, other.minimum_.y),
                   std::min(minimum_.z, other.minimum_.z)},
                  {std::max(maximum_.x, other.maximum_.x),
                   std::max(maximum_.y, other.maximum_.y),
                   std::max(maximum_.z, other.maximum_.z)});
  }

  friend bool operator==(const Bounds3d&, const Bounds3d&) = default;

 private:
  Bounds3d(Point3d minimum, Point3d maximum, Point3d center,
           double diagonal) noexcept
      : minimum_(minimum),
        maximum_(maximum),
        center_(center),
        diagonal_(diagonal) {}

  Point3d minimum_{};
  Point3d maximum_{};
  Point3d center_{};
  double diagonal_{};
};

}  // namespace solidar
