#include "ui/Viewport.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace solidar {
namespace {

struct Point3 {
  float x;
  float y;
  float z;
};

QPointF project(Point3 point, const QSize& size, float yaw, float pitch,
                float zoom) {
  const float yawRadians = yaw * std::numbers::pi_v<float> / 180.0F;
  const float pitchRadians = pitch * std::numbers::pi_v<float> / 180.0F;
  const float x1 = point.x * std::cos(yawRadians) - point.y * std::sin(yawRadians);
  const float y1 = point.x * std::sin(yawRadians) + point.y * std::cos(yawRadians);
  const float y2 = y1 * std::cos(pitchRadians) - point.z * std::sin(pitchRadians);
  const float scale = std::min(size.width(), size.height()) * 0.008F * zoom;
  return {size.width() * 0.5F + x1 * scale,
          size.height() * 0.52F + y2 * scale};
}

Point3 pointOnSupport(sketch::Point point, const QString& support,
                      const BoxParameters& box, float offsetX, float offsetY) {
  const float u = static_cast<float>(point.xMm);
  const float v = static_cast<float>(point.yMm);
  if (support.contains("XZ") || support.contains(QString::fromUtf8("Передняя")) ||
      support.contains(QString::fromUtf8("Задняя"))) {
    const float y = support.contains(QString::fromUtf8("Задняя"))
                        ? static_cast<float>(box.depthMm * 0.5) + offsetY
                        : support.contains(QString::fromUtf8("Передняя"))
                              ? static_cast<float>(-box.depthMm * 0.5) + offsetY
                              : 0.0F;
    return {u + offsetX, y, v};
  }
  if (support.contains("YZ") || support.contains(QString::fromUtf8("Правая")) ||
      support.contains(QString::fromUtf8("Левая"))) {
    const float x = support.contains(QString::fromUtf8("Правая"))
                        ? static_cast<float>(box.widthMm * 0.5) + offsetX
                        : support.contains(QString::fromUtf8("Левая"))
                              ? static_cast<float>(-box.widthMm * 0.5) + offsetX
                              : 0.0F;
    return {x, u + offsetY, v};
  }
  const float z = support.contains(QString::fromUtf8("Верхняя"))
                      ? static_cast<float>(box.heightMm)
                      : 0.0F;
  return {u + offsetX, v + offsetY, z};
}

Point3 supportNormal(const QString& support) {
  if (support.contains("XZ") || support.contains(QString::fromUtf8("Передняя")) ||
      support.contains(QString::fromUtf8("Задняя")))
    return {0.0F, 1.0F, 0.0F};
  if (support.contains("YZ") || support.contains(QString::fromUtf8("Правая")) ||
      support.contains(QString::fromUtf8("Левая")))
    return {1.0F, 0.0F, 0.0F};
  return {0.0F, 0.0F, 1.0F};
}

Point3 translated(Point3 point, Point3 direction, float distance) {
  return {point.x + direction.x * distance,
          point.y + direction.y * distance,
          point.z + direction.z * distance};
}

double signedArea(const QPolygonF& polygon) {
  double area = 0.0;
  for (qsizetype index = 0; index < polygon.size(); ++index) {
    const QPointF& current = polygon[index];
    const QPointF& next = polygon[(index + 1) % polygon.size()];
    area += current.x() * next.y() - next.x() * current.y();
  }
  return area * 0.5;
}

bool isFrontFacing(const QPolygonF& polygon) {
  // The screen Y axis points down, therefore outward, front-facing polygons
  // have clockwise winding after projection.
  return polygon.size() >= 3 && signedArea(polygon) < -0.01;
}

}  // namespace

Viewport::Viewport(QWidget* parent) : QOpenGLWidget(parent) {
  setMinimumSize(480, 320);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
}

void Viewport::setBox(BoxParameters parameters) {
  box_ = parameters;
  update();
}

void Viewport::setSketch(const sketch::Sketch& sketch) {
  sketch_ = sketch;
  update();
}

void Viewport::setSolidSketch(const sketch::Sketch& sketch) {
  solidSketch_ = sketch;
  update();
}

void Viewport::setSolidVisible(bool visible) {
  solidVisible_ = visible;
  update();
}

void Viewport::setSolidSupport(const QString& supportName) {
  solidSupportName_ = supportName;
  update();
}

void Viewport::setSketchVisible(bool visible) {
  sketchVisible_ = visible;
  update();
}

void Viewport::addSketch(const sketch::Sketch& sketch,
                         const QString& supportName) {
  sketch_ = sketch;
  displaySketches_.push_back({sketch, supportName, true});
  update();
}

void Viewport::setSketchVisible(std::size_t index, bool visible) {
  if (index >= displaySketches_.size()) return;
  displaySketches_[index].visible = visible;
  update();
}

void Viewport::setOriginVisible(bool visible) {
  originVisible_ = visible;
  update();
}

void Viewport::setBasePlaneVisible(int plane, bool visible) {
  if (plane < 0 || plane > 2) return;
  basePlanesVisible_[plane] = visible;
  update();
}

void Viewport::resetScene() {
  sketch_.clear();
  solidSketch_.clear();
  displaySketches_.clear();
  extrusionHoverPolygon_.clear();
  selectedExtrusionSketch_.clear();
  selectedExtrusionSupport_.clear();
  selectedExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  solidVisible_ = false;
  sketchVisible_ = true;
  selectedFace_ = -1;
  pickMode_ = PickMode::None;
  offsetX_ = 0.0F;
  offsetY_ = 0.0F;
  for (bool& visible : basePlanesVisible_) visible = false;
  update();
}

void Viewport::beginSketchPlaneSelection() {
  pickMode_ = PickMode::SketchPlane;
  selectedFace_ = -1;
  for (bool& visible : basePlanesVisible_) visible = true;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::beginExtrusionSurfaceSelection() {
  pickMode_ = PickMode::ExtrusionSurface;
  selectedFace_ = -1;
  setCursor(Qt::CrossCursor);
  update();
}

bool Viewport::hasSelectedFace() const noexcept { return selectedFace_ >= 0; }

QString Viewport::selectedFaceName() const {
  static const std::array<const char*, 6> names{
      "Нижняя", "Верхняя", "Передняя",
      "Правая", "Задняя", "Левая"};
  return selectedFace_ >= 0 ? QString::fromUtf8(names[selectedFace_]) : QString{};
}

const sketch::Sketch& Viewport::extrusionCandidateSketch() const noexcept {
  return selectedExtrusionSketch_;
}

QString Viewport::extrusionCandidateSupport() const {
  return selectedExtrusionSupport_;
}

std::size_t Viewport::extrusionCandidateSketchIndex() const noexcept {
  return selectedExtrusionSketchIndex_;
}

const sketch::Sketch& Viewport::solidSketch() const noexcept {
  return solidSketch_;
}

QString Viewport::solidSupport() const { return solidSupportName_; }

void Viewport::initializeGL() {
  initializeOpenGLFunctions();
  glClearColor(0.965F, 0.975F, 0.99F, 1.0F);
}

void Viewport::resizeGL(int width, int height) { glViewport(0, 0, width, height); }

void Viewport::paintGL() {
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  painter.setPen(QPen(QColor(224, 231, 241), 1));
  constexpr int gridStep = 28;
  for (int x = width() / 2 % gridStep; x < width(); x += gridStep)
    painter.drawLine(x, 0, x, height());
  for (int y = height() / 2 % gridStep; y < height(); y += gridStep)
    painter.drawLine(0, y, width(), y);

  const float x = static_cast<float>(box_.widthMm) * 0.5F;
  const float y = static_cast<float>(box_.depthMm) * 0.5F;
  const float z = static_cast<float>(box_.heightMm);
  const std::array<Point3, 8> vertices{{
      {-x + offsetX_, -y + offsetY_, 0}, {x + offsetX_, -y + offsetY_, 0},
      {x + offsetX_, y + offsetY_, 0}, {-x + offsetX_, y + offsetY_, 0},
      {-x + offsetX_, -y + offsetY_, z}, {x + offsetX_, -y + offsetY_, z},
      {x + offsetX_, y + offsetY_, z}, {-x + offsetX_, y + offsetY_, z}}};
  const std::array<std::array<int, 4>, 6> faces{{{{0, 1, 2, 3}}, {{4, 7, 6, 5}},
                                                  {{0, 4, 5, 1}}, {{1, 5, 6, 2}},
                                                  {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};
  const std::array<QColor, 6> colors{{QColor("#737d87"), QColor("#c4cbd2"),
                                      QColor("#89939d"), QColor("#a9b1b9"),
                                      QColor("#66717c"), QColor("#949ea8")}};

  const float planeSize = std::max(35.0F, std::max(x, y) * 1.35F);
  const std::array<std::array<Point3, 4>, 3> basePlanes{{
      {{{-planeSize, -planeSize, 0}, {planeSize, -planeSize, 0},
         {planeSize, planeSize, 0}, {-planeSize, planeSize, 0}}},
      {{{-planeSize, 0, -planeSize}, {planeSize, 0, -planeSize},
         {planeSize, 0, planeSize}, {-planeSize, 0, planeSize}}},
      {{{0, -planeSize, -planeSize}, {0, planeSize, -planeSize},
         {0, planeSize, planeSize}, {0, -planeSize, planeSize}}}}};
  const std::array<QColor, 3> planeColors{
      QColor(48, 155, 100, 35), QColor(225, 75, 80, 35), QColor(45, 105, 220, 35)};
  for (int plane = 0; plane < 3; ++plane) {
    if (!basePlanesVisible_[plane]) continue;
    QPolygonF polygon;
    for (const auto& point : basePlanes[plane])
      polygon << project(point, size(), yaw_, pitch_, zoom_);
    painter.setBrush(planeColors[plane]);
    painter.setPen(QPen(planeColors[plane].darker(125),
                        pickMode_ == PickMode::SketchPlane ? 2.0 : 1.0,
                        Qt::DashLine));
    painter.drawPolygon(polygon);
    painter.drawText(polygon.boundingRect().center(),
                     plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ");
  }

  if (solidVisible_ && solidSketch_.lines().empty() && solidSketch_.circles().empty())
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
    QPolygonF polygon;
    for (const int index : faces[faceIndex])
      polygon << project(vertices[index], size(), yaw_, pitch_, zoom_);
    if (!isFrontFacing(polygon)) continue;
    painter.setBrush(colors[faceIndex]);
    painter.setPen(QPen(static_cast<int>(faceIndex) == selectedFace_
                            ? QColor("#075eff") : QColor("#4d5863"),
                        static_cast<int>(faceIndex) == selectedFace_ ? 3.0 : 1.4));
    painter.drawPolygon(polygon);
  }

  if (solidVisible_ && !solidSketch_.lines().empty()) {
    QPolygonF bottom;
    QPolygonF top;
    const Point3 normal = supportNormal(solidSupportName_);
    for (const auto& line : solidSketch_.lines()) {
      const Point3 baseStart = pointOnSupport(
          line.start, solidSupportName_, box_, offsetX_, offsetY_);
      const Point3 baseEnd = pointOnSupport(
          line.end, solidSupportName_, box_, offsetX_, offsetY_);
      const Point3 topStart = translated(baseStart, normal, z);
      const Point3 topEnd = translated(baseEnd, normal, z);
      bottom.prepend(project(baseStart, size(), yaw_, pitch_, zoom_));
      top << project(topStart, size(), yaw_, pitch_, zoom_);
      QPolygonF side;
      side << project(baseStart, size(), yaw_, pitch_, zoom_)
           << project(baseEnd, size(), yaw_, pitch_, zoom_)
           << project(topEnd, size(), yaw_, pitch_, zoom_)
           << project(topStart, size(), yaw_, pitch_, zoom_);
      if (isFrontFacing(side)) {
        painter.setBrush(QColor("#87919b"));
        painter.setPen(QPen(QColor("#4d5863"), 1.35));
        painter.drawPolygon(side);
      }
    }
    painter.setPen(QPen(QColor("#4d5863"), 1.35));
    if (isFrontFacing(top)) {
      painter.setBrush(QColor("#c2c9d0"));
      painter.drawPolygon(top);
    }
    if (isFrontFacing(bottom)) {
      painter.setBrush(QColor("#68737e"));
      painter.drawPolygon(bottom);
    }
  }

  if (solidVisible_) for (const auto& circle : solidSketch_.circles()) {
    QPolygonF top;
    QPolygonF bottom;
    const Point3 normal = supportNormal(solidSupportName_);
    for (int step = 0; step < 64; ++step) {
      const float a = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
      const float b = 2.0F * std::numbers::pi_v<float> * (step + 1) / 64.0F;
      const auto point = [&](float angle, float height) {
        const sketch::Point profilePoint{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnSupport(profilePoint, solidSupportName_,
                                           box_, offsetX_, offsetY_);
        return project(translated(base, normal, height), size(), yaw_, pitch_, zoom_);
      };
      QPolygonF side;
      side << point(a, 0.0F) << point(b, 0.0F) << point(b, z) << point(a, z);
      const int shade = 120 + static_cast<int>(45.0F * std::cos(a));
      if (isFrontFacing(side)) {
        painter.setBrush(QColor(shade, shade + 5, shade + 10));
        painter.setPen(QPen(QColor("#59636d"), 0.9));
        painter.drawPolygon(side);
      }
      top << point(a, z);
      bottom.prepend(point(a, 0.0F));
    }
    if (isFrontFacing(top)) {
      painter.setBrush(QColor("#c5ccd3"));
      painter.setPen(QPen(QColor("#4d5863"), 1.35));
      painter.drawPolygon(top);
    }
    if (isFrontFacing(bottom)) {
      painter.setBrush(QColor("#68737e"));
      painter.setPen(QPen(QColor("#4d5863"), 1.35));
      painter.drawPolygon(bottom);
    }
  }

  if (solidVisible_ && selectedFace_ >= 0) {
    QPolygonF selectedPolygon;
    for (int vertex : faces[static_cast<std::size_t>(selectedFace_)])
      selectedPolygon << project(vertices[static_cast<std::size_t>(vertex)],
                                 size(), yaw_, pitch_, zoom_);
    if (isFrontFacing(selectedPolygon)) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(QColor("#075eff"), 3.0));
      painter.drawPolygon(selectedPolygon);
    }
  }

  if (sketchVisible_ && displaySketches_.empty()) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(22, 105, 215), 2.2));
    for (const auto& line : sketch_.lines()) {
      painter.drawLine(project({static_cast<float>(line.start.xMm) + offsetX_,
                                static_cast<float>(line.start.yMm) + offsetY_, 0.0F},
                               size(), yaw_, pitch_, zoom_),
                       project({static_cast<float>(line.end.xMm) + offsetX_,
                                static_cast<float>(line.end.yMm) + offsetY_, 0.0F},
                               size(), yaw_, pitch_, zoom_));
    }
    for (const auto& circle : sketch_.circles()) {
      QPolygonF curve;
      for (int step = 0; step <= 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        curve << project(
            {static_cast<float>(circle.center.xMm + circle.radiusMm * std::cos(angle)) + offsetX_,
             static_cast<float>(circle.center.yMm + circle.radiusMm * std::sin(angle)) + offsetY_, 0.0F},
            size(), yaw_, pitch_, zoom_);
      }
      painter.drawPolyline(curve);
    }
  }

  if (sketchVisible_ && !displaySketches_.empty()) {
    painter.setBrush(Qt::NoBrush);
    for (const auto& displayed : displaySketches_) {
      if (!displayed.visible) continue;
      painter.setPen(QPen(QColor("#1469d7"), 2.2));
      for (const auto& line : displayed.geometry.lines())
        painter.drawLine(
            project(pointOnSupport(line.start, displayed.supportName, box_,
                                   offsetX_, offsetY_),
                    size(), yaw_, pitch_, zoom_),
            project(pointOnSupport(line.end, displayed.supportName, box_,
                                   offsetX_, offsetY_),
                    size(), yaw_, pitch_, zoom_));
      for (const auto& circle : displayed.geometry.circles()) {
        QPolygonF curve;
        for (int step = 0; step <= 64; ++step) {
          const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
          const sketch::Point point{
              circle.center.xMm + circle.radiusMm * std::cos(angle),
              circle.center.yMm + circle.radiusMm * std::sin(angle)};
          curve << project(pointOnSupport(point, displayed.supportName, box_,
                                          offsetX_, offsetY_),
                           size(), yaw_, pitch_, zoom_);
        }
        painter.drawPolyline(curve);
      }
    }
  }

  if (pickMode_ == PickMode::ExtrusionSurface &&
      !extrusionHoverPolygon_.isEmpty()) {
    painter.setBrush(QColor(10, 105, 245, 55));
    painter.setPen(QPen(QColor("#0969e8"), 2.2));
    painter.drawPolygon(extrusionHoverPolygon_);
  }

  if (originVisible_) {
    const QPointF origin = project({0, 0, 0}, size(), yaw_, pitch_, zoom_);
    const QPointF xEnd = project({18, 0, 0}, size(), yaw_, pitch_, zoom_);
    const QPointF yEnd = project({0, 18, 0}, size(), yaw_, pitch_, zoom_);
    const QPointF zEnd = project({0, 0, 18}, size(), yaw_, pitch_, zoom_);
    painter.setPen(QPen(QColor("#dc3f45"), 2.0));
    painter.drawLine(origin, xEnd);
    painter.drawText(xEnd + QPointF(5, 3), "X");
    painter.setPen(QPen(QColor("#239653"), 2.0));
    painter.drawLine(origin, yEnd);
    painter.drawText(yEnd + QPointF(5, 3), "Y");
    painter.setPen(QPen(QColor("#1769d2"), 2.0));
    painter.drawLine(origin, zEnd);
    painter.drawText(zEnd + QPointF(5, 3), "Z");
    painter.setBrush(Qt::white);
    painter.drawEllipse(origin, 4.0, 4.0);
  }

  // Orientation cube: it uses the same yaw and pitch as the scene camera.
  const QPointF cubeCenter(width() - 68.0, 62.0);
  const QSize cubeProjectionSize(84, 84);
  const float c = 28.0F;
  const std::array<Point3, 8> cubeVertices{{{-c, -c, -c}, {c, -c, -c},
                                            {c, c, -c}, {-c, c, -c},
                                            {-c, -c, c}, {c, -c, c},
                                            {c, c, c}, {-c, c, c}}};
  const std::array<std::array<int, 4>, 6> cubeFaces{{{{0, 1, 2, 3}}, {{4, 7, 6, 5}},
                                                       {{0, 4, 5, 1}}, {{1, 5, 6, 2}},
                                                       {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};
  for (std::size_t face = 0; face < cubeFaces.size(); ++face) {
    QPolygonF polygon;
    for (int vertex : cubeFaces[face]) {
      QPointF point = project(cubeVertices[vertex], cubeProjectionSize,
                              yaw_, pitch_, 1.0F);
      point += cubeCenter - QPointF(cubeProjectionSize.width() * 0.5,
                                    cubeProjectionSize.height() * 0.52);
      polygon << point;
    }
    painter.setBrush(face == 1 ? QColor("#e8f2ff") : QColor("#f8fbff"));
    painter.setPen(QPen(QColor("#7891b5"), 1.0));
    painter.drawPolygon(polygon);
  }
  painter.setPen(QColor("#36577f"));
  painter.drawText(QRectF(width() - 112.0, 103.0, 88.0, 20.0),
                   Qt::AlignCenter, QString::fromUtf8("Ориентация"));

  painter.setPen(QColor(171, 184, 201));
  painter.drawText(16, height() - 18, "Drag to orbit  •  Wheel to zoom");
}

void Viewport::mousePressEvent(QMouseEvent* event) {
  lastMousePosition_ = event->position().toPoint();
  draggingBody_ = false;
  if (event->button() != Qt::LeftButton) return;
  if (pickMode_ == PickMode::ExtrusionSurface &&
      !extrusionHoverPolygon_.isEmpty()) {
    selectedExtrusionSketch_ = hoveredExtrusionSketch_;
    selectedExtrusionSupport_ = hoveredExtrusionSupport_;
    selectedExtrusionSketchIndex_ = hoveredExtrusionSketchIndex_;
    pickMode_ = PickMode::None;
    unsetCursor();
    emit extrusionSurfacePicked(hoveredExtrusionSurface_);
    extrusionHoverPolygon_.clear();
    update();
    return;
  }
  const float x = static_cast<float>(box_.widthMm) * 0.5F;
  const float y = static_cast<float>(box_.depthMm) * 0.5F;
  const float z = static_cast<float>(box_.heightMm);
  const std::array<Point3, 8> vertices{{
      {-x + offsetX_, -y + offsetY_, 0}, {x + offsetX_, -y + offsetY_, 0},
      {x + offsetX_, y + offsetY_, 0}, {-x + offsetX_, y + offsetY_, 0},
      {-x + offsetX_, -y + offsetY_, z}, {x + offsetX_, -y + offsetY_, z},
      {x + offsetX_, y + offsetY_, z}, {-x + offsetX_, y + offsetY_, z}}};
  const std::array<std::array<int, 4>, 6> faces{{{{0, 1, 2, 3}}, {{4, 7, 6, 5}},
                                                  {{0, 4, 5, 1}}, {{1, 5, 6, 2}},
                                                  {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};

  if (pickMode_ == PickMode::SketchPlane && solidVisible_) {
    for (int faceIndex = 5; faceIndex >= 0; --faceIndex) {
      QPolygonF polygon;
      for (int vertex : faces[faceIndex])
        polygon << project(vertices[vertex], size(), yaw_, pitch_, zoom_);
      if (!isFrontFacing(polygon)) continue;
      if (!polygon.containsPoint(event->position(), Qt::OddEvenFill)) continue;
      selectedFace_ = faceIndex;
      pickMode_ = PickMode::None;
      unsetCursor();
      emit sketchPlanePicked(QString::fromUtf8("Грань тела: ") + selectedFaceName());
      update();
      return;
    }
  }

  if (pickMode_ == PickMode::SketchPlane) {
    const float planeSize = std::max(35.0F, std::max(x, y) * 1.35F);
    const std::array<std::array<Point3, 4>, 3> planes{{
        {{{-planeSize, -planeSize, 0}, {planeSize, -planeSize, 0},
           {planeSize, planeSize, 0}, {-planeSize, planeSize, 0}}},
        {{{-planeSize, 0, -planeSize}, {planeSize, 0, -planeSize},
           {planeSize, 0, planeSize}, {-planeSize, 0, planeSize}}},
        {{{0, -planeSize, -planeSize}, {0, planeSize, -planeSize},
           {0, planeSize, planeSize}, {0, -planeSize, planeSize}}}}};
    for (int plane = 2; plane >= 0; --plane) {
      if (!basePlanesVisible_[plane]) continue;
      QPolygonF polygon;
      for (const auto& point : planes[plane])
        polygon << project(point, size(), yaw_, pitch_, zoom_);
      if (polygon.containsPoint(event->position(), Qt::OddEvenFill)) {
        const QString name = plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ";
        pickMode_ = PickMode::None;
        unsetCursor();
        emit sketchPlanePicked(QString::fromUtf8("Базовая плоскость ") + name);
        update();
        return;
      }
    }
  }

  if (pickMode_ == PickMode::ExtrusionSurface && !sketch_.lines().empty()) {
    QPolygonF contour;
    for (const auto& line : sketch_.lines())
      contour << project({static_cast<float>(line.start.xMm) + offsetX_,
                          static_cast<float>(line.start.yMm) + offsetY_, 0},
                         size(), yaw_, pitch_, zoom_);
    if (contour.size() >= 3 &&
        contour.containsPoint(event->position(), Qt::OddEvenFill)) {
      pickMode_ = PickMode::None;
      unsetCursor();
      emit extrusionSurfacePicked(QString::fromUtf8("Замкнутый контур эскиза"));
      update();
      return;
    }
  }
  if (pickMode_ == PickMode::ExtrusionSurface) {
    for (const auto& circle : sketch_.circles()) {
      QPolygonF contour;
      for (int step = 0; step < 48; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 48.0F;
        contour << project(
            {static_cast<float>(circle.center.xMm + circle.radiusMm * std::cos(angle)) + offsetX_,
             static_cast<float>(circle.center.yMm + circle.radiusMm * std::sin(angle)) + offsetY_, 0},
            size(), yaw_, pitch_, zoom_);
      }
      if (!contour.containsPoint(event->position(), Qt::OddEvenFill)) continue;
      pickMode_ = PickMode::None;
      unsetCursor();
      emit extrusionSurfacePicked(QString::fromUtf8("Замкнутый контур эскиза"));
      update();
      return;
    }
  }

  if (!solidVisible_) return;
  selectedFace_ = -1;
  for (int faceIndex = 5; faceIndex >= 0; --faceIndex) {
    QPolygonF polygon;
    for (int vertex : faces[faceIndex])
      polygon << project(vertices[vertex], size(), yaw_, pitch_, zoom_);
    if (!isFrontFacing(polygon)) continue;
    if (polygon.containsPoint(event->position(), Qt::OddEvenFill)) {
      selectedFace_ = faceIndex;
      draggingBody_ = pickMode_ == PickMode::None;
      break;
    }
  }
  emit selectionChanged(selectedFace_ >= 0
                            ? QString::fromUtf8("Тело 1 • Грань: ") + selectedFaceName()
                            : QString{});
  if (selectedFace_ >= 0 && pickMode_ == PickMode::SketchPlane) {
    pickMode_ = PickMode::None;
    unsetCursor();
    emit sketchPlanePicked(QString::fromUtf8("Грань тела: ") + selectedFaceName());
  } else if (selectedFace_ >= 0 && pickMode_ == PickMode::ExtrusionSurface) {
    pickMode_ = PickMode::None;
    unsetCursor();
    emit extrusionSurfacePicked(QString::fromUtf8("Грань тела: ") + selectedFaceName());
  }
  update();
}

void Viewport::updateExtrusionHover(QPointF position) {
  extrusionHoverPolygon_.clear();
  hoveredExtrusionSketch_.clear();
  hoveredExtrusionSupport_.clear();
  hoveredExtrusionSurface_.clear();
  hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);

  std::size_t reverseIndex = displaySketches_.size();
  for (auto displayed = displaySketches_.rbegin();
       displayed != displaySketches_.rend() && extrusionHoverPolygon_.isEmpty();
       ++displayed) {
    const std::size_t displayedIndex = --reverseIndex;
    if (!displayed->visible) continue;
    std::vector<std::size_t> ids;
    for (const auto& line : displayed->geometry.lines())
      if (std::find(ids.begin(), ids.end(), line.elementId) == ids.end())
        ids.push_back(line.elementId);
    for (const std::size_t id : ids) {
      QPolygonF polygon;
      sketch::Sketch candidate;
      for (const auto& line : displayed->geometry.lines()) {
        if (line.elementId != id) continue;
        polygon << project(pointOnSupport(line.start, displayed->supportName,
                                          box_, offsetX_, offsetY_),
                           size(), yaw_, pitch_, zoom_);
        candidate.addLine(line.start, line.end);
      }
      if (polygon.size() < 3 ||
          !polygon.containsPoint(position, Qt::OddEvenFill))
        continue;
      extrusionHoverPolygon_ = polygon;
      hoveredExtrusionSketch_ = candidate;
      hoveredExtrusionSupport_ = displayed->supportName;
      hoveredExtrusionSurface_ = QString::fromUtf8("Замкнутый контур эскиза");
      hoveredExtrusionSketchIndex_ = displayedIndex;
      break;
    }
    for (const auto& circle : displayed->geometry.circles()) {
      QPolygonF polygon;
      for (int step = 0; step < 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const sketch::Point point{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        polygon << project(pointOnSupport(point, displayed->supportName, box_,
                                          offsetX_, offsetY_),
                           size(), yaw_, pitch_, zoom_);
      }
      if (!polygon.containsPoint(position, Qt::OddEvenFill)) continue;
      sketch::Sketch candidate;
      candidate.addCircle(circle.center, circle.radiusMm);
      extrusionHoverPolygon_ = polygon;
      hoveredExtrusionSketch_ = candidate;
      hoveredExtrusionSupport_ = displayed->supportName;
      hoveredExtrusionSurface_ = QString::fromUtf8("Замкнутый контур эскиза");
      hoveredExtrusionSketchIndex_ = displayedIndex;
      break;
    }
  }

  if (!extrusionHoverPolygon_.isEmpty() || !solidVisible_) return;
  const float x = static_cast<float>(box_.widthMm) * 0.5F;
  const float y = static_cast<float>(box_.depthMm) * 0.5F;
  const float z = static_cast<float>(box_.heightMm);
  const std::array<Point3, 8> vertices{{
      {-x + offsetX_, -y + offsetY_, 0}, {x + offsetX_, -y + offsetY_, 0},
      {x + offsetX_, y + offsetY_, 0}, {-x + offsetX_, y + offsetY_, 0},
      {-x + offsetX_, -y + offsetY_, z}, {x + offsetX_, -y + offsetY_, z},
      {x + offsetX_, y + offsetY_, z}, {-x + offsetX_, y + offsetY_, z}}};
  const std::array<std::array<int, 4>, 6> faces{{
      {{0, 1, 2, 3}}, {{4, 7, 6, 5}}, {{0, 4, 5, 1}},
      {{1, 5, 6, 2}}, {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};
  static const std::array<const char*, 6> names{
      "Нижняя", "Верхняя", "Передняя", "Правая", "Задняя", "Левая"};
  for (int face = 5; face >= 0; --face) {
    QPolygonF polygon;
    for (int vertex : faces[face])
      polygon << project(vertices[vertex], size(), yaw_, pitch_, zoom_);
    if (!isFrontFacing(polygon) ||
        !polygon.containsPoint(position, Qt::OddEvenFill))
      continue;
    extrusionHoverPolygon_ = polygon;
    hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела: ") +
                               QString::fromUtf8(names[face]);
    break;
  }
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
  if (pickMode_ == PickMode::ExtrusionSurface) {
    updateExtrusionHover(event->position());
    update();
    return;
  }
  if (draggingBody_ && event->buttons().testFlag(Qt::LeftButton)) {
    const QPoint delta = event->position().toPoint() - lastMousePosition_;
    const float scale = std::max(0.01F, std::min(width(), height()) * 0.008F * zoom_);
    offsetX_ += static_cast<float>(delta.x()) / scale;
    offsetY_ -= static_cast<float>(delta.y()) / scale;
    lastMousePosition_ = event->position().toPoint();
    update();
  } else if (event->buttons().testFlag(Qt::RightButton) ||
             (event->buttons().testFlag(Qt::LeftButton) && !solidVisible_)) {
    const QPoint delta = event->position().toPoint() - lastMousePosition_;
    yaw_ += static_cast<float>(delta.x()) * 0.5F;
    pitch_ += static_cast<float>(delta.y()) * 0.5F;
    lastMousePosition_ = event->position().toPoint();
    update();
  }
}

void Viewport::wheelEvent(QWheelEvent* event) {
  zoom_ = std::clamp(zoom_ * (event->angleDelta().y() > 0 ? 1.1F : 0.9F),
                     0.25F, 5.0F);
  update();
}

}  // namespace solidar
