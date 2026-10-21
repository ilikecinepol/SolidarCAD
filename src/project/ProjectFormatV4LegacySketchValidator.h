#pragma once

#include <QString>

#include <optional>

#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/ProjectFormatV4TopologyAggregator.h"

namespace solidar::project {

enum class ProjectFormatV4LegacySketchValidationStatus {
  Candidate,
  UnsupportedCandidate,
  Invalid,
};

struct ProjectFormatV4DeferredLegacySketchValidation {
  QString path;
  QString diagnostic;

  friend bool operator==(
      const ProjectFormatV4DeferredLegacySketchValidation&,
      const ProjectFormatV4DeferredLegacySketchValidation&) = default;
};

struct ProjectFormatV4LegacySketchCandidate {
  ProjectFormatV4TopologyAggregateCandidate topology;
  std::optional<ProjectFormatV4DeferredLegacySketchValidation>
      deferredLegacySketch;
};

struct ProjectFormatV4LegacySketchValidationResult {
  ProjectFormatV4LegacySketchValidationStatus status{
      ProjectFormatV4LegacySketchValidationStatus::Invalid};
  std::optional<ProjectFormatV4LegacySketchCandidate> candidate;
  QString error;

  [[nodiscard]] bool hasCandidate() const noexcept {
    return (status == ProjectFormatV4LegacySketchValidationStatus::Candidate ||
            status == ProjectFormatV4LegacySketchValidationStatus::UnsupportedCandidate) &&
           candidate.has_value();
  }
};

// P2e1 validates the legacy typed-sketch-v1 projection only. It rebuilds the
// canonical P2d aggregate from the supplied P2c candidate, preserves authored
// JSON, never materializes ProjectData/Sketch/Document and does not participate
// in production load.
[[nodiscard]] ProjectFormatV4LegacySketchValidationResult
validateProjectFormatV4LegacySketches(ProjectFormatV4PayloadCandidate payload);

}  // namespace solidar::project
