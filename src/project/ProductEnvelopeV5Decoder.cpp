#include "project/ProductEnvelopeV5Decoder.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "project/ProjectFile.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4PayloadScanner.h"

namespace solidar::project {
namespace {

constexpr qsizetype kMaximumCapabilities = 16;
constexpr qsizetype kMaximumCapabilityUtf8Bytes = 64;

ProductEnvelopeV5ScanResult failure(ProductEnvelopeV5ScanStatus status,
                                    QString error) {
  ProductEnvelopeV5ScanResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

ProductEnvelopeV5ScanResult invalid(QString error) {
  return failure(ProductEnvelopeV5ScanStatus::Invalid, std::move(error));
}

bool containsField(std::initializer_list<const char*> fields,
                   const QString& value) {
  return std::any_of(fields.begin(), fields.end(), [&](const char* field) {
    return value == QLatin1String(field);
  });
}

bool exactFields(const QJsonObject& object,
                 std::initializer_list<const char*> fields, QString* error,
                 const QString& path) {
  for (const char* field : fields) {
    if (!object.contains(QLatin1String(field))) {
      *error = QStringLiteral("%1 is missing required field %2")
                   .arg(path, QString::fromLatin1(field));
      return false;
    }
  }
  if (object.size() != static_cast<qsizetype>(fields.size())) {
    for (auto it = object.begin(); it != object.end(); ++it) {
      if (!containsField(fields, it.key())) {
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

bool boundedString(const QJsonValue& value, bool allowEmpty, QString* decoded,
                   QString* error, const QString& path) {
  if (!value.isString()) {
    *error = path + QStringLiteral(" must be a string");
    return false;
  }
  const QString token = value.toString();
  if ((!allowEmpty && token.isEmpty()) ||
      token.size() > ProjectFile::kMaximumStringCharacters ||
      !validUtf16(token)) {
    *error = path + QStringLiteral(" has an invalid string value");
    return false;
  }
  if (decoded) *decoded = token;
  return true;
}

bool positivePersistedId(const QJsonValue& value, qint64* decoded,
                         QString* error, const QString& path) {
  if (!value.isDouble()) {
    *error = path + QStringLiteral(" must be an exact integer");
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number || number < 1.0 ||
      number > static_cast<double>(ProjectFile::kMaximumPersistedId)) {
    *error = path + QStringLiteral(" is outside its persisted ID range");
    return false;
  }
  if (decoded) *decoded = static_cast<qint64>(number);
  return true;
}

bool positiveSchemaVersion(const QJsonValue& value, qint64* decoded,
                           QString* error) {
  if (!value.isDouble()) {
    *error = QStringLiteral("$.version must be an exact integer");
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number || number < 1.0 ||
      number > 9007199254740991.0) {
    *error = QStringLiteral("$.version is outside its integer range");
    return false;
  }
  *decoded = static_cast<qint64>(number);
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

std::optional<ProjectId> parseCanonicalProjectId(const QString& text) {
  if (text.size() != 36) return std::nullopt;
  constexpr std::array<int, 4> kHyphens{8, 13, 18, 23};
  for (const int position : kHyphens) {
    if (text.at(position) != QLatin1Char('-')) return std::nullopt;
  }

  ProjectId::Bytes bytes{};
  std::size_t nibble = 0;
  auto decodeHex = [](QChar character) -> int {
    const ushort value = character.unicode();
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    return -1;
  };
  for (qsizetype index = 0; index < text.size(); ++index) {
    if (std::find(kHyphens.begin(), kHyphens.end(), index) !=
        kHyphens.end())
      continue;
    const int value = decodeHex(text.at(index));
    if (value < 0) return std::nullopt;
    const std::size_t byteIndex = nibble / 2;
    if ((nibble % 2) == 0)
      bytes[byteIndex] = static_cast<std::uint8_t>(value << 4);
    else
      bytes[byteIndex] |= static_cast<std::uint8_t>(value);
    ++nibble;
  }
  if (nibble != 32) return std::nullopt;
  ProjectId id{bytes};
  return id.isValid() ? std::optional<ProjectId>{id} : std::nullopt;
}

}  // namespace

ProductEnvelopeV5ScanResult scanProductEnvelopeV5(QByteArrayView bytes) {
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
                     &error, QStringLiteral("$.format")))
    return invalid(std::move(error));
  qint64 version = 0;
  if (!positiveSchemaVersion(root.value(QStringLiteral("version")), &version,
                             &error))
    return invalid(std::move(error));
  if (format != QStringLiteral("solidar-project") || version < 5) {
    return failure(ProductEnvelopeV5ScanStatus::NotApplicable,
                   QStringLiteral("not a SolidarCAD product envelope v5"));
  }
  if (version > 5) {
    return failure(ProductEnvelopeV5ScanStatus::Unsupported,
                   QStringLiteral("newer SolidarCAD project version"));
  }

  if (!exactFields(root,
                   {"format", "version", "capabilities", "projectId",
                    "rootDefinitionId", "metadata", "definitions"},
                   &error, QStringLiteral("$")))
    return invalid(std::move(error));

  QString projectIdText;
  if (!boundedString(root.value(QStringLiteral("projectId")), false,
                     &projectIdText, &error, QStringLiteral("$.projectId")))
    return invalid(std::move(error));
  const auto projectId = parseCanonicalProjectId(projectIdText);
  if (!projectId.has_value())
    return invalid(QStringLiteral(
        "$.projectId must be a canonical lowercase non-nil UUID"));

  qint64 rootDefinitionValue = 0;
  if (!positivePersistedId(root.value(QStringLiteral("rootDefinitionId")),
                           &rootDefinitionValue, &error,
                           QStringLiteral("$.rootDefinitionId")))
    return invalid(std::move(error));

  const QJsonValue metadataValue = root.value(QStringLiteral("metadata"));
  if (!metadataValue.isObject())
    return invalid(QStringLiteral("$.metadata must be an object"));
  const QJsonObject metadata = metadataValue.toObject();
  if (!exactFields(metadata, {"name", "createdAt"}, &error,
                   QStringLiteral("$.metadata")))
    return invalid(std::move(error));
  ProductEnvelopeV5Metadata decodedMetadata;
  if (!boundedString(metadata.value(QStringLiteral("name")), true,
                     &decodedMetadata.name, &error,
                     QStringLiteral("$.metadata.name")) ||
      !boundedString(metadata.value(QStringLiteral("createdAt")), true,
                     &decodedMetadata.createdAt, &error,
                     QStringLiteral("$.metadata.createdAt")))
    return invalid(std::move(error));

  const QJsonValue capabilitiesValue =
      root.value(QStringLiteral("capabilities"));
  if (!capabilitiesValue.isArray())
    return invalid(QStringLiteral("$.capabilities must be an array"));
  const QJsonArray capabilityValues = capabilitiesValue.toArray();
  if (capabilityValues.isEmpty() ||
      capabilityValues.size() > kMaximumCapabilities)
    return invalid(QStringLiteral(
        "$.capabilities has an invalid item count"));

  QStringList capabilities;
  std::vector<ProductEnvelopeV5DeferredUnknown> deferred;
  QByteArray previous;
  bool hasPrevious = false;
  bool hasProductEnvelopeCapability = false;
  for (qsizetype index = 0; index < capabilityValues.size(); ++index) {
    QString capability;
    const QString path = QStringLiteral("$.capabilities[%1]").arg(index);
    if (!boundedString(capabilityValues.at(index), false, &capability, &error,
                       path))
      return invalid(std::move(error));
    const QByteArray encoded = capability.toUtf8();
    if (encoded.size() > kMaximumCapabilityUtf8Bytes)
      return invalid(path + QStringLiteral(" exceeds its UTF-8 byte limit"));
    if (hasPrevious && !unsignedUtf8Less(previous, encoded))
      return invalid(QStringLiteral(
          "$.capabilities must be strictly sorted and unique"));
    hasPrevious = true;
    previous = encoded;
    capabilities.push_back(capability);
    if (capability == QStringLiteral("product-envelope-v1")) {
      hasProductEnvelopeCapability = true;
    } else {
      deferred.push_back({ProductEnvelopeV5DeferredUnknownKind::Capability,
                          path, capability});
    }
  }
  if (!hasProductEnvelopeCapability)
    return invalid(QStringLiteral(
        "$.capabilities must contain product-envelope-v1"));

  const QJsonValue definitionsValue = root.value(QStringLiteral("definitions"));
  if (!definitionsValue.isArray())
    return invalid(QStringLiteral("$.definitions must be an array"));
  const QJsonArray definitions = definitionsValue.toArray();
  if (definitions.size() != 1)
    return invalid(QStringLiteral(
        "$.definitions must contain exactly one root part definition"));
  if (!definitions.at(0).isObject())
    return invalid(QStringLiteral("$.definitions[0] must be an object"));
  const QJsonObject definition = definitions.at(0).toObject();
  if (!exactFields(definition,
                   {"definitionId", "definitionKind", "payload"}, &error,
                   QStringLiteral("$.definitions[0]")))
    return invalid(std::move(error));

  qint64 definitionValue = 0;
  if (!positivePersistedId(definition.value(QStringLiteral("definitionId")),
                           &definitionValue, &error,
                           QStringLiteral("$.definitions[0].definitionId")))
    return invalid(std::move(error));
  if (definitionValue != rootDefinitionValue)
    return invalid(QStringLiteral(
        "$.rootDefinitionId must identify $.definitions[0]"));

  QString definitionKind;
  if (!boundedString(definition.value(QStringLiteral("definitionKind")),
                     false, &definitionKind, &error,
                     QStringLiteral("$.definitions[0].definitionKind")))
    return invalid(std::move(error));
  if (definitionKind != QStringLiteral("part")) {
    deferred.push_back(
        {ProductEnvelopeV5DeferredUnknownKind::DefinitionKind,
         QStringLiteral("$.definitions[0].definitionKind"), definitionKind});
  }

  const QJsonValue payloadValue = definition.value(QStringLiteral("payload"));
  if (!payloadValue.isObject())
    return invalid(QStringLiteral("$.definitions[0].payload must be an object"));
  const QJsonObject payload = payloadValue.toObject();
  if (!exactFields(payload, {"payloadKind", "data"}, &error,
                   QStringLiteral("$.definitions[0].payload")))
    return invalid(std::move(error));
  QString payloadKind;
  if (!boundedString(payload.value(QStringLiteral("payloadKind")), false,
                     &payloadKind, &error,
                     QStringLiteral("$.definitions[0].payload.payloadKind")))
    return invalid(std::move(error));
  const QJsonValue dataValue = payload.value(QStringLiteral("data"));
  if (!dataValue.isObject())
    return invalid(
        QStringLiteral("$.definitions[0].payload.data must be an object"));

  std::optional<ProjectFormatV4ModelCandidate> embeddedPart;
  if (payloadKind == QStringLiteral("embedded-part-v4")) {
    auto envelope =
        detail::validateProjectFormatV4EnvelopeRoot(dataValue.toObject());
    if (envelope.status != ProjectFormatV4EnvelopeScanStatus::Candidate ||
        !envelope.candidate.has_value()) {
      return invalid(QStringLiteral("invalid embedded part v4: %1")
                         .arg(envelope.error));
    }
    auto nestedPayload = scanProjectFormatV4Payload(
        std::move(*envelope.candidate));
    if (nestedPayload.status != ProjectFormatV4PayloadScanStatus::Candidate ||
        !nestedPayload.candidate.has_value()) {
      return invalid(QStringLiteral("invalid embedded part v4: %1")
                         .arg(nestedPayload.error));
    }
    auto nestedModel =
        validateProjectFormatV4Model(std::move(*nestedPayload.candidate));
    if (nestedModel.status == ProjectFormatV4ModelValidationStatus::Invalid ||
        !nestedModel.candidate.has_value()) {
      return invalid(QStringLiteral("invalid embedded part v4: %1")
                         .arg(nestedModel.error));
    }
    if (nestedModel.status ==
        ProjectFormatV4ModelValidationStatus::UnsupportedCandidate) {
      deferred.push_back(
          {ProductEnvelopeV5DeferredUnknownKind::EmbeddedPartSemantic,
           QStringLiteral("$.definitions[0].payload.data"),
           QStringLiteral("unsupported embedded part v4 semantic")});
    }
    embeddedPart = std::move(*nestedModel.candidate);
  } else {
    deferred.push_back({ProductEnvelopeV5DeferredUnknownKind::PayloadKind,
                        QStringLiteral(
                            "$.definitions[0].payload.payloadKind"),
                        payloadKind});
  }

  ProductEnvelopeV5Candidate candidate;
  candidate.authoredRoot = root;
  candidate.projectId = *projectId;
  candidate.rootDefinitionId =
      DefinitionId{static_cast<std::uint64_t>(rootDefinitionValue)};
  candidate.definitionId =
      DefinitionId{static_cast<std::uint64_t>(definitionValue)};
  candidate.metadata = std::move(decodedMetadata);
  candidate.capabilities = std::move(capabilities);
  candidate.authoredDefinition = definition;
  candidate.authoredPayload = payload;
  candidate.embeddedPart = std::move(embeddedPart);
  candidate.deferredUnknowns = std::move(deferred);

  ProductEnvelopeV5ScanResult result;
  result.status = candidate.deferredUnknowns.empty()
                      ? ProductEnvelopeV5ScanStatus::Candidate
                      : ProductEnvelopeV5ScanStatus::Unsupported;
  result.candidate = std::move(candidate);
  if (result.status == ProductEnvelopeV5ScanStatus::Unsupported)
    result.error = QStringLiteral(
        "product envelope contains unsupported required semantics");
  return result;
}

}  // namespace solidar::project
