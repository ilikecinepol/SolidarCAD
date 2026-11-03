#pragma once

#include <cstdint>
#include <memory>
#include <variant>

#include "model/ProductDocument.h"
#include "ui/ComponentWorkspace.h"
#include "ui/application/PartWorkspaceProjection.h"

namespace solidar {

enum class ModelWorkspaceProjectionStatus : std::uint8_t {
  Empty,
  PreparedPart,
  PreparedComponent,
  InvalidCandidate,
  ProjectionRejected,
  ResourceFailure,
  AlreadyPrepared,
  Consumed,
};

enum class ModelWorkspaceProjectionFailurePoint : std::uint8_t {
  None,
  Resource,
  PartSelection,
  PartTopology,
  PartMeshing,
  PartBounds,
  ComponentSession,
  ComponentTree,
  Finalization,
};

// One prepare-before-swap successor used by component extraction Undo/Redo.
// It retains exactly one off-side Part or Component projection and never owns
// the candidate ProductDocument supplied by ProductDocument.
class ModelWorkspaceProjectionSuccessor final
    : public ProductDocumentSuccessor {
 public:
  ModelWorkspaceProjectionSuccessor(
      ProductPresentationEpoch epoch, AssemblyMeshProfile meshProfile,
      ComponentWorkspaceSemanticState semanticState,
      ModelWorkspaceProjectionFailurePoint failurePoint =
          ModelWorkspaceProjectionFailurePoint::None,
      std::optional<ComponentWorkspaceInstanceKey> hoveredInstance =
          std::nullopt)
      : epoch_(epoch),
        meshProfile_(meshProfile),
        semanticState_(std::move(semanticState)),
        failurePoint_(failurePoint),
        hoveredInstance_(std::move(hoveredInstance)) {}

  [[nodiscard]] ProductDocumentSuccessorStatus prepare(
      const ProductDocument& candidate,
      ProductDocumentSuccessorImpact impact) noexcept override;

  [[nodiscard]] ModelWorkspaceProjectionStatus status() const noexcept {
    return status_;
  }
  [[nodiscard]] const PartWorkspaceProjection* part() const noexcept;
  [[nodiscard]] const PreparedComponentProjection* component() const noexcept;
  [[nodiscard]] std::unique_ptr<PartWorkspaceProjection> takePart() noexcept;
  [[nodiscard]] std::unique_ptr<PreparedComponentProjection> takeComponent()
      noexcept;

 private:
  using Prepared =
      std::variant<std::monostate, std::unique_ptr<PartWorkspaceProjection>,
                   std::unique_ptr<PreparedComponentProjection>>;

  ProductPresentationEpoch epoch_;
  AssemblyMeshProfile meshProfile_;
  ComponentWorkspaceSemanticState semanticState_;
  ModelWorkspaceProjectionFailurePoint failurePoint_;
  std::optional<ComponentWorkspaceInstanceKey> hoveredInstance_;
  ModelWorkspaceProjectionStatus status_{ModelWorkspaceProjectionStatus::Empty};
  Prepared prepared_;
  bool attempted_{};
};

}  // namespace solidar
