#pragma once

#include <QString>

#include <optional>

#include "model/Document.h"
#include "project/ProjectFormatV4ModelValidator.h"

namespace solidar::project::detail {

enum class ProjectFormatV4DocumentMaterializationStatus {
  Materialized,
  Invalid,
};

struct ProjectFormatV4DocumentMaterializationResult {
  ProjectFormatV4DocumentMaterializationStatus status{
      ProjectFormatV4DocumentMaterializationStatus::Invalid};
  std::optional<Document> document;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status ==
               ProjectFormatV4DocumentMaterializationStatus::Materialized &&
           document.has_value();
  }
};

// Construction-only P3c boundary. The input must be a complete P2 Candidate.
// The returned Document is detached and remains dirty: this function never
// recomputes, resolves topology, reserves process-global IDs, publishes to the
// application, or performs file I/O.
[[nodiscard]] ProjectFormatV4DocumentMaterializationResult
materializeProjectFormatV4Document(
    const ProjectFormatV4ModelCandidate& candidate);

}  // namespace solidar::project::detail
