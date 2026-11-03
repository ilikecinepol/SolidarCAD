#pragma once

#include <QByteArray>
#include <QString>

#include <memory>

namespace solidar::project::detail {

enum class ProjectFileRelocationFailure {
  None,
  InvalidPath,
  SourceMissing,
  SourceNotRegular,
  AmbiguousPath,
  MultipleLinks,
  SameFile,
  DestinationExists,
  CrossVolume,
  SourceChanged,
  DestinationChanged,
  UnsupportedFilesystem,
  RenameFailed,
};

struct ProjectFileRelocationResult {
  ProjectFileRelocationFailure failure{
      ProjectFileRelocationFailure::InvalidPath};
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return failure == ProjectFileRelocationFailure::None;
  }
};

struct PreparedProjectFileRelocation {
  ProjectFileRelocationResult status;
  QByteArray sourceBytes;

  [[nodiscard]] bool succeeded() const noexcept {
    return status.succeeded();
  }
};

// Test seam and production contract for a single same-volume namespace
// transaction. prepare() must retain the exact source object it inspected;
// commitNoReplace() must never overwrite a destination that appears later.
class ProjectFileRelocationTransaction {
 public:
  virtual ~ProjectFileRelocationTransaction() = default;

  [[nodiscard]] virtual PreparedProjectFileRelocation prepare(
      const QString& sourcePath, const QString& targetPath) = 0;
  [[nodiscard]] virtual ProjectFileRelocationResult commitNoReplace() = 0;
  virtual void cancel() noexcept = 0;
};

// Internal factory exposed for deterministic native filesystem race tests.
[[nodiscard]] std::unique_ptr<ProjectFileRelocationTransaction>
makeProjectFileRelocationTransaction();

}  // namespace solidar::project::detail
