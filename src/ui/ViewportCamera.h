#pragma once

#include <QMatrix4x4>
#include <QPointF>
#include <QSize>

#include "model/SketchPlacement.h"

namespace solidar {

struct ViewportCameraState {
  float yawDeg{-35.0F};
  float pitchDeg{25.0F};
  float zoom{1.0F};
  QPointF pan;
  QSize logicalSize;
  float devicePixelRatio{1.0F};
  Point3d center{};
  double depthExtent{1.0};

  [[nodiscard]] QMatrix4x4 worldToClip() const;
  [[nodiscard]] QPointF worldToScreen(Point3d point) const;
  [[nodiscard]] double cameraDepth(Point3d point) const;
};

struct ViewportDepthRange {
  Point3d center{};
  double extent{1.0};

  [[nodiscard]] static ViewportDepthRange combined(
      Point3d sourceCenter, double sourceDiagonal, Point3d previewCenter,
      double previewDiagonal, bool hasPreview, double safetyFactor = 3.0);
};

}  // namespace solidar
