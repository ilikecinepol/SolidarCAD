#include <QApplication>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QPixmap>

#include <algorithm>
#include <cmath>
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

  width->setValue(36.0);
  height->setValue(24.0);
  canvas.commitCurrentDimension();

  CHECK(canvas.sketch().lines().size() == 4);
  CHECK(canvas.sketch().dimensions().size() == 2);
  CHECK(std::any_of(canvas.sketch().dimensions().begin(),
                    canvas.sketch().dimensions().end(),
                    [](const solidar::sketch::Dimension& dimension) {
                      return dimension.kind ==
                                 solidar::sketch::DimensionKind::PointDistanceX &&
                             std::abs(dimension.valueMm - 36.0) < 1e-9;
                    }));
  CHECK(std::any_of(canvas.sketch().dimensions().begin(),
                    canvas.sketch().dimensions().end(),
                    [](const solidar::sketch::Dimension& dimension) {
                      return dimension.kind ==
                                 solidar::sketch::DimensionKind::PointDistanceY &&
                             std::abs(dimension.valueMm - 24.0) < 1e-9;
                    }));
  CHECK(std::any_of(canvas.sketch().constraints().begin(),
                    canvas.sketch().constraints().end(),
                    [](const solidar::sketch::Constraint& constraint) {
                      return constraint.type ==
                                 solidar::sketch::ConstraintType::DistanceX &&
                             std::abs(constraint.value - 36.0) < 1e-9;
                    }));
  CHECK(std::any_of(canvas.sketch().constraints().begin(),
                    canvas.sketch().constraints().end(),
                    [](const solidar::sketch::Constraint& constraint) {
                      return constraint.type ==
                                 solidar::sketch::ConstraintType::DistanceY &&
                             std::abs(constraint.value - 24.0) < 1e-9;
                    }));

  solidar::SketchCanvas mouseOnlyCanvas;
  mouseOnlyCanvas.resize(900, 650);
  mouseOnlyCanvas.setTool(solidar::SketchCanvas::Tool::Rectangle);
  mouseOnlyCanvas.setRectangleMode(
      solidar::SketchCanvas::RectangleMode::TwoPoints);
  mouseOnlyCanvas.show();
  QApplication::processEvents();

  QMouseEvent mouseFirst(QEvent::MouseButtonPress, first, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&mouseOnlyCanvas, &mouseFirst);
  QMouseEvent mouseMove(QEvent::MouseMove, opposite, Qt::NoButton,
                        Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&mouseOnlyCanvas, &mouseMove);
  QMouseEvent mouseSecond(QEvent::MouseButtonPress, opposite, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&mouseOnlyCanvas, &mouseSecond);

  CHECK(mouseOnlyCanvas.sketch().lines().size() == 4);
  CHECK(mouseOnlyCanvas.sketch().dimensions().empty());
  CHECK(std::none_of(mouseOnlyCanvas.sketch().constraints().begin(),
                     mouseOnlyCanvas.sketch().constraints().end(),
                     [](const solidar::sketch::Constraint& constraint) {
                       return constraint.type ==
                                  solidar::sketch::ConstraintType::DistanceX ||
                              constraint.type ==
                                  solidar::sketch::ConstraintType::DistanceY;
                     }));
  return EXIT_SUCCESS;
}
