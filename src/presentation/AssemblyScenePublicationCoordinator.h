#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "presentation/AssemblyPresentation.h"

namespace solidar {

class ProductDocument;
class AssemblyScenePublicationCoordinator;
class AssemblyScenePublicationCoordinatorTestAccess;

namespace detail {
struct AssemblyScenePublicationState;
}

// Publication generations belong to one coordinator identity.  Numeric
// equality alone never makes handles from different coordinators equivalent.
class AssemblyScenePublicationGeneration final {
 public:
  constexpr AssemblyScenePublicationGeneration() noexcept = default;

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(
      AssemblyScenePublicationGeneration,
      AssemblyScenePublicationGeneration) noexcept = default;

 private:
  friend class AssemblyScenePublicationCoordinator;

  explicit constexpr AssemblyScenePublicationGeneration(
      std::uint64_t value) noexcept
      : value_(value) {}

  std::uint64_t value_{};
};

inline constexpr AssemblyScenePublicationGeneration
    kInvalidAssemblyScenePublicationGeneration{};

// A handle never owns coordinator state or an active-publication record.  It
// can only acquire the immutable snapshot while its state and generation are
// still current.  Calls are owner-thread-only; a call from another thread
// fails closed without reading mutable publication state.
class AssemblyScenePublicationHandle final {
 public:
  AssemblyScenePublicationHandle() noexcept = default;

  [[nodiscard]] AssemblyScenePublicationGeneration generation()
      const noexcept {
    return generation_;
  }
  [[nodiscard]] bool isCurrent() const noexcept;
  // Exact-publication comparison.  A matching numeric generation is not
  // sufficient: both handles must name the same live coordinator state and
  // that publication must still be current on the owner thread.
  [[nodiscard]] bool isSameCurrentPublication(
      const AssemblyScenePublicationHandle& other) const noexcept;
  [[nodiscard]] std::shared_ptr<const AssemblyPresentationSnapshot>
  snapshotIfCurrent() const noexcept;

 private:
  friend class AssemblyScenePublicationCoordinator;

  AssemblyScenePublicationHandle(
      std::weak_ptr<detail::AssemblyScenePublicationState> state,
      AssemblyScenePublicationGeneration generation) noexcept
      : state_(std::move(state)), generation_(generation) {}

  std::weak_ptr<detail::AssemblyScenePublicationState> state_;
  AssemblyScenePublicationGeneration generation_;
};

enum class AssemblyScenePublicationStatus : std::uint8_t {
  Published,
  BuildFailed,
  InvalidBuildResult,
  GenerationExhausted,
  PreparationFailed,
  WrongThread,
};

enum class AssemblySceneNotificationStatus : std::uint8_t {
  NotRun,
  Delivered,
  StandardException,
  UnknownException,
};

enum class AssemblySceneClearStatus : std::uint8_t {
  Cleared,
  AlreadyClear,
  WrongThread,
};

struct AssemblyScenePublicationResult {
  AssemblyScenePublicationStatus status{
      AssemblyScenePublicationStatus::PreparationFailed};
  AssemblySceneNotificationStatus notification{
      AssemblySceneNotificationStatus::NotRun};
  std::optional<AssemblyPresentationBuildStatus> builderStatus;
  AssemblyScenePublicationGeneration publishedGeneration;
  bool stillCurrent{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyScenePublicationStatus::Published;
  }
};

// Synchronous single-owner-thread coordinator.  Construction, publication,
// active(), clear() and destruction must all occur on the creator thread.  It
// stores no callback and never gives consumers a mutable snapshot or raw
// coordinator pointer.
class AssemblyScenePublicationCoordinator final {
 public:
  using Consumer = std::function<void(AssemblyScenePublicationHandle)>;

  AssemblyScenePublicationCoordinator();
  ~AssemblyScenePublicationCoordinator() noexcept;

  AssemblyScenePublicationCoordinator(
      const AssemblyScenePublicationCoordinator&) = delete;
  AssemblyScenePublicationCoordinator& operator=(
      const AssemblyScenePublicationCoordinator&) = delete;
  AssemblyScenePublicationCoordinator(
      AssemblyScenePublicationCoordinator&&) = delete;
  AssemblyScenePublicationCoordinator& operator=(
      AssemblyScenePublicationCoordinator&&) = delete;

  [[nodiscard]] AssemblyScenePublicationResult rebuildAndPublish(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const Consumer& consumer = Consumer{}) noexcept;

  // Clear never allocates a generation and never rewinds the generation
  // clock, so teardown remains available after generation exhaustion.
  [[nodiscard]] AssemblySceneClearStatus clear() noexcept;

  // Wrong-thread access returns an invalid handle without reading active
  // publication state.
  [[nodiscard]] AssemblyScenePublicationHandle active() const noexcept;

 private:
  friend class AssemblyScenePublicationCoordinatorTestAccess;

  [[nodiscard]] bool seedGenerationForTesting(std::uint64_t value) noexcept;
  [[nodiscard]] bool isOwnerThread() const noexcept;

  std::shared_ptr<detail::AssemblyScenePublicationState> state_;
};

}  // namespace solidar
