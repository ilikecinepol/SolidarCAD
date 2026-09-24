#include <QApplication>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QPixmap>

#include <cstdlib>
#include <iostream>

#include "ui/SketchCanvas.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

#define CHECK(condition) require((condition), #condition)

}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);

  solidar::SketchCanvas canvas;
  canvas.resize(900, 650);
  canvas.setTool(solidar::SketchCanvas::Tool::Rectangle);
  canvas.setRectangleMode(solidar::SketchCanvas::RectangleMode::TwoPoints);
  canvas.show();
  QApplication::processEvents();

  const QPointF first(300.0, 250.0);
  const QPointF opposite(500.0, 390.0);
  QMouseEvent press(QEvent::MouseButtonPress, first, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &press);
  QMouseEvent move(QEvent::MouseMove, opposite, Qt::NoButton, Qt::NoButton,
                   Qt::NoModifier);
  QApplication::sendEvent(&canvas, &move);
  QApplication::processEvents();

  auto* width = canvas.findChild<QDoubleSpinBox*>("primaryDimension");
  auto* height = canvas.findChild<QDoubleSpinBox*>("secondaryDimension");
  CHECK(width != nullptr && height != nullptr);
  CHECK(width->isVisible() && height->isVisible());
  CHECK(width->prefix().isEmpty() && height->prefix().isEmpty());
  CHECK(width->buttonSymbols() == QAbstractSpinBox::NoButtons);
  CHECK(height->buttonSymbols() == QAbstractSpinBox::NoButtons);

  // Width is centred on the horizontal dimension line below the rectangle;
  // height is centred on the vertical dimension line at its right side.
  CHECK(width->geometry().center().x() > first.x());
  CHECK(width->geometry().center().x() < opposite.x());
  CHECK(width->geometry().center().y() > opposite.y());
  CHECK(height->geometry().center().x() > opposite.x());
  CHECK(height->geometry().center().y() > first.y());
  CHECK(height->geometry().center().y() < opposite.y());

  const QPixmap preview = canvas.grab();
  CHECK(!preview.isNull());
  return EXIT_SUCCESS;
}
