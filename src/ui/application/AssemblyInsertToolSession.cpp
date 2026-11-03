#include "ui/application/AssemblyInsertToolSession.h"

#include <cmath>
#include <limits>
#include <utility>

namespace solidar {
namespace {

std::array<double, 4> eulerQuaternion(
    const std::array<double, 3>& degrees) noexcept {
  constexpr double kPi = 3.141592653589793238462643383279502884;
  const double hx = degrees[0] * kPi / 360.0;
  const double hy = degrees[1] * kPi / 360.0;
  const double hz = degrees[2] * kPi / 360.0;
  const double cx = std::cos(hx), sx = std::sin(hx);
  const double cy = std::cos(hy), sy = std::sin(hy);
  const double cz = std::cos(hz), sz = std::sin(hz);
  // Fixed authored convention: intrinsic X, then Y, then Z (qz*qy*qx).
  return {cz * cy * cx + sz * sy * sx,
          cz * cy * sx - sz * sy * cx,
          cz * sy * cx + sz * cy * sx,
          sz * cy * cx - cz * sy * sx};
}

}  // namespace

void AssemblyInsertToolSession::bumpRevision() noexcept {
  if (revision_ != std::numeric_limits<std::uint64_t>::max()) ++revision_;
}

bool AssemblyInsertToolSession::begin(DefinitionId definitionId,
                                      OccurrenceId occurrenceId) noexcept {
  if (!definitionId.isValid() || !occurrenceId.isValid() ||
      state_ == AssemblyInsertToolState::CommitPending)
    return false;
  cancel();
  definitionId_ = definitionId;
  occurrenceId_ = occurrenceId;
  state_ = AssemblyInsertToolState::AwaitingSource;
  bumpRevision();
  return true;
}

bool AssemblyInsertToolSession::acceptSource(
    StagedEmbeddedPart&& source) noexcept {
  if ((state_ != AssemblyInsertToolState::AwaitingSource &&
       state_ != AssemblyInsertToolState::Placing) || !source.document ||
      source.authoredDocument.isEmpty())
    return false;
  authoredDocument_ = std::move(source.authoredDocument);
  source_.emplace(std::move(source));
  translationMm_ = {};
  rotationDegrees_ = {};
  placement_ = RigidTransform3d::identity();
  state_ = AssemblyInsertToolState::Placing;
  bumpRevision();
  return true;
}

bool AssemblyInsertToolSession::rebuildPlacement() noexcept {
  const auto candidate =
      RigidTransform3d::create(translationMm_, eulerQuaternion(rotationDegrees_));
  if (!candidate) return false;
  placement_ = *candidate.transform;
  bumpRevision();
  return true;
}

bool AssemblyInsertToolSession::updateTranslation(
    std::array<double, 3> translationMm) noexcept {
  if (state_ != AssemblyInsertToolState::Placing &&
      state_ != AssemblyInsertToolState::NestedTransform)
    return false;
  const auto previous = translationMm_;
  translationMm_ = translationMm;
  if (rebuildPlacement()) return true;
  translationMm_ = previous;
  return false;
}

bool AssemblyInsertToolSession::updateRotationDegrees(
    std::array<double, 3> xyzDegrees) noexcept {
  if (state_ != AssemblyInsertToolState::Placing &&
      state_ != AssemblyInsertToolState::NestedTransform)
    return false;
  const auto previous = rotationDegrees_;
  rotationDegrees_ = xyzDegrees;
  if (rebuildPlacement()) return true;
  rotationDegrees_ = previous;
  return false;
}

bool AssemblyInsertToolSession::beginNestedSelection() noexcept {
  if (state_ != AssemblyInsertToolState::AwaitingSource &&
      state_ != AssemblyInsertToolState::Placing)
    return false;
  nestedCheckpoint_ = std::pair{translationMm_, rotationDegrees_};
  state_ = AssemblyInsertToolState::NestedSelection;
  bumpRevision();
  return true;
}

bool AssemblyInsertToolSession::beginNestedTransform() noexcept {
  if (state_ != AssemblyInsertToolState::Placing) return false;
  nestedCheckpoint_ = std::pair{translationMm_, rotationDegrees_};
  state_ = AssemblyInsertToolState::NestedTransform;
  bumpRevision();
  return true;
}

bool AssemblyInsertToolSession::finishNested() noexcept {
  if (state_ != AssemblyInsertToolState::NestedSelection &&
      state_ != AssemblyInsertToolState::NestedTransform)
    return false;
  state_ = source_ ? AssemblyInsertToolState::Placing
                   : AssemblyInsertToolState::AwaitingSource;
  nestedCheckpoint_.reset();
  bumpRevision();
  return true;
}

bool AssemblyInsertToolSession::handleEscape() noexcept {
  if (nestedCheckpoint_ &&
      (state_ == AssemblyInsertToolState::NestedSelection ||
       state_ == AssemblyInsertToolState::NestedTransform)) {
    translationMm_ = nestedCheckpoint_->first;
    rotationDegrees_ = nestedCheckpoint_->second;
    static_cast<void>(rebuildPlacement());
    state_ = source_ ? AssemblyInsertToolState::Placing
                     : AssemblyInsertToolState::AwaitingSource;
    nestedCheckpoint_.reset();
    bumpRevision();
    return true;
  }
  cancel();
  return false;
}

void AssemblyInsertToolSession::cancel(
    const CancelObserver& beforeSourceRelease) noexcept {
  if (state_ == AssemblyInsertToolState::Inactive && !source_) return;
  nestedCheckpoint_.reset();
  if (beforeSourceRelease) {
    try {
      beforeSourceRelease(source_ ? &*source_ : nullptr);
    } catch (...) {
    }
  }
  source_.reset();
  authoredDocument_.clear();
  definitionId_ = {};
  occurrenceId_ = {};
  translationMm_ = {};
  rotationDegrees_ = {};
  placement_ = RigidTransform3d::identity();
  state_ = AssemblyInsertToolState::Inactive;
  bumpRevision();
}

bool AssemblyInsertToolSession::beginCommit() noexcept {
  if (state_ != AssemblyInsertToolState::Placing || !source_ ||
      !source_->document || authoredDocument_.isEmpty())
    return false;
  state_ = AssemblyInsertToolState::CommitPending;
  bumpRevision();
  return true;
}

void AssemblyInsertToolSession::restoreAfterFailedCommit() noexcept {
  if (state_ != AssemblyInsertToolState::CommitPending) return;
  state_ = AssemblyInsertToolState::Placing;
  bumpRevision();
}

void AssemblyInsertToolSession::finishCommit() noexcept {
  if (state_ != AssemblyInsertToolState::CommitPending) return;
  cancel();
}

EmbeddedPartInsertPayload
AssemblyInsertToolSession::takeRuntimePayload() noexcept {
  EmbeddedPartInsertPayload payload;
  if (state_ != AssemblyInsertToolState::CommitPending || !source_) return payload;
  payload.definitionId = definitionId_;
  payload.occurrenceId = occurrenceId_;
  payload.placement = placement_;
  payload.flags = {};
  payload.document = std::move(source_->document);
  return payload;
}

void AssemblyInsertToolSession::restoreRuntimePayload(
    EmbeddedPartInsertPayload&& payload) noexcept {
  if (state_ != AssemblyInsertToolState::CommitPending || !source_ ||
      payload.definitionId != definitionId_ ||
      payload.occurrenceId != occurrenceId_)
    return;
  source_->document = std::move(payload.document);
}

void AssemblyInsertController::activate(AssemblyInsertToolSession& session,
                                        DefinitionId definitionId,
                                        OccurrenceId occurrenceId) noexcept {
  cancelActive();
  if (!session.begin(definitionId, occurrenceId)) return;
  active_ = &session;
  if (revision_ != std::numeric_limits<std::uint64_t>::max()) ++revision_;
}

void AssemblyInsertController::cancelActive(
    const AssemblyInsertToolSession::CancelObserver& beforeSourceRelease,
    const std::function<void()>& afterControllerDetach)
    noexcept {
  auto* session = std::exchange(active_, nullptr);
  if (afterControllerDetach) {
    try {
      afterControllerDetach();
    } catch (...) {
    }
  }
  if (session) session->cancel(beforeSourceRelease);
  if (revision_ != std::numeric_limits<std::uint64_t>::max()) ++revision_;
}

bool AssemblyInsertController::handleEscape() noexcept {
  if (!active_) return false;
  if (active_->handleEscape()) return true;
  active_ = nullptr;
  if (revision_ != std::numeric_limits<std::uint64_t>::max()) ++revision_;
  return false;
}

}  // namespace solidar
