#pragma once

#include <QPoint>
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
#include <QOpenGLWidget>
#include <vector>
#include <optional>
#include <string>

#include "model/Document.h"
#include "model/SolidFeature.h"
#include "sketch/Sketch.h"
#include "ui/BodyRenderMesh.h"
#include "ui/ViewportPicking.h"
#include "ui/ManipulatorLayout.h"
#include "ui/ViewportRenderer.h"
#include "ui/ViewCube.h"
#include "model/ToolSession.h"

class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QKeyEvent;
class QDoubleSpinBox;
class QVariantAnimation;

namespace solidar {

class ToolParameterHud;

struct BodyViewShape {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  ShapeFeature::ShapePtr shape;
};

enum class SelectionFilter { Any, Face, Edge, Plane };

// Minimum marquee extent on release below which a drag is treated as a click,
// mirroring the Sketcher selection-box threshold.
inline constexpr double kMarqueeMinSizePx = 3.0;

class Viewport final : public QOpenGLWidget {
  Q_OBJECT

 public:
  static constexpr qulonglong kGlobalXAxisToken = 0xfffffffffffffff0ULL;
  static constexpr qulonglong kGlobalYAxisToken = 0xfffffffffffffff1ULL;
  static constexpr qulonglong kGlobalZAxisToken = 0xfffffffffffffff2ULL;
  explicit Viewport(QWidget* parent = nullptr);
  ~Viewport() override;
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
  void beginRevolveAxisSelection(std::size_t sketchIndex);
  void showExtrusionManipulator(double lengthMm);
  void hideExtrusionManipulator();
  void setExtrusionPreviewLength(double lengthMm);
  [[nodiscard]] bool hasSelectedFace() const noexcept;
  [[nodiscard]] QString selectedFaceName() const;
  [[nodiscard]] std::optional<std::size_t> selectedBodyFaceIndex() const noexcept;
  [[nodiscard]] std::optional<FaceReference> selectedBodyFace() const noexcept;
  [[nodiscard]] std::vector<FaceReference> selectedBodyFaces() const;
  void setSelectedBodyFaces(const std::vector<FaceReference>& faces);
  void setFaceMultiSelectionMode(bool enabled) noexcept;
  [[nodiscard]] bool faceMultiSelectionMode() const noexcept;
  [[nodiscard]] std::optional<EdgeReference> selectedBodyEdge() const noexcept;
  [[nodiscard]] std::vector<EdgeReference> selectedBodyEdges() const;
  void setSelectedBodyEdges(const std::vector<EdgeReference>& edges);
  void setEdgeMultiSelectionMode(bool enabled) noexcept;
  [[nodiscard]] bool edgeMultiSelectionMode() const noexcept;
  [[nodiscard]] const std::vector<BodyId>& selectedBodies() const noexcept;
  void setSelectedBodies(std::vector<BodyId> ids);
  // Face ordinals passed to the renderer as the selected-faces set. When whole
  // bodies are selected this is the union of every selected body's face
  // ordinals (so a body selection renders with the standard selected-face
  // tint); otherwise it is the sub-element face selection.
  [[nodiscard]] std::vector<std::size_t> effectiveSelectedFaceIndices() const;
  [[nodiscard]] std::optional<std::size_t> hoveredBodyEdgeIndex() const noexcept;
  void setSelectionFilter(SelectionFilter filter) noexcept;
  [[nodiscard]] SelectionFilter selectionFilter() const noexcept;
  [[nodiscard]] bool marqueeActive() const noexcept;
  void setToolPreviewShape(BodyId bodyId, FeatureId featureId,
                           ShapeFeature::ShapePtr shape);
  void clearToolPreviewShape();
  void setToolManipulator(const LinearToolManipulator& manipulator);
  void setAngularToolManipulator(const AngularToolManipulator& manipulator);
  [[nodiscard]] const std::optional<AngularToolManipulator>&
  angularToolManipulator() const noexcept { return angularToolManipulator_; }
  void clearToolManipulator();
  void fitAll();
  void viewTop();
  void viewBottom();
  void viewFront();
  void viewBack();
  void viewRight();
  void viewLeft();
  void viewIsometric();
  void setDisplayMode(ViewportDisplayMode mode);
  void setMeshQuality(ViewportMeshQuality quality);
  [[nodiscard]] ViewportDisplayMode displayMode() const noexcept;
  [[nodiscard]] ViewportMeshQuality meshQuality() const noexcept;
  [[nodiscard]] float cameraYawDegrees() const noexcept;
  [[nodiscard]] float cameraPitchDegrees() const noexcept;
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
  void setWorkGridPlacement(const SketchPlacement& placement);
  void resetWorkGridPlacement();
  void setWorkGridVisible(bool visible);
  [[nodiscard]] bool workGridVisible() const noexcept;

 signals:
  void selectionChanged(const QString& description);
  void sketchPlanePicked(const QString& planeName);
  void extrusionSurfacePicked(const QString& surfaceName);
  void extrusionPreviewLengthChanged(double lengthMm);
  void bodyMoveCommitted(QPointF previous, QPointF current);
  void bodyEdgeSelectionChanged();
  void bodyFaceSelectionChanged();
  // Published whenever a whole-body (model-level) selection is committed via
  // setSelectedBodies; MainWindow consumption is intentionally deferred.
  void bodiesSelected(const std::vector<BodyId>& ids);
  void toolManipulatorValueChanged(double valueMm);
  void angularToolManipulatorValueChanged(double angleDeg);
  // Emitted after a HUD field commit via Enter (after the value has been routed
  // to the session preview). MainWindow uses it to perform the active tool's
  // Accept without re-interpreting the already-committed value.
  void toolParameterCommitted();
  // Emitted when Tab/Backtab is pressed with viewport focus and the HUD has no
  // editable field, so MainWindow can focus the active tool's dock field.
  void tabFocusRequested(bool backward);
  // Matches the Revolve axis combo data: 1/2 are sketch X/Y axes,
  // values >= 3 encode a sketch line id plus three.
  void revolveAxisPicked(qulonglong axisToken);

 protected:
  void initializeGL() override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  void animateOrientation(CameraOrientation target);
  void setStandardView(StandardView view);
  void clearCubeHover();
  QVariantAnimation* orientationAnimation_{nullptr};
  ViewCubeHit cubeHover_, cubePressed_;
  Qt::CursorShape cursorBeforeCube_{Qt::ArrowCursor};
  void updateSketchPlaneHover(QPointF position);
  void updateExtrusionHover(QPointF position);
  void pickFallbackBodyFace(QPointF position);
  void refreshSelectedExtrusionPolygon();
  [[nodiscard]] QPointF extrusionScreenOffset(double lengthMm) const;
  // Shared presentation helpers. A single layout/radius is computed once and
  // used by both the draw pass and the hit-test so the two can never diverge.
  struct AngularVisual {
    QPointF origin;
    Vector3d u{1.0, 0.0, 0.0};
    Vector3d v{0.0, 1.0, 0.0};
    Vector3d axis{0.0, 0.0, 1.0};
    double visualRadiusMm{};
  };
  [[nodiscard]] std::optional<ManipulatorLayoutResult> toolManipulatorLayout()
      const;
  [[nodiscard]] std::optional<AngularVisual> angularVisual() const;
  void rebuildSelectedExtrusionSketch();
  void updateBodyHover(QPointF position);
  void rebuildBodyDisplay(const std::vector<BodyViewShape>& shapes,
                          bool clearSelection);
  void selectInRect(const QRectF& rect, bool additive, bool singleOnly = false);
  void cancelMarquee();
  // Clears only whole-body selection and publishes bodiesSelected({}) when the
  // state actually changes. Face/edge state is intentionally left untouched.
  void clearWholeBodySelection() noexcept;
  void commitEdgeSelection(std::size_t globalIndex, bool toggle);
  void commitFaceSelection(std::size_t globalIndex, bool toggle);
  [[nodiscard]] std::optional<EdgeReference> edgeReferenceForGlobalIndex(
      std::size_t index) const noexcept;
  [[nodiscard]] std::optional<FaceReference> faceReferenceForGlobalIndex(
      std::size_t index) const noexcept;
  enum class PickMode { None, SketchPlane, ExtrusionSurface, RevolveAxis };
  BoxParameters box_;
  ShapeFeature::ShapePtr bodyShape_;
  BodyRenderMesh bodyRenderMesh_;
  ShapeFeature::ShapePtr toolPreviewShape_;
  BodyRenderMesh toolPreviewRenderMesh_;
  ViewportRenderer renderer_;
  ViewportDisplayMode displayMode_{ViewportDisplayMode::ShadedWithEdges};
  ViewportMeshQuality meshQuality_{ViewportMeshQuality::Normal};
  BodyId toolPreviewBodyId_{kInvalidBodyId};
  FeatureId toolPreviewFeatureId_{kInvalidFeatureId};
  std::vector<BodyViewShape> bodyViewShapes_;
  struct BodyTopologyRange {
    BodyId bodyId{kInvalidBodyId};
    FeatureId featureId{kInvalidFeatureId};
    ShapeFeature::ShapePtr shape;
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
  std::vector<std::size_t> selectedBodyFaceIndices_;
  std::vector<FaceReference> selectedBodyFaceReferences_;
  bool faceMultiSelectionMode_{false};
  std::size_t hoveredBodyFaceIndex_{static_cast<std::size_t>(-1)};
  std::size_t hoveredBodyEdgeIndex_{static_cast<std::size_t>(-1)};
  std::size_t selectedBodyEdgeIndex_{static_cast<std::size_t>(-1)};
  std::vector<std::size_t> selectedBodyEdgeIndices_;
  std::vector<EdgeReference> selectedBodyEdgeReferences_;
  bool edgeMultiSelectionMode_{false};
  // Transient whole-body (model-level) selection published via bodiesSelected.
  std::vector<BodyId> selectedBodyIds_;
  SelectionFilter selectionFilter_{SelectionFilter::Any};
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
  std::size_t revolveAxisSketchIndex_{static_cast<std::size_t>(-1)};
  QDoubleSpinBox* extrusionLengthEditor_{nullptr};
  ToolParameterHud* toolParameterHud_{nullptr};
  std::string toolHudParameterId_;
  QPointF extrusionManipulatorAnchor_;
  double extrusionPreviewLengthMm_{25.0};
  bool extrusionManipulatorVisible_{false};
  bool selectedExtrusionBodyFace_{false};
  bool hoveredExtrusionOnBodyCap_{false};
  bool selectedExtrusionOnBodyCap_{false};
  bool draggingExtrusionHandle_{false};
  std::optional<LinearToolManipulator> toolManipulator_;
  std::optional<AngularToolManipulator> angularToolManipulator_;
  ManipulatorStyle manipulatorStyle_;
  bool draggingToolManipulator_{false};
  std::optional<LinearDragSnapshot> linearDragSnapshot_;
  bool draggingAngularToolManipulator_{false};
  bool panningView_{false};
  QPointF cameraPan_;
  bool draggingBody_{false};
  QPointF bodyDragStart_;
  bool marqueeActive_{false};
  QPointF marqueeStart_{};
  QPointF marqueeCurrent_{};
  bool marqueeAdditive_{false};
  float offsetX_{0.0F};
  float offsetY_{0.0F};
  QPoint lastMousePosition_;
  float yaw_{-35.0F};
  float pitch_{25.0F};
  float zoom_{1.0F};
  SketchPlacement workGridPlacement_{SketchPlacement::xy()};
  bool workGridVisible_{true};
};

}  // namespace solidar
