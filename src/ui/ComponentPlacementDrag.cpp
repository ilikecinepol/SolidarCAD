#include "ui/ComponentPlacementDrag.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "ui/ComponentViewportScene.h"

namespace solidar {
namespace {

bool finitePoint(QPointF point) noexcept {
  return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool validDelta(const std::array<double, 3>& delta) noexcept {
  return std::all_of(delta.begin(), delta.end(), [](double value) {
    return std::isfinite(value) &&
           std::abs(value) <= kComponentPlacementDragMaximumDeltaMm;
  });
}

}  // namespace

ComponentPlacementDragMapResult mapComponentPlacementDrag(
    const ViewportCameraState& camera, QPointF pressLogical,
    QPointF currentLogical) noexcept {
  if (!finitePoint(pressLogical) || !finitePoint(currentLogical))
    return {ComponentPlacementDragMapStatus::NonFiniteInput, {}};
  if (camera.logicalSize.width() <= 0 || camera.logicalSize.height() <= 0 ||
      !std::isfinite(camera.yawDeg) || !std::isfinite(camera.pitchDeg) ||
      !std::isfinite(camera.zoom) || camera.zoom <= 0.0F)
    return {ComponentPlacementDragMapStatus::InvalidCamera, {}};
  const QPointF pixels = currentLogical - pressLogical;
  if (std::hypot(pixels.x(), pixels.y()) <
      kComponentPlacementDragThresholdPx)
    return {ComponentPlacementDragMapStatus::BelowThreshold, {}};
  const double scale =
      std::min(camera.logicalSize.width(), camera.logicalSize.height()) *
      0.008 * static_cast<double>(camera.zoom);
  if (!std::isfinite(scale) || scale <= 0.0)
    return {ComponentPlacementDragMapStatus::InvalidCamera, {}};
  const double yaw = camera.yawDeg * std::numbers::pi / 180.0;
  const double pitch = camera.pitchDeg * std::numbers::pi / 180.0;
  const double cy = std::cos(yaw);
  const double sy = std::sin(yaw);
  const double cp = std::cos(pitch);
  const double sp = std::sin(pitch);
  const double horizontal = pixels.x() / scale;
  const double vertical = pixels.y() / scale;
  const std::array<double, 3> delta{
      horizontal * cy + vertical * sy * cp,
      -horizontal * sy + vertical * cy * cp,
      -vertical * sp};
  if (!validDelta(delta))
    return {ComponentPlacementDragMapStatus::ValueOutOfRange, {}};
  return {ComponentPlacementDragMapStatus::Mapped, delta};
}

std::optional<ComponentPlacementPreview> ComponentPlacementPreview::create(
    ComponentScenePublicationHandle publication,
    ComponentInstanceAddress component,
    std::array<double, 3> worldDeltaMm,
    std::uint64_t ownershipRevision) noexcept {
  if (!publication.isCurrent() || ownershipRevision == 0 ||
      component.rootInstance() || !component.epoch.isValid() ||
      !component.rootDefinitionId.isValid() ||
      !component.definitionId.isValid() || !validDelta(worldDeltaMm))
    return std::nullopt;
  const auto snapshot = publication.snapshotIfCurrent();
  if (!snapshot || snapshot->epoch() != component.epoch ||
      snapshot->graphTraversal().visits.empty() ||
      snapshot->graphTraversal().visits.front().definitionId !=
          component.rootDefinitionId)
    return std::nullopt;
  const auto found = std::find_if(
      snapshot->graphTraversal().visits.begin(),
      snapshot->graphTraversal().visits.end(),
      [&component](const ComponentGraphVisit& visit) {
        return visit.definitionId == component.definitionId &&
               visit.path == component.occurrencePath;
      });
  if (found == snapshot->graphTraversal().visits.end()) return std::nullopt;
  return ComponentPlacementPreview(std::move(publication),
                                   std::move(component), worldDeltaMm,
                                   ownershipRevision);
}

bool ComponentPlacementPreview::isCurrentFor(
    const ComponentViewportScene& scene,
    std::uint64_t ownershipRevision) const noexcept {
  const auto& snapshot = scene.snapshot();
  return ownershipRevision_ == ownershipRevision && scene.isCurrent() &&
         publication_.isSameCurrentPublication(scene.publicationHandle()) &&
         snapshot && !snapshot->graphTraversal().visits.empty() &&
         component_.epoch == snapshot->epoch() &&
         component_.rootDefinitionId ==
             snapshot->graphTraversal().visits.front().definitionId;
}

bool ComponentPlacementPreview::moves(
    const ComponentInstanceAddress& candidate) const noexcept {
  if (!component_.occurrencePath || !candidate.occurrencePath ||
      candidate.epoch != component_.epoch ||
      candidate.rootDefinitionId != component_.rootDefinitionId)
    return false;
  const auto& prefix = component_.occurrencePath->entries();
  const auto& path = candidate.occurrencePath->entries();
  return path.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), path.begin());
}

}  // namespace solidar
