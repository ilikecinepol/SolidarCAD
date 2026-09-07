#include "ui/ViewportCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace solidar {

QMatrix4x4 ViewportCameraState::worldToClip() const {
  const double w = std::max(1, logicalSize.width());
  const double h = std::max(1, logicalSize.height());
  const double scale = std::min(w, h) * 0.008 * zoom;
  const double yaw = yawDeg * std::numbers::pi / 180.0;
  const double pitch = pitchDeg * std::numbers::pi / 180.0;
  const double cy = std::cos(yaw), sy = std::sin(yaw);
  const double cp = std::cos(pitch), sp = std::sin(pitch);
  const double dz = std::max(1.0, depthExtent);
  QMatrix4x4 matrix;
  matrix.fill(0.0F);
  matrix(0, 0) = static_cast<float>(2.0 * scale * cy / w);
  matrix(0, 1) = static_cast<float>(-2.0 * scale * sy / w);
  matrix(0, 3) = static_cast<float>(2.0 * pan.x() / w);
  matrix(1, 0) = static_cast<float>(-2.0 * scale * sy * cp / h);
  matrix(1, 1) = static_cast<float>(-2.0 * scale * cy * cp / h);
  matrix(1, 2) = static_cast<float>(2.0 * scale * sp / h);
  matrix(1, 3) = static_cast<float>(-0.04 - 2.0 * pan.y() / h);
  matrix(2, 0) = static_cast<float>(-sy * sp / dz);
  matrix(2, 1) = static_cast<float>(-cy * sp / dz);
  matrix(2, 2) = static_cast<float>(-cp / dz);
  const double centerDepth =
      (center.x * sy + center.y * cy) * sp + center.z * cp;
  matrix(2, 3) = static_cast<float>(centerDepth / dz);
  matrix(3, 3) = 1.0F;
  return matrix;
}

QPointF ViewportCameraState::worldToScreen(Point3d point) const {
  const QVector4D clip = worldToClip() *
                         QVector4D(point.x, point.y, point.z, 1.0F);
  return {(clip.x() + 1.0) * logicalSize.width() * 0.5,
          (1.0 - clip.y()) * logicalSize.height() * 0.5};
}

double ViewportCameraState::cameraDepth(Point3d point) const {
  const double yaw = yawDeg * std::numbers::pi / 180.0;
  const double pitch = pitchDeg * std::numbers::pi / 180.0;
  const double y = point.x * std::sin(yaw) + point.y * std::cos(yaw);
  return y * std::sin(pitch) + point.z * std::cos(pitch);
}

ViewportDepthRange ViewportDepthRange::combined(
    Point3d sourceCenter, double sourceDiagonal, Point3d previewCenter,
    double previewDiagonal, bool hasPreview, double safetyFactor) {
  if (!hasPreview)
    return {sourceCenter, std::max(1.0, sourceDiagonal * safetyFactor)};
  const Point3d center{(sourceCenter.x + previewCenter.x) * 0.5,
                       (sourceCenter.y + previewCenter.y) * 0.5,
                       (sourceCenter.z + previewCenter.z) * 0.5};
  const double dx = previewCenter.x - sourceCenter.x;
  const double dy = previewCenter.y - sourceCenter.y;
  const double dz = previewCenter.z - sourceCenter.z;
  const double unionDiameter = std::sqrt(dx * dx + dy * dy + dz * dz) +
                               (sourceDiagonal + previewDiagonal) * 0.5;
  return {center, std::max(1.0, unionDiameter * safetyFactor)};
}

}  // namespace solidar
