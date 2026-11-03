#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/ComponentScenePublicationCoordinator.h"

namespace solidar {

class ComponentScenePublicationCoordinatorTestAccess final {
 public:
  [[nodiscard]] static bool seedGeneration(
      ComponentScenePublicationCoordinator& coordinator,
      std::uint64_t value) noexcept {
    return coordinator.seedGenerationForTesting(value);
  }
};

}  // namespace solidar
namespace {

using namespace solidar;

constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

std::unique_ptr<Document> bodyDocument(double extent) {
  auto document = std::make_unique<Document>();
  Body& body = document->addBody(BodyId{20}, "body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{30},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(extent, extent + 1.0, extent + 2.0).Shape()),
      "imported"));
  CHECK(document->recompute());
  return document;
}

std::unique_ptr<ProductDocument> componentProduct(double extent = 10.0) {
  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{10}, bodyDocument(extent));
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{10});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<ProductDocument> partProduct() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{90}, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(definitions),
                                         DefinitionId{90});
  CHECK(created);
  return std::move(created.product);
}

ComponentScenePublicationResult publish(
    ComponentScenePublicationCoordinator& coordinator,
    const ProductDocument& product, std::uint64_t epoch,
    const ComponentScenePublicationCoordinator::Consumer& consumer = {}) {
  return coordinator.rebuildAndPublish(
      product, ProductPresentationEpoch{epoch}, kNormal, consumer);
}

void verifyExactIdentityAndStaleSemantics() {
  auto product = componentProduct();
  ComponentScenePublicationCoordinator first;
  ComponentScenePublicationCoordinator second;
  CHECK(publish(first, *product, 101));
  CHECK(publish(second, *product, 102));
  const auto firstOne = first.active();
  const auto secondOne = second.active();
  CHECK(firstOne.generation().value() == 1);
  CHECK(secondOne.generation().value() == 1);
  CHECK(firstOne.isCurrent());
  CHECK(secondOne.isCurrent());
  CHECK(firstOne.isSameCurrentPublication(first.active()));
  CHECK(!firstOne.isSameCurrentPublication(secondOne));

  CHECK(publish(first, *product, 103));
  CHECK(!firstOne.isCurrent());
  CHECK(!firstOne.snapshotIfCurrent());
  CHECK(secondOne.isCurrent());
  const auto firstTwo = first.active();
  CHECK(firstTwo.generation().value() == 2);
  CHECK(first.clear() == ComponentSceneClearStatus::Cleared);
  CHECK(!firstTwo.isCurrent());
  CHECK(first.clear() == ComponentSceneClearStatus::AlreadyClear);
}

void verifyReentryAndFailureAtomicity() {
  auto product = componentProduct();
  ComponentScenePublicationCoordinator coordinator;
  ComponentScenePublicationResult nested;
  const auto outer = publish(
      coordinator, *product, 201,
      [&](ComponentScenePublicationHandle handle) {
        CHECK(handle.isCurrent());
        nested = publish(coordinator, *product, 202);
      });
  CHECK(outer);
  CHECK(outer.notification == ComponentSceneNotificationStatus::Delivered);
  CHECK(!outer.stillCurrent);
  CHECK(nested && nested.stillCurrent);
  CHECK(coordinator.active().snapshotIfCurrent()->epoch() ==
        ProductPresentationEpoch{202});

  const auto incumbent = coordinator.active();
  const auto incumbentSnapshot = incumbent.snapshotIfCurrent();
  auto wrongRoot = partProduct();
  const auto rejected = publish(coordinator, *wrongRoot, 203);
  CHECK(!rejected);
  CHECK(rejected.status == ComponentScenePublicationStatus::BuildFailed);
  CHECK(rejected.builderStatus ==
        ComponentPresentationBuildStatus::RootIsNotComponent);
  CHECK(coordinator.active().generation() == incumbent.generation());
  CHECK(coordinator.active().snapshotIfCurrent().get() ==
        incumbentSnapshot.get());

  const auto clearedByConsumer = publish(
      coordinator, *product, 204,
      [&](ComponentScenePublicationHandle handle) {
        CHECK(handle.isCurrent());
        CHECK(coordinator.clear() == ComponentSceneClearStatus::Cleared);
      });
  CHECK(clearedByConsumer);
  CHECK(!clearedByConsumer.stillCurrent);
  CHECK(!coordinator.active().isCurrent());
}

void verifyWrongThreadAndExhaustion() {
  auto product = componentProduct();
  ComponentScenePublicationCoordinator coordinator;
  CHECK(publish(coordinator, *product, 301));
  const auto incumbent = coordinator.active();
  const auto incumbentSnapshot = incumbent.snapshotIfCurrent();
  ComponentScenePublicationResult wrongPublish;
  ComponentSceneClearStatus wrongClear = ComponentSceneClearStatus::Cleared;
  ComponentScenePublicationHandle wrongActive;
  std::thread foreign([&] {
    wrongPublish = publish(coordinator, *product, 302);
    wrongClear = coordinator.clear();
    wrongActive = coordinator.active();
    CHECK(!incumbent.isCurrent());
    CHECK(!incumbent.snapshotIfCurrent());
  });
  foreign.join();
  CHECK(wrongPublish.status == ComponentScenePublicationStatus::WrongThread);
  CHECK(wrongClear == ComponentSceneClearStatus::WrongThread);
  CHECK(!wrongActive.generation().isValid());
  CHECK(incumbent.isCurrent());
  CHECK(coordinator.active().snapshotIfCurrent().get() ==
        incumbentSnapshot.get());

  ComponentScenePublicationCoordinator exhausted;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  CHECK(ComponentScenePublicationCoordinatorTestAccess::seedGeneration(
      exhausted, maximum - 1));
  CHECK(publish(exhausted, *product, 303));
  const auto maximumSnapshot = exhausted.active().snapshotIfCurrent();
  CHECK(exhausted.active().generation().value() == maximum);
  const auto failure = publish(exhausted, *product, 304);
  CHECK(failure.status ==
        ComponentScenePublicationStatus::GenerationExhausted);
  CHECK(exhausted.active().snapshotIfCurrent().get() ==
        maximumSnapshot.get());
}

}  // namespace

int main() {
  verifyExactIdentityAndStaleSemantics();
  verifyReentryAndFailureAtomicity();
  verifyWrongThreadAndExhaustion();
  return EXIT_SUCCESS;
}
