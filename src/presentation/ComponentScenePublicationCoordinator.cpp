#include "presentation/ComponentScenePublicationCoordinator.h"

#include <thread>
#include <utility>

#include "presentation/ComponentPresentationBuilder.h"
#include "presentation/internal/ScenePublicationCore.h"

namespace solidar {
namespace detail {

struct ComponentScenePublicationState final
    : ScenePublicationStateCore<ComponentPresentationSnapshot> {
  explicit ComponentScenePublicationState(std::thread::id owner) noexcept
      : ScenePublicationStateCore(owner) {}
};

}  // namespace detail
namespace {

bool isCurrent(
    const std::shared_ptr<detail::ComponentScenePublicationState>& state,
    ComponentScenePublicationGeneration generation) noexcept {
  return detail::scenePublicationIsCurrent(state, generation.value());
}

std::shared_ptr<const ComponentPresentationSnapshot> snapshotIfCurrent(
    const std::shared_ptr<detail::ComponentScenePublicationState>& state,
    ComponentScenePublicationGeneration generation) noexcept {
  return detail::scenePublicationSnapshotIfCurrent(state,
                                                   generation.value());
}

ComponentScenePublicationResult result(
    ComponentScenePublicationStatus status,
    std::optional<ComponentPresentationBuildStatus> builderStatus =
        std::nullopt) noexcept {
  ComponentScenePublicationResult value;
  value.status = status;
  value.builderStatus = builderStatus;
  return value;
}

}  // namespace

bool ComponentScenePublicationHandle::isCurrent() const noexcept {
  return solidar::isCurrent(state_.lock(), generation_);
}

bool ComponentScenePublicationHandle::isSameCurrentPublication(
    const ComponentScenePublicationHandle& other) const noexcept {
  if (generation_ != other.generation_ || !generation_.isValid()) return false;
  const auto state = state_.lock();
  const auto otherState = other.state_.lock();
  return state && state == otherState && solidar::isCurrent(state, generation_);
}

std::shared_ptr<const ComponentPresentationSnapshot>
ComponentScenePublicationHandle::snapshotIfCurrent() const noexcept {
  return solidar::snapshotIfCurrent(state_.lock(), generation_);
}

ComponentScenePublicationCoordinator::ComponentScenePublicationCoordinator()
    : state_(std::make_shared<detail::ComponentScenePublicationState>(
          std::this_thread::get_id())) {}

ComponentScenePublicationCoordinator::~ComponentScenePublicationCoordinator()
    noexcept {
  detail::destroyScenePublication(state_);
}

bool ComponentScenePublicationCoordinator::isOwnerThread() const noexcept {
  return std::this_thread::get_id() == state_->ownerThread;
}

ComponentScenePublicationResult
ComponentScenePublicationCoordinator::rebuildAndPublish(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile, const Consumer& consumer) noexcept {
  if (!isOwnerThread())
    return result(ComponentScenePublicationStatus::WrongThread);

  ComponentPresentationBuildResult built =
      ComponentPresentationBuilder::build(product, epoch, meshProfile);
  return publishBuildResult(std::move(built), consumer);
}

ComponentScenePublicationResult
ComponentScenePublicationCoordinator::rebuildAndPublishProjectedIdentity(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    const ComponentIdentityTransition& transition,
    const Consumer& consumer) noexcept {
  if (!isOwnerThread())
    return result(ComponentScenePublicationStatus::WrongThread);
  ComponentPresentationBuildResult built = ComponentPresentationBuilder::build(
      product, epoch, meshProfile, transition);
  return publishBuildResult(std::move(built), consumer);
}

ComponentScenePublicationResult
ComponentScenePublicationCoordinator::publishBuildResult(
    ComponentPresentationBuildResult built,
    const Consumer& consumer) noexcept {
  const auto state = state_;
  const bool successStatus =
      built.status == ComponentPresentationBuildStatus::Success;
  const bool hasSnapshot = built.snapshot != nullptr;
  if (successStatus != hasSnapshot)
    return result(ComponentScenePublicationStatus::InvalidBuildResult,
                  built.status);
  if (!successStatus)
    return result(ComponentScenePublicationStatus::BuildFailed, built.status);

  const auto committed = detail::commitScenePublication(
      state, built.snapshot, consumer,
      [state](std::uint64_t generation) {
        return ComponentScenePublicationHandle{
            state, ComponentScenePublicationGeneration{generation}};
      });
  if (committed.status ==
      detail::ScenePublicationCoreStatus::GenerationExhausted)
    return result(ComponentScenePublicationStatus::GenerationExhausted,
                  built.status);
  if (committed.status ==
      detail::ScenePublicationCoreStatus::PreparationFailed)
    return result(ComponentScenePublicationStatus::PreparationFailed,
                  built.status);

  ComponentScenePublicationResult published =
      result(ComponentScenePublicationStatus::Published, built.status);
  switch (committed.notification) {
    case detail::ScenePublicationCoreNotification::NotRun:
      published.notification = ComponentSceneNotificationStatus::NotRun;
      break;
    case detail::ScenePublicationCoreNotification::Delivered:
      published.notification = ComponentSceneNotificationStatus::Delivered;
      break;
    case detail::ScenePublicationCoreNotification::StandardException:
      published.notification =
          ComponentSceneNotificationStatus::StandardException;
      break;
    case detail::ScenePublicationCoreNotification::UnknownException:
      published.notification =
          ComponentSceneNotificationStatus::UnknownException;
      break;
  }
  published.publishedGeneration =
      ComponentScenePublicationGeneration{committed.generation};
  published.stillCurrent = committed.stillCurrent;
  return published;
}

ComponentSceneClearStatus ComponentScenePublicationCoordinator::clear()
    noexcept {
  if (!isOwnerThread()) return ComponentSceneClearStatus::WrongThread;
  if (!state_->active) return ComponentSceneClearStatus::AlreadyClear;
  decltype(state_->active) empty;
  state_->active.swap(empty);
  return ComponentSceneClearStatus::Cleared;
}

ComponentScenePublicationHandle ComponentScenePublicationCoordinator::active()
    const noexcept {
  if (!isOwnerThread() || !state_->alive || !state_->active) return {};
  return ComponentScenePublicationHandle{
      state_, ComponentScenePublicationGeneration{state_->active->generation}};
}

bool ComponentScenePublicationCoordinator::seedGenerationForTesting(
    std::uint64_t value) noexcept {
  if (!isOwnerThread() || !state_->alive || state_->active || value == 0 ||
      value < state_->generationClock)
    return false;
  state_->generationClock = value;
  return true;
}

}  // namespace solidar
