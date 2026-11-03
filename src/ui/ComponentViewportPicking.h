#pragma once

#include <QPointF>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "model/EdgeReference.h"
#include "model/FaceReference.h"
#include "ui/ComponentViewportScene.h"
#include "ui/ViewportCamera.h"
#include "ui/internal/ViewportPickingTopology.h"

namespace solidar {

class ComponentInstanceHighlightTarget final {
 public:
  [[nodiscard]] static std::optional<ComponentInstanceHighlightTarget> create(
      const ComponentScenePublicationHandle& publication,
      const ComponentInstanceAddress& component) noexcept;

  [[nodiscard]] const ComponentInstanceAddress& component() const noexcept {
    return component_;
  }
  [[nodiscard]] const ComponentScenePublicationHandle& publication()
      const noexcept {
    return publication_;
  }
  [[nodiscard]] bool isCurrentFor(
      const ComponentViewportScene& scene) const noexcept;
  [[nodiscard]] std::vector<ComponentBodyIdentity> visibleBodies(
      const ComponentViewportScene& scene) const;

 private:
  ComponentInstanceHighlightTarget(
      ComponentScenePublicationHandle publication,
      ComponentInstanceAddress component) noexcept
      : publication_(std::move(publication)), component_(std::move(component)) {}

  ComponentScenePublicationHandle publication_;
  ComponentInstanceAddress component_;
};

struct ComponentFaceAddress final {
  ComponentBodyIdentity body;
  FaceReference face;
  friend bool operator==(const ComponentFaceAddress&,
                         const ComponentFaceAddress&) = default;
};

struct ComponentEdgeAddress final {
  ComponentBodyIdentity body;
  EdgeReference edge;
  friend bool operator==(const ComponentEdgeAddress&,
                         const ComponentEdgeAddress&) = default;
};

struct ComponentFaceRayHit final {
  ComponentFaceAddress address;
  Point3d worldPoint{};
  double rayParameter{};
  double cameraDepth{};
  ComponentScenePublicationHandle publication;

  [[nodiscard]] std::optional<ComponentInstanceHighlightTarget>
  highlightTarget() const noexcept;
};

struct ComponentEdgeRayHit final {
  ComponentEdgeAddress address;
  Point3d worldPoint{};
  double rayParameter{};
  double cameraDepth{};
  double distance{};
  ComponentScenePublicationHandle publication;

  [[nodiscard]] std::optional<ComponentInstanceHighlightTarget>
  highlightTarget() const noexcept;
};

enum class ComponentViewportPickingPublishStatus : std::uint8_t {
  Published,
  StalePublication,
  InvalidScene,
  InvalidSource,
  DuplicateSource,
  TopologyBuildFailed,
  InvalidTopologyMapping,
  PreparationFailed,
};

struct ComponentViewportPickingPublishResult final {
  ComponentViewportPickingPublishStatus status{
      ComponentViewportPickingPublishStatus::PreparationFailed};
  ComponentScenePublicationGeneration generation;
  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentViewportPickingPublishStatus::Published;
  }
};

enum class ComponentViewportPickingQueryStatus : std::uint8_t {
  Hit,
  Miss,
  NoScene,
  StalePublication,
  InvalidInput,
};

struct ComponentFaceRayQueryResult final {
  ComponentViewportPickingQueryStatus status{
      ComponentViewportPickingQueryStatus::NoScene};
  std::optional<ComponentFaceRayHit> hit;
  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentViewportPickingQueryStatus::Hit && hit;
  }
};

struct ComponentEdgeRayQueryResult final {
  ComponentViewportPickingQueryStatus status{
      ComponentViewportPickingQueryStatus::NoScene};
  std::optional<ComponentEdgeRayHit> hit;
  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentViewportPickingQueryStatus::Hit && hit;
  }
};

struct ComponentViewportPickingDiagnostics final {
  std::uint64_t publicationAttempts{};
  std::uint64_t publicationSuccesses{};
  std::uint64_t publicationFailures{};
  std::uint64_t stalePublicationRejections{};
  std::uint64_t topologySourceBuildAttempts{};
  std::uint64_t topologySourceBuildSuccesses{};
  std::uint64_t topologySourceBuildFailures{};
  std::uint64_t topologySourceReuses{};
  std::uint64_t v2FaceReferences{};
  std::uint64_t v1FaceFallbacks{};
  std::uint64_t v2EdgeReferences{};
  std::uint64_t v1EdgeFallbacks{};
  std::uint64_t faceQueries{};
  std::uint64_t edgeQueries{};
  std::uint64_t queryFailures{};
  std::uint64_t staleQueryRejections{};
  std::uint64_t broadPhaseCandidates{};
  std::uint64_t triangleTests{};
  std::uint64_t edgeSegmentTests{};
  std::uint64_t occluderTests{};
  std::size_t residentTopologySources{};
  std::size_t activeSources{};
  std::size_t activeInstances{};
  ComponentScenePublicationGeneration activeGeneration;
};

class ComponentViewportPicking final {
 public:
  [[nodiscard]] ComponentViewportPickingPublishResult publish(
      const std::shared_ptr<const ComponentViewportScene>& scene) noexcept;
  [[nodiscard]] ComponentFaceRayQueryResult faceAt(
      const ViewportCameraState& camera, QPointF logicalPixel) const noexcept;
  [[nodiscard]] ComponentEdgeRayQueryResult edgeAt(
      const ViewportCameraState& camera, QPointF logicalPixel,
      double radiusLogicalPixels = 9.0) const noexcept;

  [[nodiscard]] const ComponentViewportPickingDiagnostics& diagnostics()
      const noexcept { return diagnostics_; }
  [[nodiscard]] bool seedReusableSourcesFrom(
      const ComponentViewportPicking& source) noexcept;
  void swapPrepared(ComponentViewportPicking& prepared) noexcept;
  void clear() noexcept;

 private:
  struct PublishedScene;
  detail::ViewportPickingTopologyCache cache_;
  std::shared_ptr<const PublishedScene> active_;
  mutable ComponentViewportPickingDiagnostics diagnostics_;
  std::size_t failTopologyBuildCountdown_{};
  std::size_t failMappingCountdown_{};
};

}  // namespace solidar
