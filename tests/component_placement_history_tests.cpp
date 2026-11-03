#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "model/ComponentGraphAnalysis.h"
#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "ui/application/ModelCommandHistory.h"

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
  for (const auto [bodyId, featureId, size] :
       {std::tuple{BodyId{201}, FeatureId{301}, 8.0},
        std::tuple{BodyId{202}, FeatureId{302}, 5.0}}) {
    auto& body = document->addBody(bodyId, "body");
    body.addFeature(std::make_unique<ImportedShapeFeature>(
        featureId,
        std::make_shared<const TopoDS_Shape>(
            BRepPrimAPI_MakeBox(size, size, size).Shape()),
        "feature"));
  }
  CHECK(document->recompute());
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(DefinitionId{1},
                                       std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(registry), DefinitionId{1});
  CHECK(created);
  return std::move(created.product);
}

struct Projection {
  std::vector<ComponentWorkspaceInstanceKey> instances;
  std::vector<ComponentWorkspaceBodyKey> bodies;
};

void canonicalize(Projection& value) {
  std::sort(value.instances.begin(), value.instances.end(),
            componentWorkspaceInstanceKeyLess);
  std::sort(value.bodies.begin(), value.bodies.end(),
            componentWorkspaceBodyKeyLess);
}

Projection projection(const ProductDocument& product) {
  Projection value;
  if (const auto* part = product.rootPartDefinition()) {
    ComponentWorkspaceInstanceKey root{product.rootDefinitionId(),
                                       std::nullopt};
    value.instances.push_back(root);
    for (const auto& body : part->document().bodies())
      value.bodies.push_back({root, body.id()});
  } else {
    const auto traversal = expandComponentGraph(product.definitions(),
                                                product.rootDefinitionId());
    CHECK(traversal);
    for (const auto& visit : traversal.visits) {
      ComponentWorkspaceInstanceKey instance{visit.definitionId, visit.path};
      value.instances.push_back(instance);
      const auto* document = product.componentDocument(visit.definitionId);
      CHECK(document);
      for (const auto& body : document->bodies())
        value.bodies.push_back({instance, body.id()});
    }
  }
  canonicalize(value);
  return value;
}

ComponentExtractionTransaction extraction(const ProductDocument& product,
                                           BodyId body, DefinitionId child,
                                           OccurrenceId occurrence) {
  const ComponentExtractionAddress address{
      product.incarnation(), product.rootDefinitionId(),
      product.rootDefinitionId(),
      product.definitionStateId(product.rootDefinitionId()), body, child,
      occurrence};
  auto prepared = product.prepareComponentExtraction(address);
  CHECK(prepared);
  return *prepared.transaction;
}

ComponentExtractionColdCommand extractionCold(
    const ProductDocument& product,
    const ComponentExtractionTransaction& transaction,
    project::ProjectPersistenceFormat beforeFormat) {
  auto before = projection(product);
  auto occurrencePath =
      OccurrencePath::create({transaction.address.childOccurrenceId});
  CHECK(occurrencePath);
  ComponentWorkspaceInstanceKey root{product.rootDefinitionId(), std::nullopt};
  ComponentWorkspaceInstanceKey child{transaction.address.childDefinitionId,
                                      *occurrencePath.path};
  ComponentWorkspaceBodyKey oldBody{root, transaction.address.bodyId};
  ComponentWorkspaceBodyKey newBody{child, transaction.address.bodyId};
  auto after = before;
  after.instances.push_back(child);
  const auto found = std::find(after.bodies.begin(), after.bodies.end(), oldBody);
  CHECK(found != after.bodies.end());
  after.bodies.erase(found);
  after.bodies.push_back(newBody);
  canonicalize(after);
  return {transaction,
          {root, root, oldBody, "before extraction"},
          {child, child, newBody, "after extraction"},
          before.instances,
          after.instances,
          before.bodies,
          after.bodies,
          {beforeFormat},
          {project::ProjectPersistenceFormat::V10}};
}

ComponentPlacementTransaction placementTransaction(
    const ProductDocument& product, OccurrenceId id, DefinitionId target,
    std::array<double, 3> delta) {
  auto occurrencePath = OccurrencePath::create({id});
  CHECK(occurrencePath);
  const auto* occurrence = product.rootComponentDefinition()->find(id);
  CHECK(occurrence);
  ComponentPlacementRequest request{
      product.incarnation(), product.rootDefinitionId(), *occurrencePath.path,
      target, product.definitionStateId(product.rootDefinitionId()),
      occurrence->placement(), delta};
  auto prepared = product.prepareComponentPlacement(request);
  CHECK(prepared);
  return *prepared.transaction;
}

ComponentPlacementColdCommand placementCold(
    const ProductDocument& product,
    const ComponentPlacementTransaction& transaction,
    project::ProjectPersistenceFormat format =
        project::ProjectPersistenceFormat::V10) {
  auto workspace = projection(product);
  ComponentWorkspaceInstanceKey root{product.rootDefinitionId(), std::nullopt};
  ComponentWorkspaceInstanceKey selected{
      transaction.request.expectedTargetDefinitionId,
      transaction.request.path};
  return {transaction,
          {root, selected, std::nullopt, "before placement"},
          {root, selected, std::nullopt, "after placement"},
          workspace.instances,
          workspace.instances,
          workspace.bodies,
          workspace.bodies,
          {format},
          {format}};
}

ComponentHomePlacementTransaction homeTransaction(
    const ProductDocument& product, OccurrenceId id, DefinitionId target,
    ComponentHomePlacementOperation operation) {
  auto occurrencePath = OccurrencePath::create({id});
  CHECK(occurrencePath);
  const auto* occurrence = product.rootComponentDefinition()->find(id);
  CHECK(occurrence);
  ComponentHomePlacementRequest request{
      product.incarnation(), product.rootDefinitionId(), *occurrencePath.path,
      target, product.rootDefinitionId(),
      product.definitionStateId(product.rootDefinitionId()),
      occurrence->placement(), occurrence->homePlacement(), operation};
  auto prepared = product.prepareComponentHomePlacement(request);
  CHECK(prepared);
  return *prepared.transaction;
}

ComponentHomePlacementColdCommand homeCold(
    const ProductDocument& product,
    const ComponentHomePlacementTransaction& transaction) {
  auto workspace = projection(product);
  ComponentWorkspaceInstanceKey root{product.rootDefinitionId(), std::nullopt};
  ComponentWorkspaceInstanceKey selected{
      transaction.request.expectedTargetDefinitionId,
      transaction.request.path};
  return {transaction,
          {root, selected, std::nullopt, "before home placement"},
          {root, selected, std::nullopt, "after home placement"},
          workspace.instances,
          workspace.instances,
          workspace.bodies,
          workspace.bodies,
          {project::ProjectPersistenceFormat::V12},
          {project::ProjectPersistenceFormat::V12}};
}

ProductIncarnationId incarnationProbe() {
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(DefinitionId{9000},
                                       std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(registry),
                                         DefinitionId{9000});
  CHECK(created);
  return created.product->incarnation();
}

}  // namespace

int main() {
  using namespace solidar;

  // One stack preserves Extraction -> Placement -> Extraction chronology.
  // Wrong dispatch and failed successor preparation never move the cursor.
  {
    auto product = makeProduct();
    ModelCommandHistory history;
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);

    const auto e1 = extraction(*product, BodyId{201}, DefinitionId{2},
                               OccurrenceId{11});
    const auto e1Cold = extractionCold(
        *product, e1, project::ProjectPersistenceFormat::V3);
    CHECK(history.prepareComponentExtraction(*product, e1Cold) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentExtraction(e1, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    const auto p = placementTransaction(*product, OccurrenceId{11},
                                        DefinitionId{2}, {7, 0, 0});
    const auto pCold = placementCold(*product, p);
    CHECK(history.prepareComponentPlacement(*product, pCold) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentPlacement(p, accept) ==
          ComponentPlacementStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    CHECK(history.nextUndoKind() == ModelCommandKind::ComponentPlacement);

    const auto e2 = extraction(*product, BodyId{202}, DefinitionId{3},
                               OccurrenceId{12});
    const auto e2Cold = extractionCold(
        *product, e2, project::ProjectPersistenceFormat::V10);
    CHECK(history.prepareComponentExtraction(*product, e2Cold) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentExtraction(e2, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    CHECK(history.undoCount() == 3);

    CHECK(history.undoComponentExtraction(*product, accept).changed());
    CHECK(history.nextUndoKind() == ModelCommandKind::ComponentPlacement);
    CHECK(history.undoComponentExtraction(*product, accept).status ==
          ModelCommandHistoryStatus::WrongCommandKind);
    FixedSuccessor reject(ProductDocumentSuccessorStatus::Rejected);
    CHECK(history.undoComponentPlacement(*product, reject).status ==
          ModelCommandHistoryStatus::SuccessorRejected);
    CHECK(history.undoCount() == 2);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == p.afterPlacement);
    const auto undoPlacement =
        history.undoComponentPlacement(*product, accept);
    CHECK(undoPlacement.changed());
    CHECK(undoPlacement.restoredPersistence ==
          project::ProjectPersistenceState{
              project::ProjectPersistenceFormat::V10});
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == p.beforePlacement);
    CHECK(history.undoComponentExtraction(*product, accept).changed());
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);

    CHECK(history.redoComponentExtraction(*product, accept).changed());
    CHECK(history.nextRedoKind() == ModelCommandKind::ComponentPlacement);
    CHECK(history.redoComponentPlacement(*product, accept).changed());
    CHECK(history.redoComponentExtraction(*product, accept).changed());
    CHECK(product->rootComponentDefinition()->size() == 2);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == p.afterPlacement);
  }

  // An empty V11 component remains V11 across placement Undo/Redo.
  {
    auto product = makeProduct();
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    const auto e = extraction(*product, BodyId{201}, DefinitionId{2},
                              OccurrenceId{11});
    CHECK(product->applyComponentExtraction(e, accept) ==
          ComponentExtractionStatus::Committed);
    const auto p = placementTransaction(*product, OccurrenceId{11},
                                        DefinitionId{2}, {3, 2, 1});
    auto command = placementCold(*product, p,
                                 project::ProjectPersistenceFormat::V11);
    auto downgradeCommand = command;
    downgradeCommand.afterPersistence =
        {project::ProjectPersistenceFormat::V10};
    ModelCommandHistory downgrade;
    CHECK(downgrade.prepareComponentPlacement(*product, downgradeCommand) ==
          ModelCommandHistoryStatus::InvalidCommand);
    ModelCommandHistory history;
    CHECK(history.prepareComponentPlacement(*product, command) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentPlacement(p, accept) ==
          ComponentPlacementStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    const auto undone = history.undoComponentPlacement(*product, accept);
    CHECK(undone.changed());
    CHECK(undone.restoredPersistence ==
          project::ProjectPersistenceState{
              project::ProjectPersistenceFormat::V11});
    const auto redone = history.redoComponentPlacement(*product, accept);
    CHECK(redone.changed());
    CHECK(redone.restoredPersistence ==
          project::ProjectPersistenceState{
              project::ProjectPersistenceFormat::V11});

  }

  // Prepare validation, byte budget, redo-tail and global ID cursor remain
  // exact on every rejected cold command.
  {
    auto product = makeProduct();
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    ModelCommandHistory history;
    const auto e1 = extraction(*product, BodyId{201}, DefinitionId{2},
                               OccurrenceId{11});
    auto e1Cold = extractionCold(*product, e1,
                                 project::ProjectPersistenceFormat::V3);
    CHECK(history.prepareComponentExtraction(*product, e1Cold) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentExtraction(e1, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    history.markSavepoint();
    const auto savedRevision = history.currentRevision();
    const auto p = placementTransaction(*product, OccurrenceId{11},
                                        DefinitionId{2}, {1, 0, 0});
    auto cold = placementCold(*product, p);
    auto malformed = cold;
    malformed.beforePersistence.format =
        project::ProjectPersistenceFormat::V9;
    CHECK(history.prepareComponentPlacement(*product, malformed) ==
          ModelCommandHistoryStatus::InvalidCommand);
    CHECK(history.undoCount() == 1);
    CHECK(history.currentRevision() == savedRevision);
    CHECK(history.isAtSavepoint());

    ModelCommandHistory tiny(1);
    CHECK(tiny.prepareComponentPlacement(*product, cold) ==
          ModelCommandHistoryStatus::ResourceLimit);
    CHECK(!tiny.canUndo());

    const auto beforeProbe = incarnationProbe();
    CHECK(history.prepareComponentPlacement(*product, cold) ==
          ModelCommandHistoryStatus::Prepared);
    history.discardPrepared();
    CHECK(!history.canRedo());
    const auto afterProbe = incarnationProbe();
    CHECK(afterProbe.value() == beforeProbe.value() + 1);
  }

  // Adopt and Restore are first-class entries on the unified stack. Undo and
  // Redo restore the exact independent pose, preserve savepoint accounting,
  // and a rejected successor leaves the cursor and model unchanged.
  {
    auto product = makeProduct();
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    const auto extractionTransaction = extraction(
        *product, BodyId{201}, DefinitionId{2}, OccurrenceId{11});
    CHECK(product->applyComponentExtraction(extractionTransaction, accept) ==
          ComponentExtractionStatus::Committed);
    const auto move = placementTransaction(*product, OccurrenceId{11},
                                           DefinitionId{2}, {6, 1, 0});
    CHECK(product->applyComponentPlacement(move, accept) ==
          ComponentPlacementStatus::Committed);
    const auto moved = product->rootComponentDefinition()
                           ->find(OccurrenceId{11})
                           ->placement();
    const auto originalHome = product->rootComponentDefinition()
                                  ->find(OccurrenceId{11})
                                  ->homePlacement();
    CHECK(moved != originalHome);

    ModelCommandHistory history;
    history.markSavepoint();
    const auto adopt = homeTransaction(
        *product, OccurrenceId{11}, DefinitionId{2},
        ComponentHomePlacementOperation::AdoptCurrent);
    const auto adoptCold = homeCold(*product, adopt);
    CHECK(history.prepareComponentHomePlacement(*product, adoptCold) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentHomePlacement(adopt, accept) ==
          ComponentHomePlacementStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    CHECK(history.nextUndoKind() ==
          ModelCommandKind::ComponentHomePlacement);
    CHECK(!history.isAtSavepoint());
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == moved);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->homePlacement() == moved);

    FixedSuccessor reject(ProductDocumentSuccessorStatus::Rejected);
    CHECK(history.undoComponentHomePlacement(*product, reject).status ==
          ModelCommandHistoryStatus::SuccessorRejected);
    CHECK(history.undoCount() == 1);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->homePlacement() == moved);
    CHECK(history.undoComponentHomePlacement(*product, accept).changed());
    CHECK(history.isAtSavepoint());
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == moved);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->homePlacement() == originalHome);
    const auto redoCount = history.redoCount();
    const auto revisionBeforeNoOp = history.currentRevision();
    auto noOp = adoptCold;
    noOp.transaction.afterPlacement = noOp.transaction.beforePlacement;
    noOp.transaction.afterHomePlacement =
        noOp.transaction.beforeHomePlacement;
    CHECK(history.prepareComponentHomePlacement(*product, noOp) ==
          ModelCommandHistoryStatus::InvalidCommand);
    CHECK(history.redoCount() == redoCount);
    CHECK(history.currentRevision() == revisionBeforeNoOp);
    CHECK(history.redoComponentHomePlacement(*product, accept).changed());
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->homePlacement() == moved);

    const auto moveAgain = placementTransaction(
        *product, OccurrenceId{11}, DefinitionId{2}, {4, -2, 3});
    CHECK(product->applyComponentPlacement(moveAgain, accept) ==
          ComponentPlacementStatus::Committed);
    const auto displaced = product->rootComponentDefinition()
                               ->find(OccurrenceId{11})
                               ->placement();
    const auto restore = homeTransaction(
        *product, OccurrenceId{11}, DefinitionId{2},
        ComponentHomePlacementOperation::Restore);
    const auto restoreCold = homeCold(*product, restore);
    CHECK(history.prepareComponentHomePlacement(*product, restoreCold) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyComponentHomePlacement(restore, accept) ==
          ComponentHomePlacementStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == moved);
    CHECK(history.undoComponentHomePlacement(*product, accept).changed());
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == displaced);
    CHECK(history.redoComponentHomePlacement(*product, accept).changed());
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == moved);
  }

  // Restore-all remains one history command even when several component
  // occurrences are displaced. Undo and redo move the complete set.
  {
    auto product = makeProduct();
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    const auto firstExtraction =
        extraction(*product, BodyId{201}, DefinitionId{2}, OccurrenceId{11});
    CHECK(product->applyComponentExtraction(firstExtraction, accept) ==
          ComponentExtractionStatus::Committed);
    const auto secondExtraction =
        extraction(*product, BodyId{202}, DefinitionId{3}, OccurrenceId{12});
    CHECK(product->applyComponentExtraction(secondExtraction, accept) ==
          ComponentExtractionStatus::Committed);

    const auto firstMove = placementTransaction(
        *product, OccurrenceId{11}, DefinitionId{2}, {4, 0, 0});
    CHECK(product->applyComponentPlacement(firstMove, accept) ==
          ComponentPlacementStatus::Committed);
    const auto secondMove = placementTransaction(
        *product, OccurrenceId{12}, DefinitionId{3}, {0, 5, 0});
    CHECK(product->applyComponentPlacement(secondMove, accept) ==
          ComponentPlacementStatus::Committed);
    const auto firstMoved = product->rootComponentDefinition()
                                ->find(OccurrenceId{11})
                                ->placement();
    const auto secondMoved = product->rootComponentDefinition()
                                 ->find(OccurrenceId{12})
                                 ->placement();

    const auto prepared = product->prepareAllComponentHomePlacements();
    CHECK(prepared);
    CHECK(prepared.transactions.size() == 2);
    auto command = homeCold(*product, prepared.transactions.front());
    command.additionalTransactions.assign(prepared.transactions.begin() + 1,
                                          prepared.transactions.end());
    ModelCommandHistory history;
    history.markSavepoint();
    CHECK(history.prepareComponentHomePlacement(*product, command) ==
          ModelCommandHistoryStatus::Prepared);
    CHECK(product->applyAllComponentHomePlacements(prepared.transactions,
                                                   accept) ==
          ComponentHomePlacementStatus::Committed);
    CHECK(history.commitPrepared() == ModelCommandHistoryStatus::Committed);
    CHECK(history.undoCount() == 1);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == product->rootComponentDefinition()
                                    ->find(OccurrenceId{11})
                                    ->homePlacement());
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{12})
              ->placement() == product->rootComponentDefinition()
                                    ->find(OccurrenceId{12})
                                    ->homePlacement());
    CHECK(history.undoComponentHomePlacement(*product, accept).changed());
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == firstMoved);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{12})
              ->placement() == secondMoved);
    CHECK(history.redoComponentHomePlacement(*product, accept).changed());
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == product->rootComponentDefinition()
                                    ->find(OccurrenceId{11})
                                    ->homePlacement());
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{12})
              ->placement() == product->rootComponentDefinition()
                                    ->find(OccurrenceId{12})
                                    ->homePlacement());
  }

  return 0;
}
