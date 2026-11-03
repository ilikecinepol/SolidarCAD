#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/Document.h"
#include "model/EdgeReference.h"
#include "model/ProductDocument.h"
#include "model/TopologyReference.h"
#include "project/ProjectPersistenceState.h"
#include "ui/application/DocumentDelta.h"

namespace solidar {

struct EditorPresentationState {
  double bodyPositionX{};
  double bodyPositionY{};
  bool originVisible{true};
  bool sketchVisible{true};
  std::array<bool, 3> basePlanesVisible{false, false, false};
  std::vector<std::uint8_t> sketchVisibilities;

  bool operator==(const EditorPresentationState&) const = default;
};

// Plain committed editor state that accompanies a Document revision.  It is
// intentionally free of QObject/QWidget and Qt presentation types: the
// application shell converts stable IDs and typed presentation values back
// into current UI views only after a transaction has completed successfully.
struct EditorCommittedState {
  EditorPresentationState presentation;
  std::vector<SketchId> sketchViewIds;
  std::vector<BodyId> bodies;
  std::vector<EdgeReference> edges;
  std::vector<FaceReference> faces;
  BodyId historyBodyId{kInvalidBodyId};
  FeatureId historyFeatureId{kInvalidFeatureId};
  SketchId historySketchId{kInvalidSketchId};
  int historyPosition{};
  bool atEnd{true};
  std::optional<SketchId> historicalLegacyExtrusionSourceSketchId;

  bool operator==(const EditorCommittedState&) const = default;
};

enum class HistoryCommitStatus {
  Accepted,
  NoChange,
  Rejected,
};

struct HistoryCommitResult {
  HistoryCommitStatus status{HistoryCommitStatus::Rejected};
  EditorCommittedState state;
  std::string error;

  [[nodiscard]] bool accepted() const noexcept {
    return status != HistoryCommitStatus::Rejected;
  }
};

struct HistoryApplyResult {
  bool changed{};
  EditorCommittedState state;
  std::string error;
};

struct DocumentDeltaMetrics {
  std::size_t sliceCount{};
  std::size_t retainedBytes{};
};

enum class ModelCommandKind : std::uint8_t {
  DocumentTransition,
  ComponentExtraction,
  ComponentPlacement,
  ComponentHomePlacement,
};

enum class ModelCommandHistoryStatus : std::uint8_t {
  Prepared,
  Committed,
  NoChange,
  Busy,
  NothingToUndo,
  NothingToRedo,
  WrongCommandKind,
  InvalidCommand,
  ModelRejected,
  SuccessorRejected,
  SuccessorResourceFailure,
  ResourceLimit,
};

struct ComponentWorkspaceInstanceKey final {
  DefinitionId definitionId;
  std::optional<OccurrencePath> path;

  friend bool operator==(const ComponentWorkspaceInstanceKey&,
                         const ComponentWorkspaceInstanceKey&) = default;
};

struct ComponentWorkspaceBodyKey final {
  ComponentWorkspaceInstanceKey instance;
  BodyId bodyId{kInvalidBodyId};

  friend bool operator==(const ComponentWorkspaceBodyKey&,
                         const ComponentWorkspaceBodyKey&) = default;
};

struct ComponentWorkspaceDrawingKey final {
  ComponentWorkspaceInstanceKey instance;
  DrawingId drawingId{kInvalidDrawingId};

  friend bool operator==(const ComponentWorkspaceDrawingKey&,
                         const ComponentWorkspaceDrawingKey&) = default;
};

// Canonical cold-projection ordering: root first, then full occurrence path,
// then definition; bodies extend the owner key with BodyId. Capture code must
// sort by these comparators before preparing a command.
[[nodiscard]] bool componentWorkspaceInstanceKeyLess(
    const ComponentWorkspaceInstanceKey& left,
    const ComponentWorkspaceInstanceKey& right) noexcept;
[[nodiscard]] bool componentWorkspaceBodyKeyLess(
    const ComponentWorkspaceBodyKey& left,
    const ComponentWorkspaceBodyKey& right) noexcept;
[[nodiscard]] bool componentWorkspaceDrawingKeyLess(
    const ComponentWorkspaceDrawingKey& left,
    const ComponentWorkspaceDrawingKey& right) noexcept;

struct ComponentWorkspaceSemanticState final {
  ComponentWorkspaceInstanceKey activeInstance;
  std::optional<ComponentWorkspaceInstanceKey> selectedInstance;
  std::optional<ComponentWorkspaceBodyKey> selectedBody;
  std::string diagnostic;

  friend bool operator==(const ComponentWorkspaceSemanticState&,
                         const ComponentWorkspaceSemanticState&) = default;
};

struct ComponentExtractionColdCommand final {
  ComponentExtractionTransaction transaction;
  ComponentWorkspaceSemanticState beforeState;
  ComponentWorkspaceSemanticState afterState;
  std::vector<ComponentWorkspaceInstanceKey> beforeInstances;
  std::vector<ComponentWorkspaceInstanceKey> afterInstances;
  std::vector<ComponentWorkspaceBodyKey> beforeBodies;
  std::vector<ComponentWorkspaceBodyKey> afterBodies;
  project::ProjectPersistenceState beforePersistence;
  project::ProjectPersistenceState afterPersistence;
  std::vector<ComponentWorkspaceDrawingKey> beforeDrawings;
  std::vector<ComponentWorkspaceDrawingKey> afterDrawings;
};

struct ComponentExtractionHistoryResult final {
  ModelCommandHistoryStatus status{ModelCommandHistoryStatus::NoChange};
  std::optional<ComponentExtractionStatus> modelStatus;
  std::optional<ComponentWorkspaceSemanticState> restoredState;
  std::optional<project::ProjectPersistenceState> restoredPersistence;

  [[nodiscard]] bool changed() const noexcept {
    return status == ModelCommandHistoryStatus::Committed;
  }
};

struct ComponentPlacementColdCommand final {
  ComponentPlacementTransaction transaction;
  ComponentWorkspaceSemanticState beforeState;
  ComponentWorkspaceSemanticState afterState;
  std::vector<ComponentWorkspaceInstanceKey> beforeInstances;
  std::vector<ComponentWorkspaceInstanceKey> afterInstances;
  std::vector<ComponentWorkspaceBodyKey> beforeBodies;
  std::vector<ComponentWorkspaceBodyKey> afterBodies;
  project::ProjectPersistenceState beforePersistence;
  project::ProjectPersistenceState afterPersistence;
};

struct ComponentPlacementHistoryResult final {
  ModelCommandHistoryStatus status{ModelCommandHistoryStatus::NoChange};
  std::optional<ComponentPlacementStatus> modelStatus;
  std::optional<ComponentWorkspaceSemanticState> restoredState;
  std::optional<project::ProjectPersistenceState> restoredPersistence;

  [[nodiscard]] bool changed() const noexcept {
    return status == ModelCommandHistoryStatus::Committed;
  }
};

struct ComponentHomePlacementColdCommand final {
  ComponentHomePlacementTransaction transaction;
  ComponentWorkspaceSemanticState beforeState;
  ComponentWorkspaceSemanticState afterState;
  std::vector<ComponentWorkspaceInstanceKey> beforeInstances;
  std::vector<ComponentWorkspaceInstanceKey> afterInstances;
  std::vector<ComponentWorkspaceBodyKey> beforeBodies;
  std::vector<ComponentWorkspaceBodyKey> afterBodies;
  project::ProjectPersistenceState beforePersistence;
  project::ProjectPersistenceState afterPersistence;
  // Empty for "adopt current" and legacy single-occurrence restore. A
  // restore-all command keeps the first transaction above for compatibility
  // and the remaining ordered transactions here.
  std::vector<ComponentHomePlacementTransaction> additionalTransactions;
};

struct ComponentHomePlacementHistoryResult final {
  ModelCommandHistoryStatus status{ModelCommandHistoryStatus::NoChange};
  std::optional<ComponentHomePlacementStatus> modelStatus;
  std::optional<ComponentWorkspaceSemanticState> restoredState;
  std::optional<project::ProjectPersistenceState> restoredPersistence;

  [[nodiscard]] bool changed() const noexcept {
    return status == ModelCommandHistoryStatus::Committed;
  }
};

namespace detail {
class ModelCommandHistoryTestAccess;
}

// Non-QObject application service for committed model commands. The active
// root Document remains owned by MainWindow's ProductDocument; this service
// owns only cold, bounded deltas and stable editor state needed to apply
// Undo/Redo symmetrically.
class ModelCommandHistory final {
 public:
  static constexpr std::size_t kDefaultByteBudget =
      128U * 1024U * 1024U;
  static constexpr std::size_t kMaxCommands = 100;

  explicit ModelCommandHistory(
      std::size_t byteBudget = kDefaultByteBudget);
  ~ModelCommandHistory();

  ModelCommandHistory(ModelCommandHistory&&) noexcept;
  ModelCommandHistory& operator=(ModelCommandHistory&&) noexcept;
  ModelCommandHistory(const ModelCommandHistory&) = delete;
  ModelCommandHistory& operator=(const ModelCommandHistory&) = delete;

  [[nodiscard]] HistoryCommitResult recordTransition(
      Document& current, Document previous, EditorCommittedState before,
      EditorCommittedState after) noexcept;
  [[nodiscard]] HistoryApplyResult undo(
      Document& current, const EditorCommittedState& currentState) noexcept;
  [[nodiscard]] HistoryApplyResult redo(
      Document& current, const EditorCommittedState& currentState) noexcept;
  [[nodiscard]] ModelCommandHistoryStatus prepareComponentExtraction(
      const ProductDocument& product,
      const ComponentExtractionColdCommand& command) noexcept;
  [[nodiscard]] ModelCommandHistoryStatus commitPrepared() noexcept;
  void discardPrepared() noexcept;
  [[nodiscard]] ComponentExtractionHistoryResult undoComponentExtraction(
      ProductDocument& product,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentExtractionHistoryResult redoComponentExtraction(
      ProductDocument& product,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ModelCommandHistoryStatus prepareComponentPlacement(
      const ProductDocument& product,
      const ComponentPlacementColdCommand& command) noexcept;
  [[nodiscard]] ComponentPlacementHistoryResult undoComponentPlacement(
      ProductDocument& product,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentPlacementHistoryResult redoComponentPlacement(
      ProductDocument& product,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ModelCommandHistoryStatus prepareComponentHomePlacement(
      const ProductDocument& product,
      const ComponentHomePlacementColdCommand& command) noexcept;
  [[nodiscard]] ComponentHomePlacementHistoryResult
  undoComponentHomePlacement(
      ProductDocument& product,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;
  [[nodiscard]] ComponentHomePlacementHistoryResult
  redoComponentHomePlacement(
      ProductDocument& product,
      ProductDocumentSuccessor& prepareSuccessor) noexcept;

  void clear(bool currentIsSavepoint = true) noexcept;
  void markSavepoint() noexcept;
  void setByteBudget(std::size_t bytes) noexcept;

  [[nodiscard]] bool canUndo() const noexcept;
  [[nodiscard]] bool canRedo() const noexcept;
  [[nodiscard]] bool isApplying() const noexcept;
  [[nodiscard]] std::size_t undoCount() const noexcept;
  [[nodiscard]] std::size_t redoCount() const noexcept;
  [[nodiscard]] std::size_t retainedBytes() const noexcept;
  [[nodiscard]] std::size_t byteBudget() const noexcept;
  [[nodiscard]] bool isAtSavepoint() const noexcept;
  [[nodiscard]] std::uint64_t currentRevision() const noexcept;
  [[nodiscard]] std::optional<ModelCommandKind> nextUndoKind() const noexcept;
  [[nodiscard]] std::optional<ModelCommandKind> nextRedoKind() const noexcept;
  [[nodiscard]] std::optional<std::reference_wrapper<
      const ComponentWorkspaceSemanticState>>
  nextUndoComponentWorkspaceState() const noexcept;
  [[nodiscard]] std::optional<std::reference_wrapper<
      const ComponentWorkspaceSemanticState>>
  nextRedoComponentWorkspaceState() const noexcept;

  [[nodiscard]] static DocumentDeltaMetrics inspectDelta(
      const Document& before, const Document& after);

 private:
  friend class detail::ModelCommandHistoryTestAccess;
  void throwInjectedFailure(std::uint8_t point);
  void setNextRevisionForTesting(std::uint64_t value) noexcept;
  void setProjectionCompareLimitForTesting(std::size_t value) noexcept;
  [[nodiscard]] std::size_t projectionCompareCountForTesting() const noexcept;

  class Impl;
  std::unique_ptr<Impl> impl_;
  std::uint8_t testFailurePoint_{};
  std::uint8_t testFailureKind_{};
  std::size_t testProjectionCompareLimit_{8U * 1024U * 1024U};
  std::size_t testProjectionCompareCount_{};
};

}  // namespace solidar
