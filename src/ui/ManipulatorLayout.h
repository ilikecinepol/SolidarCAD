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
};

[[nodiscard]] ManipulatorLayoutResult computeManipulatorLayout(
    const ManipulatorLayoutInput& input,
    const ManipulatorStyle& style = {});
[[nodiscard]] double safeAngularManipulatorRadius(
    QPointF origin, double requestedRadiusPx, const QRectF& bodySilhouette,
    double clearancePx = 26.0);

}  // namespace solidar
