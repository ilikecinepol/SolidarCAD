#include "project/ProductEnvelopeV10Encoder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <exception>
#include <utility>

#include "model/ComponentGraphAnalysis.h"
#include "project/ProductEnvelopeV10Decoder.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectIdCodec.h"
#include "project/internal/StrictOccurrenceCodec.h"

namespace solidar::project {
namespace {

ProductEnvelopeV10EncodeResult failed(ProductEnvelopeV10EncodeStatus status,
                                      QString error) {
  ProductEnvelopeV10EncodeResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

bool persisted(std::uint64_t value) {
  return value > 0 &&
         value <= static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId);
}

}  // namespace

ProductEnvelopeV10EncodeResult encodeProductEnvelopeV10Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata) {
  try {
    const auto projectText = detail::formatCanonicalProjectId(outputProjectId);
    if (!projectText ||
        source.rootDefinitionKind() != DefinitionKind::Component)
      return failed(ProductEnvelopeV10EncodeStatus::Invalid,
                    QStringLiteral(
                        "v10 requires valid identity and component root"));
    // V10 has no drawing schema.  Reject every authored drawing before graph
    // validation or JSON construction so an unreachable definition cannot be
    // silently lossy and no caller can observe a partially encoded payload.
    for (const DefinitionId id : source.definitions().definitionIds()) {
      const auto* component = source.definitions().findComponent(id);
      if (!component)
        return failed(ProductEnvelopeV10EncodeStatus::Invalid,
                      QStringLiteral(
                          "v10 supports component definitions only"));
      if (!component->drawings().empty())
        return failed(
            ProductEnvelopeV10EncodeStatus::UnsupportedComponentDrawings,
            QStringLiteral(
                "v10 cannot persist component drawings without data loss"));
    }
    if (analyzeComponentGraph(source.definitions(),
                              source.rootDefinitionId()) !=
        ComponentGraphStatus::Valid)
      return failed(ProductEnvelopeV10EncodeStatus::Invalid,
                    QStringLiteral("source component graph is invalid"));

    const detail::ProjectFormatV4DocumentMetadata nestedMetadata{
        metadata.name, metadata.createdAt};
    QJsonArray definitions;
    for (const DefinitionId id : source.definitions().definitionIds()) {
      if (!persisted(id.value()))
        return failed(ProductEnvelopeV10EncodeStatus::Invalid,
                      QStringLiteral("definition ID is outside range"));
      const auto* component = source.definitions().findComponent(id);
      if (!component)
        return failed(ProductEnvelopeV10EncodeStatus::Invalid,
                      QStringLiteral("v10 supports component definitions only"));
      const auto encodedDocument =
          detail::encodeProjectFormatV4DocumentRoot(component->document(),
                                                     nestedMetadata);
      if (!encodedDocument.succeeded())
        return failed(
            encodedDocument.status ==
                    detail::ProjectFormatV4DocumentEncodeStatus::PayloadTooLarge
                ? ProductEnvelopeV10EncodeStatus::PayloadTooLarge
                : ProductEnvelopeV10EncodeStatus::Invalid,
            encodedDocument.error);
      QJsonArray occurrences;
      for (const auto& [occurrenceId, occurrence] : component->occurrences()) {
        if (occurrence.homePlacement() != occurrence.placement())
          return failed(ProductEnvelopeV10EncodeStatus::Invalid,
                        QStringLiteral(
                            "v10 cannot encode independent home placement"));
        auto encodedOccurrence =
            detail::encodeStrictOccurrence(occurrenceId, occurrence);
        if (!encodedOccurrence)
          return failed(ProductEnvelopeV10EncodeStatus::Invalid,
                        QStringLiteral("occurrence ID is outside range"));
        occurrences.push_back(std::move(*encodedOccurrence.occurrence));
      }
      definitions.push_back(QJsonObject{
          {QStringLiteral("definitionId"), static_cast<qint64>(id.value())},
          {QStringLiteral("definitionKind"), QStringLiteral("component")},
          {QStringLiteral("payload"),
           QJsonObject{{QStringLiteral("payloadKind"),
                        QStringLiteral("embedded-component-v4")},
                       {QStringLiteral("data"), *encodedDocument.root}}},
          {QStringLiteral("occurrences"), occurrences}});
    }
    const QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("solidar-project")},
        {QStringLiteral("version"), 10},
        {QStringLiteral("capabilities"),
         QJsonArray{QStringLiteral("component-definitions-v1"),
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
      return failed(ProductEnvelopeV10EncodeStatus::PayloadTooLarge,
                    QStringLiteral("serialized v10 exceeds file budget"));
    const auto rescanned = scanProductEnvelopeV10(QByteArrayView(bytes));
    if (!rescanned.hasCandidate() || rescanned.candidate->authoredRoot != root)
      return failed(ProductEnvelopeV10EncodeStatus::SerializedPayloadValidation,
                    rescanned.error.isEmpty()
                        ? QStringLiteral("serialized v10 failed self-scan")
                        : rescanned.error);
    ProductEnvelopeV10EncodeResult result;
    result.status = ProductEnvelopeV10EncodeStatus::Valid;
    result.payload.emplace(ProductEnvelopeV10EncodedPayload{root, bytes});
    return result;
  } catch (const std::exception& exception) {
    return failed(ProductEnvelopeV10EncodeStatus::Invalid,
                  QString::fromUtf8(exception.what()));
  } catch (...) {
    return failed(ProductEnvelopeV10EncodeStatus::Invalid,
                  QStringLiteral("unknown v10 encoding failure"));
  }
}

}  // namespace solidar::project
