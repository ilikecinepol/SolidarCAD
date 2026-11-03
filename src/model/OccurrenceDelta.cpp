#include "model/OccurrenceDelta.h"

#include <new>

namespace solidar {
namespace {

class TransactionGuard final {
 public:
  explicit TransactionGuard(bool& active) noexcept : active_(active) {
    active_ = true;
  }
  ~TransactionGuard() { active_ = false; }

  TransactionGuard(const TransactionGuard&) = delete;
  TransactionGuard& operator=(const TransactionGuard&) = delete;

 private:
  bool& active_;
};

class AcceptSuccessor final : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    return ProductDocumentSuccessorStatus::Prepared;
  }
};

OccurrenceDeltaStatus validateAddress(
    const ProductDocument& product, const OccurrenceDelta& delta,
    const ComponentOccurrence*& occurrence) noexcept {
  const RootOccurrenceAddress& address = delta.address();
  if (!address.productIncarnation.isValid() ||
      address.productIncarnation != product.incarnation())
    return OccurrenceDeltaStatus::ProductIncarnationMismatch;
  if (!address.rootDefinitionId.isValid() ||
      address.rootDefinitionId != product.rootDefinitionId())
    return OccurrenceDeltaStatus::RootDefinitionMismatch;
  if (product.rootAssemblyDefinition() == nullptr)
    return OccurrenceDeltaStatus::RootIsNotAssembly;
  if (address.path.depth() != 1)
    return OccurrenceDeltaStatus::UnsupportedPathDepth;
  const auto resolved = product.resolveRootOccurrence(address.path);
  if (!resolved)
    return OccurrenceDeltaStatus::OccurrenceMissing;
  if (!address.expectedTargetDefinitionId.isValid() ||
      resolved.occurrence->targetDefinitionId() !=
          address.expectedTargetDefinitionId)
    return OccurrenceDeltaStatus::TargetDefinitionMismatch;
  occurrence = resolved.occurrence;
  return OccurrenceDeltaStatus::NoChange;
}

}  // namespace

OccurrenceDeltaStatus ProductDocument::mutateOccurrenceDeltaInternal(
    const OccurrenceDelta& delta, bool apply,
    ProductDocumentSuccessor& successor) noexcept {
  const ComponentOccurrence* occurrence = nullptr;
  const auto addressStatus = validateAddress(*this, delta, occurrence);
  if (addressStatus != OccurrenceDeltaStatus::NoChange) return addressStatus;
  if (const VisibilityOccurrenceDelta* visibility =
          delta.visibilityChange()) {
    const bool expected = apply ? visibility->beforeVisible
                                : visibility->afterVisible;
    const bool requested = apply ? visibility->afterVisible
                                 : visibility->beforeVisible;
    if (occurrence->flags().visible != expected)
      return OccurrenceDeltaStatus::CurrentValueMismatch;
    if (visibility->beforeVisible == visibility->afterVisible)
      return OccurrenceDeltaStatus::NoChange;

    auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
    const OccurrenceId occurrenceId =
        delta.address().path.entries().front();
    if (!assembly || !assembly->setVisible(occurrenceId, requested))
      return OccurrenceDeltaStatus::OccurrenceMissing;
    const auto prepared = successor.prepare(
        *this, ProductDocumentSuccessorImpact::VisibilityProjection);
    if (prepared != ProductDocumentSuccessorStatus::Prepared) {
      static_cast<void>(assembly->setVisible(occurrenceId, expected));
      return prepared == ProductDocumentSuccessorStatus::ResourceFailure
                 ? OccurrenceDeltaStatus::ResourceFailure
                 : OccurrenceDeltaStatus::SuccessorRejected;
    }
    return OccurrenceDeltaStatus::Committed;
  }
  if (const GroundedOccurrenceDelta* grounded = delta.groundedChange()) {
    const bool expected = apply ? grounded->beforeGrounded
                                : grounded->afterGrounded;
    const bool requested = apply ? grounded->afterGrounded
                                 : grounded->beforeGrounded;
    if (occurrence->flags().grounded != expected)
      return OccurrenceDeltaStatus::CurrentValueMismatch;
    if (grounded->beforeGrounded == grounded->afterGrounded)
      return OccurrenceDeltaStatus::NoChange;

    auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
    const OccurrenceId occurrenceId =
        delta.address().path.entries().front();
    if (!assembly || !assembly->setGrounded(occurrenceId, requested))
      return OccurrenceDeltaStatus::OccurrenceMissing;
    const auto prepared = successor.prepare(
        *this, ProductDocumentSuccessorImpact::GroundingProjection);
    if (prepared != ProductDocumentSuccessorStatus::Prepared) {
      static_cast<void>(assembly->setGrounded(occurrenceId, expected));
      return prepared == ProductDocumentSuccessorStatus::ResourceFailure
                 ? OccurrenceDeltaStatus::ResourceFailure
                 : OccurrenceDeltaStatus::SuccessorRejected;
    }
    return OccurrenceDeltaStatus::Committed;
  }
  if (const SuppressedOccurrenceDelta* suppressed =
          delta.suppressedChange()) {
    const bool expected = apply ? suppressed->beforeSuppressed
                                : suppressed->afterSuppressed;
    const bool requested = apply ? suppressed->afterSuppressed
                                 : suppressed->beforeSuppressed;
    if (occurrence->flags().suppressed != expected)
      return OccurrenceDeltaStatus::CurrentValueMismatch;
    if (suppressed->beforeSuppressed == suppressed->afterSuppressed)
      return OccurrenceDeltaStatus::NoChange;

    auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
    const OccurrenceId occurrenceId =
        delta.address().path.entries().front();
    if (!assembly || !assembly->setSuppressed(occurrenceId, requested))
      return OccurrenceDeltaStatus::OccurrenceMissing;
    const auto prepared = successor.prepare(
        *this, ProductDocumentSuccessorImpact::SuppressionProjection);
    if (prepared != ProductDocumentSuccessorStatus::Prepared) {
      static_cast<void>(assembly->setSuppressed(occurrenceId, expected));
      return prepared == ProductDocumentSuccessorStatus::ResourceFailure
                 ? OccurrenceDeltaStatus::ResourceFailure
                 : OccurrenceDeltaStatus::SuccessorRejected;
    }
    return OccurrenceDeltaStatus::Committed;
  }
  const RenameOccurrenceDelta* rename = delta.rename();
  if (!rename) return OccurrenceDeltaStatus::NoChange;
  if (validateOccurrenceDisplayNameUtf8(rename->beforeUtf8, true) !=
          OccurrenceDisplayNameStatus::Valid ||
      validateOccurrenceDisplayNameUtf8(rename->afterUtf8) !=
          OccurrenceDisplayNameStatus::Valid)
    return OccurrenceDeltaStatus::InvalidName;
  const std::string& expected = apply ? rename->beforeUtf8 : rename->afterUtf8;
  const std::string& requested = apply ? rename->afterUtf8 : rename->beforeUtf8;
  if (occurrence->displayNameUtf8() != expected)
    return OccurrenceDeltaStatus::CurrentValueMismatch;
  if (rename->beforeUtf8 == rename->afterUtf8)
    return OccurrenceDeltaStatus::NoChange;

  try {
    std::string replacement = requested;
    auto* assembly = definitions_.findAssemblyMutable(rootDefinitionId_);
    if (!assembly || !assembly->swapDisplayName(
                         delta.address().path.entries().front(), replacement))
      return OccurrenceDeltaStatus::OccurrenceMissing;
    const auto prepared = successor.prepare(
        *this, ProductDocumentSuccessorImpact::TreeOnly);
    if (prepared != ProductDocumentSuccessorStatus::Prepared) {
      static_cast<void>(assembly->swapDisplayName(
          delta.address().path.entries().front(), replacement));
      return prepared == ProductDocumentSuccessorStatus::ResourceFailure
                 ? OccurrenceDeltaStatus::ResourceFailure
                 : OccurrenceDeltaStatus::SuccessorRejected;
    }
    return OccurrenceDeltaStatus::Committed;
  } catch (const std::bad_alloc&) {
    return OccurrenceDeltaStatus::ResourceFailure;
  } catch (...) {
    return OccurrenceDeltaStatus::UnexpectedFailure;
  }
}

OccurrenceDeltaCreateResult OccurrenceDelta::rename(
    RootOccurrenceAddress address, std::string beforeUtf8,
    std::string afterUtf8) noexcept {
  if (validateOccurrenceDisplayNameUtf8(beforeUtf8, true) !=
      OccurrenceDisplayNameStatus::Valid)
    return {OccurrenceDeltaCreateStatus::InvalidBeforeName, std::nullopt};
  if (validateOccurrenceDisplayNameUtf8(afterUtf8) !=
      OccurrenceDisplayNameStatus::Valid)
    return {OccurrenceDeltaCreateStatus::InvalidAfterName, std::nullopt};
  try {
    OccurrenceDelta result(
        std::move(address),
        RenameOccurrenceDelta{std::move(beforeUtf8), std::move(afterUtf8)});
    return {OccurrenceDeltaCreateStatus::Created, std::move(result)};
  } catch (...) {
    return {OccurrenceDeltaCreateStatus::ResourceFailure, std::nullopt};
  }
}

OccurrenceDeltaStatus ProductDocument::applyOccurrenceDelta(
    const OccurrenceDelta& delta) noexcept {
  AcceptSuccessor successor;
  return applyOccurrenceDelta(delta, successor);
}

OccurrenceDeltaStatus ProductDocument::applyOccurrenceDelta(
    const OccurrenceDelta& delta,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return OccurrenceDeltaStatus::Busy;
  TransactionGuard transaction(productMutationTransactionActive_);
  try {
    return mutateOccurrenceDeltaInternal(delta, true, prepareSuccessor);
  } catch (...) {
    return OccurrenceDeltaStatus::UnexpectedFailure;
  }
}

OccurrenceDeltaStatus ProductDocument::revertOccurrenceDelta(
    const OccurrenceDelta& delta) noexcept {
  AcceptSuccessor successor;
  return revertOccurrenceDelta(delta, successor);
}

OccurrenceDeltaStatus ProductDocument::revertOccurrenceDelta(
    const OccurrenceDelta& delta,
    ProductDocumentSuccessor& prepareSuccessor) noexcept {
  if (productMutationTransactionActive_)
    return OccurrenceDeltaStatus::Busy;
  TransactionGuard transaction(productMutationTransactionActive_);
  try {
    return mutateOccurrenceDeltaInternal(delta, false, prepareSuccessor);
  } catch (...) {
    return OccurrenceDeltaStatus::UnexpectedFailure;
  }
}

}  // namespace solidar
