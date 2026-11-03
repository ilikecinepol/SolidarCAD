#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "model/OccurrenceDelta.h"
#include "model/OccurrenceDuplicate.h"
#include "model/OccurrenceRemove.h"
#include "model/PlanarMateSolver.h"
#include "ui/application/DocumentDelta.h"

namespace solidar {

class ProductDocument;

enum class AssemblyCommandHistoryStatus {
  Committed,
  NoChange,
  Busy,
  NothingToUndo,
  NothingToRedo,
  InvalidCommand,
  ReconstructionFailed,
  ModelRejected,
  SuccessorRejected,
  ResourceLimit,
};

class AssemblyCommandRevision final {
 public:
  constexpr AssemblyCommandRevision() noexcept = default;
  explicit constexpr AssemblyCommandRevision(std::uint64_t value) noexcept
      : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(AssemblyCommandRevision,
                                   AssemblyCommandRevision) noexcept = default;

 private:
  std::uint64_t value_{};
};

struct AssemblyCommandSemanticState final {
  std::optional<OccurrencePath> selectedOccurrence;
  std::optional<DefinitionId> selectedDefinition;

  friend bool operator==(const AssemblyCommandSemanticState&,
                         const AssemblyCommandSemanticState&) = default;
};

struct EmbeddedPartInsertColdCommand final {
  DefinitionId definitionId;
  OccurrenceId occurrenceId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags{};
  std::vector<std::byte> authoredDocument;
  std::string displayNameUtf8;
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
};

struct OccurrenceDeltaCommand final {
  OccurrenceDelta delta;
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
};

struct DuplicateOccurrenceColdCommand final {
  DuplicateOccurrenceColdPayload payload;
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
};

struct RemoveOccurrenceColdCommand final {
  RemoveOccurrenceColdPayload payload;
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
};

struct DefinitionEditColdCommand final {
  DefinitionEditAddress address;
  DocumentDelta delta;
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
};

struct LinkedPartReloadColdCommand final {
  DefinitionEditAddress address;
  LinkedPartBinding beforeBinding;
  LinkedPartBinding afterBinding;
  DocumentDelta delta;
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
};

struct LinkedPartRepairColdCommand final {
  DefinitionEditAddress address;
  LinkedPartBinding beforeBinding;
  LinkedPartBinding afterBinding;
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
};

struct PlanarMateColdCommand final {
  std::unique_ptr<PlanarMateAuthoringTransaction> transaction;
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
};

using AssemblyCommand =
    std::variant<EmbeddedPartInsertColdCommand, OccurrenceDeltaCommand,
                 DuplicateOccurrenceColdCommand,
                 RemoveOccurrenceColdCommand, DefinitionEditColdCommand,
                 LinkedPartReloadColdCommand, LinkedPartRepairColdCommand,
                 PlanarMateColdCommand>;

struct AssemblyCommandHistoryResult final {
  AssemblyCommandHistoryStatus status{AssemblyCommandHistoryStatus::NoChange};
  std::optional<OccurrenceDeltaStatus> deltaStatus;
  std::optional<AssemblyCommandSemanticState> restoredState;
};

namespace detail {
class AssemblyCommandHistoryTestAccess;
}

// The single bounded assembly history. Entries are closed cold commands:
// stable scalar IDs/transforms plus authored bytes or typed model deltas.
// Runtime Document/B-Rep, scene-generation and UI objects are never retained.
class AssemblyCommandHistory final {
 public:
  AssemblyCommandHistory() noexcept;

  [[nodiscard]] AssemblyCommandHistoryStatus recordApplied(
      EmbeddedPartInsertColdCommand command) noexcept;
  [[nodiscard]] AssemblyCommandHistoryStatus prepareRecord(
      EmbeddedPartInsertColdCommand command) noexcept;
  [[nodiscard]] AssemblyCommandHistoryStatus commitPrepared() noexcept;
  void discardPrepared() noexcept;

  [[nodiscard]] AssemblyCommandHistoryResult executeOccurrenceDelta(
      ProductDocument& product,
      OccurrenceDeltaCommand command,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] AssemblyCommandHistoryResult executeDuplicate(
      ProductDocument& product,
      DuplicateOccurrenceColdCommand command,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] AssemblyCommandHistoryResult executeRemove(
      ProductDocument& product,
      RemoveOccurrenceColdCommand command,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] AssemblyCommandHistoryResult executeDefinitionEdit(
      ProductDocument& product, DefinitionEditColdCommand command,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] AssemblyCommandHistoryResult executeLinkedPartReload(
      ProductDocument& product, LinkedPartReloadColdCommand command,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] AssemblyCommandHistoryResult executeLinkedPartRepair(
      ProductDocument& product, LinkedPartRepairColdCommand command,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] AssemblyCommandHistoryResult executePlanarMate(
      ProductDocument& product, PlanarMateColdCommand command,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] AssemblyCommandHistoryResult undo(
      ProductDocument& product,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] AssemblyCommandHistoryResult redo(
      ProductDocument& product,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;

  void clear() noexcept;
  void markSavepoint() noexcept { savepointRevision_ = currentRevision_; }

  [[nodiscard]] bool canUndo() const noexcept { return cursor_ != 0; }
  [[nodiscard]] bool canRedo() const noexcept {
    return cursor_ < commands_.size();
  }
  [[nodiscard]] bool isAtSavepoint() const noexcept {
    return currentRevision_ == savepointRevision_;
  }
  [[nodiscard]] bool isBusy() const noexcept { return busy_; }
  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
  [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
  [[nodiscard]] std::size_t retainedBytes() const noexcept {
    return retainedBytes_;
  }
  [[nodiscard]] AssemblyCommandRevision currentRevision() const noexcept {
    return currentRevision_;
  }
  [[nodiscard]] AssemblyCommandRevision savepointRevision() const noexcept {
    return savepointRevision_;
  }
  [[nodiscard]] const std::optional<OccurrencePath>& currentOccurrence()
      const noexcept;
  [[nodiscard]] std::optional<
      std::reference_wrapper<const AssemblyCommandSemanticState>>
  nextUndoState() const noexcept;
  [[nodiscard]] std::optional<
      std::reference_wrapper<const AssemblyCommandSemanticState>>
  nextRedoState() const noexcept;
  [[nodiscard]] std::optional<DefinitionId>
  nextUndoLinkedDefinition() const noexcept;
  [[nodiscard]] std::optional<DefinitionId>
  nextRedoLinkedDefinition() const noexcept;

  [[nodiscard]] static constexpr std::size_t maximumRetainedBytes() noexcept {
    return kMaximumRetainedBytes;
  }
  [[nodiscard]] static constexpr std::size_t maximumCommands() noexcept {
    return kMaximumCommands;
  }

 private:
  friend class detail::AssemblyCommandHistoryTestAccess;
  class FinalizingSuccessor;

  struct Entry final {
    AssemblyCommand command;
    std::size_t dynamicBytes{};
    AssemblyCommandRevision beforeRevision;
    AssemblyCommandRevision afterRevision;
  };

  struct Pending final {
    AssemblyCommand command;
    std::size_t dynamicBytes{};
    std::size_t retainedBytesAfterCommit{};
  };

  [[nodiscard]] AssemblyCommandHistoryStatus prepareCommand(
      AssemblyCommand command) noexcept;
  [[nodiscard]] static std::optional<std::size_t> dynamicBytes(
      const AssemblyCommand& command) noexcept;
  [[nodiscard]] static std::optional<std::size_t>
  mateRegistryDynamicBytesForTesting(const MateRegistry& registry) noexcept;
  [[nodiscard]] static const AssemblyCommandSemanticState& beforeState(
      const AssemblyCommand& command) noexcept;
  [[nodiscard]] static const AssemblyCommandSemanticState& afterState(
      const AssemblyCommand& command) noexcept;
  [[nodiscard]] AssemblyCommandHistoryStatus prepareSemanticResult(
      const AssemblyCommandSemanticState& state,
      std::optional<AssemblyCommandSemanticState>& prepared) noexcept;
  [[nodiscard]] AssemblyCommandHistoryStatus
  executeSyntheticTwoStepForTesting(
      OccurrenceDeltaCommand command, int& first, int& second,
      std::array<int, 4>& trace, std::size_t& traceSize,
      bool contradictory) noexcept;
  void throwInjectedFailure(std::uint8_t point);

  static constexpr std::size_t kMaximumCommands = 256;
  static constexpr std::size_t kMaximumRetainedBytes = 64 * 1024 * 1024;

  std::vector<Entry> commands_;
  std::size_t cursor_{};
  std::size_t retainedBytes_{};
  std::optional<Pending> pending_;
  AssemblyCommandRevision currentRevision_{1};
  AssemblyCommandRevision savepointRevision_{1};
  std::uint64_t nextRevisionValue_{2};
  bool busy_{};
  std::uint8_t testFailurePoint_{};
  std::uint8_t testFailureKind_{};
  DefinitionEditFailurePoint definitionEditFailurePointForTesting_{
      DefinitionEditFailurePoint::None};
};

}  // namespace solidar
