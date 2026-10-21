#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/ShellFeature.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4ModelValidator.h"
#include "project/ProjectFormatV4PayloadScanner.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "project/internal/ProjectFormatV4DocumentMaterialization.h"

namespace {

using namespace solidar;
using namespace solidar::project;
using namespace solidar::project::detail;

constexpr BodyId kBody = 10;
constexpr BodyId kOtherBody = 20;
constexpr FeatureId kProducer = 1;
constexpr FeatureId kOtherProducer = 20;
constexpr SketchId kSketch = 100;

ShapeFeature::ShapePtr box() {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(2.0, 3.0, 4.0).Shape());
}

ProjectFormatV4DocumentMetadata metadata() {
  return {QStringLiteral("materializer"),
          QStringLiteral("2026-10-21T12:34:56Z")};
}

FaceReference faceV1(std::size_t index, std::string tag = {}) {
  return FaceReference(kBody, kProducer, index, std::move(tag));
}

EdgeReference edgeV1(std::size_t index, std::string tag = {}) {
  return EdgeReference(kBody, kProducer, index, std::move(tag));
}

TopologyReferenceV2 v2Carrier(TopologyKind kind, std::string localName) {
  return {kBody,
          TopologyProvenanceKey{kProducer, kind,
                                TopologyProvenanceOperation::Generated,
                                kind == TopologyKind::Face
                                    ? TopologySemanticRole::Side
                                    : TopologySemanticRole::Custom,
                                std::move(localName)}};
}

FaceReference faceV2(const char* localName) {
  return FaceReference::fromV2(v2Carrier(TopologyKind::Face, localName));
}

EdgeReference edgeV2(const char* localName) {
  return EdgeReference::fromV2(v2Carrier(TopologyKind::Edge, localName));
}

FaceReference richFaceV2() {
  std::vector<TopologyProvenanceOrigin> origins{
      {2, TopologyKind::Edge, TopologyProvenanceOperation::Seed,
       TopologySemanticRole::SketchBoundary, "face-origin-edge-seed",
       TopologyOccurrenceKey{2, 7}},
      {3, TopologyKind::Face, TopologyProvenanceOperation::Modified,
       TopologySemanticRole::Cap, "face-origin-face-modified"},
      {4, TopologyKind::Edge, TopologyProvenanceOperation::Merge,
       TopologySemanticRole::BooleanResult, "face-origin-edge-merge",
       TopologyOccurrenceKey{4, 9}}};
  return FaceReference::fromV2(
      {kBody,
       TopologyProvenanceKey{
           kProducer, TopologyKind::Face,
           TopologyProvenanceOperation::Modified,
           TopologySemanticRole::BooleanResult, "shell-rich-v2",
           std::move(origins), TopologyOccurrenceKey{kProducer, 101}}});
}

EdgeReference richEdgeV2() {
  std::vector<TopologyProvenanceOrigin> origins{
      {5, TopologyKind::Face, TopologyProvenanceOperation::Generated,
       TopologySemanticRole::Side, "edge-origin-face-generated",
       TopologyOccurrenceKey{5, 11}},
      {6, TopologyKind::Edge, TopologyProvenanceOperation::Modified,
       TopologySemanticRole::Reflected, "edge-origin-edge-modified"},
      {7, TopologyKind::Face, TopologyProvenanceOperation::Merge,
       TopologySemanticRole::BooleanResult, "edge-origin-face-merge",
       TopologyOccurrenceKey{7, 13}}};
  return EdgeReference::fromV2(
      {kBody,
       TopologyProvenanceKey{
           kProducer, TopologyKind::Edge,
           TopologyProvenanceOperation::Retained,
           TopologySemanticRole::Original, "fillet-rich-v2",
           std::move(origins), TopologyOccurrenceKey{kProducer, 202}}});
}

QJsonObject jsonV1(const char* kind, qint64 index,
                   const char* tag = nullptr) {
  QJsonObject result{{"referenceVersion", "V1"},
                     {"bodyId", static_cast<qint64>(kBody)},
                     {"featureId", static_cast<qint64>(kProducer)},
                     {"kind", kind},
                     {"legacyIndex", index}};
  if (tag) result["persistentTag"] = tag;
  return result;
}

QJsonObject jsonV2(const char* kind, const char* localName) {
  return QJsonObject{
      {"referenceVersion", "V2"},
      {"bodyId", static_cast<qint64>(kBody)},
      {"provenance",
       QJsonObject{{"featureId", static_cast<qint64>(kProducer)},
                   {"kind", kind},
                   {"operation", "Generated"},
                   {"role", QString::fromLatin1(kind) == QStringLiteral("Face")
                                ? "Side"
                                : "Custom"},
                   {"localName", localName},
                   {"origins", QJsonArray{}}}}};
}

QJsonObject jsonInstance(qint64 featureId, qint64 slot) {
  return QJsonObject{{"featureId", featureId}, {"slot", slot}};
}

QJsonObject jsonOrigin(qint64 featureId, const char* kind,
                       const char* operation, const char* role,
                       const char* localName,
                       QJsonValue instanceKey = QJsonValue{}) {
  QJsonObject result{{"featureId", featureId},
                     {"kind", kind},
                     {"operation", operation},
                     {"role", role},
                     {"localName", localName}};
  if (!instanceKey.isUndefined() && !instanceKey.isNull())
    result["instanceKey"] = std::move(instanceKey);
  return result;
}

QJsonObject jsonRichFaceV2() {
  return QJsonObject{
      {"referenceVersion", "V2"},
      {"bodyId", static_cast<qint64>(kBody)},
      {"provenance",
       QJsonObject{
           {"featureId", static_cast<qint64>(kProducer)},
           {"kind", "Face"},
           {"operation", "Modified"},
           {"role", "BooleanResult"},
           {"localName", "shell-rich-v2"},
           {"instanceKey", jsonInstance(kProducer, 101)},
           {"origins",
            QJsonArray{
                jsonOrigin(2, "Edge", "Seed", "SketchBoundary",
                           "face-origin-edge-seed", jsonInstance(2, 7)),
                jsonOrigin(3, "Face", "Modified", "Cap",
                           "face-origin-face-modified"),
                jsonOrigin(4, "Edge", "Merge", "BooleanResult",
                           "face-origin-edge-merge", jsonInstance(4, 9))}}}}};
}

QJsonObject jsonRichEdgeV2() {
  return QJsonObject{
      {"referenceVersion", "V2"},
      {"bodyId", static_cast<qint64>(kBody)},
      {"provenance",
       QJsonObject{
           {"featureId", static_cast<qint64>(kProducer)},
           {"kind", "Edge"},
           {"operation", "Retained"},
           {"role", "Original"},
           {"localName", "fillet-rich-v2"},
           {"instanceKey", jsonInstance(kProducer, 202)},
           {"origins",
            QJsonArray{
                jsonOrigin(5, "Face", "Generated", "Side",
                           "edge-origin-face-generated",
                           jsonInstance(5, 11)),
                jsonOrigin(6, "Edge", "Modified", "Reflected",
                           "edge-origin-edge-modified"),
                jsonOrigin(7, "Face", "Merge", "BooleanResult",
                           "edge-origin-face-merge",
                           jsonInstance(7, 13))}}}}};
}

QJsonObject wrapper(QJsonObject reference) {
  return QJsonObject{{"reference", std::move(reference)}};
}

Document allCodecDocument() {
  Document document;
  document.setBox({71.0, 43.0, 29.0});
  auto& profile = document.addSketch(kSketch, "profile");
  profile.geometry.addRectangle({0.0, 0.0}, {10.0, 8.0});
  profile.geometry.addCircle({3.0, 4.0}, 1.5);
  CHECK(profile.geometry.restoreGeometryIds({11, 12, 13, 14}, {21}, {}, {}));
  CHECK(profile.geometry.restoreConstraints(
      {sketch::Constraint{sketch::HorizontalConstraint{11},
                          sketch::ConstraintMode::Driving, 41},
       sketch::Constraint{sketch::LengthConstraint{11, std::nullopt},
                          sketch::ConstraintMode::Reference, 42}}));
  sketch::Dimension annotation;
  annotation.kind = sketch::DimensionKind::LineLength;
  annotation.geometryId = 11;
  annotation.offsetMm = 7.5;
  annotation.angleRad = 0.25;
  annotation.id = 51;
  annotation.constraintId = 42;
  CHECK(profile.geometry.storeDimension(annotation));
  CHECK(profile.geometry.restoreIdAllocators(70, 80, 90, 100));
  profile.support.type = SketchSupportType::Face;
  profile.support.face = faceV1(0);

  ReferenceImage image;
  image.id = 300;
  image.name = "reference";
  image.sourcePath = "images/reference.png";
  image.supportName = "YZ";
  image.placement.origin = {1.0, 2.0, 3.0};
  image.placement.xDirection = {0.0, 1.0, 0.0};
  image.placement.yDirection = {0.0, 0.0, 1.0};
  image.offsetXMm = 4.0;
  image.offsetYMm = 5.0;
  image.offsetZMm = 6.0;
  image.scale = 2.5;
  image.pixelWidth = 640;
  image.pixelHeight = 480;
  image.visible = false;
  document.addReferenceImage(std::move(image));

  auto& body = document.addBody(kBody, "main");
  body.setVisible(false);
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kProducer, box(), "seed"));
  body.addFeature(std::make_unique<ExtrudeFeature>(
      2, kSketch, 5.0, "extrude", ExtrudeOperation::NewBody, false));
  body.addFeature(std::make_unique<RevolveFeature>(
      3, kSketch, AxisReference{AxisReferenceType::GlobalX}, 180.0,
      "revolve", ExtrudeOperation::NewBody, false));
  body.addFeature(std::make_unique<PocketFeature>(4, kSketch, 2.0, "pocket"));
  body.addFeature(std::make_unique<FilletFeature>(
      5, std::vector<EdgeReference>{edgeV1(0), edgeV2("source-fillet")},
      0.5, "fillet"));
  body.addFeature(std::make_unique<ChamferFeature>(
      6, std::vector<EdgeReference>{edgeV2("source-chamfer"), edgeV1(1)},
      0.25, "chamfer"));
  body.addFeature(std::make_unique<MirrorFeature>(
      7, kProducer, faceV1(2), "mirror"));
  body.addFeature(std::make_unique<MoveFeature>(
      8, kProducer, Vector3d{1.0, 2.0, 3.0}, "move"));
  body.addFeature(std::make_unique<LinearPatternFeature>(
      9, kBody, 8, PrincipalAxis::X, 2, 4.0, PatternOperation::Join,
      "linear"));
  body.addFeature(std::make_unique<CircularPatternFeature>(
      10, kBody, 9, PrincipalAxis::Z, 3, 180.0, PatternOperation::Join,
      "circular"));
  body.addFeature(std::make_unique<JoinBodiesFeature>(
      11, kBody, 10, kOtherBody, kOtherProducer, "join"));
  body.addFeature(std::make_unique<ShellFeature>(
      12, kProducer, std::vector<FaceReference>{faceV1(3), faceV2("source-shell")},
      0.4, false, "shell"));
  body.addFeature(std::make_unique<DraftFeature>(
      13, kProducer,
      std::vector<FaceReference>{faceV2("source-drafted"), faceV1(4)},
      PlaneReference{NeutralPlaneType::BodyFace, faceV2("source-neutral")},
      AxisReference{AxisReferenceType::SketchLine, kSketch, 11}, 3.0, false,
      "draft",
      edgeV2("source-rotation")));
  body.addFeature(std::make_unique<ExtrudeFeature>(
      14, faceV2("source-extrude"), 1.0, "face extrude",
      ExtrudeOperation::Join, false));

  document.addBody(kOtherBody, "other")
      .addFeature(std::make_unique<ImportedShapeFeature>(
          kOtherProducer, box(), "other seed"));
  return document;
}

QJsonObject independentlyAuthoredTopologyRoot() {
  const auto encoded =
      encodeProjectFormatV4DocumentRoot(allCodecDocument(), metadata());
  CHECK(encoded.succeeded());
  QJsonObject root = *encoded.root;
  auto model = root.value("model").toObject();
  auto sketches = model.value("sketches").toArray();
  auto sketch = sketches[0].toObject();
  auto support = sketch.value("support").toObject();
  support["reference"] = jsonV1("Face", 0, "support-authored");
  sketch["support"] = support;
  sketches[0] = sketch;
  model["sketches"] = sketches;

  auto bodies = model.value("bodies").toArray();
  auto mainBody = bodies[0].toObject();
  auto features = mainBody.value("features").toArray();

  auto fillet = features[4].toObject();
  fillet["edges"] = QJsonArray{
      wrapper(jsonV1("Edge", 0, "fillet-v1")),
      wrapper(jsonRichEdgeV2())};
  features[4] = fillet;

  auto chamfer = features[5].toObject();
  chamfer["edges"] = QJsonArray{
      wrapper(jsonV2("Edge", "chamfer-v2")),
      wrapper(jsonV1("Edge", 1, "chamfer-v1"))};
  features[5] = chamfer;

  auto mirror = features[6].toObject();
  mirror["planeFace"] = wrapper(jsonV1("Face", 2, "mirror-v1"));
  features[6] = mirror;

  auto shell = features[11].toObject();
  shell["removedFaces"] = QJsonArray{
      wrapper(jsonV1("Face", 3, "shell-v1")),
      wrapper(jsonRichFaceV2())};
  features[11] = shell;

  auto draft = features[12].toObject();
  draft["neutralPlaneFace"] = wrapper(jsonV2("Face", "draft-neutral"));
  draft["rotationEdge"] = wrapper(jsonV2("Edge", "draft-rotation"));
  draft["draftedFaces"] = QJsonArray{
      wrapper(jsonV2("Face", "drafted-zero")),
      wrapper(jsonV1("Face", 4, "drafted-one"))};
  features[12] = draft;

  auto faceExtrude = features[13].toObject();
  faceExtrude["face"] = wrapper(jsonV2("Face", "extrude-face"));
  features[13] = faceExtrude;

  mainBody["features"] = features;
  bodies[0] = mainBody;
  model["bodies"] = bodies;
  root["model"] = model;
  return root;
}

ProjectFormatV4ModelCandidate candidateFor(const QJsonObject& root) {
  auto envelope = scanProjectFormatV4Envelope(
      QJsonDocument(root).toJson(QJsonDocument::Compact));
  CHECK(envelope.hasCandidate());
  auto payload = scanProjectFormatV4Payload(std::move(*envelope.candidate));
  CHECK(payload.hasCandidate());
  auto model = validateProjectFormatV4Model(std::move(*payload.candidate));
  CHECK(model.status == ProjectFormatV4ModelValidationStatus::Candidate);
  CHECK(model.candidate);
  return std::move(*model.candidate);
}

void checkInvalid(const ProjectFormatV4DocumentMaterializationResult& result) {
  CHECK(result.status == ProjectFormatV4DocumentMaterializationStatus::Invalid);
  CHECK(!result.succeeded());
  CHECK(!result.document);
  CHECK(!result.error.isEmpty());
}

struct AutoIds {
  SketchId sketch{};
  ReferenceImageId image{};
  BodyId body{};
  FeatureId feature{};
};

AutoIds consumeAutoIds(const char* suffix) {
  Document document;
  AutoIds ids;
  ids.sketch = document.addSketch(std::string("sketch-") + suffix).id;
  ids.image = document
                  .addReferenceImage(std::string("image-") + suffix,
                                     "image.png", "XY",
                                     SketchPlacement::xy(), 10, 10)
                  .id;
  auto& body = document.addBody(std::string("body-") + suffix);
  ids.body = body.id();
  auto feature = std::make_unique<ImportedShapeFeature>(box(), "feature");
  ids.feature = feature->id();
  body.addFeature(std::move(feature));
  return ids;
}

void checkOnlyExpectedAutoIdAdvance(const AutoIds& before,
                                    const AutoIds& after) {
  CHECK(after.sketch == before.sketch + 1);
  CHECK(after.image == before.image + 1);
  CHECK(after.body == before.body + 1);
  CHECK(after.feature == before.feature + 1);
}

}  // namespace

int main() {
  // Topology wrappers are overwritten with an independent literal fixture;
  // the encoder supplies only the deterministic non-topology/B-Rep scaffold.
  {
    const QJsonObject root = independentlyAuthoredTopologyRoot();
    const auto candidate = candidateFor(root);
    const auto rootBefore =
        candidate.legacySketch.topology.payload.envelope.authoredRoot;
    const auto topologySitesBefore =
        candidate.legacySketch.topology.payload.topologySites;
    const auto featureOwnersBefore =
        candidate.legacySketch.topology.payload.featureOwners;
    const auto referencesBefore =
        candidate.legacySketch.topology.topologyReferences;
    const auto inspectionsBefore =
        candidate.legacySketch.topology.topologyInspections;
    const auto deferredTopologyBefore =
        candidate.legacySketch.topology.deferredUnsupported;
    const auto deferredEnvelopeBefore =
        candidate.legacySketch.topology.payload.envelope.deferredUnknowns;
    const auto deferredSketchBefore =
        candidate.legacySketch.deferredLegacySketch;
    const auto provenanceRecordCountBefore =
        candidate.legacySketch.topology.provenanceRecordCount;
    const auto localNameUtf8BytesBefore =
        candidate.legacySketch.topology.localNameUtf8Bytes;
    const AutoIds beforeIds = consumeAutoIds("before-success");
    const auto result = materializeProjectFormatV4Document(candidate);
    const AutoIds afterIds = consumeAutoIds("after-success");
    checkOnlyExpectedAutoIdAdvance(beforeIds, afterIds);
    CHECK(result.succeeded());
    CHECK(candidate.legacySketch.topology.payload.envelope.authoredRoot ==
          rootBefore);
    CHECK(candidate.legacySketch.topology.payload.topologySites ==
          topologySitesBefore);
    CHECK(candidate.legacySketch.topology.payload.featureOwners ==
          featureOwnersBefore);
    CHECK(candidate.legacySketch.topology.topologyReferences ==
          referencesBefore);
    CHECK(candidate.legacySketch.topology.topologyInspections ==
          inspectionsBefore);
    CHECK(candidate.legacySketch.topology.deferredUnsupported ==
          deferredTopologyBefore);
    CHECK(candidate.legacySketch.topology.payload.envelope.deferredUnknowns ==
          deferredEnvelopeBefore);
    CHECK(candidate.legacySketch.deferredLegacySketch == deferredSketchBefore);
    CHECK(candidate.legacySketch.topology.provenanceRecordCount ==
          provenanceRecordCountBefore);
    CHECK(candidate.legacySketch.topology.localNameUtf8Bytes ==
          localNameUtf8BytesBefore);

    const Document& document = *result.document;
    CHECK(document.box().widthMm == 71.0);
    CHECK(document.box().depthMm == 43.0);
    CHECK(document.box().heightMm == 29.0);
    CHECK(document.sketches().size() == 1);
    CHECK(document.sketches()[0].id == kSketch);
    CHECK(document.sketches()[0].geometry.lines().size() == 4);
    CHECK(document.sketches()[0].geometry.circles().size() == 1);
    CHECK(document.sketches()[0].geometry.lineId(0) == 11);
    CHECK(document.sketches()[0].geometry.lineId(1) == 12);
    CHECK(document.sketches()[0].geometry.lineId(2) == 13);
    CHECK(document.sketches()[0].geometry.lineId(3) == 14);
    CHECK(document.sketches()[0].geometry.circleId(0) == 21);
    const auto& constraints = document.sketches()[0].geometry.constraints();
    CHECK(constraints.size() == 2);
    CHECK(constraints[0].id == 41);
    CHECK(constraints[0].mode == sketch::ConstraintMode::Driving);
    const auto* horizontal =
        std::get_if<sketch::HorizontalConstraint>(&constraints[0].payload());
    CHECK(horizontal);
    CHECK(horizontal->lineId == 11);
    CHECK(constraints[1].id == 42);
    CHECK(constraints[1].mode == sketch::ConstraintMode::Reference);
    const auto* length =
        std::get_if<sketch::LengthConstraint>(&constraints[1].payload());
    CHECK(length);
    CHECK(length->lineId == 11);
    CHECK(!length->drivingValue);
    const auto& dimensions = document.sketches()[0].geometry.dimensions();
    CHECK(dimensions.size() == 1);
    CHECK(dimensions[0].id == 51);
    CHECK(dimensions[0].constraintId == 42);
    CHECK(dimensions[0].kind == sketch::DimensionKind::LineLength);
    CHECK(dimensions[0].geometryId == 11);
    CHECK(dimensions[0].offsetMm == 7.5);
    CHECK(dimensions[0].angleRad == 0.25);
    CHECK(document.sketches()[0].geometry.nextElementId() == 70);
    CHECK(document.sketches()[0].geometry.nextGeometryId() == 80);
    CHECK(document.sketches()[0].geometry.nextConstraintId() == 90);
    CHECK(document.sketches()[0].geometry.nextDimensionId() == 100);
    CHECK(document.sketches()[0].support.face ==
          faceV1(0, "support-authored"));
    CHECK(document.referenceImages().size() == 1);
    const auto& image = document.referenceImages()[0];
    CHECK(image.id == 300);
    CHECK(image.name == "reference");
    CHECK(image.sourcePath == "images/reference.png");
    CHECK(image.supportName == "YZ");
    CHECK(image.placement.origin.x == 1.0);
    CHECK(image.placement.origin.y == 2.0);
    CHECK(image.placement.origin.z == 3.0);
    CHECK(image.placement.xDirection.x == 0.0);
    CHECK(image.placement.xDirection.y == 1.0);
    CHECK(image.placement.xDirection.z == 0.0);
    CHECK(image.placement.yDirection.x == 0.0);
    CHECK(image.placement.yDirection.y == 0.0);
    CHECK(image.placement.yDirection.z == 1.0);
    CHECK(image.offsetXMm == 4.0);
    CHECK(image.offsetYMm == 5.0);
    CHECK(image.offsetZMm == 6.0);
    CHECK(image.scale == 2.5);
    CHECK(image.pixelWidth == 640);
    CHECK(image.pixelHeight == 480);
    CHECK(!image.visible);
    CHECK(document.bodies().size() == 2);
    CHECK(document.bodies()[0].id() == kBody);
    CHECK(document.bodies()[0].name() == "main");
    CHECK(!document.bodies()[0].visible());
    CHECK(document.bodies()[1].id() == kOtherBody);
    CHECK(document.bodies()[1].name() == "other");
    CHECK(document.bodies()[1].visible());
    CHECK(document.bodies()[0].features().size() == 14);
    const std::array<FeatureKind, 14> kinds{
        FeatureKind::ImportedShape, FeatureKind::Extrude,
        FeatureKind::Revolve,       FeatureKind::Pocket,
        FeatureKind::Fillet,        FeatureKind::Chamfer,
        FeatureKind::Mirror,        FeatureKind::Move,
        FeatureKind::LinearPattern, FeatureKind::CircularPattern,
        FeatureKind::JoinBodies,    FeatureKind::Shell,
        FeatureKind::Draft,         FeatureKind::Extrude};
    const std::array<const char*, 14> names{
        "seed",    "extrude",  "revolve", "pocket", "fillet",
        "chamfer", "mirror",   "move",    "linear", "circular",
        "join",    "shell",    "draft",   "face extrude"};
    for (std::size_t index = 0; index < kinds.size(); ++index) {
      CHECK(document.bodies()[0].features()[index]->kind() == kinds[index]);
      CHECK(document.bodies()[0].features()[index]->id() == index + 1);
      CHECK(document.bodies()[0].features()[index]->name() == names[index]);
    }
    CHECK(document.bodies()[1].features().size() == 1);
    CHECK(document.bodies()[1].features()[0]->id() == kOtherProducer);
    CHECK(document.bodies()[1].features()[0]->name() == "other seed");

    const auto* imported = dynamic_cast<const ImportedShapeFeature*>(
        document.bodies()[0].features()[0].get());
    const auto* sketchExtrude = dynamic_cast<const ExtrudeFeature*>(
        document.bodies()[0].features()[1].get());
    const auto* revolve = dynamic_cast<const RevolveFeature*>(
        document.bodies()[0].features()[2].get());
    const auto* pocket = dynamic_cast<const PocketFeature*>(
        document.bodies()[0].features()[3].get());
    const auto* fillet = dynamic_cast<const FilletFeature*>(
        document.bodies()[0].features()[4].get());
    const auto* chamfer = dynamic_cast<const ChamferFeature*>(
        document.bodies()[0].features()[5].get());
    const auto* mirror = dynamic_cast<const MirrorFeature*>(
        document.bodies()[0].features()[6].get());
    const auto* move = dynamic_cast<const MoveFeature*>(
        document.bodies()[0].features()[7].get());
    const auto* linear = dynamic_cast<const LinearPatternFeature*>(
        document.bodies()[0].features()[8].get());
    const auto* circular = dynamic_cast<const CircularPatternFeature*>(
        document.bodies()[0].features()[9].get());
    const auto* join = dynamic_cast<const JoinBodiesFeature*>(
        document.bodies()[0].features()[10].get());
    const auto* shell = dynamic_cast<const ShellFeature*>(
        document.bodies()[0].features()[11].get());
    const auto* draft = dynamic_cast<const DraftFeature*>(
        document.bodies()[0].features()[12].get());
    const auto* extrude = dynamic_cast<const ExtrudeFeature*>(
        document.bodies()[0].features()[13].get());
    CHECK(imported && sketchExtrude && revolve && pocket && fillet && chamfer &&
          mirror && move && linear && circular && join && shell && draft &&
          extrude);
    CHECK(imported->importedShape());
    CHECK(!sketchExtrude->isFaceSource());
    CHECK(sketchExtrude->profileSketchId() == kSketch);
    CHECK(sketchExtrude->lengthMm() == 5.0);
    CHECK(sketchExtrude->operation() == ExtrudeOperation::NewBody);
    CHECK(!sketchExtrude->reversed());
    CHECK(revolve->profileSketchId() == kSketch);
    CHECK(revolve->axis().type == AxisReferenceType::GlobalX);
    CHECK(revolve->angleDeg() == 180.0);
    CHECK(revolve->operation() == ExtrudeOperation::NewBody);
    CHECK(!revolve->reversed());
    CHECK(pocket->profileSketchId() == kSketch);
    CHECK(pocket->depthMm() == 2.0);
    CHECK(fillet->radiusMm() == 0.5);
    CHECK(chamfer->distanceMm() == 0.25);
    CHECK(mirror->sourceFeatureId() == kProducer);
    CHECK(move->sourceFeatureId() == kProducer);
    CHECK(move->offsetMm().x == 1.0);
    CHECK(move->offsetMm().y == 2.0);
    CHECK(move->offsetMm().z == 3.0);
    CHECK(linear->sourceBodyId() == kBody);
    CHECK(linear->sourceFeatureId() == 8);
    CHECK(linear->direction() == PrincipalAxis::X);
    CHECK(linear->count() == 2);
    CHECK(linear->spacingMm() == 4.0);
    CHECK(linear->operation() == PatternOperation::Join);
    CHECK(circular->sourceBodyId() == kBody);
    CHECK(circular->sourceFeatureId() == 9);
    CHECK(circular->axis() == PrincipalAxis::Z);
    CHECK(circular->count() == 3);
    CHECK(circular->angleDeg() == 180.0);
    CHECK(circular->operation() == PatternOperation::Join);
    CHECK(join->firstBodyId() == kBody);
    CHECK(join->firstFeatureId() == 10);
    CHECK(join->secondBodyId() == kOtherBody);
    CHECK(join->secondFeatureId() == kOtherProducer);
    CHECK(shell->sourceFeatureId() == kProducer);
    CHECK(shell->thicknessMm() == 0.4);
    CHECK(!shell->outside());
    CHECK(draft->sourceFeatureId() == kProducer);
    CHECK(draft->neutralPlane().type == NeutralPlaneType::BodyFace);
    CHECK(draft->neutralPlane().face.has_value());
    CHECK(draft->pullDirection().type == AxisReferenceType::SketchLine);
    CHECK(draft->pullDirection().sketchId == kSketch);
    CHECK(draft->pullDirection().lineId == 11);
    CHECK(draft->angleDeg() == 3.0);
    CHECK(!draft->reversed());
    CHECK(extrude->isFaceSource());
    CHECK(extrude->lengthMm() == 1.0);
    CHECK(extrude->operation() == ExtrudeOperation::Join);
    CHECK(!extrude->reversed());
    CHECK(fillet->edges() ==
          std::vector<EdgeReference>({edgeV1(0, "fillet-v1"),
                                      richEdgeV2()}));
    CHECK(chamfer->edges() ==
          std::vector<EdgeReference>({edgeV2("chamfer-v2"),
                                      edgeV1(1, "chamfer-v1")}));
    CHECK(mirror->planeFace() == faceV1(2, "mirror-v1"));
    CHECK(shell->removedFaces() ==
          std::vector<FaceReference>({faceV1(3, "shell-v1"),
                                      richFaceV2()}));
    CHECK(fillet->edges()[1].v2());
    CHECK((fillet->edges()[1].v2()->provenance.occurrence ==
           TopologyOccurrenceKey{kProducer, 202}));
    CHECK(fillet->edges()[1].v2()->provenance.origins ==
          richEdgeV2().v2()->provenance.origins);
    CHECK(fillet->edges()[1].v2()->provenance.origins[0].featureId == 5);
    CHECK(fillet->edges()[1].v2()->provenance.origins[1].featureId == 6);
    CHECK(fillet->edges()[1].v2()->provenance.origins[2].featureId == 7);
    CHECK(shell->removedFaces()[1].v2());
    CHECK((shell->removedFaces()[1].v2()->provenance.occurrence ==
           TopologyOccurrenceKey{kProducer, 101}));
    CHECK(shell->removedFaces()[1].v2()->provenance.origins ==
          richFaceV2().v2()->provenance.origins);
    CHECK(shell->removedFaces()[1].v2()->provenance.origins[0].featureId == 2);
    CHECK(shell->removedFaces()[1].v2()->provenance.origins[1].featureId == 3);
    CHECK(shell->removedFaces()[1].v2()->provenance.origins[2].featureId == 4);
    CHECK(fillet->edges()[1].v2()->provenance.occurrence !=
          shell->removedFaces()[1].v2()->provenance.occurrence);
    CHECK(draft->neutralPlane().face == faceV2("draft-neutral"));
    CHECK(draft->rotationEdge() == edgeV2("draft-rotation"));
    CHECK(draft->draftedFaces() ==
          std::vector<FaceReference>({faceV2("drafted-zero"),
                                      faceV1(4, "drafted-one")}));
    CHECK(extrude->faceReference() == faceV2("extrude-face"));
    CHECK(!document.bodies()[0].features()[1]->shape());
  }

  // Public candidate structs are defensively checked against the authored
  // root; swapping two valid decoded carriers cannot redirect a consumer.
  {
    auto candidate = candidateFor(independentlyAuthoredTopologyRoot());
    std::swap(candidate.legacySketch.topology.topologyReferences[1],
              candidate.legacySketch.topology.topologyReferences[2]);
    checkInvalid(materializeProjectFormatV4Document(candidate));
  }

  // Strict Base64 can pass P2 while still not being an OCCT B-Rep. The late
  // failure publishes no Document and does not advance process-global IDs.
  {
    QJsonObject root = independentlyAuthoredTopologyRoot();
    auto model = root.value("model").toObject();
    auto bodies = model.value("bodies").toArray();
    auto body = bodies[0].toObject();
    auto features = body.value("features").toArray();
    auto imported = features[0].toObject();
    imported["brep"] = QStringLiteral("TQ==");
    features[0] = imported;
    body["features"] = features;
    bodies[0] = body;
    model["bodies"] = bodies;
    root["model"] = model;
    const auto candidate = candidateFor(root);

    const AutoIds beforeIds = consumeAutoIds("before-failure");
    const auto first = materializeProjectFormatV4Document(candidate);
    const auto second = materializeProjectFormatV4Document(candidate);
    checkInvalid(first);
    checkInvalid(second);
    CHECK(first.error == second.error);
    const AutoIds afterIds = consumeAutoIds("after-failure");
    checkOnlyExpectedAutoIdAdvance(beforeIds, afterIds);
  }

  // A structurally valid authored V2 miss still materializes. Runtime rebuild
  // remains an explicit later concern and may fail without changing the P3c
  // materialization verdict or carrier.
  {
    Document source;
    auto& body = source.addBody(kBody, "missing");
    body.addFeature(
        std::make_unique<ImportedShapeFeature>(kProducer, box(), "seed"));
    const FaceReference missing = faceV2("definitely-missing");
    body.addFeature(std::make_unique<ExtrudeFeature>(
        2, missing, 1.0, "missing face", ExtrudeOperation::Join, false));
    const auto encoded = encodeProjectFormatV4DocumentRoot(source, metadata());
    CHECK(encoded.succeeded());
    const auto candidate = candidateFor(*encoded.root);
    auto result = materializeProjectFormatV4Document(candidate);
    CHECK(result.succeeded());
    const auto* extrude = dynamic_cast<const ExtrudeFeature*>(
        result.document->bodies()[0].features()[1].get());
    CHECK(extrude);
    CHECK(extrude->faceReference() == missing);
    CHECK(!result.document->recompute());
    CHECK(extrude->faceReference() == missing);
  }

  return EXIT_SUCCESS;
}
