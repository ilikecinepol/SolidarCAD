#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/Document.h"
#include "model/TopologyReference.h"

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

// Non-QObject application service for committed model commands.  Document
// remains owned by MainWindow; this service owns only cold, bounded deltas and
// stable editor state needed to apply Undo/Redo symmetrically.
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

  [[nodiscard]] static DocumentDeltaMetrics inspectDelta(
      const Document& before, const Document& after);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace solidar
