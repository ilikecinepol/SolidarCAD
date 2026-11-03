#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

#include "model/ProductDocument.h"
#include "ui/application/ProjectApplicationService.h"

namespace solidar {

enum class AssemblyInsertToolState : std::uint8_t {
  Inactive,
  AwaitingSource,
  Placing,
  NestedSelection,
  NestedTransform,
  CommitPending,
};

class AssemblyInsertToolSession final {
 public:
  using CancelObserver = std::function<void(const StagedEmbeddedPart*)>;
  [[nodiscard]] bool begin(DefinitionId proposedDefinitionId,
                           OccurrenceId proposedOccurrenceId) noexcept;
  [[nodiscard]] bool acceptSource(StagedEmbeddedPart&& source) noexcept;
  [[nodiscard]] bool updateTranslation(
      std::array<double, 3> translationMm) noexcept;
  [[nodiscard]] bool updateRotationDegrees(
      std::array<double, 3> xyzDegrees) noexcept;
  [[nodiscard]] bool beginNestedSelection() noexcept;
  [[nodiscard]] bool beginNestedTransform() noexcept;
  [[nodiscard]] bool finishNested() noexcept;
  // First Escape restores the nested checkpoint; the next performs a full
  // cancel. The return value is true only for the nested-only escape.
  [[nodiscard]] bool handleEscape() noexcept;
  void cancel(const CancelObserver& beforeSourceRelease = {}) noexcept;

  [[nodiscard]] bool beginCommit() noexcept;
  void restoreAfterFailedCommit() noexcept;
  void finishCommit() noexcept;

  [[nodiscard]] AssemblyInsertToolState state() const noexcept { return state_; }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] const RigidTransform3d& placement() const noexcept {
    return placement_;
  }
  [[nodiscard]] const StagedEmbeddedPart* source() const noexcept {
    return source_ ? &*source_ : nullptr;
  }
  [[nodiscard]] DefinitionId proposedDefinitionId() const noexcept {
    return definitionId_;
  }
  [[nodiscard]] OccurrenceId proposedOccurrenceId() const noexcept {
    return occurrenceId_;
  }
  [[nodiscard]] EmbeddedPartInsertPayload takeRuntimePayload() noexcept;
  void restoreRuntimePayload(EmbeddedPartInsertPayload&& payload) noexcept;
  [[nodiscard]] QByteArray authoredDocument() const { return authoredDocument_; }

 private:
  [[nodiscard]] bool rebuildPlacement() noexcept;
  void bumpRevision() noexcept;

  AssemblyInsertToolState state_{AssemblyInsertToolState::Inactive};
  std::uint64_t revision_{};
  DefinitionId definitionId_;
  OccurrenceId occurrenceId_;
  std::optional<StagedEmbeddedPart> source_;
  QByteArray authoredDocument_;
  std::array<double, 3> translationMm_{};
  std::array<double, 3> rotationDegrees_{};
  RigidTransform3d placement_{RigidTransform3d::identity()};
  std::optional<std::pair<std::array<double, 3>, std::array<double, 3>>>
      nestedCheckpoint_;
};

class AssemblyInsertController final {
 public:
  void activate(AssemblyInsertToolSession& session,
                DefinitionId definitionId, OccurrenceId occurrenceId) noexcept;
  void cancelActive(
      const AssemblyInsertToolSession::CancelObserver& beforeSourceRelease =
          {},
      const std::function<void()>& afterControllerDetach = {}) noexcept;
  [[nodiscard]] bool handleEscape() noexcept;
  [[nodiscard]] AssemblyInsertToolSession* active() const noexcept {
    return active_;
  }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

 private:
  AssemblyInsertToolSession* active_{};
  std::uint64_t revision_{};
};

}  // namespace solidar
