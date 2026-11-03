#include "presentation/AssemblyScenePublicationCoordinator.h"

#include <thread>
#include <utility>

#include "presentation/AssemblyPresentationBuilder.h"
#include "presentation/internal/ScenePublicationCore.h"

namespace solidar {
namespace detail {

struct AssemblyScenePublicationState final
    : ScenePublicationStateCore<AssemblyPresentationSnapshot> {
  explicit AssemblyScenePublicationState(std::thread::id owner) noexcept
      : ScenePublicationStateCore(owner) {}
};

}  // namespace detail
namespace {

bool isCurrent(
    const std::shared_ptr<detail::AssemblyScenePublicationState>& state,
    AssemblyScenePublicationGeneration generation) noexcept {
  if (!state || std::this_thread::get_id() != state->ownerThread) return false;
  return detail::scenePublicationIsCurrent(state, generation.value());
}

std::shared_ptr<const AssemblyPresentationSnapshot> snapshotIfCurrent(
    const std::shared_ptr<detail::AssemblyScenePublicationState>& state,
    AssemblyScenePublicationGeneration generation) noexcept {
  return detail::scenePublicationSnapshotIfCurrent(state,
                                                   generation.value());
}

AssemblyScenePublicationResult result(
    AssemblyScenePublicationStatus status,
    std::optional<AssemblyPresentationBuildStatus> builderStatus =
        std::nullopt) noexcept {
  AssemblyScenePublicationResult value;
  value.status = status;
  value.builderStatus = builderStatus;
  return value;
}

}  // namespace

bool AssemblyScenePublicationHandle::isCurrent() const noexcept {
  const auto state = state_.lock();
  return solidar::isCurrent(state, generation_);
}

bool AssemblyScenePublicationHandle::isSameCurrentPublication(
    const AssemblyScenePublicationHandle& other) const noexcept {
  if (generation_ != other.generation_ || !generation_.isValid()) return false;
  const auto state = state_.lock();
  const auto otherState = other.state_.lock();
  return state && state == otherState && solidar::isCurrent(state, generation_);
}

std::shared_ptr<const AssemblyPresentationSnapshot>
AssemblyScenePublicationHandle::snapshotIfCurrent() const noexcept {
  const auto state = state_.lock();
  return solidar::snapshotIfCurrent(state, generation_);
}

AssemblyScenePublicationCoordinator::AssemblyScenePublicationCoordinator()
    : state_(std::make_shared<detail::AssemblyScenePublicationState>(
          std::this_thread::get_id())) {}

AssemblyScenePublicationCoordinator::~AssemblyScenePublicationCoordinator()
    noexcept {
  // Destruction is part of the owner-thread contract.  No callback can keep
  // State alive as an active owner: handles contain only weak references.
  detail::destroyScenePublication(state_);
}

bool AssemblyScenePublicationCoordinator::isOwnerThread() const noexcept {
  return std::this_thread::get_id() == state_->ownerThread;
}

AssemblyScenePublicationResult
AssemblyScenePublicationCoordinator::rebuildAndPublish(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile, const Consumer& consumer) noexcept {
  if (!isOwnerThread()) return result(AssemblyScenePublicationStatus::WrongThread);

  const auto state = state_;
  const AssemblyPresentationBuildResult built =
      AssemblyPresentationBuilder::build(product, epoch, meshProfile);
  const bool successStatus =
      built.status == AssemblyPresentationBuildStatus::Success;
  const bool hasSnapshot = built.snapshot != nullptr;
  if (successStatus != hasSnapshot) {
    return result(AssemblyScenePublicationStatus::InvalidBuildResult,
                  built.status);
  }
  if (!successStatus) {
    return result(AssemblyScenePublicationStatus::BuildFailed, built.status);
  }

  const auto committed = detail::commitScenePublication(
      state, built.snapshot, consumer,
      [state](std::uint64_t generation) {
        return AssemblyScenePublicationHandle{
            state, AssemblyScenePublicationGeneration{generation}};
      });
  if (committed.status ==
      detail::ScenePublicationCoreStatus::GenerationExhausted)
    return result(AssemblyScenePublicationStatus::GenerationExhausted,
                  built.status);
  if (committed.status ==
      detail::ScenePublicationCoreStatus::PreparationFailed)
    return result(AssemblyScenePublicationStatus::PreparationFailed,
                  built.status);

  // The callback may have re-entered publication, cleared the state, or
  // destroyed the coordinator.  From this point onward use only the strong
  // local State and never access this.
  AssemblyScenePublicationResult published =
      result(AssemblyScenePublicationStatus::Published, built.status);
  switch (committed.notification) {
    case detail::ScenePublicationCoreNotification::NotRun:
      published.notification = AssemblySceneNotificationStatus::NotRun;
      break;
    case detail::ScenePublicationCoreNotification::Delivered:
      published.notification = AssemblySceneNotificationStatus::Delivered;
      break;
    case detail::ScenePublicationCoreNotification::StandardException:
      published.notification =
          AssemblySceneNotificationStatus::StandardException;
      break;
    case detail::ScenePublicationCoreNotification::UnknownException:
      published.notification = AssemblySceneNotificationStatus::UnknownException;
      break;
  }
  published.publishedGeneration =
      AssemblyScenePublicationGeneration{committed.generation};
  published.stillCurrent = committed.stillCurrent;
  return published;
}

AssemblySceneClearStatus AssemblyScenePublicationCoordinator::clear() noexcept {
  if (!isOwnerThread()) return AssemblySceneClearStatus::WrongThread;
  if (!state_->active) return AssemblySceneClearStatus::AlreadyClear;
  decltype(state_->active) empty;
  state_->active.swap(empty);
  return AssemblySceneClearStatus::Cleared;
}

AssemblyScenePublicationHandle AssemblyScenePublicationCoordinator::active()
    const noexcept {
  if (!isOwnerThread() || !state_->alive || !state_->active) return {};
  return AssemblyScenePublicationHandle{
      state_, AssemblyScenePublicationGeneration{state_->active->generation}};
}

bool AssemblyScenePublicationCoordinator::seedGenerationForTesting(
    std::uint64_t value) noexcept {
  if (!isOwnerThread() || !state_->alive || state_->active || value == 0 ||
      value < state_->generationClock)
    return false;
  state_->generationClock = value;
  return true;
}

}  // namespace solidar
