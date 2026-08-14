#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPoint>
#include <QPolygonF>
#include <QString>
#include <vector>

#include "model/Document.h"
#include "sketch/Sketch.h"

class QMouseEvent;
class QWheelEvent;

namespace solidar {

class Viewport final : public QOpenGLWidget, protected QOpenGLFunctions {
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
  void setSketchVisible(std::size_t index, bool visible);
  void setOriginVisible(bool visible);
  void setBasePlaneVisible(int plane, bool visible);
  void resetScene();
  void beginSketchPlaneSelection();
  void beginExtrusionSurfaceSelection();
  [[nodiscard]] bool hasSelectedFace() const noexcept;
  [[nodiscard]] QString selectedFaceName() const;
  [[nodiscard]] const sketch::Sketch& extrusionCandidateSketch() const noexcept;
  [[nodiscard]] QString extrusionCandidateSupport() const;
  [[nodiscard]] std::size_t extrusionCandidateSketchIndex() const noexcept;
  [[nodiscard]] const sketch::Sketch& solidSketch() const noexcept;
  [[nodiscard]] QString solidSupport() const;

 signals:
  void selectionChanged(const QString& description);
  void sketchPlanePicked(const QString& planeName);
  void extrusionSurfacePicked(const QString& surfaceName);

 protected:
  void initializeGL() override;
  void resizeGL(int width, int height) override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

 private:
  void updateExtrusionHover(QPointF position);
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
  QPolygonF extrusionHoverPolygon_;
  sketch::Sketch hoveredExtrusionSketch_;
  sketch::Sketch selectedExtrusionSketch_;
  QString hoveredExtrusionSupport_;
  QString selectedExtrusionSupport_;
  QString hoveredExtrusionSurface_;
  std::size_t hoveredExtrusionSketchIndex_{static_cast<std::size_t>(-1)};
  std::size_t selectedExtrusionSketchIndex_{static_cast<std::size_t>(-1)};
  bool draggingBody_{false};
  float offsetX_{0.0F};
  float offsetY_{0.0F};
  QPoint lastMousePosition_;
  float yaw_{-35.0F};
  float pitch_{25.0F};
  float zoom_{1.0F};
};

}  // namespace solidar
