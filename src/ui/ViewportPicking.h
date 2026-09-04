#pragma once

#include <QPointF>

#include <optional>

namespace solidar {

inline constexpr double kEdgeHitRadiusPx = 7.0;
inline constexpr double kVertexHitRadiusPx = 9.0;
inline constexpr double kDepthEpsilonScale = 1e-4;

struct ProjectedPoint {
  QPointF screen;
  double depth{};
};

struct SegmentHit {
  double distance{};
  double parameter{};
  double depth{};
};

[[nodiscard]] std::optional<double> triangleDepthAt(
    QPointF point, const ProjectedPoint& a, const ProjectedPoint& b,
    const ProjectedPoint& c);
[[nodiscard]] SegmentHit closestSegmentHit(
    QPointF point, const ProjectedPoint& a, const ProjectedPoint& b);

}  // namespace solidar
