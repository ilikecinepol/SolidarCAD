#include "model/Document.h"
#include "model/TopologyReferenceResolver.h"
#include "sketch/Sketch.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Face.hxx>

#include "TestAssertions.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

class TestShapeFeature final : public solidar::ShapeFeature {
 public:
  TestShapeFeature(std::string name, bool succeeds,
                   solidar::SketchId dependsOn = solidar::kInvalidSketchId)
      : ShapeFeature(std::move(name)),
        succeeds_(succeeds),
        dependsOn_(dependsOn) {}
  TestShapeFeature(solidar::FeatureId id, std::string name, bool succeeds)
      : ShapeFeature(id, std::move(name)), succeeds_(succeeds) {}

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    ++rebuildCount;
    if (succeeds_) {
      setShape(std::make_shared<TopoDS_Shape>(
          BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape()));
      markValid();
      return true;
    }
    clearShape();
    markError("test rebuild failure");
    return false;
  }

 public:

  [[nodiscard]] solidar::FeatureDependencies dependencies() const override {
    solidar::FeatureDependencies result;
    if (dependsOn_ != solidar::kInvalidSketchId)
      result.sketchIds.push_back(dependsOn_);
    return result;
  }
  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<TestShapeFeature>(*this);
  }

  int rebuildCount{0};

  void failWithoutMessage() { markError({}); }

 private:
  bool succeeds_{true};
  solidar::SketchId dependsOn_{solidar::kInvalidSketchId};
};

enum class GuardedMode {
  Valid,
  InvalidBRep,
  OcctException,
  StandardException,
  UnknownException,
};

class GuardedShapeFeature final : public solidar::ShapeFeature {
 public:
  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<GuardedShapeFeature>(*this);
  }

  GuardedMode mode{GuardedMode::Valid};
  int rebuildCount{0};

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    ++rebuildCount;
    if (mode == GuardedMode::OcctException)
      throw Standard_Failure("synthetic OCCT failure");
    if (mode == GuardedMode::StandardException)
      throw std::runtime_error("synthetic standard failure");
    if (mode == GuardedMode::UnknownException) throw 7;
    if (mode == GuardedMode::InvalidBRep) {
      BRep_Builder builder;
      TopoDS_Face invalidFace;
      builder.MakeFace(invalidFace);
      setShape(std::make_shared<TopoDS_Shape>(invalidFace));
      markValid();
      return true;
    }
    setShape(std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(2.0, 3.0, 4.0).Shape()));
    markValid();
    return true;
  }
};

}  // namespace

int main() {
  {
    solidar::Document imageDocument;
    auto& image = imageDocument.addReferenceImage(
        "Photo", "C:/missing/reference.png", "Базовая плоскость XY",
        solidar::SketchPlacement::xy(), 1920, 1080);
    const auto id = image.id;
    image.offsetXMm = 12.5;
    image.offsetYMm = -7.0;
    image.offsetZMm = 3.0;
    image.scale = 0.4;
    image.visible = false;
    CHECK(imageDocument.referenceImages().size() == 1);
    CHECK(imageDocument.findReferenceImage(id));
    CHECK(imageDocument.findReferenceImage(id)->pixelWidth == 1920);
    CHECK(imageDocument.findReferenceImage(id)->scale == 0.4);
    CHECK(imageDocument.findReferenceImage(id)->offsetZMm == 3.0);
    CHECK(imageDocument.removeReferenceImage(id));
    CHECK(!imageDocument.findReferenceImage(id));
    CHECK(!imageDocument.removeReferenceImage(id));
  }

  solidar::Document document;
  CHECK(document.sketches().empty());
  CHECK(document.bodies().empty());

  auto& profile = document.addSketch();
  const auto profileId = profile.id;
  CHECK(profileId != solidar::kInvalidSketchId);
  CHECK(profile.name == "Sketch 1");
  CHECK(document.findSketch(profileId) == &profile);
  CHECK(document.findSketch(solidar::kInvalidSketchId) == nullptr);

  document.addSketch(5000, "Restored sketch");
  const auto generatedSketchId = document.addSketch("After restore").id;
  CHECK(generatedSketchId > 5000);

  auto& firstBody = document.addBody();
  const auto firstBodyId = firstBody.id();
  CHECK(firstBodyId != solidar::kInvalidBodyId);
  CHECK(firstBody.name() == "Body 1");

  auto& secondBody = document.addBody("Body 2");
  const auto secondBodyId = secondBody.id();
  CHECK(secondBodyId != firstBodyId);
  CHECK(document.activeBody() == &secondBody);
  CHECK(document.findBody(firstBodyId) != nullptr);
  CHECK(document.findBody(secondBodyId) == &secondBody);
  CHECK(document.findBody(solidar::kInvalidBodyId) == nullptr);

  // Adding to a vector may invalidate references, so resolve model objects by
  // their stable ID just as a future ExtrudeFeature will resolve its Sketch.
  auto* body = document.findBody(firstBodyId);
  CHECK(body != nullptr);
  auto first = std::make_unique<TestShapeFeature>("First", true);
  const auto firstId = first->id();
  auto* firstPtr = first.get();
  auto second = std::make_unique<TestShapeFeature>("Second", true);
  const auto secondId = second->id();
  CHECK(secondId != firstId);
  auto* secondPtr = second.get();
  body->addFeature(std::move(first));
  body->addFeature(std::move(second));
  CHECK(document.rebuild());
  CHECK(firstPtr->rebuildCount == 1);
  CHECK(firstPtr->isValid());
  CHECK(body->activeFeature()->name() == "Second");

  body->markDirtyFrom(1);
  CHECK(!firstPtr->isDirty());
  CHECK(secondPtr->isDirty());
  CHECK(document.rebuild());
  CHECK(firstPtr->rebuildCount == 1);
  CHECK(secondPtr->rebuildCount == 2);

  // Sketch edits propagate from the first dependent feature through the
  // ordered Body history, without UI knowledge of concrete feature types.
  solidar::Document dependencyDocument;
  auto& dependencySketch = dependencyDocument.addSketch("Dependency");
  const auto dependencySketchId = dependencySketch.id;
  auto& dependencyBody = dependencyDocument.addBody("Dependency body");
  auto dependent = std::make_unique<TestShapeFeature>(
      "Dependent", true, dependencySketchId);
  auto* dependentPtr = dependent.get();
  auto downstream = std::make_unique<TestShapeFeature>("Downstream", true);
  auto* downstreamPtr = downstream.get();
  dependencyBody.addFeature(std::move(dependent));
  dependencyBody.addFeature(std::move(downstream));
  CHECK(dependencyDocument.recompute());
  CHECK(dependentPtr->rebuildCount == 1);
  CHECK(downstreamPtr->rebuildCount == 1);

  solidar::sketch::Sketch replacement;
  replacement.setRectangle(20.0, 10.0);
  CHECK(dependencyDocument.replaceSketchGeometry(dependencySketchId,
                                                   replacement));
  CHECK(dependentPtr->isDirty());
  CHECK(downstreamPtr->isDirty());
  CHECK(dependencyDocument.recompute());
  CHECK(dependentPtr->rebuildCount == 2);
  CHECK(downstreamPtr->rebuildCount == 2);

  CHECK(dependencyDocument.recomputeFrom(downstreamPtr->id()));
  CHECK(dependentPtr->rebuildCount == 2);
  CHECK(downstreamPtr->rebuildCount == 3);
  CHECK(!dependencyDocument.recomputeFrom(solidar::kInvalidFeatureId));

  // Document snapshots used by the existing undo stack must deep-copy bodies.
  solidar::Document snapshot = document;
  auto* snapshotBody = snapshot.findBody(firstBodyId);
  CHECK(snapshotBody != nullptr);
  CHECK(snapshotBody != document.findBody(firstBodyId));
  CHECK(snapshotBody->id() == firstBodyId);
  CHECK(snapshotBody->features().front()->id() == firstId);
  CHECK(snapshot.findSketch(profileId) != document.findSketch(profileId));
  CHECK(snapshot.findSketch(profileId)->id == profileId);
  firstPtr->setDirty();
  document.findSketch(profileId)->name = "Changed profile";
  CHECK(snapshotBody->features().front()->isValid());
  CHECK(snapshot.findSketch(profileId)->name == "Sketch 1");

  // Restored IDs advance the generators, preventing collisions after load.
  solidar::Document restored;
  restored.addBody(6000, "Restored body");
  CHECK(restored.addBody("Generated body").id() > 6000);
  auto restoredFeature =
      std::make_unique<TestShapeFeature>(7000, "Restored feature", true);
  const auto restoredFeatureId = restoredFeature->id();
  auto generatedFeature = std::make_unique<TestShapeFeature>("Generated", true);
  CHECK(restoredFeatureId == 7000);
  CHECK(generatedFeature->id() > restoredFeatureId);

  auto& failingBody = document.addBody("Failing body");
  auto failing = std::make_unique<TestShapeFeature>("Failure", false);
  auto* failingPtr = failing.get();
  failingBody.addFeature(std::move(failing));
  CHECK(!document.rebuild());
  CHECK(!failingPtr->isValid());
  CHECK(failingPtr->isFailed());
  CHECK(!failingPtr->error().empty());
  CHECK(document.rebuildError() == "test rebuild failure");

  // Every feature rebuild crosses one noexcept geometry boundary.  An OCCT
  // exception must become a stable model error while preserving the last
  // valid committed B-Rep pointer for a later parameter correction.
  for (const GuardedMode failureMode : {
           GuardedMode::InvalidBRep, GuardedMode::OcctException,
           GuardedMode::StandardException, GuardedMode::UnknownException}) {
    solidar::Document guardedDocument;
    auto& guardedBody = guardedDocument.addBody("Guarded body");
    const auto guardedBodyId = guardedBody.id();
    auto guarded = std::make_unique<GuardedShapeFeature>();
    auto* guardedPtr = guarded.get();
    guardedBody.addFeature(std::move(guarded));
    auto downstream =
        std::make_unique<TestShapeFeature>("Guarded downstream", true);
    auto* downstreamPtr = downstream.get();
    guardedBody.addFeature(std::move(downstream));
    auto& independentBody = guardedDocument.addBody("Independent body");
    auto independent =
        std::make_unique<TestShapeFeature>("Independent", true);
    auto* independentPtr = independent.get();
    independentBody.addFeature(std::move(independent));

    CHECK(guardedDocument.recompute());
    const auto lastValidShape = guardedPtr->shape();
    const auto downstreamLastValid = downstreamPtr->shape();
    CHECK(lastValidShape && downstreamLastValid);
    guardedPtr->mode = failureMode;
    guardedPtr->setDirty();
    independentPtr->setDirty();
    if (failureMode == GuardedMode::InvalidBRep) {
      BRep_Builder builder;
      TopoDS_Face invalidFace;
      builder.MakeFace(invalidFace);
      CHECK(!invalidFace.IsNull());
      CHECK(!BRepCheck_Analyzer(invalidFace).IsValid());
    }
    CHECK(!guardedDocument.recompute());
    CHECK(guardedPtr->isFailed());
    CHECK(!guardedPtr->error().empty());
    CHECK(!guardedPtr->shape());
    CHECK(guardedPtr->lastValidShape() == lastValidShape);
    CHECK(downstreamPtr->isFailed());
    CHECK(!downstreamPtr->error().empty());
    CHECK(!downstreamPtr->shape());
    CHECK(downstreamPtr->lastValidShape() == downstreamLastValid);
    const auto* failedBody = guardedDocument.findBody(guardedBodyId);
    CHECK(failedBody);
    CHECK(!failedBody->resultShape());
    CHECK(failedBody->lastValidResultShape() == downstreamLastValid);
    CHECK(independentPtr->isValid());
    CHECK(independentPtr->rebuildCount == 2);
  }

  // The public, non-virtual entry itself is the firewall: direct callers do
  // not need to know about a second guarded API and cannot invoke rebuildImpl.
  {
    solidar::Document directDocument;
    GuardedShapeFeature directFeature;
    solidar::Feature& publicEntry = directFeature;
    solidar::RebuildContext context{directDocument, nullptr, nullptr};
    CHECK(publicEntry.rebuild(context));
    auto committed = directFeature.shape();
    CHECK(committed);
    for (const GuardedMode failureMode : {
             GuardedMode::OcctException, GuardedMode::StandardException,
             GuardedMode::UnknownException}) {
      directFeature.mode = failureMode;
      directFeature.setDirty();
      CHECK(!publicEntry.rebuild(context));
      CHECK(directFeature.isFailed());
      CHECK(!directFeature.error().empty());
      CHECK(!directFeature.shape());
      CHECK(directFeature.lastValidShape() == committed);

      directFeature.mode = GuardedMode::Valid;
      directFeature.setDirty();
      CHECK(publicEntry.rebuild(context));
      committed = directFeature.shape();
      CHECK(committed);
    }
  }

  // A persisted Error state without its old text must be recomputed so the
  // concrete builder can produce a current diagnostic.
  solidar::Document retryDocument;
  auto& retryBody = retryDocument.addBody("Retry body");
  auto retry = std::make_unique<TestShapeFeature>("Retry feature", false);
  auto* retryPtr = retry.get();
  retryPtr->failWithoutMessage();
  retryBody.addFeature(std::move(retry));
  CHECK(!retryDocument.rebuild());
  CHECK(retryPtr->rebuildCount == 1);
  CHECK(retryDocument.rebuildError() == "test rebuild failure");

  // A failed Body must not prevent independent Bodies from recomputing.
  auto& laterBody = document.addBody("Later body");
  auto later = std::make_unique<TestShapeFeature>("Not reached", true);
  auto* laterPtr = later.get();
  laterBody.addFeature(std::move(later));
  CHECK(!document.rebuild());
  CHECK(!laterPtr->isDirty());
  CHECK(laterPtr->isValid());
  CHECK(laterPtr->rebuildCount == 1);
  CHECK(laterPtr->error().empty());
  CHECK(document.rebuildError() == "test rebuild failure");

  // Geometry-only edits dirty consumers of a face-supported Sketch, but do
  // not recompute its placement. Attachment and upstream Feature rebuilds do.
  {
    solidar::Document placementDocument;
    auto& placementBody = placementDocument.addBody("Placement body");
    auto support = std::make_unique<TestShapeFeature>("Support", true);
    auto* supportPtr = support.get();
    placementBody.addFeature(std::move(support));
    CHECK(placementDocument.recompute());
    auto& supportedSketch =
        placementDocument.addSketch("Face-supported sketch");
    const auto supportedSketchId = supportedSketch.id;
    supportedSketch.geometry.addRectangle({0.1, 0.1}, {0.4, 0.4});
    auto& independentSketch =
        placementDocument.addSketch("Independent face-supported sketch");
    const auto independentSketchId = independentSketch.id;
    independentSketch.geometry.addCircle({0.5, 0.5}, 0.1);
    const auto supportReference = solidar::makeFaceReference(
        *supportPtr->shape(), placementBody.id(), supportPtr->id(), 0);
    CHECK(supportReference.signature);
    CHECK(placementDocument.attachSketchToFace(independentSketchId,
                                                supportReference));
    const auto* independentState =
        placementDocument.findSketch(independentSketchId);
    CHECK(independentState);
    const auto independentPlacementRevision =
        independentState->placementRevision;
    const auto buildsBeforeTargetAttach =
        solidar::TopologyIndex::buildAttemptCount();
    CHECK(placementDocument.attachSketchToFace(supportedSketchId,
                                               supportReference));
    CHECK(independentState->placementRevision ==
          independentPlacementRevision);
    CHECK(solidar::TopologyIndex::buildAttemptCount() ==
          buildsBeforeTargetAttach);
    auto consumer = std::make_unique<TestShapeFeature>(
        "Sketch consumer", true, supportedSketchId);
    auto* consumerPtr = consumer.get();
    placementBody.addFeature(std::move(consumer));
    CHECK(placementDocument.recompute());
    auto* sketchState = placementDocument.findSketch(supportedSketchId);
    CHECK(sketchState && !sketchState->placementDirty);
    const auto placementRevision = sketchState->placementRevision;
    const auto placement = sketchState->placement;
    const auto consumerRebuilds = consumerPtr->rebuildCount;

    solidar::sketch::Sketch geometryEdit;
    geometryEdit.addRectangle({0.2, 0.2}, {0.6, 0.6});
    CHECK(placementDocument.replaceSketchGeometry(supportedSketchId,
                                                   geometryEdit));
    CHECK(consumerPtr->isDirty());
    CHECK(!sketchState->placementDirty);
    CHECK(independentState->placementRevision ==
          independentPlacementRevision);
    CHECK(solidar::TopologyIndex::buildAttemptCount() ==
          buildsBeforeTargetAttach);
    CHECK(placementDocument.recompute());
    CHECK(consumerPtr->rebuildCount == consumerRebuilds + 1);
    CHECK(sketchState->placementRevision == placementRevision);
    CHECK(sketchState->placement.origin.x == placement.origin.x);
    CHECK(sketchState->placement.origin.y == placement.origin.y);
    CHECK(sketchState->placement.origin.z == placement.origin.z);
    CHECK(sketchState->placement.xDirection.x == placement.xDirection.x);
    CHECK(sketchState->placement.xDirection.y == placement.xDirection.y);
    CHECK(sketchState->placement.xDirection.z == placement.xDirection.z);
    CHECK(sketchState->placement.yDirection.x == placement.yDirection.x);
    CHECK(sketchState->placement.yDirection.y == placement.yDirection.y);
    CHECK(sketchState->placement.yDirection.z == placement.yDirection.z);

    supportPtr->setDirty();
    CHECK(placementDocument.recompute());
    CHECK(sketchState->placementRevision == placementRevision + 1);
  }

  document.setBox({100.0, 50.0, 12.0});
  CHECK(document.box().widthMm == 100.0);

  bool rejected = false;
  try {
    document.setBox({0.0, 50.0, 12.0});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);

  solidar::sketch::Sketch sketch;
  CHECK(sketch.lines().empty());
  CHECK(sketch.circles().empty());
  sketch.setRectangle(80.0, 35.0);
  CHECK(sketch.lines().size() == 4);
  CHECK(sketch.isClosed());
  CHECK(sketch.widthMm() == 80.0);
  const auto rectangleId = sketch.lines().front().elementId;
  sketch.translateElement(rectangleId, 10.0, 5.0);
  CHECK(sketch.lines().front().start.xMm == -30.0);
  CHECK(sketch.lines().front().start.yMm == -12.5);
  for (const auto& line : sketch.lines()) CHECK(line.elementId == rectangleId);

  sketch.addCircle({0.0, 0.0}, 5.0);
  sketch.translateCircle(0, 15.0, -10.0);
  CHECK(sketch.circles().front().center.xMm == 15.0);
  CHECK(sketch.circles().front().center.yMm == -10.0);
  bool sketchRejected = false;
  try {
    sketch.setRectangle(-1.0, 35.0);
  } catch (const std::invalid_argument&) {
    sketchRejected = true;
  }
  CHECK(sketchRejected);
  return 0;
}
