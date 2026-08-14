#pragma once

#include <QWidget>

#include "sketch/Sketch.h"

class QPaintEvent;

namespace solidar {

class DrawingSheetView final : public QWidget {
  Q_OBJECT

 public:
  explicit DrawingSheetView(QWidget* parent = nullptr);
  void setRectangle(double widthMm, double heightMm);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  sketch::Sketch sketch_;
};

}  // namespace solidar
