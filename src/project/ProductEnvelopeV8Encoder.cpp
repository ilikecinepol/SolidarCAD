#include "project/ProductEnvelopeV8Encoder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <exception>
#include <utility>

#include "model/AssemblyGraphAnalysis.h"
#include "model/AssemblyMate.h"
#include "project/ProductEnvelopeV8Decoder.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectIdCodec.h"
#include "project/internal/ProductEnvelopeV8Encoding.h"
#include "project/internal/StrictOccurrenceCodec.h"

namespace solidar::project {
namespace {

ProductEnvelopeV8EncodeResult failed(ProductEnvelopeV8EncodeStatus status,
                                     QString error) {
  ProductEnvelopeV8EncodeResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

bool persisted(std::uint64_t value) {
  return value > 0 &&
         value <= static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId);
}

QString hashText(const SourceContentHash& hash) {
  static constexpr char digits[] = "0123456789abcdef";
  QByteArray text(64, Qt::Uninitialized);
  for (std::size_t i = 0; i < hash.bytes.size(); ++i) {
    text[static_cast<qsizetype>(i * 2)] = digits[hash.bytes[i] >> 4];
    text[static_cast<qsizetype>(i * 2 + 1)] = digits[hash.bytes[i] & 15];
  }
  return QString::fromLatin1(text);
}

}  // namespace

ProductEnvelopeV8EncodeResult detail::encodeProductEnvelopeV8BaseRoot(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata, bool rejectAuthoredMates) {
  try {
    const auto projectText = detail::formatCanonicalProjectId(outputProjectId);
    if (!projectText || source.rootDefinitionKind() != DefinitionKind::Assembly)
      return failed(ProductEnvelopeV8EncodeStatus::Invalid,
                    QStringLiteral("v8 requires valid identity and assembly root"));
    const auto graph = analyzeAssemblyGraph(source.definitions(),
                                            source.rootDefinitionId());
    if (!graph)
      return failed(ProductEnvelopeV8EncodeStatus::Invalid,
                    QStringLiteral("source nested graph is invalid"));
    for (const DefinitionId id : source.definitions().definitionIds()) {
      const auto* assembly = source.definitions().findAssembly(id);
      if (rejectAuthoredMates && assembly &&
          (assembly->mateRegistry().size() != 0 ||
           assembly->mateRegistry().options() != MateSolverOptions{}))
        return failed(ProductEnvelopeV8EncodeStatus::Invalid,
                      QStringLiteral(
                          "v8 cannot discard authored mate semantics"));
    }
    const detail::ProjectFormatV4DocumentMetadata nestedMetadata{
        metadata.name, metadata.createdAt};
    QJsonArray definitions;
    for (const DefinitionId id : source.definitions().definitionIds()) {
      if (!persisted(id.value()))
        return failed(ProductEnvelopeV8EncodeStatus::Invalid,
                      QStringLiteral("definition ID is outside range"));
      if (const auto* part = source.definitions().findPart(id)) {
        const auto encoded = detail::encodeProjectFormatV4DocumentRoot(
            part->document(), nestedMetadata);
        if (!encoded.succeeded())
          return failed(
              encoded.status ==
                      detail::ProjectFormatV4DocumentEncodeStatus::PayloadTooLarge
                  ? ProductEnvelopeV8EncodeStatus::PayloadTooLarge
                  : ProductEnvelopeV8EncodeStatus::Invalid,
              encoded.error);
        QJsonObject payload;
        if (part->linkedBinding()) {
          const auto& binding = *part->linkedBinding();
          const auto external = detail::formatCanonicalProjectId(
              binding.key.projectId);
          if (!binding.isValid() || !external ||
              !persisted(binding.key.definitionId.value()))
            return failed(ProductEnvelopeV8EncodeStatus::Invalid,
                          QStringLiteral("linked binding is invalid"));
          payload = QJsonObject{
              {QStringLiteral("payloadKind"), QStringLiteral("linked-part-v1")},
              {QStringLiteral("binding"),
               QJsonObject{
                   {QStringLiteral("projectId"),
                    QString::fromLatin1(external->data(),
                                        static_cast<qsizetype>(external->size()))},
                   {QStringLiteral("definitionId"),
                    static_cast<qint64>(binding.key.definitionId.value())},
                   {QStringLiteral("relativePathHint"),
                    QString::fromUtf8(
                        binding.relativePathHintUtf8.data(),
                        static_cast<qsizetype>(
                            binding.relativePathHintUtf8.size()))},
                   {QStringLiteral("sha256"), hashText(binding.acceptedHash)}}},
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
      if (!assembly)
        return failed(ProductEnvelopeV8EncodeStatus::Invalid,
                      QStringLiteral("definition has no kind"));
      QJsonArray occurrences;
      for (const auto& [occurrenceId, occurrence] : assembly->occurrences()) {
        auto encodedOccurrence =
            detail::encodeStrictOccurrence(occurrenceId, occurrence);
        if (!encodedOccurrence)
          return failed(ProductEnvelopeV8EncodeStatus::Invalid,
                        QStringLiteral("occurrence ID is outside range"));
        occurrences.push_back(std::move(*encodedOccurrence.occurrence));
      }
      definitions.push_back(QJsonObject{
          {QStringLiteral("definitionId"), static_cast<qint64>(id.value())},
          {QStringLiteral("definitionKind"), QStringLiteral("assembly")},
          {QStringLiteral("occurrences"), occurrences}});
    }
    const QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("solidar-project")},
        {QStringLiteral("version"), 8},
        {QStringLiteral("capabilities"),
         QJsonArray{QStringLiteral("linked-parts-v1"),
                    QStringLiteral("nested-assemblies-v1"),
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
      return failed(ProductEnvelopeV8EncodeStatus::PayloadTooLarge,
                    QStringLiteral("serialized v8 exceeds file budget"));
    const auto rescanned = scanProductEnvelopeV8(QByteArrayView(bytes));
    if (!rescanned.hasCandidate() || rescanned.candidate->authoredRoot != root)
      return failed(ProductEnvelopeV8EncodeStatus::SerializedPayloadValidation,
                    rescanned.error.isEmpty()
                        ? QStringLiteral("serialized v8 failed self-scan")
                        : rescanned.error);
    ProductEnvelopeV8EncodeResult result;
    result.status = ProductEnvelopeV8EncodeStatus::Valid;
    result.payload.emplace(ProductEnvelopeV8EncodedPayload{root, bytes});
    return result;
  } catch (const std::exception& exception) {
    return failed(ProductEnvelopeV8EncodeStatus::Invalid,
                  QString::fromUtf8(exception.what()));
  } catch (...) {
    return failed(ProductEnvelopeV8EncodeStatus::Invalid,
                  QStringLiteral("unknown v8 encoding failure"));
  }
}

ProductEnvelopeV8EncodeResult encodeProductEnvelopeV8Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata) {
  return detail::encodeProductEnvelopeV8BaseRoot(
      source, outputProjectId, metadata, true);
}

}  // namespace solidar::project
