#pragma once

#include <QPoint>
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
#include <QWidget>
#include <vector>

#include "model/Document.h"
#include "sketch/Sketch.h"

class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QKeyEvent;
class QDoubleSpinBox;

namespace solidar {

class Viewport final : public QWidget {
  Q_OBJECT

 public:
  explicit Viewport(QWidget* parent = nullptr);
  void setBox(BoxParameters parameters);
  void setSketch(const sketch::Sketch& sketch);
  void setSolidSketch(const sketch::Sketch& sketch);
  void setSolidVisible(bool visible);
  void setSolidSupport(const QString& supportName);
  void setSketchVisible(bool visible);
  void addSketch(const sketch::Sketch& sketch, const QString& supportName);
  void updateSketch(std::size_t index, const sketch::Sketch& sketch,
                    const QString& supportName);
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
  [[nodiscard]] const sketch::Sketch& extrusionCandidateSketch() const noexcept;
  [[nodiscard]] QString extrusionCandidateSupport() const;
  [[nodiscard]] std::size_t extrusionCandidateSketchIndex() const noexcept;
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
  enum class PickMode { None, SketchPlane, ExtrusionSurface };
  BoxParameters box_;
  sketch::Sketch sketch_;
  sketch::Sketch solidSketch_;
  bool solidVisible_{false};
  QString solidSupportName_{QStringLiteral("XY")};
  bool sketchVisible_{true};
  struct DisplaySketch {
    sketch::Sketch geometry;
    QString supportName;
    bool visible{true};
  };
  std::vector<DisplaySketch> displaySketches_;
  bool originVisible_{true};
  bool basePlanesVisible_[3]{false, false, false};
  PickMode pickMode_{PickMode::None};
  int selectedFace_{-1};
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
