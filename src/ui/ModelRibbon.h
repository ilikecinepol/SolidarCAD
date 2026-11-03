#pragma once

#include <QWidget>

class QButtonGroup;
class QToolButton;

namespace solidar {

class ModelRibbon final : public QWidget {
  Q_OBJECT

 public:
  explicit ModelRibbon(QWidget* parent = nullptr);
  void clearActiveTool();
  void setRulerActive(bool active);
  void setAuthoringEnabled(bool enabled, bool moveOnly = false);
  void setMeshQualityControlEnabled(bool enabled);
  void setCreateComponentFromBodyEnabled(bool enabled);
  void setComponentHomePlacementEnabled(bool adoptEnabled,
                                        bool restoreEnabled);
  void setComponentEditing(bool active, bool rootComponent = false);

 signals:
  void createSketchRequested();
  void referenceImageRequested();
  void extrudeRequested();
  void revolveRequested();
  void pocketRequested();
  void filletRequested();
  void chamferRequested();
  void joinBodiesRequested();
  void moveRequested();
  void shellRequested();
  void draftRequested();
  void mirrorRequested();
  void linearPatternRequested();
  void circularPatternRequested();
  void createComponentFromBodyRequested();
  void setComponentHomePlacementRequested();
  void restoreComponentHomePlacementRequested();
  void rulerToggled(bool active);
  void displayModeRequested(int mode);
  void meshQualityRequested(int quality);

 private:
  QButtonGroup* toolGroup_{nullptr};
  QToolButton* rulerButton_{nullptr};
  QToolButton* meshQualityButton_{nullptr};
  QToolButton* createComponentFromBodyButton_{nullptr};
  QToolButton* setComponentHomePlacementButton_{nullptr};
  QToolButton* restoreComponentHomePlacementButton_{nullptr};
};

}  // namespace solidar
