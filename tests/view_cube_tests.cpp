#include "ui/ViewCube.h"
#include "ui/Viewport.h"
#include "ui/ViewportCamera.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
  return EXIT_FAILURE; } } while (false)

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

  // Every face/edge/corner target looks along its actual world direction.
  for (int x=-1; x<=1; ++x) for (int y=-1; y<=1; ++y) for (int z=-1; z<=1; ++z) {
    if (!x && !y && !z) continue;
    Point3d d{double(x),double(y),double(z)};
    auto orientation=orientationForDirection(d);
    ViewportCameraState camera{orientation.yaw,orientation.pitch,1,{},QSize(800,600)};
    CHECK(std::abs(camera.cameraDepth(d)-std::sqrt(double(x*x+y*y+z*z)))<1e-5);
  }
  const QSize size(800,600);
  const CameraOrientation iso{-45,30};
  const auto geometry=viewCubeGeometry(size,iso);
  CHECK(geometry.patches.size()==27); // three visible planes, each partitioned 3x3
  CHECK(!geometry.hitTest({0,0}));
  CHECK(!geometry.hitTest({size.width()-5.0,64}));
  CHECK(geometry.hitTest(geometry.home.center()).zone==ViewCubeZone::Home);
  CHECK(geometry.hitTest(geometry.fit.center()).zone==ViewCubeZone::Fit);
  for (const auto& patch : geometry.patches) {
    CHECK(geometry.hitTest(centroid(patch.polygon))==patch.hit);
  }
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
  viewport.resetScene();
  CHECK(animation->state()==QAbstractAnimation::Stopped);
  return EXIT_SUCCESS;
}
