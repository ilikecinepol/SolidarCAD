#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/ComponentScenePublicationCoordinator.h"
#include "ui/ComponentViewportFit.h"
#include "ui/ComponentViewportScene.h"

namespace solidar {

class ComponentViewportSceneAdapterTestAccess final {
 public:
  static void failNthNewMesh(ComponentViewportSceneAdapter& adapter,
                             std::size_t countdown) noexcept {
    adapter.failMeshBuildCountdown_ = countdown;
  }
};

}  // namespace solidar
namespace {

using namespace solidar;

constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};
constexpr BodyId kLocalBody{7};
constexpr FeatureId kLocalFeature{70};

bool near(double left, double right, double tolerance = 2e-7) {
  return std::abs(left - right) <= tolerance;
}

bool samePoint(const Point3d& left, const Point3d& right,
               double tolerance = 2e-7) {
  return near(left.x, right.x, tolerance) &&
         near(left.y, right.y, tolerance) &&
         near(left.z, right.z, tolerance);
}

std::shared_ptr<const TopoDS_Shape> box(double x, double y, double z) {
  TopoDS_Shape shape = BRepPrimAPI_MakeBox(x, y, z).Shape();
  BRepTools::Clean(shape);
  return std::make_shared<const TopoDS_Shape>(std::move(shape));
}

std::unique_ptr<Document> bodyDocument(bool visible = true) {
  auto document = std::make_unique<Document>();
  Body& body = document->addBody(kLocalBody, "body");
  body.setVisible(visible);
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      kLocalFeature, box(2.0, 3.0, 4.0), "feature"));
  CHECK(document->recompute());
  return document;
}

RigidTransform3d translation(double x, double y, double z) {
  const auto created =
      RigidTransform3d::create({x, y, z}, {1.0, 0.0, 0.0, 0.0});
  CHECK(created);
  return *created.transform;
}

RigidTransform3d z90(double x, double y, double z) {
  const double halfSqrt = std::sqrt(0.5);
  const auto created = RigidTransform3d::create(
      {x, y, z}, {halfSqrt, 0.0, 0.0, halfSqrt});
  CHECK(created);
  return *created.transform;
}

std::optional<OccurrencePath> path(
    std::initializer_list<std::uint64_t> ids) {
  std::vector<OccurrenceId> entries;
  entries.reserve(ids.size());
  for (const auto id : ids) entries.emplace_back(id);
  auto created = OccurrencePath::create(std::move(entries));
  CHECK(created);
  return std::move(created.path);
}

struct GraphFixture final {
  std::unique_ptr<ProductDocument> product;
  RigidTransform3d firstMiddlePlacement;
};

GraphFixture componentGraph() {
  constexpr DefinitionId rootId{300};
  constexpr DefinitionId middleId{200};
  constexpr DefinitionId leafId{100};
  ComponentDefinition root(rootId, bodyDocument());
  ComponentDefinition middle(middleId, bodyDocument());
  ComponentDefinition leaf(leafId, bodyDocument());
  CHECK(middle.insert(ComponentOccurrence(
            OccurrenceId{20}, leafId, translation(2.0, 0.0, 0.0),
            {.visible = true, .suppressed = false, .grounded = true})) ==
        ComponentOccurrenceInsertStatus::Inserted);
  const RigidTransform3d first = z90(10.0, 0.0, 0.0);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, middleId, first)) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{11}, middleId, translation(30.0, 0.0, 0.0))) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{12}, middleId, translation(100.0, 0.0, 0.0),
            {.visible = false, .suppressed = true, .grounded = false})) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(middle));
  definitions.push_back(std::move(root));
  definitions.push_back(std::move(leaf));
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), rootId);
  CHECK(created);
  return {std::move(created.product), first};
}

std::unique_ptr<ProductDocument> emptyComponent() {
  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{900}, std::make_unique<Document>());
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{900});
  CHECK(created);
  return std::move(created.product);
}

void replaceRootPlacement(ProductDocument& product, OccurrenceId id,
                          RigidTransform3d placement) {
  // U4b1 deliberately introduces no component editing command/API. Mutate the
  // fixture through ComponentDefinition's existing public erase/insert pair
  // so this test can isolate the renderer contract for placement-only edits.
  auto* root = const_cast<ComponentDefinition*>(
      product.rootComponentDefinition());
  CHECK(root);
  const auto* existing = root->find(id);
  CHECK(existing);
  const DefinitionId target = existing->targetDefinitionId();
  const OccurrenceFlags flags = existing->flags();
  const std::string name = existing->displayNameUtf8();
  CHECK(root->erase(id) == ComponentOccurrenceEraseStatus::Erased);
  CHECK(root->insert(ComponentOccurrence(id, target, std::move(placement),
                                         flags, name)) ==
        ComponentOccurrenceInsertStatus::Inserted);
}

const ComponentViewportSceneInstance& findInstance(
    const ComponentViewportScene& scene, DefinitionId definition,
    const std::optional<OccurrencePath>& occurrencePath) {
  for (const auto& instance : scene.instances()) {
    if (instance.bodyIdentity.component.definitionId == definition &&
        instance.bodyIdentity.component.occurrencePath == occurrencePath)
      return instance;
  }
  CHECK(false);
  std::abort();
}

void verifyHierarchyIdentityBoundsAndFit() {
  auto fixture = componentGraph();
  ComponentScenePublicationCoordinator coordinator;
  CHECK(coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{41}, kNormal));
  ComponentViewportSceneAdapter adapter;
  CHECK(adapter.publish(coordinator.active()));
  const auto scene = adapter.active();
  CHECK(scene);
  CHECK(scene->sources().size() == 3);
  CHECK(scene->instances().size() == 5);
  CHECK(scene->worldBounds());
  CHECK(samePoint(scene->worldBounds()->minimum(), {0.0, 0.0, 0.0}));
  CHECK(samePoint(scene->worldBounds()->maximum(), {34.0, 4.0, 4.0}));

  const auto& root = findInstance(*scene, DefinitionId{300}, std::nullopt);
  const auto& firstMiddle =
      findInstance(*scene, DefinitionId{200}, path({10}));
  const auto& firstLeaf =
      findInstance(*scene, DefinitionId{100}, path({10, 20}));
  const auto& secondMiddle =
      findInstance(*scene, DefinitionId{200}, path({11}));
  const auto& secondLeaf =
      findInstance(*scene, DefinitionId{100}, path({11, 20}));
  CHECK(root.bodyIdentity.component.rootInstance());
  CHECK(!firstMiddle.bodyIdentity.component.rootInstance());
  CHECK(firstLeaf.bodyIdentity.component.occurrencePath->depth() == 2);
  CHECK(firstLeaf.grounded);
  CHECK(firstMiddle.localToWorld == fixture.firstMiddlePlacement);
  const auto composed = RigidTransform3d::compose(
      fixture.firstMiddlePlacement, translation(2.0, 0.0, 0.0));
  CHECK(composed);
  CHECK(firstLeaf.localToWorld == *composed.transform);
  CHECK(samePoint(firstLeaf.worldBounds.minimum(), {7.0, 2.0, 0.0}));
  CHECK(samePoint(firstLeaf.worldBounds.maximum(), {10.0, 4.0, 4.0}));

  CHECK(firstMiddle.sourceKey == secondMiddle.sourceKey);
  CHECK(firstMiddle.mesh == secondMiddle.mesh);
  CHECK(firstLeaf.sourceKey == secondLeaf.sourceKey);
  CHECK(firstLeaf.mesh == secondLeaf.mesh);
  CHECK(scene->sources()[0].key.bodyId == kLocalBody);
  CHECK(scene->sources()[1].key.bodyId == kLocalBody);
  CHECK(scene->sources()[0].key.featureId == kLocalFeature);
  CHECK(scene->sources()[1].key.featureId == kLocalFeature);
  CHECK(scene->sources()[0].key.definitionId !=
        scene->sources()[1].key.definitionId);
  for (const auto& source : scene->sources()) {
    CHECK(source.key.productEpoch == 41);
    CHECK(source.key.shapeRevision != 0);
    CHECK(source.key.shapeIdentity == source.shape.get());
  }

  const auto fitted = fitComponentViewport(
      scene, QSize{960, 640}, -35.0F, 25.0F);
  CHECK(fitted);
  CHECK(fitted.fit->generation == scene->generation());
  CHECK(fitted.fit->fullyContained);
  CHECK(samePoint(fitted.fit->center, scene->worldBounds()->center()));
}

void verifyPlacementReuseFailureAtomicityAndEmptyCommit() {
  auto fixture = componentGraph();
  ComponentScenePublicationCoordinator coordinator;
  ComponentViewportSceneAdapter adapter;
  CHECK(coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{51}, kNormal));
  CHECK(adapter.publish(coordinator.active()));
  const auto beforePlacement = adapter.active();
  const auto attemptsBefore = adapter.diagnostics().meshBuildAttempts;
  std::vector<std::shared_ptr<const BodyRenderMesh>> meshes;
  for (const auto& source : beforePlacement->sources())
    meshes.push_back(source.mesh);

  replaceRootPlacement(*fixture.product, OccurrenceId{11},
                       translation(50.0, 6.0, 0.0));
  CHECK(coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{51}, kNormal));
  CHECK(adapter.publish(coordinator.active()));
  CHECK(adapter.diagnostics().meshBuildAttempts == attemptsBefore);
  CHECK(adapter.active()->worldBounds());
  CHECK(samePoint(adapter.active()->worldBounds()->maximum(),
                  {54.0, 9.0, 4.0}));
  for (std::size_t index = 0; index < meshes.size(); ++index)
    CHECK(adapter.active()->sources()[index].mesh == meshes[index]);

  const auto incumbent = adapter.active();
  const auto resident = adapter.diagnostics().residentSources;
  CHECK(coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{52}, kNormal));
  ComponentViewportSceneAdapterTestAccess::failNthNewMesh(adapter, 2);
  const auto failed = adapter.publish(coordinator.active());
  CHECK(!failed);
  CHECK(failed.status == ComponentViewportSceneStatus::MeshBuildFailed);
  CHECK(adapter.active() == incumbent);
  CHECK(adapter.diagnostics().residentSources == resident);
  CHECK(adapter.publish(coordinator.active()));
  CHECK(adapter.active()->sources().size() == 3);
  CHECK(adapter.active()->sources().front().key.productEpoch == 52);

  const auto nowStale = coordinator.active();
  CHECK(coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{53}, kNormal));
  const auto activeBeforeStale = adapter.active();
  const auto stale = adapter.publish(nowStale);
  CHECK(stale.status == ComponentViewportSceneStatus::StalePublication);
  CHECK(adapter.active() == activeBeforeStale);
  CHECK(fitComponentViewport(
            activeBeforeStale, QSize{800, 600}, 0.0F, 0.0F)
            .status == ComponentViewportFitStatus::StalePublication);

  auto empty = emptyComponent();
  ComponentScenePublicationCoordinator emptyCoordinator;
  CHECK(emptyCoordinator.rebuildAndPublish(
      *empty, ProductPresentationEpoch{60}, kNormal));
  CHECK(adapter.publish(emptyCoordinator.active()));
  CHECK(adapter.active());
  CHECK(adapter.active()->sources().empty());
  CHECK(adapter.active()->instances().empty());
  CHECK(!adapter.active()->worldBounds());
  CHECK(adapter.diagnostics().residentSources == 0);
  const auto emptyFit = fitComponentViewport(
      adapter.active(), QSize{800, 600}, 0.0F, 0.0F);
  CHECK(emptyFit.status == ComponentViewportFitStatus::EmptyBounds);
}

}  // namespace

int main() {
  verifyHierarchyIdentityBoundsAndFit();
  verifyPlacementReuseFailureAtomicityAndEmptyCommit();
  return EXIT_SUCCESS;
}
