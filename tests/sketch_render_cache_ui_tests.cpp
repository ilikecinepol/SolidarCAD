#include "TestAssertions.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "ui/SketchCanvas.h"

namespace {

QPointF screenPoint(const solidar::SketchCanvas& canvas,
                    solidar::sketch::Point point) {
  constexpr double rulerLeft = 44.0;
  constexpr double rulerTop = 30.0;
  const double centerX = rulerLeft + (canvas.width() - rulerLeft) * 0.5;
  const double centerY = rulerTop + (canvas.height() - rulerTop) * 0.5;
  return {centerX + point.xMm * 5.0, centerY - point.yMm * 5.0};
}

QImage paint(solidar::SketchCanvas& canvas) {
  const QPixmap pixmap = canvas.grab();
  return pixmap.toImage();
}

void press(solidar::SketchCanvas& canvas, QPointF point) {
  QMouseEvent event(QEvent::MouseButtonPress, point, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &event);
}

void dragMove(solidar::SketchCanvas& canvas, QPointF point) {
  QMouseEvent event(QEvent::MouseMove, point, Qt::NoButton,
                    Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &event);
}

void release(solidar::SketchCanvas& canvas, QPointF point) {
  QMouseEvent event(QEvent::MouseButtonRelease, point, Qt::LeftButton,
                    Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &event);
}

int colorDistance(QColor first, QColor second) {
  return std::abs(first.red() - second.red()) +
         std::abs(first.green() - second.green()) +
         std::abs(first.blue() - second.blue());
}

int maxDifferenceInBox(const QImage& first, const QImage& second,
                       QRect box) {
  box = box.intersected(first.rect()).intersected(second.rect());
  int maximum = 0;
  for (int y = box.top(); y <= box.bottom(); ++y)
    for (int x = box.left(); x <= box.right(); ++x)
      maximum = std::max(maximum,
                         colorDistance(first.pixelColor(x, y),
                                       second.pixelColor(x, y)));
  return maximum;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);

  // Public committed mutation: the first paint caches an empty scene;
  // setRectangle must invalidate it and the next paint must contain geometry.
  solidar::SketchCanvas rectangle;
  rectangle.resize(640, 460);
  rectangle.show();
  QApplication::processEvents();
  const QImage empty = paint(rectangle);
  const auto emptyBuilds = rectangle.committedRenderSceneBuildCount();
  CHECK(emptyBuilds >= 1);
  rectangle.setRectangle(40.0, 20.0);
  CHECK(rectangle.committedRenderSceneBuildCount() == emptyBuilds);
  const QImage withRectangle = paint(rectangle);
  CHECK(rectangle.committedRenderSceneBuildCount() == emptyBuilds + 1);
  const QPoint bottom = screenPoint(rectangle, {0.0, -10.0}).toPoint();
  CHECK(maxDifferenceInBox(empty, withRectangle,
                           QRect(bottom - QPoint(90, 3), QSize(181, 7))) > 30);

  // Hover is transient and must not rebuild the committed scene.
  const auto beforeHover = rectangle.committedRenderSceneBuildCount();
  QMouseEvent hover(QEvent::MouseMove, QPointF(590.0, 410.0), Qt::NoButton,
                    Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&rectangle, &hover);
  static_cast<void>(paint(rectangle));
  CHECK(rectangle.committedRenderSceneBuildCount() == beforeHover);

  // A live geometry drag mutates the model before release. The intermediate
  // paint must use the new committed geometry, not the pre-drag cached scene.
  rectangle.setTool(solidar::SketchCanvas::Tool::Select);
  const QPointF dragStart = screenPoint(rectangle, {0.0, -10.0});
  const QPointF dragEnd = screenPoint(rectangle, {5.0, -5.0});
  const auto beforeDrag = rectangle.committedRenderSceneBuildCount();
  const auto beforeFingerprint = rectangle.sketch().semanticFingerprint();
  press(rectangle, dragStart);
  dragMove(rectangle, dragEnd);
  CHECK(rectangle.sketch().semanticFingerprint() != beforeFingerprint);
  const QImage duringDrag = paint(rectangle);
  CHECK(rectangle.committedRenderSceneBuildCount() == beforeDrag + 1);
  CHECK(maxDifferenceInBox(withRectangle, duringDrag,
                           duringDrag.rect()) > 30);
  const auto duringFingerprint = rectangle.sketch().semanticFingerprint();
  release(rectangle, dragEnd);
  static_cast<void>(paint(rectangle));
  CHECK(rectangle.sketch().semanticFingerprint() == duringFingerprint);
  CHECK(rectangle.committedRenderSceneBuildCount() == beforeDrag + 2);

  // Stored dimension line placement is committed model state. Label placement
  // is intentionally a small transient annotation delta; both update through
  // real mouse paths and remain visible after release.
  solidar::sketch::Sketch dimensionSketch;
  dimensionSketch.addLine({-10.0, 0.0}, {10.0, 0.0});
  solidar::sketch::Dimension dimension;
  dimension.kind = solidar::sketch::DimensionKind::LineLength;
  dimension.geometryId = dimensionSketch.lineId(0);
  dimension.valueMm = 20.0;
  dimension.offsetMm = 4.0;
  dimensionSketch.storeDimension(dimension);

  solidar::SketchCanvas dimensions;
  dimensions.resize(640, 460);
  dimensions.loadSketch(dimensionSketch);
  dimensions.setTool(solidar::SketchCanvas::Tool::Select);
  dimensions.show();
  QApplication::processEvents();
  const QImage dimensionBefore = paint(dimensions);
  const auto initialDimensionBuilds =
      dimensions.committedRenderSceneBuildCount();
  const QPointF geometryCenter = screenPoint(dimensions, {0.0, 0.0});

  // Stay outside the label OBB while remaining on the dimension line.
  const QPointF lineHit = geometryCenter + QPointF(-49.0, 20.0);
  const QPointF movedLine = lineHit + QPointF(0.0, 20.0);
  press(dimensions, lineHit);
  CHECK(dimensions.interactionState().dimension.draggingLine.has_value());
  dragMove(dimensions, movedLine);
  CHECK(std::abs(dimensions.sketch().dimensions()[0].offsetMm - 8.0) < 0.25);
  const QImage dimensionDuringLine = paint(dimensions);
  CHECK(dimensions.committedRenderSceneBuildCount() ==
        initialDimensionBuilds + 1);
  CHECK(dimensionDuringLine != dimensionBefore);
  release(dimensions, movedLine);
  const double persistedOffset = dimensions.sketch().dimensions()[0].offsetMm;
  static_cast<void>(paint(dimensions));
  CHECK(std::abs(dimensions.sketch().dimensions()[0].offsetMm -
                 persistedOffset) < 1e-9);

  const auto beforeLabelBuilds = dimensions.committedRenderSceneBuildCount();
  const QPointF labelHit = geometryCenter + QPointF(0.0, 50.0);
  const QPointF movedLabel = labelHit + QPointF(30.0, 15.0);
  const QImage labelBefore = paint(dimensions);
  press(dimensions, labelHit);
  CHECK(dimensions.interactionState().dimension.draggingLabel.has_value());
  dragMove(dimensions, movedLabel);
  CHECK(!dimensions.interactionState().dimension.labelAlongMm.empty());
  CHECK(!dimensions.interactionState().dimension.labelOffsetMm.empty());
  const auto labelAlong =
      dimensions.interactionState().dimension.labelAlongMm.front();
  const auto labelOffset =
      dimensions.interactionState().dimension.labelOffsetMm.front();
  const QImage labelDuring = paint(dimensions);
  CHECK(labelDuring != labelBefore);
  CHECK(dimensions.committedRenderSceneBuildCount() == beforeLabelBuilds);
  release(dimensions, movedLabel);
  static_cast<void>(paint(dimensions));
  CHECK(std::abs(dimensions.interactionState().dimension.labelAlongMm.front() -
                 labelAlong) < 1e-9);
  CHECK(std::abs(dimensions.interactionState().dimension.labelOffsetMm.front() -
                 labelOffset) < 1e-9);
  CHECK(dimensions.committedRenderSceneBuildCount() == beforeLabelBuilds);

  // Project/load replacement has its own cache generation even when the new
  // sketch starts from the same local ID sequence as the previous project.
  solidar::SketchCanvas replacement;
  replacement.resize(640, 460);
  replacement.show();
  QApplication::processEvents();
  const QImage replacementEmpty = paint(replacement);
  const auto replacementEmptyBuilds =
      replacement.committedRenderSceneBuildCount();
  solidar::sketch::Sketch loaded;
  loaded.addCircle({0.0, 0.0}, 12.0);
  replacement.loadSketch(loaded);
  CHECK(replacement.committedRenderSceneBuildCount() ==
        replacementEmptyBuilds);
  const QImage replacementLoaded = paint(replacement);
  CHECK(replacement.committedRenderSceneBuildCount() ==
        replacementEmptyBuilds + 1);
  CHECK(replacement.sketch().circles().size() == 1);
  CHECK(maxDifferenceInBox(replacementEmpty, replacementLoaded,
                           replacementLoaded.rect()) > 30);
  replacement.resetSketch();
  static_cast<void>(paint(replacement));
  CHECK(replacement.committedRenderSceneBuildCount() ==
        replacementEmptyBuilds + 2);
  CHECK(replacement.sketch().circles().empty());

  return EXIT_SUCCESS;
}
