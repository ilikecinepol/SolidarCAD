#pragma once

#include <QLineF>
#include <QPainterPath>
#include <QPointF>

#include <vector>

namespace solidar {

// Builds the longitudinal outline of the screen-space Extrude preview.
// Polygonal contours expose every vertex edge so each side face remains
// readable. Curved contours keep only their two silhouette generators and
// therefore do not turn circles/arcs into vertical hatching.
[[nodiscard]] std::vector<QLineF> extrusionPreviewGenerators(
    const QPainterPath& basePath, QPointF offset, bool curvedBoundary);

}  // namespace solidar
