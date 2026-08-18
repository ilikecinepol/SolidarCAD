#pragma once

#include <QPoint>
#include <QWidget>

#include <optional>
#include <vector>

#include "sketch/Sketch.h"
#include "model/Document.h"

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QDoubleSpinBox;
class QEvent;

namespace solidar {

class SketchCanvas final : public QWidget {
  Q_OBJECT

 public:
  enum class Tool { Select, Line, Rectangle, Circle, AutoDimension };
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
  void setReferenceBody(BoxParameters box, const QString& support, bool visible);
  void setReferenceProfile(const sketch::Sketch& profile, bool visible);
  [[nodiscard]] bool canUndo() const noexcept;
  [[nodiscard]] Tool tool() const noexcept;
  [[nodiscard]] const sketch::Sketch& sketch() const noexcept;

signals:
  void geometryChanged(double widthMm, double heightMm);
  void selectionChanged(const QString& description);
  void toolChanged(Tool tool);
  void undoAvailable(bool available);
  void primaryDimensionChanged(double value);
  void lineStyleSelectionChanged(bool lineSelected, bool dashed);

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

  [[nodiscard]] QPointF mapPoint(sketch::Point point) const;
  [[nodiscard]] sketch::Point unmapPoint(QPointF point) const;
  [[nodiscard]] sketch::Point snappedPoint(QPointF point) const;
  void selectAt(QPointF position);
  void commitPoint(sketch::Point point);
  void showDimensionEditor(QPoint position);
  void updateDimensionEditor();
  void positionDimensionEditor();
  void commitDimensionEditor();
  void hideDimensionEditor();
  void notifyGeometryChanged();
  void pushUndoState();
  void commitCirclePoint(sketch::Point point);
  void commitRectanglePoint(sketch::Point point);
  void handleAutoDimensionClick(QPointF position);
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
  std::size_t selectionElementId_{};
  std::optional<sketch::Point> anchor_;
  sketch::Point hoverPoint_{};
  sketch::Point dragPoint_{};
  bool dragging_{false};
  QDoubleSpinBox* primaryDimension_{nullptr};
  QDoubleSpinBox* secondaryDimension_{nullptr};
  double pixelsPerMm_{5.0};
  double snapStepMm_{5.0};
  bool snapEnabled_{true};
  bool gridVisible_{true};
  CircleMode circleMode_{CircleMode::CenterRadius};
  double circleDiameterMm_{20.0};
  std::vector<sketch::Point> circlePoints_;
  std::vector<sketch::Line> circleGuideLines_;
  RectangleMode rectangleMode_{RectangleMode::TwoPoints};
  std::vector<sketch::Point> rectanglePoints_;
  BoxParameters referenceBox_{};
  QString referenceSupport_;
  bool referenceBodyVisible_{false};
  sketch::Sketch referenceProfile_;
  bool referenceProfileVisible_{false};
};

}  // namespace solidar
