#pragma once

#include <QPointF>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "model/EdgeReference.h"
#include "model/FaceReference.h"
#include "ui/AssemblyViewportScene.h"
#include "ui/ViewportCamera.h"
#include "ui/internal/ViewportPickingTopology.h"

namespace solidar {

class AssemblyViewportPickingTestAccess;

// A renderer highlight is useful only while the exact publication from which
// it was produced remains current.  The weak publication handle prevents the
// target from extending coordinator lifetime and prevents equal numeric
// generations from distinct coordinators from aliasing.
class AssemblyOccurrenceHighlightTarget final {
 public:
  [[nodiscard]] static std::optional<AssemblyOccurrenceHighlightTarget> create(
      const AssemblyScenePublicationHandle& publication,
      const AssemblyOccurrenceIdentity& occurrence) noexcept;

  [[nodiscard]] const AssemblyOccurrenceIdentity& occurrence() const noexcept {
    return occurrence_;
  }
  [[nodiscard]] const AssemblyScenePublicationHandle& publication()
      const noexcept {
    return publication_;
  }
  [[nodiscard]] bool isCurrentFor(
      const AssemblyViewportScene& scene) const noexcept;

 private:
  AssemblyOccurrenceHighlightTarget(
      AssemblyScenePublicationHandle publication,
      AssemblyOccurrenceIdentity occurrence) noexcept
      : publication_(std::move(publication)),
        occurrence_(std::move(occurrence)) {}

  AssemblyScenePublicationHandle publication_;
  AssemblyOccurrenceIdentity occurrence_;
};

struct AssemblyFaceAddress final {
  AssemblyOccurrenceIdentity occurrence;
  DefinitionId definitionId;
  FaceReference face;

  friend bool operator==(const AssemblyFaceAddress&,
                         const AssemblyFaceAddress&) = default;
};

struct AssemblyEdgeAddress final {
  AssemblyOccurrenceIdentity occurrence;
  DefinitionId definitionId;
  EdgeReference edge;

  friend bool operator==(const AssemblyEdgeAddress&,
                         const AssemblyEdgeAddress&) = default;
};

struct AssemblyFaceRayHit final {
  AssemblyFaceAddress address;
  Point3d worldPoint{};
  double rayParameter{};
  double cameraDepth{};
  AssemblyScenePublicationHandle publication;

  [[nodiscard]] std::optional<AssemblyOccurrenceHighlightTarget>
  highlightTarget() const noexcept;
};

struct AssemblyEdgeRayHit final {
  AssemblyEdgeAddress address;
  Point3d worldPoint{};
  double rayParameter{};
  double cameraDepth{};
  double distance{};
  AssemblyScenePublicationHandle publication;

  [[nodiscard]] std::optional<AssemblyOccurrenceHighlightTarget>
  highlightTarget() const noexcept;
};

enum class AssemblyViewportPickingPublishStatus : std::uint8_t {
  Published,
  StalePublication,
  InvalidScene,
  InvalidSource,
  DuplicateSource,
  TopologyBuildFailed,
  InvalidTopologyMapping,
  PreparationFailed,
};

struct AssemblyViewportPickingPublishResult final {
  AssemblyViewportPickingPublishStatus status{
      AssemblyViewportPickingPublishStatus::PreparationFailed};
  AssemblyScenePublicationGeneration generation;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyViewportPickingPublishStatus::Published;
  }
};

enum class AssemblyViewportPickingQueryStatus : std::uint8_t {
  Hit,
  Miss,
  Uncertain,
  NoScene,
  StalePublication,
  InvalidInput,
};

// A zero limit means unlimited work.  A bounded query never publishes a
// partial answer: exhausting either counter produces Uncertain.
struct AssemblyViewportSpatialBudget final {
  std::size_t maximumPrimitiveTests{};
  std::size_t maximumOccluderTests{};
};

struct AssemblyFaceRayQueryResult final {
  AssemblyViewportPickingQueryStatus status{
      AssemblyViewportPickingQueryStatus::NoScene};
  std::optional<AssemblyFaceRayHit> hit;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyViewportPickingQueryStatus::Hit &&
           hit.has_value();
  }
};

struct AssemblyEdgeRayQueryResult final {
  AssemblyViewportPickingQueryStatus status{
      AssemblyViewportPickingQueryStatus::NoScene};
  std::optional<AssemblyEdgeRayHit> hit;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyViewportPickingQueryStatus::Hit &&
           hit.has_value();
  }
};

struct AssemblyFaceRectangleHit final {
  AssemblyFaceAddress address;
  AssemblyScenePublicationHandle publication;
};

struct AssemblyEdgeRectangleHit final {
  AssemblyEdgeAddress address;
  AssemblyScenePublicationHandle publication;
};

struct AssemblyFaceRectangleQueryResult final {
  AssemblyViewportPickingQueryStatus status{
      AssemblyViewportPickingQueryStatus::NoScene};
  std::vector<AssemblyFaceRectangleHit> hits;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyViewportPickingQueryStatus::Hit && !hits.empty();
  }
};

struct AssemblyEdgeRectangleQueryResult final {
  AssemblyViewportPickingQueryStatus status{
      AssemblyViewportPickingQueryStatus::NoScene};
  std::vector<AssemblyEdgeRectangleHit> hits;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyViewportPickingQueryStatus::Hit && !hits.empty();
  }
};

enum class AssemblySnapKind : std::uint8_t { Vertex, Edge, Surface };

// Vertices are deliberately transient and publication-scoped.  There is no
// invented persistent VertexReference; where possible a vertex is described
// by its persistent supporting edge.
struct AssemblyViewportSnapHit final {
  AssemblySnapKind kind{AssemblySnapKind::Surface};
  AssemblyOccurrenceIdentity occurrence;
  DefinitionId definitionId;
  Point3d worldPoint{};
  QPointF logicalScreenPoint;
  double cameraDepth{};
  std::optional<AssemblyFaceAddress> supportingFace;
  std::optional<AssemblyEdgeAddress> supportingEdge;
  AssemblyScenePublicationHandle publication;

  [[nodiscard]] std::optional<AssemblyOccurrenceHighlightTarget>
  highlightTarget() const noexcept;
};

struct AssemblyViewportSnapQueryResult final {
  AssemblyViewportPickingQueryStatus status{
      AssemblyViewportPickingQueryStatus::NoScene};
  std::optional<AssemblyViewportSnapHit> hit;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyViewportPickingQueryStatus::Hit &&
           hit.has_value();
  }
};

struct AssemblyViewportPickingDiagnostics final {
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
  std::uint64_t spatialBuilds{};
  std::uint64_t faceQueries{};
  std::uint64_t edgeQueries{};
  std::uint64_t rectangleQueries{};
  std::uint64_t snapQueries{};
  std::uint64_t queryFailures{};
  std::uint64_t staleQueryRejections{};
  std::uint64_t uncertainQueries{};
  std::uint64_t projectedBuilds{};
  std::uint64_t projectedReuses{};
  std::uint64_t broadPhaseCandidates{};
  std::uint64_t triangleTests{};
  std::uint64_t edgeSegmentTests{};
  std::uint64_t occluderTests{};
  std::size_t residentTopologySources{};
  std::size_t activeSources{};
  std::size_t activeInstances{};
  AssemblyScenePublicationGeneration activeGeneration;
};

// Owner-thread CPU spatial service for immutable assembly scenes.  It has no
// QObject, OpenGL, callbacks, or model mutation.  Ray, rectangle and snap
// consumers share exactly one source-topology cache.
class AssemblyViewportPicking final {
 public:
  [[nodiscard]] AssemblyViewportPickingPublishResult publish(
      const std::shared_ptr<const AssemblyViewportScene>& scene) noexcept;
  [[nodiscard]] AssemblyFaceRayQueryResult faceAt(
      const ViewportCameraState& camera, QPointF logicalPixel) const noexcept;
  [[nodiscard]] AssemblyEdgeRayQueryResult edgeAt(
      const ViewportCameraState& camera, QPointF logicalPixel,
      double radiusLogicalPixels = 9.0) const noexcept;
  [[nodiscard]] AssemblyFaceRectangleQueryResult facesInRectangle(
      const ViewportCameraState& camera, QPointF firstLogicalCorner,
      QPointF secondLogicalCorner,
      AssemblyViewportSpatialBudget budget = {}) const noexcept;
  [[nodiscard]] AssemblyEdgeRectangleQueryResult edgesInRectangle(
      const ViewportCameraState& camera, QPointF firstLogicalCorner,
      QPointF secondLogicalCorner,
      AssemblyViewportSpatialBudget budget = {}) const noexcept;
  [[nodiscard]] AssemblyViewportSnapQueryResult snapAt(
      const ViewportCameraState& camera, QPointF logicalPixel,
      double radiusLogicalPixels = 9.0,
      AssemblyViewportSpatialBudget budget = {}) const noexcept;

  [[nodiscard]] const AssemblyViewportPickingDiagnostics& diagnostics()
      const noexcept {
    return diagnostics_;
  }
  [[nodiscard]] bool seedReusableSourcesFrom(
      const AssemblyViewportPicking& source) noexcept;
  void swapPrepared(AssemblyViewportPicking& prepared) noexcept;
  void clear() noexcept;

 private:
  friend class AssemblyViewportPickingTestAccess;
  using SourceTopology = detail::ViewportPickingTopologySource;
  struct PublishedScene;
  struct ProjectedScene;

  [[nodiscard]] std::shared_ptr<const ProjectedScene> projectedFor(
      const ViewportCameraState& camera,
      AssemblyViewportPickingQueryStatus& failureStatus) const noexcept;

  std::unordered_map<BodyMeshKey, std::shared_ptr<const SourceTopology>,
                     BodyMeshKeyHash>
      cache_;
  std::shared_ptr<const PublishedScene> active_;
  mutable std::shared_ptr<const ProjectedScene> projected_;
  mutable AssemblyViewportPickingDiagnostics diagnostics_;
  std::size_t failTopologyBuildCountdown_{};
  std::size_t failMappingCountdown_{};
  mutable std::size_t failProjectionBuildCountdown_{};
};

}  // namespace solidar
