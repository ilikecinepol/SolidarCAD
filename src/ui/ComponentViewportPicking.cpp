#include "ui/ComponentViewportPicking.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <numbers>
#include <utility>

#include "ui/RigidTransform3dOps.h"
#include "ui/internal/ViewportPickingRaycast.h"

namespace solidar {
namespace {

bool addressExists(const ComponentPresentationSnapshot& snapshot,
                   const ComponentInstanceAddress& address) noexcept {
  if(address.epoch!=snapshot.epoch()||snapshot.graphTraversal().visits.empty()||
     address.rootDefinitionId!=snapshot.graphTraversal().visits.front().definitionId)
    return false;
  return std::any_of(snapshot.graphTraversal().visits.begin(),
      snapshot.graphTraversal().visits.end(),[&](const auto& visit){
        return visit.definitionId==address.definitionId&&visit.path==address.occurrencePath;
      });
}

ComponentViewportPickingPublishStatus mapStatus(
    detail::ViewportPickingTopologyStatus status) noexcept {
  using S=detail::ViewportPickingTopologyStatus;
  switch(status){
    case S::Prepared:return ComponentViewportPickingPublishStatus::Published;
    case S::StalePublication:return ComponentViewportPickingPublishStatus::StalePublication;
    case S::InvalidSource:return ComponentViewportPickingPublishStatus::InvalidSource;
    case S::DuplicateSource:return ComponentViewportPickingPublishStatus::DuplicateSource;
    case S::TopologyBuildFailed:return ComponentViewportPickingPublishStatus::TopologyBuildFailed;
    case S::InvalidTopologyMapping:return ComponentViewportPickingPublishStatus::InvalidTopologyMapping;
    case S::ResourceFailure:return ComponentViewportPickingPublishStatus::PreparationFailed;
  }
  return ComponentViewportPickingPublishStatus::PreparationFailed;
}

}  // namespace

struct ComponentViewportPicking::PublishedScene final {
  struct Instance final {
    std::shared_ptr<const detail::ViewportPickingTopologySource> source;
    std::array<double,16> matrix{};
    ComponentBodyIdentity identity;
    Bounds3d worldBounds;
    std::size_t ordinal{};
  };
  std::shared_ptr<const ComponentViewportScene> scene;
  ComponentScenePublicationHandle publication;
  std::vector<Instance> instances;
  std::vector<detail::ViewportPickingRayInstance> rayInstances;
};

std::optional<ComponentInstanceHighlightTarget>
ComponentInstanceHighlightTarget::create(
    const ComponentScenePublicationHandle& publication,
    const ComponentInstanceAddress& component) noexcept {
  const auto snapshot=publication.snapshotIfCurrent();
  if(!snapshot||!addressExists(*snapshot,component)) return std::nullopt;
  return ComponentInstanceHighlightTarget(publication,component);
}

bool ComponentInstanceHighlightTarget::isCurrentFor(
    const ComponentViewportScene& scene) const noexcept {
  return scene.isCurrent()&&publication_.isSameCurrentPublication(
      scene.publicationHandle())&&scene.snapshot()&&
      addressExists(*scene.snapshot(),component_);
}

std::vector<ComponentBodyIdentity>
ComponentInstanceHighlightTarget::visibleBodies(
    const ComponentViewportScene& scene) const {
  std::vector<ComponentBodyIdentity> result;
  if(!isCurrentFor(scene)) return result;
  for(const auto& instance:scene.instances())
    if(instance.bodyIdentity.component==component_)
      result.push_back(instance.bodyIdentity);
  return result;
}

std::optional<ComponentInstanceHighlightTarget>
ComponentFaceRayHit::highlightTarget() const noexcept {
  return ComponentInstanceHighlightTarget::create(publication,address.body.component);
}
std::optional<ComponentInstanceHighlightTarget>
ComponentEdgeRayHit::highlightTarget() const noexcept {
  return ComponentInstanceHighlightTarget::create(publication,address.body.component);
}

ComponentViewportPickingPublishResult ComponentViewportPicking::publish(
    const std::shared_ptr<const ComponentViewportScene>& scene) noexcept {
  ++diagnostics_.publicationAttempts;
  const auto fail=[this](ComponentViewportPickingPublishStatus status){
    ++diagnostics_.publicationFailures;
    if(status==ComponentViewportPickingPublishStatus::StalePublication)
      ++diagnostics_.stalePublicationRejections;
    return ComponentViewportPickingPublishResult{status,{}};
  };
  try {
    if(!scene) return fail(ComponentViewportPickingPublishStatus::InvalidScene);
    if(!scene->isCurrent()) return fail(ComponentViewportPickingPublishStatus::StalePublication);
    if(!scene->snapshot()) return fail(ComponentViewportPickingPublishStatus::InvalidScene);
    std::vector<detail::ViewportPickingTopologyInput> inputs;
    inputs.reserve(scene->sources().size());
    for(const auto& source:scene->sources())
      inputs.push_back({source.key,source.shape,source.topologyLineage,source.mesh});
    detail::ViewportPickingTopologyCounters counters{
      &diagnostics_.topologySourceBuildAttempts,
      &diagnostics_.topologySourceBuildSuccesses,
      &diagnostics_.topologySourceBuildFailures,
      &diagnostics_.topologySourceReuses,
      &diagnostics_.v2FaceReferences,&diagnostics_.v1FaceFallbacks,
      &diagnostics_.v2EdgeReferences,&diagnostics_.v1EdgeFallbacks};
    auto topology=detail::prepareViewportPickingTopology(
      inputs,cache_,[&scene]{return scene->isCurrent();},
      failTopologyBuildCountdown_,failMappingCountdown_,counters);
    if(!topology) return fail(mapStatus(topology.status));
    auto pending=std::make_shared<PublishedScene>();
    pending->scene=scene; pending->publication=scene->publicationHandle();
    pending->instances.reserve(scene->instances().size());
    pending->rayInstances.reserve(scene->instances().size());
    for(std::size_t ordinal=0;ordinal<scene->instances().size();++ordinal){
      const auto& instance=scene->instances()[ordinal];
      if(!scene->isCurrent()) return fail(ComponentViewportPickingPublishStatus::StalePublication);
      const auto source=topology.activeSources.find(instance.sourceKey);
      const auto matrix=rigidTransformMatrix(instance.localToWorld);
      if(source==topology.activeSources.end()||!source->second||
         source->second->mesh!=instance.mesh||!matrix||
         instance.bodyIdentity.bodyId!=instance.sourceKey.bodyId||
         !addressExists(*scene->snapshot(),instance.bodyIdentity.component))
        return fail(ComponentViewportPickingPublishStatus::InvalidScene);
      pending->instances.push_back({source->second,*matrix,instance.bodyIdentity,
                                    instance.worldBounds,ordinal});
      pending->rayInstances.push_back(
          {source->second,*matrix,instance.worldBounds,ordinal});
    }
    if(!scene->isCurrent()||!pending->publication.isSameCurrentPublication(
        scene->publicationHandle()))
      return fail(ComponentViewportPickingPublishStatus::StalePublication);
    cache_.swap(topology.cache); active_=std::move(pending);
    ++diagnostics_.publicationSuccesses;
    diagnostics_.residentTopologySources=cache_.size();
    diagnostics_.activeSources=topology.activeSources.size();
    diagnostics_.activeInstances=active_->instances.size();
    diagnostics_.activeGeneration=scene->generation();
    return {ComponentViewportPickingPublishStatus::Published,scene->generation()};
  }catch(...){return fail(ComponentViewportPickingPublishStatus::PreparationFailed);}
}

ComponentFaceRayQueryResult ComponentViewportPicking::faceAt(
    const ViewportCameraState& camera,QPointF pixel) const noexcept {
  ++diagnostics_.faceQueries;
  const auto fail=[this](ComponentViewportPickingQueryStatus status){
    ++diagnostics_.queryFailures;
    if(status==ComponentViewportPickingQueryStatus::StalePublication)
      ++diagnostics_.staleQueryRejections;
    return ComponentFaceRayQueryResult{status,{}};
  };
  const auto published=active_;
  if(!published) return fail(ComponentViewportPickingQueryStatus::NoScene);
  const auto result=detail::viewportFaceAt(
      published->rayInstances,published->scene->worldBounds(),camera,pixel,
      [&published]{return published->scene&&published->scene->isCurrent()&&
        published->publication.isSameCurrentPublication(
            published->scene->publicationHandle());},
      {&diagnostics_.broadPhaseCandidates,&diagnostics_.triangleTests,
       &diagnostics_.edgeSegmentTests,&diagnostics_.occluderTests});
  using Shared=detail::ViewportPickingRayStatus;
  if(result.status==Shared::Miss)return {ComponentViewportPickingQueryStatus::Miss,{}};
  if(result.status!=Shared::Hit||!result.hit)
    return fail(result.status==Shared::StalePublication
                    ? ComponentViewportPickingQueryStatus::StalePublication
                    : ComponentViewportPickingQueryStatus::InvalidInput);
  if(result.hit->instanceOrdinal>=published->instances.size())
    return fail(ComponentViewportPickingQueryStatus::InvalidInput);
  const auto& instance=published->instances[result.hit->instanceOrdinal];
  ComponentFaceRayHit hit{{instance.identity,result.hit->face},
      result.hit->worldPoint,result.hit->rayParameter,result.hit->cameraDepth,
      published->publication};
  return {ComponentViewportPickingQueryStatus::Hit,std::move(hit)};
}

ComponentEdgeRayQueryResult ComponentViewportPicking::edgeAt(
    const ViewportCameraState& camera,QPointF pixel,double radiusPixels) const noexcept {
  ++diagnostics_.edgeQueries;
  const auto fail=[this](ComponentViewportPickingQueryStatus status){
    ++diagnostics_.queryFailures;
    if(status==ComponentViewportPickingQueryStatus::StalePublication)++diagnostics_.staleQueryRejections;
    return ComponentEdgeRayQueryResult{status,{}};
  };
  const auto published=active_;
  if(!published)return fail(ComponentViewportPickingQueryStatus::NoScene);
  const auto result=detail::viewportEdgeAt(
      published->rayInstances,published->scene->worldBounds(),camera,pixel,
      radiusPixels,[&published]{return published->scene&&
        published->scene->isCurrent()&&
        published->publication.isSameCurrentPublication(
            published->scene->publicationHandle());},
      {&diagnostics_.broadPhaseCandidates,&diagnostics_.triangleTests,
       &diagnostics_.edgeSegmentTests,&diagnostics_.occluderTests});
  using Shared=detail::ViewportPickingRayStatus;
  if(result.status==Shared::Miss)return {ComponentViewportPickingQueryStatus::Miss,{}};
  if(result.status!=Shared::Hit||!result.hit)
    return fail(result.status==Shared::StalePublication
                    ? ComponentViewportPickingQueryStatus::StalePublication
                    : ComponentViewportPickingQueryStatus::InvalidInput);
  if(result.hit->instanceOrdinal>=published->instances.size())
    return fail(ComponentViewportPickingQueryStatus::InvalidInput);
  const auto& instance=published->instances[result.hit->instanceOrdinal];
  ComponentEdgeRayHit hit{{instance.identity,result.hit->edge},
      result.hit->worldPoint,result.hit->rayParameter,result.hit->cameraDepth,
      result.hit->distance,published->publication};
  return {ComponentViewportPickingQueryStatus::Hit,std::move(hit)};
}

bool ComponentViewportPicking::seedReusableSourcesFrom(
    const ComponentViewportPicking& source) noexcept {
  if(this==&source)return true;
  try{cache_=source.cache_;active_.reset();diagnostics_.residentTopologySources=cache_.size();diagnostics_.activeSources=0;diagnostics_.activeInstances=0;diagnostics_.activeGeneration={};return true;}
  catch(...){clear();return false;}
}
void ComponentViewportPicking::swapPrepared(ComponentViewportPicking& prepared) noexcept {
  cache_.swap(prepared.cache_);active_.swap(prepared.active_);std::swap(diagnostics_,prepared.diagnostics_);
  std::swap(failTopologyBuildCountdown_,prepared.failTopologyBuildCountdown_);std::swap(failMappingCountdown_,prepared.failMappingCountdown_);
}
void ComponentViewportPicking::clear() noexcept {
  active_.reset();cache_.clear();diagnostics_.residentTopologySources=0;diagnostics_.activeSources=0;diagnostics_.activeInstances=0;diagnostics_.activeGeneration={};
}

}  // namespace solidar
