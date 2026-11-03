#include "ui/AssemblyViewportRuler.h"

#include <cmath>

namespace solidar {

bool AssemblyViewportRuler::exactAndCurrent(
    const AssemblyViewportSnapHit& hit) noexcept {
  return hit.publication.isCurrent() && hit.occurrence.epoch.isValid() &&
         hit.occurrence.rootAssemblyDefinitionId.isValid() &&
         hit.occurrence.path.depth() > 0;
}

bool AssemblyViewportRuler::compatible(
    const AssemblyViewportSnapHit& hit) const noexcept {
  if (!exactAndCurrent(hit)) return false;
  return !first_ || first_->publication.isSameCurrentPublication(
                        hit.publication);
}

void AssemblyViewportRuler::clear() noexcept {
  first_.reset();
  second_.reset();
  hover_.reset();
}

void AssemblyViewportRuler::updateHover(
    const AssemblyViewportSnapQueryResult& query) noexcept {
  if (query.status == AssemblyViewportPickingQueryStatus::Uncertain) return;
  if (query.status == AssemblyViewportPickingQueryStatus::Hit && query.hit &&
      compatible(*query.hit)) {
    hover_ = query.hit;
    return;
  }
  if (query.status == AssemblyViewportPickingQueryStatus::StalePublication ||
      (query.hit && !compatible(*query.hit))) {
    clear();
    return;
  }
  hover_.reset();
}

AssemblyViewportRulerCommitStatus AssemblyViewportRuler::commit(
    const AssemblyViewportSnapQueryResult& query) noexcept {
  if (query.status == AssemblyViewportPickingQueryStatus::Uncertain)
    return AssemblyViewportRulerCommitStatus::RejectedUncertain;
  if (query.status != AssemblyViewportPickingQueryStatus::Hit || !query.hit)
    return AssemblyViewportRulerCommitStatus::RejectedMiss;
  if (!compatible(*query.hit)) {
    clear();
    return AssemblyViewportRulerCommitStatus::RejectedStaleOrForeign;
  }
  if (!first_) {
    first_ = query.hit;
    second_.reset();
    hover_ = query.hit;
    return AssemblyViewportRulerCommitStatus::AcceptedFirstPoint;
  }
  second_ = query.hit;
  hover_ = query.hit;
  return AssemblyViewportRulerCommitStatus::AcceptedSecondPoint;
}

void AssemblyViewportRuler::validateCurrent(
    const AssemblyScenePublicationHandle& currentPublication) noexcept {
  if ((first_ && !first_->publication.isSameCurrentPublication(
                     currentPublication)) ||
      (second_ && !second_->publication.isSameCurrentPublication(
                      currentPublication)) ||
      (hover_ && !hover_->publication.isSameCurrentPublication(
                     currentPublication)))
    clear();
}

std::optional<double> AssemblyViewportRuler::distance() const noexcept {
  if (!first_ || !second_ ||
      !first_->publication.isSameCurrentPublication(second_->publication))
    return std::nullopt;
  const double dx = second_->worldPoint.x - first_->worldPoint.x;
  const double dy = second_->worldPoint.y - first_->worldPoint.y;
  const double dz = second_->worldPoint.z - first_->worldPoint.z;
  const double value = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!std::isfinite(value)) return std::nullopt;
  return value;
}

}  // namespace solidar
