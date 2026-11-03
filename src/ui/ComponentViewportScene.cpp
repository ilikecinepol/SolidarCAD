#include "ui/ComponentViewportScene.h"

#include <TopoDS_Shape.hxx>

#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "ui/internal/ViewportScenePreparation.h"

namespace solidar {
namespace {

std::optional<BodyMeshKey> renderKey(
    const ComponentMeshSourceKey& source) noexcept {
  return detail::viewportBodyMeshKey(
      source.epoch, source.definitionId, source.bodyId, source.featureId,
      source.shapeRevision, source.shapeIdentity, source.meshProfile);
}

ComponentViewportSceneResult failure(ComponentViewportSceneStatus status,
                                     GeometryFailure geometry = {}) noexcept {
  return {status, std::move(geometry), {}};
}

}  // namespace

ComponentViewportSceneResult ComponentViewportSceneAdapter::publish(
    const ComponentScenePublicationHandle& handle) noexcept {
  ++diagnostics_.publicationAttempts;
  const auto fail = [this](ComponentViewportSceneStatus status,
                           GeometryFailure geometry = {}) {
    ++diagnostics_.publicationFailures;
    if (status == ComponentViewportSceneStatus::StalePublication)
      ++diagnostics_.staleRejections;
    return failure(status, std::move(geometry));
  };

  try {
    if (!handle.isCurrent())
      return fail(ComponentViewportSceneStatus::StalePublication);
    const auto snapshot = handle.snapshotIfCurrent();
    if (!snapshot || !snapshot->epoch().isValid() ||
        !snapshot->meshProfile().isValid() ||
        snapshot->graphTraversal().visits.empty())
      return fail(ComponentViewportSceneStatus::InvalidSnapshot);
    const DefinitionId rootDefinitionId =
        snapshot->graphTraversal().visits.front().definitionId;
    if (!rootDefinitionId.isValid() ||
        !snapshot->graphTraversal().visits.front().rootInstance())
      return fail(ComponentViewportSceneStatus::InvalidSnapshot);
    if (!handle.isCurrent())
      return fail(ComponentViewportSceneStatus::StalePublication);

    struct VisibleSource final {
      const ComponentPresentationSource* presentation{};
      BodyMeshKey key;
    };
    std::vector<VisibleSource> visibleSources;
    std::vector<const ComponentPresentationInstance*> visibleInstances;
    visibleInstances.reserve(snapshot->instances().size());

    std::unordered_map<BodyMeshKey, const ComponentPresentationSource*,
                       BodyMeshKeyHash>
        sourcesByKey;
    sourcesByKey.reserve(snapshot->sources().size());
    for (const auto& source : snapshot->sources()) {
      const auto key = renderKey(source.key());
      if (!key || source.key().epoch != snapshot->epoch() ||
          source.key().meshProfile != snapshot->meshProfile() ||
          !source.key().definitionId.isValid() ||
          source.key().bodyId == kInvalidBodyId ||
          source.key().featureId == kInvalidFeatureId ||
          source.key().shapeRevision == kInvalidShapeRevision ||
          !source.shape() || source.shape()->IsNull() ||
          source.key().shapeIdentity != source.shape().get())
        return fail(ComponentViewportSceneStatus::InvalidSource);
      const auto [position, inserted] = sourcesByKey.emplace(*key, &source);
      static_cast<void>(position);
      if (!inserted)
        return fail(ComponentViewportSceneStatus::DuplicateSource);
    }

    std::unordered_set<BodyMeshKey, BodyMeshKeyHash> visibleKeys;
    visibleKeys.reserve(snapshot->sources().size());
    for (const auto& instance : snapshot->instances()) {
      const auto key = renderKey(instance.sourceKey());
      const auto& identity = instance.bodyIdentity();
      const auto& address = identity.component;
      if (!key || instance.sourceKey().epoch != snapshot->epoch() ||
          instance.sourceKey().meshProfile != snapshot->meshProfile() ||
          address.epoch != snapshot->epoch() ||
          address.rootDefinitionId != rootDefinitionId ||
          address.definitionId != instance.sourceKey().definitionId ||
          identity.bodyId != instance.sourceKey().bodyId ||
          (address.rootInstance() && address.definitionId != rootDefinitionId) ||
          (!address.rootInstance() && address.occurrencePath->depth() == 0))
        return fail(ComponentViewportSceneStatus::InvalidInstance);
      const auto source = sourcesByKey.find(*key);
      if (source == sourcesByKey.end() ||
          source->second->key().shapeIdentity !=
              instance.sourceKey().shapeIdentity)
        return fail(ComponentViewportSceneStatus::InvalidInstance);
      if (!instance.effectivelyVisible()) continue;
      visibleInstances.push_back(&instance);
      if (visibleKeys.insert(*key).second)
        visibleSources.push_back({source->second, *key});
    }

    std::vector<detail::ViewportSceneSourceInput> sourceInputs;
    sourceInputs.reserve(visibleSources.size());
    for (const auto& source : visibleSources)
      sourceInputs.push_back({source.key, source.presentation->shape(),
                              source.presentation->topologyLineage()});
    std::vector<detail::ViewportSceneInstanceInput> instanceInputs;
    instanceInputs.reserve(visibleInstances.size());
    for (const auto* instance : visibleInstances) {
      const auto key = renderKey(instance->sourceKey());
      if (!key) return fail(ComponentViewportSceneStatus::InvalidInstance);
      instanceInputs.push_back(
          {*key, instance->worldTransform(), instance->grounded()});
    }

    auto scenePreparation = detail::prepareViewportScene(
        sourceInputs, instanceInputs, cache_,
        [&handle] { return handle.isCurrent(); }, failMeshBuildCountdown_,
        {&diagnostics_.meshBuildAttempts, &diagnostics_.meshBuildSuccesses,
         &diagnostics_.meshBuildFailures});
    if (!scenePreparation) {
      switch (scenePreparation.status) {
        case detail::ViewportScenePreparationStatus::StalePublication:
          return fail(ComponentViewportSceneStatus::StalePublication);
        case detail::ViewportScenePreparationStatus::InvalidBounds:
          return fail(ComponentViewportSceneStatus::InvalidBounds);
        case detail::ViewportScenePreparationStatus::MeshBuildFailed:
          return fail(ComponentViewportSceneStatus::MeshBuildFailed,
                      std::move(scenePreparation.meshFailure));
        case detail::ViewportScenePreparationStatus::PreparationFailed:
          return fail(ComponentViewportSceneStatus::PreparationFailed);
        case detail::ViewportScenePreparationStatus::Prepared:
          break;
      }
    }

    std::vector<ComponentViewportSceneSource> preparedSources;
    preparedSources.reserve(scenePreparation.sources.size());
    for (auto& source : scenePreparation.sources)
      preparedSources.push_back(
          {source.key, std::move(source.shape),
           std::move(source.topologyLineage), std::move(source.mesh)});
    std::vector<ComponentViewportSceneInstance> preparedInstances;
    preparedInstances.reserve(scenePreparation.instances.size());
    for (std::size_t index = 0; index < scenePreparation.instances.size();
         ++index) {
      auto& instance = scenePreparation.instances[index];
      preparedInstances.push_back(
          {instance.sourceKey, std::move(instance.mesh),
           instance.localToWorld, visibleInstances[index]->bodyIdentity(),
           instance.grounded, instance.worldBounds});
    }

    auto preparedScene = std::shared_ptr<const ComponentViewportScene>(
        new ComponentViewportScene(
            handle, snapshot, std::move(preparedSources),
            std::move(preparedInstances),
            std::move(scenePreparation.worldBounds)));
    if (!handle.isCurrent())
      return fail(ComponentViewportSceneStatus::StalePublication);

    cache_ = std::move(scenePreparation.cache);
    active_.swap(preparedScene);
    ++diagnostics_.publicationSuccesses;
    diagnostics_.residentSources = cache_.entryCount();
    diagnostics_.activeSources = active_->sources().size();
    diagnostics_.activeInstances = active_->instances().size();
    diagnostics_.activeGeneration = active_->generation();
    return {ComponentViewportSceneStatus::Published, {},
            active_->generation()};
  } catch (const std::exception&) {
    return fail(ComponentViewportSceneStatus::PreparationFailed);
  } catch (...) {
    return fail(ComponentViewportSceneStatus::PreparationFailed);
  }
}

bool ComponentViewportSceneAdapter::seedReusableSourcesFrom(
    const ComponentViewportSceneAdapter& source) noexcept {
  if (this == &source) return true;
  try {
    cache_ = source.cache_;
    diagnostics_.residentSources = cache_.entryCount();
    diagnostics_.activeSources = 0;
    diagnostics_.activeInstances = 0;
    diagnostics_.activeGeneration = {};
    active_.reset();
    return true;
  } catch (...) {
    cache_.clear();
    active_.reset();
    diagnostics_.residentSources = 0;
    diagnostics_.activeSources = 0;
    diagnostics_.activeInstances = 0;
    diagnostics_.activeGeneration = {};
    return false;
  }
}

void ComponentViewportSceneAdapter::clear() noexcept {
  active_.reset();
  cache_.clear();
  diagnostics_.residentSources = 0;
  diagnostics_.activeSources = 0;
  diagnostics_.activeInstances = 0;
  diagnostics_.activeGeneration = {};
}

}  // namespace solidar
