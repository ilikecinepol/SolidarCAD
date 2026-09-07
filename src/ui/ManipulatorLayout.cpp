#include "ui/ManipulatorLayout.h"

#include <QLineF>

#include <algorithm>
#include <array>
#include <limits>

namespace solidar {
namespace {
bool conflicts(const QRectF& rect, const std::vector<QRectF>& exclusions) {
  return std::any_of(exclusions.begin(), exclusions.end(),
                     [&](const QRectF& other) { return rect.intersects(other); });
}
}  // namespace

ManipulatorLayoutResult computeManipulatorLayout(
    const ManipulatorLayoutInput& input, const ManipulatorStyle& style) {
  QPointF direction = input.semanticDirection;
  const double magnitude = QLineF({}, direction).length();
  direction = magnitude > 1e-6 ? direction / magnitude : QPointF(0.0, -1.0);
  const QRectF blocked = input.bodySilhouette.adjusted(
      -style.bodyClearance, -style.bodyClearance, style.bodyClearance,
      style.bodyClearance);
  double bestScore = -std::numeric_limits<double>::max();
  ManipulatorLayoutResult result;
  for (double sign : {1.0, -1.0}) {
    double length = std::clamp(
        std::max(std::abs(input.semanticLengthPx), style.preferredLength),
        style.minimumLength, style.maximumLength);
    QPointF handle = input.anchor + direction * (sign * length);
    int extensionAttempts = 0;
    for (; extensionAttempts < 8 && blocked.contains(handle) &&
           length < style.maximumLength;
         ++extensionAttempts) {
      length = std::min(style.maximumLength, length + style.bodyClearance);
      handle = input.anchor + direction * (sign * length);
    }
    double score = blocked.contains(handle) ? -10000.0 : 1000.0;
    score -= extensionAttempts * 120.0;
    score += input.viewport.adjusted(10, 10, -10, -10).contains(handle) ? 200.0 : -500.0;
    score += sign > 0.0 ? 1.0 : 0.0;
    for (const QRectF& exclusion : input.exclusions)
      if (exclusion.contains(handle)) score -= 800.0;
    if (score > bestScore) {
      bestScore = score;
      result = {input.anchor, handle, {}, sign, length};
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
                                    double clearancePx) {
  double radius = std::clamp(requestedRadiusPx, 40.0, 110.0);
  if (bodySilhouette.contains(origin)) {
    const double edgeDistance = std::max(
        {origin.x() - bodySilhouette.left(), bodySilhouette.right() - origin.x(),
         origin.y() - bodySilhouette.top(), bodySilhouette.bottom() - origin.y()});
    radius = std::min(110.0, std::max(radius, edgeDistance + clearancePx));
  }
  return radius;
}

double linearDragValue(const LinearManipulatorDragContext& context,
                       QPointF mouse) {
  const QPointF axis = context.screenAxis;
  const double length = QLineF({}, axis).length();
  if (length <= 1e-9 || context.pixelsPerUnit <= 1e-9)
    return std::clamp(context.pressValue, context.minValue, context.maxValue);
  const QPointF unitAxis = axis / length;
  const double pixels = QPointF::dotProduct(mouse - context.pressMouse,
                                            unitAxis);
  const double raw = context.pressValue +
                     pixels * context.visualSign / context.pixelsPerUnit;
  return std::clamp(raw, context.minValue, context.maxValue);
}

double signedShortestAngleDelta(double previousDeg, double currentDeg) {
  double delta = std::fmod(currentDeg - previousDeg + 540.0, 360.0) - 180.0;
  return delta;
}

double angularDragValue(AngularManipulatorDragContext& context,
                        double currentMouseAngleDeg) {
  context.accumulatedDelta +=
      signedShortestAngleDelta(context.previousMouseAngle,
                               currentMouseAngleDeg);
  context.previousMouseAngle = currentMouseAngleDeg;
  return std::clamp(context.pressAngle + context.accumulatedDelta,
                    context.minValue, context.maxValue);
}

}  // namespace solidar
