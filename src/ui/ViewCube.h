#pragma once

#include <QColor>
#include <QPolygonF>
#include <QSize>
#include <QString>
#include <vector>

#include "model/SketchPlacement.h"

class QPainter;

namespace solidar {

enum class StandardView { Front, Back, Left, Right, Top, Bottom, Isometric };
struct CameraOrientation { float yaw{}, pitch{}; };
CameraOrientation orientationFor(StandardView view);
CameraOrientation orientationForDirection(Point3d direction);
float shortestAngleDelta(float from, float to);
CameraOrientation interpolateOrientation(CameraOrientation from,
                                        CameraOrientation to, float progress);

enum class ViewCubeZone { None, Face, Edge, Corner, Home, Fit };
struct ViewCubeHit {
  ViewCubeZone zone{ViewCubeZone::None};
  Point3d direction{};
  bool operator==(const ViewCubeHit& other) const;
  explicit operator bool() const { return zone != ViewCubeZone::None; }
};
struct ViewCubePatch {
  QPolygonF polygon;
  ViewCubeHit hit;
  int face{};
};
struct ViewCubeGeometry {
  std::vector<ViewCubePatch> patches;
  QRectF home, fit;
  ViewCubeHit hitTest(QPointF point) const;
};

// All coordinates are logical pixels. QPainter's device transform handles DPR;
// paint and input share exactly the same projected polygons at every scale.
ViewCubeGeometry viewCubeGeometry(QSize viewportSize, CameraOrientation camera);
QString viewCubeToolTip(ViewCubeHit hit);

struct ViewCubeStyle {
  QColor top{245, 248, 252}, front{219, 227, 238}, side{191, 204, 221};
  QColor outline{88, 106, 130}, text{38, 57, 82}, bevel{248, 251, 255};
  QColor hover{196, 224, 255}, pressed{147, 196, 249};
  QColor active{221, 235, 251}, accent{40, 116, 201}, shadow{35, 52, 76};
};
void paintViewCube(QPainter& painter, const ViewCubeGeometry& geometry,
                   CameraOrientation camera, ViewCubeHit hover,
                   ViewCubeHit pressed, const ViewCubeStyle& style = {});

}  // namespace solidar
