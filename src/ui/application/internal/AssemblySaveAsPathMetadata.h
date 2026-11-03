#pragma once

#include <vector>

#include "ui/application/ProjectApplicationService.h"

namespace solidar::application::detail {

// Read-only, capability-narrow metadata seam used by assembly Save As. It cannot
// enumerate, open, mutate or publish files; it only compares two named paths.
enum class AssemblySaveAsPathRelation {
  Same,
  Different,
  Missing,
  Error,
};

enum class PathNameComparisonPolicy {
  CaseSensitiveExact,
  UnicodeNormalizedCaseSensitive,
  UnicodeNormalizedCaseInsensitive,
  Unknown,
  Error,
};

enum class MissingPathResolutionStatus {
  Resolved,
  Missing,
  Error,
};

struct MissingPathResolution final {
  MissingPathResolutionStatus status{MissingPathResolutionStatus::Missing};
  QString physicalParent;
  std::vector<QString> suffix;
  PathNameComparisonPolicy nameComparisonPolicy{
      PathNameComparisonPolicy::Unknown};
};

class AssemblySaveAsPathMetadataReader {
 public:
  virtual ~AssemblySaveAsPathMetadataReader() = default;

  [[nodiscard]] virtual AssemblySaveAsPathRelation compareFileIdentity(
      const QString& left, const QString& right) const noexcept = 0;
  [[nodiscard]] virtual AssemblySaveAsPathRelation compareCanonicalPaths(
      const QString& left, const QString& right) const noexcept = 0;
  // Used only when a leaf is missing. Implementations resolve the longest
  // existing physical parent without creating or enumerating anything and
  // return the untouched remaining components plus the filesystem's proven
  // name-comparison policy. Unknown policy and every metadata failure are
  // fail-closed by the caller.
  [[nodiscard]] virtual MissingPathResolution resolveMissingPath(
      const QString& path) const noexcept = 0;
};

// Deterministic application-boundary entry point for tests. Production uses
// the native read-only implementation through ProjectApplicationService.
[[nodiscard]] ProjectApplicationStatus saveAssemblyV7As(
    const QString& targetPath, ProductDocument& productDocument,
    const AssemblySaveAsRequest& request,
    const AssemblySaveAsPathMetadataReader& metadataReader) noexcept;

[[nodiscard]] ProjectApplicationStatus saveAssemblyAs(
    const QString& targetPath, ProductDocument& productDocument,
    project::ProjectPersistenceState persistenceState,
    const AssemblySaveAsRequest& request,
    const AssemblySaveAsPathMetadataReader& metadataReader) noexcept;

}  // namespace solidar::application::detail
