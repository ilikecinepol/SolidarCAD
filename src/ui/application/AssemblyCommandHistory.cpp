#include "ui/application/AssemblyCommandHistory.h"

#include <QByteArray>
#include <QByteArrayView>

#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "model/Document.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4ModelValidator.h"
#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/internal/ProjectFormatV4ApplicationGate.h"
#include "project/internal/ProjectFormatV4DocumentMaterialization.h"
#include "ui/application/internal/AssemblyCommandHistoryTestAccess.h"

namespace solidar {
namespace {

class BusyGuard final {
 public:
  explicit BusyGuard(bool& busy) noexcept : busy_(busy) { busy_ = true; }
  ~BusyGuard() { busy_ = false; }

  BusyGuard(const BusyGuard&) = delete;
  BusyGuard& operator=(const BusyGuard&) = delete;

 private:
  bool& busy_;
};

std::unique_ptr<Document> reconstruct(
    const std::vector<std::byte>& bytes) noexcept {
  try {
    const QByteArray encoded(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qsizetype>(bytes.size()));
    auto envelope =
        project::scanProjectFormatV4Envelope(QByteArrayView(encoded));
    if (!envelope.hasCandidate()) return {};
    auto payload =
        project::scanProjectFormatV4Payload(std::move(*envelope.candidate));
    if (!payload.hasCandidate()) return {};
    auto model = project::validateProjectFormatV4Model(
        std::move(*payload.candidate));
    if (model.status !=
            project::ProjectFormatV4ModelValidationStatus::Candidate ||
        !model.candidate)
      return {};
    auto materialized = project::detail::materializeProjectFormatV4Document(
        *model.candidate);
    if (!materialized.succeeded()) return {};
    const RecomputeResult recomputed =
        materialized.document->recomputeDetailed();
    if (!project::detail::acceptsProjectFormatV4RecomputeResult(recomputed))
      return {};
    return std::make_unique<Document>(std::move(*materialized.document));
  } catch (...) {
    return {};
  }
}

EmbeddedPartInsertPayload payloadFor(
    const EmbeddedPartInsertColdCommand& command,
    std::unique_ptr<Document> document = {}) {
  return {command.definitionId, command.occurrenceId, command.placement,
          command.flags, std::move(document), command.displayNameUtf8};
}

struct PreparedDefinitionEditCandidate final {
  std::unique_ptr<Document> document;
};

struct PreparedLinkedPartReloadCandidate final {
  std::unique_ptr<Document> document;
};

using PreparedUndoRedoPayload =
    std::variant<std::monostate, EmbeddedPartInsertPayload,
                 PreparedDefinitionEditCandidate,
                 PreparedLinkedPartReloadCandidate>;

AssemblyCommandHistoryStatus modelStatus(EmbeddedPartInsertStatus status) {
  if (status == EmbeddedPartInsertStatus::SuccessorResourceFailure)
    return AssemblyCommandHistoryStatus::ResourceLimit;
  return status == EmbeddedPartInsertStatus::SuccessorRejected
             ? AssemblyCommandHistoryStatus::SuccessorRejected
             : AssemblyCommandHistoryStatus::ModelRejected;
}

AssemblyCommandHistoryStatus modelStatus(DuplicateOccurrenceStatus status) {
  if (status == DuplicateOccurrenceStatus::SuccessorResourceFailure ||
      status == DuplicateOccurrenceStatus::AllocationFailure)
    return AssemblyCommandHistoryStatus::ResourceLimit;
  if (status == DuplicateOccurrenceStatus::SuccessorRejected)
    return AssemblyCommandHistoryStatus::SuccessorRejected;
  return status == DuplicateOccurrenceStatus::Busy
             ? AssemblyCommandHistoryStatus::Busy
             : AssemblyCommandHistoryStatus::ModelRejected;
}

AssemblyCommandHistoryStatus modelStatus(RemoveOccurrenceStatus status) {
  if (status == RemoveOccurrenceStatus::SuccessorResourceFailure ||
      status == RemoveOccurrenceStatus::AllocationFailure)
    return AssemblyCommandHistoryStatus::ResourceLimit;
  if (status == RemoveOccurrenceStatus::SuccessorRejected)
    return AssemblyCommandHistoryStatus::SuccessorRejected;
  return status == RemoveOccurrenceStatus::Busy
             ? AssemblyCommandHistoryStatus::Busy
             : AssemblyCommandHistoryStatus::ModelRejected;
}

AssemblyCommandHistoryStatus modelStatus(DefinitionEditStatus status) {
  if (status == DefinitionEditStatus::SuccessorResourceFailure ||
      status == DefinitionEditStatus::ResourceFailure)
    return AssemblyCommandHistoryStatus::ResourceLimit;
  if (status == DefinitionEditStatus::SuccessorRejected)
    return AssemblyCommandHistoryStatus::SuccessorRejected;
  return status == DefinitionEditStatus::Busy
             ? AssemblyCommandHistoryStatus::Busy
             : AssemblyCommandHistoryStatus::ModelRejected;
}

AssemblyCommandHistoryStatus modelStatus(LinkedPartMutationStatus status) {
  if (status == LinkedPartMutationStatus::SuccessorResourceFailure ||
      status == LinkedPartMutationStatus::ResourceFailure)
    return AssemblyCommandHistoryStatus::ResourceLimit;
  if (status == LinkedPartMutationStatus::SuccessorRejected)
    return AssemblyCommandHistoryStatus::SuccessorRejected;
  return status == LinkedPartMutationStatus::Busy
             ? AssemblyCommandHistoryStatus::Busy
             : status == LinkedPartMutationStatus::NoChange
                   ? AssemblyCommandHistoryStatus::NoChange
                   : AssemblyCommandHistoryStatus::ModelRejected;
}

AssemblyCommandHistoryStatus modelStatus(MatePlacementApplyStatus status) {
  if (status == MatePlacementApplyStatus::ResourceFailure)
    return AssemblyCommandHistoryStatus::ResourceLimit;
  if (status == MatePlacementApplyStatus::SuccessorRejected)
    return AssemblyCommandHistoryStatus::SuccessorRejected;
  if (status == MatePlacementApplyStatus::Busy)
    return AssemblyCommandHistoryStatus::Busy;
  return status == MatePlacementApplyStatus::Applied
             ? AssemblyCommandHistoryStatus::Committed
             : AssemblyCommandHistoryStatus::ModelRejected;
}

}  // namespace

class AssemblyCommandHistory::FinalizingSuccessor final
    : public ProductDocumentSuccessor {
 public:
  FinalizingSuccessor(AssemblyCommandHistory& history,
                      ProductDocumentSuccessor& successor) noexcept
      : history_(history), successor_(successor) {}

  ProductDocumentSuccessorStatus prepare(
      const ProductDocument& candidate,
      ProductDocumentSuccessorImpact impact) noexcept override {
    const auto status = successor_.prepare(candidate, impact);
    if (status != ProductDocumentSuccessorStatus::Prepared) return status;
    try {
      history_.throwInjectedFailure(static_cast<std::uint8_t>(
          detail::AssemblyCommandHistoryFailurePoint::Finalization));
      return ProductDocumentSuccessorStatus::Prepared;
    } catch (const std::bad_alloc&) {
      return ProductDocumentSuccessorStatus::ResourceFailure;
    } catch (...) {
      return ProductDocumentSuccessorStatus::Rejected;
    }
  }

 private:
  AssemblyCommandHistory& history_;
  ProductDocumentSuccessor& successor_;
};

AssemblyCommandHistory::AssemblyCommandHistory() noexcept = default;

AssemblyCommandHistoryStatus AssemblyCommandHistory::recordApplied(
    EmbeddedPartInsertColdCommand command) noexcept {
  const auto prepared = prepareRecord(std::move(command));
  return prepared == AssemblyCommandHistoryStatus::Committed
             ? commitPrepared()
             : prepared;
}

AssemblyCommandHistoryStatus AssemblyCommandHistory::prepareRecord(
    EmbeddedPartInsertColdCommand command) noexcept {
  if (!command.definitionId.isValid() || !command.occurrenceId.isValid() ||
      command.authoredDocument.empty())
    return AssemblyCommandHistoryStatus::InvalidCommand;
  return prepareCommand(AssemblyCommand(std::move(command)));
}

AssemblyCommandHistoryStatus AssemblyCommandHistory::prepareCommand(
    AssemblyCommand command) noexcept {
  if (busy_ || pending_) return AssemblyCommandHistoryStatus::Busy;
  if (cursor_ > commands_.size() || cursor_ >= kMaximumCommands ||
      nextRevisionValue_ == 0)
    return AssemblyCommandHistoryStatus::ResourceLimit;
  const auto bytes = dynamicBytes(command);
  if (!bytes || *bytes > kMaximumRetainedBytes)
    return AssemblyCommandHistoryStatus::ResourceLimit;

  std::size_t retainedPrefix = 0;
  for (std::size_t i = 0; i < cursor_; ++i) {
    if (retainedPrefix > kMaximumRetainedBytes - commands_[i].dynamicBytes)
      return AssemblyCommandHistoryStatus::ResourceLimit;
    retainedPrefix += commands_[i].dynamicBytes;
  }
  if (retainedPrefix > kMaximumRetainedBytes - *bytes)
    return AssemblyCommandHistoryStatus::ResourceLimit;

  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::Allocation));
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::Reserve));
    commands_.reserve(cursor_ + 1);
    pending_.emplace(
        Pending{std::move(command), *bytes, retainedPrefix + *bytes});
    busy_ = true;
    return AssemblyCommandHistoryStatus::Committed;
  } catch (...) {
    pending_.reset();
    busy_ = false;
    return AssemblyCommandHistoryStatus::ResourceLimit;
  }
}

AssemblyCommandHistoryStatus AssemblyCommandHistory::commitPrepared()
    noexcept {
  if (!busy_ || !pending_)
    return AssemblyCommandHistoryStatus::InvalidCommand;

  static_assert(std::is_nothrow_move_constructible_v<AssemblyCommand>);
  static_assert(std::is_nothrow_move_constructible_v<Entry>);
  if (cursor_ < commands_.size())
    commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                    commands_.end());
  commands_.push_back(Entry{std::move(pending_->command),
                            pending_->dynamicBytes, currentRevision_,
                            AssemblyCommandRevision{nextRevisionValue_}});
  retainedBytes_ = pending_->retainedBytesAfterCommit;
  ++nextRevisionValue_;
  ++cursor_;
  currentRevision_ = commands_.back().afterRevision;
  pending_.reset();
  busy_ = false;
  return AssemblyCommandHistoryStatus::Committed;
}

void AssemblyCommandHistory::discardPrepared() noexcept {
  pending_.reset();
  busy_ = false;
}

AssemblyCommandHistoryStatus AssemblyCommandHistory::prepareSemanticResult(
    const AssemblyCommandSemanticState& state,
    std::optional<AssemblyCommandSemanticState>& prepared) noexcept {
  prepared.reset();
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::
            SemanticStatePreparation));
    prepared.emplace(state);
    return AssemblyCommandHistoryStatus::Committed;
  } catch (const std::bad_alloc&) {
    prepared.reset();
    return AssemblyCommandHistoryStatus::ResourceLimit;
  } catch (...) {
    prepared.reset();
    return AssemblyCommandHistoryStatus::ModelRejected;
  }
}

AssemblyCommandHistoryResult AssemblyCommandHistory::executeOccurrenceDelta(
    ProductDocument& product,
    OccurrenceDeltaCommand command,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (busy_ || pending_) return {AssemblyCommandHistoryStatus::Busy};
  if (command.delta.isEmpty()) {
    BusyGuard guard(busy_);
    OccurrenceDeltaStatus status;
    try {
      throwInjectedFailure(static_cast<std::uint8_t>(
          detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
      status = product.applyOccurrenceDelta(command.delta);
    } catch (const std::bad_alloc&) {
      return {AssemblyCommandHistoryStatus::ResourceLimit};
    } catch (...) {
      return {AssemblyCommandHistoryStatus::ModelRejected};
    }
    if (status == OccurrenceDeltaStatus::NoChange)
      return {AssemblyCommandHistoryStatus::NoChange, status};
    if (status == OccurrenceDeltaStatus::Busy)
      return {AssemblyCommandHistoryStatus::Busy, status};
    return {AssemblyCommandHistoryStatus::ModelRejected, status};
  }
  std::optional<AssemblyCommandSemanticState> resultState;
  const auto semanticPrepared =
      prepareSemanticResult(command.afterState, resultState);
  if (semanticPrepared != AssemblyCommandHistoryStatus::Committed)
    return {semanticPrepared};
  const auto prepared = prepareCommand(AssemblyCommand(std::move(command)));
  if (prepared != AssemblyCommandHistoryStatus::Committed)
    return {prepared};
  auto* pendingCommand =
      std::get_if<OccurrenceDeltaCommand>(&pending_->command);
  if (!pendingCommand) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  }
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
    FinalizingSuccessor finalizing(*this, prepareSuccessor);
    const auto status =
        product.applyOccurrenceDelta(pendingCommand->delta, finalizing);
    if (status == OccurrenceDeltaStatus::NoChange) {
      discardPrepared();
      return {AssemblyCommandHistoryStatus::NoChange, status};
    }
    if (status != OccurrenceDeltaStatus::Committed) {
      discardPrepared();
      return {status == OccurrenceDeltaStatus::ResourceFailure
                  ? AssemblyCommandHistoryStatus::ResourceLimit
                  : status == OccurrenceDeltaStatus::SuccessorRejected
                        ? AssemblyCommandHistoryStatus::SuccessorRejected
                        : AssemblyCommandHistoryStatus::ModelRejected,
              status};
    }
    const auto committed = commitPrepared();
    return {committed, status,
            committed == AssemblyCommandHistoryStatus::Committed
                ? std::move(resultState)
                : std::nullopt};
  } catch (const std::bad_alloc&) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ModelRejected};
  }
}

AssemblyCommandHistoryResult AssemblyCommandHistory::executeDuplicate(
    ProductDocument& product,
    DuplicateOccurrenceColdCommand command,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (busy_ || pending_) return {AssemblyCommandHistoryStatus::Busy};
  if (!command.payload.duplicateId.isValid() ||
      command.payload.duplicateId.value() > kMaximumRootOccurrences ||
      !command.afterState.selectedOccurrence ||
      command.afterState.selectedOccurrence->depth() != 1 ||
      command.afterState.selectedOccurrence->entries().front() !=
          command.payload.duplicateId)
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  std::optional<AssemblyCommandSemanticState> resultState;
  const auto semanticPrepared =
      prepareSemanticResult(command.afterState, resultState);
  if (semanticPrepared != AssemblyCommandHistoryStatus::Committed)
    return {semanticPrepared};
  const auto prepared = prepareCommand(AssemblyCommand(std::move(command)));
  if (prepared != AssemblyCommandHistoryStatus::Committed)
    return {prepared};
  auto* pendingCommand =
      std::get_if<DuplicateOccurrenceColdCommand>(&pending_->command);
  if (!pendingCommand) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  }
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
    FinalizingSuccessor finalizing(*this, prepareSuccessor);
    const auto status = product.applyDuplicateOccurrence(
        pendingCommand->payload, finalizing,
        DuplicateOccurrenceFailurePoint::None);
    if (status != DuplicateOccurrenceStatus::Committed) {
      discardPrepared();
      return {modelStatus(status)};
    }
    const auto committed = commitPrepared();
    return {committed, std::nullopt,
            committed == AssemblyCommandHistoryStatus::Committed
                ? std::move(resultState)
                : std::nullopt};
  } catch (const std::bad_alloc&) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ModelRejected};
  }
}

AssemblyCommandHistoryResult AssemblyCommandHistory::executeRemove(
    ProductDocument& product, RemoveOccurrenceColdCommand command,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (busy_ || pending_) return {AssemblyCommandHistoryStatus::Busy};
  if (command.payload.occurrence.path.depth() != 1 ||
      !command.beforeState.selectedOccurrence ||
      *command.beforeState.selectedOccurrence !=
          command.payload.occurrence.path ||
      command.afterState.selectedOccurrence)
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  std::optional<AssemblyCommandSemanticState> resultState;
  const auto semanticPrepared =
      prepareSemanticResult(command.afterState, resultState);
  if (semanticPrepared != AssemblyCommandHistoryStatus::Committed)
    return {semanticPrepared};
  const auto prepared = prepareCommand(AssemblyCommand(std::move(command)));
  if (prepared != AssemblyCommandHistoryStatus::Committed)
    return {prepared};
  auto* pendingCommand =
      std::get_if<RemoveOccurrenceColdCommand>(&pending_->command);
  if (!pendingCommand) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  }
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
    FinalizingSuccessor finalizing(*this, prepareSuccessor);
    const auto status = product.applyRemoveOccurrence(
        pendingCommand->payload, finalizing,
        RemoveOccurrenceFailurePoint::None);
    if (status != RemoveOccurrenceStatus::Removed) {
      discardPrepared();
      return {modelStatus(status)};
    }
    const auto committed = commitPrepared();
    return {committed, std::nullopt,
            committed == AssemblyCommandHistoryStatus::Committed
                ? std::move(resultState)
                : std::nullopt};
  } catch (const std::bad_alloc&) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ModelRejected};
  }
}

AssemblyCommandHistoryResult AssemblyCommandHistory::executeDefinitionEdit(
    ProductDocument& product, DefinitionEditColdCommand command,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (busy_ || pending_) return {AssemblyCommandHistoryStatus::Busy};
  if (!command.address.productIncarnation.isValid() ||
      !command.address.rootDefinitionId.isValid() ||
      !command.address.targetDefinitionId.isValid() ||
      !command.address.expectedStateId.isValid() || !command.delta)
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  if (command.delta.empty())
    return {AssemblyCommandHistoryStatus::NoChange};
  std::optional<AssemblyCommandSemanticState> resultState;
  const auto semanticPrepared =
      prepareSemanticResult(command.afterState, resultState);
  if (semanticPrepared != AssemblyCommandHistoryStatus::Committed)
    return {semanticPrepared};
  const auto prepared = prepareCommand(AssemblyCommand(std::move(command)));
  if (prepared != AssemblyCommandHistoryStatus::Committed)
    return {prepared};
  auto* pendingCommand =
      std::get_if<DefinitionEditColdCommand>(&pending_->command);
  if (!pendingCommand) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  }
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
    const Document* current =
        product.partDocument(pendingCommand->address.targetDefinitionId);
    if (!current) {
      discardPrepared();
      return {AssemblyCommandHistoryStatus::ModelRejected};
    }
    Document candidate = *current;
    pendingCommand->delta.applyForwardUnrecomputed(candidate);
    FinalizingSuccessor finalizing(*this, prepareSuccessor);
    const auto result = product.applyDefinitionEdit(
        pendingCommand->address, candidate, finalizing,
        definitionEditFailurePointForTesting_);
    if (result.status != DefinitionEditStatus::Committed) {
      discardPrepared();
      return {modelStatus(result.status)};
    }
    pendingCommand->address.expectedStateId = result.committedStateId;
    const auto committed = commitPrepared();
    return {committed, std::nullopt,
            committed == AssemblyCommandHistoryStatus::Committed
                ? std::move(resultState)
                : std::nullopt};
  } catch (const std::bad_alloc&) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ReconstructionFailed};
  }
}

AssemblyCommandHistoryResult AssemblyCommandHistory::executeLinkedPartReload(
    ProductDocument& product, LinkedPartReloadColdCommand command,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (busy_ || pending_) return {AssemblyCommandHistoryStatus::Busy};
  if (!command.address.productIncarnation.isValid() ||
      !command.address.rootDefinitionId.isValid() ||
      !command.address.targetDefinitionId.isValid() ||
      !command.address.expectedStateId.isValid() || !command.delta ||
      !command.beforeBinding.isValid() || !command.afterBinding.isValid())
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  if (command.delta.empty() &&
      command.beforeBinding == command.afterBinding)
    return {AssemblyCommandHistoryStatus::NoChange};
  std::optional<AssemblyCommandSemanticState> resultState;
  const auto semanticPrepared =
      prepareSemanticResult(command.afterState, resultState);
  if (semanticPrepared != AssemblyCommandHistoryStatus::Committed)
    return {semanticPrepared};
  const auto prepared = prepareCommand(AssemblyCommand(std::move(command)));
  if (prepared != AssemblyCommandHistoryStatus::Committed) return {prepared};
  auto* pendingCommand =
      std::get_if<LinkedPartReloadColdCommand>(&pending_->command);
  if (!pendingCommand) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  }
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
    const Document* current =
        product.partDocument(pendingCommand->address.targetDefinitionId);
    if (!current) {
      discardPrepared();
      return {AssemblyCommandHistoryStatus::ModelRejected};
    }
    Document candidate = *current;
    pendingCommand->delta.applyForwardUnrecomputed(candidate);
    FinalizingSuccessor finalizing(*this, prepareSuccessor);
    const auto result = product.applyLinkedPartReload(
        pendingCommand->address, pendingCommand->beforeBinding,
        pendingCommand->afterBinding, candidate, finalizing,
        definitionEditFailurePointForTesting_);
    if (result.status != LinkedPartMutationStatus::Committed) {
      discardPrepared();
      return {modelStatus(result.status)};
    }
    pendingCommand->address.expectedStateId = result.committedStateId;
    const auto committed = commitPrepared();
    return {committed, std::nullopt,
            committed == AssemblyCommandHistoryStatus::Committed
                ? std::move(resultState)
                : std::nullopt};
  } catch (const std::bad_alloc&) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ReconstructionFailed};
  }
}

AssemblyCommandHistoryResult AssemblyCommandHistory::executeLinkedPartRepair(
    ProductDocument& product, LinkedPartRepairColdCommand command,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (busy_ || pending_) return {AssemblyCommandHistoryStatus::Busy};
  if (!command.address.productIncarnation.isValid() ||
      !command.address.rootDefinitionId.isValid() ||
      !command.address.targetDefinitionId.isValid() ||
      !command.address.expectedStateId.isValid() ||
      !command.beforeBinding.isValid() || !command.afterBinding.isValid())
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  if (command.beforeBinding == command.afterBinding)
    return {AssemblyCommandHistoryStatus::NoChange};
  std::optional<AssemblyCommandSemanticState> resultState;
  const auto semanticPrepared =
      prepareSemanticResult(command.afterState, resultState);
  if (semanticPrepared != AssemblyCommandHistoryStatus::Committed)
    return {semanticPrepared};
  const auto prepared = prepareCommand(AssemblyCommand(std::move(command)));
  if (prepared != AssemblyCommandHistoryStatus::Committed) return {prepared};
  auto* pendingCommand =
      std::get_if<LinkedPartRepairColdCommand>(&pending_->command);
  if (!pendingCommand) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  }
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
    FinalizingSuccessor finalizing(*this, prepareSuccessor);
    const auto result = product.applyLinkedPartRepair(
        pendingCommand->address, pendingCommand->beforeBinding,
        pendingCommand->afterBinding, finalizing,
        definitionEditFailurePointForTesting_);
    if (result.status != LinkedPartMutationStatus::Committed) {
      discardPrepared();
      return {modelStatus(result.status)};
    }
    pendingCommand->address.expectedStateId = result.committedStateId;
    const auto committed = commitPrepared();
    return {committed, std::nullopt,
            committed == AssemblyCommandHistoryStatus::Committed
                ? std::move(resultState)
                : std::nullopt};
  } catch (const std::bad_alloc&) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ModelRejected};
  }
}

AssemblyCommandHistoryResult AssemblyCommandHistory::executePlanarMate(
    ProductDocument& product, PlanarMateColdCommand command,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (busy_ || pending_) return {AssemblyCommandHistoryStatus::Busy};
  if (!command.transaction)
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  const auto& transaction = *command.transaction;
  const auto* after =
      transaction.afterRegistry().find(transaction.relation().id());
  if (!transaction.integrityValid() ||
      !transaction.ownerAssemblyId().isValid() ||
      transaction.candidate().address().ownerAssemblyId !=
          transaction.ownerAssemblyId() ||
      !after || *after != transaction.relation() ||
      transaction.beforeRegistry().findMate(transaction.relation().id()))
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  std::optional<AssemblyCommandSemanticState> resultState;
  const auto semanticPrepared =
      prepareSemanticResult(command.afterState, resultState);
  if (semanticPrepared != AssemblyCommandHistoryStatus::Committed)
    return {semanticPrepared};
  const auto prepared = prepareCommand(AssemblyCommand(std::move(command)));
  if (prepared != AssemblyCommandHistoryStatus::Committed) return {prepared};
  auto* pendingCommand = std::get_if<PlanarMateColdCommand>(&pending_->command);
  if (!pendingCommand) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::InvalidCommand};
  }
  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
    FinalizingSuccessor finalizing(*this, prepareSuccessor);
    const auto status = product.applyPlanarMateAuthoring(
        *pendingCommand->transaction, finalizing);
    if (status != MatePlacementApplyStatus::Applied) {
      discardPrepared();
      return {modelStatus(status)};
    }
    const auto committed = commitPrepared();
    return {committed, std::nullopt,
            committed == AssemblyCommandHistoryStatus::Committed
                ? std::move(resultState)
                : std::nullopt};
  } catch (const std::bad_alloc&) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    discardPrepared();
    return {AssemblyCommandHistoryStatus::ModelRejected};
  }
}

AssemblyCommandHistoryResult AssemblyCommandHistory::undo(
    ProductDocument& product,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (busy_ || pending_) return {AssemblyCommandHistoryStatus::Busy};
  if (!canUndo()) return {AssemblyCommandHistoryStatus::NothingToUndo};
  Entry& entry = commands_[cursor_ - 1];
  std::optional<AssemblyCommandSemanticState> resultState;
  const auto semanticPrepared =
      prepareSemanticResult(beforeState(entry.command), resultState);
  if (semanticPrepared != AssemblyCommandHistoryStatus::Committed)
    return {semanticPrepared};
  PreparedUndoRedoPayload preparedPayload;
  try {
    if (const auto* insert =
            std::get_if<EmbeddedPartInsertColdCommand>(&entry.command)) {
      throwInjectedFailure(static_cast<std::uint8_t>(
          detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation));
      preparedPayload.emplace<EmbeddedPartInsertPayload>(payloadFor(*insert));
    } else if (const auto* edit =
                   std::get_if<DefinitionEditColdCommand>(&entry.command)) {
      throwInjectedFailure(static_cast<std::uint8_t>(
          detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation));
      const Document* current =
          product.partDocument(edit->address.targetDefinitionId);
      if (!current) return {AssemblyCommandHistoryStatus::ModelRejected};
      auto candidate = std::make_unique<Document>(*current);
      edit->delta.applyBackwardUnrecomputed(*candidate);
      preparedPayload.emplace<PreparedDefinitionEditCandidate>(
          PreparedDefinitionEditCandidate{std::move(candidate)});
    } else if (const auto* reload =
                   std::get_if<LinkedPartReloadColdCommand>(&entry.command)) {
      throwInjectedFailure(static_cast<std::uint8_t>(
          detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation));
      const Document* current =
          product.partDocument(reload->address.targetDefinitionId);
      if (!current) return {AssemblyCommandHistoryStatus::ModelRejected};
      auto candidate = std::make_unique<Document>(*current);
      reload->delta.applyBackwardUnrecomputed(*candidate);
      preparedPayload.emplace<PreparedLinkedPartReloadCandidate>(
          PreparedLinkedPartReloadCandidate{std::move(candidate)});
    }
  } catch (const std::bad_alloc&) {
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    return {AssemblyCommandHistoryStatus::ReconstructionFailed};
  }
  BusyGuard guard(busy_);

  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
  } catch (const std::bad_alloc&) {
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    return {AssemblyCommandHistoryStatus::ModelRejected};
  }

  FinalizingSuccessor finalizing(*this, prepareSuccessor);
  if (const auto* insert =
          std::get_if<EmbeddedPartInsertColdCommand>(&entry.command)) {
    auto* payload = std::get_if<EmbeddedPartInsertPayload>(&preparedPayload);
    Q_ASSERT(payload);
    if (!payload) return {AssemblyCommandHistoryStatus::ModelRejected};
    const auto status = product.extractEmbeddedPartInsert(*payload, finalizing);
    if (status != EmbeddedPartInsertStatus::Extracted)
      return {modelStatus(status)};
    payload->document.reset();
  } else if (const auto* delta =
                 std::get_if<OccurrenceDeltaCommand>(&entry.command)) {
    const auto status =
        product.revertOccurrenceDelta(delta->delta, finalizing);
    if (status != OccurrenceDeltaStatus::Committed)
      return {status == OccurrenceDeltaStatus::ResourceFailure
                  ? AssemblyCommandHistoryStatus::ResourceLimit
                  : status == OccurrenceDeltaStatus::SuccessorRejected
                        ? AssemblyCommandHistoryStatus::SuccessorRejected
                        : AssemblyCommandHistoryStatus::ModelRejected,
              status};
  } else if (const auto* duplicate =
                 std::get_if<DuplicateOccurrenceColdCommand>(
                     &entry.command)) {
    const auto status = product.extractDuplicateOccurrence(
        duplicate->payload, finalizing,
        DuplicateOccurrenceFailurePoint::None);
    if (status != DuplicateOccurrenceStatus::Extracted)
      return {modelStatus(status)};
  } else if (const auto* remove =
                 std::get_if<RemoveOccurrenceColdCommand>(&entry.command)) {
    const auto status = product.restoreRemovedOccurrence(
        remove->payload, finalizing, RemoveOccurrenceFailurePoint::None);
    if (status != RemoveOccurrenceStatus::Restored)
      return {modelStatus(status)};
  } else if (auto* edit =
                 std::get_if<DefinitionEditColdCommand>(&entry.command)) {
    auto* prepared =
        std::get_if<PreparedDefinitionEditCandidate>(&preparedPayload);
    Q_ASSERT(prepared && prepared->document);
    if (!prepared || !prepared->document)
      return {AssemblyCommandHistoryStatus::ModelRejected};
    const auto result = product.revertDefinitionEdit(
        edit->address, *prepared->document, finalizing,
        definitionEditFailurePointForTesting_);
    if (result.status != DefinitionEditStatus::Committed)
      return {modelStatus(result.status)};
    edit->address.expectedStateId = result.committedStateId;
  } else if (auto* reload =
                 std::get_if<LinkedPartReloadColdCommand>(&entry.command)) {
    auto* prepared =
        std::get_if<PreparedLinkedPartReloadCandidate>(&preparedPayload);
    Q_ASSERT(prepared && prepared->document);
    if (!prepared || !prepared->document)
      return {AssemblyCommandHistoryStatus::ModelRejected};
    const auto result = product.applyLinkedPartReload(
        reload->address, reload->afterBinding, reload->beforeBinding,
        *prepared->document, finalizing, definitionEditFailurePointForTesting_);
    if (result.status != LinkedPartMutationStatus::Committed)
      return {modelStatus(result.status)};
    reload->address.expectedStateId = result.committedStateId;
  } else if (auto* repair =
                 std::get_if<LinkedPartRepairColdCommand>(&entry.command)) {
    const auto result = product.applyLinkedPartRepair(
        repair->address, repair->afterBinding, repair->beforeBinding,
        finalizing, definitionEditFailurePointForTesting_);
    if (result.status != LinkedPartMutationStatus::Committed)
      return {modelStatus(result.status)};
    repair->address.expectedStateId = result.committedStateId;
  } else if (const auto* mate =
                 std::get_if<PlanarMateColdCommand>(&entry.command)) {
    const auto status =
        product.revertPlanarMateAuthoring(*mate->transaction, finalizing);
    if (status != MatePlacementApplyStatus::Applied)
      return {modelStatus(status)};
  } else {
    return {AssemblyCommandHistoryStatus::ModelRejected};
  }
  --cursor_;
  currentRevision_ = entry.beforeRevision;
  return {AssemblyCommandHistoryStatus::Committed, std::nullopt,
          std::move(resultState)};
}

AssemblyCommandHistoryResult AssemblyCommandHistory::redo(
    ProductDocument& product,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (busy_ || pending_) return {AssemblyCommandHistoryStatus::Busy};
  if (!canRedo()) return {AssemblyCommandHistoryStatus::NothingToRedo};
  Entry& entry = commands_[cursor_];
  std::optional<AssemblyCommandSemanticState> resultState;
  const auto semanticPrepared =
      prepareSemanticResult(afterState(entry.command), resultState);
  if (semanticPrepared != AssemblyCommandHistoryStatus::Committed)
    return {semanticPrepared};
  PreparedUndoRedoPayload preparedPayload;
  try {
    if (const auto* insert =
            std::get_if<EmbeddedPartInsertColdCommand>(&entry.command)) {
      throwInjectedFailure(static_cast<std::uint8_t>(
          detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation));
      auto document = reconstruct(insert->authoredDocument);
      if (!document)
        return {AssemblyCommandHistoryStatus::ReconstructionFailed};
      preparedPayload.emplace<EmbeddedPartInsertPayload>(
          payloadFor(*insert, std::move(document)));
    } else if (const auto* edit =
                   std::get_if<DefinitionEditColdCommand>(&entry.command)) {
      throwInjectedFailure(static_cast<std::uint8_t>(
          detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation));
      const Document* current =
          product.partDocument(edit->address.targetDefinitionId);
      if (!current) return {AssemblyCommandHistoryStatus::ModelRejected};
      auto candidate = std::make_unique<Document>(*current);
      edit->delta.applyForwardUnrecomputed(*candidate);
      preparedPayload.emplace<PreparedDefinitionEditCandidate>(
          PreparedDefinitionEditCandidate{std::move(candidate)});
    } else if (const auto* reload =
                   std::get_if<LinkedPartReloadColdCommand>(&entry.command)) {
      throwInjectedFailure(static_cast<std::uint8_t>(
          detail::AssemblyCommandHistoryFailurePoint::PayloadPreparation));
      const Document* current =
          product.partDocument(reload->address.targetDefinitionId);
      if (!current) return {AssemblyCommandHistoryStatus::ModelRejected};
      auto candidate = std::make_unique<Document>(*current);
      reload->delta.applyForwardUnrecomputed(*candidate);
      preparedPayload.emplace<PreparedLinkedPartReloadCandidate>(
          PreparedLinkedPartReloadCandidate{std::move(candidate)});
    }
  } catch (const std::bad_alloc&) {
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    return {AssemblyCommandHistoryStatus::ReconstructionFailed};
  }
  BusyGuard guard(busy_);

  try {
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::BeforeOperation));
  } catch (const std::bad_alloc&) {
    return {AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    return {AssemblyCommandHistoryStatus::ModelRejected};
  }

  FinalizingSuccessor finalizing(*this, prepareSuccessor);
  if (const auto* insert =
          std::get_if<EmbeddedPartInsertColdCommand>(&entry.command)) {
    auto* payload = std::get_if<EmbeddedPartInsertPayload>(&preparedPayload);
    Q_ASSERT(payload);
    if (!payload) return {AssemblyCommandHistoryStatus::ModelRejected};
    const auto status = product.applyEmbeddedPartInsert(*payload, finalizing);
    if (status != EmbeddedPartInsertStatus::Committed)
      return {modelStatus(status)};
  } else if (const auto* delta =
                 std::get_if<OccurrenceDeltaCommand>(&entry.command)) {
    const auto status = product.applyOccurrenceDelta(delta->delta, finalizing);
    if (status != OccurrenceDeltaStatus::Committed)
      return {status == OccurrenceDeltaStatus::ResourceFailure
                  ? AssemblyCommandHistoryStatus::ResourceLimit
                  : status == OccurrenceDeltaStatus::SuccessorRejected
                        ? AssemblyCommandHistoryStatus::SuccessorRejected
                        : AssemblyCommandHistoryStatus::ModelRejected,
              status};
  } else if (const auto* duplicate =
                 std::get_if<DuplicateOccurrenceColdCommand>(
                     &entry.command)) {
    const auto status = product.applyDuplicateOccurrence(
        duplicate->payload, finalizing,
        DuplicateOccurrenceFailurePoint::None);
    if (status != DuplicateOccurrenceStatus::Committed)
      return {modelStatus(status)};
  } else if (const auto* remove =
                 std::get_if<RemoveOccurrenceColdCommand>(&entry.command)) {
    const auto status = product.applyRemoveOccurrence(
        remove->payload, finalizing, RemoveOccurrenceFailurePoint::None);
    if (status != RemoveOccurrenceStatus::Removed)
      return {modelStatus(status)};
  } else if (auto* edit =
                 std::get_if<DefinitionEditColdCommand>(&entry.command)) {
    auto* prepared =
        std::get_if<PreparedDefinitionEditCandidate>(&preparedPayload);
    Q_ASSERT(prepared && prepared->document);
    if (!prepared || !prepared->document)
      return {AssemblyCommandHistoryStatus::ModelRejected};
    const auto result = product.applyDefinitionEdit(
        edit->address, *prepared->document, finalizing,
        definitionEditFailurePointForTesting_);
    if (result.status != DefinitionEditStatus::Committed)
      return {modelStatus(result.status)};
    edit->address.expectedStateId = result.committedStateId;
  } else if (auto* reload =
                 std::get_if<LinkedPartReloadColdCommand>(&entry.command)) {
    auto* prepared =
        std::get_if<PreparedLinkedPartReloadCandidate>(&preparedPayload);
    Q_ASSERT(prepared && prepared->document);
    if (!prepared || !prepared->document)
      return {AssemblyCommandHistoryStatus::ModelRejected};
    const auto result = product.applyLinkedPartReload(
        reload->address, reload->beforeBinding, reload->afterBinding,
        *prepared->document, finalizing, definitionEditFailurePointForTesting_);
    if (result.status != LinkedPartMutationStatus::Committed)
      return {modelStatus(result.status)};
    reload->address.expectedStateId = result.committedStateId;
  } else if (auto* repair =
                 std::get_if<LinkedPartRepairColdCommand>(&entry.command)) {
    const auto result = product.applyLinkedPartRepair(
        repair->address, repair->beforeBinding, repair->afterBinding,
        finalizing, definitionEditFailurePointForTesting_);
    if (result.status != LinkedPartMutationStatus::Committed)
      return {modelStatus(result.status)};
    repair->address.expectedStateId = result.committedStateId;
  } else if (const auto* mate =
                 std::get_if<PlanarMateColdCommand>(&entry.command)) {
    const auto status =
        product.applyPlanarMateAuthoring(*mate->transaction, finalizing);
    if (status != MatePlacementApplyStatus::Applied)
      return {modelStatus(status)};
  } else {
    return {AssemblyCommandHistoryStatus::ModelRejected};
  }

  ++cursor_;
  currentRevision_ = entry.afterRevision;
  return {AssemblyCommandHistoryStatus::Committed, std::nullopt,
          std::move(resultState)};
}

void AssemblyCommandHistory::clear() noexcept {
  pending_.reset();
  commands_.clear();
  cursor_ = 0;
  retainedBytes_ = 0;
  busy_ = false;
  if (nextRevisionValue_ != 0) {
    currentRevision_ = AssemblyCommandRevision{nextRevisionValue_};
    ++nextRevisionValue_;
  }
  savepointRevision_ = currentRevision_;
}

const std::optional<OccurrencePath>&
AssemblyCommandHistory::currentOccurrence() const noexcept {
  static const std::optional<OccurrencePath> kNoOccurrence;
  return cursor_ == 0
             ? kNoOccurrence
             : afterState(commands_[cursor_ - 1].command).selectedOccurrence;
}

std::optional<std::reference_wrapper<const AssemblyCommandSemanticState>>
AssemblyCommandHistory::nextUndoState() const noexcept {
  if (!canUndo()) return std::nullopt;
  return std::cref(beforeState(commands_[cursor_ - 1].command));
}

std::optional<std::reference_wrapper<const AssemblyCommandSemanticState>>
AssemblyCommandHistory::nextRedoState() const noexcept {
  if (!canRedo()) return std::nullopt;
  return std::cref(afterState(commands_[cursor_].command));
}

namespace {
std::optional<DefinitionId> linkedDefinition(
    const AssemblyCommand& command) noexcept {
  return std::visit(
      [](const auto& typed) -> std::optional<DefinitionId> {
        using Command = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Command, LinkedPartReloadColdCommand> ||
                      std::is_same_v<Command, LinkedPartRepairColdCommand>)
          return typed.address.targetDefinitionId;
        return std::nullopt;
      },
      command);
}
}  // namespace

std::optional<DefinitionId>
AssemblyCommandHistory::nextUndoLinkedDefinition() const noexcept {
  return canUndo() ? linkedDefinition(commands_[cursor_ - 1].command)
                   : std::nullopt;
}

std::optional<DefinitionId>
AssemblyCommandHistory::nextRedoLinkedDefinition() const noexcept {
  return canRedo() ? linkedDefinition(commands_[cursor_].command)
                   : std::nullopt;
}

std::optional<std::size_t>
AssemblyCommandHistory::mateRegistryDynamicBytesForTesting(
    const MateRegistry& registry) noexcept {
  std::size_t bytes = 0;
  const auto add = [&bytes](std::size_t value) {
    if (value > std::numeric_limits<std::size_t>::max() - bytes) return false;
    bytes += value;
    return true;
  };
  const auto addProduct = [&add](std::size_t count, std::size_t width) {
    return count <= std::numeric_limits<std::size_t>::max() / width &&
           add(count * width);
  };
  const auto addReference = [&add, &addProduct](const auto& ref) {
    if (const auto* v1 = ref.v1()) return add(v1->persistentTag.capacity());
    const auto* v2 = ref.v2();
    if (!v2 || !add(v2->provenance.localName.capacity()) ||
        !addProduct(v2->provenance.origins.capacity(),
                    sizeof(TopologyProvenanceOrigin)))
      return false;
    for (const auto& origin : v2->provenance.origins)
      if (!add(origin.localName.capacity())) return false;
    return true;
  };
  const auto addPath = [&addProduct](const OccurrencePath& path) {
    return addProduct(path.entries().capacity(), sizeof(OccurrenceId));
  };
  const auto addPlanar = [&addPath, &addReference](const MateEndpoint& endpoint) {
    return addPath(endpoint.occurrencePath) && addReference(endpoint.face);
  };
  constexpr std::size_t kConservativeMapNodeOverhead = 5 * sizeof(void*);
  constexpr std::size_t kConservativeMapNodeBytes =
      sizeof(std::pair<const MateId, AssemblyMate>) +
      kConservativeMapNodeOverhead;
  // Charge one full sentinel/head node even when the registry is empty.
  if (!add(kConservativeMapNodeBytes) ||
      !addProduct(registry.relations().size(), kConservativeMapNodeBytes))
    return std::nullopt;
  for (const auto& [id, relation] : registry.relations()) {
    static_cast<void>(id);
    if (!std::visit(
            [&](const auto& mate) {
              using Mate = std::decay_t<decltype(mate)>;
              if constexpr (std::is_same_v<Mate, AxisConcentricMate>) {
                const auto endpoint = [&](const MateAxisEndpoint& value) {
                  if (!addPath(value.occurrencePath)) return false;
                  const auto* face =
                      std::get_if<CylindricalFaceAxis>(&value.reference);
                  return !face || addReference(face->face);
                };
                return endpoint(mate.first()) && endpoint(mate.second());
              } else if constexpr (std::is_same_v<Mate, PointDistanceMate>) {
                const auto endpoint = [&](const MatePointEndpoint& value) {
                  if (!addPath(value.occurrencePath)) return false;
                  const auto* edge =
                      std::get_if<CircularEdgeCenter>(&value.reference);
                  return !edge || addReference(edge->edge);
                };
                return endpoint(mate.first()) && endpoint(mate.second());
              } else {
                return addPlanar(mate.first()) && addPlanar(mate.second());
              }
            },
            relation))
      return std::nullopt;
  }
  return bytes;
}

std::optional<std::size_t> AssemblyCommandHistory::dynamicBytes(
    const AssemblyCommand& command) noexcept {
  const auto base = std::visit(
      [](const auto& typed) -> std::optional<std::size_t> {
        using Command = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Command,
                                     EmbeddedPartInsertColdCommand>) {
          const std::size_t nameBytes = typed.displayNameUtf8.empty()
                                                ? 0
                                                : typed.displayNameUtf8.capacity();
          if (typed.authoredDocument.capacity() >
              std::numeric_limits<std::size_t>::max() -
                  nameBytes)
            return std::nullopt;
          return typed.authoredDocument.capacity() +
                 nameBytes;
        } else if constexpr (std::is_same_v<Command,
                                            OccurrenceDeltaCommand>) {
          const std::size_t capacity =
              typed.delta.address().path.entries().capacity();
          if (capacity > std::numeric_limits<std::size_t>::max() /
                          sizeof(OccurrenceId))
            return std::nullopt;
          std::size_t bytes = capacity * sizeof(OccurrenceId);
          if (const auto* rename = typed.delta.rename()) {
            if (bytes > std::numeric_limits<std::size_t>::max() -
                            rename->beforeUtf8.capacity())
              return std::nullopt;
            bytes += rename->beforeUtf8.capacity();
            if (bytes > std::numeric_limits<std::size_t>::max() -
                            rename->afterUtf8.capacity())
              return std::nullopt;
            bytes += rename->afterUtf8.capacity();
          }
          return bytes;
        } else if constexpr (std::is_same_v<
                                 Command, DuplicateOccurrenceColdCommand>) {
          const std::size_t capacity =
              typed.payload.source.path.entries().capacity();
          if (capacity > std::numeric_limits<std::size_t>::max() /
                             sizeof(OccurrenceId))
            return std::nullopt;
          const std::size_t pathBytes = capacity * sizeof(OccurrenceId);
          if (pathBytes > std::numeric_limits<std::size_t>::max() -
                              typed.payload.displayNameUtf8.capacity())
            return std::nullopt;
          return pathBytes + typed.payload.displayNameUtf8.capacity();
        } else if constexpr (std::is_same_v<
                                 Command, RemoveOccurrenceColdCommand>) {
          const std::size_t capacity =
              typed.payload.occurrence.path.entries().capacity();
          if (capacity > std::numeric_limits<std::size_t>::max() /
                             sizeof(OccurrenceId))
            return std::nullopt;
          const std::size_t pathBytes = capacity * sizeof(OccurrenceId);
          if (pathBytes > std::numeric_limits<std::size_t>::max() -
                              typed.payload.displayNameUtf8.capacity())
            return std::nullopt;
          return pathBytes + typed.payload.displayNameUtf8.capacity();
        } else if constexpr (std::is_same_v<Command,
                                            DefinitionEditColdCommand>) {
          return typed.delta.retainedBytes();
        } else if constexpr (std::is_same_v<Command,
                                            LinkedPartReloadColdCommand>) {
          const std::size_t before =
              typed.beforeBinding.relativePathHintUtf8.capacity();
          const std::size_t after =
              typed.afterBinding.relativePathHintUtf8.capacity();
          const std::size_t delta = typed.delta.retainedBytes();
          if (before > std::numeric_limits<std::size_t>::max() - after)
            return std::nullopt;
          const std::size_t paths = before + after;
          if (paths > std::numeric_limits<std::size_t>::max() - delta)
            return std::nullopt;
          return paths + delta;
        } else if constexpr (std::is_same_v<
                                 Command, LinkedPartRepairColdCommand>) {
          const std::size_t before =
              typed.beforeBinding.relativePathHintUtf8.capacity();
          const std::size_t after =
              typed.afterBinding.relativePathHintUtf8.capacity();
          return before > std::numeric_limits<std::size_t>::max() - after
                     ? std::nullopt
                     : std::optional<std::size_t>(before + after);
        } else {
          if (!typed.transaction) return std::nullopt;
          const auto& transaction = *typed.transaction;
          std::size_t bytes = sizeof(PlanarMateAuthoringTransaction);
          const auto add = [&bytes](std::size_t value) {
            if (value > std::numeric_limits<std::size_t>::max() - bytes)
              return false;
            bytes += value;
            return true;
          };
          const auto addProduct = [&add](std::size_t count,
                                         std::size_t width) {
            return count <= std::numeric_limits<std::size_t>::max() / width &&
                   add(count * width);
          };
          const auto addTopologyReference = [&add, &addProduct](const auto& ref) {
            if (const auto* v1 = ref.v1())
              return add(v1->persistentTag.capacity());
            const auto* v2 = ref.v2();
            if (!v2 || !add(v2->provenance.localName.capacity()) ||
                !addProduct(v2->provenance.origins.capacity(),
                            sizeof(TopologyProvenanceOrigin)))
              return false;
            for (const auto& origin : v2->provenance.origins)
              if (!add(origin.localName.capacity())) return false;
            return true;
          };
          const auto addPath = [&addProduct](const OccurrencePath& path) {
            return addProduct(path.entries().capacity(), sizeof(OccurrenceId));
          };
          const auto addPlanarEndpoint =
              [&addPath, &addTopologyReference](const MateEndpoint& endpoint) {
                return addPath(endpoint.occurrencePath) &&
                       addTopologyReference(endpoint.face);
              };
          if (!addPlanarEndpoint(transaction.relation().first()) ||
              !addPlanarEndpoint(transaction.relation().second()))
            return std::nullopt;
          for (const auto* registry : {&transaction.beforeRegistry(),
                                       &transaction.afterRegistry()}) {
            const auto registryBytes =
                AssemblyCommandHistory::mateRegistryDynamicBytesForTesting(
                    *registry);
            if (!registryBytes || !add(*registryBytes)) return std::nullopt;
          }
          const auto& candidate = transaction.candidate();
          if (!addProduct(candidate.componentMateIds().capacity(),
                          sizeof(MateId)) ||
              !addProduct(candidate.changes().capacity(),
                          sizeof(MatePlacementChange)) ||
              !addProduct(candidate.occurrenceDependencies().capacity(),
                          sizeof(MateOccurrenceDependencySeal)) ||
              !addProduct(candidate.definitionDependencies().capacity(),
                          sizeof(MateDefinitionDependencySeal)))
            return std::nullopt;
          for (const auto& dependency : candidate.occurrenceDependencies())
            if (!add(dependency.displayNameUtf8.capacity()))
              return std::nullopt;
          return bytes;
        }
      },
      command);
  if (!base) return std::nullopt;
  const auto stateBytes = [](const AssemblyCommandSemanticState& state)
      -> std::optional<std::size_t> {
    if (!state.selectedOccurrence) return std::size_t{0};
    const std::size_t capacity =
        state.selectedOccurrence->entries().capacity();
    if (capacity > std::numeric_limits<std::size_t>::max() /
                       sizeof(OccurrenceId))
      return std::nullopt;
    return capacity * sizeof(OccurrenceId);
  };
  const auto before = stateBytes(beforeState(command));
  const auto after = stateBytes(afterState(command));
  if (!before || !after ||
      *base > std::numeric_limits<std::size_t>::max() - *before ||
      *base + *before >
          std::numeric_limits<std::size_t>::max() - *after)
    return std::nullopt;
  return *base + *before + *after;
}

const AssemblyCommandSemanticState& AssemblyCommandHistory::beforeState(
    const AssemblyCommand& command) noexcept {
  return std::visit(
      [](const auto& typed) -> const AssemblyCommandSemanticState& {
        return typed.beforeState;
      },
      command);
}

const AssemblyCommandSemanticState& AssemblyCommandHistory::afterState(
    const AssemblyCommand& command) noexcept {
  return std::visit(
      [](const auto& typed) -> const AssemblyCommandSemanticState& {
        return typed.afterState;
      },
      command);
}

AssemblyCommandHistoryStatus
AssemblyCommandHistory::executeSyntheticTwoStepForTesting(
    OccurrenceDeltaCommand command, int& first, int& second,
    std::array<int, 4>& trace, std::size_t& traceSize,
    bool contradictory) noexcept {
  if (&first == &second || contradictory || command.delta.isEmpty())
    return AssemblyCommandHistoryStatus::InvalidCommand;
  traceSize = 0;
  const auto prepared = prepareCommand(AssemblyCommand(std::move(command)));
  if (prepared != AssemblyCommandHistoryStatus::Committed) return prepared;

  bool firstApplied = false;
  bool secondApplied = false;
  auto rollback = [&]() noexcept {
    if (secondApplied) {
      --second;
      trace[traceSize++] = -2;
    }
    if (firstApplied) {
      --first;
      trace[traceSize++] = -1;
    }
  };

  try {
    ++first;
    firstApplied = true;
    trace[traceSize++] = 1;
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::AfterFirstSyntheticOperation));
    ++second;
    secondApplied = true;
    trace[traceSize++] = 2;
    throwInjectedFailure(static_cast<std::uint8_t>(
        detail::AssemblyCommandHistoryFailurePoint::Finalization));
  } catch (const std::bad_alloc&) {
    rollback();
    discardPrepared();
    return AssemblyCommandHistoryStatus::ResourceLimit;
  } catch (...) {
    rollback();
    discardPrepared();
    return AssemblyCommandHistoryStatus::ModelRejected;
  }
  return commitPrepared();
}

void AssemblyCommandHistory::throwInjectedFailure(std::uint8_t point) {
  if (testFailurePoint_ != point) return;
  switch (static_cast<detail::AssemblyCommandHistoryFailureKind>(
      testFailureKind_)) {
    case detail::AssemblyCommandHistoryFailureKind::BadAlloc:
      throw std::bad_alloc();
    case detail::AssemblyCommandHistoryFailureKind::Runtime:
      throw std::runtime_error("assembly command history injected failure");
    case detail::AssemblyCommandHistoryFailureKind::Unknown:
      throw 17;
  }
}

}  // namespace solidar
