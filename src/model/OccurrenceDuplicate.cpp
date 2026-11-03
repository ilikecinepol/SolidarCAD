#include "model/OccurrenceDuplicate.h"

#include <new>
#include <utility>

namespace solidar {
namespace {

class MutationGuard final {
 public:
  explicit MutationGuard(bool& active) noexcept : active_(active) {
    active_ = true;
  }
  ~MutationGuard() { active_ = false; }

  MutationGuard(const MutationGuard&) = delete;
  MutationGuard& operator=(const MutationGuard&) = delete;

 private:
  bool& active_;
};

DuplicateOccurrenceStatus validateSource(
    const ProductDocument& product,
    const DuplicateOccurrenceColdPayload& payload,
    const ComponentOccurrence*& source) noexcept {
  if (!payload.source.productIncarnation.isValid() ||
      payload.source.productIncarnation != product.incarnation())
    return DuplicateOccurrenceStatus::ProductIncarnationMismatch;
  if (!payload.source.rootDefinitionId.isValid() ||
      payload.source.rootDefinitionId != product.rootDefinitionId())
    return DuplicateOccurrenceStatus::RootDefinitionMismatch;
  if (!product.rootAssemblyDefinition())
    return DuplicateOccurrenceStatus::RootIsNotAssembly;
  if (payload.source.path.depth() != 1)
    return DuplicateOccurrenceStatus::UnsupportedPathDepth;
  const auto resolved = product.resolveRootOccurrence(payload.source.path);
  if (!resolved)
    return DuplicateOccurrenceStatus::SourceMissing;
  if (!payload.source.expectedTargetDefinitionId.isValid() ||
      !payload.targetDefinitionId.isValid() ||
      payload.source.expectedTargetDefinitionId != payload.targetDefinitionId ||
      resolved.occurrence->targetDefinitionId() != payload.targetDefinitionId)
    return DuplicateOccurrenceStatus::TargetDefinitionMismatch;
  if (resolved.occurrence->placement() != payload.placement ||
      resolved.occurrence->homePlacement() != payload.homePlacement ||
      resolved.occurrence->flags() != payload.flags ||
      resolved.occurrence->displayNameUtf8() != payload.displayNameUtf8)
    return DuplicateOccurrenceStatus::SourceSnapshotMismatch;
  source = resolved.occurrence;
  return DuplicateOccurrenceStatus::Committed;
}

DuplicateOccurrenceStatus successorFailure(
    ProductDocumentSuccessorStatus status) noexcept {
  return status == ProductDocumentSuccessorStatus::ResourceFailure
             ? DuplicateOccurrenceStatus::SuccessorResourceFailure
             : DuplicateOccurrenceStatus::SuccessorRejected;
}

bool validColdPayloadValues(
    const DuplicateOccurrenceColdPayload& payload) noexcept {
  if (!payload.duplicateId.isValid() ||
      payload.duplicateId.value() > kMaximumRootOccurrences ||
      !payload.targetDefinitionId.isValid() ||
      validateOccurrenceDisplayNameUtf8(payload.displayNameUtf8, true) !=
          OccurrenceDisplayNameStatus::Valid)
    return false;
  return payload.source.path.depth() != 1 ||
         payload.source.path.entries().front() != payload.duplicateId;
}

}  // namespace

DuplicateOccurrenceStatus ProductDocument::applyDuplicateOccurrence(
    const DuplicateOccurrenceColdPayload& payload,
    ProductDocumentSuccessor& prepareSuccessor,
    DuplicateOccurrenceFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return DuplicateOccurrenceStatus::Busy;
  MutationGuard transaction(productMutationTransactionActive_);

  if (!validColdPayloadValues(payload))
    return DuplicateOccurrenceStatus::InvalidPayload;

  const ComponentOccurrence* source = nullptr;
  const auto sourceStatus = validateSource(*this, payload, source);
  if (sourceStatus != DuplicateOccurrenceStatus::Committed)
    return sourceStatus;
  static_cast<void>(source);
  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly) return DuplicateOccurrenceStatus::RootIsNotAssembly;
  if (assembly->size() >= kMaximumRootOccurrences)
    return DuplicateOccurrenceStatus::OccurrenceLimit;
  if (assembly->find(payload.duplicateId))
    return DuplicateOccurrenceStatus::DuplicateIdCollision;
  if (!definitions_.findPart(payload.targetDefinitionId))
    return DuplicateOccurrenceStatus::TargetPartMissing;

  AssemblyDefinition staged(rootDefinitionId_);
  try {
    if (failurePoint ==
        DuplicateOccurrenceFailurePoint::OccurrenceConstruction)
      throw std::bad_alloc();
    ComponentOccurrence duplicate(
        payload.duplicateId, payload.targetDefinitionId, payload.placement,
        payload.homePlacement, payload.flags, payload.displayNameUtf8);
    if (failurePoint == DuplicateOccurrenceFailurePoint::NodeAllocation)
      throw std::bad_alloc();
    if (staged.insert(std::move(duplicate)) !=
        AssemblyOccurrenceInsertStatus::Inserted)
      return DuplicateOccurrenceStatus::NodeFailure;
  } catch (...) {
    return DuplicateOccurrenceStatus::AllocationFailure;
  }

  auto node = staged.occurrences_.extract(payload.duplicateId);
  auto inserted = assembly->occurrences_.insert(std::move(node));
  if (!inserted.inserted)
    return DuplicateOccurrenceStatus::DuplicateIdCollision;
  if (failurePoint == DuplicateOccurrenceFailurePoint::NodeCommit) {
    staged.occurrences_.insert(
        assembly->occurrences_.extract(payload.duplicateId));
    return DuplicateOccurrenceStatus::NodeFailure;
  }

  const auto successor = prepareSuccessor.prepare(
      *this, ProductDocumentSuccessorImpact::DuplicateProjection);
  if (successor != ProductDocumentSuccessorStatus::Prepared) {
    staged.occurrences_.insert(
        assembly->occurrences_.extract(payload.duplicateId));
    return successorFailure(successor);
  }
  return DuplicateOccurrenceStatus::Committed;
}

DuplicateOccurrenceStatus ProductDocument::extractDuplicateOccurrence(
    const DuplicateOccurrenceColdPayload& payload,
    ProductDocumentSuccessor& prepareSuccessor,
    DuplicateOccurrenceFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return DuplicateOccurrenceStatus::Busy;
  MutationGuard transaction(productMutationTransactionActive_);

  if (!validColdPayloadValues(payload))
    return DuplicateOccurrenceStatus::InvalidPayload;

  const ComponentOccurrence* source = nullptr;
  const auto sourceStatus = validateSource(*this, payload, source);
  if (sourceStatus != DuplicateOccurrenceStatus::Committed)
    return sourceStatus;
  static_cast<void>(source);
  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly) return DuplicateOccurrenceStatus::RootIsNotAssembly;
  const auto* duplicate = assembly->find(payload.duplicateId);
  if (!duplicate) return DuplicateOccurrenceStatus::DuplicateMissing;
  if (duplicate->targetDefinitionId() != payload.targetDefinitionId ||
      duplicate->placement() != payload.placement ||
      duplicate->homePlacement() != payload.homePlacement ||
      duplicate->flags() != payload.flags ||
      duplicate->displayNameUtf8() != payload.displayNameUtf8)
    return DuplicateOccurrenceStatus::DuplicateGuardMismatch;

  auto node = assembly->occurrences_.extract(payload.duplicateId);
  if (node.empty()) return DuplicateOccurrenceStatus::NodeFailure;
  if (failurePoint == DuplicateOccurrenceFailurePoint::NodeExtract) {
    assembly->occurrences_.insert(std::move(node));
    return DuplicateOccurrenceStatus::NodeFailure;
  }
  const auto successor = prepareSuccessor.prepare(
      *this, ProductDocumentSuccessorImpact::DuplicateProjection);
  if (successor != ProductDocumentSuccessorStatus::Prepared) {
    assembly->occurrences_.insert(std::move(node));
    return successorFailure(successor);
  }
  return DuplicateOccurrenceStatus::Extracted;
}

}  // namespace solidar
