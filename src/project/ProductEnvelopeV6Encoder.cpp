#include "project/ProductEnvelopeV6Encoder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <exception>
#include <utility>

#include "project/ProductEnvelopeV6Decoder.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectIdCodec.h"

namespace solidar::project {
namespace {

ProductEnvelopeV6EncodeResult failure(ProductEnvelopeV6EncodeStatus status,
                                      QString error) {
  ProductEnvelopeV6EncodeResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

bool persisted(DefinitionId id) {
  return id.isValid() &&
         id.value() <=
             static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId);
}

bool persisted(OccurrenceId id) {
  return id.isValid() &&
         id.value() <=
             static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId);
}

QJsonArray numbers(const std::array<double, 3>& values) {
  return {values[0], values[1], values[2]};
}

QJsonArray numbers(const std::array<double, 4>& values) {
  return {values[0], values[1], values[2], values[3]};
}

}  // namespace

ProductEnvelopeV6EncodeResult encodeProductEnvelopeV6Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata) {
  try {
    const auto projectText = detail::formatCanonicalProjectId(outputProjectId);
    if (!projectText)
      return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                     QStringLiteral("output project ID is invalid"));
    if (source.rootDefinitionKind() != DefinitionKind::Assembly ||
        !source.rootAssemblyDefinition())
      return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                     QStringLiteral("v6 requires an assembly-root product"));
    if (!persisted(source.rootDefinitionId()))
      return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                     QStringLiteral("root definition ID is outside its range"));
    const auto ids = source.definitions().definitionIds();
    if (ids.empty() ||
        ids.size() >
            static_cast<std::size_t>(kProductEnvelopeV6MaximumDefinitions))
      return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                     QStringLiteral("definition count is outside v6 limits"));
    for (const DefinitionId id : ids) {
      const auto* part = source.definitions().findPart(id);
      if (part && part->isLinked())
        return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                       QStringLiteral(
                           "v6 cannot persist linked part definitions"));
    }

    const detail::ProjectFormatV4DocumentMetadata nestedMetadata{
        metadata.name, metadata.createdAt};
    QJsonArray definitions;
    for (const DefinitionId id : ids) {
      if (!persisted(id))
        return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                       QStringLiteral("definition ID is outside its range"));
      if (const auto* part = source.definitions().findPart(id)) {
        auto encoded = detail::encodeProjectFormatV4DocumentRoot(
            part->document(), nestedMetadata);
        if (!encoded.succeeded())
          return failure(
              encoded.status ==
                      detail::ProjectFormatV4DocumentEncodeStatus::PayloadTooLarge
                  ? ProductEnvelopeV6EncodeStatus::PayloadTooLarge
                  : ProductEnvelopeV6EncodeStatus::Invalid,
              encoded.error.isEmpty()
                  ? QStringLiteral("embedded v4 part encoding failed")
                  : std::move(encoded.error));
        definitions.push_back(QJsonObject{
            {QStringLiteral("definitionId"), static_cast<qint64>(id.value())},
            {QStringLiteral("definitionKind"), QStringLiteral("part")},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("payloadKind"),
                          QStringLiteral("embedded-part-v4")},
                         {QStringLiteral("data"), *encoded.root}}}});
        continue;
      }
      const auto* assembly = source.definitions().findAssembly(id);
      if (!assembly || id != source.rootDefinitionId())
        return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                       QStringLiteral("v6 permits only the root assembly"));
      if (assembly->size() >
          static_cast<std::size_t>(kProductEnvelopeV6MaximumOccurrences))
        return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                       QStringLiteral("occurrence count exceeds v6 limits"));
      QJsonArray occurrences;
      for (const auto& [occurrenceId, occurrence] : assembly->occurrences()) {
        if (!occurrence.displayNameUtf8().empty())
          return failure(
              ProductEnvelopeV6EncodeStatus::UnsupportedAuthoredOccurrenceName,
              QStringLiteral(
                  "v6 cannot persist authored occurrence display names"));
        if (!persisted(occurrenceId) ||
            !persisted(occurrence.targetDefinitionId()) ||
            source.definitions().findPart(occurrence.targetDefinitionId()) ==
                nullptr)
          return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                         QStringLiteral("occurrence identity is invalid"));
        const auto& transform = occurrence.placement();
        const auto flags = occurrence.flags();
        occurrences.push_back(QJsonObject{
            {QStringLiteral("occurrenceId"),
             static_cast<qint64>(occurrenceId.value())},
            {QStringLiteral("targetDefinitionId"),
             static_cast<qint64>(occurrence.targetDefinitionId().value())},
            {QStringLiteral("placement"),
             QJsonObject{
                 {QStringLiteral("translationMm"),
                  numbers(transform.translationMm())},
                 {QStringLiteral("quaternionWxyz"),
                  numbers(transform.quaternionWxyz())}}},
            {QStringLiteral("visible"), flags.visible},
            {QStringLiteral("suppressed"), flags.suppressed},
            {QStringLiteral("grounded"), flags.grounded}});
      }
      definitions.push_back(QJsonObject{
          {QStringLiteral("definitionId"), static_cast<qint64>(id.value())},
          {QStringLiteral("definitionKind"), QStringLiteral("assembly")},
          {QStringLiteral("occurrences"), occurrences}});
    }

    const QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("solidar-project")},
        {QStringLiteral("version"), 6},
        {QStringLiteral("capabilities"),
         QJsonArray{QStringLiteral("flat-assembly-v1"),
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
    QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > ProjectFile::kMaximumFileBytes)
      return failure(ProductEnvelopeV6EncodeStatus::PayloadTooLarge,
                     QStringLiteral("serialized v6 payload exceeds file limit"));
    auto scanned = scanProductEnvelopeV6(QByteArrayView(bytes));
    if (scanned.status != ProductEnvelopeV6ScanStatus::Candidate ||
        !scanned.candidate || scanned.candidate->authoredRoot != root ||
        scanned.candidate->projectId != outputProjectId ||
        scanned.candidate->rootDefinitionId != source.rootDefinitionId() ||
        scanned.candidate->metadata != metadata ||
        !scanned.candidate->deferredUnknowns.empty() ||
        scanned.candidate->parts.size() + 1 != ids.size() ||
        !scanned.candidate->assembly ||
        scanned.candidate->assembly->definitionId !=
            source.rootDefinitionId() ||
        scanned.candidate->assembly->occurrences.size() !=
            source.rootAssemblyDefinition()->size())
      return failure(
          ProductEnvelopeV6EncodeStatus::SerializedPayloadValidation,
          scanned.error.isEmpty()
              ? QStringLiteral("serialized v6 payload failed self-scan")
              : std::move(scanned.error));

    std::size_t decodedPartIndex = 0;
    for (const DefinitionId id : ids) {
      if (id == source.rootDefinitionId()) continue;
      if (decodedPartIndex >= scanned.candidate->parts.size() ||
          scanned.candidate->parts[decodedPartIndex].definitionId != id)
        return failure(
            ProductEnvelopeV6EncodeStatus::SerializedPayloadValidation,
            QStringLiteral("serialized v6 part order does not match source"));
      ++decodedPartIndex;
    }
    std::size_t decodedOccurrenceIndex = 0;
    for (const auto& [id, occurrence] :
         source.rootAssemblyDefinition()->occurrences()) {
      const auto& decoded =
          scanned.candidate->assembly->occurrences[decodedOccurrenceIndex++];
      if (decoded.occurrenceId != id ||
          decoded.targetDefinitionId != occurrence.targetDefinitionId() ||
          decoded.placement != occurrence.placement() ||
          decoded.flags != occurrence.flags())
        return failure(
            ProductEnvelopeV6EncodeStatus::SerializedPayloadValidation,
            QStringLiteral(
                "serialized v6 occurrence does not match source"));
    }

    ProductEnvelopeV6EncodeResult result;
    result.status = ProductEnvelopeV6EncodeStatus::Valid;
    result.payload.emplace(ProductEnvelopeV6EncodedPayload{root,
                                                           std::move(bytes)});
    return result;
  } catch (const std::exception& exception) {
    return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                   QString::fromUtf8(exception.what()));
  } catch (...) {
    return failure(ProductEnvelopeV6EncodeStatus::Invalid,
                   QStringLiteral("unknown exception while encoding v6"));
  }
}

}  // namespace solidar::project
