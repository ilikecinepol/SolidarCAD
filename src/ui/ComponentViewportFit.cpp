#include "ui/ComponentViewportFit.h"

#include <utility>

#include "ui/internal/ViewportFitGeometry.h"

namespace solidar {

ComponentViewportFitResult fitComponentViewport(
    const std::shared_ptr<const ComponentViewportScene>& scene,
    const QSize& logicalSize, float yawDeg, float pitchDeg,
    double contentFraction) noexcept {
  if (!scene) return {ComponentViewportFitStatus::MissingScene, std::nullopt};
  if (!scene->isCurrent())
    return {ComponentViewportFitStatus::StalePublication, std::nullopt};
  if (!scene->worldBounds())
    return {ComponentViewportFitStatus::EmptyBounds, std::nullopt};
  const auto geometry = detail::calculateViewportFitGeometry(
      *scene->worldBounds(), logicalSize.width(), logicalSize.height(), yawDeg,
      pitchDeg, contentFraction);
  if (!geometry)
    return {ComponentViewportFitStatus::InvalidInput, std::nullopt};
  if (!scene->isCurrent())
    return {ComponentViewportFitStatus::StalePublication, std::nullopt};
  ComponentViewportFit fit{
      scene->generation(),
      static_cast<float>(geometry->zoom),
      QPointF{geometry->panX, geometry->panY},
      geometry->center,
      geometry->depthExtent,
      geometry->fullyContained,
      geometry->minimumZoomClamped,
      geometry->maximumZoomClamped};
  return {ComponentViewportFitStatus::Fitted, std::move(fit)};
}

}  // namespace solidar
