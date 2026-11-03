#include "ui/AssemblyViewportScene.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "ui/internal/ViewportScenePreparation.h"

namespace solidar {
namespace {

std::optional<BodyMeshKey> renderKey(
    const AssemblyMeshSourceKey& source) noexcept {
  return detail::viewportBodyMeshKey(
      source.epoch, source.definitionId, source.bodyId, source.featureId,
      source.shapeRevision, source.shapeIdentity, source.meshProfile);
}

AssemblyViewportSceneResult failure(AssemblyViewportSceneStatus status,
                                    GeometryFailure geometry = {}) noexcept {
  return {status, std::move(geometry), {}};
}

}  // namespace

AssemblyViewportSceneResult AssemblyViewportSceneAdapter::publish(
    const AssemblyScenePublicationHandle& handle) noexcept {
  ++diagnostics_.publicationAttempts;
  const auto fail = [this](AssemblyViewportSceneStatus status,
                           GeometryFailure geometry = {}) {
    ++diagnostics_.publicationFailures;
    if (status == AssemblyViewportSceneStatus::StalePublication)
      ++diagnostics_.staleRejections;
    return failure(status, std::move(geometry));
  };

  try {
    if (!handle.isCurrent())
      return fail(AssemblyViewportSceneStatus::StalePublication);
    const auto snapshot = handle.snapshotIfCurrent();
    if (!snapshot || !snapshot->epoch().isValid() ||
        !snapshot->meshProfile().isValid())
      return fail(AssemblyViewportSceneStatus::InvalidSnapshot);
    if (!handle.isCurrent())
      return fail(AssemblyViewportSceneStatus::StalePublication);

    struct VisibleSource final {
      const AssemblyPresentationSource* presentation{};
      BodyMeshKey key;
    };
    std::vector<VisibleSource> visibleSources;
    std::vector<const AssemblyPresentationInstance*> visibleInstances;
    visibleInstances.reserve(snapshot->instances().size());

    // Validate the complete source table even when an instance is hidden.
    std::unordered_map<BodyMeshKey, const AssemblyPresentationSource*,
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
        return fail(AssemblyViewportSceneStatus::InvalidSource);
      const auto [position, inserted] = sourcesByKey.emplace(*key, &source);
      static_cast<void>(position);
      if (!inserted)
        return fail(AssemblyViewportSceneStatus::DuplicateSource);
    }

    std::unordered_set<BodyMeshKey, BodyMeshKeyHash> visibleKeys;
    visibleKeys.reserve(snapshot->sources().size());
    for (const auto& instance : snapshot->instances()) {
      const auto key = renderKey(instance.sourceKey());
      if (!key || instance.sourceKey().epoch != snapshot->epoch() ||
          instance.sourceKey().meshProfile != snapshot->meshProfile() ||
          instance.occurrenceIdentity().epoch != snapshot->epoch() ||
          !instance.occurrenceIdentity().rootAssemblyDefinitionId.isValid() ||
          instance.occurrencePath().depth() == 0)
        return fail(AssemblyViewportSceneStatus::InvalidInstance);
      const auto source = sourcesByKey.find(*key);
      if (source == sourcesByKey.end() ||
          source->second->key().shapeIdentity !=
              instance.sourceKey().shapeIdentity)
        return fail(AssemblyViewportSceneStatus::InvalidInstance);
      if (!instance.effectivelyVisible()) continue;
      visibleInstances.push_back(&instance);
      if (visibleKeys.insert(*key).second)
        visibleSources.push_back({source->second, *key});
    }

    std::vector<detail::ViewportSceneSourceInput> sourceInputs;
    sourceInputs.reserve(visibleSources.size());
    for (const VisibleSource& source : visibleSources) {
      sourceInputs.push_back({source.key, source.presentation->shape(),
                              source.presentation->topologyLineage()});
    }
    std::vector<detail::ViewportSceneInstanceInput> instanceInputs;
    instanceInputs.reserve(visibleInstances.size());
    for (const auto* instance : visibleInstances) {
      const auto key = renderKey(instance->sourceKey());
      if (!key)
        return fail(AssemblyViewportSceneStatus::InvalidInstance);
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
          return fail(AssemblyViewportSceneStatus::StalePublication);
        case detail::ViewportScenePreparationStatus::InvalidBounds:
          return fail(AssemblyViewportSceneStatus::InvalidBounds);
        case detail::ViewportScenePreparationStatus::MeshBuildFailed:
          return fail(AssemblyViewportSceneStatus::MeshBuildFailed,
                      std::move(scenePreparation.meshFailure));
        case detail::ViewportScenePreparationStatus::PreparationFailed:
          return fail(AssemblyViewportSceneStatus::PreparationFailed);
        case detail::ViewportScenePreparationStatus::Prepared:
          break;
      }
    }
    std::vector<AssemblyViewportSceneSource> preparedSources;
    preparedSources.reserve(scenePreparation.sources.size());
    for (auto& source : scenePreparation.sources)
      preparedSources.push_back(
          {source.key, std::move(source.shape),
           std::move(source.topologyLineage), std::move(source.mesh)});
    std::vector<AssemblyViewportSceneInstance> preparedInstances;
    preparedInstances.reserve(scenePreparation.instances.size());
    for (std::size_t index = 0; index < scenePreparation.instances.size();
         ++index) {
      auto& instance = scenePreparation.instances[index];
      preparedInstances.push_back(
          {instance.sourceKey, std::move(instance.mesh),
           instance.localToWorld,
           visibleInstances[index]->occurrenceIdentity(), instance.grounded,
           instance.worldBounds});
    }

    auto preparedScene = std::shared_ptr<const AssemblyViewportScene>(
        new AssemblyViewportScene(handle, snapshot, std::move(preparedSources),
                                  std::move(preparedInstances),
                                  std::move(scenePreparation.worldBounds)));
    if (!handle.isCurrent())
      return fail(AssemblyViewportSceneStatus::StalePublication);

    cache_ = std::move(scenePreparation.cache);
    active_.swap(preparedScene);
    ++diagnostics_.publicationSuccesses;
    diagnostics_.residentSources = cache_.entryCount();
    diagnostics_.activeSources = active_->sources().size();
    diagnostics_.activeInstances = active_->instances().size();
    diagnostics_.activeGeneration = active_->generation();
    return {AssemblyViewportSceneStatus::Published, {}, active_->generation()};
  } catch (const std::exception&) {
    return fail(AssemblyViewportSceneStatus::PreparationFailed);
  } catch (...) {
    return fail(AssemblyViewportSceneStatus::PreparationFailed);
  }
}

bool AssemblyViewportSceneAdapter::seedReusableSourcesFrom(
    const AssemblyViewportSceneAdapter& source) noexcept {
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

void AssemblyViewportSceneAdapter::clear() noexcept {
  active_.reset();
  cache_.clear();
  diagnostics_.residentSources = 0;
  diagnostics_.activeSources = 0;
  diagnostics_.activeInstances = 0;
  diagnostics_.activeGeneration = {};
}

}  // namespace solidar
