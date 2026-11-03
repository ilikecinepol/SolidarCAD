#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "presentation/AssemblyScenePublicationCoordinator.h"
#include "ui/AssemblyOccurrenceSelection.h"
#include "ui/AssemblyViewportPicking.h"
#include "model/PlanarMateSolver.h"
#include "ui/AssemblyViewportScene.h"
#include "ui/ViewportCamera.h"

namespace solidar {

struct PreparedAssemblySelectionState final {
  std::optional<AssemblyOccurrenceSelection> selection;
  std::optional<AssemblyOccurrenceHighlightTarget> highlight;
};

enum class PrepareAssemblySelectionStatus {
  Prepared,
  Rejected,
  ResourceFailure,
};

struct PrepareAssemblySelectionResult final {
  PrepareAssemblySelectionStatus status{PrepareAssemblySelectionStatus::Rejected};
  PreparedAssemblySelectionState state;
};

enum class AssemblyViewportSessionFailurePoint : std::uint8_t {
  None,
  Presentation,
  Scene,
  Picking,
  Spatial,
  Selection,
  Resource,
  FinalPreparation,
};

enum class AssemblyViewportSessionStatus : std::uint8_t {
  Prepared,
  InvalidProduct,
  InvalidEpoch,
  PresentationFailed,
  SceneFailed,
  PickingFailed,
  ResourceFailure,
  PreparationFailed,
};

struct AssemblyOccurrenceSessionState final {
  AssemblyOccurrenceIdentity identity;
  DefinitionId targetDefinitionId;
  bool visible{};
  bool suppressed{};
  bool grounded{};
  bool effectivelyVisible{};
};

struct PreparedAssemblyIsolationState final {
  std::shared_ptr<const AssemblyViewportScene> displayedScene;
  std::unique_ptr<AssemblyViewportPicking> picking;
  std::optional<AssemblyOccurrenceIdentity> isolatedOccurrence;
  PreparedAssemblySelectionState selection;
};

enum class PrepareAssemblyIsolationStatus : std::uint8_t {
  Prepared,
  NotMeaningful,
  InvalidOccurrence,
  ResourceFailure,
};

// Deterministic preparation seams used to prove that transient isolate never
// publishes a partial scene/picking state.  They do not alter runtime
// semantics when left at None.
enum class AssemblyIsolationFailurePoint : std::uint8_t {
  None,
  Scene,
  Picking,
  Resource,
};

struct PrepareAssemblyIsolationResult final {
  PrepareAssemblyIsolationStatus status{
      PrepareAssemblyIsolationStatus::InvalidOccurrence};
  std::optional<PreparedAssemblyIsolationState> state;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == PrepareAssemblyIsolationStatus::Prepared &&
           state.has_value();
  }
};

class AssemblyViewportSession final {
 public:
  struct PrepareResult final {
    AssemblyViewportSessionStatus status{
        AssemblyViewportSessionStatus::PreparationFailed};
    std::unique_ptr<AssemblyViewportSession> session;

    [[nodiscard]] explicit operator bool() const noexcept {
      return status == AssemblyViewportSessionStatus::Prepared &&
             session != nullptr;
    }
  };

  [[nodiscard]] static PrepareResult prepare(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint =
          AssemblyViewportSessionFailurePoint::None) noexcept;
  [[nodiscard]] static PrepareResult prepareVisibilityProjection(
      const ProductDocument& product,
      const AssemblyViewportSession& incumbent,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint =
          AssemblyViewportSessionFailurePoint::None) noexcept;
  [[nodiscard]] static PrepareResult prepareGroundingProjection(
      const ProductDocument& product,
      const AssemblyViewportSession& incumbent,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint =
          AssemblyViewportSessionFailurePoint::None) noexcept;
  [[nodiscard]] static PrepareResult prepareSuppressionProjection(
      const ProductDocument& product,
      const AssemblyViewportSession& incumbent,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint =
          AssemblyViewportSessionFailurePoint::None) noexcept;
  [[nodiscard]] static PrepareResult prepareDuplicateProjection(
      const ProductDocument& product,
      const AssemblyViewportSession& incumbent,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint =
          AssemblyViewportSessionFailurePoint::None) noexcept;
  [[nodiscard]] static PrepareResult prepareRemoveProjection(
      const ProductDocument& product,
      const AssemblyViewportSession& incumbent,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint =
          AssemblyViewportSessionFailurePoint::None) noexcept;
  [[nodiscard]] static PrepareResult prepareDefinitionProjection(
      const ProductDocument& product,
      const AssemblyViewportSession& incumbent,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint =
          AssemblyViewportSessionFailurePoint::None) noexcept;

  ~AssemblyViewportSession() noexcept;
  AssemblyViewportSession(const AssemblyViewportSession&) = delete;
  AssemblyViewportSession& operator=(const AssemblyViewportSession&) = delete;
  AssemblyViewportSession(AssemblyViewportSession&&) = delete;
  AssemblyViewportSession& operator=(AssemblyViewportSession&&) = delete;

  [[nodiscard]] ProductPresentationEpoch epoch() const noexcept {
    return epoch_;
  }
  [[nodiscard]] const std::shared_ptr<const AssemblyViewportScene>& scene()
      const noexcept {
    return scene_;
  }
  [[nodiscard]] const AssemblyViewportPicking& picking() const noexcept {
    return picking_;
  }
  [[nodiscard]] const AssemblyViewportSceneDiagnostics& sceneDiagnostics()
      const noexcept {
    return sceneAdapter_.diagnostics();
  }
  [[nodiscard]] const AssemblyScenePublicationHandle& publicationHandle()
      const noexcept {
    return scene_->publicationHandle();
  }
  [[nodiscard]] const std::vector<AssemblyOccurrenceSessionState>& occurrences()
      const noexcept {
    return occurrences_;
  }
  [[nodiscard]] bool isolationActive() const noexcept {
    return isolatedOccurrence_.has_value();
  }
  [[nodiscard]] const std::optional<AssemblyOccurrenceIdentity>&
  isolatedOccurrence() const noexcept {
    return isolatedOccurrence_;
  }
  [[nodiscard]] bool canIsolate(
      const AssemblyOccurrenceIdentity& occurrence) const noexcept;
  [[nodiscard]] PrepareAssemblyIsolationResult prepareIsolation(
      const std::optional<AssemblyOccurrenceIdentity>& occurrence,
      AssemblyIsolationFailurePoint failurePoint =
          AssemblyIsolationFailurePoint::None) const noexcept;
  void installPreparedIsolation(
      PreparedAssemblyIsolationState&& prepared) noexcept;

  [[nodiscard]] ViewportCameraState camera(
      float yawDeg, float pitchDeg, float zoom, QPointF pan,
      QSize logicalSize) const noexcept;
  [[nodiscard]] AssemblyFaceRayQueryResult faceAt(
      const ViewportCameraState& camera, QPointF logicalPixel) const noexcept;
  // Prepared visual-only mate projection. It reuses the incumbent immutable
  // mesh sources/publication, never publishes picking for candidate poses.
  [[nodiscard]] std::shared_ptr<const AssemblyViewportScene>
  prepareMatePreview(const MatePlacementCandidate& candidate) const noexcept;

  [[nodiscard]] bool setSelectedOccurrence(
      const std::optional<AssemblyOccurrenceSelection>& selection) noexcept;
  [[nodiscard]] PrepareAssemblySelectionResult prepareSelectedOccurrence(
      const std::optional<AssemblyOccurrenceSelection>& selection) const noexcept;
  void installPreparedSelectedOccurrence(
      PreparedAssemblySelectionState&& prepared) noexcept;
  [[nodiscard]] bool setHoveredOccurrence(
      const std::optional<AssemblyOccurrenceSelection>& selection) noexcept;
  void clearInteraction() noexcept;

  [[nodiscard]] const std::optional<AssemblyOccurrenceSelection>&
  selectedOccurrence() const noexcept {
    return selectedOccurrence_;
  }
  [[nodiscard]] const std::optional<AssemblyOccurrenceHighlightTarget>&
  selectedHighlight() const noexcept {
    return selectedHighlight_;
  }
  [[nodiscard]] const std::optional<AssemblyOccurrenceHighlightTarget>&
  hoveredHighlight() const noexcept {
    return hoveredHighlight_;
  }
  [[nodiscard]] const std::optional<AssemblyOccurrenceSelection>&
  hoveredOccurrence() const noexcept {
    return hoveredOccurrence_;
  }

 private:
  AssemblyViewportSession() = default;

  [[nodiscard]] static PrepareResult prepareSameEpochOccurrenceProjection(
      const ProductDocument& product,
      const AssemblyViewportSession& incumbent,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint) noexcept;

  [[nodiscard]] const AssemblyOccurrenceSessionState* findOccurrence(
      const AssemblyOccurrenceIdentity& occurrence) const noexcept;
  [[nodiscard]] std::optional<AssemblyOccurrenceHighlightTarget>
  highlightFor(const AssemblyOccurrenceSelection& selection) const;

  // Destruction order is intentional: coordinator is declared first and is
  // therefore destroyed last, after every publication-scoped consumer.
  AssemblyScenePublicationCoordinator coordinator_;
  AssemblyViewportSceneAdapter sceneAdapter_;
  AssemblyViewportPicking picking_;
  std::shared_ptr<const AssemblyViewportScene> scene_;
  // The authored scene remains resident while a filtered isolate view is
  // active, so isolate/show-all never releases definition-local CPU/GPU
  // sources and never changes the publication or product epoch.
  std::shared_ptr<const AssemblyViewportScene> authoredScene_;
  std::vector<AssemblyOccurrenceSessionState> occurrences_;
  ProductPresentationEpoch epoch_;
  std::optional<AssemblyOccurrenceSelection> selectedOccurrence_;
  std::optional<AssemblyOccurrenceSelection> hoveredOccurrence_;
  std::optional<AssemblyOccurrenceHighlightTarget> selectedHighlight_;
  std::optional<AssemblyOccurrenceHighlightTarget> hoveredHighlight_;
  std::optional<AssemblyOccurrenceIdentity> isolatedOccurrence_;
};

}  // namespace solidar
