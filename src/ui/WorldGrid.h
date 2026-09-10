#pragma once

#include <QColor>
#include <QSize>
#include <vector>

#include "model/SketchPlacement.h"
#include "ui/ViewportCamera.h"

class QPainter;

namespace solidar {

enum class GridLineKind { Minor, Major, AxisU, AxisV };

struct GridLine {
  Point3d a{};
  Point3d b{};
  GridLineKind kind{GridLineKind::Minor};
};

// The world grid is a bounded, world-anchored lattice on a single work plane.
// It carries no model state: placement describes only where the grid lives.
struct WorldGridLayout {
  SketchPlacement placement{SketchPlacement::xy()};
  double minorSpacingMm{10.0};
  double majorSpacingMm{50.0};
  std::vector<GridLine> lines;
};

// Theme-ready palette. Full dark-theme support is out of scope, but colors are
// collected here so they never leak into Viewport paint code.
struct WorldGridStyle {
  QColor minor{228, 233, 240};
  QColor major{203, 212, 224};
  QColor axisX{196, 112, 108};
  QColor axisY{108, 162, 108};
  QColor axisZ{106, 132, 200};
};

// Returns a spacing of the form 1/2/5 x 10^n mm that keeps the projected
// line density near targetPixels. Never returns 0, NaN, infinity or negative.
double chooseNiceGridSpacing(double pixelsPerMm, double targetPixels = 40.0);

// Builds the world-space grid for a work plane as seen by `camera` (which is
// pan-inclusive: its worldToScreen already applies cameraPan_). Line count is
// bounded and every line is anchored to an exact multiple of the spacing.
WorldGridLayout buildWorldGrid(const SketchPlacement& placement,
                               const ViewportCameraState& camera,
                               const QSize& viewportSize);

// Projects and paints the pre-built layout. The caller must pass the same
// pan-inclusive camera used to build it and must NOT apply an additional
// painter.translate(cameraPan_).
void paintWorldGrid(QPainter& painter, const WorldGridLayout& layout,
                    const ViewportCameraState& camera,
                    const WorldGridStyle& style = {});

}  // namespace solidar
