#include "project/ProductEnvelopeV8Decoder.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <utility>

#include "project/ProjectFile.h"
#include "project/internal/ProjectIdCodec.h"
#include "project/internal/ProductEnvelopeV7Budgets.h"
#include "project/internal/StrictOccurrenceCodec.h"

namespace solidar::project {
namespace {

constexpr int kMaximumJsonDepth = 64;
constexpr qsizetype kMaximumCapabilityUtf8Bytes = 64;

ProductEnvelopeV8ScanResult fail(ProductEnvelopeV8ScanStatus status,
                                 QString error) {
  ProductEnvelopeV8ScanResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

ProductEnvelopeV8ScanResult invalid(QString error) {
  return fail(ProductEnvelopeV8ScanStatus::Invalid, std::move(error));
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
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
    if (!listed(fields, iterator.key())) {
      *error = QStringLiteral("%1 contains unexpected field %2")
                   .arg(path, iterator.key());
      return false;
    }
  }
  return object.size() == static_cast<qsizetype>(fields.size());
}

bool validUtf16(const QString& value) {
  for (qsizetype i = 0; i < value.size(); ++i) {
    if (value[i].isHighSurrogate()) {
      if (++i >= value.size() || !value[i].isLowSurrogate()) return false;
    } else if (value[i].isLowSurrogate()) {
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
    const auto text = value.toString();
    return validUtf16(text) && detail::addProductEnvelopeV7Budget(
                                   text.toUtf8().size(),
                                   kProductEnvelopeV7MaximumJsonStringUtf8Bytes,
                                   total);
  }
  if (value.isArray()) {
    for (const auto& item : value.toArray())
      if (!accountStrings(item, depth + 1, total)) return false;
  } else if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
      if (!validUtf16(iterator.key()) ||
          !detail::addProductEnvelopeV7Budget(
              iterator.key().toUtf8().size(),
              kProductEnvelopeV7MaximumJsonStringUtf8Bytes, total) ||
          !accountStrings(iterator.value(), depth + 1, total))
        return false;
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

}  // namespace

ProductEnvelopeV8ScanResult scanProductEnvelopeV8(QByteArrayView bytes) {
  try {
    if (bytes.isEmpty() || bytes.size() > ProjectFile::kMaximumFileBytes)
      return invalid(QStringLiteral("v8 input size is invalid"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), bytes.size()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
      return invalid(QStringLiteral("v8 root is not valid JSON object"));
    const QJsonObject root = document.object();
    QString error;
    QString format;
    qint64 version = 0;
    if (!boundedString(root.value(QStringLiteral("format")), false, &format,
                       QStringLiteral("$.format"), &error) ||
        !integer(root.value(QStringLiteral("version")), 1,
                 9007199254740991LL, &version, QStringLiteral("$.version"),
                 &error))
      return invalid(std::move(error));
    if (format != QStringLiteral("solidar-project") || version < 8)
      return fail(ProductEnvelopeV8ScanStatus::NotApplicable,
                  QStringLiteral("not a v8 product envelope"));
    if (version > 8)
      return fail(ProductEnvelopeV8ScanStatus::Unsupported,
                  QStringLiteral("newer product envelope"));
    // Raw-token policy is a V8 schema rule, not a generic JSON gate. Exact
    // format/version dispatch above must remain side-effect-free for V6/V7,
    // future envelopes and unrelated JSON.
    QString rawTokenError;
    if (!detail::validateStrictOccurrenceRawTokens(bytes, &rawTokenError))
      return invalid(std::move(rawTokenError));
    if (!exactFields(root,
                     {"format", "version", "capabilities", "projectId",
                      "rootDefinitionId", "metadata", "definitions"},
                     QStringLiteral("$"), &error))
      return invalid(std::move(error));
    qsizetype stringBytes = 0;
    if (!accountStrings(root, 0, &stringBytes))
      return invalid(QStringLiteral("v8 JSON string/depth budget exceeded"));

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
        {projectBytes.constData(), static_cast<std::size_t>(projectBytes.size())});
    if (!projectId) return invalid(QStringLiteral("projectId is not canonical"));
    const DefinitionId rootId{static_cast<std::uint64_t>(rootIdValue)};

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
      return invalid(QStringLiteral("v8 requires exactly three capabilities"));
    const QStringList expected{QStringLiteral("linked-parts-v1"),
                               QStringLiteral("nested-assemblies-v1"),
                               QStringLiteral("product-envelope-v1")};
    QStringList capabilities;
    QByteArray previous;
    for (qsizetype i = 0; i < 3; ++i) {
      QString capability;
      if (!boundedString(capabilitiesValue.toArray().at(i), false, &capability,
                         QStringLiteral("$.capabilities[%1]").arg(i), &error))
        return invalid(std::move(error));
      const auto encoded = capability.toUtf8();
      if (encoded.size() > kMaximumCapabilityUtf8Bytes ||
          (i > 0 && !byteLess(previous, encoded)) || capability != expected[i])
        return invalid(QStringLiteral("v8 capability set/order is invalid"));
      previous = encoded;
      capabilities.push_back(capability);
    }

    const auto definitionsValue = root.value(QStringLiteral("definitions"));
    if (!definitionsValue.isArray() || definitionsValue.toArray().isEmpty() ||
        definitionsValue.toArray().size() >
            static_cast<qsizetype>(kAssemblyGraphMaximumDefinitions))
      return invalid(QStringLiteral("definition count exceeds v8 budget"));
    const QJsonArray definitions = definitionsValue.toArray();
    // Validate every potentially large part cache, linked locator and rebuild
    // budget before allocating decoded graph occurrences/paths.
    QJsonArray partPreflightDefinitions;
    for (const auto& value : definitions) {
      if (!value.isObject())
        return invalid(QStringLiteral("v8 definition must be an object"));
      const QJsonObject definition = value.toObject();
      qint64 definitionIdValue = 0;
      QString kind;
      if (!persistedId(definition.value(QStringLiteral("definitionId")),
                       &definitionIdValue,
                       QStringLiteral("preflight.definitionId"), &error) ||
          !boundedString(definition.value(QStringLiteral("definitionKind")),
                         false, &kind,
                         QStringLiteral("preflight.definitionKind"), &error))
        return invalid(std::move(error));
      if (kind == QStringLiteral("part")) {
        partPreflightDefinitions.push_back(definition);
      } else if (kind == QStringLiteral("assembly") &&
                 definitionIdValue == rootIdValue) {
        partPreflightDefinitions.push_back(QJsonObject{
            {QStringLiteral("definitionId"), definitionIdValue},
            {QStringLiteral("definitionKind"), QStringLiteral("assembly")},
            {QStringLiteral("occurrences"), QJsonArray{}}});
      }
    }
    const QJsonObject partPreflightRoot{
        {QStringLiteral("format"), QStringLiteral("solidar-project")},
        {QStringLiteral("version"), 7},
        {QStringLiteral("capabilities"),
         QJsonArray{QStringLiteral("flat-assembly-v1"),
                    QStringLiteral("linked-parts-v1"),
                    QStringLiteral("product-envelope-v1")}},
        {QStringLiteral("projectId"), projectText},
        {QStringLiteral("rootDefinitionId"), rootIdValue},
        {QStringLiteral("metadata"), metadataValue.toObject()},
        {QStringLiteral("definitions"), partPreflightDefinitions}};
    const QByteArray partPreflightBytes =
        QJsonDocument(partPreflightRoot).toJson(QJsonDocument::Compact);
    const auto partPreflight =
        scanProductEnvelopeV7(QByteArrayView(partPreflightBytes));
    if (partPreflight.status != ProductEnvelopeV7ScanStatus::Candidate ||
        !partPreflight.candidate)
      return invalid(QStringLiteral("v8 part preflight rejected: %1")
                         .arg(partPreflight.error));

    ProductEnvelopeV8Candidate candidate;
    candidate.authoredRoot = root;
    candidate.projectId = *projectId;
    candidate.rootDefinitionId = rootId;
    candidate.metadata = metadata;
    candidate.capabilities = capabilities;
    AssemblyGraphView graph;
    graph.rootDefinitionId = rootId;
    qint64 previousDefinitionId = 0;
    std::size_t occurrenceTotal = 0;
    for (qsizetype definitionIndex = 0;
         definitionIndex < definitions.size(); ++definitionIndex) {
      const QString path =
          QStringLiteral("$.definitions[%1]").arg(definitionIndex);
      if (!definitions.at(definitionIndex).isObject())
        return invalid(path + QStringLiteral(" must be an object"));
      const QJsonObject definition = definitions.at(definitionIndex).toObject();
      qint64 definitionIdValue = 0;
      QString kind;
      if (!persistedId(definition.value(QStringLiteral("definitionId")),
                       &definitionIdValue, path + QStringLiteral(".definitionId"),
                       &error) ||
          !boundedString(definition.value(QStringLiteral("definitionKind")),
                         false, &kind,
                         path + QStringLiteral(".definitionKind"), &error))
        return invalid(std::move(error));
      if (definitionIdValue <= previousDefinitionId)
        return invalid(QStringLiteral("definitions must be strictly sorted"));
      previousDefinitionId = definitionIdValue;
      const DefinitionId definitionId{
          static_cast<std::uint64_t>(definitionIdValue)};
      if (kind == QStringLiteral("part")) {
        if (!exactFields(definition,
                         {"definitionId", "definitionKind", "payload"}, path,
                         &error))
          return invalid(std::move(error));
        graph.definitions.emplace(definitionId,
                                  AssemblyGraphDefinition{DefinitionKind::Part,
                                                          {}});
        continue;
      }
      if (kind != QStringLiteral("assembly") ||
          !exactFields(definition,
                       {"definitionId", "definitionKind", "occurrences"}, path,
                       &error))
        return invalid(error.isEmpty() ? path + QStringLiteral(" has invalid kind")
                                       : std::move(error));
      ProductEnvelopeV7AssemblyCandidate assembly;
      assembly.definitionId = definitionId;
      assembly.authoredDefinition = definition;
      AssemblyGraphDefinition graphAssembly{DefinitionKind::Assembly, {}};
      auto decoded = detail::decodeStrictOccurrences(
          definition.value(QStringLiteral("occurrences")), path,
          occurrenceTotal, kAssemblyGraphMaximumAuthoredOccurrences);
      if (!decoded) return invalid(std::move(decoded.error));
      occurrenceTotal = decoded.aggregateAuthoredOccurrences;
      assembly.occurrences = std::move(decoded.occurrences);
      graphAssembly.occurrences.reserve(assembly.occurrences.size());
      for (const auto& occurrence : assembly.occurrences)
        graphAssembly.occurrences.push_back(
            {occurrence.occurrenceId, occurrence.targetDefinitionId,
             occurrence.placement, occurrence.flags});
      graph.definitions.emplace(definitionId, std::move(graphAssembly));
      candidate.assemblies.push_back(std::move(assembly));
    }
    if (!graph.definitions.contains(rootId))
      return invalid(QStringLiteral("root definition is dangling"));

    std::set<ExternalSourceKey> keys;
    for (const auto& part : partPreflight.candidate->parts) {
      if (part.binding && !keys.insert(part.binding->key).second)
        return invalid(QStringLiteral("duplicate external source identity"));
    }
    candidate.parts = partPreflight.candidate->parts;
    candidate.partPayloadBytes = partPreflight.candidate->partPayloadBytes;
    candidate.relativePathUtf8Bytes =
        partPreflight.candidate->relativePathUtf8Bytes;
    candidate.rebuildWork = partPreflight.candidate->rebuildWork;
    candidate.graph = analyzeAssemblyGraph(graph);
    if (!candidate.graph)
      return invalid(QStringLiteral("nested assembly graph rejected (%1)")
                         .arg(static_cast<int>(candidate.graph.status)));
    candidate.jsonStringUtf8Bytes = stringBytes;
    ProductEnvelopeV8ScanResult result;
    result.status = ProductEnvelopeV8ScanStatus::Candidate;
    result.candidate = std::move(candidate);
    return result;
  } catch (const std::exception& exception) {
    return invalid(QStringLiteral("v8 scan failure: %1")
                       .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return invalid(QStringLiteral("unknown v8 scan failure"));
  }
}

}  // namespace solidar::project
