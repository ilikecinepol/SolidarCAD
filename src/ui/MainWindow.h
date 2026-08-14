#pragma once

#include <QMainWindow>
#include <optional>

#include "model/Document.h"

class QTreeWidget;
class QStackedWidget;
class QAction;

namespace solidar {

class Viewport;
class SketchCanvas;
class DrawingSheetView;
class SketchRibbon;
class ModelRibbon;

class MainWindow final : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  void setProjectPath(const QString& path);

 private:
  void buildUi();
  void buildMenus();
  void createProject();
  void openProject();
  void saveProject();
  void updateFromSketch(double widthMm, double heightMm);
  void finishSketch();
  void extrudeSketch();
  void rebuildFeatureTree();
  void exportPdf();
  void printDrawing();

  Document document_;
  Viewport* viewport_{nullptr};
  SketchCanvas* sketchCanvas_{nullptr};
  DrawingSheetView* drawingSheet_{nullptr};
  SketchRibbon* sketchRibbon_{nullptr};
  ModelRibbon* modelRibbon_{nullptr};
  QStackedWidget* ribbonStack_{nullptr};
  QStackedWidget* workspaceStack_{nullptr};
  QTreeWidget* featureTree_{nullptr};
  bool hasExtrusion_{false};
  std::size_t sketchCount_{0};
  std::optional<std::size_t> extrusionSourceSketch_;
  QString selectedExtrusionSurface_;
  QString currentSketchSupport_{QStringLiteral("XY")};
  QAction* undoAction_{nullptr};
};

}  // namespace solidar
