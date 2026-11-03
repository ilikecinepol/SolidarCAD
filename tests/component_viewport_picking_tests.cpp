#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/ComponentScenePublicationCoordinator.h"
#include "ui/ComponentViewportPicking.h"
#include "ui/ComponentViewportScene.h"

namespace {
using namespace solidar;
constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

std::shared_ptr<const TopoDS_Shape> box(double size) {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(size,size,size).Shape());
}
std::unique_ptr<Document> document(double size) {
  auto result=std::make_unique<Document>();
  auto& body=result->addBody(BodyId{7},"body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{70},box(size),"box"));
  CHECK(result->recompute()); return result;
}
RigidTransform3d translate(double x,double y=0,double z=0) {
  auto made=RigidTransform3d::create({x,y,z},{1,0,0,0});CHECK(made);
  return *made.transform;
}
std::unique_ptr<ProductDocument> product() {
  ComponentDefinition root(DefinitionId{300},document(10));
  ComponentDefinition child(DefinitionId{200},document(8));
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{10},DefinitionId{200},translate(30)))==ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{11},DefinitionId{200},translate(60)))==ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(child));definitions.push_back(std::move(root));
  auto made=ProductDocument::createComponentGraph(std::move(definitions),DefinitionId{300});
  CHECK(made);return std::move(made.product);
}
std::shared_ptr<const ComponentViewportScene> publish(
    ProductDocument& product,ProductPresentationEpoch epoch,
    ComponentScenePublicationCoordinator& coordinator,
    ComponentViewportSceneAdapter& adapter,ComponentViewportPicking& picking) {
  CHECK(coordinator.rebuildAndPublish(product,epoch,kNormal));
  CHECK(adapter.publish(coordinator.active()));CHECK(picking.publish(adapter.active()));
  return adapter.active();
}
ViewportCameraState camera(const ComponentViewportScene& scene) {
  CHECK(scene.worldBounds());
  return {0,0,1,{},QSize{900,600},1,scene.worldBounds()->center(),
          std::max(1.0,scene.worldBounds()->diagonal()*3)};
}
const ComponentViewportSceneInstance& instance(
    const ComponentViewportScene& scene,DefinitionId definition,
    std::optional<OccurrenceId> occurrence) {
  const auto found=std::find_if(scene.instances().begin(),scene.instances().end(),
      [&](const auto& value){
        if(value.bodyIdentity.component.definitionId!=definition)return false;
        if(!occurrence)return value.bodyIdentity.component.rootInstance();
        return value.bodyIdentity.component.occurrencePath&&
          value.bodyIdentity.component.occurrencePath->entries().back()==*occurrence;
      });
  CHECK(found!=scene.instances().end());return *found;
}

void verifyRootDescendantTransformsAndStableIdentity() {
  auto value=product();ComponentScenePublicationCoordinator coordinator;
  ComponentViewportSceneAdapter adapter;ComponentViewportPicking picking;
  auto scene=publish(*value,ProductPresentationEpoch{41},coordinator,adapter,picking);
  CHECK(scene->instances().size()==3);CHECK(scene->sources().size()==2);
  const auto& root=instance(*scene,DefinitionId{300},std::nullopt);
  const auto& first=instance(*scene,DefinitionId{200},OccurrenceId{10});
  CHECK(root.sourceKey.bodyId==first.sourceKey.bodyId);
  CHECK(root.sourceKey.featureId==first.sourceKey.featureId);
  CHECK(root.sourceKey.definitionId!=first.sourceKey.definitionId);
  const auto view=camera(*scene);
  const auto rootPixel=view.worldToScreen({5,5,10});
  const auto rootHit=picking.faceAt(view,rootPixel);CHECK(rootHit);
  CHECK(rootHit.hit->address.body==root.bodyIdentity);
  CHECK(rootHit.hit->address.body.component.rootInstance());
  CHECK(rootHit.hit->highlightTarget());
  CHECK(rootHit.hit->highlightTarget()->visibleBodies(*scene).size()==1);
  const auto childPixel=view.worldToScreen({34,4,8});
  const auto childHit=picking.faceAt(view,childPixel);CHECK(childHit);
  CHECK(childHit.hit->address.body==first.bodyIdentity);
  CHECK(childHit.hit->address.body.component.occurrencePath->depth()==1);
  CHECK(childHit.hit->address.face.bodyId()==BodyId{7});
  const auto edgeHit=picking.edgeAt(view,view.worldToScreen({30,0,8}),12);
  CHECK(edgeHit);CHECK(edgeHit.hit->address.body==first.bodyIdentity);
}

void verifyExactPublicationAndCacheReuse() {
  auto value=product();ComponentScenePublicationCoordinator coordinator;
  ComponentViewportSceneAdapter adapter;ComponentViewportPicking picking;
  auto oldScene=publish(*value,ProductPresentationEpoch{51},coordinator,adapter,picking);
  const auto oldHandle=oldScene->publicationHandle();
  const auto root=instance(*oldScene,DefinitionId{300},std::nullopt).bodyIdentity.component;
  auto oldTarget=ComponentInstanceHighlightTarget::create(oldHandle,root);CHECK(oldTarget);
  ComponentViewportPicking reusable;CHECK(reusable.seedReusableSourcesFrom(picking));
  CHECK(coordinator.rebuildAndPublish(*value,ProductPresentationEpoch{51},kNormal));
  CHECK(adapter.publish(coordinator.active()));CHECK(reusable.publish(adapter.active()));
  CHECK(reusable.diagnostics().topologySourceBuildAttempts==0);
  CHECK(reusable.diagnostics().topologySourceReuses==2);
  CHECK(!oldScene->isCurrent());
  CHECK(picking.faceAt(camera(*adapter.active()),QPointF{0,0}).status==
        ComponentViewportPickingQueryStatus::StalePublication);

  ComponentScenePublicationCoordinator foreign;
  ComponentViewportSceneAdapter foreignAdapter;
  CHECK(foreign.rebuildAndPublish(*value,ProductPresentationEpoch{51},kNormal));
  CHECK(foreign.active().generation().value()==oldHandle.generation().value());
  CHECK(foreignAdapter.publish(foreign.active()));
  CHECK(!oldTarget->isCurrentFor(*foreignAdapter.active()));
  CHECK(ComponentInstanceHighlightTarget::create(foreign.active(),root));
}
}  // namespace

int main(){
  verifyRootDescendantTransformsAndStableIdentity();
  verifyExactPublicationAndCacheReuse();
  return EXIT_SUCCESS;
}
