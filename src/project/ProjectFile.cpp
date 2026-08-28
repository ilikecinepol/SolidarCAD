#include "project/ProjectFile.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>

#include <utility>

#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"

namespace solidar::project {
namespace {

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
      qint64 secondGeometryIndex = -1;
      if (dimension.kind == sketch::DimensionKind::LineLength) {
        const auto index = saved.geometry.lineIndex(dimension.geometryId);
        if (!index) continue;
        geometryIndex = static_cast<qint64>(*index);
      } else if (dimension.kind == sketch::DimensionKind::CircleDiameter) {
        const auto index = saved.geometry.circleIndex(dimension.geometryId);
        if (!index) continue;
        geometryIndex = static_cast<qint64>(*index);
      } else if (dimension.kind == sketch::DimensionKind::LineAngle) {
        const auto firstIndex = saved.geometry.lineIndex(dimension.geometryId);
        const auto secondIndex =
            saved.geometry.lineIndex(dimension.secondPoint.lineId);
        if (!firstIndex || !secondIndex) continue;
        geometryIndex = static_cast<qint64>(*firstIndex);
        secondGeometryIndex = static_cast<qint64>(*secondIndex);
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
          {"secondGeometryIndex", secondGeometryIndex},
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
          {"firstPointElementCenter",
           static_cast<qint64>(constraint.firstPoint.elementCenterId)},
          {"secondPointLine", secondPointLine},
          {"secondPointStart", constraint.secondPoint.start},
          {"secondPointCircle", secondPointCircle},
          {"secondPointElementCenter",
           static_cast<qint64>(constraint.secondPoint.elementCenterId)},
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

bool ProjectFile::saveDocument(const QString& path, const Document& document,
                               QString* error) {
  ProjectData legacy;
  legacy.box = document.box();
  for (const auto& item : document.sketches())
    legacy.sketches.push_back({item.geometry, QStringLiteral("XY")});
  if (!save(path, legacy, error)) return false;

  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) {
    setError(error, input.errorString());
    return false;
  }
  auto root = QJsonDocument::fromJson(input.readAll()).object();
  input.close();
  root["version"] = 2;

  QJsonArray sketches;
  for (const auto& item : document.sketches()) {
    QJsonObject support{{"type", static_cast<int>(item.support.type)}};
    if (item.support.type == SketchSupportType::Face) {
      support["bodyId"] = static_cast<qint64>(item.support.face.bodyId);
      support["featureId"] = static_cast<qint64>(item.support.face.featureId);
      support["faceIndex"] = static_cast<qint64>(item.support.face.faceIndex);
    }
    sketches.append(QJsonObject{
        {"id", static_cast<qint64>(item.id)},
        {"name", QString::fromStdString(item.name)},
        {"origin", vector3(item.placement.origin.x, item.placement.origin.y,
                           item.placement.origin.z)},
        {"xDirection", vector3(item.placement.xDirection.x,
                               item.placement.xDirection.y,
                               item.placement.xDirection.z)},
        {"yDirection", vector3(item.placement.yDirection.x,
                               item.placement.yDirection.y,
                               item.placement.yDirection.z)},
        {"support", support}});
  }

  QJsonArray bodies;
  for (const auto& body : document.bodies()) {
    QJsonArray features;
    for (const auto& feature : body.features()) {
      QJsonObject saved{{"id", static_cast<qint64>(feature->id())},
                        {"name", QString::fromStdString(feature->name())},
                        {"type", QString::fromStdString(feature->typeName())}};
      if (const auto* extrude =
              dynamic_cast<const ExtrudeFeature*>(feature.get())) {
        saved["sketchId"] = static_cast<qint64>(extrude->profileSketchId());
        saved["lengthMm"] = extrude->lengthMm();
        saved["operation"] = static_cast<int>(extrude->operation());
        saved["reversed"] = extrude->reversed();
      } else if (const auto* revolve =
                     dynamic_cast<const RevolveFeature*>(feature.get())) {
        saved["profileSketchId"] = static_cast<qint64>(revolve->profileSketchId());
        saved["axisType"] = static_cast<int>(revolve->axis().type);
        saved["axisSketchId"] = static_cast<qint64>(revolve->axis().sketchId);
        saved["axisLineId"] = static_cast<qint64>(revolve->axis().lineId);
        saved["angleDeg"] = revolve->angleDeg();
        saved["operation"] = static_cast<int>(revolve->operation());
        saved["reversed"] = revolve->reversed();
      } else if (const auto* pocket =
                     dynamic_cast<const PocketFeature*>(feature.get())) {
        saved["sketchId"] = static_cast<qint64>(pocket->profileSketchId());
        saved["depthMm"] = pocket->depthMm();
      } else if (const auto* fillet =
                     dynamic_cast<const FilletFeature*>(feature.get())) {
        saved["radiusMm"] = fillet->radiusMm();
        QJsonArray edges;
        for (const auto& edge : fillet->edges())
          edges.append(QJsonObject{
              {"bodyId", static_cast<qint64>(edge.bodyId)},
              {"featureId", static_cast<qint64>(edge.featureId)},
              {"edgeIndex", static_cast<qint64>(edge.edgeIndex)}});
        saved["edges"] = edges;
      } else {
        setError(error, QString::fromUtf8("Неподдерживаемый тип фичи: ") +
                            saved.value("type").toString());
        return false;
      }
      features.append(saved);
    }
    bodies.append(QJsonObject{{"id", static_cast<qint64>(body.id())},
                              {"name", QString::fromStdString(body.name())},
                              {"features", features}});
  }
  root["model"] = QJsonObject{{"sketches", sketches}, {"bodies", bodies}};

  QSaveFile output(path);
  if (!output.open(QIODevice::WriteOnly)) {
    setError(error, output.errorString());
    return false;
  }
  output.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  if (!output.commit()) {
    setError(error, output.errorString());
    return false;
  }
  return true;
}

bool ProjectFile::loadDocument(const QString& path, Document* document,
                               QString* error) {
  if (!document) {
    setError(error, QString::fromUtf8("Не задан документ для загрузки."));
    return false;
  }
  ProjectData legacy;
  if (!load(path, &legacy, error)) return false;

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    setError(error, file.errorString());
    return false;
  }
  const auto root = QJsonDocument::fromJson(file.readAll()).object();
  if (root.value("version").toInt() != 2 || !root.contains("model")) {
    setError(error, QString::fromUtf8(
                        "Проект не содержит параметрическую историю v2."));
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
  for (qsizetype index = 0; index < sketchMetadata.size(); ++index) {
    const auto saved = sketchMetadata[index].toObject();
    auto& sketch = loaded.addSketch(
        static_cast<SketchId>(saved.value("id").toInteger()),
        saved.value("name").toString().toStdString(),
        legacy.sketches[static_cast<std::size_t>(index)].geometry);
    sketch.placement.origin = readPoint3(saved.value("origin"), {});
    sketch.placement.xDirection =
        readVector3(saved.value("xDirection"), {1.0, 0.0, 0.0});
    sketch.placement.yDirection =
        readVector3(saved.value("yDirection"), {0.0, 1.0, 0.0});
    const auto support = saved.value("support").toObject();
    sketch.support.type = static_cast<SketchSupportType>(
        support.value("type").toInt(static_cast<int>(SketchSupportType::BasePlane)));
    if (sketch.support.type == SketchSupportType::Face)
      sketch.support.face = {
          static_cast<BodyId>(support.value("bodyId").toInteger()),
          static_cast<FeatureId>(support.value("featureId").toInteger()),
          static_cast<std::size_t>(support.value("faceIndex").toInteger())};
  }

  for (const auto bodyValue : model.value("bodies").toArray()) {
    const auto savedBody = bodyValue.toObject();
    auto& body = loaded.addBody(
        static_cast<BodyId>(savedBody.value("id").toInteger()),
        savedBody.value("name").toString().toStdString());
    for (const auto featureValue : savedBody.value("features").toArray()) {
      const auto saved = featureValue.toObject();
      const auto id = static_cast<FeatureId>(saved.value("id").toInteger());
      const auto name = saved.value("name").toString().toStdString();
      const auto type = saved.value("type").toString();
      if (type == QStringLiteral("Extrude")) {
        body.addFeature(std::make_unique<ExtrudeFeature>(
            id, static_cast<SketchId>(saved.value("sketchId").toInteger()),
            saved.value("lengthMm").toDouble(), name,
            static_cast<ExtrudeOperation>(saved.value("operation").toInt()),
            saved.value("reversed").toBool()));
      } else if (type == QStringLiteral("Revolve")) {
        AxisReference axis{
            static_cast<AxisReferenceType>(saved.value("axisType").toInt()),
            static_cast<SketchId>(saved.value("axisSketchId").toInteger()),
            static_cast<sketch::GeometryId>(saved.value("axisLineId").toInteger())};
        body.addFeature(std::make_unique<RevolveFeature>(
            id, static_cast<SketchId>(saved.value("profileSketchId").toInteger()),
            axis, saved.value("angleDeg").toDouble(360.0), name,
            static_cast<ExtrudeOperation>(saved.value("operation").toInt()),
            saved.value("reversed").toBool()));
      } else if (type == QStringLiteral("Pocket")) {
        body.addFeature(std::make_unique<PocketFeature>(
            id, static_cast<SketchId>(saved.value("sketchId").toInteger()),
            saved.value("depthMm").toDouble(), name));
      } else if (type == QStringLiteral("Fillet")) {
        std::vector<EdgeReference> edges;
        for (const auto edgeValue : saved.value("edges").toArray()) {
          const auto edge = edgeValue.toObject();
          edges.push_back({
              static_cast<BodyId>(edge.value("bodyId").toInteger()),
              static_cast<FeatureId>(edge.value("featureId").toInteger()),
              static_cast<std::size_t>(edge.value("edgeIndex").toInteger())});
        }
        body.addFeature(std::make_unique<FilletFeature>(
            id, std::move(edges), saved.value("radiusMm").toDouble(), name));
      } else {
        setError(error, QString::fromUtf8("Неизвестный тип фичи: ") + type);
        return false;
      }
    }
  }
  if (!loaded.recompute()) {
    setError(error, QString::fromUtf8("Не удалось перестроить проект: ") +
                        QString::fromStdString(
                            loaded.activeBody() && loaded.activeBody()->activeFeature()
                                ? loaded.activeBody()->activeFeature()->error()
                                : std::string{}));
    return false;
  }
  *document = std::move(loaded);
  return true;
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
  const int version = root.value("version").toInt();
  if (root.value("format").toString() != "solidar-project" ||
      (version != 1 && version != 2)) {
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
      } else if (kind == sketch::DimensionKind::LineAngle) {
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

      const qint64 firstPointElementCenter =
          object.value("firstPointElementCenter").toInteger(0);
      if (firstPointElementCenter > 0) {
        constraint.firstPoint.elementCenterId =
            static_cast<std::size_t>(
                firstPointElementCenter);
      }

      const qint64 secondPointElementCenter =
          object.value("secondPointElementCenter").toInteger(0);
      if (secondPointElementCenter > 0) {
        constraint.secondPoint.elementCenterId =
            static_cast<std::size_t>(
                secondPointElementCenter);
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
