#include "ui/AssemblyViewportSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>

#include "model/ProductDocument.h"
#include "model/AssemblyGraphAnalysis.h"
#include "ui/RigidTransform3dOps.h"

namespace solidar {
namespace {

std::optional<RigidTransform3d> inverseTransform(
    const RigidTransform3d& transform) noexcept {
  const auto& q = transform.quaternionWxyz();
  const auto& t = transform.translationMm();
  const std::array<double, 4> inverseQ{q[0], -q[1], -q[2], -q[3]};
  const double w = inverseQ[0];
  const double x = inverseQ[1];
  const double y = inverseQ[2];
  const double z = inverseQ[3];
  const std::array<double, 3> inverseT{
      -(1.0 - 2.0 * (y * y + z * z)) * t[0] -
          2.0 * (x * y - z * w) * t[1] -
          2.0 * (x * z + y * w) * t[2],
      -2.0 * (x * y + z * w) * t[0] -
          (1.0 - 2.0 * (x * x + z * z)) * t[1] -
          2.0 * (y * z - x * w) * t[2],
      -2.0 * (x * z - y * w) * t[0] -
          2.0 * (y * z + x * w) * t[1] -
          (1.0 - 2.0 * (x * x + y * y)) * t[2]};
  const auto result = RigidTransform3d::create(inverseT, inverseQ);
  return result ? result.transform : std::nullopt;
}

std::optional<Bounds3d> previewBounds(
    const BodyRenderMesh& mesh,
    const RigidTransform3d& localToWorld) noexcept {
  if (!mesh.localBounds()) return std::nullopt;
  const auto matrix = rigidTransformMatrix(localToWorld);
  if (!matrix) return std::nullopt;
  std::optional<Bounds3d> result;
  for (const auto corner : mesh.localBounds()->corners()) {
    const auto point = transformPoint(*matrix, corner);
    if (!point) return std::nullopt;
    const auto one = Bounds3d::create(*point, *point);
    if (!one) return std::nullopt;
    result = result ? result->united(*one) : one;
    if (!result) return std::nullopt;
  }
  return result;
}

bool sameOccurrence(const AssemblyOccurrenceIdentity& left,
                    const AssemblyOccurrenceIdentity& right) noexcept {
  return left == right;
}

bool occurrenceHasInstance(
    const AssemblyViewportScene& scene,
    const AssemblyOccurrenceIdentity& occurrence) noexcept {
  return std::any_of(scene.instances().begin(), scene.instances().end(),
                     [&occurrence](const auto& instance) {
                       return instance.occurrenceIdentity == occurrence;
                     });
}

bool populateOccurrenceStates(const AssemblyPresentationSnapshot& snapshot,
                              DefinitionId rootDefinitionId,
                              std::vector<AssemblyOccurrenceSessionState>* out) {
  if (!out) return false;
  const auto& graph = snapshot.graphAnalysis();
  if (!graph) return false;
  out->clear();
  out->reserve(graph.visits.size());
  for (const auto& visit : graph.visits) {
    out->push_back({{snapshot.epoch(), rootDefinitionId, visit.path},
                    visit.targetDefinitionId, visit.authoredFlags.visible,
                    visit.authoredFlags.suppressed,
                    visit.authoredFlags.grounded,
                    visit.effectiveVisible && !visit.effectiveSuppressed});
  }
  return true;
}

}  // namespace

AssemblyViewportSession::PrepareResult AssemblyViewportSession::prepare(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint) noexcept {
  if (!epoch.isValid())
    return {AssemblyViewportSessionStatus::InvalidEpoch, {}};
  const AssemblyDefinition* root = product.rootAssemblyDefinition();
  if (!root)
    return {AssemblyViewportSessionStatus::InvalidProduct, {}};
  if (failurePoint == AssemblyViewportSessionFailurePoint::Resource)
    return {AssemblyViewportSessionStatus::ResourceFailure, {}};
  if (failurePoint == AssemblyViewportSessionFailurePoint::Presentation)
    return {AssemblyViewportSessionStatus::PresentationFailed, {}};

  try {
    auto candidate = std::unique_ptr<AssemblyViewportSession>(
        new AssemblyViewportSession);
    candidate->epoch_ = epoch;
    const auto publication = candidate->coordinator_.rebuildAndPublish(
        product, epoch, meshProfile);
    if (!publication)
      return {AssemblyViewportSessionStatus::PresentationFailed, {}};
    if (failurePoint == AssemblyViewportSessionFailurePoint::Scene)
      return {AssemblyViewportSessionStatus::SceneFailed, {}};

    const AssemblyScenePublicationHandle handle = candidate->coordinator_.active();
    const auto presentation = handle.snapshotIfCurrent();
    if (!presentation ||
        !populateOccurrenceStates(*presentation, product.rootDefinitionId(),
                                  &candidate->occurrences_))
      return {AssemblyViewportSessionStatus::InvalidProduct, {}};
    const auto sceneResult = candidate->sceneAdapter_.publish(handle);
    if (!sceneResult)
      return {AssemblyViewportSessionStatus::SceneFailed, {}};
    candidate->scene_ = candidate->sceneAdapter_.active();
    candidate->authoredScene_ = candidate->scene_;
    if (!candidate->scene_ || !candidate->scene_->isCurrent())
      return {AssemblyViewportSessionStatus::SceneFailed, {}};
    if (failurePoint == AssemblyViewportSessionFailurePoint::Picking)
      return {AssemblyViewportSessionStatus::PickingFailed, {}};

    const auto pickingResult = candidate->picking_.publish(candidate->scene_);
    if (!pickingResult)
      return {AssemblyViewportSessionStatus::PickingFailed, {}};
    if (failurePoint == AssemblyViewportSessionFailurePoint::Spatial ||
        failurePoint == AssemblyViewportSessionFailurePoint::Selection)
      return {AssemblyViewportSessionStatus::PreparationFailed, {}};
    if (failurePoint == AssemblyViewportSessionFailurePoint::FinalPreparation)
      return {AssemblyViewportSessionStatus::PreparationFailed, {}};
    if (!handle.isCurrent() || !candidate->scene_->isCurrent())
      return {AssemblyViewportSessionStatus::PreparationFailed, {}};
    return {AssemblyViewportSessionStatus::Prepared, std::move(candidate)};
  } catch (const std::bad_alloc&) {
    return {AssemblyViewportSessionStatus::ResourceFailure, {}};
  } catch (const std::exception&) {
    return {AssemblyViewportSessionStatus::PreparationFailed, {}};
  } catch (...) {
    return {AssemblyViewportSessionStatus::PreparationFailed, {}};
  }
}

AssemblyViewportSession::PrepareResult
AssemblyViewportSession::prepareVisibilityProjection(
    const ProductDocument& product,
    const AssemblyViewportSession& incumbent,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint) noexcept {
  return prepareSameEpochOccurrenceProjection(product, incumbent, meshProfile,
                                               failurePoint);
}

AssemblyViewportSession::PrepareResult
AssemblyViewportSession::prepareGroundingProjection(
    const ProductDocument& product,
    const AssemblyViewportSession& incumbent,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint) noexcept {
  return prepareSameEpochOccurrenceProjection(product, incumbent, meshProfile,
                                               failurePoint);
}

AssemblyViewportSession::PrepareResult
AssemblyViewportSession::prepareSuppressionProjection(
    const ProductDocument& product,
    const AssemblyViewportSession& incumbent,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint) noexcept {
  return prepareSameEpochOccurrenceProjection(product, incumbent, meshProfile,
                                               failurePoint);
}

AssemblyViewportSession::PrepareResult
AssemblyViewportSession::prepareDuplicateProjection(
    const ProductDocument& product,
    const AssemblyViewportSession& incumbent,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint) noexcept {
  return prepareSameEpochOccurrenceProjection(product, incumbent, meshProfile,
                                               failurePoint);
}

AssemblyViewportSession::PrepareResult
AssemblyViewportSession::prepareRemoveProjection(
    const ProductDocument& product,
    const AssemblyViewportSession& incumbent,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint) noexcept {
  return prepareSameEpochOccurrenceProjection(product, incumbent, meshProfile,
                                               failurePoint);
}

AssemblyViewportSession::PrepareResult
AssemblyViewportSession::prepareDefinitionProjection(
    const ProductDocument& product,
    const AssemblyViewportSession& incumbent,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint) noexcept {
  return prepareSameEpochOccurrenceProjection(product, incumbent, meshProfile,
                                               failurePoint);
}

AssemblyViewportSession::PrepareResult
AssemblyViewportSession::prepareSameEpochOccurrenceProjection(
    const ProductDocument& product,
    const AssemblyViewportSession& incumbent,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint) noexcept {
  if (!incumbent.epoch_.isValid())
    return {AssemblyViewportSessionStatus::InvalidEpoch, {}};
  const AssemblyDefinition* root = product.rootAssemblyDefinition();
  if (!root)
    return {AssemblyViewportSessionStatus::InvalidProduct, {}};
  if (failurePoint == AssemblyViewportSessionFailurePoint::Resource)
    return {AssemblyViewportSessionStatus::ResourceFailure, {}};
  if (failurePoint == AssemblyViewportSessionFailurePoint::Presentation)
    return {AssemblyViewportSessionStatus::PresentationFailed, {}};
  try {
    auto candidate = std::unique_ptr<AssemblyViewportSession>(
        new AssemblyViewportSession);
    candidate->epoch_ = incumbent.epoch_;
    if (!candidate->sceneAdapter_.seedReusableSourcesFrom(
            incumbent.sceneAdapter_) ||
        !candidate->picking_.seedReusableSourcesFrom(incumbent.picking_))
      return {AssemblyViewportSessionStatus::ResourceFailure, {}};
    const auto publication = candidate->coordinator_.rebuildAndPublish(
        product, candidate->epoch_, meshProfile);
    if (!publication)
      return {AssemblyViewportSessionStatus::PresentationFailed, {}};
    if (failurePoint == AssemblyViewportSessionFailurePoint::Scene)
      return {AssemblyViewportSessionStatus::SceneFailed, {}};
    const auto handle = candidate->coordinator_.active();
    const auto presentation = handle.snapshotIfCurrent();
    if (!presentation ||
        !populateOccurrenceStates(*presentation, product.rootDefinitionId(),
                                  &candidate->occurrences_))
      return {AssemblyViewportSessionStatus::InvalidProduct, {}};
    if (!candidate->sceneAdapter_.publish(handle))
      return {AssemblyViewportSessionStatus::SceneFailed, {}};
    candidate->scene_ = candidate->sceneAdapter_.active();
    candidate->authoredScene_ = candidate->scene_;
    if (!candidate->scene_ || !candidate->scene_->isCurrent())
      return {AssemblyViewportSessionStatus::SceneFailed, {}};
    if (failurePoint == AssemblyViewportSessionFailurePoint::Picking)
      return {AssemblyViewportSessionStatus::PickingFailed, {}};
    if (!candidate->picking_.publish(candidate->scene_))
      return {AssemblyViewportSessionStatus::PickingFailed, {}};
    if (failurePoint == AssemblyViewportSessionFailurePoint::Spatial)
      return {AssemblyViewportSessionStatus::PreparationFailed, {}};
    if (failurePoint == AssemblyViewportSessionFailurePoint::Selection)
      return {AssemblyViewportSessionStatus::PreparationFailed, {}};
    if (failurePoint == AssemblyViewportSessionFailurePoint::FinalPreparation)
      return {AssemblyViewportSessionStatus::PreparationFailed, {}};
    return {AssemblyViewportSessionStatus::Prepared, std::move(candidate)};
  } catch (const std::bad_alloc&) {
    return {AssemblyViewportSessionStatus::ResourceFailure, {}};
  } catch (...) {
    return {AssemblyViewportSessionStatus::PreparationFailed, {}};
  }
}

AssemblyViewportSession::~AssemblyViewportSession() noexcept {
  clearInteraction();
  picking_.clear();
  scene_.reset();
  authoredScene_.reset();
  sceneAdapter_.clear();
  static_cast<void>(coordinator_.clear());
}

bool AssemblyViewportSession::canIsolate(
    const AssemblyOccurrenceIdentity& occurrence) const noexcept {
  const auto* state = findOccurrence(occurrence);
  const auto& base = authoredScene_ ? authoredScene_ : scene_;
  if (!state || !state->effectivelyVisible || !base || !base->isCurrent() ||
      !occurrenceHasInstance(*base, occurrence))
    return false;
  return std::any_of(base->instances().begin(), base->instances().end(),
                     [&occurrence](const auto& instance) {
                       return instance.occurrenceIdentity != occurrence;
                     });
}

PrepareAssemblyIsolationResult AssemblyViewportSession::prepareIsolation(
    const std::optional<AssemblyOccurrenceIdentity>& occurrence,
    AssemblyIsolationFailurePoint failurePoint) const noexcept {
  const auto& base = authoredScene_ ? authoredScene_ : scene_;
  if (!base || !base->isCurrent())
    return {PrepareAssemblyIsolationStatus::InvalidOccurrence, {}};
  if (occurrence && !canIsolate(*occurrence))
    return {PrepareAssemblyIsolationStatus::NotMeaningful, {}};
  if (!occurrence && !isolatedOccurrence_)
    return {PrepareAssemblyIsolationStatus::NotMeaningful, {}};
  if (failurePoint == AssemblyIsolationFailurePoint::Resource)
    return {PrepareAssemblyIsolationStatus::ResourceFailure, {}};
  if (failurePoint == AssemblyIsolationFailurePoint::Scene)
    return {PrepareAssemblyIsolationStatus::InvalidOccurrence, {}};
  try {
    std::shared_ptr<const AssemblyViewportScene> displayed = base;
    if (occurrence) {
      std::vector<AssemblyViewportSceneSource> retainedSources =
          base->sources();
      std::vector<AssemblyViewportSceneInstance> filtered;
      std::optional<Bounds3d> bounds;
      for (const auto& instance : base->instances()) {
        if (instance.occurrenceIdentity != *occurrence) continue;
        filtered.push_back(instance);
        bounds = bounds ? bounds->united(instance.worldBounds)
                        : std::optional<Bounds3d>{instance.worldBounds};
        if (!bounds)
          return {PrepareAssemblyIsolationStatus::InvalidOccurrence, {}};
      }
      if (filtered.empty())
        return {PrepareAssemblyIsolationStatus::InvalidOccurrence, {}};
      displayed = std::shared_ptr<const AssemblyViewportScene>(
          new AssemblyViewportScene(base->publicationHandle(), base->snapshot(),
                                    std::move(retainedSources),
                                    std::move(filtered), std::move(bounds)));
    }

    if (failurePoint == AssemblyIsolationFailurePoint::Picking)
      return {PrepareAssemblyIsolationStatus::InvalidOccurrence, {}};
    auto preparedPicking = std::make_unique<AssemblyViewportPicking>();
    if (!preparedPicking->seedReusableSourcesFrom(picking_) ||
        !preparedPicking->publish(displayed))
      return {PrepareAssemblyIsolationStatus::ResourceFailure, {}};

    PreparedAssemblySelectionState selection;
    selection.selection = selectedOccurrence_;
    if (selection.selection &&
        occurrenceHasInstance(*displayed,
                              selection.selection->occurrence)) {
      selection.highlight = AssemblyOccurrenceHighlightTarget::create(
          selection.selection->publication,
          selection.selection->occurrence);
    }
    PreparedAssemblyIsolationState state;
    state.displayedScene = std::move(displayed);
    state.picking = std::move(preparedPicking);
    state.isolatedOccurrence = occurrence;
    state.selection = std::move(selection);
    return {PrepareAssemblyIsolationStatus::Prepared, std::move(state)};
  } catch (const std::bad_alloc&) {
    return {PrepareAssemblyIsolationStatus::ResourceFailure, {}};
  } catch (...) {
    return {PrepareAssemblyIsolationStatus::InvalidOccurrence, {}};
  }
}

void AssemblyViewportSession::installPreparedIsolation(
    PreparedAssemblyIsolationState&& prepared) noexcept {
  if (!prepared.displayedScene || !prepared.picking) return;
  static_assert(std::is_nothrow_move_assignable_v<
                std::optional<AssemblyOccurrenceIdentity>>);
  scene_.swap(prepared.displayedScene);
  picking_.swapPrepared(*prepared.picking);
  isolatedOccurrence_ = std::move(prepared.isolatedOccurrence);
  installPreparedSelectedOccurrence(std::move(prepared.selection));
  hoveredOccurrence_.reset();
  hoveredHighlight_.reset();
}

ViewportCameraState AssemblyViewportSession::camera(
    float yawDeg, float pitchDeg, float zoom, QPointF pan,
    QSize logicalSize) const noexcept {
  Point3d center{};
  double depthExtent = 1.0;
  if (scene_ && scene_->worldBounds()) {
    center = scene_->worldBounds()->center();
    depthExtent = std::max(1.0, scene_->worldBounds()->diagonal() * 3.0);
  }
  return {yawDeg, pitchDeg, zoom, pan, logicalSize, 1.0F, center,
          depthExtent};
}

AssemblyFaceRayQueryResult AssemblyViewportSession::faceAt(
    const ViewportCameraState& cameraState,
    QPointF logicalPixel) const noexcept {
  return picking_.faceAt(cameraState, logicalPixel);
}

std::shared_ptr<const AssemblyViewportScene>
AssemblyViewportSession::prepareMatePreview(
    const MatePlacementCandidate& candidate) const noexcept {
  const auto& base = authoredScene_ ? authoredScene_ : scene_;
  if (!base || !base->isCurrent() || base->instances().empty() ||
      candidate.address().rootDefinitionId !=
          base->instances().front().occurrenceIdentity.rootAssemblyDefinitionId)
    return {};
  if (candidate.changes().empty()) return base;
  try {
    struct Delta final {
      OccurrenceId id;
      RigidTransform3d transform;
    };
    std::vector<Delta> deltas;
    deltas.reserve(candidate.changes().size());
    for (const auto& change : candidate.changes()) {
      const auto inverse = inverseTransform(change.before.placement);
      if (!inverse) return {};
      const auto composed =
          RigidTransform3d::compose(change.afterPlacement, *inverse);
      if (!composed) return {};
      deltas.push_back({change.before.occurrenceId, *composed.transform});
    }
    std::vector<AssemblyViewportSceneInstance> instances = base->instances();
    std::optional<Bounds3d> aggregate;
    bool changedAny = false;
    for (auto& instance : instances) {
      if (instance.occurrenceIdentity.path.entries().empty()) return {};
      const auto id = instance.occurrenceIdentity.path.entries().front();
      const auto found = std::find_if(
          deltas.begin(), deltas.end(),
          [id](const Delta& delta) { return delta.id == id; });
      if (found != deltas.end()) {
        const auto transformed =
            RigidTransform3d::compose(found->transform, instance.localToWorld);
        if (!transformed || !instance.mesh) return {};
        const auto bounds = previewBounds(*instance.mesh, *transformed.transform);
        if (!bounds) return {};
        instance.localToWorld = *transformed.transform;
        instance.worldBounds = *bounds;
        changedAny = true;
      }
      aggregate = aggregate ? aggregate->united(instance.worldBounds)
                            : std::optional<Bounds3d>{instance.worldBounds};
      if (!aggregate) return {};
    }
    if (!changedAny) return {};
    return std::shared_ptr<const AssemblyViewportScene>(
        new AssemblyViewportScene(base->publicationHandle(), base->snapshot(),
                                  base->sources(), std::move(instances),
                                  std::move(aggregate)));
  } catch (const std::bad_alloc&) {
    return {};
  } catch (...) {
    return {};
  }
}

const AssemblyOccurrenceSessionState* AssemblyViewportSession::findOccurrence(
    const AssemblyOccurrenceIdentity& occurrence) const noexcept {
  const auto found = std::find_if(
      occurrences_.begin(), occurrences_.end(),
      [&occurrence](const auto& candidate) {
        return sameOccurrence(candidate.identity, occurrence);
      });
  return found == occurrences_.end() ? nullptr : &*found;
}

std::optional<AssemblyOccurrenceHighlightTarget>
AssemblyViewportSession::highlightFor(
    const AssemblyOccurrenceSelection& selection) const {
  const auto& occurrence = selection.occurrence;
  const auto* state = findOccurrence(occurrence);
  if (!state || !state->effectivelyVisible || !scene_ || !scene_->isCurrent())
    return std::nullopt;
  const bool hasVisibleInstance = std::any_of(
      scene_->instances().begin(), scene_->instances().end(),
      [&occurrence](const auto& instance) {
        return instance.occurrenceIdentity == occurrence;
      });
  if (!hasVisibleInstance) return std::nullopt;
  return AssemblyOccurrenceHighlightTarget::create(
      selection.publication, occurrence);
}

bool AssemblyViewportSession::setSelectedOccurrence(
    const std::optional<AssemblyOccurrenceSelection>& selection) noexcept {
  if (!selection) {
    selectedOccurrence_.reset();
    selectedHighlight_.reset();
    return true;
  }
  auto prepared = prepareSelectedOccurrence(selection);
  if (prepared.status != PrepareAssemblySelectionStatus::Prepared)
    return false;
  installPreparedSelectedOccurrence(std::move(prepared.state));
  return true;
}

PrepareAssemblySelectionResult
AssemblyViewportSession::prepareSelectedOccurrence(
    const std::optional<AssemblyOccurrenceSelection>& selection) const noexcept {
  if (!selection)
    return {PrepareAssemblySelectionStatus::Prepared, {}};
  if (!scene_ || !scene_->isCurrent() ||
      !selection->isCurrentFor(scene_->publicationHandle()) ||
      !findOccurrence(selection->occurrence))
    return {PrepareAssemblySelectionStatus::Rejected, {}};
  try {
    PreparedAssemblySelectionState state;
    state.selection = *selection;
    state.highlight = highlightFor(*selection);
    return {PrepareAssemblySelectionStatus::Prepared, std::move(state)};
  } catch (const std::bad_alloc&) {
    return {PrepareAssemblySelectionStatus::ResourceFailure, {}};
  } catch (...) {
    return {PrepareAssemblySelectionStatus::Rejected, {}};
  }
}

void AssemblyViewportSession::installPreparedSelectedOccurrence(
    PreparedAssemblySelectionState&& prepared) noexcept {
  static_assert(std::is_nothrow_move_assignable_v<
                std::optional<AssemblyOccurrenceSelection>>);
  static_assert(std::is_nothrow_move_assignable_v<
                std::optional<AssemblyOccurrenceHighlightTarget>>);
  selectedOccurrence_ = std::move(prepared.selection);
  selectedHighlight_ = std::move(prepared.highlight);
}

bool AssemblyViewportSession::setHoveredOccurrence(
    const std::optional<AssemblyOccurrenceSelection>& selection) noexcept {
  if (!selection) {
    hoveredOccurrence_.reset();
    hoveredHighlight_.reset();
    return true;
  }
  auto prepared = prepareSelectedOccurrence(selection);
  if (prepared.status != PrepareAssemblySelectionStatus::Prepared)
    return false;
  hoveredOccurrence_ = std::move(prepared.state.selection);
  hoveredHighlight_ = std::move(prepared.state.highlight);
  return true;
}

void AssemblyViewportSession::clearInteraction() noexcept {
  selectedOccurrence_.reset();
  hoveredOccurrence_.reset();
  selectedHighlight_.reset();
  hoveredHighlight_.reset();
}

}  // namespace solidar
