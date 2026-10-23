#include "project/ProductEnvelopeV5Encoder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <exception>
#include <utility>

#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectIdCodec.h"

namespace solidar::project {
namespace {

ProductEnvelopeV5EncodeResult failure(ProductEnvelopeV5EncodeStatus status,
                                      QString error) {
  ProductEnvelopeV5EncodeResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

QString exceptionError(const std::exception& exception) {
  return QString::fromUtf8(exception.what());
}

}  // namespace

ProductEnvelopeV5EncodeResult encodeProductEnvelopeV5Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata) {
  return encodeProductEnvelopeV5Root(source, outputProjectId,
                                     source.rootDefinitionId(), metadata);
}

ProductEnvelopeV5EncodeResult encodeProductEnvelopeV5Root(
    const ProductDocument& source, ProjectId outputProjectId,
    DefinitionId outputRootDefinitionId,
    const ProductEnvelopeV5Metadata& metadata) {
  try {
    const auto encodedProjectId =
        detail::formatCanonicalProjectId(outputProjectId);
    if (!encodedProjectId) {
      return failure(ProductEnvelopeV5EncodeStatus::Invalid,
                     QStringLiteral("output project ID is invalid"));
    }
    if (source.definitions().size() != 1) {
      return failure(ProductEnvelopeV5EncodeStatus::Invalid,
                     QStringLiteral(
                         "product envelope v5 requires exactly one definition"));
    }
    const std::uint64_t definitionId = outputRootDefinitionId.value();
    if (definitionId == 0 ||
        definitionId > static_cast<std::uint64_t>(
                           ProjectFile::kMaximumPersistedId)) {
      return failure(ProductEnvelopeV5EncodeStatus::Invalid,
                     QStringLiteral(
                         "root definition ID is outside its persisted range"));
    }

    const detail::ProjectFormatV4DocumentMetadata nestedMetadata{
        metadata.name, metadata.createdAt};
    auto nested = detail::encodeProjectFormatV4DocumentRoot(
        source.rootDocument(), nestedMetadata);
    if (!nested.succeeded()) {
      return failure(
          nested.status ==
                  detail::ProjectFormatV4DocumentEncodeStatus::PayloadTooLarge
              ? ProductEnvelopeV5EncodeStatus::PayloadTooLarge
              : ProductEnvelopeV5EncodeStatus::Invalid,
          nested.error.isEmpty()
              ? QStringLiteral("embedded v4 part encoding failed")
              : std::move(nested.error));
    }

    const QJsonObject payload{
        {QStringLiteral("payloadKind"),
         QStringLiteral("embedded-part-v4")},
        {QStringLiteral("data"), *nested.root}};
    const QJsonObject definition{
        {QStringLiteral("definitionId"), static_cast<qint64>(definitionId)},
        {QStringLiteral("definitionKind"), QStringLiteral("part")},
        {QStringLiteral("payload"), payload}};
    const QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("solidar-project")},
        {QStringLiteral("version"), 5},
        {QStringLiteral("capabilities"),
         QJsonArray{QStringLiteral("product-envelope-v1")}},
        {QStringLiteral("projectId"),
         QString::fromLatin1(encodedProjectId->data(),
                             static_cast<qsizetype>(encodedProjectId->size()))},
        {QStringLiteral("rootDefinitionId"),
         static_cast<qint64>(definitionId)},
        {QStringLiteral("metadata"),
         QJsonObject{{QStringLiteral("name"), metadata.name},
                     {QStringLiteral("createdAt"), metadata.createdAt}}},
        {QStringLiteral("definitions"), QJsonArray{definition}}};

    // This is the sole serialization. The exact resulting bytes are then
    // bounded and fed back through the disconnected v5 scanner.
    QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > ProjectFile::kMaximumFileBytes) {
      return failure(ProductEnvelopeV5EncodeStatus::PayloadTooLarge,
                     QStringLiteral("serialized v5 payload exceeds %1 bytes")
                         .arg(ProjectFile::kMaximumFileBytes));
    }

    auto scanned = scanProductEnvelopeV5(QByteArrayView(bytes));
    if (scanned.status != ProductEnvelopeV5ScanStatus::Candidate ||
        !scanned.candidate || scanned.candidate->authoredRoot != root ||
        scanned.candidate->projectId != outputProjectId ||
        scanned.candidate->rootDefinitionId != outputRootDefinitionId ||
        scanned.candidate->definitionId != outputRootDefinitionId ||
        scanned.candidate->metadata != metadata ||
        scanned.candidate->capabilities !=
            QStringList{QStringLiteral("product-envelope-v1")} ||
        !scanned.candidate->embeddedPart ||
        !scanned.candidate->deferredUnknowns.empty() ||
        scanned.candidate->embeddedPart->legacySketch.topology.payload.envelope
                .authoredRoot != *nested.root) {
      return failure(
          ProductEnvelopeV5EncodeStatus::SerializedPayloadValidation,
          scanned.error.isEmpty()
              ? QStringLiteral(
                    "serialized v5 payload does not match the encoded root")
              : std::move(scanned.error));
    }

    ProductEnvelopeV5EncodeResult result;
    result.status = ProductEnvelopeV5EncodeStatus::Valid;
    result.payload.emplace(
        ProductEnvelopeV5EncodedPayload{root, std::move(bytes)});
    return result;
  } catch (const std::exception& exception) {
    return failure(ProductEnvelopeV5EncodeStatus::Invalid,
                   exceptionError(exception));
  } catch (...) {
    return failure(ProductEnvelopeV5EncodeStatus::Invalid,
                   QStringLiteral(
                       "unknown exception while encoding v5 payload"));
  }
}

}  // namespace solidar::project
