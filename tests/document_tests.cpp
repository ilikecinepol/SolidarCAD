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
  TestShapeFeature(solidar::FeatureId id, std::string name, bool succeeds)
      : ShapeFeature(id, std::move(name)), succeeds_(succeeds) {}

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
  assert(document.sketches().empty());
  assert(document.bodies().empty());

  auto& profile = document.addSketch();
  const auto profileId = profile.id;
  assert(profileId != solidar::kInvalidSketchId);
  assert(profile.name == "Sketch 1");
  assert(document.findSketch(profileId) == &profile);
  assert(document.findSketch(solidar::kInvalidSketchId) == nullptr);

  document.addSketch(5000, "Restored sketch");
  const auto generatedSketchId = document.addSketch("After restore").id;
  assert(generatedSketchId > 5000);

  auto& firstBody = document.addBody();
  const auto firstBodyId = firstBody.id();
  assert(firstBodyId != solidar::kInvalidBodyId);
  assert(firstBody.name() == "Body 1");

  auto& secondBody = document.addBody("Body 2");
  const auto secondBodyId = secondBody.id();
  assert(secondBodyId != firstBodyId);
  assert(document.activeBody() == &secondBody);
  assert(document.findBody(firstBodyId) != nullptr);
  assert(document.findBody(secondBodyId) == &secondBody);
  assert(document.findBody(solidar::kInvalidBodyId) == nullptr);

  // Adding to a vector may invalidate references, so resolve model objects by
  // their stable ID just as a future ExtrudeFeature will resolve its Sketch.
  auto* body = document.findBody(firstBodyId);
  assert(body != nullptr);
  auto first = std::make_unique<TestShapeFeature>("First", true);
  const auto firstId = first->id();
  auto* firstPtr = first.get();
  auto second = std::make_unique<TestShapeFeature>("Second", true);
  const auto secondId = second->id();
  assert(secondId != firstId);
  auto* secondPtr = second.get();
  body->addFeature(std::move(first));
  body->addFeature(std::move(second));
  assert(document.rebuild());
  assert(firstPtr->rebuildCount == 1);
  assert(firstPtr->isValid());
  assert(body->activeFeature()->name() == "Second");

  body->markDirtyFrom(1);
  assert(!firstPtr->isDirty());
  assert(secondPtr->isDirty());
  assert(document.rebuild());
  assert(firstPtr->rebuildCount == 1);
  assert(secondPtr->rebuildCount == 2);

  // Document snapshots used by the existing undo stack must deep-copy bodies.
  solidar::Document snapshot = document;
  auto* snapshotBody = snapshot.findBody(firstBodyId);
  assert(snapshotBody != nullptr);
  assert(snapshotBody != document.findBody(firstBodyId));
  assert(snapshotBody->id() == firstBodyId);
  assert(snapshotBody->features().front()->id() == firstId);
  assert(snapshot.findSketch(profileId) != document.findSketch(profileId));
  assert(snapshot.findSketch(profileId)->id == profileId);
  firstPtr->setDirty();
  document.findSketch(profileId)->name = "Changed profile";
  assert(snapshotBody->features().front()->isValid());
  assert(snapshot.findSketch(profileId)->name == "Sketch 1");

  // Restored IDs advance the generators, preventing collisions after load.
  solidar::Document restored;
  restored.addBody(6000, "Restored body");
  assert(restored.addBody("Generated body").id() > 6000);
  auto restoredFeature =
      std::make_unique<TestShapeFeature>(7000, "Restored feature", true);
  const auto restoredFeatureId = restoredFeature->id();
  auto generatedFeature = std::make_unique<TestShapeFeature>("Generated", true);
  assert(restoredFeatureId == 7000);
  assert(generatedFeature->id() > restoredFeatureId);

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
