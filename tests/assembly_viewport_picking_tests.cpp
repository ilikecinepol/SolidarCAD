#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <tuple>
#include <utility>
#include <vector>

#include "model/ExtrudeFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "model/TopologyReferenceResolver.h"
#include "presentation/AssemblyScenePublicationCoordinator.h"
#include "ui/AssemblyViewportPicking.h"
#include "ui/AssemblyViewportScene.h"
#include "ui/RigidTransform3dOps.h"

namespace solidar {

class AssemblyViewportPickingTestAccess final {
 public:
  static void failNthTopologyBuild(AssemblyViewportPicking& picking,
                                   std::size_t ordinal) noexcept {
    picking.failTopologyBuildCountdown_ = ordinal;
  }
  static void failNthMapping(AssemblyViewportPicking& picking,
                             std::size_t ordinal) noexcept {
    picking.failMappingCountdown_ = ordinal;
  }
  static void failNthProjectionBuild(AssemblyViewportPicking& picking,
                                     std::size_t ordinal) noexcept {
    picking.failProjectionBuildCountdown_ = ordinal;
  }
};

}  // namespace solidar

namespace {

using namespace solidar;

constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

ShapeFeature::ShapePtr box(double x, double y, double z,
                           Point3d origin = {}) {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(gp_Pnt(origin.x, origin.y, origin.z), x, y, z)
          .Shape());
}

RigidTransform3d placement(double x, double y, double z) {
  const auto made =
      RigidTransform3d::create({x, y, z}, {1.0, 0.0, 0.0, 0.0});
  CHECK(made);
  return *made.transform;
}

RigidTransform3d z90(double x, double y, double z) {
  const double half = std::sqrt(0.5);
  const auto made =
      RigidTransform3d::create({x, y, z}, {half, 0.0, 0.0, half});
  CHECK(made);
  return *made.transform;
}

OccurrencePath path(OccurrenceId id) {
  auto made = OccurrencePath::create({id});
  CHECK(made);
  return std::move(*made.path);
}

OccurrencePath path(std::initializer_list<std::uint64_t> ids) {
  std::vector<OccurrenceId> entries;
  entries.reserve(ids.size());
  for (const auto id : ids) entries.emplace_back(id);
  auto made = OccurrencePath::create(std::move(entries));
  CHECK(made);
  return std::move(*made.path);
}

struct BodyInput final {
  BodyId bodyId{};
  FeatureId featureId{};
  ShapeFeature::ShapePtr shape;
  bool visible{true};
};

struct OccurrenceInput final {
  OccurrenceId id;
  DefinitionId definitionId;
  RigidTransform3d transform{RigidTransform3d::identity()};
  OccurrenceFlags flags;
};

std::unique_ptr<Document> importedDocument(std::vector<BodyInput> inputs) {
  auto document = std::make_unique<Document>();
  for (auto& input : inputs) {
    Body& body = document->addBody(input.bodyId, "body");
    body.setVisible(input.visible);
    body.addFeature(std::make_unique<ImportedShapeFeature>(
        input.featureId, std::move(input.shape), "shape"));
  }
  CHECK(document->recompute());
  return document;
}

std::unique_ptr<ProductDocument> product(
    std::vector<std::pair<DefinitionId, std::unique_ptr<Document>>> parts,
    std::vector<OccurrenceInput> occurrences,
    DefinitionId rootId = DefinitionId{100}) {
  DefinitionRegistry definitions;
  for (auto& [id, document] : parts) {
    CHECK(definitions.insert(PartDefinition(id, std::move(document))) ==
          DefinitionInsertStatus::Inserted);
  }
  AssemblyDefinition root(rootId);
  for (auto& occurrence : occurrences) {
    CHECK(root.insert(ComponentOccurrence(
              occurrence.id, occurrence.definitionId, occurrence.transform,
              occurrence.flags)) == AssemblyOccurrenceInsertStatus::Inserted);
  }
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), rootId);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> nestedProduct() {
  std::vector<PartDefinition> parts;
  parts.emplace_back(
      DefinitionId{10},
      importedDocument({{11, 101, box(10.0, 10.0, 10.0)}}));
  std::vector<AssemblyDefinition> assemblies;
  AssemblyDefinition child(DefinitionId{200});
  CHECK(child.insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{10},
            RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{100});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{1}, DefinitionId{200}, placement(0, 0, 0))) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{2}, DefinitionId{200}, placement(30, 0, 0))) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  assemblies.push_back(std::move(child));
  assemblies.push_back(std::move(root));
  auto made = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), DefinitionId{100});
  CHECK(made);
  return std::move(made.product);
}

using PartInputs =
    std::vector<std::pair<DefinitionId, std::unique_ptr<Document>>>;

PartInputs partInputs(DefinitionId id, std::unique_ptr<Document> document) {
  PartInputs result;
  result.emplace_back(id, std::move(document));
  return result;
}

PartInputs partInputs(DefinitionId firstId,
                     std::unique_ptr<Document> first,
                     DefinitionId secondId,
                     std::unique_ptr<Document> second) {
  PartInputs result;
  result.emplace_back(firstId, std::move(first));
  result.emplace_back(secondId, std::move(second));
  return result;
}

std::shared_ptr<const AssemblyViewportScene> publish(
    ProductDocument& value, ProductPresentationEpoch epoch,
    AssemblyScenePublicationCoordinator& coordinator,
    AssemblyViewportSceneAdapter& adapter,
    AssemblyViewportPicking& picking) {
  CHECK(coordinator.rebuildAndPublish(value, epoch, kNormal));
  CHECK(adapter.publish(coordinator.active()));
  CHECK(picking.publish(adapter.active()));
  return adapter.active();
}

ViewportCameraState cameraFor(const AssemblyViewportScene& scene, float yaw,
                              float pitch, QSize size = QSize{800, 600},
                              float zoom = 1.0F, QPointF pan = {}) {
  CHECK(scene.worldBounds());
  return {yaw, pitch, zoom, pan, size, 1.0F, scene.worldBounds()->center(),
          std::max(1.0, scene.worldBounds()->diagonal() * 3.0)};
}

Point3d centroid(const RenderTriangle& triangle) {
  return {(triangle.a.x + triangle.b.x + triangle.c.x) / 3.0,
          (triangle.a.y + triangle.b.y + triangle.c.y) / 3.0,
          (triangle.a.z + triangle.b.z + triangle.c.z) / 3.0};
}

bool near(double left, double right, double tolerance = 1e-5) {
  return std::abs(left - right) <= tolerance;
}

void verifyOccurrenceScopedReferencesAndBodyOwners() {
  auto shared = product(
      partInputs(DefinitionId{10},
                 importedDocument({{11, 101, box(10, 10, 10)}})),
      {{OccurrenceId{1001}, DefinitionId{10}, placement(0, 0, 0)},
       {OccurrenceId{1002}, DefinitionId{10}, placement(30, 0, 0)}});
  AssemblyScenePublicationCoordinator coordinator;
  AssemblyViewportSceneAdapter adapter;
  AssemblyViewportPicking picking;
  auto scene = publish(*shared, ProductPresentationEpoch{1}, coordinator,
                       adapter, picking);
  const auto camera = cameraFor(*scene, 0.0F, 0.0F);
  const auto first = picking.faceAt(camera, camera.worldToScreen({5, 5, 10}));
  const auto second =
      picking.faceAt(camera, camera.worldToScreen({35, 5, 10}));
  CHECK(first && second);
  CHECK(first.hit->address.face == second.hit->address.face);
  CHECK(first.hit->address.occurrence != second.hit->address.occurrence);
  CHECK(first.hit->address.definitionId == DefinitionId{10});
  CHECK(first.hit->address.face.bodyId() == 11);
  CHECK(first.hit->address.face.featureId() == 101);

  auto twoBodies = product(
      partInputs(DefinitionId{20},
                 importedDocument({{21, 201, box(8, 8, 8)},
                                   {22, 202,
                                    box(8, 8, 8, {0, 20, 0})}})),
      {{OccurrenceId{2001}, DefinitionId{20}, placement(0, 0, 0)}});
  AssemblyScenePublicationCoordinator bodyCoordinator;
  AssemblyViewportSceneAdapter bodyAdapter;
  AssemblyViewportPicking bodyPicking;
  scene = publish(*twoBodies, ProductPresentationEpoch{2}, bodyCoordinator,
                  bodyAdapter, bodyPicking);
  const auto& instances = scene->instances();
  CHECK(instances.size() == 2);
  const RenderTriangle firstRawZero = instances[0].mesh->triangle(0);
  const RenderTriangle secondRawZero = instances[1].mesh->triangle(0);
  CHECK(firstRawZero.faceIndex == 0);
  CHECK(secondRawZero.faceIndex == 0);
  const auto firstWorld = transformPoint(instances[0].localToWorld,
                                         centroid(firstRawZero));
  const auto secondWorld = transformPoint(instances[1].localToWorld,
                                          centroid(secondRawZero));
  CHECK(firstWorld && secondWorld);
  const auto faceZeroCamera = [&](const RenderTriangle& triangle) {
    const double length = std::hypot(
        std::hypot(triangle.normal.x, triangle.normal.y), triangle.normal.z);
    CHECK(length > 0.0);
    const double nx = triangle.normal.x / length;
    const double ny = triangle.normal.y / length;
    const double nz = triangle.normal.z / length;
    const float pitch = static_cast<float>(
        std::acos(std::clamp(nz, -1.0, 1.0)) * 180.0 / std::numbers::pi);
    const float yaw = static_cast<float>(
        std::atan2(nx, ny) * 180.0 / std::numbers::pi);
    return cameraFor(*scene, yaw, pitch);
  };
  const auto firstBodyCamera = faceZeroCamera(firstRawZero);
  const auto secondBodyCamera = faceZeroCamera(secondRawZero);
  const auto firstBody =
      bodyPicking.faceAt(firstBodyCamera,
                         firstBodyCamera.worldToScreen(*firstWorld));
  const auto secondBody =
      bodyPicking.faceAt(secondBodyCamera,
                         secondBodyCamera.worldToScreen(*secondWorld));
  CHECK(firstBody && secondBody);
  CHECK(firstBody.hit->address.face.bodyId() !=
        secondBody.hit->address.face.bodyId());
  CHECK(firstBody.hit->address.face.featureId() !=
        secondBody.hit->address.face.featureId());
  std::array<FaceReference, 2> rawZeroReferences;
  for (std::size_t index = 0; index < instances.size(); ++index) {
    const auto source = std::find_if(
        scene->sources().begin(), scene->sources().end(),
        [&](const AssemblyViewportSceneSource& candidate) {
          return candidate.key == instances[index].sourceKey;
        });
    CHECK(source != scene->sources().end());
    const auto topology = TopologyIndex::build(
        source->shape, source->key.shapeRevision, source->topologyLineage);
    CHECK(topology);
    const auto expected = topology->createFaceReference(
        source->key.bodyId, source->key.featureId, 0);
    CHECK(expected);
    rawZeroReferences[index] = expected.reference;
  }
  CHECK(rawZeroReferences[0].faceIndex() == 0);
  CHECK(rawZeroReferences[1].faceIndex() == 0);
  CHECK(rawZeroReferences[0].bodyId() != rawZeroReferences[1].bodyId());
  CHECK(rawZeroReferences[0].featureId() != rawZeroReferences[1].featureId());
  CHECK(rawZeroReferences[0] != rawZeroReferences[1]);
}

void verifyRecursivePrefixIdentityAcrossQueries() {
  auto nested = nestedProduct();
  AssemblyScenePublicationCoordinator coordinator;
  AssemblyViewportSceneAdapter adapter;
  AssemblyViewportPicking picking;
  const auto scene = publish(*nested, ProductPresentationEpoch{12},
                             coordinator, adapter, picking);
  CHECK(scene->sources().size() == 1);
  CHECK(scene->instances().size() == 2);
  const auto camera = cameraFor(*scene, 0.0F, 0.0F);
  const auto first = picking.faceAt(camera, camera.worldToScreen({5, 5, 10}));
  const auto second =
      picking.faceAt(camera, camera.worldToScreen({35, 5, 10}));
  CHECK(first && second);
  CHECK(first.hit->address.face == second.hit->address.face);
  CHECK(first.hit->address.occurrence.path == path({1, 7}));
  CHECK(second.hit->address.occurrence.path == path({2, 7}));
  CHECK(first.hit->highlightTarget());
  CHECK(first.hit->highlightTarget()->occurrence().path == path({1, 7}));
  CHECK(second.hit->highlightTarget());
  CHECK(second.hit->highlightTarget()->occurrence().path == path({2, 7}));

  const auto rectangle = picking.facesInRectangle(
      camera, QPointF{0.0, 0.0},
      QPointF{static_cast<double>(camera.logicalSize.width()),
              static_cast<double>(camera.logicalSize.height())});
  CHECK(rectangle);
  CHECK(std::any_of(rectangle.hits.begin(), rectangle.hits.end(),
                    [](const auto& hit) {
                      return hit.address.occurrence.path == path({1, 7});
                    }));
  CHECK(std::any_of(rectangle.hits.begin(), rectangle.hits.end(),
                    [](const auto& hit) {
                      return hit.address.occurrence.path == path({2, 7});
                    }));
  const auto snap =
      picking.snapAt(camera, camera.worldToScreen({5, 5, 10}), 20.0);
  CHECK(snap);
  CHECK(snap.hit->occurrence.path == path({1, 7}));
  CHECK(snap.hit->highlightTarget());
  CHECK(snap.hit->highlightTarget()->occurrence().path == path({1, 7}));
}

void verifyFrontmostPlacementAndLexicalTie() {
  auto value = product(
      partInputs(DefinitionId{10},
                 importedDocument({{1, 1, box(10, 10, 10)}})),
      {{OccurrenceId{1001}, DefinitionId{10}, placement(0, 0, 0)},
       {OccurrenceId{1002}, DefinitionId{10}, placement(0, 0, 20)}});
  AssemblyScenePublicationCoordinator coordinator;
  AssemblyViewportSceneAdapter adapter;
  AssemblyViewportPicking picking;
  auto scene = publish(*value, ProductPresentationEpoch{3}, coordinator,
                       adapter, picking);
  auto camera = cameraFor(*scene, 0.0F, 0.0F);
  auto hit = picking.faceAt(camera, camera.worldToScreen({5, 5, 30}));
  CHECK(hit);
  CHECK(hit.hit->address.occurrence.path == path(OccurrenceId{1002}));
  CHECK(near(hit.hit->worldPoint.z, 30.0));

  CHECK(value->setRootOccurrencePlacement(path(OccurrenceId{1001}),
                                          placement(0, 0, 25)) ==
        ProductOccurrenceMutationStatus::Committed);
  CHECK(value->setRootOccurrencePlacement(path(OccurrenceId{1002}),
                                          placement(0, 0, 0)) ==
        ProductOccurrenceMutationStatus::Committed);
  scene = publish(*value, ProductPresentationEpoch{3}, coordinator, adapter,
                  picking);
  camera = cameraFor(*scene, 0.0F, 0.0F);
  hit = picking.faceAt(camera, camera.worldToScreen({5, 5, 35}));
  CHECK(hit);
  CHECK(hit.hit->address.occurrence.path == path(OccurrenceId{1001}));

  CHECK(value->setRootOccurrencePlacement(path(OccurrenceId{1001}),
                                          placement(0, 0, 0)) ==
        ProductOccurrenceMutationStatus::Committed);
  scene = publish(*value, ProductPresentationEpoch{3}, coordinator, adapter,
                  picking);
  camera = cameraFor(*scene, 0.0F, 0.0F);
  hit = picking.faceAt(camera, camera.worldToScreen({5, 5, 10}));
  CHECK(hit);
  CHECK(hit.hit->address.occurrence.path == path(OccurrenceId{1001}));

  auto reversedInput = product(
      partInputs(DefinitionId{10},
                 importedDocument({{1, 1, box(10, 10, 10)}})),
      {{OccurrenceId{1002}, DefinitionId{10}, placement(0, 0, 0)},
       {OccurrenceId{1001}, DefinitionId{10}, placement(0, 0, 0)}});
  AssemblyScenePublicationCoordinator reversedCoordinator;
  AssemblyViewportSceneAdapter reversedAdapter;
  AssemblyViewportPicking reversedPicking;
  const auto reversedScene = publish(
      *reversedInput, ProductPresentationEpoch{31}, reversedCoordinator,
      reversedAdapter, reversedPicking);
  const auto reversedCamera = cameraFor(*reversedScene, 0.0F, 0.0F);
  const auto reversedHit = reversedPicking.faceAt(
      reversedCamera, reversedCamera.worldToScreen({5, 5, 10}));
  CHECK(reversedHit);
  CHECK(reversedHit.hit->address.occurrence.path == path(OccurrenceId{1001}));
}

void verifyTransformsAndCameras() {
  auto value = product(
      partInputs(DefinitionId{10},
                 importedDocument({{1, 1, box(10, 10, 10)}})),
      {{OccurrenceId{1001}, DefinitionId{10}, placement(0, 0, 0)}});
  AssemblyScenePublicationCoordinator coordinator;
  AssemblyViewportSceneAdapter adapter;
  AssemblyViewportPicking picking;
  auto scene = publish(*value, ProductPresentationEpoch{4}, coordinator,
                       adapter, picking);
  auto initialCamera = cameraFor(*scene, 0.0F, 0.0F);
  const QPointF oldCursor = initialCamera.worldToScreen({5, 5, 5});
  CHECK(picking.faceAt(initialCamera, oldCursor));

  CHECK(value->setRootOccurrencePlacement(path(OccurrenceId{1001}),
                                          z90(90, 45, 7)) ==
        ProductOccurrenceMutationStatus::Committed);
  scene = publish(*value, ProductPresentationEpoch{4}, coordinator, adapter,
                  picking);
  auto movedCamera = cameraFor(*scene, 0.0F, 0.0F);
  CHECK(picking.faceAt(movedCamera, oldCursor).status ==
        AssemblyViewportPickingQueryStatus::Miss);
  const auto movedCenter = transformPoint(z90(90, 45, 7), {5, 5, 5});
  CHECK(movedCenter);
  CHECK(picking.faceAt(movedCamera,
                       movedCamera.worldToScreen(*movedCenter)));

  const auto arbitrary = RigidTransform3d::create(
      {-17, 23, 31}, {0.35, -0.2, 0.8, 0.42});
  CHECK(arbitrary);
  CHECK(value->setRootOccurrencePlacement(path(OccurrenceId{1001}),
                                          *arbitrary.transform) ==
        ProductOccurrenceMutationStatus::Committed);
  scene = publish(*value, ProductPresentationEpoch{4}, coordinator, adapter,
                  picking);
  const auto worldCenter = transformPoint(*arbitrary.transform, {5, 5, 5});
  CHECK(worldCenter);
  for (const auto [yaw, pitch] :
       {std::pair{0.0F, 0.0F}, std::pair{35.0F, -20.0F},
        std::pair{-70.0F, 55.0F}}) {
    const auto camera = cameraFor(*scene, yaw, pitch, QSize{913, 677}, 1.3F,
                                  QPointF{19.0, -11.0});
    const QPointF cursor = camera.worldToScreen(*worldCenter);
    const auto hit = picking.faceAt(camera, cursor);
    CHECK(hit);
    CHECK(hit.hit->address.occurrence.path == path(OccurrenceId{1001}));
    const QPointF projectedHit = camera.worldToScreen(hit.hit->worldPoint);
    CHECK(near(projectedHit.x(), cursor.x(), 1e-3));
    CHECK(near(projectedHit.y(), cursor.y(), 1e-3));
    CHECK(near(hit.hit->cameraDepth,
               camera.cameraDepth(hit.hit->worldPoint), 1e-8));
  }
}

void verifyEdgeOcclusionAndVisibility() {
  auto value = product(
      partInputs(DefinitionId{10},
                 importedDocument({{1, 1, box(10, 10, 10)}}),
                 DefinitionId{20},
                 importedDocument({{2, 2,
                                    box(2, 2, 10, {4, 4, 0})}})),
      {{OccurrenceId{1001}, DefinitionId{10}, placement(0, 0, 20)},
       {OccurrenceId{1002}, DefinitionId{20}, placement(0, 0, 0)}});
  AssemblyScenePublicationCoordinator coordinator;
  AssemblyViewportSceneAdapter adapter;
  AssemblyViewportPicking picking;
  auto scene = publish(*value, ProductPresentationEpoch{5}, coordinator,
                       adapter, picking);
  auto camera = cameraFor(*scene, 0.0F, 0.0F);

  // A front silhouette/same-surface edge is accepted.
  const auto frontEdge =
      picking.edgeAt(camera, camera.worldToScreen({0, 0, 30}), 4.0);
  CHECK(frontEdge);
  CHECK(frontEdge.hit->address.occurrence.path == path(OccurrenceId{1001}));

  // The small rear box has an edge at (4,4), but the broad front surface is
  // strictly in front and has no edge inside this narrow logical radius.
  const QPointF rearCursor = camera.worldToScreen({4, 4, 10});
  CHECK(picking.edgeAt(camera, rearCursor, 1.0).status ==
        AssemblyViewportPickingQueryStatus::Miss);
  CHECK(value->setRootOccurrenceFlags(path(OccurrenceId{1001}),
                                      {true, true, false}) ==
        ProductOccurrenceMutationStatus::Committed);
  scene = publish(*value, ProductPresentationEpoch{5}, coordinator, adapter,
                  picking);
  camera = cameraFor(*scene, 0.0F, 0.0F);
  const auto revealedRear =
      picking.edgeAt(camera, camera.worldToScreen({4, 4, 10}), 1.0);
  CHECK(revealedRear);
  CHECK(revealedRear.hit->address.occurrence.path == path(OccurrenceId{1002}));

  CHECK(value->setRootOccurrenceFlags(path(OccurrenceId{1002}),
                                      {false, false, false}) ==
        ProductOccurrenceMutationStatus::Committed);
  scene = publish(*value, ProductPresentationEpoch{5}, coordinator, adapter,
                  picking);
  CHECK(scene->instances().empty());
  const ViewportCameraState emptyCamera{0, 0, 1, {}, QSize{800, 600}, 1,
                                        {}, 100};
  CHECK(picking.faceAt(emptyCamera, QPointF{400, 312}).status ==
        AssemblyViewportPickingQueryStatus::Miss);

  auto bodyHidden = product(
      partInputs(DefinitionId{30},
                 importedDocument({{3, 3, box(5, 5, 5), false}})),
      {{OccurrenceId{3001}, DefinitionId{30}, placement(0, 0, 0)}});
  AssemblyScenePublicationCoordinator hiddenCoordinator;
  AssemblyViewportSceneAdapter hiddenAdapter;
  AssemblyViewportPicking hiddenPicking;
  auto hiddenScene = publish(*bodyHidden, ProductPresentationEpoch{6},
                             hiddenCoordinator, hiddenAdapter, hiddenPicking);
  CHECK(hiddenScene->instances().empty());
  CHECK(hiddenPicking.faceAt(emptyCamera, QPointF{400, 312}).status ==
        AssemblyViewportPickingQueryStatus::Miss);
}

void verifyReuseGuardsAndAtomicFailure() {
  auto value = product(
      partInputs(DefinitionId{10},
                 importedDocument({{1, 1, box(10, 10, 10)}})),
      {{OccurrenceId{1001}, DefinitionId{10}, placement(0, 0, 0)}});
  AssemblyScenePublicationCoordinator coordinator;
  AssemblyViewportSceneAdapter adapter;
  AssemblyViewportPicking picking;
  auto scene = publish(*value, ProductPresentationEpoch{7}, coordinator,
                       adapter, picking);
  const auto mesh = scene->sources().front().mesh;
  const auto meshBuilds = adapter.diagnostics().meshBuildAttempts;
  const auto topologyBuilds = picking.diagnostics().topologySourceBuildAttempts;
  const auto globalTopologyBuilds = TopologyIndex::buildAttemptCount();
  const auto spatialBuilds = picking.diagnostics().spatialBuilds;
  auto camera = cameraFor(*scene, 0.0F, 0.0F);
  const auto currentHit =
      picking.faceAt(camera, camera.worldToScreen({5, 5, 10}));
  CHECK(currentHit);
  const auto currentTarget = currentHit.hit->highlightTarget();
  CHECK(currentTarget);
  CHECK(currentTarget->isCurrentFor(*scene));

  CHECK(value->setRootOccurrencePlacement(path(OccurrenceId{1001}),
                                          placement(40, 0, 0)) ==
        ProductOccurrenceMutationStatus::Committed);
  scene = publish(*value, ProductPresentationEpoch{7}, coordinator, adapter,
                  picking);
  CHECK(scene->sources().front().mesh == mesh);
  CHECK(adapter.diagnostics().meshBuildAttempts == meshBuilds);
  CHECK(picking.diagnostics().topologySourceBuildAttempts == topologyBuilds);
  CHECK(TopologyIndex::buildAttemptCount() == globalTopologyBuilds);
  CHECK(picking.diagnostics().spatialBuilds == spatialBuilds + 1);
  CHECK(!currentTarget->isCurrentFor(*scene));
  CHECK(!currentHit.hit->highlightTarget());

  AssemblyScenePublicationCoordinator foreignCoordinator;
  AssemblyViewportSceneAdapter foreignAdapter;
  AssemblyViewportPicking foreignPicking;
  auto foreignScene = publish(*value, ProductPresentationEpoch{7},
                              foreignCoordinator, foreignAdapter,
                              foreignPicking);
  foreignScene = publish(*value, ProductPresentationEpoch{7},
                         foreignCoordinator, foreignAdapter, foreignPicking);
  CHECK(foreignScene->generation() == scene->generation());
  const auto foreignCamera = cameraFor(*foreignScene, 0.0F, 0.0F);
  const auto foreignHit = foreignPicking.faceAt(
      foreignCamera, foreignCamera.worldToScreen({45, 5, 10}));
  CHECK(foreignHit);
  const auto foreignTarget = foreignHit.hit->highlightTarget();
  CHECK(foreignTarget);
  CHECK(foreignTarget->isCurrentFor(*foreignScene));
  CHECK(!foreignTarget->isCurrentFor(*scene));

  CHECK(picking.faceAt(
            ViewportCameraState{0, 0, 0, {}, QSize{800, 600}, 1, {}, 100},
            QPointF{400, 300})
            .status == AssemblyViewportPickingQueryStatus::InvalidInput);

  const auto activeGeneration = picking.diagnostics().activeGeneration;
  CHECK(coordinator.rebuildAndPublish(*value, ProductPresentationEpoch{8},
                                      kNormal));
  CHECK(adapter.publish(coordinator.active()));
  AssemblyViewportPickingTestAccess::failNthTopologyBuild(picking, 1);
  const auto failed = picking.publish(adapter.active());
  CHECK(failed.status ==
        AssemblyViewportPickingPublishStatus::TopologyBuildFailed);
  CHECK(picking.diagnostics().activeGeneration == activeGeneration);
  CHECK(picking.faceAt(camera, QPointF{400, 300}).status ==
        AssemblyViewportPickingQueryStatus::StalePublication);

  // Mapping failure is equally transactional on the next fresh source key.
  CHECK(coordinator.rebuildAndPublish(*value, ProductPresentationEpoch{9},
                                      kNormal));
  CHECK(adapter.publish(coordinator.active()));
  AssemblyViewportPickingTestAccess::failNthMapping(picking, 1);
  const auto mappingFailure = picking.publish(adapter.active());
  CHECK(mappingFailure.status ==
        AssemblyViewportPickingPublishStatus::InvalidTopologyMapping);
  CHECK(picking.diagnostics().activeGeneration == activeGeneration);
}

void verifyV2PreferredAndV1HonestFallback() {
  auto v2Document = std::make_unique<Document>();
  auto& sketch = v2Document->addSketch("rectangle");
  sketch.geometry.addRectangle({0, 0}, {10, 10});
  Body& v2Body = v2Document->addBody(41, "v2");
  v2Body.addFeature(std::make_unique<ExtrudeFeature>(sketch.id, 10.0, "v2"));
  CHECK(v2Document->recompute());
  auto v2Product = product(
      partInputs(DefinitionId{40}, std::move(v2Document)),
      {{OccurrenceId{4001}, DefinitionId{40}, placement(0, 0, 0)}},
      DefinitionId{400});
  AssemblyScenePublicationCoordinator v2Coordinator;
  AssemblyViewportSceneAdapter v2Adapter;
  AssemblyViewportPicking v2Picking;
  const auto v2Scene = publish(*v2Product, ProductPresentationEpoch{10},
                               v2Coordinator, v2Adapter, v2Picking);
  CHECK(v2Scene->sources().front().topologyLineage);
  CHECK(v2Picking.diagnostics().v2FaceReferences > 0);
  const auto v2Camera = cameraFor(*v2Scene, 0.0F, 0.0F);
  const auto v2Hit =
      v2Picking.faceAt(v2Camera, v2Camera.worldToScreen({5, 5, 10}));
  CHECK(v2Hit);
  CHECK(v2Hit.hit->address.face.isV2());

  auto v1Product = product(
      partInputs(DefinitionId{50},
                 importedDocument({{51, 501, box(10, 10, 10)}})),
      {{OccurrenceId{5001}, DefinitionId{50}, placement(0, 0, 0)}},
      DefinitionId{500});
  AssemblyScenePublicationCoordinator v1Coordinator;
  AssemblyViewportSceneAdapter v1Adapter;
  AssemblyViewportPicking v1Picking;
  const auto v1Scene = publish(*v1Product, ProductPresentationEpoch{11},
                               v1Coordinator, v1Adapter, v1Picking);
  CHECK(!v1Scene->sources().front().topologyLineage);
  CHECK(v1Picking.diagnostics().v2FaceReferences == 0);
  CHECK(v1Picking.diagnostics().v1FaceFallbacks > 0);
  const auto v1Camera = cameraFor(*v1Scene, 0.0F, 0.0F);
  const auto v1Hit =
      v1Picking.faceAt(v1Camera, v1Camera.worldToScreen({5, 5, 10}));
  CHECK(v1Hit);
  CHECK(v1Hit.hit->address.face.isV1());
}

}  // namespace

int main() {
  verifyOccurrenceScopedReferencesAndBodyOwners();
  verifyRecursivePrefixIdentityAcrossQueries();
  verifyFrontmostPlacementAndLexicalTie();
  verifyTransformsAndCameras();
  verifyEdgeOcclusionAndVisibility();
  verifyReuseGuardsAndAtomicFailure();
  verifyV2PreferredAndV1HonestFallback();
  return EXIT_SUCCESS;
}
