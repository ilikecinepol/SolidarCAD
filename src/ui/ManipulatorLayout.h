#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>

#include <vector>

namespace solidar {

struct ManipulatorStyle {
  double handleRadius{7.0};
  double shaftWidth{3.0};
  double arrowHeadLength{12.0};
  double arrowHeadWidth{8.0};
  double bodyClearance{26.0};
  double hudClearance{12.0};
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
