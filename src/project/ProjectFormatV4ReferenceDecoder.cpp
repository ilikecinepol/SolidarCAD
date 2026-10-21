#include "project/ProjectFormatV4ReferenceDecoder.h"
#include "project/ProjectFormatV4ReferenceEncoder.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "project/ProjectFile.h"

namespace solidar::project {
namespace {

constexpr qint64 kMaximumExactJsonInteger = 9007199254740991LL;
constexpr qsizetype kMaximumLocalNameUtf8Bytes = 16384;
constexpr std::size_t kMaximumReferenceLocalNameUtf8Bytes = 256 * 1024;
constexpr double kMaximumCoordinateMagnitude = 1000000.0;
constexpr double kMaximumMeasure = 1.0e18;
constexpr double kUnitTolerance = 1.0e-6;

ProjectFormatV4ReferenceDecodeResult failure(
    ProjectFormatV4ReferenceDecodeStatus status, QString error) {
  ProjectFormatV4ReferenceDecodeResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

ProjectFormatV4ReferenceDecodeResult knownUnsupported(
    ProjectFormatV4ReferenceInspection inspection,
    std::size_t provenanceRecordCount, std::size_t localNameUtf8Bytes,
    QString error) {
  ProjectFormatV4ReferenceDecodeResult result;
  result.status = ProjectFormatV4ReferenceDecodeStatus::Unsupported;
  result.inspection = std::move(inspection);
  result.provenanceRecordCount = provenanceRecordCount;
  result.localNameUtf8Bytes = localNameUtf8Bytes;
  result.error = std::move(error);
  return result;
}

bool containsField(std::initializer_list<const char*> fields,
                   const QString& value) {
  return std::any_of(fields.begin(), fields.end(), [&](const char* field) {
    return value == QLatin1String(field);
  });
}

bool exactFields(const QJsonObject& object,
                 std::initializer_list<const char*> required,
                 std::initializer_list<const char*> optional,
                 QString* error) {
  for (const char* field : required) {
    if (!object.contains(QLatin1String(field))) {
      if (error)
        *error = QStringLiteral("Missing required field: %1")
                     .arg(QString::fromLatin1(field));
      return false;
    }
  }
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!containsField(required, it.key()) &&
        !containsField(optional, it.key())) {
      if (error)
        *error = QStringLiteral("Unexpected field: %1").arg(it.key());
      return false;
    }
  }
  return true;
}

bool exactInteger(const QJsonValue& value, qint64 minimum, qint64 maximum,
                  qint64* decoded, QString* error, const QString& field) {
  if (!value.isDouble()) {
    if (error) *error = field + QStringLiteral(" must be an exact integer");
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(maximum)) {
    if (error) *error = field + QStringLiteral(" is outside its integer range");
    return false;
  }
  if (decoded) *decoded = static_cast<qint64>(number);
  return true;
}

bool finiteNumber(const QJsonValue& value, double minimum, double maximum,
                  double* decoded, QString* error, const QString& field) {
  if (!value.isDouble()) {
    if (error) *error = field + QStringLiteral(" must be numeric");
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < minimum || number > maximum) {
    if (error) *error = field + QStringLiteral(" is outside its numeric range");
    return false;
  }
  if (decoded) *decoded = number;
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

bool topologyPoint(const QJsonValue& value, TopologyPoint3d* decoded,
                   QString* error, const QString& field) {
  if (!value.isArray()) {
    if (error) *error = field + QStringLiteral(" must be a 3-vector");
    return false;
  }
  const auto array = value.toArray();
  if (array.size() != 3) {
    if (error) *error = field + QStringLiteral(" must contain three values");
    return false;
  }
  std::array<double, 3> coordinates{};
  for (qsizetype index = 0; index < 3; ++index) {
    if (!finiteNumber(array.at(index), -kMaximumCoordinateMagnitude,
                      kMaximumCoordinateMagnitude,
                      &coordinates[static_cast<std::size_t>(index)], error,
                      field))
      return false;
  }
  if (decoded)
    *decoded = {coordinates[0], coordinates[1], coordinates[2]};
  return true;
}

bool boundsValue(const QJsonValue& value, TopologyBounds* decoded,
                 QString* error) {
  if (!value.isObject()) {
    if (error) *error = QStringLiteral("bounds must be an object");
    return false;
  }
  const auto object = value.toObject();
  if (!exactFields(object, {"minimum", "maximum"}, {}, error)) return false;
  TopologyBounds bounds;
  if (!topologyPoint(object.value(QStringLiteral("minimum")), &bounds.minimum,
                     error, QStringLiteral("bounds.minimum")) ||
      !topologyPoint(object.value(QStringLiteral("maximum")), &bounds.maximum,
                     error, QStringLiteral("bounds.maximum")))
    return false;
  if (bounds.minimum.x > bounds.maximum.x ||
      bounds.minimum.y > bounds.maximum.y ||
      bounds.minimum.z > bounds.maximum.z) {
    if (error) *error = QStringLiteral("bounds minimum exceeds maximum");
    return false;
  }
  if (decoded) *decoded = bounds;
  return true;
}

bool unitDirection(const TopologyPoint3d& point) {
  return std::abs(std::hypot(point.x, point.y, point.z) - 1.0) <=
         kUnitTolerance;
}

bool faceSignature(const QJsonValue& value, FaceSignature* decoded,
                   QString* error) {
  if (!value.isObject()) {
    if (error) *error = QStringLiteral("face signature must be an object");
    return false;
  }
  const auto object = value.toObject();
  if (!exactFields(object,
                   {"surfaceType", "area", "centroid", "normal", "bounds",
                    "radius", "axis"},
                   {}, error))
    return false;
  qint64 surface = 0;
  FaceSignature signature;
  if (!exactInteger(object.value(QStringLiteral("surfaceType")), 0,
                    static_cast<qint64>(SurfaceKind::Cylinder), &surface,
                    error, QStringLiteral("surfaceType")) ||
      !finiteNumber(object.value(QStringLiteral("area")), 0.0,
                    kMaximumMeasure, &signature.area, error,
                    QStringLiteral("area")) ||
      !topologyPoint(object.value(QStringLiteral("centroid")),
                     &signature.centroid, error, QStringLiteral("centroid")) ||
      !topologyPoint(object.value(QStringLiteral("normal")), &signature.normal,
                     error, QStringLiteral("normal")) ||
      !boundsValue(object.value(QStringLiteral("bounds")), &signature.bounds,
                   error) ||
      !finiteNumber(object.value(QStringLiteral("radius")), 0.0,
                    kMaximumCoordinateMagnitude, &signature.radius, error,
                    QStringLiteral("radius")) ||
      !topologyPoint(object.value(QStringLiteral("axis")), &signature.axis,
                     error, QStringLiteral("axis")))
    return false;
  signature.surface = static_cast<SurfaceKind>(surface);
  if (signature.surface == SurfaceKind::Plane &&
      !unitDirection(signature.normal)) {
    if (error) *error = QStringLiteral("plane normal must be unit length");
    return false;
  }
  if (signature.surface == SurfaceKind::Cylinder &&
      (!unitDirection(signature.axis) || signature.radius <= 0.0)) {
    if (error) *error = QStringLiteral("cylinder axis/radius is invalid");
    return false;
  }
  if (decoded) *decoded = signature;
  return true;
}

bool edgeSignature(const QJsonValue& value, EdgeSignature* decoded,
                   QString* error) {
  if (!value.isObject()) {
    if (error) *error = QStringLiteral("edge signature must be an object");
    return false;
  }
  const auto object = value.toObject();
  if (!exactFields(object,
                   {"curveType", "length", "midpoint", "tangent", "bounds",
                    "radius", "center"},
                   {}, error))
    return false;
  qint64 curve = 0;
  EdgeSignature signature;
  if (!exactInteger(object.value(QStringLiteral("curveType")), 0,
                    static_cast<qint64>(CurveKind::Other), &curve, error,
                    QStringLiteral("curveType")) ||
      !finiteNumber(object.value(QStringLiteral("length")), 0.0,
                    kMaximumMeasure, &signature.length, error,
                    QStringLiteral("length")) ||
      !topologyPoint(object.value(QStringLiteral("midpoint")),
                     &signature.midpoint, error, QStringLiteral("midpoint")) ||
      !topologyPoint(object.value(QStringLiteral("tangent")),
                     &signature.tangent, error, QStringLiteral("tangent")) ||
      !boundsValue(object.value(QStringLiteral("bounds")), &signature.bounds,
                   error) ||
      !finiteNumber(object.value(QStringLiteral("radius")), 0.0,
                    kMaximumCoordinateMagnitude, &signature.radius, error,
                    QStringLiteral("radius")) ||
      !topologyPoint(object.value(QStringLiteral("center")), &signature.center,
                     error, QStringLiteral("center")))
    return false;
  signature.curve = static_cast<CurveKind>(curve);
  if (!unitDirection(signature.tangent)) {
    if (error) *error = QStringLiteral("edge tangent must be unit length");
    return false;
  }
  if (signature.curve == CurveKind::Circle && signature.radius <= 0.0) {
    if (error) *error = QStringLiteral("circle radius must be positive");
    return false;
  }
  if (decoded) *decoded = signature;
  return true;
}

template <typename Enum>
struct TokenValue {
  std::optional<Enum> value;
  bool structurallyValid{};
  bool unsupported{};
};

template <typename Enum, std::size_t Size>
struct TokenTable {
  std::array<std::pair<std::string_view, Enum>, Size> entries;
};

std::optional<QString> boundedTokenString(const QJsonValue& value);

constexpr TokenTable<TopologyKind, 2> kKindTokens{{{
    {"Face", TopologyKind::Face},
    {"Edge", TopologyKind::Edge},
}}};

constexpr TokenTable<TopologyProvenanceOperation, 5> kOperationTokens{{{
    {"Seed", TopologyProvenanceOperation::Seed},
    {"Generated", TopologyProvenanceOperation::Generated},
    {"Modified", TopologyProvenanceOperation::Modified},
    {"Retained", TopologyProvenanceOperation::Retained},
    {"Merge", TopologyProvenanceOperation::Merge},
}}};

constexpr TokenTable<TopologySemanticRole, 8> kRoleTokens{{{
    {"SketchBoundary", TopologySemanticRole::SketchBoundary},
    {"Cap", TopologySemanticRole::Cap},
    {"Side", TopologySemanticRole::Side},
    {"Original", TopologySemanticRole::Original},
    {"Reflected", TopologySemanticRole::Reflected},
    {"PatternInstance", TopologySemanticRole::PatternInstance},
    {"BooleanResult", TopologySemanticRole::BooleanResult},
    {"Custom", TopologySemanticRole::Custom},
}}};

template <typename Enum, std::size_t Size>
TokenValue<Enum> decodedToken(
    const QJsonValue& value, const TokenTable<Enum, Size>& table) {
  const auto token = boundedTokenString(value);
  if (!token) return {};
  for (const auto& [name, decoded] : table.entries) {
    if (*token == QLatin1String(name.data(), static_cast<qsizetype>(name.size())))
      return {decoded, true, false};
  }
  return {std::nullopt, true, true};
}

template <typename Enum, std::size_t Size>
std::optional<QString> encodedToken(Enum value,
                                    const TokenTable<Enum, Size>& table) {
  for (const auto& [name, candidate] : table.entries) {
    if (candidate == value)
      return QString::fromLatin1(name.data(),
                                 static_cast<qsizetype>(name.size()));
  }
  return std::nullopt;
}

std::optional<QString> boundedTokenString(const QJsonValue& value) {
  if (!value.isString()) return std::nullopt;
  const QString token = value.toString();
  if (token.isEmpty() ||
      token.size() > ProjectFile::kMaximumStringCharacters ||
      !validUtf16(token))
    return std::nullopt;
  return token;
}

TokenValue<TopologyKind> kindToken(const QJsonValue& value) {
  return decodedToken(value, kKindTokens);
}

TokenValue<TopologyProvenanceOperation> operationToken(
    const QJsonValue& value) {
  return decodedToken(value, kOperationTokens);
}

TokenValue<TopologySemanticRole> roleToken(const QJsonValue& value) {
  return decodedToken(value, kRoleTokens);
}

struct ParsedProvenance {
  FeatureId featureId{kInvalidFeatureId};
  TokenValue<TopologyKind> kind;
  TokenValue<TopologyProvenanceOperation> operation;
  TokenValue<TopologySemanticRole> role;
  QByteArray localName;
  std::optional<TopologyOccurrenceKey> occurrence;
  QJsonObject raw;
};

bool instanceKey(const QJsonValue& value, FeatureId owner,
                 std::optional<TopologyOccurrenceKey>* decoded,
                 QString* error) {
  if (value.isUndefined()) {
    decoded->reset();
    return true;
  }
  if (!value.isObject()) {
    if (error) *error = QStringLiteral("instanceKey must be an object");
    return false;
  }
  const auto object = value.toObject();
  if (!exactFields(object, {"featureId", "slot"}, {}, error)) return false;
  qint64 featureId = 0;
  qint64 slot = 0;
  if (!exactInteger(object.value(QStringLiteral("featureId")), 1,
                    ProjectFile::kMaximumPersistedId, &featureId, error,
                    QStringLiteral("instanceKey.featureId")) ||
      !exactInteger(object.value(QStringLiteral("slot")), 0,
                    kMaximumExactJsonInteger, &slot, error,
                    QStringLiteral("instanceKey.slot")))
    return false;
  if (static_cast<FeatureId>(featureId) != owner) {
    if (error) *error = QStringLiteral("instanceKey owner does not match");
    return false;
  }
  *decoded = TopologyOccurrenceKey{static_cast<FeatureId>(featureId),
                                   static_cast<std::uint64_t>(slot)};
  return true;
}

bool provenanceFields(const QJsonObject& object, bool origin,
                      ParsedProvenance* decoded, bool* sawUnsupported,
                      std::size_t* localNameBytes, QString* error) {
  if (!exactFields(object,
                   origin ? std::initializer_list<const char*>{
                                "featureId", "kind", "operation", "role",
                                "localName"}
                          : std::initializer_list<const char*>{
                                "featureId", "kind", "operation", "role",
                                "localName", "origins"},
                   {"instanceKey"}, error))
    return false;
  qint64 featureId = 0;
  if (!exactInteger(object.value(QStringLiteral("featureId")), 1,
                    ProjectFile::kMaximumPersistedId, &featureId, error,
                    QStringLiteral("featureId")))
    return false;
  const auto kind = kindToken(object.value(QStringLiteral("kind")));
  const auto operation =
      operationToken(object.value(QStringLiteral("operation")));
  const auto role = roleToken(object.value(QStringLiteral("role")));
  if (!kind.structurallyValid || !operation.structurallyValid ||
      !role.structurallyValid) {
    if (error) *error = QStringLiteral("provenance enum token must be a string");
    return false;
  }
  if (!object.value(QStringLiteral("localName")).isString()) {
    if (error) *error = QStringLiteral("localName must be a string");
    return false;
  }
  const QString name = object.value(QStringLiteral("localName")).toString();
  if (name.isEmpty() || !validUtf16(name)) {
    if (error) *error = QStringLiteral("localName is empty or invalid UTF-16");
    return false;
  }
  const QByteArray encoded = name.toUtf8();
  if (encoded.size() > kMaximumLocalNameUtf8Bytes) {
    if (error) *error = QStringLiteral("localName exceeds its UTF-8 limit");
    return false;
  }
  const auto delta = static_cast<std::size_t>(encoded.size());
  if (*localNameBytes > kMaximumReferenceLocalNameUtf8Bytes - delta) {
    if (error) *error = QStringLiteral("reference local-name budget exceeded");
    return false;
  }
  *localNameBytes += delta;
  decoded->featureId = static_cast<FeatureId>(featureId);
  decoded->kind = kind;
  decoded->operation = operation;
  decoded->role = role;
  decoded->localName = encoded;
  decoded->raw = object;
  if (!instanceKey(object.value(QStringLiteral("instanceKey")),
                   decoded->featureId, &decoded->occurrence, error))
    return false;
  *sawUnsupported = *sawUnsupported || kind.unsupported ||
                    operation.unsupported || role.unsupported;
  return true;
}

int compareUtf8(const QByteArray& left, const QByteArray& right) {
  const qsizetype common = std::min(left.size(), right.size());
  for (qsizetype index = 0; index < common; ++index) {
    const auto a = static_cast<unsigned char>(left.at(index));
    const auto b = static_cast<unsigned char>(right.at(index));
    if (a < b) return -1;
    if (a > b) return 1;
  }
  if (left.size() < right.size()) return -1;
  if (left.size() > right.size()) return 1;
  return 0;
}

template <typename Enum>
std::optional<int> compareToken(const TokenValue<Enum>& left,
                                const TokenValue<Enum>& right,
                                const QJsonValue& leftRaw,
                                const QJsonValue& rightRaw) {
  if (left.value && right.value) {
    if (*left.value < *right.value) return -1;
    if (*right.value < *left.value) return 1;
    return 0;
  }
  // Equal unknown tokens do not obstruct comparison of later tuple members.
  // Unequal unknown tokens have no enum order in this reader, so ordering is
  // indeterminate rather than silently normalized by their spelling.
  if (leftRaw == rightRaw) return 0;
  return std::nullopt;
}

std::optional<int> compareProvenance(const ParsedProvenance& left,
                                     const ParsedProvenance& right) {
  if (left.featureId < right.featureId) return -1;
  if (right.featureId < left.featureId) return 1;
  const auto kinds = compareToken(left.kind, right.kind,
                                  left.raw.value(QStringLiteral("kind")),
                                  right.raw.value(QStringLiteral("kind")));
  if (!kinds || *kinds != 0) return kinds;
  const auto operations = compareToken(
      left.operation, right.operation,
      left.raw.value(QStringLiteral("operation")),
      right.raw.value(QStringLiteral("operation")));
  if (!operations || *operations != 0) return operations;
  const auto roles = compareToken(left.role, right.role,
                                  left.raw.value(QStringLiteral("role")),
                                  right.raw.value(QStringLiteral("role")));
  if (!roles || *roles != 0) return roles;
  const int names = compareUtf8(left.localName, right.localName);
  if (names != 0) return names;
  if (left.occurrence.has_value() != right.occurrence.has_value())
    return left.occurrence.has_value() ? 1 : -1;
  if (!left.occurrence) return 0;
  if (left.occurrence->featureId != right.occurrence->featureId)
    return left.occurrence->featureId < right.occurrence->featureId ? -1 : 1;
  if (left.occurrence->slot < right.occurrence->slot) return -1;
  if (right.occurrence->slot < left.occurrence->slot) return 1;
  return 0;
}

TopologyProvenanceOrigin materializeOrigin(const ParsedProvenance& value) {
  TopologyProvenanceOrigin result;
  result.featureId = value.featureId;
  result.kind = *value.kind.value;
  result.operation = *value.operation.value;
  result.role = *value.role.value;
  result.localName = value.localName.toStdString();
  result.occurrence = value.occurrence;
  return result;
}

ProjectFormatV4ReferenceDecodeResult decodeV1(const QJsonObject& object,
                                               TopologyKind expectedKind) {
  QString error;
  if (!exactFields(object,
                   {"referenceVersion", "bodyId", "featureId", "kind",
                    "legacyIndex"},
                   {"persistentTag", "signature"}, &error))
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   std::move(error));
  qint64 bodyId = 0;
  qint64 featureId = 0;
  qint64 legacyIndex = 0;
  if (!exactInteger(object.value(QStringLiteral("bodyId")), 1,
                    ProjectFile::kMaximumPersistedId, &bodyId, &error,
                    QStringLiteral("bodyId")) ||
      !exactInteger(object.value(QStringLiteral("featureId")), 1,
                    ProjectFile::kMaximumPersistedId, &featureId, &error,
                    QStringLiteral("featureId")) ||
      !exactInteger(object.value(QStringLiteral("legacyIndex")), 0,
                    kMaximumExactJsonInteger, &legacyIndex, &error,
                    QStringLiteral("legacyIndex")))
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   std::move(error));

  const auto kind = kindToken(object.value(QStringLiteral("kind")));
  if (!kind.structurallyValid)
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QStringLiteral("kind must be a string"));

  std::string persistentTag;
  if (object.contains(QStringLiteral("persistentTag"))) {
    const auto value = object.value(QStringLiteral("persistentTag"));
    if (!value.isString() || value.toString().isEmpty() ||
        value.toString().size() > ProjectFile::kMaximumStringCharacters ||
        !validUtf16(value.toString()))
      return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                     QStringLiteral("persistentTag is invalid"));
    persistentTag = value.toString().toUtf8().toStdString();
  }

  std::optional<FaceSignature> decodedFace;
  std::optional<EdgeSignature> decodedEdge;
  if (object.contains(QStringLiteral("signature"))) {
    if (kind.value && *kind.value == TopologyKind::Face) {
      FaceSignature signature;
      if (!faceSignature(object.value(QStringLiteral("signature")), &signature,
                         &error))
        return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                       std::move(error));
      decodedFace = signature;
    } else if (kind.value && *kind.value == TopologyKind::Edge) {
      EdgeSignature signature;
      if (!edgeSignature(object.value(QStringLiteral("signature")), &signature,
                         &error))
        return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                       std::move(error));
      decodedEdge = signature;
    } else {
      // The signature schema is still closed for known V1 even when the kind
      // token itself is from a future vocabulary. Accept either current
      // structural alternative only for purposes of reaching Unsupported.
      FaceSignature face;
      EdgeSignature edge;
      QString faceError;
      QString edgeError;
      if (!faceSignature(object.value(QStringLiteral("signature")), &face,
                         &faceError) &&
          !edgeSignature(object.value(QStringLiteral("signature")), &edge,
                         &edgeError))
        return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                       QStringLiteral("signature does not match a V1 schema"));
    }
  }

  if (kind.unsupported)
    return knownUnsupported(
        {ProjectFormatV4ReferenceVariant::V1, static_cast<BodyId>(bodyId),
         static_cast<FeatureId>(featureId), {}},
        0, 0, QStringLiteral("unsupported topology kind token"));
  if (*kind.value != expectedKind)
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QStringLiteral("topology kind does not match embedding"));

  TopologyReference reference;
  reference.bodyId = static_cast<BodyId>(bodyId);
  reference.featureId = static_cast<FeatureId>(featureId);
  reference.kind = expectedKind;
  reference.legacyIndex = static_cast<std::size_t>(legacyIndex);
  reference.persistentTag = std::move(persistentTag);
  reference.faceSignature = std::move(decodedFace);
  reference.edgeSignature = std::move(decodedEdge);

  ProjectFormatV4ReferenceDecodeResult result;
  result.status = ProjectFormatV4ReferenceDecodeStatus::Valid;
  result.success = true;
  result.inspection = ProjectFormatV4ReferenceInspection{
      ProjectFormatV4ReferenceVariant::V1, static_cast<BodyId>(bodyId),
      static_cast<FeatureId>(featureId), {}};
  if (expectedKind == TopologyKind::Face)
    result.reference = FaceReference::fromV1(std::move(reference));
  else
    result.reference = EdgeReference::fromV1(std::move(reference));
  return result;
}

ProjectFormatV4ReferenceDecodeResult decodeV2(const QJsonObject& object,
                                               TopologyKind expectedKind) {
  QString error;
  if (!exactFields(object, {"referenceVersion", "bodyId", "provenance"}, {},
                   &error))
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   std::move(error));
  qint64 bodyId = 0;
  if (!exactInteger(object.value(QStringLiteral("bodyId")), 1,
                    ProjectFile::kMaximumPersistedId, &bodyId, &error,
                    QStringLiteral("bodyId")))
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   std::move(error));
  if (!object.value(QStringLiteral("provenance")).isObject())
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QStringLiteral("provenance must be an object"));

  const auto provenanceObject =
      object.value(QStringLiteral("provenance")).toObject();
  bool sawUnsupported = false;
  std::size_t localNameBytes = 0;
  ParsedProvenance top;
  if (!provenanceFields(provenanceObject, false, &top, &sawUnsupported,
                        &localNameBytes, &error))
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   std::move(error));
  const auto originsValue = provenanceObject.value(QStringLiteral("origins"));
  if (!originsValue.isArray())
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QStringLiteral("origins must be an array"));
  const auto originsArray = originsValue.toArray();
  if (originsArray.size() >
      static_cast<qsizetype>(kMaximumTopologyImmediateOrigins))
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QStringLiteral("too many immediate origins"));

  std::vector<ParsedProvenance> origins;
  origins.reserve(static_cast<std::size_t>(originsArray.size()));
  for (const auto& originValue : originsArray) {
    if (!originValue.isObject())
      return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                     QStringLiteral("origin must be an object"));
    ParsedProvenance origin;
    if (!provenanceFields(originValue.toObject(), true, &origin,
                          &sawUnsupported, &localNameBytes, &error))
      return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                     std::move(error));
    origins.push_back(std::move(origin));
  }

  for (std::size_t left = 0; left < origins.size(); ++left) {
    for (std::size_t right = left + 1; right < origins.size(); ++right) {
      if (origins[left].raw == origins[right].raw)
        return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                       QStringLiteral("duplicate provenance origin"));
      const auto order = compareProvenance(origins[left], origins[right]);
      if (order && *order >= 0)
        return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                       QStringLiteral("provenance origins are not canonical"));
    }
  }

  if (top.kind.value && *top.kind.value != expectedKind)
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QStringLiteral("topology kind does not match embedding"));
  ProjectFormatV4ReferenceInspection inspection{
      ProjectFormatV4ReferenceVariant::V2, static_cast<BodyId>(bodyId),
      top.featureId, {}};
  inspection.immediateOriginFeatureIds.reserve(origins.size());
  for (const auto& origin : origins)
    inspection.immediateOriginFeatureIds.push_back(origin.featureId);
  const std::size_t provenanceRecordCount = 1 + origins.size();
  if (sawUnsupported)
    return knownUnsupported(std::move(inspection), provenanceRecordCount,
                            localNameBytes,
                            QStringLiteral("unsupported provenance token"));

  TopologyProvenanceKey key;
  key.featureId = top.featureId;
  key.kind = *top.kind.value;
  key.operation = *top.operation.value;
  key.role = *top.role.value;
  key.localName = top.localName.toStdString();
  key.occurrence = top.occurrence;
  key.origins.reserve(origins.size());
  for (const auto& origin : origins)
    key.origins.push_back(materializeOrigin(origin));
  std::string keyError;
  if (!key.isCanonical(&keyError))
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QString::fromStdString(keyError));

  ProjectFormatV4ReferenceDecodeResult result;
  result.status = ProjectFormatV4ReferenceDecodeStatus::Valid;
  result.success = true;
  result.inspection = std::move(inspection);
  result.provenanceRecordCount = provenanceRecordCount;
  result.localNameUtf8Bytes = localNameBytes;
  TopologyReferenceV2 reference{static_cast<BodyId>(bodyId), std::move(key)};
  if (expectedKind == TopologyKind::Face)
    result.reference = FaceReference::fromV2(std::move(reference));
  else
    result.reference = EdgeReference::fromV2(std::move(reference));
  return result;
}

ProjectFormatV4ReferenceEncodeResult encodeFailure(QString error) {
  ProjectFormatV4ReferenceEncodeResult result;
  result.error = std::move(error);
  return result;
}

ProjectFormatV4ReferenceEncodeResult encodeSuccess(
    QJsonObject reference, std::size_t provenanceRecordCount = 0,
    std::size_t localNameUtf8Bytes = 0) {
  ProjectFormatV4ReferenceEncodeResult result;
  result.status = ProjectFormatV4ReferenceEncodeStatus::Valid;
  result.reference = std::move(reference);
  result.provenanceRecordCount = provenanceRecordCount;
  result.localNameUtf8Bytes = localNameUtf8Bytes;
  return result;
}

QJsonArray savedPoint(const TopologyPoint3d& point) {
  return {point.x, point.y, point.z};
}

QJsonObject savedBounds(const TopologyBounds& bounds) {
  return {{"minimum", savedPoint(bounds.minimum)},
          {"maximum", savedPoint(bounds.maximum)}};
}

QJsonObject savedFaceSignature(const FaceSignature& signature) {
  return {{"surfaceType", static_cast<int>(signature.surface)},
          {"area", signature.area},
          {"centroid", savedPoint(signature.centroid)},
          {"normal", savedPoint(signature.normal)},
          {"bounds", savedBounds(signature.bounds)},
          {"radius", signature.radius},
          {"axis", savedPoint(signature.axis)}};
}

QJsonObject savedEdgeSignature(const EdgeSignature& signature) {
  return {{"curveType", static_cast<int>(signature.curve)},
          {"length", signature.length},
          {"midpoint", savedPoint(signature.midpoint)},
          {"tangent", savedPoint(signature.tangent)},
          {"bounds", savedBounds(signature.bounds)},
          {"radius", signature.radius},
          {"center", savedPoint(signature.center)}};
}

bool persistedId(std::uint64_t value) {
  return value >= 1 &&
         value <= static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId);
}

bool exactUnsignedInteger(std::uint64_t value) {
  return value <= static_cast<std::uint64_t>(kMaximumExactJsonInteger);
}

std::optional<QString> savedUtf8String(const std::string& value,
                                       bool boundedByUtf16,
                                       QString* error) {
  const QByteArray bytes(value.data(), static_cast<qsizetype>(value.size()));
  const QString decoded = QString::fromUtf8(bytes);
  if (decoded.toUtf8() != bytes) {
    if (error) *error = QStringLiteral("string is not valid UTF-8");
    return std::nullopt;
  }
  if (boundedByUtf16 &&
      decoded.size() > ProjectFile::kMaximumStringCharacters) {
    if (error) *error = QStringLiteral("string exceeds its UTF-16 limit");
    return std::nullopt;
  }
  return decoded;
}

bool appendLocalName(const std::string& value, QString* encoded,
                     std::size_t* totalBytes, QString* error) {
  if (value.empty()) {
    if (error) *error = QStringLiteral("localName is empty");
    return false;
  }
  auto text = savedUtf8String(value, false, error);
  if (!text) return false;
  const std::size_t bytes = value.size();
  if (bytes > static_cast<std::size_t>(kMaximumLocalNameUtf8Bytes)) {
    if (error) *error = QStringLiteral("localName exceeds its UTF-8 limit");
    return false;
  }
  if (*totalBytes > kMaximumReferenceLocalNameUtf8Bytes - bytes) {
    if (error) *error = QStringLiteral("reference local-name budget exceeded");
    return false;
  }
  *totalBytes += bytes;
  *encoded = std::move(*text);
  return true;
}

std::optional<QJsonObject> savedOccurrence(
    const std::optional<TopologyOccurrenceKey>& occurrence, FeatureId owner,
    QString* error) {
  if (!occurrence) return QJsonObject{};
  if (occurrence->featureId != owner || !persistedId(occurrence->featureId) ||
      !exactUnsignedInteger(occurrence->slot)) {
    if (error) *error = QStringLiteral("instanceKey is invalid");
    return std::nullopt;
  }
  return QJsonObject{
      {"featureId", static_cast<qint64>(occurrence->featureId)},
      {"slot", static_cast<qint64>(occurrence->slot)}};
}

std::optional<QJsonObject> savedOrigin(
    const TopologyProvenanceOrigin& origin, std::size_t* localNameBytes,
    QString* error) {
  if (!persistedId(origin.featureId)) {
    if (error) *error = QStringLiteral("origin featureId is invalid");
    return std::nullopt;
  }
  const auto kind = encodedToken(origin.kind, kKindTokens);
  const auto operation = encodedToken(origin.operation, kOperationTokens);
  const auto role = encodedToken(origin.role, kRoleTokens);
  if (!kind || !operation || !role) {
    if (error) *error = QStringLiteral("origin enum value is invalid");
    return std::nullopt;
  }
  QString localName;
  if (!appendLocalName(origin.localName, &localName, localNameBytes, error))
    return std::nullopt;
  const auto occurrence = savedOccurrence(origin.occurrence, origin.featureId,
                                          error);
  if (!occurrence) return std::nullopt;
  QJsonObject result{{"featureId", static_cast<qint64>(origin.featureId)},
                     {"kind", *kind},
                     {"operation", *operation},
                     {"role", *role},
                     {"localName", std::move(localName)}};
  if (origin.occurrence) result[QStringLiteral("instanceKey")] = *occurrence;
  return result;
}

template <typename Reference>
ProjectFormatV4ReferenceEncodeResult encodeV1Reference(
    const Reference& carrier, TopologyKind expectedKind) {
  const TopologyReference* reference = carrier.v1();
  if (!reference || reference->kind != expectedKind ||
      !persistedId(reference->bodyId) ||
      !persistedId(reference->featureId) ||
      reference->legacyIndex >
          static_cast<std::size_t>(kMaximumExactJsonInteger))
    return encodeFailure(QStringLiteral("V1 topology reference is invalid"));

  const auto kind = encodedToken(reference->kind, kKindTokens);
  if (!kind)
    return encodeFailure(QStringLiteral("V1 topology kind is invalid"));
  QJsonObject result{{"referenceVersion", "V1"},
                     {"bodyId", static_cast<qint64>(reference->bodyId)},
                     {"featureId", static_cast<qint64>(reference->featureId)},
                     {"kind", *kind},
                     {"legacyIndex",
                      static_cast<qint64>(reference->legacyIndex)}};
  if (!reference->persistentTag.empty()) {
    QString error;
    const auto tag = savedUtf8String(reference->persistentTag, true, &error);
    if (!tag)
      return encodeFailure(QStringLiteral("persistentTag is invalid: %1")
                               .arg(error));
    result[QStringLiteral("persistentTag")] = *tag;
  }
  if (expectedKind == TopologyKind::Face) {
    if (reference->edgeSignature)
      return encodeFailure(QStringLiteral("V1 face contains an edge signature"));
    if (reference->faceSignature)
      result[QStringLiteral("signature")] =
          savedFaceSignature(*reference->faceSignature);
  } else {
    if (reference->faceSignature)
      return encodeFailure(QStringLiteral("V1 edge contains a face signature"));
    if (reference->edgeSignature)
      result[QStringLiteral("signature")] =
          savedEdgeSignature(*reference->edgeSignature);
  }

  const auto decoded =
      decodeProjectFormatV4TopologyReference(result, expectedKind);
  if (!decoded.succeeded())
    return encodeFailure(QStringLiteral("encoded V1 reference is invalid: %1")
                             .arg(decoded.error));
  if constexpr (std::is_same_v<Reference, FaceReference>) {
    if (!std::holds_alternative<FaceReference>(*decoded.reference) ||
        std::get<FaceReference>(*decoded.reference) != carrier)
      return encodeFailure(QStringLiteral("V1 face round-trip mismatch"));
  } else {
    if (!std::holds_alternative<EdgeReference>(*decoded.reference) ||
        std::get<EdgeReference>(*decoded.reference) != carrier)
      return encodeFailure(QStringLiteral("V1 edge round-trip mismatch"));
  }
  return encodeSuccess(std::move(result));
}

template <typename Reference>
ProjectFormatV4ReferenceEncodeResult encodeV2Reference(
    const Reference& carrier, TopologyKind expectedKind) {
  const TopologyReferenceV2* reference = carrier.v2();
  if (!reference || !persistedId(reference->bodyId) ||
      !persistedId(reference->provenance.featureId) ||
      reference->provenance.kind != expectedKind)
    return encodeFailure(QStringLiteral("V2 topology reference is invalid"));

  TopologyProvenanceKey key = reference->provenance;
  key.canonicalizeOrigins();
  std::string canonicalError;
  if (!key.isCanonical(&canonicalError))
    return encodeFailure(
        QStringLiteral("V2 provenance is invalid: %1")
            .arg(QString::fromLatin1(canonicalError)));
  if (key.origins.size() > kMaximumTopologyImmediateOrigins)
    return encodeFailure(QStringLiteral("too many immediate origins"));

  const auto kind = encodedToken(key.kind, kKindTokens);
  const auto operation = encodedToken(key.operation, kOperationTokens);
  const auto role = encodedToken(key.role, kRoleTokens);
  if (!kind || !operation || !role)
    return encodeFailure(QStringLiteral("V2 provenance enum value is invalid"));

  std::size_t localNameBytes = 0;
  QString localName;
  QString error;
  if (!appendLocalName(key.localName, &localName, &localNameBytes, &error))
    return encodeFailure(std::move(error));
  const auto occurrence =
      savedOccurrence(key.occurrence, key.featureId, &error);
  if (!occurrence) return encodeFailure(std::move(error));

  QJsonArray origins;
  for (const auto& origin : key.origins) {
    auto saved = savedOrigin(origin, &localNameBytes, &error);
    if (!saved) return encodeFailure(std::move(error));
    origins.append(std::move(*saved));
  }
  QJsonObject provenance{
      {"featureId", static_cast<qint64>(key.featureId)},
      {"kind", *kind},
      {"operation", *operation},
      {"role", *role},
      {"localName", std::move(localName)},
      {"origins", std::move(origins)}};
  if (key.occurrence)
    provenance[QStringLiteral("instanceKey")] = *occurrence;
  QJsonObject result{{"referenceVersion", "V2"},
                     {"bodyId", static_cast<qint64>(reference->bodyId)},
                     {"provenance", std::move(provenance)}};

  const auto decoded =
      decodeProjectFormatV4TopologyReference(result, expectedKind);
  if (!decoded.succeeded())
    return encodeFailure(QStringLiteral("encoded V2 reference is invalid: %1")
                             .arg(decoded.error));
  const std::size_t provenanceRecordCount = 1 + key.origins.size();
  TopologyReferenceV2 normalized{reference->bodyId, std::move(key)};
  if constexpr (std::is_same_v<Reference, FaceReference>) {
    const FaceReference expected = FaceReference::fromV2(std::move(normalized));
    if (!std::holds_alternative<FaceReference>(*decoded.reference) ||
        std::get<FaceReference>(*decoded.reference) != expected)
      return encodeFailure(QStringLiteral("V2 face round-trip mismatch"));
  } else {
    const EdgeReference expected = EdgeReference::fromV2(std::move(normalized));
    if (!std::holds_alternative<EdgeReference>(*decoded.reference) ||
        std::get<EdgeReference>(*decoded.reference) != expected)
      return encodeFailure(QStringLiteral("V2 edge round-trip mismatch"));
  }
  return encodeSuccess(std::move(result), provenanceRecordCount,
                       localNameBytes);
}

}  // namespace

ProjectFormatV4ReferenceDecodeResult
decodeProjectFormatV4TopologyReference(const QJsonValue& value,
                                       TopologyKind expectedKind) {
  if (!value.isObject())
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QStringLiteral("topology reference must be an object"));
  const auto object = value.toObject();
  const auto discriminator = object.value(QStringLiteral("referenceVersion"));
  if (!discriminator.isString())
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QStringLiteral("referenceVersion must be a string"));
  const QString version = discriminator.toString();
  if (version.isEmpty() ||
      version.size() > ProjectFile::kMaximumStringCharacters ||
      !validUtf16(version))
    return failure(ProjectFormatV4ReferenceDecodeStatus::Invalid,
                   QStringLiteral("referenceVersion is invalid"));
  if (version == QStringLiteral("V1")) return decodeV1(object, expectedKind);
  if (version == QStringLiteral("V2")) return decodeV2(object, expectedKind);
  return failure(ProjectFormatV4ReferenceDecodeStatus::Unsupported,
                 QStringLiteral("unsupported referenceVersion"));
}

ProjectFormatV4ReferenceEncodeResult
encodeProjectFormatV4TopologyReference(const FaceReference& reference) {
  return reference.isV1()
             ? encodeV1Reference(reference, TopologyKind::Face)
             : encodeV2Reference(reference, TopologyKind::Face);
}

ProjectFormatV4ReferenceEncodeResult
encodeProjectFormatV4TopologyReference(const EdgeReference& reference) {
  return reference.isV1()
             ? encodeV1Reference(reference, TopologyKind::Edge)
             : encodeV2Reference(reference, TopologyKind::Edge);
}

}  // namespace solidar::project
