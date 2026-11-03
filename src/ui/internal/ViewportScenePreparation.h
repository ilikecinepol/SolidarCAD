#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "model/GeometryOperation.h"
#include "model/RigidTransform3d.h"
#include "presentation/AssemblyPresentation.h"
#include "ui/BodyRenderMesh.h"

class TopoDS_Shape;

namespace solidar {

class TopologyLineage;

namespace detail {

[[nodiscard]] std::optional<BodyMeshKey> viewportBodyMeshKey(
    ProductPresentationEpoch epoch, DefinitionId definitionId, BodyId bodyId,
    FeatureId featureId, ShapeRevision shapeRevision,
    const TopoDS_Shape* shapeIdentity,
    AssemblyMeshProfile meshProfile) noexcept;

[[nodiscard]] std::optional<Bounds3d> transformedViewportBounds(
    const Bounds3d& localBounds,
    const RigidTransform3d& localToWorld) noexcept;

struct ViewportSceneSourceInput final {
  BodyMeshKey key;
  std::shared_ptr<const TopoDS_Shape> shape;
  std::shared_ptr<const TopologyLineage> topologyLineage;
};

struct ViewportSceneInstanceInput final {
  BodyMeshKey sourceKey;
  RigidTransform3d localToWorld{RigidTransform3d::identity()};
  bool grounded{};
};

struct PreparedViewportSceneSource final {
  BodyMeshKey key;
  std::shared_ptr<const TopoDS_Shape> shape;
  std::shared_ptr<const TopologyLineage> topologyLineage;
  std::shared_ptr<const BodyRenderMesh> mesh;
};

struct PreparedViewportSceneInstance final {
  BodyMeshKey sourceKey;
  std::shared_ptr<const BodyRenderMesh> mesh;
  RigidTransform3d localToWorld{RigidTransform3d::identity()};
  bool grounded{};
  Bounds3d worldBounds;
};

enum class ViewportScenePreparationStatus {
  Prepared,
  StalePublication,
  InvalidBounds,
  MeshBuildFailed,
  PreparationFailed,
};

struct ViewportSceneMeshCounters final {
  std::uint64_t* attempts{};
  std::uint64_t* successes{};
  std::uint64_t* failures{};
};

struct PreparedViewportScene final {
  ViewportScenePreparationStatus status{
      ViewportScenePreparationStatus::PreparationFailed};
  GeometryFailure meshFailure;
  BodyRenderMeshCache cache;
  std::vector<PreparedViewportSceneSource> sources;
  std::vector<PreparedViewportSceneInstance> instances;
  std::optional<Bounds3d> worldBounds;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ViewportScenePreparationStatus::Prepared;
  }
};

using ViewportSceneCurrentProbe = std::function<bool()>;

// Identity-neutral mesh/cache/bounds transaction shared by Assembly and
// Component scene adapters. Callers validate typed source/instance identity
// before entering this boundary and attach it again after success.
[[nodiscard]] PreparedViewportScene prepareViewportScene(
    const std::vector<ViewportSceneSourceInput>& sources,
    const std::vector<ViewportSceneInstanceInput>& instances,
    const BodyRenderMeshCache& incumbentCache,
    const ViewportSceneCurrentProbe& current,
    std::size_t& failMeshBuildCountdown,
    ViewportSceneMeshCounters counters) noexcept;

}  // namespace detail
}  // namespace solidar
