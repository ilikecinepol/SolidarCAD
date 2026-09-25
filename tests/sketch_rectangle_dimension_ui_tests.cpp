#include <QApplication>
#include <QDoubleSpinBox>
#include <QDir>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "ui/SketchCanvas.h"
#include "project/ProjectFile.h"
#include "sketch/SketchConstraintDiagnostics.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

#define CHECK(condition) require((condition), #condition)

bool editDimension(solidar::SketchCanvas& canvas, double currentValue,
                   double newValue) {
  auto* editor = canvas.findChild<QDoubleSpinBox*>("primaryDimension");
  if (!editor) return false;
  for (int y = 20; y < canvas.height() - 20; y += 4) {
    for (int x = 20; x < canvas.width() - 20; x += 4) {
      QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(x, y),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(&canvas, &doubleClick);
      if (!editor->isVisible()) continue;
      if (std::abs(editor->value() - currentValue) < 1e-6) {
        editor->setValue(newValue);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(editor, &enter);
        QApplication::processEvents();
        return !editor->isVisible();
      }
      QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      QApplication::sendEvent(editor, &escape);
    }
  }
  return false;
}

double dimensionSpan(const solidar::sketch::Sketch& sketch,
                     solidar::sketch::DimensionKind kind) {
  const auto found = std::find_if(
      sketch.dimensions().begin(), sketch.dimensions().end(),
      [kind](const solidar::sketch::Dimension& item) {
        return item.kind == kind;
      });
  if (found == sketch.dimensions().end()) return -1.0;
  const auto firstIndex = sketch.lineIndex(found->firstPoint.lineId);
  const auto secondIndex = sketch.lineIndex(found->secondPoint.lineId);
  if (!firstIndex || !secondIndex) return -1.0;
  const auto& firstLine = sketch.lines()[*firstIndex];
  const auto& secondLine = sketch.lines()[*secondIndex];
  const auto first = found->firstPoint.start ? firstLine.start : firstLine.end;
  const auto second =
      found->secondPoint.start ? secondLine.start : secondLine.end;
  return kind == solidar::sketch::DimensionKind::PointDistanceX
             ? std::abs(second.xMm - first.xMm)
             : std::abs(second.yMm - first.yMm);
}

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
  CHECK(std::count_if(canvas.sketch().dimensions().begin(),
                      canvas.sketch().dimensions().end(),
                      [](const solidar::sketch::Dimension& dimension) {
                        return dimension.kind ==
                               solidar::sketch::DimensionKind::PointDistanceX;
                      }) == 1);
  CHECK(std::count_if(canvas.sketch().dimensions().begin(),
                      canvas.sketch().dimensions().end(),
                      [](const solidar::sketch::Dimension& dimension) {
                        return dimension.kind ==
                               solidar::sketch::DimensionKind::PointDistanceY;
                      }) == 1);
  CHECK(std::count_if(canvas.sketch().constraints().begin(),
                      canvas.sketch().constraints().end(),
                      [](const solidar::sketch::Constraint& constraint) {
                        return constraint.type ==
                               solidar::sketch::ConstraintType::DistanceX;
                      }) == 1);
  CHECK(std::count_if(canvas.sketch().constraints().begin(),
                      canvas.sketch().constraints().end(),
                      [](const solidar::sketch::Constraint& constraint) {
                        return constraint.type ==
                               solidar::sketch::ConstraintType::DistanceY;
                      }) == 1);
  CHECK(std::count_if(canvas.sketch().constraints().begin(),
                      canvas.sketch().constraints().end(),
                      [](const solidar::sketch::Constraint& constraint) {
                        return constraint.type ==
                               solidar::sketch::ConstraintType::Horizontal;
                      }) == 1);
  CHECK(std::count_if(canvas.sketch().constraints().begin(),
                      canvas.sketch().constraints().end(),
                      [](const solidar::sketch::Constraint& constraint) {
                        return constraint.type ==
                               solidar::sketch::ConstraintType::Vertical;
                      }) == 1);
  for (const auto& dimension : canvas.sketch().dimensions()) {
    CHECK(canvas.sketch().lineIndex(dimension.firstPoint.lineId).has_value());
    CHECK(canvas.sketch().lineIndex(dimension.secondPoint.lineId).has_value());
  }
  CHECK(!solidar::sketch::analyzeConstraintSystem(canvas.sketch()).conflicting);
  CHECK(canvas.canUndo());
  CHECK(!canvas.canRedo());
  canvas.undo();
  CHECK(canvas.sketch().lines().empty());
  CHECK(canvas.sketch().dimensions().empty());
  CHECK(canvas.canRedo());
  canvas.redo();
  CHECK(canvas.sketch().lines().size() == 4);
  CHECK(canvas.sketch().dimensions().size() == 2);
  CHECK(canvas.sketch().constraints().size() >= 2);

  CHECK(editDimension(canvas, 36.0, 42.0));
  CHECK(std::abs(dimensionSpan(
                     canvas.sketch(),
                     solidar::sketch::DimensionKind::PointDistanceX) -
                 42.0) < 1e-6);
  canvas.undo();
  CHECK(std::abs(dimensionSpan(
                     canvas.sketch(),
                     solidar::sketch::DimensionKind::PointDistanceX) -
                 36.0) < 1e-6);
  CHECK(canvas.canRedo());
  canvas.redo();
  CHECK(std::abs(dimensionSpan(
                     canvas.sketch(),
                     solidar::sketch::DimensionKind::PointDistanceX) -
                 42.0) < 1e-6);

  // Persist the actual UI-created sketch, then edit its restored dimension
  // through the same UI path. The annotation must remain driving.
  QTemporaryDir projectDirectory(
      QDir::current().filePath("rectangle-dimension-ui-XXXXXX"));
  CHECK(projectDirectory.isValid());
  solidar::Document document;
  auto& savedSketch = document.addSketch("UI numeric rectangle");
  const auto savedSketchId = savedSketch.id;
  savedSketch.geometry = canvas.sketch();
  const QString projectPath = projectDirectory.filePath("rectangle.solidar");
  QString projectError;
  CHECK(solidar::project::ProjectFile::saveDocument(
      projectPath, document, &projectError));
  solidar::Document restoredDocument;
  CHECK(solidar::project::ProjectFile::loadDocument(
      projectPath, &restoredDocument, &projectError));
  const auto* restoredSketch = restoredDocument.findSketch(savedSketchId);
  CHECK(restoredSketch);
  solidar::SketchCanvas restoredCanvas;
  restoredCanvas.resize(900, 650);
  restoredCanvas.loadSketch(restoredSketch->geometry);
  restoredCanvas.show();
  QApplication::processEvents();
  CHECK(editDimension(restoredCanvas, 42.0, 48.0));
  CHECK(std::abs(dimensionSpan(
                     restoredCanvas.sketch(),
                     solidar::sketch::DimensionKind::PointDistanceX) -
                 48.0) < 1e-6);
  CHECK(!solidar::sketch::analyzeConstraintSystem(restoredCanvas.sketch())
             .conflicting);

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

  // Numeric FromCenter creation has the same persistent virtual center node
  // as mouse creation, in addition to its two driving dimensions.
  solidar::SketchCanvas centeredCanvas;
  centeredCanvas.resize(900, 650);
  centeredCanvas.setTool(solidar::SketchCanvas::Tool::Rectangle);
  centeredCanvas.setRectangleMode(
      solidar::SketchCanvas::RectangleMode::FromCenter);
  centeredCanvas.show();
  QApplication::processEvents();
  QMouseEvent centerPress(QEvent::MouseButtonPress, first, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&centeredCanvas, &centerPress);
  QMouseEvent centerMove(QEvent::MouseMove, opposite, Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&centeredCanvas, &centerMove);
  auto* centeredWidth =
      centeredCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
  auto* centeredHeight =
      centeredCanvas.findChild<QDoubleSpinBox*>("secondaryDimension");
  CHECK(centeredWidth && centeredHeight);
  centeredWidth->setValue(30.0);
  centeredHeight->setValue(18.0);
  centeredCanvas.commitCurrentDimension();
  CHECK(centeredCanvas.sketch().lines().size() == 4);
  CHECK(centeredCanvas.sketch().dimensions().size() == 2);
  CHECK(centeredCanvas.sketch().centerNodeElementIds().size() == 1);

  // Existing projected/reference geometry is not adopted by the two new
  // driving dimensions or moved by their solve.
  solidar::sketch::Sketch withReference;
  withReference.addLine({100.0, 100.0}, {110.0, 100.0});
  const auto referenceId = withReference.lineId(0);
  withReference.setElementDashed(withReference.lines().front().elementId,
                                 true);
  solidar::SketchCanvas referenceCanvas;
  referenceCanvas.resize(900, 650);
  referenceCanvas.loadSketch(withReference);
  referenceCanvas.setTool(solidar::SketchCanvas::Tool::Rectangle);
  referenceCanvas.show();
  QApplication::processEvents();
  QMouseEvent referencePress(QEvent::MouseButtonPress, first, Qt::LeftButton,
                             Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&referenceCanvas, &referencePress);
  QMouseEvent referenceMove(QEvent::MouseMove, opposite, Qt::NoButton,
                            Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&referenceCanvas, &referenceMove);
  auto* referenceWidth =
      referenceCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
  auto* referenceHeight =
      referenceCanvas.findChild<QDoubleSpinBox*>("secondaryDimension");
  CHECK(referenceWidth && referenceHeight);
  referenceWidth->setValue(28.0);
  referenceHeight->setValue(16.0);
  referenceCanvas.commitCurrentDimension();
  const auto referenceIndex = referenceCanvas.sketch().lineIndex(referenceId);
  CHECK(referenceIndex.has_value());
  const auto& preservedReference =
      referenceCanvas.sketch().lines()[*referenceIndex];
  CHECK(preservedReference.dashed);
  CHECK(std::abs(preservedReference.start.xMm - 100.0) < 1e-9);
  CHECK(std::abs(preservedReference.end.xMm - 110.0) < 1e-9);
  for (const auto& dimension : referenceCanvas.sketch().dimensions()) {
    CHECK(dimension.firstPoint.lineId != referenceId);
    CHECK(dimension.secondPoint.lineId != referenceId);
  }
  return EXIT_SUCCESS;
}
