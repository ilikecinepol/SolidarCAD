#include "ui/ViewCube.h"
#include "ui/ViewportCamera.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace solidar {
namespace {
constexpr std::array<Point3d, 8> vertices{{
    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
    {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1}}};
constexpr std::array<std::array<int,4>,6> faces{{
    {{0,1,2,3}}, {{4,7,6,5}}, {{0,4,5,1}},
    {{1,5,6,2}}, {{2,6,7,3}}, {{3,7,4,0}}}};
constexpr std::array<Point3d,6> normals{{
    {0,0,-1}, {0,0,1}, {0,-1,0}, {1,0,0}, {0,1,0}, {-1,0,0}}};
const std::array<QString,6> labels{
    QStringLiteral(u"СНИЗУ"), QStringLiteral(u"СВЕРХУ"),
    QStringLiteral(u"СПЕРЕДИ"), QStringLiteral(u"СПРАВА"),
    QStringLiteral(u"СЗАДИ"), QStringLiteral(u"СЛЕВА")};
Point3d mix(Point3d a, Point3d b, double t) {
  return {a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t};
}
Point3d onFace(int face, double u, double v) {
  const auto& f = faces[face];
  return mix(mix(vertices[f[0]], vertices[f[1]], u),
             mix(vertices[f[3]], vertices[f[2]], u), v);
}
Point3d viewingDirection(CameraOrientation camera) {
  const double y = camera.yaw * std::numbers::pi / 180;
  const double p = camera.pitch * std::numbers::pi / 180;
  return {std::sin(y)*std::sin(p), std::cos(y)*std::sin(p), std::cos(p)};
}
bool isActive(ViewCubeHit hit, CameraOrientation camera) {
  if (hit.zone == ViewCubeZone::Home) {
    const auto target = orientationFor(StandardView::Isometric);
    return std::abs(shortestAngleDelta(camera.yaw, target.yaw)) < 0.1F &&
           std::abs(shortestAngleDelta(camera.pitch, target.pitch)) < 0.1F;
  }
  const auto d = hit.direction;
  const double length = std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);
  if (!length) return false;
  const auto eye = viewingDirection(camera);
  return (d.x*eye.x+d.y*eye.y+d.z*eye.z)/length > 0.99999;
}
}  // namespace

CameraOrientation orientationFor(StandardView view) {
  static constexpr std::array<CameraOrientation,7> orientations{{
      {0,-90}, {0,90}, {-90,90}, {90,90}, {0,0}, {0,180}, {-45,30}}};
  return orientations.at(static_cast<std::size_t>(view));
}
CameraOrientation orientationForDirection(Point3d d) {
  // Preserve the established canonical Euler representation for all six faces.
  if (!d.x && !d.y) return orientationFor(d.z > 0 ? StandardView::Top : StandardView::Bottom);
  if (!d.x && !d.z) return orientationFor(d.y > 0 ? StandardView::Back : StandardView::Front);
  if (!d.y && !d.z) return orientationFor(d.x > 0 ? StandardView::Right : StandardView::Left);
  const double length = std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);
  return {static_cast<float>(std::atan2(d.x,d.y)*180/std::numbers::pi),
          static_cast<float>(std::acos(d.z/length)*180/std::numbers::pi)};
}
float shortestAngleDelta(float from, float to) { return std::remainder(to-from, 360.0F); }
CameraOrientation interpolateOrientation(CameraOrientation from, CameraOrientation to, float t) {
  t = std::clamp(t, 0.0F, 1.0F);
  return {from.yaw+shortestAngleDelta(from.yaw,to.yaw)*t,
          from.pitch+shortestAngleDelta(from.pitch,to.pitch)*t};
}
bool ViewCubeHit::operator==(const ViewCubeHit& other) const {
  return zone == other.zone && direction.x == other.direction.x &&
         direction.y == other.direction.y && direction.z == other.direction.z;
}
ViewCubeHit ViewCubeGeometry::hitTest(QPointF point) const {
  if (home.contains(point)) return {ViewCubeZone::Home};
  if (fit.contains(point)) return {ViewCubeZone::Fit};
  for (auto zone : {ViewCubeZone::Corner, ViewCubeZone::Edge, ViewCubeZone::Face})
    for (const auto& patch : patches)
      if (patch.hit.zone == zone && patch.polygon.containsPoint(point, Qt::OddEvenFill))
        return patch.hit;
  return {};
}
ViewCubeGeometry viewCubeGeometry(QSize size, CameraOrientation camera) {
  ViewCubeGeometry result;
  const QPointF center(size.width()-66.0, 64.0);
  const ViewportCameraState projection{camera.yaw,camera.pitch,1.0F,{},QSize(92,92)};
  const auto project = [&](Point3d p) {
    return projection.worldToScreen({p.x*32,p.y*32,p.z*32}) + center - QPointF(46,92*0.52);
  };
  constexpr std::array<double,4> steps{0,0.18,0.82,1};
  constexpr std::array<double,3> target{0,0.5,1};
  for (int face = 0; face < 6; ++face) {
    if (projection.cameraDepth(normals[face]) <= 1e-6) continue;
    for (int u=0; u<3; ++u) for (int v=0; v<3; ++v) {
      const auto zone = u==1 && v==1 ? ViewCubeZone::Face :
                        u!=1 && v!=1 ? ViewCubeZone::Corner : ViewCubeZone::Edge;
      QPolygonF polygon;
      polygon << project(onFace(face,steps[u],steps[v]))
              << project(onFace(face,steps[u+1],steps[v]))
              << project(onFace(face,steps[u+1],steps[v+1]))
              << project(onFace(face,steps[u],steps[v+1]));
      result.patches.push_back({polygon,{zone,onFace(face,target[u],target[v])},face});
    }
  }
  result.home = {center.x()-30,116,26,24};
  result.fit = {center.x()+4,116,26,24};
  return result;
}
QString viewCubeToolTip(ViewCubeHit hit) {
  if (hit.zone == ViewCubeZone::Fit) return QStringLiteral(u"Показать всё");
  if (hit.zone == ViewCubeZone::Home) return QStringLiteral(u"Основной изометрический вид");
  if (hit.zone == ViewCubeZone::Corner) return QStringLiteral(u"Изометрический вид");
  if (hit.zone == ViewCubeZone::Edge) return QStringLiteral(u"Вид по ребру");
  if (hit.zone == ViewCubeZone::Face)
    for (int i=0; i<6; ++i)
      if (ViewCubeHit{ViewCubeZone::Face,normals[i]} == hit) return labels[i];
  return {};
}
void paintViewCube(QPainter& painter, const ViewCubeGeometry& g,
                   CameraOrientation camera, ViewCubeHit hover,
                   ViewCubeHit pressed, const ViewCubeStyle& s) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  QPainterPath silhouette;
  for (const auto& p : g.patches) { QPainterPath part; part.addPolygon(p.polygon); silhouette = silhouette.united(part); }
  painter.save();
  painter.translate(2,4);
  for (int width=10; width>=2; width-=2) {
    auto shadow=s.shadow; shadow.setAlpha(5);
    painter.setPen(QPen(shadow,width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    painter.setBrush(shadow); painter.drawPath(silhouette);
  }
  painter.restore();
  QFont font=painter.font(); font.setPixelSize(8); font.setWeight(QFont::DemiBold);
  painter.setFont(font);
  for (const auto& patch : g.patches) {
    QColor base = patch.face==1 ? s.top : patch.face==3 || patch.face==5 ? s.side : s.front;
    const bool active = isActive(patch.hit,camera);
    if (active) base=s.active;
    if (patch.hit==hover) base=s.hover;
    if (patch.hit==pressed) base=s.pressed;
    const QRectF bounds=patch.polygon.boundingRect();
    QLinearGradient gradient(bounds.topLeft(),bounds.bottomRight());
    gradient.setColorAt(0,base.lighter(104)); gradient.setColorAt(1,base);
    painter.setBrush(gradient);
    painter.setPen(QPen(base,0.6)); painter.drawPolygon(patch.polygon);
    if (patch.hit.zone==ViewCubeZone::Face) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(s.bevel,1)); painter.drawPolygon(patch.polygon);
      painter.save(); painter.setClipRegion(QRegion(patch.polygon.toPolygon()),Qt::IntersectClip);
      if (bounds.width()>24 && bounds.height()>12) {
        painter.setPen(s.text); painter.drawText(bounds,Qt::AlignCenter,labels[patch.face]);
      }
      painter.restore();
    }
    if (active || patch.hit==hover || patch.hit==pressed) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(s.accent,active ? 1.6 : 1.1, active ? Qt::DashLine : Qt::SolidLine));
      painter.drawPolygon(patch.polygon);
    }
  }
  painter.setBrush(Qt::NoBrush); painter.setPen(QPen(s.outline,1,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
  painter.drawPath(silhouette);
  for (auto zone : {ViewCubeZone::Home,ViewCubeZone::Fit}) {
    const ViewCubeHit hit{zone}; const QRectF r=zone==ViewCubeZone::Home ? g.home : g.fit;
    painter.setPen(QPen(hit==hover ? s.accent : s.outline,1));
    painter.setBrush(hit==pressed ? s.pressed : hit==hover ? s.hover : s.top);
    painter.drawRoundedRect(r,5,5);
    const QPointF c=r.center(); painter.setBrush(Qt::NoBrush);
    if (zone==ViewCubeZone::Home) {
      painter.drawPolyline(QPolygonF{c+QPointF(-7,0),c+QPointF(0,-6),c+QPointF(7,0)});
      painter.drawPolyline(QPolygonF{c+QPointF(-5,-1),c+QPointF(-5,6),c+QPointF(5,6),c+QPointF(5,-1)});
    } else {
      for (int x : {-1,1}) for (int y : {-1,1})
        painter.drawPolyline(QPolygonF{c+QPointF(x*3,y*6),c+QPointF(x*7,y*6),c+QPointF(x*7,y*2)});
    }
  }
  painter.restore();
}
}  // namespace solidar
