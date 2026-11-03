#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/AssemblyScenePublicationCoordinator.h"
#include "ui/AssemblyViewportPicking.h"
#include "ui/AssemblyViewportRuler.h"
#include "ui/AssemblyViewportScene.h"

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

ShapeFeature::ShapePtr box(double x, double y, double z) {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), x, y, z).Shape());
}

ShapeFeature::ShapePtr planarSquare() {
  return std::make_shared<const TopoDS_Shape>(
      BRepBuilderAPI_MakeFace(
          gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0.0, 10.0, 0.0,
          10.0)
          .Shape());
}

RigidTransform3d placement(double x, double y, double z) {
  const auto made =
      RigidTransform3d::create({x, y, z}, {1.0, 0.0, 0.0, 0.0});
  CHECK(made);
  return *made.transform;
}

OccurrencePath path(std::uint64_t value) {
  auto made = OccurrencePath::create({OccurrenceId{value}});
  CHECK(made);
  return std::move(*made.path);
}

std::unique_ptr<Document> documentFromShape(ShapeFeature::ShapePtr shape,
                                            bool visible = true) {
  auto result = std::make_unique<Document>();
  Body& body = result->addBody(1, "body");
  body.setVisible(visible);
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(1, std::move(shape), "shape"));
  CHECK(result->recompute());
  return result;
}

std::unique_ptr<Document> document(bool visible = true) {
  return documentFromShape(box(10, 10, 10), visible);
}

struct OccurrenceSpec final {
  std::uint64_t id{};
  RigidTransform3d transform{RigidTransform3d::identity()};
  OccurrenceFlags flags;
};

std::unique_ptr<ProductDocument> productFromDocument(
    std::vector<OccurrenceSpec> specs, std::unique_ptr<Document> source) {
  DefinitionRegistry registry;
  CHECK(registry.insert(PartDefinition(DefinitionId{10},
                                       std::move(source))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition assembly(DefinitionId{100});
  for (auto& spec : specs) {
    CHECK(assembly.insert(ComponentOccurrence(
              OccurrenceId{spec.id}, DefinitionId{10}, spec.transform,
              spec.flags)) == AssemblyOccurrenceInsertStatus::Inserted);
  }
  CHECK(registry.insert(std::move(assembly)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(registry), DefinitionId{100});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> product(std::vector<OccurrenceSpec> specs,
                                         bool bodyVisible = true) {
  return productFromDocument(std::move(specs), document(bodyVisible));
}

struct Published final {
  AssemblyScenePublicationCoordinator coordinator;
  AssemblyViewportSceneAdapter adapter;
  AssemblyViewportPicking picking;
  std::shared_ptr<const AssemblyViewportScene> scene;
};

void publish(Published& target, ProductDocument& product,
             ProductPresentationEpoch epoch = ProductPresentationEpoch{1}) {
  CHECK(target.coordinator.rebuildAndPublish(product, epoch, kNormal));
  CHECK(target.adapter.publish(target.coordinator.active()));
  CHECK(target.picking.publish(target.adapter.active()));
  target.scene = target.adapter.active();
  CHECK(target.scene);
}

ViewportCameraState camera(const AssemblyViewportScene& scene,
                           float yaw = 0.0F, float pitch = 0.0F,
                           float dpr = 1.0F, float zoom = 1.0F,
                           QPointF pan = {}) {
  CHECK(scene.worldBounds());
  return {yaw, pitch, zoom, pan, QSize{800, 600}, dpr,
          scene.worldBounds()->center(),
          std::max(1.0, scene.worldBounds()->diagonal() * 3.0)};
}

QPointF projectLogical(const ViewportCameraState& camera, Point3d world) {
  const double width = camera.logicalSize.width();
  const double height = camera.logicalSize.height();
  const double scale = std::min(width, height) * 0.008 * camera.zoom;
  const double yaw = camera.yawDeg * std::numbers::pi / 180.0;
  const double pitch = camera.pitchDeg * std::numbers::pi / 180.0;
  const double cy = std::cos(yaw);
  const double sy = std::sin(yaw);
  const double cp = std::cos(pitch);
  const double sp = std::sin(pitch);
  const double right = world.x * cy - world.y * sy;
  const double up = -world.x * sy * cp - world.y * cy * cp + world.z * sp;
  return {width * 0.5 + camera.pan.x() + scale * right,
          height * 0.52 + camera.pan.y() - scale * up};
}

bool containsPath(const auto& hits, std::uint64_t id) {
  return std::any_of(hits.begin(), hits.end(), [id](const auto& hit) {
    return hit.address.occurrence.path == path(id);
  });
}

std::size_t projectedSegmentCount(const AssemblyViewportScene& scene) {
  std::size_t count = 0;
  for (const auto& instance : scene.instances())
    for (const auto& edge : instance.mesh->edges())
      if (edge.points.size() > 1) count += edge.points.size() - 1;
  return count;
}

void verifyRectangleIdentityOcclusionAndBoundaries() {
  auto value = product({{1001, placement(0, 0, 20), {}},
                        {1002, placement(0, 0, 0), {}}});
  Published published;
  publish(published, *value);
  auto view = camera(*published.scene);
  const QPointF center = view.worldToScreen({5, 5, 30});
  auto faces = published.picking.facesInRectangle(
      view, center - QPointF{30, 30}, center + QPointF{30, 30});
  CHECK(faces);
  CHECK(containsPath(faces.hits, 1001));
  CHECK(!containsPath(faces.hits, 1002));
  const auto frontSnap = published.picking.snapAt(view, center, 1.0);
  CHECK(frontSnap);
  CHECK(frontSnap.hit->occurrence.path == path(1001));

  CHECK(value->setRootOccurrencePlacement(path(1002), placement(7, 0, 0)) ==
        ProductOccurrenceMutationStatus::Committed);
  publish(published, *value);
  view = camera(*published.scene);
  const QPointF unionCenter = view.worldToScreen({8.5, 5, 30});
  faces = published.picking.facesInRectangle(
      view, unionCenter + QPointF{80, 40}, unionCenter - QPointF{80, 40});
  CHECK(faces);
  CHECK(containsPath(faces.hits, 1001));
  CHECK(containsPath(faces.hits, 1002));

  CHECK(value->setRootOccurrencePlacement(path(1002), placement(0, 0, 20)) ==
        ProductOccurrenceMutationStatus::Committed);
  publish(published, *value);
  view = camera(*published.scene);
  faces = published.picking.facesInRectangle(
      view, view.worldToScreen({0, 0, 30}), view.worldToScreen({10, 10, 30}));
  CHECK(faces);
  CHECK(containsPath(faces.hits, 1001));
  CHECK(containsPath(faces.hits, 1002));
  CHECK(faces.hits.front().address.occurrence.path == path(1001));
  const auto coplanarSnap = published.picking.snapAt(
      view, view.worldToScreen({5, 5, 30}), 1.0);
  CHECK(coplanarSnap);
  CHECK(coplanarSnap.hit->occurrence.path == path(1001));

  const QPointF boundary = view.worldToScreen({0, 0, 30});
  const auto edges = published.picking.edgesInRectangle(
      view, boundary, boundary + QPointF{1, 1});
  CHECK(edges);
  CHECK(containsPath(edges.hits, 1001));
  CHECK(containsPath(edges.hits, 1002));
  bool sharedLocalEdge = false;
  for (const auto& first : edges.hits)
    for (const auto& second : edges.hits)
      if (first.address.occurrence.path == path(1001) &&
          second.address.occurrence.path == path(1002) &&
          first.address.edge == second.address.edge)
        sharedLocalEdge = true;
  CHECK(sharedLocalEdge);
}

void verifySnapRulerCameraAndDpr() {
  auto value = product({{1001, placement(0, 0, 0), {}},
                        {1002, placement(30, 0, 0), {}}});
  Published published;
  publish(published, *value, ProductPresentationEpoch{2});
  for (const float dpr : {1.0F, 1.25F, 1.5F, 2.0F}) {
    const auto view = camera(*published.scene, 0.0F, 0.0F, dpr);
    const QPointF vertexPixel = view.worldToScreen({0, 0, 10});
    const auto vertex = published.picking.snapAt(view, vertexPixel, 8.0);
    CHECK(vertex);
    CHECK(vertex.hit->kind == AssemblySnapKind::Vertex);
    CHECK(vertex.hit->supportingEdge.has_value());
    CHECK(vertex.hit->occurrence.path == path(1001));

    const QPointF surfacePixel = view.worldToScreen({5, 5, 10});
    const auto surface = published.picking.snapAt(view, surfacePixel, 1.0);
    CHECK(surface);
    CHECK(surface.hit->kind == AssemblySnapKind::Surface);
    CHECK(surface.hit->supportingFace.has_value());
    const auto edge = published.picking.snapAt(
        view, view.worldToScreen({5, 0, 10}), 3.0);
    CHECK(edge);
    CHECK(edge.hit->kind == AssemblySnapKind::Edge);
    CHECK(edge.hit->supportingEdge.has_value());
  }

  const auto budgetView = camera(*published.scene);
  const std::size_t segmentCount = projectedSegmentCount(*published.scene);
  CHECK(segmentCount > 0);
  const QPointF edgePixel = budgetView.worldToScreen({5, 0, 10});
  const auto exactEdge = published.picking.snapAt(budgetView, edgePixel, 3.0);
  CHECK(exactEdge);
  CHECK(exactEdge.hit->kind == AssemblySnapKind::Edge);
  const auto edgeBudgetExhausted = published.picking.snapAt(
      budgetView, edgePixel, 3.0, {segmentCount * 2, 0});
  CHECK(edgeBudgetExhausted.status ==
        AssemblyViewportPickingQueryStatus::Uncertain);
  CHECK(!edgeBudgetExhausted.hit);
  const QPointF facePixel = budgetView.worldToScreen({5, 5, 10});
  const auto exactFace = published.picking.snapAt(budgetView, facePixel, 1.0);
  CHECK(exactFace);
  CHECK(exactFace.hit->kind == AssemblySnapKind::Surface);
  const auto faceBudgetExhausted = published.picking.snapAt(
      budgetView, facePixel, 1.0, {segmentCount * 3, 0});
  CHECK(faceBudgetExhausted.status ==
        AssemblyViewportPickingQueryStatus::Uncertain);
  CHECK(!faceBudgetExhausted.hit);
  const auto edgeOcclusionBudgetExhausted = published.picking.snapAt(
      budgetView, edgePixel, 3.0, {0, 1});
  CHECK(edgeOcclusionBudgetExhausted.status ==
        AssemblyViewportPickingQueryStatus::Uncertain);
  CHECK(!edgeOcclusionBudgetExhausted.hit);
  AssemblyViewportRuler hoverRuler;
  hoverRuler.updateHover(exactFace);
  CHECK(hoverRuler.hoverPoint());
  const Point3d provenHover = hoverRuler.hoverPoint()->worldPoint;
  hoverRuler.updateHover(edgeBudgetExhausted);
  CHECK(hoverRuler.hoverPoint());
  CHECK(hoverRuler.hoverPoint()->worldPoint.x == provenHover.x);
  CHECK(hoverRuler.hoverPoint()->worldPoint.y == provenHover.y);
  CHECK(hoverRuler.hoverPoint()->worldPoint.z == provenHover.z);

  const auto rotated = RigidTransform3d::create(
      {20, 15, 5}, {std::sqrt(0.5), 0.0, 0.0, std::sqrt(0.5)});
  CHECK(rotated);
  CHECK(value->setRootOccurrencePlacement(path(1002), *rotated.transform) ==
        ProductOccurrenceMutationStatus::Committed);
  const auto topologyBuilds = published.picking.diagnostics()
                                  .topologySourceBuildSuccesses;
  publish(published, *value, ProductPresentationEpoch{2});
  CHECK(published.picking.diagnostics().topologySourceBuildSuccesses ==
        topologyBuilds);
  CHECK(published.picking.diagnostics().topologySourceReuses > 0);
  auto view = camera(*published.scene, 35.0F, -20.0F, 1.5F, 1.3F,
                     QPointF{19, -11});
  const auto first = published.picking.snapAt(
      view, view.worldToScreen({0, 0, 10}), 8.0);
  const auto second = published.picking.snapAt(
      view, view.worldToScreen({20, 15, 15}), 8.0);
  CHECK(first && second);
  const QPointF transformedCenter = view.worldToScreen({15, 20, 15});
  const auto transformedRectangle = published.picking.facesInRectangle(
      view, transformedCenter - QPointF{20, 20},
      transformedCenter + QPointF{20, 20});
  CHECK(transformedRectangle);
  CHECK(containsPath(transformedRectangle.hits, 1002));
  AssemblyViewportRuler ruler;
  CHECK(ruler.commit(first) ==
        AssemblyViewportRulerCommitStatus::AcceptedFirstPoint);
  ruler.updateHover(second);
  CHECK(ruler.hoverPoint());
  CHECK(ruler.commit(second) ==
        AssemblyViewportRulerCommitStatus::AcceptedSecondPoint);
  CHECK(ruler.distance());
  const double dx = second.hit->worldPoint.x - first.hit->worldPoint.x;
  const double dy = second.hit->worldPoint.y - first.hit->worldPoint.y;
  const double dz = second.hit->worldPoint.z - first.hit->worldPoint.z;
  CHECK(std::abs(*ruler.distance() - std::sqrt(dx * dx + dy * dy + dz * dz)) <
        1e-8);

  const auto projectedBuilds = published.picking.diagnostics().projectedBuilds;
  CHECK(published.picking.facesInRectangle(
      view, QPointF{0, 0}, QPointF{800, 600}));
  CHECK(published.picking.diagnostics().projectedReuses > 0);
  view.yawDeg += 10.0F;
  CHECK(published.picking.facesInRectangle(
      view, QPointF{0, 0}, QPointF{800, 600}));
  CHECK(published.picking.diagnostics().projectedBuilds ==
        projectedBuilds + 1);

  const auto successfulBuilds =
      published.picking.diagnostics().projectedBuilds;
  AssemblyViewportPickingTestAccess::failNthProjectionBuild(
      published.picking, 1);
  auto failingCamera = view;
  failingCamera.pitchDeg += 7.0F;
  CHECK(published.picking
            .facesInRectangle(failingCamera, QPointF{0, 0},
                              QPointF{800, 600})
            .status == AssemblyViewportPickingQueryStatus::InvalidInput);
  CHECK(published.picking.diagnostics().projectedBuilds == successfulBuilds);
  CHECK(published.picking.facesInRectangle(
      view, QPointF{0, 0}, QPointF{800, 600}));
  CHECK(published.picking.diagnostics().projectedBuilds == successfulBuilds);
}

void verifyPositiveFaceAreaAndEdgeBoundaryContact() {
  auto value = product({{1001, placement(0, 0, 0), {}}});
  Published published;
  publish(published, *value, ProductPresentationEpoch{21});
  const auto view = camera(*published.scene);
  const QPointF maximum = projectLogical(view, {10, 10, 10});

  const auto vertexTouchFaces = published.picking.facesInRectangle(
      view, maximum, maximum + QPointF{8, 8});
  CHECK(vertexTouchFaces.status == AssemblyViewportPickingQueryStatus::Miss);
  const auto vertexTouchEdges = published.picking.edgesInRectangle(
      view, maximum, maximum + QPointF{8, 8});
  CHECK(vertexTouchEdges);

  const double top = projectLogical(view, {10, 2, 10}).y();
  const double bottom = projectLogical(view, {10, 8, 10}).y();
  const auto edgeTouchFaces = published.picking.facesInRectangle(
      view, QPointF{maximum.x(), top},
      QPointF{maximum.x() + 8, bottom});
  CHECK(edgeTouchFaces.status == AssemblyViewportPickingQueryStatus::Miss);
  const auto edgeTouchEdges = published.picking.edgesInRectangle(
      view, QPointF{maximum.x(), top},
      QPointF{maximum.x() + 8, bottom});
  CHECK(edgeTouchEdges);

  const auto nearZeroFaces = published.picking.facesInRectangle(
      view, QPointF{maximum.x() - 1e-13, top},
      QPointF{maximum.x() + 8, bottom});
  CHECK(nearZeroFaces.status == AssemblyViewportPickingQueryStatus::Miss);

  auto planarShape = std::make_shared<const TopoDS_Shape>(
      BRepBuilderAPI_MakeFace(
          gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0.0, 10.0, 0.0,
          10.0)
          .Shape());
  auto planar = productFromDocument(
      {{1001, placement(0, 0, 0), {}}},
      documentFromShape(std::move(planarShape)));
  Published planarPublished;
  publish(planarPublished, *planar, ProductPresentationEpoch{22});
  const auto edgeOnView = camera(*planarPublished.scene, 0.0F, 90.0F);
  const QPointF lineStart = projectLogical(edgeOnView, {0, 5, 0});
  const QPointF lineEnd = projectLogical(edgeOnView, {10, 5, 0});
  const auto edgeOnFaces = planarPublished.picking.facesInRectangle(
      edgeOnView, lineStart - QPointF{2, 2}, lineEnd + QPointF{2, 2});
  CHECK(edgeOnFaces.status == AssemblyViewportPickingQueryStatus::Miss);
  const auto edgeOnEdges = planarPublished.picking.edgesInRectangle(
      edgeOnView, lineStart - QPointF{2, 2}, lineEnd + QPointF{2, 2});
  CHECK(edgeOnEdges);
}

void verifySnapClipRange() {
  auto mixed = productFromDocument(
      {{1001, placement(0, 0, 3), {}},
       {1002, placement(0, 0, 0), {}}},
      documentFromShape(planarSquare()));
  Published mixedPublished;
  publish(mixedPublished, *mixed, ProductPresentationEpoch{23});
  auto mixedView = camera(*mixedPublished.scene);
  mixedView.center = {5, 5, 0};
  mixedView.depthExtent = 1.0;

  const auto mixedVertex = mixedPublished.picking.snapAt(
      mixedView, projectLogical(mixedView, {0, 0, 0}), 0.5);
  CHECK(mixedVertex.status == AssemblyViewportPickingQueryStatus::Hit);
  CHECK(mixedVertex.hit);
  CHECK(mixedVertex.hit->kind == AssemblySnapKind::Vertex);
  CHECK(mixedVertex.hit->occurrence.path == path(1002));

  const auto mixedEdge = mixedPublished.picking.snapAt(
      mixedView, projectLogical(mixedView, {5, 0, 0}), 0.5);
  CHECK(mixedEdge.status == AssemblyViewportPickingQueryStatus::Hit);
  CHECK(mixedEdge.hit);
  CHECK(mixedEdge.hit->kind == AssemblySnapKind::Edge);
  CHECK(mixedEdge.hit->occurrence.path == path(1002));

  const auto mixedSurface = mixedPublished.picking.snapAt(
      mixedView, projectLogical(mixedView, {5, 5, 0}), 0.5);
  CHECK(mixedSurface.status == AssemblyViewportPickingQueryStatus::Hit);
  CHECK(mixedSurface.hit);
  CHECK(mixedSurface.hit->kind == AssemblySnapKind::Surface);
  CHECK(mixedSurface.hit->occurrence.path == path(1002));

  auto outside = productFromDocument(
      {{1001, placement(0, 0, 3), {}},
       {1002, placement(0, 0, -3), {}}},
      documentFromShape(planarSquare()));
  Published outsidePublished;
  publish(outsidePublished, *outside, ProductPresentationEpoch{24});
  auto outsideView = camera(*outsidePublished.scene);
  outsideView.center = {5, 5, 0};
  outsideView.depthExtent = 1.0;
  for (const Point3d query :
       {Point3d{0, 0, 0}, Point3d{5, 0, 0}, Point3d{5, 5, 0}}) {
    const auto result = outsidePublished.picking.snapAt(
        outsideView, projectLogical(outsideView, query), 0.5);
    CHECK(result.status == AssemblyViewportPickingQueryStatus::Miss);
    CHECK(!result.hit);
  }

  auto boundaries = productFromDocument(
      {{1001, placement(0, 0, 1), {}},
       {1002, placement(20, 0, -1), {}}},
      documentFromShape(planarSquare()));
  Published boundaryPublished;
  publish(boundaryPublished, *boundaries, ProductPresentationEpoch{25});
  auto boundaryView = camera(*boundaryPublished.scene);
  boundaryView.center = {15, 5, 0};
  boundaryView.depthExtent = 1.0;
  for (const auto [world, expectedPath] :
       {std::pair{Point3d{5, 5, 1}, std::uint64_t{1001}},
        std::pair{Point3d{25, 5, -1}, std::uint64_t{1002}}}) {
    const QPointF pixel = projectLogical(boundaryView, world);
    const auto face = boundaryPublished.picking.faceAt(boundaryView, pixel);
    CHECK(face.status == AssemblyViewportPickingQueryStatus::Hit);
    CHECK(face.hit);
    CHECK(face.hit->address.occurrence.path == path(expectedPath));
    const auto snap = boundaryPublished.picking.snapAt(boundaryView, pixel, 0.5);
    CHECK(snap.status == AssemblyViewportPickingQueryStatus::Hit);
    CHECK(snap.hit);
    CHECK(snap.hit->kind == AssemblySnapKind::Surface);
    CHECK(snap.hit->occurrence.path == path(expectedPath));
  }
}

void verifyVisibilityStaleForeignBudgetsAndInvalidInput() {
  auto value = product({{1001, placement(0, 0, 0), {}},
                        {1002, placement(30, 0, 0),
                         {false, false, false}},
                        {1003, placement(60, 0, 0),
                         {true, true, false}}});
  Published published;
  publish(published, *value, ProductPresentationEpoch{3});
  const auto view = camera(*published.scene);
  auto faces = published.picking.facesInRectangle(
      view, QPointF{0, 0}, QPointF{800, 600});
  CHECK(faces);
  CHECK(containsPath(faces.hits, 1001));
  CHECK(!containsPath(faces.hits, 1002));
  CHECK(!containsPath(faces.hits, 1003));

  const auto hit = published.picking.snapAt(
      view, view.worldToScreen({0, 0, 10}), 8.0);
  CHECK(hit);
  AssemblyViewportRuler ruler;
  CHECK(ruler.commit(hit) ==
        AssemblyViewportRulerCommitStatus::AcceptedFirstPoint);
  ruler.updateHover(hit);
  const AssemblyViewportSnapQueryResult uncertain{
      AssemblyViewportPickingQueryStatus::Uncertain, std::nullopt};
  ruler.updateHover(uncertain);
  CHECK(ruler.hoverPoint());
  CHECK(ruler.commit(uncertain) ==
        AssemblyViewportRulerCommitStatus::RejectedUncertain);

  const auto bounded = published.picking.facesInRectangle(
      view, QPointF{0, 0}, QPointF{800, 600}, {1, 1});
  CHECK(bounded.status == AssemblyViewportPickingQueryStatus::Uncertain);
  CHECK(bounded.hits.empty());

  Published foreign;
  auto foreignValue = product({{1001, placement(0, 0, 0), {}}});
  publish(foreign, *foreignValue, ProductPresentationEpoch{3});
  const auto foreignView = camera(*foreign.scene);
  const auto foreignHit = foreign.picking.snapAt(
      foreignView, foreignView.worldToScreen({0, 0, 10}), 8.0);
  CHECK(foreignHit);
  CHECK(ruler.commit(foreignHit) ==
        AssemblyViewportRulerCommitStatus::RejectedStaleOrForeign);
  CHECK(!ruler.firstPoint());

  const auto old = hit;
  CHECK(value->setRootOccurrencePlacement(path(1001), placement(5, 0, 0)) ==
        ProductOccurrenceMutationStatus::Committed);
  publish(published, *value, ProductPresentationEpoch{3});
  CHECK(old.hit);
  CHECK(!old.hit->publication.isCurrent());
  CHECK(ruler.commit(old) ==
        AssemblyViewportRulerCommitStatus::RejectedStaleOrForeign);

  const double nan = std::numeric_limits<double>::quiet_NaN();
  CHECK(published.picking.facesInRectangle(
            camera(*published.scene), QPointF{nan, 0}, QPointF{1, 1})
            .status == AssemblyViewportPickingQueryStatus::InvalidInput);
  CHECK(published.picking.edgesInRectangle(
            camera(*published.scene), QPointF{1, 1}, QPointF{1, 2})
            .status == AssemblyViewportPickingQueryStatus::InvalidInput);
  CHECK(published.picking.snapAt(camera(*published.scene), QPointF{0, 0}, 0.0)
            .status == AssemblyViewportPickingQueryStatus::InvalidInput);

  auto bodyHidden = product({{1001, placement(0, 0, 0), {}}}, false);
  Published hidden;
  publish(hidden, *bodyHidden, ProductPresentationEpoch{4});
  CHECK(hidden.scene->instances().empty());
}

}  // namespace

int main() {
  verifyRectangleIdentityOcclusionAndBoundaries();
  verifySnapRulerCameraAndDpr();
  verifyPositiveFaceAreaAndEdgeBoundaryContact();
  verifySnapClipRange();
  verifyVisibilityStaleForeignBudgetsAndInvalidInput();
  return EXIT_SUCCESS;
}
