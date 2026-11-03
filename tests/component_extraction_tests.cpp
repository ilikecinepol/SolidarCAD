#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ComponentGraphAnalysis.h"
#include "model/JoinBodiesFeature.h"
#include "model/MirrorFeature.h"
#include "model/ProductDocument.h"
#include "model/ExtrudeFeature.h"

namespace solidar {

class ComponentExtractionDependencyTestAccess final {
 public:
  static bool setComponentState(ProductDocument& product, DefinitionId id,
                                DefinitionStateId state,
                                std::uint64_t nextStateValue) {
    const auto found = product.definitions_.definitions_.find(id);
    if (found == product.definitions_.definitions_.end()) return false;
    auto* component = std::get_if<ComponentDefinition>(&found->second);
    if (!component) return false;
    component->stateId_ = state;
    component->nextStateIdValue_ = nextStateValue;
    return true;
  }
};

}  // namespace solidar

namespace {

using namespace solidar;

class SketchBoxFeature final : public ShapeFeature {
 public:
  SketchBoxFeature(FeatureId id, SketchId sketchId, std::string name)
      : ShapeFeature(id, std::move(name)), sketchId_(sketchId) {}

  FeatureDependencies dependencies() const override {
    FeatureDependencies result;
    if (sketchId_ != kInvalidSketchId) result.sketchIds.push_back(sketchId_);
    return result;
  }
  std::unique_ptr<Feature> clone() const override {
    return std::make_unique<SketchBoxFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    clearShape();
    setShape(std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(8.0, 6.0, 4.0).Shape()));
    markValid();
    return true;
  }

 private:
  SketchId sketchId_{kInvalidSketchId};
};

class FailingFeature final : public ShapeFeature {
 public:
  explicit FailingFeature(FeatureId id) : ShapeFeature(id, "failing") {}
  std::unique_ptr<Feature> clone() const override {
    return std::make_unique<FailingFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    clearShape();
    markError("intentional recompute failure");
    return false;
  }
};

class FixedSuccessor final : public ProductDocumentSuccessor {
 public:
  explicit FixedSuccessor(ProductDocumentSuccessorStatus status)
      : status_(status) {}

  ProductDocumentSuccessorStatus prepare(
      const ProductDocument& candidate,
      ProductDocumentSuccessorImpact impact) noexcept override {
    ++calls;
    lastKind = candidate.rootDefinitionKind();
    lastImpact = impact;
    return status_;
  }

  int calls{};
  DefinitionKind lastKind{DefinitionKind::Part};
  ProductDocumentSuccessorImpact lastImpact{
      ProductDocumentSuccessorImpact::TreeOnly};

 private:
  ProductDocumentSuccessorStatus status_;
};

bool samePlacement(const SketchPlacement& left,
                   const SketchPlacement& right) {
  return left.origin.x == right.origin.x &&
         left.origin.y == right.origin.y &&
         left.origin.z == right.origin.z &&
         left.xDirection.x == right.xDirection.x &&
         left.xDirection.y == right.xDirection.y &&
         left.xDirection.z == right.xDirection.z &&
         left.yDirection.x == right.yDirection.x &&
         left.yDirection.y == right.yDirection.y &&
         left.yDirection.z == right.yDirection.z;
}

std::unique_ptr<Document> makeTwoBodyDocument() {
  auto document = std::make_unique<Document>();
  auto& firstSketch = document->addSketch(SketchId{101}, "first sketch");
  firstSketch.geometry.addRectangle({0.0, 0.0}, {12.0, 8.0});
  const SketchId firstSketchId = firstSketch.id;
  auto& secondSketch = document->addSketch(SketchId{102}, "second sketch");
  secondSketch.geometry.addRectangle({20.0, 0.0}, {30.0, 6.0});
  const SketchId secondSketchId = secondSketch.id;
  auto& firstBody = document->addBody(BodyId{201}, "first body");
  firstBody.addFeature(std::make_unique<SketchBoxFeature>(
      FeatureId{301}, firstSketchId, "first feature"));
  auto& secondBody = document->addBody(BodyId{202}, "second body");
  secondBody.addFeature(std::make_unique<SketchBoxFeature>(
      FeatureId{302}, secondSketchId, "second feature"));
  document->addReferenceImage("reference", "image.png", "XY",
                              SketchPlacement::xy(), 640, 480);
  const auto recomputed = document->recomputeDetailed();
  if (!recomputed.success) {
    std::cerr << "fixture recompute: " << document->rebuildError() << '\n';
    for (const auto& failure : recomputed.primaryFailures)
      std::cerr << "  " << failure.failure.detail << '\n';
  }
  CHECK(recomputed.success);
  return document;
}

std::unique_ptr<ProductDocument> makePartProduct() {
  DefinitionRegistry registry;
  CHECK(registry.insert(
            PartDefinition(DefinitionId{1}, makeTwoBodyDocument())) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(registry), DefinitionId{1});
  CHECK(created);
  return std::move(created.product);
}

ComponentDrawing drawing(std::uint64_t id, std::string name,
                         std::vector<BodyId> bodies = {}) {
  return ComponentDrawing(DrawingId{id}, std::move(name),
                          ComponentDrawingPaper::A4,
                          ComponentDrawingOrientation::Landscape, 2.0,
                          std::move(bodies));
}

std::unique_ptr<ProductDocument> makeDrawingComponentProduct(
    std::vector<ComponentDrawing> rootDrawings) {
  ComponentDefinition root(DefinitionId{1}, makeTwoBodyDocument(),
                           std::move(rootDrawings));
  auto sharedDocument = std::make_unique<Document>();
  sharedDocument->addBody(BodyId{901}, "shared body");
  ComponentDefinition shared(
      DefinitionId{2}, std::move(sharedDocument),
      std::vector<ComponentDrawing>{
          drawing(901, "shared drawing", {BodyId{901}})});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{51}, DefinitionId{2},
            RigidTransform3d::identity())) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{52}, DefinitionId{2},
            RigidTransform3d::identity())) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(root));
  definitions.push_back(std::move(shared));
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{1});
  CHECK(created);
  return std::move(created.product);
}

ProductIncarnationId incarnationMarker() {
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(
            DefinitionId{9000}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(registry),
                                         DefinitionId{9000});
  CHECK(created);
  return created.product->incarnation();
}

void checkOnlyMarkerConsumed(ProductIncarnationId before,
                             ProductIncarnationId after) {
  CHECK(before.isValid());
  CHECK(after.value() == before.value() + 1);
}

ComponentExtractionAddress addressFor(const ProductDocument& product,
                                      BodyId bodyId,
                                      DefinitionId childDefinitionId,
                                      OccurrenceId occurrenceId) {
  return {product.incarnation(), product.rootDefinitionId(),
          product.rootDefinitionId(),
          product.definitionStateId(product.rootDefinitionId()), bodyId,
          childDefinitionId, occurrenceId};
}

ComponentExtractionAddress nestedAddressFor(
    const ProductDocument& product, DefinitionId ownerDefinitionId,
    BodyId bodyId, DefinitionId childDefinitionId,
    OccurrenceId occurrenceId) {
  return {product.incarnation(), product.rootDefinitionId(),
          ownerDefinitionId, product.definitionStateId(ownerDefinitionId),
          bodyId, childDefinitionId, occurrenceId};
}

std::unique_ptr<ShapeFeature> imported(FeatureId id, double offset) {
  return std::make_unique<ImportedShapeFeature>(
      id,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(gp_Pnt(offset, 0.0, 0.0), 4.0, 4.0, 4.0)
              .Shape()),
      "imported");
}

}  // namespace

int main() {
  // Planning and staging preserve authored order/IDs/topology while images
  // remain in the parent definition.
  {
    auto source = makeTwoBodyDocument();
    const auto* originalFeature = source->findFeature(BodyId{201},
                                                      FeatureId{301});
    CHECK(originalFeature && originalFeature->isValid());
    const ShapeRevision originalRevision = originalFeature->shapeRevision();
    const auto originalShape = originalFeature->shape();
    const auto plan = source->planComponentExtraction(BodyId{201});
    CHECK(plan);
    CHECK(plan.originalBodyIndex == 0);
    CHECK(plan.featureIds == std::vector<FeatureId>{FeatureId{301}});
    CHECK(plan.sketches.size() == 1);
    CHECK(plan.sketches[0].id == SketchId{101});
    CHECK(plan.sketches[0].originalIndex == 0);

    const auto staged = source->stageComponentExtraction(plan);
    CHECK(staged);
    CHECK(source->bodies().size() == 2);
    CHECK(staged.parentAfter->bodies().size() == 1);
    CHECK(staged.parentAfter->bodies()[0].id() == BodyId{202});
    CHECK(staged.parentAfter->sketches().size() == 1);
    CHECK(staged.parentAfter->sketches()[0].id == SketchId{102});
    CHECK(staged.parentAfter->referenceImages().size() == 1);
    CHECK(staged.child->bodies().size() == 1);
    CHECK(staged.child->bodies()[0].id() == BodyId{201});
    CHECK(staged.child->sketches().size() == 1);
    CHECK(staged.child->sketches()[0].id == SketchId{101});
    CHECK(staged.child->referenceImages().empty());
    const auto* childFeature =
        staged.child->findFeature(BodyId{201}, FeatureId{301});
    CHECK(childFeature && childFeature->shapeRevision() == originalRevision);
    CHECK(childFeature->shape().get() == originalShape.get());

    Document changed(*source);
    changed.findBody(BodyId{201})
        ->addFeature(imported(FeatureId{399}, 50.0));
    const auto stale = changed.stageComponentExtraction(plan);
    CHECK(stale.status == ComponentExtractionStageStatus::StalePlan);
    CHECK(changed.bodies().size() == 2);
  }

  // Shared sketches and cross-body Features remain rejected. A sole external
  // face support is safe to split: its resolved placement is frozen at the
  // component boundary in either direction.
  {
    Document shared;
    auto& sketch = shared.addSketch(SketchId{1}, "shared");
    sketch.geometry.addRectangle({0.0, 0.0}, {5.0, 5.0});
    shared.addBody(BodyId{10}, "a")
        .addFeature(std::make_unique<SketchBoxFeature>(
            FeatureId{20}, sketch.id, "a"));
    shared.addBody(BodyId{11}, "b")
        .addFeature(std::make_unique<SketchBoxFeature>(
            FeatureId{21}, sketch.id, "b"));
    CHECK(shared.planComponentExtraction(BodyId{10}).status ==
          ComponentExtractionPlanStatus::CrossDefinitionDependency);

    Document joined;
    auto& first = joined.addBody(BodyId{30}, "first");
    first.addFeature(imported(FeatureId{40}, 0.0));
    auto& second = joined.addBody(BodyId{31}, "second");
    second.addFeature(imported(FeatureId{41}, 2.0));
    joined.addBody(BodyId{32}, "result")
        .addFeature(std::make_unique<JoinBodiesFeature>(
            FeatureId{42}, first.id(), FeatureId{40}, second.id(),
            FeatureId{41}, "join"));
    CHECK(joined.planComponentExtraction(BodyId{30}).status ==
          ComponentExtractionPlanStatus::CrossDefinitionDependency);
    CHECK(joined.planComponentExtraction(BodyId{32}).status ==
          ComponentExtractionPlanStatus::CrossDefinitionDependency);

    Document supported;
    auto& supportBody = supported.addBody(BodyId{50}, "support");
    supportBody.addFeature(imported(FeatureId{60}, 0.0));
    auto& retainedSketch =
        supported.addSketch(SketchId{70}, "retained supported sketch");
    retainedSketch.geometry.addRectangle({0.0, 0.0}, {5.0, 5.0});
    retainedSketch.support =
        {SketchSupportType::Face,
         FaceReference{supportBody.id(), FeatureId{60}, 0}};
    CHECK(supported.recomputeDetailed().success);
    const auto retainedPlacement = retainedSketch.placement;
    const auto supportedPlan = supported.planComponentExtraction(BodyId{50});
    CHECK(supportedPlan);
    CHECK(supportedPlan.frozenSketchSupports.size() == 1);
    CHECK(supportedPlan.frozenSketchSupports[0].id == SketchId{70});
    CHECK(!supportedPlan.frozenSketchSupports[0].movesToChild);
    const auto supportedStage =
        supported.stageComponentExtraction(supportedPlan);
    CHECK(supportedStage);
    const auto* fixedRetained = supportedStage.parentAfter->findSketch(
        SketchId{70});
    CHECK(fixedRetained);
    CHECK(fixedRetained->support.type == SketchSupportType::BasePlane);
    CHECK(fixedRetained->supportResolved);
    CHECK(samePlacement(fixedRetained->placement, retainedPlacement));
    CHECK(supported.findSketch(SketchId{70})->support.type ==
          SketchSupportType::Face);

    Document inverseSupport;
    auto& retainedBody = inverseSupport.addBody(BodyId{80}, "retained");
    retainedBody.addFeature(imported(FeatureId{81}, 0.0));
    auto& movedSketch =
        inverseSupport.addSketch(SketchId{82}, "moved supported sketch");
    movedSketch.geometry.addRectangle({0.0, 0.0}, {5.0, 5.0});
    movedSketch.support =
        {SketchSupportType::Face,
         FaceReference{retainedBody.id(), FeatureId{81}, 0}};
    inverseSupport.addBody(BodyId{83}, "moved")
        .addFeature(std::make_unique<SketchBoxFeature>(
            FeatureId{84}, movedSketch.id, "consumer"));
    CHECK(inverseSupport.recomputeDetailed().success);
    const auto movedPlacement = movedSketch.placement;
    const auto inversePlan =
        inverseSupport.planComponentExtraction(BodyId{83});
    CHECK(inversePlan);
    CHECK(inversePlan.frozenSketchSupports.size() == 1);
    CHECK(inversePlan.frozenSketchSupports[0].id == SketchId{82});
    CHECK(inversePlan.frozenSketchSupports[0].movesToChild);
    const auto inverseStage =
        inverseSupport.stageComponentExtraction(inversePlan);
    CHECK(inverseStage);
    const auto* fixedMoved = inverseStage.child->findSketch(SketchId{82});
    CHECK(fixedMoved);
    CHECK(fixedMoved->support.type == SketchSupportType::BasePlane);
    CHECK(fixedMoved->supportResolved);
    CHECK(samePlacement(fixedMoved->placement, movedPlacement));
    CHECK(inverseSupport.findSketch(SketchId{82})->support.type ==
          SketchSupportType::Face);
  }

  // Candidate recompute failures are typed and leave the source untouched.
  {
    Document childFailure;
    childFailure.addBody(BodyId{90}, "selected")
        .addFeature(std::make_unique<FailingFeature>(FeatureId{91}));
    auto childPlan = childFailure.planComponentExtraction(BodyId{90});
    CHECK(childPlan);
    CHECK(childFailure.stageComponentExtraction(childPlan).status ==
          ComponentExtractionStageStatus::ChildRecomputeFailed);
    CHECK(childFailure.bodies().size() == 1);

    Document parentFailure;
    parentFailure.addBody(BodyId{92}, "selected");
    parentFailure.addBody(BodyId{93}, "retained")
        .addFeature(std::make_unique<FailingFeature>(FeatureId{94}));
    auto parentPlan = parentFailure.planComponentExtraction(BodyId{92});
    CHECK(parentPlan);
    CHECK(parentFailure.stageComponentExtraction(parentPlan).status ==
          ComponentExtractionStageStatus::ParentRecomputeFailed);
    CHECK(parentFailure.bodies().size() == 2);
  }

  // First Apply converts a standalone Part root to a Component root. Undo is
  // a strict inverse and Redo reuses the same scalar IDs and original indices.
  ComponentExtractionTransaction firstTransaction;
  {
    auto product = makePartProduct();
    const auto originalIncarnation = product->incarnation();
    const auto originalAddress = addressFor(
        *product, BodyId{201}, DefinitionId{2}, OccurrenceId{11});
    auto prepared = product->prepareComponentExtraction(originalAddress);
    CHECK(prepared);
    CHECK(prepared.transaction->convertedPartRoot);
    firstTransaction = *prepared.transaction;
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    const auto beforeApplyIncarnation = incarnationMarker();
    CHECK(product->applyComponentExtraction(firstTransaction, accept) ==
          ComponentExtractionStatus::Committed);
    checkOnlyMarkerConsumed(beforeApplyIncarnation, incarnationMarker());
    CHECK(accept.calls == 1);
    CHECK(accept.lastKind == DefinitionKind::Component);
    CHECK(accept.lastImpact ==
          ProductDocumentSuccessorImpact::FullProjection);
    CHECK(product->incarnation() == originalIncarnation);
    CHECK(product->identity() == std::nullopt);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Component);
    CHECK(product->definitions().size() == 2);
    CHECK(product->rootComponentDefinition()->document().bodies().size() == 1);
    CHECK(product->rootComponentDefinition()->document().bodies()[0].id() ==
          BodyId{202});
    CHECK(product->rootComponentDefinition()->document()
              .referenceImages()
              .size() == 1);
    const auto* child = product->definitions().findComponent(DefinitionId{2});
    CHECK(child && child->document().bodies().size() == 1);
    CHECK(child->document().bodies()[0].id() == BodyId{201});
    CHECK(child->document().referenceImages().empty());

    CHECK(product->prepareComponentExtraction(addressFor(
              *product, BodyId{202}, DefinitionId{2}, OccurrenceId{99}))
              .status == ComponentExtractionStatus::DuplicateDefinitionId);
    CHECK(product->prepareComponentExtraction(addressFor(
              *product, BodyId{202}, DefinitionId{4}, OccurrenceId{11}))
              .status == ComponentExtractionStatus::DuplicateOccurrenceId);

    // A second extraction works directly inside the already unified root.
    auto secondPrepared = product->prepareComponentExtraction(addressFor(
        *product, BodyId{202}, DefinitionId{3}, OccurrenceId{12}));
    CHECK(secondPrepared);
    CHECK(!secondPrepared.transaction->convertedPartRoot);
    const auto secondTransaction = *secondPrepared.transaction;
    CHECK(product->applyComponentExtraction(secondTransaction, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(product->definitions().size() == 3);
    CHECK(product->rootComponentDefinition()->document().bodies().empty());
    FixedSuccessor rejectRevert(ProductDocumentSuccessorStatus::Rejected);
    const auto beforeRejectedRevert = incarnationMarker();
    CHECK(product->revertComponentExtraction(secondTransaction,
                                              rejectRevert) ==
          ComponentExtractionStatus::SuccessorRejected);
    checkOnlyMarkerConsumed(beforeRejectedRevert, incarnationMarker());
    CHECK(product->definitions().size() == 3);

    CHECK(ComponentExtractionDependencyTestAccess::setComponentState(
        *product, DefinitionId{3}, DefinitionStateId{2}, 3));
    CHECK(product->revertComponentExtraction(secondTransaction, accept) ==
          ComponentExtractionStatus::StructureMismatch);
    CHECK(accept.calls == 2);
    CHECK(product->definitions().size() == 3);
    CHECK(ComponentExtractionDependencyTestAccess::setComponentState(
        *product, DefinitionId{3},
        secondTransaction.createdChildStateId,
        secondTransaction.createdChildNextStateIdValue));

    const auto beforeSuccessfulRevert = incarnationMarker();
    CHECK(product->revertComponentExtraction(secondTransaction, accept) ==
          ComponentExtractionStatus::Reverted);
    checkOnlyMarkerConsumed(beforeSuccessfulRevert, incarnationMarker());
    CHECK(product->definitions().size() == 2);
    CHECK(product->rootComponentDefinition()->document().bodies()[0].id() ==
          BodyId{202});

    CHECK(product->revertComponentExtraction(firstTransaction, accept) ==
          ComponentExtractionStatus::Reverted);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);
    CHECK(product->definitions().size() == 1);
    CHECK(product->rootDocument().bodies().size() == 2);
    CHECK(product->rootDocument().bodies()[0].id() == BodyId{201});
    CHECK(product->rootDocument().bodies()[1].id() == BodyId{202});
    CHECK(product->rootDocument().sketches()[0].id == SketchId{101});
    CHECK(product->rootDocument().sketches()[1].id == SketchId{102});
    const auto beforeRedo = incarnationMarker();
    CHECK(product->applyComponentExtraction(firstTransaction, accept) ==
          ComponentExtractionStatus::Committed);
    checkOnlyMarkerConsumed(beforeRedo, incarnationMarker());
    CHECK(product->definitions().findComponent(DefinitionId{2}) != nullptr);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11}) !=
          nullptr);
  }

  // Extraction from a non-root component creates a genuinely nested child
  // definition and remains exactly reversible.
  {
    auto product = makePartProduct();
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    auto first = product->prepareComponentExtraction(addressFor(
        *product, BodyId{201}, DefinitionId{2}, OccurrenceId{11}));
    CHECK(first);
    CHECK(product->applyComponentExtraction(*first.transaction, accept) ==
          ComponentExtractionStatus::Committed);

    auto nested = product->prepareComponentExtraction(nestedAddressFor(
        *product, DefinitionId{2}, BodyId{201}, DefinitionId{3},
        OccurrenceId{12}));
    CHECK(nested);
    CHECK(!nested.transaction->convertedPartRoot);
    CHECK(product->applyComponentExtraction(*nested.transaction, accept) ==
          ComponentExtractionStatus::Committed);
    CHECK(product->definitions().size() == 3);
    const auto* owner =
        product->definitions().findComponent(DefinitionId{2});
    const auto* child =
        product->definitions().findComponent(DefinitionId{3});
    CHECK(owner && owner->document().bodies().empty());
    CHECK(owner->find(OccurrenceId{12}));
    CHECK(owner->find(OccurrenceId{12})->targetDefinitionId() ==
          DefinitionId{3});
    CHECK(child && child->document().findBody(BodyId{201}));
    const auto traversal = expandComponentGraph(
        product->definitions(), product->rootDefinitionId());
    CHECK(traversal);
    CHECK(std::any_of(
        traversal.visits.begin(), traversal.visits.end(),
        [](const auto& visit) {
          return visit.definitionId == DefinitionId{3} && visit.path &&
                 visit.path->entries() ==
                     std::vector<OccurrenceId>{OccurrenceId{11},
                                               OccurrenceId{12}};
        }));

    CHECK(product->revertComponentExtraction(*nested.transaction, accept) ==
          ComponentExtractionStatus::Reverted);
    owner = product->definitions().findComponent(DefinitionId{2});
    CHECK(owner && owner->document().findBody(BodyId{201}));
    CHECK(!owner->find(OccurrenceId{12}));
    CHECK(!product->definitions().findComponent(DefinitionId{3}));
    CHECK(product->applyComponentExtraction(*nested.transaction, accept) ==
          ComponentExtractionStatus::Committed);
  }

  // Stale plans, successor failures, collision guards and deterministic
  // failure seams are exact model no-ops.
  {
    auto product = makePartProduct();
    auto prepared = product->prepareComponentExtraction(addressFor(
        *product, BodyId{201}, DefinitionId{2}, OccurrenceId{11}));
    CHECK(prepared);
    FixedSuccessor reject(ProductDocumentSuccessorStatus::Rejected);
    const auto beforeRejectedApply = incarnationMarker();
    CHECK(product->applyComponentExtraction(*prepared.transaction, reject) ==
          ComponentExtractionStatus::SuccessorRejected);
    checkOnlyMarkerConsumed(beforeRejectedApply, incarnationMarker());
    CHECK(reject.calls == 1);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);
    CHECK(product->definitions().size() == 1);
    CHECK(product->rootDocument().bodies().size() == 2);

    FixedSuccessor resource(
        ProductDocumentSuccessorStatus::ResourceFailure);
    CHECK(product->applyComponentExtraction(*prepared.transaction, resource) ==
          ComponentExtractionStatus::SuccessorResourceFailure);
    CHECK(resource.calls == 1);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);

    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyComponentExtraction(
              *prepared.transaction, accept,
              ComponentExtractionFailurePoint::BeforeCommit) ==
          ComponentExtractionStatus::ResourceFailure);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);
    CHECK(product->definitions().size() == 1);

    product->rootDocument()
        .findBody(BodyId{201})
        ->addFeature(imported(FeatureId{399}, 60.0));
    CHECK(product->applyComponentExtraction(*prepared.transaction, accept) ==
          ComponentExtractionStatus::StalePlan);
    CHECK(product->rootDefinitionKind() == DefinitionKind::Part);

    auto missing = product->prepareComponentExtraction(addressFor(
        *product, BodyId{999}, DefinitionId{4}, OccurrenceId{14}));
    CHECK(missing.status == ComponentExtractionStatus::BodyMissing);
    auto invalid = addressFor(*product, BodyId{201}, DefinitionId{1},
                              OccurrenceId{15});
    CHECK(product->prepareComponentExtraction(invalid).status ==
          ComponentExtractionStatus::InvalidAddress);
  }

  // Definition-owned drawings follow the extracted body without ID remap.
  // Empty-source and retained-body drawings stay in the parent; shared
  // nested definitions are byte-for-byte unaffected. Revert restores order,
  // state and next-state so a repeated prepare is identical.
  {
    auto product = makeDrawingComponentProduct(
        {drawing(1, "empty"),
         drawing(2, "moved", {BodyId{201}}),
         drawing(3, "retained", {BodyId{202}})});
    const auto beforeState = product->definitionStateId(DefinitionId{1});
    const auto* sharedBefore =
        product->definitions().findComponent(DefinitionId{2});
    CHECK(sharedBefore && sharedBefore->drawings().size() == 1);
    const auto sharedDrawings = sharedBefore->drawings();
    auto prepared = product->prepareComponentExtraction(addressFor(
        *product, BodyId{201}, DefinitionId{3}, OccurrenceId{53}));
    CHECK(prepared);
    CHECK(prepared.transaction->beforeOwnerDrawings.size() == 3);
    CHECK(prepared.transaction->afterOwnerDrawings.size() == 2);
    CHECK(prepared.transaction->afterOwnerDrawings[0].id() == DrawingId{1});
    CHECK(prepared.transaction->afterOwnerDrawings[1].id() == DrawingId{3});
    CHECK(prepared.transaction->createdChildDrawings.size() == 1);
    CHECK(prepared.transaction->createdChildDrawings[0].id() == DrawingId{2});
    const auto transaction = *prepared.transaction;
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    auto forged = transaction;
    forged.afterOwnerDrawings.clear();
    CHECK(product->applyComponentExtraction(forged, accept) ==
          ComponentExtractionStatus::StalePlan);
    CHECK(product->definitionStateId(DefinitionId{1}) == beforeState);
    CHECK(product->rootComponentDefinition()->drawings() ==
          transaction.beforeOwnerDrawings);
    CHECK(product->applyComponentExtraction(transaction, accept) ==
          ComponentExtractionStatus::Committed);
    const auto* root = product->rootComponentDefinition();
    const auto* child =
        product->definitions().findComponent(DefinitionId{3});
    CHECK(root && child);
    CHECK(root->stateId() == transaction.afterOwnerStateId);
    CHECK(root->drawings() == transaction.afterOwnerDrawings);
    CHECK(child->drawings() == transaction.createdChildDrawings);
    CHECK(child->findDrawing(DrawingId{2}) != nullptr);
    CHECK(product->definitions().findComponent(DefinitionId{2})->drawings() ==
          sharedDrawings);

    CHECK(product->revertComponentExtraction(transaction, accept) ==
          ComponentExtractionStatus::Reverted);
    root = product->rootComponentDefinition();
    CHECK(root && root->stateId() == beforeState);
    CHECK(root->drawings() == transaction.beforeOwnerDrawings);
    CHECK(product->definitions().findComponent(DefinitionId{3}) == nullptr);
    const auto repeated = product->prepareComponentExtraction(addressFor(
        *product, BodyId{201}, DefinitionId{3}, OccurrenceId{53}));
    CHECK(repeated);
    CHECK(repeated.transaction->beforeOwnerStateId ==
          transaction.beforeOwnerStateId);
    CHECK(repeated.transaction->afterOwnerStateId ==
          transaction.afterOwnerStateId);
    CHECK(repeated.transaction->beforeNextStateIdValue ==
          transaction.beforeNextStateIdValue);
    CHECK(repeated.transaction->afterNextStateIdValue ==
          transaction.afterNextStateIdValue);
    CHECK(repeated.transaction->beforeOwnerDrawings ==
          transaction.beforeOwnerDrawings);
  }

  // A drawing spanning the moved and retained bodies is a definition-boundary
  // dependency. The rejected prepare is an exact model/state no-op.
  {
    auto product = makeDrawingComponentProduct(
        {drawing(10, "mixed", {BodyId{201}, BodyId{202}})});
    const auto state = product->definitionStateId(DefinitionId{1});
    const auto before = product->rootComponentDefinition()->drawings();
    const auto rejected = product->prepareComponentExtraction(addressFor(
        *product, BodyId{201}, DefinitionId{3}, OccurrenceId{53}));
    CHECK(rejected.status ==
          ComponentExtractionStatus::CrossDefinitionDependency);
    CHECK(product->definitionStateId(DefinitionId{1}) == state);
    CHECK(product->rootComponentDefinition()->drawings() == before);
    CHECK(product->definitions().findComponent(DefinitionId{3}) == nullptr);
  }

  // With one body, every body-referencing drawing moves while empty-source
  // drawings remain authored by the parent component.
  {
    auto document = std::make_unique<Document>();
    document->addBody(BodyId{70}, "only");
    ComponentDefinition root(
        DefinitionId{70}, std::move(document),
        std::vector<ComponentDrawing>{drawing(70, "empty"),
                                      drawing(71, "all bodies", {BodyId{70}})});
    std::vector<ComponentDefinition> definitions;
    definitions.push_back(std::move(root));
    auto created = ProductDocument::createComponentGraph(
        std::move(definitions), DefinitionId{70});
    CHECK(created);
    auto prepared = created.product->prepareComponentExtraction(addressFor(
        *created.product, BodyId{70}, DefinitionId{71}, OccurrenceId{70}));
    CHECK(prepared);
    CHECK(prepared.transaction->afterOwnerDrawings.size() == 1);
    CHECK(prepared.transaction->afterOwnerDrawings[0].id() == DrawingId{70});
    CHECK(prepared.transaction->createdChildDrawings.size() == 1);
    CHECK(prepared.transaction->createdChildDrawings[0].id() ==
          DrawingId{71});
  }


  // Authored V2 topology identity survives stage, Apply, strict Revert and
  // deterministic Redo unchanged (not merely the runtime shape pointer).
  {
    auto document = std::make_unique<Document>();
    auto& sketch = document->addSketch(SketchId{501}, "topology profile");
    sketch.geometry.addRectangle({0.0, 0.0}, {14.0, 9.0});
    const SketchId sketchId = sketch.id;
    auto& body = document->addBody(BodyId{502}, "topology body");
    auto extrude = std::make_unique<ExtrudeFeature>(
        FeatureId{503}, sketchId, 6.0, "topology extrude");
    auto* const extrudePtr = extrude.get();
    body.addFeature(std::move(extrude));
    CHECK(document->recomputeDetailed().success);
    const auto createdFace =
        extrudePtr->createFaceReferenceV2(body.id(), 0);
    CHECK(createdFace);
    CHECK(createdFace.reference.isV2());
    const FaceReference authoredFace = createdFace.reference;
    body.addFeature(std::make_unique<MirrorFeature>(
        FeatureId{504}, extrudePtr->id(), authoredFace, "topology mirror"));
    CHECK(document->recomputeDetailed().success);
    document->addBody(BodyId{505}, "retained body")
        .addFeature(imported(FeatureId{506}, 30.0));
    CHECK(document->recomputeDetailed().success);

    const auto plan = document->planComponentExtraction(BodyId{502});
    CHECK(plan);
    const auto staged = document->stageComponentExtraction(plan);
    CHECK(staged);
    const auto* stagedMirror = dynamic_cast<const MirrorFeature*>(
        staged.child->findFeature(BodyId{502}, FeatureId{504}));
    CHECK(stagedMirror && stagedMirror->planeFace());
    CHECK(*stagedMirror->planeFace() == authoredFace);

    DefinitionRegistry registry;
    CHECK(registry.insert(PartDefinition(DefinitionId{20},
                                         std::move(document))) ==
          DefinitionInsertStatus::Inserted);
    auto createdProduct = ProductDocument::create(std::move(registry),
                                                  DefinitionId{20});
    CHECK(createdProduct);
    auto& product = *createdProduct.product;
    auto prepared = product.prepareComponentExtraction(addressFor(
        product, BodyId{502}, DefinitionId{21}, OccurrenceId{22}));
    CHECK(prepared);
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product.applyComponentExtraction(*prepared.transaction, accept) ==
          ComponentExtractionStatus::Committed);
    const auto* appliedMirror = dynamic_cast<const MirrorFeature*>(
        product.componentDocument(DefinitionId{21})
            ->findFeature(BodyId{502}, FeatureId{504}));
    CHECK(appliedMirror && appliedMirror->planeFace());
    CHECK(*appliedMirror->planeFace() == authoredFace);
    CHECK(product.revertComponentExtraction(*prepared.transaction, accept) ==
          ComponentExtractionStatus::Reverted);
    const auto* revertedMirror = dynamic_cast<const MirrorFeature*>(
        product.rootDocument().findFeature(BodyId{502}, FeatureId{504}));
    CHECK(revertedMirror && revertedMirror->planeFace());
    CHECK(*revertedMirror->planeFace() == authoredFace);
    CHECK(product.applyComponentExtraction(*prepared.transaction, accept) ==
          ComponentExtractionStatus::Committed);
    const auto* redoneMirror = dynamic_cast<const MirrorFeature*>(
        product.componentDocument(DefinitionId{21})
            ->findFeature(BodyId{502}, FeatureId{504}));
    CHECK(redoneMirror && redoneMirror->planeFace());
    CHECK(*redoneMirror->planeFace() == authoredFace);
  }

  return EXIT_SUCCESS;
}
