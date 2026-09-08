#pragma once

#include <QPointF>

#include <optional>

namespace solidar {

inline constexpr double kEdgeHitRadiusPx = 9.0;
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

struct LinearDragSnapshot {
  QPointF cursorStart;
  QPointF projectedUnitAxis;
  double initialValue{};
};

[[nodiscard]] double linearValueFromDrag(const LinearDragSnapshot& drag,
                                         QPointF cursor,
                                         double minimum = 0.0,
                                         double maximum = 100000.0);

[[nodiscard]] std::optional<double> triangleDepthAt(
    QPointF point, const ProjectedPoint& a, const ProjectedPoint& b,
    const ProjectedPoint& c);
[[nodiscard]] SegmentHit closestSegmentHit(
    QPointF point, const ProjectedPoint& a, const ProjectedPoint& b);
// Resolves a cursor against the projected basis of a 3D angular manipulator.
// The range is (0, 360], so positive U represents a complete revolution.
[[nodiscard]] double angularValueFromProjectedBasis(
    QPointF cursor, QPointF origin, QPointF uPoint, QPointF vPoint);

}  // namespace solidar
