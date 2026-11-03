#pragma once

#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace solidar::detail {

template <typename Snapshot>
struct ScenePublicationActive final {
  const std::uint64_t generation;
  const std::shared_ptr<const Snapshot> snapshot;
};

template <typename Snapshot>
struct ScenePublicationStateCore {
  explicit ScenePublicationStateCore(std::thread::id owner) noexcept
      : ownerThread(owner) {}

  const std::thread::id ownerThread;
  bool alive{true};
  std::uint64_t generationClock{};
  std::shared_ptr<const ScenePublicationActive<Snapshot>> active;
};

template <typename State>
[[nodiscard]] bool scenePublicationIsCurrent(
    const std::shared_ptr<State>& state, std::uint64_t generation) noexcept {
  if (!state || std::this_thread::get_id() != state->ownerThread) return false;
  return state->alive && generation != 0 && state->active &&
         state->active->generation == generation;
}

template <typename State>
[[nodiscard]] auto scenePublicationSnapshotIfCurrent(
    const std::shared_ptr<State>& state, std::uint64_t generation) noexcept
    -> decltype(state->active->snapshot) {
  if (!scenePublicationIsCurrent(state, generation)) return {};
  return state->active->snapshot;
}

enum class ScenePublicationCoreStatus {
  Published,
  GenerationExhausted,
  PreparationFailed,
};

enum class ScenePublicationCoreNotification {
  NotRun,
  Delivered,
  StandardException,
  UnknownException,
};

struct ScenePublicationCoreResult final {
  ScenePublicationCoreStatus status{
      ScenePublicationCoreStatus::PreparationFailed};
  ScenePublicationCoreNotification notification{
      ScenePublicationCoreNotification::NotRun};
  std::uint64_t generation{};
  bool stillCurrent{};
};

// Shared prepare-before-swap lifecycle for typed scene coordinators. Handle
// construction remains in the public coordinator, so generation types and
// exact coordinator identity never cross this internal boundary.
template <typename State, typename Snapshot, typename Consumer,
          typename MakeHandle>
[[nodiscard]] ScenePublicationCoreResult commitScenePublication(
    const std::shared_ptr<State>& state,
    std::shared_ptr<const Snapshot> snapshot, const Consumer& consumer,
    MakeHandle&& makeHandle) noexcept {
  if (state->generationClock == std::numeric_limits<std::uint64_t>::max())
    return {ScenePublicationCoreStatus::GenerationExhausted};
  const std::uint64_t nextGeneration = state->generationClock + 1;

  std::shared_ptr<const ScenePublicationActive<Snapshot>> prepared;
  try {
    prepared = std::make_shared<const ScenePublicationActive<Snapshot>>(
        ScenePublicationActive<Snapshot>{nextGeneration,
                                         std::move(snapshot)});
  } catch (...) {
    return {ScenePublicationCoreStatus::PreparationFailed};
  }

  const auto handle = makeHandle(nextGeneration);
  state->generationClock = nextGeneration;
  state->active.swap(prepared);

  ScenePublicationCoreNotification notification =
      ScenePublicationCoreNotification::NotRun;
  if (consumer) {
    try {
      consumer(handle);
      notification = ScenePublicationCoreNotification::Delivered;
    } catch (const std::exception&) {
      notification = ScenePublicationCoreNotification::StandardException;
    } catch (...) {
      notification = ScenePublicationCoreNotification::UnknownException;
    }
  }
  return {ScenePublicationCoreStatus::Published, notification, nextGeneration,
          scenePublicationIsCurrent(state, nextGeneration)};
}

template <typename State>
void destroyScenePublication(const std::shared_ptr<State>& state) noexcept {
  state->alive = false;
  decltype(state->active) empty;
  state->active.swap(empty);
}

}  // namespace solidar::detail
