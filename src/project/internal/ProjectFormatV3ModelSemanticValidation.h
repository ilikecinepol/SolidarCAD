#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <optional>
#include <vector>

#include "model/ModelIds.h"
#include "model/TopologyReference.h"

namespace solidar::project::detail {

enum class ProjectFormatV3ModelSemanticValidationStatus {
  Candidate,
  Unsupported,
  Invalid,
};

struct ProjectFormatV3TopologyInspectionView {
  QString path;
  QJsonValue authoredReference;
  TopologyKind expectedKind{TopologyKind::Face};
  std::optional<BodyId> bodyId;
  std::optional<FeatureId> featureId;
};

struct ProjectFormatV3ModelSemanticValidationResult {
  ProjectFormatV3ModelSemanticValidationStatus status{
      ProjectFormatV3ModelSemanticValidationStatus::Invalid};
  QString diagnostic;
};

// Internal pure-QJson seam beside the authoritative v3 feature codec registry.
// It validates feature semantics without materializing model/runtime objects.
[[nodiscard]] ProjectFormatV3ModelSemanticValidationResult
validateProjectFormatV3ModelSemantics(
    const QJsonObject& authoredRoot,
    const std::vector<ProjectFormatV3TopologyInspectionView>& topology);

}  // namespace solidar::project::detail
