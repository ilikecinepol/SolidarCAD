#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "presentation/ComponentPresentation.h"

namespace solidar {

class ProductDocument;
class ComponentScenePublicationCoordinator;
class ComponentScenePublicationCoordinatorTestAccess;

namespace detail {
struct ComponentScenePublicationState;
}

class ComponentScenePublicationGeneration final {
 public:
  constexpr ComponentScenePublicationGeneration() noexcept = default;

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(
      ComponentScenePublicationGeneration,
      ComponentScenePublicationGeneration) noexcept = default;

 private:
  friend class ComponentScenePublicationCoordinator;

  explicit constexpr ComponentScenePublicationGeneration(
      std::uint64_t value) noexcept
      : value_(value) {}

  std::uint64_t value_{};
};

inline constexpr ComponentScenePublicationGeneration
    kInvalidComponentScenePublicationGeneration{};

class ComponentScenePublicationHandle final {
 public:
  ComponentScenePublicationHandle() noexcept = default;

  [[nodiscard]] ComponentScenePublicationGeneration generation()
      const noexcept {
    return generation_;
  }
  [[nodiscard]] bool isCurrent() const noexcept;
  [[nodiscard]] bool isSameCurrentPublication(
      const ComponentScenePublicationHandle& other) const noexcept;
  [[nodiscard]] std::shared_ptr<const ComponentPresentationSnapshot>
  snapshotIfCurrent() const noexcept;

 private:
  friend class ComponentScenePublicationCoordinator;

  ComponentScenePublicationHandle(
      std::weak_ptr<detail::ComponentScenePublicationState> state,
      ComponentScenePublicationGeneration generation) noexcept
      : state_(std::move(state)), generation_(generation) {}

  std::weak_ptr<detail::ComponentScenePublicationState> state_;
  ComponentScenePublicationGeneration generation_;
};

enum class ComponentScenePublicationStatus : std::uint8_t {
  Published,
  BuildFailed,
  InvalidBuildResult,
  GenerationExhausted,
  PreparationFailed,
  WrongThread,
};

enum class ComponentSceneNotificationStatus : std::uint8_t {
  NotRun,
  Delivered,
  StandardException,
  UnknownException,
};

enum class ComponentSceneClearStatus : std::uint8_t {
  Cleared,
  AlreadyClear,
  WrongThread,
};

struct ComponentScenePublicationResult final {
  ComponentScenePublicationStatus status{
      ComponentScenePublicationStatus::PreparationFailed};
  ComponentSceneNotificationStatus notification{
      ComponentSceneNotificationStatus::NotRun};
  std::optional<ComponentPresentationBuildStatus> builderStatus;
  ComponentScenePublicationGeneration publishedGeneration;
  bool stillCurrent{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentScenePublicationStatus::Published;
  }
};

// Synchronous exact-generation publication owned by its creator thread.
class ComponentScenePublicationCoordinator final {
 public:
  using Consumer = std::function<void(ComponentScenePublicationHandle)>;

  ComponentScenePublicationCoordinator();
  ~ComponentScenePublicationCoordinator() noexcept;

  ComponentScenePublicationCoordinator(
      const ComponentScenePublicationCoordinator&) = delete;
  ComponentScenePublicationCoordinator& operator=(
      const ComponentScenePublicationCoordinator&) = delete;
  ComponentScenePublicationCoordinator(
      ComponentScenePublicationCoordinator&&) = delete;
  ComponentScenePublicationCoordinator& operator=(
      ComponentScenePublicationCoordinator&&) = delete;

  [[nodiscard]] ComponentScenePublicationResult rebuildAndPublish(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const Consumer& consumer = Consumer{}) noexcept;
  // Publishes the exact post-identity-transition projection without mutating
  // the live ProductDocument. Used to prepare Save/Save As UI state before
  // the atomic file write and identity commit.
  [[nodiscard]] ComponentScenePublicationResult
  rebuildAndPublishProjectedIdentity(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const ComponentIdentityTransition& transition,
      const Consumer& consumer = Consumer{}) noexcept;
  [[nodiscard]] ComponentSceneClearStatus clear() noexcept;
  [[nodiscard]] ComponentScenePublicationHandle active() const noexcept;

 private:
  friend class ComponentScenePublicationCoordinatorTestAccess;

  [[nodiscard]] bool seedGenerationForTesting(std::uint64_t value) noexcept;
  [[nodiscard]] bool isOwnerThread() const noexcept;
  [[nodiscard]] ComponentScenePublicationResult publishBuildResult(
      ComponentPresentationBuildResult built,
      const Consumer& consumer) noexcept;

  std::shared_ptr<detail::ComponentScenePublicationState> state_;
};

}  // namespace solidar
