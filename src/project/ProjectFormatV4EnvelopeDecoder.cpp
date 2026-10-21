#include "project/ProjectFormatV4EnvelopeDecoder.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "project/ProjectFile.h"

namespace solidar::project {
namespace {

constexpr qint64 kMaximumExactJsonInteger = 9007199254740991LL;
constexpr double kMaximumModelLengthMm = 100000.0;
constexpr qsizetype kMaximumCapabilities = 16;
constexpr qsizetype kMaximumCapabilityUtf8Bytes = 64;

ProjectFormatV4EnvelopeScanResult failure(
    ProjectFormatV4EnvelopeScanStatus status, QString error) {
  ProjectFormatV4EnvelopeScanResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

ProjectFormatV4EnvelopeScanResult invalid(QString error) {
  return failure(ProjectFormatV4EnvelopeScanStatus::Invalid,
                 std::move(error));
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
      if (error)
        *error = QStringLiteral("%1 is missing required field %2")
                     .arg(path, QString::fromLatin1(field));
      return false;
    }
  }
  if (object.size() != static_cast<qsizetype>(fields.size())) {
    for (auto it = object.begin(); it != object.end(); ++it) {
      if (!containsField(fields, it.key())) {
        if (error)
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

bool boundedUtf16String(const QJsonValue& value, qsizetype maximumUnits,
                        bool allowEmpty, QString* decoded, QString* error,
                        const QString& path) {
  if (!value.isString()) {
    if (error) *error = path + QStringLiteral(" must be a string");
    return false;
  }
  const QString token = value.toString();
  if ((!allowEmpty && token.isEmpty()) || token.size() > maximumUnits ||
      !validUtf16(token)) {
    if (error) *error = path + QStringLiteral(" has an invalid string value");
    return false;
  }
  if (decoded) *decoded = token;
  return true;
}

bool exactInteger(const QJsonValue& value, qint64* decoded, QString* error,
                  const QString& path) {
  if (!value.isDouble()) {
    if (error) *error = path + QStringLiteral(" must be an exact integer");
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number || number < 1.0 ||
      number > static_cast<double>(kMaximumExactJsonInteger)) {
    if (error) *error = path + QStringLiteral(" is outside its integer range");
    return false;
  }
  if (decoded) *decoded = static_cast<qint64>(number);
  return true;
}

bool positiveModelLength(const QJsonValue& value, QString* error,
                         const QString& path) {
  if (!value.isDouble()) {
    if (error) *error = path + QStringLiteral(" must be numeric");
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || number <= 0.0 ||
      number > kMaximumModelLengthMm) {
    if (error) *error = path + QStringLiteral(" is outside its numeric range");
    return false;
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

}  // namespace

ProjectFormatV4EnvelopeScanResult scanProjectFormatV4Envelope(
    QByteArrayView bytes) {
  if (bytes.isEmpty()) return invalid(QStringLiteral("input is empty"));
  if (bytes.size() > ProjectFile::kMaximumFileBytes)
    return invalid(QStringLiteral("input exceeds the project file limit"));

  QJsonParseError parseError;
  const QJsonDocument parsed = QJsonDocument::fromJson(
      QByteArray(bytes.data(), bytes.size()), &parseError);
  if (parseError.error != QJsonParseError::NoError)
    return invalid(QStringLiteral("invalid JSON: %1").arg(parseError.errorString()));
  if (!parsed.isObject())
    return invalid(QStringLiteral("project root must be an object"));
  const QJsonObject root = parsed.object();

  QString error;
  QString format;
  if (!boundedUtf16String(root.value(QStringLiteral("format")),
                          ProjectFile::kMaximumStringCharacters, false,
                          &format, &error, QStringLiteral("$.format")))
    return invalid(std::move(error));
  qint64 version = 0;
  if (!exactInteger(root.value(QStringLiteral("version")), &version, &error,
                    QStringLiteral("$.version")))
    return invalid(std::move(error));

  if (format != QStringLiteral("solidar-project") || version != 4) {
    return failure(ProjectFormatV4EnvelopeScanStatus::NotApplicable,
                   QStringLiteral("not a SolidarCAD project format v4 envelope"));
  }

  if (!exactFields(root,
                   {"format", "version", "capabilities",
                    "constraintTypeEncoding", "name", "createdAt", "document",
                    "sketches", "extrusion", "model"},
                   &error, QStringLiteral("$")))
    return invalid(std::move(error));

  QString constraintTypeEncoding;
  if (!boundedUtf16String(root.value(QStringLiteral("constraintTypeEncoding")),
                          ProjectFile::kMaximumStringCharacters, false,
                          &constraintTypeEncoding, &error,
                          QStringLiteral("$.constraintTypeEncoding")))
    return invalid(std::move(error));

  if (!boundedUtf16String(root.value(QStringLiteral("name")),
                          ProjectFile::kMaximumStringCharacters, true, nullptr,
                          &error, QStringLiteral("$.name")) ||
      !boundedUtf16String(root.value(QStringLiteral("createdAt")),
                          ProjectFile::kMaximumStringCharacters, true, nullptr,
                          &error, QStringLiteral("$.createdAt")))
    return invalid(std::move(error));

  const QJsonValue documentValue = root.value(QStringLiteral("document"));
  if (!documentValue.isObject())
    return invalid(QStringLiteral("$.document must be an object"));
  const QJsonObject document = documentValue.toObject();
  if (!exactFields(document, {"widthMm", "heightMm", "extrusionMm"},
                   &error, QStringLiteral("$.document")))
    return invalid(std::move(error));
  for (const char* field : {"widthMm", "heightMm", "extrusionMm"}) {
    if (!positiveModelLength(document.value(QLatin1String(field)), &error,
                             QStringLiteral("$.document.%1")
                                 .arg(QString::fromLatin1(field))))
      return invalid(std::move(error));
  }

  const QJsonValue sketchesValue = root.value(QStringLiteral("sketches"));
  if (!sketchesValue.isArray())
    return invalid(QStringLiteral("$.sketches must be an array"));
  const QJsonArray legacySketches = sketchesValue.toArray();
  if (legacySketches.size() > ProjectFile::kMaximumDocumentSketches)
    return invalid(QStringLiteral("$.sketches exceeds its item limit"));

  const QJsonValue extrusionValue = root.value(QStringLiteral("extrusion"));
  if (!extrusionValue.isObject())
    return invalid(QStringLiteral("$.extrusion must be an object"));

  const QJsonValue modelValue = root.value(QStringLiteral("model"));
  if (!modelValue.isObject())
    return invalid(QStringLiteral("$.model must be an object"));
  const QJsonObject model = modelValue.toObject();
  if (!exactFields(model, {"sketches", "bodies", "referenceImages"},
                   &error, QStringLiteral("$.model")))
    return invalid(std::move(error));
  for (const char* field : {"sketches", "bodies", "referenceImages"}) {
    if (!model.value(QLatin1String(field)).isArray())
      return invalid(QStringLiteral("$.model.%1 must be an array")
                         .arg(QString::fromLatin1(field)));
  }
  if (model.value(QStringLiteral("sketches")).toArray().size() >
      ProjectFile::kMaximumDocumentSketches)
    return invalid(QStringLiteral("$.model.sketches exceeds its item limit"));
  if (model.value(QStringLiteral("bodies")).toArray().size() >
      ProjectFile::kMaximumDocumentBodies)
    return invalid(QStringLiteral("$.model.bodies exceeds its item limit"));
  if (model.value(QStringLiteral("referenceImages")).toArray().size() >
      ProjectFile::kMaximumCollectionItems)
    return invalid(
        QStringLiteral("$.model.referenceImages exceeds its item limit"));

  const QJsonValue capabilitiesValue =
      root.value(QStringLiteral("capabilities"));
  if (!capabilitiesValue.isArray())
    return invalid(QStringLiteral("$.capabilities must be an array"));
  const QJsonArray capabilityValues = capabilitiesValue.toArray();
  if (capabilityValues.isEmpty() ||
      capabilityValues.size() > kMaximumCapabilities)
    return invalid(QStringLiteral("$.capabilities has an invalid item count"));

  QStringList capabilities;
  std::vector<ProjectFormatV4DeferredUnknown> deferred;
  QByteArray previous;
  bool hasPrevious = false;
  bool hasRequiredCapability = false;
  for (qsizetype index = 0; index < capabilityValues.size(); ++index) {
    QString capability;
    const QString path = QStringLiteral("$.capabilities[%1]").arg(index);
    if (!boundedUtf16String(capabilityValues.at(index),
                            ProjectFile::kMaximumStringCharacters, false,
                            &capability, &error, path))
      return invalid(std::move(error));
    const QByteArray encoded = capability.toUtf8();
    if (encoded.size() > kMaximumCapabilityUtf8Bytes)
      return invalid(path + QStringLiteral(" exceeds its UTF-8 byte limit"));
    if (hasPrevious && !unsignedUtf8Less(previous, encoded))
      return invalid(QStringLiteral("$.capabilities must be strictly sorted and unique"));
    hasPrevious = true;
    previous = encoded;
    capabilities.push_back(capability);
    if (capability == QStringLiteral("persistent-topology-v2")) {
      hasRequiredCapability = true;
    } else {
      deferred.push_back({ProjectFormatV4DeferredUnknownKind::Capability,
                          path, capability});
    }
  }
  if (!hasRequiredCapability)
    return invalid(QStringLiteral(
        "$.capabilities must contain persistent-topology-v2"));

  if (constraintTypeEncoding != QStringLiteral("stable-name-v1")) {
    deferred.push_back(
        {ProjectFormatV4DeferredUnknownKind::ConstraintTypeEncoding,
         QStringLiteral("$.constraintTypeEncoding"),
         constraintTypeEncoding});
  }

  ProjectFormatV4EnvelopeCandidate candidate;
  candidate.authoredRoot = root;
  candidate.document = document;
  candidate.legacySketches = legacySketches;
  candidate.legacyExtrusion = extrusionValue.toObject();
  candidate.model = model;
  candidate.capabilities = std::move(capabilities);
  candidate.deferredUnknowns = std::move(deferred);

  ProjectFormatV4EnvelopeScanResult result;
  result.status = ProjectFormatV4EnvelopeScanStatus::Candidate;
  result.candidate = std::move(candidate);
  return result;
}

}  // namespace solidar::project
