#include "ui/ManipulatorLayout.h"

#include <QLineF>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace solidar {
namespace {
bool conflicts(const QRectF& rect, const std::vector<QRectF>& exclusions) {
  return std::any_of(exclusions.begin(), exclusions.end(),
                     [&](const QRectF& other) { return rect.intersects(other); });
}

std::optional<QPointF> normalizeSafe(QPointF value) {
  if (!std::isfinite(value.x()) || !std::isfinite(value.y()))
    return std::nullopt;
  const double magnitude = QLineF({}, value).length();
  if (magnitude < 1e-9) return std::nullopt;
  return value / magnitude;
}
}  // namespace

StableProjectedDirection stableProjectedDirection(
    QPointF semanticDirection, QPointF fallbackDirection,
    double nearEndOnThresholdPx) {
  const double threshold = std::max(1e-9, nearEndOnThresholdPx);
  if (std::isfinite(semanticDirection.x()) &&
      std::isfinite(semanticDirection.y())) {
    const double magnitude = QLineF({}, semanticDirection).length();
    if (magnitude >= threshold)
      return {semanticDirection / magnitude, false};
  }
  if (const auto fallback = normalizeSafe(fallbackDirection))
    return {*fallback, true};
  return {{0.0, -1.0}, true};
}

ManipulatorLayoutResult computeManipulatorLayout(
    const ManipulatorLayoutInput& input, const ManipulatorStyle& style) {
  const auto stable = stableProjectedDirection(
      input.semanticDirection, QPointF(0.0, -1.0), style.nearEndOnThresholdPx);
  const QPointF direction = stable.normalizedDirection;
  const QRectF blocked = input.bodySilhouette.adjusted(
      -style.bodyClearance, -style.bodyClearance, style.bodyClearance,
      style.bodyClearance);
  double bestScore = -std::numeric_limits<double>::max();
  ManipulatorLayoutResult result;
  for (double sign : {1.0, -1.0}) {
    const double length = std::clamp(std::abs(input.semanticLengthPx),
                                     style.minimumLength,
                                     style.maximumLength);
    const QPointF handle = input.anchor + direction * (sign * length);
    double score = blocked.contains(handle) ? 0.0 : 1000.0;
    score += input.viewport.adjusted(10, 10, -10, -10).contains(handle) ? 200.0 : -500.0;
    // In screen coordinates positive Y points down. Prefer that presentation
    // when both directions are otherwise equally usable, without coupling the
    // parameter math itself to the screen Y axis.
    score += direction.y() * sign > 1e-6 ? 80.0 : 0.0;
    score += sign > 0.0 ? 1.0 : 0.0;
    for (const QRectF& exclusion : input.exclusions)
      if (exclusion.contains(handle)) score -= 800.0;
    if (score > bestScore) {
      bestScore = score;
      result = {input.anchor, handle, {}, sign, length, direction,
                stable.usedFallback};
    }
  }

  const std::array<QPointF, 4> offsets{{
      {style.hudClearance, -input.hudSize.height() - style.hudClearance},
      {style.hudClearance, style.hudClearance},
      {-input.hudSize.width() - style.hudClearance,
       -input.hudSize.height() - style.hudClearance},
      {-input.hudSize.width() - style.hudClearance, style.hudClearance}}};
  QRectF usable = input.viewport.adjusted(4, 4, -4, -4);
  for (const QPointF& offset : offsets) {
    QRectF candidate(result.handle + offset, input.hudSize);
    if (usable.contains(candidate) && !candidate.intersects(blocked) &&
        !conflicts(candidate, input.exclusions)) {
      result.hudTopLeft = candidate.topLeft();
      return result;
    }
  }
  result.hudTopLeft = QPointF(
      std::clamp(result.handle.x() + style.hudClearance, usable.left(),
                 usable.right() - input.hudSize.width()),
      std::clamp(result.handle.y() + style.hudClearance, usable.top(),
                 usable.bottom() - input.hudSize.height()));
  return result;
}

double safeAngularManipulatorRadius(QPointF origin, double requestedRadiusPx,
                                    const QRectF& bodySilhouette,
                                    double clearancePx, double minimumPx,
                                    double maximumPx) {
  const double ceiling = std::max(minimumPx, maximumPx);
  double radius = std::max(requestedRadiusPx, minimumPx);
  if (bodySilhouette.contains(origin)) {
    const double edgeDistance = std::max(
        {origin.x() - bodySilhouette.left(), bodySilhouette.right() - origin.x(),
         origin.y() - bodySilhouette.top(), bodySilhouette.bottom() - origin.y()});
    radius = std::max(radius, edgeDistance + clearancePx);
  }
  // Keep the arc readable: the inside-silhouette expansion must not grow the
  // visual radius without bound.
  return std::clamp(radius, minimumPx, ceiling);
}

AngularVisualRadius computeAngularVisualRadius(
    QPointF origin, QPointF uPoint, QPointF vPoint, double radiusMm,
    const QRectF& bodySilhouette, const ManipulatorStyle& style) {
  // Bound by the larger of the two projected basis extents. Using only u lets a
  // near-collapsed u (with a visible v) enlarge the world radius so the
  // v-extent far exceeds the style ceiling; the larger extent bounds the whole
  // projected arc/handle envelope to the clamped safe radius.
  const double uPx = QLineF(origin, uPoint).length();
  const double vPx = QLineF(origin, vPoint).length();
  const double requestedPx = std::max(uPx, vPx);
  const double safePx = safeAngularManipulatorRadius(
      origin, requestedPx, bodySilhouette, style.angularClearancePx,
      style.minimumAngularRadiusPx, style.maximumAngularRadiusPx);
  const double visualRadiusMm =
      requestedPx > 1e-6 ? radiusMm * safePx / requestedPx : radiusMm;
  return {visualRadiusMm, safePx};
}

}  // namespace solidar
