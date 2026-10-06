#include <QApplication>
#include <QColor>
#include <QDoubleSpinBox>
#include <QDir>
#include <QKeyEvent>
#include <QImage>
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
  return EXIT_SUCCESS;
}
