#include "model/Document.h"
#include "sketch/Sketch.h"

#include <cassert>
#include <memory>
#include <stdexcept>

namespace {

class TestShapeFeature final : public solidar::ShapeFeature {
 public:
  TestShapeFeature(std::string name, bool succeeds)
      : ShapeFeature(std::move(name)), succeeds_(succeeds) {}

  bool rebuild() override {
    ++rebuildCount;
    if (succeeds_) {
      markValid();
      return true;
    }
    clearShape();
    markError("test rebuild failure");
    return false;
  }

  [[nodiscard]] std::string typeName() const override { return "TestShape"; }
  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<TestShapeFeature>(*this);
  }

  int rebuildCount{0};

 private:
  bool succeeds_{true};
};

}  // namespace

int main() {
  solidar::Document document;
  assert(document.features().size() == 2);
  assert(document.bodies().empty());

  auto& body = document.addBody();
  assert(document.activeBody() == &body);
  assert(body.name() == "Body 1");
  auto first = std::make_unique<TestShapeFeature>("First", true);
  const auto firstId = first->id();
  auto* firstPtr = first.get();
  body.addFeature(std::move(first));
  body.addFeature(std::make_unique<TestShapeFeature>("Second", true));
  assert(document.rebuild());
  assert(firstPtr->rebuildCount == 1);
  assert(firstPtr->isValid());
  assert(body.activeFeature()->name() == "Second");

  // Document snapshots used by the existing undo stack must deep-copy bodies.
  solidar::Document snapshot = document;
  assert(snapshot.activeBody() != document.activeBody());
  assert(snapshot.activeBody()->features().front()->id() == firstId);
  firstPtr->setDirty();
  assert(snapshot.activeBody()->features().front()->isValid());

  auto& failingBody = document.addBody("Failing body");
  auto failing = std::make_unique<TestShapeFeature>("Failure", false);
  auto* failingPtr = failing.get();
  failingBody.addFeature(std::move(failing));
  assert(!document.rebuild());
  assert(!failingPtr->isValid());
  assert(!failingPtr->error().empty());

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
