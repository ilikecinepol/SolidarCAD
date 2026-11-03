#include "project/ProductEnvelopeV11Materialization.h"

#include <QJsonDocument>

#include <Standard_Failure.hxx>

#include <exception>
#include <utility>
#include <vector>

#include "model/IdGeneration.h"
#include "project/internal/ProjectFormatV4ApplicationGate.h"
#include "project/internal/ProjectFormatV4CandidateComparison.h"
#include "project/internal/ProjectFormatV4DocumentMaterialization.h"

namespace solidar::project {
namespace {

ProductEnvelopeV11MaterializationResult failed(QString error) {
  ProductEnvelopeV11MaterializationResult result;
  result.error = std::move(error);
  return result;
}

std::string utf8(const QString& value) {
  const QByteArray bytes = value.toUtf8();
  return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

bool sameCandidate(const ProductEnvelopeV11Candidate& left,
                   const ProductEnvelopeV11Candidate& right) {
  if (left.authoredRoot != right.authoredRoot ||
      left.projectId != right.projectId ||
      left.rootDefinitionId != right.rootDefinitionId ||
      left.metadata != right.metadata || left.capabilities != right.capabilities ||
      left.graph != right.graph ||
      left.jsonStringUtf8Bytes != right.jsonStringUtf8Bytes ||
      left.embeddedPayloadBytes != right.embeddedPayloadBytes ||
      left.topologyReferenceCount != right.topologyReferenceCount ||
      left.provenanceRecordCount != right.provenanceRecordCount ||
      left.localNameUtf8Bytes != right.localNameUtf8Bytes ||
      left.rebuildWork != right.rebuildWork ||
      left.drawingCount != right.drawingCount ||
      left.drawingSourceReferenceCount !=
          right.drawingSourceReferenceCount ||
      left.drawingNameUtf8Bytes != right.drawingNameUtf8Bytes ||
      left.drawingWork != right.drawingWork ||
      left.definitions.size() != right.definitions.size())
    return false;
  for (std::size_t index = 0; index < left.definitions.size(); ++index) {
    const auto& a = left.definitions[index];
    const auto& b = right.definitions[index];
    if (a.definitionId != b.definitionId ||
        a.authoredDefinition != b.authoredDefinition ||
        a.authoredPayload != b.authoredPayload ||
        a.drawings != b.drawings ||
        a.occurrences != b.occurrences ||
        !detail::sameProjectFormatV4Candidate(a.document, b.document))
      return false;
  }
  return true;
}

}  // namespace

ProductEnvelopeV11MaterializationResult materializeProductEnvelopeV11(
    const ProductEnvelopeV11Candidate& candidate,
    ProductEnvelopeV11MaterializationFailurePoint failurePoint,
    ProductEnvelopeV11MaterializationMode mode) {
  try {
    ::solidar::detail::ScopedExplicitIdReservationPause reservationPause;
    const QByteArray bytes =
        QJsonDocument(candidate.authoredRoot).toJson(QJsonDocument::Compact);
    auto rescanned = scanProductEnvelopeV11(QByteArrayView(bytes));
    if (!rescanned.hasCandidate() ||
        !sameCandidate(candidate, *rescanned.candidate))
      return failed(QStringLiteral("v11 candidate revalidation failed"));

    std::vector<ComponentDefinition> components;
    components.reserve(rescanned.candidate->definitions.size());
    for (std::size_t index = 0;
         index < rescanned.candidate->definitions.size(); ++index) {
      const auto& definition = rescanned.candidate->definitions[index];
      if (failurePoint ==
              ProductEnvelopeV11MaterializationFailurePoint::BeforeLastDocument &&
          index + 1 == rescanned.candidate->definitions.size())
        return failed(QStringLiteral("injected v11 document failure"));
      auto materialized = detail::materializeProjectFormatV4Document(
          definition.document);
      if (!materialized.succeeded())
        return failed(QStringLiteral("v11 document materialization failed: %1")
                          .arg(materialized.error));
      const RecomputeResult recomputed =
          materialized.document->recomputeDetailed();
      if (!detail::acceptsProjectFormatV4RecomputeResult(recomputed))
        return failed(QStringLiteral("v11 document recompute failed"));
      ComponentDefinition component(
          definition.definitionId,
          std::make_unique<Document>(std::move(*materialized.document)),
          std::vector<ComponentDrawing>(definition.drawings));
      for (const auto& occurrence : definition.occurrences) {
        if (component.insert(ComponentOccurrence(
                occurrence.occurrenceId, occurrence.targetDefinitionId,
                occurrence.placement, occurrence.flags,
                occurrence.displayNameUtf8)) !=
            ComponentOccurrenceInsertStatus::Inserted)
          return failed(QStringLiteral("v11 occurrence insertion failed"));
      }
      components.push_back(std::move(component));
    }

    if (failurePoint == ProductEnvelopeV11MaterializationFailurePoint::
                            BeforeProductConstruction)
      return failed(QStringLiteral("injected v11 pre-product failure"));
    ProductDocumentIdentity identity{
        rescanned.candidate->projectId,
        {utf8(rescanned.candidate->metadata.name),
         utf8(rescanned.candidate->metadata.createdAt)}};
    auto created = ProductDocument::createComponentGraph(
        std::move(components), rescanned.candidate->rootDefinitionId,
        std::move(identity));
    if (!created)
      return failed(QStringLiteral("v11 atomic product construction failed"));
    if (failurePoint ==
        ProductEnvelopeV11MaterializationFailurePoint::Finalization)
      return failed(QStringLiteral("injected v11 finalization failure"));
    if (mode == ProductEnvelopeV11MaterializationMode::Publication &&
        !created.product->finalizeReservedIncarnation())
      return failed(QStringLiteral("v11 incarnation commit failed"));

    ProductEnvelopeV11MaterializationResult result;
    result.status = ProductEnvelopeV11MaterializationStatus::Materialized;
    result.product = std::move(created.product);
    return result;
  } catch (const Standard_Failure& failure) {
    return failed(QStringLiteral("OCCT failure while materializing v11: %1")
                      .arg(QString::fromUtf8(failure.what())));
  } catch (const std::exception& exception) {
    return failed(QStringLiteral("v11 materialization failure: %1")
                      .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return failed(QStringLiteral("unknown v11 materialization failure"));
  }
}

}  // namespace solidar::project
