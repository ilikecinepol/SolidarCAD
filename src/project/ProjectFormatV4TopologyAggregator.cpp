#include "project/ProjectFormatV4TopologyAggregator.h"

#include <algorithm>
#include <map>
#include <utility>

namespace solidar::project {
namespace {

ProjectFormatV4TopologyAggregateResult invalid(QString error) {
  ProjectFormatV4TopologyAggregateResult result;
  result.error = std::move(error);
  return result;
}

QString envelopeDeferredDiagnostic(
    const ProjectFormatV4DeferredUnknown& deferred) {
  const QString category =
      deferred.kind == ProjectFormatV4DeferredUnknownKind::Capability
          ? QStringLiteral("unsupported capability")
          : QStringLiteral("unsupported constraint type encoding");
  return QStringLiteral("%1: %2").arg(category, deferred.token);
}

bool checkedAdd(std::size_t delta, std::size_t limit, std::size_t* total) {
  if (*total > limit || delta > limit - *total) return false;
  *total += delta;
  return true;
}

bool validateInspectionOwnership(
    const ProjectFormatV4ReferenceInspection& inspection,
    const std::map<FeatureId, BodyId>& featureOwners, QString* error) {
  const auto producer = featureOwners.find(inspection.topLevelFeatureId);
  if (producer == featureOwners.end() || producer->second != inspection.bodyId) {
    if (error)
      *error = QStringLiteral("topology producer owner is not in the payload");
    return false;
  }
  if (inspection.variant == ProjectFormatV4ReferenceVariant::V2) {
    for (const FeatureId origin : inspection.immediateOriginFeatureIds) {
      if (!featureOwners.contains(origin)) {
        if (error)
          *error =
              QStringLiteral("topology origin feature is not in the payload");
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool ProjectFormatV4TopologyAggregateResult::allTopologyReferencesDecoded()
    const noexcept {
  return hasCandidate() &&
         std::all_of(candidate->topologyReferences.begin(),
                     candidate->topologyReferences.end(),
                     [](const auto& reference) { return reference.has_value(); });
}

ProjectFormatV4TopologyAggregateResult aggregateProjectFormatV4Topology(
    ProjectFormatV4PayloadCandidate payload) {
  auto rebuiltResult = scanProjectFormatV4Payload(payload.envelope);
  if (!rebuiltResult.hasCandidate())
    return invalid(QStringLiteral("$: payload envelope is not canonical: %1")
                       .arg(rebuiltResult.error));

  ProjectFormatV4PayloadCandidate canonical =
      std::move(*rebuiltResult.candidate);
  if (payload.topologySites != canonical.topologySites)
    return invalid(QStringLiteral("$: topologySites do not match authored payload"));
  if (payload.featureOwners != canonical.featureOwners)
    return invalid(QStringLiteral("$: featureOwners do not match authored payload"));

  std::map<FeatureId, BodyId> featureOwners;
  for (const auto& owner : canonical.featureOwners) {
    if (!featureOwners.emplace(owner.featureId, owner.bodyId).second)
      return invalid(QStringLiteral("$: duplicate feature owner inventory"));
  }

  std::vector<ProjectFormatV4DeferredTopologyReference> deferred;
  deferred.reserve(canonical.envelope.deferredUnknowns.size());
  for (const auto& unknown : canonical.envelope.deferredUnknowns)
    deferred.push_back(
        {unknown.path, envelopeDeferredDiagnostic(unknown)});

  std::vector<std::optional<DecodedProjectFormatV4TopologyReference>> decoded;
  decoded.reserve(canonical.topologySites.size());
  std::size_t provenanceRecordCount = 0;
  std::size_t localNameUtf8Bytes = 0;

  for (const auto& site : canonical.topologySites) {
    auto reference = decodeProjectFormatV4TopologyReference(
        site.authoredReference, site.expectedKind);
    if (reference.status == ProjectFormatV4ReferenceDecodeStatus::Invalid)
      return invalid(QStringLiteral("%1: %2").arg(site.path, reference.error));

    if (!checkedAdd(reference.provenanceRecordCount,
                    kProjectFormatV4MaximumProvenanceRecords,
                    &provenanceRecordCount))
      return invalid(site.path +
                     QStringLiteral(": document provenance record limit exceeded"));
    if (!checkedAdd(reference.localNameUtf8Bytes,
                    kProjectFormatV4MaximumLocalNameUtf8Bytes,
                    &localNameUtf8Bytes))
      return invalid(site.path +
                     QStringLiteral(": document local-name byte limit exceeded"));

    if (reference.inspection) {
      QString ownershipError;
      if (!validateInspectionOwnership(*reference.inspection, featureOwners,
                                       &ownershipError))
        return invalid(QStringLiteral("%1: %2").arg(site.path, ownershipError));
    } else if (reference.status !=
               ProjectFormatV4ReferenceDecodeStatus::Unsupported) {
      return invalid(site.path +
                     QStringLiteral(": decoded reference lacks inspection"));
    }

    if (reference.status == ProjectFormatV4ReferenceDecodeStatus::Unsupported) {
      decoded.emplace_back(std::nullopt);
      deferred.push_back({site.path, reference.error});
      continue;
    }
    if (!reference.reference)
      return invalid(site.path +
                     QStringLiteral(": valid reference lacks a carrier"));
    decoded.emplace_back(std::move(reference.reference));
  }

  ProjectFormatV4TopologyAggregateCandidate candidate;
  candidate.payload = std::move(canonical);
  candidate.topologyReferences = std::move(decoded);
  candidate.deferredUnsupported = std::move(deferred);
  candidate.provenanceRecordCount = provenanceRecordCount;
  candidate.localNameUtf8Bytes = localNameUtf8Bytes;

  ProjectFormatV4TopologyAggregateResult result;
  result.status = candidate.deferredUnsupported.empty()
                      ? ProjectFormatV4TopologyAggregateStatus::Candidate
                      : ProjectFormatV4TopologyAggregateStatus::UnsupportedCandidate;
  result.candidate = std::move(candidate);
  return result;
}

}  // namespace solidar::project
