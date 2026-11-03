#include "ui/application/LinkedPartLifecycleService.h"

#include <QByteArray>

#include <new>
#include <utility>

#include "model/Document.h"
#include "ui/application/DocumentDelta.h"

namespace solidar {
namespace {

LinkedPartLifecycleResult resolverFailure(
    project::LinkedPartSourceResolveStatus status) noexcept {
  return {LinkedPartLifecycleStatus::ResolverRejected, status, std::nullopt,
          false};
}

LinkedPartLifecycleResult historyResult(
    AssemblyCommandHistoryResult result, bool outdated = false) noexcept {
  return {result.status == AssemblyCommandHistoryStatus::Committed
              ? LinkedPartLifecycleStatus::Committed
              : LinkedPartLifecycleStatus::HistoryRejected,
          project::LinkedPartSourceResolveStatus::Resolved, result.status,
          outdated};
}

project::LinkedPartSourceDescriptor descriptorFor(
    const LinkedPartBinding& binding, QString locator) {
  return {binding.key, std::move(locator), binding.acceptedHash};
}

DefinitionEditAddress addressFor(const ProductDocument& product,
                                 DefinitionId definitionId) noexcept {
  return {product.incarnation(), product.rootDefinitionId(), definitionId,
          product.definitionStateId(definitionId)};
}

}  // namespace

LinkedPartLifecycleResult LinkedPartLifecycleService::reload(
    ProductDocument& product, DefinitionId definitionId,
    const project::LinkedPartSourcePolicy& policy,
    project::detail::LinkedPartSourceFileSystem& fileSystem,
    AssemblyCommandHistory& history,
    ProductDocumentSuccessor& prepareSuccessor,
    AssemblyCommandSemanticState beforeState,
    AssemblyCommandSemanticState afterState) noexcept {
  try {
    const LinkedPartBinding* binding = product.partLinkedBinding(definitionId);
    const Document* current = product.partDocument(definitionId);
    if (!binding || !current)
      return {LinkedPartLifecycleStatus::InvalidDefinition};
    const QByteArray locator =
        QByteArray::fromStdString(binding->relativePathHintUtf8);
    auto resolved = project::resolveLinkedPartSourcesForReload(
        {descriptorFor(*binding, QString::fromUtf8(locator))}, policy,
        fileSystem);
    if (!resolved.succeeded()) return resolverFailure(resolved.status);
    if (resolved.sources.size() != 1 || !resolved.sources.front().product)
      return {LinkedPartLifecycleStatus::MalformedResolvedSource,
              resolved.status};
    auto candidate =
        std::move(*resolved.sources.front().product).releaseRootDocument();
    if (!candidate)
      return {LinkedPartLifecycleStatus::MalformedResolvedSource,
              resolved.status};
    LinkedPartBinding after = *binding;
    after.acceptedHash = resolved.sources.front().contentHash;
    LinkedPartReloadColdCommand command{
        addressFor(product, definitionId), *binding, std::move(after),
        DocumentDelta::create(*current, *candidate), std::move(beforeState),
        std::move(afterState)};
    return historyResult(history.executeLinkedPartReload(
        product, std::move(command), prepareSuccessor));
  } catch (const std::bad_alloc&) {
    return {LinkedPartLifecycleStatus::HistoryRejected, std::nullopt,
            AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    return {LinkedPartLifecycleStatus::MalformedResolvedSource};
  }
}

LinkedPartLifecycleResult LinkedPartLifecycleService::reload(
    ProductDocument& product, DefinitionId definitionId,
    project::LinkedPartSourcePolicy policy,
    AssemblyCommandHistory& history,
    ProductDocumentSuccessor& prepareSuccessor,
    AssemblyCommandSemanticState beforeState,
    AssemblyCommandSemanticState afterState) noexcept {
  try {
    const LinkedPartBinding* binding = product.partLinkedBinding(definitionId);
    const Document* current = product.partDocument(definitionId);
    if (!binding || !current)
      return {LinkedPartLifecycleStatus::InvalidDefinition};
    const QByteArray locator =
        QByteArray::fromStdString(binding->relativePathHintUtf8);
    auto resolved = project::resolveLinkedPartSourcesForReloadNative(
        {descriptorFor(*binding, QString::fromUtf8(locator))},
        std::move(policy));
    if (!resolved.succeeded()) return resolverFailure(resolved.status);
    if (resolved.sources.size() != 1 || !resolved.sources.front().product)
      return {LinkedPartLifecycleStatus::MalformedResolvedSource,
              resolved.status};
    auto candidate =
        std::move(*resolved.sources.front().product).releaseRootDocument();
    if (!candidate)
      return {LinkedPartLifecycleStatus::MalformedResolvedSource,
              resolved.status};
    LinkedPartBinding after = *binding;
    after.acceptedHash = resolved.sources.front().contentHash;
    LinkedPartReloadColdCommand command{
        addressFor(product, definitionId), *binding, std::move(after),
        DocumentDelta::create(*current, *candidate), std::move(beforeState),
        std::move(afterState)};
    return historyResult(history.executeLinkedPartReload(
        product, std::move(command), prepareSuccessor));
  } catch (const std::bad_alloc&) {
    return {LinkedPartLifecycleStatus::HistoryRejected, std::nullopt,
            AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    return {LinkedPartLifecycleStatus::MalformedResolvedSource};
  }
}

LinkedPartLifecycleResult LinkedPartLifecycleService::repair(
    ProductDocument& product, DefinitionId definitionId,
    QString replacementRelativePathHint,
    const project::LinkedPartSourcePolicy& policy,
    project::detail::LinkedPartSourceFileSystem& fileSystem,
    AssemblyCommandHistory& history,
    ProductDocumentSuccessor& prepareSuccessor,
    AssemblyCommandSemanticState beforeState,
    AssemblyCommandSemanticState afterState,
    bool* resolvedOutdatedBeforeCommit) noexcept {
  try {
    const LinkedPartBinding* binding = product.partLinkedBinding(definitionId);
    if (!binding) return {LinkedPartLifecycleStatus::InvalidDefinition};
    auto resolved = project::probeLinkedPartSourcesForRepair(
        {descriptorFor(*binding, replacementRelativePathHint)}, policy,
        fileSystem);
    if (!resolved.succeeded()) return resolverFailure(resolved.status);
    if (resolved.sources.size() != 1 || resolved.sources.front().product)
      return {LinkedPartLifecycleStatus::MalformedResolvedSource,
              resolved.status};
    const bool outdated =
        resolved.sources.front().contentHash != binding->acceptedHash;
    if (resolvedOutdatedBeforeCommit)
      *resolvedOutdatedBeforeCommit = outdated;
    LinkedPartBinding after = *binding;
    const QByteArray utf8 = replacementRelativePathHint.toUtf8();
    after.relativePathHintUtf8.assign(utf8.constData(),
                                      static_cast<std::size_t>(utf8.size()));
    LinkedPartRepairColdCommand command{
        addressFor(product, definitionId), *binding, std::move(after),
        std::move(beforeState), std::move(afterState)};
    return historyResult(history.executeLinkedPartRepair(
                             product, std::move(command), prepareSuccessor),
                         outdated);
  } catch (const std::bad_alloc&) {
    return {LinkedPartLifecycleStatus::HistoryRejected, std::nullopt,
            AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    return {LinkedPartLifecycleStatus::MalformedResolvedSource};
  }
}

LinkedPartLifecycleResult LinkedPartLifecycleService::repair(
    ProductDocument& product, DefinitionId definitionId,
    QString replacementRelativePathHint,
    project::LinkedPartSourcePolicy policy,
    AssemblyCommandHistory& history,
    ProductDocumentSuccessor& prepareSuccessor,
    AssemblyCommandSemanticState beforeState,
    AssemblyCommandSemanticState afterState,
    bool* resolvedOutdatedBeforeCommit) noexcept {
  try {
    const LinkedPartBinding* binding = product.partLinkedBinding(definitionId);
    if (!binding) return {LinkedPartLifecycleStatus::InvalidDefinition};
    auto resolved = project::probeLinkedPartSourcesForRepairNative(
        {descriptorFor(*binding, replacementRelativePathHint)},
        std::move(policy));
    if (!resolved.succeeded()) return resolverFailure(resolved.status);
    if (resolved.sources.size() != 1 || resolved.sources.front().product)
      return {LinkedPartLifecycleStatus::MalformedResolvedSource,
              resolved.status};
    const bool outdated =
        resolved.sources.front().contentHash != binding->acceptedHash;
    if (resolvedOutdatedBeforeCommit)
      *resolvedOutdatedBeforeCommit = outdated;
    LinkedPartBinding after = *binding;
    const QByteArray utf8 = replacementRelativePathHint.toUtf8();
    after.relativePathHintUtf8.assign(utf8.constData(),
                                      static_cast<std::size_t>(utf8.size()));
    LinkedPartRepairColdCommand command{
        addressFor(product, definitionId), *binding, std::move(after),
        std::move(beforeState), std::move(afterState)};
    return historyResult(history.executeLinkedPartRepair(
                             product, std::move(command), prepareSuccessor),
                         outdated);
  } catch (const std::bad_alloc&) {
    return {LinkedPartLifecycleStatus::HistoryRejected, std::nullopt,
            AssemblyCommandHistoryStatus::ResourceLimit};
  } catch (...) {
    return {LinkedPartLifecycleStatus::MalformedResolvedSource};
  }
}

}  // namespace solidar
