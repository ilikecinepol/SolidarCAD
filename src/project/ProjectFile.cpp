#include "project/ProjectFile.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>

#include <utility>

namespace solidar::project {
namespace {

void setError(QString* target, const QString& value) {
  if (target) *target = value;
}

}  // namespace

bool ProjectFile::create(const QString& path, QString* error) {
  return save(path, ProjectData{}, error);
}

bool ProjectFile::save(const QString& path, const ProjectData& data,
                       QString* error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    setError(error, file.errorString());
    return false;
  }
  QJsonObject root;
  root["format"] = "solidar-project";
  root["version"] = 1;
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
    QJsonArray dimensions;
    for (const auto& dimension : saved.geometry.dimensions()) {
      // Project format v1 stores geometry positions as vector indices.
      // Internally the sketch now uses stable GeometryId values, so resolve
      // them only at the serialization boundary to keep old project files
      // readable without changing the file-format version.
      qint64 geometryIndex = -1;
      if (dimension.kind == sketch::DimensionKind::LineLength) {
        const auto index = saved.geometry.lineIndex(dimension.geometryId);
        if (!index) continue;
        geometryIndex = static_cast<qint64>(*index);
      } else if (dimension.kind == sketch::DimensionKind::CircleDiameter) {
        const auto index = saved.geometry.circleIndex(dimension.geometryId);
        if (!index) continue;
        geometryIndex = static_cast<qint64>(*index);
      }

      qint64 firstLine = -1;
      qint64 secondLine = -1;
      if (dimension.kind == sketch::DimensionKind::PointDistance ||
          dimension.kind == sketch::DimensionKind::PointDistanceX ||
          dimension.kind == sketch::DimensionKind::PointDistanceY) {
        const auto firstIndex =
            saved.geometry.lineIndex(dimension.firstPoint.lineId);
        const auto secondIndex =
            saved.geometry.lineIndex(dimension.secondPoint.lineId);
        if (!firstIndex || !secondIndex) continue;
        firstLine = static_cast<qint64>(*firstIndex);
        secondLine = static_cast<qint64>(*secondIndex);
      }

      dimensions.append(QJsonObject{
          {"kind", static_cast<int>(dimension.kind)},
          {"geometryIndex", geometryIndex},
          {"firstLine", firstLine},
          {"firstStart", dimension.firstPoint.start},
          {"secondLine", secondLine},
          {"secondStart", dimension.secondPoint.start},
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

      auto linePosition = [&saved](sketch::GeometryId id) -> qint64 {
        const auto index = saved.geometry.lineIndex(id);
        return index ? static_cast<qint64>(*index) : -1;
      };
      auto circlePosition = [&saved](sketch::GeometryId id) -> qint64 {
        const auto index = saved.geometry.circleIndex(id);
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
          if (firstGeometry >= 0) firstKind = QStringLiteral("circle");
        }
      }
      if (constraint.secondGeometry != sketch::kInvalidGeometryId) {
        secondGeometry = linePosition(constraint.secondGeometry);
        if (secondGeometry >= 0) {
          secondKind = QStringLiteral("line");
        } else {
          secondGeometry = circlePosition(constraint.secondGeometry);
          if (secondGeometry >= 0) secondKind = QStringLiteral("circle");
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

      constraints.append(QJsonObject{
          {"id", static_cast<qint64>(constraint.id)},
          {"type", static_cast<int>(constraint.type)},
          {"firstGeometryKind", firstKind},
          {"firstGeometry", firstGeometry},
          {"secondGeometryKind", secondKind},
          {"secondGeometry", secondGeometry},
          {"firstPointLine", firstPointLine},
          {"firstPointStart", constraint.firstPoint.start},
          {"firstPointCircle", firstPointCircle},
          {"secondPointLine", secondPointLine},
          {"secondPointStart", constraint.secondPoint.start},
          {"secondPointCircle", secondPointCircle},
          {"value", constraint.value}});
    }

    sketches.append(QJsonObject{{"support", saved.support},
                                {"lines", lines},
                                {"circles", circles},
                                {"dimensions", dimensions},
                                {"constraints", constraints}});
  }
  root["sketches"] = sketches;
  QJsonObject extrusion{{"enabled", data.hasExtrusion}};
  if (data.extrusionSourceSketch)
    extrusion["sourceSketch"] = static_cast<qint64>(*data.extrusionSourceSketch);
  root["extrusion"] = extrusion;
  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  if (!file.commit()) {
    setError(error, file.errorString());
    return false;
  }
  return true;
}

bool ProjectFile::validate(const QString& path, QString* error) {
  ProjectData unused;
  return load(path, &unused, error);
}

bool ProjectFile::load(const QString& path, ProjectData* data, QString* error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    setError(error, file.errorString());
    return false;
  }
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    setError(error, QString::fromUtf8("Файл проекта повреждён: ") +
                        parseError.errorString());
    return false;
  }
  const auto root = document.object();
  if (root.value("format").toString() != "solidar-project" ||
      root.value("version").toInt() != 1) {
    setError(error, QString::fromUtf8("Неподдерживаемый формат проекта."));
    return false;
  }
  if (!data) {
    setError(error, QString::fromUtf8("Не задано хранилище для данных проекта."));
    return false;
  }
  ProjectData loaded;
  const auto object = root.value("document").toObject();
  loaded.box = {object.value("widthMm").toDouble(60.0),
                object.value("heightMm").toDouble(40.0),
                object.value("extrusionMm").toDouble(25.0)};
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
        saved.geometry.setElementDashed(saved.geometry.lines().back().elementId, true);
    }
    for (const auto circleValue : savedObject.value("circles").toArray()) {
      const auto circle = circleValue.toObject();
      saved.geometry.addCircle({circle.value("x").toDouble(), circle.value("y").toDouble()},
                               circle.value("radius").toDouble());
      if (circle.value("dashed").toBool())
        saved.geometry.setCircleDashed(saved.geometry.circles().size() - 1, true);
    }
    for (const auto dimensionValue : savedObject.value("dimensions").toArray()) {
      const auto object = dimensionValue.toObject();
      const auto kind =
          static_cast<sketch::DimensionKind>(object.value("kind").toInt());

      sketch::Dimension dimension;
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
      } else if (kind == sketch::DimensionKind::PointDistance ||
                 kind == sketch::DimensionKind::PointDistanceX ||
                 kind == sketch::DimensionKind::PointDistanceY) {
        const auto firstIndex =
            static_cast<std::size_t>(object.value("firstLine").toInteger());
        const auto secondIndex =
            static_cast<std::size_t>(object.value("secondLine").toInteger());
        const auto firstId = saved.geometry.lineId(firstIndex);
        const auto secondId = saved.geometry.lineId(secondIndex);
        if (firstId == sketch::kInvalidGeometryId ||
            secondId == sketch::kInvalidGeometryId)
          continue;
        dimension.firstPoint = {
            firstId, object.value("firstStart").toBool(true)};
        dimension.secondPoint = {
            secondId, object.value("secondStart").toBool(true)};
      }

      saved.geometry.storeDimension(dimension);
    }

    for (const auto constraintValue : savedObject.value("constraints").toArray()) {
      const auto object = constraintValue.toObject();
      sketch::Constraint constraint;
      constraint.id =
          static_cast<sketch::ConstraintId>(object.value("id").toInteger());
      constraint.type =
          static_cast<sketch::ConstraintType>(object.value("type").toInt());
      constraint.value = object.value("value").toDouble();

      const auto resolveGeometry =
          [&saved](const QString& kind, qint64 position) -> sketch::GeometryId {
        if (position < 0) return sketch::kInvalidGeometryId;
        const auto index = static_cast<std::size_t>(position);
        if (kind == QStringLiteral("line"))
          return saved.geometry.lineId(index);
        if (kind == QStringLiteral("circle"))
          return saved.geometry.circleId(index);
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

      saved.geometry.addConstraint(constraint);
    }

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

}  // namespace solidar::project
