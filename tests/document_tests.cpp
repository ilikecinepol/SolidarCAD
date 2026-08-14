#include "model/Document.h"
#include "sketch/Sketch.h"

#include <cassert>
#include <stdexcept>

int main() {
  solidar::Document document;
  assert(document.features().size() == 2);

  document.setBox({100.0, 50.0, 12.0});
  assert(document.box().widthMm == 100.0);

  bool rejected = false;
  try {
    document.setBox({0.0, 50.0, 12.0});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);

  solidar::sketch::Sketch sketch;
  assert(sketch.lines().empty());
  assert(sketch.circles().empty());
  sketch.setRectangle(80.0, 35.0);
  assert(sketch.lines().size() == 4);
  assert(sketch.isClosed());
  assert(sketch.widthMm() == 80.0);
  const auto rectangleId = sketch.lines().front().elementId;
  sketch.translateElement(rectangleId, 10.0, 5.0);
  assert(sketch.lines().front().start.xMm == -30.0);
  assert(sketch.lines().front().start.yMm == -12.5);
  for (const auto& line : sketch.lines()) assert(line.elementId == rectangleId);

  sketch.addCircle({0.0, 0.0}, 5.0);
  sketch.translateCircle(0, 15.0, -10.0);
  assert(sketch.circles().front().center.xMm == 15.0);
  assert(sketch.circles().front().center.yMm == -10.0);
  bool sketchRejected = false;
  try {
    sketch.setRectangle(-1.0, 35.0);
  } catch (const std::invalid_argument&) {
    sketchRejected = true;
  }
  assert(sketchRejected);
  return 0;
}
