#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV3ModelSemanticValidation.h"
#include "project/internal/ProjectFormatV3TypedSketchValidation.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectFormatV4DocumentMaterialization.h"
#include "project/internal/ProjectFormatV4ApplicationGate.h"
#include "project/internal/ProjectFormatV4AtomicWriting.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4ModelValidator.h"
#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/ProjectFormatV4ReferenceEncoder.h"

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
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <sstream>
#include <type_traits>
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
#include "sketch/SketchSchema.h"

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
  QJsonObject result{{"bodyId", static_cast<qint64>(face.bodyId())},
                     {"featureId", static_cast<qint64>(face.featureId())},
                     {"faceIndex", static_cast<qint64>(face.faceIndex())}};
  if (!face.persistentTag().empty())
    result["persistentTag"] = QString::fromStdString(face.persistentTag());
  if (face.signature())
    result["signature"] = faceSignature(*face.signature());
  return result;
}

std::optional<QJsonObject> savedEdgeReference(const EdgeReference& edge) {
  const auto* legacy = edge.v1();
  if (!legacy) return std::nullopt;
  QJsonObject result{{"bodyId", static_cast<qint64>(legacy->bodyId)},
                     {"featureId", static_cast<qint64>(legacy->featureId)},
                     {"edgeIndex", static_cast<qint64>(legacy->legacyIndex)}};
  if (!legacy->persistentTag.empty())
    result["persistentTag"] =
        QString::fromStdString(legacy->persistentTag);
  if (legacy->edgeSignature)
    result["signature"] = edgeSignature(*legacy->edgeSignature);
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
      static_cast<std::size_t>(saved.value("faceIndex").toInteger()),
      saved.value("persistentTag").toString().toStdString(),
      readFaceSignature(saved.value("signature"))};
  return result;
}

EdgeReference loadedEdgeReference(const QJsonValue& value) {
  const auto saved = value.toObject();
  return EdgeReference{
      static_cast<BodyId>(saved.value("bodyId").toInteger()),
      static_cast<FeatureId>(saved.value("featureId").toInteger()),
      static_cast<std::size_t>(saved.value("edgeIndex").toInteger()),
      saved.value("persistentTag").toString().toStdString(),
      readEdgeSignature(saved.value("signature"))};
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

bool exactObjectFields(const QJsonObject& object,
                       std::initializer_list<const char*> fields,
                       const QString& context, QString* error) {
  if (object.size() != static_cast<qsizetype>(fields.size()))
    return invalid(error, QString::fromUtf8("%1 содержит лишние или отсутствующие поля.")
                              .arg(context));
  for (const char* field : fields) {
    if (!object.contains(QLatin1String(field)))
      return invalid(error, QString::fromUtf8("%1 не содержит поле %2.")
                                .arg(context, QString::fromLatin1(field)));
  }
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
    if (!integerNumber(dimension.value("kind"), QStringLiteral("kind"), 0, 7,
                       &kind, error) ||
        !finiteNumber(dimension.value("value"), QStringLiteral("value"), 0.0,
                      kMaximumModelLengthMm, error) ||
        dimension.value("value").toDouble() == 0.0 ||
        !finiteNumber(dimension.value("offset"), QStringLiteral("offset"),
                      -kMaximumModelLengthMm, kMaximumModelLengthMm, error) ||
        !finiteNumber(dimension.value("angle"), QStringLiteral("angle"),
                      -1.0e6, 1.0e6, error))
      return false;
    if ((kind == 0 || kind == 2 || kind == 7) &&
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
         (kind == static_cast<qint64>(sketch::DimensionKind::CircleDiameter) ||
          kind == static_cast<qint64>(sketch::DimensionKind::CircleRadius))) &&
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
  QString topologyPathPrefix;
  QString* error{};
  qint64& totalRebuildWork;
  std::function<bool(const QJsonObject&, const char*)> localFeature;
  std::function<bool(const QJsonObject&, const char*)> sketchReference;
  std::function<bool(const QJsonObject&, const char*, const char*, const char*)>
      sketchLineReference;
  std::function<bool(const QJsonObject&, const char*, const char*)>
      externalFeature;
  struct TopologyOwner {
    bool available{false};
    BodyId bodyId{kInvalidBodyId};
    FeatureId featureId{kInvalidFeatureId};
  };
  std::function<bool(const QJsonValue&, bool, const QString&, TopologyOwner*)>
      topologyReference;
  std::function<bool(qsizetype)> accountTopologyReferences;
  std::function<bool(const QJsonValue&)> accountProfileGeometry;
};

struct FeatureEncodeContext {
  const Document& document;
  QString* error{};
  std::function<bool(const FaceReference&, QJsonObject*)> faceReference;
  std::function<bool(const EdgeReference&, QJsonObject*)> edgeReference;
};

struct FeatureDecodeContext {
  Document& document;
  QString* error{};
  std::function<sketch::GeometryId(const QJsonObject&, const char*,
                                   const char*, const char*)>
      restoredSketchLineId;
  std::function<bool(const QJsonValue&, const QString&, FaceReference*)>
      faceReference;
  std::function<bool(const QJsonValue&, const QString&, EdgeReference*)>
      edgeReference;
  QString topologyPathPrefix;
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
    if (!context.topologyReference(feature.value("face"), true,
                                   context.topologyPathPrefix + ".face", nullptr))
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
  for (qsizetype index = 0; index < references.size(); ++index)
    if (!context.topologyReference(
            references[index], false,
            QStringLiteral("%1.edges[%2]").arg(context.topologyPathPrefix).arg(index),
            nullptr))
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
  if (!context.localFeature(feature, "sourceFeatureId") ||
      !enumNumber(feature.value("plane"), QStringLiteral("plane"),
                  static_cast<qint64>(MirrorPlane::YZ), context.error))
    return false;
  if (!feature.contains("planeFace")) return true;
  FeatureValidationContext::TopologyOwner owner;
  if (!context.accountTopologyReferences(1) ||
      !context.topologyReference(feature.value("planeFace"), true,
                                 context.topologyPathPrefix + ".planeFace",
                                 &owner))
    return false;
  if (owner.available &&
      (owner.bodyId != context.bodyId ||
       owner.featureId != static_cast<FeatureId>(
                              feature.value("sourceFeatureId").toInteger())))
    return invalid(
        context.error,
        QString::fromUtf8(
            "грань плоскости Mirror должна принадлежать исходной фиче текущего тела."));
  return true;
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
  for (qsizetype index = 0; index < faces.size(); ++index)
    if (!context.topologyReference(
            faces[index], true,
            QStringLiteral("%1.removedFaces[%2]")
                .arg(context.topologyPathPrefix)
                .arg(index),
            nullptr))
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
  for (qsizetype index = 0; index < faces.size(); ++index)
    if (!context.topologyReference(
            faces[index], true,
            QStringLiteral("%1.draftedFaces[%2]")
                .arg(context.topologyPathPrefix)
                .arg(index),
            nullptr))
      return false;
  if (feature.value("neutralPlaneType").toInt() ==
      static_cast<int>(NeutralPlaneType::BodyFace)) {
    const auto neutralValue = feature.value("neutralPlaneFace");
    FeatureValidationContext::TopologyOwner owner;
    if (!context.topologyReference(
            neutralValue, true,
            context.topologyPathPrefix + ".neutralPlaneFace", &owner))
      return false;
    if (owner.available &&
        (owner.bodyId != context.bodyId ||
         owner.featureId != static_cast<FeatureId>(
                                feature.value("sourceFeatureId").toInteger())))
      return invalid(
          context.error,
          QString::fromUtf8(
              "нейтральная грань Draft должна принадлежать исходной фиче текущего тела."));
  }
  if (feature.contains("rotationEdge") &&
      !context.topologyReference(feature.value("rotationEdge"), false,
                                 context.topologyPathPrefix + ".rotationEdge",
                                 nullptr))
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
    if (const auto face = extrude->faceReference()) {
      QJsonObject encoded;
      if (!context.faceReference(*face, &encoded)) return false;
      saved["face"] = encoded;
    }
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
  for (const auto& edge : treatment->edges()) {
    QJsonObject encoded;
    if (!context.edgeReference(edge, &encoded)) return false;
    edges.append(encoded);
  }
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
  if (mirror->planeFace()) {
    QJsonObject encoded;
    if (!context.faceReference(*mirror->planeFace(), &encoded)) return false;
    saved["planeFace"] = encoded;
  }
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
  for (const auto& face : shell->removedFaces()) {
    QJsonObject encoded;
    if (!context.faceReference(face, &encoded)) return false;
    faces.append(encoded);
  }
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
  if (draft->neutralPlane().face) {
    QJsonObject encoded;
    if (!context.faceReference(*draft->neutralPlane().face, &encoded))
      return false;
    saved["neutralPlaneFace"] = encoded;
  }
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
  if (draft->rotationEdge()) {
    QJsonObject encoded;
    if (!context.edgeReference(*draft->rotationEdge(), &encoded)) return false;
    saved["rotationEdge"] = encoded;
  }
  QJsonArray faces;
  for (const auto& face : draft->draftedFaces()) {
    QJsonObject encoded;
    if (!context.faceReference(face, &encoded)) return false;
    faces.append(encoded);
  }
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
                   const FeatureDecodeContext& context) {
  const auto sourceKind = saved.value("sourceKind").toString();
  const auto lengthMm = saved.value("lengthMm").toDouble();
  const auto operation =
      static_cast<ExtrudeOperation>(saved.value("operation").toInt());
  const auto reversed = saved.value("reversed").toBool();
  if (sourceKind == QStringLiteral("face")) {
    FaceReference face;
    if (!context.faceReference(
            saved.value("face"), context.topologyPathPrefix + ".face.reference",
            &face))
      return false;
    body.addFeature(std::make_unique<ExtrudeFeature>(
        id, std::move(face), lengthMm, name, operation, reversed));
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

bool loadedEdges(const QJsonArray& values, const FeatureDecodeContext& context,
                 std::vector<EdgeReference>* edges) {
  if (!edges) return false;
  edges->clear();
  edges->reserve(static_cast<std::size_t>(values.size()));
  for (qsizetype index = 0; index < values.size(); ++index) {
    EdgeReference edge;
    if (!context.edgeReference(
            values[index],
            QStringLiteral("%1.edges[%2].reference")
                .arg(context.topologyPathPrefix)
                .arg(index),
            &edge))
      return false;
    edges->push_back(std::move(edge));
  }
  return true;
}

bool decodeFillet(const QJsonObject& saved, FeatureId id,
                  const std::string& name, Body& body,
                  const FeatureDecodeContext& context) {
  std::vector<EdgeReference> edges;
  if (!loadedEdges(saved.value("edges").toArray(), context, &edges))
    return false;
  body.addFeature(std::make_unique<FilletFeature>(
      id, std::move(edges), saved.value("radiusMm").toDouble(), name));
  return true;
}

bool decodeChamfer(const QJsonObject& saved, FeatureId id,
                   const std::string& name, Body& body,
                   const FeatureDecodeContext& context) {
  std::vector<EdgeReference> edges;
  if (!loadedEdges(saved.value("edges").toArray(), context, &edges))
    return false;
  body.addFeature(std::make_unique<ChamferFeature>(
      id, std::move(edges), saved.value("distanceMm").toDouble(), name));
  return true;
}

bool decodeMirror(const QJsonObject& saved, FeatureId id,
                  const std::string& name, Body& body,
                  const FeatureDecodeContext& context) {
  const auto source =
      static_cast<FeatureId>(saved.value("sourceFeatureId").toInteger());
  if (saved.contains("planeFace")) {
    FaceReference face;
    if (!context.faceReference(
            saved.value("planeFace"),
            context.topologyPathPrefix + ".planeFace.reference", &face))
      return false;
    body.addFeature(std::make_unique<MirrorFeature>(
        id, source, std::move(face), name));
  } else
    body.addFeature(std::make_unique<MirrorFeature>(
        id, source, static_cast<MirrorPlane>(saved.value("plane").toInt()),
        name));
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
                 const FeatureDecodeContext& context) {
  std::vector<FaceReference> faces;
  const auto values = saved.value("removedFaces").toArray();
  faces.reserve(static_cast<std::size_t>(values.size()));
  for (qsizetype index = 0; index < values.size(); ++index) {
    FaceReference face;
    if (!context.faceReference(
            values[index],
            QStringLiteral("%1.removedFaces[%2].reference")
                .arg(context.topologyPathPrefix)
                .arg(index),
            &face))
      return false;
    faces.push_back(std::move(face));
  }
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
  const auto drafted = saved.value("draftedFaces").toArray();
  faces.reserve(static_cast<std::size_t>(drafted.size()));
  for (qsizetype index = 0; index < drafted.size(); ++index) {
    FaceReference face;
    if (!context.faceReference(
            drafted[index],
            QStringLiteral("%1.draftedFaces[%2].reference")
                .arg(context.topologyPathPrefix)
                .arg(index),
            &face))
      return false;
    faces.push_back(std::move(face));
  }
  PlaneReference plane{
      static_cast<NeutralPlaneType>(saved.value("neutralPlaneType").toInt())};
  if (saved.contains("neutralPlaneFace")) {
    FaceReference face;
    if (!context.faceReference(
            saved.value("neutralPlaneFace"),
            context.topologyPathPrefix + ".neutralPlaneFace.reference", &face))
      return false;
    plane.face = std::move(face);
  }
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
  if (saved.contains("rotationEdge")) {
    EdgeReference edge;
    if (!context.edgeReference(
            saved.value("rotationEdge"),
            context.topologyPathPrefix + ".rotationEdge.reference", &edge))
      return false;
    rotationEdge = std::move(edge);
  }
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
  auto topologyReference = [&](const QJsonValue& value, bool face,
                               const QString&,
                               FeatureValidationContext::TopologyOwner* owner) {
    if (!validateTopologyReference(value, face, featureSets, error))
      return false;
    if (owner) {
      const auto reference = value.toObject();
      owner->available = true;
      owner->bodyId =
          static_cast<BodyId>(reference.value("bodyId").toInteger());
      owner->featureId =
          static_cast<FeatureId>(reference.value("featureId").toInteger());
    }
    return true;
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
          {},
          error,
          totalRebuildWork,
          [&](const QJsonObject& candidate, const char* key) {
            return localFeature(bodyId, candidate, key);
          },
          sketchReference,
          sketchLineReference,
          externalFeature,
          topologyReference,
          accountTopologyReferences,
          accountProfileGeometry};
      if (!codec->validate(feature, context)) return false;
    }
  }
  return true;
}

}  // namespace

namespace detail {

ProjectFormatV3ModelSemanticValidationResult
validateProjectFormatV3ModelSemantics(
    const QJsonObject& authoredRoot,
    const std::vector<ProjectFormatV3TopologyInspectionView>& topology) {
  QString error;
  const QJsonObject model = authoredRoot.value("model").toObject();
  const QJsonArray sketches = model.value("sketches").toArray();
  const QJsonArray bodies = model.value("bodies").toArray();
  const QJsonArray legacySketches = authoredRoot.value("sketches").toArray();

  std::unordered_set<SketchId> sketchIds;
  std::unordered_map<SketchId, qsizetype> sketchLineCounts;
  for (qsizetype index = 0; index < sketches.size(); ++index) {
    const SketchId id = static_cast<SketchId>(
        sketches[index].toObject().value("id").toInteger());
    sketchIds.insert(id);
    sketchLineCounts.emplace(
        id, legacySketches[index].toObject().value("lines").toArray().size());
  }

  FeatureSets featureSets;
  for (qsizetype bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex) {
    const auto bodyValue = bodies[bodyIndex];
    const auto body = bodyValue.toObject();
    const BodyId bodyId =
        static_cast<BodyId>(body.value("id").toInteger());
    auto& features = featureSets[bodyId];
    for (const auto featureValue : body.value("features").toArray())
      features.insert(static_cast<FeatureId>(
          featureValue.toObject().value("id").toInteger()));
  }

  std::map<QString, std::size_t> topologyByPath;
  for (std::size_t index = 0; index < topology.size(); ++index) {
    static const QString suffix = QStringLiteral(".reference");
    if (!topology[index].path.endsWith(suffix))
      return {ProjectFormatV3ModelSemanticValidationStatus::Invalid,
              QStringLiteral("topology inspection path inventory is invalid")};
    const QString wrapperPath =
        topology[index].path.left(topology[index].path.size() - suffix.size());
    if (wrapperPath.isEmpty() ||
        !topologyByPath.emplace(wrapperPath, index).second)
      return {ProjectFormatV3ModelSemanticValidationStatus::Invalid,
              QStringLiteral("topology inspection path inventory is invalid")};
  }
  std::vector<bool> topologyConsumed(topology.size(), false);
  bool unsupported = false;
  auto topologyReference = [&](const QJsonValue& wrapper, bool face,
                               const QString& path,
                               FeatureValidationContext::TopologyOwner* owner) {
    if (!wrapper.isObject() ||
        !wrapper.toObject().value("reference").isObject())
      return invalid(&error,
                     QStringLiteral("v4 topology wrapper is malformed"));
    const auto found = topologyByPath.find(path);
    if (found == topologyByPath.end())
      return invalid(&error,
                     QStringLiteral("topology inspection path is missing"));
    const std::size_t topologyIndex = found->second;
    if (topologyConsumed[topologyIndex])
      return invalid(&error,
                     QStringLiteral("topology inspection path is duplicated"));
    topologyConsumed[topologyIndex] = true;
    const auto& view = topology[topologyIndex];
    const QJsonValue authored = wrapper.toObject().value("reference");
    if (authored != view.authoredReference ||
        view.expectedKind !=
            (face ? TopologyKind::Face : TopologyKind::Edge))
      return invalid(&error,
                     QStringLiteral("topology inspection inventory is not canonical"));
    if (!view.bodyId || !view.featureId) {
      unsupported = true;
      return true;
    }
    if (owner) {
      owner->available = true;
      owner->bodyId = *view.bodyId;
      owner->featureId = *view.featureId;
    }
    return true;
  };

  qsizetype totalTopologyReferences = 0;
  const auto accountTopologyReferences = [&](qsizetype count) {
    if (count > ProjectFile::kMaximumFeatureTopologyReferences)
      return false;
    totalTopologyReferences += count;
    return totalTopologyReferences <=
           ProjectFile::kMaximumDocumentTopologyReferences;
  };

  for (qsizetype sketchIndex = 0; sketchIndex < sketches.size(); ++sketchIndex) {
    const auto sketchValue = sketches[sketchIndex];
    const auto support =
        sketchValue.toObject().value("support").toObject();
    if (support.value("type").toInt() ==
        static_cast<int>(SketchSupportType::Face)) {
      if (!accountTopologyReferences(1) ||
          !topologyReference(
              support, true,
              QStringLiteral("$.model.sketches[%1].support").arg(sketchIndex),
              nullptr))
        return {ProjectFormatV3ModelSemanticValidationStatus::Invalid,
                error.isEmpty()
                    ? QStringLiteral("topology reference limit exceeded")
                    : error};
    }
  }

  auto localFeature = [&](BodyId bodyId, const QJsonObject& feature,
                          const char* key) {
    qint64 id = 0;
    return integerNumber(feature.value(QLatin1String(key)),
                         QString::fromLatin1(key), 1,
                         ProjectFile::kMaximumPersistedId, &id, &error) &&
           validateFeatureReference(bodyId, static_cast<FeatureId>(id),
                                    featureSets, &error);
  };
  auto sketchReference = [&](const QJsonObject& feature, const char* key) {
    qint64 id = 0;
    if (!integerNumber(feature.value(QLatin1String(key)),
                       QString::fromLatin1(key), 1,
                       ProjectFile::kMaximumPersistedId, &id, &error))
      return false;
    if (!sketchIds.contains(static_cast<SketchId>(id)))
      return invalid(&error,
                     QString::fromUtf8("ссылка указывает на неизвестный эскиз."));
    return true;
  };
  auto sketchLineReference = [&](const QJsonObject& feature,
                                 const char* sketchKey, const char* lineIdKey,
                                 const char* lineIndexKey) {
    qint64 sketchIdValue = 0, rawLineId = 0, lineIndex = -1;
    if (!integerNumber(feature.value(QLatin1String(sketchKey)),
                       QString::fromLatin1(sketchKey), 1,
                       ProjectFile::kMaximumPersistedId, &sketchIdValue,
                       &error) ||
        !integerNumber(feature.value(QLatin1String(lineIdKey)),
                       QString::fromLatin1(lineIdKey), 1,
                       kMaximumExactJsonInteger, &rawLineId, &error) ||
        !integerNumber(feature.value(QLatin1String(lineIndexKey)),
                       QString::fromLatin1(lineIndexKey), 0,
                       ProjectFile::kMaximumCollectionItems, &lineIndex,
                       &error))
      return false;
    const auto found =
        sketchLineCounts.find(static_cast<SketchId>(sketchIdValue));
    if (found == sketchLineCounts.end())
      return invalid(&error,
                     QString::fromUtf8("ось ссылается на неизвестный эскиз."));
    if (lineIndex >= static_cast<qint64>(found->second))
      return invalid(&error,
                     QString::fromUtf8("ось ссылается на неизвестную линию."));
    if (rawLineId != lineIndex + 1)
      return invalid(&error,
                     QString::fromUtf8("ID и позиция линии оси не согласованы."));
    return true;
  };
  auto externalFeature = [&](const QJsonObject& feature, const char* bodyKey,
                             const char* featureKey) {
    qint64 bodyId = 0, featureId = 0;
    return integerNumber(feature.value(QLatin1String(bodyKey)),
                         QString::fromLatin1(bodyKey), 1,
                         ProjectFile::kMaximumPersistedId, &bodyId, &error) &&
           integerNumber(feature.value(QLatin1String(featureKey)),
                         QString::fromLatin1(featureKey), 1,
                         ProjectFile::kMaximumPersistedId, &featureId,
                         &error) &&
           validateFeatureReference(static_cast<BodyId>(bodyId),
                                    static_cast<FeatureId>(featureId),
                                    featureSets, &error);
  };

  qsizetype totalProfileGeometryItems = 0;
  for (const auto sketchValue : legacySketches) {
    const auto sketch = sketchValue.toObject();
    totalProfileGeometryItems += sketch.value("lines").toArray().size() +
                                 sketch.value("circles").toArray().size() +
                                 sketch.value("arcs").toArray().size() +
                                 sketch.value("beziers").toArray().size();
  }
  const auto accountProfileGeometry = [&](const QJsonValue& value) {
    const auto profile = value.toObject();
    totalProfileGeometryItems += profile.value("lines").toArray().size() +
                                 profile.value("circles").toArray().size() +
                                 profile.value("arcs").toArray().size() +
                                 profile.value("beziers").toArray().size();
    return totalProfileGeometryItems <=
           ProjectFile::kMaximumDocumentSketchGeometryItems;
  };

  qint64 totalRebuildWork = 0;
  for (qsizetype bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex) {
    const auto bodyValue = bodies[bodyIndex];
    const auto body = bodyValue.toObject();
    const BodyId bodyId =
        static_cast<BodyId>(body.value("id").toInteger());
    const QJsonArray features = body.value("features").toArray();
    for (qsizetype featureIndex = 0; featureIndex < features.size();
         ++featureIndex) {
      const QJsonObject feature = features[featureIndex].toObject();
      if (++totalRebuildWork > ProjectFile::kMaximumDocumentRebuildWork)
        return {ProjectFormatV3ModelSemanticValidationStatus::Invalid,
                QString::fromUtf8(
                    "параметрическая история превышает лимит перестроения.")};
      const auto* codec =
          findFeatureCodec(QStringView{feature.value("type").toString()});
      if (!codec)
        return {ProjectFormatV3ModelSemanticValidationStatus::Invalid,
                QString::fromUtf8("неизвестный тип фичи.")};
      FeatureValidationContext context{
          true,
          bodyId,
          featureIndex,
          features,
          featureSets,
          QStringLiteral("$.model.bodies[%1].features[%2]")
              .arg(bodyIndex)
              .arg(featureIndex),
          &error,
          totalRebuildWork,
          [&](const QJsonObject& candidate, const char* key) {
            return localFeature(bodyId, candidate, key);
          },
          sketchReference,
          sketchLineReference,
          externalFeature,
          topologyReference,
          accountTopologyReferences,
          accountProfileGeometry};
      if (!codec->validate(feature, context))
        return {ProjectFormatV3ModelSemanticValidationStatus::Invalid,
                error};
    }
  }
  if (std::any_of(topologyConsumed.begin(), topologyConsumed.end(),
                  [](bool consumed) { return !consumed; }))
    return {ProjectFormatV3ModelSemanticValidationStatus::Invalid,
            QStringLiteral("topology inspection inventory is long")};
  return {unsupported
              ? ProjectFormatV3ModelSemanticValidationStatus::Unsupported
              : ProjectFormatV3ModelSemanticValidationStatus::Candidate,
          {}};
}

}  // namespace detail

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
using MaterializedFaceReference =
    std::function<bool(const QJsonValue&, const QString&, FaceReference*)>;
using MaterializedEdgeReference =
    std::function<bool(const QJsonValue&, const QString&, EdgeReference*)>;
static bool populateDocumentRoot(
    const QJsonObject& root, const ProjectData& legacy, Document* document,
    const MaterializedFaceReference& faceReference,
    const MaterializedEdgeReference& edgeReference, QString* error);
static bool loadDocumentRoot(const QJsonObject& root,
                             const ProjectData& legacy, Document* document,
                             QString* error);
static bool migrateLegacyDimensionLinks(
    sketch::Sketch* geometry,
    std::vector<sketch::Dimension> legacyDimensions,
    QString* error);
static double measuredConstraintValue(const sketch::Sketch& geometry,
                                      const sketch::Constraint& constraint);
static bool upgradeSketchSchemaToV3(QJsonObject* root,
                                    const ProjectData& data,
                                    QString* error);
static bool loadV3Root(const QJsonObject& root, ProjectData* data,
                       QString* error, bool* unsupported = nullptr);

bool ProjectFile::create(const QString& path, QString* error) {
  // New projects must start in the canonical parametric format.  Creating a
  // legacy v1 shell and upgrading it only on the first Save made the create
  // path different from every subsequent load/save cycle.
  return saveDocument(path, Document{}, error);
}

static bool serializedProjectRootWithMetadata(
    const QString& name, const QString& createdAt, const ProjectData& data,
    QJsonObject* result, QString* error) {
  if (!result) {
    setError(error, QString::fromUtf8("Не задано хранилище для проекта."));
    return false;
  }
  QJsonObject root;
  root["format"] = "solidar-project";
  root["version"] = 1;
  root[kConstraintTypeEncodingKey] = kStableConstraintTypeEncoding;
  root["name"] = name;
  root["createdAt"] = createdAt;
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
      } else if (dimension.kind == sketch::DimensionKind::CircleDiameter ||
                 dimension.kind == sketch::DimensionKind::CircleRadius) {
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

      const bool pointDimension =
          dimension.kind == sketch::DimensionKind::PointDistance ||
          dimension.kind == sketch::DimensionKind::PointDistanceX ||
          dimension.kind == sketch::DimensionKind::PointDistanceY;
      dimensions.append(QJsonObject{
          {"id", static_cast<qint64>(dimension.id)},
          {"kind", static_cast<int>(dimension.kind)},
          {"geometryIndex", geometryIndex},
          {"secondGeometryIndex", secondGeometryIndex},
          {"firstLine", firstLine},
          {"firstCircle", firstCircle},
          {"firstArc", firstArc},
          {"firstBezier", firstBezier},
          {"firstBezierPoint",
           static_cast<int>(dimension.firstPoint.bezierPoint)},
          {"firstElementCenter", firstElementCenter},
          {"firstStart", static_cast<bool>(dimension.firstPoint.start)},
          {"firstOrigin",
           pointDimension && static_cast<bool>(dimension.firstPoint.origin)},
          {"secondLine", secondLine},
          {"secondCircle", secondCircle},
          {"secondArc", secondArc},
          {"secondBezier", secondBezier},
          {"secondBezierPoint",
           static_cast<int>(dimension.secondPoint.bezierPoint)},
          {"secondElementCenter", secondElementCenter},
          {"secondStart", static_cast<bool>(dimension.secondPoint.start)},
          {"secondOrigin",
           pointDimension && static_cast<bool>(dimension.secondPoint.origin)},
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

      const auto constraintType =
          static_cast<sketch::ConstraintType>(constraint.type);
      const bool hasFirstPoint =
          constraintType == sketch::ConstraintType::Coincident ||
          constraintType == sketch::ConstraintType::Distance ||
          constraintType == sketch::ConstraintType::DistanceX ||
          constraintType == sketch::ConstraintType::DistanceY;
      const bool hasSecondPoint =
          hasFirstPoint ||
          constraintType == sketch::ConstraintType::PointOnLine ||
          constraintType == sketch::ConstraintType::PointOnCircle ||
          constraintType == sketch::ConstraintType::PointOnArc ||
          constraintType == sketch::ConstraintType::Midpoint ||
          constraintType == sketch::ConstraintType::PointOnXAxis ||
          constraintType == sketch::ConstraintType::PointOnYAxis;
      constraints.append(QJsonObject{
          {"id", static_cast<qint64>(constraint.id)},
          {"type", encodedConstraintType(constraint.type)},
          {"typeKey", encodedConstraintTypeKey(constraint.type)},
          {"firstGeometryKind", firstKind},
          {"firstGeometry", firstGeometry},
          {"secondGeometryKind", secondKind},
          {"secondGeometry", secondGeometry},
          {"firstPointLine", firstPointLine},
          {"firstPointStart", static_cast<bool>(constraint.firstPoint.start)},
          {"firstPointCircle", firstPointCircle},
          {"firstPointArc", firstPointArc},
          {"firstPointBezier", firstPointBezier},
          {"firstPointBezierPoint",
           static_cast<int>(constraint.firstPoint.bezierPoint)},
          {"firstPointElementCenter",
           static_cast<qint64>(constraint.firstPoint.elementCenterId)},
          {"firstPointOrigin",
           hasFirstPoint && static_cast<bool>(constraint.firstPoint.origin)},
          {"secondPointLine", secondPointLine},
          {"secondPointStart", static_cast<bool>(constraint.secondPoint.start)},
          {"secondPointCircle", secondPointCircle},
          {"secondPointArc", secondPointArc},
          {"secondPointBezier", secondPointBezier},
          {"secondPointBezierPoint",
           static_cast<int>(constraint.secondPoint.bezierPoint)},
          {"secondPointElementCenter",
           static_cast<qint64>(constraint.secondPoint.elementCenterId)},
          {"secondPointOrigin",
           hasSecondPoint && static_cast<bool>(constraint.secondPoint.origin)},
          {"value", constraint.mode == sketch::ConstraintMode::Reference
                        ? measuredConstraintValue(saved.geometry, constraint)
                        : static_cast<double>(constraint.value)}});
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

static bool serializedProjectRoot(const QString& path, const ProjectData& data,
                                  QJsonObject* result, QString* error) {
  return serializedProjectRootWithMetadata(
      QFileInfo(path).completeBaseName(),
      QDateTime::currentDateTimeUtc().toString(Qt::ISODate), data, result,
      error);
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
  switch (staged.payloadKind) {
    case ProjectPayloadKind::LegacyPartV1:
    case ProjectPayloadKind::ParametricPartV2:
    case ProjectPayloadKind::ParametricPartV3:
    case ProjectPayloadKind::ParametricPartV4:
      return true;
    case ProjectPayloadKind::None:
      setError(error,
               QString::fromUtf8("Проект не содержит поддерживаемых данных детали."));
      return false;
  }
  setError(error, QString::fromUtf8("Неизвестный вид данных проекта."));
  return false;
}

enum class ProjectDocumentEncodingDialect { V3, V4 };

struct ProjectDocumentEncodingStats {
  std::size_t topologyReferenceCount{};
  std::size_t provenanceRecordCount{};
  std::size_t localNameUtf8Bytes{};
};

static bool checkedEncodingAdd(std::size_t delta, std::size_t limit,
                               std::size_t* value) {
  if (!value || *value > limit || delta > limit - *value) return false;
  *value += delta;
  return true;
}

static bool encodeCurrentPartRoot(
    const QString& name, const QString& createdAt, const Document& document,
    ProjectDocumentEncodingDialect dialect, QJsonObject* result,
    ProjectDocumentEncodingStats* resultStats, QString* error,
    detail::ProjectFormatV4DocumentEncodeStatus* encodeStatus = nullptr) {
  if (encodeStatus)
    *encodeStatus = detail::ProjectFormatV4DocumentEncodeStatus::Invalid;
  if (!result) {
    setError(error, QString::fromUtf8("Не задано хранилище для проекта."));
    return false;
  }
  ProjectData legacy;
  legacy.box = document.box();
  for (const auto& item : document.sketches()) {
    legacy.sketches.push_back({item.geometry, QStringLiteral("XY")});
    if (!sketch::validateSketchSchema(legacy.sketches.back().geometry)) {
      setError(error,
               QString::fromUtf8(
                   "Эскиз документа нарушает typed schema contract."));
      return false;
    }
  }
  QJsonObject root;
  if (!serializedProjectRootWithMetadata(name, createdAt, legacy, &root,
                                         error))
    return false;
  root["version"] =
      dialect == ProjectDocumentEncodingDialect::V4 ? 4 : 3;
  if (dialect == ProjectDocumentEncodingDialect::V4)
    root["capabilities"] = QJsonArray{QStringLiteral("persistent-topology-v2")};
  auto persistedSketches = root.value("sketches").toArray();
  for (qsizetype position = 0; position < persistedSketches.size();
       ++position) {
    auto persistedSketch = persistedSketches[position].toObject();
    persistedSketch["sketchId"] = static_cast<qint64>(
        document.sketches()[static_cast<std::size_t>(position)].id);
    persistedSketches[position] = persistedSketch;
  }
  root["sketches"] = persistedSketches;

  ProjectDocumentEncodingStats stats;
  std::size_t featureTopologyReferenceCount = 0;
  const auto accountReference =
      [&](const ProjectFormatV4ReferenceEncodeResult& encoded,
          QJsonObject* target) {
        if (!target || !encoded.succeeded()) {
          setError(error, encoded.error.isEmpty()
                              ? QString::fromUtf8(
                                    "Не удалось кодировать топологическую ссылку.")
                              : encoded.error);
          return false;
        }
        if (!checkedEncodingAdd(
                1, static_cast<std::size_t>(
                       ProjectFile::kMaximumFeatureTopologyReferences),
                &featureTopologyReferenceCount) ||
            !checkedEncodingAdd(
                1, static_cast<std::size_t>(
                       ProjectFile::kMaximumDocumentTopologyReferences),
                &stats.topologyReferenceCount) ||
            !checkedEncodingAdd(
                encoded.provenanceRecordCount,
                kProjectFormatV4MaximumProvenanceRecords,
                &stats.provenanceRecordCount) ||
            !checkedEncodingAdd(encoded.localNameUtf8Bytes,
                                kProjectFormatV4MaximumLocalNameUtf8Bytes,
                                &stats.localNameUtf8Bytes)) {
          setError(error,
                   QString::fromUtf8(
                       "Топологические ссылки документа превышают лимит."));
          return false;
        }
        *target = QJsonObject{{"reference", *encoded.reference}};
        return true;
      };
  const auto encodeFace = [&](const FaceReference& face,
                              QJsonObject* target) {
    if (dialect == ProjectDocumentEncodingDialect::V4)
      return accountReference(encodeProjectFormatV4TopologyReference(face),
                              target);
    if (!target) return false;
    *target = savedFaceReference(face);
    return true;
  };
  const auto encodeEdge = [&](const EdgeReference& edge,
                              QJsonObject* target) {
    if (dialect == ProjectDocumentEncodingDialect::V4)
      return accountReference(encodeProjectFormatV4TopologyReference(edge),
                              target);
    if (!target) return false;
    const auto encoded = savedEdgeReference(edge);
    if (!encoded) {
      setError(error,
               QString::fromUtf8(
                   "Сохранение ссылок v2 на ребра пока не поддерживается."));
      return false;
    }
    *target = *encoded;
    return true;
  };

  QJsonArray sketches;
  for (const auto& item : document.sketches()) {
    featureTopologyReferenceCount = 0;
    QJsonObject support{{"type", static_cast<int>(item.support.type)}};
    if (item.support.type == SketchSupportType::Face) {
      if (dialect == ProjectDocumentEncodingDialect::V4) {
        QJsonObject encoded;
        if (!encodeFace(item.support.face, &encoded)) return false;
        support["reference"] = encoded.value("reference");
      } else {
        const FaceReference& persistedFace =
            item.effectiveSupportAuthoredFace &&
                    *item.effectiveSupportAuthoredFace == item.support.face &&
                    item.effectiveSupportFace
                ? *item.effectiveSupportFace
                : item.support.face;
        const QJsonObject encoded = savedFaceReference(persistedFace);
        for (auto it = encoded.constBegin(); it != encoded.constEnd(); ++it)
          support[it.key()] = it.value();
      }
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
      featureTopologyReferenceCount = 0;
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
      if (!codec->encode(
              *feature, saved,
              FeatureEncodeContext{document, error, encodeFace, encodeEdge}))
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
         vector3(image.placement.xDirection.x, image.placement.xDirection.y,
                 image.placement.xDirection.z)},
        {"yDirection",
         vector3(image.placement.yDirection.x, image.placement.yDirection.y,
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

  if (!upgradeSketchSchemaToV3(&root, legacy, error)) return false;
  if (dialect == ProjectDocumentEncodingDialect::V3) {
    if (!validateV2Root(root, legacy, error)) return false;
  } else {
    const QByteArray compact = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (compact.size() > ProjectFile::kMaximumFileBytes) {
      if (encodeStatus) {
        *encodeStatus =
            detail::ProjectFormatV4DocumentEncodeStatus::PayloadTooLarge;
      }
      setError(error,
               QString::fromUtf8("Проект превышает допустимый размер файла."));
      return false;
    }
    auto envelope = scanProjectFormatV4Envelope(compact);
    if (!envelope.hasCandidate()) {
      setError(error, QStringLiteral("v4 envelope self-validation failed: %1")
                          .arg(envelope.error));
      return false;
    }
    auto payload = scanProjectFormatV4Payload(std::move(*envelope.candidate));
    if (!payload.hasCandidate()) {
      setError(error, QStringLiteral("v4 payload self-validation failed: %1")
                          .arg(payload.error));
      return false;
    }
    auto model = validateProjectFormatV4Model(std::move(*payload.candidate));
    if (model.status != ProjectFormatV4ModelValidationStatus::Candidate ||
        !model.candidate) {
      setError(error, QStringLiteral("v4 model self-validation failed: %1")
                          .arg(model.error));
      return false;
    }
    const auto& topology = model.candidate->legacySketch.topology;
    if (topology.payload.envelope.authoredRoot != root ||
        topology.topologyReferences.size() != stats.topologyReferenceCount ||
        topology.provenanceRecordCount != stats.provenanceRecordCount ||
        topology.localNameUtf8Bytes != stats.localNameUtf8Bytes ||
        !std::all_of(topology.topologyReferences.begin(),
                     topology.topologyReferences.end(),
                     [](const auto& reference) { return reference.has_value(); })) {
      setError(error,
               QString::fromUtf8(
                   "v4 self-validation returned inconsistent accounting."));
      return false;
    }
  }
  *result = std::move(root);
  if (resultStats) *resultStats = stats;
  if (encodeStatus)
    *encodeStatus = detail::ProjectFormatV4DocumentEncodeStatus::Valid;
  return true;
}

static bool encodeCurrentPartV3Root(const QString& path,
                                    const Document& document,
                                    QJsonObject* result, QString* error) {
  return encodeCurrentPartRoot(
      QFileInfo(path).completeBaseName(),
      QDateTime::currentDateTimeUtc().toString(Qt::ISODate), document,
      ProjectDocumentEncodingDialect::V3, result, nullptr, error);
}

detail::ProjectFormatV4DocumentEncodeResult
detail::encodeProjectFormatV4DocumentRoot(
    const Document& document,
    const ProjectFormatV4DocumentMetadata& metadata) {
  ProjectFormatV4DocumentEncodeResult encoded;
  QJsonObject root;
  ProjectDocumentEncodingStats stats;
  QString error;
  if (!encodeCurrentPartRoot(metadata.name, metadata.createdAt, document,
                             ProjectDocumentEncodingDialect::V4, &root,
                             &stats, &error, &encoded.status)) {
    encoded.error = error.isEmpty()
                        ? QStringLiteral("v4 document encoding failed")
                        : std::move(error);
    return encoded;
  }
  encoded.root = std::move(root);
  encoded.topologyReferenceCount = stats.topologyReferenceCount;
  encoded.provenanceRecordCount = stats.provenanceRecordCount;
  encoded.localNameUtf8Bytes = stats.localNameUtf8Bytes;
  return encoded;
}

static bool containsUnsupportedV2TopologyReference(const Document& document) {
  for (const auto& sketch : document.sketches())
    if (sketch.support.type == SketchSupportType::Face &&
        sketch.support.face.isV2())
      return true;

  for (const auto& body : document.bodies())
    for (const auto& feature : body.features()) {
      if (const auto* extrude =
              dynamic_cast<const ExtrudeFeature*>(feature.get())) {
        if (const auto reference = extrude->faceReference();
            reference && reference->isV2())
          return true;
      } else if (const auto* fillet =
                     dynamic_cast<const FilletFeature*>(feature.get())) {
        if (std::any_of(fillet->edges().begin(), fillet->edges().end(),
                        [](const EdgeReference& edge) { return edge.isV2(); }))
          return true;
      } else if (const auto* chamfer =
                     dynamic_cast<const ChamferFeature*>(feature.get())) {
        if (std::any_of(chamfer->edges().begin(), chamfer->edges().end(),
                        [](const EdgeReference& edge) { return edge.isV2(); }))
          return true;
      } else if (const auto* mirror =
                     dynamic_cast<const MirrorFeature*>(feature.get())) {
        if (mirror->planeFace() && mirror->planeFace()->isV2()) return true;
      } else if (const auto* shell =
                     dynamic_cast<const ShellFeature*>(feature.get())) {
        if (std::any_of(shell->removedFaces().begin(),
                        shell->removedFaces().end(),
                        [](const FaceReference& face) { return face.isV2(); }))
          return true;
      } else if (const auto* draft =
                     dynamic_cast<const DraftFeature*>(feature.get())) {
        if (std::any_of(draft->draftedFaces().begin(),
                        draft->draftedFaces().end(),
                        [](const FaceReference& face) { return face.isV2(); }) ||
            (draft->neutralPlane().face &&
             draft->neutralPlane().face->isV2()) ||
            (draft->rotationEdge() && draft->rotationEdge()->isV2()))
          return true;
      }
    }
  return false;
}

bool ProjectFile::saveDocument(const QString& path, const Document& document,
                               QString* error) {
  const auto saveOperation = [&]() -> bool {
    // v3 has only the legacy geometric topology-reference schema. Reject the
    // whole document before constructing QSaveFile so an existing target is
    // left byte-for-byte untouched instead of silently downgrading v2.
    if (containsUnsupportedV2TopologyReference(document)) {
      setError(error, QString::fromUtf8(
                          "Формат проекта v3 не поддерживает устойчивые ссылки на грани v2."));
      return false;
    }
    QJsonObject root;
    if (!encodeCurrentPartV3Root(path, document, &root, error)) return false;
    ProjectData restoredLegacy;
    if (!loadV3Root(root, &restoredLegacy, error))
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

bool ProjectFile::saveDocument(const QString& path, const Document& document,
                               ProjectPersistenceState persistenceState,
                               QString* error) {
  switch (persistenceState.format) {
    case ProjectPersistenceFormat::V3:
      return saveDocument(path, document, error);
    case ProjectPersistenceFormat::V4: {
      const auto written =
          detail::writeProjectFormatV4DocumentAtomically(path, document);
      if (!written.succeeded()) {
        setError(error, written.error.isEmpty()
                            ? QString::fromUtf8(
                                  "Не удалось атомарно сохранить проект v4.")
                            : written.error);
        return false;
      }
      return true;
    }
    case ProjectPersistenceFormat::V5:
      setError(error,
               QString::fromUtf8(
                   "Формат v5 требует ProductDocument с identity."));
      return false;
  }
  setError(error, QString::fromUtf8("Неизвестный формат сохранения проекта."));
  return false;
}

static bool populateDocumentRoot(
    const QJsonObject& root, const ProjectData& legacy, Document* document,
    const MaterializedFaceReference& faceReference,
    const MaterializedEdgeReference& edgeReference, QString* error) {
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
      const QJsonValue authored = support.contains("reference")
                                      ? support.value("reference")
                                      : QJsonValue{support};
      if (!faceReference(
              authored,
              QStringLiteral("$.model.sketches[%1].support.reference")
                  .arg(index),
              &sketch.support.face))
        return false;
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

  const auto savedBodies = model.value("bodies").toArray();
  for (qsizetype bodyIndex = 0; bodyIndex < savedBodies.size(); ++bodyIndex) {
    const auto bodyValue = savedBodies[bodyIndex];
    const auto savedBody = bodyValue.toObject();
    auto& body = loaded.addBody(
        static_cast<BodyId>(savedBody.value("id").toInteger()),
        savedBody.value("name").toString().toStdString());
    body.setVisible(savedBody.value("visible").toBool(true));
    const auto savedFeatures = savedBody.value("features").toArray();
    for (qsizetype featureIndex = 0; featureIndex < savedFeatures.size();
         ++featureIndex) {
      const auto featureValue = savedFeatures[featureIndex];
      const auto saved = featureValue.toObject();
      const auto id = static_cast<FeatureId>(saved.value("id").toInteger());
      const auto name = saved.value("name").toString().toStdString();
      const auto type = saved.value("type").toString();
      const auto* codec = findFeatureCodec(QStringView{type});
      if (!codec) {
        setError(error, QString::fromUtf8("Неизвестный тип фичи: ") + type);
        return false;
      }
      FeatureDecodeContext context{
          loaded,
          error,
          restoredSketchLineId,
          faceReference,
          edgeReference,
          QStringLiteral("$.model.bodies[%1].features[%2]")
              .arg(bodyIndex)
              .arg(featureIndex)};
      if (!codec->decode(saved, id, name, body, context)) return false;
    }
  }
  *document = std::move(loaded);
  return true;
}

static bool loadDocumentRoot(const QJsonObject& root,
                             const ProjectData& legacy, Document* document,
                             QString* error) {
  const auto faceReference = [](const QJsonValue& value, const QString&,
                                FaceReference* result) {
    if (!result) return false;
    *result = loadedFaceReference(value);
    return true;
  };
  const auto edgeReference = [](const QJsonValue& value, const QString&,
                                EdgeReference* result) {
    if (!result) return false;
    *result = loadedEdgeReference(value);
    return true;
  };
  Document loaded;
  if (!populateDocumentRoot(root, legacy, &loaded, faceReference,
                            edgeReference, error))
    return false;
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

static bool constraintMatchesLegacyDimension(
    const sketch::Constraint& constraint,
    const sketch::Dimension& dimension) {
  const auto samePoints = [&](const sketch::PointReference& first,
                              const sketch::PointReference& second) {
    return (constraint.firstPoint == first &&
            constraint.secondPoint == second) ||
           (constraint.firstPoint == second &&
            constraint.secondPoint == first);
  };
  const auto sameGeometryPair = [&](sketch::GeometryId first,
                                    sketch::GeometryId second) {
    return (constraint.firstGeometry == first &&
            constraint.secondGeometry == second) ||
           (constraint.firstGeometry == second &&
            constraint.secondGeometry == first);
  };
  switch (dimension.kind) {
    case sketch::DimensionKind::LineLength:
      return constraint.type == sketch::ConstraintType::Length &&
             constraint.firstGeometry == dimension.geometryId;
    case sketch::DimensionKind::CircleDiameter:
      return constraint.type == sketch::ConstraintType::Diameter &&
             constraint.firstGeometry == dimension.geometryId;
    case sketch::DimensionKind::CircleRadius:
      return constraint.type == sketch::ConstraintType::Radius &&
             constraint.firstGeometry == dimension.geometryId;
    case sketch::DimensionKind::PointDistance:
      return constraint.type == sketch::ConstraintType::Distance &&
             samePoints(dimension.firstPoint, dimension.secondPoint);
    case sketch::DimensionKind::PointDistanceX:
      return constraint.type == sketch::ConstraintType::DistanceX &&
             samePoints(dimension.firstPoint, dimension.secondPoint);
    case sketch::DimensionKind::PointDistanceY:
      return constraint.type == sketch::ConstraintType::DistanceY &&
             samePoints(dimension.firstPoint, dimension.secondPoint);
    case sketch::DimensionKind::LineAngle:
      return constraint.type == sketch::ConstraintType::Angle &&
             sameGeometryPair(dimension.geometryId,
                              dimension.secondPoint.lineId);
    case sketch::DimensionKind::LineDistance:
      return constraint.type == sketch::ConstraintType::LineDistance &&
             sameGeometryPair(dimension.geometryId,
                              dimension.secondPoint.lineId);
  }
  return false;
}

static sketch::Constraint referenceConstraintForLegacyDimension(
    const sketch::Dimension& dimension, sketch::ConstraintId id) {
  sketch::Constraint constraint;
  constraint.id = id;
  constraint.mode = sketch::ConstraintMode::Reference;
  switch (dimension.kind) {
    case sketch::DimensionKind::LineLength:
      constraint.type = sketch::ConstraintType::Length;
      constraint.firstGeometry = dimension.geometryId;
      break;
    case sketch::DimensionKind::CircleDiameter:
      constraint.type = sketch::ConstraintType::Diameter;
      constraint.firstGeometry = dimension.geometryId;
      break;
    case sketch::DimensionKind::CircleRadius:
      constraint.type = sketch::ConstraintType::Radius;
      constraint.firstGeometry = dimension.geometryId;
      break;
    case sketch::DimensionKind::PointDistance:
      constraint.type = sketch::ConstraintType::Distance;
      constraint.firstPoint = dimension.firstPoint;
      constraint.secondPoint = dimension.secondPoint;
      break;
    case sketch::DimensionKind::PointDistanceX:
      constraint.type = sketch::ConstraintType::DistanceX;
      constraint.firstPoint = dimension.firstPoint;
      constraint.secondPoint = dimension.secondPoint;
      break;
    case sketch::DimensionKind::PointDistanceY:
      constraint.type = sketch::ConstraintType::DistanceY;
      constraint.firstPoint = dimension.firstPoint;
      constraint.secondPoint = dimension.secondPoint;
      break;
    case sketch::DimensionKind::LineAngle:
      constraint.type = sketch::ConstraintType::Angle;
      constraint.firstGeometry = dimension.geometryId;
      constraint.secondGeometry = dimension.secondPoint.lineId;
      break;
    case sketch::DimensionKind::LineDistance:
      constraint.type = sketch::ConstraintType::LineDistance;
      constraint.firstGeometry = dimension.geometryId;
      constraint.secondGeometry = dimension.secondPoint.lineId;
      break;
  }
  return constraint;
}

static bool migrateLegacyDimensionLinks(
    sketch::Sketch* geometry,
    std::vector<sketch::Dimension> legacyDimensions,
    QString* error) {
  if (!geometry) return false;
  if (!geometry->dimensions().empty()) {
    setError(error,
             QString::fromUtf8(
                 "Legacy migration получила уже материализованные annotations."));
    return false;
  }
  std::vector<sketch::Constraint> constraints = geometry->constraints();
  std::unordered_set<sketch::ConstraintId> linked;
  sketch::ConstraintId nextId = 1;
  for (const auto& constraint : constraints)
    nextId = std::max(nextId, constraint.id + 1);

  std::vector<sketch::ConstraintId> links;
  links.reserve(legacyDimensions.size());
  for (const auto& dimension : legacyDimensions) {
    sketch::ConstraintId selected = sketch::kInvalidConstraintId;
    if (dimension.constraintId != sketch::kInvalidConstraintId) {
      const auto exact = std::find_if(
          constraints.begin(), constraints.end(),
          [&](const sketch::Constraint& constraint) {
            return constraint.id == dimension.constraintId;
          });
      if (exact == constraints.end() || linked.contains(dimension.constraintId)) {
        setError(error,
                 QString::fromUtf8(
                     "Annotation содержит устаревшую или повторную точную связь."));
        return false;
      }
      selected = dimension.constraintId;
    }

    std::vector<sketch::ConstraintId> candidates;
    if (selected == sketch::kInvalidConstraintId) {
      for (const auto& constraint : constraints) {
        if (!linked.contains(constraint.id) &&
            constraintMatchesLegacyDimension(constraint, dimension))
          candidates.push_back(constraint.id);
      }
    }

    if (selected != sketch::kInvalidConstraintId) {
      // A v3/model annotation already has an authoritative link.  Never
      // rediscover it by shape, even when another constraint looks identical.
    } else if (candidates.size() == 1) {
      selected = candidates.front();
    } else {
      while (nextId == sketch::kInvalidConstraintId ||
             std::any_of(constraints.begin(), constraints.end(),
                         [nextId](const sketch::Constraint& constraint) {
                           return constraint.id == nextId;
                         }))
        ++nextId;
      selected = nextId++;
      constraints.push_back(
          referenceConstraintForLegacyDimension(dimension, selected));
    }
    linked.insert(selected);
    links.push_back(selected);
  }

  if (!geometry->restoreConstraints(std::move(constraints))) {
    setError(error,
             QString::fromUtf8(
                 "Не удалось создать однозначные связи размеров при миграции."));
    return false;
  }
  for (std::size_t index = 0; index < links.size(); ++index) {
    legacyDimensions[index].constraintId = links[index];
    if (!geometry->storeDimension(legacyDimensions[index])) {
      setError(error,
               QString::fromUtf8(
                   "Не удалось атомарно восстановить annotation при миграции."));
      return false;
    }
  }
  if (sketch::validateSketchSchema(*geometry)) return true;
  setError(error,
           QString::fromUtf8(
               "Миграция annotations нарушает typed schema contract."));
  return false;
}

static QJsonObject encodedPointReferenceV3(
    const sketch::PointReference& reference) {
  return std::visit([](const auto& source) -> QJsonObject {
    using T = std::decay_t<decltype(source)>;
    if constexpr (std::is_same_v<T, sketch::DatumOriginReference>) {
      return {{"source", "datum-origin"}};
    } else if constexpr (std::is_same_v<T,
                                        sketch::LineEndpointReference>) {
      return {{"source", "line-endpoint"},
              {"geometryId", static_cast<qint64>(source.lineId)},
              {"endpoint", source.endpoint == sketch::Endpoint::Start
                               ? "start" : "end"}};
    } else if constexpr (std::is_same_v<T,
                                        sketch::CircleCenterReference>) {
      return {{"source", "circle-center"},
              {"geometryId", static_cast<qint64>(source.circleId)}};
    } else if constexpr (std::is_same_v<T,
                                        sketch::ArcEndpointReference>) {
      return {{"source", "arc-endpoint"},
              {"geometryId", static_cast<qint64>(source.arcId)},
              {"endpoint", source.endpoint == sketch::Endpoint::Start
                               ? "start" : "end"}};
    } else if constexpr (std::is_same_v<T,
                                        sketch::BezierPointReference>) {
      return {{"source", "bezier-point"},
              {"geometryId", static_cast<qint64>(source.bezierId)},
              {"pointIndex", static_cast<int>(source.pointIndex)}};
    } else {
      return {{"source", "composite-center"},
              {"elementId", static_cast<qint64>(source.elementId)}};
    }
  }, reference.payload());
}

static bool decodedPointReferenceV3(const QJsonValue& value,
                                    sketch::PointReference* result,
                                    QString* error) {
  if (!result || !value.isObject())
    return invalid(error,
                   QString::fromUtf8("point reference v3 должен быть объектом."));
  const auto object = value.toObject();
  if (!object.value("source").isString())
    return invalid(error,
                   QString::fromUtf8("point reference v3 не содержит source."));
  const auto source = object.value("source").toString();
  qint64 id = 0;
  if (source == QStringLiteral("datum-origin")) {
    if (object.size() != 1)
      return invalid(error,
                     QString::fromUtf8("datum-origin содержит лишние поля."));
    *result = sketch::PointReference{sketch::DatumOriginReference{}};
    return true;
  }
  if (source == QStringLiteral("composite-center")) {
    if (object.size() != 2 ||
        !integerNumber(object.value("elementId"), QStringLiteral("elementId"),
                       1, ProjectFile::kMaximumPersistedId, &id, error))
      return false;
    *result = sketch::PointReference{
        sketch::CompositeCenterReference{static_cast<std::uint64_t>(id)}};
    return true;
  }
  if (!integerNumber(object.value("geometryId"),
                     QStringLiteral("geometryId"), 1,
                     ProjectFile::kMaximumPersistedId, &id, error))
    return false;
  if (source == QStringLiteral("circle-center")) {
    if (object.size() != 2)
      return invalid(error,
                     QString::fromUtf8("circle-center содержит лишние поля."));
    *result = sketch::PointReference{
        sketch::CircleCenterReference{static_cast<sketch::GeometryId>(id)}};
    return true;
  }
  if (source == QStringLiteral("bezier-point")) {
    qint64 pointIndex = -1;
    if (object.size() != 3 ||
        !integerNumber(object.value("pointIndex"),
                       QStringLiteral("pointIndex"), 0, 3, &pointIndex,
                       error))
      return false;
    *result = sketch::PointReference{sketch::BezierPointReference{
        static_cast<sketch::GeometryId>(id),
        static_cast<std::uint8_t>(pointIndex)}};
    return true;
  }
  if (source == QStringLiteral("line-endpoint") ||
      source == QStringLiteral("arc-endpoint")) {
    if (object.size() != 3 || !object.value("endpoint").isString())
      return invalid(error,
                     QString::fromUtf8("endpoint reference v3 повреждён."));
    const auto endpointText = object.value("endpoint").toString();
    if (endpointText != QStringLiteral("start") &&
        endpointText != QStringLiteral("end"))
      return invalid(error,
                     QString::fromUtf8("неизвестный endpoint reference v3."));
    const auto endpoint = endpointText == QStringLiteral("start")
        ? sketch::Endpoint::Start : sketch::Endpoint::End;
    if (source == QStringLiteral("line-endpoint"))
      *result = sketch::PointReference{sketch::LineEndpointReference{
          static_cast<sketch::GeometryId>(id), endpoint}};
    else
      *result = sketch::PointReference{sketch::ArcEndpointReference{
          static_cast<sketch::GeometryId>(id), endpoint}};
    return true;
  }
  return invalid(error,
                 QString::fromUtf8("неизвестный source point reference v3."));
}

static bool dimensionalConstraintType(sketch::ConstraintType type) {
  return type == sketch::ConstraintType::Distance ||
         type == sketch::ConstraintType::Length ||
         type == sketch::ConstraintType::Radius ||
         type == sketch::ConstraintType::Diameter ||
         type == sketch::ConstraintType::Angle ||
         type == sketch::ConstraintType::DistanceX ||
         type == sketch::ConstraintType::DistanceY ||
         type == sketch::ConstraintType::LineDistance;
}

static QJsonObject encodedConstraintV3(const sketch::Constraint& constraint) {
  const auto type = static_cast<sketch::ConstraintType>(constraint.type);
  QJsonObject payload{{"type", encodedConstraintType(type)},
                      {"typeKey", encodedConstraintTypeKey(type)}};
  const auto firstGeometry = static_cast<sketch::GeometryId>(
      constraint.firstGeometry);
  const auto secondGeometry = static_cast<sketch::GeometryId>(
      constraint.secondGeometry);
  switch (type) {
    case sketch::ConstraintType::Horizontal:
    case sketch::ConstraintType::Vertical:
    case sketch::ConstraintType::Length:
    case sketch::ConstraintType::Radius:
    case sketch::ConstraintType::Diameter:
    case sketch::ConstraintType::Lock:
      payload["geometryId"] = static_cast<qint64>(firstGeometry);
      break;
    case sketch::ConstraintType::Parallel:
    case sketch::ConstraintType::Perpendicular:
    case sketch::ConstraintType::Equal:
    case sketch::ConstraintType::Angle:
    case sketch::ConstraintType::Tangent:
    case sketch::ConstraintType::LineDistance:
      payload["firstGeometryId"] = static_cast<qint64>(firstGeometry);
      payload["secondGeometryId"] = static_cast<qint64>(secondGeometry);
      break;
    case sketch::ConstraintType::Coincident:
    case sketch::ConstraintType::Distance:
    case sketch::ConstraintType::DistanceX:
    case sketch::ConstraintType::DistanceY:
      payload["firstPoint"] = encodedPointReferenceV3(
          static_cast<sketch::PointReference>(constraint.firstPoint));
      payload["secondPoint"] = encodedPointReferenceV3(
          static_cast<sketch::PointReference>(constraint.secondPoint));
      break;
    case sketch::ConstraintType::PointOnLine:
    case sketch::ConstraintType::PointOnCircle:
    case sketch::ConstraintType::PointOnArc:
    case sketch::ConstraintType::Midpoint:
      payload["geometryId"] = static_cast<qint64>(firstGeometry);
      payload["point"] = encodedPointReferenceV3(
          static_cast<sketch::PointReference>(constraint.secondPoint));
      break;
    case sketch::ConstraintType::PointOnXAxis:
    case sketch::ConstraintType::PointOnYAxis:
      payload["point"] = encodedPointReferenceV3(
          static_cast<sketch::PointReference>(constraint.secondPoint));
      break;
  }
  if (dimensionalConstraintType(type) &&
      constraint.mode != sketch::ConstraintMode::Reference)
    payload["value"] = static_cast<double>(constraint.value);
  const char* mode = constraint.mode == sketch::ConstraintMode::Driving
      ? "driving"
      : constraint.mode == sketch::ConstraintMode::Reference
            ? "reference" : "disabled";
  return {{"id", static_cast<qint64>(constraint.id)},
          {"mode", mode}, {"payload", payload}};
}

static bool decodedConstraintV3(const QJsonValue& value,
                                sketch::Constraint* result,
                                QString* error, bool* unsupported) {
  if (!result || !value.isObject())
    return invalid(error, QString::fromUtf8("constraint v3 должен быть объектом."));
  const auto object = value.toObject();
  if (!exactObjectFields(object, {"id", "mode", "payload"},
                         QStringLiteral("constraint v3"), error))
    return false;
  qint64 id = 0;
  if (!integerNumber(object.value("id"), QStringLiteral("constraint.id"), 1,
                     ProjectFile::kMaximumPersistedId, &id, error) ||
      !object.value("mode").isString() ||
      !object.value("payload").isObject())
    return invalid(error, QString::fromUtf8("оболочка constraint v3 повреждена."));
  const auto modeText = object.value("mode").toString();
  sketch::ConstraintMode mode{};
  if (modeText == QStringLiteral("driving"))
    mode = sketch::ConstraintMode::Driving;
  else if (modeText == QStringLiteral("reference"))
    mode = sketch::ConstraintMode::Reference;
  else if (modeText == QStringLiteral("disabled"))
    mode = sketch::ConstraintMode::Disabled;
  else
    return invalid(error, QString::fromUtf8("неизвестный constraint mode v3."));
  const auto payload = object.value("payload").toObject();
  sketch::ConstraintType type{};
  if (!decodeConstraintType(payload, ConstraintTypeEncoding::StableNameV1,
                            &type, unsupported, error))
    return false;

  sketch::Constraint constraint;
  constraint.id = static_cast<sketch::ConstraintId>(id);
  constraint.type = type;
  constraint.mode = mode;
  const auto geometryId = [&](const char* key,
                              sketch::GeometryId* target) {
    qint64 decoded = 0;
    if (!integerNumber(payload.value(QLatin1String(key)),
                       QString::fromLatin1(key), 1,
                       ProjectFile::kMaximumPersistedId, &decoded, error))
      return false;
    *target = static_cast<sketch::GeometryId>(decoded);
    return true;
  };
  sketch::GeometryId firstGeometry = sketch::kInvalidGeometryId;
  sketch::GeometryId secondGeometry = sketch::kInvalidGeometryId;
  sketch::PointReference firstPoint;
  sketch::PointReference secondPoint;
  switch (type) {
    case sketch::ConstraintType::Horizontal:
    case sketch::ConstraintType::Vertical:
    case sketch::ConstraintType::Length:
    case sketch::ConstraintType::Radius:
    case sketch::ConstraintType::Diameter:
    case sketch::ConstraintType::Lock:
      if (!geometryId("geometryId", &firstGeometry)) return false;
      constraint.firstGeometry = firstGeometry;
      break;
    case sketch::ConstraintType::Parallel:
    case sketch::ConstraintType::Perpendicular:
    case sketch::ConstraintType::Equal:
    case sketch::ConstraintType::Angle:
    case sketch::ConstraintType::Tangent:
    case sketch::ConstraintType::LineDistance:
      if (!geometryId("firstGeometryId", &firstGeometry) ||
          !geometryId("secondGeometryId", &secondGeometry))
        return false;
      constraint.firstGeometry = firstGeometry;
      constraint.secondGeometry = secondGeometry;
      break;
    case sketch::ConstraintType::Coincident:
    case sketch::ConstraintType::Distance:
    case sketch::ConstraintType::DistanceX:
    case sketch::ConstraintType::DistanceY:
      if (!decodedPointReferenceV3(payload.value("firstPoint"), &firstPoint,
                                   error) ||
          !decodedPointReferenceV3(payload.value("secondPoint"), &secondPoint,
                                   error))
        return false;
      constraint.firstPoint = firstPoint;
      constraint.secondPoint = secondPoint;
      break;
    case sketch::ConstraintType::PointOnLine:
    case sketch::ConstraintType::PointOnCircle:
    case sketch::ConstraintType::PointOnArc:
    case sketch::ConstraintType::Midpoint:
      if (!geometryId("geometryId", &firstGeometry) ||
          !decodedPointReferenceV3(payload.value("point"), &secondPoint,
                                   error))
        return false;
      constraint.firstGeometry = firstGeometry;
      constraint.secondPoint = secondPoint;
      break;
    case sketch::ConstraintType::PointOnXAxis:
    case sketch::ConstraintType::PointOnYAxis:
      if (!decodedPointReferenceV3(payload.value("point"), &secondPoint,
                                   error))
        return false;
      constraint.secondPoint = secondPoint;
      break;
  }
  if (dimensionalConstraintType(type)) {
    if (mode == sketch::ConstraintMode::Reference) {
      if (payload.contains("value"))
        return invalid(error,
                       QString::fromUtf8("reference constraint содержит value."));
    } else {
      if (!finiteNumber(payload.value("value"), QStringLiteral("value"),
                        std::numeric_limits<double>::min(),
                        kMaximumModelLengthMm, error))
        return false;
      constraint.value = payload.value("value").toDouble();
    }
  } else if (payload.contains("value")) {
    return invalid(error,
                   QString::fromUtf8("нерaзмерный constraint содержит value."));
  }
  const bool carriesValue = dimensionalConstraintType(type) &&
                            mode != sketch::ConstraintMode::Reference;
  const auto exactPayload = [&](std::initializer_list<const char*> fields) {
    const qsizetype expected = static_cast<qsizetype>(fields.size()) + 2 +
                               (carriesValue ? 1 : 0);
    if (payload.size() != expected || !payload.contains("type") ||
        !payload.contains("typeKey") ||
        (carriesValue && !payload.contains("value")))
      return invalid(error,
                     QString::fromUtf8("payload constraint v3 содержит лишние или отсутствующие поля."));
    for (const char* field : fields)
      if (!payload.contains(QLatin1String(field)))
        return invalid(error,
                       QString::fromUtf8("payload constraint v3 не содержит поле %1.")
                           .arg(QString::fromLatin1(field)));
    return true;
  };
  bool exact = false;
  switch (type) {
    case sketch::ConstraintType::Horizontal:
    case sketch::ConstraintType::Vertical:
    case sketch::ConstraintType::Length:
    case sketch::ConstraintType::Radius:
    case sketch::ConstraintType::Diameter:
    case sketch::ConstraintType::Lock:
      exact = exactPayload({"geometryId"});
      break;
    case sketch::ConstraintType::Parallel:
    case sketch::ConstraintType::Perpendicular:
    case sketch::ConstraintType::Equal:
    case sketch::ConstraintType::Angle:
    case sketch::ConstraintType::Tangent:
    case sketch::ConstraintType::LineDistance:
      exact = exactPayload({"firstGeometryId", "secondGeometryId"});
      break;
    case sketch::ConstraintType::Coincident:
    case sketch::ConstraintType::Distance:
    case sketch::ConstraintType::DistanceX:
    case sketch::ConstraintType::DistanceY:
      exact = exactPayload({"firstPoint", "secondPoint"});
      break;
    case sketch::ConstraintType::PointOnLine:
    case sketch::ConstraintType::PointOnCircle:
    case sketch::ConstraintType::PointOnArc:
    case sketch::ConstraintType::Midpoint:
      exact = exactPayload({"geometryId", "point"});
      break;
    case sketch::ConstraintType::PointOnXAxis:
    case sketch::ConstraintType::PointOnYAxis:
      exact = exactPayload({"point"});
      break;
  }
  if (!exact) return false;
  *result = std::move(constraint);
  return true;
}

static bool upgradeSketchSchemaToV3(QJsonObject* root,
                                    const ProjectData& data,
                                    QString* error) {
  if (!root) return false;
  auto sketches = root->value("sketches").toArray();
  if (sketches.size() != static_cast<qsizetype>(data.sketches.size()))
    return invalid(error, QString::fromUtf8("число sketches v3 не совпадает."));
  for (qsizetype sketchPosition = 0; sketchPosition < sketches.size();
       ++sketchPosition) {
    auto object = sketches[sketchPosition].toObject();
    const auto& geometry =
        data.sketches[static_cast<std::size_t>(sketchPosition)].geometry;
    const auto addIds = [&](const char* key, auto idAt) {
      auto values = object.value(QLatin1String(key)).toArray();
      for (qsizetype index = 0; index < values.size(); ++index) {
        auto item = values[index].toObject();
        item["id"] = static_cast<qint64>(
            idAt(static_cast<std::size_t>(index)));
        values[index] = item;
      }
      object[QLatin1String(key)] = values;
    };
    addIds("lines", [&](std::size_t index) { return geometry.lineId(index); });
    addIds("circles",
           [&](std::size_t index) { return geometry.circleId(index); });
    addIds("arcs", [&](std::size_t index) { return geometry.arcId(index); });
    addIds("beziers",
           [&](std::size_t index) { return geometry.bezierId(index); });

    QJsonArray constraints;
    for (const auto& constraint : geometry.constraints())
      constraints.append(encodedConstraintV3(constraint));
    QJsonArray dimensions;
    for (const auto& dimension : geometry.dimensions()) {
      if (dimension.constraintId == sketch::kInvalidConstraintId ||
          !geometry.constraintIndex(dimension.constraintId))
        return invalid(error,
                       QString::fromUtf8("annotation v3 не связан с constraint."));
      dimensions.append(QJsonObject{
          {"id", static_cast<qint64>(dimension.id)},
          {"constraintId", static_cast<qint64>(dimension.constraintId)},
          {"offset", dimension.offsetMm}, {"angle", dimension.angleRad}});
    }
    object["constraints"] = constraints;
    object["dimensions"] = dimensions;
    object["idAllocators"] = QJsonObject{
        {"nextElementId", static_cast<qint64>(geometry.nextElementId())},
        {"nextGeometryId", static_cast<qint64>(geometry.nextGeometryId())},
        {"nextConstraintId", static_cast<qint64>(geometry.nextConstraintId())},
        {"nextDimensionId", static_cast<qint64>(geometry.nextDimensionId())}};
    object["schema"] = QStringLiteral("typed-sketch-v1");
    sketches[sketchPosition] = object;
  }
  (*root)["sketches"] = sketches;
  return true;
}

static double measuredConstraintValue(const sketch::Sketch& geometry,
                                      const sketch::Constraint& constraint) {
  const auto type = static_cast<sketch::ConstraintType>(constraint.type);
  if (constraint.mode == sketch::ConstraintMode::Driving)
    return static_cast<double>(constraint.value);
  if (type == sketch::ConstraintType::Length) {
    const auto index = geometry.lineIndex(constraint.firstGeometry);
    if (index) {
      const auto& line = geometry.lines()[*index];
      return std::hypot(line.end.xMm - line.start.xMm,
                        line.end.yMm - line.start.yMm);
    }
  } else if (type == sketch::ConstraintType::Diameter ||
             type == sketch::ConstraintType::Radius) {
    const auto index = geometry.circleIndex(constraint.firstGeometry);
    if (index)
      return geometry.circles()[*index].radiusMm *
             (type == sketch::ConstraintType::Diameter ? 2.0 : 1.0);
  } else if (type == sketch::ConstraintType::Distance ||
             type == sketch::ConstraintType::DistanceX ||
             type == sketch::ConstraintType::DistanceY) {
    const auto first = geometry.referencedPoint(constraint.firstPoint);
    const auto second = geometry.referencedPoint(constraint.secondPoint);
    if (first && second) {
      if (type == sketch::ConstraintType::DistanceX)
        return std::abs(second->xMm - first->xMm);
      if (type == sketch::ConstraintType::DistanceY)
        return std::abs(second->yMm - first->yMm);
      return std::hypot(second->xMm - first->xMm,
                        second->yMm - first->yMm);
    }
  } else if (type == sketch::ConstraintType::Angle) {
    const auto first = geometry.lineIndex(constraint.firstGeometry);
    const auto second = geometry.lineIndex(constraint.secondGeometry);
    if (first && second) {
      const auto& a = geometry.lines()[*first];
      const auto& b = geometry.lines()[*second];
      const double ax = a.end.xMm - a.start.xMm;
      const double ay = a.end.yMm - a.start.yMm;
      const double bx = b.end.xMm - b.start.xMm;
      const double by = b.end.yMm - b.start.yMm;
      const double lengths = std::hypot(ax, ay) * std::hypot(bx, by);
      if (lengths > 1e-12)
        return std::acos(std::clamp((ax * bx + ay * by) / lengths,
                                    -1.0, 1.0)) * 180.0 / std::numbers::pi;
    }
  } else if (type == sketch::ConstraintType::LineDistance) {
    const auto first = geometry.lineIndex(constraint.firstGeometry);
    const auto second = geometry.lineIndex(constraint.secondGeometry);
    if (first && second) {
      const auto& a = geometry.lines()[*first];
      const auto& b = geometry.lines()[*second];
      const double dx = a.end.xMm - a.start.xMm;
      const double dy = a.end.yMm - a.start.yMm;
      const double length = std::hypot(dx, dy);
      if (length > 1e-12)
        return std::abs(dx * (b.start.yMm - a.start.yMm) -
                        dy * (b.start.xMm - a.start.xMm)) / length;
    }
  }
  return 0.0;
}

static std::optional<sketch::Dimension> dimensionFromConstraintV3(
    const sketch::Sketch& geometry, const sketch::Constraint& constraint,
    sketch::DimensionId id, double offset, double angle) {
  sketch::Dimension dimension;
  dimension.id = id;
  dimension.constraintId = constraint.id;
  dimension.offsetMm = offset;
  dimension.angleRad = angle;
  dimension.valueMm = measuredConstraintValue(geometry, constraint);
  switch (static_cast<sketch::ConstraintType>(constraint.type)) {
    case sketch::ConstraintType::Length:
      dimension.kind = sketch::DimensionKind::LineLength;
      dimension.geometryId = constraint.firstGeometry;
      break;
    case sketch::ConstraintType::Diameter:
      dimension.kind = sketch::DimensionKind::CircleDiameter;
      dimension.geometryId = constraint.firstGeometry;
      break;
    case sketch::ConstraintType::Radius:
      dimension.kind = sketch::DimensionKind::CircleRadius;
      dimension.geometryId = constraint.firstGeometry;
      break;
    case sketch::ConstraintType::Distance:
      dimension.kind = sketch::DimensionKind::PointDistance;
      dimension.firstPoint = constraint.firstPoint;
      dimension.secondPoint = constraint.secondPoint;
      break;
    case sketch::ConstraintType::DistanceX:
      dimension.kind = sketch::DimensionKind::PointDistanceX;
      dimension.firstPoint = constraint.firstPoint;
      dimension.secondPoint = constraint.secondPoint;
      break;
    case sketch::ConstraintType::DistanceY:
      dimension.kind = sketch::DimensionKind::PointDistanceY;
      dimension.firstPoint = constraint.firstPoint;
      dimension.secondPoint = constraint.secondPoint;
      break;
    case sketch::ConstraintType::Angle:
      dimension.kind = sketch::DimensionKind::LineAngle;
      dimension.geometryId = constraint.firstGeometry;
      dimension.secondPoint.lineId = constraint.secondGeometry;
      break;
    case sketch::ConstraintType::LineDistance:
      dimension.kind = sketch::DimensionKind::LineDistance;
      dimension.geometryId = constraint.firstGeometry;
      dimension.secondPoint.lineId = constraint.secondGeometry;
      break;
    default:
      return std::nullopt;
  }
  return dimension;
}

static bool loadV3Root(const QJsonObject& root, ProjectData* data,
                       QString* error, bool* unsupported) {
  if (!data) return false;
  QJsonArray sketchObjects;
  if (!arrayField(root, "sketches", &sketchObjects, error)) return false;
  QJsonObject geometryOnly = root;
  auto stripped = sketchObjects;
  for (qsizetype index = 0; index < stripped.size(); ++index) {
    if (!stripped[index].isObject())
      return invalid(error, QString::fromUtf8("sketch v3 должен быть объектом."));
    auto object = stripped[index].toObject();
    if (object.value("schema").toString() !=
        QStringLiteral("typed-sketch-v1")) {
      if (unsupported) *unsupported = true;
      return invalid(error, QString::fromUtf8("неизвестная sketch schema v3."));
    }
    QJsonArray required;
    for (const char* key : {"lines", "circles", "arcs", "beziers",
                            "constraints", "dimensions",
                            "centerNodeElementIds"}) {
      if (!arrayField(object, key, &required, error)) return false;
    }
    QJsonObject allocators;
    if (!objectField(object, "idAllocators", &allocators, error) ||
        !exactObjectFields(allocators,
                           {"nextElementId", "nextGeometryId",
                            "nextConstraintId", "nextDimensionId"},
                           QStringLiteral("idAllocators v3"), error))
      return false;
    object["constraints"] = QJsonArray{};
    object["dimensions"] = QJsonArray{};
    stripped[index] = object;
  }
  geometryOnly["sketches"] = stripped;
  ProjectData loaded;
  if (!loadLegacyRoot(geometryOnly, ConstraintTypeEncoding::StableNameV1,
                      &loaded, error))
    return false;
  if (loaded.sketches.size() != static_cast<std::size_t>(sketchObjects.size()))
    return invalid(error, QString::fromUtf8("число sketch v3 не совпадает."));

  for (qsizetype position = 0; position < sketchObjects.size(); ++position) {
    const auto object = sketchObjects[position].toObject();
    auto& geometry = loaded.sketches[static_cast<std::size_t>(position)].geometry;
    const auto ids = [&](const char* key, std::vector<sketch::GeometryId>* out) {
      const auto values = object.value(QLatin1String(key)).toArray();
      out->clear();
      out->reserve(static_cast<std::size_t>(values.size()));
      for (const auto& value : values) {
        qint64 id = 0;
        if (!value.isObject() ||
            !integerNumber(value.toObject().value("id"),
                           QStringLiteral("geometry.id"), 1,
                           ProjectFile::kMaximumPersistedId, &id, error))
          return false;
        out->push_back(static_cast<sketch::GeometryId>(id));
      }
      return true;
    };
    std::vector<sketch::GeometryId> lineIds, circleIds, arcIds, bezierIds;
    if (!ids("lines", &lineIds) || !ids("circles", &circleIds) ||
        !ids("arcs", &arcIds) || !ids("beziers", &bezierIds) ||
        !geometry.restoreGeometryIds(std::move(lineIds), std::move(circleIds),
                                     std::move(arcIds), std::move(bezierIds)))
      return invalid(error,
                     QString::fromUtf8("geometry IDs v3 повреждены или повторяются."));

    std::vector<sketch::Constraint> constraints;
    const auto constraintValues = object.value("constraints").toArray();
    constraints.reserve(static_cast<std::size_t>(constraintValues.size()));
    for (const auto& value : constraintValues) {
      sketch::Constraint constraint;
      if (!decodedConstraintV3(value, &constraint, error, unsupported))
        return false;
      constraints.push_back(std::move(constraint));
    }
    if (!geometry.restoreConstraints(std::move(constraints)))
      return invalid(error,
                     QString::fromUtf8("constraint payload v3 невалиден."));

    std::unordered_set<sketch::ConstraintId> annotated;
    std::unordered_set<sketch::DimensionId> dimensionIds;
    for (const auto& value : object.value("dimensions").toArray()) {
      if (!value.isObject())
        return invalid(error, QString::fromUtf8("annotation v3 повреждён."));
      const auto annotation = value.toObject();
      if (!exactObjectFields(annotation,
                             {"id", "constraintId", "offset", "angle"},
                             QStringLiteral("annotation v3"), error))
        return false;
      qint64 id = 0;
      qint64 constraintId = 0;
      if (!integerNumber(annotation.value("id"), QStringLiteral("dimension.id"),
                         1, ProjectFile::kMaximumPersistedId, &id, error) ||
          !integerNumber(annotation.value("constraintId"),
                         QStringLiteral("dimension.constraintId"), 1,
                         ProjectFile::kMaximumPersistedId, &constraintId,
                         error) ||
          !finiteNumber(annotation.value("offset"), QStringLiteral("offset"),
                        -kMaximumModelLengthMm, kMaximumModelLengthMm, error) ||
          !finiteNumber(annotation.value("angle"), QStringLiteral("angle"),
                        -kMaximumCoordinateMagnitude,
                        kMaximumCoordinateMagnitude, error))
        return false;
      const auto linkedId = static_cast<sketch::ConstraintId>(constraintId);
      const auto dimensionId = static_cast<sketch::DimensionId>(id);
      if (!dimensionIds.insert(dimensionId).second)
        return invalid(error,
                       QString::fromUtf8("dimension.id v3 повторяется."));
      if (!annotated.insert(linkedId).second)
        return invalid(error,
                       QString::fromUtf8("constraint v3 имеет две annotations."));
      const auto linkedIndex = geometry.constraintIndex(linkedId);
      if (!linkedIndex)
        return invalid(error,
                       QString::fromUtf8("annotation v3 содержит stale constraintId."));
      const auto dimension = dimensionFromConstraintV3(
          geometry, geometry.constraints()[*linkedIndex],
          dimensionId,
          annotation.value("offset").toDouble(),
          annotation.value("angle").toDouble());
      if (!dimension)
        return invalid(error,
                       QString::fromUtf8("annotation связан с неразмерным constraint."));
      if (!geometry.storeDimension(*dimension))
        return invalid(error,
                       QString::fromUtf8("annotation v3 не удалось восстановить."));
    }
    const auto allocators = object.value("idAllocators").toObject();
    qint64 nextElementId = 0;
    qint64 nextGeometryId = 0;
    qint64 nextConstraintId = 0;
    qint64 nextDimensionId = 0;
    if (!integerNumber(allocators.value("nextElementId"),
                       QStringLiteral("nextElementId"), 1,
                       ProjectFile::kMaximumPersistedId, &nextElementId,
                       error) ||
        !integerNumber(allocators.value("nextGeometryId"),
                       QStringLiteral("nextGeometryId"), 1,
                       ProjectFile::kMaximumPersistedId, &nextGeometryId,
                       error) ||
        !integerNumber(allocators.value("nextConstraintId"),
                       QStringLiteral("nextConstraintId"), 1,
                       ProjectFile::kMaximumPersistedId, &nextConstraintId,
                       error) ||
        !integerNumber(allocators.value("nextDimensionId"),
                       QStringLiteral("nextDimensionId"), 1,
                       ProjectFile::kMaximumPersistedId, &nextDimensionId,
                       error) ||
        !geometry.restoreIdAllocators(
            static_cast<std::size_t>(nextElementId),
            static_cast<sketch::GeometryId>(nextGeometryId),
            static_cast<sketch::ConstraintId>(nextConstraintId),
            static_cast<sketch::DimensionId>(nextDimensionId)))
      return invalid(error,
                     QString::fromUtf8("allocator cursors v3 повреждены или заняты."));
    if (auto validation = sketch::validateSketchSchema(geometry); !validation) {
      const auto& schemaError = *validation.error;
      const auto constraintId = schemaError.constraintIds.empty()
          ? sketch::kInvalidConstraintId : schemaError.constraintIds.front();
      const auto geometryId = schemaError.geometryIds.empty()
          ? sketch::kInvalidGeometryId : schemaError.geometryIds.front();
      return invalid(
          error,
          QString::fromUtf8(
              "model-level schema validation v3 failed: code=%1, constraint=%2, geometry=%3.")
              .arg(static_cast<int>(schemaError.code))
              .arg(static_cast<qulonglong>(constraintId))
              .arg(static_cast<qulonglong>(geometryId)));
    }
  }
  *data = std::move(loaded);
  return true;
}

namespace {

enum class TypedSketchGeometryKind { Line, Circle, Arc, Bezier };

struct TypedSketchGeometryInventory {
  std::unordered_map<qint64, TypedSketchGeometryKind> kinds;
  std::unordered_set<qint64> centerElementIds;
  qint64 maximumElementId{};
  qint64 maximumGeometryId{};
};

struct TypedSketchConstraintRecord {
  QJsonObject authored;
  qint64 id{};
};

bool typedSketchWellFormedUtf16(const QString& value) {
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

bool collectTypedSketchGeometry(const QJsonObject& saved,
                                TypedSketchGeometryInventory* inventory,
                                QString* error) {
  if (!inventory) return false;
  const auto collect = [&](const char* field, TypedSketchGeometryKind kind) {
    const auto values = saved.value(QLatin1String(field)).toArray();
    for (qsizetype index = 0; index < values.size(); ++index) {
      if (!values.at(index).isObject())
        return invalid(error, QStringLiteral("geometry item must be an object."));
      const auto object = values.at(index).toObject();
      qint64 id = 0;
      if (!integerNumber(object.value("id"), QStringLiteral("geometry.id"), 1,
                         ProjectFile::kMaximumPersistedId, &id, error))
        return false;
      if (!inventory->kinds.emplace(id, kind).second)
        return invalid(error,
                       QStringLiteral("geometry IDs repeat across sketch kinds."));
      inventory->maximumGeometryId =
          std::max(inventory->maximumGeometryId, id);
      if (kind == TypedSketchGeometryKind::Line) {
        qint64 elementId = 0;
        if (!integerNumber(object.value("elementId"),
                           QStringLiteral("line.elementId"), 1,
                           ProjectFile::kMaximumPersistedId, &elementId,
                           error))
          return false;
        inventory->maximumElementId =
            std::max(inventory->maximumElementId, elementId);
      }
    }
    return true;
  };
  if (!collect("lines", TypedSketchGeometryKind::Line) ||
      !collect("circles", TypedSketchGeometryKind::Circle) ||
      !collect("arcs", TypedSketchGeometryKind::Arc) ||
      !collect("beziers", TypedSketchGeometryKind::Bezier))
    return false;
  for (const auto& value : saved.value("centerNodeElementIds").toArray()) {
    qint64 elementId = 0;
    if (!integerNumber(value, QStringLiteral("centerNodeElementIds"), 1,
                       ProjectFile::kMaximumPersistedId, &elementId, error))
      return false;
    inventory->centerElementIds.insert(elementId);
  }
  return true;
}

bool typedSketchPointExists(const TypedSketchGeometryInventory& inventory,
                            const sketch::PointReference& point) {
  return std::visit(
      [&](const auto& source) {
        using T = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<T, sketch::DatumOriginReference>) {
          return true;
        } else if constexpr (std::is_same_v<T,
                                            sketch::CompositeCenterReference>) {
          return inventory.centerElementIds.contains(
              static_cast<qint64>(source.elementId));
        } else {
          qint64 id = 0;
          TypedSketchGeometryKind expected = TypedSketchGeometryKind::Line;
          if constexpr (std::is_same_v<T, sketch::LineEndpointReference>) {
            id = static_cast<qint64>(source.lineId);
            expected = TypedSketchGeometryKind::Line;
          } else if constexpr (std::is_same_v<
                                   T, sketch::CircleCenterReference>) {
            id = static_cast<qint64>(source.circleId);
            expected = TypedSketchGeometryKind::Circle;
          } else if constexpr (std::is_same_v<T,
                                                   sketch::ArcEndpointReference>) {
            id = static_cast<qint64>(source.arcId);
            expected = TypedSketchGeometryKind::Arc;
          } else if constexpr (std::is_same_v<
                                   T, sketch::BezierPointReference>) {
            id = static_cast<qint64>(source.bezierId);
            expected = TypedSketchGeometryKind::Bezier;
          }
          const auto found = inventory.kinds.find(id);
          return found != inventory.kinds.end() && found->second == expected;
        }
      },
      point.payload());
}

bool typedSketchConstraintReferencesValid(
    const TypedSketchGeometryInventory& inventory,
    const sketch::Constraint& constraint, QString* error) {
  const auto type = static_cast<sketch::ConstraintType>(constraint.type);
  const auto first =
      static_cast<qint64>(static_cast<sketch::GeometryId>(constraint.firstGeometry));
  const auto second = static_cast<qint64>(
      static_cast<sketch::GeometryId>(constraint.secondGeometry));
  const auto kind = [&](qint64 id, TypedSketchGeometryKind expected) {
    const auto found = inventory.kinds.find(id);
    return found != inventory.kinds.end() && found->second == expected;
  };
  const auto anyGeometry = [&](qint64 id) {
    return inventory.kinds.contains(id);
  };
  const auto firstPoint =
      static_cast<sketch::PointReference>(constraint.firstPoint);
  const auto secondPoint =
      static_cast<sketch::PointReference>(constraint.secondPoint);
  const auto twoPoints = [&] {
    return typedSketchPointExists(inventory, firstPoint) &&
           typedSketchPointExists(inventory, secondPoint) &&
           firstPoint != secondPoint;
  };
  const auto twoLines = [&] {
    return first != second && kind(first, TypedSketchGeometryKind::Line) &&
           kind(second, TypedSketchGeometryKind::Line);
  };

  bool valid = false;
  switch (type) {
    case sketch::ConstraintType::Horizontal:
    case sketch::ConstraintType::Vertical:
    case sketch::ConstraintType::Length:
      valid = kind(first, TypedSketchGeometryKind::Line);
      break;
    case sketch::ConstraintType::Radius:
    case sketch::ConstraintType::Diameter:
      valid = kind(first, TypedSketchGeometryKind::Circle);
      break;
    case sketch::ConstraintType::Coincident:
    case sketch::ConstraintType::Distance:
    case sketch::ConstraintType::DistanceX:
    case sketch::ConstraintType::DistanceY:
      valid = twoPoints();
      break;
    case sketch::ConstraintType::PointOnLine:
    case sketch::ConstraintType::Midpoint:
      valid = kind(first, TypedSketchGeometryKind::Line) &&
              typedSketchPointExists(inventory, secondPoint);
      break;
    case sketch::ConstraintType::PointOnCircle:
      valid = kind(first, TypedSketchGeometryKind::Circle) &&
              typedSketchPointExists(inventory, secondPoint);
      break;
    case sketch::ConstraintType::PointOnArc:
      valid = kind(first, TypedSketchGeometryKind::Arc) &&
              typedSketchPointExists(inventory, secondPoint);
      break;
    case sketch::ConstraintType::PointOnXAxis:
    case sketch::ConstraintType::PointOnYAxis:
      valid = typedSketchPointExists(inventory, secondPoint);
      break;
    case sketch::ConstraintType::Parallel:
    case sketch::ConstraintType::Perpendicular:
    case sketch::ConstraintType::Angle:
    case sketch::ConstraintType::LineDistance:
      valid = twoLines();
      break;
    case sketch::ConstraintType::Equal: {
      const auto firstKind = inventory.kinds.find(first);
      const auto secondKind = inventory.kinds.find(second);
      valid = first != second && firstKind != inventory.kinds.end() &&
              secondKind != inventory.kinds.end() &&
              firstKind->second == secondKind->second &&
              (firstKind->second == TypedSketchGeometryKind::Line ||
               firstKind->second == TypedSketchGeometryKind::Circle);
      break;
    }
    case sketch::ConstraintType::Tangent: {
      const auto firstKind = inventory.kinds.find(first);
      const auto secondKind = inventory.kinds.find(second);
      valid = first != second && firstKind != inventory.kinds.end() &&
              secondKind != inventory.kinds.end() &&
              !(firstKind->second == TypedSketchGeometryKind::Line &&
                secondKind->second == TypedSketchGeometryKind::Line);
      break;
    }
    case sketch::ConstraintType::Lock:
      valid = anyGeometry(first);
      break;
  }
  if (!valid)
    return invalid(error,
                   QStringLiteral("typed constraint has invalid references."));
  if (constraint.mode == sketch::ConstraintMode::Reference &&
      !dimensionalConstraintType(type))
    return invalid(error,
                   QStringLiteral("reference mode requires a dimensional constraint."));
  if (type == sketch::ConstraintType::Angle &&
      constraint.mode != sketch::ConstraintMode::Reference &&
      static_cast<double>(constraint.value) >= 180.0)
    return invalid(error,
                   QStringLiteral("angle constraint must be below 180 degrees."));
  return true;
}

bool typedSketchConstraintIsAnnotatable(sketch::ConstraintType type) {
  return type == sketch::ConstraintType::Length ||
         type == sketch::ConstraintType::Radius ||
         type == sketch::ConstraintType::Diameter ||
         type == sketch::ConstraintType::Distance ||
         type == sketch::ConstraintType::DistanceX ||
         type == sketch::ConstraintType::DistanceY ||
         type == sketch::ConstraintType::Angle ||
         type == sketch::ConstraintType::LineDistance;
}

bool validateTypedSketchObject(
    const QJsonObject& saved,
    detail::ProjectFormatV3TypedSketchValidationMode mode,
    bool* sawUnsupported, QString* unsupportedDiagnostic, QString* error) {
  TypedSketchGeometryInventory geometry;
  if (!collectTypedSketchGeometry(saved, &geometry, error)) return false;

  const QJsonArray authoredConstraints = saved.value("constraints").toArray();
  std::vector<TypedSketchConstraintRecord> records;
  records.reserve(static_cast<std::size_t>(authoredConstraints.size()));
  std::unordered_set<qint64> constraintIds;
  qint64 maximumConstraintId = 0;
  for (qsizetype index = 0; index < authoredConstraints.size(); ++index) {
    if (!authoredConstraints.at(index).isObject())
      return invalid(error, QStringLiteral("constraint v3 must be an object."));
    const auto object = authoredConstraints.at(index).toObject();
    if (!exactObjectFields(object, {"id", "mode", "payload"},
                           QStringLiteral("constraint v3"), error))
      return false;
    qint64 id = 0;
    if (!integerNumber(object.value("id"), QStringLiteral("constraint.id"), 1,
                       ProjectFile::kMaximumPersistedId, &id, error) ||
        !constraintIds.insert(id).second)
      return invalid(error,
                     QStringLiteral("constraint IDs must be positive and unique."));
    if (!object.value("mode").isString())
      return invalid(error, QStringLiteral("constraint mode must be a string."));
    const QString modeText = object.value("mode").toString();
    if (modeText != QStringLiteral("driving") &&
        modeText != QStringLiteral("reference") &&
        modeText != QStringLiteral("disabled"))
      return invalid(error, QStringLiteral("unknown constraint mode v3."));
    if (!object.value("payload").isObject())
      return invalid(error, QStringLiteral("constraint payload must be an object."));
    if (mode == detail::ProjectFormatV3TypedSketchValidationMode::StableNameV1) {
      const auto payload = object.value("payload").toObject();
      qint64 ignoredType = 0;
      if (!boundedString(payload.value("typeKey"),
                         QStringLiteral("constraint.typeKey"), error, false) ||
          !typedSketchWellFormedUtf16(payload.value("typeKey").toString()) ||
          !integerNumber(payload.value("type"),
                         QStringLiteral("constraint.type"), 0,
                         kMaximumExactJsonInteger, &ignoredType, error))
        return invalid(error,
                       QStringLiteral("constraint.typeKey or type is malformed."));
    }
    maximumConstraintId = std::max(maximumConstraintId, id);
    records.push_back({object, id});
  }

  std::unordered_map<qint64, std::optional<sketch::ConstraintType>>
      decodedTypes;
  decodedTypes.reserve(records.size());
  if (mode == detail::ProjectFormatV3TypedSketchValidationMode::
                  OpaqueConstraintEncoding) {
    if (sawUnsupported) *sawUnsupported = true;
    if (unsupportedDiagnostic && unsupportedDiagnostic->isEmpty())
      *unsupportedDiagnostic =
          QStringLiteral("constraint type encoding is unsupported");
    for (const auto& record : records)
      decodedTypes.emplace(record.id, std::nullopt);
  } else {
    for (const auto& record : records) {
      const auto payload = record.authored.value("payload").toObject();
      sketch::ConstraintType type{};
      bool unsupportedType = false;
      QString typeDiagnostic;
      if (!decodeConstraintType(payload, ConstraintTypeEncoding::StableNameV1,
                                &type, &unsupportedType, &typeDiagnostic)) {
        if (!unsupportedType) {
          if (error) *error = std::move(typeDiagnostic);
          return false;
        }
        if (sawUnsupported) *sawUnsupported = true;
        if (unsupportedDiagnostic && unsupportedDiagnostic->isEmpty())
          *unsupportedDiagnostic = std::move(typeDiagnostic);
        decodedTypes.emplace(record.id, std::nullopt);
        continue;
      }
      sketch::Constraint decoded;
      bool decoderUnsupported = false;
      if (!decodedConstraintV3(record.authored, &decoded, error,
                               &decoderUnsupported) || decoderUnsupported)
        return false;
      if (!typedSketchConstraintReferencesValid(geometry, decoded, error))
        return false;
      decodedTypes.emplace(record.id, type);
    }
  }

  const QJsonArray dimensions = saved.value("dimensions").toArray();
  std::unordered_set<qint64> dimensionIds;
  std::unordered_set<qint64> annotatedConstraintIds;
  qint64 maximumDimensionId = 0;
  for (const auto& value : dimensions) {
    if (!value.isObject())
      return invalid(error, QStringLiteral("annotation v3 must be an object."));
    const auto annotation = value.toObject();
    if (!exactObjectFields(annotation, {"id", "constraintId", "offset", "angle"},
                           QStringLiteral("annotation v3"), error))
      return false;
    qint64 id = 0;
    qint64 constraintId = 0;
    if (!integerNumber(annotation.value("id"), QStringLiteral("dimension.id"),
                       1, ProjectFile::kMaximumPersistedId, &id, error) ||
        !dimensionIds.insert(id).second)
      return invalid(error,
                     QStringLiteral("dimension IDs must be positive and unique."));
    if (!integerNumber(annotation.value("constraintId"),
                       QStringLiteral("dimension.constraintId"), 1,
                       ProjectFile::kMaximumPersistedId, &constraintId,
                       error) ||
        !decodedTypes.contains(constraintId))
      return invalid(error,
                     QStringLiteral("annotation has a stale constraintId."));
    if (!annotatedConstraintIds.insert(constraintId).second)
      return invalid(error,
                     QStringLiteral("constraint has duplicate annotations."));
    if (!finiteNumber(annotation.value("offset"), QStringLiteral("offset"),
                      -kMaximumModelLengthMm, kMaximumModelLengthMm, error) ||
        !finiteNumber(annotation.value("angle"), QStringLiteral("angle"),
                      -kMaximumCoordinateMagnitude,
                      kMaximumCoordinateMagnitude, error))
      return false;
    const auto linkedType = decodedTypes.find(constraintId)->second;
    if (linkedType && !typedSketchConstraintIsAnnotatable(*linkedType))
      return invalid(error,
                     QStringLiteral("annotation targets a non-dimensional constraint."));
    maximumDimensionId = std::max(maximumDimensionId, id);
  }

  const auto allocators = saved.value("idAllocators").toObject();
  qint64 nextElementId = 0;
  qint64 nextGeometryId = 0;
  qint64 nextConstraintId = 0;
  qint64 nextDimensionId = 0;
  if (!integerNumber(allocators.value("nextElementId"),
                     QStringLiteral("nextElementId"), 1,
                     ProjectFile::kMaximumPersistedId, &nextElementId, error) ||
      !integerNumber(allocators.value("nextGeometryId"),
                     QStringLiteral("nextGeometryId"), 1,
                     ProjectFile::kMaximumPersistedId, &nextGeometryId, error) ||
      !integerNumber(allocators.value("nextConstraintId"),
                     QStringLiteral("nextConstraintId"), 1,
                     ProjectFile::kMaximumPersistedId, &nextConstraintId,
                     error) ||
      !integerNumber(allocators.value("nextDimensionId"),
                     QStringLiteral("nextDimensionId"), 1,
                     ProjectFile::kMaximumPersistedId, &nextDimensionId,
                     error) ||
      nextElementId <= geometry.maximumElementId ||
      nextGeometryId <= geometry.maximumGeometryId ||
      nextConstraintId <= maximumConstraintId ||
      nextDimensionId <= maximumDimensionId)
    return invalid(error,
                   QStringLiteral("allocator cursors are occupied or invalid."));
  return true;
}

}  // namespace

namespace detail {

ProjectFormatV3TypedSketchValidationResult
validateProjectFormatV3TypedSketches(
    const QJsonObject& authoredRoot,
    ProjectFormatV3TypedSketchValidationMode mode) {
  const auto result = [](ProjectFormatV3TypedSketchValidationStatus status,
                         QString diagnostic = {}) {
    ProjectFormatV3TypedSketchValidationResult value;
    value.status = status;
    value.diagnostic = std::move(diagnostic);
    return value;
  };
  const auto invalidResult = [&](QString diagnostic) {
    if (diagnostic.isEmpty())
      diagnostic = QStringLiteral("typed sketch validation failed");
    return result(ProjectFormatV3TypedSketchValidationStatus::Invalid,
                  std::move(diagnostic));
  };

  QJsonObject legacyProjection{
      {QStringLiteral("name"), authoredRoot.value(QStringLiteral("name"))},
      {QStringLiteral("createdAt"),
       authoredRoot.value(QStringLiteral("createdAt"))},
      {QStringLiteral("document"),
       authoredRoot.value(QStringLiteral("document"))},
      {QStringLiteral("sketches"),
       authoredRoot.value(QStringLiteral("sketches"))},
      {QStringLiteral("extrusion"),
       authoredRoot.value(QStringLiteral("extrusion"))}};

  const QJsonValue sketchesValue =
      legacyProjection.value(QStringLiteral("sketches"));
  if (!sketchesValue.isArray())
    return invalidResult(QStringLiteral("sketches must be an array"));
  QJsonArray geometryOnlySketches = sketchesValue.toArray();
  for (qsizetype index = 0; index < geometryOnlySketches.size(); ++index) {
    if (!geometryOnlySketches.at(index).isObject())
      return invalidResult(
          QStringLiteral("sketch %1 must be an object").arg(index));
    QJsonObject sketch = geometryOnlySketches.at(index).toObject();
    sketch[QStringLiteral("constraints")] = QJsonArray{};
    sketch[QStringLiteral("dimensions")] = QJsonArray{};
    geometryOnlySketches[index] = sketch;
  }
  QJsonObject geometryOnly = legacyProjection;
  geometryOnly[QStringLiteral("sketches")] = geometryOnlySketches;

  QString diagnostic;
  bool ignoredUnsupported = false;
  if (!validateLegacyRoot(geometryOnly, false,
                          LegacySketchSchema::StableNameV1,
                          ConstraintTypeEncoding::StableNameV1, nullptr,
                          &ignoredUnsupported, &diagnostic))
    return invalidResult(std::move(diagnostic));

  const bool opaqueEncoding =
      mode == ProjectFormatV3TypedSketchValidationMode::
                  OpaqueConstraintEncoding;
  bool sawUnsupported = opaqueEncoding;
  QString unsupportedDiagnostic =
      opaqueEncoding
          ? QStringLiteral("constraint type encoding is unsupported")
          : QString{};
  const QJsonArray sketches = sketchesValue.toArray();
  for (const auto& value : sketches) {
    if (!validateTypedSketchObject(value.toObject(), mode, &sawUnsupported,
                                   &unsupportedDiagnostic, &diagnostic))
      return invalidResult(std::move(diagnostic));
  }
  return result(sawUnsupported
                    ? ProjectFormatV3TypedSketchValidationStatus::Unsupported
                    : ProjectFormatV3TypedSketchValidationStatus::Candidate,
                std::move(unsupportedDiagnostic));
}

}  // namespace detail

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
    std::vector<sketch::Dimension> legacyDimensions;
    legacyDimensions.reserve(
        static_cast<std::size_t>(serializedDimensions.size()));
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
        if (dimension.geometryId == sketch::kInvalidGeometryId) {
          setError(error,
                   QString::fromUtf8(
                       "Размер длины ссылается на неизвестную линию."));
          return false;
        }
      } else if (kind == sketch::DimensionKind::CircleDiameter ||
                 kind == sketch::DimensionKind::CircleRadius) {
        const auto index =
            static_cast<std::size_t>(object.value("geometryIndex").toInteger());
        dimension.geometryId = saved.geometry.circleId(index);
        if (dimension.geometryId == sketch::kInvalidGeometryId) {
          setError(error,
                   QString::fromUtf8(
                       "Радиальный размер ссылается на неизвестную окружность."));
          return false;
        }
      } else if (kind == sketch::DimensionKind::LineAngle ||
                 kind == sketch::DimensionKind::LineDistance) {
        const auto firstIndex =
            static_cast<std::size_t>(object.value("geometryIndex").toInteger());
        const auto secondIndex = static_cast<std::size_t>(
            object.value("secondGeometryIndex").toInteger());
        dimension.geometryId = saved.geometry.lineId(firstIndex);
        dimension.secondPoint.lineId = saved.geometry.lineId(secondIndex);
        if (dimension.geometryId == sketch::kInvalidGeometryId ||
            dimension.secondPoint.lineId == sketch::kInvalidGeometryId) {
          setError(error,
                   QString::fromUtf8(
                       "Угловой или линейный размер содержит stale line reference."));
          return false;
        }
      } else if (kind == sketch::DimensionKind::PointDistance ||
                 kind == sketch::DimensionKind::PointDistanceX ||
                 kind == sketch::DimensionKind::PointDistanceY) {
        const auto restorePoint = [&saved, &object](
                                      const QString& prefix,
                                      sketch::PointReference* point) {
          const bool start =
              object.value(prefix + QStringLiteral("Start")).toBool(true);
          if (object.value(prefix + QStringLiteral("Origin")).toBool(false)) {
            point->origin = true;
            return true;
          }
          const qint64 line =
              object.value(prefix + QStringLiteral("Line")).toInteger(-1);
          if (line >= 0) {
            point->lineId = saved.geometry.lineId(static_cast<std::size_t>(line));
            point->start = start;
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
            point->start = start;
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

      legacyDimensions.push_back(std::move(dimension));
    }

    std::vector<sketch::Constraint> restoredConstraints;
    restoredConstraints.reserve(
        static_cast<std::size_t>(savedObject.value("constraints").toArray().size()));
    for (const auto constraintValue : savedObject.value("constraints").toArray()) {
      const auto object = constraintValue.toObject();
      sketch::Constraint constraint;
      constraint.id =
          static_cast<sketch::ConstraintId>(object.value("id").toInteger());
      sketch::ConstraintType decodedType{};
      if (!decodeConstraintType(object, constraintEncoding, &decodedType,
                                nullptr, error))
        return false;
      constraint.type = decodedType;
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
    if (!migrateLegacyDimensionLinks(&saved.geometry,
                                     std::move(legacyDimensions), error))
      return false;
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

namespace {

bool equivalentV4ModelCandidate(const ProjectFormatV4ModelCandidate& lhs,
                                const ProjectFormatV4ModelCandidate& rhs) {
  const auto& leftLegacy = lhs.legacySketch;
  const auto& rightLegacy = rhs.legacySketch;
  const auto& left = leftLegacy.topology;
  const auto& right = rightLegacy.topology;
  const auto& leftPayload = left.payload;
  const auto& rightPayload = right.payload;
  const auto& leftEnvelope = leftPayload.envelope;
  const auto& rightEnvelope = rightPayload.envelope;
  return leftLegacy.deferredLegacySketch ==
             rightLegacy.deferredLegacySketch &&
         leftEnvelope.authoredRoot == rightEnvelope.authoredRoot &&
         leftEnvelope.document == rightEnvelope.document &&
         leftEnvelope.legacySketches == rightEnvelope.legacySketches &&
         leftEnvelope.legacyExtrusion == rightEnvelope.legacyExtrusion &&
         leftEnvelope.model == rightEnvelope.model &&
         leftEnvelope.capabilities == rightEnvelope.capabilities &&
         leftEnvelope.deferredUnknowns == rightEnvelope.deferredUnknowns &&
         leftPayload.topologySites == rightPayload.topologySites &&
         leftPayload.featureOwners == rightPayload.featureOwners &&
         left.topologyReferences == right.topologyReferences &&
         left.topologyInspections == right.topologyInspections &&
         left.deferredUnsupported == right.deferredUnsupported &&
         left.provenanceRecordCount == right.provenanceRecordCount &&
         left.localNameUtf8Bytes == right.localNameUtf8Bytes;
}

struct V4MaterializationInventory {
  const ProjectFormatV4TopologyAggregateCandidate& topology;
  std::unordered_map<std::string, std::size_t> byPath;
  std::vector<bool> consumed;
  QString* error{};

  bool initialize() {
    const std::size_t count = topology.payload.topologySites.size();
    if (!topology.deferredUnsupported.empty() ||
        topology.payload.envelope.deferredUnknowns.size() != 0 ||
        topology.topologyReferences.size() != count ||
        topology.topologyInspections.size() != count) {
      setError(error,
               QStringLiteral("v4 topology inventory is incomplete"));
      return false;
    }
    consumed.assign(count, false);
    byPath.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto& site = topology.payload.topologySites[index];
      if (!topology.topologyReferences[index] ||
          !topology.topologyInspections[index] ||
          !byPath.emplace(site.path.toStdString(), index).second) {
        setError(error,
                 QStringLiteral("v4 topology inventory is not canonical"));
        return false;
      }
    }
    return true;
  }

  const DecodedProjectFormatV4TopologyReference* consume(
      const QJsonValue& authored, const QString& path,
      TopologyKind expectedKind) {
    const auto found = byPath.find(path.toStdString());
    if (found == byPath.end()) {
      setError(error, path + QStringLiteral(": topology site is missing"));
      return nullptr;
    }
    const std::size_t index = found->second;
    const auto& site = topology.payload.topologySites[index];
    const QJsonObject object = authored.toObject();
    const QJsonValue tagged = object.contains("reference")
                                  ? object.value("reference")
                                  : authored;
    if (consumed[index] || site.expectedKind != expectedKind ||
        site.authoredReference != tagged) {
      setError(error, path +
                          QStringLiteral(": topology site does not match payload"));
      return nullptr;
    }
    consumed[index] = true;
    return &*topology.topologyReferences[index];
  }

  bool consumeFace(const QJsonValue& authored, const QString& path,
                   FaceReference* result) {
    const auto* reference = consume(authored, path, TopologyKind::Face);
    const auto* face = reference ? std::get_if<FaceReference>(reference)
                                 : nullptr;
    if (!face || !result) {
      if (reference)
        setError(error, path + QStringLiteral(": expected a face carrier"));
      return false;
    }
    *result = *face;
    return true;
  }

  bool consumeEdge(const QJsonValue& authored, const QString& path,
                   EdgeReference* result) {
    const auto* reference = consume(authored, path, TopologyKind::Edge);
    const auto* edge = reference ? std::get_if<EdgeReference>(reference)
                                 : nullptr;
    if (!edge || !result) {
      if (reference)
        setError(error, path + QStringLiteral(": expected an edge carrier"));
      return false;
    }
    *result = *edge;
    return true;
  }

  bool completelyConsumed() const {
    return std::all_of(consumed.begin(), consumed.end(),
                       [](bool value) { return value; });
  }
};

struct DecodedPartPayload {
  std::optional<qint64> sourceVersion;
  ProjectPayloadKind payloadKind{ProjectPayloadKind::None};
  QJsonObject root;
  ProjectData legacy;
};

struct PartPayloadDecodeResult {
  ProjectLoadStatus status{ProjectLoadStatus::Invalid};
  DecodedPartPayload payload;
  QString error;
};

struct MaterializedPartPayloadResult {
  std::optional<Document> document;
  QString error;
};

PartPayloadDecodeResult decodeAndValidatePartPayload(
    QJsonObject root, std::optional<qint64>& observedSourceVersion) {
  PartPayloadDecodeResult result;
  result.payload.root = std::move(root);
  const QJsonObject& payloadRoot = result.payload.root;

  if (!payloadRoot.value("format").isString()) {
    result.error = QString::fromUtf8(
        "Файл проекта повреждён: отсутствует строковое поле format.");
    return result;
  }
  if (payloadRoot.value("format").toString() !=
      QStringLiteral("solidar-project")) {
    result.status = ProjectLoadStatus::Unsupported;
    result.error = QString::fromUtf8("Неподдерживаемый формат проекта.");
    return result;
  }

  qint64 version = 0;
  if (!integerNumber(payloadRoot.value("version"), QStringLiteral("version"),
                     1, kMaximumExactJsonInteger, &version, &result.error))
    return result;
  observedSourceVersion = version;
  result.payload.sourceVersion = observedSourceVersion;
  if (version != 1 && version != 2 && version != 3) {
    result.status = ProjectLoadStatus::Unsupported;
    result.error = QString::fromUtf8("Неподдерживаемая версия проекта: %1.")
                       .arg(version);
    return result;
  }

  // The parametric model was introduced together with format v2. Treating
  // a downgraded/corrupted v2 root as legacy would silently discard its
  // history and allow the next Save to overwrite the original model.
  if (version == 1 && payloadRoot.contains("model")) {
    result.error = QString::fromUtf8(
        "Проект v1 не может содержать параметрическую модель v2.");
    return result;
  }

  ProjectData legacy;
  if (version == 3) {
    bool unsupportedSchema = false;
    if (!loadV3Root(payloadRoot, &legacy, &result.error,
                    &unsupportedSchema)) {
      if (unsupportedSchema) result.status = ProjectLoadStatus::Unsupported;
      return result;
    }
    bool unsupportedModel = false;
    if (!validateV2Root(payloadRoot, legacy, &result.error,
                        &unsupportedModel)) {
      if (unsupportedModel) result.status = ProjectLoadStatus::Unsupported;
      return result;
    }
    result.status = ProjectLoadStatus::Valid;
    result.payload.payloadKind = ProjectPayloadKind::ParametricPartV3;
    result.payload.legacy = std::move(legacy);
    result.error.clear();
    return result;
  }

  LegacyDecodePlan decodePlan;
  bool unsupportedSchema = false;
  if (!validateAndSelectLegacyPlan(payloadRoot, version, &decodePlan,
                                   &unsupportedSchema, &result.error)) {
    if (unsupportedSchema) result.status = ProjectLoadStatus::Unsupported;
    return result;
  }
  if (!loadLegacyRoot(payloadRoot, decodePlan.constraintEncoding, &legacy,
                      &result.error)) {
    if (result.error.isEmpty())
      result.error =
          QString::fromUtf8("Не удалось восстановить данные проекта.");
    return result;
  }
  if (version == 1) {
    result.status = ProjectLoadStatus::Valid;
    result.payload.payloadKind = ProjectPayloadKind::LegacyPartV1;
    result.payload.legacy = std::move(legacy);
    result.error.clear();
    return result;
  }

  bool unsupportedModel = false;
  if (!validateV2Root(payloadRoot, legacy, &result.error,
                      &unsupportedModel)) {
    if (unsupportedModel) result.status = ProjectLoadStatus::Unsupported;
    return result;
  }
  result.status = ProjectLoadStatus::Valid;
  result.payload.payloadKind = ProjectPayloadKind::ParametricPartV2;
  result.payload.legacy = std::move(legacy);
  result.error.clear();
  return result;
}

MaterializedPartPayloadResult materializeAndRecomputePartPayload(
    const DecodedPartPayload& payload) {
  MaterializedPartPayloadResult result;
  if (payload.payloadKind != ProjectPayloadKind::ParametricPartV2 &&
      payload.payloadKind != ProjectPayloadKind::ParametricPartV3) {
    result.error = QString::fromUtf8("Неизвестный вид данных проекта.");
    return result;
  }

  Document candidate;
  {
    ::solidar::detail::ScopedExplicitIdReservationPause reservationPause;
    if (!loadDocumentRoot(payload.root, payload.legacy, &candidate,
                          &result.error)) {
      if (payload.payloadKind == ProjectPayloadKind::ParametricPartV2 &&
          result.error.isEmpty()) {
        result.error = QString::fromUtf8(
            "Не удалось восстановить параметрическую модель.");
      }
      return result;
    }
  }
  result.document.emplace(std::move(candidate));
  return result;
}

}  // namespace

detail::ProjectFormatV4DocumentMaterializationResult
detail::materializeProjectFormatV4Document(
    const ProjectFormatV4ModelCandidate& candidate) {
  ProjectFormatV4DocumentMaterializationResult result;
  try {
    const QJsonObject authoredRoot =
        candidate.legacySketch.topology.payload.envelope.authoredRoot;
    auto envelope = scanProjectFormatV4Envelope(
        QJsonDocument(authoredRoot).toJson(QJsonDocument::Compact));
    if (!envelope.hasCandidate()) {
      result.error = QStringLiteral("v4 envelope revalidation failed: %1")
                         .arg(envelope.error);
      return result;
    }
    auto payload = scanProjectFormatV4Payload(std::move(*envelope.candidate));
    if (!payload.hasCandidate()) {
      result.error = QStringLiteral("v4 payload revalidation failed: %1")
                         .arg(payload.error);
      return result;
    }
    auto validated = validateProjectFormatV4Model(std::move(*payload.candidate));
    if (validated.status != ProjectFormatV4ModelValidationStatus::Candidate ||
        !validated.candidate) {
      result.error = QStringLiteral("v4 model revalidation failed: %1")
                         .arg(validated.error);
      return result;
    }
    if (!equivalentV4ModelCandidate(candidate, *validated.candidate)) {
      result.error = QStringLiteral("v4 candidate does not match authored root");
      return result;
    }

    ProjectData legacy;
    bool unsupported = false;
    QString error;
    if (!loadV3Root(authoredRoot, &legacy, &error, &unsupported)) {
      result.error = error.isEmpty()
                         ? QStringLiteral("v4 sketch materialization failed")
                         : std::move(error);
      return result;
    }

    const auto& topology = validated.candidate->legacySketch.topology;
    V4MaterializationInventory inventory{topology, {}, {}, &error};
    if (!inventory.initialize()) {
      result.error = std::move(error);
      return result;
    }
    const auto faceReference =
        [&inventory](const QJsonValue& authored, const QString& path,
                     FaceReference* face) {
          return inventory.consumeFace(authored, path, face);
        };
    const auto edgeReference =
        [&inventory](const QJsonValue& authored, const QString& path,
                     EdgeReference* edge) {
          return inventory.consumeEdge(authored, path, edge);
        };

    Document materialized;
    {
      ::solidar::detail::ScopedExplicitIdReservationPause reservationPause;
      if (!populateDocumentRoot(authoredRoot, legacy, &materialized,
                                faceReference, edgeReference, &error)) {
        result.error = error.isEmpty()
                           ? QStringLiteral("v4 document materialization failed")
                           : std::move(error);
        return result;
      }
    }
    if (!inventory.completelyConsumed()) {
      result.error = QStringLiteral("v4 topology inventory was not fully consumed");
      return result;
    }
    result.status =
        ProjectFormatV4DocumentMaterializationStatus::Materialized;
    result.document.emplace(std::move(materialized));
    return result;
  } catch (const Standard_Failure& failure) {
    result.error = QStringLiteral("OCCT failure while materializing v4: %1")
                       .arg(QString::fromUtf8(failure.what()));
  } catch (const std::exception& exception) {
    result.error = QStringLiteral("failure while materializing v4: %1")
                       .arg(QString::fromUtf8(exception.what()));
  } catch (...) {
    result.error = QStringLiteral("unknown failure while materializing v4");
  }
  return result;
}

bool detail::acceptsProjectFormatV4RecomputeResult(
    const RecomputeResult& result) noexcept {
  if (result.success) return result.primaryFailures.empty();
  if (result.primaryFailures.empty()) return false;
  return std::all_of(
      result.primaryFailures.begin(), result.primaryFailures.end(),
      [](const RecomputeFailure& failure) {
        if (failure.site != RecomputeFailureSite::Feature &&
            failure.site != RecomputeFailureSite::SketchSupport)
          return false;
        return failure.failure.code ==
                   OperationFailureCode::TopologyReferenceMissing ||
               failure.failure.code ==
                   OperationFailureCode::TopologyReferenceAmbiguous;
      });
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

    const QJsonObject parsedRoot = parsed.object();
    qint64 parsedVersion = 0;
    QString versionError;
    if (parsedRoot.value("format").isString() &&
        parsedRoot.value("format").toString() ==
            QStringLiteral("solidar-project") &&
        integerNumber(parsedRoot.value("version"), QStringLiteral("version"),
                      1, kMaximumExactJsonInteger, &parsedVersion,
                      &versionError) &&
        parsedVersion == 4) {
      result.sourceVersion = parsedVersion;
      ::solidar::detail::ScopedExplicitIdReservationPause reservationPause;
      auto envelope = scanProjectFormatV4Envelope(QByteArrayView(payload));
      if (!envelope.hasCandidate()) {
        result.status =
            envelope.status == ProjectFormatV4EnvelopeScanStatus::NotApplicable
                ? ProjectLoadStatus::Unsupported
                : ProjectLoadStatus::Invalid;
        result.kind = result.status == ProjectLoadStatus::Unsupported
                          ? ProjectLoadKind::Unsupported
                          : ProjectLoadKind::Invalid;
        result.error = std::move(envelope.error);
        return result;
      }
      auto scanned = scanProjectFormatV4Payload(std::move(*envelope.candidate));
      if (!scanned.hasCandidate()) {
        result.error = std::move(scanned.error);
        return result;
      }
      auto validated = validateProjectFormatV4Model(
          std::move(*scanned.candidate));
      if (validated.status ==
          ProjectFormatV4ModelValidationStatus::UnsupportedCandidate) {
        result.status = ProjectLoadStatus::Unsupported;
        result.kind = ProjectLoadKind::Unsupported;
        result.error = std::move(validated.error);
        return result;
      }
      if (validated.status != ProjectFormatV4ModelValidationStatus::Candidate ||
          !validated.candidate) {
        result.error = std::move(validated.error);
        return result;
      }
      ProjectData restoredLegacy;
      bool unsupportedLegacy = false;
      if (!loadV3Root(
              validated.candidate->legacySketch.topology.payload.envelope
                  .authoredRoot,
              &restoredLegacy, &result.error, &unsupportedLegacy)) {
        if (unsupportedLegacy) {
          result.status = ProjectLoadStatus::Unsupported;
          result.kind = ProjectLoadKind::Unsupported;
        }
        return result;
      }
      auto materialized = detail::materializeProjectFormatV4Document(
          *validated.candidate);
      if (!materialized.succeeded()) {
        result.error = std::move(materialized.error);
        return result;
      }
      const RecomputeResult recomputed =
          materialized.document->recomputeDetailed();
      if (!detail::acceptsProjectFormatV4RecomputeResult(recomputed)) {
        result.error = QString::fromUtf8(
            "Документ v4 содержит недопустимую ошибку пересчёта модели.");
        return result;
      }
      result.status = ProjectLoadStatus::Valid;
      result.payloadKind = ProjectPayloadKind::ParametricPartV4;
      result.kind = ProjectLoadKind::ValidV2;
      result.persistenceState.format = ProjectPersistenceFormat::V4;
      result.legacy = std::move(restoredLegacy);
      result.document.emplace(std::move(*materialized.document));
      result.error.clear();
      return result;
    }
    auto decoded =
        decodeAndValidatePartPayload(parsedRoot, result.sourceVersion);
    if (decoded.status != ProjectLoadStatus::Valid) {
      result.status = decoded.status;
      if (decoded.status == ProjectLoadStatus::Unsupported)
        result.kind = ProjectLoadKind::Unsupported;
      result.error = std::move(decoded.error);
      return result;
    }

    auto decodedPayload = std::move(decoded.payload);
    if (decodedPayload.payloadKind == ProjectPayloadKind::LegacyPartV1) {
      result.status = ProjectLoadStatus::Valid;
      result.payloadKind = ProjectPayloadKind::LegacyPartV1;
      result.kind = ProjectLoadKind::ValidV1;
      result.legacy = std::move(decodedPayload.legacy);
      result.error.clear();
      return result;
    }

    auto materialized = materializeAndRecomputePartPayload(decodedPayload);
    if (!materialized.document) {
      result.error = std::move(materialized.error);
      return result;
    }

    result.status = ProjectLoadStatus::Valid;
    result.payloadKind = decodedPayload.payloadKind;
    result.kind = ProjectLoadKind::ValidV2;
    result.legacy = std::move(decodedPayload.legacy);
    result.document.emplace(std::move(*materialized.document));
    result.persistenceState.format = ProjectPersistenceFormat::V3;
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
  result.status = ProjectLoadStatus::Invalid;
  result.payloadKind = ProjectPayloadKind::None;
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
  switch (staged.payloadKind) {
    case ProjectPayloadKind::LegacyPartV1:
    case ProjectPayloadKind::ParametricPartV2:
    case ProjectPayloadKind::ParametricPartV3:
    case ProjectPayloadKind::ParametricPartV4:
      *data = std::move(staged.legacy);
      return true;
    case ProjectPayloadKind::None:
      break;
  }
  setError(error,
           QString::fromUtf8("Проект не содержит поддерживаемых данных детали."));
  return false;
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
  if (classifyPartPayload(staged.payloadKind) !=
      PartPayloadDisposition::Parametric) {
    setError(error, QString::fromUtf8(
                        "Проект не содержит параметрическую историю v2/v3/v4."));
    return false;
  }
  if (!staged.document) {
    setError(error, QString::fromUtf8(
                        "Проект не содержит восстановленного документа."));
    return false;
  }
  *document = std::move(*staged.document);
  document->reserveIdsForEditing();
  return true;
}

}  // namespace solidar::project
