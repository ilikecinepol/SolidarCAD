#include "project/ProductEnvelopeV12Decoder.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStringDecoder>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <set>
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

ProductEnvelopeV12ScanResult fail(ProductEnvelopeV12ScanStatus status,
                                  QString error) {
  ProductEnvelopeV12ScanResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

ProductEnvelopeV12ScanResult invalid(QString error) {
  return fail(ProductEnvelopeV12ScanStatus::Invalid, std::move(error));
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

bool numberArray(const QJsonValue& value, qsizetype count,
                 std::vector<double>* output, const QString& path,
                 QString* error) {
  if (!value.isArray() || value.toArray().size() != count) {
    *error = path + QStringLiteral(" has the wrong length");
    return false;
  }
  output->clear();
  for (const auto& item : value.toArray()) {
    if (!item.isDouble() || !std::isfinite(item.toDouble())) {
      *error = path + QStringLiteral(" must contain finite numbers");
      return false;
    }
    output->push_back(item.toDouble());
  }
  return true;
}

bool exactQuaternion(const std::array<double, 4>& authored,
                     const std::array<double, 4>& canonical) noexcept {
  for (std::size_t index = 0; index < authored.size(); ++index) {
    if (std::bit_cast<std::uint64_t>(authored[index]) !=
        std::bit_cast<std::uint64_t>(canonical[index]))
      return false;
  }
  return true;
}

bool exactTranslation(const std::array<double, 3>& authored,
                      const std::array<double, 3>& canonical) noexcept {
  for (std::size_t index = 0; index < authored.size(); ++index) {
    if (std::bit_cast<std::uint64_t>(authored[index]) !=
        std::bit_cast<std::uint64_t>(canonical[index]))
      return false;
  }
  return true;
}

std::optional<RigidTransform3d> decodeTransform(
    const QJsonValue& value, const QString& path, QString* error) {
  if (!value.isObject() ||
      !exactFields(value.toObject(), {"translationMm", "quaternionWxyz"},
                   path, error)) {
    if (error->isEmpty()) *error = path + QStringLiteral(" is invalid");
    return std::nullopt;
  }
  std::vector<double> translation;
  std::vector<double> rotation;
  if (!numberArray(value.toObject().value(QStringLiteral("translationMm")),
                   3, &translation, path + QStringLiteral(".translationMm"),
                   error) ||
      !numberArray(value.toObject().value(QStringLiteral("quaternionWxyz")),
                   4, &rotation, path + QStringLiteral(".quaternionWxyz"),
                   error))
    return std::nullopt;
  const auto transform = RigidTransform3d::create(
      {translation[0], translation[1], translation[2]},
      {rotation[0], rotation[1], rotation[2], rotation[3]});
  if (!transform) {
    *error = path + QStringLiteral(" transform is invalid");
    return std::nullopt;
  }
  const std::array<double, 3> authoredTranslation{
      translation[0], translation[1], translation[2]};
  if (!exactTranslation(authoredTranslation,
                        transform.transform->translationMm())) {
    *error = path + QStringLiteral(
                        " translation is not in canonical persisted form");
    return std::nullopt;
  }
  const std::array<double, 4> authoredRotation{
      rotation[0], rotation[1], rotation[2], rotation[3]};
  if (!exactQuaternion(authoredRotation,
                       transform.transform->quaternionWxyz())) {
    *error = path + QStringLiteral(
                        " quaternion is not in canonical persisted form");
    return std::nullopt;
  }
  return *transform.transform;
}

struct V12OccurrenceDecodeResult final {
  std::vector<ProductEnvelopeV12OccurrenceCandidate> occurrences;
  std::size_t aggregateAuthoredOccurrences{};
  QString error;
  bool valid{};
};

V12OccurrenceDecodeResult decodeV12Occurrences(
    const QJsonValue& value, const QString& definitionPath,
    std::size_t currentAuthoredOccurrences,
    std::size_t maximumAuthoredOccurrences) {
  V12OccurrenceDecodeResult result;
  if (!value.isArray()) {
    result.error = definitionPath +
                   QStringLiteral(".occurrences must be an array");
    return result;
  }
  const QJsonArray authored = value.toArray();
  if (!fitsAssemblyGraphBudget(currentAuthoredOccurrences,
                               static_cast<std::size_t>(authored.size()),
                               maximumAuthoredOccurrences)) {
    result.error =
        QStringLiteral("aggregate authored occurrence budget exceeded");
    return result;
  }
  result.aggregateAuthoredOccurrences =
      currentAuthoredOccurrences + static_cast<std::size_t>(authored.size());
  result.occurrences.reserve(static_cast<std::size_t>(authored.size()));
  qint64 previousOccurrenceId = 0;
  for (qsizetype index = 0; index < authored.size(); ++index) {
    const QString path = definitionPath +
                         QStringLiteral(".occurrences[%1]").arg(index);
    if (!authored.at(index).isObject()) {
      result.error = path + QStringLiteral(" must be an object");
      return result;
    }
    const QJsonObject object = authored.at(index).toObject();
    QString error;
    if (!exactFields(object,
                     {"occurrenceId", "targetDefinitionId", "placement",
                      "homePlacement", "name", "visible", "suppressed",
                      "grounded"},
                     path, &error)) {
      result.error = std::move(error);
      return result;
    }
    qint64 occurrenceIdValue = 0;
    qint64 targetIdValue = 0;
    if (!persistedId(object.value(QStringLiteral("occurrenceId")),
                     &occurrenceIdValue,
                     path + QStringLiteral(".occurrenceId"), &error) ||
        !persistedId(object.value(QStringLiteral("targetDefinitionId")),
                     &targetIdValue,
                     path + QStringLiteral(".targetDefinitionId"), &error)) {
      result.error = std::move(error);
      return result;
    }
    if (occurrenceIdValue <= previousOccurrenceId) {
      result.error = definitionPath +
                     QStringLiteral(" occurrence IDs are not sorted");
      return result;
    }
    previousOccurrenceId = occurrenceIdValue;
    const auto placement = decodeTransform(
        object.value(QStringLiteral("placement")),
        path + QStringLiteral(".placement"), &error);
    const auto homePlacement = decodeTransform(
        object.value(QStringLiteral("homePlacement")),
        path + QStringLiteral(".homePlacement"), &error);
    if (!placement || !homePlacement) {
      result.error = std::move(error);
      return result;
    }
    QString name;
    if (!boundedString(object.value(QStringLiteral("name")), true, &name,
                       path + QStringLiteral(".name"), &error)) {
      result.error = std::move(error);
      return result;
    }
    const QByteArray nameBytes = name.toUtf8();
    std::string nameUtf8(nameBytes.constData(),
                         static_cast<std::size_t>(nameBytes.size()));
    if (validateOccurrenceDisplayNameUtf8(nameUtf8, true) !=
        OccurrenceDisplayNameStatus::Valid) {
      result.error = path + QStringLiteral(" name is invalid");
      return result;
    }
    const auto visible = object.value(QStringLiteral("visible"));
    const auto suppressed = object.value(QStringLiteral("suppressed"));
    const auto grounded = object.value(QStringLiteral("grounded"));
    if (!visible.isBool() || !suppressed.isBool() || !grounded.isBool()) {
      result.error = path + QStringLiteral(" flags are invalid");
      return result;
    }
    result.occurrences.push_back(ProductEnvelopeV12OccurrenceCandidate{
        OccurrenceId{static_cast<std::uint64_t>(occurrenceIdValue)},
        DefinitionId{static_cast<std::uint64_t>(targetIdValue)}, *placement,
        *homePlacement,
        OccurrenceFlags{visible.toBool(), suppressed.toBool(),
                        grounded.toBool()},
        std::move(nameUtf8), object});
  }
  result.valid = true;
  return result;
}

bool accountStrings(const QJsonValue& value, int depth, qsizetype* total) {
  if (depth > kMaximumJsonDepth) return false;
  if (value.isString()) {
    const QString text = value.toString();
    const qsizetype bytes = text.toUtf8().size();
    if (!validUtf16(text) || *total < 0 || bytes < 0 ||
        bytes > kProductEnvelopeV12MaximumJsonStringUtf8Bytes - *total)
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
          keyBytes > kProductEnvelopeV12MaximumJsonStringUtf8Bytes - *total)
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

ProductEnvelopeV12ScanResult scanProductEnvelopeV12(QByteArrayView bytes) {
  try {
    if (bytes.isEmpty() || bytes.size() > ProjectFile::kMaximumFileBytes)
      return invalid(QStringLiteral("v12 input size is invalid"));
    QStringDecoder utf8Decoder(QStringDecoder::Utf8);
    static_cast<void>(utf8Decoder.decode(bytes));
    if (utf8Decoder.hasError())
      return invalid(QStringLiteral("v12 input is not exact UTF-8"));
    QJsonParseError parseError;
    const auto parsed = QJsonDocument::fromJson(
        QByteArray(bytes.data(), bytes.size()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
      return invalid(QStringLiteral("v12 root is not valid JSON object"));
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
    if (format != QStringLiteral("solidar-project") || version < 12)
      return fail(ProductEnvelopeV12ScanStatus::NotApplicable,
                  QStringLiteral("not a v12 product envelope"));
    if (version > 12)
      return fail(ProductEnvelopeV12ScanStatus::Unsupported,
                  QStringLiteral("newer product envelope"));
    QString rawError;
    if (!detail::validateProductEnvelopeV12TransformRawTokens(bytes,
                                                               &rawError))
      return invalid(std::move(rawError));
    if (!exactFields(root,
                     {"format", "version", "capabilities", "projectId",
                      "rootDefinitionId", "metadata", "definitions"},
                     QStringLiteral("$"), &error))
      return invalid(std::move(error));

    qsizetype stringBytes = 0;
    if (!accountStrings(root, 0, &stringBytes))
      return invalid(QStringLiteral("v12 JSON string/depth budget exceeded"));

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
    if (!capabilitiesValue.isArray() || capabilitiesValue.toArray().size() != 5)
      return invalid(QStringLiteral("v12 requires exactly five capabilities"));
    const QStringList expected{
        QStringLiteral("component-definitions-v1"),
        QStringLiteral("component-drawings-v1"),
        QStringLiteral("component-home-placement-v1"),
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
        return invalid(QStringLiteral("v12 capability set/order is invalid"));
      previous = encoded;
      capabilities.push_back(capability);
    }

    const auto definitionsValue = root.value(QStringLiteral("definitions"));
    if (!definitionsValue.isArray() || definitionsValue.toArray().isEmpty() ||
        definitionsValue.toArray().size() >
            static_cast<qsizetype>(kAssemblyGraphMaximumDefinitions))
      return invalid(QStringLiteral("definition count exceeds v12 budget"));

    ProductEnvelopeV12Candidate candidate;
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
                        "drawings", "occurrences"},
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
                     kProductEnvelopeV12MaximumEmbeddedPayloadBytes,
                     &candidate.embeddedPayloadBytes))
        return invalid(QStringLiteral("embedded payload budget exceeded"));
      auto model = scanEmbeddedDocument(
          data, metadata, path + QStringLiteral(".payload.data"), &error);
      if (!model) return invalid(std::move(error));
      std::set<BodyId> owningBodyIds;
      std::vector<BodyId> owningBodyOrder;
      const QJsonArray authoredBodies =
          data.value(QStringLiteral("model"))
              .toObject()
              .value(QStringLiteral("bodies"))
              .toArray();
      owningBodyOrder.reserve(
          static_cast<std::size_t>(authoredBodies.size()));
      for (qsizetype bodyIndex = 0; bodyIndex < authoredBodies.size();
           ++bodyIndex) {
        qint64 bodyIdValue = 0;
        if (!persistedId(authoredBodies.at(bodyIndex)
                             .toObject()
                             .value(QStringLiteral("id")),
                         &bodyIdValue,
                         path + QStringLiteral(".payload.data.model.bodies[%1].id")
                                    .arg(bodyIndex),
                         &error))
          return invalid(std::move(error));
        const BodyId bodyId = static_cast<BodyId>(bodyIdValue);
        if (!owningBodyIds.insert(bodyId).second)
          return invalid(path + QStringLiteral(" has duplicate body ID"));
        owningBodyOrder.push_back(bodyId);
      }
      const auto& topology = model->legacySketch.topology;
      const auto& scannedPayload = topology.payload;
      if (!addBudget(topology.topologyReferences.size(),
                     kProductEnvelopeV12MaximumTopologyReferences,
                     &candidate.topologyReferenceCount) ||
          !addBudget(topology.provenanceRecordCount,
                     kProductEnvelopeV12MaximumProvenanceRecords,
                     &candidate.provenanceRecordCount) ||
          !addBudget(topology.localNameUtf8Bytes,
                     kProductEnvelopeV12MaximumLocalNameUtf8Bytes,
                     &candidate.localNameUtf8Bytes) ||
          !addBudget(scannedPayload.rebuildWork,
                     kProductEnvelopeV12MaximumRebuildWork,
                     &candidate.rebuildWork))
        return invalid(QStringLiteral("aggregate component payload budget exceeded"));

      const auto drawingsValue = definition.value(QStringLiteral("drawings"));
      if (!drawingsValue.isArray())
        return invalid(path + QStringLiteral(".drawings must be an array"));
      const QJsonArray authoredDrawings = drawingsValue.toArray();
      if (!addBudget(static_cast<std::size_t>(authoredDrawings.size()),
                     kComponentGraphMaximumDrawings,
                     &candidate.drawingCount))
        return invalid(QStringLiteral("aggregate drawing count exceeded"));
      std::set<DrawingId> drawingIds;
      std::vector<ComponentDrawing> drawings;
      drawings.reserve(static_cast<std::size_t>(authoredDrawings.size()));
      for (qsizetype drawingIndex = 0;
           drawingIndex < authoredDrawings.size(); ++drawingIndex) {
        const QString drawingPath =
            path + QStringLiteral(".drawings[%1]").arg(drawingIndex);
        if (!authoredDrawings.at(drawingIndex).isObject())
          return invalid(drawingPath + QStringLiteral(" must be an object"));
        const QJsonObject authoredDrawing =
            authoredDrawings.at(drawingIndex).toObject();
        if (!exactFields(authoredDrawing,
                         {"drawingId", "name", "paper", "orientation",
                          "scale", "sourceBodyIds"},
                         drawingPath, &error))
          return invalid(std::move(error));
        qint64 drawingIdValue = 0;
        QString name;
        QString paper;
        QString orientation;
        if (!persistedId(authoredDrawing.value(QStringLiteral("drawingId")),
                         &drawingIdValue,
                         drawingPath + QStringLiteral(".drawingId"), &error) ||
            !boundedString(authoredDrawing.value(QStringLiteral("name")),
                           false, &name,
                           drawingPath + QStringLiteral(".name"), &error) ||
            !boundedString(authoredDrawing.value(QStringLiteral("paper")),
                           false, &paper,
                           drawingPath + QStringLiteral(".paper"), &error) ||
            !boundedString(
                authoredDrawing.value(QStringLiteral("orientation")), false,
                &orientation,
                drawingPath + QStringLiteral(".orientation"), &error))
          return invalid(std::move(error));
        const DrawingId drawingId{
            static_cast<std::uint64_t>(drawingIdValue)};
        if (!drawingIds.insert(drawingId).second)
          return invalid(drawingPath + QStringLiteral(" duplicates drawing ID"));
        const QByteArray nameBytes = name.toUtf8();
        if (nameBytes.isEmpty() ||
            nameBytes.size() >
                static_cast<qsizetype>(
                    kComponentDrawingMaximumNameUtf8Bytes) ||
            !addBudget(static_cast<std::size_t>(nameBytes.size()),
                       kProductEnvelopeV12MaximumDrawingNameUtf8Bytes,
                       &candidate.drawingNameUtf8Bytes))
          return invalid(QStringLiteral("drawing name budget exceeded"));
        if (paper != QStringLiteral("A4") ||
            (orientation != QStringLiteral("portrait") &&
             orientation != QStringLiteral("landscape")))
          return invalid(drawingPath + QStringLiteral(" enum is invalid"));
        const QJsonValue scaleValue =
            authoredDrawing.value(QStringLiteral("scale"));
        const double scale = scaleValue.toDouble();
        if (!scaleValue.isDouble() || !std::isfinite(scale) ||
            std::signbit(scale) || scale < kComponentDrawingMinimumScale ||
            scale > kComponentDrawingMaximumScale)
          return invalid(drawingPath + QStringLiteral(".scale is invalid"));
        const QJsonValue sourceIdsValue =
            authoredDrawing.value(QStringLiteral("sourceBodyIds"));
        if (!sourceIdsValue.isArray())
          return invalid(drawingPath +
                         QStringLiteral(".sourceBodyIds must be an array"));
        const QJsonArray authoredSourceIds = sourceIdsValue.toArray();
        if (!addBudget(
                static_cast<std::size_t>(authoredSourceIds.size()),
                kComponentGraphMaximumDrawingBodyReferences,
                &candidate.drawingSourceReferenceCount))
          return invalid(QStringLiteral("drawing source budget exceeded"));
        std::vector<BodyId> sourceBodyIds;
        sourceBodyIds.reserve(
            static_cast<std::size_t>(authoredSourceIds.size()));
        qint64 previousBodyId = 0;
        for (qsizetype sourceIndex = 0;
             sourceIndex < authoredSourceIds.size(); ++sourceIndex) {
          qint64 sourceBodyId = 0;
          if (!persistedId(authoredSourceIds.at(sourceIndex), &sourceBodyId,
                           drawingPath +
                               QStringLiteral(".sourceBodyIds[%1]")
                                   .arg(sourceIndex),
                           &error))
            return invalid(std::move(error));
          if (sourceBodyId <= previousBodyId)
            return invalid(drawingPath +
                           QStringLiteral(" source IDs are not canonical"));
          previousBodyId = sourceBodyId;
          const BodyId bodyId = static_cast<BodyId>(sourceBodyId);
          if (!owningBodyIds.contains(bodyId))
            return invalid(drawingPath +
                           QStringLiteral(" references missing body"));
          sourceBodyIds.push_back(bodyId);
        }
        if (!addBudget(
                static_cast<std::size_t>(1) + sourceBodyIds.size(),
                kProductEnvelopeV12MaximumDrawingWork,
                &candidate.drawingWork))
          return invalid(QStringLiteral("drawing work budget exceeded"));
        drawings.emplace_back(
            drawingId,
            std::string(nameBytes.constData(),
                        static_cast<std::size_t>(nameBytes.size())),
            ComponentDrawingPaper::A4,
            orientation == QStringLiteral("portrait")
                ? ComponentDrawingOrientation::Portrait
                : ComponentDrawingOrientation::Landscape,
            scale, std::move(sourceBodyIds));
      }

      auto decoded = decodeV12Occurrences(
          definition.value(QStringLiteral("occurrences")), path,
          occurrenceTotal, kAssemblyGraphMaximumAuthoredOccurrences);
      if (!decoded.valid) return invalid(std::move(decoded.error));
      occurrenceTotal = decoded.aggregateAuthoredOccurrences;
      const DefinitionId id{static_cast<std::uint64_t>(idValue)};
      ComponentGraphDefinition graphDefinition;
      graphDefinition.bodyIds = std::move(owningBodyOrder);
      graphDefinition.drawings.reserve(drawings.size());
      for (const auto& drawing : drawings)
        graphDefinition.drawings.push_back(
            {drawing.id(), drawing.sourceBodyIds()});
      graphDefinition.occurrences.reserve(decoded.occurrences.size());
      for (const auto& occurrence : decoded.occurrences)
        graphDefinition.occurrences.push_back(
            {occurrence.occurrenceId, occurrence.targetDefinitionId});
      if (!candidate.graph.definitions.emplace(
              id, std::move(graphDefinition)).second)
        return invalid(QStringLiteral("duplicate component definition ID"));
      candidate.definitions.push_back(
          {id, definition, payload, std::move(*model),
           std::move(drawings), std::move(decoded.occurrences)});
    }
    const auto graphStatus = analyzeComponentGraph(candidate.graph);
    if (graphStatus != ComponentGraphStatus::Valid)
      return invalid(QStringLiteral("component graph rejected (%1)")
                         .arg(static_cast<int>(graphStatus)));

    ProductEnvelopeV12ScanResult result;
    result.status = ProductEnvelopeV12ScanStatus::Candidate;
    result.candidate = std::move(candidate);
    return result;
  } catch (const std::exception& exception) {
    return invalid(QStringLiteral("v12 scan failure: %1")
                       .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return invalid(QStringLiteral("unknown v12 scan failure"));
  }
}

}  // namespace solidar::project
