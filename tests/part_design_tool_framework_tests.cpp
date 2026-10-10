#include "TestAssertions.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

#include "model/ChamferToolSession.h"
#include "model/Document.h"
#include "model/DraftToolSession.h"
#include "model/FilletToolSession.h"
#include "model/ShellToolSession.h"
#include "model/TopologyReferenceResolver.h"
#include "ui/tools/PartDesignToolController.h"
#include "ui/application/PartDesignCoordinator.h"
#include "ui/PartDesignToolHelp.h"
#include "ui/PartDesignErrorLocalization.h"

namespace {
class FakeSession final : public solidar::ToolSession {
 public:
  solidar::ToolLifecycle lifecycleValue{solidar::ToolLifecycle::SelectingInput};
  solidar::ToolSelectionStage stage{solidar::ToolSelectionStage::SelectingInput};
  bool cancelled{};
  solidar::ToolLifecycle lifecycle() const noexcept override { return lifecycleValue; }
  solidar::ToolSelectionStage selectionStage() const noexcept override { return stage; }
  std::shared_ptr<const TopoDS_Shape> previewShape() const override { return {}; }
  const std::string& error() const noexcept override { return errorValue; }
  bool updatePreview() override { return false; }
  void cancel() noexcept override {
    cancelled = true;
    lifecycleValue = solidar::ToolLifecycle::Inactive;
  }
 private:
  std::string errorValue;
};
}

int main() {
  // Localization and UI branching depend only on the stable code. Technical
  // diagnostics may change (including exception text) without changing the
  // message shown to the user.
  {
    const solidar::OperationFailure first{
        solidar::OperationFailureCode::TopologyReferenceMissing,
        "old diagnostic"};
    const solidar::OperationFailure second{
        solidar::OperationFailureCode::TopologyReferenceMissing,
        "completely different diagnostic"};
    CHECK(solidar::localizedFaceToolError(first) ==
          solidar::localizedFaceToolError(second));
    CHECK(solidar::localizedPartDesignError(
              solidar::PartDesignToolKind::Fillet, first) ==
          solidar::localizedPartDesignError(
              solidar::PartDesignToolKind::Fillet, second));

    const solidar::OperationFailure unknown{
        solidar::OperationFailureCode::Unknown,
        "must not be exposed to the user"};
    CHECK(solidar::localizedPartDesignError(
              solidar::PartDesignToolKind::None, unknown) ==
          QString::fromUtf8("Не удалось построить предпросмотр операции."));

    const solidar::OperationFailure stale{
        solidar::OperationFailureCode::TopologyReferenceMissing,
        "identical diagnostic"};
    const solidar::OperationFailure invalidInput{
        solidar::OperationFailureCode::InvalidInput,
        "identical diagnostic"};
    CHECK(solidar::localizedPartDesignError(
              solidar::PartDesignToolKind::Draft, stale) !=
          solidar::localizedPartDesignError(
              solidar::PartDesignToolKind::Draft, invalidInput));

    constexpr std::array allCodes{
        solidar::OperationFailureCode::None,
        solidar::OperationFailureCode::Unknown,
        solidar::OperationFailureCode::InvalidInput,
        solidar::OperationFailureCode::MissingSource,
        solidar::OperationFailureCode::TopologyIndexUnavailable,
        solidar::OperationFailureCode::TopologyReferenceMismatch,
        solidar::OperationFailureCode::TopologyReferenceMissing,
        solidar::OperationFailureCode::TopologyReferenceAmbiguous,
        solidar::OperationFailureCode::TopologyReferenceInvalid,
        solidar::OperationFailureCode::TopologyResolutionUnexpected,
        solidar::OperationFailureCode::NonPlanarFace,
        solidar::OperationFailureCode::UnsupportedSurface,
        solidar::OperationFailureCode::UnsupportedGeometry,
        solidar::OperationFailureCode::NoIntersection,
        solidar::OperationFailureCode::InvalidProfileOpen,
        solidar::OperationFailureCode::InvalidProfileOverlap,
        solidar::OperationFailureCode::InvalidProfile,
        solidar::OperationFailureCode::BodiesDoNotTouch,
        solidar::OperationFailureCode::GeometryOperationFailed,
    };
    constexpr std::array allToolKinds{
        solidar::PartDesignToolKind::None,
        solidar::PartDesignToolKind::Extrude,
        solidar::PartDesignToolKind::Pocket,
        solidar::PartDesignToolKind::Revolve,
        solidar::PartDesignToolKind::Fillet,
        solidar::PartDesignToolKind::Chamfer,
        solidar::PartDesignToolKind::JoinBodies,
        solidar::PartDesignToolKind::Move,
        solidar::PartDesignToolKind::Mirror,
        solidar::PartDesignToolKind::LinearPattern,
        solidar::PartDesignToolKind::CircularPattern,
        solidar::PartDesignToolKind::Shell,
        solidar::PartDesignToolKind::Draft,
    };
    const QString reselectionMessage = QString::fromUtf8(
        "Выбранная геометрия больше не соответствует текущей модели. "
        "Выберите её заново.");
    const QString faceReselectionMessage = QString::fromUtf8(
        "Не удалось восстановить выбранную грань после изменения модели.");
    const QString faceAmbiguousMessage = QString::fromUtf8(
        "Выбранная грань не может быть однозначно определена.");
    for (const auto code : allCodes) {
      const bool expectedReselection =
          code == solidar::OperationFailureCode::TopologyReferenceMismatch ||
          code == solidar::OperationFailureCode::TopologyReferenceMissing ||
          code == solidar::OperationFailureCode::TopologyReferenceAmbiguous;
      CHECK(solidar::requiresPartDesignReselection(code) ==
            expectedReselection);
      for (const auto kind : allToolKinds) {
        const solidar::OperationFailure failure{code, "policy diagnostic"};
        CHECK((solidar::localizedPartDesignError(kind, failure) ==
               reselectionMessage) == expectedReselection);
      }
      const QString faceMessage = solidar::localizedFaceToolError(
          {code, "face policy diagnostic"});
      CHECK(((faceMessage == faceReselectionMessage) ||
             (faceMessage == faceAmbiguousMessage)) == expectedReselection);
    }
  }

  const auto& definitions = solidar::standardPartDesignToolDefinitions();
  CHECK(definitions.size() == 12);
  for (const auto& definition : definitions) {
    CHECK(definition.kind != solidar::PartDesignToolKind::None);
    CHECK(!definition.selections.empty());
    const auto* help = solidar::partDesignToolHelp(definition.kind);
    CHECK(help != nullptr);
    CHECK(!help->title.isEmpty());
    CHECK(!help->detailedDescription.isEmpty());
  }
  const auto& join = definitions[5];
  CHECK(join.selections.size() == 1);
  CHECK(join.selections.front().minimumCount == 2);
  CHECK(join.selections.front().maximumCount == 2);
  CHECK(join.selections.front().multiSelect);
  CHECK(join.parameters.empty());
  const auto& move = definitions[6];
  CHECK(solidar::acceptsSelection(move.selections[0],
                                  solidar::SelectionType::Body));
  CHECK(move.parameters.size() == 3);
  CHECK(std::get<double>(move.parameters[0].value) == 0.0);
  CHECK(move.parameters[0].minimum < 0.0);
  const auto& mirror = definitions[7];
  CHECK(solidar::acceptsSelection(mirror.selections[0],
                                  solidar::SelectionType::Body));
  CHECK(!solidar::acceptsSelection(mirror.selections[0],
                                   solidar::SelectionType::Feature));
  CHECK(solidar::acceptsSelection(mirror.selections[1],
                                  solidar::SelectionType::Plane));
  CHECK(!solidar::acceptsSelection(mirror.selections[1],
                                   solidar::SelectionType::Body));
  const auto& linear = definitions[8];
  CHECK(solidar::acceptsSelection(linear.selections[0],
                                  solidar::SelectionType::Body));
  CHECK(!solidar::acceptsSelection(linear.selections[0],
                                   solidar::SelectionType::Feature));
  CHECK(solidar::acceptsSelection(linear.selections[1],
                                  solidar::SelectionType::Axis));
  const auto& circular = definitions[9];
  CHECK(solidar::acceptsSelection(circular.selections[0],
                                  solidar::SelectionType::Body));
  CHECK(!solidar::acceptsSelection(circular.selections[0],
                                   solidar::SelectionType::Feature));
  CHECK(solidar::acceptsSelection(circular.selections[1],
                                  solidar::SelectionType::Axis));
  const auto& draft = definitions[11];
  CHECK(draft.kind == solidar::PartDesignToolKind::Draft);
  CHECK(draft.selections.size() == 2);
  CHECK(draft.selections[0].type == solidar::SelectionType::Face);
  CHECK(draft.selections[0].maximumCount == 1);
  CHECK(!draft.selections[0].multiSelect);
  CHECK(draft.selections[1].type == solidar::SelectionType::Axis);
  CHECK(draft.parameters.size() == 1);
  CHECK(draft.parameters.front().id == "angle");
  CHECK(draft.parameters.front().minimum == -89.99);
  CHECK(draft.parameters.front().maximum == 89.99);
  const auto& revolveDefinition = definitions[2];
  CHECK(revolveDefinition.selections.front().multiSelect);
  CHECK(revolveDefinition.selections.front().maximumCount > 1);
  CHECK(solidar::partDesignToolStepHint(
            solidar::PartDesignToolKind::Revolve,
            solidar::ToolSelectionStage::SelectingInput) ==
        solidar::partDesignToolHelp(
            solidar::PartDesignToolKind::Revolve)->selectionHint);
  solidar::PartDesignToolController controller;
  FakeSession revolve;
  FakeSession fillet;
  controller.registerTool(solidar::PartDesignToolKind::Revolve,
                          {&revolve});
  controller.registerTool(solidar::PartDesignToolKind::Fillet,
                          {&fillet});

  controller.activate(solidar::PartDesignToolKind::Revolve);
  CHECK(controller.activeSession() == &revolve);
  controller.beginReselection(solidar::ToolSelectionStage::SelectingReference);
  CHECK(controller.isReselecting());
  CHECK(controller.handleEscape());
  CHECK(!revolve.cancelled);
  CHECK(controller.activeTool() == solidar::PartDesignToolKind::Revolve);

  revolve.cancelled = false;
  controller.activate(solidar::PartDesignToolKind::Revolve);
  CHECK(revolve.cancelled);

  controller.activate(solidar::PartDesignToolKind::Fillet);
  CHECK(revolve.cancelled);
  CHECK(controller.activeTool() == solidar::PartDesignToolKind::Fillet);
  CHECK(!controller.handleEscape());
  CHECK(fillet.cancelled);
  CHECK(controller.activeTool() == solidar::PartDesignToolKind::None);

  // The application coordinator owns all actual sessions and exposes one
  // revisioned lifecycle. Nested Escape only leaves re-selection; a second
  // Escape cancels the whole active tool. No widget callback participates.
  {
    solidar::PartDesignCoordinator coordinator;
    CHECK(coordinator.sessionCount() == 11);
    CHECK(coordinator.invariantHolds());
    const auto initial = coordinator.revisionToken();

    // A command for a non-active kind is ignored and cannot activate or
    // mutate that session through the public API.
    coordinator.setMoveOffset({4.0, 5.0, 6.0});
    CHECK(coordinator.activeTool() == solidar::PartDesignToolKind::None);
    const auto inactiveOffset =
        coordinator.snapshot(solidar::PartDesignToolKind::Move).offsetMm;
    CHECK(inactiveOffset.x == 0.0 && inactiveOffset.y == 0.0 &&
          inactiveOffset.z == 0.0);

    const auto activated = coordinator.beginMove();
    CHECK(activated.effect ==
          solidar::PartDesignTransitionEffect::Activated);
    CHECK(coordinator.isCurrent(activated.revision));
    CHECK(!coordinator.isCurrent(initial));
    CHECK(coordinator.invariantHolds());

    coordinator.beginReselection(
        solidar::ToolSelectionStage::SelectingInput);
    const auto nested =
        coordinator.dispatchActiveAction(solidar::PartDesignAction::Escape);
    CHECK(nested.transition.effect ==
          solidar::PartDesignTransitionEffect::ReselectionCancelled);
    CHECK(coordinator.activeTool() == solidar::PartDesignToolKind::Move);
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Move).lifecycle !=
          solidar::ToolLifecycle::Inactive);

    const auto cancelled =
        coordinator.dispatchActiveAction(solidar::PartDesignAction::Escape);
    CHECK(cancelled.transition.effect ==
          solidar::PartDesignTransitionEffect::Cancelled);
    CHECK(coordinator.activeTool() == solidar::PartDesignToolKind::None);
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Move).lifecycle ==
          solidar::ToolLifecycle::Inactive);
    CHECK(coordinator.invariantHolds());

    const auto box = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());

    // ClearSelection is one active-tool command, not a MainWindow lifecycle
    // chain. Every selection-backed session is routed internally; inactive is
    // a strict no-op.
    solidar::Document clearDocument;
    const auto face = solidar::makeFaceReference(*box, 1, 2, 0);
    const auto edge = solidar::makeEdgeReference(*box, 1, 2, 0);
    const auto checkCleared = [&](solidar::PartDesignToolKind kind) {
      const auto effect = coordinator.dispatchActiveAction(
          solidar::PartDesignAction::ClearSelection, &clearDocument);
      return effect.transition.effect ==
                 solidar::PartDesignTransitionEffect::SelectionCleared &&
             effect.transition.activeTool == kind && effect.clearSelection &&
             effect.preserveSelectionMode && coordinator.invariantHolds();
    };
    CHECK(coordinator.beginFillet(1, 2, box, {edge}, 1.0).effect ==
          solidar::PartDesignTransitionEffect::Activated);
    CHECK(checkCleared(solidar::PartDesignToolKind::Fillet));
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Fillet)
              .edges.empty());
    CHECK(coordinator.beginChamfer(1, 2, box, {edge}, 1.0).effect ==
          solidar::PartDesignTransitionEffect::Activated);
    CHECK(checkCleared(solidar::PartDesignToolKind::Chamfer));
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Chamfer)
              .edges.empty());
    CHECK(coordinator.beginJoinBodies().effect ==
          solidar::PartDesignTransitionEffect::Activated);
    coordinator.setJoinBodies({{1, 2, box}});
    CHECK(checkCleared(solidar::PartDesignToolKind::JoinBodies));
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::JoinBodies)
              .bodies.empty());
    CHECK(coordinator.beginShell(1, 2, box, {face}).effect ==
          solidar::PartDesignTransitionEffect::Activated);
    CHECK(checkCleared(solidar::PartDesignToolKind::Shell));
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Shell)
              .faces.empty());
    CHECK(coordinator.beginDraft(clearDocument, 1, 2, box, {face}).effect ==
          solidar::PartDesignTransitionEffect::Activated);
    CHECK(checkCleared(solidar::PartDesignToolKind::Draft));
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Draft,
                               clearDocument)
              .faces.empty());
    CHECK(coordinator.beginExtrude(1, 2, box, face, 1.0,
                                   solidar::ExtrudeOperation::Join, false)
              .effect == solidar::PartDesignTransitionEffect::Activated);
    CHECK(checkCleared(solidar::PartDesignToolKind::Extrude));
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Extrude)
              .face.bodyId == solidar::kInvalidBodyId);
    coordinator.cancelAll();
    CHECK(coordinator.dispatchActiveAction(
              solidar::PartDesignAction::ClearSelection, &clearDocument)
              .transition.effect ==
          solidar::PartDesignTransitionEffect::None);

    coordinator.beginMove();
    coordinator.setMoveBody(1, 2, box);
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Move).lifecycle ==
          solidar::ToolLifecycle::PreviewValid);
    const auto pendingToken = coordinator.revisionToken();
    {
      auto rejected = coordinator.startCommit();
      CHECK(rejected);
      CHECK(coordinator.commitPending());
    }
    CHECK(!coordinator.commitPending());
    CHECK(coordinator.activeTool() == solidar::PartDesignToolKind::Move);
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Move).lifecycle ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(!coordinator.isCurrent(pendingToken));

    auto accepted = coordinator.startCommit();
    CHECK(accepted);
    const auto reentrantCancel =
        coordinator.dispatchActiveAction(solidar::PartDesignAction::Cancel);
    CHECK(reentrantCancel.transition.effect ==
          solidar::PartDesignTransitionEffect::Rejected);
    CHECK(coordinator.beginMirror().effect ==
          solidar::PartDesignTransitionEffect::Rejected);
    coordinator.setMoveOffset({99.0, 0.0, 0.0});
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Move).offsetMm.x ==
          0.0);
    CHECK(accepted.accept().transition.effect ==
          solidar::PartDesignTransitionEffect::Accepted);
    CHECK(coordinator.activeTool() == solidar::PartDesignToolKind::None);
    CHECK(!coordinator.startCommit());

    coordinator.beginMove();
    const auto switchToken = coordinator.revisionToken();
    coordinator.beginMirror();
    CHECK(!coordinator.isCurrent(switchToken));
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Move).lifecycle ==
          solidar::ToolLifecycle::Inactive);
    const auto allCancelled = coordinator.cancelAll();
    CHECK(allCancelled.effect ==
          solidar::PartDesignTransitionEffect::Cancelled);
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Mirror).lifecycle ==
          solidar::ToolLifecycle::Inactive);
    CHECK(coordinator.invariantHolds());

    const std::array allKinds{
        solidar::PartDesignToolKind::Extrude,
        solidar::PartDesignToolKind::Revolve,
        solidar::PartDesignToolKind::Fillet,
        solidar::PartDesignToolKind::Chamfer,
        solidar::PartDesignToolKind::JoinBodies,
        solidar::PartDesignToolKind::Shell,
        solidar::PartDesignToolKind::Draft,
        solidar::PartDesignToolKind::Mirror,
        solidar::PartDesignToolKind::Move,
        solidar::PartDesignToolKind::LinearPattern,
        solidar::PartDesignToolKind::CircularPattern};
    for (const auto kind : allKinds)
      CHECK(coordinator.snapshot(kind).lifecycle ==
            solidar::ToolLifecycle::Inactive);

    // Document replacement is a coordinator-owned teardown gate. Revolve and
    // Draft retain no document pointer after this call.
    solidar::Document document;
    coordinator.beginRevolve(document, solidar::kInvalidBodyId,
                             solidar::kInvalidFeatureId);
    const auto replacementRevision = coordinator.revisionToken().value;
    CHECK(coordinator.prepareDocumentReplacement().effect ==
          solidar::PartDesignTransitionEffect::Cancelled);
    CHECK(coordinator.revisionToken().value == replacementRevision + 1);
    document = solidar::Document{};
    CHECK(coordinator.invariantHolds());

    // Source-backed tools reject malformed starts atomically. Neither the
    // controller nor a half-initialized session can remain active.
    CHECK(coordinator.beginFillet(solidar::kInvalidBodyId, 2, {}, {}, 1.0)
              .effect == solidar::PartDesignTransitionEffect::Rejected);
    CHECK(coordinator.beginChamfer(1, solidar::kInvalidFeatureId, {}, {}, 1.0)
              .effect == solidar::PartDesignTransitionEffect::Rejected);
    CHECK(coordinator.beginShell(1, 2, {}, {}, 1.0)
              .effect == solidar::PartDesignTransitionEffect::Rejected);
    CHECK(coordinator.beginDraft(document, 1, 2, {}, {})
              .effect == solidar::PartDesignTransitionEffect::Rejected);
    CHECK(coordinator.activeTool() == solidar::PartDesignToolKind::None);
    CHECK(coordinator.invariantHolds());
  }

  // Fillet/Chamfer sessions expose a finite unit-direction manipulator whose
  // value tracks radius/distance, and whose direction is translation-invariant
  // (derived purely from the selected edge and its adjacent faces).
  {
    const solidar::BodyId bodyId = 7;
    const solidar::FeatureId featureId = 9;
    const auto box = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(30.0, 20.0, 10.0).Shape());
    const auto edge = solidar::makeEdgeReference(*box, bodyId, featureId, 0);
    CHECK(edge.signature);

    const auto unitLength = [](solidar::Vector3d v) {
      return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    };
    const auto finiteUnit = [&](solidar::Vector3d v) {
      return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
             std::abs(unitLength(v) - 1.0) < 1e-6;
    };

    solidar::FilletToolSession fillet;
    fillet.begin(bodyId, featureId, box, {edge}, 3.0);
    const auto filletManip = fillet.manipulator();
    CHECK(filletManip.has_value());
    CHECK(finiteUnit(filletManip->direction));
    CHECK(std::isfinite(filletManip->origin.x) &&
          std::isfinite(filletManip->origin.y) &&
          std::isfinite(filletManip->origin.z));
    CHECK(std::abs(filletManip->valueMm - fillet.radiusMm()) < 1e-9);

    solidar::ChamferToolSession chamfer;
    chamfer.begin(bodyId, featureId, box, {edge}, 2.5);
    const auto chamferManip = chamfer.manipulator();
    CHECK(chamferManip.has_value());
    CHECK(finiteUnit(chamferManip->direction));
    CHECK(std::abs(chamferManip->valueMm - chamfer.distanceMm()) < 1e-9);

    // Owner mismatch is a typed topology failure for every edge/face tool;
    // diagnostic wording is not part of the contract.
    auto foreignEdge = edge;
    foreignEdge.bodyId = bodyId + 1;
    fillet.setEdges({foreignEdge});
    CHECK(fillet.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);
    chamfer.setEdges({foreignEdge});
    CHECK(chamfer.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);

    auto face = solidar::makeFaceReference(*box, bodyId, featureId, 0);
    CHECK(face.signature);
    auto foreignFace = face;
    foreignFace.featureId = featureId + 1;
    solidar::ShellToolSession shell;
    shell.begin(bodyId, featureId, box, {foreignFace});
    CHECK(shell.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);

    solidar::Document draftDocument;
    solidar::DraftToolSession draftSession;
    solidar::PlaneReference plane;
    plane.type = solidar::NeutralPlaneType::GlobalYZ;
    solidar::AxisReference direction;
    direction.type = solidar::AxisReferenceType::GlobalX;
    draftSession.begin(draftDocument, bodyId, featureId, box, {foreignFace},
                       plane, direction);
    CHECK(draftSession.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);

    // Every direct Draft session precondition has one unambiguous typed code.
    solidar::DraftToolSession missingSourceDraft;
    missingSourceDraft.begin(draftDocument, bodyId, featureId, {}, {face},
                             plane, direction);
    CHECK(missingSourceDraft.errorCode() ==
          solidar::OperationFailureCode::MissingSource);

    solidar::DraftToolSession invalidIdsDraft;
    invalidIdsDraft.begin(draftDocument, solidar::kInvalidBodyId, featureId,
                          box, {face}, plane, direction);
    CHECK(invalidIdsDraft.errorCode() ==
          solidar::OperationFailureCode::InvalidInput);

    solidar::DraftToolSession emptySelectionDraft;
    emptySelectionDraft.begin(draftDocument, bodyId, featureId, box, {}, plane,
                              direction);
    CHECK(emptySelectionDraft.errorCode() ==
          solidar::OperationFailureCode::InvalidInput);

    solidar::DraftToolSession incompleteReferenceDraft;
    incompleteReferenceDraft.begin(draftDocument, bodyId, featureId, box,
                                   {face}, std::nullopt, direction);
    CHECK(incompleteReferenceDraft.errorCode() ==
          solidar::OperationFailureCode::InvalidInput);

    TopoDS_Compound emptyCompound;
    BRep_Builder compoundBuilder;
    compoundBuilder.MakeCompound(emptyCompound);
    const auto unindexableShape =
        std::make_shared<TopoDS_Shape>(emptyCompound);
    solidar::FaceReference emptyShapeFace{bodyId, featureId, 0};
    solidar::DraftToolSession unavailableIndexDraft;
    unavailableIndexDraft.begin(draftDocument, bodyId, featureId,
                                unindexableShape, {emptyShapeFace}, plane,
                                direction);
    CHECK(unavailableIndexDraft.errorCode() ==
          solidar::OperationFailureCode::TopologyIndexUnavailable);

    auto foreignRotationEdge = edge;
    foreignRotationEdge.bodyId = bodyId + 1;
    solidar::DraftToolSession rotationOwnerDraft;
    rotationOwnerDraft.begin(draftDocument, bodyId, featureId, box, {face},
                             plane, direction);
    CHECK(!rotationOwnerDraft.setRotationEdge(draftDocument,
                                              foreignRotationEdge));
    CHECK(rotationOwnerDraft.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);

    solidar::DraftToolSession rotationCountDraft;
    rotationCountDraft.begin(draftDocument, bodyId, featureId, box,
                             {face, face}, plane, direction);
    CHECK(!rotationCountDraft.setRotationEdge(draftDocument, edge));
    CHECK(rotationCountDraft.errorCode() ==
          solidar::OperationFailureCode::InvalidInput);

    // Translation invariance through the session seam: translating the base
    // shape shifts the midpoint but leaves the unit direction unchanged.
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(1000.0, -700.0, 350.0));
    const auto movedBox = std::make_shared<TopoDS_Shape>(
        BRepBuilderAPI_Transform(*box, translation).Shape());
    const auto movedEdge =
        solidar::makeEdgeReference(*movedBox, bodyId, featureId, 0);
    CHECK(movedEdge.signature);
    solidar::FilletToolSession movedFillet;
    movedFillet.begin(bodyId, featureId, movedBox, {movedEdge}, 3.0);
    const auto movedManip = movedFillet.manipulator();
    CHECK(movedManip.has_value());
    CHECK(finiteUnit(movedManip->direction));
    CHECK(std::abs(movedManip->direction.x - filletManip->direction.x) < 1e-6);
    CHECK(std::abs(movedManip->direction.y - filletManip->direction.y) < 1e-6);
    CHECK(std::abs(movedManip->direction.z - filletManip->direction.z) < 1e-6);
    CHECK(std::abs(movedManip->origin.x - (filletManip->origin.x + 1000.0)) <
          1e-6);
    CHECK(std::abs(movedManip->origin.y - (filletManip->origin.y - 700.0)) <
          1e-6);
    CHECK(std::abs(movedManip->origin.z - (filletManip->origin.z + 350.0)) <
          1e-6);
  }

  return EXIT_SUCCESS;
}
