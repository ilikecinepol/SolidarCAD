#pragma once

#include <QPoint>
#include <QRectF>
#include <QStringList>
#include <QWidget>

#include <optional>
#include <utility>
#include <vector>

#include "sketch/Sketch.h"
#include "model/Document.h"
#include "ui/BodyRenderMesh.h"

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QDoubleSpinBox;
class QEvent;

namespace solidar {

struct SketchEditContext {
  SketchId sketchId{kInvalidSketchId};
  SketchPlacement placement{SketchPlacement::xy()};
  ShapeFeature::ShapePtr supportShape;
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
    Projection,
    AutoDimension,
    LockConstraint,
    OrthogonalConstraint,
    CoincidentConstraint,
    PerpendicularConstraint,
    ParallelConstraint,
    EqualConstraint,
    TangentConstraint
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
  void rotateViewClockwise();
  void rotateViewCounterClockwise();
  void resetViewRotation();
  [[nodiscard]] int viewQuarterTurns() const noexcept;
  void setReferenceBody(BoxParameters box, const QString& support, bool visible);
  void setSketchEditContext(const SketchEditContext& context);
  void clearSketchEditContext();
  void setReferenceProfile(const sketch::Sketch& profile, bool visible);
  [[nodiscard]] bool canUndo() const noexcept;
  [[nodiscard]] Tool tool() const noexcept;
  [[nodiscard]] const sketch::Sketch& sketch() const noexcept;
  [[nodiscard]] bool hasRealReferenceBody() const noexcept;
  [[nodiscard]] std::size_t referenceFaceEdgeCount() const noexcept;
  [[nodiscard]] std::size_t referenceBodyEdgeCount() const noexcept;
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

signals:
  void geometryChanged(double widthMm, double heightMm);
  void selectionChanged(const QString& description);
  void toolChanged(Tool tool);
  void undoAvailable(bool available);
  void primaryDimensionChanged(double value);
  void lineStyleSelectionChanged(bool lineSelected, bool dashed);
  void constraintStatusChanged(const QString& status);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  enum class SelectionKind { None, Line, Circle };
  enum class ConstructionSnapKind {
    None,
    LinePoint,
    LineMidpoint,
    CircleCenter,
    ElementCenter,
    LineBody,
    CircleBody
  };

  struct ConstructionSnap {
    sketch::Point point{};
    ConstructionSnapKind kind{ConstructionSnapKind::None};
    sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
    std::size_t elementId{};
  };

  [[nodiscard]] sketch::Point rotateForView(sketch::Point point) const noexcept;
  [[nodiscard]] sketch::Point rotateFromView(sketch::Point point) const noexcept;
  [[nodiscard]] QRectF viewCubeBodyRect() const;
  [[nodiscard]] QRectF viewCubeLeftRect() const;
  [[nodiscard]] QRectF viewCubeRightRect() const;
  [[nodiscard]] QPointF mapPoint(sketch::Point point) const;
  [[nodiscard]] sketch::Point unmapPoint(QPointF point) const;
  [[nodiscard]] sketch::Point snappedPoint(QPointF point) const;
  [[nodiscard]] ConstructionSnap constructionSnapAt(QPointF position) const;
  [[nodiscard]] std::optional<std::size_t> referenceEdgeAt(QPointF position) const;
  bool appendProjectedEdge(const RenderEdge& edge, bool recordUndo,
                           bool reportStatus);
  void selectAt(QPointF position, bool additive = false,
                bool preserveExistingIfHit = false);
  void selectInRect(const QRectF& rect, bool additive);
  void clearGeometrySelection();
  [[nodiscard]] bool lineElementSelected(std::size_t elementId) const;
  [[nodiscard]] bool circleSelected(sketch::GeometryId id) const;
  void commitPoint(sketch::Point point);
  void showDimensionEditor(QPoint position);
  void updateDimensionEditor();
  void positionDimensionEditor();
  void commitDimensionEditor();
  void hideDimensionEditor();
  void notifyGeometryChanged();
  void pushUndoState();
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
  void commitAutoDimension();
  [[nodiscard]] bool dimensionSegment(std::size_t index, QPointF& first,
                                      QPointF& second) const;
  [[nodiscard]] QPointF dimensionLabelCenter(std::size_t index,
                                             QPointF first,
                                             QPointF second) const;
  [[nodiscard]] bool beginDimensionLabelDrag(QPointF position);
  [[nodiscard]] bool beginDimensionLineDrag(QPointF position);
  [[nodiscard]] std::optional<std::size_t> dimensionAt(
      QPointF position) const;

  sketch::Sketch sketch_;
  std::vector<sketch::Sketch> undoStack_;
  Tool tool_{Tool::Select};
  SelectionKind selectionKind_{SelectionKind::None};
  sketch::GeometryId selectionCircleId_{sketch::kInvalidGeometryId};
  sketch::GeometryId selectionLineId_{sketch::kInvalidGeometryId};
  std::size_t selectionElementId_{};
  std::vector<std::size_t> selectedElementIds_;
  std::vector<sketch::GeometryId> selectedCircleIds_;
  bool selectionBoxActive_{false};
  QPointF selectionBoxStart_{};
  QPointF selectionBoxCurrent_{};
  bool selectionBoxAdditive_{false};
  std::optional<sketch::Point> anchor_;
  std::optional<sketch::PointReference> coincidentFirstPoint_;
  sketch::Point hoverPoint_{};
  std::optional<ConstructionSnap> constructionHover_;
  sketch::Point dragPoint_{};
  bool dragging_{false};
  QDoubleSpinBox* primaryDimension_{nullptr};
  QDoubleSpinBox* secondaryDimension_{nullptr};
  double pixelsPerMm_{5.0};
  double snapStepMm_{5.0};
  bool snapEnabled_{true};
  bool gridVisible_{true};
  std::optional<std::size_t> hoveredProjectionEdge_;
  int viewQuarterTurns_{0};
  CircleMode circleMode_{CircleMode::CenterRadius};
  double circleDiameterMm_{20.0};
  std::vector<sketch::Point> circlePoints_;
  std::vector<sketch::Point> arcPoints_;
  std::vector<sketch::Line> circleGuideLines_;
  RectangleMode rectangleMode_{RectangleMode::TwoPoints};
  std::vector<sketch::Point> rectanglePoints_;
  BoxParameters referenceBox_{};
  QString referenceSupport_;
  bool referenceBodyVisible_{false};
  BodyRenderMesh referenceBodyMesh_;
  BodyRenderMesh referenceFaceMesh_;
  SketchPlacement referencePlacement_{SketchPlacement::xy()};
  bool realReferenceBodyVisible_{false};
  sketch::Sketch referenceProfile_;
  bool referenceProfileVisible_{false};
};

}  // namespace solidar
