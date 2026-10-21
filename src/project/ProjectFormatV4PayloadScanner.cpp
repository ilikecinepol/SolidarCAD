#include "project/ProjectFormatV4PayloadScanner.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <utility>

#include "model/Feature.h"
#include "project/ProjectFile.h"

namespace solidar::project {
namespace {

constexpr qint64 kMaximumExactJsonInteger = 9007199254740991LL;
constexpr double kMaximumModelLengthMm = 100000.0;
constexpr double kMaximumCoordinateMagnitude = 1000000.0;

ProjectFormatV4PayloadScanResult invalid(QString error) {
  ProjectFormatV4PayloadScanResult result;
  result.error = std::move(error);
  return result;
}

bool fail(QString* error, const QString& path, const QString& message) {
  if (error) *error = path + QStringLiteral(": ") + message;
  return false;
}

bool contains(std::initializer_list<const char*> fields,
              const QString& field) {
  return std::any_of(fields.begin(), fields.end(), [&](const char* candidate) {
    return field == QLatin1String(candidate);
  });
}

bool exactFields(const QJsonObject& object,
                 std::initializer_list<const char*> required,
                 std::initializer_list<const char*> optional,
                 const QString& path, QString* error) {
  for (const char* field : required) {
    if (!object.contains(QLatin1String(field)))
      return fail(error, path,
                  QStringLiteral("missing required field %1")
                      .arg(QString::fromLatin1(field)));
  }
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!contains(required, it.key()) && !contains(optional, it.key()))
      return fail(error, path,
                  QStringLiteral("unexpected field %1").arg(it.key()));
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

bool boundedString(const QJsonValue& value, const QString& path,
                   bool allowEmpty, QString* error,
                   QString* decoded = nullptr) {
  if (!value.isString()) return fail(error, path, QStringLiteral("must be a string"));
  const QString text = value.toString();
  if ((!allowEmpty && text.isEmpty()) ||
      text.size() > ProjectFile::kMaximumStringCharacters ||
      !validUtf16(text))
    return fail(error, path, QStringLiteral("has an invalid string value"));
  if (decoded) *decoded = text;
  return true;
}

bool exactInteger(const QJsonValue& value, qint64 minimum, qint64 maximum,
                  const QString& path, QString* error,
                  qint64* decoded = nullptr) {
  if (!value.isDouble())
    return fail(error, path, QStringLiteral("must be an exact integer"));
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(maximum))
    return fail(error, path, QStringLiteral("is outside its integer range"));
  if (decoded) *decoded = static_cast<qint64>(number);
  return true;
}

bool persistedId(const QJsonValue& value, const QString& path, QString* error,
                 qint64* decoded = nullptr) {
  return exactInteger(value, 1, ProjectFile::kMaximumPersistedId, path, error,
                      decoded);
}

bool finiteNumber(const QJsonValue& value, double minimum, double maximum,
                  const QString& path, QString* error) {
  if (!value.isDouble()) return fail(error, path, QStringLiteral("must be numeric"));
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < minimum || number > maximum)
    return fail(error, path, QStringLiteral("is outside its numeric range"));
  return true;
}

bool booleanValue(const QJsonValue& value, const QString& path,
                  QString* error) {
  return value.isBool() || fail(error, path, QStringLiteral("must be boolean"));
}

bool vector3(const QJsonValue& value, const QString& path, QString* error) {
  if (!value.isArray()) return fail(error, path, QStringLiteral("must be a 3-vector"));
  const QJsonArray coordinates = value.toArray();
  if (coordinates.size() != 3)
    return fail(error, path, QStringLiteral("must contain three coordinates"));
  for (qsizetype index = 0; index < 3; ++index)
    if (!finiteNumber(coordinates.at(index), -kMaximumCoordinateMagnitude,
                      kMaximumCoordinateMagnitude,
                      QStringLiteral("%1[%2]").arg(path).arg(index), error))
      return false;
  return true;
}

bool placementBasis(const QJsonValue& xValue, const QJsonValue& yValue,
                    const QString& path, QString* error) {
  const QJsonArray x = xValue.toArray();
  const QJsonArray y = yValue.toArray();
  const double xx = x.at(0).toDouble();
  const double xy = x.at(1).toDouble();
  const double xz = x.at(2).toDouble();
  const double yx = y.at(0).toDouble();
  const double yy = y.at(1).toDouble();
  const double yz = y.at(2).toDouble();
  const double xNorm = std::hypot(xx, xy, xz);
  const double yNorm = std::hypot(yx, yy, yz);
  if (xNorm <= 1.0e-12 || yNorm <= 1.0e-12)
    return fail(error, path, QStringLiteral("contains a zero direction"));
  const double dot = xx * yx + xy * yy + xz * yz;
  if (std::abs(xNorm - 1.0) > 1.0e-9 ||
      std::abs(yNorm - 1.0) > 1.0e-9 || std::abs(dot) > 1.0e-9)
    return fail(error, path, QStringLiteral("must be orthonormal"));
  const double crossX = xy * yz - xz * yy;
  const double crossY = xz * yx - xx * yz;
  const double crossZ = xx * yy - xy * yx;
  const double sine = std::hypot(crossX, crossY, crossZ) / (xNorm * yNorm);
  if (sine <= 1.0e-9)
    return fail(error, path, QStringLiteral("contains parallel directions"));
  return true;
}

bool arrayValue(const QJsonValue& value, const QString& path, QString* error,
                QJsonArray* decoded = nullptr) {
  if (!value.isArray()) return fail(error, path, QStringLiteral("must be an array"));
  if (value.toArray().size() > ProjectFile::kMaximumCollectionItems)
    return fail(error, path, QStringLiteral("exceeds its item limit"));
  if (decoded) *decoded = value.toArray();
  return true;
}

bool boundedBrepString(const QJsonValue& value, const QString& path,
                       QString* error) {
  if (!value.isString()) return fail(error, path, QStringLiteral("must be a string"));
  const QString encoded = value.toString();
  if (encoded.isEmpty() ||
      encoded.size() > ProjectFile::kMaximumBRepBase64Characters)
    return fail(error, path, QStringLiteral("has an invalid bounded string value"));
  // Base64 decoding validity deliberately remains with the later v3-compatible
  // payload decoder. Reproducing QByteArray's decoder here without allocating
  // could otherwise narrow the accepted legacy syntax.
  return true;
}

bool sameEnvelope(const ProjectFormatV4EnvelopeCandidate& authored,
                  const ProjectFormatV4EnvelopeCandidate& validated,
                  QString* error) {
  if (authored.authoredRoot != validated.authoredRoot ||
      authored.document != validated.document ||
      authored.legacySketches != validated.legacySketches ||
      authored.legacyExtrusion != validated.legacyExtrusion ||
      authored.model != validated.model ||
      authored.capabilities != validated.capabilities ||
      authored.deferredUnknowns != validated.deferredUnknowns)
    return fail(error, QStringLiteral("$"),
                QStringLiteral("envelope candidate is inconsistent with authoredRoot"));
  return true;
}

bool validateLegacySketches(const QJsonArray& sketches,
                            qsizetype* documentGeometryCount,
                            QString* error) {
  std::set<qint64> sketchIds;
  qsizetype totalGeometry = 0;
  qsizetype totalConstraints = 0;
  qint64 totalComplexity = 0;
  for (qsizetype index = 0; index < sketches.size(); ++index) {
    const QString path = QStringLiteral("$.sketches[%1]").arg(index);
    if (!sketches.at(index).isObject())
      return fail(error, path, QStringLiteral("must be an object"));
    const QJsonObject sketch = sketches.at(index).toObject();
    if (!exactFields(sketch,
                     {"support", "lines", "circles", "arcs", "beziers",
                      "dimensions", "constraints", "centerNodeElementIds",
                      "sketchId", "idAllocators", "schema"},
                     {}, path, error))
      return false;
    if (!boundedString(sketch.value(QStringLiteral("support")),
                       path + QStringLiteral(".support"), false, error))
      return false;
    if (!boundedString(sketch.value(QStringLiteral("schema")),
                       path + QStringLiteral(".schema"), false, error) ||
        sketch.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("typed-sketch-v1"))
      return fail(error, path + QStringLiteral(".schema"),
                  QStringLiteral("must be typed-sketch-v1"));
    qint64 sketchId = 0;
    if (!persistedId(sketch.value(QStringLiteral("sketchId")),
                     path + QStringLiteral(".sketchId"), error, &sketchId) ||
        !sketchIds.insert(sketchId).second)
      return fail(error, path + QStringLiteral(".sketchId"),
                  QStringLiteral("must be unique"));

    std::array<QJsonArray, 7> arrays;
    constexpr std::array<const char*, 7> arrayNames{
        "lines", "circles", "arcs", "beziers", "dimensions",
        "constraints", "centerNodeElementIds"};
    for (std::size_t slot = 0; slot < arrayNames.size(); ++slot)
      if (!arrayValue(sketch.value(QLatin1String(arrayNames[slot])),
                      path + QLatin1Char('.') + QLatin1String(arrayNames[slot]),
                      error, &arrays[slot]))
        return false;
    const qsizetype geometryCount = arrays[0].size() + arrays[1].size() +
                                    arrays[2].size() + arrays[3].size();
    const qsizetype constraintCount = arrays[5].size();
    if (geometryCount > ProjectFile::kMaximumSketchGeometryItems ||
        constraintCount > ProjectFile::kMaximumSketchConstraintItems)
      return fail(error, path, QStringLiteral("exceeds per-sketch limits"));
    const qint64 complexity =
        static_cast<qint64>(std::max<qsizetype>(1, geometryCount)) *
            constraintCount +
        static_cast<qint64>(constraintCount) * constraintCount;
    if (complexity > ProjectFile::kMaximumSketchSolveComplexity)
      return fail(error, path, QStringLiteral("exceeds solve complexity limit"));
    totalGeometry += geometryCount;
    totalConstraints += constraintCount;
    totalComplexity += complexity;
    if (totalGeometry > ProjectFile::kMaximumDocumentSketchGeometryItems ||
        totalConstraints >
            ProjectFile::kMaximumDocumentSketchConstraintItems ||
        totalComplexity >
            ProjectFile::kMaximumDocumentSketchSolveComplexity)
      return fail(error, QStringLiteral("$.sketches"),
                  QStringLiteral("exceeds document sketch limits"));

    const QJsonValue allocatorsValue =
        sketch.value(QStringLiteral("idAllocators"));
    if (!allocatorsValue.isObject())
      return fail(error, path + QStringLiteral(".idAllocators"),
                  QStringLiteral("must be an object"));
    const QJsonObject allocators = allocatorsValue.toObject();
    if (!exactFields(allocators,
                     {"nextElementId", "nextGeometryId", "nextConstraintId",
                      "nextDimensionId"},
                     {}, path + QStringLiteral(".idAllocators"), error))
      return false;
    for (const char* field : {"nextElementId", "nextGeometryId",
                              "nextConstraintId", "nextDimensionId"})
      if (!persistedId(allocators.value(QLatin1String(field)),
                       path + QStringLiteral(".idAllocators.") +
                           QLatin1String(field),
                       error))
        return false;
  }
  *documentGeometryCount = totalGeometry;
  return true;
}

bool validateExtrusion(const QJsonObject& extrusion, QString* error) {
  if (!exactFields(extrusion, {"enabled"}, {"sourceSketch"},
                   QStringLiteral("$.extrusion"), error) ||
      !booleanValue(extrusion.value(QStringLiteral("enabled")),
                    QStringLiteral("$.extrusion.enabled"), error))
    return false;
  if (extrusion.contains(QStringLiteral("sourceSketch")) &&
      !exactInteger(extrusion.value(QStringLiteral("sourceSketch")), 0,
                    ProjectFile::kMaximumCollectionItems,
                    QStringLiteral("$.extrusion.sourceSketch"), error))
    return false;
  return true;
}

bool checkedAdd(qsizetype amount, qsizetype limit, qsizetype* total,
                const QString& path, const QString& message, QString* error) {
  if (amount < 0 || *total < 0 || amount > limit - *total)
    return fail(error, path, message);
  *total += amount;
  return true;
}

bool validateProfileOverride(const QJsonValue& value, const QString& path,
                             qsizetype* documentGeometryCount,
                             QString* error) {
  if (!value.isObject()) return fail(error, path, QStringLiteral("must be an object"));
  const QJsonObject profile = value.toObject();
  if (!exactFields(profile, {"lines", "circles", "arcs", "beziers"}, {},
                   path, error))
    return false;
  qsizetype count = 0;
  for (const char* field : {"lines", "circles", "arcs", "beziers"}) {
    QJsonArray values;
    if (!arrayValue(profile.value(QLatin1String(field)),
                    path + QLatin1Char('.') + QLatin1String(field), error,
                    &values))
      return false;
    if (!checkedAdd(values.size(), ProjectFile::kMaximumSketchGeometryItems,
                    &count, path,
                    QStringLiteral("exceeds geometry item limit"), error))
      return false;
  }
  return checkedAdd(count, ProjectFile::kMaximumDocumentSketchGeometryItems,
                    documentGeometryCount, path,
                    QStringLiteral("exceeds document geometry limit"), error);
}

bool addSite(const QJsonValue& wrapperValue, const QString& wrapperPath,
             ProjectFormatV4TopologySiteFamily family, TopologyKind kind,
             std::optional<BodyId> bodyId,
             std::optional<FeatureId> featureId,
             std::vector<ProjectFormatV4TopologySite>* sites,
             QString* error) {
  if (!wrapperValue.isObject())
    return fail(error, wrapperPath, QStringLiteral("wrapper must be an object"));
  const QJsonObject wrapper = wrapperValue.toObject();
  if (!exactFields(wrapper, {"reference"}, {}, wrapperPath, error))
    return false;
  const QJsonValue reference = wrapper.value(QStringLiteral("reference"));
  if (!reference.isObject())
    return fail(error, wrapperPath + QStringLiteral(".reference"),
                QStringLiteral("must be an object"));
  sites->push_back({family, wrapperPath + QStringLiteral(".reference"), kind,
                    reference, bodyId, featureId});
  return true;
}

bool addSiteArray(const QJsonArray& wrappers, const QString& path,
                  ProjectFormatV4TopologySiteFamily family, TopologyKind kind,
                  BodyId bodyId, FeatureId featureId,
                  std::vector<ProjectFormatV4TopologySite>* sites,
                  QString* error) {
  for (qsizetype index = 0; index < wrappers.size(); ++index)
    if (!addSite(wrappers.at(index),
                 QStringLiteral("%1[%2]").arg(path).arg(index), family, kind,
                 bodyId, featureId, sites, error))
      return false;
  return true;
}

bool topologyArray(const QJsonValue& value, const QString& path,
                   QJsonArray* wrappers, QString* error) {
  if (!arrayValue(value, path, error, wrappers)) return false;
  return !wrappers->isEmpty() ||
         fail(error, path, QStringLiteral("must not be empty"));
}

bool preflightFeatureTopology(qsizetype count, qsizetype* documentCount,
                              const QString& path, QString* error) {
  if (count > ProjectFile::kMaximumFeatureTopologyReferences)
    return fail(error, path,
                QStringLiteral("exceeds per-feature topology limit"));
  return checkedAdd(count, ProjectFile::kMaximumDocumentTopologyReferences,
                    documentCount, QStringLiteral("$.model"),
                    QStringLiteral("exceeds document topology limit"), error);
}

bool integerField(const QJsonObject& object, const char* field, qint64 minimum,
                  qint64 maximum, const QString& path, QString* error) {
  return exactInteger(object.value(QLatin1String(field)), minimum, maximum,
                      path + QLatin1Char('.') + QLatin1String(field), error);
}

bool persistedIdField(const QJsonObject& object, const char* field,
                      const QString& path, QString* error) {
  return persistedId(object.value(QLatin1String(field)),
                     path + QLatin1Char('.') + QLatin1String(field), error);
}

bool numberField(const QJsonObject& object, const char* field, double minimum,
                 double maximum, const QString& path, QString* error) {
  return finiteNumber(object.value(QLatin1String(field)), minimum, maximum,
                      path + QLatin1Char('.') + QLatin1String(field), error);
}

bool booleanField(const QJsonObject& object, const char* field,
                  const QString& path, QString* error) {
  return booleanValue(object.value(QLatin1String(field)),
                      path + QLatin1Char('.') + QLatin1String(field), error);
}

std::optional<FeatureKind> kindForToken(
    const QString& token, const std::vector<FeatureCodecDescriptor>& codecs) {
  for (const auto& codec : codecs) {
    if (token == QString::fromLatin1(codec.token.data(),
                                    static_cast<qsizetype>(codec.token.size())))
      return codec.kind;
  }
  return std::nullopt;
}

bool completeCodecRegistry(const std::vector<FeatureCodecDescriptor>& codecs,
                           QString* error) {
  constexpr std::array<FeatureKind, 13> kinds{
      FeatureKind::ImportedShape, FeatureKind::Extrude,
      FeatureKind::Revolve,       FeatureKind::Pocket,
      FeatureKind::Fillet,        FeatureKind::Chamfer,
      FeatureKind::Mirror,        FeatureKind::Move,
      FeatureKind::LinearPattern, FeatureKind::CircularPattern,
      FeatureKind::JoinBodies,    FeatureKind::Shell,
      FeatureKind::Draft};
  static_assert(kinds.size() == kPersistedFeatureKindCount);
  if (codecs.size() != kinds.size())
    return fail(error, QStringLiteral("$.model"),
                QStringLiteral("feature codec registry is incomplete"));
  for (FeatureKind kind : kinds) {
    const auto count = std::count_if(
        codecs.begin(), codecs.end(),
        [kind](const FeatureCodecDescriptor& codec) {
          return codec.kind == kind && !codec.token.empty();
        });
    if (count != 1)
      return fail(error, QStringLiteral("$.model"),
                  QStringLiteral("feature codec registry is inconsistent"));
  }
  return true;
}

bool validateFeature(
    const QJsonObject& feature, const QString& path, BodyId bodyId,
    FeatureId featureId, FeatureKind kind,
    std::vector<ProjectFormatV4TopologySite>* sites, QString* error,
    qsizetype* documentTopologyCount, qsizetype* documentGeometryCount,
    qsizetype* documentRebuildWork) {
  const double positiveMinimum = std::numeric_limits<double>::min();

  switch (kind) {
    case FeatureKind::ImportedShape:
      return exactFields(feature, {"id", "name", "type", "brep"}, {}, path,
                         error) &&
             boundedBrepString(feature.value(QStringLiteral("brep")),
                               path + QStringLiteral(".brep"), error);
    case FeatureKind::Extrude: {
      QString sourceKind;
      if (!boundedString(feature.value(QStringLiteral("sourceKind")),
                         path + QStringLiteral(".sourceKind"), false, error,
                         &sourceKind))
        return false;
      if (sourceKind == QStringLiteral("face")) {
        if (!exactFields(feature,
                         {"id", "name", "type", "lengthMm", "operation",
                          "reversed", "sourceKind", "face"},
                         {}, path, error) ||
            !preflightFeatureTopology(1, documentTopologyCount, path,
                                      error) ||
            !addSite(feature.value(QStringLiteral("face")),
                     path + QStringLiteral(".face"),
                     ProjectFormatV4TopologySiteFamily::ExtrudeFace,
                     TopologyKind::Face, bodyId, featureId, sites, error))
          return false;
      } else if (sourceKind == QStringLiteral("sketch")) {
        if (!exactFields(feature,
                         {"id", "name", "type", "lengthMm", "operation",
                          "reversed", "sourceKind", "sketchId"},
                         {"profileOverride"}, path, error) ||
            !persistedIdField(feature, "sketchId", path, error))
          return false;
        if (feature.contains(QStringLiteral("profileOverride")) &&
            !validateProfileOverride(
                feature.value(QStringLiteral("profileOverride")),
                path + QStringLiteral(".profileOverride"),
                documentGeometryCount, error))
          return false;
      } else {
        return fail(error, path + QStringLiteral(".sourceKind"),
                    QStringLiteral("has an unknown token"));
      }
      return numberField(feature, "lengthMm", positiveMinimum,
                         kMaximumModelLengthMm, path, error) &&
             integerField(feature, "operation", 0, 2, path, error) &&
             booleanField(feature, "reversed", path, error);
    }
    case FeatureKind::Revolve: {
      qint64 axisType = 0;
      if (!exactInteger(feature.value(QStringLiteral("axisType")), 0, 5,
                        path + QStringLiteral(".axisType"), error, &axisType))
        return false;
      const bool lineAxis = axisType == 2;
      if (!exactFields(feature,
                       {"id", "name", "type", "profileSketchId", "axisType",
                        "axisSketchId", "axisLineId", "angleDeg", "operation",
                        "reversed"},
                       {"axisLineIndex", "profileOverride"},
                       path, error) ||
          !persistedIdField(feature, "profileSketchId", path, error) ||
          !integerField(feature, "axisSketchId", axisType <= 2 ? 1 : 0,
                        ProjectFile::kMaximumPersistedId, path, error) ||
          !integerField(feature, "axisLineId", lineAxis ? 1 : 0,
                        kMaximumExactJsonInteger, path, error) ||
          (lineAxis &&
           !integerField(feature, "axisLineIndex", 0,
                         ProjectFile::kMaximumCollectionItems, path, error)) ||
          !numberField(feature, "angleDeg", positiveMinimum, 360.0, path,
                       error) ||
          !integerField(feature, "operation", 0, 2, path, error) ||
          !booleanField(feature, "reversed", path, error))
        return false;
      if (!lineAxis && feature.contains(QStringLiteral("axisLineIndex")))
        return fail(error, path + QStringLiteral(".axisLineIndex"),
                    QStringLiteral("is forbidden for this axis type"));
      if (feature.contains(QStringLiteral("profileOverride")) &&
          !validateProfileOverride(feature.value(QStringLiteral("profileOverride")),
                                   path + QStringLiteral(".profileOverride"),
                                   documentGeometryCount, error))
        return false;
      return true;
    }
    case FeatureKind::Pocket:
      return exactFields(feature,
                         {"id", "name", "type", "sketchId", "depthMm"}, {},
                         path, error) &&
             persistedIdField(feature, "sketchId", path, error) &&
             numberField(feature, "depthMm", positiveMinimum,
                         kMaximumModelLengthMm, path, error);
    case FeatureKind::Fillet:
    case FeatureKind::Chamfer: {
      const char* parameter = kind == FeatureKind::Fillet ? "radiusMm"
                                                           : "distanceMm";
      QJsonArray edges;
      if (!exactFields(feature, {"id", "name", "type", parameter, "edges"},
                       {}, path, error) ||
          !numberField(feature, parameter, positiveMinimum,
                       kMaximumModelLengthMm, path, error) ||
          !topologyArray(feature.value(QStringLiteral("edges")),
                         path + QStringLiteral(".edges"), &edges, error) ||
          !preflightFeatureTopology(edges.size(), documentTopologyCount, path,
                                    error) ||
          !addSiteArray(edges, path + QStringLiteral(".edges"),
              kind == FeatureKind::Fillet
                  ? ProjectFormatV4TopologySiteFamily::FilletEdge
                  : ProjectFormatV4TopologySiteFamily::ChamferEdge,
              TopologyKind::Edge, bodyId, featureId, sites, error))
        return false;
      return true;
    }
    case FeatureKind::Mirror:
      if (!exactFields(feature,
                       {"id", "name", "type", "sourceFeatureId", "plane"},
                       {"planeFace"}, path, error) ||
          !persistedIdField(feature, "sourceFeatureId", path, error) ||
          !integerField(feature, "plane", 0, 2, path, error))
        return false;
      if (feature.contains(QStringLiteral("planeFace"))) {
        if (!preflightFeatureTopology(1, documentTopologyCount, path,
                                      error) ||
            !addSite(feature.value(QStringLiteral("planeFace")),
                     path + QStringLiteral(".planeFace"),
                     ProjectFormatV4TopologySiteFamily::MirrorPlaneFace,
                     TopologyKind::Face, bodyId, featureId, sites, error))
          return false;
      }
      return true;
    case FeatureKind::Move:
      if (!exactFields(feature,
                       {"id", "name", "type", "sourceFeatureId", "offsetXmm",
                        "offsetYmm", "offsetZmm"},
                       {}, path, error) ||
          !persistedIdField(feature, "sourceFeatureId", path, error))
        return false;
      for (const char* field : {"offsetXmm", "offsetYmm", "offsetZmm"})
        if (!numberField(feature, field, -kMaximumModelLengthMm,
                         kMaximumModelLengthMm, path, error))
          return false;
      return true;
    case FeatureKind::LinearPattern:
    case FeatureKind::CircularPattern: {
      const bool linear = kind == FeatureKind::LinearPattern;
      const char* axis = linear ? "direction" : "axis";
      const char* parameter = linear ? "spacingMm" : "angleDeg";
      qint64 operation = 0;
      qint64 count = 0;
      if (!exactFields(feature,
                       {"id", "name", "type", "sourceBodyId",
                        "sourceFeatureId", axis, "count", parameter,
                        "operation"},
                       {}, path, error) ||
          !persistedIdField(feature, "sourceFeatureId", path, error) ||
          !integerField(feature, axis, 0, 2, path, error) ||
          !exactInteger(feature.value(QStringLiteral("count")),
                        ProjectFile::kMinimumPatternCount,
                        ProjectFile::kMaximumPatternCount,
                        path + QStringLiteral(".count"), error, &count) ||
          !numberField(feature, parameter, 0.01,
                       linear ? 100000.0 : 360.0, path, error) ||
          !exactInteger(feature.value(QStringLiteral("operation")), 0, 1,
                        path + QStringLiteral(".operation"), error,
                        &operation) ||
          !exactInteger(feature.value(QStringLiteral("sourceBodyId")),
                        operation == 1 ? 0 : 1,
                        ProjectFile::kMaximumPersistedId,
                        path + QStringLiteral(".sourceBodyId"), error) ||
          !checkedAdd(static_cast<qsizetype>(count - 1),
                      ProjectFile::kMaximumDocumentRebuildWork,
                      documentRebuildWork, QStringLiteral("$.model"),
                      QStringLiteral("exceeds document rebuild work limit"),
                      error))
        return false;
      return true;
    }
    case FeatureKind::JoinBodies:
      if (!exactFields(feature,
                       {"id", "name", "type", "firstBodyId",
                        "firstFeatureId", "secondBodyId", "secondFeatureId"},
                       {}, path, error))
        return false;
      for (const char* field : {"firstBodyId", "firstFeatureId",
                                "secondBodyId", "secondFeatureId"})
        if (!persistedIdField(feature, field, path, error)) return false;
      return true;
    case FeatureKind::Shell: {
      QJsonArray removedFaces;
      if (!exactFields(feature,
                       {"id", "name", "type", "sourceFeatureId",
                        "thicknessMm", "outside", "removedFaces"},
                       {}, path, error) ||
          !persistedIdField(feature, "sourceFeatureId", path, error) ||
          !numberField(feature, "thicknessMm", positiveMinimum,
                       kMaximumModelLengthMm, path, error) ||
          !booleanField(feature, "outside", path, error) ||
          !topologyArray(feature.value(QStringLiteral("removedFaces")),
                         path + QStringLiteral(".removedFaces"),
                         &removedFaces, error) ||
          !preflightFeatureTopology(removedFaces.size(),
                                    documentTopologyCount, path, error) ||
          !addSiteArray(removedFaces, path + QStringLiteral(".removedFaces"),
                        ProjectFormatV4TopologySiteFamily::ShellRemovedFace,
                        TopologyKind::Face, bodyId, featureId, sites, error))
        return false;
      return true;
    }
    case FeatureKind::Draft: {
      qint64 neutralType = 0;
      qint64 directionType = 0;
      if (!exactInteger(feature.value(QStringLiteral("neutralPlaneType")), 0,
                        3, path + QStringLiteral(".neutralPlaneType"), error,
                        &neutralType) ||
          !exactInteger(feature.value(QStringLiteral("pullDirectionType")), 0,
                        5, path + QStringLiteral(".pullDirectionType"), error,
                        &directionType))
        return false;
      const bool faceNeutral = neutralType == 3;
      const bool lineDirection = directionType == 2;
      QJsonArray draftedFaces;
      if (!exactFields(feature,
                       {"id", "name", "type", "sourceFeatureId", "angleDeg",
                        "reversed", "neutralPlaneType", "pullDirectionType",
                        "pullDirectionSketchId", "pullDirectionLineId",
                        "draftedFaces"},
                       {"neutralPlaneFace", "rotationEdge",
                        "pullDirectionLineIndex"},
                       path, error) ||
          !persistedIdField(feature, "sourceFeatureId", path, error) ||
          !numberField(feature, "angleDeg", -89.99, 89.99, path, error) ||
          !booleanField(feature, "reversed", path, error) ||
          !integerField(feature, "pullDirectionSketchId",
                        directionType <= 2 ? 1 : 0,
                        ProjectFile::kMaximumPersistedId, path, error) ||
          !integerField(feature, "pullDirectionLineId",
                        lineDirection ? 1 : 0, kMaximumExactJsonInteger, path,
                        error) ||
          (lineDirection &&
           !integerField(feature, "pullDirectionLineIndex", 0,
                         ProjectFile::kMaximumCollectionItems, path, error)) ||
          !topologyArray(feature.value(QStringLiteral("draftedFaces")),
                         path + QStringLiteral(".draftedFaces"),
                         &draftedFaces, error))
        return false;
      if (!lineDirection &&
          feature.contains(QStringLiteral("pullDirectionLineIndex")))
        return fail(error, path + QStringLiteral(".pullDirectionLineIndex"),
                    QStringLiteral("is forbidden for this direction type"));
      qsizetype topologyCount = 0;
      if (!checkedAdd(draftedFaces.size(),
                      ProjectFile::kMaximumFeatureTopologyReferences,
                      &topologyCount, path,
                      QStringLiteral("exceeds per-feature topology limit"),
                      error) ||
          !checkedAdd(faceNeutral ? 1 : 0,
                      ProjectFile::kMaximumFeatureTopologyReferences,
                      &topologyCount, path,
                      QStringLiteral("exceeds per-feature topology limit"),
                      error) ||
          !checkedAdd(feature.contains(QStringLiteral("rotationEdge")) ? 1
                                                                       : 0,
                      ProjectFile::kMaximumFeatureTopologyReferences,
                      &topologyCount, path,
                      QStringLiteral("exceeds per-feature topology limit"),
                      error))
        return false;
      if (!preflightFeatureTopology(topologyCount, documentTopologyCount,
                                    path, error))
        return false;
      if (faceNeutral) {
        if (!feature.contains(QStringLiteral("neutralPlaneFace")))
          return fail(error, path + QStringLiteral(".neutralPlaneFace"),
                      QStringLiteral("is required for a face neutral plane"));
        if (!addSite(feature.value(QStringLiteral("neutralPlaneFace")),
                     path + QStringLiteral(".neutralPlaneFace"),
                     ProjectFormatV4TopologySiteFamily::DraftNeutralPlaneFace,
                     TopologyKind::Face, bodyId, featureId, sites, error))
          return false;
      } else if (feature.contains(QStringLiteral("neutralPlaneFace"))) {
        return fail(error, path + QStringLiteral(".neutralPlaneFace"),
                    QStringLiteral("is forbidden for a datum plane"));
      }
      if (feature.contains(QStringLiteral("rotationEdge"))) {
        if (!addSite(feature.value(QStringLiteral("rotationEdge")),
                     path + QStringLiteral(".rotationEdge"),
                     ProjectFormatV4TopologySiteFamily::DraftRotationEdge,
                     TopologyKind::Edge, bodyId, featureId, sites, error))
          return false;
      }
      if (!addSiteArray(draftedFaces, path + QStringLiteral(".draftedFaces"),
                        ProjectFormatV4TopologySiteFamily::DraftedFace,
                        TopologyKind::Face, bodyId, featureId, sites, error))
        return false;
      return true;
    }
    case FeatureKind::Unknown:
      return fail(error, path, QStringLiteral("unknown feature kind"));
  }
  return fail(error, path, QStringLiteral("unhandled feature kind"));
}

bool validateModel(
    const QJsonObject& model,
    qsizetype* documentGeometryCount,
    std::vector<ProjectFormatV4TopologySite>* sites, QString* error) {
  const auto codecs = featureCodecDescriptors();
  if (!completeCodecRegistry(codecs, error)) return false;
  const QJsonArray sketches = model.value(QStringLiteral("sketches")).toArray();
  const QJsonArray bodies = model.value(QStringLiteral("bodies")).toArray();
  const QJsonArray images =
      model.value(QStringLiteral("referenceImages")).toArray();

  qsizetype documentTopologyCount = 0;
  qsizetype documentRebuildWork = 0;
  std::set<qint64> sketchIds;
  for (qsizetype index = 0; index < sketches.size(); ++index) {
    const QString path = QStringLiteral("$.model.sketches[%1]").arg(index);
    if (!sketches.at(index).isObject())
      return fail(error, path, QStringLiteral("must be an object"));
    const QJsonObject sketch = sketches.at(index).toObject();
    if (!exactFields(sketch,
                     {"id", "name", "origin", "xDirection", "yDirection",
                      "support"},
                     {}, path, error))
      return false;
    qint64 id = 0;
    if (!persistedId(sketch.value(QStringLiteral("id")),
                     path + QStringLiteral(".id"), error, &id) ||
        !sketchIds.insert(id).second)
      return fail(error, path + QStringLiteral(".id"),
                  QStringLiteral("must be unique"));
    if (!boundedString(sketch.value(QStringLiteral("name")),
                       path + QStringLiteral(".name"), true, error) ||
        !vector3(sketch.value(QStringLiteral("origin")),
                 path + QStringLiteral(".origin"), error) ||
        !vector3(sketch.value(QStringLiteral("xDirection")),
                 path + QStringLiteral(".xDirection"), error) ||
        !vector3(sketch.value(QStringLiteral("yDirection")),
                 path + QStringLiteral(".yDirection"), error) ||
        !placementBasis(sketch.value(QStringLiteral("xDirection")),
                        sketch.value(QStringLiteral("yDirection")), path,
                        error))
      return false;
    const QJsonValue supportValue = sketch.value(QStringLiteral("support"));
    if (!supportValue.isObject())
      return fail(error, path + QStringLiteral(".support"),
                  QStringLiteral("must be an object"));
    const QJsonObject support = supportValue.toObject();
    qint64 supportType = 0;
    if (!exactInteger(support.value(QStringLiteral("type")), 0, 1,
                      path + QStringLiteral(".support.type"), error,
                      &supportType))
      return false;
    if (supportType == 0) {
      if (!exactFields(support, {"type"}, {},
                       path + QStringLiteral(".support"), error))
        return false;
    } else {
      if (!exactFields(support, {"type", "reference"}, {},
                       path + QStringLiteral(".support"), error))
        return false;
      const QJsonValue reference = support.value(QStringLiteral("reference"));
      if (!reference.isObject())
        return fail(error, path + QStringLiteral(".support.reference"),
                    QStringLiteral("must be an object"));
      if (!checkedAdd(1, ProjectFile::kMaximumDocumentTopologyReferences,
                      &documentTopologyCount, QStringLiteral("$.model"),
                      QStringLiteral("exceeds document topology limit"),
                      error))
        return false;
      sites->push_back({ProjectFormatV4TopologySiteFamily::SketchSupport,
                        path + QStringLiteral(".support.reference"),
                        TopologyKind::Face, reference, std::nullopt,
                        std::nullopt});
    }
  }

  std::set<qint64> bodyIds;
  std::set<qint64> featureIds;
  qsizetype totalFeatures = 0;
  for (qsizetype bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex) {
    const QString bodyPath =
        QStringLiteral("$.model.bodies[%1]").arg(bodyIndex);
    if (!bodies.at(bodyIndex).isObject())
      return fail(error, bodyPath, QStringLiteral("must be an object"));
    const QJsonObject body = bodies.at(bodyIndex).toObject();
    if (!exactFields(body, {"id", "name", "visible", "features"}, {},
                     bodyPath, error))
      return false;
    qint64 bodyId = 0;
    if (!persistedId(body.value(QStringLiteral("id")),
                     bodyPath + QStringLiteral(".id"), error, &bodyId) ||
        !bodyIds.insert(bodyId).second)
      return fail(error, bodyPath + QStringLiteral(".id"),
                  QStringLiteral("must be unique"));
    if (!boundedString(body.value(QStringLiteral("name")),
                       bodyPath + QStringLiteral(".name"), true, error) ||
        !booleanValue(body.value(QStringLiteral("visible")),
                      bodyPath + QStringLiteral(".visible"), error))
      return false;
    QJsonArray features;
    if (!arrayValue(body.value(QStringLiteral("features")),
                    bodyPath + QStringLiteral(".features"), error, &features))
      return false;
    if (!checkedAdd(features.size(), ProjectFile::kMaximumDocumentFeatures,
                    &totalFeatures, QStringLiteral("$.model.bodies"),
                    QStringLiteral("exceeds feature count limit"), error) ||
        !checkedAdd(features.size(),
                    ProjectFile::kMaximumDocumentRebuildWork,
                    &documentRebuildWork, QStringLiteral("$.model"),
                    QStringLiteral("exceeds document rebuild work limit"),
                    error))
      return false;
    for (qsizetype featureIndex = 0; featureIndex < features.size();
         ++featureIndex) {
      const QString path =
          QStringLiteral("%1.features[%2]").arg(bodyPath).arg(featureIndex);
      if (!features.at(featureIndex).isObject())
        return fail(error, path, QStringLiteral("must be an object"));
      const QJsonObject feature = features.at(featureIndex).toObject();
      qint64 featureId = 0;
      QString token;
      if (!persistedId(feature.value(QStringLiteral("id")),
                       path + QStringLiteral(".id"), error, &featureId) ||
          !featureIds.insert(featureId).second)
        return fail(error, path + QStringLiteral(".id"),
                    QStringLiteral("must be document-global unique"));
      if (!boundedString(feature.value(QStringLiteral("name")),
                         path + QStringLiteral(".name"), true, error) ||
          !boundedString(feature.value(QStringLiteral("type")),
                         path + QStringLiteral(".type"), false, error,
                         &token))
        return false;
      const auto kind = kindForToken(token, codecs);
      if (!kind)
        return fail(error, path + QStringLiteral(".type"),
                    QStringLiteral("is not in featureCodecDescriptors"));
      if (!validateFeature(feature, path, static_cast<BodyId>(bodyId),
                           static_cast<FeatureId>(featureId), *kind, sites,
                           error, &documentTopologyCount,
                           documentGeometryCount, &documentRebuildWork))
        return false;
    }
  }

  std::set<qint64> imageIds;
  for (qsizetype index = 0; index < images.size(); ++index) {
    const QString path =
        QStringLiteral("$.model.referenceImages[%1]").arg(index);
    if (!images.at(index).isObject())
      return fail(error, path, QStringLiteral("must be an object"));
    const QJsonObject image = images.at(index).toObject();
    if (!exactFields(image,
                     {"id", "name", "sourcePath", "supportName", "origin",
                      "xDirection", "yDirection", "offsetX", "offsetY",
                      "offsetZ", "scale", "pixelWidth", "pixelHeight",
                      "visible"},
                     {}, path, error))
      return false;
    qint64 id = 0;
    qint64 width = 0;
    qint64 height = 0;
    if (!persistedId(image.value(QStringLiteral("id")),
                     path + QStringLiteral(".id"), error, &id) ||
        !imageIds.insert(id).second)
      return fail(error, path + QStringLiteral(".id"),
                  QStringLiteral("must be unique"));
    if (!boundedString(image.value(QStringLiteral("name")),
                       path + QStringLiteral(".name"), true, error) ||
        !boundedString(image.value(QStringLiteral("sourcePath")),
                       path + QStringLiteral(".sourcePath"), false, error) ||
        !boundedString(image.value(QStringLiteral("supportName")),
                       path + QStringLiteral(".supportName"), true, error) ||
        !vector3(image.value(QStringLiteral("origin")),
                 path + QStringLiteral(".origin"), error) ||
        !vector3(image.value(QStringLiteral("xDirection")),
                 path + QStringLiteral(".xDirection"), error) ||
        !vector3(image.value(QStringLiteral("yDirection")),
                 path + QStringLiteral(".yDirection"), error) ||
        !placementBasis(image.value(QStringLiteral("xDirection")),
                        image.value(QStringLiteral("yDirection")), path,
                        error) ||
        !numberField(image, "offsetX", -kMaximumCoordinateMagnitude,
                     kMaximumCoordinateMagnitude, path, error) ||
        !numberField(image, "offsetY", -kMaximumCoordinateMagnitude,
                     kMaximumCoordinateMagnitude, path, error) ||
        !numberField(image, "offsetZ", -kMaximumCoordinateMagnitude,
                     kMaximumCoordinateMagnitude, path, error) ||
        !numberField(image, "scale", 0.0, 10000.0, path, error) ||
        !exactInteger(image.value(QStringLiteral("pixelWidth")), 1, 16384,
                      path + QStringLiteral(".pixelWidth"), error, &width) ||
        !exactInteger(image.value(QStringLiteral("pixelHeight")), 1, 16384,
                      path + QStringLiteral(".pixelHeight"), error, &height) ||
        !booleanValue(image.value(QStringLiteral("visible")),
                      path + QStringLiteral(".visible"), error))
      return false;
    if (image.value(QStringLiteral("scale")).toDouble() == 0.0)
      return fail(error, path + QStringLiteral(".scale"),
                  QStringLiteral("must be positive"));
    if (width * height > 100000000LL)
      return fail(error, path, QStringLiteral("exceeds image pixel limit"));
  }
  return true;
}

}  // namespace

ProjectFormatV4PayloadScanResult scanProjectFormatV4Payload(
    ProjectFormatV4EnvelopeCandidate envelope) {
  QString error;
  auto validated =
      detail::validateProjectFormatV4EnvelopeRoot(envelope.authoredRoot);
  if (!validated.hasCandidate())
    return invalid(validated.error.isEmpty()
                       ? QStringLiteral("$: authoredRoot is not a v4 envelope")
                       : std::move(validated.error));
  if (!sameEnvelope(envelope, *validated.candidate, &error))
    return invalid(std::move(error));
  const QJsonObject& root = envelope.authoredRoot;
  qsizetype documentGeometryCount = 0;
  if (!validateLegacySketches(root.value(QStringLiteral("sketches")).toArray(),
                              &documentGeometryCount, &error) ||
      !validateExtrusion(root.value(QStringLiteral("extrusion")).toObject(),
                         &error))
    return invalid(std::move(error));

  std::vector<ProjectFormatV4TopologySite> sites;
  if (!validateModel(root.value(QStringLiteral("model")).toObject(),
                     &documentGeometryCount, &sites, &error))
    return invalid(std::move(error));

  ProjectFormatV4PayloadCandidate candidate;
  candidate.envelope = std::move(envelope);
  candidate.topologySites = std::move(sites);
  ProjectFormatV4PayloadScanResult result;
  result.status = ProjectFormatV4PayloadScanStatus::Candidate;
  result.candidate = std::move(candidate);
  return result;
}

}  // namespace solidar::project
