#pragma once

#include <QMainWindow>
#include <optional>
#include <functional>
#include <vector>

#include "model/Document.h"
#include "sketch/Sketch.h"

class QTreeWidget;
class QStackedWidget;
class QAction;
class QHBoxLayout;
class QSlider;
class QDockWidget;
class QDoubleSpinBox;
class QPushButton;
class QComboBox;
class QCheckBox;
class QTreeWidget;

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
  bool loadProject(const QString& path, QString* error = nullptr);

 private:
  void buildUi();
  void buildMenus();
  void createProject();
  void openProject();
  void saveProject();
  void exportStl();
  void updateFromSketch(double widthMm, double heightMm);
  void finishSketch();
  void extrudeSketch();
  void updateAutomaticExtrudeOperation();
  void normalizeExtrusionDistance();
  void createPocket();
  void createFillet();
  void refreshBodyViewFromDocument();
  void rebuildFeatureTree();
  void rebuildHistoryPanel();
  void applyHistoryPosition(int position);
  void editSketchStep(std::size_t index);
  void editExtrusionStep();
  void editPocketStep();
  void editFilletStep();
  void exportPdf();
  void printDrawing();
  void undoLastAction();
  void pushUndoAction(std::function<void()> action);
  void updateUndoAvailability();
  void updateSketchConstraintPanel();
  bool configureSketchEditContext();

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
  SketchPlacement currentSketchPlacement_{SketchPlacement::xy()};
  std::optional<FaceReference> currentSketchFaceReference_;
  QAction* undoAction_{nullptr};
  QWidget* historyContent_{nullptr};
  QHBoxLayout* historyLayout_{nullptr};
  QSlider* historySlider_{nullptr};
  QDockWidget* extrusionDock_{nullptr};
  QDoubleSpinBox* extrusionLengthSpin_{nullptr};
  QComboBox* extrusionOperationCombo_{nullptr};
  QCheckBox* extrusionReverseCheck_{nullptr};
  bool extrudeOperationManuallyChanged_{false};
  QDockWidget* sketchSettingsDock_{nullptr};
  QComboBox* sketchLineTypeCombo_{nullptr};
  QTreeWidget* sketchConstraintsList_{nullptr};
  int historyPosition_{0};
  struct SketchHistoryEntry {
    sketch::Sketch geometry;
    QString support;
    SketchId documentSketchId{kInvalidSketchId};
  };
  std::vector<SketchHistoryEntry> sketchHistory_;
  std::optional<std::size_t> editingSketchIndex_;
  std::vector<std::function<void()>> modelUndoStack_;
  bool applyingUndo_{false};
};

}  // namespace solidar
