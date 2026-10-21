#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <memory>
#include <set>
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

TopologyProvenanceOrigin origin(FeatureId featureId, TopologyKind kind,
                                std::string localName) {
  return {featureId, kind, TopologyProvenanceOperation::Seed,
          TopologySemanticRole::Custom, std::move(localName)};
}

FaceReference v2Face(std::vector<TopologyProvenanceOrigin> origins = {}) {
  return FaceReference::fromV2(
      {kBody,
       TopologyProvenanceKey{kProducer, TopologyKind::Face,
                             TopologyProvenanceOperation::Generated,
                             TopologySemanticRole::Side, "face",
                             std::move(origins)}});
}

EdgeReference v2Edge(std::vector<TopologyProvenanceOrigin> origins = {}) {
  return EdgeReference::fromV2(
      {kBody,
       TopologyProvenanceKey{kProducer, TopologyKind::Edge,
                             TopologyProvenanceOperation::Generated,
                             TopologySemanticRole::Custom, "edge",
                             std::move(origins)}});
}

EdgeReference v2EdgeWithNonCanonicalOrigins() {
  TopologyReferenceV2 reference{
      kBody,
      TopologyProvenanceKey{kProducer, TopologyKind::Edge,
                            TopologyProvenanceOperation::Generated,
                            TopologySemanticRole::Custom, "edge"}};
  reference.provenance.origins = {
      origin(kOtherProducer, TopologyKind::Edge, "other"),
      origin(kProducer, TopologyKind::Edge, "seed"),
      origin(kOtherProducer, TopologyKind::Edge, "other")};
  return EdgeReference::fromV2(std::move(reference));
}

ProjectFormatV4DocumentMetadata metadata() {
  return {QString::fromUtf8("Деталь Ω"),
          QStringLiteral("2026-10-21T12:34:56Z")};
}

void checkInvalid(const ProjectFormatV4DocumentEncodeResult& result) {
  CHECK(result.status == ProjectFormatV4DocumentEncodeStatus::Invalid);
  CHECK(!result.succeeded());
  CHECK(!result.root);
  CHECK(result.topologyReferenceCount == 0);
  CHECK(result.provenanceRecordCount == 0);
  CHECK(result.localNameUtf8Bytes == 0);
  CHECK(!result.error.isEmpty());
}

Document topologyDocument() {
  Document document;
  document.setBox({71.0, 43.0, 29.0});
  auto& profile = document.addSketch(kSketch, "profile");
  profile.geometry.addRectangle({0.0, 0.0}, {10.0, 8.0});
  profile.support.type = SketchSupportType::Face;
  profile.support.face = FaceReference(kBody, kProducer, 0, "support");

  auto& body = document.addBody(kBody, "main");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kProducer, box(), "seed"));
  body.addFeature(std::make_unique<ExtrudeFeature>(
      2, kSketch, 5.0, "extrude", ExtrudeOperation::NewBody, false));
  body.addFeature(std::make_unique<RevolveFeature>(
      3, kSketch, AxisReference{AxisReferenceType::GlobalX}, 180.0,
      "revolve", ExtrudeOperation::NewBody, false));
  body.addFeature(std::make_unique<PocketFeature>(4, kSketch, 2.0, "pocket"));
  body.addFeature(std::make_unique<FilletFeature>(
      5, std::vector<EdgeReference>{EdgeReference(kBody, kProducer, 0),
                                    v2EdgeWithNonCanonicalOrigins()},
      0.5, "fillet"));
  body.addFeature(std::make_unique<ChamferFeature>(
      6, std::vector<EdgeReference>{v2Edge(),
                                    EdgeReference(kBody, kProducer, 1)},
      0.25, "chamfer"));
  body.addFeature(std::make_unique<MirrorFeature>(
      7, kProducer, FaceReference(kBody, kProducer, 2), "mirror"));
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
      12, kProducer,
      std::vector<FaceReference>{FaceReference(kBody, kProducer, 3),
                                 v2Face()},
      0.4, false, "shell"));
  body.addFeature(std::make_unique<DraftFeature>(
      13, kProducer,
      std::vector<FaceReference>{v2Face(),
                                 FaceReference(kBody, kProducer, 4)},
      PlaneReference{NeutralPlaneType::BodyFace, v2Face()},
      AxisReference{AxisReferenceType::GlobalZ}, 3.0, false, "draft",
      v2Edge()));
  body.addFeature(std::make_unique<ExtrudeFeature>(
      14, v2Face(), 1.0, "face extrude", ExtrudeOperation::Join, false));

  document.addBody(kOtherBody, "other")
      .addFeature(std::make_unique<ImportedShapeFeature>(
          kOtherProducer, box(), "other seed"));
  return document;
}

struct ExpectedSite {
  ProjectFormatV4TopologySiteFamily family;
  const char* path;
  TopologyKind kind;
  std::optional<BodyId> bodyId;
  std::optional<FeatureId> featureId;
  const char* referenceVersion;
};

QString referenceVersion(const ProjectFormatV4TopologySite& site) {
  return site.authoredReference.toObject().value("referenceVersion").toString();
}

FaceReference budgetV2Face(std::size_t localNameBytes,
                           std::size_t originCount) {
  std::vector<TopologyProvenanceOrigin> origins;
  origins.reserve(originCount);
  for (std::size_t index = 0; index < originCount; ++index) {
    origins.push_back(origin(
        kProducer, TopologyKind::Face,
        std::string("o") + std::to_string(index)));
  }
  return FaceReference::fromV2(
      {kBody,
       TopologyProvenanceKey{kProducer, TopologyKind::Face,
                             TopologyProvenanceOperation::Generated,
                             TopologySemanticRole::Side,
                             std::string(localNameBytes, 'n'),
                             std::move(origins)}});
}

Document topologyBudgetDocument(std::size_t referenceCount,
                                const FaceReference& common,
                                std::optional<FaceReference> final = {}) {
  Document document;
  auto& body = document.addBody(kBody, "budget");
  body.addFeature(
      std::make_unique<ImportedShapeFeature>(kProducer, box(), "seed"));
  std::size_t emitted = 0;
  FeatureId featureId = 2;
  while (emitted < referenceCount) {
    const std::size_t count =
        std::min<std::size_t>(1024, referenceCount - emitted);
    std::vector<FaceReference> faces(count, common);
    if (final && emitted + count == referenceCount)
      faces.back() = *final;
    body.addFeature(std::make_unique<ShellFeature>(
        featureId++, kProducer, std::move(faces), 0.5, false, "budget"));
    emitted += count;
  }
  return document;
}

void checkDeterministicInvalid(const Document& document) {
  const auto first = encodeProjectFormatV4DocumentRoot(document, metadata());
  const auto second = encodeProjectFormatV4DocumentRoot(document, metadata());
  checkInvalid(first);
  checkInvalid(second);
  CHECK(first.error == second.error);
}

}  // namespace

int main() {
  // Exact deterministic envelope for the smallest typed document. The seam
  // has no path, clock or filesystem dependency.
  {
    Document document;
    document.setBox({70.0, 40.0, 30.0});
    const auto first = encodeProjectFormatV4DocumentRoot(document, metadata());
    const auto second = encodeProjectFormatV4DocumentRoot(document, metadata());
    CHECK(first.succeeded());
    CHECK(second.succeeded());
    CHECK(first.root == second.root);
    CHECK(first.topologyReferenceCount == 0);
    CHECK(first.provenanceRecordCount == 0);
    CHECK(first.localNameUtf8Bytes == 0);

    const QJsonObject& root = *first.root;
    const std::set<QString> expectedKeys{
        "format", "version", "capabilities", "constraintTypeEncoding",
        "name", "createdAt", "document", "sketches", "extrusion", "model"};
    std::set<QString> actualKeys;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
      actualKeys.insert(it.key());
    CHECK(actualKeys == expectedKeys);
    CHECK(root.value("format") == QStringLiteral("solidar-project"));
    CHECK(root.value("version") == 4);
    CHECK(root.value("capabilities").toArray() ==
          QJsonArray{QStringLiteral("persistent-topology-v2")});
    CHECK(root.value("constraintTypeEncoding") ==
          QStringLiteral("stable-name-v1"));
    CHECK(root.value("name") == metadata().name);
    CHECK(root.value("createdAt") == metadata().createdAt);
    const QJsonObject expectedDocument{{"widthMm", 70.0},
                                       {"heightMm", 40.0},
                                       {"extrusionMm", 30.0}};
    CHECK(root.value("document").toObject() == expectedDocument);
    CHECK(root.value("sketches").toArray() == QJsonArray{});
    const QJsonObject expectedExtrusion{{"enabled", false}};
    CHECK(root.value("extrusion").toObject() == expectedExtrusion);
    const QJsonObject expectedModel{
        {"sketches", QJsonArray{}},
        {"bodies", QJsonArray{}},
        {"referenceImages", QJsonArray{}}};
    CHECK(root.value("model").toObject() == expectedModel);
  }

  // Runtime enrichment of an index-only V1 sketch support is transient. The
  // v4 encoder must persist the authored carrier literally, without deriving
  // a tag or signature from the current B-Rep.
  {
    Document document;
    auto& body = document.addBody(kBody, "runtime support owner");
    body.addFeature(
        std::make_unique<ImportedShapeFeature>(kProducer, box(), "seed"));
    CHECK(document.recompute());
    auto& sketch = document.addSketch(kSketch, "runtime enriched support");
    const FaceReference authored(kBody, kProducer, 0);
    sketch.support = {SketchSupportType::Face, authored};
    document.updateSketchPlacements();
    CHECK(sketch.supportResolved);
    CHECK(sketch.support.face == authored);
    CHECK(sketch.effectiveSupportFace);
    CHECK(sketch.effectiveSupportFace->signature());

    const auto encoded =
        encodeProjectFormatV4DocumentRoot(document, metadata());
    CHECK(encoded.succeeded());
    const auto reference =
        encoded.root->value("model")
            .toObject()
            .value("sketches")
            .toArray()[0]
            .toObject()
            .value("support")
            .toObject()
            .value("reference")
            .toObject();
    const QJsonObject expected{{"referenceVersion", "V1"},
                               {"bodyId", static_cast<qint64>(kBody)},
                               {"featureId",
                                static_cast<qint64>(kProducer)},
                               {"kind", "Face"},
                               {"legacyIndex", 0}};
    CHECK(reference == expected);
    CHECK(!reference.contains("persistentTag"));
    CHECK(!reference.contains("signature"));
  }

  // One typed fixture exercises all 13 codec tokens, all nine topology-site
  // families, mixed V1/V2 arrays and the P2 accounting oracle.
  {
    Document document = topologyDocument();
    const auto beforeFace =
        dynamic_cast<const ShellFeature*>(document.bodies()[0].features()[11].get())
            ->removedFaces()[1];
    const auto beforeEdge =
        dynamic_cast<const FilletFeature*>(document.bodies()[0].features()[4].get())
            ->edges()[1];
    const auto encoded = encodeProjectFormatV4DocumentRoot(document, metadata());
    CHECK(encoded.succeeded());
    CHECK(encoded.topologyReferenceCount == 13);
    CHECK(encoded.provenanceRecordCount == 9);
    CHECK(encoded.localNameUtf8Bytes == 37);
    CHECK(dynamic_cast<const ShellFeature*>(
              document.bodies()[0].features()[11].get())
              ->removedFaces()[1] == beforeFace);
    CHECK(dynamic_cast<const FilletFeature*>(
              document.bodies()[0].features()[4].get())
              ->edges()[1] == beforeEdge);

    const auto model = encoded.root->value("model").toObject();
    const auto bodies = model.value("bodies").toArray();
    const auto features = bodies[0].toObject().value("features").toArray();
    CHECK(features.size() == 14);
    QJsonArray tokens;
    for (const auto& value : features)
      tokens.append(value.toObject().value("type"));
    const QJsonArray expectedTokens{
        "ImportedShape", "Extrude",       "Revolve",       "Pocket",
        "Fillet",       "Chamfer",       "Mirror",        "Move",
        "LinearPattern", "CircularPattern", "JoinBodies", "Shell",
        "Draft",        "Extrude"};
    CHECK(tokens == expectedTokens);
    CHECK(features[4].toObject().value("edges").toArray()[0]
              .toObject().value("reference").toObject()
              .value("referenceVersion") == QStringLiteral("V1"));
    CHECK(features[4].toObject().value("edges").toArray()[1]
              .toObject().value("reference").toObject()
              .value("referenceVersion") == QStringLiteral("V2"));
    const auto origins = features[4].toObject().value("edges").toArray()[1]
                             .toObject().value("reference").toObject()
                             .value("provenance").toObject()
                             .value("origins").toArray();
    CHECK(origins.size() == 2);
    CHECK(origins[0].toObject().value("featureId") ==
          static_cast<qint64>(kProducer));
    CHECK(origins[1].toObject().value("featureId") ==
          static_cast<qint64>(kOtherProducer));

    auto envelope = detail::validateProjectFormatV4EnvelopeRoot(*encoded.root);
    CHECK(envelope.hasCandidate());
    auto payload = scanProjectFormatV4Payload(std::move(*envelope.candidate));
    CHECK(payload.hasCandidate());
    CHECK(payload.candidate->topologySites.size() ==
          encoded.topologyReferenceCount);

    const std::array<ExpectedSite, 13> expectedSites{{
        {ProjectFormatV4TopologySiteFamily::SketchSupport,
         "$.model.sketches[0].support.reference", TopologyKind::Face,
         std::nullopt, std::nullopt, "V1"},
        {ProjectFormatV4TopologySiteFamily::FilletEdge,
         "$.model.bodies[0].features[4].edges[0].reference", TopologyKind::Edge,
         kBody, 5, "V1"},
        {ProjectFormatV4TopologySiteFamily::FilletEdge,
         "$.model.bodies[0].features[4].edges[1].reference", TopologyKind::Edge,
         kBody, 5, "V2"},
        {ProjectFormatV4TopologySiteFamily::ChamferEdge,
         "$.model.bodies[0].features[5].edges[0].reference", TopologyKind::Edge,
         kBody, 6, "V2"},
        {ProjectFormatV4TopologySiteFamily::ChamferEdge,
         "$.model.bodies[0].features[5].edges[1].reference", TopologyKind::Edge,
         kBody, 6, "V1"},
        {ProjectFormatV4TopologySiteFamily::MirrorPlaneFace,
         "$.model.bodies[0].features[6].planeFace.reference", TopologyKind::Face,
         kBody, 7, "V1"},
        {ProjectFormatV4TopologySiteFamily::ShellRemovedFace,
         "$.model.bodies[0].features[11].removedFaces[0].reference", TopologyKind::Face,
         kBody, 12, "V1"},
        {ProjectFormatV4TopologySiteFamily::ShellRemovedFace,
         "$.model.bodies[0].features[11].removedFaces[1].reference", TopologyKind::Face,
         kBody, 12, "V2"},
        {ProjectFormatV4TopologySiteFamily::DraftNeutralPlaneFace,
         "$.model.bodies[0].features[12].neutralPlaneFace.reference",
         TopologyKind::Face, kBody, 13, "V2"},
        {ProjectFormatV4TopologySiteFamily::DraftRotationEdge,
         "$.model.bodies[0].features[12].rotationEdge.reference", TopologyKind::Edge,
         kBody, 13, "V2"},
        {ProjectFormatV4TopologySiteFamily::DraftedFace,
         "$.model.bodies[0].features[12].draftedFaces[0].reference", TopologyKind::Face,
         kBody, 13, "V2"},
        {ProjectFormatV4TopologySiteFamily::DraftedFace,
         "$.model.bodies[0].features[12].draftedFaces[1].reference", TopologyKind::Face,
         kBody, 13, "V1"},
        {ProjectFormatV4TopologySiteFamily::ExtrudeFace,
         "$.model.bodies[0].features[13].face.reference", TopologyKind::Face,
         kBody, 14, "V2"},
    }};
    for (std::size_t index = 0; index < expectedSites.size(); ++index) {
      const auto& actual = payload.candidate->topologySites[index];
      const auto& expected = expectedSites[index];
      CHECK(actual.family == expected.family);
      CHECK(actual.path == QString::fromLatin1(expected.path));
      CHECK(actual.expectedKind == expected.kind);
      CHECK(actual.containingBodyId == expected.bodyId);
      CHECK(actual.containingFeatureId == expected.featureId);
      CHECK(referenceVersion(actual) ==
            QString::fromLatin1(expected.referenceVersion));
    }

    const auto versions = [](const QJsonArray& values) {
      QJsonArray result;
      for (const auto& value : values)
        result.append(value.toObject().value("reference").toObject().value(
            "referenceVersion"));
      return result;
    };
    CHECK(versions(features[4].toObject().value("edges").toArray()) ==
          QJsonArray({"V1", "V2"}));
    CHECK(versions(features[5].toObject().value("edges").toArray()) ==
          QJsonArray({"V2", "V1"}));
    CHECK(versions(features[11].toObject().value("removedFaces").toArray()) ==
          QJsonArray({"V1", "V2"}));
    CHECK(versions(features[12].toObject().value("draftedFaces").toArray()) ==
          QJsonArray({"V2", "V1"}));
    auto validated = validateProjectFormatV4Model(std::move(*payload.candidate));
    CHECK(validated.status == ProjectFormatV4ModelValidationStatus::Candidate);
    CHECK(validated.hasCandidate());
    CHECK(validated.candidate->legacySketch.topology.provenanceRecordCount ==
          encoded.provenanceRecordCount);
    CHECK(validated.candidate->legacySketch.topology.localNameUtf8Bytes ==
          encoded.localNameUtf8Bytes);
  }

  // Failure is atomic: neither a partial root nor partial counters escape.
  {
    Document document;
    auto& body = document.addBody(kBody, "budget");
    body.addFeature(
        std::make_unique<ImportedShapeFeature>(kProducer, box(), "seed"));
    std::vector<FaceReference> faces;
    faces.reserve(1025);
    for (std::size_t index = 0; index < 1024; ++index)
      faces.emplace_back(kBody, kProducer, index);
    auto shell = std::make_unique<ShellFeature>(
        2, kProducer, std::move(faces), 0.5, false, "boundary");
    auto* shellPtr = shell.get();
    body.addFeature(std::move(shell));
    const auto boundary =
        encodeProjectFormatV4DocumentRoot(document, metadata());
    CHECK(boundary.succeeded());
    CHECK(boundary.topologyReferenceCount == 1024);

    auto oversized = shellPtr->removedFaces();
    oversized.emplace_back(kBody, kProducer, 1024);
    shellPtr->setRemovedFaces(std::move(oversized));
    const auto first = encodeProjectFormatV4DocumentRoot(document, metadata());
    const auto second = encodeProjectFormatV4DocumentRoot(document, metadata());
    checkInvalid(first);
    checkInvalid(second);
    CHECK(first.error == second.error);
  }

  // Exact document-wide reference boundary, split so no individual feature
  // exceeds its independent 1024-reference budget.
  {
    const FaceReference common(kBody, kProducer, 0);
    const Document boundary = topologyBudgetDocument(4096, common);
    const auto encoded =
        encodeProjectFormatV4DocumentRoot(boundary, metadata());
    CHECK(encoded.succeeded());
    CHECK(encoded.topologyReferenceCount == 4096);
    CHECK(encoded.provenanceRecordCount == 0);
    CHECK(encoded.localNameUtf8Bytes == 0);

    const Document oversized = topologyBudgetDocument(4097, common);
    checkDeterministicInvalid(oversized);
  }

  // 4096 references with one top-level provenance record and 15 immediate
  // origins each total exactly 65536 records. One extra origin is rejected.
  {
    const FaceReference sixteenRecords = budgetV2Face(1, 15);
    const Document boundary =
        topologyBudgetDocument(4096, sixteenRecords);
    const auto encoded =
        encodeProjectFormatV4DocumentRoot(boundary, metadata());
    CHECK(encoded.succeeded());
    CHECK(encoded.topologyReferenceCount == 4096);
    CHECK(encoded.provenanceRecordCount == 65536);

    const FaceReference seventeenRecords = budgetV2Face(1, 16);
    const Document oversized =
        topologyBudgetDocument(4096, sixteenRecords, seventeenRecords);
    checkDeterministicInvalid(oversized);
  }

  // Local-name accounting is UTF-8 byte based. 4096 references with 2048
  // ASCII bytes each hit 8 MiB exactly; one additional byte is rejected.
  {
    const FaceReference twoKiBName = budgetV2Face(2048, 0);
    const Document boundary = topologyBudgetDocument(4096, twoKiBName);
    const auto encoded =
        encodeProjectFormatV4DocumentRoot(boundary, metadata());
    CHECK(encoded.succeeded());
    CHECK(encoded.topologyReferenceCount == 4096);
    CHECK(encoded.localNameUtf8Bytes == 8U * 1024U * 1024U);

    const FaceReference twoKiBPlusOneName = budgetV2Face(2049, 0);
    const Document oversized =
        topologyBudgetDocument(4096, twoKiBName, twoKiBPlusOneName);
    checkDeterministicInvalid(oversized);
  }

  return EXIT_SUCCESS;
}
