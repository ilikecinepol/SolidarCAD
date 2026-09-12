#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>

#include <vector>

namespace solidar {

struct ManipulatorStyle {
  double handleRadius{8.0};
  double shaftWidth{4.0};
  double arrowHeadLength{11.0};
  double arrowHeadWidth{9.0};
  double bodyClearance{18.0};
  double hudClearance{12.0};
  double minimumLength{36.0};
  double maximumLength{72.0};
  // Projected semantic directions below this screen-space magnitude (px) are
  // considered end-on/unreliable and replaced by a deterministic fallback.
  double nearEndOnThresholdPx{2.0};
  // Angular arc visual radius bounds (px): floor, then ceiling.
  double minimumAngularRadiusPx{34.0};
  double maximumAngularRadiusPx{200.0};
  double angularClearancePx{26.0};
  // A 5В° Draft angle otherwise produces only a few pixels of visible arc.
  // Keep the handle at the true angle, but draw at least this much trail so
  // the angular manipulator is obviously interactive.
  double minimumAngularSweepDeg{42.0};
  // Directional manipulators (e.g. Face Extrude) must never invert the drawn
  // arrow against the semantic geometry direction. When false the layout keeps
  // visualSign = +1 (no direction flip) and only moves the HUD independently.
  bool allowVisualDirectionFlip{true};
};

struct ManipulatorLayoutInput {
  QPointF anchor;
  QPointF semanticDirection;
  double semanticLengthPx{};
  QRectF bodySilhouette;
  QRectF viewport;
  QSizeF hudSize;
  std::vector<QRectF> exclusions;
};

struct ManipulatorLayoutResult {
  QPointF anchor;
  QPointF handle;
  QPointF hudTopLeft;
  double visualSign{1.0};
  double visualLengthPx{};
  // Normalized screen direction actually used for the drawn arrow (before the
  // visualSign is applied), so hit-testing can share the exact same axis.
  QPointF direction{0.0, -1.0};
  bool usedFallback{false};
};

struct StableProjectedDirection {
  QPointF normalizedDirection{0.0, -1.0};
  bool usedFallback{false};
};

struct AngularVisualRadius {
  double visualRadiusMm{};
  double safeRadiusPx{};
};

// Deterministically resolves a semantic screen direction: normalize it when its
// magnitude reaches the end-on threshold, otherwise normalize the caller's
// fallback. Non-finite inputs always take the fallback. This eliminates the
// chaotic flip that a near-zero (end-on) semantic projection otherwise causes.
[[nodiscard]] StableProjectedDirection stableProjectedDirection(
    QPointF semanticDirection, QPointF fallbackDirection,
    double nearEndOnThresholdPx);

[[nodiscard]] ManipulatorLayoutResult computeManipulatorLayout(
    const ManipulatorLayoutInput& input,
    const ManipulatorStyle& style = {});

// Single source of the angular visual arc radius: applies the floor/ceiling and
// silhouette expansion, then converts the resulting screen radius back to
// world millimeters so draw and hit-test never diverge. The requested radius is
// derived from BOTH projected basis directions (uPoint/vPoint are the projected
// positions of origin + u*radiusMm and origin + v*radiusMm), so a near-collapsed
// u cannot inflate the world radius past the ceiling while v stays visible.
[[nodiscard]] AngularVisualRadius computeAngularVisualRadius(
    QPointF origin, QPointF uPoint, QPointF vPoint, double radiusMm,
    const QRectF& bodySilhouette, const ManipulatorStyle& style = {});

[[nodiscard]] double safeAngularManipulatorRadius(
    QPointF origin, double requestedRadiusPx, const QRectF& bodySilhouette,
    double clearancePx = 26.0, double minimumPx = 34.0,
    double maximumPx = 200.0);

}  // namespace solidar
