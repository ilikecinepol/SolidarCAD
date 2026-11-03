#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "presentation/AssemblyScenePublicationCoordinator.h"
#include "ui/BodyRenderMesh.h"

class TopoDS_Shape;

namespace solidar {

class AssemblyViewportSceneAdapterTestAccess;
class AssemblyViewportSession;

struct AssemblyViewportSceneSource final {
  BodyMeshKey key;
  std::shared_ptr<const TopoDS_Shape> shape;
  std::shared_ptr<const TopologyLineage> topologyLineage;
  std::shared_ptr<const BodyRenderMesh> mesh;
};

struct AssemblyViewportSceneInstance final {
  BodyMeshKey sourceKey;
  std::shared_ptr<const BodyRenderMesh> mesh;
  RigidTransform3d localToWorld{RigidTransform3d::identity()};
  AssemblyOccurrenceIdentity occurrenceIdentity;
  bool grounded{};
  Bounds3d worldBounds;
};

// Immutable CPU scene. It retains both the authoritative presentation
// snapshot and every source mesh used by its instances. The publication
// handle remains a generation guard; ownership of this object alone does not
// make a stale publication current again.
class AssemblyViewportScene final {
 public:
  [[nodiscard]] AssemblyScenePublicationGeneration generation() const noexcept {
    return generation_;
  }
  [[nodiscard]] bool isCurrent() const noexcept { return handle_.isCurrent(); }
  [[nodiscard]] const AssemblyScenePublicationHandle& publicationHandle()
      const noexcept {
    return handle_;
  }
  [[nodiscard]] const std::shared_ptr<const AssemblyPresentationSnapshot>&
  snapshot() const noexcept {
    return snapshot_;
  }
  [[nodiscard]] const std::vector<AssemblyViewportSceneSource>& sources()
      const noexcept {
    return sources_;
  }
  [[nodiscard]] const std::vector<AssemblyViewportSceneInstance>& instances()
      const noexcept {
    return instances_;
  }
  [[nodiscard]] const std::optional<Bounds3d>& worldBounds() const noexcept {
    return worldBounds_;
  }

 private:
  friend class AssemblyViewportSceneAdapter;
  friend class AssemblyViewportSession;

  AssemblyViewportScene(
      AssemblyScenePublicationHandle handle,
      std::shared_ptr<const AssemblyPresentationSnapshot> snapshot,
      std::vector<AssemblyViewportSceneSource> sources,
      std::vector<AssemblyViewportSceneInstance> instances,
      std::optional<Bounds3d> worldBounds) noexcept
      : generation_(handle.generation()),
        handle_(std::move(handle)),
        snapshot_(std::move(snapshot)),
        sources_(std::move(sources)),
        instances_(std::move(instances)),
        worldBounds_(std::move(worldBounds)) {}

  AssemblyScenePublicationGeneration generation_;
  AssemblyScenePublicationHandle handle_;
  std::shared_ptr<const AssemblyPresentationSnapshot> snapshot_;
  std::vector<AssemblyViewportSceneSource> sources_;
  std::vector<AssemblyViewportSceneInstance> instances_;
  std::optional<Bounds3d> worldBounds_;
};

enum class AssemblyViewportSceneStatus : std::uint8_t {
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

struct AssemblyViewportSceneResult final {
  AssemblyViewportSceneStatus status{
      AssemblyViewportSceneStatus::PreparationFailed};
  GeometryFailure meshFailure;
  AssemblyScenePublicationGeneration generation;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyViewportSceneStatus::Published;
  }
};

struct AssemblyViewportSceneDiagnostics final {
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
  AssemblyScenePublicationGeneration activeGeneration;
};

// Owner-thread, non-QObject adapter from A4a2 publications to immutable,
// definition-local CPU render sources plus occurrence transforms. It never
// creates transformed B-Reps, topology, bounds, or one mesh per occurrence.
class AssemblyViewportSceneAdapter final {
 public:
  [[nodiscard]] AssemblyViewportSceneResult publish(
      const AssemblyScenePublicationHandle& handle) noexcept;
  [[nodiscard]] const std::shared_ptr<const AssemblyViewportScene>& active()
      const noexcept {
    return active_;
  }
  [[nodiscard]] const AssemblyViewportSceneDiagnostics& diagnostics()
      const noexcept {
    return diagnostics_;
  }
  // Copies only definition-local immutable CPU sources.  No publication or
  // active scene crosses this boundary; the destination must still publish
  // its own exact-generation scene before becoming visible.
  [[nodiscard]] bool seedReusableSourcesFrom(
      const AssemblyViewportSceneAdapter& source) noexcept;
  void clear() noexcept;

 private:
  friend class AssemblyViewportSceneAdapterTestAccess;

  BodyRenderMeshCache cache_;
  std::shared_ptr<const AssemblyViewportScene> active_;
  AssemblyViewportSceneDiagnostics diagnostics_;
  std::size_t failMeshBuildCountdown_{};
};

}  // namespace solidar
