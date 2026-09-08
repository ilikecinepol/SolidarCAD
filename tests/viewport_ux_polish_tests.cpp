#include "model/EdgeManipulatorGeometry.h"
#include "ui/ManipulatorLayout.h"
#include "ui/ViewportCamera.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <QVector4D>

#include <cassert>
#include <cmath>
#include <iostream>

namespace {
bool close(double a, double b, double epsilon = 1e-5) {
  return std::abs(a - b) < epsilon;
}
}

#undef assert
#define assert(condition)                                                   \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return 1;                                                             \
    }                                                                       \
  } while (false)

int main() {
  using namespace solidar;
  const QRectF viewport(0, 0, 800, 600);
  const QRectF body(300, 200, 200, 180);

  auto layout = computeManipulatorLayout(
      {{500, 290}, {1, 0}, 45, body, viewport, {130, 40}, {}});
  assert(layout.visualSign > 0.0);
  assert(close(layout.visualLengthPx, 45.0));

  layout = computeManipulatorLayout(
      {{305, 290}, {1, 0}, 45, body, viewport, {130, 40}, {}});
  assert(layout.visualSign < 0.0);
  assert(close(layout.visualLengthPx, 45.0));

  layout = computeManipulatorLayout(
      {{400, 290}, {1, 0}, 5, body, viewport, {130, 40},
       {QRectF(500, 0, 300, 130)}});
  assert(close(layout.visualLengthPx, 36.0));
  assert(viewport.contains(QRectF(layout.hudTopLeft, QSizeF(130, 40))));
  assert(!QRectF(layout.hudTopLeft, QSizeF(130, 40))
              .intersects(QRectF(500, 0, 300, 130)));
  layout = computeManipulatorLayout(
      {{400, 290}, {0, 1}, 500, body, viewport, {130, 40}, {}});
  assert(layout.handle.y() > layout.anchor.y());
  assert(close(layout.visualLengthPx, 72.0));
  assert(safeAngularManipulatorRadius({400, 290}, 20, body) > 100.0);

  for (float yaw : {-180.0F, -90.0F, 0.0F, 90.0F, 180.0F}) {
    for (float pitch : {-85.0F, -45.0F, 0.0F, 45.0F, 85.0F}) {
      const auto range = ViewportDepthRange::combined(
          {0, 0, 0}, 100, {100, 20, 0}, 80, true);
      ViewportCameraState camera{yaw, pitch, 1.2F, {13, -7}, {1000, 800},
                                 2.0F, range.center, range.extent};
      for (const Point3d p : {Point3d{-50, -50, -50}, Point3d{140, 60, 40}}) {
        const QVector4D clip = camera.worldToClip() *
                               QVector4D(p.x, p.y, p.z, 1.0F);
        assert(std::isfinite(clip.x()) && std::isfinite(clip.y()));
        assert(clip.z() >= -1.0F && clip.z() <= 1.0F);
        const QPointF screen = camera.worldToScreen(p);
        const double mx = (clip.x() + 1.0) * 500.0;
        const double my = (1.0 - clip.y()) * 400.0;
        assert(close(screen.x(), mx) && close(screen.y(), my));
      }
    }
  }

  const TopoDS_Shape box = BRepPrimAPI_MakeBox(30, 20, 10).Shape();
  TopExp_Explorer firstEdge(box, TopAbs_EDGE);
  assert(firstEdge.More());
  const auto local = localEdgeManipulatorGeometry(box, firstEdge.Current());
  assert(local);
  gp_Trsf translation;
  translation.SetTranslation(gp_Vec(1000, -700, 350));
  const TopoDS_Shape moved = BRepBuilderAPI_Transform(box, translation).Shape();
  TopExp_Explorer movedEdge(moved, TopAbs_EDGE);
  assert(movedEdge.More());
  const auto translated =
      localEdgeManipulatorGeometry(moved, movedEdge.Current());
  assert(translated);
  assert(close(local->outwardDirection.x, translated->outwardDirection.x));
  assert(close(local->outwardDirection.y, translated->outwardDirection.y));
  assert(close(local->outwardDirection.z, translated->outwardDirection.z));
}
