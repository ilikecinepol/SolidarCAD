#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/ComponentScenePublicationCoordinator.h"
#include "ui/ComponentPlacementDrag.h"
#include "ui/ComponentViewportScene.h"

namespace {
using namespace solidar;

bool near(double left, double right) {
  return std::abs(left - right) <= 1.0e-9;
}

std::unique_ptr<Document> boxDocument(std::uint64_t id, double size) {
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(BodyId{id}, "body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{id}, std::make_shared<const TopoDS_Shape>(
                         BRepPrimAPI_MakeBox(size, size, size).Shape()),
      "box"));
  CHECK(document->recompute());
  return document;
}

RigidTransform3d translated(double x) {
  const auto made = RigidTransform3d::create({x, 0.0, 0.0},
                                             {1.0, 0.0, 0.0, 0.0});
  CHECK(made);
  return *made.transform;
}

std::unique_ptr<ProductDocument> nestedProduct() {
  ComponentDefinition root(DefinitionId{300}, boxDocument(3, 3.0));
  ComponentDefinition middle(DefinitionId{200}, boxDocument(2, 2.0));
  ComponentDefinition leaf(DefinitionId{100}, boxDocument(1, 1.0));
  CHECK(middle.insert(ComponentOccurrence(OccurrenceId{20}, DefinitionId{100},
                                          translated(20.0))) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{10}, DefinitionId{200},
                                        translated(10.0))) ==
        ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{11}, DefinitionId{100},
                                        translated(-10.0))) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(leaf));
  definitions.push_back(std::move(middle));
  definitions.push_back(std::move(root));
  auto made = ProductDocument::createComponentGraph(std::move(definitions),
                                                     DefinitionId{300});
  CHECK(made);
  return std::move(made.product);
}

}  // namespace

int main() {
  using namespace solidar;
  ViewportCameraState camera;
  camera.logicalSize = QSize(1000, 1000);
  camera.yawDeg = 0.0F;
  camera.pitchDeg = 0.0F;
  camera.zoom = 1.0F;

  auto mapped = mapComponentPlacementDrag(camera, {100.0, 100.0},
                                          {180.0, 180.0});
  CHECK(mapped);
  CHECK(near(mapped.worldDeltaMm[0], 10.0));
  CHECK(near(mapped.worldDeltaMm[1], 10.0));
  CHECK(near(mapped.worldDeltaMm[2], 0.0));

  camera.yawDeg = 90.0F;
  camera.pitchDeg = 90.0F;
  mapped = mapComponentPlacementDrag(camera, {0.0, 0.0}, {80.0, 80.0});
  CHECK(mapped);
  CHECK(near(mapped.worldDeltaMm[0], 0.0));
  CHECK(near(mapped.worldDeltaMm[1], -10.0));
  CHECK(near(mapped.worldDeltaMm[2], -10.0));

  CHECK(mapComponentPlacementDrag(camera, {0.0, 0.0}, {2.99, 0.0}).status ==
        ComponentPlacementDragMapStatus::BelowThreshold);
  CHECK(mapComponentPlacementDrag(camera, {0.0, 0.0}, {3.0, 0.0}).status ==
        ComponentPlacementDragMapStatus::Mapped);
  camera.zoom = 0.0F;
  CHECK(mapComponentPlacementDrag(camera, {}, {10.0, 0.0}).status ==
        ComponentPlacementDragMapStatus::InvalidCamera);
  camera.zoom = 1.0F;
  CHECK(mapComponentPlacementDrag(
            camera, {}, {std::numeric_limits<double>::infinity(), 0.0})
            .status == ComponentPlacementDragMapStatus::NonFiniteInput);
  CHECK(mapComponentPlacementDrag(camera, {}, {1.0e14, 0.0}).status ==
        ComponentPlacementDragMapStatus::ValueOutOfRange);

  auto product = nestedProduct();
  ComponentScenePublicationCoordinator coordinator;
  ComponentViewportSceneAdapter adapter;
  constexpr AssemblyMeshProfile profile{AssemblyMeshQuality::Normal};
  CHECK(coordinator.rebuildAndPublish(*product, ProductPresentationEpoch{77},
                                      profile));
  CHECK(adapter.publish(coordinator.active()));
  const auto scene = adapter.active();
  CHECK(scene);

  const ComponentInstanceAddress* middle = nullptr;
  const ComponentInstanceAddress* nestedLeaf = nullptr;
  const ComponentInstanceAddress* siblingLeaf = nullptr;
  for (const auto& instance : scene->instances()) {
    const auto& address = instance.bodyIdentity.component;
    if (!address.occurrencePath) continue;
    if (address.occurrencePath->entries() ==
        std::vector<OccurrenceId>{OccurrenceId{10}})
      middle = &address;
    else if (address.occurrencePath->entries() ==
             std::vector<OccurrenceId>{OccurrenceId{10}, OccurrenceId{20}})
      nestedLeaf = &address;
    else if (address.occurrencePath->entries() ==
             std::vector<OccurrenceId>{OccurrenceId{11}})
      siblingLeaf = &address;
  }
  CHECK(middle && nestedLeaf && siblingLeaf);
  auto preview = ComponentPlacementPreview::create(
      scene->publicationHandle(), *middle, {4.0, 5.0, 6.0}, 9);
  CHECK(preview);
  CHECK(preview->isCurrentFor(*scene, 9));
  CHECK(!preview->isCurrentFor(*scene, 8));
  CHECK(preview->moves(*middle));
  CHECK(preview->moves(*nestedLeaf));
  CHECK(!preview->moves(*siblingLeaf));
  CHECK(!preview->moves(scene->instances().front().bodyIdentity.component));
  const auto request = preview->request();
  CHECK(request.publication.isSameCurrentPublication(
      scene->publicationHandle()));
  CHECK(request.component == *middle);
  const std::array<double, 3> expectedDelta{4.0, 5.0, 6.0};
  CHECK(request.worldDeltaMm == expectedDelta);
  CHECK(request.ownershipRevision == 9);

  CHECK(!ComponentPlacementPreview::create(
      scene->publicationHandle(),
      scene->instances().front().bodyIdentity.component, {1.0, 0.0, 0.0},
      9));
  CHECK(!ComponentPlacementPreview::create(scene->publicationHandle(),
                                            *middle, {1.0, 0.0, 0.0}, 0));

  CHECK(coordinator.rebuildAndPublish(*product, ProductPresentationEpoch{78},
                                      profile));
  CHECK(!preview->isCurrentFor(*scene, 9));
  return EXIT_SUCCESS;
}
