#include "project/ProductEnvelopeV6Materialization.h"

#include <Standard_Failure.hxx>

#include <QByteArray>
#include <QJsonDocument>

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/IdGeneration.h"
#include "project/internal/ProjectFormatV4ApplicationGate.h"
#include "project/internal/ProjectFormatV4DocumentMaterialization.h"

namespace solidar::project {
namespace {

bool equivalentV4(const ProjectFormatV4ModelCandidate& lhs,
                  const ProjectFormatV4ModelCandidate& rhs) {
  const auto& ll = lhs.legacySketch;
  const auto& rl = rhs.legacySketch;
  const auto& lt = ll.topology;
  const auto& rt = rl.topology;
  const auto& lp = lt.payload;
  const auto& rp = rt.payload;
  const auto& le = lp.envelope;
  const auto& re = rp.envelope;
  return ll.deferredLegacySketch == rl.deferredLegacySketch &&
         le.authoredRoot == re.authoredRoot && le.document == re.document &&
         le.legacySketches == re.legacySketches &&
         le.legacyExtrusion == re.legacyExtrusion && le.model == re.model &&
         le.capabilities == re.capabilities &&
         le.deferredUnknowns == re.deferredUnknowns &&
         lp.topologySites == rp.topologySites &&
         lp.featureOwners == rp.featureOwners &&
         lp.rebuildWork == rp.rebuildWork &&
         lt.topologyReferences == rt.topologyReferences &&
         lt.topologyInspections == rt.topologyInspections &&
         lt.deferredUnsupported == rt.deferredUnsupported &&
         lt.provenanceRecordCount == rt.provenanceRecordCount &&
         lt.localNameUtf8Bytes == rt.localNameUtf8Bytes;
}

bool equivalentPart(const ProductEnvelopeV6PartCandidate& lhs,
                    const ProductEnvelopeV6PartCandidate& rhs) {
  return lhs.definitionId == rhs.definitionId &&
         lhs.authoredDefinition == rhs.authoredDefinition &&
         lhs.authoredPayload == rhs.authoredPayload &&
         lhs.embeddedPart.has_value() == rhs.embeddedPart.has_value() &&
         (!lhs.embeddedPart || equivalentV4(*lhs.embeddedPart,
                                            *rhs.embeddedPart));
}

bool equivalentCandidate(const ProductEnvelopeV6Candidate& lhs,
                         const ProductEnvelopeV6Candidate& rhs) {
  if (lhs.authoredRoot != rhs.authoredRoot ||
      lhs.projectId != rhs.projectId ||
      lhs.rootDefinitionId != rhs.rootDefinitionId ||
      lhs.metadata != rhs.metadata || lhs.capabilities != rhs.capabilities ||
      lhs.assembly != rhs.assembly ||
      lhs.deferredUnknowns != rhs.deferredUnknowns ||
      lhs.jsonStringUtf8Bytes != rhs.jsonStringUtf8Bytes ||
      lhs.embeddedPartBytes != rhs.embeddedPartBytes ||
      lhs.rebuildWork != rhs.rebuildWork || lhs.parts.size() != rhs.parts.size())
    return false;
  for (std::size_t index = 0; index < lhs.parts.size(); ++index) {
    if (!equivalentPart(lhs.parts[index], rhs.parts[index])) return false;
  }
  return true;
}

std::string exactUtf8(const QString& value) {
  const QByteArray bytes = value.toUtf8();
  return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

ProductEnvelopeV6MaterializationResult failure(QString error) {
  ProductEnvelopeV6MaterializationResult result;
  result.error = std::move(error);
  return result;
}

ProductEnvelopeV6MaterializationResult unsupported(QString error) {
  ProductEnvelopeV6MaterializationResult result;
  result.status = ProductEnvelopeV6MaterializationStatus::Unsupported;
  result.error = std::move(error);
  return result;
}

}  // namespace

ProductEnvelopeV6MaterializationResult materializeProductEnvelopeV6(
    const ProductEnvelopeV6Candidate& candidate) {
  try {
    const QByteArray bytes =
        QJsonDocument(candidate.authoredRoot).toJson(QJsonDocument::Compact);
    auto rescanned = scanProductEnvelopeV6(QByteArrayView(bytes));
    if (!rescanned.hasCandidate())
      return failure(QStringLiteral("v6 envelope revalidation failed: %1")
                         .arg(rescanned.error));
    if (!equivalentCandidate(candidate, *rescanned.candidate))
      return failure(QStringLiteral(
          "v6 candidate does not match its authored root"));
    if (rescanned.status == ProductEnvelopeV6ScanStatus::Unsupported)
      return unsupported(
          rescanned.error.isEmpty()
              ? QStringLiteral("v6 envelope contains unsupported semantics")
              : std::move(rescanned.error));
    if (!rescanned.candidate->assembly)
      return failure(QStringLiteral("v6 root assembly candidate is missing"));

    struct MaterializedPart {
      DefinitionId id;
      Document document;
    };
    std::vector<MaterializedPart> materializedParts;
    materializedParts.reserve(rescanned.candidate->parts.size());
    DefinitionRegistry definitions;
    {
      ::solidar::detail::ScopedExplicitIdReservationPause reservationPause;
      for (const auto& part : rescanned.candidate->parts) {
        if (!part.embeddedPart)
          return failure(QStringLiteral("v6 part has no embedded candidate"));
        auto nested = detail::materializeProjectFormatV4Document(
            *part.embeddedPart);
        if (!nested.succeeded())
          return failure(
              nested.error.isEmpty()
                  ? QStringLiteral("embedded v4 part materialization failed")
                  : QStringLiteral(
                        "embedded v4 part materialization failed: %1")
                        .arg(nested.error));
        const RecomputeResult recomputed = nested.document->recomputeDetailed();
        if (!detail::acceptsProjectFormatV4RecomputeResult(recomputed))
          return failure(QStringLiteral(
              "embedded v4 part recompute was rejected"));
        materializedParts.push_back(
            {part.definitionId, std::move(*nested.document)});
      }

      for (auto& part : materializedParts) {
        if (definitions.insert(PartDefinition(
                part.id,
                std::make_unique<Document>(std::move(part.document)))) !=
            DefinitionInsertStatus::Inserted)
          return failure(QStringLiteral("v6 part insertion failed"));
      }
      AssemblyDefinition assembly(rescanned.candidate->assembly->definitionId);
      for (const auto& occurrence :
           rescanned.candidate->assembly->occurrences) {
        if (assembly.insert(ComponentOccurrence(
                occurrence.occurrenceId, occurrence.targetDefinitionId,
                occurrence.placement, occurrence.flags)) !=
            AssemblyOccurrenceInsertStatus::Inserted)
          return failure(QStringLiteral("v6 occurrence insertion failed"));
      }
      if (definitions.insert(std::move(assembly)) !=
          DefinitionInsertStatus::Inserted)
        return failure(QStringLiteral("v6 root assembly insertion failed"));
    }

    ProductDocumentIdentity identity{
        rescanned.candidate->projectId,
        {exactUtf8(rescanned.candidate->metadata.name),
         exactUtf8(rescanned.candidate->metadata.createdAt)}};
    auto created = ProductDocument::create(
        std::move(definitions), rescanned.candidate->rootDefinitionId,
        std::move(identity));
    if (!created)
      return failure(QStringLiteral("v6 product construction failed"));

    ProductEnvelopeV6MaterializationResult result;
    result.status = ProductEnvelopeV6MaterializationStatus::Materialized;
    result.product = std::move(created.product);
    return result;
  } catch (const Standard_Failure& failureValue) {
    return failure(QStringLiteral("OCCT failure while materializing v6: %1")
                       .arg(QString::fromUtf8(failureValue.what())));
  } catch (const std::exception& exception) {
    return failure(QStringLiteral("failure while materializing v6: %1")
                       .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return failure(QStringLiteral("unknown failure while materializing v6"));
  }
}

}  // namespace solidar::project
