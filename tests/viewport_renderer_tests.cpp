#include "ui/ViewportRenderer.h"

#include <QVector4D>

#include <cassert>
#include <cmath>
#include <numbers>

int main() {
  const QSize size(1000, 800);
  const QPointF pan(17.0, -11.0);
  const auto matrix = solidar::ViewportRenderer::projectionMatrix(
      size, -35.0F, 25.0F, 1.4F, pan, 500.0);
  const QVector4D world(13.0F, -7.0F, 22.0F, 1.0F);
  const QVector4D clip = matrix * world;

  const double yaw = -35.0 * std::numbers::pi / 180.0;
  const double pitch = 25.0 * std::numbers::pi / 180.0;
  const double x1 = world.x() * std::cos(yaw) - world.y() * std::sin(yaw);
  const double y1 = world.x() * std::sin(yaw) + world.y() * std::cos(yaw);
  const double y2 = y1 * std::cos(pitch) - world.z() * std::sin(pitch);
  const double scale = 800.0 * 0.008 * 1.4;
  const double expectedX = 500.0 + x1 * scale + pan.x();
  const double expectedY = 800.0 * 0.52 + y2 * scale + pan.y();
  const double actualX = (clip.x() + 1.0) * 500.0;
  const double actualY = (1.0 - clip.y()) * 400.0;
  assert(std::abs(actualX - expectedX) < 1e-3);
  assert(std::abs(actualY - expectedY) < 1e-3);
}
