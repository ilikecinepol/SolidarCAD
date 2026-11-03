#pragma once

#include <QByteArrayView>
#include <QString>
#include <QStringList>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "model/ProductDocument.h"

namespace solidar::project {

namespace detail {
class LinkedPartSourceFileSystem;
}

enum class LinkedPathSemantics { Windows, Posix };

[[nodiscard]] SourceContentHash linkedSourceSha256(QByteArrayView bytes);

// Pure lexical validation shared by persistence and runtime resolution.
// This performs no filesystem access.
[[nodiscard]] bool isSafeRelativeLinkedPathHint(
    QStringView hint,
    LinkedPathSemantics semantics = LinkedPathSemantics::Windows,
    std::size_t maximumComponents = 256);

struct LinkedPartSourceDescriptor {
  ExternalSourceKey key;
  QString relativePathHint;
  SourceContentHash expectedHash;
};

struct LinkedPartSourceBudgets {
  std::size_t maximumDescriptors{256};
  std::uint64_t maximumFileBytes{64ULL * 1024ULL * 1024ULL};
  std::uint64_t maximumTotalBytes{64ULL * 1024ULL * 1024ULL};
  std::uint64_t maximumTotalPathUtf8Bytes{1024ULL * 1024ULL};
  std::size_t maximumRoots{32};
  std::uint64_t maximumTotalRootUtf8Bytes{64ULL * 1024ULL};
  std::size_t maximumPathComponents{256};
  std::uint32_t maximumAliasDepth{32};
  std::uint64_t maximumFileSystemWork{4096};
  std::uint64_t maximumRebuildWork{10000};
};

struct LinkedPartSourcePolicy {
  QString projectRoot;
  QStringList libraryRoots;
  LinkedPathSemantics pathSemantics{LinkedPathSemantics::Windows};
  LinkedPartSourceBudgets budgets;
};

enum class LinkedPartSourceResolveStatus {
  Resolved,
  Missing,
  InvalidDescriptor,
  PathEscape,
  OutsideAllowedRoot,
  NetworkDisabled,
  SourceNotRegular,
  TraversalLoop,
  BudgetExceeded,
  InvalidSource,
  UnsupportedSource,
  IdentityUnavailable,
  DefinitionMissing,
  NestingUnsupported,
  IdentityMismatch,
  Outdated,
  IdentityConflict,
  SourceChanged,
  ResourceFailure,
};

struct ResolvedLinkedPartSource {
  ExternalSourceKey key;
  SourceContentHash contentHash;
  QString canonicalPath;
  std::vector<std::size_t> descriptorIndexes;
  std::unique_ptr<ProductDocument> product;
};

struct LinkedPartSourceResolveResult {
  LinkedPartSourceResolveStatus status{
      LinkedPartSourceResolveStatus::InvalidDescriptor};
  std::size_t failureIndex{};
  QString error;
  std::vector<ResolvedLinkedPartSource> sources;
  // Deterministic evidence counters. They describe staged work only and do
  // not imply publication.
  std::uint64_t filesInspected{};
  std::uint64_t filesRead{};
  std::uint64_t sourcesDecoded{};

  [[nodiscard]] bool succeeded() const noexcept {
    return status == LinkedPartSourceResolveStatus::Resolved;
  }
};

// Runtime-only A6a boundary.  The returned products remain disconnected; the
// function performs no publication, persistence update, reload, repair, scan,
// network access, or write.
[[nodiscard]] LinkedPartSourceResolveResult resolveLinkedPartSources(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    const LinkedPartSourcePolicy& policy,
    detail::LinkedPartSourceFileSystem& fileSystem);

// Explicit lifecycle intents. Reload keeps the strict identity checks but
// accepts a new content revision and returns a fully materialized candidate.
// Repair probes identity/currentness only; its returned source intentionally
// owns no ProductDocument and cannot accept a revision by itself.
[[nodiscard]] LinkedPartSourceResolveResult resolveLinkedPartSourcesForReload(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    const LinkedPartSourcePolicy& policy,
    detail::LinkedPartSourceFileSystem& fileSystem);
[[nodiscard]] LinkedPartSourceResolveResult probeLinkedPartSourcesForRepair(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    const LinkedPartSourcePolicy& policy,
    detail::LinkedPartSourceFileSystem& fileSystem);

[[nodiscard]] LinkedPartSourceResolveResult resolveLinkedPartSourcesNative(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    LinkedPartSourcePolicy policy);
[[nodiscard]] LinkedPartSourceResolveResult
resolveLinkedPartSourcesForReloadNative(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    LinkedPartSourcePolicy policy);
[[nodiscard]] LinkedPartSourceResolveResult
probeLinkedPartSourcesForRepairNative(
    const std::vector<LinkedPartSourceDescriptor>& descriptors,
    LinkedPartSourcePolicy policy);

}  // namespace solidar::project
