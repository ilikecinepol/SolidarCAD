#include "TestAssertions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <concepts>
#include <cstdlib>
#include <utility>
#include <vector>

#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4LegacySketchValidator.h"
#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/ProjectFile.h"
#include "project/internal/ProjectFormatV3TypedSketchValidation.h"

namespace {

using solidar::project::ProjectFormatV4LegacySketchCandidate;
using solidar::project::ProjectFormatV4LegacySketchValidationResult;
using solidar::project::ProjectFormatV4LegacySketchValidationStatus;

template <typename T>
concept HasSucceeded = requires(const T& value) { value.succeeded(); };

template <typename T>
concept HasSuccessMember = requires(const T& value) { value.success; };

template <typename T>
concept HasDocumentMember = requires(const T& value) { value.document; };

template <typename T>
concept HasLegacyMember = requires(const T& value) { value.legacy; };

template <typename T>
concept HasSketchMember = requires(const T& value) { value.sketch; };

static_assert(!HasSucceeded<ProjectFormatV4LegacySketchValidationResult>);
static_assert(!HasSuccessMember<ProjectFormatV4LegacySketchValidationResult>);
static_assert(!HasDocumentMember<ProjectFormatV4LegacySketchValidationResult>);
static_assert(!HasLegacyMember<ProjectFormatV4LegacySketchValidationResult>);
static_assert(!HasSketchMember<ProjectFormatV4LegacySketchValidationResult>);
static_assert(!HasSucceeded<ProjectFormatV4LegacySketchCandidate>);
static_assert(!HasSuccessMember<ProjectFormatV4LegacySketchCandidate>);
static_assert(!HasDocumentMember<ProjectFormatV4LegacySketchCandidate>);
static_assert(!HasLegacyMember<ProjectFormatV4LegacySketchCandidate>);
static_assert(!HasSketchMember<ProjectFormatV4LegacySketchCandidate>);

QJsonObject baseRoot() {
  QJsonObject root{
      {"format", "solidar-project"},
      {"version", 4},
      {"capabilities", QJsonArray{"persistent-topology-v2"}},
      {"constraintTypeEncoding", "stable-name-v1"},
      {"name", "legacy-sketch-fixture"},
      {"createdAt", "2026-10-21T00:00:00Z"},
      {"document",
       QJsonObject{{"widthMm", 60}, {"heightMm", 40}, {"extrusionMm", 25}}},
      {"sketches", QJsonArray{}},
      {"extrusion", QJsonObject{{"enabled", false}}}};
  root["model"] = QJsonObject{{"sketches", QJsonArray{}},
                               {"bodies", QJsonArray{}},
                               {"referenceImages", QJsonArray{}}};
  return root;
}

QJsonObject modelSketch(qint64 id) {
  return QJsonObject{{"id", id},
                     {"name", "sketch"},
                     {"origin", QJsonArray{0, 0, 0}},
                     {"xDirection", QJsonArray{1, 0, 0}},
                     {"yDirection", QJsonArray{0, 1, 0}},
                     {"support", QJsonObject{{"type", 0}}}};
}

QJsonObject emptyLegacySketch(qint64 id) {
  return QJsonObject{
      {"support", "XY"},
      {"lines", QJsonArray{}},
      {"circles", QJsonArray{}},
      {"arcs", QJsonArray{}},
      {"beziers", QJsonArray{}},
      {"dimensions", QJsonArray{}},
      {"constraints", QJsonArray{}},
      {"centerNodeElementIds", QJsonArray{}},
      {"sketchId", id},
      {"idAllocators", QJsonObject{{"nextElementId", 1},
                                    {"nextGeometryId", 1},
                                    {"nextConstraintId", 1},
                                    {"nextDimensionId", 1}}},
      {"schema", "typed-sketch-v1"}};
}

QJsonObject line(qint64 id, double x1 = 0.0, double y1 = 0.0,
                 double x2 = 10.0, double y2 = 0.0) {
  return QJsonObject{{"x1", x1},
                     {"y1", y1},
                     {"x2", x2},
                     {"y2", y2},
                     {"elementId", id},
                     {"dashed", false},
                     {"id", id}};
}

QJsonObject lineWithElement(qint64 id, qint64 elementId, double y) {
  return QJsonObject{{"x1", 0.0},
                     {"y1", y},
                     {"x2", 10.0},
                     {"y2", y},
                     {"elementId", elementId},
                     {"dashed", false},
                     {"id", id}};
}

QJsonObject lengthConstraint(qint64 id = 1, qint64 geometryId = 1) {
  return QJsonObject{
      {"id", id},
      {"mode", "driving"},
      {"payload", QJsonObject{{"type", 5},
                              {"typeKey", "Length"},
                              {"geometryId", geometryId},
                              {"value", 10.0}}}};
}

QJsonObject circle(qint64 id, double x) {
  return QJsonObject{{"x", x},
                     {"y", 5.0},
                     {"radius", 2.0},
                     {"dashed", false},
                     {"id", id}};
}

QJsonObject arc(qint64 id, double x) {
  return QJsonObject{{"x", x},
                     {"y", 8.0},
                     {"radius", 2.0},
                     {"startAngle", 0.0},
                     {"sweepAngle", 1.0},
                     {"dashed", false},
                     {"id", id}};
}

QJsonObject bezier(qint64 id, double y) {
  return QJsonObject{{"x0", 0.0},
                     {"y0", y},
                     {"x1", 2.0},
                     {"y1", y + 1.0},
                     {"x2", 8.0},
                     {"y2", y + 1.0},
                     {"x3", 10.0},
                     {"y3", y},
                     {"dashed", false},
                     {"id", id}};
}

QJsonObject datumOrigin() {
  return QJsonObject{{"source", "datum-origin"}};
}

QJsonObject compositeCenter(qint64 elementId = 100) {
  return QJsonObject{{"source", "composite-center"},
                     {"elementId", elementId}};
}

QJsonObject circleCenter(qint64 geometryId = 3) {
  return QJsonObject{{"source", "circle-center"},
                     {"geometryId", geometryId}};
}

QJsonObject bezierPoint(qint64 geometryId = 7, qint64 pointIndex = 2) {
  return QJsonObject{{"source", "bezier-point"},
                     {"geometryId", geometryId},
                     {"pointIndex", pointIndex}};
}

QJsonObject lineEndpoint(qint64 geometryId = 1,
                         const char* endpoint = "start") {
  return QJsonObject{{"source", "line-endpoint"},
                     {"geometryId", geometryId},
                     {"endpoint", endpoint}};
}

QJsonObject arcEndpoint(qint64 geometryId = 5,
                        const char* endpoint = "start") {
  return QJsonObject{{"source", "arc-endpoint"},
                     {"geometryId", geometryId},
                     {"endpoint", endpoint}};
}

QJsonObject typedConstraint(qint64 id, int ordinal, const char* typeKey,
                            const char* mode = "driving") {
  QJsonObject payload{{"type", ordinal}, {"typeKey", typeKey}};
  const bool reference = QString::fromLatin1(mode) == QStringLiteral("reference");
  switch (ordinal) {
    case 0:
      payload["geometryId"] = 1;
      break;
    case 1:
      payload["geometryId"] = 2;
      break;
    case 5:
      payload["geometryId"] = 1;
      if (!reference) payload["value"] = 10.0;
      break;
    case 6:
    case 7:
      payload["geometryId"] = 3;
      if (!reference) payload["value"] = 2.0;
      break;
    case 17:
      payload["geometryId"] = 7;
      break;
    case 8:
    case 9:
    case 10:
      payload["firstGeometryId"] = 1;
      payload["secondGeometryId"] = 2;
      break;
    case 11:
      payload["firstGeometryId"] = 1;
      payload["secondGeometryId"] = 2;
      if (!reference) payload["value"] = 90.0;
      break;
    case 15:
      payload["firstGeometryId"] = 1;
      payload["secondGeometryId"] = 3;
      break;
    case 16:
      payload["firstGeometryId"] = 1;
      payload["secondGeometryId"] = 2;
      if (!reference) payload["value"] = 5.0;
      break;
    case 2:
      payload["firstPoint"] = lineEndpoint(1, "start");
      payload["secondPoint"] = lineEndpoint(1, "end");
      break;
    case 4:
      payload["firstPoint"] = datumOrigin();
      payload["secondPoint"] = compositeCenter();
      if (!reference) payload["value"] = 5.0;
      break;
    case 12:
      payload["firstPoint"] = circleCenter();
      payload["secondPoint"] = bezierPoint();
      if (!reference) payload["value"] = 5.0;
      break;
    case 13:
      payload["firstPoint"] = arcEndpoint();
      payload["secondPoint"] = lineEndpoint(2, "end");
      if (!reference) payload["value"] = 5.0;
      break;
    case 3:
      payload["geometryId"] = 1;
      payload["point"] = circleCenter();
      break;
    case 14:
      payload["geometryId"] = 3;
      payload["point"] = lineEndpoint();
      break;
    case 18:
      payload["geometryId"] = 5;
      payload["point"] = arcEndpoint(6, "end");
      break;
    case 19:
      payload["geometryId"] = 1;
      payload["point"] = bezierPoint();
      break;
    case 20:
      payload["point"] = compositeCenter();
      break;
    case 21:
      payload["point"] = datumOrigin();
      break;
    default:
      break;
  }
  return QJsonObject{{"id", id}, {"mode", mode}, {"payload", payload}};
}

QJsonObject completeTypedSketch(qint64 id = 1) {
  static constexpr std::array<const char*, 22> kTypeKeys{
      "Horizontal",   "Vertical",     "Coincident",   "PointOnLine",
      "Distance",     "Length",       "Radius",       "Diameter",
      "Parallel",     "Perpendicular", "Equal",       "Angle",
      "DistanceX",    "DistanceY",    "PointOnCircle", "Tangent",
      "LineDistance", "Lock",         "PointOnArc",   "Midpoint",
      "PointOnXAxis", "PointOnYAxis"};
  QJsonObject sketch = emptyLegacySketch(id);
  sketch["lines"] =
      QJsonArray{lineWithElement(1, 100, 0.0),
                 lineWithElement(2, 100, 1.0),
                 lineWithElement(9, 100, 2.0),
                 lineWithElement(10, 100, 3.0)};
  sketch["circles"] = QJsonArray{circle(3, 0.0), circle(4, 10.0)};
  sketch["arcs"] = QJsonArray{arc(5, 0.0), arc(6, 10.0)};
  sketch["beziers"] = QJsonArray{bezier(7, 12.0), bezier(8, 15.0)};
  sketch["centerNodeElementIds"] = QJsonArray{100};
  QJsonArray constraints;
  qint64 constraintId = 1;
  for (int ordinal = 0; ordinal < static_cast<int>(kTypeKeys.size()); ++ordinal)
    constraints.append(
        typedConstraint(constraintId++, ordinal, kTypeKeys[ordinal]));
  constraints.append(typedConstraint(constraintId++, 5, "Length", "reference"));
  constraints.append(typedConstraint(constraintId++, 5, "Length", "disabled"));
  sketch["constraints"] = constraints;
  sketch["dimensions"] =
      QJsonArray{QJsonObject{{"id", 1},
                             {"constraintId", 6},
                             {"offset", 4.0},
                             {"angle", 0.0}}};
  sketch["idAllocators"] = QJsonObject{{"nextElementId", 101},
                                        {"nextGeometryId", 11},
                                        {"nextConstraintId", constraintId},
                                        {"nextDimensionId", 2}};
  return sketch;
}

QJsonObject validTypedSketch(qint64 id = 1) {
  QJsonObject sketch = emptyLegacySketch(id);
  sketch["lines"] = QJsonArray{line(1)};
  sketch["constraints"] = QJsonArray{lengthConstraint()};
  sketch["dimensions"] =
      QJsonArray{QJsonObject{{"id", 1},
                             {"constraintId", 1},
                             {"offset", 4.0},
                             {"angle", 0.0}}};
  sketch["idAllocators"] = QJsonObject{{"nextElementId", 2},
                                        {"nextGeometryId", 2},
                                        {"nextConstraintId", 2},
                                        {"nextDimensionId", 2}};
  return sketch;
}

void setSketches(QJsonObject* root, const QJsonArray& legacy,
                 const QJsonArray& model) {
  (*root)["sketches"] = legacy;
  QJsonObject modelObject = root->value("model").toObject();
  modelObject["sketches"] = model;
  (*root)["model"] = modelObject;
}

solidar::project::ProjectFormatV4PayloadCandidate payloadFor(
    const QJsonObject& root) {
  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  auto envelope = solidar::project::scanProjectFormatV4Envelope(bytes);
  CHECK(envelope.hasCandidate());
  auto payload = solidar::project::scanProjectFormatV4Payload(
      std::move(*envelope.candidate));
  CHECK(payload.hasCandidate());
  return std::move(*payload.candidate);
}

ProjectFormatV4LegacySketchValidationResult validate(const QJsonObject& root) {
  return solidar::project::validateProjectFormatV4LegacySketches(
      payloadFor(root));
}

void checkCandidate(const ProjectFormatV4LegacySketchValidationResult& result) {
  CHECK(result.status ==
        ProjectFormatV4LegacySketchValidationStatus::Candidate);
  CHECK(result.hasCandidate());
  CHECK(result.candidate.has_value());
  CHECK(!result.candidate->deferredLegacySketch.has_value());
  CHECK(result.error.isEmpty());
}

void checkUnsupported(
    const ProjectFormatV4LegacySketchValidationResult& result) {
  CHECK(result.status ==
        ProjectFormatV4LegacySketchValidationStatus::UnsupportedCandidate);
  CHECK(result.hasCandidate());
  CHECK(result.candidate.has_value());
  CHECK(result.error.isEmpty());
}

void checkInvalid(const ProjectFormatV4LegacySketchValidationResult& result) {
  CHECK(result.status ==
        ProjectFormatV4LegacySketchValidationStatus::Invalid);
  CHECK(!result.hasCandidate());
  CHECK(!result.candidate.has_value());
  CHECK(!result.error.isEmpty());
}

void checkInvalidDeterministic(const QJsonObject& root) {
  const auto first = validate(root);
  const auto second = validate(root);
  checkInvalid(first);
  checkInvalid(second);
  CHECK(first.error == second.error);
}

}  // namespace

int main() {
  // Empty and representative typed sketches remain staged candidates. The
  // authored root survives the P2d composition byte-for-byte at JSON level.
  {
    QJsonObject root = baseRoot();
    checkCandidate(validate(root));

    setSketches(&root, QJsonArray{validTypedSketch()},
                QJsonArray{modelSketch(1)});
    const auto result = validate(root);
    checkCandidate(result);
    CHECK(result.candidate->topology.payload.envelope.authoredRoot == root);
  }

  // Hand-authored registry coverage exercises all 22 stable typeKey/ordinal
  // pairs, all four geometry kinds, all six point-reference variants, all five
  // payload shapes and driving/reference/disabled modes without using writer
  // output as an oracle.
  {
    QJsonObject root = baseRoot();
    setSketches(&root, QJsonArray{completeTypedSketch()},
                QJsonArray{modelSketch(1)});
    checkCandidate(validate(root));
  }

  // Radius is dimensional in the typed sketch schema and may own a v4
  // annotation exactly like Diameter. Keep this hand-authored so the writer
  // cannot accidentally serve as the validator oracle.
  {
    QJsonObject root = baseRoot();
    QJsonObject sketch = emptyLegacySketch(1);
    sketch["circles"] = QJsonArray{circle(3, 0.0)};
    sketch["constraints"] =
        QJsonArray{typedConstraint(1, 6, "Radius")};
    sketch["dimensions"] =
        QJsonArray{QJsonObject{{"id", 1},
                               {"constraintId", 1},
                               {"offset", 6.0},
                               {"angle", 0.75}}};
    sketch["idAllocators"] = QJsonObject{{"nextElementId", 1},
                                           {"nextGeometryId", 4},
                                           {"nextConstraintId", 2},
                                           {"nextDimensionId", 2}};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkCandidate(validate(root));
  }

  // Geometry JSON types, numeric bounds and degeneracy are validated before
  // any scratch sketch is accepted.
  {
    QJsonObject root = baseRoot();
    QJsonObject sketch = validTypedSketch();
    QJsonObject badLine = line(1);
    badLine["x1"] = "not-a-number";
    sketch["lines"] = QJsonArray{badLine};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    badLine = line(1);
    badLine["x2"] = 1000001.0;
    sketch["lines"] = QJsonArray{badLine};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    badLine = line(1, 0.0, 0.0, 0.0, 0.0);
    sketch["lines"] = QJsonArray{badLine};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);
  }

  // Stable-name validation is two-pass: unsupported keys are remembered while
  // all later constraints, sketches, annotations and cursors are still checked.
  {
    const auto unknownConstraint = [] {
      QJsonObject unknown = lengthConstraint();
      QJsonObject payload = unknown.value("payload").toObject();
      payload["type"] = 999;
      payload["typeKey"] = "FutureConstraint";
      unknown["payload"] = payload;
      return unknown;
    };
    const auto malformedKnown = [] {
      QJsonObject malformed = lengthConstraint(2);
      QJsonObject payload = malformed.value("payload").toObject();
      payload.remove("geometryId");
      malformed["payload"] = payload;
      return malformed;
    };

    QJsonObject root = baseRoot();
    QJsonObject sketch = validTypedSketch();
    sketch["constraints"] = QJsonArray{unknownConstraint()};
    sketch["dimensions"] = QJsonArray{};
    sketch["idAllocators"] = QJsonObject{{"nextElementId", 2},
                                          {"nextGeometryId", 2},
                                          {"nextConstraintId", 2},
                                          {"nextDimensionId", 1}};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkUnsupported(validate(root));

    sketch["constraints"] = QJsonArray{malformedKnown()};
    sketch["idAllocators"] = QJsonObject{{"nextElementId", 2},
                                          {"nextGeometryId", 2},
                                          {"nextConstraintId", 3},
                                          {"nextDimensionId", 1}};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    sketch["constraints"] =
        QJsonArray{unknownConstraint(), malformedKnown()};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    sketch["constraints"] = QJsonArray{unknownConstraint()};
    sketch["dimensions"] =
        QJsonArray{QJsonObject{{"id", 1},
                               {"constraintId", 99},
                               {"offset", 0.0},
                               {"angle", 0.0}}};
    sketch["idAllocators"] = QJsonObject{{"nextElementId", 2},
                                          {"nextGeometryId", 2},
                                          {"nextConstraintId", 2},
                                          {"nextDimensionId", 2}};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    sketch["dimensions"] = QJsonArray{};
    sketch["idAllocators"] = QJsonObject{{"nextElementId", 2},
                                          {"nextGeometryId", 2},
                                          {"nextConstraintId", 1},
                                          {"nextDimensionId", 1}};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    QJsonObject firstSketch = emptyLegacySketch(1);
    firstSketch["constraints"] = QJsonArray{unknownConstraint()};
    firstSketch["idAllocators"] =
        QJsonObject{{"nextElementId", 1},
                    {"nextGeometryId", 1},
                    {"nextConstraintId", 2},
                    {"nextDimensionId", 1}};
    QJsonObject secondSketch = validTypedSketch(2);
    secondSketch["constraints"] = QJsonArray{malformedKnown()};
    secondSketch["dimensions"] = QJsonArray{};
    secondSketch["idAllocators"] =
        QJsonObject{{"nextElementId", 2},
                    {"nextGeometryId", 2},
                    {"nextConstraintId", 3},
                    {"nextDimensionId", 1}};
    setSketches(&root, QJsonArray{firstSketch, secondSketch},
                QJsonArray{modelSketch(1), modelSketch(2)});
    checkInvalidDeterministic(root);
  }

  // Duplicate stable IDs, occupied allocator cursors and stale annotations
  // are rejected without publishing a partial candidate.
  {
    QJsonObject root = baseRoot();
    QJsonObject sketch = validTypedSketch();
    sketch["lines"] = QJsonArray{line(1), line(1, 0.0, 1.0, 10.0, 1.0)};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    sketch = validTypedSketch();
    sketch["constraints"] =
        QJsonArray{lengthConstraint(1), lengthConstraint(1)};
    QJsonObject allocators = sketch.value("idAllocators").toObject();
    allocators["nextConstraintId"] = 2;
    sketch["idAllocators"] = allocators;
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    sketch = validTypedSketch();
    QJsonObject duplicateAnnotation =
        sketch.value("dimensions").toArray().at(0).toObject();
    duplicateAnnotation["id"] = 2;
    sketch["dimensions"] =
        QJsonArray{sketch.value("dimensions").toArray().at(0),
                   duplicateAnnotation};
    allocators = sketch.value("idAllocators").toObject();
    allocators["nextDimensionId"] = 3;
    sketch["idAllocators"] = allocators;
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    sketch = validTypedSketch();
    for (const char* key : {"nextElementId", "nextGeometryId",
                            "nextConstraintId", "nextDimensionId"}) {
      QJsonObject occupied = sketch;
      allocators = occupied.value("idAllocators").toObject();
      allocators[QLatin1String(key)] = 1;
      occupied["idAllocators"] = allocators;
      setSketches(&root, QJsonArray{occupied}, QJsonArray{modelSketch(1)});
      checkInvalidDeterministic(root);
    }

    sketch = validTypedSketch();
    QJsonArray dimensions = sketch.value("dimensions").toArray();
    QJsonObject annotation = dimensions.at(0).toObject();
    annotation["constraintId"] = 99;
    sketch["dimensions"] = QJsonArray{annotation};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);
  }

  // Every allocator domain accepts the largest persisted ID minus one with
  // the maximum cursor, then rejects the occupied maximum cursor itself.
  {
    constexpr qint64 maximum =
        solidar::project::ProjectFile::kMaximumPersistedId;
    const auto boundarySketch = [maximum](int domain, qint64 occupiedId) {
      QJsonObject sketch = validTypedSketch();
      QJsonObject authoredLine = line(1);
      QJsonObject constraint = lengthConstraint();
      QJsonObject annotation =
          sketch.value("dimensions").toArray().at(0).toObject();
      if (domain == 0)
        authoredLine["elementId"] = occupiedId;
      else if (domain == 1) {
        authoredLine["id"] = occupiedId;
        QJsonObject payload = constraint.value("payload").toObject();
        payload["geometryId"] = occupiedId;
        constraint["payload"] = payload;
      } else if (domain == 2) {
        constraint["id"] = occupiedId;
        annotation["constraintId"] = occupiedId;
      } else {
        annotation["id"] = occupiedId;
      }
      sketch["lines"] = QJsonArray{authoredLine};
      sketch["constraints"] = QJsonArray{constraint};
      sketch["dimensions"] = QJsonArray{annotation};
      QJsonObject allocators = sketch.value("idAllocators").toObject();
      const std::array<const char*, 4> keys{
          "nextElementId", "nextGeometryId", "nextConstraintId",
          "nextDimensionId"};
      allocators[QLatin1String(keys[static_cast<std::size_t>(domain)])] =
          maximum;
      sketch["idAllocators"] = allocators;
      return sketch;
    };
    for (int domain = 0; domain < 4; ++domain) {
      QJsonObject root = baseRoot();
      setSketches(&root, QJsonArray{boundarySketch(domain, maximum - 1)},
                  QJsonArray{modelSketch(1)});
      checkCandidate(validate(root));

      setSketches(&root, QJsonArray{boundarySketch(domain, maximum)},
                  QJsonArray{modelSketch(1)});
      checkInvalidDeterministic(root);
    }
  }

  // A future type under the known encoding follows current v3 behavior: the
  // authored candidate is preserved but its typed sketch is deferred.
  {
    QJsonObject root = baseRoot();
    QJsonObject sketch = validTypedSketch();
    QJsonObject unknown = lengthConstraint();
    QJsonObject payload = unknown.value("payload").toObject();
    payload["type"] = 999;
    payload["typeKey"] = "FutureConstraint";
    unknown["payload"] = payload;
    sketch["constraints"] = QJsonArray{unknown};
    sketch["dimensions"] = QJsonArray{};
    sketch["idAllocators"] = QJsonObject{{"nextElementId", 2},
                                          {"nextGeometryId", 2},
                                          {"nextConstraintId", 2},
                                          {"nextDimensionId", 1}};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    const auto result = validate(root);
    checkUnsupported(result);
    CHECK(result.candidate->deferredLegacySketch.has_value());
    CHECK(result.candidate->deferredLegacySketch->path == "$.sketches");
    CHECK(result.candidate->deferredLegacySketch->diagnostic.contains(
        "FutureConstraint"));
    CHECK(result.candidate->topology.payload.envelope.authoredRoot == root);
  }

  // Even for an unknown stable key, the common type fields remain bounded
  // and structurally valid. Invalid wins over the deferred unknown type.
  {
    const auto rootWithType = [](const QJsonValue& type, bool includeType) {
      QJsonObject root = baseRoot();
      QJsonObject sketch = validTypedSketch();
      QJsonObject constraint = lengthConstraint();
      QJsonObject payload = constraint.value("payload").toObject();
      payload["typeKey"] = "FutureConstraint";
      if (includeType)
        payload["type"] = type;
      else
        payload.remove("type");
      constraint["payload"] = payload;
      sketch["constraints"] = QJsonArray{constraint};
      sketch["dimensions"] = QJsonArray{};
      QJsonObject allocators = sketch.value("idAllocators").toObject();
      allocators["nextDimensionId"] = 1;
      sketch["idAllocators"] = allocators;
      setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
      return root;
    };
    checkInvalidDeterministic(rootWithType({}, false));
    checkInvalidDeterministic(rootWithType("999", true));
    checkInvalidDeterministic(rootWithType(999.5, true));
    checkInvalidDeterministic(rootWithType(-1, true));
    checkInvalidDeterministic(
        rootWithType(9007199254740992.0, true));

    QJsonObject root = rootWithType(999, true);
    QJsonArray sketches = root.value("sketches").toArray();
    QJsonObject sketch = sketches.at(0).toObject();
    QJsonArray constraints = sketch.value("constraints").toArray();
    QJsonObject constraint = constraints.at(0).toObject();
    QJsonObject payload = constraint.value("payload").toObject();
    payload["typeKey"] = QString(QChar(0xd800));
    constraint["payload"] = payload;
    constraints[0] = constraint;
    sketch["constraints"] = constraints;
    sketches[0] = sketch;
    root["sketches"] = sketches;
    const auto malformedUtf16 =
        solidar::project::detail::validateProjectFormatV3TypedSketches(
            root,
            solidar::project::detail::
                ProjectFormatV3TypedSketchValidationMode::StableNameV1);
    CHECK(malformedUtf16.status ==
          solidar::project::detail::
              ProjectFormatV3TypedSketchValidationStatus::Invalid);
    CHECK(!malformedUtf16.diagnostic.isEmpty());
  }

  // Unknown encoding keeps a genuinely future constraint body opaque. Common
  // envelopes, annotations, cursors and geometry are still fully checked.
  {
    QJsonObject root = baseRoot();
    root["constraintTypeEncoding"] = "future-name-v2";
    QJsonObject future = validTypedSketch();
    future["constraints"] =
        QJsonArray{QJsonObject{{"id", 1},
                               {"mode", "driving"},
                               {"payload",
                                QJsonObject{{"futureShape",
                                             QJsonArray{1, "two"}}}}}};
    setSketches(&root, QJsonArray{future}, QJsonArray{modelSketch(1)});
    const auto clean = validate(root);
    checkUnsupported(clean);
    CHECK(!clean.candidate->deferredLegacySketch.has_value());
    CHECK(!clean.candidate->topology.deferredUnsupported.empty());

    QJsonObject sketch = future;
    QJsonObject malformedEnvelope =
        sketch.value("constraints").toArray().at(0).toObject();
    malformedEnvelope["extra"] = true;
    sketch["constraints"] = QJsonArray{malformedEnvelope};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    sketch = future;
    QJsonObject malformedAnnotation =
        sketch.value("dimensions").toArray().at(0).toObject();
    malformedAnnotation["angle"] = "not-a-number";
    sketch["dimensions"] = QJsonArray{malformedAnnotation};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    sketch = future;
    QJsonObject allocators = sketch.value("idAllocators").toObject();
    allocators["nextConstraintId"] = 1;
    sketch["idAllocators"] = allocators;
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);

    sketch = future;
    QJsonObject badLine = line(1);
    badLine["radius"] = 5;
    badLine["x1"] = QJsonArray{};
    sketch["lines"] = QJsonArray{badLine};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);
  }

  // An unrelated unknown capability never masks a malformed stable sketch;
  // the same upstream deferral is preserved for a clean sketch.
  {
    QJsonObject root = baseRoot();
    root["capabilities"] =
        QJsonArray{"future-capability", "persistent-topology-v2"};
    setSketches(&root, QJsonArray{validTypedSketch()},
                QJsonArray{modelSketch(1)});
    checkUnsupported(validate(root));

    QJsonObject sketch = validTypedSketch();
    QJsonObject badConstraint = lengthConstraint();
    QJsonObject payload = badConstraint.value("payload").toObject();
    payload["typeKey"] = "Radius";
    badConstraint["payload"] = payload;
    sketch["constraints"] = QJsonArray{badConstraint};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    checkInvalidDeterministic(root);
  }

  // Root legacy projection and model sketches must have the same authored
  // count, order and ID correspondence.
  {
    QJsonObject root = baseRoot();
    setSketches(&root, QJsonArray{emptyLegacySketch(1)}, QJsonArray{});
    const auto countMismatch = validate(root);
    checkInvalid(countMismatch);
    CHECK(countMismatch.error ==
          "$.model.sketches: count does not match $.sketches");

    setSketches(&root,
                QJsonArray{emptyLegacySketch(1), emptyLegacySketch(2)},
                QJsonArray{modelSketch(2), modelSketch(1)});
    const auto orderMismatch = validate(root);
    checkInvalid(orderMismatch);
    CHECK(orderMismatch.error ==
          "$.model.sketches[0].id: does not match "
          "$.sketches[0].sketchId");
  }

  // Existing v3 reader permissiveness for extra geometry fields is retained.
  {
    QJsonObject root = baseRoot();
    QJsonObject sketch = validTypedSketch();
    QJsonObject authoredLine = line(1);
    authoredLine["futureIgnoredField"] = QJsonObject{{"opaque", true}};
    sketch["lines"] = QJsonArray{authoredLine};
    setSketches(&root, QJsonArray{sketch}, QJsonArray{modelSketch(1)});
    const auto result = validate(root);
    checkCandidate(result);
    CHECK(result.candidate->topology.payload.envelope.authoredRoot == root);
  }

  return EXIT_SUCCESS;
}
