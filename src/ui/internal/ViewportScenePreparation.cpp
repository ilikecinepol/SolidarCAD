#include "ui/internal/ViewportScenePreparation.h"

#include <TopoDS_Shape.hxx>

#include <cmath>
#include <exception>
#include <unordered_map>
#include <utility>

#include "ui/RigidTransform3dOps.h"

namespace solidar::detail {

std::optional<BodyMeshKey> viewportBodyMeshKey(
    ProductPresentationEpoch epoch, DefinitionId definitionId, BodyId bodyId,
    FeatureId featureId, ShapeRevision shapeRevision,
    const TopoDS_Shape* shapeIdentity,
    AssemblyMeshProfile meshProfile) noexcept {
  std::optional<ViewportMeshQuality> quality;
  switch (meshProfile.quality()) {
    case AssemblyMeshQuality::Normal:
      quality = ViewportMeshQuality::Normal;
      break;
    case AssemblyMeshQuality::High:
      quality = ViewportMeshQuality::High;
      break;
    case AssemblyMeshQuality::Invalid:
      return std::nullopt;
  }
  return BodyMeshKey{bodyId,
                     featureId,
                     shapeRevision,
                     shapeIdentity,
                     *quality,
                     epoch.value(),
                     definitionId.value()};
}

std::optional<Bounds3d> transformedViewportBounds(
    const Bounds3d& localBounds,
    const RigidTransform3d& localToWorld) noexcept {
  const auto matrix = rigidTransformMatrix(localToWorld);
  if (!matrix) return std::nullopt;
  std::optional<Bounds3d> result;
  for (const Point3d corner : localBounds.corners()) {
    const auto transformed = transformPoint(*matrix, corner);
    if (!transformed) return std::nullopt;
    const auto pointBounds = Bounds3d::create(*transformed, *transformed);
    if (!pointBounds) return std::nullopt;
    result = result ? result->united(*pointBounds) : pointBounds;
    if (!result) return std::nullopt;
  }
  return result;
}

PreparedViewportScene prepareViewportScene(
    const std::vector<ViewportSceneSourceInput>& sources,
    const std::vector<ViewportSceneInstanceInput>& instances,
    const BodyRenderMeshCache& incumbentCache,
    const ViewportSceneCurrentProbe& current,
    std::size_t& failMeshBuildCountdown,
    ViewportSceneMeshCounters counters) noexcept {
  try {
    PreparedViewportScene result;
    result.cache = incumbentCache;
    result.sources.reserve(sources.size());
    for (const auto& source : sources) {
      if (!current()) {
        result.status = ViewportScenePreparationStatus::StalePublication;
        return result;
      }
      const bool cacheHit = result.cache.contains(source.key);
      if (!cacheHit && counters.attempts) ++*counters.attempts;
      if (!cacheHit && failMeshBuildCountdown > 0 &&
          --failMeshBuildCountdown == 0) {
        result.meshFailure = {GeometryFailureKind::MeshingFailed,
                              "Injected CPU mesh failure"};
        if (counters.failures) ++*counters.failures;
        result.status = ViewportScenePreparationStatus::MeshBuildFailed;
        return result;
      }
      GeometryFailure geometry;
      auto mesh = result.cache.resolve(source.key, *source.shape, &geometry);
      if (!cacheHit) {
        if (mesh) {
          if (counters.successes) ++*counters.successes;
        } else if (counters.failures) {
          ++*counters.failures;
        }
      }
      if (!current()) {
        result.status = ViewportScenePreparationStatus::StalePublication;
        return result;
      }
      if (!mesh) {
        result.meshFailure = std::move(geometry);
        result.status = ViewportScenePreparationStatus::MeshBuildFailed;
        return result;
      }
      result.sources.push_back({source.key, source.shape,
                                source.topologyLineage, std::move(mesh)});
    }

    std::unordered_map<BodyMeshKey, std::shared_ptr<const BodyRenderMesh>,
                       BodyMeshKeyHash>
        meshes;
    meshes.reserve(result.sources.size());
    for (const auto& source : result.sources)
      meshes.emplace(source.key, source.mesh);

    result.instances.reserve(instances.size());
    for (const auto& instance : instances) {
      const auto found = meshes.find(instance.sourceKey);
      if (found == meshes.end() || !found->second->localBounds()) {
        result.status = ViewportScenePreparationStatus::InvalidBounds;
        return result;
      }
      const auto worldBounds = transformedViewportBounds(
          *found->second->localBounds(), instance.localToWorld);
      if (!worldBounds) {
        result.status = ViewportScenePreparationStatus::InvalidBounds;
        return result;
      }
      result.worldBounds = result.worldBounds
                               ? result.worldBounds->united(*worldBounds)
                               : worldBounds;
      if (!result.worldBounds ||
          !std::isfinite(result.worldBounds->diagonal() * 3.0)) {
        result.status = ViewportScenePreparationStatus::InvalidBounds;
        return result;
      }
      result.instances.push_back({instance.sourceKey, found->second,
                                  instance.localToWorld, instance.grounded,
                                  *worldBounds});
    }

    std::vector<BodyMeshKey> activeKeys;
    activeKeys.reserve(result.sources.size());
    for (const auto& source : result.sources) activeKeys.push_back(source.key);
    if (!current()) {
      result.status = ViewportScenePreparationStatus::StalePublication;
      return result;
    }
    result.cache.retainActive(activeKeys, 0);
    if (!current()) {
      result.status = ViewportScenePreparationStatus::StalePublication;
      return result;
    }
    result.status = ViewportScenePreparationStatus::Prepared;
    return result;
  } catch (const std::exception&) {
    return {};
  } catch (...) {
    return {};
  }
}

}  // namespace solidar::detail
