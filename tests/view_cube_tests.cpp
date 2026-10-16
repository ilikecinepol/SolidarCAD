#include "TestAssertions.h"

#include "ui/ViewCube.h"
#include "ui/Viewport.h"
#include "ui/ViewportCamera.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>
#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace solidar {
class MainWindowUndoTestAccess {
 public:
  static QPointF worldScreen(const Viewport& viewport, Point3d world) {
    const ViewportCameraState camera{
        viewport.yaw_, viewport.pitch_, viewport.zoom_, viewport.cameraPan_,
        viewport.size(), 1.0F, viewport.displayedBodyCenter_,
        std::max(1.0, viewport.displayedBodyDiagonal_ * 3.0)};
    return camera.worldToScreen(world);
  }
};
}  // namespace solidar

namespace {
QPointF centroid(const QPolygonF& polygon) {
  QPointF result;
  for (auto p : polygon) result += p;
  return result / polygon.size();
}
void mouse(solidar::Viewport& view, QEvent::Type type, QPointF position,
           Qt::MouseButton button, Qt::MouseButtons buttons) {
  QMouseEvent event(type,position,position,button,buttons,Qt::NoModifier);
  QApplication::sendEvent(&view,&event);
}
}

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM","offscreen");
  QApplication application(argc,argv);
  using namespace solidar;
  const std::array<StandardView,7> views{StandardView::Front,StandardView::Back,
      StandardView::Left,StandardView::Right,StandardView::Top,StandardView::Bottom,
      StandardView::Isometric};
  const std::array<CameraOrientation,7> expected{{
      {0,-90},{0,90},{-90,90},{90,90},{0,0},{0,180},{-45,30}}};
  for (std::size_t i=0; i<views.size(); ++i) {
    auto orientation=orientationFor(views[i]);
    CHECK(orientation.yaw==expected[i].yaw && orientation.pitch==expected[i].pitch);
  }
  CHECK(shortestAngleDelta(350,10)==20);
  CHECK(shortestAngleDelta(10,350)==-20);
  CHECK(interpolateOrientation({350,0},{10,0},0.5F).yaw==360);
  CHECK(interpolateOrientation({10,0},{350,0},0.5F).yaw==0);
  CHECK(shortestAngleDelta(179,-179)==2);
  const CameraOrientation iso{-45,30};

  // Sketch transitions use an upright target and interpolate the eye vector
  // on the shortest great-circle arc, not as two unrelated Euler angles.
  const auto uprightFront =
      uprightOrientationForDirection({0.0, -1.0, 0.0}, iso.yaw);
  CHECK(std::abs(uprightFront.pitch - 90.0F) < 1e-6F);
  CHECK(cameraScreenUpDirection(uprightFront).z > 0.999);
  const auto uprightTop =
      uprightOrientationForDirection({0.0, 0.0, 1.0}, iso.yaw);
  CHECK(std::abs(shortestAngleDelta(uprightTop.yaw, iso.yaw)) < 1e-6F);
  const auto shortestMid =
      interpolateOrientationShortestArc(iso, uprightFront, 0.5F);
  const auto startEye = cameraViewingDirection(iso);
  const auto targetEye = cameraViewingDirection(uprightFront);
  const auto middleEye = cameraViewingDirection(shortestMid);
  const auto angleBetween = [](Point3d a, Point3d b) {
    return std::acos(std::clamp(a.x * b.x + a.y * b.y + a.z * b.z,
                               -1.0, 1.0));
  };
  CHECK(std::abs(angleBetween(startEye, middleEye) * 2.0 -
                 angleBetween(startEye, targetEye)) <
        1e-5);

  // Every face/edge/corner target looks along its actual world direction.
  for (int x=-1; x<=1; ++x) for (int y=-1; y<=1; ++y) for (int z=-1; z<=1; ++z) {
    if (!x && !y && !z) continue;
    Point3d d{double(x),double(y),double(z)};
    auto orientation=orientationForDirection(d);
    ViewportCameraState camera{orientation.yaw,orientation.pitch,1,{},QSize(800,600)};
    CHECK(std::abs(camera.cameraDepth(d)-std::sqrt(double(x*x+y*y+z*z)))<1e-5);
  }
  const QSize size(800,600);
  const auto geometry=viewCubeGeometry(size,iso);
  CHECK(geometry.patches.size()==27); // three visible planes, each partitioned 3x3
  CHECK(!geometry.hitTest({0,0}));
  CHECK(!geometry.hitTest({size.width()-1.0,150}));
  CHECK(geometry.hitTest(geometry.rotateCounterClockwise.center()).zone==
        ViewCubeZone::RotateCounterClockwise);
  CHECK(geometry.hitTest(geometry.rotateClockwise.center()).zone==
        ViewCubeZone::RotateClockwise);
  CHECK(geometry.hitTest(geometry.home.center()).zone==ViewCubeZone::Home);
  CHECK(geometry.hitTest(geometry.fit.center()).zone==ViewCubeZone::Fit);
  for (const auto& patch : geometry.patches) {
    CHECK(geometry.hitTest(centroid(patch.polygon))==patch.hit);
    const int nonZero = (std::abs(patch.hit.direction.x)>1e-9) +
                        (std::abs(patch.hit.direction.y)>1e-9) +
                        (std::abs(patch.hit.direction.z)>1e-9);
    if (patch.hit.zone==ViewCubeZone::Edge) CHECK(nonZero==2);
    if (patch.hit.zone==ViewCubeZone::Corner) CHECK(nonZero==3);
  }
  CHECK(viewCubeToolTip({ViewCubeZone::Edge,{1,0,1}}).contains("45"));
  CHECK(viewCubeToolTip({ViewCubeZone::RotateCounterClockwise}).contains("45"));
  CHECK(viewCubeToolTip({ViewCubeZone::RotateClockwise}).contains("45"));
  // Overlap priority is explicit, independent of insertion/paint order.
  const QPolygonF area{QPointF(10,10),QPointF(50,10),QPointF(50,50),QPointF(10,50)};
  ViewCubeGeometry overlap;
  overlap.patches={{area,{ViewCubeZone::Face,{0,0,1}},0},
                   {area,{ViewCubeZone::Edge,{1,0,1}},0},
                   {area,{ViewCubeZone::Corner,{1,1,1}},0}};
  CHECK(overlap.hitTest({20,20}).zone==ViewCubeZone::Corner);
  overlap.patches.pop_back();
  CHECK(overlap.hitTest({20,20}).zone==ViewCubeZone::Edge);
  overlap.patches.pop_back();
  CHECK(overlap.hitTest({20,20}).zone==ViewCubeZone::Face);
  // Bounding-box corners outside the actual diamond do not hit a face.
  overlap.patches.front().polygon={QPointF(30,10),QPointF(50,30),QPointF(30,50),QPointF(10,30)};
  CHECK(!overlap.hitTest({11,11}));

  // Paint to real high-DPI devices; geometry stays in logical coordinates.
  for (double dpr : {1.0,1.25,1.5,2.0}) {
    QImage image(QSize(qRound(size.width()*dpr),qRound(size.height()*dpr)),QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr); image.fill(Qt::white);
    QPainter painter(&image);
    paintViewCube(painter,geometry,iso,geometry.patches[4].hit,{});
    painter.end();
    for (const auto& patch : geometry.patches) {
      const auto logical=centroid(patch.polygon);
      const QPoint physical(qRound(logical.x()*dpr),qRound(logical.y()*dpr));
      CHECK(image.pixelColor(physical)!=QColor(Qt::white));
      CHECK(geometry.hitTest(QPointF(physical)/dpr)==patch.hit);
    }
  }
  const auto resized=viewCubeGeometry({1000,700},iso);
  CHECK(centroid(resized.patches.front().polygon)-centroid(geometry.patches.front().polygon)==QPointF(200,0));

  // The real Viewport consumes overlay clicks before CAD selection. Animation
  // is driven by setCurrentTime, with no sleeps, timers-for-clicks or GPU.
  Viewport viewport;
  viewport.resize(size);
  viewport.viewIsometric();
  auto* animation=viewport.findChild<QVariantAnimation*>("viewOrientationTransition");
  CHECK(animation && animation->duration()==200);
  int selections=0;
  QObject::connect(&viewport,&Viewport::selectionChanged,[&](const QString&) { ++selections; });
  const auto face = geometry.patches[4];
  const auto point=centroid(face.polygon);
  mouse(viewport,QEvent::MouseMove,point,Qt::NoButton,Qt::NoButton);
  CHECK(viewport.cursor().shape()==Qt::PointingHandCursor);
  mouse(viewport,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
  mouse(viewport,QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton);
  CHECK(animation->state()==QAbstractAnimation::Running);
  CHECK(viewport.cameraYawDegrees()==iso.yaw && viewport.cameraPitchDegrees()==iso.pitch);
  const auto target=orientationForDirection(face.hit.direction);
  animation->setCurrentTime(100);
  const auto midpoint=interpolateOrientation(iso,target,0.5F);
  CHECK(std::abs(viewport.cameraYawDegrees()-midpoint.yaw)<1e-5);
  CHECK(std::abs(viewport.cameraPitchDegrees()-midpoint.pitch)<1e-5);
  animation->setCurrentTime(200);
  CHECK(std::abs(shortestAngleDelta(viewport.cameraYawDegrees(),target.yaw))<1e-5);
  CHECK(std::abs(shortestAngleDelta(viewport.cameraPitchDegrees(),target.pitch))<1e-5);
  CHECK(selections==0);

  // The two curved arrows orbit the ordinary 3D camera by exactly 45 degrees
  // while preserving its current inclination.
  const float arrowStartYaw=viewport.cameraYawDegrees();
  const float arrowStartPitch=viewport.cameraPitchDegrees();
  const auto arrowGeometry=viewCubeGeometry(
      size,{arrowStartYaw,arrowStartPitch});
  mouse(viewport,QEvent::MouseButtonPress,
        arrowGeometry.rotateClockwise.center(),Qt::LeftButton,Qt::LeftButton);
  mouse(viewport,QEvent::MouseButtonRelease,
        arrowGeometry.rotateClockwise.center(),Qt::LeftButton,Qt::NoButton);
  CHECK(animation->state()==QAbstractAnimation::Running);
  animation->setCurrentTime(200);
  CHECK(std::abs(shortestAngleDelta(
            viewport.cameraYawDegrees(),arrowStartYaw+45.0F))<1e-5);
  CHECK(std::abs(viewport.cameraPitchDegrees()-arrowStartPitch)<1e-5);
  CHECK(selections==0);

  // A visible chamfer is also an interaction target. Its direction has two
  // equal components, so activating it produces a 45-degree principal view.
  const CameraOrientation afterFace{viewport.cameraYawDegrees(),
                                    viewport.cameraPitchDegrees()};
  const auto edgeGeometry=viewCubeGeometry(size,afterFace);
  const auto edgeIt=std::find_if(
      edgeGeometry.patches.begin(),edgeGeometry.patches.end(),
      [](const ViewCubePatch& patch) {
        return patch.hit.zone==ViewCubeZone::Edge;
      });
  CHECK(edgeIt!=edgeGeometry.patches.end());
  const auto edgePoint=centroid(edgeIt->polygon);
  mouse(viewport,QEvent::MouseButtonPress,edgePoint,Qt::LeftButton,Qt::LeftButton);
  mouse(viewport,QEvent::MouseButtonRelease,edgePoint,Qt::LeftButton,Qt::NoButton);
  CHECK(animation->state()==QAbstractAnimation::Running);
  const auto edgeTarget=orientationForDirection(edgeIt->hit.direction);
  animation->setCurrentTime(200);
  CHECK(std::abs(shortestAngleDelta(viewport.cameraYawDegrees(),edgeTarget.yaw))<1e-5);
  CHECK(std::abs(shortestAngleDelta(viewport.cameraPitchDegrees(),edgeTarget.pitch))<1e-5);
  CHECK(selections==0);

  // Home survives removal of Ribbon ISO and animates back to the legacy ISO.
  mouse(viewport,QEvent::MouseButtonPress,geometry.home.center(),Qt::LeftButton,Qt::LeftButton);
  mouse(viewport,QEvent::MouseButtonRelease,geometry.home.center(),Qt::LeftButton,Qt::NoButton);
  CHECK(animation->state()==QAbstractAnimation::Running);
  animation->setCurrentTime(50);
  mouse(viewport,QEvent::MouseButtonPress,{200,200},Qt::RightButton,Qt::RightButton);
  CHECK(animation->state()==QAbstractAnimation::Stopped);
  const auto yaw=viewport.cameraYawDegrees();
  mouse(viewport,QEvent::MouseMove,{220,200},Qt::NoButton,Qt::RightButton);
  CHECK(viewport.cameraYawDegrees()==yaw+10);
  mouse(viewport,QEvent::MouseButtonRelease,{220,200},Qt::RightButton,Qt::NoButton);
  // A press on the overlay followed by release outside must not orbit or pick.
  const auto orientation=CameraOrientation{viewport.cameraYawDegrees(),viewport.cameraPitchDegrees()};
  mouse(viewport,QEvent::MouseButtonPress,geometry.home.center(),Qt::LeftButton,Qt::LeftButton);
  mouse(viewport,QEvent::MouseMove,{200,200},Qt::NoButton,Qt::LeftButton);
  mouse(viewport,QEvent::MouseButtonRelease,{200,200},Qt::LeftButton,Qt::NoButton);
  CHECK(viewport.cameraYawDegrees()==orientation.yaw && viewport.cameraPitchDegrees()==orientation.pitch);
  CHECK(animation->state()==QAbstractAnimation::Stopped);

  // Ordinary 3D orbiting keeps the real world origin fixed on screen even
  // when the visible body's bounding-box centre is elsewhere.
  viewport.setBodyShape(std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(gp_Pnt(80.0, 20.0, 10.0), 30.0, 20.0, 15.0)
          .Shape()));
  const QPointF worldOriginBefore =
      MainWindowUndoTestAccess::worldScreen(viewport, {});
  mouse(viewport,QEvent::MouseButtonPress,{240,220},Qt::RightButton,
        Qt::RightButton);
  mouse(viewport,QEvent::MouseMove,{280,245},Qt::NoButton,Qt::RightButton);
  mouse(viewport,QEvent::MouseButtonRelease,{280,245},Qt::RightButton,
        Qt::NoButton);
  const QPointF worldOriginAfter =
      MainWindowUndoTestAccess::worldScreen(viewport, {});
  CHECK(QLineF(worldOriginBefore, worldOriginAfter).length() < 1e-5);
  viewport.resetScene();
  CHECK(animation->state()==QAbstractAnimation::Stopped);
  return EXIT_SUCCESS;
}
