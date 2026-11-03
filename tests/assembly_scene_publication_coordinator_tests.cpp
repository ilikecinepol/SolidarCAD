#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/AssemblyScenePublicationCoordinator.h"

namespace solidar {

class AssemblyScenePublicationCoordinatorTestAccess final {
 public:
  [[nodiscard]] static bool seedGeneration(
      AssemblyScenePublicationCoordinator& coordinator,
      std::uint64_t value) noexcept {
    return coordinator.seedGenerationForTesting(value);
  }
};

}  // namespace solidar
namespace {

using namespace solidar;

constexpr DefinitionId kPartDefinition{10};
constexpr DefinitionId kAssemblyDefinition{100};
constexpr OccurrenceId kOccurrence{1001};
constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

struct ProductFixture {
  std::shared_ptr<const TopoDS_Shape> sourceShape;
  std::unique_ptr<ProductDocument> product;
};

ProductFixture makeAssembly(double extent = 10.0) {
  auto shape = std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(extent, extent + 1.0, extent + 2.0).Shape());
  auto document = std::make_unique<Document>();
  Body& body = document->addBody(20, "body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      30, shape, "imported"));
  CHECK(document->recompute());

  DefinitionRegistry registry;
  CHECK(registry.insert(
            PartDefinition(kPartDefinition, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(kAssemblyDefinition);
  CHECK(root.insert(ComponentOccurrence(kOccurrence, kPartDefinition,
                                        RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(registry.insert(std::move(root)) == DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(registry), kAssemblyDefinition);
  CHECK(created);
  return {std::move(shape), std::move(created.product)};
}

std::unique_ptr<ProductDocument> makePartRoot() {
  DefinitionRegistry registry;
  CHECK(registry.insert(
            PartDefinition(kPartDefinition, std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(std::move(registry), kPartDefinition);
  CHECK(created);
  return std::move(created.product);
}

AssemblyScenePublicationResult publish(
    AssemblyScenePublicationCoordinator& coordinator,
    const ProductDocument& product, std::uint64_t epoch,
    const AssemblyScenePublicationCoordinator::Consumer& consumer = {}) {
  return coordinator.rebuildAndPublish(
      product, ProductPresentationEpoch{epoch}, kNormal, consumer);
}

void verifyInitialAndSuccessfulPublication() {
  AssemblyScenePublicationCoordinator coordinator;
  CHECK(!coordinator.active().generation().isValid());
  CHECK(!coordinator.active().isCurrent());
  CHECK(!coordinator.active().snapshotIfCurrent());
  CHECK(coordinator.clear() == AssemblySceneClearStatus::AlreadyClear);

  auto fixture = makeAssembly();
  AssemblyScenePublicationHandle delivered;
  std::shared_ptr<const AssemblyPresentationSnapshot> seenSnapshot;
  const auto published = publish(
      coordinator, *fixture.product, 101,
      [&](AssemblyScenePublicationHandle handle) {
        // Commit is complete before notification begins.
        CHECK(handle.isCurrent());
        delivered = handle;
        seenSnapshot = handle.snapshotIfCurrent();
        CHECK(seenSnapshot);
        CHECK(seenSnapshot->epoch() == ProductPresentationEpoch{101});
        CHECK(coordinator.active().generation() == handle.generation());
        CHECK(coordinator.active().snapshotIfCurrent().get() ==
              seenSnapshot.get());
      });
  CHECK(published);
  CHECK(published.status == AssemblyScenePublicationStatus::Published);
  CHECK(published.notification ==
        AssemblySceneNotificationStatus::Delivered);
  CHECK(published.builderStatus ==
        AssemblyPresentationBuildStatus::Success);
  CHECK(published.publishedGeneration.isValid());
  CHECK(published.publishedGeneration.value() == 1);
  CHECK(published.stillCurrent);
  CHECK(delivered.isCurrent());
  CHECK(delivered.snapshotIfCurrent().get() == seenSnapshot.get());
}

void verifyBuildFailurePreservesExactPublication() {
  AssemblyScenePublicationCoordinator coordinator;
  auto fixture = makeAssembly();
  CHECK(publish(coordinator, *fixture.product, 201));
  const auto beforeHandle = coordinator.active();
  const auto beforeSnapshot = beforeHandle.snapshotIfCurrent();
  const auto beforeGeneration = beforeHandle.generation();
  CHECK(beforeSnapshot);

  auto partRoot = makePartRoot();
  bool notified = false;
  const auto failed = publish(
      coordinator, *partRoot, 202,
      [&](AssemblyScenePublicationHandle) { notified = true; });
  CHECK(!failed);
  CHECK(failed.status == AssemblyScenePublicationStatus::BuildFailed);
  CHECK(failed.notification == AssemblySceneNotificationStatus::NotRun);
  CHECK(failed.builderStatus ==
        AssemblyPresentationBuildStatus::RootIsNotAssembly);
  CHECK(!failed.publishedGeneration.isValid());
  CHECK(!failed.stillCurrent);
  CHECK(!notified);

  const auto afterHandle = coordinator.active();
  CHECK(afterHandle.generation() == beforeGeneration);
  CHECK(afterHandle.snapshotIfCurrent().get() == beforeSnapshot.get());
  CHECK(beforeHandle.isCurrent());
}

void verifyStaleHandlesAcrossPublishClearAndDestruction() {
  auto coordinator = std::make_unique<AssemblyScenePublicationCoordinator>();
  auto first = makeAssembly(3.0);
  auto second = makeAssembly(6.0);

  CHECK(publish(*coordinator, *first.product, 301));
  const auto firstHandle = coordinator->active();
  auto retainedFirstSnapshot = firstHandle.snapshotIfCurrent();
  CHECK(retainedFirstSnapshot);
  CHECK(retainedFirstSnapshot->sources().size() == 1);
  const TopoDS_Shape* firstShapeAddress =
      retainedFirstSnapshot->sources().front().shape().get();
  std::weak_ptr<const TopoDS_Shape> firstShapeLifetime = first.sourceShape;

  CHECK(publish(*coordinator, *second.product, 302));
  CHECK(!firstHandle.isCurrent());
  CHECK(!firstHandle.snapshotIfCurrent());
  CHECK(retainedFirstSnapshot->sources().front().shape().get() ==
        firstShapeAddress);

  // Neither the product nor the coordinator's new publication owns the old
  // source.  A previously acquired immutable snapshot remains readable.
  first.product.reset();
  first.sourceShape.reset();
  CHECK(!firstShapeLifetime.expired());
  CHECK(!retainedFirstSnapshot->sources().front().shape()->IsNull());

  const auto secondHandle = coordinator->active();
  CHECK(coordinator->clear() == AssemblySceneClearStatus::Cleared);
  CHECK(!secondHandle.isCurrent());
  CHECK(!secondHandle.snapshotIfCurrent());
  CHECK(coordinator->clear() == AssemblySceneClearStatus::AlreadyClear);

  CHECK(publish(*coordinator, *second.product, 303));
  const auto destructionHandle = coordinator->active();
  coordinator.reset();
  CHECK(!destructionHandle.isCurrent());
  CHECK(!destructionHandle.snapshotIfCurrent());

  retainedFirstSnapshot.reset();
  CHECK(firstShapeLifetime.expired());
}

void verifyReentrantPublicationWins() {
  AssemblyScenePublicationCoordinator coordinator;
  auto fixture = makeAssembly();
  AssemblyScenePublicationResult nested;
  AssemblyScenePublicationHandle firstHandle;
  AssemblyScenePublicationHandle secondHandle;

  const auto first = publish(
      coordinator, *fixture.product, 401,
      [&](AssemblyScenePublicationHandle handle) {
        firstHandle = handle;
        CHECK(firstHandle.isCurrent());
        nested = publish(coordinator, *fixture.product, 402);
        secondHandle = coordinator.active();
      });
  CHECK(first);
  CHECK(first.notification == AssemblySceneNotificationStatus::Delivered);
  CHECK(!first.stillCurrent);
  CHECK(nested && nested.stillCurrent);
  CHECK(nested.publishedGeneration.value() == 2);
  CHECK(!firstHandle.isCurrent());
  CHECK(!firstHandle.snapshotIfCurrent());
  CHECK(secondHandle.isCurrent());
  CHECK(secondHandle.snapshotIfCurrent()->epoch() ==
        ProductPresentationEpoch{402});
}

void verifyReentrantClearWins() {
  AssemblyScenePublicationCoordinator coordinator;
  auto fixture = makeAssembly();
  AssemblyScenePublicationHandle delivered;
  const auto published = publish(
      coordinator, *fixture.product, 501,
      [&](AssemblyScenePublicationHandle handle) {
        delivered = handle;
        CHECK(handle.isCurrent());
        CHECK(coordinator.clear() == AssemblySceneClearStatus::Cleared);
      });
  CHECK(published);
  CHECK(published.notification ==
        AssemblySceneNotificationStatus::Delivered);
  CHECK(!published.stillCurrent);
  CHECK(!delivered.isCurrent());
  CHECK(!coordinator.active().isCurrent());
}

void verifyConsumerExceptionsDoNotEscapeOrRollback() {
  auto fixture = makeAssembly();

  AssemblyScenePublicationCoordinator standardCoordinator;
  const auto standard = publish(
      standardCoordinator, *fixture.product, 601,
      [](AssemblyScenePublicationHandle) {
        throw std::runtime_error("notification failure");
      });
  CHECK(standard);
  CHECK(standard.notification ==
        AssemblySceneNotificationStatus::StandardException);
  CHECK(standard.stillCurrent);
  CHECK(standardCoordinator.active().isCurrent());

  AssemblyScenePublicationCoordinator unknownCoordinator;
  const auto unknown = publish(
      unknownCoordinator, *fixture.product, 602,
      [](AssemblyScenePublicationHandle) { throw 42; });
  CHECK(unknown);
  CHECK(unknown.notification ==
        AssemblySceneNotificationStatus::UnknownException);
  CHECK(unknown.stillCurrent);
  CHECK(unknownCoordinator.active().isCurrent());

  AssemblyScenePublicationCoordinator reentrantCoordinator;
  AssemblyScenePublicationResult nested;
  const auto reentrant = publish(
      reentrantCoordinator, *fixture.product, 603,
      [&](AssemblyScenePublicationHandle) {
        nested = publish(reentrantCoordinator, *fixture.product, 604);
        throw std::runtime_error("old consumer after new publication");
      });
  CHECK(reentrant);
  CHECK(reentrant.notification ==
        AssemblySceneNotificationStatus::StandardException);
  CHECK(!reentrant.stillCurrent);
  CHECK(nested && nested.stillCurrent);
  CHECK(reentrantCoordinator.active().snapshotIfCurrent()->epoch() ==
        ProductPresentationEpoch{604});
}

void verifyDestructionInsideConsumerIsSafe() {
  auto fixture = makeAssembly();
  auto coordinator = std::make_unique<AssemblyScenePublicationCoordinator>();
  AssemblyScenePublicationHandle delivered;
  const auto published = publish(
      *coordinator, *fixture.product, 701,
      [&](AssemblyScenePublicationHandle handle) {
        delivered = handle;
        CHECK(handle.isCurrent());
        coordinator.reset();
      });
  CHECK(!coordinator);
  CHECK(published);
  CHECK(published.notification ==
        AssemblySceneNotificationStatus::Delivered);
  CHECK(!published.stillCurrent);
  CHECK(!delivered.isCurrent());
  CHECK(!delivered.snapshotIfCurrent());
}

void verifyCoordinatorIdentityIsolation() {
  auto fixture = makeAssembly();
  AssemblyScenePublicationCoordinator first;
  AssemblyScenePublicationCoordinator second;
  CHECK(publish(first, *fixture.product, 801));
  CHECK(publish(second, *fixture.product, 802));
  const auto firstHandle = first.active();
  const auto secondHandle = second.active();
  CHECK(firstHandle.generation().value() == 1);
  CHECK(secondHandle.generation().value() == 1);
  CHECK(firstHandle.isCurrent());
  CHECK(secondHandle.isCurrent());
  CHECK(firstHandle.isSameCurrentPublication(first.active()));
  CHECK(secondHandle.isSameCurrentPublication(second.active()));
  CHECK(!firstHandle.isSameCurrentPublication(secondHandle));
  CHECK(!secondHandle.isSameCurrentPublication(firstHandle));
  CHECK(first.clear() == AssemblySceneClearStatus::Cleared);
  CHECK(!firstHandle.isCurrent());
  CHECK(!firstHandle.isSameCurrentPublication(firstHandle));
  CHECK(secondHandle.isCurrent());
  CHECK(secondHandle.snapshotIfCurrent()->epoch() ==
        ProductPresentationEpoch{802});
}

void verifyGenerationExhaustionAndClear() {
  auto fixture = makeAssembly();
  AssemblyScenePublicationCoordinator coordinator;
  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  CHECK(AssemblyScenePublicationCoordinatorTestAccess::seedGeneration(
      coordinator, maximum - 1));
  const auto atMaximum = publish(coordinator, *fixture.product, 901);
  CHECK(atMaximum);
  CHECK(atMaximum.publishedGeneration.value() == maximum);
  const auto maximumHandle = coordinator.active();
  const auto maximumSnapshot = maximumHandle.snapshotIfCurrent();
  CHECK(maximumSnapshot);

  bool notified = false;
  const auto exhausted = publish(
      coordinator, *fixture.product, 902,
      [&](AssemblyScenePublicationHandle) { notified = true; });
  CHECK(!exhausted);
  CHECK(exhausted.status ==
        AssemblyScenePublicationStatus::GenerationExhausted);
  CHECK(exhausted.notification == AssemblySceneNotificationStatus::NotRun);
  CHECK(exhausted.builderStatus ==
        AssemblyPresentationBuildStatus::Success);
  CHECK(!exhausted.publishedGeneration.isValid());
  CHECK(!notified);
  CHECK(coordinator.active().generation().value() == maximum);
  CHECK(coordinator.active().snapshotIfCurrent().get() ==
        maximumSnapshot.get());

  // Exhaustion never prevents teardown and clear never reuses generation.
  CHECK(coordinator.clear() == AssemblySceneClearStatus::Cleared);
  CHECK(!maximumHandle.isCurrent());
  CHECK(coordinator.clear() == AssemblySceneClearStatus::AlreadyClear);
  const auto stillExhausted = publish(coordinator, *fixture.product, 903);
  CHECK(stillExhausted.status ==
        AssemblyScenePublicationStatus::GenerationExhausted);
  CHECK(!coordinator.active().isCurrent());
}

void verifyWrongThreadFailsClosed() {
  auto fixture = makeAssembly();
  AssemblyScenePublicationCoordinator coordinator;
  CHECK(publish(coordinator, *fixture.product, 1001));
  const auto before = coordinator.active();
  const auto beforeSnapshot = before.snapshotIfCurrent();
  CHECK(beforeSnapshot);

  AssemblyScenePublicationResult wrongPublish;
  AssemblySceneClearStatus wrongClear = AssemblySceneClearStatus::Cleared;
  AssemblyScenePublicationHandle wrongActive;
  bool notified = false;
  std::thread foreign([&] {
    wrongPublish = publish(
        coordinator, *fixture.product, 1002,
        [&](AssemblyScenePublicationHandle) { notified = true; });
    wrongClear = coordinator.clear();
    wrongActive = coordinator.active();
    CHECK(!before.isCurrent());
    CHECK(!before.snapshotIfCurrent());
  });
  foreign.join();

  CHECK(!wrongPublish);
  CHECK(wrongPublish.status == AssemblyScenePublicationStatus::WrongThread);
  CHECK(!wrongPublish.builderStatus);
  CHECK(wrongPublish.notification ==
        AssemblySceneNotificationStatus::NotRun);
  CHECK(wrongClear == AssemblySceneClearStatus::WrongThread);
  CHECK(!wrongActive.generation().isValid());
  CHECK(!notified);
  CHECK(before.isCurrent());
  CHECK(coordinator.active().generation() == before.generation());
  CHECK(coordinator.active().snapshotIfCurrent().get() ==
        beforeSnapshot.get());
}

}  // namespace

int main() {
  verifyInitialAndSuccessfulPublication();
  verifyBuildFailurePreservesExactPublication();
  verifyStaleHandlesAcrossPublishClearAndDestruction();
  verifyReentrantPublicationWins();
  verifyReentrantClearWins();
  verifyConsumerExceptionsDoNotEscapeOrRollback();
  verifyDestructionInsideConsumerIsSafe();
  verifyCoordinatorIdentityIsolation();
  verifyGenerationExhaustionAndClear();
  verifyWrongThreadFailsClosed();
  return EXIT_SUCCESS;
}
