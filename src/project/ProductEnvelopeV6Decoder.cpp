#include "project/ProductEnvelopeV6Decoder.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <utility>

#include "project/ProjectFile.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/internal/ProjectIdCodec.h"
#include "project/internal/ProductEnvelopeV6Budgets.h"

namespace solidar::project {
namespace {

constexpr qsizetype kMaximumCapabilities = 16;
constexpr qsizetype kMaximumCapabilityUtf8Bytes = 64;
constexpr int kMaximumJsonDepth = 64;

ProductEnvelopeV6ScanResult failure(ProductEnvelopeV6ScanStatus status,
                                    QString error) {
  ProductEnvelopeV6ScanResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

ProductEnvelopeV6ScanResult invalid(QString error) {
  return failure(ProductEnvelopeV6ScanStatus::Invalid, std::move(error));
}

bool contains(std::initializer_list<const char*> fields,
              const QString& value) {
  return std::any_of(fields.begin(), fields.end(), [&](const char* field) {
    return value == QLatin1String(field);
  });
}

bool exactFields(const QJsonObject& object,
                 std::initializer_list<const char*> fields,
                 const QString& path, QString* error) {
  for (const char* field : fields) {
    if (!object.contains(QLatin1String(field))) {
      *error = QStringLiteral("%1 is missing required field %2")
                   .arg(path, QString::fromLatin1(field));
      return false;
    }
  }
  if (object.size() != static_cast<qsizetype>(fields.size())) {
    for (auto it = object.begin(); it != object.end(); ++it) {
      if (!contains(fields, it.key())) {
        *error = QStringLiteral("%1 contains unexpected field %2")
                     .arg(path, it.key());
        return false;
      }
    }
  }
  return true;
}

bool validUtf16(const QString& value) {
  for (qsizetype index = 0; index < value.size(); ++index) {
    const QChar current = value.at(index);
    if (current.isHighSurrogate()) {
      if (index + 1 >= value.size() ||
          !value.at(index + 1).isLowSurrogate())
        return false;
      ++index;
    } else if (current.isLowSurrogate()) {
      return false;
    }
  }
  return true;
}

bool checkedAdd(qsizetype amount, qsizetype limit, qsizetype* total) {
  return detail::addProductEnvelopeV6Budget(amount, limit, total);
}

bool accountJsonStrings(const QJsonValue& value, int depth, qsizetype* total,
                        QString* error) {
  if (depth > kMaximumJsonDepth) {
    *error = QStringLiteral("JSON nesting exceeds the v6 depth limit");
    return false;
  }
  if (value.isString()) {
    const QString text = value.toString();
    if (!validUtf16(text) ||
        !checkedAdd(text.toUtf8().size(),
                    kProductEnvelopeV6MaximumJsonStringUtf8Bytes, total)) {
      *error = QStringLiteral("JSON strings exceed the v6 UTF-8 budget");
      return false;
    }
  } else if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const auto item : array) {
      if (!accountJsonStrings(item, depth + 1, total, error)) return false;
    }
  } else if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
      if (!validUtf16(it.key()) ||
          !checkedAdd(it.key().toUtf8().size(),
                      kProductEnvelopeV6MaximumJsonStringUtf8Bytes, total)) {
        *error = QStringLiteral("JSON strings exceed the v6 UTF-8 budget");
        return false;
      }
      if (!accountJsonStrings(it.value(), depth + 1, total, error))
        return false;
    }
  }
  return true;
}

bool boundedString(const QJsonValue& value, bool allowEmpty, QString* decoded,
                   const QString& path, QString* error) {
  if (!value.isString()) {
    *error = path + QStringLiteral(" must be a string");
    return false;
  }
  const QString text = value.toString();
  if ((!allowEmpty && text.isEmpty()) ||
      text.size() > ProjectFile::kMaximumStringCharacters ||
      !validUtf16(text)) {
    *error = path + QStringLiteral(" has an invalid string value");
    return false;
  }
  if (decoded) *decoded = text;
  return true;
}

bool exactInteger(const QJsonValue& value, qint64 minimum, qint64 maximum,
                  qint64* decoded, const QString& path, QString* error) {
  if (!value.isDouble()) {
    *error = path + QStringLiteral(" must be an exact integer");
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(maximum)) {
    *error = path + QStringLiteral(" is outside its integer range");
    return false;
  }
  *decoded = static_cast<qint64>(number);
  return true;
}

bool persistedId(const QJsonValue& value, qint64* decoded,
                 const QString& path, QString* error) {
  return exactInteger(value, 1, ProjectFile::kMaximumPersistedId, decoded,
                      path, error);
}

bool booleanValue(const QJsonValue& value, bool* decoded,
                  const QString& path, QString* error) {
  if (!value.isBool()) {
    *error = path + QStringLiteral(" must be boolean");
    return false;
  }
  *decoded = value.toBool();
  return true;
}

bool numberArray(const QJsonValue& value, qsizetype expected,
                 std::vector<double>* decoded, const QString& path,
                 QString* error) {
  if (!value.isArray() || value.toArray().size() != expected) {
    *error = QStringLiteral("%1 must contain exactly %2 numbers")
                 .arg(path)
                 .arg(expected);
    return false;
  }
  decoded->clear();
  for (qsizetype index = 0; index < expected; ++index) {
    const auto entry = value.toArray().at(index);
    if (!entry.isDouble() || !std::isfinite(entry.toDouble())) {
      *error = QStringLiteral("%1[%2] must be finite").arg(path).arg(index);
      return false;
    }
    decoded->push_back(entry.toDouble());
  }
  return true;
}

bool unsignedUtf8Less(const QByteArray& left, const QByteArray& right) {
  return std::lexicographical_compare(
      left.cbegin(), left.cend(), right.cbegin(), right.cend(),
      [](char lhs, char rhs) {
        return static_cast<unsigned char>(lhs) <
               static_cast<unsigned char>(rhs);
      });
}

enum class ScannedDefinitionKind { Part, Assembly, Unknown };

struct ScannedDefinition {
  DefinitionId id;
  ScannedDefinitionKind kind{ScannedDefinitionKind::Unknown};
};

}  // namespace

ProductEnvelopeV6ScanResult scanProductEnvelopeV6(QByteArrayView bytes) {
  if (bytes.isEmpty()) return invalid(QStringLiteral("input is empty"));
  if (bytes.size() > ProjectFile::kMaximumFileBytes)
    return invalid(QStringLiteral("input exceeds the project file limit"));

  QJsonParseError parseError;
  const QJsonDocument parsed = QJsonDocument::fromJson(
      QByteArray(bytes.data(), bytes.size()), &parseError);
  if (parseError.error != QJsonParseError::NoError)
    return invalid(
        QStringLiteral("invalid JSON: %1").arg(parseError.errorString()));
  if (!parsed.isObject())
    return invalid(QStringLiteral("project root must be an object"));
  const QJsonObject root = parsed.object();

  QString error;
  QString format;
  if (!boundedString(root.value(QStringLiteral("format")), false, &format,
                     QStringLiteral("$.format"), &error))
    return invalid(std::move(error));
  qint64 version = 0;
  if (!exactInteger(root.value(QStringLiteral("version")), 1,
                    9007199254740991LL, &version,
                    QStringLiteral("$.version"), &error))
    return invalid(std::move(error));
  if (format != QStringLiteral("solidar-project") || version < 6)
    return failure(ProductEnvelopeV6ScanStatus::NotApplicable,
                   QStringLiteral("not a SolidarCAD product envelope v6"));
  if (version > 6)
    return failure(ProductEnvelopeV6ScanStatus::Unsupported,
                   QStringLiteral("newer SolidarCAD project version"));
  if (!exactFields(root,
                   {"format", "version", "capabilities", "projectId",
                    "rootDefinitionId", "metadata", "definitions"},
                   QStringLiteral("$"), &error))
    return invalid(std::move(error));

  QString projectText;
  if (!boundedString(root.value(QStringLiteral("projectId")), false,
                     &projectText, QStringLiteral("$.projectId"), &error))
    return invalid(std::move(error));
  const QByteArray projectUtf8 = projectText.toUtf8();
  const auto projectId = detail::parseCanonicalProjectId(
      std::string_view(projectUtf8.constData(),
                       static_cast<std::size_t>(projectUtf8.size())));
  if (!projectId)
    return invalid(QStringLiteral(
        "$.projectId must be a canonical lowercase non-nil UUID"));

  qint64 rootIdValue = 0;
  if (!persistedId(root.value(QStringLiteral("rootDefinitionId")),
                   &rootIdValue, QStringLiteral("$.rootDefinitionId"),
                   &error))
    return invalid(std::move(error));
  const DefinitionId rootId{static_cast<std::uint64_t>(rootIdValue)};

  const auto metadataValue = root.value(QStringLiteral("metadata"));
  if (!metadataValue.isObject())
    return invalid(QStringLiteral("$.metadata must be an object"));
  const QJsonObject metadataObject = metadataValue.toObject();
  if (!exactFields(metadataObject, {"name", "createdAt"},
                   QStringLiteral("$.metadata"), &error))
    return invalid(std::move(error));
  ProductEnvelopeV5Metadata metadata;
  if (!boundedString(metadataObject.value(QStringLiteral("name")), true,
                     &metadata.name, QStringLiteral("$.metadata.name"),
                     &error) ||
      !boundedString(metadataObject.value(QStringLiteral("createdAt")), true,
                     &metadata.createdAt,
                     QStringLiteral("$.metadata.createdAt"), &error))
    return invalid(std::move(error));

  const auto capabilitiesValue = root.value(QStringLiteral("capabilities"));
  if (!capabilitiesValue.isArray())
    return invalid(QStringLiteral("$.capabilities must be an array"));
  const QJsonArray capabilityValues = capabilitiesValue.toArray();
  if (capabilityValues.isEmpty() ||
      capabilityValues.size() > kMaximumCapabilities)
    return invalid(QStringLiteral("$.capabilities has an invalid item count"));
  QStringList capabilities;
  std::vector<ProductEnvelopeV6DeferredUnknown> deferred;
  QByteArray previousCapability;
  bool hasPreviousCapability = false;
  bool hasFlat = false;
  bool hasProduct = false;
  for (qsizetype index = 0; index < capabilityValues.size(); ++index) {
    const QString path = QStringLiteral("$.capabilities[%1]").arg(index);
    QString capability;
    if (!boundedString(capabilityValues.at(index), false, &capability, path,
                       &error))
      return invalid(std::move(error));
    const QByteArray encoded = capability.toUtf8();
    if (encoded.size() > kMaximumCapabilityUtf8Bytes)
      return invalid(path + QStringLiteral(" exceeds its UTF-8 byte limit"));
    if (hasPreviousCapability &&
        !unsignedUtf8Less(previousCapability, encoded))
      return invalid(QStringLiteral(
          "$.capabilities must be strictly sorted and unique"));
    hasPreviousCapability = true;
    previousCapability = encoded;
    capabilities.push_back(capability);
    if (capability == QStringLiteral("flat-assembly-v1"))
      hasFlat = true;
    else if (capability == QStringLiteral("product-envelope-v1"))
      hasProduct = true;
    else
      deferred.push_back({ProductEnvelopeV6DeferredUnknownKind::Capability,
                          path, capability});
  }
  if (!hasFlat || !hasProduct)
    return invalid(QStringLiteral(
        "$.capabilities must contain flat-assembly-v1 and product-envelope-v1"));

  const auto definitionsValue = root.value(QStringLiteral("definitions"));
  if (!definitionsValue.isArray())
    return invalid(QStringLiteral("$.definitions must be an array"));
  const QJsonArray definitions = definitionsValue.toArray();
  if (definitions.isEmpty() ||
      definitions.size() > kProductEnvelopeV6MaximumDefinitions)
    return invalid(QStringLiteral("$.definitions has an invalid item count"));

  ProductEnvelopeV6Candidate candidate;
  candidate.authoredRoot = root;
  candidate.projectId = *projectId;
  candidate.rootDefinitionId = rootId;
  candidate.metadata = metadata;
  candidate.capabilities = capabilities;

  std::map<DefinitionId, ScannedDefinitionKind> kinds;
  qint64 previousDefinitionId = 0;
  qsizetype occurrenceTotal = 0;
  for (qsizetype index = 0; index < definitions.size(); ++index) {
    const QString path = QStringLiteral("$.definitions[%1]").arg(index);
    if (!definitions.at(index).isObject())
      return invalid(path + QStringLiteral(" must be an object"));
    const QJsonObject definition = definitions.at(index).toObject();
    qint64 definitionIdValue = 0;
    if (!persistedId(definition.value(QStringLiteral("definitionId")),
                     &definitionIdValue, path + QStringLiteral(".definitionId"),
                     &error))
      return invalid(std::move(error));
    if (definitionIdValue <= previousDefinitionId)
      return invalid(QStringLiteral(
          "$.definitions must be strictly sorted and unique"));
    previousDefinitionId = definitionIdValue;
    const DefinitionId definitionId{
        static_cast<std::uint64_t>(definitionIdValue)};
    QString kind;
    if (!boundedString(definition.value(QStringLiteral("definitionKind")),
                       false, &kind, path + QStringLiteral(".definitionKind"),
                       &error))
      return invalid(std::move(error));

    if (kind == QStringLiteral("assembly")) {
      if (!exactFields(definition,
                       {"definitionId", "definitionKind", "occurrences"}, path,
                       &error))
        return invalid(std::move(error));
      if (definitionId != rootId)
        return invalid(path + QStringLiteral(
                                  " is a forbidden nested assembly"));
      if (candidate.assembly)
        return invalid(QStringLiteral("v6 requires exactly one assembly"));
      const auto occurrencesValue =
          definition.value(QStringLiteral("occurrences"));
      if (!occurrencesValue.isArray())
        return invalid(path + QStringLiteral(".occurrences must be an array"));
      const QJsonArray occurrences = occurrencesValue.toArray();
      if (!checkedAdd(occurrences.size(),
                      kProductEnvelopeV6MaximumOccurrences,
                      &occurrenceTotal))
        return invalid(QStringLiteral(
            "assembly occurrences exceed the v6 aggregate limit"));
      ProductEnvelopeV6AssemblyCandidate assembly;
      assembly.definitionId = definitionId;
      assembly.authoredDefinition = definition;
      qint64 previousOccurrenceId = 0;
      for (qsizetype occurrenceIndex = 0;
           occurrenceIndex < occurrences.size(); ++occurrenceIndex) {
        const QString occurrencePath =
            path + QStringLiteral(".occurrences[%1]").arg(occurrenceIndex);
        if (!occurrences.at(occurrenceIndex).isObject())
          return invalid(occurrencePath + QStringLiteral(" must be an object"));
        const QJsonObject occurrence =
            occurrences.at(occurrenceIndex).toObject();
        if (!exactFields(occurrence,
                         {"occurrenceId", "targetDefinitionId", "placement",
                          "visible", "suppressed", "grounded"},
                         occurrencePath, &error))
          return invalid(std::move(error));
        qint64 occurrenceIdValue = 0;
        qint64 targetIdValue = 0;
        if (!persistedId(occurrence.value(QStringLiteral("occurrenceId")),
                         &occurrenceIdValue,
                         occurrencePath + QStringLiteral(".occurrenceId"),
                         &error) ||
            !persistedId(
                occurrence.value(QStringLiteral("targetDefinitionId")),
                &targetIdValue,
                occurrencePath + QStringLiteral(".targetDefinitionId"),
                &error))
          return invalid(std::move(error));
        if (occurrenceIdValue <= previousOccurrenceId)
          return invalid(path + QStringLiteral(
                                    ".occurrences must be strictly sorted and unique"));
        previousOccurrenceId = occurrenceIdValue;
        const auto placementValue = occurrence.value(QStringLiteral("placement"));
        if (!placementValue.isObject())
          return invalid(occurrencePath +
                         QStringLiteral(".placement must be an object"));
        const QJsonObject placementObject = placementValue.toObject();
        if (!exactFields(placementObject,
                         {"translationMm", "quaternionWxyz"},
                         occurrencePath + QStringLiteral(".placement"),
                         &error))
          return invalid(std::move(error));
        std::vector<double> translationValues;
        std::vector<double> quaternionValues;
        if (!numberArray(placementObject.value(QStringLiteral("translationMm")),
                         3, &translationValues,
                         occurrencePath +
                             QStringLiteral(".placement.translationMm"),
                         &error) ||
            !numberArray(
                placementObject.value(QStringLiteral("quaternionWxyz")), 4,
                &quaternionValues,
                occurrencePath +
                    QStringLiteral(".placement.quaternionWxyz"),
                &error))
          return invalid(std::move(error));
        const auto transform = RigidTransform3d::create(
            {translationValues[0], translationValues[1],
             translationValues[2]},
            {quaternionValues[0], quaternionValues[1], quaternionValues[2],
             quaternionValues[3]});
        if (!transform)
          return invalid(occurrencePath +
                         QStringLiteral(".placement is not a rigid transform"));
        OccurrenceFlags flags;
        if (!booleanValue(occurrence.value(QStringLiteral("visible")),
                          &flags.visible,
                          occurrencePath + QStringLiteral(".visible"),
                          &error) ||
            !booleanValue(occurrence.value(QStringLiteral("suppressed")),
                          &flags.suppressed,
                          occurrencePath + QStringLiteral(".suppressed"),
                          &error) ||
            !booleanValue(occurrence.value(QStringLiteral("grounded")),
                          &flags.grounded,
                          occurrencePath + QStringLiteral(".grounded"),
                          &error))
          return invalid(std::move(error));
        assembly.occurrences.push_back(
            {OccurrenceId{static_cast<std::uint64_t>(occurrenceIdValue)},
             DefinitionId{static_cast<std::uint64_t>(targetIdValue)},
             *transform.transform, flags, occurrence});
      }
      candidate.assembly = std::move(assembly);
      kinds.emplace(definitionId, ScannedDefinitionKind::Assembly);
      continue;
    }

    if (!exactFields(definition,
                     {"definitionId", "definitionKind", "payload"}, path,
                     &error))
      return invalid(std::move(error));
    const auto payloadValue = definition.value(QStringLiteral("payload"));
    if (!payloadValue.isObject())
      return invalid(path + QStringLiteral(".payload must be an object"));
    const QJsonObject payload = payloadValue.toObject();
    if (!exactFields(payload, {"payloadKind", "data"},
                     path + QStringLiteral(".payload"), &error))
      return invalid(std::move(error));
    QString payloadKind;
    if (!boundedString(payload.value(QStringLiteral("payloadKind")), false,
                       &payloadKind,
                       path + QStringLiteral(".payload.payloadKind"),
                       &error))
      return invalid(std::move(error));
    if (!payload.value(QStringLiteral("data")).isObject())
      return invalid(path + QStringLiteral(".payload.data must be an object"));
    const QJsonObject data = payload.value(QStringLiteral("data")).toObject();
    const qsizetype compactBytes =
        QJsonDocument(data).toJson(QJsonDocument::Compact).size();
    if (!checkedAdd(compactBytes,
                    kProductEnvelopeV6MaximumEmbeddedPartBytes,
                    &candidate.embeddedPartBytes))
      return invalid(QStringLiteral(
          "embedded part payloads exceed the v6 byte budget"));

    ProductEnvelopeV6PartCandidate part;
    part.definitionId = definitionId;
    part.authoredDefinition = definition;
    part.authoredPayload = payload;
    if (kind != QStringLiteral("part")) {
      deferred.push_back(
          {ProductEnvelopeV6DeferredUnknownKind::DefinitionKind,
           path + QStringLiteral(".definitionKind"), kind});
      kinds.emplace(definitionId, ScannedDefinitionKind::Unknown);
    } else {
      kinds.emplace(definitionId, ScannedDefinitionKind::Part);
    }

    if (payloadKind != QStringLiteral("embedded-part-v4")) {
      deferred.push_back({ProductEnvelopeV6DeferredUnknownKind::PayloadKind,
                          path + QStringLiteral(".payload.payloadKind"),
                          payloadKind});
    } else {
      if (data.value(QStringLiteral("name")) != metadata.name ||
          data.value(QStringLiteral("createdAt")) != metadata.createdAt)
        return invalid(QStringLiteral(
            "$: metadata must match every embedded part v4 payload"));
      auto envelope = detail::validateProjectFormatV4EnvelopeRoot(data);
      if (!envelope.hasCandidate())
        return invalid(QStringLiteral("invalid embedded part v4: %1")
                           .arg(envelope.error));
      auto nestedPayload =
          scanProjectFormatV4Payload(std::move(*envelope.candidate));
      if (!nestedPayload.hasCandidate())
        return invalid(QStringLiteral("invalid embedded part v4: %1")
                           .arg(nestedPayload.error));
      auto nestedModel =
          validateProjectFormatV4Model(std::move(*nestedPayload.candidate));
      if (!nestedModel.hasCandidate())
        return invalid(QStringLiteral("invalid embedded part v4: %1")
                           .arg(nestedModel.error));
      const qsizetype nestedWork =
          nestedModel.candidate->legacySketch.topology.payload.rebuildWork;
      if (!checkedAdd(nestedWork, ProjectFile::kMaximumDocumentRebuildWork,
                      &candidate.rebuildWork))
        return invalid(QStringLiteral(
            "embedded parts exceed the v6 rebuild-work budget"));
      if (nestedModel.status ==
          ProjectFormatV4ModelValidationStatus::UnsupportedCandidate)
        deferred.push_back(
            {ProductEnvelopeV6DeferredUnknownKind::EmbeddedPartSemantic,
             path + QStringLiteral(".payload.data"),
             QStringLiteral("unsupported embedded part v4 semantic")});
      part.embeddedPart = std::move(*nestedModel.candidate);
    }
    candidate.parts.push_back(std::move(part));
  }

  if (!kinds.contains(rootId))
    return invalid(QStringLiteral("$.rootDefinitionId is dangling"));
  if (kinds.at(rootId) == ScannedDefinitionKind::Part)
    return invalid(QStringLiteral("v6 root definition must be an assembly"));
  if (kinds.at(rootId) == ScannedDefinitionKind::Assembly &&
      !candidate.assembly)
    return invalid(QStringLiteral("v6 root assembly is missing"));
  if (kinds.at(rootId) != ScannedDefinitionKind::Unknown &&
      candidate.assembly) {
    for (const auto& occurrence : candidate.assembly->occurrences) {
      const auto target = kinds.find(occurrence.targetDefinitionId);
      if (target == kinds.end())
        return invalid(QStringLiteral("assembly occurrence target is dangling"));
      if (target->second == ScannedDefinitionKind::Assembly)
        return invalid(QStringLiteral("nested assembly targets are forbidden"));
    }
  }
  qsizetype jsonStringBytes = 0;
  if (!accountJsonStrings(root, 0, &jsonStringBytes, &error))
    return invalid(std::move(error));
  candidate.jsonStringUtf8Bytes = jsonStringBytes;
  candidate.deferredUnknowns = std::move(deferred);

  ProductEnvelopeV6ScanResult result;
  result.status = candidate.deferredUnknowns.empty()
                      ? ProductEnvelopeV6ScanStatus::Candidate
                      : ProductEnvelopeV6ScanStatus::Unsupported;
  result.candidate = std::move(candidate);
  if (result.status == ProductEnvelopeV6ScanStatus::Unsupported)
    result.error = QStringLiteral(
        "product envelope contains unsupported required semantics");
  return result;
}

}  // namespace solidar::project
