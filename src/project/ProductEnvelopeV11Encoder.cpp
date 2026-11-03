#include "project/ProductEnvelopeV11Encoder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <exception>
#include <utility>

#include "model/ComponentGraphAnalysis.h"
#include "project/ProductEnvelopeV11Decoder.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectIdCodec.h"
#include "project/internal/StrictOccurrenceCodec.h"

namespace solidar::project {
namespace {

ProductEnvelopeV11EncodeResult failed(ProductEnvelopeV11EncodeStatus status,
                                      QString error) {
  ProductEnvelopeV11EncodeResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

bool persisted(std::uint64_t value) {
  return value > 0 &&
         value <= static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId);
}

}  // namespace

ProductEnvelopeV11EncodeResult encodeProductEnvelopeV11Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata) {
  try {
    const auto projectText = detail::formatCanonicalProjectId(outputProjectId);
    if (!projectText ||
        source.rootDefinitionKind() != DefinitionKind::Component)
      return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                    QStringLiteral(
                        "v11 requires valid identity and component root"));
    if (analyzeComponentGraph(source.definitions(),
                              source.rootDefinitionId()) !=
        ComponentGraphStatus::Valid)
      return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                    QStringLiteral("source component graph is invalid"));

    const detail::ProjectFormatV4DocumentMetadata nestedMetadata{
        metadata.name, metadata.createdAt};
    QJsonArray definitions;
    for (const DefinitionId id : source.definitions().definitionIds()) {
      if (!persisted(id.value()))
        return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                      QStringLiteral("definition ID is outside range"));
      const auto* component = source.definitions().findComponent(id);
      if (!component)
        return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                      QStringLiteral("v11 supports component definitions only"));
      const auto encodedDocument =
          detail::encodeProjectFormatV4DocumentRoot(component->document(),
                                                     nestedMetadata);
      if (!encodedDocument.succeeded())
        return failed(
            encodedDocument.status ==
                    detail::ProjectFormatV4DocumentEncodeStatus::PayloadTooLarge
                ? ProductEnvelopeV11EncodeStatus::PayloadTooLarge
                : ProductEnvelopeV11EncodeStatus::Invalid,
            encodedDocument.error);
      QJsonArray occurrences;
      for (const auto& [occurrenceId, occurrence] : component->occurrences()) {
        if (occurrence.homePlacement() != occurrence.placement())
          return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                        QStringLiteral(
                            "v11 cannot encode independent home placement"));
        auto encodedOccurrence =
            detail::encodeStrictOccurrence(occurrenceId, occurrence);
        if (!encodedOccurrence)
          return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                        QStringLiteral("occurrence ID is outside range"));
        occurrences.push_back(std::move(*encodedOccurrence.occurrence));
      }
      QJsonArray drawings;
      for (const auto& drawing : component->drawings()) {
        if (!persisted(drawing.id().value()))
          return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                        QStringLiteral("drawing ID is outside range"));
        QJsonArray sourceBodyIds;
        for (const BodyId bodyId : drawing.sourceBodyIds()) {
          if (!persisted(bodyId))
            return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                          QStringLiteral("drawing body ID is outside range"));
          sourceBodyIds.push_back(static_cast<qint64>(bodyId));
        }
        drawings.push_back(QJsonObject{
            {QStringLiteral("drawingId"),
             static_cast<qint64>(drawing.id().value())},
            {QStringLiteral("name"),
             QString::fromUtf8(
                 drawing.nameUtf8().data(),
                 static_cast<qsizetype>(drawing.nameUtf8().size()))},
            {QStringLiteral("paper"), QStringLiteral("A4")},
            {QStringLiteral("orientation"),
             drawing.orientation() == ComponentDrawingOrientation::Portrait
                 ? QStringLiteral("portrait")
                 : QStringLiteral("landscape")},
            {QStringLiteral("scale"), drawing.scale()},
            {QStringLiteral("sourceBodyIds"), sourceBodyIds}});
      }
      definitions.push_back(QJsonObject{
          {QStringLiteral("definitionId"), static_cast<qint64>(id.value())},
          {QStringLiteral("definitionKind"), QStringLiteral("component")},
          {QStringLiteral("payload"),
           QJsonObject{{QStringLiteral("payloadKind"),
                        QStringLiteral("embedded-component-v4")},
                       {QStringLiteral("data"), *encodedDocument.root}}},
          {QStringLiteral("drawings"), drawings},
          {QStringLiteral("occurrences"), occurrences}});
    }
    const QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("solidar-project")},
        {QStringLiteral("version"), 11},
        {QStringLiteral("capabilities"),
         QJsonArray{QStringLiteral("component-definitions-v1"),
                    QStringLiteral("component-drawings-v1"),
                    QStringLiteral("nested-components-v1"),
                    QStringLiteral("product-envelope-v1")}},
        {QStringLiteral("projectId"),
         QString::fromLatin1(projectText->data(),
                             static_cast<qsizetype>(projectText->size()))},
        {QStringLiteral("rootDefinitionId"),
         static_cast<qint64>(source.rootDefinitionId().value())},
        {QStringLiteral("metadata"),
         QJsonObject{{QStringLiteral("name"), metadata.name},
                     {QStringLiteral("createdAt"), metadata.createdAt}}},
        {QStringLiteral("definitions"), definitions}};
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > ProjectFile::kMaximumFileBytes)
      return failed(ProductEnvelopeV11EncodeStatus::PayloadTooLarge,
                    QStringLiteral("serialized v11 exceeds file budget"));
    const auto rescanned = scanProductEnvelopeV11(QByteArrayView(bytes));
    if (!rescanned.hasCandidate() || rescanned.candidate->authoredRoot != root)
      return failed(ProductEnvelopeV11EncodeStatus::SerializedPayloadValidation,
                    rescanned.error.isEmpty()
                        ? QStringLiteral("serialized v11 failed self-scan")
                        : rescanned.error);
    ProductEnvelopeV11EncodeResult result;
    result.status = ProductEnvelopeV11EncodeStatus::Valid;
    result.payload.emplace(ProductEnvelopeV11EncodedPayload{root, bytes});
    return result;
  } catch (const std::exception& exception) {
    return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                  QString::fromUtf8(exception.what()));
  } catch (...) {
    return failed(ProductEnvelopeV11EncodeStatus::Invalid,
                  QStringLiteral("unknown v11 encoding failure"));
  }
}

}  // namespace solidar::project
