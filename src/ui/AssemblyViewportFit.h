#pragma once

#include <QPointF>
#include <QSize>

#include <cstdint>
#include <memory>
#include <optional>

#include "ui/AssemblyViewportScene.h"

namespace solidar {

inline constexpr double kAssemblyFitContentFraction = 0.88;

enum class AssemblyViewportFitStatus : std::uint8_t {
  Fitted,
  MissingScene,
  StalePublication,
  EmptyBounds,
  InvalidInput,
};

struct AssemblyViewportFit final {
  AssemblyScenePublicationGeneration generation;
  float zoom{1.0F};
  QPointF pan;
  Point3d center{};
  double depthExtent{1.0};
  bool fullyContained{};
  bool minimumZoomClamped{};
  bool maximumZoomClamped{};
};

struct AssemblyViewportFitResult final {
  AssemblyViewportFitStatus status{AssemblyViewportFitStatus::InvalidInput};
  std::optional<AssemblyViewportFit> fit;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyViewportFitStatus::Fitted && fit.has_value();
  }
};

// Pure logical-pixel fit. DPR is deliberately absent: the renderer applies
// it only to the GL viewport and line widths, never to camera framing.
[[nodiscard]] AssemblyViewportFitResult fitAssemblyViewport(
    const std::shared_ptr<const AssemblyViewportScene>& scene,
    const QSize& logicalSize, float yawDeg, float pitchDeg,
    double contentFraction = kAssemblyFitContentFraction) noexcept;

}  // namespace solidar
