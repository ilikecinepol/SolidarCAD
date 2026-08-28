#include "ui/Viewport.h"

#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pnt.hxx>

#include <QLineF>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace solidar {
namespace {

struct Point3 {
  float x;
  float y;
  float z;
};

struct ProjectedPoint {
  QPointF screen;
  double depth{};
};

ProjectedPoint projectBodyPoint(Point3d point, Point3d center, const QSize& size,
                                float yaw, float pitch, float zoom) {
  // Keep the solid in the same world-space projection as sketches and
  // construction geometry. Fit All supplies the screen-space centering.
  (void)center;
  const double x = point.x;
  const double y = point.y;
  const double z = point.z;
  const double yr = yaw * std::numbers::pi / 180.0;
  const double pr = pitch * std::numbers::pi / 180.0;
  const double x1 = x * std::cos(yr) - y * std::sin(yr);
  const double y1 = x * std::sin(yr) + y * std::cos(yr);
  const double y2 = y1 * std::cos(pr) - z * std::sin(pr);
  const double depth = y1 * std::sin(pr) + z * std::cos(pr);
  const double scale = std::min(size.width(), size.height()) * 0.008 * zoom;
  return {{size.width() * 0.5 + x1 * scale,
           size.height() * 0.52 + y2 * scale}, depth};
}

double pointSegmentDistance(QPointF point, QPointF a, QPointF b) {
  const QPointF ab = b - a;
  const double length2 = QPointF::dotProduct(ab, ab);
  if (length2 < 1e-12) return QLineF(point, a).length();
  const double t = std::clamp(QPointF::dotProduct(point - a, ab) / length2,
                              0.0, 1.0);
  return QLineF(point, a + ab * t).length();
}

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
  const float direction = support.contains(QStringLiteral("|NEG")) ? -1.0F : 1.0F;
  if (support.contains("XZ") || support.contains(QString::fromUtf8("Передняя")) ||
      support.contains(QString::fromUtf8("Задняя")))
    return {0.0F, direction, 0.0F};
  if (support.contains("YZ") || support.contains(QString::fromUtf8("Правая")) ||
      support.contains(QString::fromUtf8("Левая")))
    return {direction, 0.0F, 0.0F};
  return {0.0F, 0.0F, direction};
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

Viewport::Viewport(QWidget* parent) : QWidget(parent) {
  setMinimumSize(480, 320);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  extrusionLengthEditor_ = new QDoubleSpinBox(this);
  extrusionLengthEditor_->setRange(-100000.0, 100000.0);
  extrusionLengthEditor_->setDecimals(2);
  extrusionLengthEditor_->setSuffix(QStringLiteral(" mm"));
  extrusionLengthEditor_->setFixedSize(132, 42);
  extrusionLengthEditor_->setStyleSheet(
      "QDoubleSpinBox{background:#ffffff;color:#20252c;border:1px solid #c8d1de;"
      "border-radius:8px;padding:6px 8px;font-size:16px;}"
      "QDoubleSpinBox:focus{border:2px solid #1477ed;}"
      "QDoubleSpinBox::up-button,QDoubleSpinBox::down-button{width:28px;"
      "border:none;background:transparent;}");
  extrusionLengthEditor_->hide();
  connect(extrusionLengthEditor_, &QDoubleSpinBox::valueChanged, this,
          &Viewport::setExtrusionPreviewLength);
}

void Viewport::setBox(BoxParameters parameters) {
  box_ = parameters;
  update();
}

Point3 pointOnPlacement(sketch::Point point,
                        const SketchPlacement& placement,
                        float offsetX, float offsetY) {
  const auto world = placement.toWorld(point.xMm, point.yMm);
  return {static_cast<float>(world.x) + offsetX,
          static_cast<float>(world.y) + offsetY,
          static_cast<float>(world.z)};
}

SketchPlacement legacyPlacement(const QString& support) {
  if (support.contains(QStringLiteral("XZ"))) return SketchPlacement::xz();
  if (support.contains(QStringLiteral("YZ"))) return SketchPlacement::yz();
  return SketchPlacement::xy();
}

std::vector<QPolygonF> projectedBodyFaces(const TopoDS_Shape& shape,
                                          const QSize& size, float yaw,
                                          float pitch, float zoom,
                                          float offsetX, float offsetY) {
  std::vector<QPolygonF> polygons;
  for (TopExp_Explorer faceExplorer(shape, TopAbs_FACE); faceExplorer.More();
       faceExplorer.Next()) {
    const TopoDS_Face face = TopoDS::Face(faceExplorer.Current());
    const TopoDS_Wire wire = BRepTools::OuterWire(face);
    QPolygonF polygon;
    if (wire.IsNull()) {
      polygons.push_back(std::move(polygon));
      continue;
    }
    for (BRepTools_WireExplorer vertexExplorer(wire, face);
         vertexExplorer.More(); vertexExplorer.Next()) {
      const gp_Pnt point = BRep_Tool::Pnt(vertexExplorer.CurrentVertex());
      polygon << project({static_cast<float>(point.X()) + offsetX,
                          static_cast<float>(point.Y()) + offsetY,
                          static_cast<float>(point.Z())},
                         size, yaw, pitch, zoom);
    }
    polygons.push_back(std::move(polygon));
  }
  return polygons;
}

void Viewport::setBodyShape(ShapeFeature::ShapePtr shape, BodyId bodyId,
                            FeatureId featureId) {
  std::vector<BodyViewShape> shapes;
  if (shape) shapes.push_back({bodyId, featureId, std::move(shape)});
  setBodyShapes(std::move(shapes));
}

void Viewport::setBodyShapes(std::vector<BodyViewShape> shapes) {
  bodyViewShapes_ = shapes;
  rebuildBodyDisplay(shapes, true);
}

void Viewport::rebuildBodyDisplay(const std::vector<BodyViewShape>& shapes,
                                  bool clearSelection) {
  bodyShape_.reset();
  bodyId_ = kInvalidBodyId;
  bodyFeatureId_ = kInvalidFeatureId;
  selectedFace_ = -1;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  if (clearSelection) {
    selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
    selectedBodyEdgeIndices_.clear();
  }
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  bodyRenderMesh_.clear();
  bodyTopologyRanges_.clear();
  TopoDS_Compound compound;
  BRep_Builder compoundBuilder;
  compoundBuilder.MakeCompound(compound);
  std::size_t firstFace = 0;
  std::size_t firstEdge = 0;
  for (const auto& item : shapes) {
    if (!item.shape || item.shape->IsNull()) continue;
    std::size_t faceCount = 0;
    std::size_t edgeCount = 0;
    for (TopExp_Explorer faces(*item.shape, TopAbs_FACE); faces.More();
         faces.Next())
      ++faceCount;
    for (TopExp_Explorer edges(*item.shape, TopAbs_EDGE); edges.More();
         edges.Next())
      ++edgeCount;
    bodyTopologyRanges_.push_back({item.bodyId, item.featureId,
        firstFace, faceCount, firstEdge, edgeCount});
    firstFace += faceCount;
    firstEdge += edgeCount;
    compoundBuilder.Add(compound, *item.shape);
    bodyId_ = item.bodyId;
    bodyFeatureId_ = item.featureId;
  }
  if (!bodyTopologyRanges_.empty())
    bodyShape_ = std::make_shared<TopoDS_Shape>(compound);
  if (bodyShape_ && !bodyShape_->IsNull()) {
    bodyRenderMesh_.rebuild(*bodyShape_);
    Bnd_Box bounds;
    BRepBndLib::Add(*bodyShape_, bounds);
    double xMin = 0.0;
    double yMin = 0.0;
    double zMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    double zMax = 0.0;
    bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
    box_ = {xMax - xMin, yMax - yMin, zMax - zMin};
  }
  update();
}

void Viewport::setToolPreviewShape(BodyId bodyId, FeatureId featureId,
                                   ShapeFeature::ShapePtr shape) {
  auto display = bodyViewShapes_;
  for (auto& item : display)
    if (item.bodyId == bodyId) {
      item.shape = std::move(shape);
      item.featureId = featureId;
      break;
    }
  rebuildBodyDisplay(display, false);
}

void Viewport::clearToolPreviewShape() {
  rebuildBodyDisplay(bodyViewShapes_, false);
}

void Viewport::setSketch(const sketch::Sketch& sketch) {
  setSketch(sketch, SketchPlacement::xy());
}

void Viewport::setSketch(const sketch::Sketch& sketch,
                         const SketchPlacement& placement) {
  sketch_ = sketch;
  sketchPlacement_ = placement;
  update();
}

void Viewport::setSolidSketch(const sketch::Sketch& sketch) {
  solidSketch_ = sketch;
  update();
}

void Viewport::commitAdditiveExtrusion(const sketch::Sketch& sketch,
                                       const QString& supportName,
                                       double startMm, double lengthMm) {
  additiveExtrusions_.push_back({sketch, supportName, startMm, lengthMm});
  update();
}

void Viewport::removeLastAdditiveExtrusion() {
  if (!additiveExtrusions_.empty()) additiveExtrusions_.pop_back();
  update();
}

const std::vector<SolidFeature>& Viewport::solidFeatures() const noexcept {
  return additiveExtrusions_;
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
  addSketch(sketch, supportName, legacyPlacement(supportName));
}

void Viewport::addSketch(const sketch::Sketch& sketch,
                         const QString& supportName,
                         const SketchPlacement& placement) {
  sketch_ = sketch;
  sketchPlacement_ = placement;
  displaySketches_.push_back({sketch, supportName, placement, true});
  update();
}

void Viewport::updateSketch(std::size_t index, const sketch::Sketch& sketch,
                            const QString& supportName) {
  updateSketch(index, sketch, supportName, legacyPlacement(supportName));
}

void Viewport::updateSketch(std::size_t index, const sketch::Sketch& sketch,
                            const QString& supportName,
                            const SketchPlacement& placement) {
  if (index >= displaySketches_.size()) return;
  displaySketches_[index].geometry = sketch;
  displaySketches_[index].supportName = supportName;
  displaySketches_[index].placement = placement;
  sketch_ = sketch;
  sketchPlacement_ = placement;
  update();
}

void Viewport::removeSketch(std::size_t index) {
  if (index >= displaySketches_.size()) return;
  displaySketches_.erase(displaySketches_.begin() +
                         static_cast<std::ptrdiff_t>(index));
  sketch_ = displaySketches_.empty() ? sketch::Sketch{}
                                     : displaySketches_.back().geometry;
  selectedExtrusionSketch_.clear();
  hoveredExtrusionSketch_.clear();
  selectedExtrusionPolygon_.clear();
  selectedExtrusionPolygons_.clear();
  selectedExtrusionPaths_.clear();
  selectedExtrusionRegionSketches_.clear();
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  selectedExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  selectedExtrusionBodyFace_ = false;
  selectedExtrusionOnBodyCap_ = false;
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
  bodyShape_.reset();
  bodyRenderMesh_.clear();
  bodyTopologyRanges_.clear();
  bodyViewShapes_.clear();
  bodyId_ = kInvalidBodyId;
  bodyFeatureId_ = kInvalidFeatureId;
  sketch_.clear();
  solidSketch_.clear();
  additiveExtrusions_.clear();
  displaySketches_.clear();
  extrusionHoverPolygon_.clear();
  selectedExtrusionPolygon_.clear();
  selectedExtrusionPolygons_.clear();
  selectedExtrusionPaths_.clear();
  selectedExtrusionRegionSketches_.clear();
  selectedExtrusionSketch_.clear();
  selectedExtrusionSupport_.clear();
  selectedExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
  solidVisible_ = false;
  sketchVisible_ = true;
  selectedFace_ = -1;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBodyEdgeIndices_.clear();
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBasePlane_ = -1;
  selectedVertex_ = -1;
  selectedOrigin_ = false;
  pickMode_ = PickMode::None;
  offsetX_ = 0.0F;
  offsetY_ = 0.0F;
  cameraPan_ = {};
  hideExtrusionManipulator();
  for (bool& visible : basePlanesVisible_) visible = false;
  update();
}

void Viewport::beginSketchPlaneSelection() {
  pickMode_ = PickMode::SketchPlane;
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  hoveredExtrusionSurface_.clear();
  hoveredExtrusionSupport_.clear();
  selectedFace_ = -1;
  selectedBasePlane_ = -1;
  selectedVertex_ = -1;
  selectedOrigin_ = false;
  for (bool& visible : basePlanesVisible_) visible = true;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::beginExtrusionSurfaceSelection() {
  pickMode_ = PickMode::ExtrusionSurface;
  selectedExtrusionPolygons_.clear();
  selectedExtrusionPaths_.clear();
  selectedExtrusionRegionSketches_.clear();
  selectedExtrusionPolygon_.clear();
  selectedExtrusionSketch_.clear();
  selectedExtrusionSupport_.clear();
  selectedExtrusionBodyFace_ = false;
  selectedExtrusionOnBodyCap_ = false;
  selectedFace_ = -1;
  selectedBasePlane_ = -1;
  selectedVertex_ = -1;
  selectedOrigin_ = false;
  setCursor(Qt::CrossCursor);
  update();
}

void Viewport::showExtrusionManipulator(double lengthMm) {
  extrusionManipulatorVisible_ = true;
  setExtrusionPreviewLength(lengthMm);
  extrusionLengthEditor_->show();
  extrusionLengthEditor_->raise();
  update();
}

void Viewport::hideExtrusionManipulator() {
  extrusionManipulatorVisible_ = false;
  draggingExtrusionHandle_ = false;
  extrusionLengthEditor_->hide();
  update();
}

void Viewport::setExtrusionPreviewLength(double lengthMm) {
  extrusionPreviewLengthMm_ = std::clamp(lengthMm, -100000.0, 100000.0);
  if (extrusionLengthEditor_ &&
      !qFuzzyCompare(extrusionLengthEditor_->value(), extrusionPreviewLengthMm_)) {
    const QSignalBlocker blocker(extrusionLengthEditor_);
    extrusionLengthEditor_->setValue(extrusionPreviewLengthMm_);
  }
  if (extrusionManipulatorVisible_) {
    const QPointF handle = extrusionManipulatorAnchor_ + extrusionScreenOffset() + cameraPan_;
    extrusionLengthEditor_->move(
        std::clamp(static_cast<int>(handle.x() + 16), 4,
                   std::max(4, width() - extrusionLengthEditor_->width() - 4)),
        std::clamp(static_cast<int>(handle.y() - 15), 4,
                   std::max(4, height() - extrusionLengthEditor_->height() - 4)));
  }
  emit extrusionPreviewLengthChanged(extrusionPreviewLengthMm_);
  update();
}

QPointF Viewport::extrusionScreenOffset() const {
  const QString support = selectedExtrusionSupport_.isEmpty()
                              ? solidSupportName_
                              : selectedExtrusionSupport_;
  const Point3 normal = supportNormal(support);
  const QPointF origin = project({0.0F, 0.0F, 0.0F}, size(), yaw_, pitch_, zoom_);
  const Point3 end3 = translated({0.0F, 0.0F, 0.0F}, normal,
                                 static_cast<float>(extrusionPreviewLengthMm_));
  return project(end3, size(), yaw_, pitch_, zoom_) - origin;
}

bool Viewport::hasSelectedFace() const noexcept { return selectedFace_ >= 0; }

QString Viewport::selectedFaceName() const {
  static const std::array<const char*, 6> names{
      "Нижняя", "Верхняя", "Передняя",
      "Правая", "Задняя", "Левая"};
  if (selectedFace_ < 0) return {};
  if (selectedFace_ < static_cast<int>(names.size()))
    return QString::fromUtf8(names[static_cast<std::size_t>(selectedFace_)]);
  return QString::fromUtf8("Грань #%1").arg(selectedFace_ + 1);
}

std::optional<std::size_t> Viewport::selectedBodyFaceIndex() const noexcept {
  return selectedFace_ >= 0
             ? std::optional<std::size_t>(static_cast<std::size_t>(selectedFace_))
             : std::nullopt;
}

std::optional<FaceReference> Viewport::selectedBodyFace() const noexcept {
  if (selectedFace_ < 0) return std::nullopt;
  const auto global = static_cast<std::size_t>(selectedFace_);
  for (const auto& range : bodyTopologyRanges_)
    if (global >= range.firstFace && global < range.firstFace + range.faceCount)
      return FaceReference{range.bodyId, range.featureId,
                           global - range.firstFace};
  return std::nullopt;
}

std::optional<EdgeReference> Viewport::selectedBodyEdge() const noexcept {
  if (selectedBodyEdgeIndices_.empty()) return std::nullopt;
  return edgeReferenceForGlobalIndex(selectedBodyEdgeIndices_.front());
}

std::optional<EdgeReference> Viewport::edgeReferenceForGlobalIndex(
    std::size_t global) const noexcept {
  for (const auto& range : bodyTopologyRanges_)
    if (global >= range.firstEdge && global < range.firstEdge + range.edgeCount)
      return EdgeReference{range.bodyId, range.featureId,
                            global - range.firstEdge};
  return std::nullopt;
}

std::vector<EdgeReference> Viewport::selectedBodyEdges() const {
  std::vector<EdgeReference> result;
  for (const auto index : selectedBodyEdgeIndices_)
    if (const auto edge = edgeReferenceForGlobalIndex(index))
      result.push_back(*edge);
  return result;
}

void Viewport::setSelectedBodyEdges(const std::vector<EdgeReference>& edges) {
  selectedBodyEdgeIndices_.clear();
  for (const auto& edge : edges)
    for (const auto& range : bodyTopologyRanges_)
      if (range.bodyId == edge.bodyId && range.featureId == edge.featureId &&
          edge.edgeIndex < range.edgeCount)
        selectedBodyEdgeIndices_.push_back(range.firstEdge + edge.edgeIndex);
  selectedBodyEdgeIndex_ = selectedBodyEdgeIndices_.empty()
                               ? static_cast<std::size_t>(-1)
                               : selectedBodyEdgeIndices_.front();
  update();
}

void Viewport::setToolManipulator(const LinearToolManipulator& manipulator) {
  toolManipulator_ = manipulator;
  update();
}

void Viewport::clearToolManipulator() {
  toolManipulator_.reset();
  draggingToolManipulator_ = false;
  update();
}

void Viewport::fitAll() {
  if (bodyRenderMesh_.diagonal() <= 1e-9) return;
  double minX = std::numeric_limits<double>::max();
  double minY = minX;
  double maxX = -minX;
  double maxY = -minX;
  for (const auto& triangle : bodyRenderMesh_.triangles()) {
    for (const Point3d point : {triangle.a, triangle.b, triangle.c}) {
      const auto projected = projectBodyPoint(point, bodyRenderMesh_.center(),
                                              size(), yaw_, pitch_, 1.0F);
      minX = std::min(minX, projected.screen.x());
      minY = std::min(minY, projected.screen.y());
      maxX = std::max(maxX, projected.screen.x());
      maxY = std::max(maxY, projected.screen.y());
    }
  }
  const double projectedWidth = std::max(1.0, maxX - minX);
  const double projectedHeight = std::max(1.0, maxY - minY);
  zoom_ = static_cast<float>(std::clamp(
      0.88 * std::min(width() / projectedWidth, height() / projectedHeight),
      0.02, 100.0));
  const QPointF viewportCenter(width() * 0.5, height() * 0.52);
  const QPointF boundsCenter((minX + maxX) * 0.5, (minY + maxY) * 0.5);
  cameraPan_ = viewportCenter -
      (viewportCenter + (boundsCenter - viewportCenter) * zoom_);
  update();
}

void Viewport::viewTop() { yaw_ = 0; pitch_ = 0; fitAll(); }
void Viewport::viewBottom() { yaw_ = 0; pitch_ = 180; fitAll(); }
void Viewport::viewFront() { yaw_ = 0; pitch_ = -90; fitAll(); }
void Viewport::viewBack() { yaw_ = 0; pitch_ = 90; fitAll(); }
void Viewport::viewRight() { yaw_ = 90; pitch_ = 90; fitAll(); }
void Viewport::viewLeft() { yaw_ = -90; pitch_ = 90; fitAll(); }
void Viewport::viewIsometric() { yaw_ = -45; pitch_ = 30; fitAll(); }

const sketch::Sketch& Viewport::extrusionCandidateSketch() const noexcept {
  return selectedExtrusionSketch_;
}

QString Viewport::extrusionCandidateSupport() const {
  return selectedExtrusionSupport_;
}

std::size_t Viewport::extrusionCandidateSketchIndex() const noexcept {
  return selectedExtrusionSketchIndex_;
}

bool Viewport::extrusionCandidateOnBodyCap() const noexcept {
  return selectedExtrusionOnBodyCap_;
}

const sketch::Sketch& Viewport::solidSketch() const noexcept {
  return solidSketch_;
}

QString Viewport::solidSupport() const { return solidSupportName_; }

QPointF Viewport::bodyPosition() const noexcept { return {offsetX_, offsetY_}; }

void Viewport::setBodyPosition(QPointF position) {
  offsetX_ = static_cast<float>(position.x());
  offsetY_ = static_cast<float>(position.y());
  update();
}

void Viewport::refreshSelectedExtrusionPolygon() {
  // Screen coordinates become stale whenever a dock is opened, the viewport
  // is resized, or the camera changes.  Reproject the selected sketch from
  // model coordinates instead of moving the previously cached screen polygon.
  if (selectedExtrusionBodyFace_ &&
      (!solidSketch_.lines().empty() || !solidSketch_.circles().empty())) {
    QPolygonF polygon;
    const Point3 normal = supportNormal(solidSupportName_);
    const bool initialFace = selectedExtrusionSupport_.contains(
        QStringLiteral("|NEG"));
    const float distance = initialFace ? 0.0F
                                       : static_cast<float>(box_.heightMm);
    if (!solidSketch_.circles().empty()) {
      const auto& circle = solidSketch_.circles().front();
      for (int step = 0; step < 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const sketch::Point point{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnSupport(point, solidSupportName_, box_,
                                           offsetX_, offsetY_);
        polygon << project(translated(base, normal, distance), size(), yaw_,
                           pitch_, zoom_);
      }
    } else {
      for (const auto& line : solidSketch_.lines()) {
        const Point3 base = pointOnSupport(line.start, solidSupportName_, box_,
                                           offsetX_, offsetY_);
        polygon << project(translated(base, normal, distance), size(), yaw_,
                           pitch_, zoom_);
      }
    }
    if (initialFace) std::reverse(polygon.begin(), polygon.end());
    if (polygon.size() >= 3) {
      selectedExtrusionPolygon_ = polygon;
      if (selectedExtrusionPolygons_.size() == 1)
        selectedExtrusionPolygons_.front() = polygon;
      extrusionManipulatorAnchor_ = polygon.boundingRect().center();
    }
    return;
  }

  // Selected regions are model data, not screen artefacts. Reproject every
  // region after zoom, orbit, pan or a viewport resize so all Ctrl-selected
  // contours remain coincident with their sketches.
  if (!selectedExtrusionRegionSketches_.empty()) {
    selectedExtrusionPaths_.clear();
    selectedExtrusionPolygons_.clear();
    const auto projectSelectedPoint = [&](sketch::Point point) {
      Point3 base = pointOnSupport(
          point, selectedExtrusionOnBodyCap_ ? solidSupportName_
                                             : selectedExtrusionSupport_,
          box_, offsetX_, offsetY_);
      if (selectedExtrusionOnBodyCap_)
        base = translated(base, supportNormal(solidSupportName_),
                          static_cast<float>(box_.heightMm));
      return project(base, size(), yaw_, pitch_, zoom_);
    };
    for (const auto& regionSketch : selectedExtrusionRegionSketches_) {
      QPainterPath regionPath;
      regionPath.setFillRule(Qt::OddEvenFill);
      QPolygonF firstBoundary;
      QPolygonF boundary;
      for (const auto& line : regionSketch.lines()) {
        if (boundary.isEmpty()) boundary << projectSelectedPoint(line.start);
        boundary << projectSelectedPoint(line.end);
        if (boundary.size() >= 4 &&
            QLineF(boundary.front(), boundary.back()).length() < 0.5) {
          regionPath.addPolygon(boundary);
          regionPath.closeSubpath();
          if (firstBoundary.isEmpty()) firstBoundary = boundary;
          boundary.clear();
        }
      }
      if (!boundary.isEmpty()) {
        regionPath.addPolygon(boundary);
        regionPath.closeSubpath();
        if (firstBoundary.isEmpty()) firstBoundary = boundary;
      }
      for (const auto& circle : regionSketch.circles()) {
        QPolygonF circleBoundary;
        for (int step = 0; step < 96; ++step) {
          const float angle = 2.0F * std::numbers::pi_v<float> * step / 96.0F;
          circleBoundary << projectSelectedPoint(
              {circle.center.xMm + circle.radiusMm * std::cos(angle),
               circle.center.yMm + circle.radiusMm * std::sin(angle)});
        }
        regionPath.addPolygon(circleBoundary);
        regionPath.closeSubpath();
        if (firstBoundary.isEmpty()) firstBoundary = circleBoundary;
      }
      if (!regionPath.isEmpty()) {
        selectedExtrusionPaths_.push_back(regionPath);
        selectedExtrusionPolygons_.push_back(firstBoundary);
      }
    }
    if (!selectedExtrusionPaths_.empty()) {
      QRectF bounds;
      for (const auto& path : selectedExtrusionPaths_)
        bounds = bounds.united(path.boundingRect());
      selectedExtrusionPolygon_ = selectedExtrusionPolygons_.front();
      extrusionManipulatorAnchor_ = bounds.center();
      return;
    }
  }

  if (selectedExtrusionSketch_.lines().empty() &&
      selectedExtrusionSketch_.circles().empty())
    return;

  QPolygonF polygon;
  const auto projectSelectedPoint = [&](sketch::Point point) {
    Point3 base = pointOnSupport(point,
                                 selectedExtrusionOnBodyCap_ ? solidSupportName_
                                                             : selectedExtrusionSupport_,
                                 box_, offsetX_, offsetY_);
    if (selectedExtrusionOnBodyCap_)
      base = translated(base, supportNormal(solidSupportName_),
                        static_cast<float>(box_.heightMm));
    return project(base, size(), yaw_, pitch_, zoom_);
  };
  if (!selectedExtrusionSketch_.circles().empty()) {
    const auto& circle = selectedExtrusionSketch_.circles().front();
    for (int step = 0; step < 64; ++step) {
      const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
      const sketch::Point point{
          circle.center.xMm + circle.radiusMm * std::cos(angle),
          circle.center.yMm + circle.radiusMm * std::sin(angle)};
      polygon << projectSelectedPoint(point);
    }
  } else {
    for (const auto& line : selectedExtrusionSketch_.lines())
      polygon << projectSelectedPoint(line.start);
  }

  if (polygon.size() >= 3) {
    const QPointF previousCenter = selectedExtrusionPolygon_.boundingRect().center();
    const QPointF newCenter = polygon.boundingRect().center();
    if (selectedExtrusionOnBodyCap_ && !selectedExtrusionPaths_.empty() &&
        !selectedExtrusionPolygon_.isEmpty()) {
      QTransform translation;
      translation.translate(newCenter.x() - previousCenter.x(),
                            newCenter.y() - previousCenter.y());
      for (auto& path : selectedExtrusionPaths_) path = translation.map(path);
      for (auto& selectedPolygon : selectedExtrusionPolygons_)
        selectedPolygon.translate(newCenter - previousCenter);
    }
    selectedExtrusionPolygon_ = polygon;
    if (selectedExtrusionPolygons_.size() == 1)
      selectedExtrusionPolygons_.front() = polygon;
    extrusionManipulatorAnchor_ = polygon.boundingRect().center();
  }
}

void Viewport::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(rect(), QColor(246, 249, 252));

  painter.setPen(QPen(QColor(224, 231, 241), 1));
  constexpr int gridStep = 28;
  for (int x = width() / 2 % gridStep; x < width(); x += gridStep)
    painter.drawLine(x, 0, x, height());
  for (int y = height() / 2 % gridStep; y < height(); y += gridStep)
    painter.drawLine(0, y, width(), y);

  painter.save();
  painter.translate(cameraPan_);

  refreshSelectedExtrusionPolygon();
  const bool hasParametricBody = bodyShape_ && !bodyShape_->IsNull();

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
    painter.setBrush(selectedBasePlane_ == plane
                         ? QColor(30, 115, 245, 60) : planeColors[plane]);
    painter.setPen(QPen(selectedBasePlane_ == plane
                            ? QColor("#075eff") : planeColors[plane].darker(125),
                        selectedBasePlane_ == plane
                            ? 3.0 : pickMode_ == PickMode::SketchPlane ? 2.0 : 1.0,
                        selectedBasePlane_ == plane ? Qt::SolidLine : Qt::DashLine));
    painter.drawPolygon(polygon);
    painter.drawText(polygon.boundingRect().center(),
                     plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ");
  }

  if (solidVisible_ && hasParametricBody) {
    struct PaintedTriangle {
      QPolygonF polygon;
      double depth;
      double brightness;
      std::size_t faceIndex;
    };
    std::vector<PaintedTriangle> painted;
    painted.reserve(bodyRenderMesh_.triangles().size());
    const Point3d center = bodyRenderMesh_.center();
    const Vector3d light{0.25, -0.35, 0.90};
    for (const auto& triangle : bodyRenderMesh_.triangles()) {
      const auto a = projectBodyPoint(triangle.a, center, size(), yaw_, pitch_, zoom_);
      const auto b = projectBodyPoint(triangle.b, center, size(), yaw_, pitch_, zoom_);
      const auto c = projectBodyPoint(triangle.c, center, size(), yaw_, pitch_, zoom_);
      const double diffuse = std::max(0.0, triangle.normal.x * light.x +
          triangle.normal.y * light.y + triangle.normal.z * light.z);
      painted.push_back({QPolygonF{a.screen, b.screen, c.screen},
                         (a.depth + b.depth + c.depth) / 3.0,
                         0.48 + 0.42 * diffuse, triangle.faceIndex});
    }
    std::sort(painted.begin(), painted.end(),
              [](const auto& a, const auto& b) { return a.depth < b.depth; });
    painter.setPen(Qt::NoPen);
    for (const auto& triangle : painted) {
      QColor color(205, 214, 224);
      if (triangle.faceIndex == static_cast<std::size_t>(selectedFace_))
        color = QColor(54, 132, 245);
      else if (triangle.faceIndex == hoveredBodyFaceIndex_)
        color = QColor(126, 181, 247);
      color = color.darker(static_cast<int>(100.0 / triangle.brightness));
      painter.setBrush(color);
      painter.drawPolygon(triangle.polygon);
    }
    for (const auto& edge : bodyRenderMesh_.edges()) {
      const bool selected = std::find(selectedBodyEdgeIndices_.begin(),
                                      selectedBodyEdgeIndices_.end(),
                                      edge.edgeIndex) != selectedBodyEdgeIndices_.end();
      const bool hovered = edge.edgeIndex == hoveredBodyEdgeIndex_;
      painter.setPen(QPen(selected ? QColor("#ff8a00")
                                  : hovered ? QColor("#00a6ff")
                                            : QColor("#344353"),
                          selected ? 3.2 : hovered ? 2.8 : 1.15));
      for (std::size_t index = 1; index < edge.points.size(); ++index) {
        const auto a = projectBodyPoint(edge.points[index - 1], center, size(),
                                        yaw_, pitch_, zoom_);
        const auto b = projectBodyPoint(edge.points[index], center, size(),
                                        yaw_, pitch_, zoom_);
        const QPointF midpoint = (a.screen + b.screen) * 0.5;
        double surfaceDepth = -std::numeric_limits<double>::max();
        for (const auto& triangle : painted)
          if (triangle.polygon.containsPoint(midpoint, Qt::OddEvenFill))
            surfaceDepth = std::max(surfaceDepth, triangle.depth);
        const double segmentDepth = (a.depth + b.depth) * 0.5;
        if (!selected && !hovered &&
            segmentDepth + bodyRenderMesh_.diagonal() * 1e-4 < surfaceDepth)
          continue;
        painter.drawLine(a.screen, b.screen);
      }
    }
    if (toolManipulator_) {
      const auto start = projectBodyPoint(toolManipulator_->origin, center, size(),
                                          yaw_, pitch_, zoom_).screen;
      const Point3d endWorld{
          toolManipulator_->origin.x + toolManipulator_->direction.x * toolManipulator_->valueMm,
          toolManipulator_->origin.y + toolManipulator_->direction.y * toolManipulator_->valueMm,
          toolManipulator_->origin.z + toolManipulator_->direction.z * toolManipulator_->valueMm};
      const auto end = projectBodyPoint(endWorld, center, size(), yaw_, pitch_, zoom_).screen;
      painter.setPen(QPen(QColor("#0874f9"), 3.0));
      painter.drawLine(start, end);
      painter.setBrush(QColor("#0874f9"));
      painter.drawEllipse(end, 6.0, 6.0);
    }
  } else if (solidVisible_ && solidSketch_.lines().empty() &&
             solidSketch_.circles().empty()) {
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
      QPolygonF polygon;
      for (const int index : faces[faceIndex])
        polygon << project(vertices[index], size(), yaw_, pitch_, zoom_);
      if (!isFrontFacing(polygon)) continue;
      painter.setBrush(colors[faceIndex]);
      painter.setPen(QPen(static_cast<int>(faceIndex) == selectedFace_
                              ? QColor("#075eff")
                              : QColor("#4d5863"),
                          static_cast<int>(faceIndex) == selectedFace_ ? 3.0
                                                                       : 1.4));
      painter.drawPolygon(polygon);
    }
  }

  if (solidVisible_ && !hasParametricBody && !solidSketch_.lines().empty()) {
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

  if (solidVisible_ && !hasParametricBody)
    for (const auto& circle : solidSketch_.circles()) {
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

  // Features created on an existing end face remain separate construction
  // records, but are painted as one additive body.  In particular their base
  // starts on the old cap instead of at the global sketch plane; drawing them
  // from zero was the source of the cylinder-through-block artefact.
  if (solidVisible_ && !hasParametricBody)
    for (const auto& feature : additiveExtrusions_) {
    const Point3 normal = supportNormal(feature.supportName);
    const float baseDistance = static_cast<float>(feature.startMm);
    const float capDistance = static_cast<float>(feature.startMm + feature.lengthMm);
    for (const auto& circle : feature.geometry.circles()) {
      QPolygonF topCap;
      for (int step = 0; step < 64; ++step) {
        const float a = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const float b = 2.0F * std::numbers::pi_v<float> * (step + 1) / 64.0F;
        const auto point = [&](float angle, float distance) {
          const sketch::Point profilePoint{
              circle.center.xMm + circle.radiusMm * std::cos(angle),
              circle.center.yMm + circle.radiusMm * std::sin(angle)};
          const Point3 origin = pointOnSupport(profilePoint, feature.supportName,
                                                box_, offsetX_, offsetY_);
          return project(translated(origin, normal, distance), size(), yaw_,
                         pitch_, zoom_);
        };
        QPolygonF side{point(a, baseDistance), point(b, baseDistance),
                       point(b, capDistance), point(a, capDistance)};
        const int shade = 120 + static_cast<int>(45.0F * std::cos(a));
        if (isFrontFacing(side)) {
          painter.setBrush(QColor(shade, shade + 5, shade + 10));
          painter.setPen(QPen(QColor("#59636d"), 0.9));
          painter.drawPolygon(side);
        }
        topCap << point(a, capDistance);
      }
      if (isFrontFacing(topCap)) {
        painter.setBrush(QColor("#c5ccd3"));
        painter.setPen(QPen(QColor("#4d5863"), 1.35));
        painter.drawPolygon(topCap);
      }
      // The coincident base cap is internal to a union and must not be drawn.
    }

    if (!feature.geometry.lines().empty()) {
      QPolygonF topCap;
      for (const auto& line : feature.geometry.lines()) {
        const Point3 a = pointOnSupport(line.start, feature.supportName, box_,
                                        offsetX_, offsetY_);
        const Point3 b = pointOnSupport(line.end, feature.supportName, box_,
                                        offsetX_, offsetY_);
        QPolygonF side{
            project(translated(a, normal, baseDistance), size(), yaw_, pitch_, zoom_),
            project(translated(b, normal, baseDistance), size(), yaw_, pitch_, zoom_),
            project(translated(b, normal, capDistance), size(), yaw_, pitch_, zoom_),
            project(translated(a, normal, capDistance), size(), yaw_, pitch_, zoom_)};
        if (isFrontFacing(side)) {
          painter.setBrush(QColor("#87919b"));
          painter.setPen(QPen(QColor("#4d5863"), 1.35));
          painter.drawPolygon(side);
        }
        topCap << project(translated(a, normal, capDistance), size(), yaw_,
                          pitch_, zoom_);
      }
      if (isFrontFacing(topCap)) {
        painter.setBrush(QColor("#c2c9d0"));
        painter.setPen(QPen(QColor("#4d5863"), 1.35));
        painter.drawPolygon(topCap);
      }
    }
  }

  if (solidVisible_ && !hasParametricBody && selectedFace_ >= 0) {
    if (!solidSketch_.circles().empty()) {
      const auto& circle = solidSketch_.circles().front();
      const Point3 normal = supportNormal(solidSupportName_);
      QPolygonF bottomCap;
      QPolygonF topCap;
      for (int step = 0; step < 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const sketch::Point profilePoint{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnSupport(profilePoint, solidSupportName_, box_,
                                           offsetX_, offsetY_);
        bottomCap.prepend(project(base, size(), yaw_, pitch_, zoom_));
        topCap << project(translated(base, normal, z), size(), yaw_, pitch_, zoom_);
      }
      painter.setBrush(QColor(7, 94, 255, 45));
      painter.setPen(QPen(QColor("#075eff"), 3.0));
      if (selectedFace_ == 0 && isFrontFacing(bottomCap))
        painter.drawPolygon(bottomCap);
      else if (selectedFace_ == 1 && isFrontFacing(topCap))
        painter.drawPolygon(topCap);
      else if (selectedFace_ == 2) {
        for (int step = 0; step < 64; ++step) {
          const float a = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
          const float b = 2.0F * std::numbers::pi_v<float> * (step + 1) / 64.0F;
          const auto surfacePoint = [&](float angle, float height) {
            const sketch::Point profilePoint{
                circle.center.xMm + circle.radiusMm * std::cos(angle),
                circle.center.yMm + circle.radiusMm * std::sin(angle)};
            const Point3 base = pointOnSupport(profilePoint, solidSupportName_,
                                               box_, offsetX_, offsetY_);
            return project(translated(base, normal, height), size(), yaw_, pitch_, zoom_);
          };
          QPolygonF side{surfacePoint(a, 0.0F), surfacePoint(b, 0.0F),
                         surfacePoint(b, z), surfacePoint(a, z)};
          if (isFrontFacing(side)) painter.drawPolygon(side);
        }
      }
    } else {
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
  }

  if (solidVisible_ && selectedVertex_ >= 0 && selectedVertex_ < 8) {
    const QPointF vertex = project(vertices[static_cast<std::size_t>(selectedVertex_)],
                                   size(), yaw_, pitch_, zoom_);
    painter.setBrush(QColor("#ffffff"));
    painter.setPen(QPen(QColor("#075eff"), 2.5));
    painter.drawEllipse(vertex, 6.0, 6.0);
  }

  if (sketchVisible_ && displaySketches_.empty()) {
    painter.setBrush(Qt::NoBrush);
    for (const auto& line : sketch_.lines()) {
      painter.setPen(QPen(QColor(22, 105, 215), 2.2,
                          line.dashed ? Qt::DashLine : Qt::SolidLine));
      painter.drawLine(project(pointOnPlacement(line.start, sketchPlacement_,
                                                offsetX_, offsetY_),
                               size(), yaw_, pitch_, zoom_),
                       project(pointOnPlacement(line.end, sketchPlacement_,
                                                offsetX_, offsetY_),
                               size(), yaw_, pitch_, zoom_));
    }
    for (const auto& circle : sketch_.circles()) {
      painter.setPen(QPen(QColor(22, 105, 215), 2.2,
                          circle.dashed ? Qt::DashLine : Qt::SolidLine));
      QPolygonF curve;
      for (int step = 0; step <= 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        curve << project(pointOnPlacement(
                             {circle.center.xMm + circle.radiusMm * std::cos(angle),
                              circle.center.yMm + circle.radiusMm * std::sin(angle)},
                             sketchPlacement_, offsetX_, offsetY_),
                         size(), yaw_, pitch_, zoom_);
      }
      painter.drawPolyline(curve);
    }
  }

  if (sketchVisible_ && !displaySketches_.empty()) {
    painter.setBrush(Qt::NoBrush);
    for (const auto& displayed : displaySketches_) {
      if (!displayed.visible) continue;
      for (const auto& line : displayed.geometry.lines()) {
        painter.setPen(QPen(QColor("#1469d7"), 2.2,
                            line.dashed ? Qt::DashLine : Qt::SolidLine));
        painter.drawLine(
            project(pointOnPlacement(line.start, displayed.placement,
                                     offsetX_, offsetY_),
                    size(), yaw_, pitch_, zoom_),
            project(pointOnPlacement(line.end, displayed.placement,
                                     offsetX_, offsetY_),
                    size(), yaw_, pitch_, zoom_));
      }
      painter.setPen(QPen(QColor("#1469d7"), 2.2));
      for (const auto& circle : displayed.geometry.circles()) {
        painter.setPen(QPen(QColor("#1469d7"), 2.2,
                            circle.dashed ? Qt::DashLine : Qt::SolidLine));
        QPolygonF curve;
        for (int step = 0; step <= 64; ++step) {
          const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
          const sketch::Point point{
              circle.center.xMm + circle.radiusMm * std::cos(angle),
              circle.center.yMm + circle.radiusMm * std::sin(angle)};
          curve << project(pointOnPlacement(point, displayed.placement,
                                            offsetX_, offsetY_),
                           size(), yaw_, pitch_, zoom_);
        }
        painter.drawPolyline(curve);
      }
    }
  }

  if ((pickMode_ == PickMode::ExtrusionSurface ||
       pickMode_ == PickMode::SketchPlane) &&
      !extrusionHoverPolygon_.isEmpty()) {
    painter.setBrush(QColor(10, 105, 245, 55));
    painter.setPen(QPen(QColor("#0969e8"), 2.2));
    if (!extrusionHoverPath_.isEmpty())
      painter.drawPath(extrusionHoverPath_);
    else
      painter.drawPolygon(extrusionHoverPolygon_);
  }

  if (extrusionManipulatorVisible_) {
    const QPointF offset = extrusionScreenOffset();
    const QPointF tip = extrusionManipulatorAnchor_ + offset;
    const QColor previewColor = extrusionPreviewLengthMm_ >= 0.0
                                    ? QColor(25, 125, 245, 122)
                                    : QColor(224, 66, 76, 112);
    const QColor previewEdge = extrusionPreviewLengthMm_ >= 0.0
                                   ? QColor("#0874f9")
                                   : QColor("#d93645");
    const auto previewPolygons = selectedExtrusionPolygons_.empty()
                                     ? std::vector<QPolygonF>{selectedExtrusionPolygon_}
                                     : selectedExtrusionPolygons_;
    // The selected regions are cached in screen coordinates while the
    // manipulator anchor is reprojected from model coordinates on every
    // paint.  Opening the properties dock or changing the camera used to
    // update only the anchor, leaving the preview displaced.  Apply one
    // common correction to all regions so their collective centre remains
    // attached to the manipulator base without collapsing multi-selection.
    QRectF cachedRegionsBounds;
    for (const auto& path : selectedExtrusionPaths_)
      cachedRegionsBounds = cachedRegionsBounds.united(path.boundingRect());
    const QPointF previewAlignment = cachedRegionsBounds.isEmpty()
                                         ? QPointF{}
                                         : extrusionManipulatorAnchor_ -
                                               cachedRegionsBounds.center();
    if (!qFuzzyIsNull(extrusionPreviewLengthMm_)) {
      for (std::size_t regionIndex = 0; regionIndex < previewPolygons.size();
           ++regionIndex) {
        const auto& polygon = previewPolygons[regionIndex];
        if (polygon.size() < 3) continue;
        QPainterPath basePath;
        if (regionIndex < selectedExtrusionPaths_.size())
          basePath = selectedExtrusionPaths_[regionIndex];
        else {
          basePath.addPolygon(polygon);
          basePath.closeSubpath();
        }
        if (!previewAlignment.isNull()) {
          QTransform alignment;
          alignment.translate(previewAlignment.x(), previewAlignment.y());
          basePath = alignment.map(basePath);
        }
        QTransform translation;
        translation.translate(offset.x(), offset.y());
        const QPainterPath capPath = translation.map(basePath);
        std::vector<QPolygonF> sideFaces;
        for (const auto& boundary : basePath.toSubpathPolygons()) {
          const QPolygonF cap = boundary.translated(offset);
          for (qsizetype index = 0; index < boundary.size(); ++index) {
            const qsizetype next = (index + 1) % boundary.size();
            QPolygonF side;
            side << boundary[index] << boundary[next] << cap[next] << cap[index];
            sideFaces.push_back(side);
          }
        }
        QLinearGradient bodyGradient(basePath.boundingRect().center(),
                                     capPath.boundingRect().center());
        const QColor bodyStart = extrusionPreviewLengthMm_ >= 0.0
                                     ? QColor(80, 158, 248, 150)
                                     : QColor(235, 102, 111, 140);
        const QColor bodyEnd = extrusionPreviewLengthMm_ >= 0.0
                                   ? QColor(22, 108, 230, 105)
                                   : QColor(200, 44, 59, 100);
        bodyGradient.setColorAt(0.0, bodyStart);
        bodyGradient.setColorAt(0.55, previewColor);
        bodyGradient.setColorAt(1.0, bodyEnd);
        painter.setPen(Qt::NoPen);
        painter.setBrush(bodyGradient);
        // Draw faces independently without a pen. A single winding path can
        // cancel adjacent quads with opposite winding, which made the body
        // disappear and left only two caps connected by thin lines.
        for (const auto& side : sideFaces) painter.drawPolygon(side);

        painter.setPen(QPen(QColor(previewEdge.red(), previewEdge.green(),
                                  previewEdge.blue(), 115), 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(basePath);
        painter.setPen(QPen(previewEdge, 2.4));
        painter.setBrush(extrusionPreviewLengthMm_ >= 0.0
                             ? QColor(157, 202, 255, 150)
                             : QColor(248, 164, 170, 145));
        painter.drawPath(capPath);

        // Only silhouette generators are outlined. Avoid drawing every
        // tessellation edge of a circular contour as vertical hatching.
        if (offset.manhattanLength() > 1.0) {
          const QPointF perpendicular(-offset.y(), offset.x());
          for (const auto& boundary : basePath.toSubpathPolygons()) {
            if (boundary.isEmpty()) continue;
            auto minPoint = boundary.front();
            auto maxPoint = boundary.front();
            double minProjection = QPointF::dotProduct(minPoint, perpendicular);
            double maxProjection = minProjection;
            for (const QPointF& point : boundary) {
              const double projection = QPointF::dotProduct(point, perpendicular);
              if (projection < minProjection) {
                minProjection = projection;
                minPoint = point;
              }
              if (projection > maxProjection) {
                maxProjection = projection;
                maxPoint = point;
              }
            }
            painter.drawLine(minPoint, minPoint + offset);
            painter.drawLine(maxPoint, maxPoint + offset);
          }
        }
      }
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(previewEdge, 4.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(extrusionManipulatorAnchor_, tip);
    QLineF direction(extrusionManipulatorAnchor_, tip);
    if (direction.length() < 1.0) direction.setP2(direction.p1() + QPointF(0.0, -1.0));
    const QPointF unit = (direction.p2() - direction.p1()) / direction.length();
    const QPointF perpendicular(-unit.y(), unit.x());
    QPolygonF arrowHead;
    arrowHead << tip << tip - unit * 15.0 + perpendicular * 8.0
              << tip - unit * 15.0 - perpendicular * 8.0;
    painter.setBrush(previewEdge);
    painter.drawPolygon(arrowHead);
    painter.setBrush(QColor("#ffffff"));
    painter.setPen(QPen(previewEdge, 3.0));
    painter.drawEllipse(tip, 7.0, 7.0);
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
    painter.setBrush(selectedOrigin_ ? QColor("#d9eaff") : Qt::white);
    painter.setPen(QPen(selectedOrigin_ ? QColor("#075eff") : QColor("#1769d2"),
                        selectedOrigin_ ? 3.0 : 1.5));
    painter.drawEllipse(origin, 4.0, 4.0);
  }

  painter.restore();

  // Orientation cube: only visible faces are drawn, with their names placed
  // directly on the corresponding face.
  const QPointF cubeCenter(width() - 70.0, 66.0);
  const QSize cubeProjectionSize(92, 92);
  const float c = 28.0F;
  const std::array<Point3, 8> cubeVertices{{{-c, -c, -c}, {c, -c, -c},
                                            {c, c, -c}, {-c, c, -c},
                                            {-c, -c, c}, {c, -c, c},
                                            {c, c, c}, {-c, c, c}}};
  const std::array<std::array<int, 4>, 6> cubeFaces{{
      {{0, 1, 2, 3}}, {{4, 7, 6, 5}}, {{0, 4, 5, 1}},
      {{1, 5, 6, 2}}, {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};
  const std::array<QString, 6> cubeLabels{
      QStringLiteral(u"СНИЗУ"), QStringLiteral(u"СВЕРХУ"),
      QStringLiteral(u"СПЕРЕДИ"), QStringLiteral(u"СПРАВА"),
      QStringLiteral(u"СЗАДИ"), QStringLiteral(u"СЛЕВА")};
  QFont cubeFont = painter.font();
  cubeFont.setBold(true);
  cubeFont.setPixelSize(8);
  painter.setFont(cubeFont);
  for (std::size_t face = 0; face < cubeFaces.size(); ++face) {
    QPolygonF polygon;
    for (int vertex : cubeFaces[face]) {
      QPointF point = project(cubeVertices[vertex], cubeProjectionSize,
                              yaw_, pitch_, 1.0F);
      point += cubeCenter - QPointF(cubeProjectionSize.width() * 0.5,
                                    cubeProjectionSize.height() * 0.52);
      polygon << point;
    }
    if (!isFrontFacing(polygon)) continue;
    painter.setBrush(face == 1 ? QColor("#e5f0ff") : QColor("#f8fbff"));
    painter.setPen(QPen(QColor("#5275a8"), 1.2));
    painter.drawPolygon(polygon);
    painter.setPen(QColor("#123d84"));
    painter.drawText(polygon.boundingRect(), Qt::AlignCenter, cubeLabels[face]);
  }

  painter.setPen(QColor(171, 184, 201));
  painter.drawText(16, height() - 18, "Drag to orbit  •  Wheel to zoom");
}

void Viewport::mousePressEvent(QMouseEvent* event) {
  setFocus(Qt::MouseFocusReason);
  lastMousePosition_ = event->position().toPoint();
  draggingBody_ = false;
  bodyDragStart_ = {offsetX_, offsetY_};
  if (event->button() == Qt::LeftButton) {
    const QPointF cubeCenter(width() - 70.0, 66.0);
    const QSize cubeProjectionSize(92, 92);
    constexpr float c = 28.0F;
    const std::array<Point3, 8> cubeVertices{{
        {-c, -c, -c}, {c, -c, -c}, {c, c, -c}, {-c, c, -c},
        {-c, -c, c},  {c, -c, c},  {c, c, c},  {-c, c, c}}};
    const std::array<std::array<int, 4>, 6> cubeFaces{{
        {{0, 1, 2, 3}}, {{4, 7, 6, 5}}, {{0, 4, 5, 1}},
        {{1, 5, 6, 2}}, {{2, 6, 7, 3}}, {{3, 7, 4, 0}}}};
    for (std::size_t face = 0; face < cubeFaces.size(); ++face) {
      QPolygonF polygon;
      for (const int vertex : cubeFaces[face]) {
        QPointF point = project(cubeVertices[vertex], cubeProjectionSize,
                                yaw_, pitch_, 1.0F);
        point += cubeCenter - QPointF(cubeProjectionSize.width() * 0.5,
                                      cubeProjectionSize.height() * 0.52);
        polygon << point;
      }
      if (!isFrontFacing(polygon) ||
          !polygon.containsPoint(event->position(), Qt::OddEvenFill))
        continue;
      switch (face) {
        case 0:  // Bottom
          yaw_ = 0.0F;
          pitch_ = 180.0F;
          break;
        case 1:  // Top
          yaw_ = 0.0F;
          pitch_ = 0.0F;
          break;
        case 2:  // Front
          yaw_ = 0.0F;
          pitch_ = -90.0F;
          break;
        case 3:  // Right
          yaw_ = 90.0F;
          pitch_ = 90.0F;
          break;
        case 4:  // Back
          yaw_ = 0.0F;
          pitch_ = 90.0F;
          break;
        case 5:  // Left
          yaw_ = -90.0F;
          pitch_ = 90.0F;
          break;
      }
      update();
      event->accept();
      return;
    }
  }
  if (event->button() == Qt::MiddleButton) {
    panningView_ = true;
    setCursor(Qt::ClosedHandCursor);
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  const QPointF scenePosition = event->position() - cameraPan_;
  if (toolManipulator_) {
    const Point3d center = bodyRenderMesh_.center();
    const Point3d endWorld{
        toolManipulator_->origin.x + toolManipulator_->direction.x * toolManipulator_->valueMm,
        toolManipulator_->origin.y + toolManipulator_->direction.y * toolManipulator_->valueMm,
        toolManipulator_->origin.z + toolManipulator_->direction.z * toolManipulator_->valueMm};
    const QPointF handle = projectBodyPoint(endWorld, center, size(), yaw_, pitch_, zoom_).screen;
    if (QLineF(scenePosition, handle).length() <= 18.0) {
      draggingToolManipulator_ = true;
      setCursor(Qt::SizeAllCursor);
      return;
    }
  }
  if (extrusionManipulatorVisible_) {
    const QPointF handle = extrusionManipulatorAnchor_ + extrusionScreenOffset();
    if (QLineF(scenePosition, handle).length() <= 18.0) {
      draggingExtrusionHandle_ = true;
      setCursor(Qt::SizeVerCursor);
      return;
    }
  }
  if (pickMode_ == PickMode::ExtrusionSurface &&
      !extrusionHoverPolygon_.isEmpty()) {
    const bool append = event->modifiers().testFlag(Qt::ControlModifier);
    if (!append) {
      selectedExtrusionPolygons_.clear();
      selectedExtrusionPaths_.clear();
      selectedExtrusionRegionSketches_.clear();
    }

    QPainterPath hoveredPath = extrusionHoverPath_;
    if (hoveredPath.isEmpty()) {
      hoveredPath.addPolygon(extrusionHoverPolygon_);
      hoveredPath.closeSubpath();
    }
    const QRectF hoveredBounds = hoveredPath.boundingRect();
    const auto sameRegion = [&hoveredBounds](const QPainterPath& path) {
      const QRectF bounds = path.boundingRect();
      return QLineF(bounds.center(), hoveredBounds.center()).length() < 1.0 &&
             std::abs(bounds.width() - hoveredBounds.width()) < 1.0 &&
             std::abs(bounds.height() - hoveredBounds.height()) < 1.0;
    };
    const auto existing = std::find_if(selectedExtrusionPaths_.begin(),
                                       selectedExtrusionPaths_.end(), sameRegion);
    if (append && existing != selectedExtrusionPaths_.end()) {
      const auto index = static_cast<std::size_t>(
          std::distance(selectedExtrusionPaths_.begin(), existing));
      selectedExtrusionPaths_.erase(existing);
      if (index < selectedExtrusionPolygons_.size())
        selectedExtrusionPolygons_.erase(selectedExtrusionPolygons_.begin() +
                                         static_cast<std::ptrdiff_t>(index));
      if (index < selectedExtrusionRegionSketches_.size())
        selectedExtrusionRegionSketches_.erase(
            selectedExtrusionRegionSketches_.begin() +
            static_cast<std::ptrdiff_t>(index));
    } else {
      selectedExtrusionPaths_.push_back(hoveredPath);
      selectedExtrusionPolygons_.push_back(extrusionHoverPolygon_);
      selectedExtrusionRegionSketches_.push_back(hoveredExtrusionSketch_);
    }
    selectedExtrusionSupport_ = hoveredExtrusionSupport_;
    selectedExtrusionSketchIndex_ = hoveredExtrusionSketchIndex_;
    selectedExtrusionOnBodyCap_ = hoveredExtrusionOnBodyCap_;
    selectedExtrusionBodyFace_ = hoveredExtrusionSurface_.startsWith(
        QString::fromUtf8("Грань тела"));
    rebuildSelectedExtrusionSketch();
    if (selectedExtrusionPaths_.empty()) {
      selectedExtrusionPolygon_.clear();
      hideExtrusionManipulator();
      update();
      return;
    }
    QRectF selectedBounds;
    for (const auto& path : selectedExtrusionPaths_)
      selectedBounds = selectedBounds.united(path.boundingRect());
    selectedExtrusionPolygon_ = selectedExtrusionPolygons_.front();
    extrusionManipulatorAnchor_ = selectedBounds.center();
    if (!append) {
      pickMode_ = PickMode::None;
      unsetCursor();
    }
    emit extrusionSurfacePicked(hoveredExtrusionSurface_);
    extrusionHoverPolygon_.clear();
    update();
    return;
  }
  if (pickMode_ == PickMode::SketchPlane &&
      !extrusionHoverPolygon_.isEmpty()) {
    const QString pickedPlane = hoveredExtrusionSurface_;
    if (hoveredBodyFaceIndex_ != static_cast<std::size_t>(-1))
      selectedFace_ = static_cast<int>(hoveredBodyFaceIndex_);
    pickMode_ = PickMode::None;
    unsetCursor();
    extrusionHoverPolygon_.clear();
    extrusionHoverPath_ = {};
    hoveredExtrusionSurface_.clear();
    hoveredExtrusionSupport_.clear();
    emit sketchPlanePicked(pickedPlane);
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
    if (!solidSketch_.circles().empty()) {
      const auto& circle = solidSketch_.circles().front();
      const Point3 normal = supportNormal(solidSupportName_);
      QPolygonF bottomCap;
      QPolygonF topCap;
      for (int step = 0; step < 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const sketch::Point profilePoint{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnSupport(profilePoint, solidSupportName_, box_,
                                           offsetX_, offsetY_);
        bottomCap.prepend(project(base, size(), yaw_, pitch_, zoom_));
        topCap << project(translated(base, normal, z), size(), yaw_, pitch_, zoom_);
      }
      const std::array<QPolygonF, 2> caps{bottomCap, topCap};
      std::array<QString, 2> names;
      if (solidSupportName_.contains("XZ") ||
          solidSupportName_.contains(QString::fromUtf8("Передняя")) ||
          solidSupportName_.contains(QString::fromUtf8("Задняя"))) {
        names = {QString::fromUtf8("Передняя"), QString::fromUtf8("Задняя")};
      } else if (solidSupportName_.contains("YZ") ||
                 solidSupportName_.contains(QString::fromUtf8("Правая")) ||
                 solidSupportName_.contains(QString::fromUtf8("Левая"))) {
        names = {QString::fromUtf8("Левая"), QString::fromUtf8("Правая")};
      } else {
        names = {QString::fromUtf8("Нижняя"), QString::fromUtf8("Верхняя")};
      }
      for (int cap = 1; cap >= 0; --cap) {
        if (!isFrontFacing(caps[cap]) ||
            !caps[cap].containsPoint(scenePosition, Qt::OddEvenFill))
          continue;
        selectedFace_ = cap;
        pickMode_ = PickMode::None;
        unsetCursor();
        emit sketchPlanePicked(QString::fromUtf8("Грань тела: ") + names[cap]);
        update();
        return;
      }
    } else {
      for (int faceIndex = 5; faceIndex >= 0; --faceIndex) {
        QPolygonF polygon;
        for (int vertex : faces[faceIndex])
          polygon << project(vertices[vertex], size(), yaw_, pitch_, zoom_);
        if (!isFrontFacing(polygon)) continue;
        if (!polygon.containsPoint(scenePosition, Qt::OddEvenFill)) continue;
        selectedFace_ = faceIndex;
        pickMode_ = PickMode::None;
        unsetCursor();
        emit sketchPlanePicked(QString::fromUtf8("Грань тела: ") + selectedFaceName());
        update();
        return;
      }
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
      if (polygon.containsPoint(scenePosition, Qt::OddEvenFill)) {
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
      if (!line.dashed)
      contour << project({static_cast<float>(line.start.xMm) + offsetX_,
                          static_cast<float>(line.start.yMm) + offsetY_, 0},
                         size(), yaw_, pitch_, zoom_);
    if (contour.size() >= 3 &&
        contour.containsPoint(scenePosition, Qt::OddEvenFill)) {
      pickMode_ = PickMode::None;
      unsetCursor();
      emit extrusionSurfacePicked(QString::fromUtf8("Замкнутый контур эскиза"));
      update();
      return;
    }
  }
  if (pickMode_ == PickMode::ExtrusionSurface) {
    for (const auto& circle : sketch_.circles()) {
      if (circle.dashed) continue;
      QPolygonF contour;
      for (int step = 0; step < 48; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 48.0F;
        contour << project(
            {static_cast<float>(circle.center.xMm + circle.radiusMm * std::cos(angle)) + offsetX_,
             static_cast<float>(circle.center.yMm + circle.radiusMm * std::sin(angle)) + offsetY_, 0},
            size(), yaw_, pitch_, zoom_);
      }
      if (!contour.containsPoint(scenePosition, Qt::OddEvenFill)) continue;
      pickMode_ = PickMode::None;
      unsetCursor();
      emit extrusionSurfacePicked(QString::fromUtf8("Замкнутый контур эскиза"));
      update();
      return;
    }
  }

  if (pickMode_ != PickMode::None) return;

  selectedFace_ = -1;
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  const bool appendEdge = event->modifiers().testFlag(Qt::ControlModifier);
  if (!appendEdge) {
    selectedBodyEdgeIndex_ = static_cast<std::size_t>(-1);
    selectedBodyEdgeIndices_.clear();
  }
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  selectedBasePlane_ = -1;
  selectedVertex_ = -1;
  selectedOrigin_ = false;

  if (bodyShape_ && !bodyShape_->IsNull() && solidVisible_) {
    updateBodyHover(scenePosition);
    if (hoveredBodyEdgeIndex_ != static_cast<std::size_t>(-1)) {
      const auto clicked = edgeReferenceForGlobalIndex(hoveredBodyEdgeIndex_);
      if (appendEdge) {
        if (!selectedBodyEdgeIndices_.empty()) {
          const auto first = edgeReferenceForGlobalIndex(selectedBodyEdgeIndices_.front());
          if (first && clicked &&
              (first->bodyId != clicked->bodyId ||
               first->featureId != clicked->featureId))
            selectedBodyEdgeIndices_.clear();
        }
        const auto found = std::find(selectedBodyEdgeIndices_.begin(),
                                     selectedBodyEdgeIndices_.end(),
                                     hoveredBodyEdgeIndex_);
        if (found == selectedBodyEdgeIndices_.end())
          selectedBodyEdgeIndices_.push_back(hoveredBodyEdgeIndex_);
        else
          selectedBodyEdgeIndices_.erase(found);
      } else {
        selectedBodyEdgeIndices_ = {hoveredBodyEdgeIndex_};
      }
      selectedBodyEdgeIndex_ = selectedBodyEdgeIndices_.empty()
                                   ? static_cast<std::size_t>(-1)
                                   : selectedBodyEdgeIndices_.front();
      emit selectionChanged(QString::fromUtf8("Тело 1 • Ребро ") +
                            QString::number(hoveredBodyEdgeIndex_ + 1));
      emit bodyEdgeSelectionChanged();
      update();
      return;
    }
    if (hoveredBodyFaceIndex_ != static_cast<std::size_t>(-1)) {
      selectedFace_ = static_cast<int>(hoveredBodyFaceIndex_);
      emit selectionChanged(QString::fromUtf8("Тело 1 • Грань ") +
                            QString::number(selectedFace_ + 1));
      update();
      return;
    }
    emit selectionChanged({});
    update();
    return;
  }

  // Points have the highest picking priority because they occupy the
  // smallest area on screen.
  const QPointF originPoint = project({0, 0, 0}, size(), yaw_, pitch_, zoom_);
  if (originVisible_ &&
      QLineF(originPoint, scenePosition).length() <= 9.0) {
    selectedOrigin_ = true;
    emit selectionChanged(QString::fromUtf8("Точка: начало координат"));
    update();
    return;
  }

  if (solidVisible_ && solidSketch_.circles().empty()) {
    for (int vertex = 0; vertex < static_cast<int>(vertices.size()); ++vertex) {
      const QPointF projected = project(vertices[static_cast<std::size_t>(vertex)],
                                        size(), yaw_, pitch_, zoom_);
      if (QLineF(projected, scenePosition).length() > 9.0) continue;
      selectedVertex_ = vertex;
      emit selectionChanged(QString::fromUtf8("Тело 1 • Точка ") +
                            QString::number(vertex + 1));
      update();
      return;
    }
  }

  // Faces are checked before construction planes so that a visible solid
  // remains easy to select where both objects overlap.
  selectedFace_ = -1;
  if (solidVisible_ && !solidSketch_.circles().empty()) {
    const auto& circle = solidSketch_.circles().front();
    const Point3 normal = supportNormal(solidSupportName_);
    QPolygonF bottomCap;
    QPolygonF topCap;
    for (int step = 0; step < 64; ++step) {
      const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
      const sketch::Point profilePoint{
          circle.center.xMm + circle.radiusMm * std::cos(angle),
          circle.center.yMm + circle.radiusMm * std::sin(angle)};
      const Point3 base = pointOnSupport(profilePoint, solidSupportName_, box_,
                                         offsetX_, offsetY_);
      bottomCap.prepend(project(base, size(), yaw_, pitch_, zoom_));
      topCap << project(translated(base, normal, z), size(), yaw_, pitch_, zoom_);
    }
    const std::array<QPolygonF, 2> caps{bottomCap, topCap};
    for (int cap = 1; cap >= 0; --cap) {
      if (!isFrontFacing(caps[cap]) ||
          !caps[cap].containsPoint(scenePosition, Qt::OddEvenFill))
        continue;
      selectedFace_ = cap;
      draggingBody_ = true;
      emit selectionChanged(QString::fromUtf8("Тело 1 • Круглый торец"));
      update();
      return;
    }
    for (int step = 63; step >= 0; --step) {
      const float a = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
      const float b = 2.0F * std::numbers::pi_v<float> * (step + 1) / 64.0F;
      const auto surfacePoint = [&](float angle, float height) {
        const sketch::Point profilePoint{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnSupport(profilePoint, solidSupportName_, box_,
                                           offsetX_, offsetY_);
        return project(translated(base, normal, height), size(), yaw_, pitch_, zoom_);
      };
      QPolygonF side{surfacePoint(a, 0.0F), surfacePoint(b, 0.0F),
                     surfacePoint(b, z), surfacePoint(a, z)};
      if (!isFrontFacing(side) ||
          !side.containsPoint(scenePosition, Qt::OddEvenFill))
        continue;
      selectedFace_ = 2;
      draggingBody_ = true;
      emit selectionChanged(QString::fromUtf8("Тело 1 • Боковая грань"));
      update();
      return;
    }
  } else if (solidVisible_) {
    for (int faceIndex = 5; faceIndex >= 0; --faceIndex) {
      QPolygonF polygon;
      for (int vertex : faces[faceIndex])
        polygon << project(vertices[vertex], size(), yaw_, pitch_, zoom_);
      if (!isFrontFacing(polygon)) continue;
      if (polygon.containsPoint(scenePosition, Qt::OddEvenFill)) {
        selectedFace_ = faceIndex;
        draggingBody_ = true;
        emit selectionChanged(QString::fromUtf8("Тело 1 • Грань: ") +
                              selectedFaceName());
        update();
        return;
      }
    }
  }

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
    if (!polygon.containsPoint(scenePosition, Qt::OddEvenFill)) continue;
    selectedBasePlane_ = plane;
    const QString name = plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ";
    emit selectionChanged(QString::fromUtf8("Базовая плоскость ") + name);
    update();
    return;
  }

  emit selectionChanged(QString{});
  update();
}

void Viewport::rebuildSelectedExtrusionSketch() {
  selectedExtrusionSketch_.clear();
  if (!selectedExtrusionRegionSketches_.empty()) {
    for (const auto& region : selectedExtrusionRegionSketches_) {
      for (const auto& line : region.lines())
        selectedExtrusionSketch_.addLine(line.start, line.end);
      for (const auto& circle : region.circles())
        selectedExtrusionSketch_.addCircle(circle.center, circle.radiusMm);
    }
    return;
  }
  QString support = selectedExtrusionOnBodyCap_ ? solidSupportName_
                                                : selectedExtrusionSupport_;
  Point3 p0 = pointOnSupport({0, 0}, support, box_, offsetX_, offsetY_);
  Point3 pU = pointOnSupport({1, 0}, support, box_, offsetX_, offsetY_);
  Point3 pV = pointOnSupport({0, 1}, support, box_, offsetX_, offsetY_);
  if (selectedExtrusionOnBodyCap_) {
    const Point3 normal = supportNormal(solidSupportName_);
    const float distance = static_cast<float>(box_.heightMm);
    p0 = translated(p0, normal, distance);
    pU = translated(pU, normal, distance);
    pV = translated(pV, normal, distance);
  }
  const QPointF origin = project(p0, size(), yaw_, pitch_, zoom_);
  const QPointF u = project(pU, size(), yaw_, pitch_, zoom_) - origin;
  const QPointF v = project(pV, size(), yaw_, pitch_, zoom_) - origin;
  const double determinant = u.x() * v.y() - u.y() * v.x();
  if (std::abs(determinant) < 1e-9) return;

  for (const auto& path : selectedExtrusionPaths_) {
    for (const auto& boundary : path.toSubpathPolygons()) {
      std::vector<sketch::Point> points;
      points.reserve(static_cast<std::size_t>(boundary.size()));
      for (const QPointF& screenPoint : boundary) {
        const QPointF delta = screenPoint - origin;
        points.push_back({
            (delta.x() * v.y() - delta.y() * v.x()) / determinant,
            (u.x() * delta.y() - u.y() * delta.x()) / determinant});
      }
      if (points.size() > 1 &&
          QLineF(boundary.front(), boundary.back()).length() < 0.01)
        points.pop_back();
      for (std::size_t index = 0; index < points.size(); ++index)
        selectedExtrusionSketch_.addLine(
            points[index], points[(index + 1) % points.size()]);
    }
  }
}

void Viewport::updateBodyHover(QPointF position) {
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
  hoveredBodyEdgeIndex_ = static_cast<std::size_t>(-1);
  if (!solidVisible_ || bodyRenderMesh_.triangles().empty()) return;
  const Point3d center = bodyRenderMesh_.center();
  double nearestDepth = -std::numeric_limits<double>::max();
  for (const auto& triangle : bodyRenderMesh_.triangles()) {
    const auto a = projectBodyPoint(triangle.a, center, size(), yaw_, pitch_, zoom_);
    const auto b = projectBodyPoint(triangle.b, center, size(), yaw_, pitch_, zoom_);
    const auto c = projectBodyPoint(triangle.c, center, size(), yaw_, pitch_, zoom_);
    const QPolygonF polygon{a.screen, b.screen, c.screen};
    if (!polygon.containsPoint(position, Qt::OddEvenFill)) continue;
    const double depth = (a.depth + b.depth + c.depth) / 3.0;
    if (depth > nearestDepth) {
      nearestDepth = depth;
      hoveredBodyFaceIndex_ = triangle.faceIndex;
    }
  }

  double edgeDistance = 7.0;
  for (const auto& edge : bodyRenderMesh_.edges()) {
    for (std::size_t index = 1; index < edge.points.size(); ++index) {
      const auto a = projectBodyPoint(edge.points[index - 1], center, size(),
                                      yaw_, pitch_, zoom_);
      const auto b = projectBodyPoint(edge.points[index], center, size(),
                                      yaw_, pitch_, zoom_);
      const double distance = pointSegmentDistance(position, a.screen, b.screen);
      const double depth = (a.depth + b.depth) * 0.5;
      // Suppress segments clearly behind the surface under the cursor. This is
      // a lightweight visible-edge approximation, not a full HLR pass.
      if (distance <= edgeDistance && depth + bodyRenderMesh_.diagonal() * 1e-4 >= nearestDepth) {
        edgeDistance = distance;
        hoveredBodyEdgeIndex_ = edge.edgeIndex;
      }
    }
  }
  if (hoveredBodyEdgeIndex_ != static_cast<std::size_t>(-1))
    hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);
}

void Viewport::updateSketchPlaneHover(QPointF position) {
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  hoveredExtrusionSurface_.clear();
  hoveredExtrusionSupport_.clear();
  hoveredBodyFaceIndex_ = static_cast<std::size_t>(-1);

  const float x = static_cast<float>(box_.widthMm) * 0.5F;
  const float y = static_cast<float>(box_.depthMm) * 0.5F;
  const float z = static_cast<float>(box_.heightMm);

  // Planar faces of the current solid have priority over construction planes.
  if (solidVisible_ && bodyShape_ && !bodyShape_->IsNull()) {
    const Point3d center = bodyRenderMesh_.center();
    double depth = -std::numeric_limits<double>::max();
    for (const auto& triangle : bodyRenderMesh_.triangles()) {
      const auto a = projectBodyPoint(triangle.a, center, size(), yaw_, pitch_, zoom_);
      const auto b = projectBodyPoint(triangle.b, center, size(), yaw_, pitch_, zoom_);
      const auto c = projectBodyPoint(triangle.c, center, size(), yaw_, pitch_, zoom_);
      const QPolygonF polygon{a.screen, b.screen, c.screen};
      const double candidateDepth = (a.depth + b.depth + c.depth) / 3.0;
      if (!polygon.containsPoint(position, Qt::OddEvenFill) || candidateDepth <= depth)
        continue;
      depth = candidateDepth;
      hoveredBodyFaceIndex_ = triangle.faceIndex;
    }
    if (hoveredBodyFaceIndex_ != static_cast<std::size_t>(-1)) {
      QPainterPath facePath;
      for (const auto& triangle : bodyRenderMesh_.triangles()) {
        if (triangle.faceIndex != hoveredBodyFaceIndex_) continue;
        QPolygonF polygon;
        for (const Point3d point : {triangle.a, triangle.b, triangle.c})
          polygon << projectBodyPoint(point, center, size(), yaw_, pitch_, zoom_).screen;
        facePath.addPolygon(polygon);
      }
      extrusionHoverPath_ = facePath.simplified();
      const auto polygons = extrusionHoverPath_.toFillPolygons();
      if (!polygons.empty()) extrusionHoverPolygon_ = polygons.front();
      hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела #%1").arg(
          hoveredBodyFaceIndex_ + 1);
      hoveredExtrusionSupport_ = hoveredExtrusionSurface_;
      return;
    }
  } else if (solidVisible_) {
    if (!solidSketch_.circles().empty()) {
      const auto& circle = solidSketch_.circles().front();
      const Point3 normal = supportNormal(solidSupportName_);
      std::array<QPolygonF, 2> caps;
      for (int step = 0; step < 96; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 96.0F;
        const sketch::Point profilePoint{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnSupport(profilePoint, solidSupportName_, box_,
                                           offsetX_, offsetY_);
        caps[0].prepend(project(base, size(), yaw_, pitch_, zoom_));
        caps[1] << project(translated(base, normal, z), size(), yaw_, pitch_, zoom_);
      }
      std::array<QString, 2> names;
      if (solidSupportName_.contains("XZ") ||
          solidSupportName_.contains(QString::fromUtf8("Передняя")) ||
          solidSupportName_.contains(QString::fromUtf8("Задняя")))
        names = {QString::fromUtf8("Передняя"), QString::fromUtf8("Задняя")};
      else if (solidSupportName_.contains("YZ") ||
               solidSupportName_.contains(QString::fromUtf8("Правая")) ||
               solidSupportName_.contains(QString::fromUtf8("Левая")))
        names = {QString::fromUtf8("Левая"), QString::fromUtf8("Правая")};
      else
        names = {QString::fromUtf8("Нижняя"), QString::fromUtf8("Верхняя")};
      for (int cap = 1; cap >= 0; --cap) {
        if (!isFrontFacing(caps[cap]) ||
            !caps[cap].containsPoint(position, Qt::OddEvenFill))
          continue;
        extrusionHoverPolygon_ = caps[cap];
        hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела: ") + names[cap];
        hoveredExtrusionSupport_ = names[cap];
        return;
      }
    } else {
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
        const QString name = QString::fromUtf8(names[face]);
        hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела: ") + name;
        hoveredExtrusionSupport_ = name;
        return;
      }
    }
  }

  // Construction-plane candidates are deliberately handled as a collection;
  // auxiliary user planes can be appended here without changing picking UI.
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
    if (!polygon.containsPoint(position, Qt::OddEvenFill)) continue;
    const QString name = plane == 0 ? QStringLiteral("XY")
                                    : plane == 1 ? QStringLiteral("XZ")
                                                 : QStringLiteral("YZ");
    extrusionHoverPolygon_ = polygon;
    hoveredExtrusionSurface_ = QString::fromUtf8("Базовая плоскость ") + name;
    hoveredExtrusionSupport_ = name;
    return;
  }
}

void Viewport::updateExtrusionHover(QPointF position) {
  extrusionHoverPolygon_.clear();
  extrusionHoverPath_ = {};
  hoveredExtrusionSketch_.clear();
  hoveredExtrusionSupport_.clear();
  hoveredExtrusionSurface_.clear();
  hoveredExtrusionOnBodyCap_ = false;
  hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);

  // Build atomic selectable regions from every closed contour visible on the
  // working face.  For example, a rectangle crossing a circular cap produces
  // the rectangle intersection and the individual circular segments instead
  // of treating the whole cap as a single surface.
  std::vector<QPolygonF> contours;
  QString regionSupport;
  bool regionOnBodyCap = false;
  std::size_t regionSketchIndex = static_cast<std::size_t>(-1);
  for (std::size_t displayedIndex = 0; displayedIndex < displaySketches_.size();
       ++displayedIndex) {
    const auto& displayed = displaySketches_[displayedIndex];
    if (!displayed.visible) continue;
    const auto projectContourPoint = [&](sketch::Point point) {
      return project(pointOnPlacement(point, displayed.placement,
                                      offsetX_, offsetY_),
                     size(), yaw_, pitch_, zoom_);
    };
    std::vector<std::size_t> ids;
    for (const auto& line : displayed.geometry.lines())
      if (!line.dashed &&
          std::find(ids.begin(), ids.end(), line.elementId) == ids.end())
        ids.push_back(line.elementId);
    for (const auto id : ids) {
      QPolygonF polygon;
      for (const auto& line : displayed.geometry.lines())
        if (!line.dashed && line.elementId == id)
          polygon << projectContourPoint(line.start);
      if (polygon.size() >= 3) {
        contours.push_back(polygon);
        if (polygon.containsPoint(position, Qt::OddEvenFill)) {
          regionSupport = displayed.supportName;
          regionSketchIndex = displayedIndex;
        }
      }
    }
    for (const auto& circle : displayed.geometry.circles()) {
      if (circle.dashed) continue;
      QPolygonF polygon;
      for (int step = 0; step < 96; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 96.0F;
        const sketch::Point point{circle.center.xMm + circle.radiusMm * std::cos(angle),
                                  circle.center.yMm + circle.radiusMm * std::sin(angle)};
        polygon << projectContourPoint(point);
      }
      contours.push_back(polygon);
      if (polygon.containsPoint(position, Qt::OddEvenFill)) {
          regionSupport = displayed.supportName;
        regionSketchIndex = displayedIndex;
      }
    }
  }

  // The planar cap of an existing body is another contour on which sketches
  // can split extrusion regions.
  if (solidVisible_ && !solidSketch_.circles().empty()) {
    QPolygonF cap;
    const auto& circle = solidSketch_.circles().front();
    const Point3 normal = supportNormal(solidSupportName_);
    for (int step = 0; step < 96; ++step) {
      const float angle = 2.0F * std::numbers::pi_v<float> * step / 96.0F;
      const sketch::Point point{circle.center.xMm + circle.radiusMm * std::cos(angle),
                                circle.center.yMm + circle.radiusMm * std::sin(angle)};
      const Point3 base = pointOnSupport(point, solidSupportName_, box_,
                                         offsetX_, offsetY_);
      cap << project(translated(base, normal, static_cast<float>(box_.heightMm)),
                     size(), yaw_, pitch_, zoom_);
    }
    contours.push_back(cap);
    if (regionSupport.isEmpty() && cap.containsPoint(position, Qt::OddEvenFill))
      regionSupport = solidSupportName_;
    if (cap.containsPoint(position, Qt::OddEvenFill)) regionOnBodyCap = true;
  }

  if (contours.size() >= 2) {
    QPainterPath region;
    bool initialized = false;
    for (const auto& contour : contours) {
      QPainterPath path;
      path.addPolygon(contour);
      path.closeSubpath();
      const bool inside = path.contains(position);
      if (inside) {
        region = initialized ? region.intersected(path) : path;
        initialized = true;
      }
    }
    if (initialized) {
      for (const auto& contour : contours) {
        QPainterPath path;
        path.addPolygon(contour);
        path.closeSubpath();
        if (!path.contains(position)) region = region.subtracted(path);
      }
      const auto pieces = region.toSubpathPolygons();
      int selectedPiece = -1;
      double selectedArea = std::numeric_limits<double>::max();
      for (int index = 0; index < pieces.size(); ++index) {
        const auto& piece = pieces[index];
        if (piece.size() < 3 ||
            !piece.containsPoint(position, Qt::OddEvenFill))
          continue;
        const double area = std::abs(signedArea(piece));
        if (area < selectedArea) {
          selectedArea = area;
          selectedPiece = index;
        }
      }
      if (selectedPiece >= 0) {
        const QPolygonF& piece = pieces[selectedPiece];
        QPainterPath selectedRegion;
        selectedRegion.setFillRule(Qt::OddEvenFill);
        selectedRegion.addPolygon(piece);
        selectedRegion.closeSubpath();
        // Preserve nested boundaries as holes. Thus clicking the outer circle
        // selects an annulus, while clicking inside the inner circle selects
        // only the disk. Ctrl can then combine both explicitly.
        for (int index = 0; index < pieces.size(); ++index) {
          if (index == selectedPiece || pieces[index].size() < 3) continue;
          if (!piece.containsPoint(pieces[index].boundingRect().center(),
                                   Qt::OddEvenFill))
            continue;
          selectedRegion.addPolygon(pieces[index]);
          selectedRegion.closeSubpath();
        }
        extrusionHoverPolygon_ = piece;
        extrusionHoverPath_ = selectedRegion;
        hoveredExtrusionOnBodyCap_ = regionOnBodyCap;
        hoveredExtrusionSupport_ = regionSupport.isEmpty() ? solidSupportName_
                                                           : regionSupport;
        hoveredExtrusionSurface_ = QString::fromUtf8("Замкнутая область");
        hoveredExtrusionSketchIndex_ = regionSketchIndex;

        // Convert the selected screen-space boundary back to sketch-plane
        // coordinates so the extrusion preview follows this exact region.
        Point3 modelOrigin = pointOnSupport(
            {0, 0}, hoveredExtrusionSupport_, box_, offsetX_, offsetY_);
        Point3 modelU = pointOnSupport(
            {1, 0}, hoveredExtrusionSupport_, box_, offsetX_, offsetY_);
        Point3 modelV = pointOnSupport(
            {0, 1}, hoveredExtrusionSupport_, box_, offsetX_, offsetY_);
        if (hoveredExtrusionOnBodyCap_) {
          const Point3 capNormal = supportNormal(solidSupportName_);
          const float capDistance = static_cast<float>(box_.heightMm);
          modelOrigin = translated(modelOrigin, capNormal, capDistance);
          modelU = translated(modelU, capNormal, capDistance);
          modelV = translated(modelV, capNormal, capDistance);
        }
        const QPointF origin = project(modelOrigin, size(), yaw_, pitch_, zoom_);
        const QPointF uPoint = project(modelU, size(), yaw_, pitch_, zoom_);
        const QPointF vPoint = project(modelV, size(), yaw_, pitch_, zoom_);
        const QPointF u = uPoint - origin;
        const QPointF v = vPoint - origin;
        const double determinant = u.x() * v.y() - u.y() * v.x();
        if (std::abs(determinant) > 1e-9) {
          for (const auto& boundary : selectedRegion.toSubpathPolygons()) {
            std::vector<sketch::Point> modelPoints;
            modelPoints.reserve(static_cast<std::size_t>(boundary.size()));
            for (const QPointF& screenPoint : boundary) {
              const QPointF delta = screenPoint - origin;
              modelPoints.push_back({
                  (delta.x() * v.y() - delta.y() * v.x()) / determinant,
                  (u.x() * delta.y() - u.y() * delta.x()) / determinant});
            }
            if (modelPoints.size() > 1 &&
                QLineF(boundary.front(), boundary.back()).length() < 0.01)
              modelPoints.pop_back();
            for (std::size_t index = 0; index < modelPoints.size(); ++index)
              hoveredExtrusionSketch_.addLine(
                  modelPoints[index],
                  modelPoints[(index + 1) % modelPoints.size()]);
          }
        }
        return;
      }
    }
  }

  std::size_t reverseIndex = displaySketches_.size();
  for (auto displayed = displaySketches_.rbegin();
       displayed != displaySketches_.rend() && extrusionHoverPolygon_.isEmpty();
       ++displayed) {
    const std::size_t displayedIndex = --reverseIndex;
    if (!displayed->visible) continue;
    std::vector<std::size_t> ids;
    for (const auto& line : displayed->geometry.lines())
      if (!line.dashed &&
          std::find(ids.begin(), ids.end(), line.elementId) == ids.end())
        ids.push_back(line.elementId);
    for (const std::size_t id : ids) {
      QPolygonF polygon;
      sketch::Sketch candidate;
      for (const auto& line : displayed->geometry.lines()) {
        if (line.dashed || line.elementId != id) continue;
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
      if (circle.dashed) continue;
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
  // A finished body's planar end caps are valid extrusion sources too.  Keep
  // the picked polygon in screen space so the manipulator starts exactly on
  // the visible face; the body's existing profile is reused on commit.
  if (!solidSketch_.lines().empty() || !solidSketch_.circles().empty()) {
    QPolygonF bottomCap;
    QPolygonF topCap;
    const Point3 normal = supportNormal(solidSupportName_);
    const float bodyLength = static_cast<float>(box_.heightMm);
    if (!solidSketch_.circles().empty()) {
      const auto& circle = solidSketch_.circles().front();
      for (int step = 0; step < 64; ++step) {
        const float angle = 2.0F * std::numbers::pi_v<float> * step / 64.0F;
        const sketch::Point point{
            circle.center.xMm + circle.radiusMm * std::cos(angle),
            circle.center.yMm + circle.radiusMm * std::sin(angle)};
        const Point3 base = pointOnSupport(point, solidSupportName_, box_,
                                           offsetX_, offsetY_);
        bottomCap.prepend(project(base, size(), yaw_, pitch_, zoom_));
        topCap << project(translated(base, normal, bodyLength), size(), yaw_,
                          pitch_, zoom_);
      }
    } else {
      for (const auto& line : solidSketch_.lines()) {
        const Point3 base = pointOnSupport(line.start, solidSupportName_, box_,
                                           offsetX_, offsetY_);
        bottomCap.prepend(project(base, size(), yaw_, pitch_, zoom_));
        topCap << project(translated(base, normal, bodyLength), size(), yaw_,
                          pitch_, zoom_);
      }
    }
    const std::array<QPolygonF, 2> caps{bottomCap, topCap};
    for (int cap = 1; cap >= 0; --cap) {
      if (caps[cap].size() < 3 || !isFrontFacing(caps[cap]) ||
          !caps[cap].containsPoint(position, Qt::OddEvenFill))
        continue;
      extrusionHoverPolygon_ = caps[cap];
      hoveredExtrusionSurface_ = QString::fromUtf8("Грань тела: ") +
                                 (cap == 1 ? QString::fromUtf8("Торцевая")
                                           : QString::fromUtf8("Начальная"));
      hoveredExtrusionSupport_ = solidSupportName_;
      if (cap == 0) hoveredExtrusionSupport_ += QStringLiteral("|NEG");
      hoveredExtrusionSketchIndex_ = static_cast<std::size_t>(-1);
      return;
    }
  }

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
    hoveredExtrusionSupport_ = QString::fromUtf8(names[face]);
    break;
  }
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
  if (draggingToolManipulator_ && toolManipulator_ &&
      event->buttons().testFlag(Qt::LeftButton)) {
    const Point3d center = bodyRenderMesh_.center();
    const auto origin = projectBodyPoint(toolManipulator_->origin, center, size(),
                                         yaw_, pitch_, zoom_).screen;
    const Point3d unitWorld{
        toolManipulator_->origin.x + toolManipulator_->direction.x,
        toolManipulator_->origin.y + toolManipulator_->direction.y,
        toolManipulator_->origin.z + toolManipulator_->direction.z};
    const auto unit = projectBodyPoint(unitWorld, center, size(), yaw_, pitch_,
                                       zoom_).screen;
    const QPointF axis = unit - origin;
    const double axisLengthSquared = QPointF::dotProduct(axis, axis);
    if (axisLengthSquared > 1e-6) {
      const double value = QPointF::dotProduct(
                               event->position() - cameraPan_ - origin, axis) /
                           axisLengthSquared;
      toolManipulator_->valueMm = std::max(0.01, value);
      emit toolManipulatorValueChanged(toolManipulator_->valueMm);
      update();
    }
    return;
  }
  if (panningView_ && event->buttons().testFlag(Qt::MiddleButton)) {
    const QPoint delta = event->position().toPoint() - lastMousePosition_;
    cameraPan_ += QPointF(delta);
    lastMousePosition_ = event->position().toPoint();
    if (extrusionManipulatorVisible_)
      setExtrusionPreviewLength(extrusionPreviewLengthMm_);
    update();
    return;
  }
  if (draggingExtrusionHandle_ && event->buttons().testFlag(Qt::LeftButton)) {
    const QString support = selectedExtrusionSupport_.isEmpty()
                                ? solidSupportName_
                                : selectedExtrusionSupport_;
    const Point3 normal = supportNormal(support);
    const QPointF origin = project({0.0F, 0.0F, 0.0F}, size(), yaw_, pitch_, zoom_);
    QPointF axis = project(translated({0.0F, 0.0F, 0.0F}, normal, 1.0F),
                           size(), yaw_, pitch_, zoom_) - origin;
    const double axisLengthSquared = QPointF::dotProduct(axis, axis);
    if (axisLengthSquared > 0.0001) {
      const QPointF mouseOffset = event->position() - cameraPan_ -
                                  extrusionManipulatorAnchor_;
      setExtrusionPreviewLength(
          QPointF::dotProduct(mouseOffset, axis) / axisLengthSquared);
    }
    return;
  }
  if (pickMode_ == PickMode::ExtrusionSurface) {
    updateExtrusionHover(event->position() - cameraPan_);
    update();
    return;
  }
  if (pickMode_ == PickMode::SketchPlane) {
    updateSketchPlaneHover(event->position() - cameraPan_);
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
  } else if (event->buttons() == Qt::NoButton) {
    updateBodyHover(event->position() - cameraPan_);
    update();
  }
}

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::MiddleButton && panningView_) {
    panningView_ = false;
    unsetCursor();
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && draggingExtrusionHandle_) {
    draggingExtrusionHandle_ = false;
    unsetCursor();
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && draggingToolManipulator_) {
    draggingToolManipulator_ = false;
    unsetCursor();
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && draggingBody_) {
    draggingBody_ = false;
    const QPointF current(offsetX_, offsetY_);
    if (QLineF(bodyDragStart_, current).length() > 0.0001)
      emit bodyMoveCommitted(bodyDragStart_, current);
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void Viewport::wheelEvent(QWheelEvent* event) {
  zoom_ = std::clamp(zoom_ * (event->angleDelta().y() > 0 ? 1.1F : 0.9F),
                     0.25F, 5.0F);
  update();
}

void Viewport::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Control && extrusionManipulatorVisible_) {
    hideExtrusionManipulator();
    pickMode_ = PickMode::ExtrusionSurface;
    extrusionHoverPolygon_.clear();
    extrusionHoverPath_ = {};
    setCursor(Qt::CrossCursor);
    update();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Escape) {
    pickMode_ = PickMode::None;
    hideExtrusionManipulator();
    extrusionHoverPolygon_.clear();
    extrusionHoverPath_ = {};
    selectedExtrusionPolygons_.clear();
    selectedExtrusionPaths_.clear();
    selectedExtrusionRegionSketches_.clear();
    selectedExtrusionSketch_.clear();
    unsetCursor();
    emit selectionChanged(QStringLiteral("__cancel_tools__"));
    update();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

}  // namespace solidar
