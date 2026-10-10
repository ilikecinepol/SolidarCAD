#include "TestAssertions.h"

#include <QApplication>
#include <QColor>
#include <QDoubleSpinBox>
#include <QDir>
#include <QKeyEvent>
#include <QImage>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

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

QPointF screenPoint(const solidar::SketchCanvas& canvas,
                    solidar::sketch::Point point) {
  constexpr double rulerLeft = 44.0;
  constexpr double rulerTop = 30.0;
  const double centerX = rulerLeft + (canvas.width() - rulerLeft) * 0.5;
  const double centerY = rulerTop + (canvas.height() - rulerTop) * 0.5;
  return {centerX + point.xMm * 5.0, centerY - point.yMm * 5.0};
}

void click(solidar::SketchCanvas& canvas, QPointF point) {
  QMouseEvent press(QEvent::MouseButtonPress, point, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::LeftButton,
                      Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &release);
}

void moveMouse(solidar::SketchCanvas& canvas, QPointF point) {
  QMouseEvent event(QEvent::MouseMove, point, Qt::NoButton,
                    Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &event);
}

void shortRightClick(solidar::SketchCanvas& canvas, QPointF point) {
  QMouseEvent press(QEvent::MouseButtonPress, point, Qt::RightButton,
                    Qt::RightButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::RightButton,
                      Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &release);
  QApplication::processEvents();
}

void pressEscape(solidar::SketchCanvas& canvas) {
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &escape);
  QApplication::processEvents();
}

void doubleClick(solidar::SketchCanvas& canvas, QPointF point) {
  QMouseEvent event(QEvent::MouseButtonDblClick, point, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &event);
}

bool segmentTouchesRect(QPointF first, QPointF second, const QRectF& rect) {
  if (rect.contains(first) || rect.contains(second)) return true;
  const QLineF segment(first, second);
  const QLineF borders[] = {
      {rect.topLeft(), rect.topRight()},
      {rect.topRight(), rect.bottomRight()},
      {rect.bottomRight(), rect.bottomLeft()},
      {rect.bottomLeft(), rect.topLeft()}};
  QPointF intersection;
  return std::any_of(std::begin(borders), std::end(borders),
                     [&segment, &intersection](const QLineF& border) {
                       return segment.intersects(border, &intersection) ==
                              QLineF::BoundedIntersection;
                     });
}

bool hasScissorsHighlight(solidar::SketchCanvas& canvas, QPointF point) {
  QPixmap pixmap(canvas.size());
  canvas.render(&pixmap);
  const QImage image = pixmap.toImage();
  const QPoint center = point.toPoint();
  for (int y = center.y() - 6; y <= center.y() + 6; ++y) {
    for (int x = center.x() - 6; x <= center.x() + 6; ++x) {
      if (x < 0 || y < 0 || x >= image.width() || y >= image.height())
        continue;
      const QColor color = image.pixelColor(x, y);
      if (color.red() >= 220 && color.green() <= 110 && color.blue() <= 120)
        return true;
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

  // Grid snap is opt-in, while endpoint/midpoint/carrier inference remains
  // active when the grid is disabled.
  {
    solidar::sketch::Sketch geometry;
    geometry.addLine({0.0, 0.0}, {20.0, 0.0});
    const auto carrierId = geometry.lineId(0);
    solidar::SketchCanvas snapCanvas;
    snapCanvas.resize(900, 650);
    snapCanvas.loadSketch(geometry);
    snapCanvas.setSnapEnabled(false);
    snapCanvas.setTool(solidar::SketchCanvas::Tool::Line);
    snapCanvas.show();
    QApplication::processEvents();
    click(snapCanvas, screenPoint(snapCanvas, {10.0, 0.3}));
    click(snapCanvas, screenPoint(snapCanvas, {16.2, 7.4}));
    CHECK(snapCanvas.sketch().lines().size() == 2);
    const auto& created = snapCanvas.sketch().lines().back();
    CHECK(std::abs(created.start.xMm - 10.0) < 1e-8);
    CHECK(std::abs(created.start.yMm) < 1e-8);
    CHECK(std::abs(created.end.xMm - 16.2) < 1e-6);
    CHECK(std::abs(created.end.yMm - 7.4) < 1e-6);
    CHECK(std::any_of(
        snapCanvas.sketch().constraints().begin(),
        snapCanvas.sketch().constraints().end(),
        [carrierId](const solidar::sketch::Constraint& constraint) {
          return constraint.type ==
                     solidar::sketch::ConstraintType::Midpoint &&
                 constraint.firstGeometry == carrierId;
        }));
  }

  // Principal sketch axes are CAD references even when grid snapping is off.
  // Near-origin input creates both axis constraints; an ordinary axis hit
  // creates the matching single constraint. Undo/Redo and project persistence
  // retain the semantic relationship.
  {
    solidar::SketchCanvas axisCanvas;
    axisCanvas.resize(900, 650);
    axisCanvas.setSnapEnabled(false);
    axisCanvas.setTool(solidar::SketchCanvas::Tool::Line);
    axisCanvas.show();
    QApplication::processEvents();
    click(axisCanvas, screenPoint(axisCanvas, {0.8, 0.6}));
    click(axisCanvas, screenPoint(axisCanvas, {14.0, 0.8}));
    CHECK(axisCanvas.sketch().lines().size() == 1);
    const auto lineId = axisCanvas.sketch().lineId(0);
    CHECK(std::abs(axisCanvas.sketch().lines()[0].start.xMm) < 1e-9);
    CHECK(std::abs(axisCanvas.sketch().lines()[0].start.yMm) < 1e-9);
    CHECK(std::abs(axisCanvas.sketch().lines()[0].end.yMm) < 1e-9);
    const auto hasAxisConstraint =
        [&axisCanvas, lineId](solidar::sketch::ConstraintType type,
                             bool start) {
          return std::any_of(
              axisCanvas.sketch().constraints().begin(),
              axisCanvas.sketch().constraints().end(),
              [type, lineId, start](const solidar::sketch::Constraint& item) {
                return item.type == type &&
                       item.secondPoint.lineId == lineId &&
                       item.secondPoint.start == start;
              });
        };
    CHECK(hasAxisConstraint(solidar::sketch::ConstraintType::PointOnXAxis,
                            true));
    CHECK(hasAxisConstraint(solidar::sketch::ConstraintType::PointOnYAxis,
                            true));
    CHECK(hasAxisConstraint(solidar::sketch::ConstraintType::PointOnXAxis,
                            false));
    CHECK(!solidar::sketch::analyzeConstraintSystem(axisCanvas.sketch())
               .conflicting);

    axisCanvas.undo();
    CHECK(axisCanvas.sketch().lines().empty());
    axisCanvas.redo();
    CHECK(axisCanvas.sketch().lines().size() == 1);

    QTemporaryDir axisProjectDirectory(
        QDir::current().filePath("axis-snap-ui-XXXXXX"));
    CHECK(axisProjectDirectory.isValid());
    solidar::Document axisDocument;
    auto& savedAxisSketch = axisDocument.addSketch("Axis snap");
    const auto savedAxisSketchId = savedAxisSketch.id;
    savedAxisSketch.geometry = axisCanvas.sketch();
    const QString axisProjectPath =
        axisProjectDirectory.filePath("axis-snap.solidar");
    QString axisProjectError;
    CHECK(solidar::project::ProjectFile::saveDocument(
        axisProjectPath, axisDocument, &axisProjectError));
    solidar::Document loadedAxisDocument;
    CHECK(solidar::project::ProjectFile::loadDocument(
        axisProjectPath, &loadedAxisDocument, &axisProjectError));
    const auto* loadedAxisSketch =
        loadedAxisDocument.findSketch(savedAxisSketchId);
    CHECK(loadedAxisSketch);
    CHECK(std::count_if(
              loadedAxisSketch->geometry.constraints().begin(),
              loadedAxisSketch->geometry.constraints().end(),
              [](const solidar::sketch::Constraint& item) {
                return item.type ==
                           solidar::sketch::ConstraintType::PointOnXAxis ||
                       item.type ==
                           solidar::sketch::ConstraintType::PointOnYAxis;
              }) == 3);

    solidar::SketchCanvas verticalAxisCanvas;
    verticalAxisCanvas.resize(900, 650);
    verticalAxisCanvas.setSnapEnabled(false);
    verticalAxisCanvas.setTool(solidar::SketchCanvas::Tool::Line);
    verticalAxisCanvas.show();
    QApplication::processEvents();
    click(verticalAxisCanvas,
          screenPoint(verticalAxisCanvas, {10.0, 10.0}));
    click(verticalAxisCanvas,
          screenPoint(verticalAxisCanvas, {0.8, 20.0}));
    CHECK(verticalAxisCanvas.sketch().lines().size() == 1);
    CHECK(std::abs(verticalAxisCanvas.sketch().lines()[0].end.xMm) < 1e-9);
    const auto verticalLineId = verticalAxisCanvas.sketch().lineId(0);
    CHECK(std::any_of(
        verticalAxisCanvas.sketch().constraints().begin(),
        verticalAxisCanvas.sketch().constraints().end(),
        [verticalLineId](const solidar::sketch::Constraint& item) {
          return item.type ==
                     solidar::sketch::ConstraintType::PointOnYAxis &&
                 item.secondPoint.lineId == verticalLineId &&
                 !item.secondPoint.start;
        }));
  }

  // A normal click selects one primitive rectangle side. A double click
  // expands that selection to the complete closed contour.
  {
    solidar::sketch::Sketch rectangle;
    rectangle.addRectangle({-10.0, -10.0}, {10.0, 10.0});
    solidar::SketchCanvas selectionCanvas;
    selectionCanvas.resize(900, 650);
    selectionCanvas.loadSketch(rectangle);
    selectionCanvas.setTool(solidar::SketchCanvas::Tool::Select);
    selectionCanvas.show();
    QApplication::processEvents();
    const QPointF bottom = screenPoint(selectionCanvas, {0.0, -10.0});
    click(selectionCanvas, bottom);
    selectionCanvas.setSelectedDashed(true);
    CHECK(std::count_if(selectionCanvas.sketch().lines().begin(),
                        selectionCanvas.sketch().lines().end(),
                        [](const solidar::sketch::Line& line) {
                          return line.dashed;
                        }) == 1);
    doubleClick(selectionCanvas, bottom);
    selectionCanvas.setSelectedDashed(true);
    CHECK(std::all_of(selectionCanvas.sketch().lines().begin(),
                      selectionCanvas.sketch().lines().end(),
                      [](const solidar::sketch::Line& line) {
                        return line.dashed;
                      }));
  }

  // Sketch mirror uses a double-clicked closed contour followed by one axis
  // click and creates a connected reflected copy in one Undo transaction.
  {
    solidar::sketch::Sketch geometry;
    geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    geometry.addLine({20.0, -10.0}, {20.0, 20.0});
    const auto axisId = geometry.lineId(4);
    geometry.setLineDashedById(axisId, true);
    solidar::SketchCanvas mirrorCanvas;
    mirrorCanvas.resize(900, 650);
    mirrorCanvas.loadSketch(geometry);
    mirrorCanvas.setTool(solidar::SketchCanvas::Tool::Mirror);
    mirrorCanvas.show();
    QApplication::processEvents();
    doubleClick(mirrorCanvas, screenPoint(mirrorCanvas, {5.0, 0.0}));
    click(mirrorCanvas, screenPoint(mirrorCanvas, {20.0, 5.0}));
    CHECK(mirrorCanvas.sketch().lines().size() == 9);
    CHECK(std::count_if(
              mirrorCanvas.sketch().lines().begin(),
              mirrorCanvas.sketch().lines().end(),
              [](const solidar::sketch::Line& line) {
                return line.start.xMm >= 29.999 && line.end.xMm >= 29.999;
              }) >= 2);
    CHECK(mirrorCanvas.canUndo());
    mirrorCanvas.undo();
    CHECK(mirrorCanvas.sketch().lines().size() == 5);
  }

  // One click mirrors just one primitive. Lines, circles and arcs share the
  // same selection stage; the following click must select a distinct line as
  // the symmetry axis.
  {
    solidar::sketch::Sketch geometry;
    geometry.addLine({0.0, 0.0}, {5.0, 0.0});
    geometry.addLine({10.0, -5.0}, {10.0, 5.0});
    solidar::SketchCanvas lineMirrorCanvas;
    lineMirrorCanvas.resize(900, 650);
    lineMirrorCanvas.loadSketch(geometry);
    lineMirrorCanvas.setTool(solidar::SketchCanvas::Tool::Mirror);
    lineMirrorCanvas.show();
    QApplication::processEvents();
    click(lineMirrorCanvas, screenPoint(lineMirrorCanvas, {2.5, 0.0}));
    CHECK(lineMirrorCanvas.sketch().lines().size() == 2);
    click(lineMirrorCanvas, screenPoint(lineMirrorCanvas, {10.0, 2.0}));
    CHECK(lineMirrorCanvas.sketch().lines().size() == 3);
    const auto& reflectedLine = lineMirrorCanvas.sketch().lines().back();
    CHECK(std::abs(reflectedLine.start.xMm - 20.0) < 1e-6);
    CHECK(std::abs(reflectedLine.end.xMm - 15.0) < 1e-6);
    CHECK(std::abs(reflectedLine.start.yMm) < 1e-6);
    CHECK(std::abs(reflectedLine.end.yMm) < 1e-6);
    lineMirrorCanvas.undo();
    CHECK(lineMirrorCanvas.sketch().lines().size() == 2);
    lineMirrorCanvas.redo();
    CHECK(lineMirrorCanvas.sketch().lines().size() == 3);
  }

  {
    solidar::sketch::Sketch geometry;
    geometry.addCircle({2.0, 3.0}, 4.0);
    const auto circleId = geometry.circleId(0);
    geometry.setCircleDashedById(circleId, true);
    geometry.addLine({10.0, -5.0}, {10.0, 8.0});
    solidar::SketchCanvas circleMirrorCanvas;
    circleMirrorCanvas.resize(900, 650);
    circleMirrorCanvas.loadSketch(geometry);
    circleMirrorCanvas.setTool(solidar::SketchCanvas::Tool::Mirror);
    circleMirrorCanvas.show();
    QApplication::processEvents();
    click(circleMirrorCanvas, screenPoint(circleMirrorCanvas, {6.0, 3.0}));
    click(circleMirrorCanvas, screenPoint(circleMirrorCanvas, {10.0, 3.0}));
    CHECK(circleMirrorCanvas.sketch().circles().size() == 2);
    const auto& reflectedCircle = circleMirrorCanvas.sketch().circles().back();
    CHECK(std::abs(reflectedCircle.center.xMm - 18.0) < 1e-6);
    CHECK(std::abs(reflectedCircle.center.yMm - 3.0) < 1e-6);
    CHECK(std::abs(reflectedCircle.radiusMm - 4.0) < 1e-6);
    CHECK(reflectedCircle.dashed);
  }

  // A line and an arc can form one closed contour. Double-clicking either
  // member selects both, and the mirrored copy keeps analytic arc geometry.
  {
    solidar::sketch::Sketch geometry;
    geometry.addLine({0.0, 0.0}, {10.0, 0.0});
    geometry.addArc({5.0, 0.0}, 5.0, 0.0,
                    3.14159265358979323846);
    geometry.addLine({20.0, -5.0}, {20.0, 8.0});
    solidar::SketchCanvas curvedMirrorCanvas;
    curvedMirrorCanvas.resize(900, 650);
    curvedMirrorCanvas.loadSketch(geometry);
    curvedMirrorCanvas.setTool(solidar::SketchCanvas::Tool::Mirror);
    curvedMirrorCanvas.show();
    QApplication::processEvents();
    doubleClick(curvedMirrorCanvas,
                screenPoint(curvedMirrorCanvas, {5.0, 5.0}));
    click(curvedMirrorCanvas, screenPoint(curvedMirrorCanvas, {20.0, 3.0}));
    CHECK(curvedMirrorCanvas.sketch().lines().size() == 3);
    CHECK(curvedMirrorCanvas.sketch().arcs().size() == 2);
    const auto& reflectedArc = curvedMirrorCanvas.sketch().arcs().back();
    CHECK(std::abs(reflectedArc.center.xMm - 35.0) < 1e-6);
    CHECK(std::abs(reflectedArc.center.yMm) < 1e-6);
    CHECK(std::abs(reflectedArc.radiusMm - 5.0) < 1e-6);
    CHECK(std::abs(reflectedArc.sweepAngleRad -
                   3.14159265358979323846) < 1e-6);
    curvedMirrorCanvas.undo();
    CHECK(curvedMirrorCanvas.sketch().lines().size() == 2);
    CHECK(curvedMirrorCanvas.sketch().arcs().size() == 1);
  }

  // Trim removes only the interval under the cursor, bounded by the nearest
  // line intersections, and Undo restores the source line and its relations.
  {
    solidar::sketch::Sketch geometry;
    geometry.addLine({0.0, 0.0}, {30.0, 0.0});
    geometry.addLine({10.0, -10.0}, {10.0, 10.0});
    geometry.addLine({20.0, -10.0}, {20.0, 10.0});
    solidar::SketchCanvas trimCanvas;
    trimCanvas.resize(900, 650);
    trimCanvas.loadSketch(geometry);
    trimCanvas.setTool(solidar::SketchCanvas::Tool::Trim);
    trimCanvas.show();
    QApplication::processEvents();
    click(trimCanvas, screenPoint(trimCanvas, {15.0, 0.0}));
    CHECK(trimCanvas.sketch().lines().size() == 4);
    CHECK(std::none_of(
        trimCanvas.sketch().lines().begin(), trimCanvas.sketch().lines().end(),
        [](const solidar::sketch::Line& line) {
          return std::abs(line.start.yMm) < 1e-8 &&
                 std::abs(line.end.yMm) < 1e-8 &&
                 std::min(line.start.xMm, line.end.xMm) < 10.001 &&
                 std::max(line.start.xMm, line.end.xMm) > 19.999;
        }));
    trimCanvas.undo();
    CHECK(trimCanvas.sketch().lines().size() == 3);
  }

  // Scissors preview and trimming use the same interval calculation for
  // circles. Two carrier lines bound the hovered quadrant; the complement is
  // retained as one analytic Arc and Undo/Redo restores both representations.
  {
    solidar::sketch::Sketch geometry;
    geometry.addCircle({0.0, 0.0}, 20.0);
    geometry.addLine({-30.0, 0.0}, {30.0, 0.0});
    geometry.addLine({0.0, -30.0}, {0.0, 30.0});
    solidar::SketchCanvas circleTrimCanvas;
    circleTrimCanvas.resize(900, 650);
    circleTrimCanvas.loadSketch(geometry);
    circleTrimCanvas.setTool(solidar::SketchCanvas::Tool::Trim);
    circleTrimCanvas.show();
    QApplication::processEvents();
    const double diagonal = 20.0 / std::sqrt(2.0);
    const QPointF quadrant =
        screenPoint(circleTrimCanvas, {diagonal, diagonal});
    // Offscreen Qt may deliver the first synthetic move while the window is
    // still becoming active. Repeat the same idempotent hover once so this
    // assertion tests the scissors state instead of window activation timing.
    for (int attempt = 0;
         attempt < 2 &&
         circleTrimCanvas.cursor().shape() != Qt::PointingHandCursor;
         ++attempt) {
      QMouseEvent hover(QEvent::MouseMove, quadrant, Qt::NoButton,
                        Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(&circleTrimCanvas, &hover);
      QApplication::processEvents();
    }
    CHECK(circleTrimCanvas.cursor().shape() == Qt::PointingHandCursor);
    CHECK(hasScissorsHighlight(circleTrimCanvas, quadrant));
    click(circleTrimCanvas, quadrant);
    CHECK(circleTrimCanvas.sketch().circles().empty());
    CHECK(circleTrimCanvas.sketch().arcs().size() == 1);
    CHECK(std::abs(circleTrimCanvas.sketch().arcs()[0].sweepAngleRad -
                   3.0 * 3.14159265358979323846 / 2.0) < 1e-6);
    circleTrimCanvas.undo();
    CHECK(circleTrimCanvas.sketch().circles().size() == 1);
    CHECK(circleTrimCanvas.sketch().arcs().empty());
    circleTrimCanvas.redo();
    CHECK(circleTrimCanvas.sketch().circles().empty());
    CHECK(circleTrimCanvas.sketch().arcs().size() == 1);
  }

  // An Arc is trimmed between its endpoint and the nearest intersection,
  // remaining analytic rather than being approximated with line segments.
  {
    solidar::sketch::Sketch geometry;
    geometry.addArc({0.0, 0.0}, 20.0, 0.0,
                    3.14159265358979323846);
    geometry.addLine({0.0, -30.0}, {0.0, 30.0});
    solidar::SketchCanvas arcTrimCanvas;
    arcTrimCanvas.resize(900, 650);
    arcTrimCanvas.loadSketch(geometry);
    arcTrimCanvas.setTool(solidar::SketchCanvas::Tool::Trim);
    arcTrimCanvas.show();
    QApplication::processEvents();
    const double diagonal = 20.0 / std::sqrt(2.0);
    const QPointF firstHalf = screenPoint(arcTrimCanvas, {diagonal, diagonal});
    QMouseEvent hover(QEvent::MouseMove, firstHalf, Qt::NoButton,
                      Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&arcTrimCanvas, &hover);
    QApplication::processEvents();
    CHECK(hasScissorsHighlight(arcTrimCanvas, firstHalf));
    click(arcTrimCanvas, firstHalf);
    CHECK(arcTrimCanvas.sketch().arcs().size() == 1);
    CHECK(std::abs(arcTrimCanvas.sketch().arcs()[0].startAngleRad -
                   3.14159265358979323846 / 2.0) < 1e-6);
    CHECK(std::abs(arcTrimCanvas.sketch().arcs()[0].sweepAngleRad -
                   3.14159265358979323846 / 2.0) < 1e-6);
  }

  // Numeric line HUD chooses a free side instead of covering the preview or
  // an existing carrier at the intended endpoint.
  {
    // AutoDimension accepts either selection order for datum axes. A
    // point-to-X-axis dimension is a normal driving DistanceY constraint and
    // therefore moves the complete rectangle, supports Undo/Redo and survives
    // the project round-trip.
    solidar::sketch::Sketch geometry;
    geometry.addRectangle({15.0, 12.0}, {35.0, 22.0});
    solidar::SketchCanvas datumCanvas;
    datumCanvas.resize(900, 650);
    datumCanvas.loadSketch(geometry);
    datumCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    datumCanvas.show();
    QApplication::processEvents();

    click(datumCanvas, screenPoint(datumCanvas, {15.0, 12.0}));
    click(datumCanvas, screenPoint(datumCanvas, {40.0, 0.0}));
    auto* datumEditor =
        datumCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
    CHECK(datumEditor && datumEditor->isVisible());
    CHECK(std::abs(datumEditor->value() - 12.0) < 1e-6);
    datumEditor->setValue(20.0);
    QKeyEvent datumEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(datumEditor, &datumEnter);
    QApplication::processEvents();
    CHECK(!datumCanvas.sketch().dimensions().empty());
    const auto& axisDimension = datumCanvas.sketch().dimensions().back();
    CHECK(axisDimension.kind ==
          solidar::sketch::DimensionKind::PointDistanceY);
    CHECK(axisDimension.firstPoint.origin);
    CHECK(std::all_of(
        datumCanvas.sketch().lines().begin(),
        datumCanvas.sketch().lines().end(),
        [](const solidar::sketch::Line& line) {
          return line.start.yMm >= 19.999 && line.end.yMm >= 19.999;
        }));
    const auto rectangleExtent = [](const solidar::sketch::Sketch& sketch) {
      double minimumX = std::numeric_limits<double>::infinity();
      double maximumX = -std::numeric_limits<double>::infinity();
      double minimumY = std::numeric_limits<double>::infinity();
      double maximumY = -std::numeric_limits<double>::infinity();
      for (const auto& line : sketch.lines()) {
        minimumX = std::min({minimumX, line.start.xMm, line.end.xMm});
        maximumX = std::max({maximumX, line.start.xMm, line.end.xMm});
        minimumY = std::min({minimumY, line.start.yMm, line.end.yMm});
        maximumY = std::max({maximumY, line.start.yMm, line.end.yMm});
      }
      return std::array<double, 4>{minimumX, maximumX, minimumY, maximumY};
    };
    const auto afterHorizontalAxis = rectangleExtent(datumCanvas.sketch());
    CHECK(std::abs((afterHorizontalAxis[1] - afterHorizontalAxis[0]) - 20.0) <
          1e-6);
    CHECK(std::abs((afterHorizontalAxis[3] - afterHorizontalAxis[2]) - 10.0) <
          1e-6);
    CHECK(std::any_of(
        datumCanvas.sketch().constraints().begin(),
        datumCanvas.sketch().constraints().end(),
        [](const solidar::sketch::Constraint& constraint) {
          return constraint.type ==
                     solidar::sketch::ConstraintType::DistanceY &&
                 constraint.firstPoint.origin &&
                 std::abs(constraint.value - 20.0) < 1e-9;
        }));
    datumCanvas.undo();
    CHECK(std::abs(datumCanvas.sketch().lines()[0].start.yMm - 12.0) < 1e-6);
    datumCanvas.redo();
    CHECK(std::abs(datumCanvas.sketch().lines()[0].start.yMm - 20.0) < 1e-6);

    datumCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    click(datumCanvas, screenPoint(datumCanvas, {0.0, 40.0}));
    click(datumCanvas, screenPoint(datumCanvas, {15.0, 20.0}));
    CHECK(datumEditor->isVisible());
    CHECK(std::abs(datumEditor->value() - 15.0) < 1e-6);
    datumEditor->setValue(25.0);
    QKeyEvent yAxisEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(datumEditor, &yAxisEnter);
    QApplication::processEvents();
    CHECK(datumCanvas.sketch().dimensions().size() == 2);
    CHECK(datumCanvas.sketch().dimensions().back().kind ==
          solidar::sketch::DimensionKind::PointDistanceX);
    CHECK(datumCanvas.sketch().dimensions().back().firstPoint.origin);
    CHECK(std::all_of(
        datumCanvas.sketch().lines().begin(),
        datumCanvas.sketch().lines().end(),
        [](const solidar::sketch::Line& line) {
          return line.start.xMm >= 24.999 && line.end.xMm >= 24.999;
        }));
    const auto afterVerticalAxis = rectangleExtent(datumCanvas.sketch());
    CHECK(std::abs(afterVerticalAxis[0] - 25.0) < 1e-6);
    CHECK(std::abs(afterVerticalAxis[1] - 45.0) < 1e-6);
    CHECK(std::abs(afterVerticalAxis[2] - 20.0) < 1e-6);
    CHECK(std::abs(afterVerticalAxis[3] - 30.0) < 1e-6);

    QTemporaryDir datumDirectory(
        QDir::current().filePath("datum-dimension-ui-XXXXXX"));
    CHECK(datumDirectory.isValid());
    solidar::Document datumDocument;
    datumDocument.addSketch("Datum dimension").geometry = datumCanvas.sketch();
    const QString datumPath = datumDirectory.filePath("datum.solidar");
    QString datumError;
    CHECK(solidar::project::ProjectFile::saveDocument(
        datumPath, datumDocument, &datumError));
    solidar::Document restoredDatumDocument;
    CHECK(solidar::project::ProjectFile::loadDocument(
        datumPath, &restoredDatumDocument, &datumError));
    CHECK(restoredDatumDocument.sketches().size() == 1);
    const auto& restoredDatum =
        restoredDatumDocument.sketches().front().geometry;
    CHECK(restoredDatum.dimensions().size() == 2);
    CHECK(restoredDatum.dimensions()[0].firstPoint.origin);
    CHECK(restoredDatum.dimensions()[1].firstPoint.origin);
    CHECK(std::any_of(
        restoredDatum.constraints().begin(), restoredDatum.constraints().end(),
        [](const solidar::sketch::Constraint& constraint) {
          return constraint.type ==
                     solidar::sketch::ConstraintType::DistanceY &&
                 constraint.firstPoint.origin;
        }));

    // The origin itself is also selectable first. Without an axis lock, the
    // initial dimension is radial and uses the same normal AutoDimension UI.
    solidar::sketch::Sketch radialGeometry;
    radialGeometry.addLine({3.0, 4.0}, {8.0, 4.0});
    // A real endpoint overlaps the datum exactly. Origin priority must be
    // explicit rather than inherited from adapter append order.
    radialGeometry.addLine({0.0, 0.0}, {-10.0, 0.0});
    solidar::SketchCanvas radialCanvas;
    radialCanvas.resize(900, 650);
    radialCanvas.loadSketch(radialGeometry);
    radialCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    radialCanvas.show();
    QApplication::processEvents();
    click(radialCanvas, screenPoint(radialCanvas, {0.0, 0.0}));
    click(radialCanvas, screenPoint(radialCanvas, {3.0, 4.0}));
    auto* radialEditor =
        radialCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
    CHECK(radialEditor && radialEditor->isVisible());
    CHECK(std::abs(radialEditor->value() - 5.0) < 1e-6);
    radialEditor->setValue(10.0);
    QKeyEvent radialEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(radialEditor, &radialEnter);
    QApplication::processEvents();
    CHECK(radialCanvas.sketch().dimensions().size() == 1);
    CHECK(radialCanvas.sketch().dimensions()[0].kind ==
          solidar::sketch::DimensionKind::PointDistance);
    CHECK(radialCanvas.sketch().dimensions()[0].firstPoint.origin);
    const auto moved = radialCanvas.sketch().lines()[0].start;
    CHECK(std::abs(std::hypot(moved.xMm, moved.yMm) - 10.0) < 1e-6);
  }

  {
    solidar::sketch::Sketch geometry;
    geometry.addLine({20.0, -30.0}, {20.0, 30.0});
    solidar::SketchCanvas hudCanvas;
    hudCanvas.resize(900, 650);
    hudCanvas.loadSketch(geometry);
    hudCanvas.setTool(solidar::SketchCanvas::Tool::Line);
    hudCanvas.show();
    QApplication::processEvents();
    const QPointF start = screenPoint(hudCanvas, {0.0, 0.0});
    const QPointF tip = screenPoint(hudCanvas, {20.0, 0.0});
    click(hudCanvas, start);
    QMouseEvent move(QEvent::MouseMove, tip, Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&hudCanvas, &move);
    QApplication::processEvents();
    auto* primary = hudCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
    auto* secondary =
        hudCanvas.findChild<QDoubleSpinBox*>("secondaryDimension");
    CHECK(primary && secondary && primary->isVisible() && secondary->isVisible());
    const QRectF editorBounds =
        QRectF(primary->geometry()).united(QRectF(secondary->geometry()));
    CHECK(!segmentTouchesRect(start, tip, editorBounds.adjusted(-3, -3, 3, 3)));
    CHECK(!segmentTouchesRect(screenPoint(hudCanvas, {20.0, -30.0}),
                              screenPoint(hudCanvas, {20.0, 30.0}),
                              editorBounds.adjusted(-3, -3, 3, 3)));
  }

  // Editing a dimension must retain the original constraint identity. Its ID
  // is the chronological priority: deleting and re-adding it would move the
  // dimension behind constraints created later.
  {
    solidar::sketch::Sketch geometry;
    geometry.addLine({0.0, 0.0}, {20.0, 0.0});
    const auto lineId = geometry.lineId(0);

    solidar::sketch::Constraint length;
    length.type = solidar::sketch::ConstraintType::Length;
    length.firstGeometry = lineId;
    length.value = 20.0;
    const auto lengthId = geometry.addConstraint(length);
    CHECK(lengthId != solidar::sketch::kInvalidConstraintId);

    solidar::sketch::Constraint horizontal;
    horizontal.type = solidar::sketch::ConstraintType::Horizontal;
    horizontal.firstGeometry = lineId;
    const auto horizontalId = geometry.addConstraint(horizontal);
    CHECK(horizontalId != solidar::sketch::kInvalidConstraintId);
    CHECK(lengthId < horizontalId);

    solidar::sketch::Dimension displayed;
    displayed.kind = solidar::sketch::DimensionKind::LineLength;
    displayed.geometryId = lineId;
    displayed.valueMm = 20.0;
    geometry.storeDimension(displayed);

    solidar::SketchCanvas chronologicalCanvas;
    chronologicalCanvas.resize(900, 650);
    chronologicalCanvas.loadSketch(geometry);
    chronologicalCanvas.show();
    QApplication::processEvents();

    CHECK(editDimension(chronologicalCanvas, 20.0, 30.0));
    CHECK(chronologicalCanvas.sketch().constraints().size() == 2);
    CHECK(chronologicalCanvas.sketch().constraints()[0].id == lengthId);
    CHECK(chronologicalCanvas.sketch().constraints()[1].id == horizontalId);
    const auto preserved = std::find_if(
        chronologicalCanvas.sketch().constraints().begin(),
        chronologicalCanvas.sketch().constraints().end(),
        [lengthId](const solidar::sketch::Constraint& constraint) {
          return constraint.id == lengthId;
        });
    CHECK(preserved != chronologicalCanvas.sketch().constraints().end());
    CHECK(std::abs(preserved->value - 30.0) < 1e-9);
  }

  // A locked/coincident endpoint is an anchor, not a reason to reject every
  // later size. The free opposite endpoint must move while the earlier
  // lock/coincidence chain and its stable IDs remain intact.
  {
    solidar::sketch::Sketch geometry;
    geometry.addLine({0.0, 0.0}, {20.0, 0.0});
    geometry.addLine({0.0, 0.0}, {20.0, 0.0});
    const auto anchorId = geometry.lineId(0);
    const auto sizedId = geometry.lineId(1);

    solidar::sketch::Constraint lock;
    lock.type = solidar::sketch::ConstraintType::Lock;
    lock.firstGeometry = anchorId;
    CHECK(geometry.addConstraint(lock) !=
          solidar::sketch::kInvalidConstraintId);

    solidar::sketch::Constraint coincident;
    coincident.type = solidar::sketch::ConstraintType::Coincident;
    coincident.firstPoint = {anchorId, false};
    coincident.secondPoint = {sizedId, false};
    CHECK(geometry.addConstraint(coincident) !=
          solidar::sketch::kInvalidConstraintId);

    solidar::sketch::Constraint length;
    length.type = solidar::sketch::ConstraintType::Length;
    length.firstGeometry = sizedId;
    length.value = 20.0;
    const auto lengthId = geometry.addConstraint(length);
    CHECK(lengthId != solidar::sketch::kInvalidConstraintId);

    solidar::sketch::Dimension displayed;
    displayed.kind = solidar::sketch::DimensionKind::LineLength;
    displayed.geometryId = sizedId;
    displayed.valueMm = 20.0;
    geometry.storeDimension(displayed);

    const auto originalLines = geometry.lines();
    const auto originalConstraints = geometry.constraints();
    solidar::SketchCanvas conflictCanvas;
    conflictCanvas.resize(900, 650);
    conflictCanvas.loadSketch(geometry);
    conflictCanvas.show();
    QApplication::processEvents();

    CHECK(editDimension(conflictCanvas, 20.0, 30.0));
    CHECK(conflictCanvas.sketch().constraints().size() ==
          originalConstraints.size());
    for (std::size_t index = 0; index < originalConstraints.size(); ++index) {
      CHECK(conflictCanvas.sketch().constraints()[index].id ==
            originalConstraints[index].id);
      CHECK(conflictCanvas.sketch().constraints()[index].type ==
            originalConstraints[index].type);
    }
    CHECK(conflictCanvas.sketch().lines().size() == originalLines.size());
    const auto& anchor = conflictCanvas.sketch().lines()[0];
    const auto& sized = conflictCanvas.sketch().lines()[1];
    CHECK(std::abs(anchor.start.xMm - originalLines[0].start.xMm) < 1e-9);
    CHECK(std::abs(anchor.start.yMm - originalLines[0].start.yMm) < 1e-9);
    CHECK(std::abs(anchor.end.xMm - originalLines[0].end.xMm) < 1e-9);
    CHECK(std::abs(anchor.end.yMm - originalLines[0].end.yMm) < 1e-9);
    CHECK(std::abs(sized.end.xMm - anchor.end.xMm) < 1e-9);
    CHECK(std::abs(sized.end.yMm - anchor.end.yMm) < 1e-9);
    CHECK(std::abs(std::hypot(sized.end.xMm - sized.start.xMm,
                              sized.end.yMm - sized.start.yMm) -
                   30.0) < 1e-6);
    CHECK(conflictCanvas.sketch().dimensions().size() == 1);
    CHECK(std::abs(conflictCanvas.sketch().dimensions().front().valueMm -
                   30.0) < 1e-9);
    CHECK(!solidar::sketch::analyzeConstraintSystem(conflictCanvas.sketch())
               .conflicting);
  }

  // A direct line size added after Tangent + Perpendicular must consume the
  // circle centre's remaining freedom along its datum axis. The size editor
  // must close normally instead of reporting a false overconstraint.
  {
    solidar::sketch::Sketch geometry;
    geometry.addLine({-50.0, -25.0}, {20.0, -25.0});
    geometry.addLine({0.0, -50.0}, {0.0, 50.0});
    geometry.addCircle({0.0, 8.0}, 10.0);
    geometry.addLine({-10.0, -25.0}, {-10.0, 8.0});
    const auto projection = geometry.lineId(0);
    const auto verticalProjection = geometry.lineId(1);
    const auto circle = geometry.circleId(0);
    const auto line = geometry.lineId(2);

    solidar::sketch::Constraint constraint;
    constraint.type = solidar::sketch::ConstraintType::Lock;
    constraint.firstGeometry = projection;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::Lock;
    constraint.firstGeometry = verticalProjection;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnLine;
    constraint.firstGeometry = verticalProjection;
    constraint.secondPoint.circleId = circle;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnYAxis;
    constraint.secondPoint.circleId = circle;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnLine;
    constraint.firstGeometry = projection;
    constraint.secondPoint = {line, true};
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnCircle;
    constraint.firstGeometry = circle;
    constraint.secondPoint = {line, false};
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::Tangent;
    constraint.firstGeometry = line;
    constraint.secondGeometry = circle;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::Perpendicular;
    constraint.firstGeometry = projection;
    constraint.secondGeometry = line;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);

    solidar::SketchCanvas dimensionCanvas;
    dimensionCanvas.resize(900, 650);
    dimensionCanvas.loadSketch(geometry);
    dimensionCanvas.show();
    QApplication::processEvents();
    dimensionCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    const auto before = dimensionCanvas.sketch().lines()[2];
    click(dimensionCanvas,
          screenPoint(dimensionCanvas,
                      {(before.start.xMm + before.end.xMm) * 0.5,
                       (before.start.yMm + before.end.yMm) * 0.5}));
    QApplication::processEvents();
    auto* editor =
        dimensionCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
    CHECK(editor && editor->isVisible());
    editor->setValue(25.0);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &enter);
    QApplication::processEvents();

    CHECK(!editor->isVisible());
    CHECK(dimensionCanvas.sketch().dimensions().size() == 1);
    const auto& sized = dimensionCanvas.sketch().lines()[2];
    CHECK(std::abs(std::hypot(sized.end.xMm - sized.start.xMm,
                              sized.end.yMm - sized.start.yMm) -
                   25.0) < 1e-6);
    CHECK(std::abs(dimensionCanvas.sketch().circles()[0].center.xMm) < 1e-7);
    CHECK(std::abs(dimensionCanvas.sketch().circles()[0].center.yMm) < 1e-6);
    CHECK(!solidar::sketch::analyzeConstraintSystem(
               dimensionCanvas.sketch()).conflicting);
  }

  // The direct-size UI must use the same solution when the line was drawn
  // from the upper projected carrier down to the Circle tangent point. This
  // is the orientation produced by the face sketch in the reported case.
  {
    solidar::sketch::Sketch geometry;
    geometry.addLine({-50.0, 40.0}, {50.0, 40.0});
    geometry.addLine({0.0, -50.0}, {0.0, 50.0});
    geometry.addCircle({0.0, 0.0}, 10.0);
    geometry.addLine({10.0, 40.0}, {10.0, 0.0});
    const auto projection = geometry.lineId(0);
    const auto verticalProjection = geometry.lineId(1);
    const auto circle = geometry.circleId(0);
    const auto line = geometry.lineId(2);

    solidar::sketch::Constraint constraint;
    constraint.type = solidar::sketch::ConstraintType::Lock;
    constraint.firstGeometry = projection;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::Lock;
    constraint.firstGeometry = verticalProjection;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnLine;
    constraint.firstGeometry = verticalProjection;
    constraint.secondPoint.circleId = circle;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnYAxis;
    constraint.secondPoint.circleId = circle;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnLine;
    constraint.firstGeometry = projection;
    constraint.secondPoint = {line, true};
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnCircle;
    constraint.firstGeometry = circle;
    constraint.secondPoint = {line, false};
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::Tangent;
    constraint.firstGeometry = line;
    constraint.secondGeometry = circle;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::Perpendicular;
    constraint.firstGeometry = projection;
    constraint.secondGeometry = line;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);

    solidar::SketchCanvas dimensionCanvas;
    dimensionCanvas.resize(900, 650);
    dimensionCanvas.loadSketch(geometry);
    dimensionCanvas.show();
    QApplication::processEvents();
    dimensionCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    const auto before = dimensionCanvas.sketch().lines()[2];
    click(dimensionCanvas,
          screenPoint(dimensionCanvas,
                      {(before.start.xMm + before.end.xMm) * 0.5,
                       (before.start.yMm + before.end.yMm) * 0.5}));
    QApplication::processEvents();
    auto* editor =
        dimensionCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
    CHECK(editor && editor->isVisible());
    editor->setValue(25.0);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &enter);
    QApplication::processEvents();

    CHECK(!editor->isVisible());
    CHECK(dimensionCanvas.sketch().dimensions().size() == 1);
    const auto& sized = dimensionCanvas.sketch().lines()[2];
    CHECK(std::abs(std::hypot(sized.end.xMm - sized.start.xMm,
                              sized.end.yMm - sized.start.yMm) -
                   25.0) < 1e-6);
    CHECK(std::abs(dimensionCanvas.sketch().circles()[0].center.xMm) < 1e-7);
    CHECK(std::abs(dimensionCanvas.sketch().circles()[0].center.yMm - 15.0) <
          1e-6);
    CHECK(!solidar::sketch::analyzeConstraintSystem(
               dimensionCanvas.sketch()).conflicting);

    // If the Circle centre is additionally fixed on X, no degree of freedom
    // remains for changing this vertical tangent length. Keep every older
    // relation and report the fully determined state explicitly.
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnXAxis;
    constraint.secondPoint.circleId = circle;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);

    solidar::SketchCanvas blockedCanvas;
    blockedCanvas.resize(900, 650);
    blockedCanvas.loadSketch(geometry);
    blockedCanvas.show();
    QApplication::processEvents();
    QString blockedStatus;
    QObject::connect(&blockedCanvas,
                     &solidar::SketchCanvas::constraintStatusChanged,
                     &blockedCanvas,
                     [&](const QString& status) { blockedStatus = status; });
    blockedCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    const auto blockedBefore = blockedCanvas.sketch().lines()[2];
    click(blockedCanvas,
          screenPoint(blockedCanvas,
                      {(blockedBefore.start.xMm + blockedBefore.end.xMm) * 0.5,
                       (blockedBefore.start.yMm + blockedBefore.end.yMm) *
                           0.5}));
    QApplication::processEvents();
    auto* blockedEditor =
        blockedCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
    CHECK(blockedEditor && blockedEditor->isVisible());
    blockedEditor->setValue(25.0);
    QApplication::sendEvent(blockedEditor, &enter);
    QApplication::processEvents();

    CHECK(blockedEditor->isVisible());
    CHECK(blockedCanvas.sketch().dimensions().empty());
    CHECK(blockedStatus.contains(QString::fromUtf8("по X и Y")));
    const auto& unchanged = blockedCanvas.sketch().lines()[2];
    CHECK(std::abs(std::hypot(unchanged.end.xMm - unchanged.start.xMm,
                              unchanged.end.yMm - unchanged.start.yMm) -
                   40.0) < 1e-6);
  }
  {
    for (const std::size_t removedIndex : {std::size_t{0}, std::size_t{1},
                                           std::size_t{2}}) {
      solidar::sketch::Sketch dimensionSketch;
      dimensionSketch.addLine({0.0, 0.0}, {10.0, 0.0});
      for (int index = 0; index < 3; ++index) {
        solidar::sketch::Dimension dimension;
        dimension.kind = solidar::sketch::DimensionKind::LineLength;
        dimension.geometryId = dimensionSketch.lineId(0);
        dimension.valueMm = 10.0;
        dimension.offsetMm = 2.0 + index;
        dimension.angleRad = 0.1 * index;
        dimensionSketch.storeDimension(dimension);
      }
      solidar::SketchCanvas deleteCanvas;
      deleteCanvas.resize(900, 650);
      deleteCanvas.loadSketch(dimensionSketch);
      deleteCanvas.show();
      QApplication::processEvents();
      const auto beforeDelete = deleteCanvas.sketch().semanticFingerprint();
      std::vector<solidar::sketch::DimensionId> beforeIds;
      for (const auto& dimension : deleteCanvas.sketch().dimensions())
        beforeIds.push_back(dimension.id);
      CHECK(beforeIds[0] != beforeIds[1]);
      CHECK(beforeIds[1] != beforeIds[2]);

      if (removedIndex == 2) {
        const QPointF origin = screenPoint(deleteCanvas, {0.0, 0.0});
        const double pixelsPerMm =
            std::abs(screenPoint(deleteCanvas, {1.0, 0.0}).x() - origin.x());
        const QPointF laterLabel =
            screenPoint(deleteCanvas, {5.0, 0.0}) +
            QPointF(0.0, (2.0 + 2.0 + 2.0) * pixelsPerMm);
        QMouseEvent labelPress(QEvent::MouseButtonPress, laterLabel,
                               Qt::LeftButton, Qt::LeftButton,
                               Qt::NoModifier);
        QApplication::sendEvent(&deleteCanvas, &labelPress);
        CHECK(deleteCanvas.interactionState().dimension.draggingLabel.has_value());
        CHECK(deleteCanvas.interactionState().dimension.draggingLabel->id ==
              beforeIds[2]);
        QMouseEvent labelRelease(QEvent::MouseButtonRelease, laterLabel,
                                 Qt::LeftButton, Qt::NoButton,
                                 Qt::NoModifier);
        QApplication::sendEvent(&deleteCanvas, &labelRelease);
      }
      deleteCanvas.selectDimension(removedIndex);
      QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
      QApplication::sendEvent(&deleteCanvas, &remove);
      QApplication::processEvents();
      CHECK(deleteCanvas.sketch().dimensions().size() == 2);
      const auto afterDelete = deleteCanvas.sketch().semanticFingerprint();
      CHECK(afterDelete != beforeDelete);
      std::vector<solidar::sketch::DimensionId> remaining;
      for (const auto& dimension : deleteCanvas.sketch().dimensions())
        remaining.push_back(dimension.id);
      auto expected = beforeIds;
      expected.erase(expected.begin() +
                     static_cast<std::ptrdiff_t>(removedIndex));
      CHECK(remaining == expected);
      for (int cycle = 0; cycle < 2; ++cycle) {
        deleteCanvas.undo();
        CHECK(deleteCanvas.sketch().semanticFingerprint() == beforeDelete);
        CHECK(deleteCanvas.sketch().dimensions().size() == 3);
        deleteCanvas.redo();
        CHECK(deleteCanvas.sketch().semanticFingerprint() == afterDelete);
        CHECK(deleteCanvas.sketch().dimensions().size() == 2);
      }
    }

    solidar::Document duplicateDocument;
    auto& duplicateRecord =
        duplicateDocument.addSketch("Duplicate dimensions");
    const auto duplicateSketchId = duplicateRecord.id;
    auto& duplicateSketch = duplicateRecord.geometry;
    duplicateSketch.addLine({0.0, 0.0}, {10.0, 0.0});
    for (int index = 0; index < 2; ++index) {
      solidar::sketch::Dimension dimension;
      dimension.kind = solidar::sketch::DimensionKind::LineLength;
      dimension.geometryId = duplicateSketch.lineId(0);
      dimension.valueMm = 10.0;
      dimension.offsetMm = 2.0 + index;
      dimension.angleRad = 0.1 * index;
      duplicateSketch.storeDimension(dimension);
    }
    const auto firstId = duplicateSketch.dimensions()[0].id;
    const auto secondId = duplicateSketch.dimensions()[1].id;
    QTemporaryDir duplicateDirectory(
        QDir::current().filePath("duplicate-dimension-ui-XXXXXX"));
    CHECK(duplicateDirectory.isValid());
    QString duplicateError;
    const QString duplicatePath =
        duplicateDirectory.filePath("duplicates.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        duplicatePath, duplicateDocument, &duplicateError));
    solidar::Document restoredDuplicates;
    CHECK(solidar::project::ProjectFile::loadDocument(
        duplicatePath, &restoredDuplicates, &duplicateError));
    const auto& restoredDimensions =
        restoredDuplicates.sketches().front().geometry.dimensions();
    CHECK(restoredDimensions.size() == 2);
    CHECK(restoredDimensions[0].id == firstId);
    CHECK(restoredDimensions[1].id == secondId);
    auto* mutableRestored = restoredDuplicates.findSketch(duplicateSketchId);
    CHECK(mutableRestored);
    solidar::sketch::Dimension nextDimension;
    nextDimension.kind = solidar::sketch::DimensionKind::LineLength;
    nextDimension.geometryId = mutableRestored->geometry.lineId(0);
    nextDimension.valueMm = 10.0;
    mutableRestored->geometry.storeDimension(nextDimension);
    CHECK(mutableRestored->geometry.dimensions().size() == 3);
    const auto nextId = mutableRestored->geometry.dimensions().back().id;
    CHECK(nextId != solidar::sketch::kInvalidDimensionId);
    CHECK(nextId != firstId);
    CHECK(nextId != secondId);
  }
  {
    // Tangent guides retain the picker GeometryId. Two geometrically
    // coincident lines with the same composite element id must not be resolved
    // later by fuzzy endpoint/element matching.
    solidar::sketch::Sketch tangentGeometry;
    tangentGeometry.addLine({-20.0, 0.0}, {20.0, 0.0}, 42);
    const auto excludedProjected = tangentGeometry.lineId(0);
    tangentGeometry.setLineDashedById(excludedProjected, true);
    tangentGeometry.addLine({-20.0, 0.0}, {20.0, 0.0}, 42);
    const auto excludedConstruction = tangentGeometry.lineId(1);
    tangentGeometry.setLineDashedById(excludedConstruction, true);
    tangentGeometry.addLine({-20.0, 0.0}, {20.0, 0.0}, 42);
    const auto horizontal = tangentGeometry.lineId(2);
    solidar::sketch::Constraint projectedLock;
    projectedLock.type = solidar::sketch::ConstraintType::Lock;
    projectedLock.firstGeometry = excludedProjected;
    CHECK(tangentGeometry.addConstraint(projectedLock) !=
          solidar::sketch::kInvalidConstraintId);
    tangentGeometry.addLine({0.0, -20.0}, {0.0, 20.0});
    const auto vertical = tangentGeometry.lineId(3);
    tangentGeometry.addLine({0.0, 20.0}, {20.0, 0.0});
    const auto diagonal = tangentGeometry.lineId(4);
    solidar::SketchCanvas tangentCanvas;
    tangentCanvas.resize(900, 650);
    tangentCanvas.loadSketch(tangentGeometry);
    tangentCanvas.setCircleMode(
        solidar::SketchCanvas::CircleMode::ThreeTangents);
    tangentCanvas.setTool(solidar::SketchCanvas::Tool::Circle);
    tangentCanvas.show();
    QApplication::processEvents();
    click(tangentCanvas, screenPoint(tangentCanvas, {-10.0, 0.0}));
    click(tangentCanvas, screenPoint(tangentCanvas, {0.0, 10.0}));
    click(tangentCanvas, screenPoint(tangentCanvas, {10.0, 10.0}));
    CHECK(tangentCanvas.sketch().circles().size() == 1);
    const auto circleId = tangentCanvas.sketch().circleId(0);
    std::vector<solidar::sketch::GeometryId> tangentCarriers;
    for (const auto& constraint : tangentCanvas.sketch().constraints()) {
      if (constraint.type != solidar::sketch::ConstraintType::Tangent)
        continue;
      if (constraint.secondGeometry == circleId)
        tangentCarriers.push_back(constraint.firstGeometry);
      else if (constraint.firstGeometry == circleId)
        tangentCarriers.push_back(constraint.secondGeometry);
    }
    CHECK(std::find(tangentCarriers.begin(), tangentCarriers.end(),
                    horizontal) != tangentCarriers.end());
    CHECK(std::find(tangentCarriers.begin(), tangentCarriers.end(),
                    vertical) != tangentCarriers.end());
    CHECK(std::find(tangentCarriers.begin(), tangentCarriers.end(),
                    diagonal) != tangentCarriers.end());
    CHECK(std::find(tangentCarriers.begin(), tangentCarriers.end(),
                    excludedConstruction) == tangentCarriers.end());
    CHECK(std::find(tangentCarriers.begin(), tangentCarriers.end(),
                    excludedProjected) == tangentCarriers.end());
  }
  {
    // TwoTangentsRadius is one atomic gesture: both defining tangencies and
    // the driving diameter must be present, and one Undo removes all of them.
    solidar::sketch::Sketch tangentGeometry;
    tangentGeometry.addLine({-20.0, 0.0}, {20.0, 0.0});
    const auto horizontal = tangentGeometry.lineId(0);
    tangentGeometry.addLine({0.0, -20.0}, {0.0, 20.0});
    const auto vertical = tangentGeometry.lineId(1);
    solidar::SketchCanvas tangentCanvas;
    tangentCanvas.resize(900, 650);
    tangentCanvas.loadSketch(tangentGeometry);
    tangentCanvas.setCircleDiameter(8.0);
    tangentCanvas.setCircleMode(
        solidar::SketchCanvas::CircleMode::TwoTangentsRadius);
    tangentCanvas.setTool(solidar::SketchCanvas::Tool::Circle);
    tangentCanvas.show();
    QApplication::processEvents();
    click(tangentCanvas, screenPoint(tangentCanvas, {-10.0, 0.0}));
    click(tangentCanvas, screenPoint(tangentCanvas, {0.0, 10.0}));
    moveMouse(tangentCanvas, screenPoint(tangentCanvas, {4.0, 4.0}));
    click(tangentCanvas, screenPoint(tangentCanvas, {4.0, 4.0}));
    CHECK(tangentCanvas.sketch().circles().size() == 1);
    CHECK(tangentCanvas.sketch().dimensions().size() == 1);
    CHECK(tangentCanvas.sketch().dimensions().front().kind ==
          solidar::sketch::DimensionKind::CircleDiameter);
    const auto circleId = tangentCanvas.sketch().circleId(0);
    std::vector<solidar::sketch::GeometryId> tangentCarriers;
    bool diameterFound = false;
    for (const auto& constraint : tangentCanvas.sketch().constraints()) {
      if (constraint.type == solidar::sketch::ConstraintType::Tangent &&
          constraint.secondGeometry == circleId)
        tangentCarriers.push_back(constraint.firstGeometry);
      if (constraint.type == solidar::sketch::ConstraintType::Diameter &&
          constraint.firstGeometry == circleId)
        diameterFound = true;
    }
    CHECK(tangentCarriers.size() == 2);
    CHECK(std::find(tangentCarriers.begin(), tangentCarriers.end(),
                    horizontal) != tangentCarriers.end());
    CHECK(std::find(tangentCarriers.begin(), tangentCarriers.end(), vertical) !=
          tangentCarriers.end());
    CHECK(diameterFound);
    CHECK(tangentCanvas.undoHistorySize() == 1);
    tangentCanvas.undo();
    CHECK(tangentCanvas.sketch().circles().empty());
    CHECK(tangentCanvas.sketch().dimensions().empty());
    CHECK(tangentCanvas.sketch().constraints().empty());
    tangentCanvas.redo();
    CHECK(tangentCanvas.sketch().circles().size() == 1);
    CHECK(tangentCanvas.sketch().dimensions().size() == 1);
    CHECK(tangentCanvas.sketch().constraints().size() == 3);
  }
  {
    // A required tangent rejected after circle creation must roll the complete
    // TwoTangentsRadius gesture back. The nearby locked endpoint auto-binds
    // the new center away from the exact two-line tangent solution.
    solidar::sketch::Sketch conflictGeometry;
    conflictGeometry.addLine({-20.0, 0.0}, {20.0, 0.0});
    conflictGeometry.addLine({0.0, -20.0}, {0.0, 20.0});
    conflictGeometry.addLine({4.5, 4.0}, {4.5, 4.1});
    for (std::size_t index = 0; index < conflictGeometry.lines().size();
         ++index) {
      solidar::sketch::Constraint lock;
      lock.type = solidar::sketch::ConstraintType::Lock;
      lock.firstGeometry = conflictGeometry.lineId(index);
      CHECK(conflictGeometry.addConstraint(lock) !=
            solidar::sketch::kInvalidConstraintId);
    }
    const auto initialFingerprint = conflictGeometry.semanticFingerprint();
    solidar::SketchCanvas conflictCanvas;
    conflictCanvas.resize(900, 650);
    conflictCanvas.loadSketch(conflictGeometry);
    conflictCanvas.setCircleDiameter(8.0);
    conflictCanvas.setCircleMode(
        solidar::SketchCanvas::CircleMode::TwoTangentsRadius);
    conflictCanvas.setTool(solidar::SketchCanvas::Tool::Circle);
    conflictCanvas.show();
    QApplication::processEvents();
    int geometryChangedCount = 0;
    QObject::connect(&conflictCanvas,
                     &solidar::SketchCanvas::geometryChanged,
                     [&geometryChangedCount] { ++geometryChangedCount; });
    click(conflictCanvas, screenPoint(conflictCanvas, {-10.0, 0.0}));
    click(conflictCanvas, screenPoint(conflictCanvas, {0.0, 10.0}));
    moveMouse(conflictCanvas, screenPoint(conflictCanvas, {4.0, 4.0}));
    click(conflictCanvas, screenPoint(conflictCanvas, {4.0, 4.0}));
    CHECK(conflictCanvas.sketch().semanticFingerprint() == initialFingerprint);
    CHECK(conflictCanvas.sketch().circles().empty());
    CHECK(conflictCanvas.sketch().dimensions().empty());
    CHECK(conflictCanvas.sketch().constraints().size() == 3);
    CHECK(conflictCanvas.undoHistorySize() == 0);
    CHECK(geometryChangedCount == 0);
  }
  {
    // Legacy box selection intentionally selects curved entities when the
    // rubber-band intersects their screen bounding rectangle, even if it is
    // wholly inside the curve and does not cross the sampled stroke.
    solidar::sketch::Sketch curves;
    curves.addCircle({-50.0, 0.0}, 30.0);
    curves.addArc({50.0, 0.0}, 30.0, 0.0,
                  1.5707963267948966);
    solidar::SketchCanvas boxCanvas;
    boxCanvas.resize(900, 650);
    boxCanvas.loadSketch(curves);
    boxCanvas.show();
    QApplication::processEvents();
    const auto selectBox = [&boxCanvas](solidar::sketch::Point first,
                                        solidar::sketch::Point second) {
      const QPointF start = screenPoint(boxCanvas, first);
      const QPointF finish = screenPoint(boxCanvas, second);
      QMouseEvent press(QEvent::MouseButtonPress, start, Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(&boxCanvas, &press);
      QMouseEvent move(QEvent::MouseMove, finish, Qt::NoButton,
                       Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(&boxCanvas, &move);
      QMouseEvent release(QEvent::MouseButtonRelease, finish, Qt::LeftButton,
                          Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(&boxCanvas, &release);
      QApplication::processEvents();
    };
    selectBox({-45.0, 5.0}, {-40.0, 10.0});
    QKeyEvent removeCircle(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&boxCanvas, &removeCircle);
    CHECK(boxCanvas.sketch().circles().empty());
    CHECK(boxCanvas.sketch().arcs().size() == 1);

    selectBox({60.0, 10.0}, {65.0, 15.0});
    QKeyEvent removeArc(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&boxCanvas, &removeArc);
    CHECK(boxCanvas.sketch().arcs().empty());
  }
  {
    solidar::SketchCanvas diagnosticsCanvas;
    solidar::sketch::Sketch geometry;
    geometry.addLine({0.0, 0.0}, {10.0, 0.0});
    diagnosticsCanvas.loadSketch(geometry);
    diagnosticsCanvas.loadSketch(geometry);
    diagnosticsCanvas.loadSketch(geometry);
    CHECK(diagnosticsCanvas.fullDiagnosticsCount() == 0);
    diagnosticsCanvas.flushConstraintDiagnostics();
    CHECK(diagnosticsCanvas.fullDiagnosticsCount() == 1);
    diagnosticsCanvas.flushConstraintDiagnostics();
    CHECK(diagnosticsCanvas.fullDiagnosticsCount() == 1);
  }
  {
    // Real event-path cancellation: every primitive can be abandoned with a
    // short RMB click and the next gesture starts from a genuinely empty
    // controller state.
    for (const auto tool : {solidar::SketchCanvas::Tool::Line,
                            solidar::SketchCanvas::Tool::Rectangle,
                            solidar::SketchCanvas::Tool::Circle,
                            solidar::SketchCanvas::Tool::Arc,
                            solidar::SketchCanvas::Tool::Bezier}) {
      solidar::SketchCanvas gestureCanvas;
      gestureCanvas.resize(900, 650);
      gestureCanvas.show();
      gestureCanvas.setTool(tool);
      QApplication::processEvents();

      const QPointF abandoned(300.0, 260.0);
      click(gestureCanvas, abandoned);
      CHECK(gestureCanvas.hasActiveInteraction());
      shortRightClick(gestureCanvas, abandoned);
      CHECK(!gestureCanvas.hasActiveInteraction());
      CHECK(gestureCanvas.tool() == tool);

      const QPointF first(360.0, 300.0);
      const QPointF second(450.0, 330.0);
      click(gestureCanvas, first);
      click(gestureCanvas, second);
      if (tool == solidar::SketchCanvas::Tool::Arc)
        click(gestureCanvas, QPointF(405.0, 275.0));
      if (tool == solidar::SketchCanvas::Tool::Bezier) {
        click(gestureCanvas, QPointF(420.0, 270.0));
        click(gestureCanvas, QPointF(480.0, 300.0));
      }

      if (tool == solidar::SketchCanvas::Tool::Line)
        CHECK(gestureCanvas.sketch().lines().size() == 1);
      else if (tool == solidar::SketchCanvas::Tool::Rectangle)
        CHECK(gestureCanvas.sketch().lines().size() == 4);
      else if (tool == solidar::SketchCanvas::Tool::Circle)
        CHECK(gestureCanvas.sketch().circles().size() == 1);
      else if (tool == solidar::SketchCanvas::Tool::Arc)
        CHECK(gestureCanvas.sketch().arcs().size() == 1);
      else
        CHECK(gestureCanvas.sketch().beziers().size() == 1);
    }
  }
  {
    // Control points are selectable and movable through the real mouse-event
    // path; the sampled curve body remains selectable for deletion.
    solidar::SketchCanvas bezierCanvas;
    bezierCanvas.resize(900, 650);
    bezierCanvas.setSnapEnabled(false);
    bezierCanvas.setTool(solidar::SketchCanvas::Tool::Bezier);
    bezierCanvas.show();
    QApplication::processEvents();

    const QPointF first(350.0, 350.0);
    const QPointF control(400.0, 250.0);
    click(bezierCanvas, first);
    click(bezierCanvas, control);
    click(bezierCanvas, QPointF(500.0, 250.0));
    click(bezierCanvas, QPointF(550.0, 350.0));
    CHECK(bezierCanvas.sketch().beziers().size() == 1);
    const auto before = bezierCanvas.sketch().beziers()[0].points[1];

    bezierCanvas.setTool(solidar::SketchCanvas::Tool::Select);
    const QPointF movedControl(420.0, 230.0);
    QMouseEvent press(QEvent::MouseButtonPress, control, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bezierCanvas, &press);
    QMouseEvent move(QEvent::MouseMove, movedControl, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bezierCanvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, movedControl,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&bezierCanvas, &release);
    CHECK(std::abs(bezierCanvas.sketch().beziers()[0].points[1].xMm -
                       before.xMm - 4.0) < 1e-6);
    CHECK(std::abs(bezierCanvas.sketch().beziers()[0].points[1].yMm -
                       before.yMm - 4.0) < 1e-6);

    click(bezierCanvas, QPointF(457.5, 267.5));
    QKeyEvent removeBezier(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&bezierCanvas, &removeBezier);
    CHECK(bezierCanvas.sketch().beziers().empty());
  }
  {
    // AutoDimension first operand is cancelled through the same RMB path;
    // the following direct-line gesture must produce one clean dimension.
    solidar::sketch::Sketch geometry;
    geometry.addLine({-20.0, 10.0}, {20.0, 10.0});
    solidar::SketchCanvas dimensionCanvas;
    dimensionCanvas.resize(900, 650);
    dimensionCanvas.loadSketch(geometry);
    dimensionCanvas.show();
    dimensionCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    QApplication::processEvents();
    click(dimensionCanvas,
          screenPoint(dimensionCanvas, {-20.0, 10.0}));
    CHECK(dimensionCanvas.hasActiveInteraction());
    shortRightClick(dimensionCanvas,
                    screenPoint(dimensionCanvas, {-20.0, 10.0}));
    CHECK(!dimensionCanvas.hasActiveInteraction());

    click(dimensionCanvas, screenPoint(dimensionCanvas, {5.0, 10.0}));
    auto* editor =
        dimensionCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
    CHECK(editor != nullptr);
    CHECK(dimensionCanvas.interactionState().autoDimension.target !=
          solidar::SketchAutoDimensionTarget::None);
    CHECK(editor->isVisible());
    editor->setValue(30.0);
    auto* lineEdit = editor->findChild<QLineEdit*>();
    CHECK(lineEdit != nullptr);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(lineEdit, &enter);
    QApplication::processEvents();
    CHECK(dimensionCanvas.sketch().dimensions().size() == 1);
  }
  {
    // A projected/locked vertical carrier anchors the right endpoints of two
    // horizontal segments. Placing horizontal point dimensions must resize
    // the free left endpoints; Enter must not be rejected just because the
    // selected second endpoint is constrained to the carrier.
    solidar::sketch::Sketch geometry;
    geometry.addLine({0.0, -15.0}, {0.0, 45.0});
    geometry.addLine({-12.0, 20.0}, {0.0, 20.0});
    geometry.addLine({-8.0, -5.0}, {0.0, -5.0});
    const auto carrier = geometry.lineId(0);
    const auto upper = geometry.lineId(1);
    const auto lower = geometry.lineId(2);

    solidar::sketch::Constraint constraint;
    constraint.type = solidar::sketch::ConstraintType::Lock;
    constraint.firstGeometry = carrier;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    for (const auto segment : {upper, lower}) {
      constraint = {};
      constraint.type = solidar::sketch::ConstraintType::Horizontal;
      constraint.firstGeometry = segment;
      CHECK(geometry.addConstraint(constraint) !=
            solidar::sketch::kInvalidConstraintId);
      constraint = {};
      constraint.type = solidar::sketch::ConstraintType::PointOnLine;
      constraint.firstGeometry = carrier;
      constraint.secondPoint = {segment, false};
      CHECK(geometry.addConstraint(constraint) !=
            solidar::sketch::kInvalidConstraintId);
    }

    solidar::SketchCanvas anchoredCanvas;
    anchoredCanvas.resize(900, 650);
    anchoredCanvas.loadSketch(geometry);
    anchoredCanvas.show();
    anchoredCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    QApplication::processEvents();

    const auto addHorizontalDimension =
        [&](solidar::sketch::Point first, solidar::sketch::Point second,
            solidar::sketch::Point placement, double value) {
          click(anchoredCanvas, screenPoint(anchoredCanvas, first));
          click(anchoredCanvas, screenPoint(anchoredCanvas, second));
          moveMouse(anchoredCanvas, screenPoint(anchoredCanvas, placement));
          auto* editor = anchoredCanvas.findChild<QDoubleSpinBox*>(
              "primaryDimension");
          CHECK(editor != nullptr && editor->isVisible());
          CHECK(anchoredCanvas.interactionState().autoDimension.pointMode ==
                solidar::SketchPointDimensionMode::X);
          editor->setValue(value);
          auto* lineEdit = editor->findChild<QLineEdit*>();
          CHECK(lineEdit != nullptr);
          QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
          QApplication::sendEvent(lineEdit, &enter);
          QApplication::processEvents();
          CHECK(!editor->isVisible());
        };

    addHorizontalDimension({-12.0, 20.0}, {0.0, 20.0}, {-6.0, 30.0},
                           13.0);
    CHECK(anchoredCanvas.sketch().dimensions().size() == 1);
    CHECK(anchoredCanvas.sketch().dimensions().back().kind ==
          solidar::sketch::DimensionKind::PointDistanceX);
    const auto upperIndex = anchoredCanvas.sketch().lineIndex(upper);
    CHECK(upperIndex.has_value());
    CHECK(std::abs(anchoredCanvas.sketch().lines()[*upperIndex].start.xMm +
                   13.0) < 1e-7);
    CHECK(std::abs(anchoredCanvas.sketch().lines()[*upperIndex].end.xMm) <
          1e-7);
    addHorizontalDimension({-8.0, -5.0}, {0.0, -5.0}, {-4.0, -15.0}, 5.0);
    CHECK(anchoredCanvas.sketch().dimensions().size() == 2);
    CHECK(anchoredCanvas.sketch().dimensions().back().kind ==
          solidar::sketch::DimensionKind::PointDistanceX);
    const auto lowerIndex = anchoredCanvas.sketch().lineIndex(lower);
    CHECK(lowerIndex.has_value());
    CHECK(std::abs(anchoredCanvas.sketch().lines()[*lowerIndex].start.xMm +
                   5.0) < 1e-7);
    CHECK(std::abs(anchoredCanvas.sketch().lines()[*lowerIndex].end.xMm) <
          1e-7);
    const auto carrierIndex = anchoredCanvas.sketch().lineIndex(carrier);
    CHECK(carrierIndex.has_value());
    CHECK(std::abs(anchoredCanvas.sketch().lines()[*carrierIndex].start.xMm) <
          1e-7);
    CHECK(std::abs(anchoredCanvas.sketch().lines()[*carrierIndex].end.xMm) <
          1e-7);
  }
  {
    // Direct-line AutoDimension must not depend on which endpoint of an
    // attached Vertical line was drawn first. The reversed neighbour used to
    // pull the resized junction back during the next solver pass, so Enter
    // left the editor open and rejected an otherwise valid width.
    solidar::sketch::Sketch geometry;
    geometry.addLine({-12.0, 5.0}, {0.0, 5.0});
    geometry.addLine({-12.0, -10.0}, {-12.0, 5.0});
    geometry.addLine({-12.0, -10.0}, {-5.0, -10.0});
    const auto dimensioned = geometry.lineId(0);
    const auto vertical = geometry.lineId(1);

    solidar::sketch::Constraint constraint;
    constraint.type = solidar::sketch::ConstraintType::Horizontal;
    constraint.firstGeometry = dimensioned;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::Vertical;
    constraint.firstGeometry = vertical;
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::Coincident;
    constraint.firstPoint = {dimensioned, true};
    constraint.secondPoint = {vertical, false};
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnYAxis;
    constraint.secondPoint = {dimensioned, false};
    CHECK(geometry.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);

    solidar::SketchCanvas reversedCanvas;
    reversedCanvas.resize(900, 650);
    reversedCanvas.loadSketch(geometry);
    reversedCanvas.show();
    reversedCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    QApplication::processEvents();
    click(reversedCanvas, screenPoint(reversedCanvas, {-6.0, 5.0}));
    auto* editor =
        reversedCanvas.findChild<QDoubleSpinBox*>("primaryDimension");
    CHECK(editor != nullptr && editor->isVisible());
    editor->setValue(10.0);
    auto* lineEdit = editor->findChild<QLineEdit*>();
    CHECK(lineEdit != nullptr);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(lineEdit, &enter);
    QApplication::processEvents();

    CHECK(!editor->isVisible());
    CHECK(reversedCanvas.sketch().dimensions().size() == 1);
    const auto resizedIndex = reversedCanvas.sketch().lineIndex(dimensioned);
    const auto verticalIndex = reversedCanvas.sketch().lineIndex(vertical);
    CHECK(resizedIndex.has_value() && verticalIndex.has_value());
    CHECK(std::abs(reversedCanvas.sketch().lines()[*resizedIndex].start.xMm +
                   10.0) < 1e-7);
    CHECK(std::abs(reversedCanvas.sketch().lines()[*resizedIndex].end.xMm) <
          1e-7);
    CHECK(std::abs(reversedCanvas.sketch().lines()[*verticalIndex].start.xMm +
                   10.0) < 1e-7);
    CHECK(std::abs(reversedCanvas.sketch().lines()[*verticalIndex].end.xMm +
                   10.0) < 1e-7);
    CHECK(!solidar::sketch::analyzeConstraintSystem(reversedCanvas.sketch())
               .conflicting);
  }
  {
    // Escape cancels a real two-operand constraint carrier. Re-entering the
    // tool then creates the requested relation without the stale first line.
    solidar::sketch::Sketch geometry;
    geometry.addLine({-25.0, -10.0}, {-5.0, -10.0});
    geometry.addLine({5.0, 8.0}, {25.0, 15.0});
    solidar::SketchCanvas constraintCanvas;
    constraintCanvas.resize(900, 650);
    constraintCanvas.loadSketch(geometry);
    constraintCanvas.show();
    constraintCanvas.setTool(
        solidar::SketchCanvas::Tool::ParallelConstraint);
    QApplication::processEvents();
    click(constraintCanvas,
          screenPoint(constraintCanvas, {-15.0, -10.0}));
    CHECK(constraintCanvas.hasActiveInteraction());
    pressEscape(constraintCanvas);
    CHECK(!constraintCanvas.hasActiveInteraction());
    CHECK(constraintCanvas.tool() == solidar::SketchCanvas::Tool::Select);

    constraintCanvas.setTool(
        solidar::SketchCanvas::Tool::ParallelConstraint);
    click(constraintCanvas,
          screenPoint(constraintCanvas, {-15.0, -10.0}));
    click(constraintCanvas,
          screenPoint(constraintCanvas, {15.0, 11.5}));
    CHECK(std::any_of(
        constraintCanvas.sketch().constraints().begin(),
        constraintCanvas.sketch().constraints().end(),
        [](const solidar::sketch::Constraint& constraint) {
          return constraint.type ==
                 solidar::sketch::ConstraintType::Parallel;
        }));
  }
  {
    // Tool switching clears Mirror source selection. Reset/project replacement
    // clears a live Trim preview; both canvases accept a fresh Line gesture.
    solidar::sketch::Sketch geometry;
    geometry.addLine({-20.0, 0.0}, {20.0, 0.0});
    solidar::SketchCanvas mirrorCanvas;
    mirrorCanvas.resize(900, 650);
    mirrorCanvas.loadSketch(geometry);
    mirrorCanvas.show();
    mirrorCanvas.setTool(solidar::SketchCanvas::Tool::Mirror);
    QApplication::processEvents();
    click(mirrorCanvas, screenPoint(mirrorCanvas, {0.0, 0.0}));
    CHECK(mirrorCanvas.hasActiveInteraction());
    mirrorCanvas.setTool(solidar::SketchCanvas::Tool::Line);
    CHECK(!mirrorCanvas.hasActiveInteraction());
    click(mirrorCanvas, screenPoint(mirrorCanvas, {-10.0, 10.0}));
    click(mirrorCanvas, screenPoint(mirrorCanvas, {10.0, 10.0}));
    CHECK(mirrorCanvas.sketch().lines().size() == 2);

    solidar::SketchCanvas trimCanvas;
    trimCanvas.resize(900, 650);
    trimCanvas.loadSketch(geometry);
    trimCanvas.show();
    trimCanvas.setTool(solidar::SketchCanvas::Tool::Trim);
    QApplication::processEvents();
    moveMouse(trimCanvas, screenPoint(trimCanvas, {0.0, 0.0}));
    CHECK(trimCanvas.hasActiveInteraction());
    trimCanvas.resetSketch();
    CHECK(!trimCanvas.hasActiveInteraction());
    CHECK(trimCanvas.tool() == solidar::SketchCanvas::Tool::Select);
    trimCanvas.setTool(solidar::SketchCanvas::Tool::Line);
    click(trimCanvas, screenPoint(trimCanvas, {-10.0, -10.0}));
    click(trimCanvas, screenPoint(trimCanvas, {10.0, -10.0}));
    CHECK(trimCanvas.sketch().lines().size() == 1);

    trimCanvas.setTool(solidar::SketchCanvas::Tool::Circle);
    click(trimCanvas, screenPoint(trimCanvas, {0.0, 10.0}));
    CHECK(trimCanvas.hasActiveInteraction());
    trimCanvas.loadSketch(geometry);
    CHECK(!trimCanvas.hasActiveInteraction());
    CHECK(trimCanvas.tool() == solidar::SketchCanvas::Tool::Select);
  }
  {
    // Picker tie policy is deterministic through the real event path: a line
    // and circle overlap exactly at (5, 0), and the stable line candidate
    // wins without leaking a positional index into interaction state.
    solidar::sketch::Sketch overlap;
    overlap.addLine({-10.0, 0.0}, {10.0, 0.0});
    overlap.addCircle({0.0, 0.0}, 5.0);
    solidar::SketchCanvas overlapCanvas;
    overlapCanvas.resize(900, 650);
    overlapCanvas.loadSketch(overlap);
    overlapCanvas.setTool(solidar::SketchCanvas::Tool::Select);
    overlapCanvas.show();
    QApplication::processEvents();
    click(overlapCanvas, screenPoint(overlapCanvas, {5.0, 0.0}));
    overlapCanvas.setSelectedDashed(true);
    CHECK(overlapCanvas.sketch().lines()[0].dashed);
    CHECK(!overlapCanvas.sketch().circles()[0].dashed);
    pressEscape(overlapCanvas);
    CHECK(!overlapCanvas.hasActiveInteraction());
  }
  {
    // Arc construction style uses the same typed command/history path as
    // lines and circles, including symmetric Undo/Redo.
    solidar::sketch::Sketch geometry;
    geometry.addArc({0.0, 0.0}, 12.0, 0.0,
                    3.14159265358979323846 * 0.5, false);
    solidar::SketchCanvas arcCanvas;
    arcCanvas.resize(900, 650);
    arcCanvas.loadSketch(geometry);
    arcCanvas.show();
    QApplication::processEvents();
    click(arcCanvas, screenPoint(arcCanvas, {8.485281374, 8.485281374}));
    arcCanvas.setSelectedDashed(true);
    CHECK(arcCanvas.sketch().arcs().front().dashed);
    CHECK(arcCanvas.undoHistorySize() == 1);
    arcCanvas.undo();
    CHECK(!arcCanvas.sketch().arcs().front().dashed);
    arcCanvas.redo();
    CHECK(arcCanvas.sketch().arcs().front().dashed);
  }
  {
    // A locked-geometry drag is a rejected no-op: it must not publish a
    // project modification or create an empty history entry.
    solidar::sketch::Sketch locked;
    locked.addLine({-10.0, 0.0}, {10.0, 0.0});
    solidar::sketch::Constraint lock;
    lock.type = solidar::sketch::ConstraintType::Lock;
    lock.firstGeometry = locked.lineId(0);
    CHECK(locked.addConstraint(lock) !=
          solidar::sketch::kInvalidConstraintId);
    solidar::SketchCanvas lockedCanvas;
    lockedCanvas.resize(900, 650);
    lockedCanvas.loadSketch(locked);
    lockedCanvas.show();
    QApplication::processEvents();
    int geometryChangedCount = 0;
    QObject::connect(&lockedCanvas, &solidar::SketchCanvas::geometryChanged,
                     [&geometryChangedCount] { ++geometryChangedCount; });
    const auto fingerprint = lockedCanvas.sketch().semanticFingerprint();
    const auto start = screenPoint(lockedCanvas, {-10.0, 0.0});
    QMouseEvent press(QEvent::MouseButtonPress, start, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&lockedCanvas, &press);
    QMouseEvent move(QEvent::MouseMove, start + QPointF(30.0, -20.0),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&lockedCanvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease,
                        start + QPointF(30.0, -20.0), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&lockedCanvas, &release);
    CHECK(lockedCanvas.sketch().semanticFingerprint() == fingerprint);
    CHECK(lockedCanvas.undoHistorySize() == 0);
    CHECK(geometryChangedCount == 0);
  }
  {
    // Replacing a sketch during an active drag closes the gesture transaction
    // and leaves only the replacement model.
    solidar::sketch::Sketch initial;
    initial.addLine({-10.0, 0.0}, {10.0, 0.0});
    solidar::SketchCanvas dragCanvas;
    dragCanvas.resize(900, 650);
    dragCanvas.loadSketch(initial);
    dragCanvas.show();
    QApplication::processEvents();
    const auto start = screenPoint(dragCanvas, {-10.0, 0.0});
    QMouseEvent press(QEvent::MouseButtonPress, start, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dragCanvas, &press);
    QMouseEvent move(QEvent::MouseMove, start + QPointF(25.0, -10.0),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dragCanvas, &move);
    CHECK(dragCanvas.hasActiveInteraction());
    solidar::sketch::Sketch replacement;
    replacement.addCircle({3.0, 4.0}, 2.0);
    dragCanvas.loadSketch(replacement);
    CHECK(!dragCanvas.hasActiveInteraction());
    CHECK(dragCanvas.sketch().lines().empty());
    CHECK(dragCanvas.sketch().circles().size() == 1);
    CHECK(dragCanvas.undoHistorySize() == 0);
  }
  {
    // A successful gesture publishes one change and one history entry.
    solidar::SketchCanvas lineCanvas;
    lineCanvas.resize(900, 650);
    lineCanvas.show();
    lineCanvas.setTool(solidar::SketchCanvas::Tool::Line);
    int geometryChangedCount = 0;
    QObject::connect(&lineCanvas, &solidar::SketchCanvas::geometryChanged,
                     [&geometryChangedCount] { ++geometryChangedCount; });
    click(lineCanvas, screenPoint(lineCanvas, {-5.0, 0.0}));
    click(lineCanvas, screenPoint(lineCanvas, {5.0, 0.0}));
    CHECK(lineCanvas.sketch().lines().size() == 1);
    CHECK(lineCanvas.undoHistorySize() == 1);
    CHECK(geometryChangedCount == 1);
  }
  {
    // A conflicting PointOnCircle command is an atomic UI rejection: no
    // success text, modified signal, history item, or partial constraint.
    solidar::sketch::Sketch geometry;
    geometry.addCircle({0.0, 0.0}, 5.0);
    geometry.addLine({20.0, 0.0}, {30.0, 0.0});
    for (const auto id : {geometry.circleId(0), geometry.lineId(0)}) {
      solidar::sketch::Constraint lock;
      lock.type = solidar::sketch::ConstraintType::Lock;
      lock.firstGeometry = id;
      CHECK(geometry.addConstraint(lock) !=
            solidar::sketch::kInvalidConstraintId);
    }
    solidar::SketchCanvas conflictCanvas;
    conflictCanvas.resize(900, 650);
    conflictCanvas.loadSketch(geometry);
    conflictCanvas.show();
    conflictCanvas.setTool(
        solidar::SketchCanvas::Tool::CoincidentConstraint);
    QApplication::processEvents();
    int geometryChangedCount = 0;
    QString status;
    QObject::connect(&conflictCanvas,
                     &solidar::SketchCanvas::geometryChanged,
                     [&geometryChangedCount] { ++geometryChangedCount; });
    QObject::connect(&conflictCanvas,
                     &solidar::SketchCanvas::selectionChanged,
                     [&status](const QString& value) { status = value; });
    click(conflictCanvas, screenPoint(conflictCanvas, {5.0, 0.0}));
    click(conflictCanvas, screenPoint(conflictCanvas, {20.0, 0.0}));
    CHECK(conflictCanvas.sketch().constraints().size() == 2);
    CHECK(conflictCanvas.undoHistorySize() == 0);
    CHECK(geometryChangedCount == 0);
    CHECK(status.contains(QString::fromUtf8("отклонено")));
  }
  return EXIT_SUCCESS;
}
