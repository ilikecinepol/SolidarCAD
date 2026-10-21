#include "project/ProjectFormatV4ModelValidator.h"

#include <utility>

#include "project/internal/ProjectFormatV3ModelSemanticValidation.h"

namespace solidar::project {
namespace {

ProjectFormatV4ModelValidationResult invalid(QString error) {
  ProjectFormatV4ModelValidationResult result;
  result.error = std::move(error);
  return result;
}

}  // namespace

ProjectFormatV4ModelValidationResult validateProjectFormatV4Model(
    ProjectFormatV4PayloadCandidate payload) {
  auto legacyResult = validateProjectFormatV4LegacySketches(std::move(payload));
  if (!legacyResult.hasCandidate())
    return invalid(legacyResult.error.isEmpty()
                       ? QStringLiteral("$: legacy sketch validation failed")
                       : std::move(legacyResult.error));

  ProjectFormatV4LegacySketchCandidate legacy =
      std::move(*legacyResult.candidate);
  const auto& topology = legacy.topology;
  if (topology.topologyInspections.size() !=
      topology.payload.topologySites.size())
    return invalid(QStringLiteral("$: topology inspection inventory is not aligned"));

  std::vector<detail::ProjectFormatV3TopologyInspectionView> views;
  views.reserve(topology.payload.topologySites.size());
  for (std::size_t index = 0; index < topology.payload.topologySites.size();
       ++index) {
    const auto& site = topology.payload.topologySites[index];
    detail::ProjectFormatV3TopologyInspectionView view;
    view.path = site.path;
    view.authoredReference = site.authoredReference;
    view.expectedKind = site.expectedKind;
    if (const auto& inspection = topology.topologyInspections[index]) {
      view.bodyId = inspection->bodyId;
      view.featureId = inspection->topLevelFeatureId;
    }
    views.push_back(std::move(view));
  }

  const auto semantic = detail::validateProjectFormatV3ModelSemantics(
      topology.payload.envelope.authoredRoot, views);
  if (semantic.status ==
      detail::ProjectFormatV3ModelSemanticValidationStatus::Invalid)
    return invalid(QStringLiteral("$.model: %1").arg(semantic.diagnostic));

  ProjectFormatV4ModelValidationResult result;
  const bool unsupported =
      legacyResult.status ==
          ProjectFormatV4LegacySketchValidationStatus::UnsupportedCandidate ||
      semantic.status ==
          detail::ProjectFormatV3ModelSemanticValidationStatus::Unsupported;
  result.status = unsupported
                      ? ProjectFormatV4ModelValidationStatus::UnsupportedCandidate
                      : ProjectFormatV4ModelValidationStatus::Candidate;
  result.candidate = ProjectFormatV4ModelCandidate{std::move(legacy)};
  return result;
}

}  // namespace solidar::project
