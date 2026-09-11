#include "model/EdgeManipulatorGeometry.h"
#include "ui/ManipulatorLayout.h"
#include "ui/ViewportCamera.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <QLineF>
#include <QVector4D>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {
bool close(double a, double b, double epsilon = 1e-5) {
  return std::abs(a - b) < epsilon;
}
}  // namespace

int main() {
  using namespace solidar;
  const QRectF viewport(0, 0, 800, 600);
  const QRectF body(300, 200, 200, 180);

  // Direction selection: positive/negative side chosen from anchor position.
  // Semantic directions use realistic (> nearEndOnThresholdPx) magnitudes so
  // the deterministic fallback is not engaged.
  auto layout = computeManipulatorLayout(
      {{500, 290}, {40, 0}, 45, body, viewport, {130, 40}, {}});
  CHECK(layout.visualSign > 0.0);
  CHECK(!layout.usedFallback);
  CHECK(close(layout.visualLengthPx, 45.0));

  layout = computeManipulatorLayout(
      {{305, 290}, {40, 0}, 45, body, viewport, {130, 40}, {}});
  CHECK(layout.visualSign < 0.0);
  CHECK(close(layout.visualLengthPx, 45.0));

  // Compact bounds + exclusion/offset + HUD stays inside the viewport and out
  // of the excluded corner region.
  layout = computeManipulatorLayout(
      {{400, 290}, {40, 0}, 5, body, viewport, {130, 40},
       {QRectF(500, 0, 300, 130)}});
  CHECK(close(layout.visualLengthPx, 36.0));
  CHECK(viewport.contains(QRectF(layout.hudTopLeft, QSizeF(130, 40))));
  CHECK(!QRectF(layout.hudTopLeft, QSizeF(130, 40))
              .intersects(QRectF(500, 0, 300, 130)));

  // Vertical (Y-down) semantic direction keeps the length clamp at 72.
  layout = computeManipulatorLayout(
      {{400, 290}, {0, 40}, 500, body, viewport, {130, 40}, {}});
  CHECK(layout.handle.y() > layout.anchor.y());
  CHECK(close(layout.visualLengthPx, 72.0));

  // Sub-threshold projections deterministically fall back to screen-up
  // {0, -1}; no side-flip, no NaN.
  for (const QPointF sub : {QPointF(0, 0), QPointF(0.5, 0),
                            QPointF(1e-4, 1e-4)}) {
    layout = computeManipulatorLayout(
        {{400, 290}, sub, 45, body, viewport, {130, 40}, {}});
    CHECK(layout.usedFallback);
    CHECK(close(layout.direction.x(), 0.0));
    CHECK(close(layout.direction.y(), -1.0));
    CHECK(std::isfinite(layout.handle.x()) && std::isfinite(layout.handle.y()));
    CHECK(QLineF(layout.anchor, layout.handle).length() > 0.0);
  }

  // Non-finite inputs always take the fallback.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  auto stable = stableProjectedDirection({nan, 0.0}, {0.0, -1.0}, 2.0);
  CHECK(stable.usedFallback);
  CHECK(close(stable.normalizedDirection.y(), -1.0));
  stable = stableProjectedDirection({inf, 0.0}, {0.0, -1.0}, 2.0);
  CHECK(stable.usedFallback);
  CHECK(close(stable.normalizedDirection.y(), -1.0));

  // Two sub-threshold perturbations yield the identical direction and sign —
  // no chaotic flip near the view ray.
  const auto a = computeManipulatorLayout(
      {{400, 290}, {1.0, 0.01}, 45, body, viewport, {130, 40}, {}});
  const auto b = computeManipulatorLayout(
      {{400, 290}, {1.0, -0.01}, 45, body, viewport, {130, 40}, {}});
  CHECK(a.usedFallback && b.usedFallback);
  CHECK(close(a.direction.x(), b.direction.x()));
  CHECK(close(a.direction.y(), b.direction.y()));
  CHECK(a.visualSign == b.visualSign);

  // Scale invariance: doubling the projection magnitude together with the
  // end-on threshold leaves the fallback decision and normalized direction
  // unchanged (high-DPI/hi-DPI screen scaling).
  const auto lo = stableProjectedDirection({3.0, 0.0}, {0.0, -1.0}, 2.0);
  const auto hi = stableProjectedDirection({6.0, 0.0}, {0.0, -1.0}, 4.0);
  CHECK(lo.usedFallback == hi.usedFallback);
  CHECK(close(lo.normalizedDirection.x(), hi.normalizedDirection.x()));
  CHECK(close(lo.normalizedDirection.y(), hi.normalizedDirection.y()));
  const auto loFall = stableProjectedDirection({1.0, 0.0}, {0.0, -1.0}, 2.0);
  const auto hiFall = stableProjectedDirection({2.0, 0.0}, {0.0, -1.0}, 4.0);
  CHECK(loFall.usedFallback && hiFall.usedFallback);
  CHECK(close(loFall.normalizedDirection.y(), hiFall.normalizedDirection.y()));

  // Angular radius: floor, inside-silhouette expansion and the new ceiling.
  CHECK(safeAngularManipulatorRadius({400, 290}, 1.0, body) >= 34.0);
  const double expanded = safeAngularManipulatorRadius({400, 290}, 20.0, body);
  CHECK(expanded > 100.0);  // still clears the body silhouette
  CHECK(expanded <= 200.0);  // bounded ceiling
  CHECK(safeAngularManipulatorRadius({400, 290}, 10000.0, body) <= 200.0);
  const auto angular = computeAngularVisualRadius(
      {400, 290}, {420, 290}, {400, 270}, 35.0, body);
  CHECK(close(angular.safeRadiusPx, expanded));
  CHECK(close(angular.visualRadiusMm, 35.0 * expanded / 20.0));

  // Near-collapsed u with a visible v: the visual radius is derived from the
  // larger projected extent so the rendered v-extent stays within the ceiling
  // instead of ballooning to an enormous world radius.
  {
    const QPointF collapsedOrigin{400.0, 290.0};
    const QPointF collapsedU{400.1, 290.0};  // ~0.1 px, near-collapsed
    const QPointF collapsedV{400.0, 170.0};  // 120 px, visible
    const double vPx = QLineF(collapsedOrigin, collapsedV).length();
    const auto bounded = computeAngularVisualRadius(
        collapsedOrigin, collapsedU, collapsedV, 35.0, body);
    CHECK(bounded.safeRadiusPx <= ManipulatorStyle{}.maximumAngularRadiusPx);
    // The projected v-extent of the resolved arc is bounded by the ceiling.
    const double vExtentPx = vPx * bounded.visualRadiusMm / 35.0;
    CHECK(vExtentPx <= ManipulatorStyle{}.maximumAngularRadiusPx + 1e-6);
    CHECK(vExtentPx >= ManipulatorStyle{}.minimumAngularRadiusPx - 1e-6);
  }

  // Camera projection stays finite and translation/Y-down consistent across
  // orientations.
  for (float yaw : {-180.0F, -90.0F, 0.0F, 90.0F, 180.0F}) {
    for (float pitch : {-85.0F, -45.0F, 0.0F, 45.0F, 85.0F}) {
      const auto range = ViewportDepthRange::combined(
          {0, 0, 0}, 100, {100, 20, 0}, 80, true);
      ViewportCameraState camera{yaw, pitch, 1.2F, {13, -7}, {1000, 800},
                                 2.0F, range.center, range.extent};
      for (const Point3d p : {Point3d{-50, -50, -50}, Point3d{140, 60, 40}}) {
        const QVector4D clip = camera.worldToClip() *
                               QVector4D(p.x, p.y, p.z, 1.0F);
        CHECK(std::isfinite(clip.x()) && std::isfinite(clip.y()));
        CHECK(clip.z() >= -1.0F && clip.z() <= 1.0F);
        const QPointF screen = camera.worldToScreen(p);
        const double mx = (clip.x() + 1.0) * 500.0;
        const double my = (1.0 - clip.y()) * 400.0;
        CHECK(close(screen.x(), mx) && close(screen.y(), my));
      }
    }
  }

  // Edge manipulator geometry is translation-invariant.
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(30, 20, 10).Shape();
  TopExp_Explorer firstEdge(box, TopAbs_EDGE);
  CHECK(firstEdge.More());
  const auto local = localEdgeManipulatorGeometry(box, firstEdge.Current());
  CHECK(local);
  gp_Trsf translation;
  translation.SetTranslation(gp_Vec(1000, -700, 350));
  const TopoDS_Shape moved = BRepBuilderAPI_Transform(box, translation).Shape();
  TopExp_Explorer movedEdge(moved, TopAbs_EDGE);
  CHECK(movedEdge.More());
  const auto translated =
      localEdgeManipulatorGeometry(moved, movedEdge.Current());
  CHECK(translated);
  CHECK(close(local->outwardDirection.x, translated->outwardDirection.x));
  CHECK(close(local->outwardDirection.y, translated->outwardDirection.y));
  CHECK(close(local->outwardDirection.z, translated->outwardDirection.z));
  return EXIT_SUCCESS;
}
