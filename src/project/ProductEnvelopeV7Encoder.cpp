#include "project/ProductEnvelopeV7Encoder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <exception>
#include <utility>

#include "model/AssemblyMate.h"
#include "project/ProductEnvelopeV7Decoder.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectIdCodec.h"

namespace solidar::project {
namespace {

ProductEnvelopeV7EncodeResult failure(ProductEnvelopeV7EncodeStatus status,
                                      QString error) {
  ProductEnvelopeV7EncodeResult result;
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

QString formatSha256(const SourceContentHash& hash) {
  static constexpr char kHex[] = "0123456789abcdef";
  QByteArray text(64, Qt::Uninitialized);
  for (std::size_t index = 0; index < hash.bytes.size(); ++index) {
    const auto value = hash.bytes[index];
    text[static_cast<qsizetype>(index * 2)] = kHex[value >> 4];
    text[static_cast<qsizetype>(index * 2 + 1)] = kHex[value & 0x0f];
  }
  return QString::fromLatin1(text);
}

}  // namespace

ProductEnvelopeV7EncodeResult encodeProductEnvelopeV7Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata) {
  try {
    const auto projectText = detail::formatCanonicalProjectId(outputProjectId);
    if (!projectText)
      return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                     QStringLiteral("output project ID is invalid"));
    if (source.rootDefinitionKind() != DefinitionKind::Assembly ||
        !source.rootAssemblyDefinition())
      return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                     QStringLiteral("v7 requires an assembly-root product"));
    if (!persisted(source.rootDefinitionId()))
      return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                     QStringLiteral("root definition ID is outside its range"));
    const auto ids = source.definitions().definitionIds();
    if (ids.empty() ||
        ids.size() >
            static_cast<std::size_t>(kProductEnvelopeV7MaximumDefinitions))
      return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                     QStringLiteral("definition count is outside v7 limits"));
    for (const DefinitionId id : ids) {
      const auto* assembly = source.definitions().findAssembly(id);
      if (assembly &&
          (assembly->mateRegistry().size() != 0 ||
           assembly->mateRegistry().options() != MateSolverOptions{}))
        return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                       QStringLiteral(
                           "v7 cannot discard authored mate semantics"));
    }

    const detail::ProjectFormatV4DocumentMetadata nestedMetadata{
        metadata.name, metadata.createdAt};
    QJsonArray definitions;
    for (const DefinitionId id : ids) {
      if (!persisted(id))
        return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                       QStringLiteral("definition ID is outside its range"));
      if (const auto* part = source.definitions().findPart(id)) {
        auto encoded = detail::encodeProjectFormatV4DocumentRoot(
            part->document(), nestedMetadata);
        if (!encoded.succeeded())
          return failure(
              encoded.status ==
                      detail::ProjectFormatV4DocumentEncodeStatus::PayloadTooLarge
                  ? ProductEnvelopeV7EncodeStatus::PayloadTooLarge
                  : ProductEnvelopeV7EncodeStatus::Invalid,
              encoded.error.isEmpty()
                  ? QStringLiteral("embedded v4 part encoding failed")
                  : std::move(encoded.error));
        QJsonObject payload;
        if (part->isLinked()) {
          const auto& binding = *part->linkedBinding();
          const auto linkedProjectText =
              detail::formatCanonicalProjectId(binding.key.projectId);
          if (!binding.isValid() || !linkedProjectText ||
              !persisted(binding.key.definitionId))
            return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                           QStringLiteral("linked binding is invalid"));
          const QString locator = QString::fromUtf8(
              binding.relativePathHintUtf8.data(),
              static_cast<qsizetype>(binding.relativePathHintUtf8.size()));
          payload = QJsonObject{
              {QStringLiteral("payloadKind"), QStringLiteral("linked-part-v1")},
              {QStringLiteral("binding"),
               QJsonObject{
                   {QStringLiteral("projectId"),
                    QString::fromLatin1(
                        linkedProjectText->data(),
                        static_cast<qsizetype>(linkedProjectText->size()))},
                   {QStringLiteral("definitionId"),
                    static_cast<qint64>(binding.key.definitionId.value())},
                   {QStringLiteral("relativePathHint"), locator},
                   {QStringLiteral("sha256"),
                    formatSha256(binding.acceptedHash)}}},
              {QStringLiteral("lastValid"), *encoded.root}};
        } else {
          payload = QJsonObject{
              {QStringLiteral("payloadKind"), QStringLiteral("embedded-part-v4")},
              {QStringLiteral("data"), *encoded.root}};
        }
        definitions.push_back(QJsonObject{
            {QStringLiteral("definitionId"), static_cast<qint64>(id.value())},
            {QStringLiteral("definitionKind"), QStringLiteral("part")},
            {QStringLiteral("payload"), payload}});
        continue;
      }
      const auto* assembly = source.definitions().findAssembly(id);
      if (!assembly || id != source.rootDefinitionId())
        return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                       QStringLiteral("v7 permits only the root assembly"));
      if (assembly->size() >
          static_cast<std::size_t>(kProductEnvelopeV7MaximumOccurrences))
        return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                       QStringLiteral("occurrence count exceeds v7 limits"));
      QJsonArray occurrences;
      for (const auto& [occurrenceId, occurrence] : assembly->occurrences()) {
        if (!persisted(occurrenceId) ||
            !persisted(occurrence.targetDefinitionId()) ||
            source.definitions().findPart(occurrence.targetDefinitionId()) ==
                nullptr)
          return failure(ProductEnvelopeV7EncodeStatus::Invalid,
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
            {QStringLiteral("name"),
             QString::fromUtf8(
                 occurrence.displayNameUtf8().data(),
                 static_cast<qsizetype>(occurrence.displayNameUtf8().size()))},
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
        {QStringLiteral("version"), 7},
        {QStringLiteral("capabilities"),
         QJsonArray{QStringLiteral("flat-assembly-v1"),
                    QStringLiteral("linked-parts-v1"),
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
      return failure(ProductEnvelopeV7EncodeStatus::PayloadTooLarge,
                     QStringLiteral("serialized v7 payload exceeds file limit"));
    auto scanned = scanProductEnvelopeV7(QByteArrayView(bytes));
    if (scanned.status != ProductEnvelopeV7ScanStatus::Candidate ||
        !scanned.candidate || scanned.candidate->authoredRoot != root ||
        scanned.candidate->projectId != outputProjectId ||
        scanned.candidate->rootDefinitionId != source.rootDefinitionId() ||
        scanned.candidate->metadata != metadata ||
        scanned.candidate->parts.size() + 1 != ids.size() ||
        !scanned.candidate->assembly ||
        scanned.candidate->assembly->definitionId !=
            source.rootDefinitionId() ||
        scanned.candidate->assembly->occurrences.size() !=
            source.rootAssemblyDefinition()->size())
      return failure(
          ProductEnvelopeV7EncodeStatus::SerializedPayloadValidation,
          scanned.error.isEmpty()
              ? QStringLiteral("serialized v7 payload failed self-scan")
              : std::move(scanned.error));

    std::size_t decodedPartIndex = 0;
    for (const DefinitionId id : ids) {
      if (id == source.rootDefinitionId()) continue;
      if (decodedPartIndex >= scanned.candidate->parts.size() ||
          scanned.candidate->parts[decodedPartIndex].definitionId != id)
        return failure(
            ProductEnvelopeV7EncodeStatus::SerializedPayloadValidation,
            QStringLiteral("serialized v7 part order does not match source"));
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
          decoded.flags != occurrence.flags() ||
          decoded.displayNameUtf8 != occurrence.displayNameUtf8())
        return failure(
            ProductEnvelopeV7EncodeStatus::SerializedPayloadValidation,
            QStringLiteral(
                "serialized v7 occurrence does not match source"));
    }

    ProductEnvelopeV7EncodeResult result;
    result.status = ProductEnvelopeV7EncodeStatus::Valid;
    result.payload.emplace(ProductEnvelopeV7EncodedPayload{root,
                                                           std::move(bytes)});
    return result;
  } catch (const std::exception& exception) {
    return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                   QString::fromUtf8(exception.what()));
  } catch (...) {
    return failure(ProductEnvelopeV7EncodeStatus::Invalid,
                   QStringLiteral("unknown exception while encoding v7"));
  }
}

}  // namespace solidar::project
