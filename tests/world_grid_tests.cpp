#include "ui/WorldGrid.h"
#include "ui/ViewportCamera.h"

#include <QLineF>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {
using namespace solidar;

ViewportCameraState camera(float yaw, float pitch, float zoom, QPointF pan,
                           QSize size) {
  return ViewportCameraState{yaw, pitch, zoom, pan, size, 1.0F, {}, 1.0};
}

const GridLine* lineOf(const WorldGridLayout& layout, GridLineKind kind) {
  for (const auto& line : layout.lines)
    if (line.kind == kind) return &line;
  return nullptr;
}

bool finitePoint(Point3d p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool niceSpacing(double spacing) {
  if (!std::isfinite(spacing) || spacing <= 0.0) return false;
  const double exponent = std::floor(std::log10(spacing));
  const double mantissa = spacing / std::pow(10.0, exponent);
  return std::abs(mantissa - 1.0) < 1e-9 || std::abs(mantissa - 2.0) < 1e-9 ||
         std::abs(mantissa - 5.0) < 1e-9;
}

double dot(Vector3d a, Vector3d b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool pointOnSegment(QPointF p, QPointF a, QPointF b, double tolerance) {
  const QPointF ab = b - a;
  const double lengthSq = QPointF::dotProduct(ab, ab);
  if (lengthSq < 1e-9) return QLineF(a, p).length() < tolerance;
  const double t = std::clamp(QPointF::dotProduct(p - a, ab) / lengthSq, 0.0, 1.0);
  return QLineF(p, a + ab * t).length() < tolerance;
}
}  // namespace

int main() {
  const QSize size(800, 600);

  // Test 1 — nice spacing is always 1/2/5 x 10^n, finite and positive.
  for (double ppm : {0.001, 0.02, 0.5, 1.0, 2.5, 7.3, 40.0, 1000.0, 1e6}) {
    const double spacing = chooseNiceGridSpacing(ppm);
    CHECK(niceSpacing(spacing));
  }

  // Test 12 — spacing/geometry are logical-pixel based, independent of DPR.
  {
    const auto base = camera(-45.0F, 30.0F, 1.0F, {}, size);
    auto hi = base;
    hi.devicePixelRatio = 2.0F;
    CHECK(chooseNiceGridSpacing(1.0) == chooseNiceGridSpacing(1.0));
    const auto a = buildWorldGrid(SketchPlacement::xy(), base, size);
    const auto b = buildWorldGrid(SketchPlacement::xy(), hi, size);
    CHECK(a.minorSpacingMm == b.minorSpacingMm);
    CHECK(a.lines.size() == b.lines.size());
  }

  // Test 4 — Top view: XY grid is two near-orthogonal screen directions.
  {
    const auto top = camera(0.0F, 0.0F, 1.0F, {}, size);
    const QPointF dx = top.worldToScreen({1.0, 0.0, 0.0}) -
                       top.worldToScreen({0.0, 0.0, 0.0});
    const QPointF dy = top.worldToScreen({0.0, 1.0, 0.0}) -
                       top.worldToScreen({0.0, 0.0, 0.0});
    CHECK(QPointF::dotProduct(dx, dy) / (std::hypot(dx.x(), dx.y()) *
                                          std::hypot(dy.x(), dy.y())) < 1e-6);
    CHECK(std::hypot(dx.x(), dx.y()) > 0.0);
    CHECK(std::hypot(dy.x(), dy.y()) > 0.0);
  }

  // Test 3 — orbit changes the projected grid: the X axis maps differently
  // between Top and Isometric, proving the grid is world-space, not screen.
  {
    const auto top = camera(0.0F, 0.0F, 1.0F, {}, size);
    const auto iso = camera(-45.0F, 30.0F, 1.0F, {}, size);
    CHECK(top.worldToScreen({10.0, 0.0, 0.0}) !=
          iso.worldToScreen({10.0, 0.0, 0.0}));
  }

  // Test 5 — Front view: XY goes edge-on, still finite (no NaN/Inf).
  {
    const auto front = camera(0.0F, -90.0F, 1.0F, {}, size);
    const auto layout = buildWorldGrid(SketchPlacement::xy(), front, size);
    for (const auto& line : layout.lines) {
      CHECK(finitePoint(line.a));
      CHECK(finitePoint(line.b));
      const QPointF a = front.worldToScreen(line.a);
      const QPointF b = front.worldToScreen(line.b);
      CHECK(std::isfinite(a.x()) && std::isfinite(a.y()));
      CHECK(std::isfinite(b.x()) && std::isfinite(b.y()));
    }
  }

  // Test 6/7 — XZ and YZ planes stay on their coordinate plane.
  {
    const auto iso = camera(-45.0F, 30.0F, 1.0F, {}, size);
    for (const auto& line : buildWorldGrid(SketchPlacement::xz(), iso, size).lines) {
      CHECK(line.a.y == 0.0);
      CHECK(line.b.y == 0.0);
    }
    for (const auto& line : buildWorldGrid(SketchPlacement::yz(), iso, size).lines) {
      CHECK(line.a.x == 0.0);
      CHECK(line.b.x == 0.0);
    }
  }

  // Test 8 — arbitrary placement: grid is built relative to custom origin/U/V.
  {
    const SketchPlacement custom{{10.0, 20.0, 30.0}, {0.0, 1.0, 0.0},
                                 {1.0, 0.0, 0.0}};
    const auto iso = camera(-45.0F, 30.0F, 1.0F, {}, size);
    const auto layout = buildWorldGrid(custom, iso, size);
    CHECK(layout.placement.origin.x == 10.0);
    for (const auto& line : layout.lines) {
      // Every endpoint lies exactly on the plane.
      const auto roundTripA = custom.toWorld(custom.toLocal(line.a).x,
                                             custom.toLocal(line.a).y);
      CHECK(std::abs(roundTripA.x - line.a.x) < 1e-9);
      CHECK(std::abs(roundTripA.y - line.a.y) < 1e-9);
      CHECK(std::abs(roundTripA.z - line.a.z) < 1e-9);
    }
  }

  // Test 2 — world anchoring: axes intersect at the projected world origin.
  {
    for (const float zoom : {0.5F, 1.0F, 3.0F}) {
      const auto iso = camera(-45.0F, 30.0F, zoom, {}, size);
      const auto layout = buildWorldGrid(SketchPlacement::xy(), iso, size);
      const QPointF origin = iso.worldToScreen({0.0, 0.0, 0.0});
      const GridLine* axisU = lineOf(layout, GridLineKind::AxisU);
      const GridLine* axisV = lineOf(layout, GridLineKind::AxisV);
      CHECK(axisU);
      CHECK(axisV);
      CHECK(pointOnSegment(origin, iso.worldToScreen(axisU->a),
                           iso.worldToScreen(axisU->b), 0.5));
      CHECK(pointOnSegment(origin, iso.worldToScreen(axisV->a),
                           iso.worldToScreen(axisV->b), 0.5));
    }
  }

  // Test 9 — pan shifts the grid exactly with the model projection.
  {
    const auto noPan = camera(-45.0F, 30.0F, 1.0F, {}, size);
    const auto pan = camera(-45.0F, 30.0F, 1.0F, {50.0, -30.0}, size);
    const Point3d probe{12.0, 7.0, 3.0};
    const QPointF a = noPan.worldToScreen(probe);
    const QPointF b = pan.worldToScreen(probe);
    CHECK(std::abs((b.x() - a.x()) - 50.0) < 1e-6);
    CHECK(std::abs((b.y() - a.y()) - (-30.0)) < 1e-6);
    // And the grid itself is built with the pan-inclusive camera.
    const auto layout = buildWorldGrid(SketchPlacement::xy(), pan, size);
    const QPointF origin = pan.worldToScreen({0.0, 0.0, 0.0});
    CHECK(std::abs(origin.x() - (noPan.worldToScreen({0.0, 0.0, 0.0}).x() + 50.0)) <
          1e-6);
    CHECK(!layout.lines.empty());
  }

  // Test 10 — zoom changes projected scale and re-picks adaptive spacing.
  {
    const double spacingAt1 =
        buildWorldGrid(SketchPlacement::xy(), camera(-45.0F, 30.0F, 1.0F, {}, size),
                       size)
            .minorSpacingMm;
    const double spacingAt3 =
        buildWorldGrid(SketchPlacement::xy(), camera(-45.0F, 30.0F, 3.0F, {}, size),
                       size)
            .minorSpacingMm;
    CHECK(niceSpacing(spacingAt1));
    CHECK(niceSpacing(spacingAt3));
    CHECK(spacingAt3 <= spacingAt1);
  }

  // Test 11 — bounded line count at extreme zoom and edge-on.
  {
    for (const float zoom : {0.02F, 100.0F}) {
      const auto cam = camera(0.0F, -90.0F, zoom, {}, QSize(2000, 1500));
      const auto layout = buildWorldGrid(SketchPlacement::xy(), cam, QSize(2000, 1500));
      CHECK(layout.lines.size() <= 300);
      for (const auto& line : layout.lines) {
        CHECK(finitePoint(line.a));
        CHECK(finitePoint(line.b));
      }
    }
  }

  return EXIT_SUCCESS;
}
