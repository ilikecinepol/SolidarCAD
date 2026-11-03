#pragma once

#include <cstdint>
#include <memory>

#include "model/ProductDocument.h"
#include "ui/Viewport.h"
#include "ui/application/ModelCommandHistory.h"

namespace solidar {

enum class PartWorkspaceProjectionStatus : std::uint8_t {
  Prepared,
  InvalidProduct,
  InvalidSelection,
  BodyDisplayFailed,
  ResourceFailure,
  PreparationFailed,
};

enum class PartWorkspaceProjectionFailurePoint : std::uint8_t {
  None,
  Selection,
  Topology,
  Meshing,
  Bounds,
  Resource,
  Finalization,
};

class PartWorkspaceProjection final {
 public:
  ~PartWorkspaceProjection() = default;
  PartWorkspaceProjection(const PartWorkspaceProjection&) = delete;
  PartWorkspaceProjection& operator=(const PartWorkspaceProjection&) = delete;
  PartWorkspaceProjection(PartWorkspaceProjection&&) noexcept = default;
  PartWorkspaceProjection& operator=(PartWorkspaceProjection&&) noexcept =
      default;

  [[nodiscard]] const PreparedBodyDisplay* bodyDisplay() const noexcept {
    return bodyDisplay_.get();
  }
  [[nodiscard]] std::unique_ptr<PreparedBodyDisplay> releaseBodyDisplay()
      noexcept {
    return std::move(bodyDisplay_);
  }
  [[nodiscard]] const ComponentWorkspaceSemanticState& semanticState()
      const noexcept {
    return semanticState_;
  }
  [[nodiscard]] ProductPresentationEpoch epoch() const noexcept {
    return epoch_;
  }
  [[nodiscard]] AssemblyMeshProfile meshProfile() const noexcept {
    return meshProfile_;
  }

 private:
  friend class PartWorkspaceProjectionPreparer;
  PartWorkspaceProjection(
      std::unique_ptr<PreparedBodyDisplay> bodyDisplay,
      ComponentWorkspaceSemanticState semanticState,
      ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile) noexcept
      : bodyDisplay_(std::move(bodyDisplay)),
        semanticState_(std::move(semanticState)),
        epoch_(epoch),
        meshProfile_(meshProfile) {}

  std::unique_ptr<PreparedBodyDisplay> bodyDisplay_;
  ComponentWorkspaceSemanticState semanticState_;
  ProductPresentationEpoch epoch_;
  AssemblyMeshProfile meshProfile_;
};

struct PartWorkspaceProjectionResult final {
  PartWorkspaceProjectionStatus status{
      PartWorkspaceProjectionStatus::PreparationFailed};
  std::unique_ptr<PartWorkspaceProjection> projection;
  BodyDisplayPrepareStatus bodyDisplayStatus{
      BodyDisplayPrepareStatus::InvalidInput};
  GeometryFailure failure;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == PartWorkspaceProjectionStatus::Prepared && projection;
  }
};

class PartWorkspaceProjectionPreparer final {
 public:
  [[nodiscard]] static PartWorkspaceProjectionResult prepare(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const ComponentWorkspaceSemanticState& semanticState,
      PartWorkspaceProjectionFailurePoint failurePoint =
          PartWorkspaceProjectionFailurePoint::None) noexcept;
};

}  // namespace solidar
