#include "ui/WorldGrid.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace solidar {
namespace {

constexpr std::size_t kMaxGridLines = 300;
constexpr double kOverscan = 1.4;
constexpr double kMinProjectedSpacingPx = 3.0;
constexpr double kTargetPixels = 40.0;

QColor axisColorForDirection(Vector3d direction, const WorldGridStyle& style) {
  const double ax = std::abs(direction.x);
  const double ay = std::abs(direction.y);
  const double az = std::abs(direction.z);
  QColor base;
  double dominant;
  if (ax >= ay && ax >= az) {
    base = style.axisX;
    dominant = direction.x;
  } else if (ay >= az) {
    base = style.axisY;
    dominant = direction.y;
  } else {
    base = style.axisZ;
    dominant = direction.z;
  }
  return dominant < 0.0 ? base.darker(130) : base;
}

}  // namespace

double chooseNiceGridSpacing(double pixelsPerMm, double targetPixels) {
  if (!std::isfinite(pixelsPerMm) || pixelsPerMm <= 1e-12) return 1.0;
  const double idealMm = targetPixels / pixelsPerMm;
  const double exponent = std::floor(std::log10(idealMm));
  double best = 1.0;
  double bestError = std::numeric_limits<double>::infinity();
  for (const double base : {1.0, 2.0, 5.0}) {
    for (const double e : {exponent - 1.0, exponent, exponent + 1.0}) {
      const double candidate = base * std::pow(10.0, e);
      if (!(candidate > 0.0) || !std::isfinite(candidate)) continue;
      const double error = std::abs(candidate * pixelsPerMm - targetPixels);
      if (error < bestError) {
        bestError = error;
        best = candidate;
      }
    }
  }
  return best;
}

WorldGridLayout buildWorldGrid(const SketchPlacement& placement,
                               const ViewportCameraState& camera,
                               const QSize& viewportSize) {
  WorldGridLayout layout;
  layout.placement = placement;
  if (viewportSize.width() <= 0 || viewportSize.height() <= 0) return layout;

  const QPointF p0 = camera.worldToScreen(placement.toWorld(0.0, 0.0));
  const QPointF pu = camera.worldToScreen(placement.toWorld(1.0, 0.0));
  const QPointF pv = camera.worldToScreen(placement.toWorld(0.0, 1.0));
  const QPointF du = pu - p0;
  const QPointF dv = pv - p0;

  const double pxPerMmU = std::hypot(du.x(), du.y());
  const double pxPerMmV = std::hypot(dv.x(), dv.y());
  const double pxPerMm = std::max(pxPerMmU, pxPerMmV);

  layout.minorSpacingMm = chooseNiceGridSpacing(pxPerMm, kTargetPixels);
  layout.majorSpacingMm = layout.minorSpacingMm * 5.0;

  // Recover the visible (u,v) range by inverting the affine screen mapping of
  // the plane. This keeps the grid anchored to world coordinates under pan.
  const double det = du.x() * dv.y() - du.y() * dv.x();
  const double norm = pxPerMmU * pxPerMmV;
  double uMin, uMax, vMin, vMax;
  if (norm > 1e-12 && std::abs(det) > 1e-3 * norm) {
    uMin = vMin = std::numeric_limits<double>::infinity();
    uMax = vMax = -std::numeric_limits<double>::infinity();
    const double w = viewportSize.width();
    const double h = viewportSize.height();
    for (const QPointF corner : {QPointF{0, 0}, QPointF{w, 0}, QPointF{w, h},
                                 QPointF{0, h}}) {
      const QPointF s = corner - p0;
      const double u = (dv.y() * s.x() - dv.x() * s.y()) / det;
      const double v = (-du.y() * s.x() + du.x() * s.y()) / det;
      uMin = std::min(uMin, u);
      uMax = std::max(uMax, u);
      vMin = std::min(vMin, v);
      vMax = std::max(vMax, v);
    }
  } else {
    // Edge-on / degenerate plane: fall back to a bounded centered range so we
    // never emit an unbounded number of lines.
    const double scale = std::max(pxPerMm, 1e-6);
    const double half = std::max(viewportSize.width(), viewportSize.height()) *
                        0.75 / scale;
    uMin = vMin = -half;
    uMax = vMax = half;
  }

  // Overscan so lines reach just past the viewport edge.
  const double uc = (uMin + uMax) * 0.5;
  const double vc = (vMin + vMax) * 0.5;
  const double uh = std::max((uMax - uMin) * 0.5, 0.0) * kOverscan;
  const double vh = std::max((vMax - vMin) * 0.5, 0.0) * kOverscan;
  uMin = uc - uh;
  uMax = uc + uh;
  vMin = vc - vh;
  vMax = vc + vh;

  const double spacing = layout.minorSpacingMm;
  // When a family collapses in projection (edge-on), keep only major lines and
  // the axis to avoid moiré and thousands of overlapping segments.
  const bool denseV = pxPerMmV * spacing < kMinProjectedSpacingPx;
  const bool denseU = pxPerMmU * spacing < kMinProjectedSpacingPx;

  const auto toWorld = [&](double u, double v) { return placement.toWorld(u, v); };

  const long long vStart = static_cast<long long>(std::ceil(vMin / spacing - 1e-9));
  const long long vEnd = static_cast<long long>(std::floor(vMax / spacing + 1e-9));
  const long long uStart = static_cast<long long>(std::ceil(uMin / spacing - 1e-9));
  const long long uEnd = static_cast<long long>(std::floor(uMax / spacing + 1e-9));

  for (long long k = vStart; k <= vEnd && layout.lines.size() < kMaxGridLines; ++k) {
    if (denseV && k % 5 != 0 && k != 0) continue;
    const double v = static_cast<double>(k) * spacing;
    const GridLineKind kind = k == 0 ? GridLineKind::AxisU
                                     : (k % 5 == 0 ? GridLineKind::Major
                                                   : GridLineKind::Minor);
    layout.lines.push_back({toWorld(uMin, v), toWorld(uMax, v), kind});
  }
  for (long long k = uStart; k <= uEnd && layout.lines.size() < kMaxGridLines; ++k) {
    if (denseU && k % 5 != 0 && k != 0) continue;
    const double u = static_cast<double>(k) * spacing;
    const GridLineKind kind = k == 0 ? GridLineKind::AxisV
                                     : (k % 5 == 0 ? GridLineKind::Major
                                                   : GridLineKind::Minor);
    layout.lines.push_back({toWorld(u, vMin), toWorld(u, vMax), kind});
  }
  return layout;
}

void paintWorldGrid(QPainter& painter, const WorldGridLayout& layout,
                    const ViewportCameraState& camera,
                    const WorldGridStyle& style) {
  if (layout.lines.empty()) return;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, false);
  const QPen axisU(axisColorForDirection(layout.placement.xDirection, style), 1.4);
  const QPen axisV(axisColorForDirection(layout.placement.yDirection, style), 1.4);
  const QPen minorPen(style.minor, 1.0);
  const QPen majorPen(style.major, 1.0);
  for (const GridLine& line : layout.lines) {
    switch (line.kind) {
      case GridLineKind::Minor: painter.setPen(minorPen); break;
      case GridLineKind::Major: painter.setPen(majorPen); break;
      case GridLineKind::AxisU: painter.setPen(axisU); break;
      case GridLineKind::AxisV: painter.setPen(axisV); break;
    }
    painter.drawLine(camera.worldToScreen(line.a), camera.worldToScreen(line.b));
  }
  painter.restore();
}

}  // namespace solidar
