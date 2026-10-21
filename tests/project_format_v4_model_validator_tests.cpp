#include "TestAssertions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

#include <concepts>
#include <utility>

#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4ModelValidator.h"
#include "project/ProjectFormatV4PayloadScanner.h"

namespace {

using solidar::project::ProjectFormatV4ModelCandidate;
using solidar::project::ProjectFormatV4ModelValidationResult;
using solidar::project::ProjectFormatV4ModelValidationStatus;

template <typename T>
concept HasDocument = requires(T& value) { value.document; };
template <typename T>
concept HasSuccess = requires(T& value) { value.success; };
static_assert(!HasDocument<ProjectFormatV4ModelCandidate>);
static_assert(!HasDocument<ProjectFormatV4ModelValidationResult>);
static_assert(!HasSuccess<ProjectFormatV4ModelValidationResult>);

QJsonObject legacySketch(qint64 id = 900) {
  auto line = [](qint64 id, double y) {
    return QJsonObject{{"x1", 0}, {"y1", y}, {"x2", 10}, {"y2", y},
                       {"elementId", id}, {"dashed", false}, {"id", id}};
  };
  QJsonObject secondLine = line(2, 1);
  secondLine["elementId"] = 77;
  return QJsonObject{
      {"support", "XY"},
      {"lines", QJsonArray{line(1, 0), secondLine}},
      {"circles", QJsonArray{}}, {"arcs", QJsonArray{}},
      {"beziers", QJsonArray{}}, {"dimensions", QJsonArray{}},
      {"constraints", QJsonArray{}}, {"centerNodeElementIds", QJsonArray{}},
      {"sketchId", id},
      {"idAllocators", QJsonObject{{"nextElementId", 78},
                                    {"nextGeometryId", 3},
                                    {"nextConstraintId", 1},
                                    {"nextDimensionId", 1}}},
      {"schema", "typed-sketch-v1"}};
}

QJsonObject modelSketch(qint64 id = 900) {
  return QJsonObject{{"id", id}, {"name", "sketch"},
                     {"origin", QJsonArray{0, 0, 0}},
                     {"xDirection", QJsonArray{1, 0, 0}},
                     {"yDirection", QJsonArray{0, 1, 0}},
                     {"support", QJsonObject{{"type", 0}}}};
}

QJsonObject root() {
  return QJsonObject{
      {"format", "solidar-project"}, {"version", 4},
      {"capabilities", QJsonArray{"persistent-topology-v2"}},
      {"constraintTypeEncoding", "stable-name-v1"}, {"name", "model"},
      {"createdAt", "2026-10-21T00:00:00Z"},
      {"document", QJsonObject{{"widthMm", 60}, {"heightMm", 40},
                                {"extrusionMm", 25}}},
      {"sketches", QJsonArray{legacySketch()}},
      {"extrusion", QJsonObject{{"enabled", false}}},
      {"model", QJsonObject{{"sketches", QJsonArray{modelSketch()}},
                              {"bodies", QJsonArray{}},
                              {"referenceImages", QJsonArray{}}}}};
}

QJsonObject body(qint64 id, QJsonArray features) {
  return QJsonObject{{"id", id}, {"name", "body"}, {"visible", true},
                     {"features", features}};
}

void setBodies(QJsonObject* value, QJsonArray bodies) {
  QJsonObject model = value->value("model").toObject();
  model["bodies"] = std::move(bodies);
  (*value)["model"] = model;
}

QJsonObject feature(qint64 id, const char* type) {
  return QJsonObject{{"id", id}, {"name", type}, {"type", type}};
}

QJsonObject profile() {
  return QJsonObject{{"lines", QJsonArray{}}, {"circles", QJsonArray{}},
                     {"arcs", QJsonArray{}}, {"beziers", QJsonArray{}}};
}

QJsonObject fullProfile() {
  return QJsonObject{
      {"lines", QJsonArray{QJsonObject{{"x1", 0}, {"y1", 0},
                                        {"x2", 4}, {"y2", 0},
                                        {"elementId", 1}}}},
      {"circles", QJsonArray{QJsonObject{{"x", 10}, {"y", 0},
                                          {"radius", 2}}}},
      {"arcs", QJsonArray{QJsonObject{{"x", 0}, {"y", 10},
                                       {"radius", 3}, {"startAngle", 0},
                                       {"sweepAngle", 1}}}},
      {"beziers", QJsonArray{QJsonObject{{"x0", 0}, {"y0", 20},
                                          {"x1", 1}, {"y1", 21},
                                          {"x2", 3}, {"y2", 21},
                                          {"x3", 4}, {"y3", 20}}}}};
}

QJsonObject v1(const char* kind, qint64 bodyId = 10,
               qint64 featureId = 1) {
  return QJsonObject{{"referenceVersion", "V1"}, {"bodyId", bodyId},
                     {"featureId", featureId}, {"kind", kind},
                     {"legacyIndex", 0}};
}
QJsonObject v2(const char* kind, qint64 bodyId = 10,
               qint64 featureId = 1) {
  return QJsonObject{
      {"referenceVersion", "V2"}, {"bodyId", bodyId},
      {"provenance", QJsonObject{{"featureId", featureId}, {"kind", kind},
                                  {"operation", "Generated"},
                                  {"role", "Custom"}, {"localName", "owner"},
                                  {"origins", QJsonArray{}}}}};
}
QJsonObject wrapper(QJsonObject reference) {
  return QJsonObject{{"reference", std::move(reference)}};
}

ProjectFormatV4ModelValidationResult validate(const QJsonObject& value) {
  auto envelope = solidar::project::scanProjectFormatV4Envelope(
      QJsonDocument(value).toJson(QJsonDocument::Compact));
  if (!envelope.hasCandidate()) qCritical() << envelope.error;
  CHECK(envelope.hasCandidate());
  auto payload = solidar::project::scanProjectFormatV4Payload(
      std::move(*envelope.candidate));
  if (!payload.hasCandidate()) qCritical() << payload.error;
  CHECK(payload.hasCandidate());
  return solidar::project::validateProjectFormatV4Model(
      std::move(*payload.candidate));
}

void candidate(const ProjectFormatV4ModelValidationResult& result) {
  CHECK(result.status == ProjectFormatV4ModelValidationStatus::Candidate);
  CHECK(result.hasCandidate());
  CHECK(result.error.isEmpty());
}
void unsupported(const ProjectFormatV4ModelValidationResult& result) {
  CHECK(result.status ==
        ProjectFormatV4ModelValidationStatus::UnsupportedCandidate);
  CHECK(result.hasCandidate());
  CHECK(result.error.isEmpty());
}
void invalid(const ProjectFormatV4ModelValidationResult& result) {
  CHECK(result.status == ProjectFormatV4ModelValidationStatus::Invalid);
  CHECK(!result.hasCandidate());
  CHECK(!result.error.isEmpty());
}

QJsonArray allFeatures() {
  QJsonArray a;
  QJsonObject imported = feature(1, "ImportedShape"); imported["brep"] = "TQ=="; a.append(imported);
  QJsonObject extrude = feature(2, "Extrude");
  extrude["lengthMm"] = 10; extrude["operation"] = 0;
  extrude["reversed"] = false; extrude["sourceKind"] = "sketch";
  extrude["sketchId"] = 900; extrude["profileOverride"] = profile(); a.append(extrude);
  QJsonObject revolve = feature(3, "Revolve");
  revolve["profileSketchId"] = 900; revolve["axisType"] = 2;
  revolve["axisSketchId"] = 900; revolve["axisLineId"] = 2;
  revolve["axisLineIndex"] = 1; revolve["angleDeg"] = 180;
  revolve["operation"] = 0; revolve["reversed"] = false;
  revolve["profileOverride"] = profile(); a.append(revolve);
  QJsonObject pocket = feature(4, "Pocket"); pocket["sketchId"] = 900; pocket["depthMm"] = 2; a.append(pocket);
  QJsonObject fillet = feature(5, "Fillet"); fillet["radiusMm"] = 1; fillet["edges"] = QJsonArray{wrapper(v1("Edge"))}; a.append(fillet);
  QJsonObject chamfer = feature(6, "Chamfer"); chamfer["distanceMm"] = 1; chamfer["edges"] = QJsonArray{wrapper(v1("Edge"))}; a.append(chamfer);
  QJsonObject mirror = feature(7, "Mirror"); mirror["sourceFeatureId"] = 1; mirror["plane"] = 0; a.append(mirror);
  QJsonObject move = feature(8, "Move"); move["sourceFeatureId"] = 1; move["offsetXmm"] = 0; move["offsetYmm"] = 0; move["offsetZmm"] = 0; a.append(move);
  QJsonObject linear = feature(9, "LinearPattern"); linear["sourceBodyId"] = 0; linear["sourceFeatureId"] = 8; linear["direction"] = 0; linear["count"] = 2; linear["spacingMm"] = 1; linear["operation"] = 1; a.append(linear);
  QJsonObject circular = feature(10, "CircularPattern"); circular["sourceBodyId"] = 10; circular["sourceFeatureId"] = 9; circular["axis"] = 2; circular["count"] = 2; circular["angleDeg"] = 90; circular["operation"] = 1; a.append(circular);
  QJsonObject join = feature(11, "JoinBodies"); join["firstBodyId"] = 10; join["firstFeatureId"] = 1; join["secondBodyId"] = 20; join["secondFeatureId"] = 20; a.append(join);
  QJsonObject shell = feature(12, "Shell"); shell["sourceFeatureId"] = 1; shell["thicknessMm"] = 1; shell["outside"] = false; shell["removedFaces"] = QJsonArray{wrapper(v1("Face"))}; a.append(shell);
  QJsonObject draft = feature(13, "Draft"); draft["sourceFeatureId"] = 1; draft["neutralPlaneType"] = 0; draft["pullDirectionType"] = 3; draft["pullDirectionSketchId"] = 0; draft["pullDirectionLineId"] = 0; draft["angleDeg"] = 5; draft["reversed"] = false; draft["draftedFaces"] = QJsonArray{wrapper(v1("Face"))}; a.append(draft);
  return a;
}

}  // namespace

int main() {
  {
    QJsonObject value = root();
    QJsonObject producer = feature(20, "ImportedShape"); producer["brep"] = "TQ==";
    setBodies(&value, QJsonArray{body(10, allFeatures()), body(20, QJsonArray{producer})});
    const QByteArray authored = QJsonDocument(value).toJson(QJsonDocument::Compact);
    auto result = validate(value);
    candidate(result);
    CHECK(QJsonDocument(result.candidate->legacySketch.topology.payload.envelope.authoredRoot).toJson(QJsonDocument::Compact) == authored);
  }

  for (const char* encoded : {"TQ==", "TWE=", "TWFu", "TQ", "TR=="}) {
    QJsonObject value = root(); QJsonObject imported = feature(1, "ImportedShape"); imported["brep"] = encoded;
    setBodies(&value, QJsonArray{body(10, QJsonArray{imported})}); candidate(validate(value));
  }
  for (const char* encoded : {"M", "TQ=A", "@@@@", "T Q=="}) {
    QJsonObject value = root(); QJsonObject imported = feature(1, "ImportedShape"); imported["brep"] = encoded;
    setBodies(&value, QJsonArray{body(10, QJsonArray{imported})}); invalid(validate(value));
  }

  {
    QJsonObject value = root(); QJsonObject pocket = feature(1, "Pocket"); pocket["sketchId"] = 999; pocket["depthMm"] = 1;
    value["capabilities"] =
        QJsonArray{"future-capability", "persistent-topology-v2"};
    setBodies(&value, QJsonArray{body(10, QJsonArray{pocket})}); invalid(validate(value));
  }
  {
    QJsonObject value = root(); QJsonObject revolve = feature(1, "Revolve"); revolve["profileSketchId"] = 900; revolve["axisType"] = 2; revolve["axisSketchId"] = 900; revolve["axisLineId"] = 1; revolve["axisLineIndex"] = 1; revolve["angleDeg"] = 90; revolve["operation"] = 0; revolve["reversed"] = false;
    setBodies(&value, QJsonArray{body(10, QJsonArray{revolve})}); invalid(validate(value));
  }
  {
    QJsonObject value = root(); QJsonObject mirror = feature(2, "Mirror"); mirror["sourceFeatureId"] = 1; mirror["plane"] = 0; mirror["planeFace"] = wrapper(v1("Face", 20, 20));
    QJsonObject p1 = feature(1, "ImportedShape"); p1["brep"] = "TQ=="; QJsonObject p2 = feature(20, "ImportedShape"); p2["brep"] = "TQ==";
    setBodies(&value, QJsonArray{body(10, QJsonArray{p1, mirror}), body(20, QJsonArray{p2})}); invalid(validate(value));
  }
  {
    QJsonObject value = root(); QJsonObject p1 = feature(1, "ImportedShape"); p1["brep"] = "TQ=="; QJsonObject mirror = feature(2, "Mirror"); mirror["sourceFeatureId"] = 1; mirror["plane"] = 0; mirror["planeFace"] = wrapper(QJsonObject{{"referenceVersion", "V9"}});
    value["capabilities"] =
        QJsonArray{"future-capability", "persistent-topology-v2"};
    setBodies(&value, QJsonArray{body(10, QJsonArray{p1, mirror})});
    const auto result = validate(value);
    unsupported(result);
    const auto& topology = result.candidate->legacySketch.topology;
    CHECK(topology.topologyReferences.size() == 1);
    CHECK(!topology.topologyReferences[0]);
    CHECK(topology.topologyInspections.size() == 1);
    CHECK(!topology.topologyInspections[0]);
    CHECK(topology.deferredUnsupported.size() == 2);
    CHECK(topology.provenanceRecordCount == 0);
    CHECK(topology.localNameUtf8Bytes == 0);
    CHECK(topology.payload.envelope.deferredUnknowns.size() == 1);
    CHECK(topology.payload.envelope.deferredUnknowns[0].token ==
          QStringLiteral("future-capability"));
    CHECK(topology.payload.envelope.authoredRoot == value);
  }
  {
    QJsonObject value = root(); value["extrusion"] = QJsonObject{{"enabled", true}, {"sourceSketch", 999}};
    // P2e1's legacy projection rejects the stale extrusion source before P2e2.
    invalid(validate(value));
  }
  // All nine topology site families, including Draft's intentionally
  // different scanner/codec order, are matched by canonical site path.
  {
    QJsonObject value = root();
    QJsonObject model = value["model"].toObject();
    QJsonObject supported = modelSketch();
    supported["support"] = QJsonObject{{"type", 1},
                                        {"reference", v1("Face")}};
    model["sketches"] = QJsonArray{supported};
    value["model"] = model;
    QJsonArray features = allFeatures();
    QJsonObject extrude = features[1].toObject();
    extrude.remove("sketchId"); extrude.remove("profileOverride");
    extrude["sourceKind"] = "face"; extrude["face"] = wrapper(v1("Face"));
    features[1] = extrude;
    QJsonObject mirror = features[6].toObject();
    mirror["planeFace"] = wrapper(v1("Face")); features[6] = mirror;
    QJsonObject draft = features[12].toObject();
    draft["neutralPlaneType"] = 3;
    draft["neutralPlaneFace"] = wrapper(v1("Face"));
    draft["rotationEdge"] = wrapper(v1("Edge")); features[12] = draft;
    QJsonObject other = feature(20, "ImportedShape"); other["brep"] = "TQ==";
    setBodies(&value, QJsonArray{body(10, features), body(20, QJsonArray{other})});
    candidate(validate(value));
  }
  for (bool useV2 : {false, true}) {
    QJsonObject value = root();
    QJsonObject producer = feature(1, "ImportedShape"); producer["brep"] = "TQ==";
    QJsonObject draft = feature(2, "Draft");
    draft["sourceFeatureId"] = 1; draft["angleDeg"] = 5;
    draft["reversed"] = false; draft["neutralPlaneType"] = 3;
    draft["pullDirectionType"] = 3; draft["pullDirectionSketchId"] = 0;
    draft["pullDirectionLineId"] = 0;
    const auto face = useV2 ? v2("Face") : v1("Face");
    const auto edge = useV2 ? v2("Edge") : v1("Edge");
    draft["neutralPlaneFace"] = wrapper(face);
    draft["rotationEdge"] = wrapper(edge);
    draft["draftedFaces"] = QJsonArray{wrapper(face), wrapper(face)};
    setBodies(&value, QJsonArray{body(10, QJsonArray{producer, draft})});
    candidate(validate(value));
    draft["neutralPlaneFace"] =
        wrapper(useV2 ? v2("Face", 20, 20) : v1("Face", 20, 20));
    QJsonObject other = feature(20, "ImportedShape"); other["brep"] = "TQ==";
    setBodies(&value, QJsonArray{body(10, QJsonArray{producer, draft}),
                                 body(20, QJsonArray{other})});
    invalid(validate(value));
  }
  // Recognized-but-unsupported V1/V2 variants keep inspection owner metadata:
  // a matching owner defers, while a valid-but-wrong producer pair is Invalid.
  for (bool useV2 : {false, true}) {
    QJsonObject value = root();
    QJsonObject p1 = feature(1, "ImportedShape"); p1["brep"] = "TQ==";
    QJsonObject p2 = feature(2, "ImportedShape"); p2["brep"] = "TQ==";
    QJsonObject mirror = feature(3, "Mirror"); mirror["sourceFeatureId"] = 1;
    mirror["plane"] = 0;
    mirror["planeFace"] = wrapper(useV2 ? v2("FutureKind", 10, 1)
                                         : v1("FutureKind", 10, 1));
    setBodies(&value, QJsonArray{body(10, QJsonArray{p1, p2, mirror})});
    unsupported(validate(value));
    mirror["planeFace"] = wrapper(useV2 ? v2("FutureKind", 10, 2)
                                         : v1("FutureKind", 10, 2));
    setBodies(&value, QJsonArray{body(10, QJsonArray{p1, p2, mirror})});
    invalid(validate(value));
  }
  // Every same-body source branch rejects both an existing feature owned by a
  // different body and a stale ID.
  for (const char* type : {"Mirror", "Move", "Shell", "Draft"}) {
    for (qint64 sourceId : {qint64{20}, qint64{999}}) {
      QJsonObject value = root();
      QJsonObject local = feature(1, "ImportedShape"); local["brep"] = "TQ==";
      QJsonObject foreign = feature(20, "ImportedShape"); foreign["brep"] = "TQ==";
      QJsonObject tested = feature(2, type);
      tested["sourceFeatureId"] = sourceId;
      const QString token = QString::fromLatin1(type);
      if (token == QStringLiteral("Mirror")) {
        tested["plane"] = 0;
      } else if (token == QStringLiteral("Move")) {
        tested["offsetXmm"] = 0; tested["offsetYmm"] = 0;
        tested["offsetZmm"] = 0;
      } else if (token == QStringLiteral("Shell")) {
        tested["thicknessMm"] = 1; tested["outside"] = false;
        tested["removedFaces"] = QJsonArray{wrapper(v1("Face"))};
      } else {
        tested["angleDeg"] = 5; tested["reversed"] = false;
        tested["neutralPlaneType"] = 0; tested["pullDirectionType"] = 3;
        tested["pullDirectionSketchId"] = 0;
        tested["pullDirectionLineId"] = 0;
        tested["draftedFaces"] = QJsonArray{wrapper(v1("Face"))};
      }
      setBodies(&value,
                QJsonArray{body(10, QJsonArray{local, tested}),
                           body(20, QJsonArray{foreign})});
      invalid(validate(value));
    }
  }
  // Both Pattern operations share the authoritative codec rules. NewBody
  // must name an exact external pair and Join must name the immediate previous
  // feature in the current body.
  for (const char* type : {"LinearPattern", "CircularPattern"}) {
    QJsonObject value = root();
    QJsonObject local = feature(1, "ImportedShape"); local["brep"] = "TQ==";
    QJsonObject external = feature(20, "ImportedShape"); external["brep"] = "TQ==";
    QJsonObject patternFeature = feature(2, type);
    patternFeature["sourceBodyId"] = 20; patternFeature["sourceFeatureId"] = 20;
    patternFeature["count"] = 2; patternFeature["operation"] = 0;
    if (QString::fromLatin1(type) == QStringLiteral("LinearPattern")) {
      patternFeature["direction"] = 0; patternFeature["spacingMm"] = 1;
    } else {
      patternFeature["axis"] = 2; patternFeature["angleDeg"] = 90;
    }
    setBodies(&value, QJsonArray{body(10, QJsonArray{local, patternFeature}),
                                 body(20, QJsonArray{external})});
    candidate(validate(value));
    patternFeature["sourceFeatureId"] = 1;
    setBodies(&value, QJsonArray{body(10, QJsonArray{local, patternFeature}),
                                 body(20, QJsonArray{external})});
    invalid(validate(value));
    patternFeature["operation"] = 1; patternFeature["sourceBodyId"] = 0;
    patternFeature["sourceFeatureId"] = 1;
    setBodies(&value, QJsonArray{body(10, QJsonArray{local, patternFeature}),
                                 body(20, QJsonArray{external})});
    candidate(validate(value));

    auto joinPattern = [&](qint64 id, qint64 sourceBody,
                           qint64 sourceFeature) {
      QJsonObject result = feature(id, type);
      result["sourceBodyId"] = sourceBody;
      result["sourceFeatureId"] = sourceFeature;
      result["count"] = 2; result["operation"] = 1;
      if (QString::fromLatin1(type) == QStringLiteral("LinearPattern")) {
        result["direction"] = 0; result["spacingMm"] = 1;
      } else {
        result["axis"] = 2; result["angleDeg"] = 90;
      }
      return result;
    };
    QJsonObject firstPosition = joinPattern(2, 0, 1);
    setBodies(&value, QJsonArray{body(10, QJsonArray{firstPosition, local}),
                                 body(20, QJsonArray{external})});
    invalid(validate(value));
    QJsonObject middle = feature(3, "ImportedShape"); middle["brep"] = "TQ==";
    QJsonObject nonPrevious = joinPattern(2, 0, 1);
    setBodies(&value,
              QJsonArray{body(10, QJsonArray{local, middle, nonPrevious}),
                         body(20, QJsonArray{external})});
    invalid(validate(value));
    QJsonObject foreignBody = joinPattern(2, 20, 1);
    setBodies(&value, QJsonArray{body(10, QJsonArray{local, foreignBody}),
                                 body(20, QJsonArray{external})});
    invalid(validate(value));
    patternFeature["operation"] = 0; patternFeature["sourceBodyId"] = 10;
    patternFeature["sourceFeatureId"] = 1;
    setBodies(&value, QJsonArray{body(10, QJsonArray{local, patternFeature}),
                                 body(20, QJsonArray{external})});
    invalid(validate(value));
  }
  {
    QJsonObject value = root();
    QJsonObject a = feature(1, "ImportedShape"); a["brep"] = "TQ==";
    QJsonObject b = feature(20, "ImportedShape"); b["brep"] = "TQ==";
    QJsonObject join = feature(2, "JoinBodies");
    join["firstBodyId"] = 10; join["firstFeatureId"] = 20;
    join["secondBodyId"] = 20; join["secondFeatureId"] = 20;
    setBodies(&value, QJsonArray{body(10, QJsonArray{a, join}),
                                 body(20, QJsonArray{b})});
    invalid(validate(value));
  }
  // Draft sketch-line axes use the stable positional tuple, not the authored
  // geometry elementId (the second line has elementId 77, lineId 2).
  {
    QJsonObject value = root();
    QJsonObject producer = feature(1, "ImportedShape"); producer["brep"] = "TQ==";
    QJsonObject draft = feature(2, "Draft");
    draft["sourceFeatureId"] = 1; draft["angleDeg"] = 5;
    draft["reversed"] = false; draft["neutralPlaneType"] = 0;
    draft["pullDirectionType"] = 2; draft["pullDirectionSketchId"] = 900;
    draft["pullDirectionLineId"] = 2;
    draft["pullDirectionLineIndex"] = 1;
    draft["draftedFaces"] = QJsonArray{wrapper(v1("Face"))};
    setBodies(&value, QJsonArray{body(10, QJsonArray{producer, draft})});
    candidate(validate(value));
    draft["pullDirectionSketchId"] = 999;
    setBodies(&value, QJsonArray{body(10, QJsonArray{producer, draft})});
    invalid(validate(value));
    draft["pullDirectionSketchId"] = 900;
    draft["pullDirectionLineIndex"] = 2;
    setBodies(&value, QJsonArray{body(10, QJsonArray{producer, draft})});
    invalid(validate(value));
    draft["pullDirectionLineIndex"] = 1;
    draft["pullDirectionLineId"] = 1;
    setBodies(&value, QJsonArray{body(10, QJsonArray{producer, draft})});
    invalid(validate(value));
  }
  // The pure profile validator covers all four geometry families and catches
  // numerical/degenerate defects before any runtime sketch is constructed.
  for (const char* type : {"Extrude", "Revolve"}) {
    QJsonObject value = root();
    QJsonObject shaped = feature(1, type);
    shaped["profileOverride"] = fullProfile();
    if (QString::fromLatin1(type) == QStringLiteral("Extrude")) {
      shaped["lengthMm"] = 10; shaped["operation"] = 0;
      shaped["reversed"] = false; shaped["sourceKind"] = "sketch";
      shaped["sketchId"] = 900;
    } else {
      shaped["profileSketchId"] = 900; shaped["axisType"] = 2;
      shaped["axisSketchId"] = 900; shaped["axisLineId"] = 2;
      shaped["axisLineIndex"] = 1; shaped["angleDeg"] = 90;
      shaped["operation"] = 0; shaped["reversed"] = false;
    }
    setBodies(&value, QJsonArray{body(10, QJsonArray{shaped})});
    candidate(validate(value));
    for (const bool removeElementId : {true, false}) {
      QJsonObject invalidElementProfile = fullProfile();
      QJsonObject invalidLine =
          invalidElementProfile["lines"].toArray()[0].toObject();
      if (removeElementId)
        invalidLine.remove("elementId");
      else
        invalidLine["elementId"] = 0;
      invalidElementProfile["lines"] = QJsonArray{invalidLine};
      shaped["profileOverride"] = invalidElementProfile;
      setBodies(&value, QJsonArray{body(10, QJsonArray{shaped})});
      invalid(validate(value));
    }
    QJsonObject badProfile = fullProfile();
    QJsonObject badLine = badProfile["lines"].toArray()[0].toObject();
    badLine["x2"] = 0; badLine["y2"] = 0;
    badProfile["lines"] = QJsonArray{badLine}; shaped["profileOverride"] = badProfile;
    setBodies(&value, QJsonArray{body(10, QJsonArray{shaped})});
    const auto first = validate(value); const auto second = validate(value);
    invalid(first); invalid(second); CHECK(first.error == second.error);

    QJsonObject badCircleProfile = fullProfile();
    QJsonObject badCircle = badCircleProfile["circles"].toArray()[0].toObject();
    badCircle["radius"] = 0;
    badCircleProfile["circles"] = QJsonArray{badCircle};
    QJsonObject badArcProfile = fullProfile();
    QJsonObject badArc = badArcProfile["arcs"].toArray()[0].toObject();
    badArc["sweepAngle"] = 0;
    badArcProfile["arcs"] = QJsonArray{badArc};
    QJsonObject badBezierProfile = fullProfile();
    QJsonObject badBezier = badBezierProfile["beziers"].toArray()[0].toObject();
    for (const char* coordinate : {"x0", "y0", "x1", "y1",
                                   "x2", "y2", "x3", "y3"})
      badBezier[QLatin1String(coordinate)] = 0;
    badBezierProfile["beziers"] = QJsonArray{badBezier};
    for (const auto& bad : {badCircleProfile, badArcProfile,
                            badBezierProfile}) {
      shaped["profileOverride"] = bad;
      setBodies(&value, QJsonArray{body(10, QJsonArray{shaped})});
      invalid(validate(value));
    }
  }
  // Unsupported upstream state never masks a later semantic defect.
  {
    QJsonObject value = root();
    value["capabilities"] =
        QJsonArray{"future-capability", "persistent-topology-v2"};
    QJsonObject bad = feature(1, "ImportedShape"); bad["brep"] = "@@@@";
    setBodies(&value, QJsonArray{body(10, QJsonArray{bad})});
    invalid(validate(value));
  }
  {
    QJsonObject value = root();
    QJsonObject face = feature(1, "Extrude"); face["lengthMm"] = 1;
    face["operation"] = 0; face["reversed"] = false;
    face["sourceKind"] = "face";
    face["face"] = wrapper(QJsonObject{{"referenceVersion", "V9"}});
    QJsonObject bad = feature(2, "ImportedShape"); bad["brep"] = "@@@@";
    setBodies(&value, QJsonArray{body(10, QJsonArray{face, bad})});
    invalid(validate(value));
  }
  return EXIT_SUCCESS;
}
