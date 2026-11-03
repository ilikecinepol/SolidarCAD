#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "presentation/ComponentScenePublicationCoordinator.h"
#include "ui/BodyRenderMesh.h"

class TopoDS_Shape;

namespace solidar {

class ComponentViewportSceneAdapterTestAccess;

struct ComponentViewportSceneSource final {
  BodyMeshKey key;
  std::shared_ptr<const TopoDS_Shape> shape;
  std::shared_ptr<const TopologyLineage> topologyLineage;
  std::shared_ptr<const BodyRenderMesh> mesh;
};

struct ComponentViewportSceneInstance final {
  BodyMeshKey sourceKey;
  std::shared_ptr<const BodyRenderMesh> mesh;
  RigidTransform3d localToWorld{RigidTransform3d::identity()};
  ComponentBodyIdentity bodyIdentity;
  bool grounded{};
  Bounds3d worldBounds;
};

class ComponentViewportScene final {
 public:
  [[nodiscard]] ComponentScenePublicationGeneration generation()
      const noexcept {
    return generation_;
  }
  [[nodiscard]] bool isCurrent() const noexcept { return handle_.isCurrent(); }
  [[nodiscard]] const ComponentScenePublicationHandle& publicationHandle()
      const noexcept {
    return handle_;
  }
  [[nodiscard]] const std::shared_ptr<const ComponentPresentationSnapshot>&
  snapshot() const noexcept {
    return snapshot_;
  }
  [[nodiscard]] const std::vector<ComponentViewportSceneSource>& sources()
      const noexcept {
    return sources_;
  }
  [[nodiscard]] const std::vector<ComponentViewportSceneInstance>& instances()
      const noexcept {
    return instances_;
  }
  [[nodiscard]] const std::optional<Bounds3d>& worldBounds() const noexcept {
    return worldBounds_;
  }

 private:
  friend class ComponentViewportSceneAdapter;

  ComponentViewportScene(
      ComponentScenePublicationHandle handle,
      std::shared_ptr<const ComponentPresentationSnapshot> snapshot,
      std::vector<ComponentViewportSceneSource> sources,
      std::vector<ComponentViewportSceneInstance> instances,
      std::optional<Bounds3d> worldBounds) noexcept
      : generation_(handle.generation()),
        handle_(std::move(handle)),
        snapshot_(std::move(snapshot)),
        sources_(std::move(sources)),
        instances_(std::move(instances)),
        worldBounds_(std::move(worldBounds)) {}

  ComponentScenePublicationGeneration generation_;
  ComponentScenePublicationHandle handle_;
  std::shared_ptr<const ComponentPresentationSnapshot> snapshot_;
  std::vector<ComponentViewportSceneSource> sources_;
  std::vector<ComponentViewportSceneInstance> instances_;
  std::optional<Bounds3d> worldBounds_;
};

enum class ComponentViewportSceneStatus : std::uint8_t {
  Published,
  StalePublication,
  InvalidSnapshot,
  InvalidSource,
  DuplicateSource,
  InvalidInstance,
  InvalidBounds,
  MeshBuildFailed,
  PreparationFailed,
};

struct ComponentViewportSceneResult final {
  ComponentViewportSceneStatus status{
      ComponentViewportSceneStatus::PreparationFailed};
  GeometryFailure meshFailure;
  ComponentScenePublicationGeneration generation;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentViewportSceneStatus::Published;
  }
};

struct ComponentViewportSceneDiagnostics final {
  std::uint64_t publicationAttempts{};
  std::uint64_t publicationSuccesses{};
  std::uint64_t publicationFailures{};
  std::uint64_t staleRejections{};
  std::uint64_t meshBuildAttempts{};
  std::uint64_t meshBuildSuccesses{};
  std::uint64_t meshBuildFailures{};
  std::size_t residentSources{};
  std::size_t activeSources{};
  std::size_t activeInstances{};
  ComponentScenePublicationGeneration activeGeneration;
};

class ComponentViewportSceneAdapter final {
 public:
  [[nodiscard]] ComponentViewportSceneResult publish(
      const ComponentScenePublicationHandle& handle) noexcept;
  [[nodiscard]] const std::shared_ptr<const ComponentViewportScene>& active()
      const noexcept {
    return active_;
  }
  [[nodiscard]] const ComponentViewportSceneDiagnostics& diagnostics()
      const noexcept {
    return diagnostics_;
  }
  [[nodiscard]] bool seedReusableSourcesFrom(
      const ComponentViewportSceneAdapter& source) noexcept;
  void clear() noexcept;

 private:
  friend class ComponentViewportSceneAdapterTestAccess;

  BodyRenderMeshCache cache_;
  std::shared_ptr<const ComponentViewportScene> active_;
  ComponentViewportSceneDiagnostics diagnostics_;
  std::size_t failMeshBuildCountdown_{};
};

}  // namespace solidar
