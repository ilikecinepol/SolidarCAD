#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <memory>
#include <numbers>

#include "TestGeometryUtils.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/ExtrudeOperationDetector.h"
#include "model/FilletFeature.h"

int main() {
  // NewBody is the first and only body-creating feature in a Body.
  solidar::Document invalidNewBody;
  auto& firstSketch = invalidNewBody.addSketch("First");
  firstSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
  const auto firstSketchId = firstSketch.id;
  auto& secondSketch = invalidNewBody.addSketch("Second");
  secondSketch.geometry.addRectangle({0.0, 0.0}, {5.0, 5.0});
  const auto secondSketchId = secondSketch.id;
  auto& invalidBody = invalidNewBody.addBody();
  invalidBody.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(firstSketchId, 10.0));
  invalidBody.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(secondSketchId, 5.0));
  assert(!invalidNewBody.rebuild());
  assert(invalidBody.activeFeature()->error().find("first feature") !=
         std::string::npos);
  assert(!invalidBody.resultShape());

  // Independent bodies retain their IDs and geometry while the other changes.
  solidar::Document multiBody;
  auto& rectangle = multiBody.addSketch("Body 1 rectangle");
  rectangle.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
  auto& body1 = multiBody.addBody("Body001");
  auto first =
      std::make_unique<solidar::ExtrudeFeature>(rectangle.id, 30.0, "First");
  auto* firstExtrude = first.get();
  body1.addFeature(std::move(first));
  auto& circle = multiBody.addSketch("Body 2 circle");
  circle.geometry.addCircle({100.0, 0.0}, 5.0);
  auto& body2 = multiBody.addBody("Body002");
  body2.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(circle.id, 12.0, "Second"));
  assert(multiBody.rebuild());
  assert(multiBody.bodies().size() == 2);
  const auto body2Id = body2.id();
  const double body2Volume = solidar::test::volumeOf(*body2.resultShape());
  firstExtrude->setLengthMm(50.0);
  assert(multiBody.rebuild());
  assert(solidar::test::near(
      solidar::test::volumeOf(*multiBody.findBody(body2Id)->resultShape()),
      body2Volume, 1e-4));

  // Snapshot is a deep copy and models the restore operation used by UI Undo.
  const solidar::Document snapshot = multiBody;
  const auto firstId = firstExtrude->id();
  firstExtrude->setLengthMm(80.0);
  firstExtrude->setReversed(true);
  assert(multiBody.rebuild());
  const auto* snapshotFeature = dynamic_cast<const solidar::ExtrudeFeature*>(
      snapshot.bodies().front().features().front().get());
  assert(snapshotFeature && snapshotFeature->id() == firstId);
  assert(solidar::test::near(snapshotFeature->lengthMm(), 50.0));
  assert(!snapshotFeature->reversed());
  assert(solidar::test::near(
      solidar::test::volumeOf(*snapshot.bodies().front().resultShape()),
      20.0 * 10.0 * 50.0));
  multiBody = snapshot;
  assert(multiBody.bodies().size() == 2);
  assert(multiBody.bodies().front().features().size() == 1);

  // Extrude -> attached Join -> attached Cut, followed by parent edit.
  solidar::Document history;
  auto& baseSketch = history.addSketch("Base");
  baseSketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  auto& historyBody = history.addBody();
  auto base =
      std::make_unique<solidar::ExtrudeFeature>(baseSketch.id, 50.0, "Base");
  auto* baseExtrude = base.get();
  historyBody.addFeature(std::move(base));
  assert(history.rebuild());
  auto top = solidar::test::topPlanarFace(*historyBody.resultShape(), 50.0);
  assert(top);
  const auto baseTopFace = *top;
  auto& joinSketch = history.addSketch("Offset join");
  joinSketch.geometry.addRectangle({-25.0, 5.0}, {-15.0, 15.0});
  const auto joinSketchId = joinSketch.id;
  assert(history.attachSketchToFace(
      joinSketch.id, {historyBody.id(), baseExtrude->id(), *top}));
  historyBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      joinSketch.id, 20.0, "Join", solidar::ExtrudeOperation::Join));
  assert(history.rebuild());
  auto& cutSketch = history.addSketch("Cut");
  cutSketch.geometry.addCircle({10.0, 10.0}, 3.0);
  assert(history.attachSketchToFace(
      cutSketch.id,
      {historyBody.id(), baseExtrude->id(), baseTopFace}));
  historyBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      cutSketch.id, 10.0, "Cut", solidar::ExtrudeOperation::Cut, true));
  assert(history.rebuild());
  assert(historyBody.features().size() == 3);
  const double expected50 =
      140000.0 + 2000.0 - std::numbers::pi * 9.0 * 10.0;
  assert(solidar::test::near(solidar::test::volumeOf(*historyBody.resultShape()),
                             expected50, 1e-2));
  baseExtrude->setLengthMm(80.0);
  assert(history.rebuild());
  const auto* movedJoinSketch = history.findSketch(joinSketchId);
  assert(movedJoinSketch && movedJoinSketch->supportResolved);
  assert(solidar::test::near(movedJoinSketch->placement.origin.z, 80.0));
  assert(solidar::test::near(
      movedJoinSketch->geometry.lines().front().start.xMm, -25.0));
  assert(solidar::test::near(
      movedJoinSketch->geometry.lines().front().start.yMm, 5.0));
  assert(historyBody.features()[1]->isValid());
  assert(historyBody.features()[2]->isValid());
  const double expected80 =
      224000.0 + 2000.0 - std::numbers::pi * 9.0 * 10.0;
  assert(solidar::test::near(solidar::test::volumeOf(*historyBody.resultShape()),
                             expected80, 1e-2));
  baseExtrude->setLengthMm(0.0);
  assert(!history.rebuild());
  assert(!baseExtrude->hasShape() && !historyBody.resultShape());
  baseExtrude->setLengthMm(60.0);
  assert(history.rebuild());
  assert(historyBody.activeFeature()->isValid() && historyBody.resultShape());

  // Detector policies: no target, separate prism, base-plane intersection,
  // and face-supported outward/inward directions.
  const auto target = historyBody.resultShape();
  const auto& attachedProfile = *history.findSketch(joinSketchId);
  assert(solidar::detectExtrudeOperation(attachedProfile, 5.0, false, nullptr,
                                         true) ==
         solidar::ExtrudeOperation::NewBody);
  assert(solidar::detectExtrudeOperation(attachedProfile, 5.0, false,
                                         target.get(), true) ==
         solidar::ExtrudeOperation::Join);
  assert(solidar::detectExtrudeOperation(attachedProfile, 5.0, true,
                                         target.get(), true) ==
         solidar::ExtrudeOperation::Cut);
  auto basePlane = attachedProfile;
  basePlane.support.type = solidar::SketchSupportType::BasePlane;
  basePlane.placement.origin.z = 55.0;
  assert(solidar::detectExtrudeOperation(basePlane, 10.0, false, target.get(),
                                         false) ==
         solidar::ExtrudeOperation::Join);
  basePlane.placement.origin.x = 500.0;
  assert(solidar::detectExtrudeOperation(basePlane, 10.0, false, target.get(),
                                         false) ==
         solidar::ExtrudeOperation::NewBody);
}
