#pragma once

#include <optional>

#include <QString>

#include "project/LinkedPartSourceResolver.h"
#include "ui/application/AssemblyCommandHistory.h"

namespace solidar {

enum class LinkedPartLifecycleStatus {
  Committed,
  InvalidDefinition,
  ResolverRejected,
  MalformedResolvedSource,
  HistoryRejected,
};

struct LinkedPartLifecycleResult final {
  LinkedPartLifecycleStatus status{LinkedPartLifecycleStatus::InvalidDefinition};
  std::optional<project::LinkedPartSourceResolveStatus> resolverStatus;
  std::optional<AssemblyCommandHistoryStatus> historyStatus;
  // Repair may relocate to the same identity at a newer revision. The new
  // locator is accepted, while the authored acceptedHash/cache stay cold
  // until an explicit Reload.
  bool outdated{};
};

// Narrow A6b1 application boundary. It owns no filesystem capability and
// performs no publication or persistence; callers supply the exact-read-only
// resolver seam and the unified cold history.
class LinkedPartLifecycleService final {
 public:
  [[nodiscard]] static LinkedPartLifecycleResult reload(
      ProductDocument& product, DefinitionId definitionId,
      const project::LinkedPartSourcePolicy& policy,
      project::detail::LinkedPartSourceFileSystem& fileSystem,
      AssemblyCommandHistory& history,
      ProductDocumentSuccessor& prepareSuccessor,
      AssemblyCommandSemanticState beforeState = {},
      AssemblyCommandSemanticState afterState = {}) noexcept;
  // Native production overload. UI callers never receive the internal
  // filesystem capability.
  [[nodiscard]] static LinkedPartLifecycleResult reload(
      ProductDocument& product, DefinitionId definitionId,
      project::LinkedPartSourcePolicy policy,
      AssemblyCommandHistory& history,
      ProductDocumentSuccessor& prepareSuccessor,
      AssemblyCommandSemanticState beforeState = {},
      AssemblyCommandSemanticState afterState = {}) noexcept;

  [[nodiscard]] static LinkedPartLifecycleResult repair(
      ProductDocument& product, DefinitionId definitionId,
      QString replacementRelativePathHint,
      const project::LinkedPartSourcePolicy& policy,
      project::detail::LinkedPartSourceFileSystem& fileSystem,
      AssemblyCommandHistory& history,
      ProductDocumentSuccessor& prepareSuccessor,
      AssemblyCommandSemanticState beforeState = {},
      AssemblyCommandSemanticState afterState = {},
      bool* resolvedOutdatedBeforeCommit = nullptr) noexcept;
  [[nodiscard]] static LinkedPartLifecycleResult repair(
      ProductDocument& product, DefinitionId definitionId,
      QString replacementRelativePathHint,
      project::LinkedPartSourcePolicy policy,
      AssemblyCommandHistory& history,
      ProductDocumentSuccessor& prepareSuccessor,
      AssemblyCommandSemanticState beforeState = {},
      AssemblyCommandSemanticState afterState = {},
      bool* resolvedOutdatedBeforeCommit = nullptr) noexcept;
};

}  // namespace solidar
