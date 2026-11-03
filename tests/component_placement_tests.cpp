#include "TestAssertions.h"

#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "model/ProductDocument.h"

namespace solidar {

class ComponentExtractionDependencyTestAccess final {
 public:
  static bool setComponentState(ProductDocument& product, DefinitionId id,
                                DefinitionStateId state,
                                std::uint64_t nextStateValue) {
    auto* component = product.definitions_.findComponentMutable(id);
    if (!component) return false;
    component->stateId_ = state;
    component->nextStateIdValue_ = nextStateValue;
    return true;
  }
  static std::pair<DefinitionStateId, std::uint64_t> componentState(
      const ProductDocument& product, DefinitionId id) {
    const auto* component = product.definitions_.findComponent(id);
    CHECK(component);
    return {component->stateId_, component->nextStateIdValue_};
  }
};

}  // namespace solidar

namespace {
using namespace solidar;

bool near(double left, double right) {
  return std::abs(left - right) <= 1.0e-10;
}

RigidTransform3d placement(double x, double y, double z,
                           std::array<double, 4> q = {1, 0, 0, 0}) {
  const auto result = RigidTransform3d::create({x, y, z}, q);
  CHECK(result);
  return *result.transform;
}

std::unique_ptr<ProductDocument> makeProduct(
    OccurrenceFlags parentFlags = {}, OccurrenceFlags nestedFlags = {},
    OccurrenceFlags directFlags = {}) {
  ComponentDefinition leaf(DefinitionId{3}, std::make_unique<Document>());
  ComponentDefinition branch(DefinitionId{2}, std::make_unique<Document>());
  CHECK(branch.insert(ComponentOccurrence(
            OccurrenceId{20}, DefinitionId{3}, placement(1, 0, 0),
            nestedFlags)) == ComponentOccurrenceInsertStatus::Inserted);
  ComponentDefinition root(DefinitionId{1}, std::make_unique<Document>());
  const double half = std::sqrt(0.5);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, DefinitionId{2},
            placement(10, 0, 0, {half, 0, 0, half}), parentFlags)) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{11}, DefinitionId{3}, placement(4, 0, 0),
            directFlags)) == ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> components;
  components.push_back(std::move(root));
  components.push_back(std::move(branch));
  components.push_back(std::move(leaf));
  auto created = ProductDocument::createComponentGraph(
      std::move(components), DefinitionId{1});
  CHECK(created);
  return std::move(created.product);
}

OccurrencePath path(std::initializer_list<std::uint64_t> values) {
  std::vector<OccurrenceId> ids;
  for (const auto value : values) ids.push_back(OccurrenceId{value});
  auto created = OccurrencePath::create(std::move(ids));
  CHECK(created);
  return *created.path;
}

ComponentPlacementRequest requestFor(
    const ProductDocument& product, OccurrencePath occurrencePath,
    DefinitionId target, DefinitionId owner, RigidTransform3d expected,
    std::array<double, 3> delta) {
  return {product.incarnation(), product.rootDefinitionId(),
          std::move(occurrencePath), target,
          product.definitionStateId(owner), std::move(expected), delta};
}

ComponentHomePlacementRequest homeRequestFor(
    const ProductDocument& product, OccurrencePath occurrencePath,
    DefinitionId target, DefinitionId owner, RigidTransform3d current,
    RigidTransform3d home, ComponentHomePlacementOperation operation) {
  return {product.incarnation(), product.rootDefinitionId(),
          std::move(occurrencePath), target, owner,
          product.definitionStateId(owner), std::move(current),
          std::move(home), operation};
}

class FixedSuccessor final : public ProductDocumentSuccessor {
 public:
  explicit FixedSuccessor(ProductDocumentSuccessorStatus value)
      : value_(value) {}
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&, ProductDocumentSuccessorImpact impact) noexcept
      override {
    ++calls;
    lastImpact = impact;
    return value_;
  }
  int calls{};
  ProductDocumentSuccessorImpact lastImpact{
      ProductDocumentSuccessorImpact::TreeOnly};
 private:
  ProductDocumentSuccessorStatus value_;
};

class ReentrantHomeSuccessor final : public ProductDocumentSuccessor {
 public:
  explicit ReentrantHomeSuccessor(ComponentHomePlacementRequest request)
      : request_(std::move(request)) {}

  ProductDocumentSuccessorStatus prepare(
      const ProductDocument& candidate,
      ProductDocumentSuccessorImpact) noexcept override {
    reentrantStatus =
        candidate.prepareComponentHomePlacement(request_).status;
    return ProductDocumentSuccessorStatus::Rejected;
  }

  ComponentHomePlacementStatus reentrantStatus{
      ComponentHomePlacementStatus::InvalidRequest};

 private:
  ComponentHomePlacementRequest request_;
};

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

  // A nested drag is converted through the inverse parent rotation. Rotation
  // is byte-stable, and a second occurrence of the shared leaf is isolated.
  {
    auto product = makeProduct();
    const auto incarnationBefore = incarnationProbe();
    const auto nestedBefore =
        product->definitions().findComponent(DefinitionId{2})
            ->find(OccurrenceId{20})->placement();
    const auto directBefore =
        product->rootComponentDefinition()->find(OccurrenceId{11})->placement();
    const auto prepared = product->prepareComponentPlacement(requestFor(
        *product, path({10, 20}), DefinitionId{3}, DefinitionId{2},
        nestedBefore, {0, 2, 0}));
    CHECK(prepared);
    const auto moved =
        prepared.transaction->afterPlacement.translationMm();
    CHECK(near(moved[0], 3.0));
    CHECK(near(moved[1], 0.0));
    CHECK(near(moved[2], 0.0));
    CHECK(prepared.transaction->afterPlacement.quaternionWxyz() ==
          nestedBefore.quaternionWxyz());
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyComponentPlacement(*prepared.transaction, accept) ==
          ComponentPlacementStatus::Committed);
    CHECK(accept.lastImpact == ProductDocumentSuccessorImpact::FullProjection);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == directBefore);
    CHECK(product->revertComponentPlacement(*prepared.transaction, accept) ==
          ComponentPlacementStatus::Reverted);
    CHECK(product->definitions().findComponent(DefinitionId{2})
              ->find(OccurrenceId{20})->placement() == nestedBefore);
    const auto incarnationAfter = incarnationProbe();
    CHECK(incarnationAfter.value() == incarnationBefore.value() + 1);
  }

  // Direct occurrence placement and shared target identity remain independent.
  {
    auto product = makeProduct();
    const auto before =
        product->rootComponentDefinition()->find(OccurrenceId{11})->placement();
    auto prepared = product->prepareComponentPlacement(requestFor(
        *product, path({11}), DefinitionId{3}, DefinitionId{1}, before,
        {5, -2, 1}));
    CHECK(prepared);
    CHECK((prepared.transaction->afterPlacement.translationMm() ==
           std::array<double, 3>{9, -2, 1}));
  }

  // Root, flags, stale identity/state/placement, numeric bounds and no-change
  // are all typed exact no-ops.
  {
    auto product = makeProduct();
    const auto before =
        product->rootComponentDefinition()->find(OccurrenceId{11})->placement();
    auto rootRequest = requestFor(*product, path({11}), DefinitionId{3},
                                  DefinitionId{1}, before, {1, 0, 0});
    rootRequest.path.reset();
    CHECK(product->prepareComponentPlacement(rootRequest).status ==
          ComponentPlacementStatus::RootCannotMove);
    auto noChange = requestFor(*product, path({11}), DefinitionId{3},
                               DefinitionId{1}, before, {1.0e-12, 0, 0});
    CHECK(product->prepareComponentPlacement(noChange).status ==
          ComponentPlacementStatus::NoChange);
    auto nonFinite = noChange;
    nonFinite.worldDeltaMm =
        {std::numeric_limits<double>::infinity(), 0, 0};
    CHECK(product->prepareComponentPlacement(nonFinite).status ==
          ComponentPlacementStatus::NonFiniteDelta);
    auto tooLarge = noChange;
    tooLarge.worldDeltaMm = {1.0e9 + 1, 0, 0};
    CHECK(product->prepareComponentPlacement(tooLarge).status ==
          ComponentPlacementStatus::ValueOutOfRange);
    auto stalePlacement = noChange;
    stalePlacement.worldDeltaMm = {1, 0, 0};
    stalePlacement.expectedPlacement = placement(999, 0, 0);
    CHECK(product->prepareComponentPlacement(stalePlacement).status ==
          ComponentPlacementStatus::PlacementMismatch);
    auto staleIncarnation = noChange;
    staleIncarnation.productIncarnation = ProductIncarnationId{999999};
    CHECK(product->prepareComponentPlacement(staleIncarnation).status ==
          ComponentPlacementStatus::ProductIncarnationMismatch);
    auto staleState = noChange;
    staleState.worldDeltaMm = {1, 0, 0};
    staleState.expectedOwnerStateId = DefinitionStateId{999};
    CHECK(product->prepareComponentPlacement(staleState).status ==
          ComponentPlacementStatus::OwnerStateMismatch);
    auto wrongTarget = noChange;
    wrongTarget.worldDeltaMm = {1, 0, 0};
    wrongTarget.expectedTargetDefinitionId = DefinitionId{2};
    CHECK(product->prepareComponentPlacement(wrongTarget).status ==
          ComponentPlacementStatus::TargetDefinitionMismatch);
    auto missing = noChange;
    missing.worldDeltaMm = {1, 0, 0};
    missing.path = path({999});
    CHECK(product->prepareComponentPlacement(missing).status ==
          ComponentPlacementStatus::OccurrenceMissing);
  }
  {
    auto grounded = makeProduct({}, {}, OccurrenceFlags{true, false, true});
    const auto before = grounded->rootComponentDefinition()
                            ->find(OccurrenceId{11})->placement();
    CHECK(grounded->prepareComponentPlacement(requestFor(
              *grounded, path({11}), DefinitionId{3}, DefinitionId{1}, before,
              {1, 0, 0})).status == ComponentPlacementStatus::Grounded);
    auto suppressed = makeProduct({}, OccurrenceFlags{true, true, false});
    const auto nested = suppressed->definitions().findComponent(DefinitionId{2})
                            ->find(OccurrenceId{20})->placement();
    CHECK(suppressed->prepareComponentPlacement(requestFor(
              *suppressed, path({10, 20}), DefinitionId{3}, DefinitionId{2},
              nested, {1, 0, 0})).status == ComponentPlacementStatus::Suppressed);
    auto hidden = makeProduct(OccurrenceFlags{false, false, false});
    const auto hiddenNested = hidden->definitions().findComponent(DefinitionId{2})
                                  ->find(OccurrenceId{20})->placement();
    CHECK(hidden->prepareComponentPlacement(requestFor(
              *hidden, path({10, 20}), DefinitionId{3}, DefinitionId{2},
              hiddenNested, {1, 0, 0})).status ==
          ComponentPlacementStatus::EffectivelyHidden);
  }

  // Successor rejection/resource failure restores placement and both state
  // scalars exactly. State exhaustion is detected before mutation.
  {
    auto product = makeProduct();
    const auto before =
        product->rootComponentDefinition()->find(OccurrenceId{11})->placement();
    auto prepared = product->prepareComponentPlacement(requestFor(
        *product, path({11}), DefinitionId{3}, DefinitionId{1}, before,
        {3, 0, 0}));
    CHECK(prepared);
    const auto beforeState =
        ComponentExtractionDependencyTestAccess::componentState(
            *product, DefinitionId{1});
    FixedSuccessor reject(ProductDocumentSuccessorStatus::Rejected);
    CHECK(product->applyComponentPlacement(*prepared.transaction, reject) ==
          ComponentPlacementStatus::SuccessorRejected);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == before);
    CHECK(product->definitionStateId(DefinitionId{1}) ==
          prepared.transaction->beforeOwnerStateId);
    CHECK(ComponentExtractionDependencyTestAccess::componentState(
              *product, DefinitionId{1}) == beforeState);
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyComponentPlacement(*prepared.transaction, accept) ==
          ComponentPlacementStatus::Committed);
    FixedSuccessor resource(ProductDocumentSuccessorStatus::ResourceFailure);
    CHECK(product->revertComponentPlacement(*prepared.transaction, resource) ==
          ComponentPlacementStatus::SuccessorResourceFailure);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == prepared.transaction->afterPlacement);
    CHECK((ComponentExtractionDependencyTestAccess::componentState(
               *product, DefinitionId{1}) ==
           std::pair{prepared.transaction->afterOwnerStateId,
                     prepared.transaction->afterNextStateIdValue}));
  }
  {
    auto product = makeProduct();
    CHECK(ComponentExtractionDependencyTestAccess::setComponentState(
        *product, DefinitionId{1}, DefinitionStateId{7},
        std::numeric_limits<std::uint64_t>::max()));
    const auto before =
        product->rootComponentDefinition()->find(OccurrenceId{11})->placement();
    CHECK(product->prepareComponentPlacement(requestFor(
              *product, path({11}), DefinitionId{3}, DefinitionId{1}, before,
              {1, 0, 0})).status ==
          ComponentPlacementStatus::DefinitionStateExhausted);
  }

  // Legacy construction initializes home=current. A drag changes current
  // only; Adopt and Restore are exact local-transform transactions.
  {
    auto product = makeProduct();
    const auto* beforeOccurrence =
        product->rootComponentDefinition()->find(OccurrenceId{11});
    CHECK(beforeOccurrence);
    const auto original = beforeOccurrence->placement();
    CHECK(beforeOccurrence->homePlacement() == original);
    CHECK(product->prepareComponentHomePlacement(homeRequestFor(
              *product, path({11}), DefinitionId{3}, DefinitionId{1},
              original, original,
              ComponentHomePlacementOperation::AdoptCurrent)).status ==
          ComponentHomePlacementStatus::NoChange);
    CHECK(product->prepareComponentHomePlacement(homeRequestFor(
              *product, path({11}), DefinitionId{3}, DefinitionId{1},
              original, original,
              ComponentHomePlacementOperation::Restore)).status ==
          ComponentHomePlacementStatus::NoChange);
    auto moved = product->prepareComponentPlacement(requestFor(
        *product, path({11}), DefinitionId{3}, DefinitionId{1}, original,
        {5, -2, 1}));
    CHECK(moved);
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyComponentPlacement(*moved.transaction, accept) ==
          ComponentPlacementStatus::Committed);
    const auto* dragged =
        product->rootComponentDefinition()->find(OccurrenceId{11});
    CHECK(dragged->placement() == moved.transaction->afterPlacement);
    CHECK(dragged->homePlacement() == original);

    auto adopt = product->prepareComponentHomePlacement(homeRequestFor(
        *product, path({11}), DefinitionId{3}, DefinitionId{1},
        dragged->placement(), dragged->homePlacement(),
        ComponentHomePlacementOperation::AdoptCurrent));
    CHECK(adopt);
    const auto adoptedPlacement = dragged->placement();
    CHECK(product->applyComponentHomePlacement(*adopt.transaction, accept) ==
          ComponentHomePlacementStatus::Committed);
    CHECK(accept.lastImpact == ProductDocumentSuccessorImpact::TreeOnly);
    const auto* adopted =
        product->rootComponentDefinition()->find(OccurrenceId{11});
    CHECK(adopted->placement() == adoptedPlacement);
    CHECK(adopted->homePlacement() == adoptedPlacement);
    CHECK(product->revertComponentHomePlacement(*adopt.transaction, accept) ==
          ComponentHomePlacementStatus::Reverted);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == adoptedPlacement);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->homePlacement() == original);
  }

  // Restore recovers translation and quaternion, including nested
  // parent-local transforms, and its inverse restores the moved pose.
  {
    auto product = makeProduct();
    const auto* nested = product->definitions()
                             .findComponent(DefinitionId{2})
                             ->find(OccurrenceId{20});
    CHECK(nested);
    const auto home = nested->homePlacement();
    auto moved = product->prepareComponentPlacement(requestFor(
        *product, path({10, 20}), DefinitionId{3}, DefinitionId{2},
        nested->placement(), {0, 3, 0}));
    CHECK(moved);
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyComponentPlacement(*moved.transaction, accept) ==
          ComponentPlacementStatus::Committed);
    const auto current = product->definitions()
                             .findComponent(DefinitionId{2})
                             ->find(OccurrenceId{20})
                             ->placement();
    auto restore = product->prepareComponentHomePlacement(homeRequestFor(
        *product, path({10, 20}), DefinitionId{3}, DefinitionId{2}, current,
        home, ComponentHomePlacementOperation::Restore));
    CHECK(restore);
    CHECK(product->applyComponentHomePlacement(*restore.transaction, accept) ==
          ComponentHomePlacementStatus::Committed);
    CHECK(accept.lastImpact == ProductDocumentSuccessorImpact::FullProjection);
    const auto* restored = product->definitions()
                               .findComponent(DefinitionId{2})
                               ->find(OccurrenceId{20});
    CHECK(restored->placement() == home);
    CHECK(restored->homePlacement() == home);
    CHECK(product->revertComponentHomePlacement(*restore.transaction,
                                                accept) ==
          ComponentHomePlacementStatus::Reverted);
    CHECK(product->definitions()
              .findComponent(DefinitionId{2})
              ->find(OccurrenceId{20})
              ->placement() == current);
  }

  // Adopt is allowed for grounded occurrences. Restore retains the movement
  // guards and successor rejection rolls both poses and state back exactly.
  {
    const auto current = placement(9, 0, 0);
    const auto groundedHome = placement(4, 0, 0);
    ComponentDefinition leaf(DefinitionId{2}, std::make_unique<Document>());
    ComponentDefinition root(DefinitionId{1}, std::make_unique<Document>());
    CHECK(root.insert(ComponentOccurrence(
              OccurrenceId{99}, DefinitionId{2}, current, groundedHome,
              OccurrenceFlags{true, false, true})) ==
          ComponentOccurrenceInsertStatus::Inserted);
    std::vector<ComponentDefinition> groundedComponents;
    groundedComponents.push_back(std::move(root));
    groundedComponents.push_back(std::move(leaf));
    auto groundedCreated = ProductDocument::createComponentGraph(
        std::move(groundedComponents), DefinitionId{1});
    CHECK(groundedCreated);
    auto& grounded = *groundedCreated.product;
    CHECK(grounded.prepareComponentHomePlacement(homeRequestFor(
              grounded, path({99}), DefinitionId{2}, DefinitionId{1}, current,
              groundedHome,
              ComponentHomePlacementOperation::AdoptCurrent)));
    CHECK(grounded.prepareComponentHomePlacement(homeRequestFor(
              grounded, path({99}), DefinitionId{2}, DefinitionId{1}, current,
              groundedHome, ComponentHomePlacementOperation::Restore)).status ==
          ComponentHomePlacementStatus::Grounded);

    const auto guardedProduct = [&](OccurrenceFlags flags) {
      ComponentDefinition guardedLeaf(DefinitionId{2},
                                      std::make_unique<Document>());
      ComponentDefinition guardedRoot(DefinitionId{1},
                                      std::make_unique<Document>());
      CHECK(guardedRoot.insert(ComponentOccurrence(
                OccurrenceId{99}, DefinitionId{2}, current, groundedHome,
                flags)) == ComponentOccurrenceInsertStatus::Inserted);
      std::vector<ComponentDefinition> components;
      components.push_back(std::move(guardedRoot));
      components.push_back(std::move(guardedLeaf));
      auto created = ProductDocument::createComponentGraph(
          std::move(components), DefinitionId{1});
      CHECK(created);
      return std::move(created.product);
    };
    auto suppressed = guardedProduct(OccurrenceFlags{true, true, false});
    CHECK(suppressed->prepareComponentHomePlacement(homeRequestFor(
              *suppressed, path({99}), DefinitionId{2}, DefinitionId{1},
              current, groundedHome,
              ComponentHomePlacementOperation::Restore)).status ==
          ComponentHomePlacementStatus::Suppressed);
    auto hidden = guardedProduct(OccurrenceFlags{false, false, false});
    CHECK(hidden->prepareComponentHomePlacement(homeRequestFor(
              *hidden, path({99}), DefinitionId{2}, DefinitionId{1}, current,
              groundedHome, ComponentHomePlacementOperation::Restore)).status ==
          ComponentHomePlacementStatus::EffectivelyHidden);

    auto product = makeProduct();
    const auto before = product->rootComponentDefinition()
                            ->find(OccurrenceId{11})
                            ->placement();
    auto move = product->prepareComponentPlacement(requestFor(
        *product, path({11}), DefinitionId{3}, DefinitionId{1}, before,
        {2, 0, 0}));
    CHECK(move);
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyComponentPlacement(*move.transaction, accept) ==
          ComponentPlacementStatus::Committed);
    const auto movedCurrent = product->rootComponentDefinition()
                                  ->find(OccurrenceId{11})
                                  ->placement();
    auto restore = product->prepareComponentHomePlacement(homeRequestFor(
        *product, path({11}), DefinitionId{3}, DefinitionId{1}, movedCurrent,
        before, ComponentHomePlacementOperation::Restore));
    CHECK(restore);
    auto stalePlacementRequest = restore.transaction->request;
    stalePlacementRequest.expectedPlacement = before;
    CHECK(product->prepareComponentHomePlacement(stalePlacementRequest).status ==
          ComponentHomePlacementStatus::PlacementMismatch);
    auto staleHomeRequest = restore.transaction->request;
    staleHomeRequest.expectedHomePlacement = movedCurrent;
    CHECK(product->prepareComponentHomePlacement(staleHomeRequest).status ==
          ComponentHomePlacementStatus::HomePlacementMismatch);
    auto staleStateRequest = restore.transaction->request;
    staleStateRequest.expectedOwnerStateId = DefinitionStateId{999};
    CHECK(product->prepareComponentHomePlacement(staleStateRequest).status ==
          ComponentHomePlacementStatus::OwnerStateMismatch);
    const auto stateBefore =
        ComponentExtractionDependencyTestAccess::componentState(
            *product, DefinitionId{1});
    FixedSuccessor reject(ProductDocumentSuccessorStatus::Rejected);
    CHECK(product->applyComponentHomePlacement(*restore.transaction, reject) ==
          ComponentHomePlacementStatus::SuccessorRejected);
    const auto* unchanged =
        product->rootComponentDefinition()->find(OccurrenceId{11});
    CHECK(unchanged->placement() == movedCurrent);
    CHECK(unchanged->homePlacement() == before);
    CHECK(ComponentExtractionDependencyTestAccess::componentState(
              *product, DefinitionId{1}) == stateBefore);

    FixedSuccessor resource(ProductDocumentSuccessorStatus::ResourceFailure);
    CHECK(product->applyComponentHomePlacement(*restore.transaction, resource) ==
          ComponentHomePlacementStatus::SuccessorResourceFailure);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == movedCurrent);
    CHECK(ComponentExtractionDependencyTestAccess::componentState(
              *product, DefinitionId{1}) == stateBefore);

    ReentrantHomeSuccessor reentrant(restore.transaction->request);
    CHECK(product->applyComponentHomePlacement(*restore.transaction,
                                               reentrant) ==
          ComponentHomePlacementStatus::SuccessorRejected);
    CHECK(reentrant.reentrantStatus == ComponentHomePlacementStatus::Busy);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == movedCurrent);
    CHECK(ComponentExtractionDependencyTestAccess::componentState(
              *product, DefinitionId{1}) == stateBefore);
  }

  // Restore-all covers direct and nested occurrences in one atomic successor
  // publication. Rejection and undo restore every local transform exactly.
  {
    auto product = makeProduct();
    FixedSuccessor accept(ProductDocumentSuccessorStatus::Prepared);
    const auto directHome = product->rootComponentDefinition()
                                ->find(OccurrenceId{11})
                                ->homePlacement();
    auto directMove = product->prepareComponentPlacement(requestFor(
        *product, path({11}), DefinitionId{3}, DefinitionId{1}, directHome,
        {7, 0, 0}));
    CHECK(directMove);
    CHECK(product->applyComponentPlacement(*directMove.transaction, accept) ==
          ComponentPlacementStatus::Committed);

    const auto nestedHome = product->definitions()
                                .findComponent(DefinitionId{2})
                                ->find(OccurrenceId{20})
                                ->homePlacement();
    auto nestedMove = product->prepareComponentPlacement(requestFor(
        *product, path({10, 20}), DefinitionId{3}, DefinitionId{2},
        nestedHome, {0, 6, 0}));
    CHECK(nestedMove);
    CHECK(product->applyComponentPlacement(*nestedMove.transaction, accept) ==
          ComponentPlacementStatus::Committed);
    const auto directMoved = product->rootComponentDefinition()
                                 ->find(OccurrenceId{11})
                                 ->placement();
    const auto nestedMoved = product->definitions()
                                 .findComponent(DefinitionId{2})
                                 ->find(OccurrenceId{20})
                                 ->placement();

    const auto all = product->prepareAllComponentHomePlacements();
    CHECK(all);
    CHECK(all.transactions.size() == 2);
    FixedSuccessor reject(ProductDocumentSuccessorStatus::Rejected);
    CHECK(product->applyAllComponentHomePlacements(all.transactions, reject) ==
          ComponentHomePlacementStatus::SuccessorRejected);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == directMoved);
    CHECK(product->definitions()
              .findComponent(DefinitionId{2})
              ->find(OccurrenceId{20})
              ->placement() == nestedMoved);

    FixedSuccessor publish(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyAllComponentHomePlacements(all.transactions, publish) ==
          ComponentHomePlacementStatus::Committed);
    CHECK(publish.calls == 1);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == directHome);
    CHECK(product->definitions()
              .findComponent(DefinitionId{2})
              ->find(OccurrenceId{20})
              ->placement() == nestedHome);

    CHECK(product->revertAllComponentHomePlacements(all.transactions, reject) ==
          ComponentHomePlacementStatus::SuccessorRejected);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == directHome);
    CHECK(product->definitions()
              .findComponent(DefinitionId{2})
              ->find(OccurrenceId{20})
              ->placement() == nestedHome);
    CHECK(product->revertAllComponentHomePlacements(all.transactions, publish) ==
          ComponentHomePlacementStatus::Reverted);
    CHECK(product->rootComponentDefinition()->find(OccurrenceId{11})
              ->placement() == directMoved);
    CHECK(product->definitions()
              .findComponent(DefinitionId{2})
              ->find(OccurrenceId{20})
              ->placement() == nestedMoved);
  }

  return 0;
}
