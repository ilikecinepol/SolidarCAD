#pragma once

#include <QPoint>
#include <QPolygonF>
#include <QRectF>
#include <QStringList>
#include <QWidget>

#include <optional>
#include <cstdint>
#include <utility>
#include <vector>

#include "sketch/Sketch.h"
#include "model/Document.h"
#include "ui/BodyRenderMesh.h"
#include "ui/SketchCommandController.h"
#include "ui/SketchInteractionController.h"
#include "ui/SketchHitSceneAdapter.h"
#include "ui/SketchRenderer.h"
#include "ui/ViewCube.h"

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QDoubleSpinBox;
class QEvent;
class QVariantAnimation;
class QTimer;

namespace solidar {

struct SketchEditContext {
  SketchId sketchId{kInvalidSketchId};
  SketchPlacement placement{SketchPlacement::xy()};
  ShapeFeature::ShapePtr supportShape;
  std::shared_ptr<const TopologyIndex> supportTopologyIndex;
  std::optional<FaceReference> supportFace;
  bool autoProjectSupportFace{false};
};

class SketchCanvas final : public QWidget {
  Q_OBJECT

 public:
  enum class Tool {
    Select,
    Line,
    Rectangle,
    Circle,
    Arc,
    Bezier,
    Projection,
    AutoDimension,
    LockConstraint,
    OrthogonalConstraint,
    CoincidentConstraint,
    PerpendicularConstraint,
    ParallelConstraint,
    EqualConstraint,
    TangentConstraint,
    Mirror,
    Trim
  };
  enum class CircleMode {
    CenterRadius,
    TwoPoints,
    ThreePoints,
    ThreeTangents,
    TwoTangentsRadius
  };
  enum class RectangleMode { TwoPoints, ThreePoints, FromCenter };

  explicit SketchCanvas(QWidget* parent = nullptr);
  void setRectangle(double widthMm, double heightMm);
  void setTool(Tool tool);
  void clearSketch();
  void resetSketch();
  void loadSketch(const sketch::Sketch& sketch);
  void deleteSelection();
  void setPrimaryDimension(double value);
  void setSelectedDashed(bool dashed);
  void commitCurrentDimension();
  void setGridVisible(bool visible);
  void setSnapEnabled(bool enabled);
  void setCircleMode(CircleMode mode);
  void setCircleDiameter(double diameterMm);
  void setRectangleMode(RectangleMode mode);
  void undo();
  void redo();
  void rotateViewClockwise();
  void rotateViewCounterClockwise();
  void orbitView(double yawDeltaDeg, double pitchDeltaDeg);
  void setViewOrientation(double yawDeg, double pitchDeg);
  void resetViewRotation();
  [[nodiscard]] int viewQuarterTurns() const noexcept;
  [[nodiscard]] double viewRotationDegrees() const noexcept;
  [[nodiscard]] double viewYawDegrees() const noexcept;
  [[nodiscard]] double viewPitchDegrees() const noexcept;
  [[nodiscard]] bool viewAlignedToSketchPlane() const noexcept;
  // Global camera orientation consumed by the shared ViewCube.  The sketch
  // camera itself is stored in the active plane's local coordinate frame.
  [[nodiscard]] CameraOrientation viewCubeCamera() const noexcept;
  void setReferenceBody(BoxParameters box, const SketchPlacement& placement,
                        bool visible);
  // Preserves the current 3D viewport's visual up direction when the sketch
  // plane is opened. This changes presentation only, never SketchPlacement.
  void setInitialViewUp(Vector3d worldUp) noexcept;
  void setSketchEditContext(const SketchEditContext& context);
  void setSceneReferences(
      SketchPlacement activePlacement,
      const std::vector<ShapeFeature::ShapePtr>& bodyShapes,
      std::vector<SketchSceneReference> sketches,
      const std::vector<ReferenceImage>& images = {});
  // Refreshes image visibility/content without resetting the sketch camera.
  void updateSceneImages(const std::vector<ReferenceImage>& images);
  void clearSketchEditContext();
  void setReferenceProfile(const sketch::Sketch& profile, bool visible);
  [[nodiscard]] bool canUndo() const noexcept;
  [[nodiscard]] bool canRedo() const noexcept;
  [[nodiscard]] std::size_t undoHistorySize() const noexcept;
  [[nodiscard]] std::size_t undoHistoryRetainedBytes() const noexcept;
  [[nodiscard]] std::size_t committedRenderSceneBuildCount() const noexcept;
  void flushConstraintDiagnostics();
  [[nodiscard]] std::size_t fullDiagnosticsCount() const noexcept;
  [[nodiscard]] Tool tool() const noexcept;
  [[nodiscard]] const sketch::Sketch& sketch() const noexcept;
  [[nodiscard]] const SketchToolState& interactionState() const noexcept;
  [[nodiscard]] bool hasActiveInteraction() const noexcept;
  void selectDimension(std::size_t index) noexcept;
  [[nodiscard]] bool hasRealReferenceBody() const noexcept;
  [[nodiscard]] std::size_t referenceFaceEdgeCount() const noexcept;
  [[nodiscard]] std::size_t referenceBodyEdgeCount() const noexcept;
  [[nodiscard]] std::size_t sceneBodyCount() const noexcept;
  [[nodiscard]] std::size_t sceneSketchCount() const noexcept;
  bool projectReferenceEdge(std::size_t edgeVectorIndex);
  struct ConstraintPanelEntry {
    QString description;
    sketch::ConstraintId constraintId{sketch::kInvalidConstraintId};
    std::size_t dimensionIndex{static_cast<std::size_t>(-1)};
    bool checked{true};

    [[nodiscard]] bool isDimension() const noexcept {
      return dimensionIndex != static_cast<std::size_t>(-1);
    }
  };

  [[nodiscard]] std::vector<ConstraintPanelEntry>
  selectedConstraintPanelEntries() const;
  bool removeConstraintById(sketch::ConstraintId id);
  bool setDimensionDriving(std::size_t dimensionIndex, bool driving);

  // Screen -> Sketch axis mapping for point AutoDimension. The user chooses a
  // dimension orientation visually (horizontal or vertical on screen), while
  // x/y constraints always store Sketch-axis semantics. This single helper
  // resolves a visual gesture to the Sketch-axis point mode ("x"/"y").
  [[nodiscard]] static QString pointDimensionModeForScreenAxis(
      bool horizontalDimensionLine, int viewQuarterTurns);

  // Resolves the full point AutoDimension mode for a cursor gesture. The two
  // booleans express the requested screen orientation; deltaX/deltaY are the
  // Sketch-axis separations of the selected points. Returns "x", "y" or
  // "aligned". The mapped Sketch axis must have a non-zero separation,
  // otherwise the gesture falls back to aligned.
  [[nodiscard]] static QString resolvePointDimensionMode(
      bool horizontalLine, bool verticalLine,
      double deltaX, double deltaY, int viewQuarterTurns);

  // Witness segment (in Sketch coordinates) that a point dimension actually
  // measures. PointDistanceX/Y project onto the Sketch X/Y axis; PointDistance
  // keeps the raw pair. Callers map each endpoint with mapPoint() so that
  // viewport rotation stays purely visual.
  [[nodiscard]] static std::pair<sketch::Point, sketch::Point>
  pointDimensionWitness(sketch::Point first, sketch::Point second,
                        sketch::DimensionKind kind);

  // Screen-space radius for angular dimensions. It follows the stored offset
  // but remains readable under extreme zoom or legacy large offsets.
  [[nodiscard]] static double angularDimensionRadiusPx(
      double offsetMm, double pixelsPerMm, double viewportExtentPx) noexcept;

signals:
  void geometryChanged(double widthMm, double heightMm);
  void selectionChanged(const QString& description);
  void toolChanged(Tool tool);
  void undoAvailable(bool available);
  void redoAvailable(bool available);
  void primaryDimensionChanged(double value);
  void lineStyleSelectionChanged(bool lineSelected, bool dashed);
  void constraintStatusChanged(const QString& status);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  [[nodiscard]] SketchRenderSnapshot renderSnapshot() const;
  void markCommittedRenderSceneDirty() noexcept;
  enum class SelectionKind { None, Line, Circle, Arc, Bezier };
  enum class ConstructionSnapKind {
    None,
    LinePoint,
    LineMidpoint,
    CircleCenter,
    ElementCenter,
    LineBody,
    CircleBody,
    XAxis,
    YAxis,
    Origin
  };

  struct ConstructionSnap {
    sketch::Point point{};
    ConstructionSnapKind kind{ConstructionSnapKind::None};
    sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
    std::size_t elementId{};
    sketch::PointReference pointReference{};
  };

  [[nodiscard]] sketch::Point rotateForView(sketch::Point point) const noexcept;
  [[nodiscard]] sketch::Point rotateFromView(sketch::Point point) const noexcept;
  [[nodiscard]] bool screenToSketchMappingAvailable() const noexcept;
  struct ProjectedLocalPoint {
    double xMm{};
    double yMm{};
    double depthMm{};
  };
  [[nodiscard]] ProjectedLocalPoint projectLocalPoint(
      double xMm, double yMm, double zMm) const noexcept;
  [[nodiscard]] QPointF mapWorldPoint(Point3d point) const;
  [[nodiscard]] double worldPointDepth(Point3d point) const noexcept;
  [[nodiscard]] QPolygonF circlePolyline(
      sketch::Point center, double radiusMm, double startAngleRad = 0.0,
      double sweepAngleRad = 2.0 * 3.14159265358979323846,
      int segmentCount = 72) const;
  void animateViewToDirection(Point3d direction);
  void animateViewRotationBy(double deltaDeg);
  void clearViewCubeHover();
  [[nodiscard]] QPointF mapPoint(sketch::Point point) const;
  [[nodiscard]] sketch::Point unmapPoint(QPointF point) const;
  [[nodiscard]] sketch::Point snappedPoint(QPointF point) const;
  [[nodiscard]] ConstructionSnap constructionSnapAt(QPointF position) const;
  bool commitDraggedPointSnap(sketch::PointReference movingPoint,
                              const ConstructionSnap& snap);
  [[nodiscard]] std::optional<SketchProjectionEdgeToken> referenceEdgeAt(
      QPointF position) const;
  [[nodiscard]] std::optional<SketchProjectionEdgeToken>
  projectionTokenForFlatEdge(std::size_t edgeVectorIndex) const noexcept;
  [[nodiscard]] const RenderEdge* referenceEdge(
      std::size_t edgeVectorIndex) const noexcept;
  [[nodiscard]] const RenderEdge* referenceEdge(
      const SketchProjectionEdgeToken& token) const noexcept;
  bool projectReferenceEdge(const SketchProjectionEdgeToken& token);
  void fitReferenceGeometry();
  bool appendProjectedEdge(const RenderEdge& edge, bool recordUndo,
                           bool reportStatus);
  bool appendProjectedCircularEdge(const RenderEdge& edge, bool recordUndo,
                                   bool reportStatus);
  void selectAt(QPointF position, bool additive = false,
                bool preserveExistingIfHit = false);
  void selectInRect(const QRectF& rect, bool additive);
  void clearGeometrySelection();
  [[nodiscard]] bool lineSelected(sketch::GeometryId id) const;
  [[nodiscard]] bool lineElementSelected(std::size_t elementId) const;
  [[nodiscard]] bool circleSelected(sketch::GeometryId id) const;
  [[nodiscard]] bool arcSelected(sketch::GeometryId id) const;
  [[nodiscard]] bool bezierSelected(sketch::GeometryId id) const;
  void commitPoint(sketch::Point point);
  void showDimensionEditor(QPoint position);
  void updateDimensionEditor();
  void positionDimensionEditor();
  void commitDimensionEditor();
  void hideDimensionEditor();
  void notifyGeometryChanged();
  [[nodiscard]] SketchCommandResult executeCommand(
      const SketchCommand& command);
  [[nodiscard]] SketchCommandResult executeLiveCommand(
      const SketchLiveCommand& command);
  void applyCommandEffects(const SketchCommandEffects& effects);
  void runConstraintDiagnostics();
  void pushUndoState();
  [[nodiscard]] bool finalizeUndoState();
  void cancelPendingUndo();
  void commitCirclePoint(sketch::Point point);
  void commitArcPoint(sketch::Point point);
  void commitRectanglePoint(sketch::Point point);
  void handleAutoDimensionClick(QPointF position);
  void handleLockConstraintClick(QPointF position);
  void handleOrthogonalConstraintClick(QPointF position);
  void handleCoincidentConstraintClick(QPointF position);
  void handlePerpendicularConstraintClick(QPointF position);
  void handleParallelConstraintClick(QPointF position);
  void handleEqualConstraintClick(QPointF position);
  void handleTangentConstraintClick(QPointF position);
  [[nodiscard]] std::optional<sketch::GeometryId> lineAt(
      QPointF position, double tolerancePx = 9.0) const;
  [[nodiscard]] SketchHitScene hitScene(
      const SketchHitTolerancePolicy& tolerance = {},
      bool includeDatums = false) const;
  [[nodiscard]] std::optional<SketchPickEntityRef> geometryAt(
      QPointF position, double tolerancePx = 9.0,
      const SketchPickFilter& filter = {},
      sketch::GeometryId excludedGeometry = sketch::kInvalidGeometryId) const;
  [[nodiscard]] std::optional<SketchPickPointRef> pointAt(
      QPointF position, double tolerancePx = 10.0,
      const SketchPickFilter& filter = {},
      sketch::GeometryId excludedGeometry = sketch::kInvalidGeometryId,
      std::size_t excludedElement = 0) const;
  [[nodiscard]] std::optional<std::vector<sketch::Line>>
  resolvedCircleGuideLines() const;
  [[nodiscard]] std::vector<sketch::GeometryId> closedLineContour(
      sketch::GeometryId seed) const;
  using MirrorGeometryKind = SketchMirrorGeometryKind;
  using MirrorGeometryRef = SketchMirrorGeometryRef;
  [[nodiscard]] std::optional<MirrorGeometryRef> mirrorGeometryAt(
      QPointF position, double tolerancePx = 9.0) const;
  [[nodiscard]] std::vector<MirrorGeometryRef> closedMirrorContour(
      MirrorGeometryRef seed) const;
  void setMirrorSourceSelection(
      const std::vector<MirrorGeometryRef>& source);
  bool mirrorContourAboutLine(sketch::GeometryId axisId);
  using TrimGeometryKind = SketchTrimGeometryKind;
  using TrimPreview = SketchTrimPreview;
  [[nodiscard]] std::optional<TrimPreview> trimPreviewAt(
      QPointF position) const;
  bool trimAt(QPointF position);
  void commitAutoDimension();
  [[nodiscard]] bool dimensionSegment(std::size_t index, QPointF& first,
                                      QPointF& second) const;
  [[nodiscard]] QPointF dimensionLabelCenter(std::size_t index,
                                             QPointF first,
                                             QPointF second) const;
  [[nodiscard]] bool beginDimensionLabelDrag(QPointF position);
  [[nodiscard]] bool beginDimensionLineDrag(QPointF position);
  [[nodiscard]] std::optional<std::size_t> dimensionAt(
      QPointF position,
      std::optional<SketchDimensionHitKind> requiredKind = std::nullopt) const;
  [[nodiscard]] std::optional<SketchDimensionReference> dimensionReference(
      std::size_t index) const;
  [[nodiscard]] std::optional<std::size_t> dimensionIndex(
      const SketchDimensionReference& reference) const;

  sketch::Sketch sketch_;
  SketchRenderer renderer_;
  mutable SketchRenderSceneCache renderSceneCache_;
  std::uint64_t renderSceneRevision_{1};
  std::uint64_t sketchGeneration_{1};
  SketchCommandController commandController_;
  SketchInteractionController interaction_;
  std::vector<sketch::SketchDelta> undoStack_;
  std::vector<sketch::SketchDelta> redoStack_;
  std::optional<SketchTransactionToken> pendingUndoTransaction_;
  bool commandSequenceFailed_{};
  std::size_t undoRetainedBytes_{};
  std::size_t redoRetainedBytes_{};
  SelectionKind selectionKind_{SelectionKind::None};
  sketch::GeometryId selectionCircleId_{sketch::kInvalidGeometryId};
  sketch::GeometryId selectionLineId_{sketch::kInvalidGeometryId};
  sketch::GeometryId selectionArcId_{sketch::kInvalidGeometryId};
  sketch::GeometryId selectionBezierId_{sketch::kInvalidGeometryId};
  std::size_t selectionElementId_{};
  std::vector<sketch::GeometryId> selectedLineIds_;
  std::vector<std::size_t> selectedElementIds_;
  std::vector<sketch::GeometryId> selectedCircleIds_;
  std::vector<sketch::GeometryId> selectedArcIds_;
  std::vector<sketch::GeometryId> selectedBezierIds_;
  sketch::Point hoverPoint_{};
  std::optional<ConstructionSnap> constructionHover_;
  QDoubleSpinBox* primaryDimension_{nullptr};
  QDoubleSpinBox* secondaryDimension_{nullptr};
  double pixelsPerMm_{5.0};
  double snapStepMm_{5.0};
  bool snapEnabled_{false};
  bool gridVisible_{true};
  std::optional<SketchProjectionEdgeToken> hoveredProjectionEdge_;
  Vector3d preferredViewUp_{};
  double initialViewRotationDeg_{0.0};
  double viewRotationDeg_{0.0};
  double viewYawDeg_{0.0};
  double viewPitchDeg_{0.0};
  QVariantAnimation* viewCubeAnimation_{nullptr};
  QTimer* constraintDiagnosticsTimer_{nullptr};
  std::size_t fullDiagnosticsCount_{};
  ViewCubeHit cubeHover_;
  ViewCubeHit cubePressed_;
  CircleMode circleMode_{CircleMode::CenterRadius};
  double circleDiameterMm_{20.0};
  RectangleMode rectangleMode_{RectangleMode::TwoPoints};
  BoxParameters referenceBox_{};
  bool referenceBodyVisible_{false};
  std::shared_ptr<const BodyRenderMesh> referenceBodyMesh_;
  std::shared_ptr<const BodyRenderMesh> referenceFaceMesh_;
  std::vector<std::shared_ptr<const BodyRenderMesh>> sceneBodyMeshes_;
  std::vector<SketchSceneReference> sceneSketches_;
  std::vector<SketchSceneImageReference> sceneImages_;
  SketchPlacement referencePlacement_{SketchPlacement::xy()};
  bool realReferenceBodyVisible_{false};
  sketch::Sketch referenceProfile_;
  bool referenceProfileVisible_{false};
};

}  // namespace solidar
