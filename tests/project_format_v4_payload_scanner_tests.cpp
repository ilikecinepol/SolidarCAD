#include "TestAssertions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>

#include "model/Document.h"
#include "model/Feature.h"
#include "project/ProjectFile.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"
#include "project/ProjectFormatV4PayloadScanner.h"

namespace {

using solidar::BodyId;
using solidar::FeatureId;
using solidar::FeatureKind;
using solidar::TopologyKind;
using solidar::project::ProjectFormatV4EnvelopeCandidate;
using solidar::project::ProjectFormatV4PayloadCandidate;
using solidar::project::ProjectFormatV4PayloadScanResult;
using solidar::project::ProjectFormatV4PayloadScanStatus;
using solidar::project::ProjectFormatV4TopologySiteFamily;
using solidar::project::ProjectFormatV4TopologySite;

template <typename T>
concept HasSucceeded = requires(const T& value) { value.succeeded(); };

template <typename T>
concept HasSuccessMember = requires(const T& value) { value.success; };

template <typename T>
concept HasDocumentMember = requires(const T& value) { value.document; };

template <typename T>
concept ExposesMaterializedDocument = requires(T& value) {
  static_cast<solidar::Document&>(value.document);
};

static_assert(!HasSucceeded<ProjectFormatV4PayloadScanResult>);
static_assert(!HasSuccessMember<ProjectFormatV4PayloadScanResult>);
static_assert(!HasDocumentMember<ProjectFormatV4PayloadScanResult>);
static_assert(!ExposesMaterializedDocument<ProjectFormatV4PayloadScanResult>);
static_assert(!HasSucceeded<ProjectFormatV4PayloadCandidate>);
static_assert(!HasSuccessMember<ProjectFormatV4PayloadCandidate>);
static_assert(!HasDocumentMember<ProjectFormatV4PayloadCandidate>);
static_assert(!ExposesMaterializedDocument<ProjectFormatV4PayloadCandidate>);
static_assert(!HasSucceeded<ProjectFormatV4EnvelopeCandidate>);
static_assert(!HasSuccessMember<ProjectFormatV4EnvelopeCandidate>);
static_assert(!ExposesMaterializedDocument<ProjectFormatV4EnvelopeCandidate>);
static_assert(!HasSucceeded<ProjectFormatV4TopologySite>);
static_assert(!HasSuccessMember<ProjectFormatV4TopologySite>);
static_assert(!HasDocumentMember<ProjectFormatV4TopologySite>);
static_assert(!ExposesMaterializedDocument<ProjectFormatV4TopologySite>);

QJsonObject parseObject(const QByteArray& bytes) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
  CHECK(error.error == QJsonParseError::NoError);
  CHECK(document.isObject());
  return document.object();
}

QJsonObject baseRoot() {
  // This deliberately stays hand-authored and independent of the writer.
  return parseObject(R"JSON({
    "format":"solidar-project",
    "version":4,
    "capabilities":["persistent-topology-v2"],
    "constraintTypeEncoding":"stable-name-v1",
    "name":"payload-fixture",
    "createdAt":"2026-10-21T00:00:00Z",
    "document":{"widthMm":60,"heightMm":40,"extrusionMm":25},
    "sketches":[],
    "extrusion":{"enabled":false},
    "model":{"sketches":[],"bodies":[],"referenceImages":[]}
  })JSON");
}

QJsonObject wrapper(QJsonObject reference = QJsonObject{{"opaque", true}}) {
  return QJsonObject{{"reference", reference}};
}

QJsonObject legacySketch(qint64 id) {
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

QJsonObject modelSketch(qint64 id, QJsonObject support = {{"type", 0}}) {
  return QJsonObject{{"id", id},
                     {"name", QStringLiteral("sketch")},
                     {"origin", QJsonArray{0, 0, 0}},
                     {"xDirection", QJsonArray{1, 0, 0}},
                     {"yDirection", QJsonArray{0, 1, 0}},
                     {"support", support}};
}

QString tokenFor(FeatureKind kind) {
  for (const auto& descriptor : solidar::project::featureCodecDescriptors())
    if (descriptor.kind == kind)
      return QString::fromLatin1(descriptor.token.data(),
                                 static_cast<qsizetype>(descriptor.token.size()));
  return {};
}

QJsonObject commonFeature(FeatureKind kind, qint64 id) {
  return QJsonObject{{"id", id}, {"name", "feature"}, {"type", tokenFor(kind)}};
}

QJsonObject profileOverride() {
  return QJsonObject{{"lines", QJsonArray{}},
                     {"circles", QJsonArray{}},
                     {"arcs", QJsonArray{}},
                     {"beziers", QJsonArray{}}};
}

QJsonObject featureFor(FeatureKind kind, qint64 id = 1) {
  QJsonObject feature = commonFeature(kind, id);
  switch (kind) {
    case FeatureKind::ImportedShape:
      feature["brep"] = "TQ==";
      break;
    case FeatureKind::Extrude:
      feature["lengthMm"] = 10;
      feature["operation"] = 0;
      feature["reversed"] = false;
      feature["sourceKind"] = "sketch";
      feature["sketchId"] = 900;
      feature["profileOverride"] = profileOverride();
      break;
    case FeatureKind::Revolve:
      feature["profileSketchId"] = 900;
      feature["axisType"] = 2;
      feature["axisSketchId"] = 901;
      feature["axisLineId"] = 902;
      feature["axisLineIndex"] = 0;
      feature["angleDeg"] = 180;
      feature["operation"] = 0;
      feature["reversed"] = false;
      feature["profileOverride"] = profileOverride();
      break;
    case FeatureKind::Pocket:
      feature["sketchId"] = 900;
      feature["depthMm"] = 10;
      break;
    case FeatureKind::Fillet:
      feature["radiusMm"] = 1;
      feature["edges"] = QJsonArray{wrapper()};
      break;
    case FeatureKind::Chamfer:
      feature["distanceMm"] = 1;
      feature["edges"] = QJsonArray{wrapper()};
      break;
    case FeatureKind::Mirror:
      feature["sourceFeatureId"] = 999;
      feature["plane"] = 0;
      break;
    case FeatureKind::Move:
      feature["sourceFeatureId"] = 999;
      feature["offsetXmm"] = 1;
      feature["offsetYmm"] = 2;
      feature["offsetZmm"] = 3;
      break;
    case FeatureKind::LinearPattern:
      feature["sourceBodyId"] = 777;
      feature["sourceFeatureId"] = 999;
      feature["direction"] = 0;
      feature["count"] = 2;
      feature["spacingMm"] = 5;
      feature["operation"] = 0;
      break;
    case FeatureKind::CircularPattern:
      feature["sourceBodyId"] = 777;
      feature["sourceFeatureId"] = 999;
      feature["axis"] = 2;
      feature["count"] = 2;
      feature["angleDeg"] = 180;
      feature["operation"] = 0;
      break;
    case FeatureKind::JoinBodies:
      feature["firstBodyId"] = 777;
      feature["firstFeatureId"] = 778;
      feature["secondBodyId"] = 779;
      feature["secondFeatureId"] = 780;
      break;
    case FeatureKind::Shell:
      feature["sourceFeatureId"] = 999;
      feature["thicknessMm"] = 1;
      feature["outside"] = false;
      feature["removedFaces"] = QJsonArray{wrapper()};
      break;
    case FeatureKind::Draft:
      feature["sourceFeatureId"] = 999;
      feature["angleDeg"] = 5;
      feature["reversed"] = false;
      feature["neutralPlaneType"] = 0;
      feature["pullDirectionType"] = 3;
      feature["pullDirectionSketchId"] = 0;
      feature["pullDirectionLineId"] = 0;
      feature["draftedFaces"] = QJsonArray{wrapper()};
      break;
    case FeatureKind::Unknown:
      break;
  }
  return feature;
}

QJsonObject body(qint64 id, QJsonArray features) {
  return QJsonObject{{"id", id},
                     {"name", "body"},
                     {"visible", true},
                     {"features", features}};
}

QJsonObject referenceImage(qint64 id) {
  return QJsonObject{{"id", id},
                     {"name", "image"},
                     {"sourcePath", "image.png"},
                     {"supportName", "XY"},
                     {"origin", QJsonArray{0, 0, 0}},
                     {"xDirection", QJsonArray{1, 0, 0}},
                     {"yDirection", QJsonArray{0, 1, 0}},
                     {"offsetX", 0},
                     {"offsetY", 0},
                     {"offsetZ", 0},
                     {"scale", 1},
                     {"pixelWidth", 10},
                     {"pixelHeight", 20},
                     {"visible", true}};
}

void setModel(QJsonObject* root, QJsonArray sketches, QJsonArray bodies,
              QJsonArray images = {}) {
  (*root)["model"] = QJsonObject{{"sketches", sketches},
                                  {"bodies", bodies},
                                  {"referenceImages", images}};
}

ProjectFormatV4EnvelopeCandidate envelopeFor(const QJsonObject& root) {
  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  auto envelope = solidar::project::scanProjectFormatV4Envelope(bytes);
  CHECK(envelope.hasCandidate());
  return std::move(*envelope.candidate);
}

ProjectFormatV4PayloadScanResult scan(const QJsonObject& root) {
  return solidar::project::scanProjectFormatV4Payload(envelopeFor(root));
}

void checkCandidate(const ProjectFormatV4PayloadScanResult& result) {
  CHECK(result.status == ProjectFormatV4PayloadScanStatus::Candidate);
  CHECK(result.hasCandidate());
  CHECK(result.candidate.has_value());
  CHECK(result.error.isEmpty());
}

void checkInvalid(const ProjectFormatV4PayloadScanResult& result) {
  CHECK(result.status == ProjectFormatV4PayloadScanStatus::Invalid);
  CHECK(!result.hasCandidate());
  CHECK(!result.candidate.has_value());
  CHECK(!result.error.isEmpty());
}

void checkInvalid(const QJsonObject& root) {
  checkInvalid(scan(root));
}

QJsonArray repeatedWrappers(qsizetype count) {
  QJsonArray result;
  for (qsizetype index = 0; index < count; ++index)
    result.append(wrapper(QJsonObject{{"ordinal", index}}));
  return result;
}

QJsonArray repeatedNulls(qsizetype count) {
  QJsonArray result;
  for (qsizetype index = 0; index < count; ++index) result.append(QJsonValue());
  return result;
}

QJsonObject allSiteRoot(const QJsonObject& reference) {
  QJsonObject extrude = featureFor(FeatureKind::Extrude, 1);
  extrude.remove("sketchId");
  extrude.remove("profileOverride");
  extrude["sourceKind"] = "face";
  extrude["face"] = wrapper(reference);
  QJsonObject fillet = featureFor(FeatureKind::Fillet, 2);
  fillet["edges"] = QJsonArray{wrapper(reference)};
  QJsonObject chamfer = featureFor(FeatureKind::Chamfer, 3);
  chamfer["edges"] = QJsonArray{wrapper(reference)};
  QJsonObject mirror = featureFor(FeatureKind::Mirror, 4);
  mirror["planeFace"] = wrapper(reference);
  QJsonObject shell = featureFor(FeatureKind::Shell, 5);
  shell["removedFaces"] = QJsonArray{wrapper(reference)};
  QJsonObject draft = featureFor(FeatureKind::Draft, 6);
  draft["neutralPlaneType"] = 3;
  draft["neutralPlaneFace"] = wrapper(reference);
  draft["rotationEdge"] = wrapper(reference);
  draft["draftedFaces"] = QJsonArray{wrapper(reference)};
  QJsonObject root = baseRoot();
  setModel(&root,
           {modelSketch(20, QJsonObject{{"type", 1},
                                         {"reference", reference}})},
           {body(30, {extrude, fillet, chamfer, mirror, shell, draft})});
  return root;
}

}  // namespace

int main() {
  // Minimal hand-authored candidate.
  {
    const auto result = scan(baseRoot());
    checkCandidate(result);
    CHECK(result.candidate->topologySites.empty());
  }

  // Every family accepts each opaque reference-object shape without asking
  // P2a to classify it.
  {
    const std::array<QJsonObject, 4> references{
        QJsonObject{{"referenceVersion", "V1"}, {"legacyIndex", 4}},
        QJsonObject{{"referenceVersion", "V2"}, {"provenance", "x"}},
        QJsonObject{{"referenceVersion", "V9"}, {"future", true}},
        QJsonObject{{"referenceVersion", 7}, {"kind", QJsonArray{}}}};
    for (const QJsonObject& reference : references) {
      const auto result = scan(allSiteRoot(reference));
      checkCandidate(result);
      CHECK(result.candidate->topologySites.size() == 9);
      for (const auto& site : result.candidate->topologySites)
        CHECK(site.authoredReference == reference);
    }
  }

  // Manifest and codec registry have exact parity for all 13 persisted kinds.
  {
    constexpr std::array<FeatureKind, 13> kinds{
        FeatureKind::ImportedShape, FeatureKind::Extrude,
        FeatureKind::Revolve,       FeatureKind::Pocket,
        FeatureKind::Fillet,        FeatureKind::Chamfer,
        FeatureKind::Mirror,        FeatureKind::Move,
        FeatureKind::LinearPattern, FeatureKind::CircularPattern,
        FeatureKind::JoinBodies,    FeatureKind::Shell,
        FeatureKind::Draft};
    static_assert(kinds.size() == solidar::kPersistedFeatureKindCount);
    const auto descriptors = solidar::project::featureCodecDescriptors();
    CHECK(descriptors.size() == kinds.size());
    std::set<FeatureKind> descriptorKinds;
    for (const auto& descriptor : descriptors)
      CHECK(descriptorKinds.insert(descriptor.kind).second);
    for (FeatureKind kind : kinds) {
      CHECK(descriptorKinds.contains(kind));
      CHECK(!tokenFor(kind).isEmpty());
      QJsonObject root = baseRoot();
      setModel(&root, {}, {body(1, {featureFor(kind)})});
      checkCandidate(scan(root));
    }
  }

  // Every documented embedding family is enumerated in authored order. Inner
  // references are deliberately opaque: V1, V2, future and malformed objects.
  {
    const QJsonObject v1{{"referenceVersion", "V1"}, {"legacyIndex", 4}};
    const QJsonObject v2{{"referenceVersion", "V2"}, {"provenance", "x"}};
    const QJsonObject future{{"referenceVersion", "V9"}, {"future", true}};
    const QJsonObject malformed{{"referenceVersion", 7}, {"kind", QJsonArray{}}};

    QJsonObject extrude = featureFor(FeatureKind::Extrude, 1);
    extrude.remove("sketchId");
    extrude.remove("profileOverride");
    extrude.remove("face");
    extrude["sourceKind"] = "face";
    extrude["face"] = wrapper(v2);
    QJsonObject fillet = featureFor(FeatureKind::Fillet, 2);
    fillet["edges"] = QJsonArray{wrapper(future)};
    QJsonObject chamfer = featureFor(FeatureKind::Chamfer, 3);
    chamfer["edges"] = QJsonArray{wrapper(malformed)};
    QJsonObject mirror = featureFor(FeatureKind::Mirror, 4);
    mirror["planeFace"] = wrapper(v1);
    QJsonObject shell = featureFor(FeatureKind::Shell, 5);
    shell["removedFaces"] = QJsonArray{wrapper(v2)};
    QJsonObject draft = featureFor(FeatureKind::Draft, 6);
    draft["neutralPlaneType"] = 3;
    draft["neutralPlaneFace"] = wrapper(future);
    draft["rotationEdge"] = wrapper(malformed);
    draft["draftedFaces"] = QJsonArray{wrapper(v1)};

    QJsonObject root = baseRoot();
    setModel(&root,
             {modelSketch(20, QJsonObject{{"type", 1},
                                           {"reference", malformed}})},
             {body(30, {extrude, fillet, chamfer, mirror, shell, draft})});
    const auto result = scan(root);
    checkCandidate(result);
    const auto& sites = result.candidate->topologySites;
    CHECK(sites.size() == 9);
    constexpr std::array<ProjectFormatV4TopologySiteFamily, 9> families{
        ProjectFormatV4TopologySiteFamily::SketchSupport,
        ProjectFormatV4TopologySiteFamily::ExtrudeFace,
        ProjectFormatV4TopologySiteFamily::FilletEdge,
        ProjectFormatV4TopologySiteFamily::ChamferEdge,
        ProjectFormatV4TopologySiteFamily::MirrorPlaneFace,
        ProjectFormatV4TopologySiteFamily::ShellRemovedFace,
        ProjectFormatV4TopologySiteFamily::DraftNeutralPlaneFace,
        ProjectFormatV4TopologySiteFamily::DraftRotationEdge,
        ProjectFormatV4TopologySiteFamily::DraftedFace};
    const std::array<QString, 9> paths{
        "$.model.sketches[0].support.reference",
        "$.model.bodies[0].features[0].face.reference",
        "$.model.bodies[0].features[1].edges[0].reference",
        "$.model.bodies[0].features[2].edges[0].reference",
        "$.model.bodies[0].features[3].planeFace.reference",
        "$.model.bodies[0].features[4].removedFaces[0].reference",
        "$.model.bodies[0].features[5].neutralPlaneFace.reference",
        "$.model.bodies[0].features[5].rotationEdge.reference",
        "$.model.bodies[0].features[5].draftedFaces[0].reference"};
    const std::array<QJsonValue, 9> references{
        malformed, v2, future, malformed, v1, v2, future, malformed, v1};
    constexpr std::array<FeatureId, 9> featureIds{0, 1, 2, 3, 4, 5, 6, 6,
                                                  6};
    for (std::size_t index = 0; index < sites.size(); ++index) {
      CHECK(sites[index].family == families[index]);
      CHECK(sites[index].path == paths[index]);
      CHECK(sites[index].expectedKind ==
            (index == 2 || index == 3 || index == 7 ? TopologyKind::Edge
                                                     : TopologyKind::Face));
      CHECK(sites[index].authoredReference == references[index]);
      if (index == 0) {
        CHECK(!sites[index].containingBodyId.has_value());
        CHECK(!sites[index].containingFeatureId.has_value());
      } else {
        CHECK(sites[index].containingBodyId == std::optional<BodyId>(30));
        CHECK(sites[index].containingFeatureId ==
              std::optional<FeatureId>(featureIds[index]));
      }
    }
  }

  // Wrapper closure/type and inline/hybrid topology forms.
  {
    for (int variant = 0; variant < 5; ++variant) {
      QJsonObject fillet = featureFor(FeatureKind::Fillet);
      if (variant == 0) fillet["edges"] = QJsonArray{QJsonObject{}};
      if (variant == 1)
        fillet["edges"] =
            QJsonArray{QJsonObject{{"reference", QJsonObject{}}, {"extra", 1}}};
      if (variant == 2)
        fillet["edges"] = QJsonArray{QJsonObject{{"reference", 7}}};
      if (variant == 3)
        fillet["edges"] = QJsonArray{QJsonObject{{"referenceVersion", "V1"}}};
      if (variant == 4) fillet["inlineTopologyReference"] = QJsonObject{};
      QJsonObject root = baseRoot();
      setModel(&root, {}, {body(1, {fillet})});
      checkInvalid(root);
    }
  }

  // Conditional topology sites and discriminator-dependent fields.
  {
    QJsonObject root = baseRoot();
    QJsonObject extrude = featureFor(FeatureKind::Extrude);
    extrude["face"] = wrapper();
    setModel(&root, {}, {body(1, {extrude})});
    checkInvalid(root);  // sketch source forbids face

    extrude.remove("sketchId");
    extrude.remove("profileOverride");
    extrude.remove("face");
    extrude["sourceKind"] = "face";
    setModel(&root, {}, {body(1, {extrude})});
    checkInvalid(root);  // face source requires face

    QJsonObject draft = featureFor(FeatureKind::Draft);
    draft["neutralPlaneFace"] = wrapper();
    setModel(&root, {}, {body(1, {draft})});
    checkInvalid(root);  // datum plane forbids reference

    draft.remove("neutralPlaneFace");
    draft["neutralPlaneType"] = 3;
    setModel(&root, {}, {body(1, {draft})});
    checkInvalid(root);  // face neutral plane requires reference

    for (FeatureKind kind : {FeatureKind::Fillet, FeatureKind::Chamfer,
                             FeatureKind::Shell, FeatureKind::Draft}) {
      QJsonObject feature = featureFor(kind);
      feature[kind == FeatureKind::Shell
                  ? "removedFaces"
                  : kind == FeatureKind::Draft ? "draftedFaces" : "edges"] =
          QJsonArray{};
      setModel(&root, {}, {body(1, {feature})});
      checkInvalid(root);
    }

    const std::array<QJsonObject, 4> invalidSupports{
        QJsonObject{{"type", 1}},
        QJsonObject{{"type", 0}, {"reference", QJsonObject{}}},
        QJsonObject{{"type", 1}, {"reference", 7}},
        QJsonObject{{"type", 1},
                    {"reference", QJsonObject{}},
                    {"bodyId", 1}}};
    for (const QJsonObject& support : invalidSupports) {
      setModel(&root, {modelSketch(1, support)}, {});
      checkInvalid(root);
    }

    QJsonObject revolve = featureFor(FeatureKind::Revolve);
    setModel(&root, {}, {body(1, {revolve})});
    checkCandidate(scan(root));  // sketch-line axis requires its index
    revolve.remove("axisLineIndex");
    setModel(&root, {}, {body(1, {revolve})});
    checkInvalid(root);
    revolve = featureFor(FeatureKind::Revolve);
    revolve["axisType"] = 3;
    revolve["axisSketchId"] = 0;
    revolve["axisLineId"] = 0;
    setModel(&root, {}, {body(1, {revolve})});
    checkInvalid(root);  // non-line axis forbids axisLineIndex
    revolve.remove("axisLineIndex");
    setModel(&root, {}, {body(1, {revolve})});
    checkCandidate(scan(root));

    draft = featureFor(FeatureKind::Draft);
    draft["pullDirectionType"] = 2;
    draft["pullDirectionSketchId"] = 1;
    draft["pullDirectionLineId"] = 1;
    draft["pullDirectionLineIndex"] = 0;
    setModel(&root, {}, {body(1, {draft})});
    checkCandidate(scan(root));
    draft.remove("pullDirectionLineIndex");
    setModel(&root, {}, {body(1, {draft})});
    checkInvalid(root);
    draft = featureFor(FeatureKind::Draft);
    draft["pullDirectionLineIndex"] = 0;
    setModel(&root, {}, {body(1, {draft})});
    checkInvalid(root);  // non-line pull direction forbids the index
    draft.remove("pullDirectionLineIndex");
    setModel(&root, {}, {body(1, {draft})});
    checkCandidate(scan(root));

    for (FeatureKind kind : {FeatureKind::LinearPattern,
                             FeatureKind::CircularPattern}) {
      QJsonObject pattern = featureFor(kind);
      pattern["operation"] = 0;
      pattern["sourceBodyId"] = 1;
      setModel(&root, {}, {body(2, {pattern})});
      checkCandidate(scan(root));
      pattern["sourceBodyId"] = 0;
      setModel(&root, {}, {body(2, {pattern})});
      checkInvalid(root);
      pattern["operation"] = 1;
      setModel(&root, {}, {body(2, {pattern})});
      checkCandidate(scan(root));
      pattern["sourceBodyId"] = 1;
      setModel(&root, {}, {body(2, {pattern})});
      checkCandidate(scan(root));  // stale cross-reference remains deferred
    }
  }

  // Placement bases use the exact v3 orthonormal tolerances for both model
  // sketches and reference images.
  {
    QJsonObject root = baseRoot();
    QJsonObject sketch = modelSketch(1);
    sketch["yDirection"] = QJsonArray{1.0e-9, 1.0, 0.0};
    setModel(&root, {sketch}, {});
    checkCandidate(scan(root));

    QJsonObject image = referenceImage(1);
    image["yDirection"] = QJsonArray{1.0e-9, 1.0, 0.0};
    setModel(&root, {}, {}, {image});
    checkCandidate(scan(root));

    const std::array<std::pair<QJsonArray, QJsonArray>, 3> invalidBases{
        std::pair{QJsonArray{0, 0, 0}, QJsonArray{0, 1, 0}},
        std::pair{QJsonArray{2, 0, 0}, QJsonArray{0, 1, 0}},
        std::pair{QJsonArray{1, 0, 0}, QJsonArray{1, 0, 0}}};
    for (const auto& [xDirection, yDirection] : invalidBases) {
      sketch = modelSketch(1);
      sketch["xDirection"] = xDirection;
      sketch["yDirection"] = yDirection;
      setModel(&root, {sketch}, {});
      checkInvalid(root);

      image = referenceImage(1);
      image["xDirection"] = xDirection;
      image["yDirection"] = yDirection;
      setModel(&root, {}, {}, {image});
      checkInvalid(root);
    }
  }

  // Closed immediate shells for legacy sketches, extrusion, model members and
  // reference images, plus duplicate persisted-ID domains.
  {
    QJsonObject root = baseRoot();
    root["sketches"] = QJsonArray{legacySketch(1), legacySketch(1)};
    checkInvalid(root);

    root = baseRoot();
    root["extrusion"] = QJsonObject{{"enabled", false}, {"extra", 1}};
    checkInvalid(root);

    for (int domain = 0; domain < 4; ++domain) {
      root = baseRoot();
      if (domain == 0)
        setModel(&root, {modelSketch(1), modelSketch(1)}, {});
      if (domain == 1)
        setModel(&root, {}, {body(1, {}), body(1, {})});
      if (domain == 2)
        setModel(&root, {},
                 {body(1, {featureFor(FeatureKind::Move, 7)}),
                  body(2, {featureFor(FeatureKind::Pocket, 7)})});
      if (domain == 3)
        setModel(&root, {}, {}, {referenceImage(1), referenceImage(1)});
      checkInvalid(root);
    }

    root = baseRoot();
    QJsonObject badImage = referenceImage(1);
    badImage["extra"] = 1;
    setModel(&root, {}, {}, {badImage});
    checkInvalid(root);
  }

  // Non-topology references are not resolved by this stage.
  {
    QJsonObject root = baseRoot();
    setModel(&root, {},
             {body(1, {featureFor(FeatureKind::JoinBodies, 1),
                        featureFor(FeatureKind::Move, 2),
                        featureFor(FeatureKind::LinearPattern, 3)})});
    checkCandidate(scan(root));
  }

  // ImportedShape only proves the bounded non-empty string shell. Base64
  // acceptance remains deferred to the existing v3-compatible decoder so P2c
  // cannot narrow legacy syntax without allocating a decoded buffer.
  {
    for (const QString& accepted : {QStringLiteral("TQ=="),
                                    QStringLiteral("TWE="),
                                    QStringLiteral("TWFu"),
                                    QStringLiteral("M"),
                                    QStringLiteral("TQ=A"),
                                    QStringLiteral("TR=="),
                                    QStringLiteral("@@@@")}) {
      QJsonObject root = baseRoot();
      QJsonObject imported = featureFor(FeatureKind::ImportedShape);
      imported["brep"] = accepted;
      setModel(&root, {}, {body(1, {imported})});
      checkCandidate(scan(root));
    }
    QJsonObject root = baseRoot();
    QJsonObject imported = featureFor(FeatureKind::ImportedShape);
    imported["brep"] = QString{};
    setModel(&root, {}, {body(1, {imported})});
    checkInvalid(root);
    imported["brep"] = 7;
    setModel(&root, {}, {body(1, {imported})});
    checkInvalid(root);
  }

  // Generic nested-array limits, aggregate profile geometry, rebuild work,
  // and the full positive image-scale range match the v3 document budgets.
  {
    for (const char* field : {"dimensions", "centerNodeElementIds"}) {
      QJsonObject root = baseRoot();
      QJsonObject sketch = legacySketch(1);
      sketch[QLatin1String(field)] = repeatedNulls(100000);
      root["sketches"] = QJsonArray{sketch};
      checkCandidate(scan(root));
      sketch[QLatin1String(field)] = repeatedNulls(100001);
      root["sketches"] = QJsonArray{sketch};
      checkInvalid(root);
    }

    QJsonObject root = baseRoot();
    QJsonArray legacy;
    for (qint64 id = 1; id <= 5; ++id) {
      QJsonObject sketch = legacySketch(id);
      sketch["lines"] = repeatedNulls(10000);
      legacy.append(sketch);
    }
    root["sketches"] = legacy;
    checkCandidate(scan(root));
    QJsonObject extrude = featureFor(FeatureKind::Extrude);
    QJsonObject override = profileOverride();
    override["beziers"] = QJsonArray{QJsonValue()};
    extrude["profileOverride"] = override;
    setModel(&root, {}, {body(1, {extrude})});
    checkInvalid(root);

    root = baseRoot();
    QJsonArray patterns;
    for (qint64 id = 1; id <= 100; ++id) {
      QJsonObject pattern = featureFor(FeatureKind::LinearPattern, id);
      pattern["count"] = 100;
      patterns.append(pattern);
    }
    setModel(&root, {}, {body(1, patterns)});
    checkCandidate(scan(root));
    patterns.append(featureFor(FeatureKind::Move, 101));
    setModel(&root, {}, {body(1, patterns)});
    checkInvalid(root);

    for (double scale : {std::numeric_limits<double>::denorm_min(),
                         10000.0}) {
      root = baseRoot();
      QJsonObject image = referenceImage(1);
      image["scale"] = scale;
      setModel(&root, {}, {}, {image});
      checkCandidate(scan(root));
    }
    root = baseRoot();
    QJsonObject image = referenceImage(1);
    image["scale"] = 0.0;
    setModel(&root, {}, {}, {image});
    checkInvalid(root);
    image["scale"] = std::nextafter(10000.0,
                                     std::numeric_limits<double>::infinity());
    setModel(&root, {}, {}, {image});
    checkInvalid(root);
  }

  // Per-feature and document topology-reference budgets.
  {
    QJsonObject root = baseRoot();
    QJsonObject fillet = featureFor(FeatureKind::Fillet);
    fillet["edges"] = repeatedWrappers(1024);
    setModel(&root, {}, {body(1, {fillet})});
    checkCandidate(scan(root));

    QJsonArray overFeatureLimit = repeatedWrappers(1025);
    overFeatureLimit[0] = QJsonObject{};
    fillet["edges"] = overFeatureLimit;
    setModel(&root, {}, {body(1, {fillet})});
    const auto overFeature = scan(root);
    checkInvalid(overFeature);
    CHECK(overFeature.error.contains("per-feature topology limit"));

    QJsonArray features;
    for (int index = 0; index < 4; ++index) {
      QJsonObject bounded = featureFor(FeatureKind::Fillet, index + 1);
      bounded["edges"] = repeatedWrappers(1024);
      features.append(bounded);
    }
    setModel(&root, {}, {body(1, features)});
    checkCandidate(scan(root));

    QJsonObject oneMore = featureFor(FeatureKind::Fillet, 5);
    oneMore["edges"] = QJsonArray{QJsonObject{}};
    features.append(oneMore);
    setModel(&root, {}, {body(1, features)});
    const auto overDocument = scan(root);
    checkInvalid(overDocument);
    CHECK(overDocument.error.contains("document topology limit"));
  }

  // The original authored root, all P2b projections and deferred unknowns are
  // moved through unchanged. A tampered projection is rejected.
  {
    QJsonObject root = baseRoot();
    root["capabilities"] =
        QJsonArray{"future-capability", "persistent-topology-v2"};
    root["constraintTypeEncoding"] = "future-encoding";
    root["sketches"] = QJsonArray{legacySketch(42)};
    root["extrusion"] = QJsonObject{{"enabled", true}, {"sourceSketch", 0}};
    setModel(&root, {modelSketch(7)}, {}, {referenceImage(9)});
    const ProjectFormatV4EnvelopeCandidate original = envelopeFor(root);
    const auto originalDeferred = original.deferredUnknowns;
    const auto result = solidar::project::scanProjectFormatV4Payload(original);
    checkCandidate(result);
    const auto& preserved = result.candidate->envelope;
    CHECK(preserved.authoredRoot == original.authoredRoot);
    CHECK(preserved.document == original.document);
    CHECK(preserved.legacySketches == original.legacySketches);
    CHECK(preserved.legacyExtrusion == original.legacyExtrusion);
    CHECK(preserved.model == original.model);
    CHECK(preserved.capabilities == original.capabilities);
    CHECK(preserved.deferredUnknowns == originalDeferred);

    auto tampered = original;
    tampered.model["bodies"] = QJsonArray{body(99, {})};
    const auto first = solidar::project::scanProjectFormatV4Payload(tampered);
    const auto second = solidar::project::scanProjectFormatV4Payload(tampered);
    checkInvalid(first);
    checkInvalid(second);
    CHECK(first.error == second.error);

    tampered = original;
    tampered.deferredUnknowns.clear();
    checkInvalid(solidar::project::scanProjectFormatV4Payload(tampered));
  }

  // Revalidating authoritative authoredRoot rejects even synchronized edits
  // to the mutable outer envelope and its projections.
  {
    const auto original = envelopeFor(baseRoot());
    auto expectTamperedInvalid = [](ProjectFormatV4EnvelopeCandidate value) {
      checkInvalid(solidar::project::scanProjectFormatV4Payload(
          std::move(value)));
    };

    auto tampered = original;
    tampered.authoredRoot["extra"] = 1;
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot.remove("extrusion");
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["sketches"] = QJsonObject{};
    tampered.legacySketches = {};
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["document"] = QJsonArray{};
    tampered.document = {};
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["model"] = QJsonArray{};
    tampered.model = {};
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["model"] = QJsonObject{};
    tampered.model = {};
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["capabilities"] = QJsonArray{};
    tampered.capabilities.clear();
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["format"] = 7;
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["version"] = 0;
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["version"] = 5;
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["name"] = 7;
    expectTamperedInvalid(std::move(tampered));

    tampered = original;
    tampered.authoredRoot["createdAt"] = QString(16385, QLatin1Char('x'));
    expectTamperedInvalid(std::move(tampered));
  }

  return EXIT_SUCCESS;
}
