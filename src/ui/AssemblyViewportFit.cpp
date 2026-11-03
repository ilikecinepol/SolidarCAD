#include "ui/AssemblyViewportFit.h"

#include <utility>

#include "ui/internal/ViewportFitGeometry.h"

namespace solidar {

AssemblyViewportFitResult fitAssemblyViewport(
    const std::shared_ptr<const AssemblyViewportScene>& scene,
    const QSize& logicalSize, float yawDeg, float pitchDeg,
    double contentFraction) noexcept {
  if (!scene) return {AssemblyViewportFitStatus::MissingScene, std::nullopt};
  if (!scene->isCurrent())
    return {AssemblyViewportFitStatus::StalePublication, std::nullopt};
  if (!scene->worldBounds())
    return {AssemblyViewportFitStatus::EmptyBounds, std::nullopt};
  const auto geometry = detail::calculateViewportFitGeometry(
      *scene->worldBounds(), logicalSize.width(), logicalSize.height(), yawDeg,
      pitchDeg, contentFraction);
  if (!geometry)
    return {AssemblyViewportFitStatus::InvalidInput, std::nullopt};

  if (!scene->isCurrent())
    return {AssemblyViewportFitStatus::StalePublication, std::nullopt};
  AssemblyViewportFit fit{
      scene->generation(),
      static_cast<float>(geometry->zoom),
      QPointF{geometry->panX, geometry->panY},
      geometry->center,
      geometry->depthExtent,
      geometry->fullyContained,
      geometry->minimumZoomClamped,
      geometry->maximumZoomClamped};
  return {AssemblyViewportFitStatus::Fitted, std::move(fit)};
}

}  // namespace solidar
