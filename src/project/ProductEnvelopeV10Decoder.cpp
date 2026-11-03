#include "project/ProductEnvelopeV10Decoder.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "model/AssemblyGraphAnalysis.h"
#include "project/ProjectFile.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/internal/ProjectIdCodec.h"
#include "project/internal/StrictOccurrenceCodec.h"

namespace solidar::project {
namespace {

constexpr int kMaximumJsonDepth = 64;
constexpr qsizetype kMaximumCapabilityUtf8Bytes = 64;

ProductEnvelopeV10ScanResult fail(ProductEnvelopeV10ScanStatus status,
                                  QString error) {
  ProductEnvelopeV10ScanResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

ProductEnvelopeV10ScanResult invalid(QString error) {
  return fail(ProductEnvelopeV10ScanStatus::Invalid, std::move(error));
}

bool listed(std::initializer_list<const char*> fields, const QString& key) {
  return std::any_of(fields.begin(), fields.end(), [&](const char* field) {
    return key == QLatin1String(field);
  });
}

bool exactFields(const QJsonObject& object,
                 std::initializer_list<const char*> fields,
                 const QString& path, QString* error) {
  for (const char* field : fields) {
    if (!object.contains(QLatin1String(field))) {
      *error = QStringLiteral("%1 is missing %2")
                   .arg(path, QString::fromLatin1(field));
      return false;
    }
  }
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!listed(fields, it.key())) {
      *error = QStringLiteral("%1 contains unexpected field %2")
                   .arg(path, it.key());
      return false;
    }
  }
  return object.size() == static_cast<qsizetype>(fields.size());
}

bool validUtf16(const QString& value) {
  for (qsizetype index = 0; index < value.size(); ++index) {
    if (value[index].isHighSurrogate()) {
      if (++index >= value.size() || !value[index].isLowSurrogate())
        return false;
    } else if (value[index].isLowSurrogate()) {
      return false;
    }
  }
  return true;
}

bool boundedString(const QJsonValue& value, bool allowEmpty, QString* output,
                   const QString& path, QString* error) {
  if (!value.isString()) {
    *error = path + QStringLiteral(" must be a string");
    return false;
  }
  const QString text = value.toString();
  if ((!allowEmpty && text.isEmpty()) || !validUtf16(text) ||
      text.size() > ProjectFile::kMaximumStringCharacters) {
    *error = path + QStringLiteral(" is invalid");
    return false;
  }
  *output = text;
  return true;
}

bool integer(const QJsonValue& value, qint64 minimum, qint64 maximum,
             qint64* output, const QString& path, QString* error) {
  if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
      std::trunc(value.toDouble()) != value.toDouble() ||
      value.toDouble() < static_cast<double>(minimum) ||
      value.toDouble() > static_cast<double>(maximum)) {
    *error = path + QStringLiteral(" must be an exact bounded integer");
    return false;
  }
  *output = static_cast<qint64>(value.toDouble());
  return true;
}

bool persistedId(const QJsonValue& value, qint64* output,
                 const QString& path, QString* error) {
  return integer(value, 1, ProjectFile::kMaximumPersistedId, output, path,
                 error);
}

bool accountStrings(const QJsonValue& value, int depth, qsizetype* total) {
  if (depth > kMaximumJsonDepth) return false;
  if (value.isString()) {
    const QString text = value.toString();
    const qsizetype bytes = text.toUtf8().size();
    if (!validUtf16(text) || *total < 0 || bytes < 0 ||
        bytes > kProductEnvelopeV10MaximumJsonStringUtf8Bytes - *total)
      return false;
    *total += bytes;
  } else if (value.isArray()) {
    for (const auto& item : value.toArray())
      if (!accountStrings(item, depth + 1, total)) return false;
  } else if (value.isObject()) {
    const auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
      const qsizetype keyBytes = it.key().toUtf8().size();
      if (!validUtf16(it.key()) || keyBytes < 0 ||
          keyBytes > kProductEnvelopeV10MaximumJsonStringUtf8Bytes - *total)
        return false;
      *total += keyBytes;
      if (!accountStrings(it.value(), depth + 1, total)) return false;
    }
  }
  return true;
}

bool byteLess(const QByteArray& left, const QByteArray& right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](char a, char b) {
        return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
      });
}

template <class T>
bool addBudget(T delta, T limit, T* total) {
  if (!total || delta > limit || *total > limit - delta) return false;
  *total += delta;
  return true;
}

std::optional<ProjectFormatV4ModelCandidate> scanEmbeddedDocument(
    const QJsonObject& data, const ProductEnvelopeV5Metadata& metadata,
    const QString& path, QString* error) {
  if (data.value(QStringLiteral("name")) != metadata.name ||
      data.value(QStringLiteral("createdAt")) != metadata.createdAt) {
    *error = path + QStringLiteral(" metadata must match product metadata");
    return std::nullopt;
  }
  auto envelope = detail::validateProjectFormatV4EnvelopeRoot(data);
  if (!envelope.hasCandidate()) {
    *error = QStringLiteral("invalid component v4 payload: %1")
                 .arg(envelope.error);
    return std::nullopt;
  }
  auto payload = scanProjectFormatV4Payload(std::move(*envelope.candidate));
  if (!payload.hasCandidate()) {
    *error = QStringLiteral("invalid component v4 payload: %1")
                 .arg(payload.error);
    return std::nullopt;
  }
  auto model = validateProjectFormatV4Model(std::move(*payload.candidate));
  if (model.status != ProjectFormatV4ModelValidationStatus::Candidate ||
      !model.candidate) {
    *error = model.error.isEmpty()
                 ? QStringLiteral("unsupported component v4 payload")
                 : QStringLiteral("invalid component v4 payload: %1")
                       .arg(model.error);
    return std::nullopt;
  }
  return std::move(*model.candidate);
}

}  // namespace

ProductEnvelopeV10ScanResult scanProductEnvelopeV10(QByteArrayView bytes) {
  try {
    if (bytes.isEmpty() || bytes.size() > ProjectFile::kMaximumFileBytes)
      return invalid(QStringLiteral("v10 input size is invalid"));
    QJsonParseError parseError;
    const auto parsed = QJsonDocument::fromJson(
        QByteArray(bytes.data(), bytes.size()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
      return invalid(QStringLiteral("v10 root is not valid JSON object"));
    const QJsonObject root = parsed.object();
    QString error;
    QString format;
    qint64 version = 0;
    if (!boundedString(root.value(QStringLiteral("format")), false, &format,
                       QStringLiteral("$.format"), &error) ||
        !integer(root.value(QStringLiteral("version")), 1,
                 9007199254740991LL, &version, QStringLiteral("$.version"),
                 &error))
      return invalid(std::move(error));
    if (format != QStringLiteral("solidar-project") || version < 10)
      return fail(ProductEnvelopeV10ScanStatus::NotApplicable,
                  QStringLiteral("not a v10 product envelope"));
    if (version > 10)
      return fail(ProductEnvelopeV10ScanStatus::Unsupported,
                  QStringLiteral("newer product envelope"));
    QString rawError;
    if (!detail::validateStrictOccurrenceRawTokens(bytes, &rawError))
      return invalid(std::move(rawError));
    if (!exactFields(root,
                     {"format", "version", "capabilities", "projectId",
                      "rootDefinitionId", "metadata", "definitions"},
                     QStringLiteral("$"), &error))
      return invalid(std::move(error));

    qsizetype stringBytes = 0;
    if (!accountStrings(root, 0, &stringBytes))
      return invalid(QStringLiteral("v10 JSON string/depth budget exceeded"));

    QString projectText;
    qint64 rootIdValue = 0;
    if (!boundedString(root.value(QStringLiteral("projectId")), false,
                       &projectText, QStringLiteral("$.projectId"), &error) ||
        !persistedId(root.value(QStringLiteral("rootDefinitionId")),
                     &rootIdValue, QStringLiteral("$.rootDefinitionId"),
                     &error))
      return invalid(std::move(error));
    const QByteArray projectBytes = projectText.toUtf8();
    const auto projectId = detail::parseCanonicalProjectId(
        {projectBytes.constData(),
         static_cast<std::size_t>(projectBytes.size())});
    if (!projectId) return invalid(QStringLiteral("projectId is not canonical"));

    const auto metadataValue = root.value(QStringLiteral("metadata"));
    if (!metadataValue.isObject() ||
        !exactFields(metadataValue.toObject(), {"name", "createdAt"},
                     QStringLiteral("$.metadata"), &error))
      return invalid(error.isEmpty() ? QStringLiteral("metadata is invalid")
                                     : std::move(error));
    ProductEnvelopeV5Metadata metadata;
    if (!boundedString(metadataValue.toObject().value(QStringLiteral("name")),
                       true, &metadata.name, QStringLiteral("$.metadata.name"),
                       &error) ||
        !boundedString(
            metadataValue.toObject().value(QStringLiteral("createdAt")), true,
            &metadata.createdAt, QStringLiteral("$.metadata.createdAt"),
            &error))
      return invalid(std::move(error));

    const auto capabilitiesValue = root.value(QStringLiteral("capabilities"));
    if (!capabilitiesValue.isArray() || capabilitiesValue.toArray().size() != 3)
      return invalid(QStringLiteral("v10 requires exactly three capabilities"));
    const QStringList expected{
        QStringLiteral("component-definitions-v1"),
        QStringLiteral("nested-components-v1"),
        QStringLiteral("product-envelope-v1")};
    QStringList capabilities;
    QByteArray previous;
    for (qsizetype index = 0; index < expected.size(); ++index) {
      QString capability;
      if (!boundedString(capabilitiesValue.toArray().at(index), false,
                         &capability,
                         QStringLiteral("$.capabilities[%1]").arg(index),
                         &error))
        return invalid(std::move(error));
      const QByteArray encoded = capability.toUtf8();
      if (encoded.size() > kMaximumCapabilityUtf8Bytes ||
          (index > 0 && !byteLess(previous, encoded)) ||
          capability != expected[index])
        return invalid(QStringLiteral("v10 capability set/order is invalid"));
      previous = encoded;
      capabilities.push_back(capability);
    }

    const auto definitionsValue = root.value(QStringLiteral("definitions"));
    if (!definitionsValue.isArray() || definitionsValue.toArray().isEmpty() ||
        definitionsValue.toArray().size() >
            static_cast<qsizetype>(kAssemblyGraphMaximumDefinitions))
      return invalid(QStringLiteral("definition count exceeds v10 budget"));

    ProductEnvelopeV10Candidate candidate;
    candidate.authoredRoot = root;
    candidate.projectId = *projectId;
    candidate.rootDefinitionId =
        DefinitionId{static_cast<std::uint64_t>(rootIdValue)};
    candidate.metadata = metadata;
    candidate.capabilities = capabilities;
    candidate.graph.rootDefinitionId = candidate.rootDefinitionId;
    candidate.jsonStringUtf8Bytes = stringBytes;
    std::size_t occurrenceTotal = 0;
    qint64 previousDefinitionId = 0;
    const QJsonArray definitions = definitionsValue.toArray();
    candidate.definitions.reserve(static_cast<std::size_t>(definitions.size()));
    for (qsizetype index = 0; index < definitions.size(); ++index) {
      const QString path = QStringLiteral("$.definitions[%1]").arg(index);
      if (!definitions.at(index).isObject())
        return invalid(path + QStringLiteral(" must be an object"));
      const QJsonObject definition = definitions.at(index).toObject();
      if (!exactFields(definition,
                       {"definitionId", "definitionKind", "payload",
                        "occurrences"},
                       path, &error))
        return invalid(std::move(error));
      qint64 idValue = 0;
      QString kind;
      if (!persistedId(definition.value(QStringLiteral("definitionId")),
                       &idValue, path + QStringLiteral(".definitionId"),
                       &error) ||
          !boundedString(definition.value(QStringLiteral("definitionKind")),
                         false, &kind,
                         path + QStringLiteral(".definitionKind"), &error))
        return invalid(std::move(error));
      if (idValue <= previousDefinitionId)
        return invalid(QStringLiteral("definitions must be strictly sorted"));
      previousDefinitionId = idValue;
      if (kind != QStringLiteral("component"))
        return invalid(path + QStringLiteral(" has invalid definition kind"));

      const auto payloadValue = definition.value(QStringLiteral("payload"));
      if (!payloadValue.isObject() ||
          !exactFields(payloadValue.toObject(), {"payloadKind", "data"},
                       path + QStringLiteral(".payload"), &error))
        return invalid(error.isEmpty()
                           ? path + QStringLiteral(" payload invalid")
                           : std::move(error));
      const QJsonObject payload = payloadValue.toObject();
      QString payloadKind;
      if (!boundedString(payload.value(QStringLiteral("payloadKind")), false,
                         &payloadKind,
                         path + QStringLiteral(".payload.payloadKind"),
                         &error) ||
          payloadKind != QStringLiteral("embedded-component-v4"))
        return invalid(error.isEmpty()
                           ? path + QStringLiteral(" payload kind invalid")
                           : std::move(error));
      if (!payload.value(QStringLiteral("data")).isObject())
        return invalid(path + QStringLiteral(" payload data invalid"));
      const QJsonObject data =
          payload.value(QStringLiteral("data")).toObject();
      const qsizetype payloadBytes =
          QJsonDocument(data).toJson(QJsonDocument::Compact).size();
      if (!addBudget(payloadBytes,
                     kProductEnvelopeV10MaximumEmbeddedPayloadBytes,
                     &candidate.embeddedPayloadBytes))
        return invalid(QStringLiteral("embedded payload budget exceeded"));
      auto model = scanEmbeddedDocument(
          data, metadata, path + QStringLiteral(".payload.data"), &error);
      if (!model) return invalid(std::move(error));
      const auto& topology = model->legacySketch.topology;
      const auto& scannedPayload = topology.payload;
      if (!addBudget(topology.topologyReferences.size(),
                     kProductEnvelopeV10MaximumTopologyReferences,
                     &candidate.topologyReferenceCount) ||
          !addBudget(topology.provenanceRecordCount,
                     kProductEnvelopeV10MaximumProvenanceRecords,
                     &candidate.provenanceRecordCount) ||
          !addBudget(topology.localNameUtf8Bytes,
                     kProductEnvelopeV10MaximumLocalNameUtf8Bytes,
                     &candidate.localNameUtf8Bytes) ||
          !addBudget(scannedPayload.rebuildWork,
                     kProductEnvelopeV10MaximumRebuildWork,
                     &candidate.rebuildWork))
        return invalid(QStringLiteral("aggregate component payload budget exceeded"));

      auto decoded = detail::decodeStrictOccurrences(
          definition.value(QStringLiteral("occurrences")), path,
          occurrenceTotal, kAssemblyGraphMaximumAuthoredOccurrences);
      if (!decoded) return invalid(std::move(decoded.error));
      occurrenceTotal = decoded.aggregateAuthoredOccurrences;
      const DefinitionId id{static_cast<std::uint64_t>(idValue)};
      ComponentGraphDefinition graphDefinition;
      graphDefinition.occurrences.reserve(decoded.occurrences.size());
      for (const auto& occurrence : decoded.occurrences)
        graphDefinition.occurrences.push_back(
            {occurrence.occurrenceId, occurrence.targetDefinitionId});
      if (!candidate.graph.definitions.emplace(
              id, std::move(graphDefinition)).second)
        return invalid(QStringLiteral("duplicate component definition ID"));
      candidate.definitions.push_back(
          {id, definition, payload, std::move(*model),
           std::move(decoded.occurrences)});
    }
    const auto graphStatus = analyzeComponentGraph(candidate.graph);
    if (graphStatus != ComponentGraphStatus::Valid)
      return invalid(QStringLiteral("component graph rejected (%1)")
                         .arg(static_cast<int>(graphStatus)));

    ProductEnvelopeV10ScanResult result;
    result.status = ProductEnvelopeV10ScanStatus::Candidate;
    result.candidate = std::move(candidate);
    return result;
  } catch (const std::exception& exception) {
    return invalid(QStringLiteral("v10 scan failure: %1")
                       .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return invalid(QStringLiteral("unknown v10 scan failure"));
  }
}

}  // namespace solidar::project
