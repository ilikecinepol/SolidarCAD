#pragma once

#include <QWidget>

class QButtonGroup;

namespace solidar {

class ModelRibbon final : public QWidget {
  Q_OBJECT

 public:
  explicit ModelRibbon(QWidget* parent = nullptr);
  void clearActiveTool();

 signals:
  void createSketchRequested();
  void extrudeRequested();
  void pocketRequested();
  void filletRequested();
  void fitRequested();
  void isoRequested();

 private:
  QButtonGroup* toolGroup_{nullptr};
};

}  // namespace solidar
