#pragma once

#include <QString>

#include <optional>

#include "project/ProjectFormatV4LegacySketchValidator.h"
#include "project/ProjectFormatV4PayloadScanner.h"

namespace solidar::project {

enum class ProjectFormatV4ModelValidationStatus {
  Candidate,
  UnsupportedCandidate,
  Invalid,
};

struct ProjectFormatV4ModelCandidate {
  ProjectFormatV4LegacySketchCandidate legacySketch;
};

struct ProjectFormatV4ModelValidationResult {
  ProjectFormatV4ModelValidationStatus status{
      ProjectFormatV4ModelValidationStatus::Invalid};
  std::optional<ProjectFormatV4ModelCandidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return (status == ProjectFormatV4ModelValidationStatus::Candidate ||
            status ==
                ProjectFormatV4ModelValidationStatus::UnsupportedCandidate) &&
           candidate.has_value();
  }
};

// P2e2 validates remaining v3-compatible model semantics on authored JSON.
// It canonically rebuilds all earlier stages and never materializes a Document.
[[nodiscard]] ProjectFormatV4ModelValidationResult
validateProjectFormatV4Model(ProjectFormatV4PayloadCandidate payload);

}  // namespace solidar::project
