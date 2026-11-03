#pragma once

#include <QMetaType>
#include <QPointF>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

#include "presentation/ComponentScenePublicationCoordinator.h"
#include "ui/ViewportCamera.h"

namespace solidar {

class ComponentViewportScene;

inline constexpr double kComponentPlacementDragThresholdPx = 3.0;
inline constexpr double kComponentPlacementDragMaximumDeltaMm = 1.0e9;

enum class ComponentPlacementDragMapStatus : std::uint8_t {
  Mapped,
  BelowThreshold,
  InvalidCamera,
  NonFiniteInput,
  ValueOutOfRange,
};

struct ComponentPlacementDragMapResult final {
  ComponentPlacementDragMapStatus status{
      ComponentPlacementDragMapStatus::InvalidCamera};
  std::array<double, 3> worldDeltaMm{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentPlacementDragMapStatus::Mapped;
  }
};

// Pure orthographic mapping onto the camera-facing plane. Logical pixels are
// used throughout; DPR belongs only to the renderer viewport and line widths.
[[nodiscard]] ComponentPlacementDragMapResult mapComponentPlacementDrag(
    const ViewportCameraState& camera, QPointF pressLogical,
    QPointF currentLogical) noexcept;

struct ComponentPlacementDragRequest final {
  ComponentScenePublicationHandle publication;
  ComponentInstanceAddress component;
  std::array<double, 3> worldDeltaMm{};
  std::uint64_t ownershipRevision{};
};

class ComponentPlacementPreview final {
 public:
  [[nodiscard]] static std::optional<ComponentPlacementPreview> create(
      ComponentScenePublicationHandle publication,
      ComponentInstanceAddress component,
      std::array<double, 3> worldDeltaMm,
      std::uint64_t ownershipRevision) noexcept;

  [[nodiscard]] const ComponentScenePublicationHandle& publication()
      const noexcept { return publication_; }
  [[nodiscard]] const ComponentInstanceAddress& component() const noexcept {
    return component_;
  }
  [[nodiscard]] const std::array<double, 3>& worldDeltaMm() const noexcept {
    return worldDeltaMm_;
  }
  [[nodiscard]] std::uint64_t ownershipRevision() const noexcept {
    return ownershipRevision_;
  }
  [[nodiscard]] bool isCurrentFor(
      const ComponentViewportScene& scene,
      std::uint64_t ownershipRevision) const noexcept;
  [[nodiscard]] bool moves(
      const ComponentInstanceAddress& candidate) const noexcept;
  [[nodiscard]] ComponentPlacementDragRequest request() const {
    return {publication_, component_, worldDeltaMm_, ownershipRevision_};
  }

 private:
  ComponentPlacementPreview(ComponentScenePublicationHandle publication,
                            ComponentInstanceAddress component,
                            std::array<double, 3> worldDeltaMm,
                            std::uint64_t ownershipRevision) noexcept
      : publication_(std::move(publication)),
        component_(std::move(component)),
        worldDeltaMm_(worldDeltaMm),
        ownershipRevision_(ownershipRevision) {}

  ComponentScenePublicationHandle publication_;
  ComponentInstanceAddress component_;
  std::array<double, 3> worldDeltaMm_{};
  std::uint64_t ownershipRevision_{};
};

}  // namespace solidar

Q_DECLARE_METATYPE(solidar::ComponentPlacementDragRequest)
