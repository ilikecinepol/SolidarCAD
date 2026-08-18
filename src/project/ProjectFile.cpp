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
    for (const auto& dimension : saved.geometry.dimensions())
      dimensions.append(QJsonObject{
          {"kind", static_cast<int>(dimension.kind)},
          {"geometryIndex", static_cast<qint64>(dimension.geometryIndex)},
          {"firstLine", static_cast<qint64>(dimension.firstPoint.lineIndex)},
          {"firstStart", dimension.firstPoint.start},
          {"secondLine", static_cast<qint64>(dimension.secondPoint.lineIndex)},
          {"secondStart", dimension.secondPoint.start}, {"value", dimension.valueMm},
          {"offset", dimension.offsetMm}, {"angle", dimension.angleRad}});
    sketches.append(QJsonObject{{"support", saved.support}, {"lines", lines},
                                {"circles", circles}, {"dimensions", dimensions}});
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
      saved.geometry.addLine({line.value("x1").toDouble(), line.value("y1").toDouble()},
                             {line.value("x2").toDouble(), line.value("y2").toDouble()});
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
      const auto dimension = dimensionValue.toObject();
      saved.geometry.storeDimension({
          static_cast<sketch::DimensionKind>(dimension.value("kind").toInt()),
          static_cast<std::size_t>(dimension.value("geometryIndex").toInteger()),
          {static_cast<std::size_t>(dimension.value("firstLine").toInteger()),
           dimension.value("firstStart").toBool(true)},
          {static_cast<std::size_t>(dimension.value("secondLine").toInteger()),
           dimension.value("secondStart").toBool(true)},
          dimension.value("value").toDouble(), dimension.value("offset").toDouble(4.0),
          dimension.value("angle").toDouble()});
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
