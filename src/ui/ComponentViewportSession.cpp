#include "ui/ComponentViewportSession.h"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

#include "model/ProductDocument.h"

namespace solidar {
namespace {

bool populateStates(const ComponentPresentationSnapshot& snapshot,
                    std::vector<ComponentInstanceSessionState>* states) {
  if(!states||snapshot.graphTraversal().visits.empty()) return false;
  const DefinitionId root=snapshot.graphTraversal().visits.front().definitionId;
  states->clear(); states->reserve(snapshot.graphTraversal().visits.size());
  for(const auto& visit:snapshot.graphTraversal().visits){
    ComponentInstanceAddress address{snapshot.epoch(),root,visit.definitionId,
                                     visit.path};
    states->push_back({std::move(address),visit.authoredFlags.visible,
                       visit.authoredFlags.suppressed,
                       visit.authoredFlags.grounded,
                       visit.effectiveVisible&&!visit.effectiveSuppressed});
  }
  return true;
}

}  // namespace

ComponentViewportSession::PrepareResult ComponentViewportSession::prepareImpl(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile profile,
    ComponentViewportSessionFailurePoint failurePoint,
    const ComponentViewportSession* reusable,
    const ComponentIdentityTransition* transition) noexcept {
  if(!epoch.isValid()) return {ComponentViewportSessionStatus::InvalidEpoch,{}};
  if(!product.rootComponentDefinition())
    return {ComponentViewportSessionStatus::InvalidProduct,{}};
  if(failurePoint==ComponentViewportSessionFailurePoint::Resource)
    return {ComponentViewportSessionStatus::ResourceFailure,{}};
  if(failurePoint==ComponentViewportSessionFailurePoint::Presentation)
    return {ComponentViewportSessionStatus::PresentationFailed,{}};
  try{
    auto candidate=std::unique_ptr<ComponentViewportSession>(new ComponentViewportSession);
    candidate->epoch_=epoch;
    if(reusable&&(!candidate->sceneAdapter_.seedReusableSourcesFrom(reusable->sceneAdapter_)||
                 !candidate->picking_.seedReusableSourcesFrom(reusable->picking_)))
      return {ComponentViewportSessionStatus::ResourceFailure,{}};
    const auto publication = transition
        ? candidate->coordinator_.rebuildAndPublishProjectedIdentity(
              product, epoch, profile, *transition)
        : candidate->coordinator_.rebuildAndPublish(product, epoch, profile);
    if(!publication)return {ComponentViewportSessionStatus::PresentationFailed,{}};
    if(failurePoint==ComponentViewportSessionFailurePoint::Scene)
      return {ComponentViewportSessionStatus::SceneFailed,{}};
    const auto handle=candidate->coordinator_.active();
    const auto snapshot=handle.snapshotIfCurrent();
    if(!snapshot||!populateStates(*snapshot,&candidate->components_))
      return {ComponentViewportSessionStatus::InvalidProduct,{}};
    if(!candidate->sceneAdapter_.publish(handle))
      return {ComponentViewportSessionStatus::SceneFailed,{}};
    candidate->scene_=candidate->sceneAdapter_.active();
    if(!candidate->scene_||!candidate->scene_->isCurrent())
      return {ComponentViewportSessionStatus::SceneFailed,{}};
    if(failurePoint==ComponentViewportSessionFailurePoint::Picking)
      return {ComponentViewportSessionStatus::PickingFailed,{}};
    if(!candidate->picking_.publish(candidate->scene_))
      return {ComponentViewportSessionStatus::PickingFailed,{}};
    if(failurePoint==ComponentViewportSessionFailurePoint::Selection||
       failurePoint==ComponentViewportSessionFailurePoint::FinalPreparation)
      return {ComponentViewportSessionStatus::PreparationFailed,{}};
    if(!handle.isCurrent()||!candidate->scene_->isCurrent())
      return {ComponentViewportSessionStatus::PreparationFailed,{}};
    return {ComponentViewportSessionStatus::Prepared,std::move(candidate)};
  }catch(const std::bad_alloc&){
    return {ComponentViewportSessionStatus::ResourceFailure,{}};
  }catch(...){
    return {ComponentViewportSessionStatus::PreparationFailed,{}};
  }
}

std::optional<ComponentInstanceSelection> ComponentInstanceSelection::create(
    const ComponentScenePublicationHandle& publication,
    const ComponentInstanceAddress& component) noexcept {
  if(!ComponentInstanceHighlightTarget::create(publication,component))
    return std::nullopt;
  return ComponentInstanceSelection(publication,component);
}

bool ComponentInstanceSelection::isCurrentFor(
    const ComponentScenePublicationHandle& publication) const noexcept {
  return publication_.isSameCurrentPublication(publication)&&
         ComponentInstanceHighlightTarget::create(publication_,component_).has_value();
}

ComponentViewportSession::PrepareResult ComponentViewportSession::prepare(
    const ProductDocument& product,ProductPresentationEpoch epoch,
    AssemblyMeshProfile profile,
    ComponentViewportSessionFailurePoint failurePoint) noexcept {
  return prepareImpl(product, epoch, profile, failurePoint, nullptr, nullptr);
}

ComponentViewportSession::PrepareResult
ComponentViewportSession::prepareProjectedIdentity(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile profile,
    const ComponentIdentityTransition& transition,
    ComponentViewportSessionFailurePoint failurePoint) noexcept {
  return prepareImpl(product, epoch, profile, failurePoint, nullptr,
                     &transition);
}

ComponentViewportSession::PrepareResult
ComponentViewportSession::prepareSameEpochProjection(
    const ProductDocument& product,const ComponentViewportSession& incumbent,
    AssemblyMeshProfile profile,
    ComponentViewportSessionFailurePoint failurePoint) noexcept {
  if(!incumbent.scene_||!incumbent.scene_->isCurrent())
    return {ComponentViewportSessionStatus::PreparationFailed,{}};
  return prepareImpl(product,incumbent.epoch_,profile,failurePoint,&incumbent,
                     nullptr);
}

ComponentViewportSession::~ComponentViewportSession() noexcept {
  clearInteraction(); picking_.clear(); scene_.reset(); sceneAdapter_.clear();
  static_cast<void>(coordinator_.clear());
}

ViewportCameraState ComponentViewportSession::camera(
    float yaw,float pitch,float zoom,QPointF pan,QSize logicalSize) const noexcept {
  Point3d center{};double extent=1.0;
  if(scene_&&scene_->worldBounds()){
    center=scene_->worldBounds()->center();
    extent=std::max(1.0,scene_->worldBounds()->diagonal()*3.0);
  }
  return {yaw,pitch,zoom,pan,logicalSize,1.0F,center,extent};
}

ComponentViewportFitResult ComponentViewportSession::fit(
    QSize logicalSize,float yaw,float pitch,double fraction) const noexcept {
  return fitComponentViewport(scene_,logicalSize,yaw,pitch,fraction);
}

ComponentFaceRayQueryResult ComponentViewportSession::faceAt(
    const ViewportCameraState& camera,QPointF pixel) const noexcept {
  return picking_.faceAt(camera,pixel);
}
ComponentEdgeRayQueryResult ComponentViewportSession::edgeAt(
    const ViewportCameraState& camera,QPointF pixel,double radius) const noexcept {
  return picking_.edgeAt(camera,pixel,radius);
}

const ComponentInstanceSessionState* ComponentViewportSession::findComponent(
    const ComponentInstanceAddress& address) const noexcept {
  const auto found=std::find_if(components_.begin(),components_.end(),
      [&address](const auto& candidate){return candidate.address==address;});
  return found==components_.end()?nullptr:&*found;
}

std::optional<ComponentInstanceHighlightTarget>
ComponentViewportSession::highlightFor(
    const ComponentInstanceSelection& selection) const {
  const auto* state=findComponent(selection.component());
  if(!state||!state->effectivelyVisible||!scene_||!scene_->isCurrent())
    return std::nullopt;
  auto target=ComponentInstanceHighlightTarget::create(
      selection.publication(),selection.component());
  if(!target||target->visibleBodies(*scene_).empty())return std::nullopt;
  return target;
}

bool ComponentViewportSession::setSelectedComponent(
    const std::optional<ComponentInstanceSelection>& selection) noexcept {
  if(!selection){selected_.reset();selectedHighlight_.reset();return true;}
  if(!scene_||!selection->isCurrentFor(scene_->publicationHandle())||
     !findComponent(selection->component()))return false;
  try{auto highlight=highlightFor(*selection);selected_=*selection;
      selectedHighlight_=std::move(highlight);return true;}catch(...){return false;}
}

bool ComponentViewportSession::setHoveredComponent(
    const std::optional<ComponentInstanceSelection>& selection) noexcept {
  if(!selection){hovered_.reset();hoveredHighlight_.reset();return true;}
  if(!scene_||!selection->isCurrentFor(scene_->publicationHandle())||
     !findComponent(selection->component()))return false;
  try{auto highlight=highlightFor(*selection);hovered_=*selection;
      hoveredHighlight_=std::move(highlight);return true;}catch(...){return false;}
}

void ComponentViewportSession::clearInteraction() noexcept {
  selected_.reset();hovered_.reset();selectedHighlight_.reset();hoveredHighlight_.reset();
}

}  // namespace solidar
