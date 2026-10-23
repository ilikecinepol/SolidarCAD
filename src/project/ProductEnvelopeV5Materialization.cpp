#include "project/ProductEnvelopeV5Materialization.h"

#include <Standard_Failure.hxx>

#include <QByteArray>
#include <QJsonDocument>

#include <exception>
#include <optional>
#include <string>
#include <utility>

#include "model/IdGeneration.h"
#include "project/internal/ProjectFormatV4ApplicationGate.h"
#include "project/internal/ProjectFormatV4DocumentMaterialization.h"

namespace solidar::project {
namespace {

bool equivalentV4ModelCandidate(const ProjectFormatV4ModelCandidate& lhs,
                                const ProjectFormatV4ModelCandidate& rhs) {
  const auto& leftLegacy = lhs.legacySketch;
  const auto& rightLegacy = rhs.legacySketch;
  const auto& left = leftLegacy.topology;
  const auto& right = rightLegacy.topology;
  const auto& leftPayload = left.payload;
  const auto& rightPayload = right.payload;
  const auto& leftEnvelope = leftPayload.envelope;
  const auto& rightEnvelope = rightPayload.envelope;
  return leftLegacy.deferredLegacySketch ==
             rightLegacy.deferredLegacySketch &&
         leftEnvelope.authoredRoot == rightEnvelope.authoredRoot &&
         leftEnvelope.document == rightEnvelope.document &&
         leftEnvelope.legacySketches == rightEnvelope.legacySketches &&
         leftEnvelope.legacyExtrusion == rightEnvelope.legacyExtrusion &&
         leftEnvelope.model == rightEnvelope.model &&
         leftEnvelope.capabilities == rightEnvelope.capabilities &&
         leftEnvelope.deferredUnknowns == rightEnvelope.deferredUnknowns &&
         leftPayload.topologySites == rightPayload.topologySites &&
         leftPayload.featureOwners == rightPayload.featureOwners &&
         left.topologyReferences == right.topologyReferences &&
         left.topologyInspections == right.topologyInspections &&
         left.deferredUnsupported == right.deferredUnsupported &&
         left.provenanceRecordCount == right.provenanceRecordCount &&
         left.localNameUtf8Bytes == right.localNameUtf8Bytes;
}

bool equivalentCandidate(const ProductEnvelopeV5Candidate& lhs,
                         const ProductEnvelopeV5Candidate& rhs) {
  if (lhs.authoredRoot != rhs.authoredRoot ||
      lhs.projectId != rhs.projectId ||
      lhs.rootDefinitionId != rhs.rootDefinitionId ||
      lhs.definitionId != rhs.definitionId || lhs.metadata != rhs.metadata ||
      lhs.capabilities != rhs.capabilities ||
      lhs.authoredDefinition != rhs.authoredDefinition ||
      lhs.authoredPayload != rhs.authoredPayload ||
      lhs.deferredUnknowns != rhs.deferredUnknowns ||
      lhs.embeddedPart.has_value() != rhs.embeddedPart.has_value()) {
    return false;
  }
  return !lhs.embeddedPart ||
         equivalentV4ModelCandidate(*lhs.embeddedPart, *rhs.embeddedPart);
}

std::string exactUtf8(const QString& value) {
  const QByteArray bytes = value.toUtf8();
  return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

ProductEnvelopeV5MaterializationResult failure(QString error) {
  ProductEnvelopeV5MaterializationResult result;
  result.error = std::move(error);
  return result;
}

ProductEnvelopeV5MaterializationResult unsupported(QString error) {
  ProductEnvelopeV5MaterializationResult result;
  result.status = ProductEnvelopeV5MaterializationStatus::Unsupported;
  result.error = std::move(error);
  return result;
}

}  // namespace

ProductEnvelopeV5MaterializationResult materializeProductEnvelopeV5(
    const ProductEnvelopeV5Candidate& candidate) {
  try {
    const QByteArray authoredBytes =
        QJsonDocument(candidate.authoredRoot).toJson(QJsonDocument::Compact);
    auto rescanned = scanProductEnvelopeV5(QByteArrayView(authoredBytes));
    if ((rescanned.status != ProductEnvelopeV5ScanStatus::Candidate &&
         rescanned.status != ProductEnvelopeV5ScanStatus::Unsupported) ||
        !rescanned.candidate) {
      return failure(QStringLiteral("v5 envelope revalidation failed: %1")
                         .arg(rescanned.error));
    }
    if (!equivalentCandidate(candidate, *rescanned.candidate)) {
      return failure(
          QStringLiteral("v5 candidate does not match authored root"));
    }
    if (rescanned.status == ProductEnvelopeV5ScanStatus::Unsupported) {
      return unsupported(
          rescanned.error.isEmpty()
              ? QStringLiteral("v5 envelope contains unsupported semantics")
              : std::move(rescanned.error));
    }
    if (!rescanned.candidate->embeddedPart) {
      return failure(QStringLiteral("v5 candidate has no embedded part"));
    }

    std::optional<Document> document;
    {
      ::solidar::detail::ScopedExplicitIdReservationPause reservationPause;
      auto nested = detail::materializeProjectFormatV4Document(
          *rescanned.candidate->embeddedPart);
      if (!nested.succeeded()) {
        return failure(
            nested.error.isEmpty()
                ? QStringLiteral("embedded v4 part materialization failed")
                : QStringLiteral("embedded v4 part materialization failed: %1")
                      .arg(nested.error));
      }

      const RecomputeResult recomputed = nested.document->recomputeDetailed();
      if (!detail::acceptsProjectFormatV4RecomputeResult(recomputed)) {
        return failure(
            QStringLiteral("embedded v4 part recompute was rejected"));
      }
      document.emplace(std::move(*nested.document));
    }

    DefinitionRegistry definitions;
    PartDefinition root(candidate.rootDefinitionId,
                        std::make_unique<Document>(std::move(*document)));
    if (definitions.insert(std::move(root)) !=
        DefinitionInsertStatus::Inserted) {
      return failure(QStringLiteral("v5 root definition insertion failed"));
    }

    ProductDocumentIdentity identity{
        candidate.projectId,
        {exactUtf8(candidate.metadata.name),
         exactUtf8(candidate.metadata.createdAt)}};
    auto created = ProductDocument::create(
        std::move(definitions), candidate.rootDefinitionId,
        std::move(identity));
    if (!created) {
      return failure(QStringLiteral("v5 product construction failed"));
    }

    ProductEnvelopeV5MaterializationResult result;
    result.status = ProductEnvelopeV5MaterializationStatus::Materialized;
    result.product = std::move(created.product);
    return result;
  } catch (const Standard_Failure& occtFailure) {
    return failure(
        QStringLiteral("OCCT failure while materializing v5: %1")
            .arg(QString::fromUtf8(occtFailure.what())));
  } catch (const std::exception& exception) {
    return failure(QStringLiteral("failure while materializing v5: %1")
                       .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return failure(QStringLiteral("unknown failure while materializing v5"));
  }
}

}  // namespace solidar::project
