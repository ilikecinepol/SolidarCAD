#include "model/OccurrenceRemove.h"

#include <new>
#include <utility>

#include "model/ProductDocument.h"

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

bool validPayload(const RemoveOccurrenceColdPayload& payload) noexcept {
  return validateOccurrenceDisplayNameUtf8(payload.displayNameUtf8, true) ==
             OccurrenceDisplayNameStatus::Valid;
}

RemoveOccurrenceStatus validateProduct(
    const ProductDocument& product,
    const RemoveOccurrenceColdPayload& payload) noexcept {
  if (!payload.occurrence.productIncarnation.isValid() ||
      payload.occurrence.productIncarnation != product.incarnation())
    return RemoveOccurrenceStatus::ProductIncarnationMismatch;
  if (!payload.occurrence.rootDefinitionId.isValid() ||
      payload.occurrence.rootDefinitionId != product.rootDefinitionId())
    return RemoveOccurrenceStatus::RootDefinitionMismatch;
  if (!product.rootAssemblyDefinition())
    return RemoveOccurrenceStatus::RootIsNotAssembly;
  if (payload.occurrence.path.depth() != 1)
    return RemoveOccurrenceStatus::UnsupportedPathDepth;
  if (!payload.occurrence.expectedTargetDefinitionId.isValid())
    return RemoveOccurrenceStatus::TargetDefinitionMismatch;
  if (!product.definitions().findPart(
          payload.occurrence.expectedTargetDefinitionId))
    return RemoveOccurrenceStatus::TargetPartMissing;
  return RemoveOccurrenceStatus::Removed;
}

bool matches(const ComponentOccurrence& occurrence,
             const RemoveOccurrenceColdPayload& payload) noexcept {
  return occurrence.targetDefinitionId() ==
             payload.occurrence.expectedTargetDefinitionId &&
         occurrence.placement() == payload.placement &&
         occurrence.homePlacement() == payload.homePlacement &&
         occurrence.flags() == payload.flags &&
         occurrence.displayNameUtf8() == payload.displayNameUtf8;
}

RemoveOccurrenceStatus successorFailure(
    ProductDocumentSuccessorStatus status) noexcept {
  return status == ProductDocumentSuccessorStatus::ResourceFailure
             ? RemoveOccurrenceStatus::SuccessorResourceFailure
             : RemoveOccurrenceStatus::SuccessorRejected;
}

}  // namespace

RemoveOccurrenceStatus ProductDocument::applyRemoveOccurrence(
    const RemoveOccurrenceColdPayload& payload,
    ProductDocumentSuccessor& prepareSuccessor,
    RemoveOccurrenceFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return RemoveOccurrenceStatus::Busy;
  MutationGuard transaction(productMutationTransactionActive_);
  if (!validPayload(payload)) return RemoveOccurrenceStatus::InvalidPayload;
  const auto productStatus = validateProduct(*this, payload);
  if (productStatus != RemoveOccurrenceStatus::Removed) return productStatus;

  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly) return RemoveOccurrenceStatus::RootIsNotAssembly;
  const OccurrenceId id = payload.occurrence.path.entries().front();
  const auto* occurrence = assembly->find(id);
  if (!occurrence) return RemoveOccurrenceStatus::OccurrenceMissing;
  if (occurrence->targetDefinitionId() !=
      payload.occurrence.expectedTargetDefinitionId)
    return RemoveOccurrenceStatus::TargetDefinitionMismatch;
  if (!matches(*occurrence, payload))
    return RemoveOccurrenceStatus::SnapshotMismatch;

  auto node = assembly->occurrences_.extract(id);
  if (node.empty()) return RemoveOccurrenceStatus::NodeFailure;
  if (failurePoint == RemoveOccurrenceFailurePoint::NodeExtract) {
    assembly->occurrences_.insert(std::move(node));
    return RemoveOccurrenceStatus::NodeFailure;
  }
  const auto successor = prepareSuccessor.prepare(
      *this, ProductDocumentSuccessorImpact::RemoveProjection);
  if (successor != ProductDocumentSuccessorStatus::Prepared) {
    assembly->occurrences_.insert(std::move(node));
    return successorFailure(successor);
  }
  return RemoveOccurrenceStatus::Removed;
}

RemoveOccurrenceStatus ProductDocument::restoreRemovedOccurrence(
    const RemoveOccurrenceColdPayload& payload,
    ProductDocumentSuccessor& prepareSuccessor,
    RemoveOccurrenceFailurePoint failurePoint) noexcept {
  if (productMutationTransactionActive_)
    return RemoveOccurrenceStatus::Busy;
  MutationGuard transaction(productMutationTransactionActive_);
  if (!validPayload(payload)) return RemoveOccurrenceStatus::InvalidPayload;
  const auto productStatus = validateProduct(*this, payload);
  if (productStatus != RemoveOccurrenceStatus::Removed) return productStatus;

  auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
  if (!assembly) return RemoveOccurrenceStatus::RootIsNotAssembly;
  const OccurrenceId id = payload.occurrence.path.entries().front();
  if (assembly->find(id))
    return RemoveOccurrenceStatus::OccurrenceIdCollision;
  if (assembly->size() >= kMaximumRootOccurrences)
    return RemoveOccurrenceStatus::OccurrenceLimit;

  AssemblyDefinition staged(rootDefinitionId_);
  try {
    if (failurePoint == RemoveOccurrenceFailurePoint::OccurrenceConstruction)
      throw std::bad_alloc();
    ComponentOccurrence occurrence(
        id, payload.occurrence.expectedTargetDefinitionId,
        payload.placement, payload.homePlacement, payload.flags,
        payload.displayNameUtf8);
    if (failurePoint == RemoveOccurrenceFailurePoint::NodeAllocation)
      throw std::bad_alloc();
    if (staged.insert(std::move(occurrence)) !=
        AssemblyOccurrenceInsertStatus::Inserted)
      return RemoveOccurrenceStatus::NodeFailure;
  } catch (...) {
    return RemoveOccurrenceStatus::AllocationFailure;
  }

  auto node = staged.occurrences_.extract(id);
  auto inserted = assembly->occurrences_.insert(std::move(node));
  if (!inserted.inserted)
    return RemoveOccurrenceStatus::OccurrenceIdCollision;
  if (failurePoint == RemoveOccurrenceFailurePoint::NodeCommit) {
    staged.occurrences_.insert(assembly->occurrences_.extract(id));
    return RemoveOccurrenceStatus::NodeFailure;
  }
  const auto successor = prepareSuccessor.prepare(
      *this, ProductDocumentSuccessorImpact::RemoveProjection);
  if (successor != ProductDocumentSuccessorStatus::Prepared) {
    staged.occurrences_.insert(assembly->occurrences_.extract(id));
    return successorFailure(successor);
  }
  return RemoveOccurrenceStatus::Restored;
}

}  // namespace solidar
