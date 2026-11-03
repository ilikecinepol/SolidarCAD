#include "project/ProductEnvelopeV12Encoder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <exception>
#include <array>
#include <optional>
#include <utility>

#include "model/ComponentGraphAnalysis.h"
#include "project/ProductEnvelopeV12Decoder.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectIdCodec.h"

namespace solidar::project {
namespace {

ProductEnvelopeV12EncodeResult failed(ProductEnvelopeV12EncodeStatus status,
                                      QString error) {
  ProductEnvelopeV12EncodeResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

bool persisted(std::uint64_t value) {
  return value > 0 &&
         value <= static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId);
}

QJsonArray numbers(const std::array<double, 3>& value) {
  return {value[0], value[1], value[2]};
}

QJsonArray numbers(const std::array<double, 4>& value) {
  return {value[0], value[1], value[2], value[3]};
}

std::optional<QJsonObject> encodeV12Occurrence(
    OccurrenceId occurrenceId, const ComponentOccurrence& occurrence) {
  if (!persisted(occurrenceId.value()) ||
      !persisted(occurrence.targetDefinitionId().value()))
    return std::nullopt;
  const auto transform = [](const RigidTransform3d& value) {
    return QJsonObject{
        {QStringLiteral("translationMm"), numbers(value.translationMm())},
        {QStringLiteral("quaternionWxyz"),
         numbers(value.quaternionWxyz())}};
  };
  const auto flags = occurrence.flags();
  return QJsonObject{
      {QStringLiteral("occurrenceId"),
       static_cast<qint64>(occurrenceId.value())},
      {QStringLiteral("targetDefinitionId"),
       static_cast<qint64>(occurrence.targetDefinitionId().value())},
      {QStringLiteral("placement"), transform(occurrence.placement())},
      {QStringLiteral("homePlacement"),
       transform(occurrence.homePlacement())},
      {QStringLiteral("name"),
       QString::fromUtf8(
           occurrence.displayNameUtf8().data(),
           static_cast<qsizetype>(occurrence.displayNameUtf8().size()))},
      {QStringLiteral("visible"), flags.visible},
      {QStringLiteral("suppressed"), flags.suppressed},
      {QStringLiteral("grounded"), flags.grounded}};
}

}  // namespace

ProductEnvelopeV12EncodeResult encodeProductEnvelopeV12Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata) {
  try {
    const auto projectText = detail::formatCanonicalProjectId(outputProjectId);
    if (!projectText ||
        source.rootDefinitionKind() != DefinitionKind::Component)
      return failed(ProductEnvelopeV12EncodeStatus::Invalid,
                    QStringLiteral(
                        "v12 requires valid identity and component root"));
    if (analyzeComponentGraph(source.definitions(),
                              source.rootDefinitionId()) !=
        ComponentGraphStatus::Valid)
      return failed(ProductEnvelopeV12EncodeStatus::Invalid,
                    QStringLiteral("source component graph is invalid"));

    const detail::ProjectFormatV4DocumentMetadata nestedMetadata{
        metadata.name, metadata.createdAt};
    QJsonArray definitions;
    for (const DefinitionId id : source.definitions().definitionIds()) {
      if (!persisted(id.value()))
        return failed(ProductEnvelopeV12EncodeStatus::Invalid,
                      QStringLiteral("definition ID is outside range"));
      const auto* component = source.definitions().findComponent(id);
      if (!component)
        return failed(ProductEnvelopeV12EncodeStatus::Invalid,
                      QStringLiteral("v12 supports component definitions only"));
      const auto encodedDocument =
          detail::encodeProjectFormatV4DocumentRoot(component->document(),
                                                     nestedMetadata);
      if (!encodedDocument.succeeded())
        return failed(
            encodedDocument.status ==
                    detail::ProjectFormatV4DocumentEncodeStatus::PayloadTooLarge
                ? ProductEnvelopeV12EncodeStatus::PayloadTooLarge
                : ProductEnvelopeV12EncodeStatus::Invalid,
            encodedDocument.error);
      QJsonArray occurrences;
      for (const auto& [occurrenceId, occurrence] : component->occurrences()) {
        auto encodedOccurrence =
            encodeV12Occurrence(occurrenceId, occurrence);
        if (!encodedOccurrence)
          return failed(ProductEnvelopeV12EncodeStatus::Invalid,
                        QStringLiteral("occurrence ID is outside range"));
        occurrences.push_back(std::move(*encodedOccurrence));
      }
      QJsonArray drawings;
      for (const auto& drawing : component->drawings()) {
        if (!persisted(drawing.id().value()))
          return failed(ProductEnvelopeV12EncodeStatus::Invalid,
                        QStringLiteral("drawing ID is outside range"));
        QJsonArray sourceBodyIds;
        for (const BodyId bodyId : drawing.sourceBodyIds()) {
          if (!persisted(bodyId))
            return failed(ProductEnvelopeV12EncodeStatus::Invalid,
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
        {QStringLiteral("version"), 12},
        {QStringLiteral("capabilities"),
         QJsonArray{QStringLiteral("component-definitions-v1"),
                    QStringLiteral("component-drawings-v1"),
                    QStringLiteral("component-home-placement-v1"),
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
      return failed(ProductEnvelopeV12EncodeStatus::PayloadTooLarge,
                    QStringLiteral("serialized v12 exceeds file budget"));
    const auto rescanned = scanProductEnvelopeV12(QByteArrayView(bytes));
    if (!rescanned.hasCandidate() || rescanned.candidate->authoredRoot != root)
      return failed(ProductEnvelopeV12EncodeStatus::SerializedPayloadValidation,
                    rescanned.error.isEmpty()
                        ? QStringLiteral("serialized v12 failed self-scan")
                        : rescanned.error);
    ProductEnvelopeV12EncodeResult result;
    result.status = ProductEnvelopeV12EncodeStatus::Valid;
    result.payload.emplace(ProductEnvelopeV12EncodedPayload{root, bytes});
    return result;
  } catch (const std::exception& exception) {
    return failed(ProductEnvelopeV12EncodeStatus::Invalid,
                  QString::fromUtf8(exception.what()));
  } catch (...) {
    return failed(ProductEnvelopeV12EncodeStatus::Invalid,
                  QStringLiteral("unknown v12 encoding failure"));
  }
}

}  // namespace solidar::project
