#pragma once

#include <QPointF>
#include <QSize>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ui/ComponentViewportFit.h"
#include "ui/ComponentViewportPicking.h"

namespace solidar {

class ProductDocument;

class ComponentInstanceSelection final {
 public:
  [[nodiscard]] static std::optional<ComponentInstanceSelection> create(
      const ComponentScenePublicationHandle& publication,
      const ComponentInstanceAddress& component) noexcept;
  [[nodiscard]] const ComponentScenePublicationHandle& publication()
      const noexcept { return publication_; }
  [[nodiscard]] const ComponentInstanceAddress& component() const noexcept {
    return component_;
  }
  [[nodiscard]] bool isCurrentFor(
      const ComponentScenePublicationHandle& publication) const noexcept;

 private:
  ComponentInstanceSelection(ComponentScenePublicationHandle publication,
                             ComponentInstanceAddress component) noexcept
      : publication_(std::move(publication)), component_(std::move(component)) {}
  ComponentScenePublicationHandle publication_;
  ComponentInstanceAddress component_;
};

struct ComponentInstanceSessionState final {
  ComponentInstanceAddress address;
  bool visible{};
  bool suppressed{};
  bool grounded{};
  bool effectivelyVisible{};
};

enum class ComponentViewportSessionFailurePoint : std::uint8_t {
  None,
  Presentation,
  Scene,
  Picking,
  Selection,
  Resource,
  FinalPreparation,
};

enum class ComponentViewportSessionStatus : std::uint8_t {
  Prepared,
  InvalidProduct,
  InvalidEpoch,
  PresentationFailed,
  SceneFailed,
  PickingFailed,
  ResourceFailure,
  PreparationFailed,
};

class ComponentViewportSession final {
 public:
  struct PrepareResult final {
    ComponentViewportSessionStatus status{
        ComponentViewportSessionStatus::PreparationFailed};
    std::unique_ptr<ComponentViewportSession> session;
    [[nodiscard]] explicit operator bool() const noexcept {
      return status == ComponentViewportSessionStatus::Prepared && session;
    }
  };

  [[nodiscard]] static PrepareResult prepare(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile profile,
      ComponentViewportSessionFailurePoint failurePoint =
          ComponentViewportSessionFailurePoint::None) noexcept;
  [[nodiscard]] static PrepareResult prepareProjectedIdentity(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile profile,
      const ComponentIdentityTransition& transition,
      ComponentViewportSessionFailurePoint failurePoint =
          ComponentViewportSessionFailurePoint::None) noexcept;
  [[nodiscard]] static PrepareResult prepareSameEpochProjection(
      const ProductDocument& product,
      const ComponentViewportSession& incumbent,
      AssemblyMeshProfile profile,
      ComponentViewportSessionFailurePoint failurePoint =
          ComponentViewportSessionFailurePoint::None) noexcept;

  ~ComponentViewportSession() noexcept;
  ComponentViewportSession(const ComponentViewportSession&) = delete;
  ComponentViewportSession& operator=(const ComponentViewportSession&) = delete;

  [[nodiscard]] ProductPresentationEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] const std::shared_ptr<const ComponentViewportScene>& scene()
      const noexcept { return scene_; }
  [[nodiscard]] const ComponentViewportPicking& picking() const noexcept {
    return picking_;
  }
  [[nodiscard]] const ComponentViewportSceneDiagnostics& sceneDiagnostics()
      const noexcept { return sceneAdapter_.diagnostics(); }
  [[nodiscard]] const ComponentScenePublicationHandle& publicationHandle()
      const noexcept { return scene_->publicationHandle(); }
  [[nodiscard]] const std::vector<ComponentInstanceSessionState>& components()
      const noexcept { return components_; }

  [[nodiscard]] ViewportCameraState camera(
      float yawDeg, float pitchDeg, float zoom, QPointF pan,
      QSize logicalSize) const noexcept;
  [[nodiscard]] ComponentViewportFitResult fit(
      QSize logicalSize, float yawDeg, float pitchDeg,
      double contentFraction = kComponentFitContentFraction) const noexcept;
  [[nodiscard]] ComponentFaceRayQueryResult faceAt(
      const ViewportCameraState& camera, QPointF pixel) const noexcept;
  [[nodiscard]] ComponentEdgeRayQueryResult edgeAt(
      const ViewportCameraState& camera, QPointF pixel,
      double radiusLogicalPixels = 9.0) const noexcept;

  [[nodiscard]] bool setSelectedComponent(
      const std::optional<ComponentInstanceSelection>& selection) noexcept;
  [[nodiscard]] bool setHoveredComponent(
      const std::optional<ComponentInstanceSelection>& selection) noexcept;
  void clearInteraction() noexcept;
  [[nodiscard]] const std::optional<ComponentInstanceSelection>&
  selectedComponent() const noexcept { return selected_; }
  [[nodiscard]] const std::optional<ComponentInstanceSelection>&
  hoveredComponent() const noexcept { return hovered_; }
  [[nodiscard]] const std::optional<ComponentInstanceHighlightTarget>&
  selectedHighlight() const noexcept { return selectedHighlight_; }
  [[nodiscard]] const std::optional<ComponentInstanceHighlightTarget>&
  hoveredHighlight() const noexcept { return hoveredHighlight_; }

 private:
  ComponentViewportSession() = default;
  [[nodiscard]] static PrepareResult prepareImpl(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile profile,
      ComponentViewportSessionFailurePoint failurePoint,
      const ComponentViewportSession* reusable,
      const ComponentIdentityTransition* transition) noexcept;
  [[nodiscard]] const ComponentInstanceSessionState* findComponent(
      const ComponentInstanceAddress& address) const noexcept;
  [[nodiscard]] std::optional<ComponentInstanceHighlightTarget> highlightFor(
      const ComponentInstanceSelection& selection) const;

  // Declaration order is intentional: the coordinator dies last, after all
  // consumers of its exact publication handle.
  ComponentScenePublicationCoordinator coordinator_;
  ComponentViewportSceneAdapter sceneAdapter_;
  ComponentViewportPicking picking_;
  std::shared_ptr<const ComponentViewportScene> scene_;
  std::vector<ComponentInstanceSessionState> components_;
  ProductPresentationEpoch epoch_;
  std::optional<ComponentInstanceSelection> selected_;
  std::optional<ComponentInstanceSelection> hovered_;
  std::optional<ComponentInstanceHighlightTarget> selectedHighlight_;
  std::optional<ComponentInstanceHighlightTarget> hoveredHighlight_;
};

}  // namespace solidar
