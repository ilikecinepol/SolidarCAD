#include "project/ProjectFile.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>

#include <BRep_Builder.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepTools.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <array>
#include <exception>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <utility>

#include "model/ExtrudeFeature.h"
#include "model/GeometryOperation.h"
#include "model/ImportedShapeFeature.h"
#include "model/IdGeneration.h"
#include "model/ChamferFeature.h"
#include "model/FilletFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/DraftFeature.h"
#include "model/ShellFeature.h"

namespace solidar::project {
namespace {

void setError(QString* target, const QString& value);

QString geometryFailureMessage(GeometryFailureKind kind,
                               const char* operation) {
  const QString action = QString::fromUtf8(operation);
  if (kind == GeometryFailureKind::OcctException)
    return QString::fromUtf8("Ошибка OpenCASCADE при %1.").arg(action);
  if (kind == GeometryFailureKind::StandardException)
    return QString::fromUtf8("Ошибка приложения при %1.").arg(action);
  return QString::fromUtf8("Неизвестная ошибка при %1.").arg(action);
}

bool serializeShape(const TopoDS_Shape& shape, QString* encoded,
                    QString* error) {
  if (!encoded || shape.IsNull()) {
    setError(error,
             QString::fromUtf8("Импортированная B-Rep геометрия пуста."));
    return false;
  }
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&]() -> bool {
        BRepCheck_Analyzer analyzer(shape);
        if (!analyzer.IsValid()) {
          setError(error, QString::fromUtf8(
                              "Импортированная B-Rep геометрия некорректна."));
          return false;
        }
        std::ostringstream stream(std::ios::out | std::ios::binary);
        // Native persistence needs exact topology, not cached visualization
        // data.
        BRepTools::Write(shape, stream, false, false,
                         TopTools_FormatVersion_CURRENT);
        if (!stream.good()) {
          setError(
              error,
              QString::fromUtf8(
                  "Не удалось сериализовать импортированную B-Rep геометрию."));
          return false;
        }
        const std::string bytes = stream.str();
        if (bytes.empty()) {
          setError(error,
                   QString::fromUtf8("Сериализованная B-Rep геометрия пуста."));
          return false;
        }
        *encoded = QString::fromLatin1(
            QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size()))
                .toBase64());
        return true;
      },
      &failure);
  if (!completed && failure.kind != GeometryFailureKind::None)
    setError(error, geometryFailureMessage(failure.kind, "сохранении B-Rep"));
  return completed;
}

std::shared_ptr<const TopoDS_Shape> deserializeShape(const QString& encoded,
                                                     QString* error) {
  std::shared_ptr<const TopoDS_Shape> result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&]() -> bool {
        const auto decoded = QByteArray::fromBase64Encoding(
            encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty()) {
          setError(error,
                   QString::fromUtf8(
                       "Данные импортированной B-Rep геометрии повреждены."));
          return false;
        }
        const QByteArray& bytes = decoded.decoded;
        std::istringstream stream(
            std::string(bytes.constData(),
                        static_cast<std::size_t>(bytes.size())),
            std::ios::in | std::ios::binary);
        BRep_Builder builder;
        TopoDS_Shape shape;
        BRepTools::Read(shape, stream, builder);
        if (!stream.good() && !stream.eof()) {
          setError(error,
                   QString::fromUtf8(
                       "Не удалось прочитать сохранённую B-Rep геометрию."));
          return {};
        }
        if (shape.IsNull()) {
          setError(error,
                   QString::fromUtf8("Сохранённая B-Rep геометрия пуста."));
          return {};
        }
        BRepCheck_Analyzer analyzer(shape);
        if (!analyzer.IsValid()) {
          setError(error, QString::fromUtf8(
                              "Сохранённая B-Rep геометрия некорректна."));
          return {};
        }
        result = std::make_shared<const TopoDS_Shape>(std::move(shape));
        return true;
      },
      &failure);
  if (!completed && failure.kind != GeometryFailureKind::None)
    setError(error, geometryFailureMessage(failure.kind, "загрузке B-Rep"));
  return completed ? result : std::shared_ptr<const TopoDS_Shape>{};
}

void setError(QString* target, const QString& value) {
  if (target) *target = value;
}

QJsonArray vector3(double x, double y, double z) { return {x, y, z}; }

Vector3d readVector3(const QJsonValue& value, Vector3d fallback) {
  const auto values = value.toArray();
  if (values.size() != 3) return fallback;
  return {values[0].toDouble(), values[1].toDouble(), values[2].toDouble()};
}

Point3d readPoint3(const QJsonValue& value, Point3d fallback) {
  const auto vector = readVector3(value, {fallback.x, fallback.y, fallback.z});
  return {vector.x, vector.y, vector.z};
}

QJsonArray topologyPoint(TopologyPoint3d value) {
  return {value.x, value.y, value.z};
}

TopologyPoint3d readTopologyPoint(const QJsonValue& value) {
  const auto array = value.toArray();
  return array.size() == 3
             ? TopologyPoint3d{array[0].toDouble(), array[1].toDouble(),
                               array[2].toDouble()}
             : TopologyPoint3d{};
}

QJsonObject topologyBounds(const TopologyBounds& bounds) {
  return {{"minimum", topologyPoint(bounds.minimum)},
          {"maximum", topologyPoint(bounds.maximum)}};
}

TopologyBounds readTopologyBounds(const QJsonValue& value) {
  const auto object = value.toObject();
  return {readTopologyPoint(object.value("minimum")),
          readTopologyPoint(object.value("maximum"))};
}

QJsonObject faceSignature(const FaceSignature& signature) {
  return {{"surfaceType", static_cast<int>(signature.surface)},
          {"area", signature.area},
          {"centroid", topologyPoint(signature.centroid)},
          {"normal", topologyPoint(signature.normal)},
          {"bounds", topologyBounds(signature.bounds)},
          {"radius", signature.radius},
          {"axis", topologyPoint(signature.axis)}};
}

std::optional<FaceSignature> readFaceSignature(const QJsonValue& value) {
  if (!value.isObject()) return std::nullopt;
  const auto object = value.toObject();
  FaceSignature result;
  result.surface =
      static_cast<SurfaceKind>(object.value("surfaceType").toInt());
  result.area = object.value("area").toDouble();
  result.centroid = readTopologyPoint(object.value("centroid"));
  result.normal = readTopologyPoint(object.value("normal"));
  result.bounds = readTopologyBounds(object.value("bounds"));
  result.radius = object.value("radius").toDouble();
  result.axis = readTopologyPoint(object.value("axis"));
  return result;
}

QJsonObject edgeSignature(const EdgeSignature& signature) {
  return {{"curveType", static_cast<int>(signature.curve)},
          {"length", signature.length},
          {"midpoint", topologyPoint(signature.midpoint)},
          {"tangent", topologyPoint(signature.tangent)},
          {"bounds", topologyBounds(signature.bounds)},
          {"radius", signature.radius},
          {"center", topologyPoint(signature.center)}};
}

std::optional<EdgeSignature> readEdgeSignature(const QJsonValue& value) {
  if (!value.isObject()) return std::nullopt;
  const auto object = value.toObject();
  EdgeSignature result;
  result.curve = static_cast<CurveKind>(object.value("curveType").toInt());
  result.length = object.value("length").toDouble();
  result.midpoint = readTopologyPoint(object.value("midpoint"));
  result.tangent = readTopologyPoint(object.value("tangent"));
  result.bounds = readTopologyBounds(object.value("bounds"));
  result.radius = object.value("radius").toDouble();
  result.center = readTopologyPoint(object.value("center"));
  return result;
}

QJsonObject savedFaceReference(const FaceReference& face) {
  QJsonObject result{{"bodyId", static_cast<qint64>(face.bodyId)},
                     {"featureId", static_cast<qint64>(face.featureId)},
                     {"faceIndex", static_cast<qint64>(face.faceIndex)}};
  if (!face.persistentTag.empty())
    result["persistentTag"] = QString::fromStdString(face.persistentTag);
  if (face.signature) result["signature"] = faceSignature(*face.signature);
  return result;
}

QJsonObject savedEdgeReference(const EdgeReference& edge) {
  QJsonObject result{{"bodyId", static_cast<qint64>(edge.bodyId)},
                     {"featureId", static_cast<qint64>(edge.featureId)},
                     {"edgeIndex", static_cast<qint64>(edge.edgeIndex)}};
  if (!edge.persistentTag.empty())
    result["persistentTag"] = QString::fromStdString(edge.persistentTag);
  if (edge.signature) result["signature"] = edgeSignature(*edge.signature);
  return result;
}

QJsonObject savedExtrudeProfileGeometry(const sketch::Sketch& geometry) {
  QJsonArray lines;
  for (const auto& line : geometry.lines()) {
    if (line.dashed) continue;
    lines.append(QJsonObject{{"x1", line.start.xMm},
                             {"y1", line.start.yMm},
                             {"x2", line.end.xMm},
                             {"y2", line.end.yMm},
                             {"elementId",
                              static_cast<qint64>(line.elementId)}});
  }

  QJsonArray circles;
  for (const auto& circle : geometry.circles()) {
    if (circle.dashed) continue;
    circles.append(QJsonObject{{"x", circle.center.xMm},
                               {"y", circle.center.yMm},
                               {"radius", circle.radiusMm}});
  }
  QJsonArray arcs;
  for (const auto& arc : geometry.arcs()) {
    if (arc.dashed) continue;
    arcs.append(QJsonObject{{"x", arc.center.xMm},
                            {"y", arc.center.yMm},
                            {"radius", arc.radiusMm},
                            {"startAngle", arc.startAngleRad},
                            {"sweepAngle", arc.sweepAngleRad}});
  }
  QJsonArray beziers;
  for (const auto& bezier : geometry.beziers()) {
    if (bezier.dashed) continue;
    beziers.append(QJsonObject{
        {"x0", bezier.points[0].xMm}, {"y0", bezier.points[0].yMm},
        {"x1", bezier.points[1].xMm}, {"y1", bezier.points[1].yMm},
        {"x2", bezier.points[2].xMm}, {"y2", bezier.points[2].yMm},
        {"x3", bezier.points[3].xMm}, {"y3", bezier.points[3].yMm}});
  }
  return QJsonObject{{"lines", lines}, {"circles", circles}, {"arcs", arcs},
                     {"beziers", beziers}};
}

sketch::Sketch loadedExtrudeProfileGeometry(const QJsonValue& value) {
  sketch::Sketch geometry;
  geometry.clear();
  const auto saved = value.toObject();

  for (const auto lineValue : saved.value("lines").toArray()) {
    const auto line = lineValue.toObject();
    geometry.addLine(
        {line.value("x1").toDouble(), line.value("y1").toDouble()},
        {line.value("x2").toDouble(), line.value("y2").toDouble()},
        static_cast<std::size_t>(line.value("elementId").toInteger()));
  }
  for (const auto circleValue : saved.value("circles").toArray()) {
    const auto circle = circleValue.toObject();
    geometry.addCircle(
        {circle.value("x").toDouble(), circle.value("y").toDouble()},
        circle.value("radius").toDouble());
  }
  for (const auto arcValue : saved.value("arcs").toArray()) {
    const auto arc = arcValue.toObject();
    geometry.addArc(
        {arc.value("x").toDouble(), arc.value("y").toDouble()},
        arc.value("radius").toDouble(),
        arc.value("startAngle").toDouble(),
        arc.value("sweepAngle").toDouble());
  }
  for (const auto bezierValue : saved.value("beziers").toArray()) {
    const auto bezier = bezierValue.toObject();
    geometry.addBezier({bezier.value("x0").toDouble(),
                        bezier.value("y0").toDouble()},
                       {bezier.value("x1").toDouble(),
                        bezier.value("y1").toDouble()},
                       {bezier.value("x2").toDouble(),
                        bezier.value("y2").toDouble()},
                       {bezier.value("x3").toDouble(),
                        bezier.value("y3").toDouble()});
  }
  return geometry;
}
FaceReference loadedFaceReference(const QJsonValue& value) {
  const auto saved = value.toObject();
  FaceReference result{
      static_cast<BodyId>(saved.value("bodyId").toInteger()),
      static_cast<FeatureId>(saved.value("featureId").toInteger()),
      static_cast<std::size_t>(saved.value("faceIndex").toInteger())};
  result.persistentTag = saved.value("persistentTag").toString().toStdString();
  result.signature = readFaceSignature(saved.value("signature"));
  return result;
}

EdgeReference loadedEdgeReference(const QJsonValue& value) {
  const auto saved = value.toObject();
  EdgeReference result{
      static_cast<BodyId>(saved.value("bodyId").toInteger()),
      static_cast<FeatureId>(saved.value("featureId").toInteger()),
      static_cast<std::size_t>(saved.value("edgeIndex").toInteger())};
  result.persistentTag = saved.value("persistentTag").toString().toStdString();
  result.signature = readEdgeSignature(saved.value("signature"));
  return result;
}

constexpr double kMaximumModelLengthMm = 100000.0;
constexpr double kMaximumCoordinateMagnitude = 1000000.0;
constexpr qint64 kMaximumExactJsonInteger = 9007199254740991LL;
constexpr double kMinimumGeometryLengthMm = 1.0e-9;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr auto kConstraintTypeEncodingKey = "constraintTypeEncoding";
constexpr auto kStableConstraintTypeEncoding = "stable-name-v1";

enum class ConstraintTypeEncoding {
  PrePointOnLineOrdinal,
  CurrentHistoricalOrdinal,
  CurrentOrdinal,
  StableNameV1
};

struct ConstraintTypeCodecEntry {
  sketch::ConstraintType type;
  int ordinal;
  const char* key;
};

constexpr std::array<ConstraintTypeCodecEntry, 22> kConstraintTypeCodec{{
    {sketch::ConstraintType::Horizontal, 0, "Horizontal"},
    {sketch::ConstraintType::Vertical, 1, "Vertical"},
    {sketch::ConstraintType::Coincident, 2, "Coincident"},
    {sketch::ConstraintType::PointOnLine, 3, "PointOnLine"},
    {sketch::ConstraintType::Distance, 4, "Distance"},
    {sketch::ConstraintType::Length, 5, "Length"},
    {sketch::ConstraintType::Radius, 6, "Radius"},
    {sketch::ConstraintType::Diameter, 7, "Diameter"},
    {sketch::ConstraintType::Parallel, 8, "Parallel"},
    {sketch::ConstraintType::Perpendicular, 9, "Perpendicular"},
    {sketch::ConstraintType::Equal, 10, "Equal"},
    {sketch::ConstraintType::Angle, 11, "Angle"},
    {sketch::ConstraintType::DistanceX, 12, "DistanceX"},
    {sketch::ConstraintType::DistanceY, 13, "DistanceY"},
    {sketch::ConstraintType::PointOnCircle, 14, "PointOnCircle"},
    {sketch::ConstraintType::Tangent, 15, "Tangent"},
    {sketch::ConstraintType::LineDistance, 16, "LineDistance"},
    {sketch::ConstraintType::Lock, 17, "Lock"},
    {sketch::ConstraintType::PointOnArc, 18, "PointOnArc"},
    {sketch::ConstraintType::Midpoint, 19, "Midpoint"},
    {sketch::ConstraintType::PointOnXAxis, 20, "PointOnXAxis"},
    {sketch::ConstraintType::PointOnYAxis, 21, "PointOnYAxis"},
}};

constexpr std::array<sketch::ConstraintType, 13>
    kPrePointOnLineConstraintTypes{{
        sketch::ConstraintType::Horizontal,
        sketch::ConstraintType::Vertical,
        sketch::ConstraintType::Coincident,
        sketch::ConstraintType::Distance,
        sketch::ConstraintType::Length,
        sketch::ConstraintType::Radius,
        sketch::ConstraintType::Diameter,
        sketch::ConstraintType::Parallel,
        sketch::ConstraintType::Perpendicular,
        sketch::ConstraintType::Equal,
        sketch::ConstraintType::Angle,
        sketch::ConstraintType::DistanceX,
        sketch::ConstraintType::DistanceY,
    }};

bool invalid(QString* error, const QString& message) {
  setError(error, QString::fromUtf8("Файл проекта повреждён: ") + message);
  return false;
}

bool boundedString(const QJsonValue& value, const QString& field,
                   QString* error, bool allowEmpty = true) {
  if (!value.isString())
    return invalid(error, QString::fromUtf8("поле %1 должно быть строкой.")
                              .arg(field));
  const QString text = value.toString();
  if ((!allowEmpty && text.isEmpty()) ||
      text.size() > ProjectFile::kMaximumStringCharacters)
    return invalid(error, QString::fromUtf8("недопустимая длина поля %1.")
                              .arg(field));
  return true;
}

bool finiteNumber(const QJsonValue& value, const QString& field, double minimum,
                  double maximum, QString* error) {
  if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
      value.toDouble() < minimum || value.toDouble() > maximum)
    return invalid(error, QString::fromUtf8("недопустимое число в поле %1.")
                              .arg(field));
  return true;
}

bool integerNumber(const QJsonValue& value, const QString& field,
                   qint64 minimum, qint64 maximum, qint64* result,
                   QString* error) {
  if (!value.isDouble())
    return invalid(error, QString::fromUtf8("поле %1 должно быть целым числом.")
                              .arg(field));
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(maximum))
    return invalid(error, QString::fromUtf8("недопустимое целое число в поле %1.")
                              .arg(field));
  if (result) *result = value.toInteger();
  return true;
}

bool enumNumber(const QJsonValue& value, const QString& field, int maximum,
                QString* error) {
  return integerNumber(value, field, 0, maximum, nullptr, error);
}

const ConstraintTypeCodecEntry* constraintTypeCodec(
    sketch::ConstraintType type) {
  for (const auto& entry : kConstraintTypeCodec)
    if (entry.type == type) return &entry;
  return nullptr;
}

const ConstraintTypeCodecEntry* constraintTypeCodec(int ordinal) {
  for (const auto& entry : kConstraintTypeCodec)
    if (entry.ordinal == ordinal) return &entry;
  return nullptr;
}

bool decodeConstraintType(const QJsonObject& constraint,
                          ConstraintTypeEncoding encoding,
                          sketch::ConstraintType* result, bool* unsupported,
                          QString* error) {
  if (encoding == ConstraintTypeEncoding::StableNameV1) {
    if (!boundedString(constraint.value("typeKey"),
                       QStringLiteral("constraint.typeKey"), error, false))
      return false;
    const QString key = constraint.value("typeKey").toString();
    const ConstraintTypeCodecEntry* keyed = nullptr;
    for (const auto& entry : kConstraintTypeCodec) {
      if (key == QLatin1String(entry.key)) {
        keyed = &entry;
        break;
      }
    }
    if (!keyed) {
      if (unsupported) *unsupported = true;
      setError(error, QString::fromUtf8("Неподдерживаемый тип ограничения: %1.")
                          .arg(key));
      return false;
    }
    qint64 stableOrdinal = -1;
    if (!integerNumber(constraint.value("type"),
                       QStringLiteral("constraint.type"), 0,
                       kMaximumExactJsonInteger, &stableOrdinal, error))
      return false;
    if (keyed->ordinal != stableOrdinal)
      return invalid(
          error,
          QString::fromUtf8("поля constraint.type и constraint.typeKey не согласованы."));
    if (result) *result = keyed->type;
    return true;
  }

  const int maximumOrdinal =
      encoding == ConstraintTypeEncoding::PrePointOnLineOrdinal
          ? static_cast<int>(kPrePointOnLineConstraintTypes.size() - 1)
          : encoding == ConstraintTypeEncoding::CurrentHistoricalOrdinal
                ? 15
                : static_cast<int>(kConstraintTypeCodec.size() - 1);
  qint64 ordinal = -1;
  if (!integerNumber(constraint.value("type"),
                     QStringLiteral("constraint.type"), 0, maximumOrdinal,
                     &ordinal, error))
    return false;

  if (constraint.contains("typeKey"))
    return invalid(error, QString::fromUtf8(
                              "typeKey задан без маркера кодировки ограничений."));

  if (encoding == ConstraintTypeEncoding::PrePointOnLineOrdinal) {
    if (result)
      *result = kPrePointOnLineConstraintTypes[static_cast<std::size_t>(ordinal)];
    return true;
  }

  const auto* entry = constraintTypeCodec(static_cast<int>(ordinal));
  if (!entry)
    return invalid(error, QString::fromUtf8("неизвестный ordinal ограничения."));
  if (result) *result = entry->type;
  return true;
}

int encodedConstraintType(sketch::ConstraintType type) {
  const auto* entry = constraintTypeCodec(type);
  return entry ? entry->ordinal : -1;
}

QString encodedConstraintTypeKey(sketch::ConstraintType type) {
  const auto* entry = constraintTypeCodec(type);
  return entry ? QString::fromLatin1(entry->key) : QString{};
}

bool booleanField(const QJsonObject& object, const char* name, QString* error,
                  bool required = true) {
  const QJsonValue value = object.value(QLatin1String(name));
  if (!required && value.isUndefined()) return true;
  if (!value.isBool())
    return invalid(error, QString::fromUtf8("поле %1 должно быть логическим.")
                              .arg(QString::fromLatin1(name)));
  return true;
}

bool arrayField(const QJsonObject& object, const char* name, QJsonArray* result,
                QString* error, bool required = true) {
  const QJsonValue value = object.value(QLatin1String(name));
  if (!required && value.isUndefined()) {
    *result = {};
    return true;
  }
  if (!value.isArray())
    return invalid(error, QString::fromUtf8("поле %1 должно быть массивом.")
                              .arg(QString::fromLatin1(name)));
  *result = value.toArray();
  if (result->size() > ProjectFile::kMaximumCollectionItems)
    return invalid(error, QString::fromUtf8("массив %1 превышает допустимый размер.")
                              .arg(QString::fromLatin1(name)));
  return true;
}

bool objectField(const QJsonObject& object, const char* name,
                 QJsonObject* result, QString* error, bool required = true) {
  const QJsonValue value = object.value(QLatin1String(name));
  if (!required && value.isUndefined()) {
    *result = {};
    return true;
  }
  if (!value.isObject())
    return invalid(error, QString::fromUtf8("поле %1 должно быть объектом.")
                              .arg(QString::fromLatin1(name)));
  *result = value.toObject();
  return true;
}

bool vector3Field(const QJsonValue& value, const QString& field,
                  QString* error) {
  if (!value.isArray() || value.toArray().size() != 3)
    return invalid(error, QString::fromUtf8("поле %1 должно содержать три координаты.")
                              .arg(field));
  const auto array = value.toArray();
  for (const auto coordinate : array) {
    if (!finiteNumber(coordinate, field, -kMaximumCoordinateMagnitude,
                      kMaximumCoordinateMagnitude, error))
      return false;
  }
  return true;
}

bool placementBasisFields(const QJsonValue& xValue, const QJsonValue& yValue,
                          QString* error) {
  const auto x = xValue.toArray();
  const auto y = yValue.toArray();
  const double xx = x[0].toDouble();
  const double xy = x[1].toDouble();
  const double xz = x[2].toDouble();
  const double yx = y[0].toDouble();
  const double yy = y[1].toDouble();
  const double yz = y[2].toDouble();
  const double xNorm = std::hypot(xx, xy, xz);
  const double yNorm = std::hypot(yx, yy, yz);
  if (xNorm <= 1.0e-12 || yNorm <= 1.0e-12)
    return invalid(error,
                   QString::fromUtf8("базис плоскости содержит нулевой вектор."));
  const double dot = xx * yx + xy * yy + xz * yz;
  if (std::abs(xNorm - 1.0) > 1.0e-9 ||
      std::abs(yNorm - 1.0) > 1.0e-9 || std::abs(dot) > 1.0e-9)
    return invalid(
        error,
        QString::fromUtf8(
            "базис плоскости должен быть ортонормированным."));
  const double crossX = xy * yz - xz * yy;
  const double crossY = xz * yx - xx * yz;
  const double crossZ = xx * yy - xy * yx;
  const double sine = std::hypot(crossX, crossY, crossZ) / (xNorm * yNorm);
  if (sine <= 1.0e-9)
    return invalid(error,
                   QString::fromUtf8("векторы базиса плоскости параллельны."));
  return true;
}

bool optionalIndex(const QJsonObject& object, const char* name,
                   qsizetype count, QString* error) {
  const auto value = object.value(QLatin1String(name));
  if (value.isUndefined()) return true;
  qint64 index = -1;
  if (!integerNumber(value, QString::fromLatin1(name), -1,
                     ProjectFile::kMaximumCollectionItems, &index, error))
    return false;
  return index < 0 || index < count
             ? true
             : invalid(error, QString::fromUtf8("ссылка %1 выходит за границы геометрии.")
                                  .arg(QString::fromLatin1(name)));
}

bool requiredIndex(const QJsonObject& object, const char* name,
                   qsizetype count, QString* error) {
  qint64 index = -1;
  if (!integerNumber(object.value(QLatin1String(name)),
                     QString::fromLatin1(name), 0,
                     ProjectFile::kMaximumCollectionItems, &index, error))
    return false;
  if (index >= count)
    return invalid(error, QString::fromUtf8("ссылка %1 выходит за границы геометрии.")
                              .arg(QString::fromLatin1(name)));
  return true;
}

enum class SavedGeometryKind { None, Line, Circle, Arc, Bezier };
enum class LegacySketchSchema {
  ProfileGeometry,
  EarlyV1,
  CurrentV1,
  V2,
  StableNameV1
};

bool validateLegacySketch(
    const QJsonObject& saved, LegacySketchSchema schema,
    ConstraintTypeEncoding constraintEncoding,
    std::vector<sketch::ConstraintType>* decodedConstraintTypes,
    bool* unsupported, QString* error) {
  if (!boundedString(saved.value("support"), QStringLiteral("support"), error,
                     false))
    return false;
  QJsonArray lines, circles, arcs, beziers, dimensions, constraints, centers;
  const bool profileGeometry = schema == LegacySketchSchema::ProfileGeometry;
  const bool strictConstraintCollections =
      schema != LegacySketchSchema::ProfileGeometry &&
      schema != LegacySketchSchema::EarlyV1;
  if (!arrayField(saved, "lines", &lines, error) ||
      !arrayField(saved, "circles", &circles, error) ||
      !arrayField(saved, "arcs", &arcs, error,
                  schema == LegacySketchSchema::StableNameV1) ||
      !arrayField(saved, "beziers", &beziers, error, false) ||
      !arrayField(saved, "dimensions", &dimensions, error,
                  !profileGeometry) ||
      !arrayField(saved, "constraints", &constraints, error,
                  strictConstraintCollections) ||
      !arrayField(saved, "centerNodeElementIds", &centers, error,
                  strictConstraintCollections))
    return false;
  const qsizetype geometryCount =
      lines.size() + circles.size() + arcs.size() + beziers.size();
  if (geometryCount > ProjectFile::kMaximumSketchGeometryItems)
    return invalid(error, QString::fromUtf8("эскиз содержит слишком много геометрии."));
  if (constraints.size() > ProjectFile::kMaximumSketchConstraintItems)
    return invalid(error,
                   QString::fromUtf8("эскиз содержит слишком много ограничений."));
  const qint64 solveComplexity =
      static_cast<qint64>(std::max<qsizetype>(1, geometryCount)) *
          static_cast<qint64>(constraints.size()) +
      static_cast<qint64>(constraints.size()) *
          static_cast<qint64>(constraints.size());
  if (solveComplexity > ProjectFile::kMaximumSketchSolveComplexity)
    return invalid(
        error,
        QString::fromUtf8("эскиз превышает допустимую сложность решателя."));

  std::unordered_set<std::size_t> elementIds;
  std::unordered_map<std::size_t, qsizetype> elementLineCounts;
  for (const auto value : lines) {
    if (!value.isObject()) return invalid(error, QString::fromUtf8("линия должна быть объектом."));
    const auto line = value.toObject();
    for (const char* key : {"x1", "y1", "x2", "y2"})
      if (!finiteNumber(line.value(QLatin1String(key)), QString::fromLatin1(key),
                        -kMaximumCoordinateMagnitude,
                        kMaximumCoordinateMagnitude, error))
        return false;
    if (std::hypot(line.value("x2").toDouble() - line.value("x1").toDouble(),
                   line.value("y2").toDouble() - line.value("y1").toDouble()) <=
        kMinimumGeometryLengthMm)
      return invalid(error, QString::fromUtf8("линия имеет нулевую длину."));
    if (!booleanField(line, "dashed", error, !profileGeometry)) return false;
    const QJsonValue elementIdValue = line.value("elementId");
    if (!profileGeometry || !elementIdValue.isUndefined()) {
      qint64 id = 0;
      if (!integerNumber(elementIdValue, QStringLiteral("elementId"), 1,
                         ProjectFile::kMaximumPersistedId, &id, error))
        return false;
      elementIds.insert(static_cast<std::size_t>(id));
      ++elementLineCounts[static_cast<std::size_t>(id)];
    }
  }
  for (const auto value : circles) {
    if (!value.isObject()) return invalid(error, QString::fromUtf8("окружность должна быть объектом."));
    const auto circle = value.toObject();
    if (!finiteNumber(circle.value("x"), QStringLiteral("x"),
                      -kMaximumCoordinateMagnitude, kMaximumCoordinateMagnitude,
                      error) ||
        !finiteNumber(circle.value("y"), QStringLiteral("y"),
                      -kMaximumCoordinateMagnitude, kMaximumCoordinateMagnitude,
                      error) ||
        !finiteNumber(circle.value("radius"), QStringLiteral("radius"), 0.0,
                      kMaximumModelLengthMm, error) ||
        circle.value("radius").toDouble() == 0.0 ||
        !booleanField(circle, "dashed", error, !profileGeometry))
      return false;
  }
  for (const auto value : arcs) {
    if (!value.isObject()) return invalid(error, QString::fromUtf8("дуга должна быть объектом."));
    const auto arc = value.toObject();
    if (!finiteNumber(arc.value("x"), QStringLiteral("x"),
                      -kMaximumCoordinateMagnitude, kMaximumCoordinateMagnitude,
                      error) ||
        !finiteNumber(arc.value("y"), QStringLiteral("y"),
                      -kMaximumCoordinateMagnitude, kMaximumCoordinateMagnitude,
                      error) ||
        !finiteNumber(arc.value("radius"), QStringLiteral("radius"), 0.0,
                      kMaximumModelLengthMm, error) ||
        arc.value("radius").toDouble() == 0.0 ||
        !finiteNumber(arc.value("startAngle"), QStringLiteral("startAngle"),
                      -1.0e6, 1.0e6, error) ||
        !finiteNumber(arc.value("sweepAngle"), QStringLiteral("sweepAngle"),
                      -1.0e6, 1.0e6, error) ||
        !booleanField(arc, "dashed", error, !profileGeometry))
      return false;
    const double sweepAngle = arc.value("sweepAngle").toDouble();
    if (sweepAngle <= kMinimumGeometryLengthMm ||
        sweepAngle >= kTwoPi - kMinimumGeometryLengthMm)
      return invalid(error, QString::fromUtf8("дуга имеет недопустимый угол."));
  }
  for (const auto value : beziers) {
    if (!value.isObject())
      return invalid(error, QString::fromUtf8("кривая Безье должна быть объектом."));
    const auto bezier = value.toObject();
    for (const char* key : {"x0", "y0", "x1", "y1", "x2", "y2", "x3", "y3"})
      if (!finiteNumber(bezier.value(QLatin1String(key)),
                        QString::fromLatin1(key),
                        -kMaximumCoordinateMagnitude,
                        kMaximumCoordinateMagnitude, error))
        return false;
    if (!booleanField(bezier, "dashed", error, !profileGeometry)) return false;
    if (std::hypot(bezier.value("x3").toDouble() -
                       bezier.value("x0").toDouble(),
                   bezier.value("y3").toDouble() -
                       bezier.value("y0").toDouble()) <=
            kMinimumGeometryLengthMm &&
        std::hypot(bezier.value("x2").toDouble() -
                       bezier.value("x1").toDouble(),
                   bezier.value("y2").toDouble() -
                       bezier.value("y1").toDouble()) <=
            kMinimumGeometryLengthMm)
      return invalid(error, QString::fromUtf8("кривая Безье вырождена."));
  }
  std::unordered_set<std::size_t> centersSet;
  for (const auto value : centers) {
    qint64 id = 0;
    if (!integerNumber(value, QStringLiteral("centerNodeElementIds"), 1,
                       ProjectFile::kMaximumPersistedId, &id, error) ||
        !elementIds.contains(static_cast<std::size_t>(id)) ||
        elementLineCounts[static_cast<std::size_t>(id)] != 4 ||
        !centersSet.insert(static_cast<std::size_t>(id)).second)
      return invalid(error, QString::fromUtf8("некорректный центр составного элемента."));
  }

  std::unordered_set<sketch::DimensionId> dimensionIds;
  for (const auto value : dimensions) {
    if (!value.isObject()) return invalid(error, QString::fromUtf8("размер должен быть объектом."));
    const auto dimension = value.toObject();
    if (dimension.contains("id")) {
      qint64 id = 0;
      if (!integerNumber(dimension.value("id"), QStringLiteral("id"), 1,
                         ProjectFile::kMaximumPersistedId, &id, error) ||
          !dimensionIds.insert(static_cast<sketch::DimensionId>(id)).second)
        return invalid(error,
                       QString::fromUtf8("идентификатор размера не уникален."));
    }
    for (const auto& pair : {
             std::pair{QStringLiteral("firstCircle"),
                       QStringLiteral("secondCircle")},
             std::pair{QStringLiteral("firstArc"),
                       QStringLiteral("secondArc")},
             std::pair{QStringLiteral("firstElementCenter"),
                       QStringLiteral("secondElementCenter")},
             std::pair{QStringLiteral("firstOrigin"),
                       QStringLiteral("secondOrigin")}}) {
      if (dimension.contains(pair.first) != dimension.contains(pair.second))
        return invalid(
            error,
            QString::fromUtf8("неполная группа полей размера %1/%2.")
                .arg(pair.first, pair.second));
    }
    if (schema == LegacySketchSchema::StableNameV1) {
      for (const char* key : {"geometryIndex", "secondGeometryIndex",
                              "firstLine", "firstCircle", "firstArc",
                              "secondLine", "secondCircle", "secondArc"}) {
        if (!dimension.contains(QLatin1String(key)))
          return invalid(
              error,
              QString::fromUtf8("в стабильной схеме отсутствует поле %1.")
                  .arg(QString::fromLatin1(key)));
      }
      for (const char* key : {"firstStart", "firstOrigin", "secondStart",
                              "secondOrigin"})
        if (!booleanField(dimension, key, error)) return false;
      for (const char* key : {"firstElementCenter",
                              "secondElementCenter"}) {
        qint64 ignored = 0;
        if (!integerNumber(dimension.value(QLatin1String(key)),
                           QString::fromLatin1(key), 0,
                           ProjectFile::kMaximumPersistedId, &ignored, error))
          return false;
      }
      qint64 ignored = 0;
      for (const char* key : {"geometryIndex", "secondGeometryIndex"})
        if (!integerNumber(dimension.value(QLatin1String(key)),
                           QString::fromLatin1(key), -1,
                           ProjectFile::kMaximumCollectionItems, &ignored,
                           error))
          return false;
      for (const char* key : {"firstLine", "secondLine"})
        if (!optionalIndex(dimension, key, lines.size(), error)) return false;
      for (const char* key : {"firstCircle", "secondCircle"})
        if (!optionalIndex(dimension, key, circles.size(), error)) return false;
      for (const char* key : {"firstArc", "secondArc"})
        if (!optionalIndex(dimension, key, arcs.size(), error)) return false;
      for (const char* key : {"firstBezier", "secondBezier"})
        if (!optionalIndex(dimension, key, beziers.size(), error)) return false;
      for (const char* key : {"firstBezierPoint", "secondBezierPoint"}) {
        const auto pointValue = dimension.value(QLatin1String(key));
        if (!pointValue.isUndefined()) {
          qint64 pointIndex = 0;
          if (!integerNumber(pointValue, QString::fromLatin1(key), 0, 3,
                             &pointIndex, error))
            return false;
        }
      }
    }
    qint64 kind = 0;
    if (!integerNumber(dimension.value("kind"), QStringLiteral("kind"), 0, 6,
                       &kind, error) ||
        !finiteNumber(dimension.value("value"), QStringLiteral("value"), 0.0,
                      kMaximumModelLengthMm, error) ||
        dimension.value("value").toDouble() == 0.0 ||
        !finiteNumber(dimension.value("offset"), QStringLiteral("offset"),
                      -kMaximumModelLengthMm, kMaximumModelLengthMm, error) ||
        !finiteNumber(dimension.value("angle"), QStringLiteral("angle"),
                      -1.0e6, 1.0e6, error))
      return false;
    if ((kind == 0 || kind == 2) &&
        !requiredIndex(dimension, "geometryIndex",
                       kind == 0 ? lines.size() : circles.size(), error))
      return false;
    if ((kind == 5 || kind == 6) &&
        (!requiredIndex(dimension, "geometryIndex", lines.size(), error) ||
         !requiredIndex(dimension, "secondGeometryIndex", lines.size(), error)))
      return false;
    const bool pointDimension =
        kind == static_cast<qint64>(sketch::DimensionKind::PointDistance) ||
        kind == static_cast<qint64>(sketch::DimensionKind::PointDistanceX) ||
        kind == static_cast<qint64>(sketch::DimensionKind::PointDistanceY);
    const auto dimensionPointSourceCount = [&](const QString& prefix) {
      int count = dimension.value(prefix + QStringLiteral("Origin")).toBool()
                      ? 1
                      : 0;
      for (const QString& suffix : {QStringLiteral("Line"),
                                    QStringLiteral("Circle"),
                                    QStringLiteral("Arc"),
                                    QStringLiteral("Bezier")})
        if (dimension.value(prefix + suffix).toInteger(-1) >= 0) ++count;
      if (dimension.value(prefix + QStringLiteral("ElementCenter"))
              .toInteger(0) > 0)
        ++count;
      return count;
    };
    if (pointDimension) {
      const qint64 legacyGeometryIndex =
          dimension.value("geometryIndex").toInteger(-1);
      const qint64 legacySecondGeometryIndex =
          dimension.value("secondGeometryIndex").toInteger(-1);
      const bool earlyV1Sentinel =
          schema == LegacySketchSchema::EarlyV1 &&
          !dimension.contains("secondGeometryIndex") &&
          (legacyGeometryIndex == -1 || legacyGeometryIndex == 0);
      if (!earlyV1Sentinel &&
          (legacyGeometryIndex >= 0 || legacySecondGeometryIndex >= 0))
        return invalid(
            error,
            QString::fromUtf8(
                "точечный размер содержит неиспользуемую ссылку на геометрию."));
    } else {
      const bool earlyV1IgnoredPointDefaults =
          schema == LegacySketchSchema::EarlyV1 &&
          !dimension.contains("secondGeometryIndex") &&
          (kind == static_cast<qint64>(sketch::DimensionKind::LineLength) ||
           kind ==
               static_cast<qint64>(sketch::DimensionKind::CircleDiameter)) &&
          dimension.value("firstLine").toInteger(-1) == 0 &&
          dimension.value("secondLine").toInteger(-1) == 0 &&
          !dimension.value("firstOrigin").toBool(false) &&
          !dimension.value("secondOrigin").toBool(false) &&
          dimension.value("firstCircle").toInteger(-1) < 0 &&
          dimension.value("secondCircle").toInteger(-1) < 0 &&
          dimension.value("firstArc").toInteger(-1) < 0 &&
          dimension.value("secondArc").toInteger(-1) < 0 &&
          dimension.value("firstBezier").toInteger(-1) < 0 &&
          dimension.value("secondBezier").toInteger(-1) < 0 &&
          dimension.value("firstElementCenter").toInteger(0) == 0 &&
          dimension.value("secondElementCenter").toInteger(0) == 0;
      if (!earlyV1IgnoredPointDefaults &&
          (dimensionPointSourceCount(QStringLiteral("first")) != 0 ||
           dimensionPointSourceCount(QStringLiteral("second")) != 0))
        return invalid(
            error,
            QString::fromUtf8(
                "размер содержит неиспользуемую точечную ссылку."));
    }
    if ((kind == static_cast<qint64>(sketch::DimensionKind::LineLength) ||
         kind == static_cast<qint64>(sketch::DimensionKind::CircleDiameter)) &&
        dimension.value("secondGeometryIndex").toInteger(-1) >= 0)
      return invalid(
          error,
          QString::fromUtf8(
              "одиночный размер содержит вторую геометрию."));
    if (kind == static_cast<qint64>(sketch::DimensionKind::LineAngle)) {
      if (dimension.value("value").toDouble() >= 180.0)
        return invalid(
            error,
            QString::fromUtf8("угловой размер должен быть меньше 180 градусов."));
    }
    if ((kind == static_cast<qint64>(sketch::DimensionKind::LineAngle) ||
         kind == static_cast<qint64>(sketch::DimensionKind::LineDistance)) &&
        dimension.value("geometryIndex").toInteger() ==
            dimension.value("secondGeometryIndex").toInteger())
      return invalid(
          error,
          QString::fromUtf8(
              "размер должен ссылаться на две разные линии."));
    if (pointDimension) {
      if (!booleanField(dimension, "firstOrigin", error,
                        schema == LegacySketchSchema::StableNameV1) ||
          !booleanField(dimension, "secondOrigin", error,
                        schema == LegacySketchSchema::StableNameV1) ||
          !booleanField(dimension, "firstStart", error) ||
          !booleanField(dimension, "secondStart", error) ||
          !optionalIndex(dimension, "firstLine", lines.size(), error) ||
          !optionalIndex(dimension, "secondLine", lines.size(), error) ||
          !optionalIndex(dimension, "firstCircle", circles.size(), error) ||
          !optionalIndex(dimension, "secondCircle", circles.size(), error) ||
          !optionalIndex(dimension, "firstArc", arcs.size(), error) ||
          !optionalIndex(dimension, "secondArc", arcs.size(), error) ||
          !optionalIndex(dimension, "firstBezier", beziers.size(), error) ||
          !optionalIndex(dimension, "secondBezier", beziers.size(), error))
        return false;
      std::array<qint64, 2> elementCenters{0, 0};
      for (std::size_t pointIndex = 0; pointIndex < 2; ++pointIndex) {
        const QString prefix = pointIndex == 0 ? QStringLiteral("first")
                                               : QStringLiteral("second");
        const QString centerKey = prefix + QStringLiteral("ElementCenter");
        const QJsonValue centerValue = dimension.value(centerKey);
        if (!centerValue.isUndefined() &&
            !integerNumber(centerValue, centerKey, 0,
                           ProjectFile::kMaximumPersistedId,
                           &elementCenters[pointIndex], error))
          return false;
        if (elementCenters[pointIndex] > 0 &&
            !centersSet.contains(
                static_cast<std::size_t>(elementCenters[pointIndex])))
          return invalid(
              error,
              QString::fromUtf8("размер ссылается на неизвестный центр."));

        if (schema == LegacySketchSchema::StableNameV1) {
          for (const QString& suffix : {QStringLiteral("Circle"),
                                        QStringLiteral("Arc"),
                                        QStringLiteral("ElementCenter")}) {
            if (!dimension.contains(prefix + suffix))
              return invalid(
                  error,
                  QString::fromUtf8("в стабильной схеме отсутствует поле %1.")
                      .arg(prefix + suffix));
          }
        }

        int sources = dimension.value(prefix + QStringLiteral("Origin")).toBool()
                          ? 1
                          : 0;
        for (const QString& suffix : {QStringLiteral("Line"),
                                      QStringLiteral("Circle"),
                                      QStringLiteral("Arc"),
                                      QStringLiteral("Bezier")})
          if (dimension.value(prefix + suffix).toInteger(-1) >= 0) ++sources;
        if (elementCenters[pointIndex] > 0) ++sources;
        if (sources != 1)
          return invalid(error,
                         pointIndex == 0
                             ? QString::fromUtf8("первая точка размера не задана однозначно.")
                             : QString::fromUtf8("вторая точка размера не задана однозначно."));
      }
    }
  }

  std::unordered_set<sketch::ConstraintId> constraintIds;
  for (const auto value : constraints) {
    if (!value.isObject()) return invalid(error, QString::fromUtf8("ограничение должно быть объектом."));
    const auto constraint = value.toObject();
    for (const auto& pair : {
             std::pair{QStringLiteral("firstPointCircle"),
                       QStringLiteral("secondPointCircle")},
             std::pair{QStringLiteral("firstPointArc"),
                       QStringLiteral("secondPointArc")},
             std::pair{QStringLiteral("firstPointElementCenter"),
                       QStringLiteral("secondPointElementCenter")},
             std::pair{QStringLiteral("firstPointOrigin"),
                       QStringLiteral("secondPointOrigin")}}) {
      if (constraint.contains(pair.first) != constraint.contains(pair.second))
        return invalid(
            error,
            QString::fromUtf8("неполная группа полей ограничения %1/%2.")
                .arg(pair.first, pair.second));
    }
    qint64 id = 0;
    if (!integerNumber(constraint.value("id"), QStringLiteral("constraint.id"),
                       1, ProjectFile::kMaximumPersistedId, &id, error) ||
        !constraintIds.insert(static_cast<sketch::ConstraintId>(id)).second)
      return invalid(error, QString::fromUtf8("ID ограничения равен нулю или повторяется."));
    sketch::ConstraintType constraintType{};
    if (!decodeConstraintType(constraint, constraintEncoding, &constraintType,
                              unsupported, error) ||
        !finiteNumber(constraint.value("value"),
                      QStringLiteral("constraint.value"),
                      -kMaximumModelLengthMm, kMaximumModelLengthMm, error))
      return false;
    std::array<SavedGeometryKind, 2> geometryKinds{
        SavedGeometryKind::None, SavedGeometryKind::None};
    std::array<qint64, 2> geometryPositions{-1, -1};
    const std::array<QString, 2> geometryPrefixes{
        QStringLiteral("first"), QStringLiteral("second")};
    for (std::size_t referenceIndex = 0;
         referenceIndex < geometryPrefixes.size(); ++referenceIndex) {
      const auto& prefix = geometryPrefixes[referenceIndex];
      const QString geometryKey = prefix + QStringLiteral("Geometry");
      const QString kindKey = prefix + QStringLiteral("GeometryKind");
      qint64& position = geometryPositions[referenceIndex];
      if (!integerNumber(constraint.value(geometryKey), geometryKey, -1,
                         ProjectFile::kMaximumCollectionItems, &position, error))
        return false;
      const QString kindName = constraint.value(kindKey).toString();
      if (!constraint.value(kindKey).isString() ||
          (kindName != QStringLiteral("line") &&
           kindName != QStringLiteral("circle") &&
           kindName != QStringLiteral("arc") &&
           kindName != QStringLiteral("bezier") && !kindName.isEmpty()))
        return invalid(error, QString::fromUtf8("неизвестный вид геометрии ограничения."));
      if ((position < 0) != kindName.isEmpty())
        return invalid(error, QString::fromUtf8("несогласованная ссылка ограничения."));
      if (kindName == QStringLiteral("line"))
        geometryKinds[referenceIndex] = SavedGeometryKind::Line;
      else if (kindName == QStringLiteral("circle"))
        geometryKinds[referenceIndex] = SavedGeometryKind::Circle;
      else if (kindName == QStringLiteral("arc"))
        geometryKinds[referenceIndex] = SavedGeometryKind::Arc;
      else if (kindName == QStringLiteral("bezier"))
        geometryKinds[referenceIndex] = SavedGeometryKind::Bezier;
      const qsizetype count = kindName == QStringLiteral("line") ? lines.size()
                                : kindName == QStringLiteral("circle") ? circles.size()
                                : kindName == QStringLiteral("arc") ? arcs.size()
                                                                      : beziers.size();
      if (position >= count)
        return invalid(error, QString::fromUtf8("ссылка ограничения выходит за границы геометрии."));
    }
    for (const char* key : {"firstPointLine", "secondPointLine"}) {
      if (schema == LegacySketchSchema::StableNameV1 &&
          !constraint.contains(QLatin1String(key)))
        return invalid(error,
                       QString::fromUtf8(
                           "в стабильной схеме отсутствует поле %1.")
                           .arg(QString::fromLatin1(key)));
      if (!optionalIndex(constraint, key, lines.size(), error)) return false;
    }
    for (const char* key : {"firstPointCircle", "secondPointCircle"}) {
      if (schema == LegacySketchSchema::StableNameV1 &&
          !constraint.contains(QLatin1String(key)))
        return invalid(error,
                       QString::fromUtf8(
                           "в стабильной схеме отсутствует поле %1.")
                           .arg(QString::fromLatin1(key)));
      if (!optionalIndex(constraint, key, circles.size(), error)) return false;
    }
    for (const char* key : {"firstPointArc", "secondPointArc"}) {
      if (schema == LegacySketchSchema::StableNameV1 &&
          !constraint.contains(QLatin1String(key)))
        return invalid(error,
                       QString::fromUtf8(
                           "в стабильной схеме отсутствует поле %1.")
                           .arg(QString::fromLatin1(key)));
      if (!optionalIndex(constraint, key, arcs.size(), error)) return false;
    }
    for (const char* key : {"firstPointBezier", "secondPointBezier"})
      if (!optionalIndex(constraint, key, beziers.size(), error)) return false;
    for (const char* key : {"firstPointBezierPoint",
                            "secondPointBezierPoint"}) {
      const auto pointValue = constraint.value(QLatin1String(key));
      if (!pointValue.isUndefined()) {
        qint64 pointIndex = 0;
        if (!integerNumber(pointValue, QString::fromLatin1(key), 0, 3,
                           &pointIndex, error))
          return false;
      }
    }
    for (const char* key : {"firstPointStart", "secondPointStart"})
      if (!booleanField(constraint, key, error)) return false;
    for (const char* key : {"firstPointOrigin", "secondPointOrigin"})
      if (!booleanField(constraint, key, error, false))
        return false;
    for (const char* key : {"firstPointElementCenter", "secondPointElementCenter"}) {
      const QJsonValue centerValue = constraint.value(QLatin1String(key));
      qint64 centerId = 0;
      if (!centerValue.isUndefined() &&
          !integerNumber(centerValue, QString::fromLatin1(key), 0,
                         ProjectFile::kMaximumPersistedId, &centerId, error))
        return false;
      if (centerId > 0 && !centersSet.contains(static_cast<std::size_t>(centerId)))
        return invalid(error, QString::fromUtf8("ограничение ссылается на неизвестный центр."));
    }
    auto pointSourceCount = [&](const char* prefix) {
      const QString base = QString::fromLatin1(prefix);
      int count = constraint.value(base + QStringLiteral("PointOrigin")).toBool()
                      ? 1
                      : 0;
      for (const QString suffix : {QStringLiteral("PointLine"),
                                   QStringLiteral("PointCircle"),
                                   QStringLiteral("PointArc"),
                                   QStringLiteral("PointBezier")})
        if (constraint.value(base + suffix).toInteger(-1) >= 0) ++count;
      if (constraint.value(base + QStringLiteral("PointElementCenter"))
              .toInteger(0) > 0)
        ++count;
      return count;
    };
    const int firstPointSources = pointSourceCount("first");
    const int secondPointSources = pointSourceCount("second");
    if (firstPointSources > 1 || secondPointSources > 1)
      return invalid(error, QString::fromUtf8("точка ограничения имеет несколько источников."));
    const bool firstGeometry = geometryKinds[0] != SavedGeometryKind::None;
    const bool secondGeometry = geometryKinds[1] != SavedGeometryKind::None;
    const bool noPointReferences =
        firstPointSources == 0 && secondPointSources == 0;
    const auto require = [&](bool condition, const QString& message) {
      return condition ? true : invalid(error, message);
    };
    const double constraintValue = constraint.value("value").toDouble();
    const bool distinctGeometry =
        geometryKinds[0] != geometryKinds[1] ||
        geometryPositions[0] != geometryPositions[1];
    switch (constraintType) {
      case sketch::ConstraintType::Horizontal:
      case sketch::ConstraintType::Vertical:
      case sketch::ConstraintType::Length:
        if (!require(geometryKinds[0] == SavedGeometryKind::Line &&
                         !secondGeometry && noPointReferences,
                     QString::fromUtf8(
                         "ограничение должно ссылаться на одну линию.")))
          return false;
        if (constraintType == sketch::ConstraintType::Length &&
            !require(constraintValue > 0.0,
                     QString::fromUtf8("длина ограничения должна быть положительной.")))
          return false;
        break;
      case sketch::ConstraintType::Radius:
      case sketch::ConstraintType::Diameter:
        if (!require(geometryKinds[0] == SavedGeometryKind::Circle &&
                         !secondGeometry && noPointReferences &&
                         constraintValue > 0.0,
                     QString::fromUtf8(
                         "размер окружности содержит некорректную ссылку или значение.")))
          return false;
        break;
      case sketch::ConstraintType::Lock:
        if (!require(firstGeometry && !secondGeometry && noPointReferences,
                     QString::fromUtf8(
                         "фиксация должна ссылаться на одну геометрию.")))
          return false;
        break;
      case sketch::ConstraintType::Parallel:
      case sketch::ConstraintType::Perpendicular:
      case sketch::ConstraintType::Angle:
      case sketch::ConstraintType::LineDistance:
        if (!require(geometryKinds[0] == SavedGeometryKind::Line &&
                         geometryKinds[1] == SavedGeometryKind::Line &&
                         distinctGeometry && noPointReferences,
                     QString::fromUtf8(
                         "ограничение должно ссылаться на две разные линии.")))
          return false;
        if (constraintType == sketch::ConstraintType::Angle &&
            !require(constraintValue > 0.0 && constraintValue < 180.0,
                     QString::fromUtf8("угол ограничения должен быть между 0 и 180 градусами.")))
          return false;
        if (constraintType == sketch::ConstraintType::LineDistance &&
            !require(constraintValue > 0.0,
                     QString::fromUtf8("расстояние между линиями должно быть положительным.")))
          return false;
        break;
      case sketch::ConstraintType::Equal:
        if (!require(
                distinctGeometry && noPointReferences &&
                    ((geometryKinds[0] == SavedGeometryKind::Line &&
                      geometryKinds[1] == SavedGeometryKind::Line) ||
                     (geometryKinds[0] == SavedGeometryKind::Circle &&
                      geometryKinds[1] == SavedGeometryKind::Circle)),
                QString::fromUtf8(
                    "равенство должно связывать две разные линии или окружности.")))
          return false;
        break;
      case sketch::ConstraintType::Tangent:
        if (!require(geometryKinds[0] == SavedGeometryKind::Line &&
                         (geometryKinds[1] == SavedGeometryKind::Circle ||
                          geometryKinds[1] == SavedGeometryKind::Arc) &&
                         noPointReferences,
                     QString::fromUtf8(
                         "касательность должна связывать линию с окружностью или дугой.")))
          return false;
        break;
      case sketch::ConstraintType::Coincident:
      case sketch::ConstraintType::Distance:
      case sketch::ConstraintType::DistanceX:
      case sketch::ConstraintType::DistanceY:
        if (!require(!firstGeometry && !secondGeometry &&
                         firstPointSources == 1 && secondPointSources == 1,
                     QString::fromUtf8("ограничение не содержит две точки.")))
          return false;
        if (constraintType != sketch::ConstraintType::Coincident &&
            !require(constraintValue > 0.0,
                     QString::fromUtf8("расстояние ограничения должно быть положительным.")))
          return false;
        break;
      case sketch::ConstraintType::Midpoint:
      case sketch::ConstraintType::PointOnLine:
        if (!require(geometryKinds[0] == SavedGeometryKind::Line &&
                         !secondGeometry && firstPointSources == 0 &&
                         secondPointSources == 1,
                     QString::fromUtf8("точечное ограничение повреждено.")))
          return false;
        break;
      case sketch::ConstraintType::PointOnCircle:
        if (!require(geometryKinds[0] == SavedGeometryKind::Circle &&
                         !secondGeometry && firstPointSources == 0 &&
                         secondPointSources == 1,
                     QString::fromUtf8("ограничение точки на окружности повреждено.")))
          return false;
        break;
      case sketch::ConstraintType::PointOnArc:
        if (!require(geometryKinds[0] == SavedGeometryKind::Arc &&
                         !secondGeometry && firstPointSources == 0 &&
                         secondPointSources == 1,
                     QString::fromUtf8("ограничение точки на дуге повреждено.")))
          return false;
        break;
      case sketch::ConstraintType::PointOnXAxis:
      case sketch::ConstraintType::PointOnYAxis:
        if (!require(!firstGeometry && !secondGeometry &&
                         firstPointSources == 0 && secondPointSources == 1,
                     QString::fromUtf8("осевое ограничение не содержит точку.")))
          return false;
        break;
    }
    if (decodedConstraintTypes)
      decodedConstraintTypes->push_back(constraintType);
  }
  return true;
}

bool validateLegacyRoot(
    const QJsonObject& root, bool allowEarlyV1Defaults,
    LegacySketchSchema sketchSchema,
    ConstraintTypeEncoding constraintEncoding,
    std::vector<sketch::ConstraintType>* decodedConstraintTypes,
    bool* unsupported, QString* error) {
  for (const char* key : {"name", "createdAt"}) {
    const auto value = root.value(QLatin1String(key));
    if (sketchSchema == LegacySketchSchema::StableNameV1 &&
        value.isUndefined())
      return invalid(
          error,
          QString::fromUtf8("в стабильной схеме отсутствует поле %1.")
              .arg(QString::fromLatin1(key)));
    if (!value.isUndefined() &&
        !boundedString(value, QString::fromLatin1(key), error))
      return false;
  }
  QJsonObject document;
  if (!objectField(root, "document", &document, error)) return false;
  for (const char* key : {"widthMm", "heightMm", "extrusionMm"})
    if (!finiteNumber(document.value(QLatin1String(key)),
                      QString::fromLatin1(key), 0.0,
                      kMaximumModelLengthMm, error) ||
        document.value(QLatin1String(key)).toDouble() == 0.0)
      return false;
  QJsonArray sketches;
  const bool hasSketches = root.contains("sketches");
  const bool hasExtrusion = root.contains("extrusion");
  if (allowEarlyV1Defaults && hasSketches != hasExtrusion)
    return invalid(
        error,
        QString::fromUtf8(
            "legacy-поля sketches и extrusion должны присутствовать вместе."));
  const bool earlyRootDefaults =
      allowEarlyV1Defaults && !hasSketches && !hasExtrusion;
  if (!arrayField(root, "sketches", &sketches, error, !earlyRootDefaults))
    return false;
  if (sketches.size() > ProjectFile::kMaximumDocumentSketches)
    return invalid(error,
                   QString::fromUtf8("документ содержит слишком много эскизов."));
  qsizetype totalGeometryCount = 0;
  qsizetype totalConstraintCount = 0;
  qint64 totalSolveComplexity = 0;
  for (const auto value : sketches) {
    if (!value.isObject() ||
        !validateLegacySketch(value.toObject(), sketchSchema,
                              constraintEncoding, decodedConstraintTypes,
                              unsupported, error))
      return false;
    const auto sketch = value.toObject();
    const qsizetype geometryCount = sketch.value("lines").toArray().size() +
                                    sketch.value("circles").toArray().size() +
                                    sketch.value("arcs").toArray().size() +
                                    sketch.value("beziers").toArray().size();
    const qsizetype constraintCount =
        sketch.value("constraints").toArray().size();
    totalGeometryCount += geometryCount;
    totalConstraintCount += constraintCount;
    totalSolveComplexity +=
        static_cast<qint64>(std::max<qsizetype>(1, geometryCount)) *
            static_cast<qint64>(constraintCount) +
        static_cast<qint64>(constraintCount) *
            static_cast<qint64>(constraintCount);
    if (totalGeometryCount >
            ProjectFile::kMaximumDocumentSketchGeometryItems ||
        totalConstraintCount >
            ProjectFile::kMaximumDocumentSketchConstraintItems ||
        totalSolveComplexity >
            ProjectFile::kMaximumDocumentSketchSolveComplexity)
      return invalid(
          error,
          QString::fromUtf8(
              "документ превышает общий лимит геометрии или ограничений."));
  }
  QJsonObject extrusion;
  if (!objectField(root, "extrusion", &extrusion, error,
                   !earlyRootDefaults))
    return false;
  if (hasExtrusion && !booleanField(extrusion, "enabled", error)) return false;
  if (extrusion.contains("sourceSketch")) {
    qint64 index = 0;
    if (!integerNumber(extrusion.value("sourceSketch"),
                       QStringLiteral("sourceSketch"), 0,
                       ProjectFile::kMaximumCollectionItems, &index, error) ||
        index >= sketches.size())
      return invalid(error, QString::fromUtf8("источник legacy-выдавливания не существует."));
  }
  return true;
}

using FeatureSets =
    std::unordered_map<BodyId, std::unordered_set<FeatureId>>;

bool validateTopologyPointValue(const QJsonValue& value, const QString& field,
                                QString* error) {
  return vector3Field(value, field, error);
}

bool validateSignature(const QJsonValue& value, bool face, QString* error) {
  if (value.isUndefined()) return true;
  if (!value.isObject()) return invalid(error, QString::fromUtf8("сигнатура топологии должна быть объектом."));
  const auto signature = value.toObject();
  if (!enumNumber(signature.value(face ? "surfaceType" : "curveType"),
                  face ? QStringLiteral("surfaceType") : QStringLiteral("curveType"),
                  face ? static_cast<int>(SurfaceKind::Cylinder)
                       : static_cast<int>(CurveKind::Other), error) ||
      !finiteNumber(signature.value(face ? "area" : "length"),
                    face ? QStringLiteral("area") : QStringLiteral("length"),
                    0.0, 1.0e18, error) ||
      !validateTopologyPointValue(signature.value(face ? "centroid" : "midpoint"),
                                  face ? QStringLiteral("centroid") : QStringLiteral("midpoint"), error) ||
      !validateTopologyPointValue(signature.value(face ? "normal" : "tangent"),
                                  face ? QStringLiteral("normal") : QStringLiteral("tangent"), error) ||
      !finiteNumber(signature.value("radius"), QStringLiteral("radius"),
                    0.0, kMaximumCoordinateMagnitude, error) ||
      !validateTopologyPointValue(signature.value(face ? "axis" : "center"),
                                  face ? QStringLiteral("axis") : QStringLiteral("center"), error))
    return false;
  QJsonObject bounds;
  if (!objectField(signature, "bounds", &bounds, error) ||
      !validateTopologyPointValue(bounds.value("minimum"), QStringLiteral("minimum"), error) ||
      !validateTopologyPointValue(bounds.value("maximum"), QStringLiteral("maximum"), error))
    return false;
  const auto unitDirection = [](const QJsonValue& direction) {
    const auto coordinates = direction.toArray();
    const double norm =
        std::hypot(coordinates[0].toDouble(), coordinates[1].toDouble(),
                   coordinates[2].toDouble());
    return std::abs(norm - 1.0) <= 1.0e-6;
  };
  const int kind =
      signature.value(face ? "surfaceType" : "curveType").toInt();
  if (face && kind == static_cast<int>(SurfaceKind::Plane) &&
      !unitDirection(signature.value("normal")))
    return invalid(error,
                   QString::fromUtf8("нормаль плоскости некорректна."));
  if (face && kind == static_cast<int>(SurfaceKind::Cylinder) &&
      (!unitDirection(signature.value("axis")) ||
       signature.value("radius").toDouble() <= 0.0))
    return invalid(error,
                   QString::fromUtf8("ось или радиус цилиндра некорректны."));
  if (!face && !unitDirection(signature.value("tangent")))
    return invalid(error,
                   QString::fromUtf8("касательная ребра некорректна."));
  if (!face && kind == static_cast<int>(CurveKind::Circle) &&
      signature.value("radius").toDouble() <= 0.0)
    return invalid(error,
                   QString::fromUtf8("радиус кругового ребра некорректен."));
  const auto minimum = bounds.value("minimum").toArray();
  const auto maximum = bounds.value("maximum").toArray();
  for (qsizetype coordinate = 0; coordinate < 3; ++coordinate)
    if (minimum[coordinate].toDouble() > maximum[coordinate].toDouble())
      return invalid(error,
                     QString::fromUtf8("границы топологии перепутаны."));
  return true;
}

bool validateFeatureReference(BodyId bodyId, FeatureId featureId,
                              const FeatureSets& features, QString* error) {
  const auto body = features.find(bodyId);
  if (body == features.end() || !body->second.contains(featureId))
    return invalid(error, QString::fromUtf8("ссылка указывает на неизвестную фичу."));
  return true;
}

bool validateTopologyReference(const QJsonValue& value, bool face,
                               const FeatureSets& features, QString* error) {
  if (!value.isObject()) return invalid(error, QString::fromUtf8("топологическая ссылка должна быть объектом."));
  const auto reference = value.toObject();
  qint64 bodyId = 0, featureId = 0, index = 0;
  const char* indexName = face ? "faceIndex" : "edgeIndex";
  if (!integerNumber(reference.value("bodyId"), QStringLiteral("bodyId"), 1,
                     ProjectFile::kMaximumPersistedId, &bodyId, error) ||
      !integerNumber(reference.value("featureId"), QStringLiteral("featureId"),
                     1, ProjectFile::kMaximumPersistedId, &featureId, error) ||
      !integerNumber(reference.value(QLatin1String(indexName)),
                     QString::fromLatin1(indexName), 0,
                     kMaximumExactJsonInteger, &index, error) ||
      !validateFeatureReference(static_cast<BodyId>(bodyId),
                                static_cast<FeatureId>(featureId), features,
                                error))
    return false;
  if (reference.contains("persistentTag") &&
      !boundedString(reference.value("persistentTag"),
                     QStringLiteral("persistentTag"), error))
    return false;
  return validateSignature(reference.value("signature"), face, error);
}

bool validateProfileGeometry(const QJsonValue& value, bool stableSchema,
                             QString* error) {
  if (!value.isObject()) return invalid(error, QString::fromUtf8("профиль должен быть объектом."));
  QJsonObject profile = value.toObject();
  if (stableSchema && !profile.contains("arcs"))
    return invalid(
        error,
        QString::fromUtf8(
            "в стабильной схеме профиль не содержит массив arcs."));
  if (stableSchema) {
    for (const auto lineValue : profile.value("lines").toArray()) {
      if (!lineValue.isObject() ||
          !lineValue.toObject().contains("elementId"))
        return invalid(
            error,
            QString::fromUtf8(
                "в стабильной схеме линия профиля не содержит elementId."));
      qint64 elementId = 0;
      if (!integerNumber(lineValue.toObject().value("elementId"),
                         QStringLiteral("profile.elementId"), 1,
                         ProjectFile::kMaximumPersistedId, &elementId, error))
        return false;
    }
  }
  profile["support"] = QStringLiteral("XY");
  return validateLegacySketch(profile, LegacySketchSchema::ProfileGeometry,
                              ConstraintTypeEncoding::CurrentOrdinal, nullptr,
                              nullptr, error);
}

struct FeatureValidationContext {
  bool stableSchema{false};
  BodyId bodyId{kInvalidBodyId};
  qsizetype featureIndex{0};
  const QJsonArray& bodyFeatures;
  const FeatureSets& featureSets;
  QString* error{};
  qint64& totalRebuildWork;
  std::function<bool(const QJsonObject&, const char*)> localFeature;
  std::function<bool(const QJsonObject&, const char*)> sketchReference;
  std::function<bool(const QJsonObject&, const char*, const char*, const char*)>
      sketchLineReference;
  std::function<bool(const QJsonObject&, const char*, const char*)>
      externalFeature;
  std::function<bool(qsizetype)> accountTopologyReferences;
  std::function<bool(const QJsonValue&)> accountProfileGeometry;
};

struct FeatureEncodeContext {
  const Document& document;
  QString* error{};
};

struct FeatureDecodeContext {
  Document& document;
  QString* error{};
  std::function<sketch::GeometryId(const QJsonObject&, const char*,
                                   const char*, const char*)>
      restoredSketchLineId;
};

using ValidateFeatureFn = bool (*)(const QJsonObject&,
                                   FeatureValidationContext&);
using EncodeFeatureFn = bool (*)(const Feature&, QJsonObject&,
                                 const FeatureEncodeContext&);
using DecodeFeatureFn = bool (*)(const QJsonObject&, FeatureId,
                                 const std::string&, Body&,
                                 const FeatureDecodeContext&);

struct FeatureCodecEntry {
  FeatureKind kind;
  std::string_view token;
  ValidateFeatureFn validate;
  EncodeFeatureFn encode;
  DecodeFeatureFn decode;
};

bool validateImportedShape(const QJsonObject& feature,
                           FeatureValidationContext& context) {
  if (!feature.value("brep").isString() ||
      feature.value("brep").toString().isEmpty() ||
      feature.value("brep").toString().size() >
          ProjectFile::kMaximumBRepBase64Characters)
    return invalid(
        context.error,
        QString::fromUtf8("B-Rep отсутствует или превышает допустимый размер."));
  const auto decoded = QByteArray::fromBase64Encoding(
      feature.value("brep").toString().toLatin1(),
      QByteArray::AbortOnBase64DecodingErrors);
  if (!decoded || decoded.decoded.isEmpty())
    return invalid(context.error,
                   QString::fromUtf8("B-Rep содержит некорректный base64."));
  return true;
}

bool validateExtrude(const QJsonObject& feature,
                     FeatureValidationContext& context) {
  if (!finiteNumber(feature.value("lengthMm"), QStringLiteral("lengthMm"),
                    std::numeric_limits<double>::min(), kMaximumModelLengthMm,
                    context.error) ||
      !enumNumber(feature.value("operation"), QStringLiteral("operation"), 2,
                  context.error) ||
      !booleanField(feature, "reversed", context.error))
    return false;
  const auto sourceKindValue = feature.value("sourceKind");
  if (context.stableSchema && sourceKindValue.isUndefined())
    return invalid(
        context.error,
        QString::fromUtf8("в стабильной схеме Extrude не содержит sourceKind."));
  const QString sourceKind = sourceKindValue.isUndefined()
                                 ? QStringLiteral("sketch")
                                 : sourceKindValue.toString();
  if ((!sourceKindValue.isUndefined() && !sourceKindValue.isString()) ||
      (sourceKind != QStringLiteral("sketch") &&
       sourceKind != QStringLiteral("face")))
    return invalid(context.error,
                   QString::fromUtf8("неизвестный источник Extrude."));
  if (sourceKind == QStringLiteral("face")) {
    if (!context.accountTopologyReferences(1))
      return invalid(
          context.error,
          QString::fromUtf8(
              "топологические ссылки документа превышают лимит."));
    if (!validateTopologyReference(feature.value("face"), true,
                                   context.featureSets, context.error))
      return false;
  } else if (!context.sketchReference(feature, "sketchId")) {
    return false;
  }
  if (feature.contains("profileOverride")) {
    if (!validateProfileGeometry(feature.value("profileOverride"),
                                 context.stableSchema, context.error))
      return false;
    if (!context.accountProfileGeometry(feature.value("profileOverride")))
      return invalid(
          context.error,
          QString::fromUtf8("профили фич превышают общий лимит геометрии."));
  }
  return true;
}

bool validateRevolve(const QJsonObject& feature,
                     FeatureValidationContext& context) {
  qint64 axisType = 0;
  if (!context.sketchReference(feature, "profileSketchId") ||
      !integerNumber(feature.value("axisType"), QStringLiteral("axisType"), 0,
                     5, &axisType, context.error) ||
      !finiteNumber(feature.value("angleDeg"), QStringLiteral("angleDeg"),
                    std::numeric_limits<double>::min(), 360.0,
                    context.error) ||
      !enumNumber(feature.value("operation"), QStringLiteral("operation"), 2,
                  context.error) ||
      !booleanField(feature, "reversed", context.error))
    return false;
  if (axisType <= 2 &&
      !context.sketchReference(feature, "axisSketchId"))
    return false;
  if (axisType == static_cast<int>(AxisReferenceType::SketchLine) &&
      !context.sketchLineReference(feature, "axisSketchId", "axisLineId",
                                   "axisLineIndex"))
    return false;
  if (feature.contains("profileOverride")) {
    if (!validateProfileGeometry(feature.value("profileOverride"),
                                 context.stableSchema, context.error))
      return false;
    if (!context.accountProfileGeometry(feature.value("profileOverride")))
      return invalid(
          context.error,
          QString::fromUtf8("профили фич превышают общий лимит геометрии."));
  }
  return true;
}

bool validatePocket(const QJsonObject& feature,
                    FeatureValidationContext& context) {
  return context.sketchReference(feature, "sketchId") &&
         finiteNumber(feature.value("depthMm"), QStringLiteral("depthMm"),
                      std::numeric_limits<double>::min(),
                      kMaximumModelLengthMm, context.error);
}

bool validateEdgeTreatment(const QJsonObject& feature,
                           FeatureValidationContext& context,
                           const char* parameterKey) {
  QJsonArray references;
  if (!arrayField(feature, "edges", &references, context.error) ||
      references.isEmpty())
    return invalid(context.error,
                   QString::fromUtf8("список рёбер пуст или повреждён."));
  if (!context.accountTopologyReferences(references.size()))
    return invalid(
        context.error,
        QString::fromUtf8(
            "выбор рёбер превышает лимит топологических ссылок."));
  for (const auto reference : references)
    if (!validateTopologyReference(reference, false, context.featureSets,
                                   context.error))
      return false;
  return finiteNumber(feature.value(QLatin1String(parameterKey)),
                      QString::fromLatin1(parameterKey),
                      std::numeric_limits<double>::min(),
                      kMaximumModelLengthMm, context.error);
}

bool validateFillet(const QJsonObject& feature,
                    FeatureValidationContext& context) {
  return validateEdgeTreatment(feature, context, "radiusMm");
}

bool validateChamfer(const QJsonObject& feature,
                     FeatureValidationContext& context) {
  return validateEdgeTreatment(feature, context, "distanceMm");
}

bool validateMirror(const QJsonObject& feature,
                    FeatureValidationContext& context) {
  return context.localFeature(feature, "sourceFeatureId") &&
         enumNumber(feature.value("plane"), QStringLiteral("plane"),
                    static_cast<qint64>(MirrorPlane::YZ), context.error);
}

bool validateMove(const QJsonObject& feature,
                  FeatureValidationContext& context) {
  if (!context.localFeature(feature, "sourceFeatureId")) return false;
  for (const char* key : {"offsetXmm", "offsetYmm", "offsetZmm"})
    if (!finiteNumber(feature.value(QLatin1String(key)),
                      QString::fromLatin1(key), -kMaximumModelLengthMm,
                      kMaximumModelLengthMm, context.error))
      return false;
  return true;
}

bool validatePattern(const QJsonObject& feature,
                     FeatureValidationContext& context, bool linear) {
  qint64 count = 0;
  const QJsonValue operationValue = feature.value("operation");
  if ((context.stableSchema || !operationValue.isUndefined()) &&
      !enumNumber(operationValue, QStringLiteral("operation"),
                  static_cast<qint64>(PatternOperation::Join), context.error))
    return false;
  const auto operation = static_cast<PatternOperation>(
      operationValue.toInt(static_cast<int>(PatternOperation::Join)));
  qint64 sourceFeatureId = 0;
  if (!integerNumber(feature.value("sourceFeatureId"),
                     QStringLiteral("sourceFeatureId"), 1,
                     ProjectFile::kMaximumPersistedId, &sourceFeatureId,
                     context.error))
    return false;
  if (operation == PatternOperation::Join) {
    qint64 sourceBodyId = 0;
    const QJsonValue sourceBodyValue = feature.value("sourceBodyId");
    if ((context.stableSchema || !sourceBodyValue.isUndefined()) &&
        !integerNumber(sourceBodyValue, QStringLiteral("sourceBodyId"), 0,
                       ProjectFile::kMaximumPersistedId, &sourceBodyId,
                       context.error))
      return false;
    if ((sourceBodyId != 0 &&
         static_cast<BodyId>(sourceBodyId) != context.bodyId) ||
        context.featureIndex == 0 ||
        context.bodyFeatures[context.featureIndex - 1]
                .toObject()
                .value("id")
                .toInteger() != sourceFeatureId)
      return invalid(
          context.error,
          QString::fromUtf8(
              "Join-паттерн должен ссылаться на предыдущую фичу текущего тела."));
  } else {
    qint64 sourceBodyId = 0;
    if (!integerNumber(feature.value("sourceBodyId"),
                       QStringLiteral("sourceBodyId"), 1,
                       ProjectFile::kMaximumPersistedId, &sourceBodyId,
                       context.error) ||
        static_cast<BodyId>(sourceBodyId) == context.bodyId ||
        !validateFeatureReference(static_cast<BodyId>(sourceBodyId),
                                  static_cast<FeatureId>(sourceFeatureId),
                                  context.featureSets, context.error))
      return false;
  }
  const char* axisKey = linear ? "direction" : "axis";
  if (!integerNumber(feature.value("count"), QStringLiteral("count"),
                     ProjectFile::kMinimumPatternCount,
                     ProjectFile::kMaximumPatternCount, &count,
                     context.error) ||
      !enumNumber(feature.value(QLatin1String(axisKey)),
                  QString::fromLatin1(axisKey),
                  static_cast<qint64>(PrincipalAxis::Z), context.error))
    return false;
  context.totalRebuildWork += count - 1;
  if (context.totalRebuildWork > ProjectFile::kMaximumDocumentRebuildWork)
    return invalid(
        context.error,
        QString::fromUtf8("паттерны превышают лимит перестроения документа."));
  const char* parameterKey = linear ? "spacingMm" : "angleDeg";
  const auto parameter = feature.value(QLatin1String(parameterKey));
  if (!finiteNumber(parameter, QString::fromLatin1(parameterKey),
                    kMinimumPatternParameter,
                    linear ? kMaximumPatternSpacingMm
                           : kMaximumPatternAngleDeg,
                    context.error))
    return false;
  if (linear ? !validPatternSpacing(parameter.toDouble())
             : !validPatternAngle(parameter.toDouble()))
    return invalid(context.error,
                   QString::fromUtf8("недопустимое число в поле %1.")
                       .arg(QString::fromLatin1(parameterKey)));
  return true;
}

bool validateLinearPattern(const QJsonObject& feature,
                           FeatureValidationContext& context) {
  return validatePattern(feature, context, true);
}

bool validateCircularPattern(const QJsonObject& feature,
                             FeatureValidationContext& context) {
  return validatePattern(feature, context, false);
}

bool validateJoinBodies(const QJsonObject& feature,
                        FeatureValidationContext& context) {
  return context.externalFeature(feature, "firstBodyId", "firstFeatureId") &&
         context.externalFeature(feature, "secondBodyId", "secondFeatureId");
}

bool validateShell(const QJsonObject& feature,
                   FeatureValidationContext& context) {
  if (!context.localFeature(feature, "sourceFeatureId") ||
      !finiteNumber(feature.value("thicknessMm"),
                    QStringLiteral("thicknessMm"),
                    std::numeric_limits<double>::min(), kMaximumModelLengthMm,
                    context.error) ||
      !booleanField(feature, "outside", context.error))
    return false;
  QJsonArray faces;
  if (!arrayField(feature, "removedFaces", &faces, context.error) ||
      faces.isEmpty())
    return invalid(context.error,
                   QString::fromUtf8(
                       "список граней Shell пуст или повреждён."));
  if (!context.accountTopologyReferences(faces.size()))
    return invalid(
        context.error,
        QString::fromUtf8(
            "выбор граней превышает лимит топологических ссылок."));
  for (const auto face : faces)
    if (!validateTopologyReference(face, true, context.featureSets,
                                   context.error))
      return false;
  return true;
}

bool validateDraft(const QJsonObject& feature,
                   FeatureValidationContext& context) {
  if (!context.localFeature(feature, "sourceFeatureId") ||
      !enumNumber(feature.value("neutralPlaneType"),
                  QStringLiteral("neutralPlaneType"), 3, context.error) ||
      !enumNumber(feature.value("pullDirectionType"),
                  QStringLiteral("pullDirectionType"), 5, context.error) ||
      !finiteNumber(feature.value("angleDeg"), QStringLiteral("angleDeg"),
                    -89.99, 89.99, context.error) ||
      !booleanField(feature, "reversed", context.error))
    return false;
  QJsonArray faces;
  if (!arrayField(feature, "draftedFaces", &faces, context.error) ||
      faces.isEmpty())
    return invalid(context.error,
                   QString::fromUtf8(
                       "список граней Draft пуст или повреждён."));
  qsizetype referenceCount = faces.size();
  if (feature.contains("neutralPlaneFace")) ++referenceCount;
  if (feature.contains("rotationEdge")) ++referenceCount;
  if (!context.accountTopologyReferences(referenceCount))
    return invalid(
        context.error,
        QString::fromUtf8(
            "выбор граней Draft превышает лимит топологических ссылок."));
  for (const auto face : faces)
    if (!validateTopologyReference(face, true, context.featureSets,
                                   context.error))
      return false;
  if (feature.value("neutralPlaneType").toInt() ==
      static_cast<int>(NeutralPlaneType::BodyFace)) {
    const auto neutralValue = feature.value("neutralPlaneFace");
    if (!validateTopologyReference(neutralValue, true, context.featureSets,
                                   context.error))
      return false;
    const auto neutral = neutralValue.toObject();
    if (static_cast<BodyId>(neutral.value("bodyId").toInteger()) !=
            context.bodyId ||
        neutral.value("featureId").toInteger() !=
            feature.value("sourceFeatureId").toInteger())
      return invalid(
          context.error,
          QString::fromUtf8(
              "нейтральная грань Draft должна принадлежать исходной фиче текущего тела."));
  }
  if (feature.contains("rotationEdge") &&
      !validateTopologyReference(feature.value("rotationEdge"), false,
                                 context.featureSets, context.error))
    return false;
  const int direction = feature.value("pullDirectionType").toInt();
  if (direction <= 2 &&
      !context.sketchReference(feature, "pullDirectionSketchId"))
    return false;
  if (direction == static_cast<int>(AxisReferenceType::SketchLine) &&
      !context.sketchLineReference(feature, "pullDirectionSketchId",
                                   "pullDirectionLineId",
                                   "pullDirectionLineIndex"))
    return false;
  return true;
}

const FeatureCodecEntry* findFeatureCodec(FeatureKind kind);
const FeatureCodecEntry* findFeatureCodec(QStringView token);

template <class Concrete>
const Concrete* codecFeature(const Feature& feature,
                             const FeatureEncodeContext& context) {
  const auto* concrete = dynamic_cast<const Concrete*>(&feature);
  if (!concrete)
    setError(context.error,
             QString::fromUtf8("Тип фичи не соответствует codec registry."));
  return concrete;
}

bool encodeImportedShape(const Feature& feature, QJsonObject& saved,
                         const FeatureEncodeContext& context) {
  const auto* imported = codecFeature<ImportedShapeFeature>(feature, context);
  if (!imported) return false;
  QString encoded;
  if (!imported->importedShape() ||
      !serializeShape(*imported->importedShape(), &encoded, context.error))
    return false;
  saved["brep"] = encoded;
  return true;
}

bool encodeExtrude(const Feature& feature, QJsonObject& saved,
                   const FeatureEncodeContext& context) {
  const auto* extrude = codecFeature<ExtrudeFeature>(feature, context);
  if (!extrude) return false;
  saved["lengthMm"] = extrude->lengthMm();
  saved["operation"] = static_cast<int>(extrude->operation());
  saved["reversed"] = extrude->reversed();
  if (extrude->isFaceSource()) {
    saved["sourceKind"] = QStringLiteral("face");
    if (const auto face = extrude->faceReference())
      saved["face"] = savedFaceReference(*face);
  } else {
    saved["sourceKind"] = QStringLiteral("sketch");
    saved["sketchId"] = static_cast<qint64>(extrude->profileSketchId());
    if (extrude->profileOverride())
      saved["profileOverride"] =
          savedExtrudeProfileGeometry(*extrude->profileOverride());
  }
  return true;
}

bool encodeRevolve(const Feature& feature, QJsonObject& saved,
                   const FeatureEncodeContext& context) {
  const auto* revolve = codecFeature<RevolveFeature>(feature, context);
  if (!revolve) return false;
  saved["profileSketchId"] = static_cast<qint64>(revolve->profileSketchId());
  saved["axisType"] = static_cast<int>(revolve->axis().type);
  saved["axisSketchId"] = static_cast<qint64>(revolve->axis().sketchId);
  saved["axisLineId"] = static_cast<qint64>(revolve->axis().lineId);
  if (revolve->axis().type == AxisReferenceType::SketchLine) {
    const auto* axisSketch =
        context.document.findSketch(revolve->axis().sketchId);
    const auto lineIndex = axisSketch
                               ? axisSketch->geometry.lineIndex(
                                     revolve->axis().lineId)
                               : std::nullopt;
    if (!lineIndex) {
      setError(context.error,
               QString::fromUtf8(
                   "Ось вращения ссылается на неизвестную линию."));
      return false;
    }
    saved["axisLineIndex"] = static_cast<qint64>(*lineIndex);
    saved["axisLineId"] = static_cast<qint64>(*lineIndex + 1);
  }
  saved["angleDeg"] = revolve->angleDeg();
  saved["operation"] = static_cast<int>(revolve->operation());
  saved["reversed"] = revolve->reversed();
  if (revolve->profileOverride())
    saved["profileOverride"] =
        savedExtrudeProfileGeometry(*revolve->profileOverride());
  return true;
}

bool encodePocket(const Feature& feature, QJsonObject& saved,
                  const FeatureEncodeContext& context) {
  const auto* pocket = codecFeature<PocketFeature>(feature, context);
  if (!pocket) return false;
  saved["sketchId"] = static_cast<qint64>(pocket->profileSketchId());
  saved["depthMm"] = pocket->depthMm();
  return true;
}

template <class EdgeFeature>
bool encodeEdgeTreatment(const Feature& feature, QJsonObject& saved,
                         const FeatureEncodeContext& context,
                         const char* parameterKey, double parameterValue) {
  const auto* treatment = codecFeature<EdgeFeature>(feature, context);
  if (!treatment) return false;
  saved[QLatin1String(parameterKey)] = parameterValue;
  QJsonArray edges;
  for (const auto& edge : treatment->edges())
    edges.append(savedEdgeReference(edge));
  saved["edges"] = edges;
  return true;
}

bool encodeFillet(const Feature& feature, QJsonObject& saved,
                  const FeatureEncodeContext& context) {
  const auto* fillet = codecFeature<FilletFeature>(feature, context);
  if (!fillet) return false;
  return encodeEdgeTreatment<FilletFeature>(feature, saved, context,
                                            "radiusMm", fillet->radiusMm());
}

bool encodeChamfer(const Feature& feature, QJsonObject& saved,
                   const FeatureEncodeContext& context) {
  const auto* chamfer = codecFeature<ChamferFeature>(feature, context);
  if (!chamfer) return false;
  return encodeEdgeTreatment<ChamferFeature>(
      feature, saved, context, "distanceMm", chamfer->distanceMm());
}

bool encodeMirror(const Feature& feature, QJsonObject& saved,
                  const FeatureEncodeContext& context) {
  const auto* mirror = codecFeature<MirrorFeature>(feature, context);
  if (!mirror) return false;
  saved["sourceFeatureId"] = static_cast<qint64>(mirror->sourceFeatureId());
  saved["plane"] = static_cast<int>(mirror->plane());
  return true;
}

bool encodeMove(const Feature& feature, QJsonObject& saved,
                const FeatureEncodeContext& context) {
  const auto* move = codecFeature<MoveFeature>(feature, context);
  if (!move) return false;
  const auto offset = move->offsetMm();
  saved["sourceFeatureId"] = static_cast<qint64>(move->sourceFeatureId());
  saved["offsetXmm"] = offset.x;
  saved["offsetYmm"] = offset.y;
  saved["offsetZmm"] = offset.z;
  return true;
}

bool encodeLinearPattern(const Feature& feature, QJsonObject& saved,
                         const FeatureEncodeContext& context) {
  const auto* pattern = codecFeature<LinearPatternFeature>(feature, context);
  if (!pattern) return false;
  saved["sourceBodyId"] = static_cast<qint64>(pattern->sourceBodyId());
  saved["sourceFeatureId"] = static_cast<qint64>(pattern->sourceFeatureId());
  saved["direction"] = static_cast<int>(pattern->direction());
  saved["count"] = pattern->count();
  saved["spacingMm"] = pattern->spacingMm();
  saved["operation"] = static_cast<int>(pattern->operation());
  return true;
}

bool encodeCircularPattern(const Feature& feature, QJsonObject& saved,
                           const FeatureEncodeContext& context) {
  const auto* pattern = codecFeature<CircularPatternFeature>(feature, context);
  if (!pattern) return false;
  saved["sourceBodyId"] = static_cast<qint64>(pattern->sourceBodyId());
  saved["sourceFeatureId"] = static_cast<qint64>(pattern->sourceFeatureId());
  saved["axis"] = static_cast<int>(pattern->axis());
  saved["count"] = pattern->count();
  saved["angleDeg"] = pattern->angleDeg();
  saved["operation"] = static_cast<int>(pattern->operation());
  return true;
}

bool encodeJoinBodies(const Feature& feature, QJsonObject& saved,
                      const FeatureEncodeContext& context) {
  const auto* joined = codecFeature<JoinBodiesFeature>(feature, context);
  if (!joined) return false;
  saved["firstBodyId"] = static_cast<qint64>(joined->firstBodyId());
  saved["firstFeatureId"] = static_cast<qint64>(joined->firstFeatureId());
  saved["secondBodyId"] = static_cast<qint64>(joined->secondBodyId());
  saved["secondFeatureId"] = static_cast<qint64>(joined->secondFeatureId());
  return true;
}

bool encodeShell(const Feature& feature, QJsonObject& saved,
                 const FeatureEncodeContext& context) {
  const auto* shell = codecFeature<ShellFeature>(feature, context);
  if (!shell) return false;
  saved["sourceFeatureId"] = static_cast<qint64>(shell->sourceFeatureId());
  saved["thicknessMm"] = shell->thicknessMm();
  saved["outside"] = shell->outside();
  QJsonArray faces;
  for (const auto& face : shell->removedFaces())
    faces.append(savedFaceReference(face));
  saved["removedFaces"] = faces;
  return true;
}

bool encodeDraft(const Feature& feature, QJsonObject& saved,
                 const FeatureEncodeContext& context) {
  const auto* draft = codecFeature<DraftFeature>(feature, context);
  if (!draft) return false;
  saved["sourceFeatureId"] = static_cast<qint64>(draft->sourceFeatureId());
  saved["angleDeg"] = draft->angleDeg();
  saved["reversed"] = draft->reversed();
  saved["neutralPlaneType"] = static_cast<int>(draft->neutralPlane().type);
  if (draft->neutralPlane().face)
    saved["neutralPlaneFace"] =
        savedFaceReference(*draft->neutralPlane().face);
  saved["pullDirectionType"] = static_cast<int>(draft->pullDirection().type);
  saved["pullDirectionSketchId"] =
      static_cast<qint64>(draft->pullDirection().sketchId);
  saved["pullDirectionLineId"] =
      static_cast<qint64>(draft->pullDirection().lineId);
  if (draft->pullDirection().type == AxisReferenceType::SketchLine) {
    const auto* directionSketch =
        context.document.findSketch(draft->pullDirection().sketchId);
    const auto lineIndex = directionSketch
                               ? directionSketch->geometry.lineIndex(
                                     draft->pullDirection().lineId)
                               : std::nullopt;
    if (!lineIndex) {
      setError(context.error,
               QString::fromUtf8(
                   "Направление уклона ссылается на неизвестную линию."));
      return false;
    }
    saved["pullDirectionLineIndex"] = static_cast<qint64>(*lineIndex);
    saved["pullDirectionLineId"] = static_cast<qint64>(*lineIndex + 1);
  }
  if (draft->rotationEdge())
    saved["rotationEdge"] = savedEdgeReference(*draft->rotationEdge());
  QJsonArray faces;
  for (const auto& face : draft->draftedFaces())
    faces.append(savedFaceReference(face));
  saved["draftedFaces"] = faces;
  return true;
}

bool decodeImportedShape(const QJsonObject& saved, FeatureId id,
                         const std::string& name, Body& body,
                         const FeatureDecodeContext& context) {
  const auto shape = deserializeShape(saved.value("brep").toString(),
                                      context.error);
  if (!shape) return false;
  body.addFeature(std::make_unique<ImportedShapeFeature>(id, shape, name));
  return true;
}

bool decodeExtrude(const QJsonObject& saved, FeatureId id,
                   const std::string& name, Body& body,
                   const FeatureDecodeContext&) {
  const auto sourceKind = saved.value("sourceKind").toString();
  const auto lengthMm = saved.value("lengthMm").toDouble();
  const auto operation =
      static_cast<ExtrudeOperation>(saved.value("operation").toInt());
  const auto reversed = saved.value("reversed").toBool();
  if (sourceKind == QStringLiteral("face")) {
    body.addFeature(std::make_unique<ExtrudeFeature>(
        id, loadedFaceReference(saved.value("face")), lengthMm, name,
        operation, reversed));
  } else {
    // Missing sourceKind is the pre-face-extrude project format.
    auto feature = std::make_unique<ExtrudeFeature>(
        id, static_cast<SketchId>(saved.value("sketchId").toInteger()),
        lengthMm, name, operation, reversed);
    if (saved.value("profileOverride").isObject())
      feature->setProfileOverride(
          loadedExtrudeProfileGeometry(saved.value("profileOverride")));
    body.addFeature(std::move(feature));
  }
  return true;
}

bool decodeRevolve(const QJsonObject& saved, FeatureId id,
                   const std::string& name, Body& body,
                   const FeatureDecodeContext& context) {
  const auto axisType =
      static_cast<AxisReferenceType>(saved.value("axisType").toInt());
  AxisReference axis{
      axisType,
      static_cast<SketchId>(saved.value("axisSketchId").toInteger()),
      axisType == AxisReferenceType::SketchLine
          ? context.restoredSketchLineId(saved, "axisSketchId", "axisLineId",
                                         "axisLineIndex")
          : static_cast<sketch::GeometryId>(
                saved.value("axisLineId").toInteger())};
  auto feature = std::make_unique<RevolveFeature>(
      id,
      static_cast<SketchId>(saved.value("profileSketchId").toInteger()), axis,
      saved.value("angleDeg").toDouble(360.0), name,
      static_cast<ExtrudeOperation>(saved.value("operation").toInt()),
      saved.value("reversed").toBool());
  if (saved.value("profileOverride").isObject())
    feature->setProfileOverride(
        loadedExtrudeProfileGeometry(saved.value("profileOverride")));
  body.addFeature(std::move(feature));
  return true;
}

bool decodePocket(const QJsonObject& saved, FeatureId id,
                  const std::string& name, Body& body,
                  const FeatureDecodeContext&) {
  body.addFeature(std::make_unique<PocketFeature>(
      id, static_cast<SketchId>(saved.value("sketchId").toInteger()),
      saved.value("depthMm").toDouble(), name));
  return true;
}

std::vector<EdgeReference> loadedEdges(const QJsonArray& values) {
  std::vector<EdgeReference> edges;
  edges.reserve(static_cast<std::size_t>(values.size()));
  for (const auto value : values) edges.push_back(loadedEdgeReference(value));
  return edges;
}

bool decodeFillet(const QJsonObject& saved, FeatureId id,
                  const std::string& name, Body& body,
                  const FeatureDecodeContext&) {
  body.addFeature(std::make_unique<FilletFeature>(
      id, loadedEdges(saved.value("edges").toArray()),
      saved.value("radiusMm").toDouble(), name));
  return true;
}

bool decodeChamfer(const QJsonObject& saved, FeatureId id,
                   const std::string& name, Body& body,
                   const FeatureDecodeContext&) {
  body.addFeature(std::make_unique<ChamferFeature>(
      id, loadedEdges(saved.value("edges").toArray()),
      saved.value("distanceMm").toDouble(), name));
  return true;
}

bool decodeMirror(const QJsonObject& saved, FeatureId id,
                  const std::string& name, Body& body,
                  const FeatureDecodeContext&) {
  body.addFeature(std::make_unique<MirrorFeature>(
      id, static_cast<FeatureId>(saved.value("sourceFeatureId").toInteger()),
      static_cast<MirrorPlane>(saved.value("plane").toInt()), name));
  return true;
}

bool decodeMove(const QJsonObject& saved, FeatureId id,
                const std::string& name, Body& body,
                const FeatureDecodeContext&) {
  body.addFeature(std::make_unique<MoveFeature>(
      id, static_cast<FeatureId>(saved.value("sourceFeatureId").toInteger()),
      Vector3d{saved.value("offsetXmm").toDouble(),
               saved.value("offsetYmm").toDouble(),
               saved.value("offsetZmm").toDouble()},
      name));
  return true;
}

bool decodeLinearPattern(const QJsonObject& saved, FeatureId id,
                         const std::string& name, Body& body,
                         const FeatureDecodeContext&) {
  body.addFeature(std::make_unique<LinearPatternFeature>(
      id, static_cast<BodyId>(saved.value("sourceBodyId").toInteger()),
      static_cast<FeatureId>(saved.value("sourceFeatureId").toInteger()),
      static_cast<PrincipalAxis>(saved.value("direction").toInt()),
      saved.value("count").toInt(), saved.value("spacingMm").toDouble(),
      static_cast<PatternOperation>(saved.value("operation").toInt(
          static_cast<int>(PatternOperation::Join))),
      name));
  return true;
}

bool decodeCircularPattern(const QJsonObject& saved, FeatureId id,
                           const std::string& name, Body& body,
                           const FeatureDecodeContext&) {
  body.addFeature(std::make_unique<CircularPatternFeature>(
      id, static_cast<BodyId>(saved.value("sourceBodyId").toInteger()),
      static_cast<FeatureId>(saved.value("sourceFeatureId").toInteger()),
      static_cast<PrincipalAxis>(saved.value("axis").toInt()),
      saved.value("count").toInt(), saved.value("angleDeg").toDouble(),
      static_cast<PatternOperation>(saved.value("operation").toInt(
          static_cast<int>(PatternOperation::Join))),
      name));
  return true;
}

bool decodeJoinBodies(const QJsonObject& saved, FeatureId id,
                      const std::string& name, Body& body,
                      const FeatureDecodeContext&) {
  body.addFeature(std::make_unique<JoinBodiesFeature>(
      id, static_cast<BodyId>(saved.value("firstBodyId").toInteger()),
      static_cast<FeatureId>(saved.value("firstFeatureId").toInteger()),
      static_cast<BodyId>(saved.value("secondBodyId").toInteger()),
      static_cast<FeatureId>(saved.value("secondFeatureId").toInteger()),
      name));
  return true;
}

bool decodeShell(const QJsonObject& saved, FeatureId id,
                 const std::string& name, Body& body,
                 const FeatureDecodeContext&) {
  std::vector<FaceReference> faces;
  for (const auto value : saved.value("removedFaces").toArray())
    faces.push_back(loadedFaceReference(value));
  body.addFeature(std::make_unique<ShellFeature>(
      id, static_cast<FeatureId>(saved.value("sourceFeatureId").toInteger()),
      std::move(faces), saved.value("thicknessMm").toDouble(2.0),
      saved.value("outside").toBool(), name));
  return true;
}

bool decodeDraft(const QJsonObject& saved, FeatureId id,
                 const std::string& name, Body& body,
                 const FeatureDecodeContext& context) {
  std::vector<FaceReference> faces;
  for (const auto value : saved.value("draftedFaces").toArray())
    faces.push_back(loadedFaceReference(value));
  PlaneReference plane{
      static_cast<NeutralPlaneType>(saved.value("neutralPlaneType").toInt())};
  if (saved.contains("neutralPlaneFace"))
    plane.face = loadedFaceReference(saved.value("neutralPlaneFace"));
  const auto directionType = static_cast<AxisReferenceType>(
      saved.value("pullDirectionType").toInt());
  AxisReference direction{
      directionType,
      static_cast<SketchId>(
          saved.value("pullDirectionSketchId").toInteger()),
      directionType == AxisReferenceType::SketchLine
          ? context.restoredSketchLineId(
                saved, "pullDirectionSketchId", "pullDirectionLineId",
                "pullDirectionLineIndex")
          : static_cast<sketch::GeometryId>(
                saved.value("pullDirectionLineId").toInteger())};
  std::optional<EdgeReference> rotationEdge;
  if (saved.contains("rotationEdge"))
    rotationEdge = loadedEdgeReference(saved.value("rotationEdge"));
  body.addFeature(std::make_unique<DraftFeature>(
      id, static_cast<FeatureId>(saved.value("sourceFeatureId").toInteger()),
      std::move(faces), std::move(plane), direction,
      saved.value("angleDeg").toDouble(5.0),
      saved.value("reversed").toBool(), name, std::move(rotationEdge)));
  return true;
}

const std::array<FeatureCodecEntry, kPersistedFeatureKindCount>&
featureCodecRegistry() {
  static const std::array<FeatureCodecEntry, kPersistedFeatureKindCount>
      registry{{
          {FeatureKind::ImportedShape, "ImportedShape", validateImportedShape,
           encodeImportedShape, decodeImportedShape},
          {FeatureKind::Extrude, "Extrude", validateExtrude, encodeExtrude,
           decodeExtrude},
          {FeatureKind::Revolve, "Revolve", validateRevolve, encodeRevolve,
           decodeRevolve},
          {FeatureKind::Pocket, "Pocket", validatePocket, encodePocket,
           decodePocket},
          {FeatureKind::Fillet, "Fillet", validateFillet, encodeFillet,
           decodeFillet},
          {FeatureKind::Chamfer, "Chamfer", validateChamfer, encodeChamfer,
           decodeChamfer},
          {FeatureKind::Mirror, "Mirror", validateMirror, encodeMirror,
           decodeMirror},
          {FeatureKind::Move, "Move", validateMove, encodeMove, decodeMove},
          {FeatureKind::LinearPattern, "LinearPattern", validateLinearPattern,
           encodeLinearPattern, decodeLinearPattern},
          {FeatureKind::CircularPattern, "CircularPattern",
           validateCircularPattern, encodeCircularPattern,
           decodeCircularPattern},
          {FeatureKind::JoinBodies, "JoinBodies", validateJoinBodies,
           encodeJoinBodies, decodeJoinBodies},
          {FeatureKind::Shell, "Shell", validateShell, encodeShell,
           decodeShell},
          {FeatureKind::Draft, "Draft", validateDraft, encodeDraft,
           decodeDraft},
      }};
  return registry;
}

const FeatureCodecEntry* findFeatureCodec(FeatureKind kind) {
  const auto& registry = featureCodecRegistry();
  const auto found =
      std::find_if(registry.begin(), registry.end(),
                   [kind](const FeatureCodecEntry& entry) {
                     return entry.kind == kind;
                   });
  return found == registry.end() ? nullptr : &*found;
}

const FeatureCodecEntry* findFeatureCodec(QStringView token) {
  const auto& registry = featureCodecRegistry();
  const auto found = std::find_if(
      registry.begin(), registry.end(),
      [token](const FeatureCodecEntry& entry) {
        return token == QString::fromLatin1(entry.token.data(),
                                            static_cast<qsizetype>(
                                                entry.token.size()));
      });
  return found == registry.end() ? nullptr : &*found;
}

bool validateV2Root(const QJsonObject& root, const ProjectData& legacy,
                    QString* error, bool* unsupported = nullptr) {
  const bool stableSchema =
      root.value(QLatin1String(kConstraintTypeEncodingKey)).toString() ==
      QLatin1String(kStableConstraintTypeEncoding);
  QJsonObject model;
  if (!objectField(root, "model", &model, error)) return false;
  QJsonArray sketches, bodies, referenceImages;
  if (!arrayField(model, "sketches", &sketches, error) ||
      !arrayField(model, "bodies", &bodies, error) ||
      !arrayField(model, "referenceImages", &referenceImages, error, false))
    return false;
  if (sketches.size() > ProjectFile::kMaximumDocumentSketches ||
      bodies.size() > ProjectFile::kMaximumDocumentBodies)
    return invalid(
        error,
        QString::fromUtf8("параметрическая модель превышает лимит объектов."));
  if (sketches.size() != static_cast<qsizetype>(legacy.sketches.size()))
    return invalid(error, QString::fromUtf8("число метаданных эскизов не совпадает с геометрией."));

  std::unordered_set<SketchId> sketchIds;
  std::unordered_map<SketchId, const sketch::Sketch*> sketchGeometries;
  std::unordered_map<SketchId, const sketch::Sketch*> stableSketchGeometries;
  if (stableSchema) {
    const auto savedSketches = root.value("sketches").toArray();
    for (qsizetype position = 0; position < savedSketches.size(); ++position) {
      if (!savedSketches[position].isObject())
        return invalid(error,
                       QString::fromUtf8("геометрия эскиза должна быть объектом."));
      const auto savedSketch = savedSketches[position].toObject();
      qint64 savedId = 0;
      if (!integerNumber(savedSketch.value("sketchId"),
                         QStringLiteral("sketchId"), 1,
                         ProjectFile::kMaximumPersistedId, &savedId, error) ||
          !stableSketchGeometries
               .emplace(static_cast<SketchId>(savedId),
                        &legacy.sketches[static_cast<std::size_t>(position)]
                             .geometry)
               .second)
        return invalid(
            error,
            QString::fromUtf8(
                "ID геометрии эскиза отсутствует или повторяется."));
    }
  }
  for (qsizetype sketchPosition = 0; sketchPosition < sketches.size();
       ++sketchPosition) {
    const auto value = sketches[sketchPosition];
    if (!value.isObject()) return invalid(error, QString::fromUtf8("метаданные эскиза должны быть объектом."));
    const auto sketch = value.toObject();
    qint64 id = 0;
    if (!integerNumber(sketch.value("id"), QStringLiteral("sketch.id"), 1,
                       ProjectFile::kMaximumPersistedId, &id, error) ||
        !sketchIds.insert(static_cast<SketchId>(id)).second)
      return invalid(error, QString::fromUtf8("ID эскиза равен нулю или повторяется."));
    if (!boundedString(sketch.value("name"), QStringLiteral("sketch.name"), error) ||
        !vector3Field(sketch.value("origin"), QStringLiteral("origin"), error) ||
        !vector3Field(sketch.value("xDirection"), QStringLiteral("xDirection"), error) ||
        !vector3Field(sketch.value("yDirection"), QStringLiteral("yDirection"), error) ||
        !placementBasisFields(sketch.value("xDirection"),
                              sketch.value("yDirection"), error))
      return false;
    const auto sketchId = static_cast<SketchId>(id);
    if (stableSchema &&
        root.value("sketches")
                .toArray()[sketchPosition]
                .toObject()
                .value("sketchId")
                .toInteger() != id)
      return invalid(
          error,
          QString::fromUtf8(
              "порядок и ID геометрии эскиза не согласованы с метаданными."));
    const sketch::Sketch* geometry =
        &legacy.sketches[static_cast<std::size_t>(sketchPosition)].geometry;
    if (stableSchema) {
      const auto found = stableSketchGeometries.find(sketchId);
      if (found == stableSketchGeometries.end())
        return invalid(
            error,
            QString::fromUtf8(
                "метаданные не имеют связанной геометрии эскиза."));
      geometry = found->second;
    }
    sketchGeometries.emplace(sketchId, geometry);
  }
  if (stableSchema && stableSketchGeometries.size() != sketchIds.size())
    return invalid(error,
                   QString::fromUtf8(
                       "набор ID геометрии и метаданных эскизов различается."));

  if (referenceImages.size() > ProjectFile::kMaximumCollectionItems)
    return invalid(error,
                   QString::fromUtf8("документ содержит слишком много изображений."));
  std::unordered_set<ReferenceImageId> referenceImageIds;
  for (const auto value : referenceImages) {
    if (!value.isObject())
      return invalid(error,
                     QString::fromUtf8("изображение должно быть объектом."));
    const auto image = value.toObject();
    qint64 id = 0, pixelWidth = 0, pixelHeight = 0;
    if (!integerNumber(image.value("id"), QStringLiteral("image.id"), 1,
                       ProjectFile::kMaximumPersistedId, &id, error) ||
        !referenceImageIds.insert(static_cast<ReferenceImageId>(id)).second)
      return invalid(error,
                     QString::fromUtf8("ID изображения равен нулю или повторяется."));
    if (!boundedString(image.value("name"), QStringLiteral("image.name"), error) ||
        !boundedString(image.value("sourcePath"),
                       QStringLiteral("image.sourcePath"), error, false) ||
        !boundedString(image.value("supportName"),
                       QStringLiteral("image.supportName"), error) ||
        !vector3Field(image.value("origin"), QStringLiteral("origin"), error) ||
        !vector3Field(image.value("xDirection"), QStringLiteral("xDirection"), error) ||
        !vector3Field(image.value("yDirection"), QStringLiteral("yDirection"), error) ||
        !placementBasisFields(image.value("xDirection"),
                              image.value("yDirection"), error) ||
        !finiteNumber(image.value("offsetX"), QStringLiteral("offsetX"),
                      -kMaximumCoordinateMagnitude,
                      kMaximumCoordinateMagnitude, error) ||
        !finiteNumber(image.value("offsetY"), QStringLiteral("offsetY"),
                      -kMaximumCoordinateMagnitude,
                      kMaximumCoordinateMagnitude, error) ||
        (image.contains("offsetZ") &&
         !finiteNumber(image.value("offsetZ"), QStringLiteral("offsetZ"),
                       -kMaximumCoordinateMagnitude,
                       kMaximumCoordinateMagnitude, error)) ||
        !finiteNumber(image.value("scale"), QStringLiteral("scale"), 0.0,
                      10000.0, error) || image.value("scale").toDouble() == 0.0 ||
        !integerNumber(image.value("pixelWidth"),
                       QStringLiteral("pixelWidth"), 1, 16384,
                       &pixelWidth, error) ||
        !integerNumber(image.value("pixelHeight"),
                       QStringLiteral("pixelHeight"), 1, 16384,
                       &pixelHeight, error) ||
        !booleanField(image, "visible", error, true) ||
        pixelWidth * pixelHeight > 100000000LL)
      return false;
  }

  FeatureSets featureSets;
  std::unordered_set<BodyId> bodyIds;
  std::unordered_set<FeatureId> globalFeatureIds;
  qsizetype totalFeatureCount = 0;
  qsizetype totalTopologyReferences = 0;
  const auto accountTopologyReferences = [&](qsizetype count) {
    if (count > ProjectFile::kMaximumFeatureTopologyReferences)
      return false;
    totalTopologyReferences += count;
    return totalTopologyReferences <=
           ProjectFile::kMaximumDocumentTopologyReferences;
  };
  for (const auto value : bodies) {
    if (!value.isObject()) return invalid(error, QString::fromUtf8("тело должно быть объектом."));
    const auto body = value.toObject();
    qint64 bodyIdValue = 0;
    if (!integerNumber(body.value("id"), QStringLiteral("body.id"), 1,
                       ProjectFile::kMaximumPersistedId, &bodyIdValue, error) ||
        !bodyIds.insert(static_cast<BodyId>(bodyIdValue)).second)
      return invalid(error, QString::fromUtf8("ID тела равен нулю или повторяется."));
    if (!boundedString(body.value("name"), QStringLiteral("body.name"), error) ||
        !booleanField(body, "visible", error, stableSchema))
      return false;
    QJsonArray features;
    if (!arrayField(body, "features", &features, error)) return false;
    totalFeatureCount += features.size();
    if (totalFeatureCount > ProjectFile::kMaximumDocumentFeatures)
      return invalid(
          error,
          QString::fromUtf8("документ содержит слишком много фич."));
    auto& local = featureSets[static_cast<BodyId>(bodyIdValue)];
    for (const auto featureValue : features) {
      if (!featureValue.isObject()) return invalid(error, QString::fromUtf8("фича должна быть объектом."));
      const auto feature = featureValue.toObject();
      qint64 featureIdValue = 0;
      if (!integerNumber(feature.value("id"), QStringLiteral("feature.id"), 1,
                         ProjectFile::kMaximumPersistedId, &featureIdValue, error) ||
          !globalFeatureIds.insert(static_cast<FeatureId>(featureIdValue)).second)
        return invalid(error, QString::fromUtf8("ID фичи равен нулю или повторяется в документе."));
      local.insert(static_cast<FeatureId>(featureIdValue));
      if (!boundedString(feature.value("name"), QStringLiteral("feature.name"), error) ||
          !boundedString(feature.value("type"), QStringLiteral("feature.type"), error, false))
        return false;
    }
  }

  for (const auto value : sketches) {
    const auto supportValue = value.toObject().value("support");
    if (!supportValue.isObject()) return invalid(error, QString::fromUtf8("опора эскиза должна быть объектом."));
    const auto support = supportValue.toObject();
    qint64 type = 0;
    if (!integerNumber(support.value("type"), QStringLiteral("support.type"), 0,
                       static_cast<int>(SketchSupportType::Face), &type, error))
      return false;
    if (type == static_cast<int>(SketchSupportType::Face)) {
      if (!accountTopologyReferences(1))
        return invalid(
            error,
            QString::fromUtf8(
                "топологические ссылки документа превышают лимит."));
      if (!validateTopologyReference(supportValue, true, featureSets, error))
        return false;
    }
  }

  auto localFeature = [&](BodyId bodyId, const QJsonObject& feature,
                          const char* key) -> bool {
    qint64 id = 0;
    return integerNumber(feature.value(QLatin1String(key)),
                         QString::fromLatin1(key), 1,
                         ProjectFile::kMaximumPersistedId,
                         &id, error) &&
           validateFeatureReference(bodyId, static_cast<FeatureId>(id),
                                    featureSets, error);
  };
  auto sketchReference = [&](const QJsonObject& feature,
                             const char* key) -> bool {
    qint64 id = 0;
    if (!integerNumber(feature.value(QLatin1String(key)),
                       QString::fromLatin1(key), 1,
                       ProjectFile::kMaximumPersistedId,
                       &id, error))
      return false;
    if (!sketchIds.contains(static_cast<SketchId>(id)))
      return invalid(error, QString::fromUtf8("ссылка указывает на неизвестный эскиз."));
    return true;
  };
  const bool stableLineReferences = stableSchema;
  auto sketchLineReference = [&](const QJsonObject& feature,
                                 const char* sketchKey, const char* lineIdKey,
                                 const char* lineIndexKey) -> bool {
    qint64 sketchIdValue = 0;
    qint64 rawLineId = 0;
    if (!integerNumber(feature.value(QLatin1String(sketchKey)),
                       QString::fromLatin1(sketchKey), 1,
                       ProjectFile::kMaximumPersistedId, &sketchIdValue,
                       error) ||
        !integerNumber(feature.value(QLatin1String(lineIdKey)),
                       QString::fromLatin1(lineIdKey), 1,
                       kMaximumExactJsonInteger, &rawLineId, error))
      return false;
    const auto geometry =
        sketchGeometries.find(static_cast<SketchId>(sketchIdValue));
    if (geometry == sketchGeometries.end())
      return invalid(error,
                     QString::fromUtf8("ось ссылается на неизвестный эскиз."));

    if (stableLineReferences) {
      qint64 lineIndex = -1;
      if (!integerNumber(feature.value(QLatin1String(lineIndexKey)),
                         QString::fromLatin1(lineIndexKey), 0,
                         ProjectFile::kMaximumCollectionItems, &lineIndex,
                         error))
        return false;
      if (lineIndex >=
          static_cast<qint64>(geometry->second->lines().size()))
        return invalid(error,
                       QString::fromUtf8("ось ссылается на неизвестную линию."));
      if (rawLineId != lineIndex + 1)
        return invalid(
            error,
            QString::fromUtf8("ID и позиция линии оси не согласованы."));
      return true;
    }

    if (feature.contains(QLatin1String(lineIndexKey)))
      return invalid(
          error,
          QString::fromUtf8("позиция линии задана без маркера стабильной схемы."));
    if (rawLineId != 1 || geometry->second->lines().empty()) {
      if (unsupported) *unsupported = true;
      setError(error,
               QString::fromUtf8(
                   "Невозможно однозначно восстановить историческую ссылку на линию."));
      return false;
    }
    return true;
  };
  auto externalFeature = [&](const QJsonObject& feature, const char* bodyKey,
                             const char* featureKey) -> bool {
    qint64 bodyId = 0, featureId = 0;
    return integerNumber(feature.value(QLatin1String(bodyKey)),
                         QString::fromLatin1(bodyKey), 1,
                         ProjectFile::kMaximumPersistedId,
                         &bodyId, error) &&
           integerNumber(feature.value(QLatin1String(featureKey)),
                         QString::fromLatin1(featureKey), 1,
                         ProjectFile::kMaximumPersistedId,
                         &featureId, error) &&
           validateFeatureReference(static_cast<BodyId>(bodyId),
                                    static_cast<FeatureId>(featureId),
                                    featureSets, error);
  };

  qsizetype totalProfileGeometryItems = 0;
  for (const auto& savedSketch : legacy.sketches)
    totalProfileGeometryItems +=
        static_cast<qsizetype>(savedSketch.geometry.lines().size() +
                               savedSketch.geometry.circles().size() +
                               savedSketch.geometry.arcs().size());
  qint64 totalRebuildWork = 0;
  const auto accountProfileGeometry = [&](const QJsonValue& profileValue) {
    const auto profile = profileValue.toObject();
    totalProfileGeometryItems += profile.value("lines").toArray().size() +
                                 profile.value("circles").toArray().size() +
                                 profile.value("arcs").toArray().size();
    return totalProfileGeometryItems <=
           ProjectFile::kMaximumDocumentSketchGeometryItems;
  };
  for (const auto bodyValue : bodies) {
    const auto body = bodyValue.toObject();
    const BodyId bodyId = static_cast<BodyId>(body.value("id").toInteger());
    const auto bodyFeatures = body.value("features").toArray();
    for (qsizetype featureIndex = 0; featureIndex < bodyFeatures.size();
         ++featureIndex) {
      const auto featureValue = bodyFeatures[featureIndex];
      const auto feature = featureValue.toObject();
      const QString type = feature.value("type").toString();
      if (++totalRebuildWork > ProjectFile::kMaximumDocumentRebuildWork)
        return invalid(
            error,
            QString::fromUtf8(
                "параметрическая история превышает лимит перестроения."));
      const auto* codec = findFeatureCodec(QStringView{type});
      if (!codec)
        return invalid(
            error,
            QString::fromUtf8("неизвестный тип фичи: %1").arg(type));
      FeatureValidationContext context{
          stableSchema,
          bodyId,
          featureIndex,
          bodyFeatures,
          featureSets,
          error,
          totalRebuildWork,
          [&](const QJsonObject& candidate, const char* key) {
            return localFeature(bodyId, candidate, key);
          },
          sketchReference,
          sketchLineReference,
          externalFeature,
          accountTopologyReferences,
          accountProfileGeometry};
      if (!codec->validate(feature, context)) return false;
    }
  }
  return true;
}

}  // namespace

std::vector<FeatureCodecDescriptor> featureCodecDescriptors() {
  std::vector<FeatureCodecDescriptor> descriptors;
  descriptors.reserve(featureCodecRegistry().size());
  for (const auto& entry : featureCodecRegistry())
    descriptors.push_back({entry.kind, entry.token});
  return descriptors;
}

static bool loadLegacyRoot(const QJsonObject& root,
                           ConstraintTypeEncoding constraintEncoding,
                           ProjectData* data, QString* error);
static bool loadDocumentRoot(const QJsonObject& root,
                             const ProjectData& legacy, Document* document,
                             QString* error);

bool ProjectFile::create(const QString& path, QString* error) {
  // New projects must start in the canonical parametric format.  Creating a
  // legacy v1 shell and upgrading it only on the first Save made the create
  // path different from every subsequent load/save cycle.
  return saveDocument(path, Document{}, error);
}

static bool serializedProjectRoot(const QString& path, const ProjectData& data,
                                  QJsonObject* result, QString* error) {
  if (!result) {
    setError(error, QString::fromUtf8("Не задано хранилище для проекта."));
    return false;
  }
  QJsonObject root;
  root["format"] = "solidar-project";
  root["version"] = 1;
  root[kConstraintTypeEncodingKey] = kStableConstraintTypeEncoding;
  root["name"] = QFileInfo(path).completeBaseName();
  root["createdAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  root["document"] = QJsonObject{{"widthMm", data.box.widthMm},
                                  {"heightMm", data.box.depthMm},
                                  {"extrusionMm", data.box.heightMm}};
  QJsonArray sketches;
  for (const auto& saved : data.sketches) {
    QJsonArray lines;
    for (const auto& line : saved.geometry.lines())
      lines.append(QJsonObject{{"x1", line.start.xMm}, {"y1", line.start.yMm},
                               {"x2", line.end.xMm}, {"y2", line.end.yMm},
                               {"elementId", static_cast<qint64>(line.elementId)},
                               {"dashed", line.dashed}});
    QJsonArray circles;
    for (const auto& circle : saved.geometry.circles())
      circles.append(QJsonObject{{"x", circle.center.xMm}, {"y", circle.center.yMm},
                                 {"radius", circle.radiusMm}, {"dashed", circle.dashed}});
    QJsonArray arcs;
    for (const auto& arc : saved.geometry.arcs())
      arcs.append(QJsonObject{{"x", arc.center.xMm},
                              {"y", arc.center.yMm},
                              {"radius", arc.radiusMm},
                              {"startAngle", arc.startAngleRad},
                              {"sweepAngle", arc.sweepAngleRad},
                              {"dashed", arc.dashed}});
    QJsonArray beziers;
    for (const auto& bezier : saved.geometry.beziers())
      beziers.append(QJsonObject{
          {"x0", bezier.points[0].xMm}, {"y0", bezier.points[0].yMm},
          {"x1", bezier.points[1].xMm}, {"y1", bezier.points[1].yMm},
          {"x2", bezier.points[2].xMm}, {"y2", bezier.points[2].yMm},
          {"x3", bezier.points[3].xMm}, {"y3", bezier.points[3].yMm},
          {"dashed", bezier.dashed}});
    QJsonArray dimensions;
    for (const auto& dimension : saved.geometry.dimensions()) {
      const auto failDimension = [&] {
        setError(error,
                 QString::fromUtf8(
                     "Размер эскиза содержит несериализуемую ссылку."));
        return false;
      };
      // Project format v1 stores geometry positions as vector indices.
      // Internally the sketch now uses stable GeometryId values, so resolve
      // them only at the serialization boundary to keep old project files
      // readable without changing the file-format version.
      qint64 geometryIndex = -1;
      qint64 secondGeometryIndex = -1;
      if (dimension.kind == sketch::DimensionKind::LineLength) {
        const auto index = saved.geometry.lineIndex(dimension.geometryId);
        if (!index) return failDimension();
        geometryIndex = static_cast<qint64>(*index);
      } else if (dimension.kind == sketch::DimensionKind::CircleDiameter) {
        const auto index = saved.geometry.circleIndex(dimension.geometryId);
        if (!index) return failDimension();
        geometryIndex = static_cast<qint64>(*index);
      } else if (dimension.kind == sketch::DimensionKind::LineAngle ||
                 dimension.kind == sketch::DimensionKind::LineDistance) {
        const auto firstIndex = saved.geometry.lineIndex(dimension.geometryId);
        const auto secondIndex =
            saved.geometry.lineIndex(dimension.secondPoint.lineId);
        if (!firstIndex || !secondIndex) return failDimension();
        geometryIndex = static_cast<qint64>(*firstIndex);
        secondGeometryIndex = static_cast<qint64>(*secondIndex);
      }

      qint64 firstLine = -1;
      qint64 secondLine = -1;
      qint64 firstCircle = -1;
      qint64 secondCircle = -1;
      qint64 firstArc = -1;
      qint64 secondArc = -1;
      qint64 firstBezier = -1;
      qint64 secondBezier = -1;
      qint64 firstElementCenter = 0;
      qint64 secondElementCenter = 0;
      if (dimension.kind == sketch::DimensionKind::PointDistance ||
          dimension.kind == sketch::DimensionKind::PointDistanceX ||
          dimension.kind == sketch::DimensionKind::PointDistanceY) {
        const auto firstLineIndex =
            saved.geometry.lineIndex(dimension.firstPoint.lineId);
        const auto secondLineIndex =
            saved.geometry.lineIndex(dimension.secondPoint.lineId);
        const auto firstCircleIndex =
            saved.geometry.circleIndex(dimension.firstPoint.circleId);
        const auto secondCircleIndex =
            saved.geometry.circleIndex(dimension.secondPoint.circleId);
        const auto firstArcIndex =
            saved.geometry.arcIndex(dimension.firstPoint.arcId);
        const auto secondArcIndex =
            saved.geometry.arcIndex(dimension.secondPoint.arcId);
        const auto firstBezierIndex =
            saved.geometry.bezierIndex(dimension.firstPoint.bezierId);
        const auto secondBezierIndex =
            saved.geometry.bezierIndex(dimension.secondPoint.bezierId);
        const auto sourceCount = [](const sketch::PointReference& point) {
          return static_cast<int>(point.origin) +
                 static_cast<int>(point.lineId != sketch::kInvalidGeometryId) +
                 static_cast<int>(point.circleId != sketch::kInvalidGeometryId) +
                 static_cast<int>(point.arcId != sketch::kInvalidGeometryId) +
                 static_cast<int>(point.bezierId != sketch::kInvalidGeometryId) +
                 static_cast<int>(point.elementCenterId != 0);
        };
        if (sourceCount(dimension.firstPoint) != 1 ||
            sourceCount(dimension.secondPoint) != 1)
          return failDimension();
        if (dimension.firstPoint.lineId != sketch::kInvalidGeometryId &&
            !firstLineIndex)
          return failDimension();
        if (dimension.secondPoint.lineId != sketch::kInvalidGeometryId &&
            !secondLineIndex)
          return failDimension();
        if (dimension.firstPoint.circleId != sketch::kInvalidGeometryId &&
            !firstCircleIndex)
          return failDimension();
        if (dimension.secondPoint.circleId != sketch::kInvalidGeometryId &&
            !secondCircleIndex)
          return failDimension();
        if (dimension.firstPoint.arcId != sketch::kInvalidGeometryId &&
            !firstArcIndex)
          return failDimension();
        if (dimension.secondPoint.arcId != sketch::kInvalidGeometryId &&
            !secondArcIndex)
          return failDimension();
        if (dimension.firstPoint.bezierId != sketch::kInvalidGeometryId &&
            !firstBezierIndex)
          return failDimension();
        if (dimension.secondPoint.bezierId != sketch::kInvalidGeometryId &&
            !secondBezierIndex)
          return failDimension();
        if (firstLineIndex) firstLine = static_cast<qint64>(*firstLineIndex);
        if (secondLineIndex) secondLine = static_cast<qint64>(*secondLineIndex);
        if (firstCircleIndex)
          firstCircle = static_cast<qint64>(*firstCircleIndex);
        if (secondCircleIndex)
          secondCircle = static_cast<qint64>(*secondCircleIndex);
        if (firstArcIndex) firstArc = static_cast<qint64>(*firstArcIndex);
        if (secondArcIndex) secondArc = static_cast<qint64>(*secondArcIndex);
        if (firstBezierIndex)
          firstBezier = static_cast<qint64>(*firstBezierIndex);
        if (secondBezierIndex)
          secondBezier = static_cast<qint64>(*secondBezierIndex);
        firstElementCenter =
            static_cast<qint64>(dimension.firstPoint.elementCenterId);
        secondElementCenter =
            static_cast<qint64>(dimension.secondPoint.elementCenterId);
      }

      dimensions.append(QJsonObject{
          {"id", static_cast<qint64>(dimension.id)},
          {"kind", static_cast<int>(dimension.kind)},
          {"geometryIndex", geometryIndex},
          {"secondGeometryIndex", secondGeometryIndex},
          {"firstLine", firstLine},
          {"firstCircle", firstCircle},
          {"firstArc", firstArc},
          {"firstBezier", firstBezier},
          {"firstBezierPoint", dimension.firstPoint.bezierPoint},
          {"firstElementCenter", firstElementCenter},
          {"firstStart", dimension.firstPoint.start},
          {"firstOrigin", dimension.firstPoint.origin},
          {"secondLine", secondLine},
          {"secondCircle", secondCircle},
          {"secondArc", secondArc},
          {"secondBezier", secondBezier},
          {"secondBezierPoint", dimension.secondPoint.bezierPoint},
          {"secondElementCenter", secondElementCenter},
          {"secondStart", dimension.secondPoint.start},
          {"secondOrigin", dimension.secondPoint.origin},
          {"value", dimension.valueMm},
          {"offset", dimension.offsetMm},
          {"angle", dimension.angleRad}});
    }
    QJsonArray constraints;
    for (const auto& constraint : saved.geometry.constraints()) {
      qint64 firstGeometry = -1;
      qint64 secondGeometry = -1;
      qint64 firstPointLine = -1;
      qint64 secondPointLine = -1;
      qint64 firstPointCircle = -1;
      qint64 secondPointCircle = -1;
      qint64 firstPointArc = -1;
      qint64 secondPointArc = -1;
      qint64 firstPointBezier = -1;
      qint64 secondPointBezier = -1;

      auto linePosition = [&saved](sketch::GeometryId id) -> qint64 {
        const auto index = saved.geometry.lineIndex(id);
        return index ? static_cast<qint64>(*index) : -1;
      };
      auto circlePosition = [&saved](sketch::GeometryId id) -> qint64 {
        const auto index = saved.geometry.circleIndex(id);
        return index ? static_cast<qint64>(*index) : -1;
      };
      auto arcPosition = [&saved](sketch::GeometryId id) -> qint64 {
        const auto index = saved.geometry.arcIndex(id);
        return index ? static_cast<qint64>(*index) : -1;
      };
      auto bezierPosition = [&saved](sketch::GeometryId id) -> qint64 {
        const auto index = saved.geometry.bezierIndex(id);
        return index ? static_cast<qint64>(*index) : -1;
      };

      QString firstKind;
      QString secondKind;

      if (constraint.firstGeometry != sketch::kInvalidGeometryId) {
        firstGeometry = linePosition(constraint.firstGeometry);
        if (firstGeometry >= 0) {
          firstKind = QStringLiteral("line");
        } else {
          firstGeometry = circlePosition(constraint.firstGeometry);
          if (firstGeometry >= 0) {
            firstKind = QStringLiteral("circle");
          } else {
            firstGeometry = arcPosition(constraint.firstGeometry);
            if (firstGeometry >= 0) {
              firstKind = QStringLiteral("arc");
            } else {
              firstGeometry = bezierPosition(constraint.firstGeometry);
              if (firstGeometry >= 0) firstKind = QStringLiteral("bezier");
            }
          }
        }
      }
      if (constraint.secondGeometry != sketch::kInvalidGeometryId) {
        secondGeometry = linePosition(constraint.secondGeometry);
        if (secondGeometry >= 0) {
          secondKind = QStringLiteral("line");
        } else {
          secondGeometry = circlePosition(constraint.secondGeometry);
          if (secondGeometry >= 0) {
            secondKind = QStringLiteral("circle");
          } else {
            secondGeometry = arcPosition(constraint.secondGeometry);
            if (secondGeometry >= 0) {
              secondKind = QStringLiteral("arc");
            } else {
              secondGeometry = bezierPosition(constraint.secondGeometry);
              if (secondGeometry >= 0) secondKind = QStringLiteral("bezier");
            }
          }
        }
      }

      if (constraint.firstPoint.lineId != sketch::kInvalidGeometryId)
        firstPointLine = linePosition(constraint.firstPoint.lineId);
      if (constraint.secondPoint.lineId != sketch::kInvalidGeometryId)
        secondPointLine = linePosition(constraint.secondPoint.lineId);
      if (constraint.firstPoint.circleId != sketch::kInvalidGeometryId)
        firstPointCircle = circlePosition(constraint.firstPoint.circleId);
      if (constraint.secondPoint.circleId != sketch::kInvalidGeometryId)
        secondPointCircle = circlePosition(constraint.secondPoint.circleId);
      if (constraint.firstPoint.arcId != sketch::kInvalidGeometryId)
        firstPointArc = arcPosition(constraint.firstPoint.arcId);
      if (constraint.secondPoint.arcId != sketch::kInvalidGeometryId)
        secondPointArc = arcPosition(constraint.secondPoint.arcId);
      if (constraint.firstPoint.bezierId != sketch::kInvalidGeometryId)
        firstPointBezier = bezierPosition(constraint.firstPoint.bezierId);
      if (constraint.secondPoint.bezierId != sketch::kInvalidGeometryId)
        secondPointBezier = bezierPosition(constraint.secondPoint.bezierId);

      constraints.append(QJsonObject{
          {"id", static_cast<qint64>(constraint.id)},
          {"type", encodedConstraintType(constraint.type)},
          {"typeKey", encodedConstraintTypeKey(constraint.type)},
          {"firstGeometryKind", firstKind},
          {"firstGeometry", firstGeometry},
          {"secondGeometryKind", secondKind},
          {"secondGeometry", secondGeometry},
          {"firstPointLine", firstPointLine},
          {"firstPointStart", constraint.firstPoint.start},
          {"firstPointCircle", firstPointCircle},
          {"firstPointArc", firstPointArc},
          {"firstPointBezier", firstPointBezier},
          {"firstPointBezierPoint", constraint.firstPoint.bezierPoint},
          {"firstPointElementCenter",
           static_cast<qint64>(constraint.firstPoint.elementCenterId)},
          {"firstPointOrigin", constraint.firstPoint.origin},
          {"secondPointLine", secondPointLine},
          {"secondPointStart", constraint.secondPoint.start},
          {"secondPointCircle", secondPointCircle},
          {"secondPointArc", secondPointArc},
          {"secondPointBezier", secondPointBezier},
          {"secondPointBezierPoint", constraint.secondPoint.bezierPoint},
          {"secondPointElementCenter",
           static_cast<qint64>(constraint.secondPoint.elementCenterId)},
          {"secondPointOrigin", constraint.secondPoint.origin},
          {"value", constraint.value}});
    }

    QJsonArray centerNodeElementIds;
    for (const auto elementId :
         saved.geometry.centerNodeElementIds()) {
      centerNodeElementIds.append(
          static_cast<qint64>(elementId));
    }

    sketches.append(QJsonObject{{"support", saved.support},
                                {"lines", lines},
                                {"circles", circles},
                                {"arcs", arcs},
                                {"beziers", beziers},
                                {"dimensions", dimensions},
                                {"constraints", constraints},
                                {"centerNodeElementIds",
                                 centerNodeElementIds}});
  }
  root["sketches"] = sketches;
  QJsonObject extrusion{{"enabled", data.hasExtrusion}};
  if (data.extrusionSourceSketch)
    extrusion["sourceSketch"] = static_cast<qint64>(*data.extrusionSourceSketch);
  root["extrusion"] = extrusion;
  bool unsupported = false;
  if (!validateLegacyRoot(root, false, LegacySketchSchema::StableNameV1,
                          ConstraintTypeEncoding::StableNameV1, nullptr,
                          &unsupported, error))
    return false;
  ProjectData restored;
  if (!loadLegacyRoot(root, ConstraintTypeEncoding::StableNameV1, &restored,
                      error))
    return false;
  *result = std::move(root);
  return true;
}

bool ProjectFile::save(const QString& path, const ProjectData& data,
                       QString* error) {
  const auto saveOperation = [&]() -> bool {
    QJsonObject root;
    if (!serializedProjectRoot(path, data, &root, error))
      return false;
    const QByteArray payload =
        QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (payload.size() > kMaximumFileBytes) {
      setError(error,
               QString::fromUtf8("Проект превышает допустимый размер файла."));
      return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
      setError(error, file.errorString());
      return false;
    }
    if (file.write(payload) != payload.size()) {
      setError(error, file.errorString());
      file.cancelWriting();
      return false;
    }
    if (!file.commit()) {
      setError(error, file.errorString());
      return false;
    }
    return true;
  };
  bool saved = false;
  GeometryFailure failure;
  if (!runGeometryOperation([&] { saved = saveOperation(); }, &failure)) {
    setError(error, geometryFailureMessage(failure.kind, "сохранении проекта"));
    return false;
  }
  return saved;
}

bool ProjectFile::validate(const QString& path, QString* error) {
  auto staged = stageLoad(path);
  if (!staged.succeeded()) {
    setError(error, staged.error);
    return false;
  }
  return true;
}

bool ProjectFile::saveDocument(const QString& path, const Document& document,
                               QString* error) {
  const auto saveOperation = [&]() -> bool {
    ProjectData legacy;
    legacy.box = document.box();
    for (const auto& item : document.sketches())
      legacy.sketches.push_back({item.geometry, QStringLiteral("XY")});
    QJsonObject root;
    if (!serializedProjectRoot(path, legacy, &root, error))
      return false;
    root["version"] = 2;
    auto persistedSketches = root.value("sketches").toArray();
    for (qsizetype position = 0; position < persistedSketches.size();
         ++position) {
      auto persistedSketch = persistedSketches[position].toObject();
      persistedSketch["sketchId"] = static_cast<qint64>(
          document.sketches()[static_cast<std::size_t>(position)].id);
      persistedSketches[position] = persistedSketch;
    }
    root["sketches"] = persistedSketches;

    QJsonArray sketches;
    for (const auto& item : document.sketches()) {
      QJsonObject support{{"type", static_cast<int>(item.support.type)}};
      if (item.support.type == SketchSupportType::Face) {
        support["bodyId"] = static_cast<qint64>(item.support.face.bodyId);
        support["featureId"] = static_cast<qint64>(item.support.face.featureId);
        support["faceIndex"] = static_cast<qint64>(item.support.face.faceIndex);
        if (!item.support.face.persistentTag.empty())
          support["persistentTag"] =
              QString::fromStdString(item.support.face.persistentTag);
        if (item.support.face.signature)
          support["signature"] = faceSignature(*item.support.face.signature);
      }
      sketches.append(QJsonObject{
          {"id", static_cast<qint64>(item.id)},
          {"name", QString::fromStdString(item.name)},
          {"origin", vector3(item.placement.origin.x, item.placement.origin.y,
                             item.placement.origin.z)},
          {"xDirection",
           vector3(item.placement.xDirection.x, item.placement.xDirection.y,
                   item.placement.xDirection.z)},
          {"yDirection",
           vector3(item.placement.yDirection.x, item.placement.yDirection.y,
                   item.placement.yDirection.z)},
          {"support", support}});
    }

    QJsonArray bodies;
    for (const auto& body : document.bodies()) {
      QJsonArray features;
      for (const auto& feature : body.features()) {
        const auto* codec = findFeatureCodec(feature->kind());
        if (!codec) {
          setError(error, QString::fromUtf8("Неподдерживаемый FeatureKind."));
          return false;
        }
        QJsonObject saved{
            {"id", static_cast<qint64>(feature->id())},
            {"name", QString::fromStdString(feature->name())},
            {"type", QString::fromLatin1(
                         codec->token.data(),
                         static_cast<qsizetype>(codec->token.size()))}};
        if (!codec->encode(*feature, saved, FeatureEncodeContext{document, error}))
          return false;
        features.append(saved);
      }
      bodies.append(QJsonObject{{"id", static_cast<qint64>(body.id())},
                                {"name", QString::fromStdString(body.name())},
                                {"visible", body.visible()},
                                {"features", features}});
    }
    QJsonArray referenceImages;
    for (const auto& image : document.referenceImages()) {
      referenceImages.append(QJsonObject{
          {"id", static_cast<qint64>(image.id)},
          {"name", QString::fromStdString(image.name)},
          {"sourcePath", QString::fromStdString(image.sourcePath)},
          {"supportName", QString::fromStdString(image.supportName)},
          {"origin", vector3(image.placement.origin.x, image.placement.origin.y,
                              image.placement.origin.z)},
          {"xDirection",
           vector3(image.placement.xDirection.x,
                   image.placement.xDirection.y,
                   image.placement.xDirection.z)},
          {"yDirection",
           vector3(image.placement.yDirection.x,
                   image.placement.yDirection.y,
                   image.placement.yDirection.z)},
          {"offsetX", image.offsetXMm},
          {"offsetY", image.offsetYMm},
          {"offsetZ", image.offsetZMm},
          {"scale", image.scale},
          {"pixelWidth", image.pixelWidth},
          {"pixelHeight", image.pixelHeight},
          {"visible", image.visible}});
    }
    root["model"] = QJsonObject{{"sketches", sketches},
                                 {"bodies", bodies},
                                 {"referenceImages", referenceImages}};

    if (!validateV2Root(root, legacy, error))
      return false;
    ProjectData restoredLegacy;
    if (!loadLegacyRoot(root, ConstraintTypeEncoding::StableNameV1,
                        &restoredLegacy, error))
      return false;
    Document restoredDocument;
    {
      ::solidar::detail::ScopedExplicitIdReservationPause reservationPause;
      if (!loadDocumentRoot(root, restoredLegacy, &restoredDocument, error))
        return false;
    }

    const QByteArray payload =
        QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (payload.size() > kMaximumFileBytes) {
      setError(error,
               QString::fromUtf8("Проект превышает допустимый размер файла."));
      return false;
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
      setError(error, output.errorString());
      return false;
    }
    if (output.write(payload) != payload.size()) {
      setError(error, output.errorString());
      output.cancelWriting();
      return false;
    }
    if (!output.commit()) {
      setError(error, output.errorString());
      return false;
    }
    return true;
  };
  bool saved = false;
  GeometryFailure failure;
  if (!runGeometryOperation([&] { saved = saveOperation(); }, &failure)) {
    setError(error, geometryFailureMessage(failure.kind, "сохранении проекта"));
    return false;
  }
  return saved;
}

static bool loadDocumentRoot(const QJsonObject& root,
                             const ProjectData& legacy, Document* document,
                             QString* error) {
  if (!document) {
    setError(error, QString::fromUtf8("Не задан документ для загрузки."));
    return false;
  }

  Document loaded;
  loaded.setBox(legacy.box);
  const auto model = root.value("model").toObject();
  const auto sketchMetadata = model.value("sketches").toArray();
  if (sketchMetadata.size() != static_cast<qsizetype>(legacy.sketches.size())) {
    setError(error, QString::fromUtf8("Метаданные эскизов повреждены."));
    return false;
  }
  const bool stableSchema =
      root.value(QLatin1String(kConstraintTypeEncodingKey)).toString() ==
      QLatin1String(kStableConstraintTypeEncoding);
  std::unordered_map<SketchId, const sketch::Sketch*> stableSketchGeometries;
  if (stableSchema) {
    const auto savedSketches = root.value("sketches").toArray();
    for (qsizetype position = 0; position < savedSketches.size(); ++position)
      stableSketchGeometries.emplace(
          static_cast<SketchId>(savedSketches[position]
                                    .toObject()
                                    .value("sketchId")
                                    .toInteger()),
          &legacy.sketches[static_cast<std::size_t>(position)].geometry);
  }
  for (qsizetype index = 0; index < sketchMetadata.size(); ++index) {
    const auto saved = sketchMetadata[index].toObject();
    const auto savedId = static_cast<SketchId>(saved.value("id").toInteger());
    const sketch::Sketch* geometry =
        &legacy.sketches[static_cast<std::size_t>(index)].geometry;
    if (stableSchema) {
      const auto found = stableSketchGeometries.find(savedId);
      if (found == stableSketchGeometries.end()) {
        setError(error,
                 QString::fromUtf8(
                     "Не найдена геометрия эскиза с ID %1.")
                     .arg(static_cast<qulonglong>(savedId)));
        return false;
      }
      geometry = found->second;
    }
    auto& sketch = loaded.addSketch(
        savedId,
        saved.value("name").toString().toStdString(),
        *geometry);
    sketch.placement.origin = readPoint3(saved.value("origin"), {});
    sketch.placement.xDirection =
        readVector3(saved.value("xDirection"), {1.0, 0.0, 0.0});
    sketch.placement.yDirection =
        readVector3(saved.value("yDirection"), {0.0, 1.0, 0.0});
    const auto support = saved.value("support").toObject();
    sketch.support.type = static_cast<SketchSupportType>(
        support.value("type").toInt(static_cast<int>(SketchSupportType::BasePlane)));
    if (sketch.support.type == SketchSupportType::Face) {
      sketch.support.face = {
          static_cast<BodyId>(support.value("bodyId").toInteger()),
          static_cast<FeatureId>(support.value("featureId").toInteger()),
          static_cast<std::size_t>(support.value("faceIndex").toInteger())};
      sketch.support.face.persistentTag =
          support.value("persistentTag").toString().toStdString();
      sketch.support.face.signature = readFaceSignature(support.value("signature"));
    }
  }

  for (const auto imageValue : model.value("referenceImages").toArray()) {
    if (!imageValue.isObject()) {
      setError(error, QString::fromUtf8("Метаданные изображения повреждены."));
      return false;
    }
    const auto saved = imageValue.toObject();
    ReferenceImage image;
    image.id = static_cast<ReferenceImageId>(saved.value("id").toInteger());
    image.name = saved.value("name").toString().toStdString();
    image.sourcePath = saved.value("sourcePath").toString().toStdString();
    image.supportName = saved.value("supportName").toString().toStdString();
    image.placement.origin = readPoint3(saved.value("origin"), {});
    image.placement.xDirection =
        readVector3(saved.value("xDirection"), {1.0, 0.0, 0.0});
    image.placement.yDirection =
        readVector3(saved.value("yDirection"), {0.0, 1.0, 0.0});
    image.offsetXMm = saved.value("offsetX").toDouble();
    image.offsetYMm = saved.value("offsetY").toDouble();
    image.offsetZMm = saved.value("offsetZ").toDouble();
    image.scale = saved.value("scale").toDouble(1.0);
    image.pixelWidth = saved.value("pixelWidth").toInt();
    image.pixelHeight = saved.value("pixelHeight").toInt();
    image.visible = saved.value("visible").toBool(true);
    const auto finiteVector = [](const auto& value) {
      return std::isfinite(value.x) && std::isfinite(value.y) &&
             std::isfinite(value.z);
    };
    if (image.id == kInvalidReferenceImageId || image.sourcePath.empty() ||
        image.pixelWidth <= 0 || image.pixelHeight <= 0 ||
        image.pixelWidth > 16384 || image.pixelHeight > 16384 ||
        static_cast<std::int64_t>(image.pixelWidth) * image.pixelHeight >
            100000000LL ||
        !std::isfinite(image.offsetXMm) ||
        !std::isfinite(image.offsetYMm) || !std::isfinite(image.offsetZMm) ||
        !std::isfinite(image.scale) ||
        image.scale <= 0.0 || image.scale > 10000.0 ||
        !finiteVector(image.placement.origin) ||
        !finiteVector(image.placement.xDirection) ||
        !finiteVector(image.placement.yDirection)) {
      setError(error, QString::fromUtf8("Параметры изображения повреждены."));
      return false;
    }
    try {
      loaded.addReferenceImage(std::move(image));
    } catch (const std::exception&) {
      setError(error, QString::fromUtf8("Метаданные изображения повреждены."));
      return false;
    }
  }

  const auto restoredSketchLineId = [&loaded](
                                        const QJsonObject& saved,
                                        const char* sketchKey,
                                        const char* lineIdKey,
                                        const char* lineIndexKey) {
    const auto* referencedSketch = loaded.findSketch(
        static_cast<SketchId>(saved.value(QLatin1String(sketchKey)).toInteger()));
    if (!referencedSketch) return sketch::kInvalidGeometryId;
    if (saved.contains(QLatin1String(lineIndexKey))) {
      return referencedSketch->geometry.lineId(static_cast<std::size_t>(
          saved.value(QLatin1String(lineIndexKey)).toInteger()));
    }
    if (saved.value(QLatin1String(lineIdKey)).toInteger() == 1)
      return referencedSketch->geometry.lineId(0);
    return sketch::kInvalidGeometryId;
  };

  for (const auto bodyValue : model.value("bodies").toArray()) {
    const auto savedBody = bodyValue.toObject();
    auto& body = loaded.addBody(
        static_cast<BodyId>(savedBody.value("id").toInteger()),
        savedBody.value("name").toString().toStdString());
    body.setVisible(savedBody.value("visible").toBool(true));
    for (const auto featureValue : savedBody.value("features").toArray()) {
      const auto saved = featureValue.toObject();
      const auto id = static_cast<FeatureId>(saved.value("id").toInteger());
      const auto name = saved.value("name").toString().toStdString();
      const auto type = saved.value("type").toString();
      const auto* codec = findFeatureCodec(QStringView{type});
      if (!codec) {
        setError(error, QString::fromUtf8("Неизвестный тип фичи: ") + type);
        return false;
      }
      FeatureDecodeContext context{loaded, error, restoredSketchLineId};
      if (!codec->decode(saved, id, name, body, context)) return false;
    }
  }
  if (!loaded.recompute()) {
    setError(error, QString::fromUtf8("Не удалось перестроить проект: ") +
                        QString::fromStdString(loaded.rebuildError()));
    return false;
  }
  for (const auto& sketch : loaded.sketches()) {
    if (sketch.support.type == SketchSupportType::Face &&
        !sketch.supportResolved) {
      setError(error,
               QString::fromUtf8(
                   "Не удалось разрешить опору эскиза на грани: %1.")
                   .arg(QString::fromStdString(sketch.name)));
      return false;
    }
  }
  *document = std::move(loaded);
  return true;
}

static bool validateMaterializedSketchGeometry(const sketch::Sketch& geometry,
                                               QString* error) {
  const auto finitePoint = [](const sketch::Point& point) {
    return std::isfinite(point.xMm) && std::isfinite(point.yMm) &&
           std::abs(point.xMm) <= kMaximumCoordinateMagnitude &&
           std::abs(point.yMm) <= kMaximumCoordinateMagnitude;
  };
  for (const auto& line : geometry.lines()) {
    if (!finitePoint(line.start) || !finitePoint(line.end) ||
        std::hypot(line.end.xMm - line.start.xMm,
                   line.end.yMm - line.start.yMm) <=
            kMinimumGeometryLengthMm)
      return invalid(
          error,
          QString::fromUtf8(
              "Решатель создал вырожденную или некорректную линию."));
  }
  for (const auto& circle : geometry.circles()) {
    if (!finitePoint(circle.center) || !std::isfinite(circle.radiusMm) ||
        circle.radiusMm <= 0.0 || circle.radiusMm > kMaximumModelLengthMm)
      return invalid(
          error,
          QString::fromUtf8(
              "Решатель создал некорректную окружность."));
  }
  for (const auto& arc : geometry.arcs()) {
    if (!finitePoint(arc.center) || !std::isfinite(arc.radiusMm) ||
        arc.radiusMm <= 0.0 || arc.radiusMm > kMaximumModelLengthMm ||
        !std::isfinite(arc.startAngleRad) ||
        !std::isfinite(arc.sweepAngleRad) ||
        arc.sweepAngleRad <= kMinimumGeometryLengthMm ||
        arc.sweepAngleRad >= kTwoPi - kMinimumGeometryLengthMm)
      return invalid(error,
                     QString::fromUtf8(
                         "Решатель создал некорректную дугу."));
  }
  for (const auto elementId : geometry.centerNodeElementIds()) {
    if (!geometry.elementCenterPoint(elementId))
      return invalid(
          error,
          QString::fromUtf8(
              "Не удалось восстановить центр составного элемента."));
  }
  return true;
}

static bool loadLegacyRoot(const QJsonObject& root,
                           ConstraintTypeEncoding constraintEncoding,
                           ProjectData* data, QString* error) {
  if (!data) {
    setError(error, QString::fromUtf8("Не задано хранилище для данных проекта."));
    return false;
  }
  ProjectData loaded;
  const auto documentObject = root.value("document").toObject();
  loaded.box = {documentObject.value("widthMm").toDouble(60.0),
                documentObject.value("heightMm").toDouble(40.0),
                documentObject.value("extrusionMm").toDouble(25.0)};
  if (loaded.box.widthMm <= 0 || loaded.box.depthMm <= 0 || loaded.box.heightMm <= 0) {
    setError(error, QString::fromUtf8("Файл проекта содержит неверные размеры."));
    return false;
  }
  for (const auto value : root.value("sketches").toArray()) {
    const auto savedObject = value.toObject();
    SavedSketch saved;
    saved.support = savedObject.value("support").toString(QStringLiteral("XY"));
    for (const auto lineValue : savedObject.value("lines").toArray()) {
      const auto line = lineValue.toObject();
      const sketch::Point start{line.value("x1").toDouble(),
                                line.value("y1").toDouble()};
      const sketch::Point end{line.value("x2").toDouble(),
                              line.value("y2").toDouble()};

      if (line.contains("elementId")) {
        const auto elementId =
            static_cast<std::size_t>(line.value("elementId").toInteger());
        saved.geometry.addLine(start, end, elementId);
      } else {
        // Backward compatibility for early project-v1 files.
        saved.geometry.addLine(start, end);
      }

      if (line.value("dashed").toBool() && !saved.geometry.lines().empty())
        saved.geometry.setLineDashedById(
            saved.geometry.lineId(saved.geometry.lines().size() - 1), true);
    }
    for (const auto circleValue : savedObject.value("circles").toArray()) {
      const auto circle = circleValue.toObject();
      saved.geometry.addCircle({circle.value("x").toDouble(), circle.value("y").toDouble()},
                               circle.value("radius").toDouble());
      if (circle.value("dashed").toBool())
        saved.geometry.setCircleDashed(saved.geometry.circles().size() - 1, true);
    }
    for (const auto arcValue : savedObject.value("arcs").toArray()) {
      const auto arc = arcValue.toObject();
      saved.geometry.addArc({arc.value("x").toDouble(), arc.value("y").toDouble()},
                            arc.value("radius").toDouble(),
                            arc.value("startAngle").toDouble(),
                            arc.value("sweepAngle").toDouble(),
                            arc.value("dashed").toBool());
    }
    for (const auto bezierValue : savedObject.value("beziers").toArray()) {
      const auto bezier = bezierValue.toObject();
      saved.geometry.addBezier(
          {bezier.value("x0").toDouble(), bezier.value("y0").toDouble()},
          {bezier.value("x1").toDouble(), bezier.value("y1").toDouble()},
          {bezier.value("x2").toDouble(), bezier.value("y2").toDouble()},
          {bezier.value("x3").toDouble(), bezier.value("y3").toDouble()},
          bezier.value("dashed").toBool());
    }
    // CRASH-FREE 05: RESTORE VIRTUAL CENTER OWNERSHIP
    //
    // elementId grouping is restored with line geometry. Re-register only
    // the composite elements that originally exposed a virtual CAD centre.
    // Old files simply have no centerNodeElementIds array.
    for (const auto centerValue :
         savedObject.value("centerNodeElementIds").toArray()) {
      const auto elementId =
          static_cast<std::size_t>(
              centerValue.toInteger());

      if (elementId != 0)
        saved.geometry.markElementCenterNode(elementId);
    }
    const auto serializedDimensions = savedObject.value("dimensions").toArray();
    sketch::DimensionId nextMigratedDimensionId = 1;
    for (const auto dimensionValue : serializedDimensions) {
      const auto persistedId = dimensionValue.toObject().value("id").toInteger(0);
      if (persistedId > 0)
        nextMigratedDimensionId = std::max(
            nextMigratedDimensionId,
            static_cast<sketch::DimensionId>(persistedId) + 1);
    }
    for (const auto dimensionValue : serializedDimensions) {
      const auto object = dimensionValue.toObject();
      const auto kind =
          static_cast<sketch::DimensionKind>(object.value("kind").toInt());

      sketch::Dimension dimension;
      dimension.id = static_cast<sketch::DimensionId>(
          object.value("id").toInteger(0));
      if (dimension.id == sketch::kInvalidDimensionId)
        dimension.id = nextMigratedDimensionId++;
      dimension.kind = kind;
      dimension.valueMm = object.value("value").toDouble();
      dimension.offsetMm = object.value("offset").toDouble(4.0);
      dimension.angleRad = object.value("angle").toDouble();

      if (kind == sketch::DimensionKind::LineLength) {
        const auto index =
            static_cast<std::size_t>(object.value("geometryIndex").toInteger());
        dimension.geometryId = saved.geometry.lineId(index);
        if (dimension.geometryId == sketch::kInvalidGeometryId) continue;
      } else if (kind == sketch::DimensionKind::CircleDiameter) {
        const auto index =
            static_cast<std::size_t>(object.value("geometryIndex").toInteger());
        dimension.geometryId = saved.geometry.circleId(index);
        if (dimension.geometryId == sketch::kInvalidGeometryId) continue;
      } else if (kind == sketch::DimensionKind::LineAngle ||
                 kind == sketch::DimensionKind::LineDistance) {
        const auto firstIndex =
            static_cast<std::size_t>(object.value("geometryIndex").toInteger());
        const auto secondIndex = static_cast<std::size_t>(
            object.value("secondGeometryIndex").toInteger());
        dimension.geometryId = saved.geometry.lineId(firstIndex);
        dimension.secondPoint.lineId = saved.geometry.lineId(secondIndex);
        if (dimension.geometryId == sketch::kInvalidGeometryId ||
            dimension.secondPoint.lineId == sketch::kInvalidGeometryId)
          continue;
      } else if (kind == sketch::DimensionKind::PointDistance ||
                 kind == sketch::DimensionKind::PointDistanceX ||
                 kind == sketch::DimensionKind::PointDistanceY) {
        const auto restorePoint = [&saved, &object](
                                      const QString& prefix,
                                      sketch::PointReference* point) {
          point->start = object.value(prefix + QStringLiteral("Start"))
                             .toBool(true);
          if (object.value(prefix + QStringLiteral("Origin")).toBool(false)) {
            point->origin = true;
            return true;
          }
          const qint64 line =
              object.value(prefix + QStringLiteral("Line")).toInteger(-1);
          if (line >= 0) {
            point->lineId = saved.geometry.lineId(static_cast<std::size_t>(line));
            return point->lineId != sketch::kInvalidGeometryId;
          }
          const qint64 circle =
              object.value(prefix + QStringLiteral("Circle")).toInteger(-1);
          if (circle >= 0) {
            point->circleId =
                saved.geometry.circleId(static_cast<std::size_t>(circle));
            return point->circleId != sketch::kInvalidGeometryId;
          }
          const qint64 arc =
              object.value(prefix + QStringLiteral("Arc")).toInteger(-1);
          if (arc >= 0) {
            point->arcId = saved.geometry.arcId(static_cast<std::size_t>(arc));
            return point->arcId != sketch::kInvalidGeometryId;
          }
          const qint64 bezier =
              object.value(prefix + QStringLiteral("Bezier")).toInteger(-1);
          if (bezier >= 0) {
            point->bezierId =
                saved.geometry.bezierId(static_cast<std::size_t>(bezier));
            point->bezierPoint = static_cast<std::uint8_t>(
                object.value(prefix + QStringLiteral("BezierPoint"))
                    .toInteger(0));
            return point->bezierId != sketch::kInvalidGeometryId &&
                   point->bezierPoint < 4;
          }
          const qint64 center =
              object.value(prefix + QStringLiteral("ElementCenter"))
                  .toInteger(0);
          if (center > 0) {
            point->elementCenterId = static_cast<std::size_t>(center);
            return true;
          }
          return false;
        };
        if (!restorePoint(QStringLiteral("first"), &dimension.firstPoint) ||
            !restorePoint(QStringLiteral("second"), &dimension.secondPoint)) {
          setError(error,
                   QString::fromUtf8("Не удалось восстановить точки размера."));
          return false;
        }
      }

      saved.geometry.storeDimension(dimension);
    }

    std::vector<sketch::Constraint> restoredConstraints;
    restoredConstraints.reserve(
        static_cast<std::size_t>(savedObject.value("constraints").toArray().size()));
    for (const auto constraintValue : savedObject.value("constraints").toArray()) {
      const auto object = constraintValue.toObject();
      sketch::Constraint constraint;
      constraint.id =
          static_cast<sketch::ConstraintId>(object.value("id").toInteger());
      if (!decodeConstraintType(object, constraintEncoding, &constraint.type,
                                nullptr, error))
        return false;
      constraint.value = object.value("value").toDouble();

      const auto resolveGeometry =
          [&saved](const QString& kind, qint64 position) -> sketch::GeometryId {
        if (position < 0) return sketch::kInvalidGeometryId;
        const auto index = static_cast<std::size_t>(position);
        if (kind == QStringLiteral("line"))
          return saved.geometry.lineId(index);
        if (kind == QStringLiteral("circle"))
          return saved.geometry.circleId(index);
        if (kind == QStringLiteral("arc"))
          return saved.geometry.arcId(index);
        if (kind == QStringLiteral("bezier"))
          return saved.geometry.bezierId(index);
        return sketch::kInvalidGeometryId;
      };

      constraint.firstGeometry = resolveGeometry(
          object.value("firstGeometryKind").toString(),
          object.value("firstGeometry").toInteger(-1));
      constraint.secondGeometry = resolveGeometry(
          object.value("secondGeometryKind").toString(),
          object.value("secondGeometry").toInteger(-1));

      const qint64 firstPointLine = object.value("firstPointLine").toInteger(-1);
      if (firstPointLine >= 0) {
        constraint.firstPoint.lineId =
            saved.geometry.lineId(static_cast<std::size_t>(firstPointLine));
        constraint.firstPoint.start =
            object.value("firstPointStart").toBool(true);
      }

      const qint64 secondPointLine =
          object.value("secondPointLine").toInteger(-1);
      if (secondPointLine >= 0) {
        constraint.secondPoint.lineId =
            saved.geometry.lineId(static_cast<std::size_t>(secondPointLine));
        constraint.secondPoint.start =
            object.value("secondPointStart").toBool(true);
      }

      const qint64 firstPointCircle =
          object.value("firstPointCircle").toInteger(-1);
      if (firstPointCircle >= 0) {
        constraint.firstPoint.circleId =
            saved.geometry.circleId(
                static_cast<std::size_t>(firstPointCircle));
      }

      const qint64 secondPointCircle =
          object.value("secondPointCircle").toInteger(-1);
      if (secondPointCircle >= 0) {
        constraint.secondPoint.circleId =
            saved.geometry.circleId(
                static_cast<std::size_t>(secondPointCircle));
      }

      const qint64 firstPointArc =
          object.value("firstPointArc").toInteger(-1);
      if (firstPointArc >= 0) {
        constraint.firstPoint.arcId =
            saved.geometry.arcId(
                static_cast<std::size_t>(firstPointArc));
        constraint.firstPoint.start =
            object.value("firstPointStart").toBool(true);
      }

      const qint64 secondPointArc =
          object.value("secondPointArc").toInteger(-1);
      if (secondPointArc >= 0) {
        constraint.secondPoint.arcId =
            saved.geometry.arcId(
                static_cast<std::size_t>(secondPointArc));
        constraint.secondPoint.start =
            object.value("secondPointStart").toBool(true);
      }

      const qint64 firstPointBezier =
          object.value("firstPointBezier").toInteger(-1);
      if (firstPointBezier >= 0) {
        constraint.firstPoint.bezierId = saved.geometry.bezierId(
            static_cast<std::size_t>(firstPointBezier));
        constraint.firstPoint.bezierPoint = static_cast<std::uint8_t>(
            object.value("firstPointBezierPoint").toInteger(0));
      }

      const qint64 secondPointBezier =
          object.value("secondPointBezier").toInteger(-1);
      if (secondPointBezier >= 0) {
        constraint.secondPoint.bezierId = saved.geometry.bezierId(
            static_cast<std::size_t>(secondPointBezier));
        constraint.secondPoint.bezierPoint = static_cast<std::uint8_t>(
            object.value("secondPointBezierPoint").toInteger(0));
      }

      const qint64 firstPointElementCenter =
          object.value("firstPointElementCenter").toInteger(0);
      if (firstPointElementCenter > 0) {
        constraint.firstPoint.elementCenterId =
            static_cast<std::size_t>(
                firstPointElementCenter);
      }
      constraint.firstPoint.origin =
          object.value("firstPointOrigin").toBool(false);

      const qint64 secondPointElementCenter =
          object.value("secondPointElementCenter").toInteger(0);
      if (secondPointElementCenter > 0) {
        constraint.secondPoint.elementCenterId =
            static_cast<std::size_t>(
                secondPointElementCenter);
      }
      constraint.secondPoint.origin =
          object.value("secondPointOrigin").toBool(false);

      restoredConstraints.push_back(std::move(constraint));
    }
    if (!saved.geometry.restoreConstraints(std::move(restoredConstraints))) {
      setError(error,
               QString::fromUtf8(
                   "Ограничения эскиза %1 содержат неподдержанную или "
                   "повреждённую ссылку.")
                   .arg(loaded.sketches.size() + 1));
      return false;
    }
    if (!validateMaterializedSketchGeometry(saved.geometry, error))
      return false;

    loaded.sketches.push_back(std::move(saved));
  }
  const auto extrusion = root.value("extrusion").toObject();
  loaded.hasExtrusion = extrusion.value("enabled").toBool(false);
  if (extrusion.contains("sourceSketch"))
    loaded.extrusionSourceSketch =
        static_cast<std::size_t>(extrusion.value("sourceSketch").toInteger());
  *data = std::move(loaded);
  return true;
}

struct LegacyDecodePlan {
  LegacySketchSchema sketchSchema{LegacySketchSchema::EarlyV1};
  ConstraintTypeEncoding constraintEncoding{
      ConstraintTypeEncoding::CurrentOrdinal};
};

bool validateAndSelectLegacyPlan(const QJsonObject& root, qint64 version,
                                 LegacyDecodePlan* plan, bool* unsupported,
                                 QString* error) {
  if (!plan) return invalid(error, QString::fromUtf8("не задан план декодирования."));
  if (unsupported) *unsupported = false;

  const QJsonValue encodingValue =
      root.value(QLatin1String(kConstraintTypeEncodingKey));
  if (!encodingValue.isUndefined()) {
    if (!encodingValue.isString())
      return invalid(error,
                     QString::fromUtf8("маркер кодировки ограничений должен быть строкой."));
    if (encodingValue.toString() !=
        QLatin1String(kStableConstraintTypeEncoding)) {
      if (unsupported) *unsupported = true;
      setError(error,
               QString::fromUtf8("Неподдерживаемая кодировка ограничений: %1.")
                   .arg(encodingValue.toString()));
      return false;
    }
    *plan = {LegacySketchSchema::StableNameV1,
             ConstraintTypeEncoding::StableNameV1};
    return validateLegacyRoot(root, false, plan->sketchSchema,
                              plan->constraintEncoding, nullptr, unsupported,
                              error);
  }

  if (version == 2) {
    *plan = {LegacySketchSchema::V2,
             ConstraintTypeEncoding::CurrentOrdinal};
    return validateLegacyRoot(root, false, plan->sketchSchema,
                              plan->constraintEncoding, nullptr, unsupported,
                              error);
  }

  const QJsonValue sketchesValue = root.value("sketches");
  const QJsonArray sketches = sketchesValue.toArray();
  bool anyCenterMarker = false;
  bool allCenterMarkers = !sketches.isEmpty();
  for (const auto& value : sketches) {
    const bool hasMarker =
        value.isObject() && value.toObject().contains("centerNodeElementIds");
    anyCenterMarker = anyCenterMarker || hasMarker;
    allCenterMarkers = allCenterMarkers && hasMarker;
  }
  if (anyCenterMarker && !allCenterMarkers)
    return invalid(
        error,
        QString::fromUtf8(
            "эскизы используют смешанные исторические схемы сериализации."));
  if (allCenterMarkers) {
    *plan = {LegacySketchSchema::CurrentV1,
             ConstraintTypeEncoding::CurrentOrdinal};
    return validateLegacyRoot(root, true, plan->sketchSchema,
                              plan->constraintEncoding, nullptr, unsupported,
                              error);
  }

  std::vector<sketch::ConstraintType> prePointOnLineTypes;
  std::vector<sketch::ConstraintType> currentHistoricalTypes;
  QString prePointOnLineError;
  QString currentHistoricalError;
  bool preUnsupported = false;
  bool currentUnsupported = false;
  const bool validPrePointOnLine = validateLegacyRoot(
      root, true, LegacySketchSchema::EarlyV1,
      ConstraintTypeEncoding::PrePointOnLineOrdinal, &prePointOnLineTypes,
      &preUnsupported, &prePointOnLineError);
  const bool validCurrentHistorical = validateLegacyRoot(
      root, true, LegacySketchSchema::EarlyV1,
      ConstraintTypeEncoding::CurrentHistoricalOrdinal,
      &currentHistoricalTypes, &currentUnsupported, &currentHistoricalError);

  if (validPrePointOnLine && validCurrentHistorical) {
    if (prePointOnLineTypes != currentHistoricalTypes) {
      if (unsupported) *unsupported = true;
      setError(error,
               QString::fromUtf8(
                   "Неоднозначная историческая кодировка ограничений v1."));
      return false;
    }
    *plan = {LegacySketchSchema::EarlyV1,
             ConstraintTypeEncoding::CurrentHistoricalOrdinal};
    return true;
  }
  if (validPrePointOnLine) {
    *plan = {LegacySketchSchema::EarlyV1,
             ConstraintTypeEncoding::PrePointOnLineOrdinal};
    return true;
  }
  if (validCurrentHistorical) {
    *plan = {LegacySketchSchema::EarlyV1,
             ConstraintTypeEncoding::CurrentHistoricalOrdinal};
    return true;
  }

  if (unsupported)
    *unsupported = preUnsupported || currentUnsupported;
  setError(error, !currentHistoricalError.isEmpty() ? currentHistoricalError
                                                     : prePointOnLineError);
  return false;
}

StagedProjectLoad ProjectFile::stageLoad(const QString& path) {
  StagedProjectLoad result;
  try {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      result.error = file.errorString();
      return result;
    }
    if (file.size() < 0 || file.size() > kMaximumFileBytes) {
      result.error = QString::fromUtf8(
          "Файл проекта превышает допустимый размер (%1 байт).")
                         .arg(kMaximumFileBytes);
      return result;
    }
    const QByteArray payload = file.readAll();
    if (payload.size() != file.size()) {
      result.error = QString::fromUtf8("Не удалось полностью прочитать файл проекта.");
      return result;
    }

    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
      result.error = QString::fromUtf8("Файл проекта повреждён: ") +
                     parseError.errorString();
      return result;
    }
    const QJsonObject root = parsed.object();
    if (!root.value("format").isString()) {
      result.error = QString::fromUtf8(
          "Файл проекта повреждён: отсутствует строковое поле format.");
      return result;
    }
    if (root.value("format").toString() != QStringLiteral("solidar-project")) {
      result.kind = ProjectLoadKind::Unsupported;
      result.error = QString::fromUtf8("Неподдерживаемый формат проекта.");
      return result;
    }
    qint64 version = 0;
    QString validationError;
    if (!integerNumber(root.value("version"), QStringLiteral("version"), 1,
                       kMaximumExactJsonInteger, &version,
                       &validationError)) {
      result.error = validationError;
      return result;
    }
    if (version != 1 && version != 2) {
      result.kind = ProjectLoadKind::Unsupported;
      result.error = QString::fromUtf8("Неподдерживаемая версия проекта: %1.")
                         .arg(version);
      return result;
    }
    // The parametric model was introduced together with format v2. Treating
    // a downgraded/corrupted v2 root as legacy would silently discard its
    // history and allow the next Save to overwrite the original model.
    if (version == 1 && root.contains("model")) {
      result.error = QString::fromUtf8(
          "Проект v1 не может содержать параметрическую модель v2.");
      return result;
    }
    LegacyDecodePlan decodePlan;
    bool unsupportedSchema = false;
    if (!validateAndSelectLegacyPlan(root, version, &decodePlan,
                                     &unsupportedSchema, &validationError)) {
      if (unsupportedSchema) result.kind = ProjectLoadKind::Unsupported;
      result.error = validationError;
      return result;
    }
    ProjectData legacy;
    if (!loadLegacyRoot(root, decodePlan.constraintEncoding, &legacy,
                        &validationError)) {
      result.error = validationError.isEmpty()
                         ? QString::fromUtf8("Не удалось восстановить данные проекта.")
                         : validationError;
      return result;
    }
    if (version == 1) {
      result.kind = ProjectLoadKind::ValidV1;
      result.legacy = std::move(legacy);
      result.error.clear();
      return result;
    }

    bool unsupportedModel = false;
    if (!validateV2Root(root, legacy, &validationError, &unsupportedModel)) {
      if (unsupportedModel) result.kind = ProjectLoadKind::Unsupported;
      result.error = validationError;
      return result;
    }
    Document document;
    {
      // Parsing and rebuilding are speculative until the complete staged
      // document has passed validation.  Explicit persisted IDs must not
      // advance process-global edit generators on a rejected file.
      ::solidar::detail::ScopedExplicitIdReservationPause reservationPause;
      if (!loadDocumentRoot(root, legacy, &document, &validationError)) {
        result.error = validationError.isEmpty()
                           ? QString::fromUtf8(
                                 "Не удалось восстановить параметрическую модель.")
                           : validationError;
        return result;
      }
    }
    result.kind = ProjectLoadKind::ValidV2;
    result.legacy = std::move(legacy);
    result.document.emplace(std::move(document));
    result.error.clear();
    return result;
  } catch (const Standard_Failure& failure) {
    result.error = QString::fromUtf8("Ошибка OCCT при загрузке проекта: ") +
                   QString::fromUtf8(failure.what());
  } catch (const std::exception& exception) {
    result.error = QString::fromUtf8("Ошибка загрузки проекта: ") +
                   QString::fromUtf8(exception.what());
  } catch (...) {
    result.error = QString::fromUtf8(
        "Неизвестная ошибка при загрузке проекта.");
  }
  if (result.error.isEmpty())
    result.error = QString::fromUtf8("Не удалось загрузить проект.");
  result.kind = ProjectLoadKind::Invalid;
  result.document.reset();
  return result;
}

bool ProjectFile::load(const QString& path, ProjectData* data, QString* error) {
  if (!data) {
    setError(error, QString::fromUtf8("Не задано хранилище для данных проекта."));
    return false;
  }
  auto staged = stageLoad(path);
  if (!staged.succeeded()) {
    setError(error, staged.error);
    return false;
  }
  *data = std::move(staged.legacy);
  return true;
}

bool ProjectFile::loadDocument(const QString& path, Document* document,
                               QString* error) {
  if (!document) {
    setError(error, QString::fromUtf8("Не задан документ для загрузки."));
    return false;
  }
  auto staged = stageLoad(path);
  if (!staged.succeeded()) {
    setError(error, staged.error);
    return false;
  }
  if (staged.kind != ProjectLoadKind::ValidV2 || !staged.document) {
    setError(error, QString::fromUtf8(
                        "Проект не содержит параметрическую историю v2."));
    return false;
  }
  *document = std::move(*staged.document);
  document->reserveIdsForEditing();
  return true;
}

}  // namespace solidar::project
