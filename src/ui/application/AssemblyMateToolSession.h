#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "model/PlanarMateSolver.h"
#include "ui/AssemblyViewportPicking.h"

namespace solidar {

enum class AssemblyMateToolState : std::uint8_t {
  Inactive,
  SelectingFirst,
  SelectingSecond,
  PreviewValid,
  PreviewFailed,
  CommitPending,
};

enum class AssemblyMateToolPickStatus : std::uint8_t {
  Accepted,
  Inactive,
  Busy,
  StalePublication,
  ForeignProduct,
  InvalidAddress,
  SameOccurrencePath,
  UnsupportedFace,
  ResourceFailure,
};

namespace detail {
enum class AssemblyMateToolAllocationFailurePoint : std::uint8_t {
  None,
  FirstPick,
  SecondRefresh,
  OrientationRefresh,
  PrepareCommit,
};

class AssemblyMateToolSessionTestAccess;
}  // namespace detail

struct AssemblyMateToolPickResult final {
  AssemblyMateToolPickStatus status{AssemblyMateToolPickStatus::Inactive};
  MateSolveStatus solveStatus{MateSolveStatus::InvalidInput};
  MateEndpointFailure endpointFailure{MateEndpointFailure::None};
};

// UI-free, value-only state machine. It owns no ProductDocument, QObject,
// QModelIndex or OCCT object. Every operation receives the current product and
// publication explicitly, so delayed callbacks fail closed.
class AssemblyMateToolSession final {
 public:
  [[nodiscard]] bool begin(
      const ProductDocument& product,
      const AssemblyScenePublicationHandle& publication) noexcept;
  [[nodiscard]] AssemblyMateToolPickResult acceptPick(
      const ProductDocument& product, const AssemblyFaceRayHit& hit,
      const AssemblyScenePublicationHandle& currentPublication) noexcept;
  [[nodiscard]] MateSolveStatus setOrientation(
      const ProductDocument& product,
      PlanarMateOrientation orientation) noexcept;
  [[nodiscard]] std::unique_ptr<PlanarMateAuthoringTransaction> prepareCommit(
      const ProductDocument& product) noexcept;
  [[nodiscard]] bool presentationSucceeded(
      std::uint64_t expectedRevision) noexcept;
  void presentationFailed(std::uint64_t expectedRevision) noexcept;
  void commitFailed() noexcept;
  void escape() noexcept;
  void cancel() noexcept;

  [[nodiscard]] AssemblyMateToolState state() const noexcept { return state_; }
  [[nodiscard]] bool active() const noexcept {
    return state_ != AssemblyMateToolState::Inactive;
  }
  [[nodiscard]] bool canApply() const noexcept {
    return state_ == AssemblyMateToolState::PreviewValid &&
           acceptedTransaction_ != nullptr &&
           displayedTransaction_ == acceptedTransaction_;
  }
  [[nodiscard]] std::uint64_t callbackRevision() const noexcept {
    return callbackRevision_;
  }
  [[nodiscard]] PlanarMateOrientation orientation() const noexcept {
    return orientation_;
  }
  [[nodiscard]] const std::optional<AssemblyFaceAddress>& firstPick()
      const noexcept {
    return first_;
  }
  [[nodiscard]] const std::optional<AssemblyFaceAddress>& secondPick()
      const noexcept {
    return second_;
  }
  // Last successfully prepared visual is intentionally retained after a
  // failed current preview. canApply() remains false in that state.
  [[nodiscard]] const std::shared_ptr<const PlanarMateAuthoringTransaction>&
  displayedPreview() const noexcept {
    return displayedTransaction_;
  }
  [[nodiscard]] const std::shared_ptr<const PlanarMateAuthoringTransaction>&
  currentPreview() const noexcept {
    return acceptedTransaction_;
  }
  [[nodiscard]] MateSolveStatus lastSolveStatus() const noexcept {
    return lastSolveStatus_;
  }

 private:
  friend class detail::AssemblyMateToolSessionTestAccess;
  [[nodiscard]] MateSolveStatus refreshPreview(
      const ProductDocument& product) noexcept;
  void clearSecondRetainingFirst() noexcept;

  AssemblyMateToolState state_{AssemblyMateToolState::Inactive};
  ProductIncarnationId productIncarnation_;
  DefinitionId rootDefinitionId_;
  ProductPresentationEpoch epoch_;
  AssemblyScenePublicationHandle publication_;
  std::optional<AssemblyFaceAddress> first_;
  std::optional<AssemblyFaceAddress> second_;
  PlanarMateOrientation orientation_{PlanarMateOrientation::Aligned};
  MateSolveStatus lastSolveStatus_{MateSolveStatus::InvalidInput};
  std::shared_ptr<const PlanarMateAuthoringTransaction> acceptedTransaction_;
  std::shared_ptr<const PlanarMateAuthoringTransaction> displayedTransaction_;
  std::uint64_t callbackRevision_{1};
};

namespace detail {
class AssemblyMateToolSessionTestAccess final {
 public:
  static void injectAllocationFailure(
      AssemblyMateToolAllocationFailurePoint point) noexcept;
};
}  // namespace detail

class AssemblyMateToolController final {
 public:
  [[nodiscard]] AssemblyMateToolSession& session() noexcept { return session_; }
  [[nodiscard]] const AssemblyMateToolSession& session() const noexcept {
    return session_;
  }
  void cancel() noexcept { session_.cancel(); }

 private:
  AssemblyMateToolSession session_;
};

}  // namespace solidar
