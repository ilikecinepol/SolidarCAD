#include "TestAssertions.h"

#include "ui/SketchCommandController.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace solidar;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

sketch::GeometryId addLine(SketchCommandController& controller,
                           sketch::Sketch& model, sketch::Point first,
                           sketch::Point second) {
  const auto result =
      controller.execute(model, AddLineCommand{first, second, std::nullopt});
  CHECK(result.accepted);
  CHECK(result.changedGeometryIds.size() == 1);
  CHECK(result.effects.geometryChanged);
  CHECK(result.effects.committedRenderSceneDirty);
  return result.changedGeometryIds.front();
}

void atomicBatchRollbackPreservesOuterTransaction() {
  sketch::Sketch model;
  SketchCommandController controller;
  const auto token = controller.beginTransaction(model, 1);
  CHECK(token.has_value());
  const auto before = model.semanticFingerprint();

  AddPrimitiveBatchCommand batch;
  batch.primitives.push_back(
      AddLineCommand{{0.0, 0.0}, {1.0, 0.0}, std::nullopt});
  batch.primitives.push_back(AddCircleCommand{{0.0, 0.0}, -1.0, false});
  const auto rejected = controller.execute(model, batch);
  CHECK(!rejected.accepted);
  CHECK(model.semanticFingerprint() == before);
  CHECK(model.deltaJournalDepth() == token->journalDepthBefore + 1);

  CHECK(controller
            .execute(model,
                     AddLineCommand{{0.0, 0.0}, {2.0, 0.0}, std::nullopt})
            .accepted);
  const auto delta = controller.finishTransaction(model, *token, 1);
  CHECK(delta.has_value());
  CHECK(!delta->empty());
  CHECK(model.lines().size() == 1);
}

void transactionTokensAreBoundAndInvalidationUnwinds() {
  sketch::Sketch first;
  sketch::Sketch second;
  SketchCommandController controller;
  const auto token = controller.beginTransaction(first, 7);
  CHECK(token.has_value());
  CHECK(first.deltaJournalDepth() == 1);

  auto altered = *token;
  ++altered.initialFingerprint;
  CHECK(!controller.finishTransaction(first, altered, 7));
  CHECK(controller.hasActiveTransaction());
  CHECK(!controller.finishTransaction(second, *token, 7));
  CHECK(controller.hasActiveTransaction());
  CHECK(!controller.finishTransaction(first, *token, 8));
  CHECK(controller.hasActiveTransaction());

  CHECK(controller.invalidateTransactions(first, 7));
  CHECK(!controller.hasActiveTransaction());
  CHECK(first.deltaJournalDepth() == 0);
  const auto next = controller.beginTransaction(first, 8);
  CHECK(next.has_value());
  CHECK(controller.cancelTransaction(first, *next, 8));
}

void nestedJournalExceptionIsContainedAndUnwound() {
  sketch::Sketch model;
  SketchCommandController controller;
  const auto lineId = addLine(controller, model, {0.0, 0.0}, {5.0, 0.0});
  const auto before = model.semanticFingerprint();
  const auto depth = model.deltaJournalDepth();
  sketch::Constraint horizontal;
  horizontal.type = sketch::ConstraintType::Horizontal;
  horizontal.firstGeometry = lineId;
  sketch::Sketch::failNextNestedConstraintJournalForTesting();
  const auto result =
      controller.execute(model, AddConstraintCommand{horizontal});
  CHECK(!result.accepted);
  CHECK(result.error == SketchCommandError::InternalFailure);
  CHECK(model.semanticFingerprint() == before);
  CHECK(model.deltaJournalDepth() == depth);
  CHECK(model.constraints().empty());
}

void liveCommandsReuseTheOuterJournal() {
  sketch::Sketch model;
  SketchCommandController controller;
  const auto lineId = addLine(controller, model, {0.0, 0.0}, {5.0, 0.0});
  sketch::Sketch::resetDeltaJournalBeginCountForTesting();
  const auto token = controller.beginTransaction(model, 1);
  CHECK(token.has_value());
  CHECK(sketch::Sketch::deltaJournalBeginCountForTesting() == 1);

  const auto moved = controller.executeInTransaction(
      model, *token, 1,
      TranslatePointCommand{sketch::PointReference{lineId, true}, 2.0, 3.0});
  CHECK(moved.accepted);
  CHECK(sketch::Sketch::deltaJournalBeginCountForTesting() == 1);
  CHECK(controller.finishTransaction(model, *token, 1).has_value());
  CHECK(model.lines().front().start.xMm == 2.0);
  CHECK(model.lines().front().start.yMm == 3.0);
}

void creationJournalWorkHasDeterministicBounds() {
  sketch::Sketch model;
  SketchCommandController controller;
  const auto carrier = addLine(controller, model, {0.0, 5.0}, {20.0, 5.0});
  (void)carrier;
  sketch::Sketch::resetDeltaJournalBeginCountForTesting();
  const auto rectangle = controller.execute(
      model, AddRectangleCommand{{5.0, 5.0}, {10.0, 10.0}, std::nullopt,
                                 std::nullopt, false});
  CHECK(rectangle.accepted);
  CHECK(rectangle.changedGeometryIds.size() == 4);
  CHECK(controller
            .execute(model, AutoConstrainNewGeometryCommand{
                                rectangle.changedGeometryIds, 1e-4})
            .accepted);
  // One creation journal, one auto-command journal, and a bounded number of
  // nested constraint journals for four rectangle corners/axis relations.
  CHECK(sketch::Sketch::deltaJournalBeginCountForTesting() <= 20);

  sketch::Sketch::resetDeltaJournalBeginCountForTesting();
  const auto circle = controller.execute(
      model, AddCircleCommand{{0.0, 0.0}, 2.0, false});
  CHECK(circle.accepted);
  CHECK(controller
            .execute(model, AutoConstrainNewGeometryCommand{
                                circle.changedGeometryIds, 1e-4})
            .accepted);
  CHECK(sketch::Sketch::deltaJournalBeginCountForTesting() <= 5);
}

void constraintsUseStrictSchemas() {
  sketch::Sketch model;
  SketchCommandController controller;
  const auto first = addLine(controller, model, {0.0, 0.0}, {5.0, 0.0});
  const auto second = addLine(controller, model, {0.0, 1.0}, {5.0, 1.0});

  sketch::Constraint malformedLock;
  malformedLock.type = sketch::ConstraintType::Lock;
  malformedLock.firstGeometry = first;
  malformedLock.secondGeometry = second;
  CHECK(!controller.execute(model, AddConstraintCommand{malformedLock})
             .accepted);

  sketch::Constraint wrongCarrier;
  wrongCarrier.type = sketch::ConstraintType::PointOnCircle;
  wrongCarrier.firstGeometry = first;
  wrongCarrier.secondPoint = {second, true};
  CHECK(!controller.execute(model, AddConstraintCommand{wrongCarrier})
             .accepted);

  sketch::Constraint horizontal;
  horizontal.type = sketch::ConstraintType::Horizontal;
  horizontal.firstGeometry = first;
  const auto valid = controller.execute(model, AddConstraintCommand{horizontal});
  CHECK(valid.accepted);
  CHECK(valid.changedConstraintIds.size() == 1);
  CHECK(valid.effects.constraintsChanged);
}

void stableIdsDriveAutoConstraints() {
  sketch::Sketch model;
  SketchCommandController controller;
  const auto removed = addLine(controller, model, {-5.0, -5.0}, {-4.0, -5.0});
  const auto carrier = addLine(controller, model, {0.0, 0.0}, {10.0, 0.0});
  CHECK(controller.execute(model, RemoveGeometryCommand{removed}).accepted);
  const auto created = addLine(controller, model, {5.0, 0.0}, {5.0, 3.0});

  const auto result = controller.execute(
      model, AutoConstrainNewGeometryCommand{{created}, 1e-4});
  CHECK(result.accepted);
  CHECK(!result.changedConstraintIds.empty());
  bool found = false;
  for (const auto& constraint : model.constraints()) {
    if (constraint.type == sketch::ConstraintType::PointOnLine &&
        constraint.firstGeometry == carrier &&
        constraint.secondPoint.lineId == created && constraint.secondPoint.start)
      found = true;
  }
  CHECK(found);
}

void arcStyleAndStaleDeltaAreSafe() {
  sketch::Sketch model;
  SketchCommandController controller;
  const auto arc = controller.execute(
      model, AddArcCommand{{0.0, 0.0}, 5.0, 0.0, 1.0, false});
  CHECK(arc.accepted && arc.changedGeometryIds.size() == 1);
  const auto arcId = arc.changedGeometryIds.front();
  const auto styled =
      controller.execute(model, SetArcDashedCommand{arcId, true});
  CHECK(styled.accepted);
  CHECK(model.arcs().front().dashed);

  sketch::Sketch source;
  const auto sourceLine = addLine(controller, source, {0.0, 0.0}, {1.0, 0.0});
  (void)sourceLine;
  const auto delta = controller.execute(
      source, AddCircleCommand{{2.0, 2.0}, 1.0, false});
  CHECK(delta.accepted && !delta.delta.empty());
  const auto fingerprint = model.semanticFingerprint();
  const auto stale =
      controller.execute(model, ApplySketchDeltaCommand{&delta.delta, true});
  CHECK(!stale.accepted);
  CHECK(model.semanticFingerprint() == fingerprint);
}

void lockedAndNoOpMutationsReportTruthfully() {
  sketch::Sketch model;
  SketchCommandController controller;
  const auto line = addLine(controller, model, {0.0, 0.0}, {5.0, 0.0});
  const auto elementId = model.lines().front().elementId;
  const auto circleResult =
      controller.execute(model, AddCircleCommand{{10.0, 10.0}, 2.0, false});
  CHECK(circleResult.accepted);
  const auto circle = circleResult.changedGeometryIds.front();
  const auto arcResult = controller.execute(
      model, AddArcCommand{{20.0, 20.0}, 3.0, 0.0, 1.0, false});
  CHECK(arcResult.accepted);
  const auto arc = arcResult.changedGeometryIds.front();

  const auto noOp =
      controller.execute(model, SetLineDashedCommand{line, false});
  CHECK(noOp.accepted);
  CHECK(noOp.delta.empty());
  CHECK(!noOp.effects.geometryChanged);
  CHECK(noOp.changedGeometryIds.empty());

  for (const auto id : {line, circle, arc}) {
    sketch::Constraint lock;
    lock.type = sketch::ConstraintType::Lock;
    lock.firstGeometry = id;
    CHECK(controller.execute(model, AddConstraintCommand{lock}).accepted);
  }
  const auto fingerprint = model.semanticFingerprint();
  const auto lineStyle =
      controller.execute(model, SetLineDashedCommand{line, true});
  const auto circleStyle =
      controller.execute(model, SetCircleDashedCommand{circle, true});
  const auto arcStyle =
      controller.execute(model, SetArcDashedCommand{arc, true});
  const auto elementStyle =
      controller.execute(model, SetElementDashedCommand{elementId, true});
  const auto selectionStyle = controller.execute(
      model, SetSelectionDashedCommand{{line}, {elementId}, {circle}, {arc},
                                       true});
  for (const auto* result : {&lineStyle, &circleStyle, &arcStyle,
                             &elementStyle, &selectionStyle}) {
    CHECK(!result->accepted);
    CHECK(result->error == SketchCommandError::Conflict);
  }
  CHECK(model.semanticFingerprint() == fingerprint);

  const auto staleSelection = controller.execute(
      model, TranslateSelectionCommand{{999999}, {}, {}, 1.0, 1.0});
  CHECK(!staleSelection.accepted);
  CHECK(staleSelection.error == SketchCommandError::StaleReference);

  const auto token = controller.beginTransaction(model, 1);
  CHECK(token.has_value());
  const auto lockedLines = controller.executeInTransaction(
      model, *token, 1, TranslateLinesCommand{{line}, 1.0, 0.0});
  const auto lockedCircle = controller.executeInTransaction(
      model, *token, 1, TranslateCircleCommand{circle, 1.0, 0.0});
  const auto lockedArc = controller.executeInTransaction(
      model, *token, 1, TranslateArcCommand{arc, 1.0, 0.0});
  const auto lockedSelection = controller.executeInTransaction(
      model, *token, 1,
      TranslateSelectionCommand{{elementId}, {circle}, {arc}, 1.0, 0.0});
  for (const auto* result : {&lockedLines, &lockedCircle, &lockedArc,
                             &lockedSelection}) {
    CHECK(!result->accepted);
    CHECK(result->error == SketchCommandError::Conflict);
  }
  CHECK(controller.cancelTransaction(model, *token, 1));
  CHECK(model.semanticFingerprint() == fingerprint);
}

void representativeCompoundCommandsPublishTypedEffects() {
  sketch::Sketch model;
  SketchCommandController controller;
  const auto axis = addLine(controller, model, {0.0, -5.0}, {0.0, 5.0});
  const auto source = addLine(controller, model, {2.0, 0.0}, {4.0, 0.0});
  const auto mirrored =
      controller.execute(model, MirrorGeometryCommand{axis, {source}});
  CHECK(mirrored.accepted);
  CHECK(!mirrored.changedGeometryIds.empty());
  CHECK(mirrored.effects.geometryChanged);

  const auto trimmed = controller.execute(
      model, TrimGeometryCommand{source, 0.25, 0.75, false});
  CHECK(trimmed.accepted);
  CHECK(trimmed.effects.selectionMayBeStale);
  CHECK(trimmed.effects.dimensionsChanged);
}

}  // namespace

int main() {
  atomicBatchRollbackPreservesOuterTransaction();
  transactionTokensAreBoundAndInvalidationUnwinds();
  nestedJournalExceptionIsContainedAndUnwound();
  liveCommandsReuseTheOuterJournal();
  creationJournalWorkHasDeterministicBounds();
  constraintsUseStrictSchemas();
  stableIdsDriveAutoConstraints();
  arcStyleAndStaleDeltaAreSafe();
  lockedAndNoOpMutationsReportTruthfully();
  representativeCompoundCommandsPublishTypedEffects();
  std::cout << "Sketch command controller tests passed\n";
  return EXIT_SUCCESS;
}
