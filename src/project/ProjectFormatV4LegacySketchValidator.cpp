#include "project/ProjectFormatV4LegacySketchValidator.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <utility>

#include "project/internal/ProjectFormatV3TypedSketchValidation.h"

namespace solidar::project {
namespace {

ProjectFormatV4LegacySketchValidationResult invalid(QString error) {
  ProjectFormatV4LegacySketchValidationResult result;
  result.error = std::move(error);
  return result;
}

bool legacySketchIdsMatchModelSketches(
    const ProjectFormatV4TopologyAggregateCandidate& topology,
    QString* error) {
  const QJsonObject& root = topology.payload.envelope.authoredRoot;
  const QJsonArray legacy = root.value(QStringLiteral("sketches")).toArray();
  const QJsonArray model =
      root.value(QStringLiteral("model")).toObject()
          .value(QStringLiteral("sketches"))
          .toArray();
  if (legacy.size() != model.size()) {
    if (error)
      *error = QStringLiteral(
          "$.model.sketches: count does not match $.sketches");
    return false;
  }
  for (qsizetype index = 0; index < legacy.size(); ++index) {
    const qint64 legacyId =
        legacy.at(index).toObject().value(QStringLiteral("sketchId")).toInteger();
    const qint64 modelId =
        model.at(index).toObject().value(QStringLiteral("id")).toInteger();
    if (legacyId != modelId) {
      if (error)
        *error = QStringLiteral(
                     "$.model.sketches[%1].id: does not match "
                     "$.sketches[%1].sketchId")
                     .arg(index);
      return false;
    }
  }
  return true;
}

bool hasOpaqueConstraintEncoding(
    const ProjectFormatV4TopologyAggregateCandidate& topology) {
  const auto& deferred = topology.payload.envelope.deferredUnknowns;
  return std::any_of(deferred.begin(), deferred.end(), [](const auto& item) {
    return item.kind ==
           ProjectFormatV4DeferredUnknownKind::ConstraintTypeEncoding;
  });
}

}  // namespace

ProjectFormatV4LegacySketchValidationResult
validateProjectFormatV4LegacySketches(ProjectFormatV4PayloadCandidate payload) {
  auto topologyResult = aggregateProjectFormatV4Topology(std::move(payload));
  if (!topologyResult.hasCandidate())
    return invalid(topologyResult.error.isEmpty()
                       ? QStringLiteral("$: topology aggregation failed")
                       : std::move(topologyResult.error));

  ProjectFormatV4TopologyAggregateCandidate topology =
      std::move(*topologyResult.candidate);
  QString error;
  if (!legacySketchIdsMatchModelSketches(topology, &error))
    return invalid(std::move(error));

  const bool opaqueEncoding = hasOpaqueConstraintEncoding(topology);
  const auto typedSketchResult = detail::validateProjectFormatV3TypedSketches(
      topology.payload.envelope.authoredRoot,
      opaqueEncoding
          ? detail::ProjectFormatV3TypedSketchValidationMode::
                OpaqueConstraintEncoding
          : detail::ProjectFormatV3TypedSketchValidationMode::StableNameV1);
  if (typedSketchResult.status ==
      detail::ProjectFormatV3TypedSketchValidationStatus::Invalid) {
    return invalid(QStringLiteral("$.sketches: %1")
                       .arg(typedSketchResult.diagnostic));
  }

  ProjectFormatV4LegacySketchCandidate candidate;
  candidate.topology = std::move(topology);
  if (!opaqueEncoding &&
      typedSketchResult.status ==
          detail::ProjectFormatV3TypedSketchValidationStatus::Unsupported) {
    candidate.deferredLegacySketch =
        ProjectFormatV4DeferredLegacySketchValidation{
            QStringLiteral("$.sketches"), typedSketchResult.diagnostic};
  }

  ProjectFormatV4LegacySketchValidationResult result;
  const bool unsupported =
      topologyResult.status ==
          ProjectFormatV4TopologyAggregateStatus::UnsupportedCandidate ||
      typedSketchResult.status ==
          detail::ProjectFormatV3TypedSketchValidationStatus::Unsupported;
  result.status = unsupported
      ? ProjectFormatV4LegacySketchValidationStatus::UnsupportedCandidate
      : ProjectFormatV4LegacySketchValidationStatus::Candidate;
  result.candidate = std::move(candidate);
  return result;
}

}  // namespace solidar::project
