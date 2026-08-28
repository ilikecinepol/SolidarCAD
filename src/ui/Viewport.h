#pragma once

#include <QPoint>
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
#include <QWidget>
#include <vector>
#include <optional>

#include "model/Document.h"
#include "model/SolidFeature.h"
#include "sketch/Sketch.h"
#include "ui/BodyRenderMesh.h"
#include "model/ToolSession.h"

class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QKeyEvent;
class QDoubleSpinBox;

namespace solidar {

struct BodyViewShape {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  ShapeFeature::ShapePtr shape;
};

class Viewport final : public QWidget {
  Q_OBJECT

 public:
  explicit Viewport(QWidget* parent = nullptr);
  void setBox(BoxParameters parameters);
  void setBodyShape(ShapeFeature::ShapePtr shape,
                    BodyId bodyId = kInvalidBodyId,
                    FeatureId featureId = kInvalidFeatureId);
  void setBodyShapes(std::vector<BodyViewShape> shapes);
  void setSketch(const sketch::Sketch& sketch);
  void setSketch(const sketch::Sketch& sketch,
                 const SketchPlacement& placement);
  void setSolidSketch(const sketch::Sketch& sketch);
  void setSolidVisible(bool visible);
  void setSolidSupport(const QString& supportName);
  void setSketchVisible(bool visible);
  void addSketch(const sketch::Sketch& sketch, const QString& supportName);
  void addSketch(const sketch::Sketch& sketch, const QString& supportName,
                 const SketchPlacement& placement);
  void updateSketch(std::size_t index, const sketch::Sketch& sketch,
                    const QString& supportName);
  void updateSketch(std::size_t index, const sketch::Sketch& sketch,
                    const QString& supportName,
                    const SketchPlacement& placement);
  void removeSketch(std::size_t index);
  void setSketchVisible(std::size_t index, bool visible);
  void setOriginVisible(bool visible);
  void setBasePlaneVisible(int plane, bool visible);
  void resetScene();
  void beginSketchPlaneSelection();
  void beginExtrusionSurfaceSelection();
  void showExtrusionManipulator(double lengthMm);
  void hideExtrusionManipulator();
  void setExtrusionPreviewLength(double lengthMm);
  [[nodiscard]] bool hasSelectedFace() const noexcept;
  [[nodiscard]] QString selectedFaceName() const;
  [[nodiscard]] std::optional<std::size_t> selectedBodyFaceIndex() const noexcept;
  [[nodiscard]] std::optional<FaceReference> selectedBodyFace() const noexcept;
  [[nodiscard]] std::optional<EdgeReference> selectedBodyEdge() const noexcept;
  [[nodiscard]] std::vector<EdgeReference> selectedBodyEdges() const;
  void setSelectedBodyEdges(const std::vector<EdgeReference>& edges);
  void setToolPreviewShape(BodyId bodyId, FeatureId featureId,
                           ShapeFeature::ShapePtr shape);
  void clearToolPreviewShape();
  void setToolManipulator(const LinearToolManipulator& manipulator);
  void clearToolManipulator();
  void fitAll();
  void viewTop();
  void viewBottom();
  void viewFront();
  void viewBack();
  void viewRight();
  void viewLeft();
  void viewIsometric();
  [[nodiscard]] const sketch::Sketch& extrusionCandidateSketch() const noexcept;
  [[nodiscard]] QString extrusionCandidateSupport() const;
  [[nodiscard]] std::size_t extrusionCandidateSketchIndex() const noexcept;
  [[nodiscard]] bool extrusionCandidateOnBodyCap() const noexcept;
  void commitAdditiveExtrusion(const sketch::Sketch& sketch,
                               const QString& supportName,
                               double startMm, double lengthMm);
  void removeLastAdditiveExtrusion();
  [[nodiscard]] const std::vector<SolidFeature>& solidFeatures() const noexcept;
  [[nodiscard]] const sketch::Sketch& solidSketch() const noexcept;
  [[nodiscard]] QString solidSupport() const;
  [[nodiscard]] QPointF bodyPosition() const noexcept;
  void setBodyPosition(QPointF position);

 signals:
  void selectionChanged(const QString& description);
  void sketchPlanePicked(const QString& planeName);
  void extrusionSurfacePicked(const QString& surfaceName);
  void extrusionPreviewLengthChanged(double lengthMm);
  void bodyMoveCommitted(QPointF previous, QPointF current);
  void bodyEdgeSelectionChanged();
  void toolManipulatorValueChanged(double valueMm);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void updateSketchPlaneHover(QPointF position);
  void updateExtrusionHover(QPointF position);
  void refreshSelectedExtrusionPolygon();
  [[nodiscard]] QPointF extrusionScreenOffset() const;
  void rebuildSelectedExtrusionSketch();
  void updateBodyHover(QPointF position);
  void rebuildBodyDisplay(const std::vector<BodyViewShape>& shapes,
                          bool clearSelection);
  [[nodiscard]] std::optional<EdgeReference> edgeReferenceForGlobalIndex(
      std::size_t index) const noexcept;
  enum class PickMode { None, SketchPlane, ExtrusionSurface };
  BoxParameters box_;
  ShapeFeature::ShapePtr bodyShape_;
  BodyRenderMesh bodyRenderMesh_;
  std::vector<BodyViewShape> bodyViewShapes_;
  struct BodyTopologyRange {
    BodyId bodyId{kInvalidBodyId};
    FeatureId featureId{kInvalidFeatureId};
    std::size_t firstFace{};
    std::size_t faceCount{};
    std::size_t firstEdge{};
    std::size_t edgeCount{};
  };
  std::vector<BodyTopologyRange> bodyTopologyRanges_;
  BodyId bodyId_{kInvalidBodyId};
  FeatureId bodyFeatureId_{kInvalidFeatureId};
  sketch::Sketch sketch_;
  SketchPlacement sketchPlacement_{SketchPlacement::xy()};
  sketch::Sketch solidSketch_;
  std::vector<SolidFeature> additiveExtrusions_;
  bool solidVisible_{false};
  QString solidSupportName_{QStringLiteral("XY")};
  bool sketchVisible_{true};
  struct DisplaySketch {
    sketch::Sketch geometry;
    QString supportName;
    SketchPlacement placement;
    bool visible{true};
  };
  std::vector<DisplaySketch> displaySketches_;
  bool originVisible_{true};
  bool basePlanesVisible_[3]{false, false, false};
  PickMode pickMode_{PickMode::None};
  int selectedFace_{-1};
  std::size_t hoveredBodyFaceIndex_{static_cast<std::size_t>(-1)};
  std::size_t hoveredBodyEdgeIndex_{static_cast<std::size_t>(-1)};
  std::size_t selectedBodyEdgeIndex_{static_cast<std::size_t>(-1)};
  std::vector<std::size_t> selectedBodyEdgeIndices_;
  int selectedBasePlane_{-1};
  int selectedVertex_{-1};
  bool selectedOrigin_{false};
  QPolygonF extrusionHoverPolygon_;
  QPainterPath extrusionHoverPath_;
  QPolygonF selectedExtrusionPolygon_;
  std::vector<QPolygonF> selectedExtrusionPolygons_;
  std::vector<QPainterPath> selectedExtrusionPaths_;
  std::vector<sketch::Sketch> selectedExtrusionRegionSketches_;
  sketch::Sketch hoveredExtrusionSketch_;
  sketch::Sketch selectedExtrusionSketch_;
  QString hoveredExtrusionSupport_;
  QString selectedExtrusionSupport_;
  QString hoveredExtrusionSurface_;
  std::size_t hoveredExtrusionSketchIndex_{static_cast<std::size_t>(-1)};
  std::size_t selectedExtrusionSketchIndex_{static_cast<std::size_t>(-1)};
  QDoubleSpinBox* extrusionLengthEditor_{nullptr};
  QPointF extrusionManipulatorAnchor_;
  double extrusionPreviewLengthMm_{25.0};
  bool extrusionManipulatorVisible_{false};
  bool selectedExtrusionBodyFace_{false};
  bool hoveredExtrusionOnBodyCap_{false};
  bool selectedExtrusionOnBodyCap_{false};
  bool draggingExtrusionHandle_{false};
  std::optional<LinearToolManipulator> toolManipulator_;
  bool draggingToolManipulator_{false};
  bool panningView_{false};
  QPointF cameraPan_;
  bool draggingBody_{false};
  QPointF bodyDragStart_;
  float offsetX_{0.0F};
  float offsetY_{0.0F};
  QPoint lastMousePosition_;
  float yaw_{-35.0F};
  float pitch_{25.0F};
  float zoom_{1.0F};
};

}  // namespace solidar
