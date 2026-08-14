#pragma once

#include <QWidget>

namespace solidar {

class SketchCanvas;

class SketchRibbon final : public QWidget {
  Q_OBJECT

 public:
  explicit SketchRibbon(SketchCanvas* canvas, QWidget* parent = nullptr);

 signals:
  void finishRequested();
};

}  // namespace solidar
