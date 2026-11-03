#include "ui/application/AssemblyMateToolSession.h"

#include <new>
#include <utility>

namespace solidar {
namespace {

thread_local detail::AssemblyMateToolAllocationFailurePoint
    gAllocationFailurePoint =
        detail::AssemblyMateToolAllocationFailurePoint::None;

void throwInjected(detail::AssemblyMateToolAllocationFailurePoint point) {
  if (gAllocationFailurePoint != point) return;
  gAllocationFailurePoint =
      detail::AssemblyMateToolAllocationFailurePoint::None;
  throw std::bad_alloc{};
}

bool validOrientation(PlanarMateOrientation orientation) noexcept {
  return orientation == PlanarMateOrientation::Aligned ||
         orientation == PlanarMateOrientation::Opposed;
}

MateEndpoint endpointFor(const AssemblyFaceAddress& address) {
  return MateEndpoint{address.occurrence.path, address.face};
}

}  // namespace

bool AssemblyMateToolSession::begin(
    const ProductDocument& product,
    const AssemblyScenePublicationHandle& publication) noexcept {
  if (active() || !publication.isCurrent() ||
      !product.rootDefinitionId().isValid() ||
      !product.definitions().findAssembly(product.rootDefinitionId()))
    return false;
  const auto snapshot = publication.snapshotIfCurrent();
  if (!snapshot) return false;
  productIncarnation_ = product.incarnation();
  rootDefinitionId_ = product.rootDefinitionId();
  epoch_ = snapshot->epoch();
  publication_ = publication;
  first_.reset();
  second_.reset();
  acceptedTransaction_.reset();
  displayedTransaction_.reset();
  orientation_ = PlanarMateOrientation::Aligned;
  lastSolveStatus_ = MateSolveStatus::InvalidInput;
  state_ = AssemblyMateToolState::SelectingFirst;
  ++callbackRevision_;
  return true;
}

AssemblyMateToolPickResult AssemblyMateToolSession::acceptPick(
    const ProductDocument& product, const AssemblyFaceRayHit& hit,
    const AssemblyScenePublicationHandle& currentPublication) noexcept {
  AssemblyMateToolPickResult result;
  if (!active()) return result;
  if (state_ == AssemblyMateToolState::CommitPending) {
    result.status = AssemblyMateToolPickStatus::Busy;
    return result;
  }
  if (!publication_.isSameCurrentPublication(currentPublication) ||
      !hit.publication.isSameCurrentPublication(currentPublication)) {
    result.status = AssemblyMateToolPickStatus::StalePublication;
    return result;
  }
  const auto& address = hit.address;
  if (product.incarnation() != productIncarnation_ ||
      product.rootDefinitionId() != rootDefinitionId_ ||
      address.occurrence.epoch != epoch_ ||
      address.occurrence.rootAssemblyDefinitionId != rootDefinitionId_) {
    result.status = AssemblyMateToolPickStatus::ForeignProduct;
    return result;
  }
  try {
    const auto resolved = product.resolveOccurrence(address.occurrence.path);
    if (!resolved || !resolved.terminalPart() ||
        resolved.targetPart->id() != address.definitionId) {
      result.status = AssemblyMateToolPickStatus::InvalidAddress;
      return result;
    }
    const auto classified = classifyPlanarMateEndpoint(
        product, rootDefinitionId_, endpointFor(address));
    result.solveStatus = classified.status;
    result.endpointFailure = classified.failure;
    if (!classified ||
        classified.terminalDefinitionId != address.definitionId) {
      result.status = classified.status == MateSolveStatus::ResourceFailure
                          ? AssemblyMateToolPickStatus::ResourceFailure
                          : AssemblyMateToolPickStatus::UnsupportedFace;
      return result;
    }
    if (!first_ || state_ == AssemblyMateToolState::SelectingFirst) {
      throwInjected(
          detail::AssemblyMateToolAllocationFailurePoint::FirstPick);
      AssemblyFaceAddress copied = address;
      first_.emplace(std::move(copied));
      second_.reset();
      acceptedTransaction_.reset();
      displayedTransaction_.reset();
      state_ = AssemblyMateToolState::SelectingSecond;
      ++callbackRevision_;
      result.status = AssemblyMateToolPickStatus::Accepted;
      result.solveStatus = MateSolveStatus::Solved;
      return result;
    }
    if (first_->occurrence.path == address.occurrence.path) {
      result.status = AssemblyMateToolPickStatus::SameOccurrencePath;
      return result;
    }
    throwInjected(
        detail::AssemblyMateToolAllocationFailurePoint::SecondRefresh);
    AssemblyFaceAddress copied = address;
    second_.emplace(std::move(copied));
    ++callbackRevision_;
    result.solveStatus = refreshPreview(product);
    result.status = result.solveStatus == MateSolveStatus::ResourceFailure
                        ? AssemblyMateToolPickStatus::ResourceFailure
                        : AssemblyMateToolPickStatus::Accepted;
    return result;
  } catch (const std::bad_alloc&) {
    result.status = AssemblyMateToolPickStatus::ResourceFailure;
    result.solveStatus = MateSolveStatus::ResourceFailure;
    if (first_) {
      acceptedTransaction_.reset();
      state_ = AssemblyMateToolState::PreviewFailed;
      lastSolveStatus_ = MateSolveStatus::ResourceFailure;
      ++callbackRevision_;
    }
    return result;
  } catch (...) {
    result.status = AssemblyMateToolPickStatus::InvalidAddress;
    return result;
  }
}

MateSolveStatus AssemblyMateToolSession::refreshPreview(
    const ProductDocument& product) noexcept {
  if (!first_ || !second_ || !validOrientation(orientation_)) {
    state_ = first_ ? AssemblyMateToolState::SelectingSecond
                    : AssemblyMateToolState::SelectingFirst;
    return lastSolveStatus_ = MateSolveStatus::InvalidInput;
  }
  if (product.incarnation() != productIncarnation_ ||
      product.rootDefinitionId() != rootDefinitionId_) {
    state_ = AssemblyMateToolState::PreviewFailed;
    return lastSolveStatus_ = MateSolveStatus::Stale;
  }
  const auto* owner = product.definitions().findAssembly(rootDefinitionId_);
  const auto id = owner ? owner->mateRegistry().lowestFreeId() : std::nullopt;
  if (!owner || !id) {
    state_ = AssemblyMateToolState::PreviewFailed;
    return lastSolveStatus_ = MateSolveStatus::ResourceFailure;
  }
  try {
    auto prepared = preparePlanarMateAuthoring(
        product, rootDefinitionId_,
        PlanarCoincidentMate(*id, endpointFor(*first_), endpointFor(*second_),
                             orientation_, 0.0));
    lastSolveStatus_ = prepared.status;
    if (!prepared.transaction) {
      acceptedTransaction_.reset();
      state_ = AssemblyMateToolState::PreviewFailed;
      return lastSolveStatus_;
    }
    auto next = std::make_shared<const PlanarMateAuthoringTransaction>(
        std::move(*prepared.transaction));
    acceptedTransaction_.swap(next);
    // A solver-valid candidate is not committable until the matching scene
    // has been completely prepared and installed by the presentation layer.
    state_ = AssemblyMateToolState::PreviewFailed;
    return lastSolveStatus_;
  } catch (const std::bad_alloc&) {
    acceptedTransaction_.reset();
    state_ = AssemblyMateToolState::PreviewFailed;
    return lastSolveStatus_ = MateSolveStatus::ResourceFailure;
  } catch (...) {
    acceptedTransaction_.reset();
    state_ = AssemblyMateToolState::PreviewFailed;
    return lastSolveStatus_ = MateSolveStatus::InvalidInput;
  }
}

MateSolveStatus AssemblyMateToolSession::setOrientation(
    const ProductDocument& product,
    PlanarMateOrientation orientation) noexcept {
  if (!active() || state_ == AssemblyMateToolState::CommitPending ||
      !validOrientation(orientation))
    return MateSolveStatus::InvalidInput;
  orientation_ = orientation;
  ++callbackRevision_;
  try {
    throwInjected(
        detail::AssemblyMateToolAllocationFailurePoint::OrientationRefresh);
  } catch (const std::bad_alloc&) {
    acceptedTransaction_.reset();
    state_ = AssemblyMateToolState::PreviewFailed;
    return lastSolveStatus_ = MateSolveStatus::ResourceFailure;
  }
  return second_ ? refreshPreview(product) : lastSolveStatus_;
}

std::unique_ptr<PlanarMateAuthoringTransaction>
AssemblyMateToolSession::prepareCommit(
    const ProductDocument& product) noexcept {
  try {
    if (!canApply() || !first_ || !second_) return {};
    throwInjected(
        detail::AssemblyMateToolAllocationFailurePoint::PrepareCommit);
    const auto* owner = product.definitions().findAssembly(rootDefinitionId_);
    const auto id = acceptedTransaction_->relation().id();
    if (!owner || owner->mateRegistry().lowestFreeId() != id) {
      acceptedTransaction_.reset();
      state_ = AssemblyMateToolState::PreviewFailed;
      lastSolveStatus_ = MateSolveStatus::Stale;
      ++callbackRevision_;
      return {};
    }
    auto fresh = preparePlanarMateAuthoring(
        product, rootDefinitionId_,
        PlanarCoincidentMate(id, endpointFor(*first_), endpointFor(*second_),
                             orientation_, 0.0));
    lastSolveStatus_ = fresh.status;
    if (!fresh.transaction) {
      acceptedTransaction_.reset();
      state_ = AssemblyMateToolState::PreviewFailed;
      ++callbackRevision_;
      return {};
    }
    auto transaction = std::make_unique<PlanarMateAuthoringTransaction>(
        std::move(*fresh.transaction));
    state_ = AssemblyMateToolState::CommitPending;
    ++callbackRevision_;
    return transaction;
  } catch (const std::bad_alloc&) {
    acceptedTransaction_.reset();
    state_ = AssemblyMateToolState::PreviewFailed;
    lastSolveStatus_ = MateSolveStatus::ResourceFailure;
    ++callbackRevision_;
    return {};
  } catch (...) {
    acceptedTransaction_.reset();
    state_ = AssemblyMateToolState::PreviewFailed;
    lastSolveStatus_ = MateSolveStatus::InvalidInput;
    ++callbackRevision_;
    return {};
  }
}

bool AssemblyMateToolSession::presentationSucceeded(
    std::uint64_t expectedRevision) noexcept {
  if (expectedRevision != callbackRevision_ || !acceptedTransaction_ ||
      state_ == AssemblyMateToolState::CommitPending)
    return false;
  displayedTransaction_ = acceptedTransaction_;
  state_ = AssemblyMateToolState::PreviewValid;
  return true;
}

void AssemblyMateToolSession::presentationFailed(
    std::uint64_t expectedRevision) noexcept {
  if (expectedRevision != callbackRevision_ ||
      state_ == AssemblyMateToolState::CommitPending)
    return;
  acceptedTransaction_.reset();
  state_ = AssemblyMateToolState::PreviewFailed;
}

void AssemblyMateToolSession::commitFailed() noexcept {
  if (state_ != AssemblyMateToolState::CommitPending) return;
  state_ = AssemblyMateToolState::PreviewFailed;
  ++callbackRevision_;
}

void AssemblyMateToolSession::clearSecondRetainingFirst() noexcept {
  second_.reset();
  acceptedTransaction_.reset();
  displayedTransaction_.reset();
  state_ = AssemblyMateToolState::SelectingSecond;
  ++callbackRevision_;
}

void AssemblyMateToolSession::escape() noexcept {
  if (!active()) return;
  if (state_ == AssemblyMateToolState::CommitPending) return;
  if (second_ || state_ == AssemblyMateToolState::PreviewValid ||
      state_ == AssemblyMateToolState::PreviewFailed) {
    clearSecondRetainingFirst();
    return;
  }
  if (first_) {
    first_.reset();
    state_ = AssemblyMateToolState::SelectingFirst;
    ++callbackRevision_;
    return;
  }
  cancel();
}

void AssemblyMateToolSession::cancel() noexcept {
  if (!active()) return;
  state_ = AssemblyMateToolState::Inactive;
  first_.reset();
  second_.reset();
  acceptedTransaction_.reset();
  displayedTransaction_.reset();
  publication_ = {};
  productIncarnation_ = {};
  rootDefinitionId_ = {};
  epoch_ = {};
  orientation_ = PlanarMateOrientation::Aligned;
  lastSolveStatus_ = MateSolveStatus::InvalidInput;
  ++callbackRevision_;
}

void detail::AssemblyMateToolSessionTestAccess::injectAllocationFailure(
    AssemblyMateToolAllocationFailurePoint point) noexcept {
  gAllocationFailurePoint = point;
}

}  // namespace solidar
