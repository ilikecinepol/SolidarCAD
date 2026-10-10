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
  void rulerToggled(bool active);
  void displayModeRequested(int mode);
  void meshQualityRequested(int quality);

 private:
  QButtonGroup* toolGroup_{nullptr};
  QToolButton* rulerButton_{nullptr};
};

}  // namespace solidar
