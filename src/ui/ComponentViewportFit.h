#pragma once

#include <QPointF>
#include <QSize>

#include <cstdint>
#include <memory>
#include <optional>

#include "ui/ComponentViewportScene.h"

namespace solidar {

inline constexpr double kComponentFitContentFraction = 0.88;

enum class ComponentViewportFitStatus : std::uint8_t {
  Fitted,
  MissingScene,
  StalePublication,
  EmptyBounds,
  InvalidInput,
};

struct ComponentViewportFit final {
  ComponentScenePublicationGeneration generation;
  float zoom{1.0F};
  QPointF pan;
  Point3d center{};
  double depthExtent{1.0};
  bool fullyContained{};
  bool minimumZoomClamped{};
  bool maximumZoomClamped{};
};

struct ComponentViewportFitResult final {
  ComponentViewportFitStatus status{ComponentViewportFitStatus::InvalidInput};
  std::optional<ComponentViewportFit> fit;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentViewportFitStatus::Fitted && fit.has_value();
  }
};

[[nodiscard]] ComponentViewportFitResult fitComponentViewport(
    const std::shared_ptr<const ComponentViewportScene>& scene,
    const QSize& logicalSize, float yawDeg, float pitchDeg,
    double contentFraction = kComponentFitContentFraction) noexcept;

}  // namespace solidar
