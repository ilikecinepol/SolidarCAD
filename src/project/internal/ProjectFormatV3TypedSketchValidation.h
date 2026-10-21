#pragma once

#include <QJsonObject>
#include <QString>

namespace solidar::project::detail {

enum class ProjectFormatV3TypedSketchValidationStatus {
  Candidate,
  Unsupported,
  Invalid,
};

enum class ProjectFormatV3TypedSketchValidationMode {
  StableNameV1,
  OpaqueConstraintEncoding,
};

struct ProjectFormatV3TypedSketchValidationResult {
  ProjectFormatV3TypedSketchValidationStatus status{
      ProjectFormatV3TypedSketchValidationStatus::Invalid};
  QString diagnostic;
};

// Internal persistence seam. The pure-QJson implementation deliberately
// remains beside the authoritative v3 constraint registry and validators in
// ProjectFile.cpp. It does not materialize ProjectData, Sketch or Document.
[[nodiscard]] ProjectFormatV3TypedSketchValidationResult
validateProjectFormatV3TypedSketches(
    const QJsonObject& authoredRoot,
    ProjectFormatV3TypedSketchValidationMode mode);

}  // namespace solidar::project::detail
