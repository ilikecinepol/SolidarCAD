#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ComponentGraphAnalysis.h"
#include "model/ProductDocument.h"
#include "ui/application/ModelCommandHistory.h"
#include "ui/application/internal/ModelCommandHistoryTestAccess.h"

namespace {

using namespace solidar;

class FixedSuccessor final : public ProductDocumentSuccessor {
 public:
  explicit FixedSuccessor(ProductDocumentSuccessorStatus status)
      : status_(status) {}

  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&, ProductDocumentSuccessorImpact) noexcept override {
    ++calls;
    return status_;
  }

  int calls{};

 private:
  ProductDocumentSuccessorStatus status_;
};

std::unique_ptr<ProductDocument> makeProduct() {
  auto document = std::make_unique<Document>();
  auto& first = document->addBody(BodyId{201}, "first");
  first.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{301},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(8.0, 6.0, 4.0).Shape()),
      "first feature"));
  auto& second = document->addBody(BodyId{202}, "second");
  second.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{302},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(5.0, 4.0, 3.0).Shape()),
      "second feature"));
  CHECK(document->recompute());

  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{1}, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(created);
  return std::move(created.product);
}

ComponentDrawing drawing(std::uint64_t id, std::string name,
                         std::vector<BodyId> bodies = {}) {
  return ComponentDrawing(DrawingId{id}, std::move(name),
                          ComponentDrawingPaper::A4,
                          ComponentDrawingOrientation::Portrait, 1.0,
                          std::move(bodies));
}

std::unique_ptr<ProductDocument> makeDrawingProduct() {
  auto document = std::make_unique<Document>();
  auto& first = document->addBody(BodyId{201}, "first");
  first.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{301},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(8.0, 6.0, 4.0).Shape()),
      "first feature"));
  auto& second = document->addBody(BodyId{202}, "second");
  second.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{302},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(5.0, 4.0, 3.0).Shape()),
      "second feature"));
  CHECK(document->recompute());
  ComponentDefinition root(
      DefinitionId{1}, std::move(document),
      std::vector<ComponentDrawing>{drawing(1, "empty"),
                                    drawing(2, "moved", {BodyId{201}}),
                                    drawing(3, "retained", {BodyId{202}})});
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(root));
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{1});
  CHECK(created);
  return std::move(created.product);
}

ComponentExtractionAddress addressFor(const ProductDocument& product,
                                      BodyId body,
                                      DefinitionId child,
                                      OccurrenceId occurrence) {
  return {product.incarnation(), product.rootDefinitionId(),
          product.rootDefinitionId(),
          product.definitionStateId(product.rootDefinitionId()), body, child,
          occurrence};
}

struct WorkspaceProjection {
  std::vector<ComponentWorkspaceInstanceKey> instances;
  std::vector<ComponentWorkspaceBodyKey> bodies;
  std::vector<ComponentWorkspaceDrawingKey> drawings;
};

void canonicalize(WorkspaceProjection& projection) {
  std::sort(projection.instances.begin(), projection.instances.end(),
            componentWorkspaceInstanceKeyLess);
  std::sort(projection.bodies.begin(), projection.bodies.end(),
            componentWorkspaceBodyKeyLess);
  std::sort(projection.drawings.begin(), projection.drawings.end(),
            componentWorkspaceDrawingKeyLess);
}

WorkspaceProjection workspaceProjection(const ProductDocument& product) {
  WorkspaceProjection result;
  if (const auto* part = product.rootPartDefinition()) {
    ComponentWorkspaceInstanceKey root{product.rootDefinitionId(),
                                       std::nullopt};
    result.instances.push_back(root);
    for (const auto& body : part->document().bodies())
      result.bodies.push_back({root, body.id()});
    canonicalize(result);
    return result;
  }
  const auto traversal =
      expandComponentGraph(product.definitions(), product.rootDefinitionId());
  CHECK(traversal);
  for (const auto& visit : traversal.visits) {
    ComponentWorkspaceInstanceKey instance{visit.definitionId, visit.path};
    result.instances.push_back(instance);
    const auto* definition =
        product.definitions().findComponent(visit.definitionId);
    CHECK(definition);
    const auto* document = &definition->document();
    for (const auto& body : document->bodies())
      result.bodies.push_back({instance, body.id()});
    for (const auto& drawing : definition->drawings())
      result.drawings.push_back({instance, drawing.id()});
  }
  canonicalize(result);
  return result;
}

ComponentExtractionColdCommand coldCommand(
    const ProductDocument& product,
    const ComponentExtractionTransaction& transaction,
    project::ProjectPersistenceFormat beforeFormat,
    std::string diagnostic) {
  auto beforeProjection = workspaceProjection(product);
  auto path = OccurrencePath::create({transaction.address.childOccurrenceId});
  CHECK(path);
  ComponentWorkspaceInstanceKey root{transaction.address.rootDefinitionId,
                                     std::nullopt};
  ComponentWorkspaceInstanceKey child{transaction.address.childDefinitionId,
                                      *path.path};
  ComponentWorkspaceBodyKey movedBefore{root, transaction.address.bodyId};
  ComponentWorkspaceBodyKey movedAfter{child, transaction.address.bodyId};
  auto afterProjection = beforeProjection;
  afterProjection.instances.push_back(child);
  const auto moved = std::find(afterProjection.bodies.begin(),
                               afterProjection.bodies.end(), movedBefore);
  CHECK(moved != afterProjection.bodies.end());
  afterProjection.bodies.erase(moved);
  afterProjection.bodies.push_back(movedAfter);
  for (const auto& movedDrawing : transaction.createdChildDrawings) {
    const ComponentWorkspaceDrawingKey before{root, movedDrawing.id()};
    const auto found = std::find(afterProjection.drawings.begin(),
                                 afterProjection.drawings.end(), before);
    CHECK(found != afterProjection.drawings.end());
    found->instance = child;
  }
  canonicalize(afterProjection);

  ComponentWorkspaceSemanticState before{root, root, movedBefore,
                                         "before " + diagnostic};
  ComponentWorkspaceSemanticState after{child, child, movedAfter,
                                        "after " + diagnostic};
  const auto afterFormat =
      beforeFormat == project::ProjectPersistenceFormat::V11 ||
              !beforeProjection.drawings.empty()
          ? project::ProjectPersistenceFormat::V11
          : project::ProjectPersistenceFormat::V10;
  return {transaction,
          std::move(before),
          std::move(after),
          std::move(beforeProjection.instances),
          std::move(afterProjection.instances),
          std::move(beforeProjection.bodies),
          std::move(afterProjection.bodies),
          {beforeFormat}, {afterFormat},
          std::move(beforeProjection.drawings),
          std::move(afterProjection.drawings)};
}

ComponentExtractionTransaction prepareTransaction(
    const ProductDocument& product, BodyId body, DefinitionId child,
    OccurrenceId occurrence) {
  auto prepared =
      product.prepareComponentExtraction(addressFor(product, body, child,
                                                    occurrence));
  CHECK(prepared);
  return std::move(*prepared.transaction);
}

ProductIncarnationId incarnationProbe() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{9000}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(definitions), DefinitionId{9000});
  CHECK(created);
  return created.product->incarnation();
}

}  // namespace

int main() {
  using namespace solidar;

  // A document delta and two extractions share one strict chronological
  // stack. Each dispatcher fails closed when the next entry has another kind.
  {
    auto product = makeProduct();
    ModelCommandHistory history;
    Document previous = product->rootDocument();
    product->rootDocument().findBody(BodyId{201})->setName("renamed");
    CHECK(history.recordTransition(product->rootDocument(),
                                   std::move(previous), {}, {})
              .status == HistoryCommitStatus::Accepted);
    CHECK(history.nextUndoKind() == ModelCommandKind::DocumentTransition);
    CHECK(!history.nextUndoComponentWorkspaceState());

    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    const auto first = prepareTransaction(*product, BodyId{201},
                                          DefinitionId{2}, OccurrenceId{11});
    auto firstCold = coldCommand(*product, first,
                                 project::ProjectPersistenceFormat::V3,
                                 "first");
    CHECK(history.prepareComponentExtraction(*product, firstCold) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(history.undoCount() == 1);
    CHECK(history.redoCount() == 0);
    CHECK(product->applyComponentExtraction(first, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    CHECK(history.nextUndoKind() == ModelCommandKind::ComponentExtraction);
    CHECK(history.nextUndoComponentWorkspaceState());
    CHECK(history.nextUndoComponentWorkspaceState()->get() ==
          firstCold.beforeState);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Component);

    const auto second = prepareTransaction(*product, BodyId{202},
                                           DefinitionId{3}, OccurrenceId{12});
    auto secondCold = coldCommand(
        *product, second, project::ProjectPersistenceFormat::V10, "second");
    CHECK(history.prepareComponentExtraction(*product, secondCold) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentExtraction(second, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    CHECK(history.undoCount() == 3);
    CHECK(history.nextUndoComponentWorkspaceState()->get() ==
          secondCold.beforeState);
    CHECK(product->rootComponentDefinition()->size() == 2);

    Document unrelated;
    const auto wrongDocumentUndo = history.undo(unrelated, {});
    CHECK(!wrongDocumentUndo.changed);
    CHECK(!wrongDocumentUndo.error.empty());
    CHECK(history.undoCount() == 3);

    const auto undoSecond =
        history.undoComponentExtraction(*product, accept);
    CHECK(undoSecond.status == ModelCommandHistoryStatus::Committed);
    CHECK(undoSecond.modelStatus == ComponentExtractionStatus::Reverted);
    CHECK(undoSecond.restoredPersistence ==
          project::ProjectPersistenceState{
              project::ProjectPersistenceFormat::V10});
    CHECK(product->rootDefinitionKind() == DefinitionKind::Component);
    CHECK(product->rootComponentDefinition()->size() == 1);

    const auto undoFirst = history.undoComponentExtraction(*product, accept);
    CHECK(undoFirst.status == ModelCommandHistoryStatus::Committed);
    CHECK(undoFirst.restoredPersistence ==
          project::ProjectPersistenceState{
              project::ProjectPersistenceFormat::V3});
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);
    CHECK(product->rootDocument().bodies().size() == 2);
    CHECK(history.nextUndoKind() == ModelCommandKind::DocumentTransition);
    CHECK(history.undoComponentExtraction(*product, accept).status ==
          ModelCommandHistoryStatus::WrongCommandKind);
    CHECK(history.undo(product->rootDocument(), {}).changed);
    CHECK(product->rootDocument().findBody(BodyId{201})->name() == "first");

    CHECK(history.nextRedoKind() == ModelCommandKind::DocumentTransition);
    CHECK(!history.nextRedoComponentWorkspaceState());
    CHECK(history.redoComponentExtraction(*product, accept).status ==
          ModelCommandHistoryStatus::WrongCommandKind);
    CHECK(history.redo(product->rootDocument(), {}).changed);
    CHECK(history.nextRedoComponentWorkspaceState());
    CHECK(history.nextRedoComponentWorkspaceState()->get() ==
          firstCold.afterState);
    CHECK(product->rootDocument().findBody(BodyId{201})->name() == "renamed");
    CHECK(history.redoComponentExtraction(*product, accept).status ==
          ModelCommandHistoryStatus::Committed);
    CHECK(history.redoComponentExtraction(*product, accept).status ==
          ModelCommandHistoryStatus::Committed);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Component);
    CHECK(product->rootComponentDefinition()->size() == 2);
    CHECK(!history.nextRedoKind());
  }

  // Drawing keys are part of the cold proof. Undo/Redo restores the exact
  // ordered values and definition state without consuming or remapping IDs.
  {
    auto product = makeDrawingProduct();
    ModelCommandHistory history;
    const auto transaction = prepareTransaction(
        *product, BodyId{201}, DefinitionId{2}, OccurrenceId{11});
    auto command = coldCommand(*product, transaction,
                               project::ProjectPersistenceFormat::V11,
                               "drawings");
    CHECK(command.beforeDrawings.size() == 3);
    CHECK(command.afterDrawings.size() == 3);
    CHECK(command.afterPersistence ==
          project::ProjectPersistenceState{
              project::ProjectPersistenceFormat::V11});
    auto lossyV10 = command;
    lossyV10.beforePersistence = {project::ProjectPersistenceFormat::V10};
    lossyV10.afterPersistence = {project::ProjectPersistenceFormat::V10};
    ModelCommandHistory rejectedLossyFormat;
    CHECK(rejectedLossyFormat.prepareComponentExtraction(*product, lossyV10) ==
          ModelCommandHistoryStatus::InvalidCommand);
    CHECK(history.prepareComponentExtraction(*product, command) ==
          ModelCommandHistoryStatus::Prepared);
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyComponentExtraction(transaction, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    CHECK(product->rootComponentDefinition()->drawings() ==
          transaction.afterOwnerDrawings);
    CHECK(product->definitions().findComponent(DefinitionId{2})->drawings() ==
          transaction.createdChildDrawings);

    const auto drawingUndo =
        history.undoComponentExtraction(*product, accept);
    CHECK(drawingUndo.status == ModelCommandHistoryStatus::Committed);
    CHECK(drawingUndo.restoredPersistence ==
          project::ProjectPersistenceState{
              project::ProjectPersistenceFormat::V11});
    CHECK(product->rootComponentDefinition()->drawings() ==
          transaction.beforeOwnerDrawings);
    CHECK(product->definitionStateId(DefinitionId{1}) ==
          transaction.beforeOwnerStateId);
    const auto drawingRedo =
        history.redoComponentExtraction(*product, accept);
    CHECK(drawingRedo.status == ModelCommandHistoryStatus::Committed);
    CHECK(drawingRedo.restoredPersistence ==
          project::ProjectPersistenceState{
              project::ProjectPersistenceFormat::V11});
    CHECK(product->rootComponentDefinition()->drawings() ==
          transaction.afterOwnerDrawings);
    CHECK(product->definitions()
              .findComponent(DefinitionId{2})
              ->findDrawing(DrawingId{2}) != nullptr);

    auto fresh = makeDrawingProduct();
    const auto freshTransaction = prepareTransaction(
        *fresh, BodyId{201}, DefinitionId{2}, OccurrenceId{11});
    const auto freshCommand = coldCommand(
        *fresh, freshTransaction, project::ProjectPersistenceFormat::V11,
        "drawing validation");
    auto forgedProjection = freshCommand;
    forgedProjection.beforeDrawings.pop_back();
    ModelCommandHistory rejectsProjection;
    CHECK(rejectsProjection.prepareComponentExtraction(
              *fresh, forgedProjection) ==
          ModelCommandHistoryStatus::InvalidCommand);
    auto forgedTransaction = freshCommand;
    forgedTransaction.transaction.afterOwnerDrawings.clear();
    ModelCommandHistory rejectsTransaction;
    CHECK(rejectsTransaction.prepareComponentExtraction(
              *fresh, forgedTransaction) ==
          ModelCommandHistoryStatus::InvalidCommand);
    ModelCommandHistory tooSmall(1);
    CHECK(tooSmall.prepareComponentExtraction(*fresh, freshCommand) ==
          ModelCommandHistoryStatus::ResourceLimit);
  }

  // Prepare and successor failures are exact no-ops for history and product;
  // commit is the sole point at which a prepared command truncates redo.
  {
    auto product = makeProduct();
    ModelCommandHistory history;
    const auto transaction = prepareTransaction(
        *product, BodyId{201}, DefinitionId{4}, OccurrenceId{21});
    auto command = coldCommand(*product, transaction,
                               project::ProjectPersistenceFormat::V3,
                               std::string(2048, 'd'));
    const auto incarnationBefore = incarnationProbe();
    const auto initialRevision = history.currentRevision();
    CHECK(history.isAtSavepoint());
    CHECK(history.prepareComponentExtraction(*product, command) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(history.currentRevision() == initialRevision);
    CHECK(history.isAtSavepoint());

    FixedSuccessor reject(ProductDocumentSuccessorStatus::Rejected);
    CHECK(product->applyComponentExtraction(transaction, reject) ==
          ComponentExtractionStatus::SuccessorRejected);
    history.discardPrepared();
    CHECK(history.undoCount() == 0);
    CHECK(history.redoCount() == 0);
    CHECK(history.currentRevision() == initialRevision);
    CHECK(history.isAtSavepoint());
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);

    ModelCommandHistory tooSmall(1);
    CHECK(tooSmall.prepareComponentExtraction(*product, command) ==
          ModelCommandHistoryStatus::ResourceLimit);
    CHECK(tooSmall.undoCount() == 0);
    CHECK(tooSmall.isAtSavepoint());

    detail::ModelCommandHistoryTestAccess::inject(
        history, detail::ModelCommandHistoryFailurePoint::PrepareReserve,
        detail::ModelCommandHistoryFailureKind::BadAlloc);
    CHECK(history.prepareComponentExtraction(*product, command) ==
          ModelCommandHistoryStatus::ResourceLimit);
    CHECK(history.undoCount() == 0);
    CHECK(history.redoCount() == 0);
    CHECK(history.currentRevision() == initialRevision);
    detail::ModelCommandHistoryTestAccess::clearInjection(history);

    ModelCommandHistory exhausted;
    detail::ModelCommandHistoryTestAccess::setNextRevision(
        exhausted, std::numeric_limits<std::uint64_t>::max());
    CHECK(exhausted.prepareComponentExtraction(*product, command) ==
          ModelCommandHistoryStatus::ResourceLimit);
    CHECK(exhausted.undoCount() == 0);

    auto invalid = command;
    invalid.transaction.address.childDefinitionId =
        invalid.transaction.address.ownerDefinitionId;
    CHECK(history.prepareComponentExtraction(*product, invalid) ==
          ModelCommandHistoryStatus::InvalidCommand);
    CHECK(history.undoCount() == 0);

    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(history.prepareComponentExtraction(*product, command) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentExtraction(transaction, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    const auto committedRevision = history.currentRevision();
    CHECK(committedRevision != initialRevision);
    CHECK(!history.isAtSavepoint());
    CHECK(history.retainedBytes() > 4096);

    FixedSuccessor undoReject(ProductDocumentSuccessorStatus::Rejected);
    const auto rejectedUndo =
        history.undoComponentExtraction(*product, undoReject);
    CHECK(rejectedUndo.status == ModelCommandHistoryStatus::SuccessorRejected);
    CHECK(rejectedUndo.modelStatus ==
          ComponentExtractionStatus::SuccessorRejected);
    CHECK(history.undoCount() == 1);
    CHECK(history.currentRevision() == committedRevision);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Component);

    detail::ModelCommandHistoryTestAccess::inject(
        history, detail::ModelCommandHistoryFailurePoint::UndoStatePreparation,
        detail::ModelCommandHistoryFailureKind::BadAlloc);
    const auto allocationUndo =
        history.undoComponentExtraction(*product, accept);
    CHECK(allocationUndo.status == ModelCommandHistoryStatus::ResourceLimit);
    CHECK(history.undoCount() == 1);
    CHECK(history.currentRevision() == committedRevision);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Component);
    detail::ModelCommandHistoryTestAccess::clearInjection(history);

    CHECK(history.undoComponentExtraction(*product, accept).status ==
          ModelCommandHistoryStatus::Committed);
    CHECK(history.isAtSavepoint());
    FixedSuccessor redoResource(
        ProductDocumentSuccessorStatus::ResourceFailure);
    const auto rejectedRedo =
        history.redoComponentExtraction(*product, redoResource);
    CHECK(rejectedRedo.status ==
          ModelCommandHistoryStatus::SuccessorResourceFailure);
    CHECK(history.redoCount() == 1);
    CHECK(history.isAtSavepoint());
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);

    detail::ModelCommandHistoryTestAccess::inject(
        history, detail::ModelCommandHistoryFailurePoint::RedoStatePreparation,
        detail::ModelCommandHistoryFailureKind::Runtime);
    CHECK(history.redoComponentExtraction(*product, accept).status ==
          ModelCommandHistoryStatus::ResourceLimit);
    CHECK(history.redoCount() == 1);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);
    detail::ModelCommandHistoryTestAccess::clearInjection(history);

    const auto incarnationAfter = incarnationProbe();
    CHECK(incarnationAfter.value() == incarnationBefore.value() + 1);
  }

  // A failed new prepare after undo retains the redo command exactly.
  {
    auto product = makeProduct();
    ModelCommandHistory history;
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    const auto transaction = prepareTransaction(
        *product, BodyId{201}, DefinitionId{5}, OccurrenceId{31});
    auto command = coldCommand(*product, transaction,
                               project::ProjectPersistenceFormat::V3,
                               "redo-tail");
    CHECK(history.prepareComponentExtraction(*product, command) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentExtraction(transaction, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    CHECK(history.undoComponentExtraction(*product, accept).status ==
          ModelCommandHistoryStatus::Committed);
    CHECK(history.redoCount() == 1);

    auto invalid = command;
    invalid.transaction.beforeNextStateIdValue = 0;
    CHECK(history.prepareComponentExtraction(*product, invalid) ==
          ModelCommandHistoryStatus::InvalidCommand);
    CHECK(history.redoCount() == 1);
    CHECK(history.nextRedoKind() == ModelCommandKind::ComponentExtraction);
    CHECK(history.redoComponentExtraction(*product, accept).status ==
          ModelCommandHistoryStatus::Committed);
  }

  // The cold projection is proof, not a hint: format, active-instance,
  // occurrence-path and body-key forgeries are rejected before reserve.
  {
    auto product = makeProduct();
    const auto first = prepareTransaction(*product, BodyId{201},
                                          DefinitionId{20}, OccurrenceId{41});
    const auto base = coldCommand(*product, first,
                                  project::ProjectPersistenceFormat::V3,
                                  "validation");
    const auto rejects = [&product](
                             const ComponentExtractionColdCommand& candidate) {
      ModelCommandHistory history;
      return history.prepareComponentExtraction(*product, candidate) ==
             ModelCommandHistoryStatus::InvalidCommand;
    };

    for (const auto format : {project::ProjectPersistenceFormat::V3,
                              project::ProjectPersistenceFormat::V4,
                              project::ProjectPersistenceFormat::V5}) {
      auto accepted = base;
      accepted.beforePersistence.format = format;
      ModelCommandHistory history;
      CHECK(history.prepareComponentExtraction(*product, accepted) ==
            ModelCommandHistoryStatus::Prepared);
      history.discardPrepared();
    }
    for (const auto format : {project::ProjectPersistenceFormat::V6,
                              project::ProjectPersistenceFormat::V7,
                              project::ProjectPersistenceFormat::V8,
                              project::ProjectPersistenceFormat::V9,
                              project::ProjectPersistenceFormat::V10}) {
      auto malformed = base;
      malformed.beforePersistence.format = format;
      CHECK(rejects(malformed));
    }
    for (const auto format : {project::ProjectPersistenceFormat::V3,
                              project::ProjectPersistenceFormat::V4,
                              project::ProjectPersistenceFormat::V5,
                              project::ProjectPersistenceFormat::V6,
                              project::ProjectPersistenceFormat::V7,
                              project::ProjectPersistenceFormat::V8,
                              project::ProjectPersistenceFormat::V9}) {
      auto malformed = base;
      malformed.afterPersistence.format = format;
      CHECK(rejects(malformed));
    }

    auto malformed = base;
    malformed.beforeState.activeInstance.definitionId = DefinitionId{999};
    CHECK(rejects(malformed));
    malformed = base;
    malformed.beforeState.activeInstance.path =
        base.afterState.activeInstance.path;
    CHECK(rejects(malformed));
    malformed = base;
    malformed.afterState.activeInstance.path = std::nullopt;
    CHECK(rejects(malformed));
    malformed = base;
    malformed.beforeState.selectedInstance = base.afterState.activeInstance;
    CHECK(rejects(malformed));
    malformed = base;
    malformed.beforeState.selectedBody->bodyId = BodyId{999};
    CHECK(rejects(malformed));
    malformed = base;
    malformed.beforeInstances.clear();
    CHECK(rejects(malformed));
    malformed = base;
    malformed.afterInstances.pop_back();
    CHECK(rejects(malformed));
    malformed = base;
    malformed.afterInstances.back().definitionId = DefinitionId{999};
    CHECK(rejects(malformed));
    malformed = base;
    malformed.beforeBodies.erase(malformed.beforeBodies.begin());
    CHECK(rejects(malformed));
    malformed = base;
    malformed.afterBodies.back().instance = malformed.beforeInstances.front();
    CHECK(rejects(malformed));

    // Comparator work is explicitly bounded. This near-authored-limit forged
    // projection cannot turn duplicate/membership validation quadratic.
    auto nearLimit = base;
    nearLimit.beforeInstances = base.beforeInstances;
    for (std::uint64_t value = 1000; value < 5095; ++value) {
      auto path = OccurrencePath::create({OccurrenceId{value}});
      CHECK(path);
      nearLimit.beforeInstances.push_back(
          {DefinitionId{value}, std::move(*path.path)});
    }
    nearLimit.afterInstances = nearLimit.beforeInstances;
    nearLimit.afterInstances.push_back(base.afterInstances.back());
    std::sort(nearLimit.beforeInstances.begin(),
              nearLimit.beforeInstances.end(),
              componentWorkspaceInstanceKeyLess);
    std::sort(nearLimit.afterInstances.begin(), nearLimit.afterInstances.end(),
              componentWorkspaceInstanceKeyLess);
    ModelCommandHistory boundedWork;
    constexpr std::size_t kCompareLimit = 4096;
    detail::ModelCommandHistoryTestAccess::setProjectionCompareLimit(
        boundedWork, kCompareLimit);
    CHECK(boundedWork.prepareComponentExtraction(*product, nearLimit) ==
          ModelCommandHistoryStatus::ResourceLimit);
    CHECK(detail::ModelCommandHistoryTestAccess::projectionCompareCount(
              boundedWork) == kCompareLimit);
    CHECK(boundedWork.undoCount() == 0);

    auto lateDuplicate = nearLimit;
    lateDuplicate.beforeInstances.push_back(
        lateDuplicate.beforeInstances.back());
    std::sort(lateDuplicate.beforeInstances.begin(),
              lateDuplicate.beforeInstances.end(),
              componentWorkspaceInstanceKeyLess);
    ModelCommandHistory linearDuplicate;
    detail::ModelCommandHistoryTestAccess::setProjectionCompareLimit(
        linearDuplicate, 20000);
    CHECK(linearDuplicate.prepareComponentExtraction(*product,
                                                     lateDuplicate) ==
          ModelCommandHistoryStatus::InvalidCommand);
    CHECK(detail::ModelCommandHistoryTestAccess::projectionCompareCount(
              linearDuplicate) < 10000);

    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyComponentExtraction(first, accept) ==
          ComponentExtractionStatus::Committed);
    const auto second = prepareTransaction(*product, BodyId{202},
                                           DefinitionId{21}, OccurrenceId{42});
    const auto componentBase = coldCommand(
        *product, second, project::ProjectPersistenceFormat::V10,
        "component validation");
    ModelCommandHistory acceptedComponent;
    CHECK(acceptedComponent.prepareComponentExtraction(*product,
                                                       componentBase) ==
          ModelCommandHistoryStatus::Prepared);
    acceptedComponent.discardPrepared();
    for (const auto format : {project::ProjectPersistenceFormat::V3,
                              project::ProjectPersistenceFormat::V4,
                              project::ProjectPersistenceFormat::V5,
                              project::ProjectPersistenceFormat::V6,
                              project::ProjectPersistenceFormat::V7,
                              project::ProjectPersistenceFormat::V8,
                              project::ProjectPersistenceFormat::V9}) {
      auto wrongBefore = componentBase;
      wrongBefore.beforePersistence.format = format;
      CHECK(rejects(wrongBefore));
    }
  }

  // clear() may consume the final non-zero revision once. Further clears are
  // exact no-ops, so wraparound cannot revive an old revision/savepoint.
  {
    ModelCommandHistory history;
    detail::ModelCommandHistoryTestAccess::setNextRevision(
        history, std::numeric_limits<std::uint64_t>::max());
    history.clear(false);
    CHECK(history.currentRevision() ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK(!history.isAtSavepoint());
    CHECK(history.undoCount() == 0);
    history.clear(true);
    CHECK(history.currentRevision() ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK(!history.isAtSavepoint());

    Document current;
    Document previous = current;
    current.addBody(BodyId{500}, "must roll back");
    CHECK(history.recordTransition(current, std::move(previous), {}, {})
              .status == HistoryCommitStatus::Rejected);
    CHECK(current.bodies().empty());
    CHECK(history.currentRevision() ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK(history.undoCount() == 0);
    CHECK(history.redoCount() == 0);
    CHECK(!history.isAtSavepoint());
  }

  return EXIT_SUCCESS;
}
