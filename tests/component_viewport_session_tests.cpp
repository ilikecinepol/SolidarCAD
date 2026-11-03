#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "ui/ComponentViewportSession.h"

namespace {
using namespace solidar;
constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

std::unique_ptr<Document> document() {
  auto result=std::make_unique<Document>();auto& body=result->addBody(BodyId{1},"body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(FeatureId{2},
    std::make_shared<const TopoDS_Shape>(BRepPrimAPI_MakeBox(10,10,10).Shape()),"box"));
  CHECK(result->recompute());return result;
}
RigidTransform3d placement(double x){auto made=RigidTransform3d::create({x,0,0},{1,0,0,0});CHECK(made);return *made.transform;}
std::unique_ptr<ProductDocument> product(){
  ComponentDefinition root(DefinitionId{10},document()),child(DefinitionId{20},document());
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{3},DefinitionId{20},placement(30)))==ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;definitions.push_back(std::move(root));definitions.push_back(std::move(child));
  auto made=ProductDocument::createComponentGraph(std::move(definitions),DefinitionId{10});CHECK(made);return std::move(made.product);
}
void moveChild(ProductDocument& product,double x){
  auto* root=const_cast<ComponentDefinition*>(product.rootComponentDefinition());CHECK(root);
  const auto* old=root->find(OccurrenceId{3});CHECK(old);const auto flags=old->flags();
  CHECK(root->erase(OccurrenceId{3})==ComponentOccurrenceEraseStatus::Erased);
  CHECK(root->insert(ComponentOccurrence(OccurrenceId{3},DefinitionId{20},placement(x),flags))==ComponentOccurrenceInsertStatus::Inserted);
}

void verifyTransactionSelectionFitAndLifetime(){
  auto value=product();
  auto prepared=ComponentViewportSession::prepare(*value,ProductPresentationEpoch{71},kNormal);CHECK(prepared);
  auto session=std::move(prepared.session);CHECK(session->components().size()==2);
  CHECK(session->scene()->isCurrent());CHECK(session->fit(QSize{800,600},0,0));
  const auto camera=session->camera(0,0,1,{},QSize{800,600});
  CHECK(session->faceAt(camera,camera.worldToScreen({5,5,10})));
  const auto root=ComponentInstanceSelection::create(session->publicationHandle(),session->components()[0].address);CHECK(root);
  CHECK(session->setSelectedComponent(root));CHECK(session->selectedHighlight());
  CHECK(session->selectedHighlight()->visibleBodies(*session->scene()).size()==1);
  const auto child=ComponentInstanceSelection::create(session->publicationHandle(),session->components()[1].address);CHECK(child);
  CHECK(session->setHoveredComponent(child));CHECK(session->hoveredHighlight());

  moveChild(*value,50);
  auto projected=ComponentViewportSession::prepareSameEpochProjection(*value,*session,kNormal);CHECK(projected);
  CHECK(projected.session->sceneDiagnostics().meshBuildAttempts==0);
  CHECK(projected.session->picking().diagnostics().topologySourceBuildAttempts==0);
  CHECK(!projected.session->setSelectedComponent(root));
  const auto incumbentScene=session->scene();
  for(auto failure:{ComponentViewportSessionFailurePoint::Presentation,
                    ComponentViewportSessionFailurePoint::Scene,
                    ComponentViewportSessionFailurePoint::Picking,
                    ComponentViewportSessionFailurePoint::Selection,
                    ComponentViewportSessionFailurePoint::Resource,
                    ComponentViewportSessionFailurePoint::FinalPreparation}){
    auto failed=ComponentViewportSession::prepareSameEpochProjection(*value,*session,kNormal,failure);
    CHECK(!failed);CHECK(session->scene()==incumbentScene);CHECK(session->scene()->isCurrent());
  }
  value.reset();
  CHECK(projected.session->scene()->isCurrent());
  const auto projectedCamera=projected.session->camera(0,0,1,{},QSize{800,600});
  CHECK(projected.session->faceAt(projectedCamera,projectedCamera.worldToScreen({55,5,10})));
  const auto handle=projected.session->publicationHandle();projected.session.reset();CHECK(!handle.isCurrent());
}
}  // namespace

int main(){verifyTransactionSelectionFitAndLifetime();return EXIT_SUCCESS;}
